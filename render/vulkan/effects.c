// Offscreen render targets for the blur ping-pong chain.
//
// These are plain VkImages owned by the renderer rather than wlr_buffers: blur
// is recorded into the *same* command buffer as the scene (between
// wlr_vk_render_pass_suspend/resume), so keeping the targets internal avoids
// DMA-BUF import and any cross-command-buffer synchronisation.
//
// Format matches the scene render pass' colour attachment (the 16F blend
// image). That is deliberate: a Vulkan pipeline may be used with any
// "compatible" render pass, and compatibility only requires matching attachment
// formats and subpass structure, so the blur pipelines built against the scene
// pass can be reused here unchanged.

#include <stdlib.h>
#include <vulkan/vulkan_core.h>
#include <wlr/render/vulkan.h>
#include <wlr/util/log.h>

#include "render/vulkan/shaders.h"
#include "render/vulkan/vulkan.h"

#define EFFECT_IMAGE_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT

// wlroots keeps its equivalent private, so scenefx needs its own.
int vk_find_mem_type(struct vk_renderer *renderer,
		VkMemoryPropertyFlags props, uint32_t type_bits) {
	VkPhysicalDeviceMemoryProperties mem_props;
	vkGetPhysicalDeviceMemoryProperties(renderer->physical_device, &mem_props);

	for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
		if (!(type_bits & (1u << i))) {
			continue;
		}
		if ((mem_props.memoryTypes[i].propertyFlags & props) == props) {
			return (int)i;
		}
	}
	return -1;
}

static void effect_image_finish(struct vk_renderer *renderer,
		struct vk_effect_image *img) {
	VkDevice dev = renderer->device;
	if (img->image == VK_NULL_HANDLE && img->ds == VK_NULL_HANDLE) {
		return;
	}
	// These are only torn down on output reconfigure or shutdown, but the
	// previous frame's command buffer may still reference the descriptor set,
	// and freeing one that is in use is invalid. Rare enough that a full idle
	// wait is cheaper than tracking per-frame lifetimes.
	//
	// The wait fails on a lost device; the objects are dead anyway and Vulkan
	// permits destroying them, so the result is reported but never fatal.
	vk_renderer_handle_result(renderer, "vkDeviceWaitIdle", vkDeviceWaitIdle(dev));
	if (img->ds != VK_NULL_HANDLE) {
		vk_tex_ds_destroy(renderer, img->ds);
	}
	vkDestroyFramebuffer(dev, img->framebuffer, NULL);
	vkDestroyImageView(dev, img->view, NULL);
	vkDestroyImage(dev, img->image, NULL);
	vkFreeMemory(dev, img->memory, NULL);
	*img = (struct vk_effect_image){0};
}

static bool effect_image_init(struct vk_renderer *renderer,
		struct vk_effect_image *img, uint32_t width, uint32_t height) {
	VkDevice dev = renderer->device;
	VkResult res;

	VkImageCreateInfo img_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = EFFECT_IMAGE_FORMAT,
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.extent = (VkExtent3D){ width, height, 1 },
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	};
	res = vkCreateImage(dev, &img_info, NULL, &img->image);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(renderer, "vkCreateImage", res);
		goto error;
	}

	VkMemoryRequirements mem_reqs;
	vkGetImageMemoryRequirements(dev, img->image, &mem_reqs);

	int mem_type = vk_find_mem_type(renderer, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		mem_reqs.memoryTypeBits);
	if (mem_type < 0) {
		wlr_log(WLR_ERROR, "no suitable memory type for effect image");
		goto error;
	}

	VkMemoryAllocateInfo mem_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mem_reqs.size,
		.memoryTypeIndex = mem_type,
	};
	res = vkAllocateMemory(dev, &mem_info, NULL, &img->memory);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(renderer, "vkAllocateMemory", res);
		goto error;
	}

	res = vkBindImageMemory(dev, img->image, img->memory, 0);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(renderer, "vkBindImageMemory", res);
		goto error;
	}

	VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = img->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = EFFECT_IMAGE_FORMAT,
		.subresourceRange = (VkImageSubresourceRange){
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1,
		},
	};
	res = vkCreateImageView(dev, &view_info, NULL, &img->view);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(renderer, "vkCreateImageView", res);
		goto error;
	}

	VkFramebufferCreateInfo fb_info = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = renderer->effect_render_pass,
		.attachmentCount = 1,
		.pAttachments = &img->view,
		.width = width,
		.height = height,
		.layers = 1,
	};
	res = vkCreateFramebuffer(dev, &fb_info, NULL, &img->framebuffer);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(renderer, "vkCreateFramebuffer", res);
		goto error;
	}

	img->ds = vk_tex_ds_create(renderer, img->view);
	if (img->ds == VK_NULL_HANDLE) {
		wlr_log(WLR_ERROR, "failed to create descriptor set for effect image");
		goto error;
	}

	img->width = width;
	img->height = height;
	return true;

