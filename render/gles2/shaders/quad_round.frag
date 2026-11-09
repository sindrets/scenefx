#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec4 v_color;
varying vec2 v_texcoord;

uniform vec4 rect_box_edges;

uniform vec2 size;
uniform vec2 position;
uniform float radius_top_left;
uniform float radius_top_right;
uniform float radius_bottom_left;
uniform float radius_bottom_right;
uniform float exponent;

uniform vec2 clip_size;
uniform vec2 clip_position;
uniform float clip_radius_top_left;
uniform float clip_radius_top_right;
uniform float clip_radius_bottom_left;
uniform float clip_radius_bottom_right;
uniform float clip_exponent;
uniform vec4 clip_box_edges;

float box_coverage(vec4 edges);
float rounded_box_coverage(vec4 edges, vec2 size, vec2 position,
		float radius_tl, float radius_tr, float radius_bl, float radius_br,
		float exponent);
float cutout_coverage(vec4 edges, vec2 size, vec2 position,
		float radius_tl, float radius_tr, float radius_bl, float radius_br,
		float exponent);

void main() {
	float quad_corner_alpha = rounded_box_coverage(
		rect_box_edges,
		size,
		position,
		radius_top_left,
		radius_top_right,
		radius_bottom_left,
		radius_bottom_right,
		exponent
	);

	// Clipping
	float clip_corner_alpha = cutout_coverage(
		clip_box_edges,
		clip_size,
		clip_position,
		clip_radius_top_left,
		clip_radius_top_right,
		clip_radius_bottom_left,
		clip_radius_bottom_right,
		clip_exponent
	);

	gl_FragColor = v_color * quad_corner_alpha * clip_corner_alpha;
}
