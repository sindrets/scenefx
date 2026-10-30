#include "render/vulkan/pipeline.h"
#include "render/vulkan/shaders.h"

void vk_pipelines_init(struct vk_renderer *renderer, struct vk_render_setup *setup,
		struct vk_pipelines *out_pipelines) {
	*out_pipelines = (struct vk_pipelines) {0};

	create_vk_quad_pipelines(renderer, setup, &out_pipelines->quad);
	create_vk_quad_grad_pipelines(renderer, setup, &out_pipelines->quad_grad);
	create_vk_box_shadow_pipelines(renderer, setup, &out_pipelines->box_shadow);
}

void vk_pipelines_destroy(struct vk_renderer *renderer, struct vk_pipelines *pipelines) {
	delete_vk_quad_pipelines(renderer, pipelines->quad);
	delete_vk_quad_grad_pipelines(renderer, pipelines->quad_grad);
	delete_vk_box_shadow_pipelines(renderer, pipelines->box_shadow);
}
