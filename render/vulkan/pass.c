#include <assert.h>
#include <math.h>
#include <pixman.h>
#include <stdlib.h>
#include <unistd.h>
#include <vulkan/vulkan_core.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/interface.h>
#include <wlr/render/vulkan.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <wlr/util/transform.h>
#include <stdio.h>

#include "render/fx_renderer.h"
#include "render/tracy.h"
#include "render/vulkan/shaders.h"
#include "render/vulkan/vulkan.h"
#include "scenefx/render/pass.h"
#include "scenefx/types/fx/blur_data.h"
#include "util/matrix.h"

///
/// wlroots functions
///

static void gradient_state_unref(struct vk_renderer *vk_renderer,
		struct vk_gradient_state *state);
static void gradient_arena_destroy(struct vk_renderer *vk_renderer,
		struct vk_gradient_arena *arena);

// TODO: Fix de-sync with upstream pass. Upstream renderer can change the bound
// pipeline without us knowing
static void bind_pipeline(struct vk_render_pass *pass, VkPipeline pipeline) {
	if (pipeline == pass->bound_pipeline) {
		return;
	}

	vkCmdBindPipeline(pass->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	pass->bound_pipeline = pipeline;
}

static void get_clip_region(struct vk_render_pass *pass,
		const pixman_region32_t *in, pixman_region32_t *out) {
	if (in != NULL) {
		pixman_region32_init(out);
		pixman_region32_copy(out, in);
	} else {
		struct wlr_buffer *buffer = pass->render_buffer->wlr_buffer;
		pixman_region32_init_rect(out, 0, 0, buffer->width, buffer->height);
	}
}

static void convert_pixman_box_to_vk_rect(const pixman_box32_t *box, VkRect2D *rect) {
	*rect = (VkRect2D){
		.offset = { .x = box->x1, .y = box->y1 },
		.extent = { .width = box->x2 - box->x1, .height = box->y2 - box->y1 },
	};
}

static float color_to_linear(float non_linear) {
	return pow(non_linear, 2.2);
}

static float color_to_linear_premult(float non_linear, float alpha) {
	return (alpha == 0) ? 0 : color_to_linear(non_linear / alpha) * alpha;
}

static void encode_proj_matrix(const float mat3[9], float mat4[4][4]) {
	float result[4][4] = {
		{ mat3[0], mat3[1], 0, mat3[2] },
		{ mat3[3], mat3[4], 0, mat3[5] },
		{ 0, 0, 1, 0 },
		{ 0, 0, 0, 1 },
	};

	memcpy(mat4, result, sizeof(result));
}

///
/// FX pass functions
///

static void render_pass_mark_box_updated(struct vk_render_pass *pass,
		const struct wlr_box *box) {
	pixman_region32_t region;
	pixman_region32_init_rect(&region, box->x, box->y, box->width, box->height);

	wlr_vk_render_pass_mark_updated(pass->fx_render_pass.render_pass, &region);

	pixman_region32_fini(&region);
}

static bool apply_clip_region(pixman_region32_t *clip_region,
		const struct wlr_box *clipped_region_box, const struct fx_corner_fradii *corners) {
	if (!wlr_box_empty(clipped_region_box)) {
		float top = fmax(corners->top_left, corners->top_right);
		float bottom = fmax(corners->bottom_left, corners->bottom_right);
		float left = fmax(corners->top_left, corners->bottom_left);
		float right = fmax(corners->top_right, corners->bottom_right);

		pixman_region32_t user_clip_region;
		pixman_region32_init_rect(
			&user_clip_region,
			clipped_region_box->x + (left * 0.3),
			clipped_region_box->y + (top * 0.3),
			fmax(clipped_region_box->width - (left + right) * 0.3, 0),
			fmax(clipped_region_box->height - (top + bottom) * 0.3, 0)
		);
		pixman_region32_subtract(clip_region, clip_region, &user_clip_region);
		pixman_region32_fini(&user_clip_region);
		return true;
	}

	return false;
}

static void push_rounding_data(struct vk_shader_rounding_data *data,
		const struct wlr_box *box, const struct fx_corner_fradii *corners) {
	*data = (struct vk_shader_rounding_data) {
		.size = { box->width, box->height },
		.position = { box->x, box->y },
		.radius = {
			.top_left = corners->top_left,
			.top_right = corners->top_right,
			.bottom_left = corners->bottom_left,
			.bottom_right = corners->bottom_right,
		},
	};
}

// Draw the currently bound pipeline over every rect of the clip region.
static void draw_clip_region(struct vk_render_pass *pass, pixman_region32_t *clip_region,
		const struct wlr_box *box) {
	VkCommandBuffer cb = pass->command_buffer;

	int clip_rects_len;
	const pixman_box32_t *clip_rects = pixman_region32_rectangles(clip_region, &clip_rects_len);
	// Record regions possibly updated for use in second subpass
	for (int i = 0; i < clip_rects_len; i++) {
		VkRect2D rect;
		convert_pixman_box_to_vk_rect(&clip_rects[i], &rect);
		vkCmdSetScissor(cb, 0, 1, &rect);
		vkCmdDraw(cb, 4, 1, 0, 0);

		struct wlr_box clip_box = {
			.x = clip_rects[i].x1,
			.y = clip_rects[i].y1,
			.width = clip_rects[i].x2 - clip_rects[i].x1,
			.height = clip_rects[i].y2 - clip_rects[i].y1,
		};
		struct wlr_box intersection;
		if (!wlr_box_intersection(&intersection, box, &clip_box)) {
			continue;
		}
		render_pass_mark_box_updated(pass, &intersection);
	}

	// We just changed the pipeline and push constants behind wlroots' back, so
	// make sure it re-binds its own state on its next draw.
	wlr_vk_render_pass_reset_pipeline(pass->fx_render_pass.render_pass);
	pass->bound_pipeline = VK_NULL_HANDLE;
}

///
/// FX Render Pass implementation
///

// Copies the blur padding ring between the blend image and our snapshot image.
//
// scenefx expands the frame's damage to include a ring around each blur node
// (wlr_scene.c), snapshots those pixels before the frame renders, and pastes
// them back afterwards. Without it, content that sits above a blur node leaks
// into that node's blur on the next frame, because the blend image still holds
// the previous frame's composited result wherever nothing was redrawn.
//
// vkCmdCopyImage cannot be recorded inside a render pass, so the scene pass is
// suspended around the transfer and resumed afterwards.
static void blur_padding_copy(struct fx_render_pass *fx_pass,
		const pixman_region32_t *region, bool save) {
	if (region == NULL || !pixman_region32_not_empty(region)) {
		return;
	}

	struct vk_render_pass *pass = vk_get_render_pass(fx_pass);
	struct vk_renderer *renderer = pass->vk_renderer;
	struct wlr_buffer *buffer = pass->render_buffer->wlr_buffer;

	VkImage blend = wlr_vk_render_pass_get_blend_image(fx_pass->render_pass);
	if (blend == VK_NULL_HANDLE) {
		return;
	}
	if (!vk_saved_pixels_ensure(renderer, buffer->width, buffer->height)) {
		return;
	}
	// Nothing has been saved yet, so there is nothing to paste back.
	if (!save && !renderer->saved_pixels.initialised) {
		return;
	}

	int rects_len;
	const pixman_box32_t *rects = pixman_region32_rectangles(region, &rects_len);
	if (rects_len == 0) {
		return;
	}

	if (!wlr_vk_render_pass_suspend(fx_pass->render_pass)) {
		return;
	}

	VkCommandBuffer cb = pass->command_buffer;
	VkImage src = save ? blend : renderer->saved_pixels.image;
	VkImage dst = save ? renderer->saved_pixels.image : blend;

	// The blend image sits in SHADER_READ_ONLY_OPTIMAL after the suspend; the
	// snapshot image is UNDEFINED until first written, then TRANSFER_SRC.
	// After any previous save the snapshot parks in TRANSFER_SRC; before the
	// first one its contents are undefined.
	VkImageLayout saved_old = renderer->saved_pixels.initialised ?
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;

	VkImageSubresourceRange range = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.levelCount = 1,
		.layerCount = 1,
	};
	VkImageMemoryBarrier to_transfer[] = {
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.newLayout = save ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL :
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = blend,
			.subresourceRange = range,
			.srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.dstAccessMask = save ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT,
		},
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.oldLayout = saved_old,
			.newLayout = save ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL :
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = renderer->saved_pixels.image,
			.subresourceRange = range,
			.srcAccessMask = 0,
			.dstAccessMask = save ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT,
		},
	};
	vkCmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, to_transfer);

	for (int i = 0; i < rects_len; i++) {
		// Clamp to the image: scenefx bounds blur_padding_region by the output
		// size, which is not always the render buffer size, and a copy that
		// runs past the edge is invalid usage (and loses the device).
		int32_t x1 = rects[i].x1 < 0 ? 0 : rects[i].x1;
		int32_t y1 = rects[i].y1 < 0 ? 0 : rects[i].y1;
		int32_t x2 = rects[i].x2 > (int32_t)renderer->saved_pixels.width ?
			(int32_t)renderer->saved_pixels.width : rects[i].x2;
		int32_t y2 = rects[i].y2 > (int32_t)renderer->saved_pixels.height ?
			(int32_t)renderer->saved_pixels.height : rects[i].y2;

		int32_t x = x1, y = y1;
		int32_t w = x2 - x1;
		int32_t h = y2 - y1;
		if (w <= 0 || h <= 0) {
			continue;
		}
		VkImageCopy copy = {
			.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.srcOffset = { x, y, 0 },
			.dstOffset = { x, y, 0 },
			.extent = { w, h, 1 },
		};
		vkCmdCopyImage(cb,
			src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	}

	// Blend goes back to being sampleable, which is the layout resume() expects
	// to transition from; the snapshot parks in TRANSFER_SRC for the next apply.
	VkImageMemoryBarrier from_transfer[] = {
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.oldLayout = save ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL :
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = blend,
			.subresourceRange = range,
			.srcAccessMask = save ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		},
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.oldLayout = save ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL :
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = renderer->saved_pixels.image,
			.subresourceRange = range,
			.srcAccessMask = save ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT,
			.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		},
	};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, NULL, 0, NULL, 2, from_transfer);

	if (save) {
		renderer->saved_pixels.initialised = true;
	}

	if (!wlr_vk_render_pass_resume(fx_pass->render_pass)) {
		wlr_log(WLR_ERROR, "Failed to resume the scene render pass after the "
			"blur padding %s; subsequent commands are recorded outside it",
			save ? "save" : "restore");
	}
}

