#version 450

// Dual-Kawase downsample pass. Port of the GLES2 blur1.frag.

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D tex;

// The vertex stage occupies the first 80 bytes (mat4 proj + uv_offset + uv_size).
// halfpixel is declared first so the vec2 keeps its 8 byte alignment without padding.
layout(push_constant) uniform UBO {
	layout(offset = 80) vec2 halfpixel;
	float radius;
	// Texel centres of the valid (already rendered) source area.
	vec2 uv_min;
	vec2 uv_max;
} data;

// Taps that land outside the rendered area would read stale buffer contents,
// so clamp them and extend the edge instead.
vec4 sample_tex(vec2 p) {
	return texture(tex, clamp(p, data.uv_min, data.uv_max));
}

void main() {
	vec2 suv = uv * 2.0;

	vec4 sum = sample_tex(suv) * 4.0;
	sum += sample_tex(suv - data.halfpixel.xy * data.radius);
	sum += sample_tex(suv + data.halfpixel.xy * data.radius);
	sum += sample_tex(suv + vec2(data.halfpixel.x, -data.halfpixel.y) * data.radius);
	sum += sample_tex(suv - vec2(data.halfpixel.x, -data.halfpixel.y) * data.radius);

	out_color = sum / 8.0;
}
