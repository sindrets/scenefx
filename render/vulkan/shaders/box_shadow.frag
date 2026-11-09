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

	// Exact device-pixel edges of the destination box, (left, top, right,
	// bottom), and whether they are meaningful (0 = whole-pixel box).
	float box_edges_valid;
	vec4 box_edges;

	vec2 clip_size;
	vec2 clip_position;
	float clip_radius_top_left;
	float clip_radius_top_right;
	float clip_radius_bottom_left;
	float clip_radius_bottom_right;

	float clip_box_edges_valid;
	vec4 clip_box_edges;

	// (blur_sigma, corner_radius, unused, unused)
	vec4 shadow_params;
} data;

layout (constant_id = 0) const int EFFECTS = 0;

// Matches enum fx_quad_shader_effects
#define EFFECT_CLIPPING 2

#include "corner_alpha.glsl"

float gaussian(float x, float sigma) {
	const float pi = 3.141592653589793;
	return exp(-(x * x) / (2.0 * sigma * sigma)) / (sqrt(2.0 * pi) * sigma);
}

// Approximates the error function, needed for the gaussian integral.
vec2 erf(vec2 x) {
	vec2 s = sign(x), a = abs(x);
	x = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
	x *= x;
	return s - s / (x * x);
}

// Return the blurred mask along the x dimension.
float rounded_box_shadow_x(float x, float y, float sigma, float extent, float corner,
		vec2 half_size) {
	float delta = min(half_size.y - corner - abs(y), 0.0);
	float curved = half_size.x - corner + sqrt(max(0.0, corner * corner - delta * delta));
	vec2 edges = clamp(x + vec2(-curved, curved), -extent, extent);
	vec2 integral = 0.5 + 0.5 * erf(edges * (sqrt(0.5) / sigma));
	return integral.y - integral.x;
}

// Return the mask for the shadow of a box from lower to upper.
float rounded_box_shadow(vec2 lower, vec2 upper, vec2 point, float sigma, float corner) {
	vec2 center = (lower + upper) * 0.5;
	vec2 half_size = (upper - lower) * 0.5;
	point -= center;

	// The signal is only non-zero in a limited range, so don't waste samples.
	float extent = sigma * 2.0;
	float low = point.y - half_size.y;
	float high = point.y + half_size.y;
	float start = clamp(-extent, low, high);
	float end = clamp(extent, low, high);

	float step = (end - start) / 4.0;
	float y = start + step * 0.5;
	float value = 0.0;
	for (int i = 0; i < 4; i++) {
		value += rounded_box_shadow_x(point.x, point.y - y, sigma, extent, corner,
			half_size) * gaussian(y, sigma) * step;
		y += step;
	}

	// Normalize the truncated gaussian.
	const float x_whole = 0.954644; // erf(2*sqrt(0.5))
	const float y_sum = 0.963166;
	return value / (x_whole * y_sum);
}

// Render a box shadow, see: https://madebyevan.com/shaders/fast-rounded-rectangle-shadows/
//
// The gles2 path draws this with GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA (i.e.
// straight alpha), while our pipelines are fixed to pre-multiplied blending.
// Pre-multiplying here gives the same result: dst*(1-a) + rgb*a.
void main() {
	float blur_sigma = data.shadow_params.x;
	float corner_radius = data.shadow_params.y;

	// Cast the shadow from the box's exact edges, which is what the border
	// around it is antialiased against. With the whole-pixel edges the shadow
	// ends up to half a pixel away from where the border really is, and the
	// border's own partially covered pixel shadows nothing, so it blends with
	// whatever is behind the window instead of with the shadow.
	vec2 shadow_position = data.position;
	vec2 shadow_size = data.size;
	if (data.box_edges_valid != 0.0) {
		shadow_position = data.box_edges.xy;
		shadow_size = data.box_edges.zw - data.box_edges.xy;
	}

	float shadow_alpha = data.color.a * rounded_box_shadow(
		shadow_position + blur_sigma,
		shadow_position + shadow_size - blur_sigma,
		gl_FragCoord.xy,
		blur_sigma * 0.5,
		corner_radius);

	// Clipping
	if ((EFFECTS & EFFECT_CLIPPING) == EFFECT_CLIPPING) {
		shadow_alpha *= cutout_coverage(
			data.clip_box_edges,
			data.clip_size,
			data.clip_position,
			data.clip_radius_top_left,
			data.clip_radius_top_right,
			data.clip_radius_bottom_left,
			data.clip_radius_bottom_right);
	}

	// Colours arrive in sRGB and the render target is an sRGB image.
	vec3 rgb = pow(max(data.color.rgb, vec3(0.0)), vec3(2.2));

	out_color = vec4(rgb * shadow_alpha, shadow_alpha);
}