error:
	effect_image_finish(renderer, img);
	return false;
}

bool vk_effect_images_ensure(struct vk_renderer *renderer,
		VkRenderPass render_pass, uint32_t width, uint32_t height) {
	if (render_pass == VK_NULL_HANDLE) {
		return false;
	}
	// Framebuffers are tied to the render pass they were created with, so a
	// different scene pass (e.g. after an output format change) invalidates them.
	if (renderer->effect_render_pass != render_pass) {
		vk_effect_images_finish(renderer);
		renderer->effect_render_pass = render_pass;
	}

	for (size_t i = 0; i < VK_EFFECT_IMAGE_COUNT; i++) {
		struct vk_effect_image *img = &renderer->effect_images[i];
		if (img->image != VK_NULL_HANDLE &&
				img->width == width && img->height == height) {
			continue;
		}
		effect_image_finish(renderer, img);
		if (!effect_image_init(renderer, img, width, height)) {
			return false;
		}
	}
	return true;
}

void vk_effect_images_finish(struct vk_renderer *renderer) {
	for (size_t i = 0; i < VK_EFFECT_IMAGE_COUNT; i++) {
		effect_image_finish(renderer, &renderer->effect_images[i]);
	}
	// effect_render_pass is owned by wlroots (it is the scene pass); just forget it.
	renderer->effect_render_pass = VK_NULL_HANDLE;
	if (renderer->blend_ds != VK_NULL_HANDLE) {
		vk_tex_ds_destroy(renderer, renderer->blend_ds);
		renderer->blend_ds = VK_NULL_HANDLE;
		renderer->blend_ds_view = VK_NULL_HANDLE;
	}
}

VkDescriptorSet vk_get_blend_ds(struct vk_renderer *renderer, VkImageView view) {
	if (view == VK_NULL_HANDLE) {
		return VK_NULL_HANDLE;
	}
	// The blend image is recreated when the output is reconfigured, so cache
	// the descriptor set and rebuild it only when the view actually changes.
	if (renderer->blend_ds != VK_NULL_HANDLE && renderer->blend_ds_view == view) {
		return renderer->blend_ds;
	}
	if (renderer->blend_ds != VK_NULL_HANDLE) {
		// Same in-use hazard as effect_image_finish().
		vk_renderer_handle_result(renderer, "vkDeviceWaitIdle",
			vkDeviceWaitIdle(renderer->device));
		vk_tex_ds_destroy(renderer, renderer->blend_ds);
	}
	renderer->blend_ds = vk_tex_ds_create(renderer, view);
	renderer->blend_ds_view = renderer->blend_ds != VK_NULL_HANDLE ? view : VK_NULL_HANDLE;
	return renderer->blend_ds;
}

