#include <assert.h>
#include <stdlib.h>
#include <vulkan/vulkan_core.h>
#include <wayland-util.h>
#include <wlr/render/vulkan.h>
#include <wlr/util/log.h>

#include "render/shaders.h"
#include "render/vulkan/shaders.h"
#include "render/vulkan/vulkan.h"

// shaders
#include "common.vert.h"
#include "quad.frag.h"
#include "quad_grad.frag.h"
#include "box_shadow.frag.h"
#include "tex.frag.h"
#include "blur1.frag.h"
#include "blur2.frag.h"
#include "blur_effects.frag.h"

void vk_shader_info_delete(struct vk_renderer *renderer, struct vk_shader_info *shader_info) {
	if (shader_info->shader_module != VK_NULL_HANDLE) {
		vkDestroyShaderModule(renderer->device, shader_info->shader_module, NULL);
		shader_info->shader_module = VK_NULL_HANDLE;
	}
	if (shader_info->pipeline_layout != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(renderer->device, shader_info->pipeline_layout, NULL);
		shader_info->pipeline_layout = VK_NULL_HANDLE;
	}
}

static bool create_shader_module(struct vk_renderer *renderer, const uint32_t *code,
		size_t code_size, const char *name, VkShaderModule *out) {
	VkShaderModuleCreateInfo shader_info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = code_size,
		.pCode = code,
	};
	VkResult res = vkCreateShaderModule(renderer->device, &shader_info, NULL, out);
	if (res != VK_SUCCESS) {
		wlr_vk_error("Failed to create shader module", res);
		wlr_log(WLR_ERROR, "Failed shader: %s", name);
		return false;
	}
	return true;
}

///
/// Vert
///

VkResult create_vk_vert_module(struct vk_renderer *renderer) {
	VkShaderModuleCreateInfo shader_info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(common_vert_data),
		.pCode = common_vert_data,
	};
	return vkCreateShaderModule(renderer->device, &shader_info, NULL, &renderer->shader_info.vert);
}

///
/// Common pipeline state
///

static void fill_common_pipeline_state(VkPipelineShaderStageCreateInfo stages[2],
		VkPipelineInputAssemblyStateCreateInfo *assembly,
		VkPipelineRasterizationStateCreateInfo *rasterization,
		VkPipelineColorBlendStateCreateInfo *blend,
		VkPipelineColorBlendAttachmentState *blend_attachment,
		VkPipelineMultisampleStateCreateInfo *multisample,
		VkPipelineViewportStateCreateInfo *viewport,
		VkPipelineDynamicStateCreateInfo *dynamic,
		VkDynamicState dyn_states[2],
		VkPipelineVertexInputStateCreateInfo *vertex,
		bool premultiplied) {
	*assembly = (VkPipelineInputAssemblyStateCreateInfo) {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
	};

	*rasterization = (VkPipelineRasterizationStateCreateInfo) {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.f,
	};

	// We generally work with pre-multiplied alpha
	*blend_attachment = (VkPipelineColorBlendAttachmentState) {
		.blendEnable = premultiplied,
		.srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.colorBlendOp = VK_BLEND_OP_ADD,
		.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.alphaBlendOp = VK_BLEND_OP_ADD,
		.colorWriteMask =
			VK_COLOR_COMPONENT_R_BIT |
			VK_COLOR_COMPONENT_G_BIT |
			VK_COLOR_COMPONENT_B_BIT |
			VK_COLOR_COMPONENT_A_BIT,
	};

	*blend = (VkPipelineColorBlendStateCreateInfo) {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = blend_attachment,
	};

	*multisample = (VkPipelineMultisampleStateCreateInfo) {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};

	*viewport = (VkPipelineViewportStateCreateInfo) {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};

	dyn_states[0] = VK_DYNAMIC_STATE_VIEWPORT;
	dyn_states[1] = VK_DYNAMIC_STATE_SCISSOR;
	*dynamic = (VkPipelineDynamicStateCreateInfo) {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.pDynamicStates = dyn_states,
		.dynamicStateCount = 2,
	};

	*vertex = (VkPipelineVertexInputStateCreateInfo) {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};

	stages[0] = (VkPipelineShaderStageCreateInfo) {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_VERTEX_BIT,
		.pName = "main",
	};
}

///
/// Quad
///

