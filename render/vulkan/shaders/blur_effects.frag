#version 450
#extension GL_ARB_shading_language_include : require

// Brightness/contrast/saturation/noise applied after the blur chain.
// Port of the GLES2 blur_effects.frag.
//
// This is also the composite that paints the blurred result into the scene, so
// it carries the corner rounding: GLES2 masks the blur with the window shape by
// drawing the blurred buffer through its rounded texture path, and Vulkan has
// to do the same or a rounded blur node renders with square corners.
//
// It also carries the transparency mask. GLES2 builds a stencil of the window
// with the mask's transparent pixels excluded and then draws the blurred
// result through it; Vulkan has no stencil in this render pass, so instead the
// mask is sampled here and the composite discarded where it is transparent.
// When the caller has no mask `has_mask` is zero and binding 1 holds a 1x1
// opaque image, so the fetch below is defined either way.

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D tex;
// The transparency mask. Always bound: when the caller has no mask, a 1x1
// opaque image is bound instead, so the fetch below is always defined and the
// blur chain can share this layout while leaving binding 1 to this shader.
layout(set = 0, binding = 1) uniform sampler2D mask;

layout(push_constant) uniform UBO {
	layout(offset = 80) float brightness;
	float contrast;
	float saturation;
	float noise;

	// Appended after the blur params so the fields above keep their offsets.
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

	// Coordinates the mask is sampled with, and the alpha below which a masked
	// pixel is treated as transparent. When has_mask is 0 the mask is ignored.
	vec2 mask_uv;
	vec2 mask_uv_size;
	float ignore_alpha_threshold;
	float has_mask;
} data;

#include "corner_alpha.glsl"

mat4 brightness_matrix() {
	float b = data.brightness - 1.0;
	return mat4(1, 0, 0, 0,
				0, 1, 0, 0,
				0, 0, 1, 0,
				b, b, b, 1);
}

mat4 contrast_matrix() {
	float t = (1.0 - data.contrast) / 2.0;
	return mat4(data.contrast, 0, 0, 0,
				0, data.contrast, 0, 0,
				0, 0, data.contrast, 0,
				t, t, t, 1);
}

mat4 saturation_matrix() {
	vec3 luminance = vec3(0.3086, 0.6094, 0.0820) * (1.0 - data.saturation);
	vec3 red = vec3(luminance.x);
	red.x += data.saturation;
	vec3 green = vec3(luminance.y);
	green.y += data.saturation;
	vec3 blue = vec3(luminance.z);
	blue.z += data.saturation;
	return mat4(red, 0,
				green, 0,
				blue, 0,
				0, 0, 0, 1);
}

float noise_amount(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 1689.1984);
	p3 += dot(p3, p3.yzx + 33.33);
	float hash = fract((p3.x + p3.y) * p3.z);
	return (mod(hash, 1.0) - 0.5) * data.noise;
}

void main() {
	vec4 color = texture(tex, uv);
	// Do *not* transpose the combined matrix when multiplying
	color = brightness_matrix() * contrast_matrix() * saturation_matrix() * color;
	color.xyz += noise_amount(uv);

	// Transparency mask: skip the composite where the window is transparent, so
	// the blur does not bleed outside the shapes that actually have content.
	// This replaces the GLES2 stencil.
	vec4 mask_color = texture(mask, data.mask_uv + uv * data.mask_uv_size);
	if (data.has_mask > 0.5 && mask_color.a <= data.ignore_alpha_threshold) {
		discard;
	}

	// Rounded corners, matching how the GLES2 path masks the blurred buffer.
	// Without exact edges (whole-pixel box) fall back to the corner ramp.
	// Only paint where the shape covers the whole pixel. The blur node shares
	// its outline with the client surface and the border ring sits over both,
	// so a faded arc would put the transparency-mask's blurred backdrop and the
	// surface's own antialiased edge on the same pixel, leaving a fringe of
	// background between them. Where the arc only partly covers a pixel the
	// ring is solid above it, so nothing is lost by not painting there.
	float coverage = data.box_edges_valid != 0.0
		? (rounded_box_coverage(
			data.box_edges,
			data.size,
			data.position,
			data.radius_top_left,
			data.radius_top_right,
			data.radius_bottom_left,
			data.radius_bottom_right) >= 1.0 ? 1.0 : 0.0)
		: corner_alpha(
			data.size,
			data.position,
			false,
			data.radius_top_left,
			data.radius_top_right,
			data.radius_bottom_left,
			data.radius_bottom_right);
	color *= coverage;

	out_color = color;
}
