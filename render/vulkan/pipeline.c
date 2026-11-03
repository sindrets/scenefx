#include <wlr/util/log.h>

#include "render/vulkan/pipeline.h"
#include "render/vulkan/shaders.h"

void vk_pipelines_init(struct vk_renderer *renderer, struct vk_render_setup *setup,
		struct vk_pipelines *out_pipelines) {
	*out_pipelines = (struct vk_pipelines) {0};

	create_vk_quad_pipelines(renderer, setup, &out_pipelines->quad);
	create_vk_quad_grad_pipelines(renderer, setup, &out_pipelines->quad_grad);
	create_vk_box_shadow_pipelines(renderer, setup, &out_pipelines->box_shadow);

	// Blur pipelines are built against the scene render pass, because the blur
	// passes render into offscreen images whose framebuffers deliberately reuse
	// that same pass (see effects.c). An effect-free pass never runs them.
	if (!create_vk_blur_pipelines(renderer, setup, &out_pipelines->blur1,
			&out_pipelines->blur2, &out_pipelines->blur_effects,
			&out_pipelines->blur_effects_replace)) {
		wlr_log(WLR_ERROR, "Could not create blur pipelines");
	}
}

void vk_pipelines_destroy(struct vk_renderer *renderer, struct vk_pipelines *pipelines) {
	delete_vk_quad_pipelines(renderer, pipelines->quad);
	delete_vk_quad_grad_pipelines(renderer, pipelines->quad_grad);
	delete_vk_box_shadow_pipelines(renderer, pipelines->box_shadow);
	delete_vk_blur_pipelines(renderer, pipelines->blur1);
	delete_vk_blur_pipelines(renderer, pipelines->blur2);
	delete_vk_blur_pipelines(renderer, pipelines->blur_effects);
	delete_vk_blur_pipelines(renderer, pipelines->blur_effects_replace);
}
