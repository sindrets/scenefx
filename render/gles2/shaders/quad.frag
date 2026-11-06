#define EFFECTS %d

#if !defined(EFFECTS)
#error Missing shader preamble
#endif

#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec4 v_color;
varying vec2 v_texcoord;

uniform vec4 box_edges;

#if EFFECTS
uniform vec2 clip_size;
uniform vec2 clip_position;
uniform float clip_radius_top_left;
uniform float clip_radius_top_right;
uniform float clip_radius_bottom_left;
uniform float clip_radius_bottom_right;
uniform vec4 clip_box_edges;
#endif

float box_coverage(vec4 edges);
float rounded_box_coverage(vec4 edges, vec2 size, vec2 position,
		float radius_tl, float radius_tr, float radius_bl, float radius_br);
float cutout_coverage(vec4 edges, vec2 size, vec2 position,
		float radius_tl, float radius_tr, float radius_bl, float radius_br);

void main() {
	// Fade the rect out over the pixels its exact edges only partly cover.
	vec4 color = v_color * box_coverage(box_edges);

#if EFFECTS
	// Clipping
	color *= cutout_coverage(
		clip_box_edges,
		clip_size,
		clip_position,
		clip_radius_top_left,
		clip_radius_top_right,
		clip_radius_bottom_left,
		clip_radius_bottom_right
	);
#endif

	gl_FragColor = color;
}