static bool create_pipeline_quad(struct vk_renderer *renderer, struct vk_render_setup *setup,
		struct vk_pipeline *variant, const enum fx_quad_shader_effects _effects) {
	const uint32_t effects = _effects;
	struct vk_shader_info *quad = &renderer->shader_info.quad;

	VkResult res;

	// Specialization constant
	VkSpecializationMapEntry spec_entry = {
		.constantID = 0,
		.offset = 0,
		.size = sizeof(effects),
	};
	VkSpecializationInfo specialization = {
		.mapEntryCount = 1,
		.pMapEntries = &spec_entry,
		.dataSize = sizeof(effects),
		.pData = &effects,
	};

	VkPipelineShaderStageCreateInfo stages[2];
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineRasterizationStateCreateInfo rasterization;
	VkPipelineColorBlendStateCreateInfo blend;
	VkPipelineColorBlendAttachmentState blend_attachment;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineViewportStateCreateInfo viewport;
	VkPipelineDynamicStateCreateInfo dynamic;
	VkDynamicState dyn_states[2];
	VkPipelineVertexInputStateCreateInfo vertex;
	fill_common_pipeline_state(stages, &assembly, &rasterization, &blend,
		&blend_attachment, &multisample, &viewport, &dynamic, dyn_states, &vertex, true);

	stages[0].module = renderer->shader_info.vert;
	stages[1] = (VkPipelineShaderStageCreateInfo) {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		.module = quad->shader_module,
		.pName = "main",
		.pSpecializationInfo = &specialization,
	};

	VkGraphicsPipelineCreateInfo pinfo = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.layout = quad->pipeline_layout,
		.renderPass = setup->render_pass,
		.subpass = 0,
		.stageCount = 2,
		.pStages = stages,

		.pInputAssemblyState = &assembly,
		.pRasterizationState = &rasterization,
		.pColorBlendState = &blend,
		.pMultisampleState = &multisample,
		.pViewportState = &viewport,
		.pDynamicState = &dynamic,
		.pVertexInputState = &vertex,
	};

	VkPipelineCache cache = VK_NULL_HANDLE;
	res = vkCreateGraphicsPipelines(renderer->device, cache, 1, &pinfo, NULL, &variant->pipeline);
	if (res != VK_SUCCESS) {
		wlr_vk_error("failed to create vulkan pipelines", res);
		return false;
	}

	wl_list_insert(&setup->pipelines, &variant->link);
	return true;
}

bool create_vk_quad_pipelines(struct vk_renderer *renderer, struct vk_render_setup *setup,
		struct vk_pipeline_quad **out_quad) {
	*out_quad = NULL;
	struct vk_pipeline_quad *quad = calloc(1, sizeof(*quad));
	if (quad == NULL) {
		wlr_log_errno(WLR_ERROR, "vk_pipeline_quad allocation failed");
		return false;
	}

	for (enum fx_quad_shader_effects effects = 0;
			effects < SHADER_QUAD_EFFECT_LAST;
			effects++) {
		struct vk_pipeline *variant = &quad->variants[effects];
		variant->pipeline = VK_NULL_HANDLE;

		if (!create_pipeline_quad(renderer, setup, variant, effects)) {
			wlr_log(WLR_ERROR, "Could not create quad shader pipeline: Effects: \"%d\"",
					effects);
			goto failed;
		}
	}

	*out_quad = quad;
	return true;

failed:
	// Remove all created pipelines
	delete_vk_quad_pipelines(renderer, quad);
	return false;
}

void delete_vk_quad_pipelines(struct vk_renderer *renderer, struct vk_pipeline_quad *quad) {
	if (quad == NULL) {
		return;
	}

	for (enum fx_quad_shader_effects effects = 0;
			effects < SHADER_QUAD_EFFECT_LAST;
			effects++) {
		struct vk_pipeline *variant = &quad->variants[effects];
		if (variant->pipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(renderer->device, variant->pipeline, NULL);
			wl_list_remove(&variant->link);
			variant->pipeline = VK_NULL_HANDLE;
		}
	}

	free(quad);
}

struct vk_pipeline *get_vk_quad_pipeline(struct vk_pipeline_quad *quad,
		enum fx_quad_shader_effects effects) {
	if (quad == NULL || effects >= SHADER_QUAD_EFFECT_LAST) {
		return NULL;
	}
	return &quad->variants[effects];
}

bool vk_shader_info_create_quad(struct vk_renderer *renderer) {
	struct vk_shader_info *quad = &renderer->shader_info.quad;

	if (!create_shader_module(renderer, quad_frag_data, sizeof(quad_frag_data),
			"quad.frag", &quad->shader_module)) {
		return false;
	}

	VkPushConstantRange pc_ranges[] = {
		{
			.size = sizeof(struct vk_vert_pcr_data),
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
		},
		{
			.offset = sizeof(struct vk_vert_pcr_data),
			.size = sizeof(struct vk_frag_quad_pcr_data),
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		},
	};

	VkPipelineLayoutCreateInfo pl_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 0,
		.pushConstantRangeCount = sizeof(pc_ranges) / sizeof(pc_ranges[0]),
		.pPushConstantRanges = pc_ranges,
	};

	VkResult res = vkCreatePipelineLayout(renderer->device, &pl_info, NULL,
		&quad->pipeline_layout);
	if (res != VK_SUCCESS) {
		vkDestroyShaderModule(renderer->device, quad->shader_module, NULL);
		quad->shader_module = VK_NULL_HANDLE;
		wlr_vk_error("vkCreatePipelineLayout", res);
		return false;
	}

	return true;
}

///
/// Quad gradient
///