static void vk_render_pass_read_to_buffer(struct fx_render_pass *fx_pass,
		const pixman_region32_t *_region, struct wlr_buffer *dst_buffer,
		struct wlr_buffer *src_buffer) {
	// Unused by the Vulkan path: the blur padding snapshot is kept in a
	// renderer-owned VkImage rather than a wlr_buffer, so save/apply above copy
	// directly instead of going through this wlr_buffer-oriented entry point.
}

static void vk_render_pass_save_blur_region(struct fx_render_pass *fx_pass) {
	blur_padding_copy(fx_pass, &fx_pass->blur_padding_region, true);
}

static void vk_render_pass_apply_saved_blur_region(struct fx_render_pass *fx_pass) {
	blur_padding_copy(fx_pass, &fx_pass->blur_padding_region, false);
}

static void vk_render_pass_destroy(struct fx_render_pass *fx_pass) {
	struct vk_render_pass *vk_render_pass = vk_get_render_pass(fx_pass);
	struct vk_renderer *vk_renderer = vk_render_pass->vk_renderer;
	// The pass is torn down before wlr_render_pass_submit() ends the command
	// buffer, so stop recording GPU timestamps against it now.
	if (vk_renderer->tracy_command_buffer == vk_render_pass->command_buffer) {
		vk_renderer_set_tracy_command_buffer(vk_renderer, VK_NULL_HANDLE);
	}

	gradient_state_unref(vk_renderer, vk_render_pass->gradient_state);
	free(vk_render_pass);
}

static void vk_render_pass_add_rect(struct fx_render_pass *fx_pass,
		const struct fx_render_rect_options *fx_options) {
	// A lost device rejects everything, so record nothing and let the
	// compositor's listener take over. vk_render_pass_init() is what actually
	// reports the loss; the pass it returns is the first thing to notice.
	if (vk_renderer_device_lost(vk_get_render_pass(fx_pass)->vk_renderer)) {
		return;
	}

	TRACY_GPU_ZONE_START(fx_pass->fx_renderer);

	const struct wlr_render_rect_options *options = &fx_options->base;
	struct vk_render_pass *pass = vk_get_render_pass(fx_pass);
	struct vk_renderer *vk_renderer = pass->vk_renderer;
	VkCommandBuffer cb = pass->command_buffer;

	// Input color values are given in sRGB space, shader expects
	// them in linear space. The shader does all computation in linear
	// space and expects the inputs in linear space since it outputs
	// colors in linear space as well (and vulkan then automatically
	// does the conversion for our sRGB render targets).
	float linear_color[] = {
		color_to_linear_premult(options->color.r, options->color.a),
		color_to_linear_premult(options->color.g, options->color.a),
		color_to_linear_premult(options->color.b, options->color.a),
		options->color.a, // no conversion for alpha
	};

	struct wlr_box box;
	wlr_render_rect_options_get_box(options, pass->render_buffer->wlr_buffer, &box);

	pixman_region32_t clip_region;
	get_clip_region(pass, options->clip, &clip_region);
	apply_clip_region(&clip_region,
		&fx_options->clipped_region.area, &fx_options->clipped_region.corners);

	const bool should_clip = clipped_fregion_is_valid(&fx_options->clipped_region);

	// Gather the effects and pick the relevant shader
	enum fx_quad_shader_effects effects = SHADER_QUAD_EFFECT_NONE;
	if (should_clip) {
		effects |= SHADER_QUAD_EFFECT_CLIPPING;
	}

	float proj[9], matrix[9];
	wlr_matrix_identity(proj);
	wlr_matrix_project_box(matrix, &box, WL_OUTPUT_TRANSFORM_NORMAL, proj);
	wlr_matrix_multiply(matrix, pass->projection, matrix);

	struct vk_pipeline *pipeline = get_vk_quad_pipeline(
		pass->render_setup->vk_pipelines.quad, effects);
	if (pipeline == NULL) {
		pixman_region32_fini(&clip_region);
		TRACY_GPU_ZONE_END;
		return;
	}

	VkPipelineLayout layout = vk_renderer->shader_info.quad.pipeline_layout;

	struct vk_vert_pcr_data vert_pcr_data = {
		.uv_off = { 0, 0 },
		.uv_size = { 1, 1 },
	};
	encode_proj_matrix(matrix, vert_pcr_data.mat4);

	struct vk_frag_quad_pcr_data quad_pcr_data = {
		.color = { linear_color[0], linear_color[1], linear_color[2], linear_color[3] },
	};
	push_rounding_data(&quad_pcr_data.corner_rounding, &box, &(struct fx_corner_fradii){ 0 });
	push_rounding_data(&quad_pcr_data.clipping,
		&fx_options->clipped_region.area, &fx_options->clipped_region.corners);

	bind_pipeline(pass, pipeline->pipeline);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT,
		0, sizeof(vert_pcr_data), &vert_pcr_data);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT,
		sizeof(vert_pcr_data), sizeof(quad_pcr_data), &quad_pcr_data);

	draw_clip_region(pass, &clip_region, &box);

	pixman_region32_fini(&clip_region);
	TRACY_GPU_ZONE_END;
}

