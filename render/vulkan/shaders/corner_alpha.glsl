// Distance to a box corner's curve, in pixels: negative inside the shape,
// positive outside it. `q` is the offset from the corner's centre, positive
// towards the corner's own square (see corner_sdf), and `radius` is the size of
// that square.
//
// `exponent` is the superellipse exponent of the curve:
//
//     |x / radius|^exponent + |y / radius|^exponent = 1
//
// 2 is a circle, i.e. the classic rounded corner. Larger values square the
// corner off and take the curvature where the curve meets the straight edge
// down to zero, so the corner flows into the edge instead of kinking. See
// struct fx_corner_radii.
float get_dist(vec2 q, float radius, float exponent) {
	// The part of the shape that lies along the box's straight edges: a point
	// outside the corner square in one axis is that far inside the edge.
	float straight = min(max(q.x, q.y), 0.0);
	vec2 to_corner = max(q, 0.0);

	if (radius <= 0.0) {
		// A square corner: fall back to the plain box distance, which is what
		// the corner's straight edges alone describe.
		return straight + length(to_corner);
	}

	if (exponent == 2.0) {
		return straight + length(to_corner) - radius;
	}

	// The curve's homogeneous norm is the scale factor of the copy of the
	// curve the point sits on, so norm - 1 crosses zero exactly on the curve.
	// Scaled back to pixels it is an exact distance on the two axes where the
	// curve meets the straight edge, and its gradient there is 1; along the
	// diagonal the gradient is 2^(1/exponent - 1/2), i.e. at most about a
	// quarter narrower, which widens the antialiasing ramp by that much and
	// no more.
	//
	// Clamped so that pow() cannot overflow for points far from the corner.
	// Any point out there is well outside the shape either way.
	vec2 u = min(to_corner / radius, vec2(100.0));
	float norm = pow(pow(u.x, exponent) + pow(u.y, exponent), 1.0 / exponent);
	return straight + radius * (norm - 1.0);
}

// Exact axis-aligned coverage of the box [edges.xy, edges.zw] in device
// pixels, box filtered over the pixel's own area. `edges` holds
// (left, top, right, bottom) as unrounded device-pixel coordinates, so a box
// that does not land on the pixel grid fades across its boundary pixels
// instead of snapping to them. Whole-pixel boxes yield exactly 0.0 or 1.0.
float box_coverage(vec4 edges) {
	vec2 px = gl_FragCoord.xy;
	vec2 cov = min(edges.zw, px + 0.5) - max(edges.xy, px - 0.5);
	cov = clamp(cov, 0.0, 1.0);
	return cov.x * cov.y;
}

// Distance field ramp of a rounded corner: 0.0 on or inside the shape, 1.0 a
// pixel beyond it. Only meaningful inside the corner regions of the box.
float corner_sdf(vec2 size, vec2 position, float radius_tl, float radius_tr,
		float radius_bl, float radius_br, float exponent) {
	vec2 relative_pos = gl_FragCoord.xy - position;

	vec2 top_left = abs(relative_pos - size) - size + radius_tl;
	vec2 top_right = abs(relative_pos - vec2(0, size.y)) - size + radius_tr;
	vec2 bottom_left = abs(relative_pos - vec2(size.x, 0)) - size + radius_bl;
	vec2 bottom_right = abs(relative_pos) - size + radius_br;

	float dist = max(
		max(get_dist(top_left, radius_tl, exponent),
			get_dist(top_right, radius_tr, exponent)),
		max(get_dist(bottom_left, radius_bl, exponent),
			get_dist(bottom_right, radius_br, exponent))
	);

	return smoothstep(0.0, 1.0, dist);
}

