#ifndef SCENEFX_TYPES_FX_CLIPPED_REGION_H
#define SCENEFX_TYPES_FX_CLIPPED_REGION_H

#include <linux/stddef.h>
#include <wlr/util/box.h>

// uint16_t is a reasonable enough range for corner radius, and four of them plus
// the exponent fit in 12 bytes.
// Note: layout is on purpose to be clockwise
struct fx_corner_radii {
	uint16_t top_left;
	uint16_t top_right;
	uint16_t bottom_right;
	uint16_t bottom_left;

	// Shape of the corner curve, as the exponent of the superellipse
	//
	//     |x / a|^n + |y / b|^n = 1
	//
	// that is inscribed in the corner's a x b box. `a` and `b` are both the
	// corner radius above, so a corner is a square box with the curve drawn
	// inside it.
	//
	// n = 2 gives a circle, i.e. the classic rounded corner, which is also
	// what an unset (0) exponent means. Raising n squares the curve off and
	// takes the curvature at the point where it meets the straight edge down
	// to zero, so the corner blends into the edge without the visible kink a
	// circular corner has. Values below 2 sharpen the corner instead, down to
	// a straight diagonal at n = 1.
	//
	// Use corner_exponent_normalize() before trusting this value, and
	// fx_corner_radii_set_exponent() to set it.
	float exponent;
};

#define CORNER_RADIUS_MAX (UINT16_MAX)

static __always_inline uint16_t corner_radius_clamp(int radius) {
	if (radius <= 0) {
		return 0;
	}

	if (radius > CORNER_RADIUS_MAX) {
		return CORNER_RADIUS_MAX;
	}

	return radius;
}

static __always_inline struct fx_corner_radii corner_radii_new(int top_left, int top_right, int bottom_right, int bottom_left) {
	return (struct fx_corner_radii) {
		corner_radius_clamp(top_left),
		corner_radius_clamp(top_right),
		corner_radius_clamp(bottom_right),
		corner_radius_clamp(bottom_left),
	};
}

static __always_inline struct fx_corner_radii corner_radii_all(int radius) {
	return corner_radii_new(radius, radius, radius, radius);
}

static __always_inline struct fx_corner_radii corner_radii_none(void) {
	return corner_radii_all(0);
}

#define CORNER_EXPONENT_CIRCLE 2.0f
#define CORNER_EXPONENT_MIN 1.0f
#define CORNER_EXPONENT_MAX 16.0f

/**
 * Clamps a superellipse exponent to the range the renderers can draw, and
 * maps the "unset" value of 0 to a circle.
 */
static __always_inline float corner_exponent_normalize(float exponent) {
	// Also catches NaN, which would otherwise slip through both comparisons.
	if (!(exponent > 0.0f)) {
		return CORNER_EXPONENT_CIRCLE;
	}

	if (exponent < CORNER_EXPONENT_MIN) {
		return CORNER_EXPONENT_MIN;
	}

	if (exponent > CORNER_EXPONENT_MAX) {
		return CORNER_EXPONENT_MAX;
	}

	return exponent;
}

static __always_inline struct fx_corner_radii fx_corner_radii_set_exponent(
		struct fx_corner_radii corners, float exponent) {
	corners.exponent = corner_exponent_normalize(exponent);
	return corners;
}

static __always_inline struct fx_corner_radii corner_radii_all_exponent(int radius, float exponent) {
	return fx_corner_radii_set_exponent(corner_radii_all(radius), exponent);
}

#define corner_radii_func(name, tl, tr, br, bl) static __always_inline struct fx_corner_radii corner_radii_##name(int radius) { return corner_radii_new(tl, tr, br, bl); }

corner_radii_func(top, radius, radius, 0, 0);
corner_radii_func(bottom, 0, 0, radius, radius);
corner_radii_func(left, radius, 0, 0, radius);
corner_radii_func(right, 0, radius, radius, 0);

struct fx_corner_radii fx_corner_radii_extend(struct fx_corner_radii corners, int extend);

void fx_corner_radii_transform(enum wl_output_transform transform,
		struct fx_corner_radii *corners);

bool fx_corner_radii_eq(struct fx_corner_radii lhs, struct fx_corner_radii rhs);

/**
 * Retains the corner values from `input` based on if the corresponding corner value in `filter` is non-zero
 *
 * This can be compared to an `AND` operation
 */
static __always_inline struct fx_corner_radii fx_corner_radii_filter(struct fx_corner_radii input, struct fx_corner_radii filter) {
	struct fx_corner_radii corners = corner_radii_new(
		input.top_left && filter.top_left ? input.top_left : 0,
		input.top_right && filter.top_right ? input.top_right : 0,
		input.bottom_right && filter.bottom_right ? input.bottom_right : 0,
		input.bottom_left && filter.bottom_left ? input.bottom_left : 0
	);
	corners.exponent = input.exponent;
	return corners;
};

/**
 * Picks the value of the corner from `lhs` if non-zero, otherwise picks the corner value from `rhs`
 *
 * This can be compared to an `OR` operation
 */
static __always_inline struct fx_corner_radii fx_corner_radii_pick(struct fx_corner_radii lhs, struct fx_corner_radii rhs) {
	struct fx_corner_radii corners = corner_radii_new(
		lhs.top_left ? lhs.top_left : rhs.top_left,
		lhs.top_right ? lhs.top_right : rhs.top_right,
		lhs.bottom_right ? lhs.bottom_right : rhs.bottom_right,
		lhs.bottom_left ? lhs.bottom_left : rhs.bottom_left
	);
	corners.exponent = lhs.top_left || lhs.top_right || lhs.bottom_right || lhs.bottom_left
		? lhs.exponent : rhs.exponent;
	return corners;
};

bool fx_corner_radii_is_empty(const struct fx_corner_radii* corners);

struct clipped_region {
	struct wlr_box area;
	struct fx_corner_radii corners;
};

struct clipped_region clipped_region_get_default(void);

#endif // !SCENEFX_TYPES_FX_CLIPPED_REGION_H