void vk_effect_image_prepare_target(VkCommandBuffer cb, struct vk_effect_image *img) {
	VkImageMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.oldLayout = img->layout,
		.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = img->image,
		.subresourceRange = (VkImageSubresourceRange){
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1,
		},
		.srcAccessMask = img->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ?
			VK_ACCESS_SHADER_READ_BIT : 0,
		.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
			VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
	};

	vkCmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		0, 0, NULL, 0, NULL, 1, &barrier);

	// The scene render pass declares finalLayout SHADER_READ_ONLY_OPTIMAL, so
	// once the upcoming pass ends the image will be samplable again.
	img->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void vk_mask_view_finish(struct vk_renderer *renderer) {
	if (renderer->mask_view.view == VK_NULL_HANDLE) {
		return;
	}
	// Same in-use hazard as effect_image_finish(): the previous frame's command
	// buffer may still sample it, and this only happens when the mask changes.
	vk_renderer_handle_result(renderer, "vkDeviceWaitIdle",
		vkDeviceWaitIdle(renderer->device));
	vkDestroyImageView(renderer->device, renderer->mask_view.view, NULL);
	renderer->mask_view = (typeof(renderer->mask_view)){0};
}

VkImageView vk_mask_view_get(struct vk_renderer *renderer, VkImage image,
		VkFormat format, VkImageLayout *layout) {
	if (renderer->mask_view.view != VK_NULL_HANDLE &&
			renderer->mask_view.image == image &&
			renderer->mask_view.format == format) {
		*layout = renderer->mask_view.layout;
		return renderer->mask_view.view;
	}

	vk_mask_view_finish(renderer);

	VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = format,
		.subresourceRange = (VkImageSubresourceRange){
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1,
		},
	};
	VkImageView view = VK_NULL_HANDLE;
	VkResult res = vkCreateImageView(renderer->device, &view_info, NULL, &view);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(renderer, "vkCreateImageView (mask)", res);
		return VK_NULL_HANDLE;
	}

	renderer->mask_view.image = image;
	renderer->mask_view.format = format;
	renderer->mask_view.view = view;
	// wlroots' own view of a scene buffer's image samples it read-only.
	renderer->mask_view.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	*layout = renderer->mask_view.layout;
	return view;
}

void vk_dummy_mask_finish(struct vk_renderer *renderer) {
	VkDevice dev = renderer->device;
	if (renderer->dummy_mask.image == VK_NULL_HANDLE) {
		return;
	}
	// Same in-use hazard as effect_image_finish(): a previous frame's command
	// buffer may still sample it, and this only happens at shutdown.
	vk_renderer_handle_result(renderer, "vkDeviceWaitIdle", vkDeviceWaitIdle(dev));
	vkDestroyImageView(dev, renderer->dummy_mask.view, NULL);
	vkDestroyImage(dev, renderer->dummy_mask.image, NULL);
	vkFreeMemory(dev, renderer->dummy_mask.memory, NULL);
	renderer->dummy_mask = (typeof(renderer->dummy_mask)){0};
}

bool vk_dummy_mask_ensure(struct vk_renderer *renderer) {
	if (renderer->dummy_mask.initialised) {
		return true;
	}
	VkDevice dev = renderer->device;
	VkResult res;

	// Opaque white, so no mask sample is ever below any sane alpha threshold and
	// `has_mask = 0` draws behave as if the mask were not there.
	VkImageCreateInfo img_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.extent = (VkExtent3D){ 1, 1, 1 },
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	};
	res = vkCreateImage(dev, &img_info, NULL, &renderer->dummy_mask.image);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(renderer, "vkCreateImage (dummy mask)", res);
		goto error;
	}

	VkMemoryRequirements mem_reqs;
	vkGetImageMemoryRequirements(dev, renderer->dummy_mask.image, &mem_reqs);
	int mem_type = vk_find_mem_type(renderer, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		mem_reqs.memoryTypeBits);
	if (mem_type < 0) {
		wlr_log(WLR_ERROR, "no suitable memory type for the dummy mask image");
		goto error;
	}
	VkMemoryAllocateInfo mem_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mem_reqs.size,
		.memoryTypeIndex = mem_type,
	};
	res = vkAllocateMemory(dev, &mem_info, NULL, &renderer->dummy_mask.memory);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(renderer, "vkAllocateMemory (dummy mask)", res);
		goto error;
	}
	res = vkBindImageMemory(dev, renderer->dummy_mask.image,
		renderer->dummy_mask.memory, 0);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(renderer, "vkBindImageMemory (dummy mask)", res);
		goto error;
	}

	VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = renderer->dummy_mask.image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.subresourceRange = (VkImageSubresourceRange){
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1,
		},
	};
	res = vkCreateImageView(dev, &view_info, NULL, &renderer->dummy_mask.view);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(renderer, "vkCreateImageView (dummy mask)", res);
		goto error;
	}

	renderer->dummy_mask.initialised = true;
	return true;