// The gradient shader takes its colours from a uniform buffer (16 colours is
// way past any reasonable set of push constants). Both the gradient and the box
// shadow shader reuse the plain quad pipeline layout's push constant block, so
// only the descriptor set layout differs.
static bool create_gradient_descriptor_layout(struct vk_renderer *renderer,
		VkDescriptorSetLayout *out) {
	VkDescriptorSetLayoutBinding binding = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	};

	VkDescriptorSetLayoutCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &binding,
	};

	VkResult res = vkCreateDescriptorSetLayout(renderer->device, &info, NULL, out);
	if (res != VK_SUCCESS) {
		wlr_vk_error("vkCreateDescriptorSetLayout failed for the gradient shader", res);
		return false;
	}

	return true;
}

static bool create_pipeline_gradient(struct vk_renderer *renderer,
		struct vk_render_setup *setup, struct vk_pipeline *variant,
		const enum fx_quad_shader_effects _effects, VkShaderModule fragment_module,
		VkPipelineLayout pipeline_layout) {
	const uint32_t effects = _effects;

	VkResult res;

	VkSpecializationMapEntry spec_entry = {
		.constantID = 0,
		.offset = 0,
		.size = sizeof(effects),
	};
	VkSpecializationInfo specialization = {
		.mapEntryCount = 1,
		.pMapEntries = &spec_entry,
		.dataSize = sizeof(effects),
		.pData = &effects,
	};

	VkPipelineShaderStageCreateInfo stages[2];
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineRasterizationStateCreateInfo rasterization;
	VkPipelineColorBlendStateCreateInfo blend;
	VkPipelineColorBlendAttachmentState blend_attachment;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineViewportStateCreateInfo viewport;
	VkPipelineDynamicStateCreateInfo dynamic;
	VkDynamicState dyn_states[2];
	VkPipelineVertexInputStateCreateInfo vertex;
	fill_common_pipeline_state(stages, &assembly, &rasterization, &blend,
		&blend_attachment, &multisample, &viewport, &dynamic, dyn_states, &vertex, true);

	stages[0].module = renderer->shader_info.vert;
	stages[1] = (VkPipelineShaderStageCreateInfo) {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		.module = fragment_module,
		.pName = "main",
		.pSpecializationInfo = &specialization,
	};

	VkGraphicsPipelineCreateInfo pinfo = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.layout = pipeline_layout,
		.renderPass = setup->render_pass,
		.subpass = 0,
		.stageCount = 2,
		.pStages = stages,

		.pInputAssemblyState = &assembly,
		.pRasterizationState = &rasterization,
		.pColorBlendState = &blend,
		.pMultisampleState = &multisample,
		.pViewportState = &viewport,
		.pDynamicState = &dynamic,
		.pVertexInputState = &vertex,
	};

	res = vkCreateGraphicsPipelines(renderer->device, VK_NULL_HANDLE, 1, &pinfo, NULL,
		&variant->pipeline);
	if (res != VK_SUCCESS) {
		wlr_vk_error("failed to create vulkan gradient pipeline", res);
		return false;
	}

	wl_list_insert(&setup->pipelines, &variant->link);
	return true;
}

static bool create_gradient_variants(struct vk_renderer *renderer,
		struct vk_render_setup *setup, struct vk_pipeline *variants,
		VkShaderModule fragment_module, VkPipelineLayout pipeline_layout) {
	for (enum fx_quad_shader_effects effects = 0;
			effects < SHADER_QUAD_EFFECT_LAST;
			effects++) {
		struct vk_pipeline *variant = &variants[effects];
		variant->pipeline = VK_NULL_HANDLE;

		if (!create_pipeline_gradient(renderer, setup, variant, effects,
				fragment_module, pipeline_layout)) {
			return false;
		}
	}

	return true;
}

static void delete_gradient_variants(struct vk_renderer *renderer,
		struct vk_pipeline *variants) {
	for (enum fx_quad_shader_effects effects = 0;
			effects < SHADER_QUAD_EFFECT_LAST;
			effects++) {
		struct vk_pipeline *variant = &variants[effects];
		if (variant->pipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(renderer->device, variant->pipeline, NULL);
			wl_list_remove(&variant->link);
			variant->pipeline = VK_NULL_HANDLE;
		}
	}
}

bool create_vk_quad_grad_pipelines(struct vk_renderer *renderer,
		struct vk_render_setup *setup, struct vk_pipeline_quad_grad **out_quad_grad) {
	*out_quad_grad = NULL;

	struct vk_pipeline_quad_grad *quad_grad = calloc(1, sizeof(*quad_grad));
	if (quad_grad == NULL) {
		wlr_log_errno(WLR_ERROR, "vk_pipeline_quad_grad allocation failed");
		return false;
	}

	if (!create_gradient_variants(renderer, setup, quad_grad->variants,
			renderer->shader_info.quad_grad.shader_module,
			renderer->shader_info.quad_grad.pipeline_layout)) {
		wlr_log(WLR_ERROR, "Could not create quad gradient shader pipelines");
		delete_vk_quad_grad_pipelines(renderer, quad_grad);
		return false;
	}

	*out_quad_grad = quad_grad;
	return true;
}

void delete_vk_quad_grad_pipelines(struct vk_renderer *renderer,
		struct vk_pipeline_quad_grad *quad_grad) {
	if (quad_grad == NULL) {
		return;
	}

	delete_gradient_variants(renderer, quad_grad->variants);
	free(quad_grad);
}