static void vk_render_pass_add_rounded_rect(struct fx_render_pass *fx_pass,
		const struct fx_render_rounded_rect_options *fx_options) {
	// A lost device rejects everything, so record nothing and let the
	// compositor's listener take over. vk_render_pass_init() is what actually
	// reports the loss; the pass it returns is the first thing to notice.
	if (vk_renderer_device_lost(vk_get_render_pass(fx_pass)->vk_renderer)) {
		return;
	}

	TRACY_GPU_ZONE_START(fx_pass->fx_renderer);

	const struct wlr_render_rect_options *options = &fx_options->base;
	struct vk_render_pass *pass = vk_get_render_pass(fx_pass);
	struct vk_renderer *vk_renderer = pass->vk_renderer;
	VkCommandBuffer cb = pass->command_buffer;

	float linear_color[] = {
		color_to_linear_premult(options->color.r, options->color.a),
		color_to_linear_premult(options->color.g, options->color.a),
		color_to_linear_premult(options->color.b, options->color.a),
		options->color.a,
	};

	struct wlr_box box;
	wlr_render_rect_options_get_box(options, pass->render_buffer->wlr_buffer, &box);

	pixman_region32_t clip_region;
	get_clip_region(pass, options->clip, &clip_region);
	apply_clip_region(&clip_region,
		&fx_options->clipped_region.area, &fx_options->clipped_region.corners);

	const bool should_round_corners = !fx_corner_fradii_is_empty(&fx_options->corners);
	const bool should_clip = clipped_fregion_is_valid(&fx_options->clipped_region);

	enum fx_quad_shader_effects effects = SHADER_QUAD_EFFECT_NONE;
	if (should_round_corners) {
		effects |= SHADER_QUAD_EFFECT_ROUND_CORNERS;
	}
	if (should_clip) {
		effects |= SHADER_QUAD_EFFECT_CLIPPING;
	}

	float proj[9], matrix[9];
	wlr_matrix_identity(proj);
	wlr_matrix_project_box(matrix, &box, WL_OUTPUT_TRANSFORM_NORMAL, proj);
	wlr_matrix_multiply(matrix, pass->projection, matrix);

	struct vk_pipeline *pipeline = get_vk_quad_pipeline(
		pass->render_setup->vk_pipelines.quad, effects);
	if (pipeline == NULL) {
		pixman_region32_fini(&clip_region);
		TRACY_GPU_ZONE_END;
		return;
	}

	VkPipelineLayout layout = vk_renderer->shader_info.quad.pipeline_layout;

	struct vk_vert_pcr_data vert_pcr_data = {
		.uv_off = { 0, 0 },
		.uv_size = { 1, 1 },
	};
	encode_proj_matrix(matrix, vert_pcr_data.mat4);

	struct vk_frag_quad_pcr_data quad_pcr_data = {
		.color = { linear_color[0], linear_color[1], linear_color[2], linear_color[3] },
	};
	push_rounding_data(&quad_pcr_data.corner_rounding, &box, &fx_options->corners);
	push_rounding_data(&quad_pcr_data.clipping,
		&fx_options->clipped_region.area, &fx_options->clipped_region.corners);

	bind_pipeline(pass, pipeline->pipeline);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT,
		0, sizeof(vert_pcr_data), &vert_pcr_data);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT,
		sizeof(vert_pcr_data), sizeof(quad_pcr_data), &quad_pcr_data);

	draw_clip_region(pass, &clip_region, &box);

	pixman_region32_fini(&clip_region);
	TRACY_GPU_ZONE_END;
}

// Min alignment required for dynamic uniform buffer offsets. 256 is the
// guaranteed floor on every real driver.
#define GRADIENT_UBO_ALIGNMENT 256
#define GRADIENT_UBO_SIZE (sizeof(struct vk_frag_quad_grad_ubo))
#define GRADIENT_UBO_STRIDE \
	((GRADIENT_UBO_SIZE + GRADIENT_UBO_ALIGNMENT - 1) / GRADIENT_UBO_ALIGNMENT \
		* GRADIENT_UBO_ALIGNMENT)
// Growing by this many entries at a time amortizes the allocation cost.
#define GRADIENT_UBO_ENTRIES 8

// wlroots' equivalent lives in its private headers, which are not installed.
static int find_mem_type(struct vk_renderer *vk_renderer,
		VkMemoryPropertyFlags required, uint32_t type_bits) {
	VkPhysicalDeviceMemoryProperties mem_props;
	vkGetPhysicalDeviceMemoryProperties(vk_renderer->physical_device, &mem_props);

	for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
		if ((type_bits & (1u << i)) == 0) {
			continue;
		}
		if ((mem_props.memoryTypes[i].propertyFlags & required) == required) {
			return (int)i;
		}
	}

	return -1;
}

static struct vk_gradient_state *gradient_state_ref(struct vk_gradient_state *state) {
	state->refs++;
	return state;
}

static void gradient_state_unref(struct vk_renderer *vk_renderer,
		struct vk_gradient_state *state) {
	assert(state->refs > 0);
	if (--state->refs > 0) {
		return;
	}

	struct vk_gradient_arena *arena, *arena_tmp;
	wl_list_for_each_safe(arena, arena_tmp, &state->arenas, link) {
		wl_list_remove(&arena->link);
		gradient_arena_destroy(vk_renderer, arena);
	}

	free(state);
}

static void gradient_arena_destroy(struct vk_renderer *vk_renderer,
		struct vk_gradient_arena *arena) {
	if (arena->descriptor_pool != VK_NULL_HANDLE) {
		vkDestroyDescriptorPool(vk_renderer->device, arena->descriptor_pool, NULL);
	}
	if (arena->buffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(vk_renderer->device, arena->buffer, NULL);
	}
	if (arena->memory != VK_NULL_HANDLE) {
		vkFreeMemory(vk_renderer->device, arena->memory, NULL);
	}
	free(arena);
}

static struct vk_gradient_arena *create_gradient_arena(struct vk_render_pass *pass) {
	struct vk_renderer *vk_renderer = pass->vk_renderer;
	VkDevice dev = vk_renderer->device;

	struct vk_gradient_arena *arena = calloc(1, sizeof(*arena));
	if (arena == NULL) {
		wlr_log_errno(WLR_ERROR, "Failed to create the gradient UBO arena");
		return NULL;
	}
	arena->size = GRADIENT_UBO_STRIDE * GRADIENT_UBO_ENTRIES;

	VkBufferCreateInfo buffer_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = arena->size,
		.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkResult res = vkCreateBuffer(dev, &buffer_info, NULL, &arena->buffer);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(vk_renderer, "vkCreateBuffer", res);
		goto error;
	}

	VkMemoryRequirements mem_reqs;
	vkGetBufferMemoryRequirements(dev, arena->buffer, &mem_reqs);

	const int mem_type_index = find_mem_type(vk_renderer,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		mem_reqs.memoryTypeBits);
	if (mem_type_index < 0) {
		wlr_log(WLR_ERROR, "Failed to find a host visible memory type for the gradient UBO");
		goto error;
	}

	VkMemoryAllocateInfo mem_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mem_reqs.size,
		.memoryTypeIndex = (uint32_t)mem_type_index,
	};
	res = vkAllocateMemory(dev, &mem_info, NULL, &arena->memory);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(vk_renderer, "vkAllocateMemory", res);
		goto error;
	}

	res = vkBindBufferMemory(dev, arena->buffer, arena->memory, 0);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(vk_renderer, "vkBindBufferMemory", res);
		goto error;
	}

	res = vkMapMemory(dev, arena->memory, 0, VK_WHOLE_SIZE, 0, &arena->mapped);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(vk_renderer, "vkMapMemory", res);
		goto error;
	}

	VkDescriptorPoolSize pool_size = {
		.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		.descriptorCount = 1,
	};
	VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 1,
		.pPoolSizes = &pool_size,
	};
	res = vkCreateDescriptorPool(dev, &pool_info, NULL, &arena->descriptor_pool);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(vk_renderer, "vkCreateDescriptorPool", res);
		goto error;
	}

	VkDescriptorSetAllocateInfo set_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = arena->descriptor_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &vk_renderer->gradient_ds_layout,
	};
	res = vkAllocateDescriptorSets(dev, &set_info, &arena->descriptor_set);
	if (res != VK_SUCCESS) {
		vk_renderer_handle_result(vk_renderer, "vkAllocateDescriptorSets", res);
		goto error;
	}

	VkDescriptorBufferInfo buffer_binding = {
		.buffer = arena->buffer,
		.offset = 0,
		.range = GRADIENT_UBO_SIZE,
	};
	VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = arena->descriptor_set,
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		.pBufferInfo = &buffer_binding,
	};
	vkUpdateDescriptorSets(dev, 1, &write, 0, NULL);

	wl_list_insert(&pass->gradient_state->arenas, &arena->link);
	return arena;

error:
	gradient_arena_destroy(vk_renderer, arena);
	return NULL;
}

static void push_gradient_ubo(struct vk_frag_quad_grad_ubo *ubo,
		const struct fx_gradient *gradient) {
	ubo->params[0] = gradient->degree;
	ubo->params[1] = (float)gradient->linear;
	ubo->params[2] = (float)gradient->blend;
	ubo->params[3] = (float)gradient->count;

	ubo->range[0] = gradient->range.width;
	ubo->range[1] = gradient->range.height;
	ubo->range[2] = gradient->range.x;
	ubo->range[3] = gradient->range.y;

	ubo->origin[0] = gradient->origin[0];
	ubo->origin[1] = gradient->origin[1];
	ubo->origin[2] = 0.0f;
	ubo->origin[3] = 0.0f;

	// Gradient colours are given in sRGB, the shader converts them into linear
	// space (the render target is an sRGB image and vulkan does the reverse
	// conversion on write).
	const int count = gradient->count < MAX_GRADIENT_COLORS
		? gradient->count : MAX_GRADIENT_COLORS;
	if (gradient->count > MAX_GRADIENT_COLORS) {
		wlr_log(WLR_DEBUG, "Gradient has %d colors, only the first %d are used",
			gradient->count, MAX_GRADIENT_COLORS);
	}

	for (int i = 0; i < count; i++) {
		ubo->colors[i][0] = gradient->colors[i * 4 + 0];
		ubo->colors[i][1] = gradient->colors[i * 4 + 1];
		ubo->colors[i][2] = gradient->colors[i * 4 + 2];
		ubo->colors[i][3] = gradient->colors[i * 4 + 3];
	}
	memset(&ubo->colors[count], 0,
		sizeof(ubo->colors) - count * sizeof(ubo->colors[0]));
}

