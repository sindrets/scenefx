#define SOURCE %d
#define EFFECTS %d

#define SOURCE_TEXTURE_RGBA 1
#define SOURCE_TEXTURE_RGBX 2
#define SOURCE_TEXTURE_EXTERNAL 3

#if !defined(SOURCE) || !defined(EFFECTS)
#error Missing shader preamble
#endif

#if SOURCE == SOURCE_TEXTURE_EXTERNAL
#extension GL_OES_EGL_image_external : require
#endif

#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_texcoord;

#if SOURCE == SOURCE_TEXTURE_EXTERNAL
uniform samplerExternalOES tex;
#elif SOURCE == SOURCE_TEXTURE_RGBA || SOURCE == SOURCE_TEXTURE_RGBX
uniform sampler2D tex;
#endif

uniform float alpha;
// Exact device-pixel edges of the destination box, (left, top, right,
// bottom), used to antialias an edge that misses the pixel grid.
uniform vec4 rect_box_edges;

#if EFFECTS
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
#endif

uniform bool discard_transparent;
uniform float ignore_alpha_threshold;

vec4 sample_texture() {
#if SOURCE == SOURCE_TEXTURE_RGBA || SOURCE == SOURCE_TEXTURE_EXTERNAL
	return texture2D(tex, v_texcoord);
#elif SOURCE == SOURCE_TEXTURE_RGBX
	return vec4(texture2D(tex, v_texcoord).rgb, 1.0);
#endif
}

#if EFFECTS
float rounded_box_coverage(vec4 edges, vec2 size, vec2 position,
		float radius_tl, float radius_tr, float radius_bl, float radius_br,
		float exponent);
float cutout_coverage(vec4 edges, vec2 size, vec2 position,
		float radius_tl, float radius_tr, float radius_bl, float radius_br,
		float exponent);
#endif

void main() {
	vec4 color = sample_texture();

#if EFFECTS
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

	gl_FragColor = color * alpha * quad_corner_alpha * clip_corner_alpha;
#else
	gl_FragColor = color * alpha;
#endif

	if (discard_transparent && color.a <= ignore_alpha_threshold) {
		discard;
	}
}
