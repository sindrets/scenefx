#ifndef _RENDERER_VULKAN_SHADERS_H
#define _RENDERER_VULKAN_SHADERS_H

#include <vulkan/vulkan_core.h>
#include <wlr/render/wlr_renderer.h>

#include "render/shaders.h"
#include "render/vulkan/pipeline.h"

struct vk_render_pass;
struct vk_renderer;
struct vk_render_setup;

// Matches the `corner_rounding` / `clipping` halves of the fragment shaders'
// push constant blocks. `edges_valid` is 0 when the caller renders a
// whole-pixel box, in which case `edges` is ignored and the shape covers its
// pixels fully.
struct vk_shader_rounding_data {
	float size[2];
	float position[2];
	struct {
		float top_left;
		float top_right;
		float bottom_left;
		float bottom_right;
	} radius;

	// Exact device-pixel edges, (left, top, right, bottom), of the shape this
	// data describes. Used to antialias edges that miss the pixel grid.
	//
	// std430 rounds the `vec4` up to a 16-byte boundary, so the shader reads
	// `edges` at offset 48 with three words of padding after `edges_valid`.
	float edges_valid;
	float _pad[3];
	float edges[4];
};

///
/// Shader info
///

struct vk_shader_info {
	VkShaderModule shader_module;
	VkPipelineLayout pipeline_layout;
};

void vk_shader_info_delete(struct vk_renderer *renderer, struct vk_shader_info *shader_info);

///
/// Vert
///

// Matches the `data` push constant block in common.vert
struct vk_vert_pcr_data {
	float mat4[4][4];
	float uv_off[2];
	float uv_size[2];
};

VkResult create_vk_vert_module(struct vk_renderer *renderer);

///
/// Quad
///

// Matches the `data` push constant block in quad.frag
struct vk_frag_quad_pcr_data {
	float color[4];
	struct vk_shader_rounding_data corner_rounding;
	struct vk_shader_rounding_data clipping;
};

// Number of gradient colours the gradient shaders can take. Matches
// MAX_GRADIENT_COLORS in quad_grad.frag / box_shadow.frag and the GLES2
// `quad_grad` program's initial max_len (which is what limits the public API in
// practice: `fx_gradient` has no documented cap, but GLES2 starts at 2 and
// relinks up to `count + 1`).
#define MAX_GRADIENT_COLORS 16

// Matches the `Gradient` uniform block in quad_grad.frag
struct vk_frag_quad_grad_ubo {
	float colors[MAX_GRADIENT_COLORS][4];
	// (degree, linear, blend, count)
	float params[4];
	// (range.width, range.height, range.x, range.y)
	float range[4];
	// (origin.x, origin.y, unused, unused)
	float origin[4];
};

/// How many blend modes a pipeline is built for. Blending is baked into the
/// VkPipeline, so each shader variant needs one pipeline per mode; the index is
/// the enum wlr_render_blend_mode value.
#define VK_BLEND_MODE_COUNT 2

struct vk_pipeline_quad {
	// 4 different combinations of effects, per blend mode
	struct vk_pipeline variants[SHADER_QUAD_EFFECT_LAST][VK_BLEND_MODE_COUNT];
};

bool create_vk_quad_pipelines(struct vk_renderer *renderer, struct vk_render_setup *setup,
		struct vk_pipeline_quad **out_quad);
void delete_vk_quad_pipelines(struct vk_renderer *renderer, struct vk_pipeline_quad *quad);
struct vk_pipeline *get_vk_quad_pipeline(struct vk_pipeline_quad *quad,
		enum fx_quad_shader_effects effects, enum wlr_render_blend_mode blend_mode);

bool vk_shader_info_create_quad(struct vk_renderer *renderer);

///
/// Quad gradient
///

struct vk_pipeline_quad_grad {
	struct vk_pipeline variants[SHADER_QUAD_EFFECT_LAST][VK_BLEND_MODE_COUNT];
};

bool create_vk_quad_grad_pipelines(struct vk_renderer *renderer,
		struct vk_render_setup *setup, struct vk_pipeline_quad_grad **out_quad_grad);
void delete_vk_quad_grad_pipelines(struct vk_renderer *renderer,
		struct vk_pipeline_quad_grad *quad_grad);
struct vk_pipeline *get_vk_quad_grad_pipeline(struct vk_pipeline_quad_grad *quad_grad,
		enum fx_quad_shader_effects effects, enum wlr_render_blend_mode blend_mode);

bool vk_shader_info_create_quad_grad(struct vk_renderer *renderer);

///
/// Box shadow
///

// Matches the `data` push constant block in box_shadow.frag
struct vk_frag_box_shadow_pcr_data {
	struct vk_frag_quad_pcr_data base;
	// (blur_sigma, corner_radius, unused, unused)
	float shadow_params[4];
};

struct vk_pipeline_box_shadow {
	struct vk_pipeline variants[SHADER_QUAD_EFFECT_LAST][VK_BLEND_MODE_COUNT];
};

bool create_vk_box_shadow_pipelines(struct vk_renderer *renderer,
		struct vk_render_setup *setup,
		struct vk_pipeline_box_shadow **out_box_shadow);
