#ifndef _RENDERER_VULKAN_VULKAN_H
#define _RENDERER_VULKAN_VULKAN_H

#include <vulkan/vulkan_core.h>
#include <wayland-util.h>
#include <wlr/render/vulkan.h>
#include <wlr/render/wlr_renderer.h>

#include "render/fx_renderer.h"
#include "render/vulkan/pipeline.h"
#include "render/vulkan/shaders.h"

struct vk_renderer;
struct vk_render_setup;

struct vk_buffer {
	struct wlr_buffer *wlr_buffer;

	struct vk_renderer *vk_renderer;
	struct wl_list link; // vk_renderer.buffers

	struct wlr_addon addon;
};

struct vk_buffer *vk_buffer_get_or_create(struct fx_renderer *fx_renderer,
		struct wlr_buffer *wlr_buffer);
void vk_buffer_get_or_allocate(struct fx_renderer *fx_renderer,
		struct wlr_allocator *allocator, int width, int height, bool has_alpha,
		struct vk_buffer **vk_buffer, bool *failed);
/** Note: Doesn't drop the wlr_buffer, so should only be used internally. */
void vk_buffer_destroy(struct vk_buffer *buffer);

// Two targets are enough: the dual-Kawase chain only ever ping-pongs between a
// source and a destination.
#define VK_EFFECT_IMAGE_COUNT 2

/**
 * Offscreen colour target for the blur chain. Full output size: the dual-Kawase
 * down/up scaling is expressed by shrinking the scissor rather than the image,
 * matching the GLES2 implementation (and what blur1.frag's `uv * 2.0` expects).
 */
struct vk_effect_image {
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkFramebuffer framebuffer;
	VkDescriptorSet ds;
	uint32_t width, height;
	// Tracked so the barrier before each use knows the source layout. The scene
	// render pass declares finalLayout SHADER_READ_ONLY_OPTIMAL, so after any
	// use the image sits there; before first use it is still UNDEFINED.
	VkImageLayout layout;
};

// How many distinct blur transparency masks an output keeps image views for.
// Masks are per scene buffer, so a handful can be live in one frame; the cache
// only has to be big enough that they do not evict each other within a frame.
#define VK_MASK_VIEW_CACHE_LEN 4

// Holds a copy of the frame's pixels in the blur padding ring, taken before the
// frame renders and pasted back afterwards. Transfer-only: no framebuffer or
// descriptor set, since nothing ever draws with it.
struct vk_saved_pixels {
	VkImage image;
	VkDeviceMemory memory;
	uint32_t width, height;
	// Whether a frame has already stored its padding ring here. A paste-back
	// before any save has nothing to restore and must be skipped.
	bool initialised;
};

// Our own view of a mask image, plus the layout it is sampled from.
struct vk_mask_view {
	VkImage image;
	VkFormat format;
	VkImageView view;
	VkImageLayout layout;
};

struct vk_offscreen_buffers {
	struct fx_offscreen_buffers fx_offscreen_buffers;

	// The Vulkan renderer keeps none of GLES2's intermediate wlr_buffers. Blur
	// ping-pongs between two 16F effect images instead (effect_images below), the
	// blur-padding snapshot is a VkImage (saved_pixels), and the optimized-blur
	// cache is a 16F effect image too. Copying the 16F chain output into an 8-bit
	// wlr_buffer would not even be expressible with vkCmdCopyImage, and the
	// unblurred copy GLES2 keeps for reduced-strength optimized blur is
	// unnecessary here: re-blurring the live blend image is the same content.

	// Backing store for the optimized blur cache, the Vulkan counterpart of
	// GLES2's optimized_blur_buffer: the blurred bottom layer for this output.
	//
	// Two writers, and they must agree, or the cache feeds itself:
	//
	//  - WLR_SCENE_NODE_OPTIMIZED_BLUR nodes refill it and draw nothing. The
	//    scene calls one of those once while it is dirty and then never draws
	//    it again, so a node that painted its own output here would have that
	//    paint blurred back into the next refill.
	//  - Blur nodes with use_optimized_blur read it instead of re-blurring.
	//    scenefx sets that from should_only_blur_bottom_layer, which MangoWM
	//    enables via blur_optimized=1 (its default). This is what makes the
	//    blur visible at all.
	//
	// Per output rather than per renderer: two monitors of different sizes
	// would otherwise tear down and rebuild each other's cache every frame.
	struct vk_effect_image optimized_blur;

