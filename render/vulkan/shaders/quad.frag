#version 450
#extension GL_ARB_shading_language_include : require

layout(location = 0) out vec4 out_color;
layout(push_constant) uniform UBO {
	layout(offset = 80) vec4 color;
	vec2 size;
	vec2 position;
	float radius_top_left;
	float radius_top_right;
	float radius_bottom_left;
	float radius_bottom_right;

	// Exact device-pixel edges, (left, top, right, bottom), and whether they
	// are meaningful. A zero `has_box_edges` means the caller renders a
	// whole-pixel box, so the quad covers its pixels fully.
	float box_edges_valid;
	float exponent;
	vec2 _pad;
	vec4 box_edges;

	vec2 clip_size;
	vec2 clip_position;
	float clip_radius_top_left;
	float clip_radius_top_right;
	float clip_radius_bottom_left;
	float clip_radius_bottom_right;

	float clip_box_edges_valid;
	float clip_exponent;
	vec2 _clip_pad;
	vec4 clip_box_edges;
} data;

layout (constant_id = 0) const int EFFECTS = 0;

// Matches enum fx_quad_shader_effects
#define EFFECT_ROUND_CORNERS 1
#define EFFECT_CLIPPING 2

#include "corner_alpha.glsl"

void main() {
	out_color = data.color;

	// Exact edge coverage, so a box that misses the pixel grid fades across
	// its boundary pixels instead of snapping to them. The colour is
	// pre-multiplied, so the coverage scales every channel. Rounded corners
	// fold the edge coverage into rounded_box_coverage(), so only the
	// plain-rect path needs it here.
	if ((EFFECTS & EFFECT_ROUND_CORNERS) != EFFECT_ROUND_CORNERS
			&& data.box_edges_valid != 0.0) {
		out_color *= box_coverage(data.box_edges);
	}

	// Corner rounding
	if ((EFFECTS & EFFECT_ROUND_CORNERS) == EFFECT_ROUND_CORNERS) {
		out_color *= rounded_box_coverage(
			data.box_edges,
			data.size,
			data.position,
			data.radius_top_left,
			data.radius_top_right,
			data.radius_bottom_left,
			data.radius_bottom_right,
			data.exponent
		);
	}

	// Clipping
	if ((EFFECTS & EFFECT_CLIPPING) == EFFECT_CLIPPING) {
		out_color *= cutout_coverage(
			data.clip_box_edges,
			data.clip_size,
			data.clip_position,
			data.clip_radius_top_left,
			data.clip_radius_top_right,
			data.clip_radius_bottom_left,
			data.clip_radius_bottom_right,
			data.clip_exponent
		);
	}
}
