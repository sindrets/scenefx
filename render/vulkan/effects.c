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

static void effect_images_finish(struct vk_offscreen_buffers *offscreen_buffers,
		struct vk_renderer *renderer, VkCommandBuffer cb);
static void blend_ds_destroy(struct vk_renderer *renderer, void *data);

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

static void effect_image_destroy(struct vk_renderer *renderer, void *data);

static void effect_image_finish(struct vk_renderer *renderer, VkCommandBuffer cb,
		struct vk_effect_image *img) {
	if (img->image == VK_NULL_HANDLE && img->ds == VK_NULL_HANDLE) {
		return;
	}
	// The previous frame's command buffer may still reference the descriptor set,
	// and freeing one that is in use is invalid. Defer to the resources callback
	// when a pass is recording; `cb` is VK_NULL_HANDLE only at shutdown, where
	// there is nothing recording and freeing immediately is correct.
	//
	// A lost device fails the wait, but the objects are dead anyway and Vulkan
	// permits destroying them.
	vk_deferred_destroy(renderer, cb, effect_image_destroy, img);
}

static void effect_image_destroy(struct vk_renderer *renderer, void *data) {
	struct vk_effect_image *img = data;
	VkDevice dev = renderer->device;

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
		VkRenderPass render_pass, VkCommandBuffer cb,
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
		.renderPass = render_pass,
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
	effect_image_finish(renderer, cb, img);
	return false;
}
bool vk_effect_images_ensure(struct vk_render_pass *pass, VkRenderPass scene_pass,
		uint32_t width, uint32_t height) {
	if (scene_pass == VK_NULL_HANDLE) {
		return false;
	}
	struct vk_offscreen_buffers *offscreen_buffers = pass->vk_offscreen_buffers;
	if (offscreen_buffers == NULL) {
		return false;
	}
	struct vk_renderer *renderer = pass->vk_renderer;
	VkCommandBuffer cb = pass->command_buffer;

	// Framebuffers are tied to the render pass they were created with, so a
	// different scene pass (e.g. after an output format change) invalidates them.
	if (offscreen_buffers->effect_render_pass != scene_pass) {
		effect_images_finish(offscreen_buffers, renderer, cb);
		offscreen_buffers->effect_render_pass = scene_pass;
	}

	for (size_t i = 0; i < VK_EFFECT_IMAGE_COUNT; i++) {
		struct vk_effect_image *img = &offscreen_buffers->effect_images[i];
		if (img->image != VK_NULL_HANDLE &&
				img->width == width && img->height == height) {
			continue;
		}
		effect_image_finish(renderer, cb, img);
		if (!effect_image_init(renderer, scene_pass, cb, img, width, height)) {
			return false;
		}
	}
	return true;
}

static void effect_images_finish(struct vk_offscreen_buffers *offscreen_buffers,
		struct vk_renderer *renderer, VkCommandBuffer cb) {
	for (size_t i = 0; i < VK_EFFECT_IMAGE_COUNT; i++) {
		effect_image_finish(renderer, cb, &offscreen_buffers->effect_images[i]);
	}
	// effect_render_pass is owned by wlroots (it is the scene pass); just forget it.
	offscreen_buffers->effect_render_pass = VK_NULL_HANDLE;
	if (offscreen_buffers->blend_ds != VK_NULL_HANDLE) {
		vk_deferred_destroy(renderer, cb, blend_ds_destroy,
			offscreen_buffers->blend_ds);
		offscreen_buffers->blend_ds = VK_NULL_HANDLE;
		offscreen_buffers->blend_ds_view = VK_NULL_HANDLE;
	}
}

void vk_effect_images_finish(struct vk_offscreen_buffers *offscreen_buffers,
		struct vk_renderer *renderer) {
	// Called from offscreen_buffers_destroy(), where nothing is recording.
	effect_images_finish(offscreen_buffers, renderer, VK_NULL_HANDLE);
}

