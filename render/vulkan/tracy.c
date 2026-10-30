#ifdef TRACY_ENABLE
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tracy/TracyC.h>
#include <wayland-util.h>
#include <wlr/render/vulkan.h>
#include <wlr/util/log.h>

#include "render/fx_renderer.h"
#include "render/tracy.h"
#include "render/vulkan/vulkan.h"

// Docs used in the implementation:
// - https://github.com/wolfpld/tracy/releases/latest/download/tracy.pdf
// - https://github.com/wolfpld/tracy/blob/master/public/tracy/TracyVulkan.hpp
// - https://docs.vulkan.org/spec/latest/chapters/queries.html

// `enum class GpuContextType` in TracyQueue.hpp: Invalid, OpenGl, Vulkan, ...
#define TRACY_GPU_CONTEXT_TYPE_VULKAN 2

// Unlike the GLES2 backend, which owns its own GL context, this backend records
// into a command buffer owned by wlroots. Timestamps must be written from
// inside the pass that is currently recording, so the renderer tracks that
// command buffer; see vk_renderer_set_tracy_command_buffer().
//
// Queries must be reset between uses. vkCmdResetQueryPool cannot be used here:
// the spec requires it to be called outside of a render pass instance
// (VUID-vkCmdResetQueryPool-renderpass), and every scenefx draw happens inside
// one. The host-side vkResetQueryPool has no such restriction, so each ring
// slot is reset on the host immediately before it is written. That is safe
// because a slot is not reused until its result has been collected, and
// collecting implies the submission that wrote it has executed
// (VUID-vkResetQueryPool-firstQuery-02741). It requires the hostQueryReset
// feature, which wlroots enables when the device supports it.
//
// A zone that is skipped (ring full, or no pass recording) leaves
// ctx->is_active false so the matching zone_end is skipped too, keeping
// Tracy's zone stack balanced.

static atomic_int id_counter = 0;

// `base` is the first member, so the downcast is a plain cast.
struct vk_tracy_data {
	struct tracy_data base;

	// Set once the device is known to support what this backend needs.
	bool usable;
};

static struct vk_tracy_data *vk_tracy_get(struct tracy_data *tracy_data) {
	return (struct vk_tracy_data *)tracy_data;
}

// vkResetQueryPool is core Vulkan 1.2, and wlroots creates its instance with
// VK_API_VERSION_1_1, so the loader leaves that entrypoint NULL. The device
// enables VK_EXT_host_query_reset instead, whose vkResetQueryPoolEXT is
// equivalent. Resolve whichever the device exposes.
static PFN_vkResetQueryPool vk_tracy_find_reset_query_pool(struct vk_renderer *renderer) {
	PFN_vkResetQueryPool reset = (PFN_vkResetQueryPool)
		vkGetDeviceProcAddr(renderer->device, "vkResetQueryPool");
	if (reset == NULL) {
		reset = (PFN_vkResetQueryPool)
			vkGetDeviceProcAddr(renderer->device, "vkResetQueryPoolEXT");
	}
	return reset;
}

static bool vk_tracy_ring_full(struct tracy_data *tracy_data) {
	return (tracy_data->queue.head + 1) % QUERY_QUEUE_LEN == tracy_data->queue.tail;
}

static void vk_tracy_write_timestamp(struct vk_renderer *renderer,
		struct tracy_data *tracy_data, unsigned int query) {
	VkCommandBuffer cb = renderer->tracy_command_buffer;
	assert(cb != VK_NULL_HANDLE);

	renderer->tracy_reset_query_pool(renderer->device, renderer->tracy_query_pool,
			query, 1);
	vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			renderer->tracy_query_pool, query);
}

static void vk_tracy_gpu_zone_begin(struct fx_renderer *fx_renderer,
		struct tracy_data *tracy_data, struct tracy_gpu_zone_context *out_ctx,
		const int line, const char *source, const char *func, const char *name) {
	struct vk_renderer *renderer = vk_get_renderer(fx_renderer);
	struct vk_tracy_data *vk_tracy = vk_tracy_get(tracy_data);

	out_ctx->tracy_data = tracy_data;
#ifdef TRACY_ON_DEMAND
	out_ctx->is_active = TracyCIsConnected;
#else
	out_ctx->is_active = true;
#endif

	// A zone recorded outside a command buffer would have nowhere to write its
	// timestamp, so drop it rather than record against a stale buffer.
	if (!out_ctx->is_active || !vk_tracy->usable
			|| renderer->tracy_command_buffer == VK_NULL_HANDLE) {
		out_ctx->is_active = false;
		return;
	}

	// Drop the zone rather than wait for the collector to free a slot.
	if (vk_tracy_ring_full(tracy_data)) {
		out_ctx->is_active = false;
		return;
	}

	const unsigned int query = tracy_get_next_query_index(tracy_data);
	vk_tracy_write_timestamp(renderer, tracy_data, query);

	const uint64_t srcloc = ___tracy_alloc_srcloc_name(line,
			source, strlen(source),
			func, strlen(func),
			name, strlen(name),
			0);

	const struct ___tracy_gpu_zone_begin_data data = {
		.context = tracy_data->context_id,
		.queryId = (uint16_t) query,
		.srcloc = srcloc,
	};
	___tracy_emit_gpu_zone_begin_alloc(data);
}