// Grab a slice of a host visible uniform buffer for one gradient definition and
// bind it. Gradients are drawn with a dynamic uniform buffer offset so a whole
// pass only needs one arena.
static bool bind_gradient(struct vk_render_pass *pass, VkPipelineLayout layout,
		const struct fx_gradient *gradient) {
	struct vk_gradient_arena *arena = NULL;
	if (!wl_list_empty(&pass->gradient_state->arenas)) {
		arena = wl_container_of(pass->gradient_state->arenas.prev, arena, link);
		if (arena->used + GRADIENT_UBO_STRIDE > arena->size) {
			arena = NULL;
		}
	}

	if (arena == NULL) {
		arena = create_gradient_arena(pass);
		if (arena == NULL) {
			return false;
		}
	}

	struct vk_frag_quad_grad_ubo *ubo = (struct vk_frag_quad_grad_ubo *)
		((uint8_t *)arena->mapped + arena->used);
	push_gradient_ubo(ubo, gradient);

	const uint32_t dynamic_offset = arena->used;
	arena->used += GRADIENT_UBO_STRIDE;

	vkCmdBindDescriptorSets(pass->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
		layout, 0, 1, &arena->descriptor_set, 1, &dynamic_offset);

	return true;
}

// Shared draw path for both gradient variants. The only difference is whether
// the rounded corners test is compiled in (fx_quad_shader_effects).
static void vk_render_pass_add_grad_common(struct vk_render_pass *pass,
		const struct wlr_render_rect_options *options,
		const struct fx_gradient *gradient, enum fx_quad_shader_effects effects,
		const struct fx_corner_fradii *corners,
		const struct clipped_fregion *clipped_region) {
	struct vk_renderer *vk_renderer = pass->vk_renderer;
	VkCommandBuffer cb = pass->command_buffer;

	struct wlr_box box;
	wlr_render_rect_options_get_box(options, pass->render_buffer->wlr_buffer, &box);

	pixman_region32_t clip_region;
	get_clip_region(pass, options->clip, &clip_region);
	if (clipped_region != NULL) {
		apply_clip_region(&clip_region, &clipped_region->area, &clipped_region->corners);
	}

	struct vk_pipeline *pipeline = get_vk_quad_grad_pipeline(
		pass->render_setup->vk_pipelines.quad_grad, effects);
	if (pipeline == NULL) {
		pixman_region32_fini(&clip_region);
		return;
	}

	VkPipelineLayout layout = vk_renderer->shader_info.quad_grad.pipeline_layout;

	float proj[9], matrix[9];
	wlr_matrix_identity(proj);
	wlr_matrix_project_box(matrix, &box, WL_OUTPUT_TRANSFORM_NORMAL, proj);
	wlr_matrix_multiply(matrix, pass->projection, matrix);

	// The base colour's alpha fades the gradient out at the corners (see
	// mix(vec4(0.0), gradient(...), rect_alpha) in the gles2 shader).
	const float alpha = 1.0f;

	struct vk_vert_pcr_data vert_pcr_data = {
		.uv_off = { 0, 0 },
		.uv_size = { 1, 1 },
	};
	encode_proj_matrix(matrix, vert_pcr_data.mat4);

	struct vk_frag_quad_pcr_data quad_pcr_data = {
		.color = { alpha, alpha, alpha, alpha },
	};
	if (corners != NULL) {
		push_rounding_data(&quad_pcr_data.corner_rounding, &box, corners);
	}
	if (clipped_region != NULL) {
		push_rounding_data(&quad_pcr_data.clipping,
			&clipped_region->area, &clipped_region->corners);
	}

	if (!bind_gradient(pass, layout, gradient)) {
		pixman_region32_fini(&clip_region);
		return;
	}

	bind_pipeline(pass, pipeline->pipeline);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT,
		0, sizeof(vert_pcr_data), &vert_pcr_data);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT,
		sizeof(vert_pcr_data), sizeof(quad_pcr_data), &quad_pcr_data);

	draw_clip_region(pass, &clip_region, &box);

	pixman_region32_fini(&clip_region);
}

static void vk_render_pass_add_rect_grad(struct fx_render_pass *fx_pass,
		const struct fx_render_rect_grad_options *fx_options) {
	struct vk_render_pass *pass = vk_get_render_pass(fx_pass);

	vk_render_pass_add_grad_common(pass, &fx_options->base, &fx_options->gradient,
		SHADER_QUAD_EFFECT_NONE, NULL, NULL);
}

static void vk_render_pass_add_rounded_rect_grad(struct fx_render_pass *fx_pass,
		const struct fx_render_rounded_rect_grad_options *fx_options) {
	struct vk_render_pass *pass = vk_get_render_pass(fx_pass);

	enum fx_quad_shader_effects effects = SHADER_QUAD_EFFECT_NONE;
	if (!fx_corner_fradii_is_empty(&fx_options->corners)) {
		effects |= SHADER_QUAD_EFFECT_ROUND_CORNERS;
	}

	vk_render_pass_add_grad_common(pass, &fx_options->base, &fx_options->gradient,
		effects, &fx_options->corners, NULL);
}

static void vk_render_pass_add_box_shadow(struct fx_render_pass *fx_pass,
		const struct fx_render_box_shadow_options *options) {
	// A lost device rejects everything, so record nothing and let the
	// compositor's listener take over. vk_render_pass_init() is what actually
	// reports the loss; the pass it returns is the first thing to notice.
	if (vk_renderer_device_lost(vk_get_render_pass(fx_pass)->vk_renderer)) {
		return;
	}

	TRACY_GPU_ZONE_START(fx_pass->fx_renderer);

	struct vk_render_pass *pass = vk_get_render_pass(fx_pass);
	struct vk_renderer *vk_renderer = pass->vk_renderer;
	VkCommandBuffer cb = pass->command_buffer;

	struct wlr_box box = options->box;
	assert(box.width > 0 && box.height > 0);

	// A shadow without blur is just a rounded rect.
	if (options->blur_sigma <= 0.0f) {
		const float radius = options->corner_radius;
		fx_render_pass_add_rounded_rect(fx_pass,
			&(struct fx_render_rounded_rect_options){
				.base = {
					.box = box,
					.color = options->color,
					.clip = options->clip,
				},
				.corners = {
					.top_left = radius,
					.top_right = radius,
					.bottom_left = radius,
					.bottom_right = radius,
				},
				.clipped_region = options->clipped_region,
			});
		TRACY_GPU_ZONE_END;
		return;
	}

	pixman_region32_t clip_region;
	get_clip_region(pass, options->clip, &clip_region);
	apply_clip_region(&clip_region,
		&options->clipped_region.area, &options->clipped_region.corners);

	const bool should_clip = clipped_fregion_is_valid(&options->clipped_region);

	enum fx_quad_shader_effects effects = SHADER_QUAD_EFFECT_NONE;
	if (should_clip) {
		effects |= SHADER_QUAD_EFFECT_CLIPPING;
	}

	struct vk_pipeline *pipeline = get_vk_box_shadow_pipeline(
		pass->render_setup->vk_pipelines.box_shadow, effects);
	if (pipeline == NULL) {
		pixman_region32_fini(&clip_region);
		TRACY_GPU_ZONE_END;
		return;
	}

	VkPipelineLayout layout = vk_renderer->shader_info.box_shadow.pipeline_layout;

	float proj[9], matrix[9];
	wlr_matrix_identity(proj);
	wlr_matrix_project_box(matrix, &box, WL_OUTPUT_TRANSFORM_NORMAL, proj);
	wlr_matrix_multiply(matrix, pass->projection, matrix);

	struct vk_vert_pcr_data vert_pcr_data = {
		.uv_off = { 0, 0 },
		.uv_size = { 1, 1 },
	};
	encode_proj_matrix(matrix, vert_pcr_data.mat4);

	struct vk_frag_box_shadow_pcr_data shadow_pcr_data = {
		.base = {
			.color = {
				options->color.r,
				options->color.g,
				options->color.b,
				options->color.a,
			},
		},
		.shadow_params = {
			options->blur_sigma,
			options->corner_radius,
			0.0f,
			0.0f,
		},
	};

	// The shader works in unrounded box space, using the box position/size to
	// compute the rounded box shadow at each fragment.
	push_rounding_data(&shadow_pcr_data.base.corner_rounding, &box,
		&(struct fx_corner_fradii){
			.top_left = options->corner_radius,
			.top_right = options->corner_radius,
			.bottom_left = options->corner_radius,
			.bottom_right = options->corner_radius,
		});
	push_rounding_data(&shadow_pcr_data.base.clipping,
		&options->clipped_region.area, &options->clipped_region.corners);

	bind_pipeline(pass, pipeline->pipeline);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT,
		0, sizeof(vert_pcr_data), &vert_pcr_data);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT,
		sizeof(vert_pcr_data), sizeof(shadow_pcr_data), &shadow_pcr_data);

	draw_clip_region(pass, &clip_region, &box);

	pixman_region32_fini(&clip_region);
	TRACY_GPU_ZONE_END;
}

