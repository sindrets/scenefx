// Shared gradient evaluation, ported from the GLES2 `gradient.frag` include.
//
// `size`, `grad_box` and `origin` have the same meaning as in the GL path: the
// gradient is evaluated in the normalized space of
// `grad_box`..`grad_box + size`, so it works identically for add_rect_grad and
// add_rounded_rect_grad.
vec4 gradient_color(vec4 colors[MAX_GRADIENT_COLORS], int count, vec2 size,
		vec2 grad_box, vec2 origin, float degree, bool linear, bool blend) {
	float step;

	vec2 normal = (gl_FragCoord.xy - grad_box) / size;
	vec2 uv = normal - origin;

	float rad = radians(degree);

	if (linear) {
		uv *= vec2(1.0) / vec2(abs(cos(rad)) + abs(sin(rad)));

		vec2 rotated = vec2(uv.x * cos(rad) - uv.y * sin(rad) + origin.x,
				uv.x * sin(rad) + uv.y * cos(rad) + origin.y);

		step = rotated.x;
	} else {
		uv = vec2(uv.x * cos(rad) - uv.y * sin(rad),
				uv.x * sin(rad) + uv.y * cos(rad));

		uv = vec2(-atan(uv.y, uv.x) / 3.14159265 * 0.5 + 0.5, 0.0);
		step = uv.x;
	}

	// Clamp so a degenerate size/origin combination cannot index out of bounds.
	int last = max(count - 1, 0);
	step = clamp(step, 0.0, 1.0);

	if (!blend) {
		float smooth_fac = 1.0 / float(max(count, 1));
		int ind = clamp(int(step / smooth_fac), 0, last);
		return colors[ind];
	}

	float smooth_fac = 1.0 / float(max(count - 1, 1));
	int ind = clamp(int(step / smooth_fac), 0, last);
	float at = float(ind) * smooth_fac;

	vec4 color = colors[ind];
	if (ind > 0) {
		color = mix(colors[ind - 1], color, smoothstep(at - smooth_fac, at, step));
	}
	if (ind <= count - 2) {
		color = mix(color, colors[ind + 1], smoothstep(at, at + smooth_fac, step));
	}

	return color;
}

// Gradient colours arrive pre-multiplied and in sRGB (exactly what the GLES2
// shader writes straight to the framebuffer). Vulkan render targets are sRGB
// images, so the hardware applies the linear->sRGB transfer on write; convert
// the incoming colours the other way first.
vec4 srgb_premult_to_linear(vec4 color) {
	if (color.a <= 0.0) {
		return vec4(0.0);
	}
	vec3 rgb = pow(max(color.rgb / color.a, vec3(0.0)), vec3(2.2));
	return vec4(rgb * color.a, color.a);
}