struct vk_pipeline *get_vk_quad_grad_pipeline(struct vk_pipeline_quad_grad *quad_grad,
		enum fx_quad_shader_effects effects) {
	if (quad_grad == NULL || effects >= SHADER_QUAD_EFFECT_LAST) {
		return NULL;
	}
	return &quad_grad->variants[effects];
}

bool vk_shader_info_create_quad_grad(struct vk_renderer *renderer) {
	struct vk_shader_info *quad_grad = &renderer->shader_info.quad_grad;

	if (!create_shader_module(renderer, quad_grad_frag_data, sizeof(quad_grad_frag_data),
			"quad_grad.frag", &quad_grad->shader_module)) {
		return false;
	}

	// The gradient shader reads its colours from a UBO in descriptor set 0, plus
	// the same push constant block as the plain quad shader.
	if (!create_gradient_descriptor_layout(renderer, &renderer->gradient_ds_layout)) {
		vkDestroyShaderModule(renderer->device, quad_grad->shader_module, NULL);
		quad_grad->shader_module = VK_NULL_HANDLE;
		return false;
	}

	VkPushConstantRange pc_ranges[] = {
		{
			.size = sizeof(struct vk_vert_pcr_data),
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
		},
		{
			.offset = sizeof(struct vk_vert_pcr_data),
			.size = sizeof(struct vk_frag_quad_pcr_data),
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		},
	};

	VkPipelineLayoutCreateInfo pl_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &renderer->gradient_ds_layout,
		.pushConstantRangeCount = sizeof(pc_ranges) / sizeof(pc_ranges[0]),
		.pPushConstantRanges = pc_ranges,
	};

	VkResult res = vkCreatePipelineLayout(renderer->device, &pl_info, NULL,
		&quad_grad->pipeline_layout);
	if (res != VK_SUCCESS) {
		vkDestroyDescriptorSetLayout(renderer->device, renderer->gradient_ds_layout, NULL);
		renderer->gradient_ds_layout = VK_NULL_HANDLE;
		vkDestroyShaderModule(renderer->device, quad_grad->shader_module, NULL);
		quad_grad->shader_module = VK_NULL_HANDLE;
		wlr_vk_error("vkCreatePipelineLayout", res);
		return false;
	}

	return true;
}

///
/// Box shadow
///

bool create_vk_box_shadow_pipelines(struct vk_renderer *renderer,
		struct vk_render_setup *setup,
		struct vk_pipeline_box_shadow **out_box_shadow) {
	*out_box_shadow = NULL;

	struct vk_pipeline_box_shadow *box_shadow = calloc(1, sizeof(*box_shadow));
	if (box_shadow == NULL) {
		wlr_log_errno(WLR_ERROR, "vk_pipeline_box_shadow allocation failed");
		return false;
	}

	if (!create_gradient_variants(renderer, setup, box_shadow->variants,
			renderer->shader_info.box_shadow.shader_module,
			renderer->shader_info.box_shadow.pipeline_layout)) {
		wlr_log(WLR_ERROR, "Could not create box shadow shader pipelines");
		delete_vk_box_shadow_pipelines(renderer, box_shadow);
		return false;
	}

	*out_box_shadow = box_shadow;
	return true;
}

void delete_vk_box_shadow_pipelines(struct vk_renderer *renderer,
		struct vk_pipeline_box_shadow *box_shadow) {
	if (box_shadow == NULL) {
		return;
	}

	delete_gradient_variants(renderer, box_shadow->variants);
	free(box_shadow);
}

struct vk_pipeline *get_vk_box_shadow_pipeline(struct vk_pipeline_box_shadow *box_shadow,
		enum fx_quad_shader_effects effects) {
	if (box_shadow == NULL || effects >= SHADER_QUAD_EFFECT_LAST) {
		return NULL;
	}
	return &box_shadow->variants[effects];
}

bool vk_shader_info_create_box_shadow(struct vk_renderer *renderer) {
	struct vk_shader_info *box_shadow = &renderer->shader_info.box_shadow;

	if (!create_shader_module(renderer, box_shadow_frag_data, sizeof(box_shadow_frag_data),
			"box_shadow.frag", &box_shadow->shader_module)) {
		return false;
	}

	// The box shadow shader uses the exact same push constant block as the quad
	// shader (with an extra shadow_params vec4 at the end).
	VkPushConstantRange pc_ranges[] = {
		{
			.size = sizeof(struct vk_vert_pcr_data),
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
		},
		{
			.offset = sizeof(struct vk_vert_pcr_data),
			.size = sizeof(struct vk_frag_box_shadow_pcr_data),
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		},
	};

	// The shadow shader has no uniforms, but it shares the gradient pipeline
	// layout shape so that one descriptor set layout covers both.
	VkPipelineLayoutCreateInfo pl_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 0,
		.pushConstantRangeCount = sizeof(pc_ranges) / sizeof(pc_ranges[0]),
		.pPushConstantRanges = pc_ranges,
	};

	VkResult res = vkCreatePipelineLayout(renderer->device, &pl_info, NULL,
		&box_shadow->pipeline_layout);
	if (res != VK_SUCCESS) {
		vkDestroyShaderModule(renderer->device, box_shadow->shader_module, NULL);
		box_shadow->shader_module = VK_NULL_HANDLE;
		wlr_vk_error("vkCreatePipelineLayout", res);
		return false;
	}

	return true;
}