// Records one dual-Kawase step: samples src_ds into dst, over the scissor rects
// in `region`. Runs inside its own effect render pass, so it must not be called
// while the scene render pass is active.
static void render_blur_step(struct vk_render_pass *pass,
		struct vk_effect_image *dst, VkDescriptorSet src_ds,
		struct vk_pipeline_blur *blur_pipeline, const pixman_region32_t *region,
		float radius, bool downsample, int src_level) {
	struct vk_renderer *renderer = pass->vk_renderer;
	VkCommandBuffer cb = pass->command_buffer;

	int rects_len;
	const pixman_box32_t *rects = pixman_region32_rectangles(region, &rects_len);
	if (rects_len == 0) {
		return;
	}

	// The scene render pass declares initialLayout COLOR_ATTACHMENT_OPTIMAL, so
	// the target has to be moved there first (it is UNDEFINED before first use,
	// SHADER_READ_ONLY_OPTIMAL after any previous chain step).
	vk_effect_image_prepare_target(cb, dst);

	VkRenderPassBeginInfo rp_info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderArea = (VkRect2D){ .extent = { dst->width, dst->height } },
		.clearValueCount = 0,
		.renderPass = renderer->effect_render_pass,
		.framebuffer = dst->framebuffer,
	};
	vkCmdBeginRenderPass(cb, &rp_info, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdSetViewport(cb, 0, 1, &(VkViewport){
		.width = dst->width,
		.height = dst->height,
		.maxDepth = 1,
	});

	// common.vert emits a unit quad in [0,1]^2, so this maps it across the whole
	// target in NDC. The scissor, not the geometry, selects the scaled region.
	float proj[9] = {
		2, 0, -1,
		0, 2, -1,
		0, 0,  1,
	};
	struct vk_vert_pcr_data vert_pcr_data = {
		.uv_off = { 0, 0 },
		.uv_size = { 1, 1 },
	};
	encode_proj_matrix(proj, vert_pcr_data.mat4);

	// Downsample reads from a texture twice the size of the area being written
	// (blur1.frag does uv * 2.0); upsample the other way around.
	float scale = downsample ? 2.0f : 0.5f;
	// The valid source area is the top-left (1 / 2^level) of the image: the
	// blur chain keeps its content there and leaves the rest untouched, so a tap
	// outside it would average in whatever the image previously held. Clamp to
	// the texel centres of that area and extend the edge instead, exactly as
	// GLES2's uv_min/uv_max do in blur1.frag and blur2.frag. It matters most for
	// blur2, which reads a region twice as wide as it writes.
	float tex_width = dst->width;
	float tex_height = dst->height;
	// Clamp every tap to the area the previous step actually wrote. The chain
	// keeps its content in a sub-region of each image and leaves the rest
	// untouched, so a tap outside it averages in whatever the image held
	// before. GLES2 avoids this by allocating a separate buffer per level, so
	// its texture->width IS the level size; scenefx's effect images are all
	// full output size, so the valid area has to come from the region instead.
	// `region` is what this step WRITES; what it READS is `scale` times that,
	// since blur1 samples twice what it writes (uv * 2.0) and blur2 half
	// (uv / 2.0).
	pixman_box32_t *extents =
		pixman_region32_extents((pixman_region32_t *)region);
	float src_x1 = extents->x1 * scale;
	float src_y1 = extents->y1 * scale;
	float src_x2 = extents->x2 * scale;
	float src_y2 = extents->y2 * scale;
	(void)src_level;

	struct vk_frag_blur_pcr_data blur_pcr_data = {
		.halfpixel = {
			0.5f / (tex_width / scale),
			0.5f / (tex_height / scale),
		},
		.radius = radius,
		.uv_min = {
			(src_x1 + 0.5f) / tex_width,
			(src_y1 + 0.5f) / tex_height,
		},
		.uv_max = {
			(src_x2 - 0.5f) / tex_width,
			(src_y2 - 0.5f) / tex_height,
		},
	};

	VkPipelineLayout layout = blur_pipeline->pipeline_layout;
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
		blur_pipeline->pipeline.pipeline);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
		0, 1, &src_ds, 0, NULL);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT,
		0, sizeof(vert_pcr_data), &vert_pcr_data);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT,
		sizeof(vert_pcr_data), sizeof(blur_pcr_data), &blur_pcr_data);

	for (int i = 0; i < rects_len; i++) {
		VkRect2D rect;
		convert_pixman_box_to_vk_rect(&rects[i], &rect);
		vkCmdSetScissor(cb, 0, 1, &rect);
		vkCmdDraw(cb, 4, 1, 0, 0);
	}

	vkCmdEndRenderPass(cb);

	// The scene pass' pipeline cache no longer reflects what is bound.
	wlr_vk_render_pass_reset_pipeline(pass->fx_render_pass.render_pass);
	pass->bound_pipeline = VK_NULL_HANDLE;
}