VkDescriptorSet vk_get_blend_ds(struct vk_render_pass *pass, VkImageView view) {
	if (view == VK_NULL_HANDLE || pass->vk_offscreen_buffers == NULL) {
		return VK_NULL_HANDLE;
	}
	struct vk_offscreen_buffers *offscreen_buffers = pass->vk_offscreen_buffers;
	struct vk_renderer *renderer = pass->vk_renderer;

	// The blend image is recreated when the output is reconfigured, so cache
	// the descriptor set and rebuild it only when the view actually changes.
	if (offscreen_buffers->blend_ds != VK_NULL_HANDLE &&
			offscreen_buffers->blend_ds_view == view) {
		return offscreen_buffers->blend_ds;
	}
	if (offscreen_buffers->blend_ds != VK_NULL_HANDLE) {
		// Deferred, so the replacement does not wait on the device: this happens
		// whenever a new frame's blend image differs from the cached one.
		vk_deferred_destroy(renderer, pass->command_buffer,
			blend_ds_destroy, offscreen_buffers->blend_ds);
	}
	offscreen_buffers->blend_ds = vk_tex_ds_create(renderer, view);
	offscreen_buffers->blend_ds_view =
		offscreen_buffers->blend_ds != VK_NULL_HANDLE ? view : VK_NULL_HANDLE;
	return offscreen_buffers->blend_ds;
}
static void blend_ds_destroy(struct vk_renderer *renderer, void *data) {
	vk_tex_ds_destroy(renderer, (VkDescriptorSet)data);
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

static void mask_view_destroy(struct vk_renderer *renderer, void *data) {
	vkDestroyImageView(renderer->device, (VkImageView)data, NULL);
}

void vk_mask_view_finish(struct vk_offscreen_buffers *offscreen_buffers,
		struct vk_renderer *renderer) {
	for (size_t i = 0; i < VK_MASK_VIEW_CACHE_LEN; i++) {
		struct vk_mask_view *entry = &offscreen_buffers->mask_views[i];
		if (entry->view == VK_NULL_HANDLE) {
			continue;
		}
		vk_deferred_destroy(renderer, VK_NULL_HANDLE, mask_view_destroy, entry->view);
		*entry = (struct vk_mask_view){0};
	}
}

VkImageView vk_mask_view_get(struct vk_offscreen_buffers *offscreen_buffers,
		struct vk_renderer *renderer, VkCommandBuffer cb, VkImage image,
		VkFormat format, VkImageLayout *layout) {
	for (size_t i = 0; i < VK_MASK_VIEW_CACHE_LEN; i++) {
		struct vk_mask_view *entry = &offscreen_buffers->mask_views[i];
		if (entry->view != VK_NULL_HANDLE && entry->image == image &&
				entry->format == format) {
			*layout = entry->layout;
			return entry->view;
		}
	}

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

	// Fill the first free slot, or recycle the oldest one. Several mask images
	// can be live in the same frame (one per layer surface with its own mask),
	// so a single slot would thrash: each mask change would tear down a view that
	// the current command buffer still samples.
	struct vk_mask_view *slot = NULL;
	for (size_t i = 0; i < VK_MASK_VIEW_CACHE_LEN; i++) {
		if (offscreen_buffers->mask_views[i].view == VK_NULL_HANDLE) {
			slot = &offscreen_buffers->mask_views[i];
			break;
		}
	}
	if (slot == NULL) {
		slot = &offscreen_buffers->mask_views[offscreen_buffers->mask_views_next];
		offscreen_buffers->mask_views_next =
			(offscreen_buffers->mask_views_next + 1) % VK_MASK_VIEW_CACHE_LEN;
		// Too many distinct masks for the cache: drop the oldest. Clearing the
		// slot first keeps the recycle path from acting on a freed view, and the
		// destruction has to be deferred because the command buffer being
		// recorded right now may still sample it.
		VkImageView stale = slot->view;
		*slot = (struct vk_mask_view){0};
		vk_deferred_destroy(renderer, cb, mask_view_destroy, stale);
	}

	slot->image = image;
	slot->format = format;
	slot->view = view;
	// wlroots' own view of a scene buffer's image samples it read-only.
	slot->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	*layout = slot->layout;
	return view;
}

// Clear the dummy mask to opaque white and park it in the layout the shader
// samples it from. This cannot be done as part of a render pass: the image is
// created in UNDEFINED, so both the layout transition and the clear have to be
// recorded before any pass binds it, or the descriptor's declared
// SHADER_READ_ONLY_OPTIMAL layout would not match the image's real one
// (VUID-vkCmdDraw-None-08114 and friends).
static bool dummy_mask_initialise(struct vk_renderer *renderer) {
	VkDevice dev = renderer->device;

	VkCommandPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
		.queueFamilyIndex =
			wlr_vk_renderer_get_queue_family(renderer->fx_renderer.wlr_renderer),
	};
	VkCommandPool pool = VK_NULL_HANDLE;
	VkResult res = vkCreateCommandPool(dev, &pool_info, NULL, &pool);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(renderer, "vkCreateCommandPool (dummy mask)", res);
		return false;
	}

	VkCommandBufferAllocateInfo alloc_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer cb = VK_NULL_HANDLE;
	res = vkAllocateCommandBuffers(dev, &alloc_info, &cb);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(renderer, "vkAllocateCommandBuffers (dummy mask)", res);
		goto out;
	}

	VkCommandBufferBeginInfo begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	res = vkBeginCommandBuffer(cb, &begin_info);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(renderer, "vkBeginCommandBuffer (dummy mask)", res);
		goto out;
	}

	VkImageSubresourceRange range = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.levelCount = 1,
		.layerCount = 1,
	};
	VkImageMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = renderer->dummy_mask.image,
		.subresourceRange = range,
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

	// Opaque white: every channel at 1.0, so the shader's alpha-threshold
	// `discard` can never trigger and `has_mask = 0` draws sample an inert mask.
	VkClearColorValue white = { .float32 = { 1.0f, 1.0f, 1.0f, 1.0f } };
	vkCmdClearColorImage(cb, renderer->dummy_mask.image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);

	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

	res = vkEndCommandBuffer(cb);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(renderer, "vkEndCommandBuffer (dummy mask)", res);
		goto out;
	}

	VkSubmitInfo submit_info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &cb,
	};
	res = vkQueueSubmit(renderer->queue, 1, &submit_info, VK_NULL_HANDLE);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(renderer, "vkQueueSubmit (dummy mask)", res);
		goto out;
	}
	// The clear has to have landed before anything samples the image.
	res = vkQueueWaitIdle(renderer->queue);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(renderer, "vkQueueWaitIdle (dummy mask)", res);
	}