///
/// Texture
///

bool create_vk_tex_pipeline(struct vk_renderer *renderer, struct vk_render_setup *setup,
		enum fx_tex_shader_effects effects, enum wlr_scale_filter_mode filter_mode,
		uint32_t texture_transform, bool srgb_image_view,
		VkDescriptorSetLayout ds_layout, struct vk_pipeline_tex *out) {
	// wlroots hands us the descriptor set layout it built for this texture (one
	// combined image sampler at binding 0, with the immutable sampler baked in).
	// We build our own pipeline layout around it so we can declare our own push
	// constant ranges; the descriptor set itself stays compatible with wlroots'
	// VkDescriptorSet.
	if (ds_layout == VK_NULL_HANDLE) {
		wlr_log(WLR_ERROR, "No descriptor set layout for the vulkan FX texture pipeline");
		return false;
	}

	VkPushConstantRange ranges[2] = {
		{
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
			.offset = 0,
			.size = sizeof(struct vk_vert_pcr_data),
		},
		{
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			.offset = sizeof(struct vk_vert_pcr_data),
			.size = sizeof(struct vk_frag_tex_pcr_data),
		},
	};

	VkPipelineLayoutCreateInfo pl_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &ds_layout,
		.pushConstantRangeCount = 2,
		.pPushConstantRanges = ranges,
	};
	VkResult res = vkCreatePipelineLayout(renderer->device, &pl_info, NULL,
		&out->pipeline_layout);
	if (res != VK_SUCCESS) {
		wlr_vk_error("failed to create vulkan tex pipeline layout", res);
		return false;
	}

	uint32_t spec_data[2] = { effects, texture_transform };
	VkSpecializationMapEntry spec_entries[2] = {
		{
			.constantID = 0,
			.offset = 0,
			.size = sizeof(uint32_t),
		},
		{
			.constantID = 1,
			.offset = sizeof(uint32_t),
			.size = sizeof(uint32_t),
		},
	};
	VkSpecializationInfo specialization = {
		.mapEntryCount = 2,
		.pMapEntries = spec_entries,
		.dataSize = sizeof(spec_data),
		.pData = spec_data,
	};

	VkPipelineShaderStageCreateInfo stages[2];
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineRasterizationStateCreateInfo rasterization;
	VkPipelineColorBlendStateCreateInfo blend;
	VkPipelineColorBlendAttachmentState blend_attachment;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineViewportStateCreateInfo viewport;
	VkPipelineDynamicStateCreateInfo dynamic;
	VkDynamicState dyn_states[2];
	VkPipelineVertexInputStateCreateInfo vertex;
	// The FX texture shader always emits pre-multiplied alpha.
	fill_common_pipeline_state(stages, &assembly, &rasterization, &blend,
		&blend_attachment, &multisample, &viewport, &dynamic, dyn_states, &vertex, true);

	stages[0].module = renderer->shader_info.vert;
	stages[1] = (VkPipelineShaderStageCreateInfo) {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		.module = renderer->shader_info.tex.shader_module,
		.pName = "main",
		.pSpecializationInfo = &specialization,
	};

	VkGraphicsPipelineCreateInfo pinfo = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.layout = out->pipeline_layout,
		.renderPass = setup->render_pass,
		.subpass = 0,
		.stageCount = 2,
		.pStages = stages,

		.pInputAssemblyState = &assembly,
		.pRasterizationState = &rasterization,
		.pColorBlendState = &blend,
		.pMultisampleState = &multisample,
		.pViewportState = &viewport,
		.pDynamicState = &dynamic,
		.pVertexInputState = &vertex,
	};

	res = vkCreateGraphicsPipelines(renderer->device, VK_NULL_HANDLE, 1, &pinfo,
		NULL, &out->pipeline);
	if (res != VK_SUCCESS) {
		vkDestroyPipelineLayout(renderer->device, out->pipeline_layout, NULL);
		out->pipeline_layout = VK_NULL_HANDLE;
		wlr_vk_error("failed to create vulkan tex pipeline", res);
		return false;
	}

	out->ds_layout = ds_layout;
	out->effects = effects;
	out->filter_mode = filter_mode;
	out->texture_transform = texture_transform;
	out->srgb_image_view = srgb_image_view;
	wl_list_insert(&setup->tex_pipelines, &out->link);
	return true;
}

void delete_vk_tex_pipeline(struct vk_renderer *renderer, struct vk_pipeline_tex *pipeline) {
	if (pipeline == NULL || pipeline->pipeline == VK_NULL_HANDLE) {
		return;
	}
	// The layout and descriptor set layout are only used by this pipeline.
	vkDestroyPipeline(renderer->device, pipeline->pipeline, NULL);
	vkDestroyPipelineLayout(renderer->device, pipeline->pipeline_layout, NULL);
	wl_list_remove(&pipeline->link);
	pipeline->pipeline = VK_NULL_HANDLE;
	pipeline->pipeline_layout = VK_NULL_HANDLE;
}