error:
	vk_dummy_mask_finish(renderer);
	return false;
}

void vk_saved_pixels_finish(struct vk_renderer *renderer) {
	if (renderer->saved_pixels.image == VK_NULL_HANDLE) {
		return;
	}
	vk_renderer_handle_result(renderer, "vkDeviceWaitIdle",
		vkDeviceWaitIdle(renderer->device));
	vkDestroyImage(renderer->device, renderer->saved_pixels.image, NULL);
	vkFreeMemory(renderer->device, renderer->saved_pixels.memory, NULL);
	renderer->saved_pixels.image = VK_NULL_HANDLE;
	renderer->saved_pixels.memory = VK_NULL_HANDLE;
	renderer->saved_pixels.initialised = false;
}

bool vk_saved_pixels_ensure(struct vk_renderer *renderer,
		uint32_t width, uint32_t height) {
	if (renderer->saved_pixels.image != VK_NULL_HANDLE &&
			renderer->saved_pixels.width == width &&
			renderer->saved_pixels.height == height) {
		return true;
	}
	vk_saved_pixels_finish(renderer);

	VkDevice dev = renderer->device;
	// Transfer only: this image is never sampled or drawn to, it just holds a
	// copy of the blend image's padding ring between save and apply.
	VkImageCreateInfo img_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = EFFECT_IMAGE_FORMAT,
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.extent = (VkExtent3D){ width, height, 1 },
		.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	};
	VkResult res = vkCreateImage(dev, &img_info, NULL,
		&renderer->saved_pixels.image);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(renderer, "vkCreateImage", res);
		return false;
	}

	VkMemoryRequirements mem_reqs;
	vkGetImageMemoryRequirements(dev, renderer->saved_pixels.image, &mem_reqs);
	int mem_type = vk_find_mem_type(renderer, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		mem_reqs.memoryTypeBits);
	if (mem_type < 0) {
		goto error;
	}

	VkMemoryAllocateInfo mem_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mem_reqs.size,
		.memoryTypeIndex = mem_type,
	};
	res = vkAllocateMemory(dev, &mem_info, NULL,
		&renderer->saved_pixels.memory);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(renderer, "vkAllocateMemory", res);
		goto error;
	}
	res = vkBindImageMemory(dev, renderer->saved_pixels.image,
		renderer->saved_pixels.memory, 0);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(renderer, "vkBindImageMemory", res);
		goto error;
	}

	renderer->saved_pixels.width = width;
	renderer->saved_pixels.height = height;
	renderer->saved_pixels.initialised = false;
	return true;

error:
	vk_saved_pixels_finish(renderer);
	return false;
}

bool vk_optimized_blur_ensure(struct vk_effect_image *cache,
		struct vk_renderer *renderer, VkRenderPass render_pass,
		uint32_t width, uint32_t height) {
	if (render_pass == VK_NULL_HANDLE) {
		return false;
	}
	if (cache->image != VK_NULL_HANDLE && cache->width == width &&
			cache->height == height) {
		return true;
	}
	vk_optimized_blur_finish(cache, renderer);
	// effect_image_init() only reads renderer->effect_render_pass to create the
	// framebuffer, and vk_render_pass_add_optimized_blur() always calls
	// vk_effect_images_ensure() just before this, so the field already holds the
	// scene pass. Do not assign it here: it is the effect images' identity, and
	// overwriting it would make vk_effect_images_ensure() skip the rebuild their
	// own framebuffers may need.
	if (!effect_image_init(renderer, cache, width, height)) {
		vk_optimized_blur_finish(cache, renderer);
		return false;
	}
	return true;
}

void vk_optimized_blur_finish(struct vk_effect_image *cache,
		struct vk_renderer *renderer) {
	if (cache->image == VK_NULL_HANDLE && cache->ds == VK_NULL_HANDLE) {
		return;
	}
	effect_image_finish(renderer, cache);
}