static void vk_render_pass_add_blur(struct fx_render_pass *fx_pass,
		const struct fx_render_blur_pass_options *fx_options) {
	// A lost device rejects everything, so record nothing and let the
	// compositor's listener take over. vk_render_pass_init() is what actually
	// reports the loss; the pass it returns is the first thing to notice.
	if (vk_renderer_device_lost(vk_get_render_pass(fx_pass)->vk_renderer)) {
		return;
	}

	TRACY_GPU_ZONE_START(fx_pass->fx_renderer);

	struct vk_render_pass *pass = vk_get_render_pass(fx_pass);
	struct vk_renderer *renderer = pass->vk_renderer;

	struct blur_data blur_data =
		blur_data_apply_strength(fx_options->blur_data, fx_options->blur_strength);
	if (fx_options->blur_strength <= 0 || !is_scene_blur_enabled(&blur_data)) {
		TRACY_GPU_ZONE_END;
		return;
	}

	struct wlr_buffer *buffer = pass->render_buffer->wlr_buffer;

	// Only the two-pass (blending buffer) pathway can be suspended, which is
	// also the pathway colour-managed/HDR output always uses. Elsewhere there is
	// no intermediate image to sample, so blur is skipped rather than corrupting
	// the frame.
	VkImageView blend_view =
		wlr_vk_render_pass_get_blend_image_view(fx_pass->render_pass);
	if (blend_view == VK_NULL_HANDLE) {
		TRACY_GPU_ZONE_END;
		return;
	}
	// Effect framebuffers must be built against the scene render pass so the
	// blur pipelines stay compatible with them.
	VkRenderPass scene_pass = wlr_vk_render_pass_get_render_pass(fx_pass->render_pass);
	if (!vk_effect_images_ensure(renderer, scene_pass, buffer->width, buffer->height)) {
		TRACY_GPU_ZONE_END;
		return;
	}
	VkDescriptorSet blend_ds = vk_get_blend_ds(renderer, blend_view);
	if (blend_ds == VK_NULL_HANDLE) {
		TRACY_GPU_ZONE_END;
		return;
	}

	struct vk_pipelines *pipelines = &pass->render_setup->vk_pipelines;
	if (pipelines->blur1 == NULL || pipelines->blur2 == NULL ||
			pipelines->blur_effects == NULL) {
		TRACY_GPU_ZONE_END;
		return;
	}

	// Expand the damage so taps near the edge have valid neighbours, then clamp
	// it to the buffer.
	pixman_region32_t damage;
	pixman_region32_init(&damage);
	if (fx_options->tex_options.base.clip != NULL) {
		pixman_region32_copy(&damage, fx_options->tex_options.base.clip);
	} else {
		pixman_region32_union_rect(&damage, &damage, 0, 0,
			buffer->width, buffer->height);
	}
	wlr_region_expand(&damage, &damage, blur_data_calc_size(&blur_data));
	pixman_region32_intersect_rect(&damage, &damage, 0, 0,
		buffer->width, buffer->height);

	if (!pixman_region32_not_empty(&damage)) {
		pixman_region32_fini(&damage);
		TRACY_GPU_ZONE_END;
		return;
	}

	// GLES2 masks the blur to the window's non-transparent pixels with a stencil.
	// This render pass has no stencil attachment, so the mask is sampled by
	// blur_effects instead: fetch the image now, while a failure can still bail
	// out before the pass is suspended.
	VkImageView mask_view = VK_NULL_HANDLE;
	VkImageLayout mask_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	bool has_mask = false;
	if (fx_options->ignore_transparent &&
			fx_options->tex_options.base.texture != NULL) {
		struct wlr_texture *mask_tex = fx_options->tex_options.base.texture;

		// Register the client's explicit sync point even though wlroots' own
		// descriptor set cannot be used: this call is the only way the pass
		// learns it must wait before this image is read.
		enum wlr_color_transfer_function mask_tf =
			fx_options->tex_options.base.transfer_function;
		if (mask_tf == 0) {
			mask_tf = WLR_COLOR_TRANSFER_FUNCTION_GAMMA22;
		}
		VkDescriptorSet sync_ds =
			wlr_vk_render_pass_get_texture_ds(pass->fx_render_pass.render_pass,
				mask_tex, fx_options->tex_options.base.filter_mode, mask_tf,
				fx_options->tex_options.base.wait_timeline,
				fx_options->tex_options.base.wait_point);
		if (sync_ds == VK_NULL_HANDLE) {
			wlr_log(WLR_ERROR, "Failed to register the blur transparency mask texture");
			pixman_region32_fini(&damage);
			TRACY_GPU_ZONE_END;
			return;
		}

		struct wlr_vk_image_attribs attribs;
		wlr_vk_texture_get_image_attribs(mask_tex, &attribs);
		mask_view = vk_mask_view_get(renderer, attribs.image, attribs.format,
			&mask_layout);
		if (mask_view == VK_NULL_HANDLE) {
			wlr_log(WLR_ERROR, "Failed to create a view of the blur transparency mask");
			pixman_region32_fini(&damage);
			TRACY_GPU_ZONE_END;
			return;
		}
		has_mask = true;
	}

	if (!wlr_vk_render_pass_suspend(fx_pass->render_pass)) {
		pixman_region32_fini(&damage);
		TRACY_GPU_ZONE_END;
		return;
	}


	pixman_region32_t scaled_damage;
	pixman_region32_init(&scaled_damage);

	// Ping-pong between the two effect images; the first read is the blend image
	// holding everything drawn so far.
	VkDescriptorSet src_ds = blend_ds;
	size_t dst_index = 0;

	for (int i = 0; i < blur_data.num_passes; i++) {
		wlr_region_scale(&scaled_damage, &damage, 1.0f / (1 << (i + 1)));
		render_blur_step(pass, &renderer->effect_images[dst_index], src_ds,
			pipelines->blur1, &scaled_damage, blur_data.radius, true, i);
		src_ds = renderer->effect_images[dst_index].ds;
		dst_index ^= 1;
	}

	for (int i = blur_data.num_passes - 1; i >= 0; i--) {
		wlr_region_scale(&scaled_damage, &damage, 1.0f / (1 << i));
		render_blur_step(pass, &renderer->effect_images[dst_index], src_ds,
			pipelines->blur2, &scaled_damage, blur_data.radius, false, i + 1);
		src_ds = renderer->effect_images[dst_index].ds;
		dst_index ^= 1;
	}

	pixman_region32_fini(&scaled_damage);

	// dst_index was advanced past the last write; step back to the image that
	// actually holds the blurred result.
	size_t result_index = dst_index ^ 1;

	if (!wlr_vk_render_pass_resume(fx_pass->render_pass)) {
		pixman_region32_fini(&damage);
		TRACY_GPU_ZONE_END;
		return;
	}

	// Composite the blurred image back into the scene with blur_effects, which
	// samples texture(tex, uv) 1:1 and applies the brightness/contrast/
	// saturation/noise stage — the same shader GLES2 finishes with.
	//
	// blur2 must NOT be used here even with zeroed constants: it is the upsample
	// shader and does `suv = uv / 2.0`, which magnifies the top-left quarter of
	// the blurred image over the whole region.
	VkCommandBuffer cb = pass->command_buffer;
	float proj[9] = {
		2, 0, -1,
		0, 2, -1,
		0, 0,  1,
	};
	struct vk_vert_pcr_data vert_pcr_data = {
		.uv_off = { 0, 0 },
		.uv_size = { 1, 1 },
	};
	encode_proj_matrix(proj, vert_pcr_data.mat4);
	struct vk_frag_blur_effects_pcr_data effects_pcr_data = {
		.brightness = blur_data.brightness,
		.contrast = blur_data.contrast,
		.saturation = blur_data.saturation,
		.noise = blur_data.noise,
	};
	// Round the composite to the node's shape. The blur node's corners live on
	// tex_options (the top-level fx_render_blur_pass_options.corners is not set
	// by the scene), and its box is the tex_options destination box. Without
	// this a rounded blur node renders with square corners.
	push_rounding_data(&effects_pcr_data.corner_rounding,
		&fx_options->tex_options.base.dst_box, &fx_options->tex_options.corners);

	// Map the shader's uv onto the mask texture.
	//
	// blur_effects' uv spans the whole output (the composite quad is sized by
	// the projection matrix, not by the blur node), while the mask only covers
	// the node's destination box. So the mapping is the affine composition of
	// "uv in the output" -> "position in the dst box" -> "texcoord in the mask":
	//
	//   mask_uv = src_offset - dst_offset/dst_size * src_size/output
	//             + uv * (output/dst_size * src_size)
	//
	// Both halves are pre-multiplied here so the shader is a single mad.
	effects_pcr_data.has_mask = has_mask ? 1.0f : 0.0f;
	effects_pcr_data.ignore_alpha_threshold = fx_options->ignore_alpha_threshold;
	if (has_mask) {
		struct wlr_texture *mask_tex = fx_options->tex_options.base.texture;
		struct wlr_fbox src_fbox;
		wlr_render_texture_options_get_src_box(
			&fx_options->tex_options.base, &src_fbox);
		const struct wlr_box *dst_box = &fx_options->tex_options.base.dst_box;
		// A zero dst box means "the whole texture", which is also what the
		// mask's geometry would have been.
		struct wlr_box dst = *dst_box;
		if (wlr_box_empty(&dst)) {
			dst = (struct wlr_box){ 0, 0, buffer->width, buffer->height };
		}

		float src_size[2] = {
			src_fbox.width / mask_tex->width,
			src_fbox.height / mask_tex->height,
		};
		float src_off[2] = {
			src_fbox.x / mask_tex->width,
			src_fbox.y / mask_tex->height,
		};
		for (int i = 0; i < 2; i++) {
			float out_size = i == 0 ? buffer->width : buffer->height;
			float dst_off = i == 0 ? dst.x : dst.y;
			float dst_size = i == 0 ? dst.width : dst.height;
			if (dst_size <= 0) {
				dst_size = out_size;
			}
			effects_pcr_data.mask_uv_size[i] =
				(out_size / dst_size) * src_size[i];
			effects_pcr_data.mask_uv[i] = src_off[i]
				- (dst_off / dst_size) * src_size[i];
		}
}

	VkPipelineLayout layout = pipelines->blur_effects->pipeline_layout;
	VkDescriptorSet result_ds = renderer->effect_images[result_index].ds;
	// Composite only where the caller asked for blur. `damage` is deliberately
	// wider (expanded so edge taps have neighbours) and must NOT be used here:
	// painting the blurred image across the expanded area covers unrelated
	// content, and because the next frame blurs the blend image again that
	// feeds back into itself and smears the whole output.
	pixman_region32_t composite_clip;
	pixman_region32_init(&composite_clip);
	if (fx_options->tex_options.base.clip != NULL) {
		pixman_region32_copy(&composite_clip, fx_options->tex_options.base.clip);
	} else {
		pixman_region32_union_rect(&composite_clip, &composite_clip, 0, 0,
			buffer->width, buffer->height);
	}

	// The effect image's own descriptor set only ever has binding 0 written, so
	// the mask gets written per frame. When there is no mask, the dummy opaque
	// image goes in, keeping the shader's fetch well defined.
	if (!vk_tex_ds_set_mask(renderer, result_ds,
			has_mask ? mask_view : VK_NULL_HANDLE, mask_layout)) {
		// Cannot happen for a live device; bail rather than draw unmasked.
		wlr_log(WLR_ERROR, "Failed to bind the blur transparency mask");
		wlr_vk_render_pass_reset_pipeline(fx_pass->render_pass);
		pass->bound_pipeline = VK_NULL_HANDLE;
		pixman_region32_fini(&composite_clip);
		pixman_region32_fini(&damage);
		TRACY_GPU_ZONE_END;
		return;
	}
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
		pipelines->blur_effects->pipeline.pipeline);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
		0, 1, &result_ds, 0, NULL);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT,
		0, sizeof(vert_pcr_data), &vert_pcr_data);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT,
		sizeof(vert_pcr_data), sizeof(effects_pcr_data), &effects_pcr_data);

	int rects_len;
	const pixman_box32_t *rects =
		pixman_region32_rectangles(&composite_clip, &rects_len);
	for (int i = 0; i < rects_len; i++) {
		VkRect2D rect;
		convert_pixman_box_to_vk_rect(&rects[i], &rect);
		vkCmdSetScissor(cb, 0, 1, &rect);
		vkCmdDraw(cb, 4, 1, 0, 0);
	}

	wlr_vk_render_pass_reset_pipeline(fx_pass->render_pass);
	pass->bound_pipeline = VK_NULL_HANDLE;

	pixman_region32_fini(&composite_clip);
	pixman_region32_fini(&damage);
	TRACY_GPU_ZONE_END;
}