// Whether the fragment lies in one of the four corner squares of the box.
bool in_corner_region(vec2 relative_pos, vec2 size, float radius_tl, float radius_tr,
		float radius_bl, float radius_br) {
	return (radius_tl > 0.0 && relative_pos.x <= radius_tl && relative_pos.y <= radius_tl) ||
		(radius_tr > 0.0 && relative_pos.x >= size.x - radius_tr && relative_pos.y <= radius_tr) ||
		(radius_bl > 0.0 && relative_pos.x <= radius_bl && relative_pos.y >= size.y - radius_bl) ||
		(radius_br > 0.0 && relative_pos.x >= size.x - radius_br && relative_pos.y >= size.y - radius_br);
}

// Coverage of the shape itself: the exact box edges multiplied by the rounded
// corner SDF. `edges` are unrounded device-pixel edges, so straight edges that
// miss the pixel grid are antialiased too, not just the corners.
float rounded_box_coverage(vec4 edges, vec2 size, vec2 position,
		float radius_tl, float radius_tr, float radius_bl, float radius_br,
		float exponent) {
	float cov = box_coverage(edges);
	if (cov <= 0.0) {
		return 0.0;
	}

	if (radius_tl <= 0.0 && radius_tr <= 0.0
			&& radius_bl <= 0.0 && radius_br <= 0.0) {
		return cov;
	}

	vec2 relative_pos = gl_FragCoord.xy - position;
	if (!in_corner_region(relative_pos, size, radius_tl, radius_tr, radius_bl, radius_br)) {
		return cov;
	}

	return cov * (1.0 - corner_sdf(size, position, radius_tl, radius_tr,
		radius_bl, radius_br, exponent));
}

// Inverse of rounded_box_coverage(), but hard: 0.0 only where the shape covers
// the whole pixel, 1.0 everywhere else.
//
// A hole must not fade. The shape drawn inside it (a client surface, say) fades
// across the very pixels the hole's edge passes through, and the border sits
// *under* that shape, so if the hole faded there too the pixel would be left
// with exactly c*(1-c) of whatever is behind the border: the background bleeds
// through the seam between border and surface.
//
// Cutting the hole hard instead makes the border solid right through the
// boundary pixel, and the shape on top — which fades by its own coverage c —
// supplies the antialiasing. The pixel ends up as c*surface + (1-c)*border,
// which is the blend the geometry asks for, with nothing behind it showing.
// The hole and the shape inside it must still describe the same geometry, so
// both are given the exact fractional box and the same corner radii (see
// client_inner_corner_location() in the compositor).
float cutout_coverage(vec4 edges, vec2 size, vec2 position,
		float radius_tl, float radius_tr, float radius_bl, float radius_br,
		float exponent) {
	return rounded_box_coverage(edges, size, position,
		radius_tl, radius_tr, radius_bl, radius_br, exponent) >= 1.0 ? 0.0 : 1.0;
}

// Note: Returns 0.0 if outside, 1.0 if inside the bounds. The is_cutout parameter
// reverses this, 0.0 inside cutout and 1.0 for outside cutout.
//
// Kept for the callers that only have a rounded box (no exact edges), such as
// box shadows: they pass a slightly inset box to the cutout so the corner ramp
// lands on whole pixels.
float corner_alpha(vec2 size, vec2 position, bool is_cutout,
		float radius_tl, float radius_tr, float radius_bl, float radius_br,
		float exponent) {
	if (radius_tl <= 0.0
			&& radius_tr <= 0.0
			&& radius_bl <= 0.0
			&& radius_br <= 0.0) {
		return 1.0;
	}

	vec2 relative_pos = (gl_FragCoord.xy - position);

	if (relative_pos.x < 0.0 || relative_pos.y < 0.0
			|| relative_pos.x > size.x || relative_pos.y > size.y) {
		if (is_cutout) {
			return 1.0;
		}
		discard;
	}

	if (!in_corner_region(relative_pos, size, radius_tl, radius_tr, radius_bl, radius_br)) {
		if (is_cutout) {
			discard;
		}
		return 1.0;
	}

	float result = corner_sdf(size, position, radius_tl, radius_tr,
		radius_bl, radius_br, exponent);
	return is_cutout ? result : 1.0 - result;
}