	// Ping-pong targets for the blur chain and the effect pass' render pass.
	//
	// All per output, like GLES2's fx_offscreen_buffers. Sharing them on the
	// renderer makes every size change tear down and rebuild the images, which
	// with two differently-sized outputs happens on every single pass - and each
	// teardown has to wait for the device (see vk_deferred_destroy).
	VkRenderPass effect_render_pass;
	struct vk_effect_image effect_images[VK_EFFECT_IMAGE_COUNT];
	struct vk_saved_pixels saved_pixels;
	// A mask image can be sampled by a blur node several times per frame, so the
	// views are cached. More than one can be live at once (each layer surface
	// with a mask has its own), hence a small set rather than a single entry.
	struct vk_mask_view mask_views[VK_MASK_VIEW_CACHE_LEN];
	// Next mask_views entry to recycle once every slot is taken.
	size_t mask_views_next;
	VkDescriptorSet blend_ds;
	VkImageView blend_ds_view;
};

struct vk_offscreen_buffers *vk_get_offscreen_buffers(
		struct fx_offscreen_buffers *offscreen_buffers);

// A host visible uniform buffer sliced up into per-draw gradient definitions.
// The gradient shader binds it with a dynamic offset, so one arena serves many
// gradient draw calls in the same pass. Arenas are created lazily (most passes
// have no gradients at all) and dropped in the command buffer's resource
// callback, see handle_release_resources in pass.c.
struct vk_gradient_arena {
	struct wl_list link; // struct vk_render_pass.gradient_arenas

	VkDescriptorPool descriptor_pool;
	VkDescriptorSet descriptor_set;
	VkBuffer buffer;
	VkDeviceMemory memory;
	void *mapped;
	VkDeviceSize size;
	VkDeviceSize used;
};

// Everything that has to outlive the pass until its command buffer has
// executed: the gradient uniform arenas, plus any Vulkan objects queued through
// vk_deferred_destroy(). wlroots allows a single resources callback per command
// buffer, so both live behind this one reference-counted object.
struct vk_pass_resources {
	size_t refs;
	struct vk_renderer *vk_renderer;
	struct wl_list arenas; // struct vk_gradient_arena.link
	VkCommandBuffer command_buffer;
};

struct vk_render_pass {
	struct fx_render_pass fx_render_pass;
	struct vk_renderer *vk_renderer;

	struct vk_buffer *render_buffer;
	// Upstream uses different VkRenderPasses for each type of rendering pathway
	struct vk_render_setup *render_setup;
	VkCommandBuffer command_buffer;
	VkPipeline bound_pipeline;
	float projection[9];

	// Contains output-specific framebuffers.
	// NULL when no advanced effects like blur is being used in the current pass.
	struct vk_offscreen_buffers *vk_offscreen_buffers;

	// Gradient support: lazily allocated uniform buffer arenas. Reference
	// counted, because the arenas can only be freed once the command buffer
	// they were recorded into is done, which is well after the pass is gone.
	struct vk_pass_resources *resources;
};

struct vk_render_pass *vk_get_render_pass(struct fx_render_pass *fx_render_pass);

struct fx_render_pass *vk_render_pass_init(struct fx_renderer *fx_renderer,
		struct wlr_render_pass *render_pass, struct wlr_buffer *wlr_buffer,
		struct wlr_output *output);

struct vk_render_setup {
	struct wl_list link; // vk_renderer.render_setups
	VkRenderPass render_pass;
	// Contains all the different per-shader pipelines
	struct vk_pipelines vk_pipelines;
	struct wl_list pipelines; // struct vk_pipeline.link
	struct wl_list tex_pipelines; // struct vk_pipeline_tex.link

};

struct vk_render_setup *vk_render_setup_find_or_create(struct vk_renderer *renderer,
		struct wlr_render_pass *render_pass);
void vk_render_setup_destroy(struct vk_renderer *renderer, struct vk_render_setup *render_setup);

struct vk_renderer {
	struct fx_renderer fx_renderer;

	VkDevice device;
	VkPhysicalDevice physical_device;
	VkQueue queue;

	struct {
		VkShaderModule vert;

		struct vk_shader_info quad;
		struct vk_shader_info quad_grad;
		struct vk_shader_info box_shadow;
		struct vk_shader_info tex;

		struct vk_shader_info blur1;
		struct vk_shader_info blur2;
		struct vk_shader_info blur_effects;