bool vk_shader_info_create_tex(struct vk_renderer *renderer) {
	struct vk_shader_info *tex = &renderer->shader_info.tex;

	if (!create_shader_module(renderer, tex_frag_data, sizeof(tex_frag_data),
			"tex.frag", &tex->shader_module)) {
		return false;
	}

	return true;
}

///
/// Blur
///

// Descriptor sets for the blur passes are allocated from this pool, in this
// layout, and always sample through our own immutable sampler. Note this is
// deliberately NOT compatible with wlroots' texture descriptor sets - see the
// comment on tex_sampler in vulkan.h.
#define VK_TEX_DS_POOL_SIZE 64

VkDescriptorSetLayout vk_get_tex_ds_layout(struct vk_renderer *renderer) {
	if (renderer->shader_info.tex_ds_layout != VK_NULL_HANDLE) {
		return renderer->shader_info.tex_ds_layout;
	}

	VkSamplerCreateInfo sampler_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.maxAnisotropy = 1.f,
		.minLod = 0.f,
		.maxLod = 0.25f,
		.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
	};
	VkResult res = vkCreateSampler(renderer->device, &sampler_info, NULL,
			&renderer->shader_info.tex_sampler);
	if (res != VK_SUCCESS) {
		wlr_vk_error("vkCreateSampler (blur)", res);
		return VK_NULL_HANDLE;
	}

	// Binding 1 is blur_effects' transparency mask. Every blur shader shares this
	// layout, so blur1/blur2 simply leave it unread; Vulkan allows a binding no
	// shader touches to stay unwritten.
	VkDescriptorSetLayoutBinding bindings[] = {
		{
			.binding = 0,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			.pImmutableSamplers = &renderer->shader_info.tex_sampler,
		},
		{
			.binding = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			.pImmutableSamplers = &renderer->shader_info.tex_sampler,
		},
	};
	VkDescriptorSetLayoutCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = sizeof(bindings) / sizeof(bindings[0]),
		.pBindings = bindings,
	};

	res = vkCreateDescriptorSetLayout(renderer->device, &info, NULL,
			&renderer->shader_info.tex_ds_layout);
	if (res != VK_SUCCESS) {
		vkDestroySampler(renderer->device, renderer->shader_info.tex_sampler, NULL);
		renderer->shader_info.tex_sampler = VK_NULL_HANDLE;
		wlr_vk_error("vkCreateDescriptorSetLayout (tex)", res);
		return VK_NULL_HANDLE;
	}

	// Two combined-image-sampler bindings per set (tex and mask).
	VkDescriptorPoolSize pool_size = {
		.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = VK_TEX_DS_POOL_SIZE * 2,
	};
	VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
		.maxSets = VK_TEX_DS_POOL_SIZE,
		.poolSizeCount = 1,
		.pPoolSizes = &pool_size,
	};
	res = vkCreateDescriptorPool(renderer->device, &pool_info, NULL,
			&renderer->shader_info.tex_ds_pool);
	if (res != VK_SUCCESS) {
		vkDestroyDescriptorSetLayout(renderer->device,
				renderer->shader_info.tex_ds_layout, NULL);
		renderer->shader_info.tex_ds_layout = VK_NULL_HANDLE;
		vkDestroySampler(renderer->device, renderer->shader_info.tex_sampler, NULL);
		renderer->shader_info.tex_sampler = VK_NULL_HANDLE;
		wlr_vk_error("vkCreateDescriptorPool (tex)", res);
		return VK_NULL_HANDLE;
	}

	return renderer->shader_info.tex_ds_layout;
}

VkDescriptorSet vk_tex_ds_create(struct vk_renderer *renderer, VkImageView view) {
	VkDescriptorSetLayout layout = vk_get_tex_ds_layout(renderer);
	if (layout == VK_NULL_HANDLE) {
		return VK_NULL_HANDLE;
	}

	VkDescriptorSetAllocateInfo alloc_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = renderer->shader_info.tex_ds_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &layout,
	};
	VkDescriptorSet ds = VK_NULL_HANDLE;
	VkResult res = vkAllocateDescriptorSets(renderer->device, &alloc_info, &ds);
	if (res != VK_SUCCESS) {
		wlr_vk_error("vkAllocateDescriptorSets (tex)", res);
		return VK_NULL_HANDLE;
	}

	VkDescriptorImageInfo image_info = {
		// sampler comes from the layout's immutable sampler
		.imageView = view,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = ds,
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &image_info,
	};
	vkUpdateDescriptorSets(renderer->device, 1, &write, 0, NULL);

	return ds;
}

