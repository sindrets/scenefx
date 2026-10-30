#version 450
#extension GL_ARB_shading_language_include : require

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform UBO {
	layout(offset = 80) vec4 params;
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
layout (constant_id = 1) const int TEXTURE_TRANSFORM = 0;

// Matches enum fx_tex_shader_effects
#define EFFECT_ROUND_CORNERS 1
#define EFFECT_CLIPPING 2
#define EFFECT_DISCARD_TRANSPARENT 4

// Matches enum wlr_vk_texture_transform
#define TEXTURE_TRANSFORM_IDENTITY 0
#define TEXTURE_TRANSFORM_SRGB 1
#define TEXTURE_TRANSFORM_ST2084_PQ 2
#define TEXTURE_TRANSFORM_GAMMA22 3
#define TEXTURE_TRANSFORM_BT1886 4

#include "corner_alpha.glsl"

float srgb_channel_to_linear(float x) {
	return mix(x / 12.92,
		pow((x + 0.055) / 1.055, 2.4),
		x > 0.04045);
}

vec3 srgb_color_to_linear(vec3 color) {
	return vec3(
		srgb_channel_to_linear(color.r),
		srgb_channel_to_linear(color.g),
		srgb_channel_to_linear(color.b)
	);
}

vec3 pq_color_to_linear(vec3 color) {
	float inv_m1 = 1 / 0.1593017578125;
	float inv_m2 = 1 / 78.84375;
	float c1 = 0.8359375;
	float c2 = 18.8515625;
	float c3 = 18.6875;
	vec3 num = max(pow(color, vec3(inv_m2)) - c1, 0);
	vec3 denom = c2 - c3 * pow(color, vec3(inv_m2));
	return pow(num / denom, vec3(inv_m1));
}

vec3 bt1886_color_to_linear(vec3 color) {
	float Lmin = 0.01;
	float Lmax = 100.0;
	float lb = pow(Lmin, 1.0 / 2.4);
	float lw = pow(Lmax, 1.0 / 2.4);
	float a  = pow(lw - lb, 2.4);
	float b  = lb / (lw - lb);
	vec3 L = a * pow(color + vec3(b), vec3(2.4));
	return (L - Lmin) / (Lmax - Lmin);
}

void main() {
	vec4 in_color = textureLod(tex, uv, 0);

	// Convert from pre-multiplied alpha to straight alpha
	float alpha = in_color.a;
	vec3 rgb;
	if (alpha == 0) {
		rgb = vec3(0);
	} else {
		rgb = in_color.rgb / alpha;
	}

	if (TEXTURE_TRANSFORM != TEXTURE_TRANSFORM_IDENTITY) {
		rgb = max(rgb, vec3(0));
	}
	if (TEXTURE_TRANSFORM == TEXTURE_TRANSFORM_SRGB) {
		rgb = srgb_color_to_linear(rgb);
	} else if (TEXTURE_TRANSFORM == TEXTURE_TRANSFORM_ST2084_PQ) {
		rgb = pq_color_to_linear(rgb);
	} else if (TEXTURE_TRANSFORM == TEXTURE_TRANSFORM_GAMMA22) {
		rgb = pow(rgb, vec3(2.2));
	} else if (TEXTURE_TRANSFORM == TEXTURE_TRANSFORM_BT1886) {
		rgb = bt1886_color_to_linear(rgb);
	}

	// Back to pre-multiplied alpha
	vec4 color = vec4(rgb * alpha, alpha);

	// Corner rounding
	if ((EFFECTS & EFFECT_ROUND_CORNERS) == EFFECT_ROUND_CORNERS) {
		color *= corner_alpha(
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

	out_color = color * data.params.x;

	if ((EFFECTS & EFFECT_DISCARD_TRANSPARENT) == EFFECT_DISCARD_TRANSPARENT) {
		if (color.a <= data.params.z) {
			discard;
		}
	}
}