static void vk_tracy_gpu_zone_end(struct fx_renderer *fx_renderer,
		struct tracy_gpu_zone_context *ctx) {
	struct vk_renderer *renderer = vk_get_renderer(fx_renderer);
	struct vk_tracy_data *vk_tracy = vk_tracy_get(ctx->tracy_data);

	if (!vk_tracy->usable || renderer->tracy_command_buffer == VK_NULL_HANDLE
			|| vk_tracy_ring_full(ctx->tracy_data)) {
		return;
	}

	const unsigned int query = tracy_get_next_query_index(ctx->tracy_data);
	vk_tracy_write_timestamp(renderer, ctx->tracy_data, query);

	const struct ___tracy_gpu_zone_end_data data = {
		.context = ctx->tracy_data->context_id,
		.queryId = (uint16_t) query,
	};
	___tracy_emit_gpu_zone_end(data);
}

static void vk_tracy_gpu_context_collect(struct fx_renderer *fx_renderer,
		struct tracy_data *tracy_data) {
	struct vk_renderer *renderer = vk_get_renderer(fx_renderer);
	struct vk_tracy_data *vk_tracy = vk_tracy_get(tracy_data);

	if (!vk_tracy->usable) {
		return;
	}

#ifdef TRACY_ON_DEMAND
	if (!TracyCIsConnected) {
		tracy_data->queue.head = 0;
		tracy_data->queue.tail = 0;
		return;
	}
#endif

	while (tracy_data->queue.tail != tracy_data->queue.head) {
		const unsigned int index = tracy_data->queue.tail;
		int64_t result[2] = {0};

		// VK_QUERY_RESULT_WITH_AVAILABILITY_BIT never blocks: it returns
		// (value, availability) pairs. A not-yet-available query leaves itself
		// and everything after it in the ring for a later frame.
		VkResult res = vkGetQueryPoolResults(renderer->device,
				renderer->tracy_query_pool, index, 1,
				sizeof(result), result, sizeof(result),
				VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
		if (res != VK_SUCCESS || result[1] == 0) {
			return;
		}

		const struct ___tracy_gpu_time_data data = {
			.context = tracy_data->context_id,
			.gpuTime = result[0],
			.queryId = (uint16_t) index,
		};
		___tracy_emit_gpu_time(data);

		tracy_data->queue.tail = (tracy_data->queue.tail + 1) % QUERY_QUEUE_LEN;
	}
}

static void vk_tracy_gpu_context_destroy(struct fx_renderer *fx_renderer,
		struct tracy_data *tracy_data) {
	struct vk_renderer *renderer = vk_get_renderer(fx_renderer);

	// Every pass must be submitted (and thus every timestamp recorded) before
	// these go away.
	renderer->tracy_command_buffer = VK_NULL_HANDLE;

	if (renderer->tracy_query_pool != VK_NULL_HANDLE) {
		vkDestroyQueryPool(renderer->device, renderer->tracy_query_pool, NULL);
		renderer->tracy_query_pool = VK_NULL_HANDLE;
	}

	if (renderer->tracy_command_pool != VK_NULL_HANDLE) {
		vkDestroyCommandPool(renderer->device, renderer->tracy_command_pool, NULL);
		renderer->tracy_command_pool = VK_NULL_HANDLE;
	}
}

// GPU timestamps are only meaningful relative to another timestamp from the same
// queue, and there is no way to ask the device for "the current time", so the
// Tracy GPU context starts from a timestamp recorded through a throwaway
// command buffer.
static bool vk_tracy_read_initial_timestamp(struct vk_renderer *renderer,
		int64_t *out) {
	VkCommandBufferAllocateInfo alloc_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = renderer->tracy_command_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer cb = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(renderer->device, &alloc_info, &cb) != VK_SUCCESS) {
		return false;
	}

	VkCommandBufferBeginInfo begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vkBeginCommandBuffer(cb, &begin_info);
	renderer->tracy_reset_query_pool(renderer->device, renderer->tracy_query_pool, 0, 1);
	vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			renderer->tracy_query_pool, 0);
	vkEndCommandBuffer(cb);

	VkSubmitInfo submit_info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &cb,
	};

	bool ok = false;
	if (vkQueueSubmit(renderer->queue, 1, &submit_info, VK_NULL_HANDLE) == VK_SUCCESS
			&& vkQueueWaitIdle(renderer->queue) == VK_SUCCESS) {
		int64_t result[2] = {0};
		if (vkGetQueryPoolResults(renderer->device, renderer->tracy_query_pool,
				0, 1, sizeof(result), result, sizeof(result),
				VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_SUCCESS
				&& result[1] != 0) {
			*out = result[0];
			ok = true;
		}
	}

	vkFreeCommandBuffers(renderer->device, renderer->tracy_command_pool, 1, &cb);
	return ok;
}

static struct tracy_data *vk_tracy_gpu_context_new(struct fx_renderer *fx_renderer) {
	struct vk_renderer *renderer = vk_get_renderer(fx_renderer);