static bool vk_render_pass_add_optimized_blur(struct fx_render_pass *fx_pass,
		const struct fx_render_blur_pass_options *fx_options) {
	// A lost device rejects everything, so record nothing and let the
	// compositor's listener take over. vk_render_pass_init() is what actually
	// reports the loss; the pass it returns is the first thing to notice.
	if (vk_renderer_device_lost(vk_get_render_pass(fx_pass)->vk_renderer)) {
		return false;
	}

	// TODO: optimized blur. Return false so the scene keeps the node dirty and
	// re-tries it, which is the same thing a failure does; the zone is closed
	// here rather than left open, so GPU timestamps stay balanced.
	TRACY_GPU_ZONE_START(fx_pass->fx_renderer);
	TRACY_GPU_ZONE_END;
	return false;
}

///
/// Textures
///

// We cannot ask wlroots directly whether it will pick the mutable-sRGB image
// view for a texture, but it does expose the view's format, and it only ever
// swaps to an _SRGB format when the texture supports mutable sRGB.
static bool texture_uses_srgb_view(struct wlr_texture *texture) {
	struct wlr_vk_image_attribs attribs;
	wlr_vk_texture_get_image_attribs(texture, &attribs);

	switch (attribs.format) {
	case VK_FORMAT_R8_SRGB:
	case VK_FORMAT_R8G8_SRGB:
	case VK_FORMAT_R8G8B8_SRGB:
	case VK_FORMAT_B8G8R8_SRGB:
	case VK_FORMAT_R8G8B8A8_SRGB:
	case VK_FORMAT_B8G8R8A8_SRGB:
	case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
		return true;
	default:
		return false;
	}
}

// Mirrors the texture transform selection done by wlroots when it builds the
// texture view for a descriptor set, so our shader knows whether the hardware
// already did the sRGB conversion for us. The values must match the
// TEXTURE_TRANSFORM_* defines in tex.frag (and wlroots' private
// enum wlr_vk_texture_transform, which we cannot include from here).
enum vk_fx_texture_transform {
	VK_FX_TEXTURE_TRANSFORM_IDENTITY = 0,
	VK_FX_TEXTURE_TRANSFORM_SRGB = 1,
	VK_FX_TEXTURE_TRANSFORM_ST2084_PQ = 2,
	VK_FX_TEXTURE_TRANSFORM_GAMMA22 = 3,
	VK_FX_TEXTURE_TRANSFORM_BT1886 = 4,
};

static enum vk_fx_texture_transform
get_texture_transform(struct wlr_texture *texture, enum wlr_color_transfer_function tf,
		bool *srgb_image_view) {
	*srgb_image_view = false;

	switch (tf) {
	case WLR_COLOR_TRANSFER_FUNCTION_SRGB:
		if (texture_uses_srgb_view(texture)) {
			*srgb_image_view = true;
			return VK_FX_TEXTURE_TRANSFORM_IDENTITY;
		}
		return VK_FX_TEXTURE_TRANSFORM_SRGB;
	case WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR:
		return VK_FX_TEXTURE_TRANSFORM_IDENTITY;
	case WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ:
		return VK_FX_TEXTURE_TRANSFORM_ST2084_PQ;
	case WLR_COLOR_TRANSFER_FUNCTION_GAMMA22:
		return VK_FX_TEXTURE_TRANSFORM_GAMMA22;
	case WLR_COLOR_TRANSFER_FUNCTION_BT1886:
		return VK_FX_TEXTURE_TRANSFORM_BT1886;
	}

	return VK_FX_TEXTURE_TRANSFORM_IDENTITY;
}

static struct vk_pipeline_tex *get_tex_pipeline(struct vk_render_pass *pass,
		struct wlr_texture *texture, enum wlr_color_transfer_function tf,
		enum fx_tex_shader_effects effects, enum wlr_scale_filter_mode filter_mode,
		enum vk_fx_texture_transform transform, bool srgb) {
	struct vk_render_setup *setup = pass->render_setup;

	struct vk_pipeline_tex *pipeline;
	wl_list_for_each(pipeline, &setup->tex_pipelines, link) {
		if (pipeline->effects == effects && pipeline->filter_mode == filter_mode
				&& pipeline->texture_transform == transform) {
			return pipeline;
		}
	}

	pipeline = calloc(1, sizeof(*pipeline));
	if (pipeline == NULL) {
		wlr_log_errno(WLR_ERROR, "Failed to allocate vk texture pipeline");
		return NULL;
	}

	// We're drawing through our own pipeline layout, but we reuse wlroots'
	// descriptor set layout (and the descriptor sets it produces) so the sampler
	// and the texture view stay owned by wlroots.
	VkDescriptorSetLayout ds_layout = wlr_vk_render_pass_get_texture_ds_layout(
		pass->fx_render_pass.render_pass, texture, filter_mode, tf);

	if (!create_vk_tex_pipeline(pass->vk_renderer, setup, effects, filter_mode,
			transform, srgb, ds_layout, pipeline)) {
		free(pipeline);
		return NULL;
	}

	return pipeline;
}

static void vk_render_pass_add_texture(struct fx_render_pass *fx_pass,
		const struct fx_render_texture_options *fx_options) {
	const struct wlr_render_texture_options *options = &fx_options->base;
	struct vk_render_pass *pass = vk_get_render_pass(fx_pass);
	VkCommandBuffer cb = pass->command_buffer;

	struct wlr_fbox src_fbox;
	wlr_render_texture_options_get_src_box(options, &src_fbox);
	struct wlr_box dst_box;
	wlr_render_texture_options_get_dst_box(options, &dst_box);
	float alpha = wlr_render_texture_options_get_alpha(options);

	const struct wlr_box *clip_box = &dst_box;
	if (!wlr_box_empty(fx_options->clip_box)) {
		clip_box = fx_options->clip_box;
	}

	float proj[9], matrix[9];
	wlr_matrix_identity(proj);
	wlr_matrix_project_box(matrix, &dst_box, options->transform, proj);
	wlr_matrix_multiply(matrix, pass->projection, matrix);

	struct vk_vert_pcr_data vert_pcr_data = {
		.uv_off = {
			src_fbox.x / options->texture->width,
			src_fbox.y / options->texture->height,
		},
		.uv_size = {
			src_fbox.width / options->texture->width,
			src_fbox.height / options->texture->height,
		},
	};
	encode_proj_matrix(matrix, vert_pcr_data.mat4);

	enum wlr_color_transfer_function tf = options->transfer_function;
	if (tf == 0) {
		tf = WLR_COLOR_TRANSFER_FUNCTION_GAMMA22;
	}

	bool srgb_image_view = false;
	enum vk_fx_texture_transform transform =
		get_texture_transform(options->texture, tf, &srgb_image_view);

	const bool should_round_corners = !fx_corner_fradii_is_empty(&fx_options->corners);
	const bool should_clip = clipped_fregion_is_valid(&fx_options->clipped_region);

	enum fx_tex_shader_effects effects = SHADER_TEXTURE_EFFECT_NONE;
	if (should_round_corners) {
		effects |= SHADER_TEXTURE_EFFECT_ROUND_CORNERS;
	}
	if (should_clip) {
		effects |= SHADER_TEXTURE_EFFECT_CLIPPING;
	}
	if (fx_options->discard_transparent) {
		effects |= SHADER_TEXTURE_EFFECT_DISCARD_TRANSPARENT;
	}

	struct vk_pipeline_tex *pipeline = get_tex_pipeline(pass, options->texture, tf,
		effects, options->filter_mode, transform, srgb_image_view);
	if (pipeline == NULL) {
		wlr_log(WLR_ERROR, "Failed to get a vulkan texture pipeline");
		return;
	}

	// Pass the client's explicit sync point along: scenefx records its own
	// texture draws, so this call is the only way the wlroots pass learns it
	// must wait for the buffer's timeline before reading the texture. Dropping
	// it here would silently ignore explicit sync for every scene buffer.
	VkDescriptorSet ds = wlr_vk_render_pass_get_texture_ds(pass->fx_render_pass.render_pass,
		options->texture, options->filter_mode, tf,
		options->wait_timeline, options->wait_point);
	if (ds == VK_NULL_HANDLE) {
		wlr_log(WLR_ERROR, "Failed to get a vulkan texture descriptor set");
		return;
	}

	struct vk_frag_tex_pcr_data tex_pcr_data = {
		.params = {
			alpha,
			fx_options->discard_transparent ? 1.0f : 0.0f,
			fx_options->ignore_alpha_threshold,
			0.0f,
		},
	};
	push_rounding_data(&tex_pcr_data.corner_rounding, clip_box, &fx_options->corners);
	push_rounding_data(&tex_pcr_data.clipping,
		&fx_options->clipped_region.area, &fx_options->clipped_region.corners);

	pixman_region32_t clip_region;
	get_clip_region(pass, options->clip, &clip_region);
	apply_clip_region(&clip_region,
		&fx_options->clipped_region.area, &fx_options->clipped_region.corners);

	bind_pipeline(pass, pipeline->pipeline);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
		pipeline->pipeline_layout, 0, 1, &ds, 0, NULL);
	vkCmdPushConstants(cb, pipeline->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
		0, sizeof(vert_pcr_data), &vert_pcr_data);
	vkCmdPushConstants(cb, pipeline->pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
		sizeof(vert_pcr_data), sizeof(tex_pcr_data), &tex_pcr_data);

	draw_clip_region(pass, &clip_region, &dst_box);

	pixman_region32_fini(&clip_region);
}