bool vk_tex_ds_set_mask(struct vk_renderer *renderer, VkDescriptorSet ds,
		VkImageView view, VkImageLayout layout) {
	if (ds == VK_NULL_HANDLE) {
		return false;
	}
	if (view == VK_NULL_HANDLE) {
		if (!vk_dummy_mask_ensure(renderer)) {
			return false;
		}
		view = renderer->dummy_mask.view;
		layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	VkDescriptorImageInfo image_info = {
		// sampler comes from the layout's immutable sampler
		.imageView = view,
		.imageLayout = layout,
	};
	VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = ds,
		.dstBinding = 1,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &image_info,
	};
	vkUpdateDescriptorSets(renderer->device, 1, &write, 0, NULL);
	return true;
}

void vk_tex_ds_destroy(struct vk_renderer *renderer, VkDescriptorSet ds) {
	if (ds == VK_NULL_HANDLE || renderer->shader_info.tex_ds_pool == VK_NULL_HANDLE) {
		return;
	}
	vkFreeDescriptorSets(renderer->device, renderer->shader_info.tex_ds_pool, 1, &ds);
}

void vk_tex_ds_layout_destroy(struct vk_renderer *renderer) {
	if (renderer->shader_info.tex_ds_pool != VK_NULL_HANDLE) {
		vkDestroyDescriptorPool(renderer->device, renderer->shader_info.tex_ds_pool, NULL);
		renderer->shader_info.tex_ds_pool = VK_NULL_HANDLE;
	}
	if (renderer->shader_info.tex_ds_layout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(renderer->device,
				renderer->shader_info.tex_ds_layout, NULL);
		renderer->shader_info.tex_ds_layout = VK_NULL_HANDLE;
	}
	if (renderer->shader_info.tex_sampler != VK_NULL_HANDLE) {
		vkDestroySampler(renderer->device, renderer->shader_info.tex_sampler, NULL);
		renderer->shader_info.tex_sampler = VK_NULL_HANDLE;
	}
}

// Shared by all three blur shaders: one sampler set plus a vertex and a
// fragment push constant range. `frag_pcr_size` is what differs between them.
static bool create_blur_shader_info(struct vk_renderer *renderer,
		struct vk_shader_info *shader, const uint32_t *spv, size_t spv_size,
		size_t frag_pcr_size, const char *name) {
	VkShaderModuleCreateInfo shader_info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = spv_size,
		.pCode = spv,
	};
	VkResult res = vkCreateShaderModule(renderer->device, &shader_info, NULL,
			&shader->shader_module);
	if (res != VK_SUCCESS) {
		wlr_vk_error("Failed to create blur fragment shader module", res);
		wlr_log(WLR_ERROR, "  shader: %s", name);
		return false;
	}

	VkDescriptorSetLayout ds_layout = vk_get_tex_ds_layout(renderer);
	if (ds_layout == VK_NULL_HANDLE) {
		vkDestroyShaderModule(renderer->device, shader->shader_module, NULL);
		return false;
	}

	VkPushConstantRange pc_ranges[] = {
		{
			.size = sizeof(struct vk_vert_pcr_data),
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
		},
		{
			// Must not be written as pc_ranges[0].size: reading the array
			// while it is still being initialised is unspecified.
			.offset = sizeof(struct vk_vert_pcr_data),
			.size = frag_pcr_size,
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		},
	};

	VkPipelineLayoutCreateInfo pl_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &ds_layout,
		.pushConstantRangeCount = sizeof(pc_ranges) / sizeof(pc_ranges[0]),
		.pPushConstantRanges = pc_ranges,
	};

	res = vkCreatePipelineLayout(renderer->device, &pl_info, NULL, &shader->pipeline_layout);
	if (res != VK_SUCCESS) {
		vkDestroyShaderModule(renderer->device, shader->shader_module, NULL);
		wlr_vk_error("vkCreatePipelineLayout (blur)", res);
		return false;
	}

	return true;
}

bool vk_shader_info_create_blur1(struct vk_renderer *renderer) {
	return create_blur_shader_info(renderer, &renderer->shader_info.blur1,
		blur1_frag_data, sizeof(blur1_frag_data),
		sizeof(struct vk_frag_blur_pcr_data), "blur1");
}

bool vk_shader_info_create_blur2(struct vk_renderer *renderer) {
	return create_blur_shader_info(renderer, &renderer->shader_info.blur2,
		blur2_frag_data, sizeof(blur2_frag_data),
		sizeof(struct vk_frag_blur_pcr_data), "blur2");
}

bool vk_shader_info_create_blur_effects(struct vk_renderer *renderer) {
	return create_blur_shader_info(renderer, &renderer->shader_info.blur_effects,
		blur_effects_frag_data, sizeof(blur_effects_frag_data),
		sizeof(struct vk_frag_blur_effects_pcr_data), "blur_effects");
}

