// Destruction of Vulkan objects that recorded commands may still reference.
//
// Effects record offscreen draws into the scene's own command buffer, so an
// offscreen target can be replaced or torn down while that buffer is still
// pending. vkDeviceWaitIdle() does not help there: the buffer being recorded has
// not been submitted yet, so the wait says nothing about it.
//
// wlroots offers the one hook that does help - the render pass' resources
// callback, invoked when the command buffer is recycled (long after the pass
// itself is gone). Only one callback is allowed per command buffer and the
// gradient arenas already own it, so deferred work is queued on the renderer and
// drained from that single callback.

#include <assert.h>
#include <stdlib.h>
#include <vulkan/vulkan_core.h>
#include <wlr/util/log.h>

#include "render/vulkan/vulkan.h"

struct vk_deferred_destroy {
	struct wl_list link; // vk_renderer.deferred_destroys
	VkCommandBuffer command_buffer;
	vk_deferred_destroy_fn fn;
	void *data;
};

void vk_deferred_destroy(struct vk_renderer *renderer, VkCommandBuffer cb,
		vk_deferred_destroy_fn fn, void *data) {
	assert(fn != NULL);

	// Nothing is recording (renderer teardown), so no pending work can reference
	// the object and it can go right away.
	if (cb == VK_NULL_HANDLE) {
		fn(renderer, data);
		return;
	}

	struct vk_deferred_destroy *destroy = calloc(1, sizeof(*destroy));
	if (destroy == NULL) {
		wlr_log_errno(WLR_ERROR, "Failed to defer a Vulkan destruction");
		// Leaking beats freeing something that may still be in flight.
		return;
	}
	destroy->command_buffer = cb;
	destroy->fn = fn;
	destroy->data = data;
	wl_list_insert(&renderer->deferred_destroys, &destroy->link);
}

void vk_deferred_destroy_run_for(struct vk_renderer *renderer, VkCommandBuffer cb) {
	struct vk_deferred_destroy *destroy, *tmp;
	wl_list_for_each_safe(destroy, tmp, &renderer->deferred_destroys, link) {
		if (destroy->command_buffer != cb) {
			continue;
		}
		wl_list_remove(&destroy->link);
		destroy->fn(renderer, destroy->data);
		free(destroy);
	}
}

void vk_deferred_destroy_flush(struct vk_renderer *renderer) {
	// The device is idle at this point, so everything queued can go, whether or
	// not its command buffer has been seen by the callback.
	struct vk_deferred_destroy *destroy, *tmp;
	wl_list_for_each_safe(destroy, tmp, &renderer->deferred_destroys, link) {
		wl_list_remove(&destroy->link);
		destroy->fn(renderer, destroy->data);
		free(destroy);
	}
}