void delete_vk_box_shadow_pipelines(struct vk_renderer *renderer,
		struct vk_pipeline_box_shadow *box_shadow);
struct vk_pipeline *get_vk_box_shadow_pipeline(struct vk_pipeline_box_shadow *box_shadow,
		enum fx_quad_shader_effects effects, enum wlr_render_blend_mode blend_mode);

bool vk_shader_info_create_box_shadow(struct vk_renderer *renderer);

///
/// Texture
///

// Matches the `data` push constant block in tex.frag
struct vk_frag_tex_pcr_data {
	// (alpha, discard_transparent, ignore_alpha_threshold, unused)
	float params[4];
	struct vk_shader_rounding_data corner_rounding;
	struct vk_shader_rounding_data clipping;
};

struct vk_pipeline_tex {
	struct wl_list link; // struct vk_render_setup.tex_pipelines
	VkPipeline pipeline;
	// Owned by this pipeline: built from the descriptor set layout wlroots hands
	// out for the texture, plus our own push constant ranges.
	VkPipelineLayout pipeline_layout;
	VkDescriptorSetLayout ds_layout;

	// Cache key: the shader specialization this pipeline was built for.
	enum fx_tex_shader_effects effects;
	enum wlr_scale_filter_mode filter_mode;
	uint32_t texture_transform;
	bool srgb_image_view;
	enum wlr_render_blend_mode blend_mode;
};

bool create_vk_tex_pipeline(struct vk_renderer *renderer, struct vk_render_setup *setup,
		enum fx_tex_shader_effects effects, enum wlr_scale_filter_mode filter_mode,
		uint32_t texture_transform, bool srgb_image_view,
		enum wlr_render_blend_mode blend_mode,
		VkDescriptorSetLayout ds_layout, struct vk_pipeline_tex *out_pipeline);
void delete_vk_tex_pipeline(struct vk_renderer *renderer, struct vk_pipeline_tex *pipeline);

bool vk_shader_info_create_tex(struct vk_renderer *renderer);

///
/// Blur
///

// Field order must match blur1.frag/blur2.frag exactly: std430 aligns a vec2
// to 8 bytes, so an explicit pad is needed after the scalar `radius`. Without
// it the compiler packs uv_min at offset 12 while the shader reads offset 16,
// and the push constant range 8 bytes short of what the shader declares.
struct vk_frag_blur_pcr_data {
	float halfpixel[2];
	float radius;
	float _pad;
	// Texel centres of the already-rendered source area. Taps outside it read
	// stale buffer contents (the coarse levels only occupy the top-left corner
	// of the image), so the shaders clamp to this box and extend the edges
	// instead. Mirrors GLES2's uv_min/uv_max in blur1.frag and blur2.frag.
	float uv_min[2];
	float uv_max[2];
};

// Matches blur_effects.frag, which applies brightness/contrast/saturation/noise
// and then masks the result to the node's rounded corners.
struct vk_frag_blur_effects_pcr_data {
	float brightness;
	float contrast;
	float saturation;
	float noise;
	struct vk_shader_rounding_data corner_rounding;

	// Transparency mask sampling. mask_uv/mask_uv_size map the composite's uv
	// onto the mask texture; has_mask 0 makes the shader ignore it entirely, in
	// which case the dummy mask is bound and the fetch still well defined.
	// ignore_alpha_threshold matches fx_render_blur_pass_options of the same
	// name: masked pixels at or below it are discarded.
	float mask_uv[2];
	float mask_uv_size[2];
	float ignore_alpha_threshold;
	float has_mask;
};

struct vk_pipeline_blur {
	struct vk_pipeline pipeline;
	// Layout of the shader this pipeline was created from. The three blur
	// layouts are currently identical, so binding through any of them happens
	// to work, but that is not something to rely on.
	VkPipelineLayout pipeline_layout;
};

// Sampling path for the blur passes: a single combined image sampler at
// binding 0 using our own immutable LINEAR/CLAMP_TO_EDGE sampler.
//
// NOT interchangeable with wlroots' texture descriptor sets: its layout
// declares a different immutable sampler, and Vulkan only considers layouts
// compatible when identically defined, immutable samplers included.
VkDescriptorSetLayout vk_get_tex_ds_layout(struct vk_renderer *renderer);
VkDescriptorSet vk_tex_ds_create(struct vk_renderer *renderer, VkImageView view);
void vk_tex_ds_destroy(struct vk_renderer *renderer, VkDescriptorSet ds);
void vk_tex_ds_layout_destroy(struct vk_renderer *renderer);

bool vk_shader_info_create_blur1(struct vk_renderer *renderer);
bool vk_shader_info_create_blur2(struct vk_renderer *renderer);
bool vk_shader_info_create_blur_effects(struct vk_renderer *renderer);

bool create_vk_blur_pipelines(struct vk_renderer *renderer, struct vk_render_setup *setup,
		struct vk_pipeline_blur **out_blur1, struct vk_pipeline_blur **out_blur2,
		struct vk_pipeline_blur **out_blur_effects,
		struct vk_pipeline_blur **out_blur_effects_replace);
void delete_vk_blur_pipelines(struct vk_renderer *renderer, struct vk_pipeline_blur *blur);

#endif // !_RENDERER_VULKAN_SHADERS_H