	struct vk_tracy_data *vk_tracy = calloc(1, sizeof(*vk_tracy));
	if (vk_tracy == NULL) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return NULL;
	}
	struct tracy_data *tracy_data = &vk_tracy->base;

	tracy_data->fx_renderer = fx_renderer;
	tracy_data->context_id = ++id_counter;
	tracy_data->queue.head = 0;
	tracy_data->queue.tail = 0;

	// Without the host reset every reused ring slot would report the value from
	// its previous use, so a zone would report a stale duration instead of
	// nothing. Better to report nothing.
	renderer->tracy_reset_query_pool = vk_tracy_find_reset_query_pool(renderer);

	if (renderer->tracy_reset_query_pool == NULL) {
		wlr_log(WLR_ERROR, "Vulkan device does not support host query reset, "
				"GPU profiling disabled");
		return tracy_data;
	}

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(renderer->physical_device, &props);
	if (props.limits.timestampPeriod == 0.0f) {
		wlr_log(WLR_ERROR, "Device does not support timestamp queries, "
				"GPU profiling disabled");
		return tracy_data;
	}

	// Timestamps written on a queue family whose timestampValidBits is 0 are
	// undefined, so make sure the graphics queue can actually write them.
	const uint32_t queue_family =
		wlr_vk_renderer_get_queue_family(fx_renderer->wlr_renderer);
	uint32_t count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(renderer->physical_device, &count, NULL);
	VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
	if (families == NULL) {
		return tracy_data;
	}
	vkGetPhysicalDeviceQueueFamilyProperties(renderer->physical_device, &count, families);
	const bool can_write =
		queue_family < count && families[queue_family].timestampValidBits > 0;
	free(families);
	if (!can_write) {
		wlr_log(WLR_ERROR, "Graphics queue cannot write timestamps, "
				"GPU profiling disabled");
		return tracy_data;
	}

	VkQueryPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		.queryType = VK_QUERY_TYPE_TIMESTAMP,
		.queryCount = QUERY_QUEUE_LEN,
	};
	VkResult res = vkCreateQueryPool(renderer->device, &pool_info, NULL,
			&renderer->tracy_query_pool);
	if (res != VK_SUCCESS) {
		wlr_log(WLR_ERROR, "Failed to create the Tracy query pool, "
				"GPU profiling disabled");
		renderer->tracy_query_pool = VK_NULL_HANDLE;
		return tracy_data;
	}

	VkCommandPoolCreateInfo cb_pool_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = queue_family,
	};
	res = vkCreateCommandPool(renderer->device, &cb_pool_info, NULL,
			&renderer->tracy_command_pool);
	if (res != VK_SUCCESS) {
		wlr_log(WLR_ERROR, "Failed to create the Tracy command pool, "
				"GPU profiling disabled");
		renderer->tracy_command_pool = VK_NULL_HANDLE;
		vkDestroyQueryPool(renderer->device, renderer->tracy_query_pool, NULL);
		renderer->tracy_query_pool = VK_NULL_HANDLE;
		return tracy_data;
	}

	int64_t initial_gpu_time = 0;
	if (!vk_tracy_read_initial_timestamp(renderer, &initial_gpu_time)) {
		wlr_log(WLR_ERROR, "Failed to read the initial GPU timestamp, "
				"GPU profiling disabled");
		vkDestroyCommandPool(renderer->device, renderer->tracy_command_pool, NULL);
		renderer->tracy_command_pool = VK_NULL_HANDLE;
		vkDestroyQueryPool(renderer->device, renderer->tracy_query_pool, NULL);
		renderer->tracy_query_pool = VK_NULL_HANDLE;
		return tracy_data;
	}

	vk_tracy->usable = true;

	// `period` converts raw timestamp ticks to nanoseconds.
	const struct ___tracy_gpu_new_context_data context_data = {
		.context = tracy_data->context_id,
		.gpuTime = initial_gpu_time,
		.period = props.limits.timestampPeriod,
		.flags = 0,
		.type = TRACY_GPU_CONTEXT_TYPE_VULKAN,
	};
	___tracy_emit_gpu_new_context(context_data);

	char device_name[sizeof("FX Renderer (Vulkan): ") + sizeof(props.deviceName)];
	snprintf(device_name, sizeof(device_name), "FX Renderer (Vulkan): %s",
			props.deviceName);

	const struct ___tracy_gpu_context_name_data name_data = {
		.context = tracy_data->context_id,
		.name = device_name,
		.len = strlen(device_name),
	};
	___tracy_emit_gpu_context_name(name_data);

	return tracy_data;
}

const struct fx_renderer_tracy_impl vk_renderer_tracy_impl = {
	.gpu_zone_begin = vk_tracy_gpu_zone_begin,
	.gpu_zone_end = vk_tracy_gpu_zone_end,
	.gpu_context_collect = vk_tracy_gpu_context_collect,
	.gpu_context_destroy = vk_tracy_gpu_context_destroy,
	.gpu_context_new = vk_tracy_gpu_context_new,
};

#endif