static const struct fx_render_pass_impl render_pass_impl = {
	.destroy = vk_render_pass_destroy,
	.add_texture = vk_render_pass_add_texture,
	.add_rect = vk_render_pass_add_rect,
	.add_rect_grad = vk_render_pass_add_rect_grad,
	.add_rounded_rect = vk_render_pass_add_rounded_rect,
	.add_rounded_rect_grad = vk_render_pass_add_rounded_rect_grad,
	.add_box_shadow = vk_render_pass_add_box_shadow,
	.add_blur = vk_render_pass_add_blur,
	.add_optimized_blur = vk_render_pass_add_optimized_blur,
	.read_to_buffer = vk_render_pass_read_to_buffer,
	.save_blur_region = vk_render_pass_save_blur_region,
	.apply_saved_blur_region = vk_render_pass_apply_saved_blur_region,
};

// Called by wlroots once the command buffer has finished executing, so every
// resource recorded into it can be dropped now.
static void handle_release_resources(void *user_data) {
	struct vk_gradient_state *state = user_data;
	gradient_state_unref(state->vk_renderer, state);
}

struct fx_render_pass *vk_render_pass_init(struct fx_renderer *fx_renderer,
		struct wlr_render_pass *render_pass, struct wlr_buffer *wlr_buffer,
		struct wlr_output *output) {
	if (fx_renderer == NULL || render_pass == NULL || wlr_buffer == NULL
			|| output == NULL) {
		wlr_log(WLR_DEBUG, "Could not create vk_render_pass, NULL parameters");
		return NULL;
	}
	struct vk_renderer *vk_renderer = vk_get_renderer(fx_renderer);

	// A lost device is reported here rather than from the call that noticed it:
	// listeners are free to destroy the renderer, and at this point nothing of
	// ours is live, so the loss cannot leave us holding freed memory. The report
	// is consumed so that later frames, which still find the device lost, do not
	// make the compositor tear down and recreate the renderer over and over.
	if (vk_renderer_take_device_lost(vk_renderer)) {
		wl_signal_emit_mutable(&fx_renderer->wlr_renderer->events.lost, NULL);
		return NULL;
	}

	struct vk_render_pass *pass = calloc(1, sizeof(*pass));
	if (pass == NULL) {
		wlr_log(WLR_ERROR, "Failed to create vk_render_pass");
		return NULL;
	}

	fx_render_pass_init(&pass->fx_render_pass, &render_pass_impl, fx_renderer, render_pass);

	pass->vk_renderer = vk_renderer;

	pass->gradient_state = calloc(1, sizeof(*pass->gradient_state));
	if (pass->gradient_state == NULL) {
		wlr_log_errno(WLR_ERROR, "Failed to create the gradient state");
		goto gradient_state_fail;
	}
	wl_list_init(&pass->gradient_state->arenas);
	pass->gradient_state->refs = 1;
	pass->gradient_state->vk_renderer = vk_renderer;

	pass->render_buffer = vk_buffer_get_or_create(fx_renderer, wlr_buffer);
	if (pass->render_buffer == NULL) {
		wlr_log(WLR_ERROR, "Failed to get/create vk main buffer");
		goto buffer_get_fail;
	}

	// Collect the previous frame's GPU timestamps now: they were recorded into a
	// command buffer that has since been submitted and executed, whereas
	// collecting at pass-destroy time would read queries that are still queued.
	fx_render_pass_collect_gpu_zones(&pass->fx_render_pass);

	pass->command_buffer = wlr_vk_render_pass_get_command_buffer(render_pass);
	vk_renderer_set_tracy_command_buffer(vk_renderer, pass->command_buffer);
	pass->render_setup = vk_render_setup_find_or_create(pass->vk_renderer, render_pass);
	assert(pass->render_setup);

	pass->bound_pipeline = VK_NULL_HANDLE;

	// Save the same matrix projection that the wlroots vulkan pass uses
	matrix_projection(pass->projection, wlr_buffer->width, wlr_buffer->height,
		WL_OUTPUT_TRANSFORM_FLIPPED_180);

	// For per output framebuffers
	struct fx_offscreen_buffers *fx_offscreen_buffers =
		fx_offscreen_buffers_try_get(fx_renderer, output);
	pass->vk_offscreen_buffers = vk_get_offscreen_buffers(fx_offscreen_buffers);
	if (pass->vk_offscreen_buffers == NULL) {
		wlr_log(WLR_ERROR, "Failed to get/create effect framebuffers for output: %s",
				output->name);
		goto offscreen_buffers_get_create_fail;
	}

	// Update the buffers if needed
	const int width = pass->render_buffer->wlr_buffer->width;
	const int height = pass->render_buffer->wlr_buffer->height;
	bool failed = false;
	vk_buffer_get_or_allocate(fx_renderer, output->allocator, width, height, false,
			&pass->vk_offscreen_buffers->blur_saved_pixels_buffer, &failed);
	vk_buffer_get_or_allocate(fx_renderer, output->allocator, width, height, true,
			&pass->vk_offscreen_buffers->effects_buffer, &failed);
	vk_buffer_get_or_allocate(fx_renderer, output->allocator, width, height, true,
			&pass->vk_offscreen_buffers->effects_buffer_swapped, &failed);
	vk_buffer_get_or_allocate(fx_renderer, output->allocator, width, height, false,
			&pass->vk_offscreen_buffers->optimized_blur_buffer, &failed);
	vk_buffer_get_or_allocate(fx_renderer, output->allocator, width, height, false,
			&pass->vk_offscreen_buffers->optimized_no_blur_buffer, &failed);

	if (failed) {
		wlr_log(WLR_ERROR, "Failed to create effect framebuffers");
		goto offscreen_buffer_allocate_fail;
	}

	// One extra reference for the command buffer's resource callback: wlroots
	// calls it once the command buffer is recycled, long after the pass is
	// destroyed.
	wlr_vk_render_pass_set_resources_callback(render_pass, handle_release_resources,
		gradient_state_ref(pass->gradient_state));

	return &pass->fx_render_pass;

offscreen_buffer_allocate_fail:
	fx_offscreen_buffers_destroy(&pass->vk_offscreen_buffers->fx_offscreen_buffers);
offscreen_buffers_get_create_fail:
	vk_buffer_destroy(pass->render_buffer);
	pass->render_buffer = NULL;
buffer_get_fail:
	gradient_state_unref(vk_renderer, pass->gradient_state);
gradient_state_fail:
	free(pass);
	return NULL;
}
