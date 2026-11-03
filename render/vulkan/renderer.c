#include <assert.h>
#include <stdlib.h>
#include <vulkan/vulkan_core.h>
#include <wayland-util.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/vulkan.h>
#include <wlr/util/log.h>

#include "render/fx_renderer.h"
#include "render/vulkan/pipeline.h"
#include "render/vulkan/shaders.h"
#include "render/vulkan/vulkan.h"
// #include "render/vulkan/shaders.h"

static inline void free_shaders(struct vk_renderer *vk_renderer) {
	vk_shader_info_delete(vk_renderer, &vk_renderer->shader_info.quad);
	vk_shader_info_delete(vk_renderer, &vk_renderer->shader_info.quad_grad);
	vk_shader_info_delete(vk_renderer, &vk_renderer->shader_info.box_shadow);
	vk_shader_info_delete(vk_renderer, &vk_renderer->shader_info.tex);
	vk_shader_info_delete(vk_renderer, &vk_renderer->shader_info.blur1);
	vk_shader_info_delete(vk_renderer, &vk_renderer->shader_info.blur2);
	vk_shader_info_delete(vk_renderer, &vk_renderer->shader_info.blur_effects);

	// The effect images, mask views and saved-pixel snapshots are per output and
	// torn down with their output's offscreen buffers, in
	// offscreen_buffers_destroy(). The device is idle by now, so anything they
	// deferred is safe to free here.
	vk_deferred_destroy_flush(vk_renderer);

	// 1x1 stand-in mask, shared by every output since it is immutable.
	vk_dummy_mask_finish(vk_renderer);

	// The blur descriptor pool/layout/sampler must go after every descriptor set
	// allocated from them, which the effect images own.
	vk_tex_ds_layout_destroy(vk_renderer);

	if (vk_renderer->gradient_ds_layout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(vk_renderer->device,
			vk_renderer->gradient_ds_layout, NULL);
		vk_renderer->gradient_ds_layout = VK_NULL_HANDLE;
	}

	if (vk_renderer->shader_info.vert != VK_NULL_HANDLE) {
		vkDestroyShaderModule(vk_renderer->device, vk_renderer->shader_info.vert, NULL);
		vk_renderer->shader_info.vert = VK_NULL_HANDLE;
	}
}

static bool link_shaders(struct vk_renderer *renderer) {
	VkResult res;

	// Vertex shader
	if ((res = create_vk_vert_module(renderer)) != VK_SUCCESS) {
		wlr_vk_error("Failed to create vertex shader module", res);
		return false;
	}

	// quad fragment shader
	if (!vk_shader_info_create_quad(renderer)) {
		wlr_log(WLR_ERROR, "Could not link quad shaders");
		goto error;
	}

	// quad gradient fragment shader
	if (!vk_shader_info_create_quad_grad(renderer)) {
		wlr_log(WLR_ERROR, "Could not link quad gradient shaders");
		goto error;
	}

	// box shadow fragment shader
	if (!vk_shader_info_create_box_shadow(renderer)) {
		wlr_log(WLR_ERROR, "Could not link box shadow shaders");
		goto error;
	}

	// blur fragment shaders (downsample, upsample, effects composite)
	if (!vk_shader_info_create_blur1(renderer)) {
		wlr_log(WLR_ERROR, "Could not link blur1 shader");
		goto error;
	}
	if (!vk_shader_info_create_blur2(renderer)) {
		wlr_log(WLR_ERROR, "Could not link blur2 shader");
		goto error;
	}
	if (!vk_shader_info_create_blur_effects(renderer)) {
		wlr_log(WLR_ERROR, "Could not link blur_effects shader");
		goto error;
	}

	// tex fragment shader
	if (!vk_shader_info_create_tex(renderer)) {
		wlr_log(WLR_ERROR, "Could not link tex shaders");
		goto error;
	}
	return true;

error:
	free_shaders(renderer);
	return false;
}