		// Sampling path owned by scenefx. The blur passes sample our own
		// offscreen images rather than wlr_textures, so we cannot reuse
		// wlroots' texture descriptor sets: its layout declares an immutable
		// sampler, and Vulkan only treats layouts as compatible when they are
		// identically defined, immutable samplers included.
		// Dual-Kawase also depends on bilinear taps, hence LINEAR/CLAMP_TO_EDGE.
		VkSampler tex_sampler;
		VkDescriptorSetLayout tex_ds_layout;
		VkDescriptorPool tex_ds_pool;
	} shader_info;

	// Layout for the gradient uniform buffer (descriptor set 0, binding 0).
	// Shared by every gradient descriptor set and pipeline layout.
	VkDescriptorSetLayout gradient_ds_layout;

	struct wl_list render_setups; // struct vk_render_setup.link
	struct wl_list buffers; // vk_buffer.link

	// 1x1 opaque image bound as blur_effects' transparency mask when the caller
	// has no mask of its own. Every draw that reaches the composite therefore
	// samples a defined image, so the shader's mask fetch needs no guard.
	// Immutable, so one copy serves every output.
	struct {
		VkImage image;
		VkDeviceMemory memory;
		VkImageView view;
		bool initialised;
	} dummy_mask;

	// Resources whose destruction has to be deferred until the command buffer
	// that referenced them has actually executed. See vk_deferred_destroy().
	struct wl_list deferred_destroys; // struct vk_deferred_destroy.link
	// Tracy GPU profiling state. Timestamps are written into the command buffer
	// that is currently being recorded, so the renderer tracks it as passes
	// begin and end; VK_NULL_HANDLE means no pass is recording and zones are
	// dropped rather than recorded against a stale buffer.
	VkQueryPool tracy_query_pool;
	VkCommandPool tracy_command_pool;
	VkCommandBuffer tracy_command_buffer;
	// vkResetQueryPool entrypoint, resolved at runtime; NULL when unavailable.
	PFN_vkResetQueryPool tracy_reset_query_pool;

	// Set once the device has been lost. Vulkan drops the device on its own
	// when that happens, and every later call into it fails, so all subsequent
	// work is skipped and the loss is reported to the compositor a single time.
	bool device_lost;
	// Whether that loss has already been reported to the compositor, so it is
	// told once rather than on every subsequent frame.
	bool device_lost_reported;
};

/**
 * Records the command buffer passes are currently recording into, for Tracy
 * GPU timestamp queries. Passing VK_NULL_HANDLE marks the end of a pass.
 */
void vk_renderer_set_tracy_command_buffer(struct vk_renderer *renderer,
		VkCommandBuffer cb);

struct fx_renderer *vk_renderer_create(struct wlr_renderer *wlr_renderer);

struct vk_renderer *vk_get_renderer(struct fx_renderer *fx_renderer);

struct vk_offscreen_buffers;

/**
 * (Re)create the per-output effect targets at the given size.
 *
 * These live on the offscreen buffers rather than the renderer because every
 * size change has to rebuild them, and two outputs of different sizes would
 * otherwise rebuild each other's targets on every pass.
 *
 * `scene_pass` is the wlroots scene render pass the framebuffers are built
 * against; it is remembered on the offscreen buffers, since Vulkan only treats
 * render passes as compatible for pipeline reuse when they match closely (see
 * the comment at the top of this file).
 */
bool vk_effect_images_ensure(struct vk_render_pass *pass, VkRenderPass scene_pass,
		uint32_t width, uint32_t height);
void vk_effect_images_finish(struct vk_offscreen_buffers *offscreen_buffers,
		struct vk_renderer *renderer);
/** Barrier the effect image into COLOR_ATTACHMENT_OPTIMAL ready to be drawn to. */
void vk_effect_image_prepare_target(VkCommandBuffer cb, struct vk_effect_image *img);

/** (Re)create the blur-padding snapshot image at the given size. */
bool vk_saved_pixels_ensure(struct vk_render_pass *pass,
		uint32_t width, uint32_t height);
void vk_saved_pixels_finish(struct vk_offscreen_buffers *offscreen_buffers,
		struct vk_renderer *renderer);

/** (Re)create the optimized-blur cache at the given output size. */
bool vk_optimized_blur_ensure(struct vk_effect_image *cache,
		struct vk_renderer *renderer, VkRenderPass render_pass,
		uint32_t width, uint32_t height);
void vk_optimized_blur_finish(struct vk_effect_image *cache,
		struct vk_renderer *renderer);
VkDescriptorSet vk_get_blend_ds(struct vk_render_pass *pass, VkImageView view);