out:
	vkDestroyCommandPool(dev, pool, NULL);
	return res == VK_SUCCESS;
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

	if (!dummy_mask_initialise(renderer)) {
		goto error;
	}

	renderer->dummy_mask.initialised = true;
	return true;

error:
	vk_dummy_mask_finish(renderer);
	return false;
}

static void saved_pixels_destroy(struct vk_renderer *renderer, void *data) {
	struct vk_saved_pixels *saved_pixels = data;
	vkDestroyImage(renderer->device, saved_pixels->image, NULL);
	vkFreeMemory(renderer->device, saved_pixels->memory, NULL);
	*saved_pixels = (struct vk_saved_pixels){0};
}

static void saved_pixels_finish(struct vk_renderer *renderer, VkCommandBuffer cb,
		struct vk_saved_pixels *saved_pixels) {
	if (saved_pixels->image == VK_NULL_HANDLE) {
		return;
	}
	vk_deferred_destroy(renderer, cb, saved_pixels_destroy, saved_pixels);
}

void vk_saved_pixels_finish(struct vk_offscreen_buffers *offscreen_buffers,
		struct vk_renderer *renderer) {
	// Called from offscreen_buffers_destroy(), where nothing is recording any
	// more, so the images can go immediately.
	saved_pixels_finish(renderer, VK_NULL_HANDLE, &offscreen_buffers->saved_pixels);
}

bool vk_saved_pixels_ensure(struct vk_render_pass *pass,
		uint32_t width, uint32_t height) {
	struct vk_offscreen_buffers *offscreen_buffers = pass->vk_offscreen_buffers;
	if (offscreen_buffers == NULL) {
		return false;
	}
	struct vk_renderer *renderer = pass->vk_renderer;
	struct vk_saved_pixels *saved_pixels = &offscreen_buffers->saved_pixels;

	if (saved_pixels->image != VK_NULL_HANDLE && saved_pixels->width == width &&
			saved_pixels->height == height) {
		return true;
	}
	saved_pixels_finish(renderer, pass->command_buffer, saved_pixels);

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
	VkResult res = vkCreateImage(dev, &img_info, NULL, &saved_pixels->image);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(renderer, "vkCreateImage", res);
		return false;
	}

	VkMemoryRequirements mem_reqs;
	vkGetImageMemoryRequirements(dev, saved_pixels->image, &mem_reqs);
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
	res = vkAllocateMemory(dev, &mem_info, NULL, &saved_pixels->memory);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(renderer, "vkAllocateMemory", res);
		goto error;
	}
	res = vkBindImageMemory(dev, saved_pixels->image, saved_pixels->memory, 0);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(renderer, "vkBindImageMemory", res);
		goto error;
	}

	saved_pixels->width = width;
	saved_pixels->height = height;
	saved_pixels->initialised = false;
	return true;

error:
	saved_pixels_finish(renderer, pass->command_buffer, saved_pixels);
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
	if (!effect_image_init(renderer, render_pass, VK_NULL_HANDLE, cache,
			width, height)) {
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
	// Only called from offscreen_buffers_destroy(), where nothing is recording.
	effect_image_finish(renderer, VK_NULL_HANDLE, cache);
}