static void offscreen_buffers_destroy(struct fx_offscreen_buffers *fx_offscreen_buffers) {
	struct vk_offscreen_buffers *vk_offscreen_buffers =
		vk_get_offscreen_buffers(fx_offscreen_buffers);
	struct vk_renderer *vk_renderer = vk_get_renderer(fx_offscreen_buffers->fx_renderer);

	// The effect targets, mask views and saved-pixel snapshot are VkImages owned
	// by this output, not wlr_buffers.
	vk_effect_images_finish(vk_offscreen_buffers, vk_renderer);
	vk_mask_view_finish(vk_offscreen_buffers, vk_renderer);
	vk_saved_pixels_finish(vk_offscreen_buffers, vk_renderer);
	vk_optimized_blur_finish(&vk_offscreen_buffers->optimized_blur, vk_renderer);

	free(vk_offscreen_buffers);
}

static const struct fx_offscreen_buffers_impl offscreen_buffers_impl = {
	.destroy = offscreen_buffers_destroy,
};

static struct fx_offscreen_buffers *offscreen_buffers_allocate(
		struct fx_renderer *fx_renderer, struct wlr_output *output) {
	struct vk_offscreen_buffers *vk_offscreen_buffers = calloc(1, sizeof(*vk_offscreen_buffers));
	if (vk_offscreen_buffers == NULL) {
		wlr_log_errno(WLR_ERROR, "vk_offscreen_buffers allocation failed");
		return NULL;
	}

	fx_offscreen_buffers_init(&vk_offscreen_buffers->fx_offscreen_buffers,
			&offscreen_buffers_impl, fx_renderer, output);
	return &vk_offscreen_buffers->fx_offscreen_buffers;
}

void vk_renderer_set_tracy_command_buffer(struct vk_renderer *renderer,
		VkCommandBuffer cb) {
	renderer->tracy_command_buffer = cb;
}

bool vk_renderer_device_lost(struct vk_renderer *renderer) {
	return renderer->device_lost;
}

bool vk_renderer_take_device_lost(struct vk_renderer *renderer) {
	if (!renderer->device_lost || renderer->device_lost_reported) {
		return false;
	}
	renderer->device_lost_reported = true;
	return true;
}

bool vk_renderer_handle_result(struct vk_renderer *renderer,
		const char *what, VkResult res) {
	if (res == VK_SUCCESS) {
		return false;
	}

	// Not wlr_vk_error(): it pastes its format argument into a literal, so it
	// only accepts a constant format string.
	wlr_log(WLR_ERROR, "%s: %s (%d)", what, vulkan_strerror(res), (int)res);

	if (res != VK_ERROR_DEVICE_LOST || renderer->device_lost) {
		// Not a loss, or one already reported. Repeating it would make the
		// compositor tear down and recreate the renderer again for a device
		// that is already gone.
		return renderer->device_lost;
	}

	renderer->device_lost = true;

	// No wl_signal_emit_mutable(&renderer->fx_renderer.wlr_renderer->events.lost, NULL)
	// here: a listener is free to destroy the renderer on the spot, which would
	// free this object mid-call. The loss is reported from vk_render_pass_init()
	// instead, where nothing of ours is live.
	return true;
}

static void renderer_destroy(struct fx_renderer *fx_renderer) {
	struct vk_renderer *vk_renderer = vk_get_renderer(fx_renderer);

	TRACY_GPU_CONTEXT_DESTROY(fx_renderer);

	struct vk_buffer *buffer, *buffer_tmp;
	wl_list_for_each_safe(buffer, buffer_tmp, &vk_renderer->buffers, link) {
		vk_buffer_destroy(buffer);
	}

	struct vk_render_setup *render_setup, *render_setup_tmp;
	wl_list_for_each_safe(render_setup, render_setup_tmp, &vk_renderer->render_setups, link) {
		vk_render_setup_destroy(vk_renderer, render_setup);
	}

	free_shaders(vk_renderer);

	free(vk_renderer);
}

#ifdef TRACY_ENABLE
extern const struct fx_renderer_tracy_impl vk_renderer_tracy_impl;
#endif

static const struct fx_renderer_impl renderer_impl = {
	.offscreen_buffers_allocate = offscreen_buffers_allocate,
	.render_pass_allocate = vk_render_pass_init,
	.renderer_destroy = renderer_destroy,

#ifdef TRACY_ENABLE
	.tracy = &vk_renderer_tracy_impl,
#endif
};

struct fx_renderer *vk_renderer_create(struct wlr_renderer *wlr_renderer) {
	struct vk_renderer *vk_renderer = calloc(1, sizeof(*vk_renderer));
	if (vk_renderer == NULL) {
		return NULL;
	}