// The blur passes write into offscreen buffers and fully replace their
// contents, so unlike the quad pipeline they disable blending.
static bool create_pipeline_blur(struct vk_renderer *renderer, struct vk_render_setup *setup,
		struct vk_shader_info *shader, struct vk_pipeline *variant,
		bool premultiplied) {
	VkPipelineShaderStageCreateInfo stages[] = {
		(VkPipelineShaderStageCreateInfo) {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = renderer->shader_info.vert,
			.pName = "main",
		},
		(VkPipelineShaderStageCreateInfo) {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = shader->shader_module,
			.pName = "main",
		},
	};

	VkPipelineInputAssemblyStateCreateInfo assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
	};

	VkPipelineRasterizationStateCreateInfo rasterization = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.f,
	};

	// blur1/blur2 fully replace their offscreen target, so they must not blend.
	// blur_effects is the composite into the scene and blends like every other
	// scenefx draw: with blending off, the rounded corners (where corner_alpha
	// drives alpha to 0) would overwrite the background with opaque black.
	VkPipelineColorBlendAttachmentState blend_attachment = {
		.blendEnable = premultiplied,
		.srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.colorBlendOp = VK_BLEND_OP_ADD,
		.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.alphaBlendOp = VK_BLEND_OP_ADD,
		.colorWriteMask =
			VK_COLOR_COMPONENT_R_BIT |
			VK_COLOR_COMPONENT_G_BIT |
			VK_COLOR_COMPONENT_B_BIT |
			VK_COLOR_COMPONENT_A_BIT,
	};

	VkPipelineColorBlendStateCreateInfo blend = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &blend_attachment,
	};

	VkPipelineMultisampleStateCreateInfo multisample = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};

	VkPipelineViewportStateCreateInfo viewport = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};

	VkDynamicState dyn_states[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
	};
	VkPipelineDynamicStateCreateInfo dynamic = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.pDynamicStates = dyn_states,
		.dynamicStateCount = sizeof(dyn_states) / sizeof(dyn_states[0]),
	};

	VkPipelineVertexInputStateCreateInfo vertex = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};

	VkGraphicsPipelineCreateInfo pinfo = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.layout = shader->pipeline_layout,
		.renderPass = setup->render_pass,
		.subpass = 0,
		.stageCount = sizeof(stages) / sizeof(stages[0]),
		.pStages = stages,

		.pInputAssemblyState = &assembly,
		.pRasterizationState = &rasterization,
		.pColorBlendState = &blend,
		.pMultisampleState = &multisample,
		.pViewportState = &viewport,
		.pDynamicState = &dynamic,
		.pVertexInputState = &vertex,
	};

	VkPipelineCache cache = VK_NULL_HANDLE;
	VkResult res = vkCreateGraphicsPipelines(renderer->device, cache, 1, &pinfo, NULL,
			&variant->pipeline);
	if (res != VK_SUCCESS) {
		wlr_vk_error("failed to create blur pipeline:", res);
		return false;
	}

	wl_list_insert(&setup->pipelines, &variant->link);
	return true;
}

static struct vk_pipeline_blur *create_one_blur_pipeline(struct vk_renderer *renderer,
		struct vk_render_setup *setup, struct vk_shader_info *shader,
		bool premultiplied) {
	struct vk_pipeline_blur *blur = calloc(1, sizeof(*blur));
	if (blur == NULL) {
		return NULL;
	}
	blur->pipeline.pipeline = VK_NULL_HANDLE;
	if (!create_pipeline_blur(renderer, setup, shader, &blur->pipeline, premultiplied)) {
		free(blur);
		return NULL;
	}
	blur->pipeline_layout = shader->pipeline_layout;
	return blur;
}

bool create_vk_blur_pipelines(struct vk_renderer *renderer, struct vk_render_setup *setup,
		struct vk_pipeline_blur **out_blur1, struct vk_pipeline_blur **out_blur2,
		struct vk_pipeline_blur **out_blur_effects,
		struct vk_pipeline_blur **out_blur_effects_replace) {
	*out_blur1 = create_one_blur_pipeline(renderer, setup,
			&renderer->shader_info.blur1, false);
	*out_blur2 = create_one_blur_pipeline(renderer, setup,
			&renderer->shader_info.blur2, false);
	*out_blur_effects = create_one_blur_pipeline(renderer, setup,
			&renderer->shader_info.blur_effects, true);
	*out_blur_effects_replace = create_one_blur_pipeline(renderer, setup,
			&renderer->shader_info.blur_effects, false);

	if (*out_blur1 == NULL || *out_blur2 == NULL || *out_blur_effects == NULL ||
			*out_blur_effects_replace == NULL) {
		wlr_log(WLR_ERROR, "Could not create blur shader pipelines");
		delete_vk_blur_pipelines(renderer, *out_blur1);
		delete_vk_blur_pipelines(renderer, *out_blur2);
		delete_vk_blur_pipelines(renderer, *out_blur_effects);
		delete_vk_blur_pipelines(renderer, *out_blur_effects_replace);
		*out_blur1 = *out_blur2 = NULL;
		*out_blur_effects = *out_blur_effects_replace = NULL;
		return false;
	}
	return true;
}

void delete_vk_blur_pipelines(struct vk_renderer *renderer, struct vk_pipeline_blur *blur) {
	if (blur == NULL) {
		return;
	}
	if (blur->pipeline.pipeline != VK_NULL_HANDLE) {
		vkDestroyPipeline(renderer->device, blur->pipeline.pipeline, NULL);
		wl_list_remove(&blur->pipeline.link);
	}
	free(blur);
}
