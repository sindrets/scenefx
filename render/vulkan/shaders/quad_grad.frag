#version 450
#extension GL_ARB_shading_language_include : require

layout(location = 0) out vec4 out_color;

// Matches MAX_GRADIENT_COLORS in include/render/vulkan/shaders.h
#define MAX_GRADIENT_COLORS 16

layout(std140, set = 0, binding = 0) uniform Gradient {
	vec4 colors[MAX_GRADIENT_COLORS];
	// (degree, linear, blend, count)
	vec4 params;
	// (range.width, range.height, range.x, range.y)
	vec4 range;
	// (origin.x, origin.y, unused, unused)
	vec4 origin;
} gradient;

layout(push_constant) uniform UBO {
	layout(offset = 80) vec4 color;
	vec2 size;
	vec2 position;
	float radius_top_left;
	float radius_top_right;
	float radius_bottom_left;
	float radius_bottom_right;

	vec2 clip_size;
	vec2 clip_position;
	float clip_radius_top_left;
	float clip_radius_top_right;
	float clip_radius_bottom_left;
	float clip_radius_bottom_right;
} data;

layout (constant_id = 0) const int EFFECTS = 0;

// Matches enum fx_quad_shader_effects
#define EFFECT_ROUND_CORNERS 1
#define EFFECT_CLIPPING 2

#include "corner_alpha.glsl"
#include "gradient.glsl"

void main() {
	vec4 color = gradient_color(
		gradient.colors,
		int(gradient.params.w),
		gradient.range.xy,
		gradient.range.zw,
		gradient.origin.xy,
		gradient.params.x,
		gradient.params.y > 0.5,
		gradient.params.z > 0.5);

	// Like the gles2 quad_grad shader, the base colour's alpha is only used to
	// fade out the corners of a rounded gradient rectangle.
	if ((EFFECTS & EFFECT_ROUND_CORNERS) == EFFECT_ROUND_CORNERS) {
		color *= data.color.a * corner_alpha(
			data.size - 0.5,
			data.position + 0.25,
			false,
			data.radius_top_left,
			data.radius_top_right,
			data.radius_bottom_left,
			data.radius_bottom_right
		);
	}

	// Clipping
	if ((EFFECTS & EFFECT_CLIPPING) == EFFECT_CLIPPING) {
		color *= corner_alpha(
			data.clip_size - 1.0,
			data.clip_position + 0.5,
			true,
			data.clip_radius_top_left,
			data.clip_radius_top_right,
			data.clip_radius_bottom_left,
			data.clip_radius_bottom_right
		);
	}

	// Gradient colours are given in sRGB, the render target is an sRGB image.
	out_color = srgb_premult_to_linear(color);
}