	wl_list_init(&vk_renderer->buffers);
	wl_list_init(&vk_renderer->deferred_destroys);

	vk_renderer->device = wlr_vk_renderer_get_device(wlr_renderer);
	vk_renderer->physical_device = wlr_vk_renderer_get_physical_device(wlr_renderer);
	uint32_t queue_family = wlr_vk_renderer_get_queue_family(wlr_renderer);
	vkGetDeviceQueue(vk_renderer->device, queue_family, 0, &vk_renderer->queue);

	wl_list_init(&vk_renderer->render_setups);

	// Link all shaders
	if (!link_shaders(vk_renderer)) {
		goto shader_error;
	}

	fx_renderer_init(&vk_renderer->fx_renderer, &renderer_impl, wlr_renderer);
	wlr_log(WLR_INFO, "Vulkan FX RENDERER: Shaders Initialized Successfully");
	return &vk_renderer->fx_renderer;

shader_error:
	free(vk_renderer);
	return NULL;
}

static bool fx_renderer_is_vk(const struct fx_renderer *fx_renderer) {
	return fx_renderer->impl == &renderer_impl;
}

static bool fx_offscreen_buffers_is_vk(const struct fx_offscreen_buffers *offscreen_buffers) {
	return offscreen_buffers->impl == &offscreen_buffers_impl;
}

struct vk_renderer *vk_get_renderer(struct fx_renderer *fx_renderer) {
	assert(fx_renderer_is_vk(fx_renderer));
	struct vk_renderer *renderer = wl_container_of(fx_renderer, renderer, fx_renderer);
	return renderer;
}

struct vk_offscreen_buffers *vk_get_offscreen_buffers(
		struct fx_offscreen_buffers *offscreen_buffers) {
	assert(fx_renderer_is_vk(offscreen_buffers->fx_renderer));
	assert(fx_offscreen_buffers_is_vk(offscreen_buffers));
	struct vk_offscreen_buffers *offscreen
		= wl_container_of(offscreen_buffers, offscreen, fx_offscreen_buffers);
	return offscreen;
}

struct vk_render_pass *vk_get_render_pass(struct fx_render_pass *fx_render_pass) {
	assert(fx_renderer_is_vk(fx_render_pass->fx_renderer));
	struct vk_render_pass *pass = wl_container_of(fx_render_pass, pass, fx_render_pass);
	return pass;
}

struct vk_render_setup *vk_render_setup_find_or_create(struct vk_renderer *renderer,
		struct wlr_render_pass *render_pass) {
	const VkRenderPass vk_pass = wlr_vk_render_pass_get_render_pass(render_pass);

	struct vk_render_setup *setup;
	wl_list_for_each(setup, &renderer->render_setups, link) {
		if (setup->render_pass == vk_pass) {
			return setup;
		}
	}

	setup = calloc(1, sizeof(*setup));
	setup->render_pass = vk_pass;

	wl_list_init(&setup->pipelines);
	wl_list_init(&setup->tex_pipelines);
	vk_pipelines_init(renderer, setup, &setup->vk_pipelines);

	wl_list_insert(&renderer->render_setups, &setup->link);
	return setup;
}

void vk_render_setup_destroy(struct vk_renderer *renderer, struct vk_render_setup *render_setup) {
	// Texture pipelines are created lazily by passes; the pipeline layouts they
	// reference belong to the wlroots renderer, so only the pipelines are freed.
	struct vk_pipeline_tex *tex_pipeline, *tex_pipeline_tmp;
	wl_list_for_each_safe(tex_pipeline, tex_pipeline_tmp, &render_setup->tex_pipelines, link) {
		delete_vk_tex_pipeline(renderer, tex_pipeline);
	}

	vk_pipelines_destroy(renderer, &render_setup->vk_pipelines);

	// Fallback
	struct vk_pipeline *pipeline, *pipeline_tmp;
	wl_list_for_each_safe(pipeline, pipeline_tmp, &render_setup->pipelines, link) {
		vkDestroyPipeline(renderer->device, pipeline->pipeline, NULL);
		wl_list_remove(&pipeline->link);
	}

	wl_list_remove(&render_setup->link);

	free(render_setup);
}