/**
 * Write `view` into binding 1 (blur_effects' transparency mask) of `ds`.
 *
 * Passing VK_NULL_HANDLE binds the renderer's 1x1 opaque dummy instead, so the
 * shader's fetch is always defined. The layout's bindings use immutable
 * samplers, so only the view and layout are written.
 */
bool vk_tex_ds_set_mask(struct vk_renderer *renderer, VkDescriptorSet ds,
	VkImageView view, VkImageLayout layout);

/**
 * The renderer's 1x1 opaque mask stand-in, created on first use.
 * Returns VK_NULL_HANDLE if it could not be created.
 */
bool vk_dummy_mask_ensure(struct vk_renderer *renderer);
void vk_dummy_mask_finish(struct vk_renderer *renderer);

/**
 * Our own view of `image`, for sampling it as the blur transparency mask.
 *
 * wlr_texture only yields a descriptor set, which is unusable here because its
 * layout declares a different immutable sampler, so the image is wrapped in a
 * view of our own instead. Cached; `layout` is set to the image's current
 * layout, which the caller must pass to vk_tex_ds_set_mask().
 *
 * Returns VK_NULL_HANDLE on failure.
 */
VkImageView vk_mask_view_get(struct vk_offscreen_buffers *offscreen_buffers,
		struct vk_renderer *renderer, VkCommandBuffer cb, VkImage image,
		VkFormat format, VkImageLayout *layout);
void vk_mask_view_finish(struct vk_offscreen_buffers *offscreen_buffers,
		struct vk_renderer *renderer);

/** Index of a memory type satisfying props, or -1. */
int vk_find_mem_type(struct vk_renderer *renderer,
	VkMemoryPropertyFlags props, uint32_t type_bits);
const char *vulkan_strerror(VkResult err);

/**
 * Whether the renderer's VkDevice has been lost and is unusable.
 *
 * Once this is true every pass fails quietly instead of submitting work that
 * Vulkan would reject anyway.
 */
bool vk_renderer_device_lost(struct vk_renderer *renderer);

/**
 * Whether the device is lost and has not been reported to the compositor yet.
 *
 * Consumes the pending report: only the first call after a loss returns true,
 * so the wlr_renderer.events.lost signal fires exactly once per loss even
 * though every following frame still finds the device gone.
 */
bool vk_renderer_take_device_lost(struct vk_renderer *renderer);

/**
 * Report a Vulkan failure, and mark the device lost if `res` says it is.
 *
 * A lost device is reported to the compositor through
 * wlr_renderer.events.lost exactly once; the renderer keeps no usable state
 * afterwards. Returns true if the device was lost, so callers can bail out.
 *
 * Must not be used once the compositor may have destroyed the renderer, i.e.
 * after wlr_renderer.events.destroy - see the deferred reporting in
 * vk_render_pass_init().
 */
bool vk_renderer_handle_result(struct vk_renderer *renderer,
		const char *what, VkResult res);

/** A destruction deferred until the recording command buffer has executed. */
typedef void (*vk_deferred_destroy_fn)(struct vk_renderer *renderer, void *data);

/** Run the deferred destructions queued against `cb`. */
void vk_deferred_destroy_run_for(struct vk_renderer *renderer, VkCommandBuffer cb);

/**
 * Free `data` through `fn` once no recorded work can still reference it.
 *
 * Vulkan objects may only be destroyed once every submitted or recorded command
 * referencing them has finished. Recording-time teardowns cannot prove that
 * with vkDeviceWaitIdle(): the command buffer being recorded has not been
 * submitted yet, so the wait says nothing about it.
 *
 * With a command buffer, the work is queued on the renderer and run from
 * wlroots' resources callback for that buffer, which fires after it executed
 * (see handle_release_resources in pass.c).
 * With VK_NULL_HANDLE there is nothing recording (renderer teardown), so it runs
 * immediately.
 */
void vk_deferred_destroy(struct vk_renderer *renderer, VkCommandBuffer cb,
		vk_deferred_destroy_fn fn, void *data);

/** Run every queued deferred destruction now; used on renderer teardown. */
void vk_deferred_destroy_flush(struct vk_renderer *renderer);
#if __STDC_VERSION__ >= 202311L

#define wlr_vk_error(fmt, res, ...) wlr_log(WLR_ERROR, fmt ": %s (%d)", \
	vulkan_strerror(res), res __VA_OPT__(,) __VA_ARGS__)

#else

#define wlr_vk_error(fmt, res, ...) wlr_log(WLR_ERROR, fmt ": %s (%d)", \
	vulkan_strerror(res), res, ##__VA_ARGS__)

#endif

#endif
