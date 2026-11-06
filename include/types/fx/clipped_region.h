#ifndef TYPES_FX_CLIPPED_REGION_H
#define TYPES_FX_CLIPPED_REGION_H
#include <wlr/util/box.h>
#include <scenefx/types/fx/clipped_region.h>

struct fx_corner_fradii {
    float top_left;
    float top_right;
    float bottom_right;
    float bottom_left;
};


bool fx_corner_fradii_is_empty(const struct fx_corner_fradii* corners);

struct fx_corner_fradii fx_corner_radii_scale(struct fx_corner_radii, float scale);

struct clipped_fregion {
    struct wlr_box area;
    /* Exact, unrounded device-pixel area. The renderers antialias against
     * these edges so a region that does not land on the pixel grid fades
     * across its boundary pixels instead of snapping to whole ones. Left
     * empty by callers that work in whole pixels, in which case `area` is
     * used as is. */
    struct wlr_fbox frac_area;
    struct fx_corner_fradii corners;
};

static __always_inline struct wlr_fbox box_to_fbox(const struct wlr_box *box) {
	return (struct wlr_fbox){
		.x = box->x,
		.y = box->y,
		.width = box->width,
		.height = box->height,
	};
}

/**
 * The exact area of the region, preferring `frac_area` when the caller
 * supplied it and falling back to the rounded `area` otherwise.
 */
static __always_inline struct wlr_fbox clipped_fregion_frac_area(
		const struct clipped_fregion *fregion) {
	return wlr_fbox_empty(&fregion->frac_area) ?
		box_to_fbox(&fregion->area) : fregion->frac_area;
}

// A clipped region is worth handing to the shader as long as it has an area.
// Rounded corners are not required: a plain rectangular hole's inner edge can
// still miss the pixel grid, and only the shader can antialias it (the integer
// scissor that apply_clip_region() subtracts cannot).
static __always_inline bool clipped_fregion_is_valid(const struct clipped_fregion *fregion) {
	struct wlr_fbox frac_area = clipped_fregion_frac_area(fregion);
	return !wlr_fbox_empty(&frac_area);
};

/**
 * The hole shrunk to the whole pixels it fully covers.
 *
 * A hole is not antialiased the way a shape is. The shape drawn inside it (a
 * client surface, say) fades across the very pixel both edges share, and if
 * the hole faded across that pixel too the two fades would compose: the pixel
 * would be left with exactly c*(1-c) of whatever sits behind the hole, so the
 * background bleeds through the seam between border and surface. Shrinking
 * the hole to its fully covered pixels instead keeps the surrounding shape
 * solid underneath, so the shape on top is the only thing fading there and it
 * lands on its own fractional edge unaided.
 */
static __always_inline struct wlr_fbox clipped_fregion_cover_area(
		const struct clipped_fregion *fregion) {
	struct wlr_fbox area = clipped_fregion_frac_area(fregion);
	float x1 = ceilf(area.x);
	float y1 = ceilf(area.y);
	float x2 = floorf(area.x + area.width);
	float y2 = floorf(area.y + area.height);

	return (struct wlr_fbox){
		.x = x1,
		.y = y1,
		.width = x2 > x1 ? x2 - x1 : 0.0f,
		.height = y2 > y1 ? y2 - y1 : 0.0f,
	};
}

// Whether a box's edges fall between device pixels, and therefore need the
// shader's coverage fade instead of landing on whole pixels anyway.
static __always_inline bool fbox_has_fractional_edges(const struct wlr_fbox *fbox) {
	return fbox->x != floorf(fbox->x) || fbox->y != floorf(fbox->y)
		|| fbox->width != floorf(fbox->width)
		|| fbox->height != floorf(fbox->height);
};

#endif
