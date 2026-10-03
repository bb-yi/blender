/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Nuru: Vulkan hardware ray-tracing backend (NVIDIA RTX, VK_KHR_acceleration_structure +
 * VK_KHR_ray_query in compute). Port of the Metal implementation in
 * `metal/mtl_nuru_raytrace_acceleration.mm`; the file structure intentionally mirrors the Metal
 * file so the two backends stay diffable.
 */

#include "vk_nuru_raytrace_acceleration.hh"

#include "GPU_batch.hh"
#include "GPU_capabilities.hh"
#include "GPU_index_buffer.hh"
#include "GPU_state.hh"
#include "GPU_vertex_buffer.hh"
#include "GPU_vertex_format.hh"

#include "vk_backend.hh"
#include "vk_buffer.hh"
#include "vk_context.hh"
#include "vk_device.hh"
#include "vk_index_buffer.hh"
#include "vk_nuru_raytrace_kernels.hh"
#ifdef WITH_NURU_OPTIX_DENOISER
#  include "vk_nuru_optix_denoise.hh"
#endif
#include "vk_storage_buffer.hh"
#include "vk_texture.hh"
#include "vk_vertex_buffer.hh"

#include "BKE_appdir.hh"

#include "BLI_fileops.hh"
#include "BLI_hash.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_path_utils.hh"
#include "BLI_time.h"

#include "shaderc/shaderc.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <vector>

#ifndef _WIN32
#  include <unistd.h>
#endif

#ifdef WITH_OPENIMAGEDENOISE
#  include <OpenImageDenoise/oidn.h>
#  if OIDN_VERSION_MAJOR < 2
#    define oidnExecuteFilterAsync oidnExecuteFilter
#  endif
#endif

namespace blender {

namespace nuru_vk {

/* Plain VMA-backed buffer owned by the ray-tracing backend. Kept separate from VKBuffer so scene
 * lifetime is independent from context/render-graph state. */
struct RTBuffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  /* Raw exportable allocation (OIDN interop). Mutually exclusive with `allocation`. */
  VkDeviceMemory external_memory = VK_NULL_HANDLE;
#ifdef _WIN32
  /* Exported NT handle for `external_memory` (OIDN zero-copy import). Kept open for the
   * lifetime of the import; closed in `rt_buffer_free`. */
  void *external_handle = nullptr;
#endif
  VkDeviceAddress device_address = 0;
  void *mapped = nullptr;
  size_t size = 0;
};

}  // namespace nuru_vk

struct GPUHardwareRaytraceScene {
  VkAccelerationStructureKHR top_level_acceleration_structure = VK_NULL_HANDLE;
  nuru_vk::RTBuffer top_level_buffer;
  nuru_vk::RTBuffer emissive_radiance_buffer;
  nuru_vk::RTBuffer emissive_light_buffer;
  nuru_vk::RTBuffer diffuse_albedo_buffer;
  nuru_vk::RTBuffer material_proxy_buffer;
  nuru_vk::RTBuffer triangle_normal_buffer;
  nuru_vk::RTBuffer triangle_smooth_normal_buffer;
  nuru_vk::RTBuffer triangle_local_position_buffer;
  nuru_vk::RTBuffer triangle_normal_range_buffer;
  std::vector<VkAccelerationStructureKHR> bottom_level_acceleration_structures;
  std::vector<nuru_vk::RTBuffer> bottom_level_buffers;
  /* RT-owned copies of the geometry (positions as tightly packed float3, indices as uint32).
   * Unlike Metal, the backend does not reference Blender's VBO/IBO at trace time; geometry is
   * re-uploaded into buffers created with acceleration-structure usage flags. */
  std::vector<nuru_vk::RTBuffer> geometry_buffers;
  std::vector<std::vector<float4>> local_triangle_normals;
  std::vector<std::vector<float4>> local_triangle_smooth_normals;
  std::vector<std::vector<float4>> local_triangle_positions;
  int geometry_count = 0;
  int instance_count = 0;
  int emissive_light_count = 0;
  /* Shadow batch: accumulate dispatches into a single command buffer between
   * `shadow_batch_begin` and `shadow_batch_end`. */
  struct NuruVKSubmission *shadow_batch_submission = nullptr;
  bool shadow_batch_has_work = false;

  ~GPUHardwareRaytraceScene();
};

}  // namespace blender

namespace blender::gpu::vulkan {

using nuru_vk::RTBuffer;

/* -------------------------------------------------------------------- */
/** \name Small utilities
 * \{ */

static int gpu_vulkan_shadow_transparency_bits(const float value)
{
  int bits = 0;
  const float clamped = std::clamp(value, 0.0f, 1.0f);
  memcpy(&bits, &clamped, sizeof(bits));
  return bits;
}

static bool env_flag_enabled(const char *name)
{
  const char *value = std::getenv(name);
  return (value != nullptr) && (value[0] != '\0') && !(value[0] == '0' && value[1] == '\0');
}

static bool vulkan_raytrace_perf_logging_enabled()
{
  return env_flag_enabled("BLENDER_EEVEE_HWRT_PERF");
}

static bool vulkan_raytrace_force_sync()
{
  /* EMERALD root-cause note: the Windows WDDM async device-lost (~50% of headless HWRT
   * renders, VK_ERROR_DEVICE_LOST on the final timeline wait, white frame) was triggered by
   * the missing `hit_barycentric_tx`/`layered_receiver_barycentric_tx`/HiZ texture binds
   * fixed in EMERALD 3: the per-dispatch missing-bind dummy fallback churn was the
   * cross-submission hazard. With the binds wired, async passed 45/45 mixed soak runs
   * (stock and renamed exe, RTX 5090 / driver 610.47), so Windows runs the same async
   * timeline-chained default as Linux again. The earlier symptom bisects (force-sync clean,
   * exe-name sensitivity) were timing artifacts of the same bug. */
  return env_flag_enabled("BLENDER_EEVEE_HWRT_FORCE_SYNC");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name RTBuffer helpers
 * \{ */

static bool rt_buffer_create(RTBuffer &r_buffer,
                             const size_t size,
                             const VkBufferUsageFlags usage,
                             const bool host_visible,
                             const bool host_cached = false)
{
  VKDevice &device = VKBackend::get().device;
  const size_t alloc_size = std::max<size_t>(size, 16);

  VkBufferCreateInfo create_info = {};
  create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  create_info.size = alloc_size;
  create_info.usage = usage;
  create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VmaAllocationCreateInfo vma_create_info = {};
  vma_create_info.usage = VMA_MEMORY_USAGE_AUTO;
  vma_create_info.priority = 0.5f;
  if (host_visible) {
    /* RANDOM requests host-cached memory: required when the CPU (or `cuMemcpy`) reads the
     * mapped pointer back, since reads from write-combined memory run at uncached speed. */
    vma_create_info.flags = (host_cached ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT :
                                           VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT) |
                            VMA_ALLOCATION_CREATE_MAPPED_BIT;
  }

  VmaAllocationInfo allocation_info = {};
  const VkResult result = vmaCreateBuffer(device.mem_allocator_get(),
                                          &create_info,
                                          &vma_create_info,
                                          &r_buffer.buffer,
                                          &r_buffer.allocation,
                                          &allocation_info);
  if (result != VK_SUCCESS) {
    r_buffer = {};
    return false;
  }
  r_buffer.size = alloc_size;
  r_buffer.mapped = host_visible ? allocation_info.pMappedData : nullptr;
  if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
    VkBufferDeviceAddressInfo address_info = {};
    address_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    address_info.buffer = r_buffer.buffer;
    r_buffer.device_address = vkGetBufferDeviceAddress(device.vk_handle(), &address_info);
  }
  return true;
}

static void rt_buffer_free(RTBuffer &buffer)
{
  if (buffer.buffer == VK_NULL_HANDLE) {
    return;
  }
  VKDevice &device = VKBackend::get().device;
  if (buffer.external_memory != VK_NULL_HANDLE) {
#ifdef _WIN32
    if (buffer.external_handle != nullptr) {
      CloseHandle(buffer.external_handle);
    }
#endif
    vkDestroyBuffer(device.vk_handle(), buffer.buffer, nullptr);
    vkFreeMemory(device.vk_handle(), buffer.external_memory, nullptr);
  }
  else {
    vmaDestroyBuffer(device.mem_allocator_get(), buffer.buffer, buffer.allocation);
  }
  buffer = {};
}

/* Platform handle type for exportable allocations (OIDN zero-copy interop). */
#ifdef _WIN32
static constexpr VkExternalMemoryHandleTypeFlagBits rt_external_handle_type =
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
static constexpr VkExternalMemoryHandleTypeFlagBits rt_external_handle_type =
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

/* Device-local buffer backed by a dedicated exportable allocation (`OPAQUE_FD` on POSIX,
 * `OPAQUE_WIN32` on Windows), so OIDN's GPU device (CUDA on NVIDIA) can import it and denoise
 * in place without PCIe round-trips. */
static bool rt_external_buffer_create(RTBuffer &r_buffer,
                                      const size_t size,
                                      const VkBufferUsageFlags usage)
{
  VKDevice &device = VKBackend::get().device;
  const size_t alloc_size = std::max<size_t>(size, 16);

  VkExternalMemoryBufferCreateInfo external_create_info = {};
  external_create_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
  external_create_info.handleTypes = rt_external_handle_type;

  VkBufferCreateInfo create_info = {};
  create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  create_info.pNext = &external_create_info;
  create_info.size = alloc_size;
  create_info.usage = usage;
  create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkBuffer vk_buffer = VK_NULL_HANDLE;
  if (vkCreateBuffer(device.vk_handle(), &create_info, nullptr, &vk_buffer) != VK_SUCCESS) {
    return false;
  }

  VkMemoryRequirements requirements = {};
  vkGetBufferMemoryRequirements(device.vk_handle(), vk_buffer, &requirements);

  VkPhysicalDeviceMemoryProperties memory_properties = {};
  vkGetPhysicalDeviceMemoryProperties(device.physical_device_get(), &memory_properties);
  uint32_t memory_type_index = UINT32_MAX;
  for (uint32_t i : IndexRange(memory_properties.memoryTypeCount)) {
    if ((requirements.memoryTypeBits & (1u << i)) &&
        (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
    {
      memory_type_index = i;
      break;
    }
  }
  if (memory_type_index == UINT32_MAX) {
    vkDestroyBuffer(device.vk_handle(), vk_buffer, nullptr);
    return false;
  }

  VkExportMemoryAllocateInfo export_info = {};
  export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
  export_info.handleTypes = rt_external_handle_type;

  VkMemoryDedicatedAllocateInfo dedicated_info = {};
  dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicated_info.pNext = &export_info;
  dedicated_info.buffer = vk_buffer;

  VkMemoryAllocateInfo allocate_info = {};
  allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocate_info.pNext = &dedicated_info;
  allocate_info.allocationSize = requirements.size;
  allocate_info.memoryTypeIndex = memory_type_index;

  VkDeviceMemory vk_memory = VK_NULL_HANDLE;
  if (vkAllocateMemory(device.vk_handle(), &allocate_info, nullptr, &vk_memory) != VK_SUCCESS) {
    vkDestroyBuffer(device.vk_handle(), vk_buffer, nullptr);
    return false;
  }
  if (vkBindBufferMemory(device.vk_handle(), vk_buffer, vk_memory, 0) != VK_SUCCESS) {
    vkDestroyBuffer(device.vk_handle(), vk_buffer, nullptr);
    vkFreeMemory(device.vk_handle(), vk_memory, nullptr);
    return false;
  }

  r_buffer = {};
  r_buffer.buffer = vk_buffer;
  r_buffer.external_memory = vk_memory;
  r_buffer.size = requirements.size;
  return true;
}

#ifdef _WIN32
/* Returns a fresh NT handle referencing the buffer's memory, or null. The handle stays owned by
 * the caller (importing APIs reference the allocation internally); close it with `CloseHandle`. */
static void *rt_external_buffer_export_win32(const RTBuffer &buffer)
{
  VKDevice &device = VKBackend::get().device;
  if (buffer.external_memory == VK_NULL_HANDLE ||
      device.functions.vkGetMemoryWin32Handle == nullptr)
  {
    return nullptr;
  }
  VkMemoryGetWin32HandleInfoKHR get_handle_info = {};
  get_handle_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
  get_handle_info.memory = buffer.external_memory;
  get_handle_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
  HANDLE handle = nullptr;
  if (device.functions.vkGetMemoryWin32Handle(device.vk_handle(), &get_handle_info, &handle) !=
      VK_SUCCESS)
  {
    return nullptr;
  }
  return handle;
}
#else
/* Returns a fresh POSIX fd referencing the buffer's memory, or -1. Ownership of the fd moves to
 * the caller (and to the importing API on successful import). */
static int rt_external_buffer_export_fd(const RTBuffer &buffer)
{
  VKDevice &device = VKBackend::get().device;
  if (buffer.external_memory == VK_NULL_HANDLE || device.functions.vkGetMemoryFd == nullptr) {
    return -1;
  }
  VkMemoryGetFdInfoKHR get_fd_info = {};
  get_fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
  get_fd_info.memory = buffer.external_memory;
  get_fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  int fd = -1;
  if (device.functions.vkGetMemoryFd(device.vk_handle(), &get_fd_info, &fd) != VK_SUCCESS) {
    return -1;
  }
  return fd;
}
#endif

/* Host-visible storage buffer for shading data (emissive, albedo, proxies, normals). */
static bool rt_storage_buffer_create(RTBuffer &r_buffer, const size_t size)
{
  return rt_buffer_create(r_buffer,
                          size,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                          /*host_visible=*/true);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Acceleration structure helpers
 * \{ */

struct AccelerationStructureBuildBatch {
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  std::vector<RTBuffer> scratch_buffers;
  std::vector<RTBuffer> transient_buffers;
};

/* Device-lifetime GPU objects owned by this backend. Freed in `raytrace_device_free()` which
 * must run before the VkDevice/VMA allocator are destroyed. */
static VkCommandPool g_raytrace_command_pool = VK_NULL_HANDLE;

static VkCommandPool raytrace_command_pool_get()
{
  VkCommandPool &pool = g_raytrace_command_pool;
  if (pool != VK_NULL_HANDLE) {
    return pool;
  }
  VKDevice &device = VKBackend::get().device;
  VkCommandPoolCreateInfo create_info = {};
  create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  create_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  create_info.queueFamilyIndex = device.queue_family_get();
  if (vkCreateCommandPool(device.vk_handle(), &create_info, nullptr, &pool) != VK_SUCCESS) {
    pool = VK_NULL_HANDLE;
  }
  return pool;
}

static std::mutex &raytrace_mutex_get()
{
  static std::mutex mutex;
  return mutex;
}

static VkCommandBuffer raytrace_command_buffer_alloc()
{
  VkCommandPool pool = raytrace_command_pool_get();
  if (pool == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;
  }
  VKDevice &device = VKBackend::get().device;
  VkCommandBufferAllocateInfo allocate_info = {};
  allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocate_info.commandPool = pool;
  allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate_info.commandBufferCount = 1;
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(device.vk_handle(), &allocate_info, &command_buffer) != VK_SUCCESS)
  {
    return VK_NULL_HANDLE;
  }
  VkCommandBufferBeginInfo begin_info = {};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(command_buffer, &begin_info);
  return command_buffer;
}

static bool raytrace_command_buffer_submit_and_wait(VkCommandBuffer command_buffer)
{
  VKDevice &device = VKBackend::get().device;
  vkEndCommandBuffer(command_buffer);

  VkFenceCreateInfo fence_info = {};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  if (vkCreateFence(device.vk_handle(), &fence_info, nullptr, &fence) != VK_SUCCESS) {
    vkFreeCommandBuffers(device.vk_handle(), raytrace_command_pool_get(), 1, &command_buffer);
    return false;
  }

  /* Chain into the device timeline like `submission_end` does. BLAS/TLAS build batches are
   * long-running, non-preemptible GPU work; unchained they may overlap earlier render-graph
   * submissions still executing on the same queue (Vulkan only orders submission start, not
   * execution) and starve them into an Xid 109 CTX SWITCH TIMEOUT. Wedge #5 reproduced with
   * only the kernel-dispatch path chained; this was the remaining unchained submit. */
  TimelineValue chain_wait_value = 0;
  const TimelineValue chain_signal_value = device.external_timeline_chain_acquire(
      &chain_wait_value);
  /* Submission-order discipline: the value we wait on may be claimed by a render-graph task
   * still sitting in the submission runner's queue. Submitting this batch first would park
   * the queue front-end on a wait whose signal arrives in a LATER submission on the same
   * queue - the queue never reaches it (observed: TLAS build batch wedge on object move,
   * GPU idle, no TDR). Block until everything up to the wait value reached `vkQueueSubmit`. */
  device.wait_for_submission_timeline(chain_wait_value);
  VkSemaphore timeline_semaphore = device.timeline_semaphore_get();
  VkPipelineStageFlags chain_wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkTimelineSemaphoreSubmitInfo timeline_info = {};
  timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
  timeline_info.waitSemaphoreValueCount = 1;
  timeline_info.pWaitSemaphoreValues = &chain_wait_value;
  timeline_info.signalSemaphoreValueCount = 1;
  timeline_info.pSignalSemaphoreValues = &chain_signal_value;

  VkSubmitInfo submit_info = {};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.pNext = &timeline_info;
  submit_info.waitSemaphoreCount = 1;
  submit_info.pWaitSemaphores = &timeline_semaphore;
  submit_info.pWaitDstStageMask = &chain_wait_stage;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &command_buffer;
  submit_info.signalSemaphoreCount = 1;
  submit_info.pSignalSemaphores = &timeline_semaphore;
  {
    std::scoped_lock lock(device.queue_mutex_get());
    if (vkQueueSubmit(device.queue_get(), 1, &submit_info, fence) != VK_SUCCESS) {
      VkSemaphoreSignalInfo signal_info = {};
      signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
      signal_info.semaphore = timeline_semaphore;
      signal_info.value = chain_signal_value;
      vkSignalSemaphore(device.vk_handle(), &signal_info);
      device.external_timeline_chain_submitted(chain_signal_value);
      vkDestroyFence(device.vk_handle(), fence, nullptr);
      vkFreeCommandBuffers(device.vk_handle(), raytrace_command_pool_get(), 1, &command_buffer);
      return false;
    }
  }
  device.external_timeline_chain_submitted(chain_signal_value);
  /* First slice short: a healthy build batch completes in milliseconds, so a 5 s stall is
   * already a wedge; dump diagnostics before the app gets killed as "Not Responding". */
  VkResult wait_result = vkWaitForFences(
      device.vk_handle(), 1, &fence, VK_TRUE, uint64_t(5) * 1000 * 1000 * 1000);
  if (wait_result == VK_TIMEOUT) {
    fprintf(stderr,
            "Vulkan RT build-batch stalled >5s (chain wait=%llu signal=%llu); dumping state\n",
            (unsigned long long)chain_wait_value,
            (unsigned long long)chain_signal_value);
    device.diagnostic_checkpoints_dump();
    wait_result = vkWaitForFences(
        device.vk_handle(), 1, &fence, VK_TRUE, uint64_t(55) * 1000 * 1000 * 1000);
  }
  if (wait_result != VK_SUCCESS) {
    fprintf(stderr, "Vulkan RT build-batch wait failed with status=%d\n", int(wait_result));
    device.diagnostic_checkpoints_dump();
  }
  vkDestroyFence(device.vk_handle(), fence, nullptr);
  vkFreeCommandBuffers(device.vk_handle(), raytrace_command_pool_get(), 1, &command_buffer);
  return wait_result == VK_SUCCESS;
}

static bool begin_acceleration_structure_build_batch(AccelerationStructureBuildBatch &r_batch)
{
  std::scoped_lock lock(raytrace_mutex_get());
  r_batch.command_buffer = raytrace_command_buffer_alloc();
  return r_batch.command_buffer != VK_NULL_HANDLE;
}

static bool commit_acceleration_structure_build_batch(AccelerationStructureBuildBatch &batch)
{
  if (batch.command_buffer == VK_NULL_HANDLE) {
    return false;
  }
  /* Acceleration structure builds inside one command buffer can overlap; serialize against
   * later TLAS builds and trace dispatches. */
  VkMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
  barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                          VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                          VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(batch.command_buffer,
                       VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                       VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       0,
                       1,
                       &barrier,
                       0,
                       nullptr,
                       0,
                       nullptr);
  const bool success = raytrace_command_buffer_submit_and_wait(batch.command_buffer);
  for (RTBuffer &scratch : batch.scratch_buffers) {
    rt_buffer_free(scratch);
  }
  for (RTBuffer &transient : batch.transient_buffers) {
    rt_buffer_free(transient);
  }
  batch = {};
  return success;
}

static uint32_t acceleration_structure_scratch_alignment()
{
  static uint32_t alignment = 0;
  if (alignment != 0) {
    return alignment;
  }
  VKDevice &device = VKBackend::get().device;
  VkPhysicalDeviceAccelerationStructurePropertiesKHR as_properties = {};
  as_properties.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
  VkPhysicalDeviceProperties2 properties = {};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  properties.pNext = &as_properties;
  vkGetPhysicalDeviceProperties2(device.physical_device_get(), &properties);
  alignment = std::max(as_properties.minAccelerationStructureScratchOffsetAlignment, 1u);
  return alignment;
}

static bool rt_scratch_buffer_create(RTBuffer &r_buffer, const size_t size)
{
  VKDevice &device = VKBackend::get().device;
  const uint32_t alignment = acceleration_structure_scratch_alignment();
  const size_t alloc_size = std::max<size_t>(size, 16);

  VkBufferCreateInfo create_info = {};
  create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  create_info.size = alloc_size;
  create_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VmaAllocationCreateInfo vma_create_info = {};
  vma_create_info.usage = VMA_MEMORY_USAGE_AUTO;
  vma_create_info.priority = 0.5f;

  const VkResult result = vmaCreateBufferWithAlignment(device.mem_allocator_get(),
                                                       &create_info,
                                                       &vma_create_info,
                                                       VkDeviceSize(alignment),
                                                       &r_buffer.buffer,
                                                       &r_buffer.allocation,
                                                       nullptr);
  if (result != VK_SUCCESS) {
    r_buffer = {};
    return false;
  }
  r_buffer.size = alloc_size;
  VkBufferDeviceAddressInfo address_info = {};
  address_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  address_info.buffer = r_buffer.buffer;
  r_buffer.device_address = vkGetBufferDeviceAddress(device.vk_handle(), &address_info);
  return true;
}

/* Build a single BLAS/TLAS from a fully prepared geometry description. The acceleration
 * structure storage buffer is owned by `r_buffer`, the handle by `r_acceleration_structure`. */
static bool build_acceleration_structure(
    AccelerationStructureBuildBatch &batch,
    VkAccelerationStructureBuildGeometryInfoKHR &build_info,
    const VkAccelerationStructureBuildRangeInfoKHR &range_info,
    const VkAccelerationStructureTypeKHR type,
    VkAccelerationStructureKHR &r_acceleration_structure,
    RTBuffer &r_buffer)
{
  VKDevice &device = VKBackend::get().device;
  if (device.functions.vkGetAccelerationStructureBuildSizes == nullptr ||
      device.functions.vkCreateAccelerationStructure == nullptr ||
      device.functions.vkCmdBuildAccelerationStructures == nullptr)
  {
    return false;
  }

  const uint32_t primitive_count = range_info.primitiveCount;
  VkAccelerationStructureBuildSizesInfoKHR size_info = {};
  size_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
  device.functions.vkGetAccelerationStructureBuildSizes(
      device.vk_handle(),
      VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
      &build_info,
      &primitive_count,
      &size_info);
  if (size_info.accelerationStructureSize == 0) {
    return false;
  }

  if (!rt_buffer_create(r_buffer,
                        size_info.accelerationStructureSize,
                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                        /*host_visible=*/false))
  {
    return false;
  }

  VkAccelerationStructureCreateInfoKHR create_info = {};
  create_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
  create_info.buffer = r_buffer.buffer;
  create_info.offset = 0;
  create_info.size = size_info.accelerationStructureSize;
  create_info.type = type;
  if (device.functions.vkCreateAccelerationStructure(
          device.vk_handle(), &create_info, nullptr, &r_acceleration_structure) != VK_SUCCESS)
  {
    rt_buffer_free(r_buffer);
    return false;
  }

  RTBuffer scratch;
  if (!rt_scratch_buffer_create(scratch, std::max<size_t>(size_info.buildScratchSize, 16))) {
    device.functions.vkDestroyAccelerationStructure(
        device.vk_handle(), r_acceleration_structure, nullptr);
    r_acceleration_structure = VK_NULL_HANDLE;
    rt_buffer_free(r_buffer);
    return false;
  }

  build_info.dstAccelerationStructure = r_acceleration_structure;
  build_info.scratchData.deviceAddress = scratch.device_address;

  const VkAccelerationStructureBuildRangeInfoKHR *range_ptr = &range_info;
  device.functions.vkCmdBuildAccelerationStructures(
      batch.command_buffer, 1, &build_info, &range_ptr);

  batch.scratch_buffers.push_back(scratch);
  return true;
}

static void destroy_acceleration_structure(VkAccelerationStructureKHR &acceleration_structure,
                                           RTBuffer &buffer)
{
  if (acceleration_structure != VK_NULL_HANDLE) {
    VKDevice &device = VKBackend::get().device;
    if (device.functions.vkDestroyAccelerationStructure != nullptr) {
      device.functions.vkDestroyAccelerationStructure(
          device.vk_handle(), acceleration_structure, nullptr);
    }
    acceleration_structure = VK_NULL_HANDLE;
  }
  rt_buffer_free(buffer);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Geometry extraction (CPU)
 * \{ */

struct SceneGeometryBuild {
  VkAccelerationStructureKHR acceleration_structure = VK_NULL_HANDLE;
  RTBuffer acceleration_buffer;
  RTBuffer vertex_buffer;
  RTBuffer index_buffer;
  uint32_t triangle_count = 0;
  uint32_t vertex_count = 0;
  float4x4 object_to_world = float4x4::identity();
  uint32_t instance_count = 1;
  uint32_t user_id = 0;
  float3 emissive_radiance = float3(0.0f);
  float3 diffuse_albedo = float3(0.8f);
  float3 reflection_color = float3(0.8f);
  float reflection_roughness = 1.0f;
  float3 transmission_color = float3(0.8f);
  float transmission_roughness = 1.0f;
  float reflection_ior = 1.45f;
  float refraction_ior = 1.45f;
  float packed_thickness = 0.0f;
  float alpha = 1.0f;
  float reflection_layer_coverage = 0.0f;
  uint32_t closure_type = 1u;
  uint32_t proxy_flags = 0u;
  std::vector<float4> triangle_normals;
  std::vector<float4> triangle_smooth_normals;
  std::vector<float4> triangle_local_positions;
};

static float scene_emissive_energy_sum(Span<SceneGeometryBuild> geometry)
{
  float energy_sum = 0.0f;
  for (const SceneGeometryBuild &entry : geometry) {
    const float emissive_max = std::max(
        std::max(entry.emissive_radiance.x, entry.emissive_radiance.y), entry.emissive_radiance.z);
    if (emissive_max <= 0.0f) {
      continue;
    }
    energy_sum += emissive_max * float(std::max(entry.instance_count, 1u));
  }
  return energy_sum;
}

struct VertexAttributeInput {
  /* CPU copy of the whole VBO. */
  std::vector<uint8_t> data;
  size_t offset = 0;
  size_t stride = 0;
  uint vertex_count = 0;
  VertAttrType format = VertAttrType::SFLOAT_32_32_32;
};

static bool resolve_attribute_input(Batch *batch, const char *name, VertexAttributeInput &r_input)
{
  for (VertBuf *vert_buf : Span<VertBuf *>(batch->verts, GPU_BATCH_VBO_MAX_LEN)) {
    if (vert_buf == nullptr) {
      continue;
    }

    const int attr_id = GPU_vertformat_attr_id_get(&vert_buf->format, name);
    if (attr_id < 0) {
      continue;
    }

    const GPUVertAttr &attr = vert_buf->format.attrs[attr_id];
    /* Upload so the device-side buffer matches what rasterization uses, then read it back. This
     * mirrors the Metal path that reads the shared-storage MTLBuffer contents. */
    vert_buf->upload();
    const size_t buffer_size = std::max(vert_buf->size_alloc_get(), vert_buf->size_used_get());
    if (buffer_size == 0 || vert_buf->format.stride == 0) {
      continue;
    }
    r_input.data.resize(buffer_size + 16, 0);
    static_cast<VKVertexBuffer *>(vert_buf)->read(r_input.data.data());
    r_input.offset = attr.offset;
    r_input.stride = vert_buf->format.stride;
    r_input.vertex_count = vert_buf->vertex_len;
    r_input.format = attr.type.format;
    return true;
  }
  return false;
}

static float3 read_vertex_position(const VertexAttributeInput &input, const uint vertex_index)
{
  const uint8_t *vertex_ptr = input.data.data() + input.offset +
                              size_t(vertex_index) * input.stride;
  switch (input.format) {
    case VertAttrType::SFLOAT_32_32: {
      const float *co = reinterpret_cast<const float *>(vertex_ptr);
      return float3(co[0], co[1], 0.0f);
    }
    case VertAttrType::SFLOAT_32_32_32: {
      const float *co = reinterpret_cast<const float *>(vertex_ptr);
      return float3(co[0], co[1], co[2]);
    }
    case VertAttrType::SFLOAT_32_32_32_32: {
      const float *co = reinterpret_cast<const float *>(vertex_ptr);
      return float3(co[0], co[1], co[2]);
    }
    default:
      return float3(0.0f);
  }
}

static float snorm10_to_float(const uint32_t value)
{
  int v = int(value & 0x3FFu);
  if ((v & 0x200) != 0) {
    v |= ~0x3FF;
  }
  return std::max(float(v) / 511.0f, -1.0f);
}

static float snorm16_to_float(const int16_t value)
{
  return std::max(float(value) / 32767.0f, -1.0f);
}

static float3 read_vertex_normal(const VertexAttributeInput &input, const uint vertex_index)
{
  const uint8_t *vertex_ptr = input.data.data() + input.offset +
                              size_t(vertex_index) * input.stride;
  switch (input.format) {
    case VertAttrType::SFLOAT_32_32_32: {
      const float *nor = reinterpret_cast<const float *>(vertex_ptr);
      return float3(nor[0], nor[1], nor[2]);
    }
    case VertAttrType::SFLOAT_32_32_32_32: {
      const float *nor = reinterpret_cast<const float *>(vertex_ptr);
      return float3(nor[0], nor[1], nor[2]);
    }
    case VertAttrType::SNORM_10_10_10_2: {
      const uint32_t packed = *reinterpret_cast<const uint32_t *>(vertex_ptr);
      return float3(snorm10_to_float(packed >> 0),
                    snorm10_to_float(packed >> 10),
                    snorm10_to_float(packed >> 20));
    }
    /* Nuru: SNORM_16_16_16_16 is the high-quality loop-normal format Blender's mesh extractor
     * uploads (see `extract_mesh_vbo_lnor.cc`). Without this case the HWRT acceleration build
     * silently zeroed every per-corner smooth normal, and the per-triangle fallback flat normal
     * was used at refraction/reflection hits, producing visibly faceted glass even on smooth-
     * shaded meshes. Honoring this format is what makes HWRT respect Blender's smooth / auto-
     * smooth / flat shading choice end-to-end. */
    case VertAttrType::SNORM_16_16_16_16: {
      const int16_t *nor = reinterpret_cast<const int16_t *>(vertex_ptr);
      return float3(snorm16_to_float(nor[0]), snorm16_to_float(nor[1]), snorm16_to_float(nor[2]));
    }
    case VertAttrType::SNORM_16_16_16_DEPRECATED: {
      const int16_t *nor = reinterpret_cast<const int16_t *>(vertex_ptr);
      return float3(snorm16_to_float(nor[0]), snorm16_to_float(nor[1]), snorm16_to_float(nor[2]));
    }
    case VertAttrType::SNORM_16_16: {
      const int16_t *nor = reinterpret_cast<const int16_t *>(vertex_ptr);
      return float3(snorm16_to_float(nor[0]), snorm16_to_float(nor[1]), 0.0f);
    }
    default:
      return float3(0.0f);
  }
}

static std::vector<float4> build_triangle_normal_data(const VertexAttributeInput &positions,
                                                      const std::vector<uint32_t> &indices,
                                                      const size_t triangle_count)
{
  std::vector<float4> triangle_normals(triangle_count, float4(0.0f));
  if (triangle_count == 0 || positions.data.empty()) {
    return triangle_normals;
  }

  for (size_t tri = 0; tri < triangle_count; tri++) {
    const uint i0 = indices[tri * 3 + 0];
    const uint i1 = indices[tri * 3 + 1];
    const uint i2 = indices[tri * 3 + 2];

    const float3 p0 = read_vertex_position(positions, i0);
    const float3 p1 = read_vertex_position(positions, i1);
    const float3 p2 = read_vertex_position(positions, i2);

    float3 N = math::cross(p1 - p0, p2 - p0);
    const float len_sq = math::length_squared(N);
    if (len_sq > 1.0e-20f) {
      N /= std::sqrt(len_sq);
    }
    else {
      N = float3(0.0f, 0.0f, 1.0f);
    }
    triangle_normals[tri] = float4(N, 0.0f);
  }

  return triangle_normals;
}

static std::vector<float4> build_triangle_smooth_normal_data(
    const VertexAttributeInput &normals,
    const bool has_normals,
    const std::vector<uint32_t> &indices,
    const size_t triangle_count,
    const std::vector<float4> &fallback_normals)
{
  std::vector<float4> smooth_normals(triangle_count * 3, float4(0.0f));
  if (triangle_count == 0) {
    return smooth_normals;
  }

  for (size_t tri = 0; tri < triangle_count; tri++) {
    const uint i0 = indices[tri * 3 + 0];
    const uint i1 = indices[tri * 3 + 1];
    const uint i2 = indices[tri * 3 + 2];

    const float3 fallback = (tri < fallback_normals.size()) ?
                                float3(fallback_normals[tri].x,
                                       fallback_normals[tri].y,
                                       fallback_normals[tri].z) :
                                float3(0.0f, 0.0f, 1.0f);
    float3 corner_normals[3] = {fallback, fallback, fallback};
    if (has_normals) {
      corner_normals[0] = read_vertex_normal(normals, i0);
      corner_normals[1] = read_vertex_normal(normals, i1);
      corner_normals[2] = read_vertex_normal(normals, i2);
    }

    for (int corner = 0; corner < 3; corner++) {
      float3 N = corner_normals[corner];
      const float len_sq = math::length_squared(N);
      if (len_sq > 1.0e-20f) {
        N /= std::sqrt(len_sq);
      }
      else {
        N = fallback;
      }
      smooth_normals[tri * 3 + corner] = float4(N, 0.0f);
    }
  }

  return smooth_normals;
}

static std::vector<float4> build_triangle_local_position_data(
    const VertexAttributeInput &positions,
    const std::vector<uint32_t> &indices,
    const size_t triangle_count)
{
  std::vector<float4> local_positions(triangle_count * 3, float4(0.0f));
  if (triangle_count == 0 || positions.data.empty()) {
    return local_positions;
  }

  for (size_t tri = 0; tri < triangle_count; tri++) {
    const uint i0 = indices[tri * 3 + 0];
    const uint i1 = indices[tri * 3 + 1];
    const uint i2 = indices[tri * 3 + 2];

    local_positions[tri * 3 + 0] = float4(read_vertex_position(positions, i0), 0.0f);
    local_positions[tri * 3 + 1] = float4(read_vertex_position(positions, i1), 0.0f);
    local_positions[tri * 3 + 2] = float4(read_vertex_position(positions, i2), 0.0f);
  }

  return local_positions;
}

/* TEMPORARY DIAGNOSTIC: report why an entry is skipped (BLENDER_EEVEE_HWRT_PERF only).
 * Remove before completion. */
static bool blas_skip(const GPUHardwareRaytraceSceneEntry &entry, const char *reason, int detail)
{
  if (vulkan_raytrace_perf_logging_enabled()) {
    std::fprintf(stderr,
                 "EEVEE HWRT DIAG blas_skip user_id=%u reason=%s detail=%d\n",
                 entry.user_id,
                 reason,
                 detail);
  }
  return false;
}
/* END DIAGNOSTIC */

static bool build_entry_blas(const GPUHardwareRaytraceSceneEntry &entry,
                             SceneGeometryBuild &r_geometry,
                             AccelerationStructureBuildBatch &build_batch)
{
  if (entry.batch == nullptr) {
    return blas_skip(entry, "null_batch", 0);
  }

  Batch *batch = entry.batch;

  VertexAttributeInput positions;
  if (!resolve_attribute_input(batch, "pos", positions)) {
    return blas_skip(entry, "no_pos_attribute", 0);
  }
  switch (positions.format) {
    case VertAttrType::SFLOAT_32_32:
    case VertAttrType::SFLOAT_32_32_32:
    case VertAttrType::SFLOAT_32_32_32_32:
      break;
    default:
      return blas_skip(entry, "pos_format", int(positions.format));
  }

  VertexAttributeInput normals;
  const bool has_normal_input = resolve_attribute_input(batch, "nor", normals);

  const GPUPrimType final_primitive_type = batch->prim_type;
  uint32_t index_base = 0;
  size_t triangle_count = 0;
  std::vector<uint32_t> indices;

  if (batch->elem != nullptr) {
    IndexBuf *index_buf = batch->elem;
    const uint index_count = index_buf->index_len_get();
    if (index_count == 0) {
      return blas_skip(entry, "empty_index_buffer", 0);
    }
    if (final_primitive_type != GPU_PRIM_TRIS || (index_count % 3) != 0) {
      return blas_skip(entry, "indexed_prim_type", int(final_primitive_type));
    }

    index_buf->upload_data();
    /* `read()` returns the full storage of the (subrange-resolved) source buffer, so size the
     * host copy from the allocated device size, not the subrange length. */
    const size_t index_storage_size = static_cast<VKIndexBuffer *>(index_buf)->
                                          allocated_size_get();
    std::vector<uint8_t> raw_indices(index_storage_size + 16, 0);
    static_cast<VKIndexBuffer *>(index_buf)->read(
        reinterpret_cast<uint32_t *>(raw_indices.data()));

    const uint index_start = index_buf->index_start_get();
    index_base = index_buf->index_base_get();
    indices.resize(index_count);
    if (index_buf->is_32bit()) {
      const uint32_t *src = reinterpret_cast<const uint32_t *>(raw_indices.data()) + index_start;
      for (uint i = 0; i < index_count; i++) {
        indices[i] = src[i];
      }
    }
    else {
      const uint16_t *src = reinterpret_cast<const uint16_t *>(raw_indices.data()) + index_start;
      for (uint i = 0; i < index_count; i++) {
        indices[i] = uint32_t(src[i]);
      }
    }
    /* Mirror the Metal path that offsets the vertex buffer by `index_base * stride`. */
    if (index_base != 0) {
      for (uint32_t &index : indices) {
        index += index_base;
      }
    }
    triangle_count = index_count / 3;
  }
  else {
    if (final_primitive_type != GPU_PRIM_TRIS || (positions.vertex_count % 3) != 0) {
      return blas_skip(entry, "nonindexed_prim_type", int(final_primitive_type));
    }
    triangle_count = positions.vertex_count / 3;
    indices.resize(triangle_count * 3);
    for (uint i = 0; i < indices.size(); i++) {
      indices[i] = i;
    }
  }

  if (triangle_count == 0) {
    return blas_skip(entry, "zero_triangles", 0);
  }

  /* Upload tightly packed float3 positions + uint32 indices into RT-owned buffers with
   * acceleration-structure build usage. */
  const uint32_t vertex_count = positions.vertex_count;
  if (vertex_count == 0) {
    return blas_skip(entry, "zero_vertices", 0);
  }
  std::vector<float> packed_positions(size_t(vertex_count) * 3, 0.0f);
  for (uint v = 0; v < vertex_count; v++) {
    const float3 co = read_vertex_position(positions, v);
    packed_positions[size_t(v) * 3 + 0] = co.x;
    packed_positions[size_t(v) * 3 + 1] = co.y;
    packed_positions[size_t(v) * 3 + 2] = co.z;
  }

  const VkBufferUsageFlags geometry_usage =
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

  RTBuffer vertex_buffer;
  if (!rt_buffer_create(
          vertex_buffer, packed_positions.size() * sizeof(float), geometry_usage, true))
  {
    return false;
  }
  std::memcpy(vertex_buffer.mapped, packed_positions.data(), packed_positions.size() * sizeof(float));

  RTBuffer index_buffer;
  if (!rt_buffer_create(index_buffer, indices.size() * sizeof(uint32_t), geometry_usage, true)) {
    rt_buffer_free(vertex_buffer);
    return false;
  }
  std::memcpy(index_buffer.mapped, indices.data(), indices.size() * sizeof(uint32_t));

  VkAccelerationStructureGeometryKHR geometry = {};
  geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
  geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
  geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
  geometry.geometry.triangles.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
  geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
  geometry.geometry.triangles.vertexData.deviceAddress = vertex_buffer.device_address;
  geometry.geometry.triangles.vertexStride = sizeof(float) * 3;
  geometry.geometry.triangles.maxVertex = vertex_count - 1;
  geometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
  geometry.geometry.triangles.indexData.deviceAddress = index_buffer.device_address;

  VkAccelerationStructureBuildGeometryInfoKHR build_info = {};
  build_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  build_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  build_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  build_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build_info.geometryCount = 1;
  build_info.pGeometries = &geometry;

  VkAccelerationStructureBuildRangeInfoKHR range_info = {};
  range_info.primitiveCount = uint32_t(triangle_count);

  VkAccelerationStructureKHR acceleration_structure = VK_NULL_HANDLE;
  RTBuffer acceleration_buffer;
  if (!build_acceleration_structure(build_batch,
                                    build_info,
                                    range_info,
                                    VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                                    acceleration_structure,
                                    acceleration_buffer))
  {
    rt_buffer_free(vertex_buffer);
    rt_buffer_free(index_buffer);
    return false;
  }

  r_geometry.acceleration_structure = acceleration_structure;
  r_geometry.acceleration_buffer = acceleration_buffer;
  r_geometry.vertex_buffer = vertex_buffer;
  r_geometry.index_buffer = index_buffer;
  r_geometry.triangle_count = uint32_t(triangle_count);
  r_geometry.vertex_count = vertex_count;
  r_geometry.object_to_world = entry.object_to_world;
  r_geometry.instance_count = std::max(entry.instance_count, uint32_t(1));
  r_geometry.user_id = entry.user_id;
  r_geometry.emissive_radiance = entry.emissive_radiance;
  r_geometry.diffuse_albedo = entry.diffuse_albedo;
  r_geometry.reflection_color = entry.reflection_color;
  r_geometry.reflection_roughness = entry.reflection_roughness;
  r_geometry.transmission_color = entry.transmission_color;
  r_geometry.transmission_roughness = entry.transmission_roughness;
  r_geometry.reflection_ior = entry.reflection_ior;
  r_geometry.refraction_ior = entry.refraction_ior;
  r_geometry.packed_thickness = entry.packed_thickness;
  r_geometry.alpha = entry.alpha;
  r_geometry.reflection_layer_coverage = entry.reflection_layer_coverage;
  r_geometry.closure_type = entry.closure_type;
  r_geometry.proxy_flags = entry.proxy_flags;
  r_geometry.triangle_normals = build_triangle_normal_data(positions, indices, triangle_count);
  r_geometry.triangle_smooth_normals = build_triangle_smooth_normal_data(
      normals, has_normal_input, indices, triangle_count, r_geometry.triangle_normals);
  r_geometry.triangle_local_positions = build_triangle_local_position_data(
      positions, indices, triangle_count);
  return true;
}

static bool build_top_level_acceleration_structure(
    const std::vector<SceneGeometryBuild> &geometry,
    VkAccelerationStructureKHR &r_tlas,
    RTBuffer &r_tlas_buffer)
{
  if (geometry.empty()) {
    return false;
  }

  size_t instance_count = 0;
  for (const SceneGeometryBuild &entry : geometry) {
    if (entry.acceleration_structure == VK_NULL_HANDLE) {
      /* Placeholder slot for an entry whose BLAS could not be built. */
      continue;
    }
    instance_count += entry.instance_count;
  }
  if (instance_count == 0) {
    return false;
  }

  VKDevice &device = VKBackend::get().device;
  if (device.functions.vkGetAccelerationStructureDeviceAddress == nullptr) {
    return false;
  }

  RTBuffer instance_buffer;
  if (!rt_buffer_create(instance_buffer,
                        instance_count * sizeof(VkAccelerationStructureInstanceKHR),
                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                        /*host_visible=*/true))
  {
    return false;
  }

  auto *instances = static_cast<VkAccelerationStructureInstanceKHR *>(instance_buffer.mapped);
  size_t write_index = 0;
  for (const SceneGeometryBuild &entry : geometry) {
    if (entry.acceleration_structure == VK_NULL_HANDLE) {
      continue;
    }
    VkAccelerationStructureDeviceAddressInfoKHR address_info = {};
    address_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    address_info.accelerationStructure = entry.acceleration_structure;
    const VkDeviceAddress blas_address = device.functions.vkGetAccelerationStructureDeviceAddress(
        device.vk_handle(), &address_info);

    for (uint32_t instance_index = 0; instance_index < entry.instance_count; instance_index++) {
      VkAccelerationStructureInstanceKHR &instance = instances[write_index++];
      std::memset(&instance, 0, sizeof(instance));
      /* Blender matrices are column-major; Vulkan wants the top three rows of the 4x4 transform
       * in row-major 3x4 form. */
      const float *src = entry.object_to_world.base_ptr();
      for (int row = 0; row < 3; row++) {
        for (int column = 0; column < 4; column++) {
          instance.transform.matrix[row][column] = src[column * 4 + row];
        }
      }
      /* Vulkan instance custom index is 24-bit; user ids are Eevee geometry indices and stay far
       * below that limit. Mask defensively to avoid touching the visibility mask bits. */
      instance.instanceCustomIndex = entry.user_id & 0x00FFFFFFu;
      instance.mask = 0xFFu;
      instance.instanceShaderBindingTableRecordOffset = 0;
      instance.flags = VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
      instance.accelerationStructureReference = blas_address;
    }
  }

  VkAccelerationStructureGeometryKHR geometry_info = {};
  geometry_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
  geometry_info.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  geometry_info.geometry.instances.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
  geometry_info.geometry.instances.arrayOfPointers = VK_FALSE;
  geometry_info.geometry.instances.data.deviceAddress = instance_buffer.device_address;

  VkAccelerationStructureBuildGeometryInfoKHR build_info = {};
  build_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  build_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  build_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  build_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build_info.geometryCount = 1;
  build_info.pGeometries = &geometry_info;

  VkAccelerationStructureBuildRangeInfoKHR range_info = {};
  range_info.primitiveCount = uint32_t(instance_count);

  AccelerationStructureBuildBatch batch;
  if (!begin_acceleration_structure_build_batch(batch)) {
    rt_buffer_free(instance_buffer);
    return false;
  }
  batch.transient_buffers.push_back(instance_buffer);

  VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
  RTBuffer tlas_buffer;
  if (!build_acceleration_structure(batch,
                                    build_info,
                                    range_info,
                                    VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                                    tlas,
                                    tlas_buffer))
  {
    commit_acceleration_structure_build_batch(batch);
    return false;
  }
  if (!commit_acceleration_structure_build_batch(batch)) {
    destroy_acceleration_structure(tlas, tlas_buffer);
    return false;
  }

  r_tlas = tlas;
  r_tlas_buffer = tlas_buffer;
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shading data buffers
 * \{ */

struct EmissiveLightRecord {
  float4 center_radius;
};

struct HardwareMaterialProxyRecord {
  float4 reflection_color_roughness;
  float4 transmission_color_roughness;
  float4 ior_closure_type;
  float4 packed_thickness;
};

struct TriangleNormalRangeRecord {
  uint32_t offset;
  uint32_t count;
};

static bool build_emissive_radiance_buffer(const std::vector<SceneGeometryBuild> &geometry,
                                           RTBuffer &r_buffer)
{
  uint32_t max_user_id = 0;
  for (const SceneGeometryBuild &entry : geometry) {
    max_user_id = std::max(max_user_id, entry.user_id);
  }

  const size_t color_count = geometry.empty() ? 1 : size_t(max_user_id) + 1;
  if (!rt_storage_buffer_create(r_buffer, color_count * sizeof(float4))) {
    return false;
  }

  auto *emissive_radiance = static_cast<float4 *>(r_buffer.mapped);
  for (size_t i = 0; i < color_count; i++) {
    emissive_radiance[i] = float4(0.0f);
  }
  for (const SceneGeometryBuild &entry : geometry) {
    emissive_radiance[entry.user_id] = float4(entry.emissive_radiance, 0.0f);
  }
  return true;
}

static float4 compute_world_bounding_sphere(const SceneGeometryBuild &entry)
{
  if (entry.triangle_local_positions.empty()) {
    return float4(entry.object_to_world.location(), 1.0f);
  }

  float3 bounds_min = math::transform_point(entry.object_to_world,
                                            float3(entry.triangle_local_positions[0].x,
                                                   entry.triangle_local_positions[0].y,
                                                   entry.triangle_local_positions[0].z));
  float3 bounds_max = bounds_min;
  for (const float4 &local_position : entry.triangle_local_positions) {
    const float3 world_position = math::transform_point(
        entry.object_to_world, float3(local_position.x, local_position.y, local_position.z));
    bounds_min = math::min(bounds_min, world_position);
    bounds_max = math::max(bounds_max, world_position);
  }

  const float3 center = (bounds_min + bounds_max) * 0.5f;
  float radius_sq = 1.0e-6f;
  for (const float4 &local_position : entry.triangle_local_positions) {
    const float3 world_position = math::transform_point(
        entry.object_to_world, float3(local_position.x, local_position.y, local_position.z));
    radius_sq = std::max(radius_sq, math::distance_squared(center, world_position));
  }
  return float4(center, std::sqrt(radius_sq));
}

static bool build_emissive_light_buffer(const std::vector<SceneGeometryBuild> &geometry,
                                        RTBuffer &r_buffer,
                                        int &r_light_count)
{
  std::vector<EmissiveLightRecord> emissive_lights;
  emissive_lights.reserve(geometry.size());
  for (const SceneGeometryBuild &entry : geometry) {
    const float emissive_peak = std::max(
        entry.emissive_radiance.x, std::max(entry.emissive_radiance.y, entry.emissive_radiance.z));
    if (!(emissive_peak > 0.0f)) {
      continue;
    }
    emissive_lights.push_back({compute_world_bounding_sphere(entry)});
  }

  r_light_count = int(emissive_lights.size());
  const size_t light_count = emissive_lights.empty() ? 1 : emissive_lights.size();
  if (!rt_storage_buffer_create(r_buffer, light_count * sizeof(EmissiveLightRecord))) {
    return false;
  }

  auto *lights = static_cast<EmissiveLightRecord *>(r_buffer.mapped);
  lights[0].center_radius = float4(0.0f, 0.0f, 0.0f, 1.0f);
  for (size_t i = 0; i < emissive_lights.size(); i++) {
    lights[i] = emissive_lights[i];
  }
  return true;
}

static bool build_diffuse_albedo_buffer(const std::vector<SceneGeometryBuild> &geometry,
                                        RTBuffer &r_buffer)
{
  /* Indirect diffuse GI intentionally consumes the lean proxy set only:
   * emissive radiance comes from the separate emissive buffer, and diffuse transport only needs
   * this coarse albedo field instead of the specular/direct continuation payload. */
  uint32_t max_user_id = 0;
  for (const SceneGeometryBuild &entry : geometry) {
    max_user_id = std::max(max_user_id, entry.user_id);
  }

  const size_t color_count = geometry.empty() ? 1 : size_t(max_user_id) + 1;
  if (!rt_storage_buffer_create(r_buffer, color_count * sizeof(float4))) {
    return false;
  }

  auto *diffuse_albedo = static_cast<float4 *>(r_buffer.mapped);
  for (size_t i = 0; i < color_count; i++) {
    diffuse_albedo[i] = float4(0.8f, 0.8f, 0.8f, 0.0f);
  }
  for (const SceneGeometryBuild &entry : geometry) {
    diffuse_albedo[entry.user_id] = float4(entry.diffuse_albedo, 0.0f);
  }
  return true;
}

static bool build_material_proxy_buffer(const std::vector<SceneGeometryBuild> &geometry,
                                        RTBuffer &r_buffer)
{
  /* Direct/specular fallback keeps the bounded continuation proxy separate from the diffuse GI
   * buffer: one dominant closure family plus tint, roughness, IOR, and the dielectric hint. */
  uint32_t max_user_id = 0;
  for (const SceneGeometryBuild &entry : geometry) {
    max_user_id = std::max(max_user_id, entry.user_id);
  }

  const size_t proxy_count = geometry.empty() ? 1 : size_t(max_user_id) + 1;
  if (!rt_storage_buffer_create(r_buffer, proxy_count * sizeof(HardwareMaterialProxyRecord))) {
    return false;
  }

  auto *proxies = static_cast<HardwareMaterialProxyRecord *>(r_buffer.mapped);
  for (size_t i = 0; i < proxy_count; i++) {
    proxies[i].reflection_color_roughness = float4(0.8f, 0.8f, 0.8f, 1.0f);
    proxies[i].transmission_color_roughness = float4(0.8f, 0.8f, 0.8f, 1.0f);
    proxies[i].ior_closure_type = float4(1.45f, 1.45f, 1.0f, 0.0f);
    proxies[i].packed_thickness = float4(0.0f);
  }

  for (const SceneGeometryBuild &entry : geometry) {
    proxies[entry.user_id].reflection_color_roughness = float4(entry.reflection_color,
                                                               entry.reflection_roughness);
    proxies[entry.user_id].transmission_color_roughness = float4(entry.transmission_color,
                                                                 entry.transmission_roughness);
    proxies[entry.user_id].ior_closure_type = float4(entry.reflection_ior,
                                                     entry.refraction_ior,
                                                     float(entry.closure_type),
                                                     float(entry.proxy_flags));
    proxies[entry.user_id].packed_thickness = float4(
        entry.packed_thickness, entry.alpha, entry.reflection_layer_coverage, 0.0f);
  }
  return true;
}

static bool build_triangle_normal_buffer(const std::vector<SceneGeometryBuild> &geometry,
                                         RTBuffer &r_buffer,
                                         std::vector<TriangleNormalRangeRecord> &r_ranges)
{
  uint32_t max_user_id = 0;
  for (const SceneGeometryBuild &entry : geometry) {
    max_user_id = std::max(max_user_id, entry.user_id);
  }

  r_ranges.assign(geometry.empty() ? 1 : max_user_id + 1, {0u, 0u});
  std::vector<float4> triangle_normals;
  for (const SceneGeometryBuild &entry : geometry) {
    TriangleNormalRangeRecord range = {};
    range.offset = uint32_t(triangle_normals.size());
    range.count = uint32_t(entry.triangle_normals.size());
    if (range.count > 0) {
      for (const float4 &normal_local : entry.triangle_normals) {
        float3 normal_world = math::transform_direction(
            entry.object_to_world, float3(normal_local.x, normal_local.y, normal_local.z));
        const float len_sq = math::length_squared(normal_world);
        if (len_sq > 1.0e-20f) {
          normal_world /= std::sqrt(len_sq);
        }
        else {
          normal_world = float3(0.0f, 0.0f, 1.0f);
        }
        triangle_normals.emplace_back(normal_world, 0.0f);
      }
    }
    r_ranges[entry.user_id] = range;
  }

  const size_t normal_count = triangle_normals.empty() ? 1 : triangle_normals.size();
  if (!rt_storage_buffer_create(r_buffer, normal_count * sizeof(float4))) {
    return false;
  }

  auto *out_normals = static_cast<float4 *>(r_buffer.mapped);
  out_normals[0] = float4(0.0f);
  for (size_t i = 0; i < triangle_normals.size(); i++) {
    out_normals[i] = triangle_normals[i];
  }
  return true;
}

static bool build_triangle_smooth_normal_buffer(const std::vector<SceneGeometryBuild> &geometry,
                                                RTBuffer &r_buffer)
{
  std::vector<float4> triangle_smooth_normals;
  for (const SceneGeometryBuild &entry : geometry) {
    if (entry.triangle_smooth_normals.empty()) {
      continue;
    }
    for (const float4 &normal_local : entry.triangle_smooth_normals) {
      float3 normal_world = math::transform_direction(
          entry.object_to_world, float3(normal_local.x, normal_local.y, normal_local.z));
      const float len_sq = math::length_squared(normal_world);
      if (len_sq > 1.0e-20f) {
        normal_world /= std::sqrt(len_sq);
      }
      else {
        normal_world = float3(0.0f, 0.0f, 1.0f);
      }
      triangle_smooth_normals.emplace_back(normal_world, 0.0f);
    }
  }

  const size_t normal_count = triangle_smooth_normals.empty() ? 1 :
                                                                triangle_smooth_normals.size();
  if (!rt_storage_buffer_create(r_buffer, normal_count * sizeof(float4))) {
    return false;
  }

  auto *out_normals = static_cast<float4 *>(r_buffer.mapped);
  out_normals[0] = float4(0.0f);
  for (size_t i = 0; i < triangle_smooth_normals.size(); i++) {
    out_normals[i] = triangle_smooth_normals[i];
  }
  return true;
}

static bool build_triangle_local_position_buffer(const std::vector<SceneGeometryBuild> &geometry,
                                                 RTBuffer &r_buffer)
{
  std::vector<float4> triangle_local_positions;
  for (const SceneGeometryBuild &entry : geometry) {
    if (entry.triangle_local_positions.empty()) {
      continue;
    }
    triangle_local_positions.insert(triangle_local_positions.end(),
                                    entry.triangle_local_positions.begin(),
                                    entry.triangle_local_positions.end());
  }

  const size_t position_count = triangle_local_positions.empty() ?
                                    1 :
                                    triangle_local_positions.size();
  if (!rt_storage_buffer_create(r_buffer, position_count * sizeof(float4))) {
    return false;
  }

  auto *out_positions = static_cast<float4 *>(r_buffer.mapped);
  out_positions[0] = float4(0.0f);
  for (size_t i = 0; i < triangle_local_positions.size(); i++) {
    out_positions[i] = triangle_local_positions[i];
  }
  return true;
}

static bool build_triangle_normal_range_buffer(
    const std::vector<TriangleNormalRangeRecord> &ranges, RTBuffer &r_buffer)
{
  const size_t range_count = ranges.empty() ? 1 : ranges.size();
  if (!rt_storage_buffer_create(r_buffer, range_count * sizeof(TriangleNormalRangeRecord))) {
    return false;
  }

  auto *out_ranges = static_cast<TriangleNormalRangeRecord *>(r_buffer.mapped);
  out_ranges[0] = {0u, 0u};
  for (size_t i = 0; i < ranges.size(); i++) {
    out_ranges[i] = ranges[i];
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Uniform structs (must match the GLSL `scalar` layout blocks)
 * \{ */

struct HardwareTraceUniforms {
  float4x4 viewinv;
  float4x4 wininv;
  int2 full_resolution;
  int resolution_scale;
  int resolution_scale_denominator;
  int closure_index;
  uint32_t feature_mask;
  int hardware_trace_phase;
  int reflection_bounces;
  int refraction_bounces;
  int _pad0;
  int2 resolution_bias;
  float clamp_indirect;
  float4 world_probe_atlas_coord;
  /* x: specular/refraction world probe, y: emissive count, z: GI sample count,
   * w: diffuse GI world probe. */
  int4 use_environment_pad;
  int4 light_count_pad;
  float4 sampling_rand;
};

struct HardwareShadowUniforms {
  float4x4 viewinv;
  float4x4 wininv;
  int4 resolution_layer;
  float4 light_direction_bias;
  float4 shadow_params;
  int4 world_sun_slot_pad;
  float4 sampling_rand;
};

struct HardwareLocalShadowUniforms {
  float4x4 viewinv;
  float4x4 wininv;
  int4 resolution_layer_type;
  float4 light_position_radius;
  float4 light_x_axis_size_x;
  float4 light_y_axis_size_y;
  float4 shadow_offset_scale;
  float4 normal_bias_pad;
  float4 sampling_rand;
  /* Nuru: caustic-shadow extras. .x = Photons intensity (0..10). Remaining slots reserved. */
  float4 caustic_params;
};

struct HardwareEnvironmentVisibilityUniforms {
  float4x4 viewinv;
  float4x4 wininv;
  int4 resolution_samples;
  float4 normal_bias_pad;
  float4 sampling_rand;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pipelines
 * \{ */

enum class NuruKernel : int {
  TRACE = 0,
  DIRECTIONAL_SHADOW,
  DIRECTIONAL_HIT_SHADOW,
  ENVIRONMENT_VISIBILITY,
  HIT_ENVIRONMENT_VISIBILITY,
  LOCAL_SHADOW,
  LOCAL_HIT_SHADOW,
  OIDN_PACK,
  OIDN_UNPACK,
  KERNEL_MAX,
};

enum class BindingKind : uint8_t {
  UBO,
  ACCELERATION_STRUCTURE,
  SSBO,
  SAMPLED,
  STORAGE_IMAGE,
};

struct BindingSpec {
  uint32_t binding;
  BindingKind kind;
};

struct KernelPipeline {
  VkShaderModule shader_module = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  std::vector<BindingSpec> bindings;
  bool failed = false;
};

static const char *kernel_name(const NuruKernel kernel)
{
  switch (kernel) {
    case NuruKernel::TRACE:
      return "eevee_hardware_trace_override";
    case NuruKernel::DIRECTIONAL_SHADOW:
      return "eevee_hardware_trace_directional_shadow";
    case NuruKernel::DIRECTIONAL_HIT_SHADOW:
      return "eevee_hardware_trace_directional_hit_shadow";
    case NuruKernel::ENVIRONMENT_VISIBILITY:
      return "eevee_hardware_trace_environment_visibility";
    case NuruKernel::HIT_ENVIRONMENT_VISIBILITY:
      return "eevee_hardware_trace_hit_environment_visibility";
    case NuruKernel::LOCAL_SHADOW:
      return "eevee_hardware_trace_local_shadow";
    case NuruKernel::LOCAL_HIT_SHADOW:
      return "eevee_hardware_trace_local_hit_shadow";
    case NuruKernel::OIDN_PACK:
      return "eevee_oidn_pack";
    case NuruKernel::OIDN_UNPACK:
      return "eevee_oidn_unpack";
    default:
      return "unknown";
  }
}

static const char *kernel_glsl(const NuruKernel kernel)
{
  switch (kernel) {
    case NuruKernel::TRACE:
      return nuru_kernels::trace_override_glsl();
    case NuruKernel::DIRECTIONAL_SHADOW:
      return nuru_kernels::directional_shadow_glsl();
    case NuruKernel::DIRECTIONAL_HIT_SHADOW:
      return nuru_kernels::directional_hit_shadow_glsl();
    case NuruKernel::ENVIRONMENT_VISIBILITY:
      return nuru_kernels::environment_visibility_glsl();
    case NuruKernel::HIT_ENVIRONMENT_VISIBILITY:
      return nuru_kernels::hit_environment_visibility_glsl();
    case NuruKernel::LOCAL_SHADOW:
      return nuru_kernels::local_shadow_glsl();
    case NuruKernel::LOCAL_HIT_SHADOW:
      return nuru_kernels::local_hit_shadow_glsl();
    case NuruKernel::OIDN_PACK:
      return nuru_kernels::oidn_pack_glsl();
    case NuruKernel::OIDN_UNPACK:
      return nuru_kernels::oidn_unpack_glsl();
    default:
      return nullptr;
  }
}

/* Per-kernel descriptor binding tables. Binding numbers follow the conventions used by the GLSL
 * kernels: 0 = UBO, 1 = TLAS, 2..15 = SSBOs (Metal buffer indices), 16..39 = sampled textures
 * (16 + Metal texture index), 40+ = storage images (40 + Metal texture index). */
static std::vector<BindingSpec> kernel_binding_specs(const NuruKernel kernel)
{
  using BK = BindingKind;
  std::vector<BindingSpec> specs;
  auto add_range = [&specs](uint32_t first, uint32_t count, BK kind) {
    for (uint32_t i = 0; i < count; i++) {
      specs.push_back({first + i, kind});
    }
  };

  switch (kernel) {
    case NuruKernel::TRACE:
      specs.push_back({0, BK::UBO});
      specs.push_back({1, BK::ACCELERATION_STRUCTURE});
      add_range(2, 10, BK::SSBO);       /* buffers 2..11. */
      add_range(16, 5, BK::SAMPLED);    /* textures 0..4. */
      specs.push_back({80, BK::SAMPLED}); /* world_probe_tx (16+35 collides with 40+11). */
      /* Include all three emission-replay payloads (textures 35..37). Omitting these
       * bindings corrupts descriptors following the hit payloads, including the world probe. */
      add_range(40 + 5, 33, BK::STORAGE_IMAGE); /* textures 5..37. */
      break;
    case NuruKernel::DIRECTIONAL_SHADOW:
      specs.push_back({0, BK::UBO});
      specs.push_back({1, BK::ACCELERATION_STRUCTURE});
      add_range(2, 5, BK::SSBO); /* sunlight, proxies, tri normals, smooth, ranges. */
      add_range(16, 3, BK::SAMPLED);
      specs.push_back({40 + 3, BK::STORAGE_IMAGE});
      break;
    case NuruKernel::DIRECTIONAL_HIT_SHADOW:
      specs.push_back({0, BK::UBO});
      specs.push_back({1, BK::ACCELERATION_STRUCTURE});
      add_range(2, 6, BK::SSBO);
      add_range(16, 3, BK::SAMPLED);
      specs.push_back({40 + 3, BK::STORAGE_IMAGE});
      break;
    case NuruKernel::ENVIRONMENT_VISIBILITY:
      specs.push_back({0, BK::UBO});
      specs.push_back({1, BK::ACCELERATION_STRUCTURE});
      add_range(16, 3, BK::SAMPLED);
      specs.push_back({40 + 3, BK::STORAGE_IMAGE});
      break;
    case NuruKernel::HIT_ENVIRONMENT_VISIBILITY:
      specs.push_back({0, BK::UBO});
      specs.push_back({1, BK::ACCELERATION_STRUCTURE});
      specs.push_back({2, BK::SSBO});
      add_range(16, 2, BK::SAMPLED);
      specs.push_back({40 + 2, BK::STORAGE_IMAGE});
      break;
    case NuruKernel::LOCAL_SHADOW:
      specs.push_back({0, BK::UBO});
      specs.push_back({1, BK::ACCELERATION_STRUCTURE});
      add_range(2, 4, BK::SSBO);
      add_range(16, 3, BK::SAMPLED);
      specs.push_back({40 + 3, BK::STORAGE_IMAGE});
      break;
    case NuruKernel::LOCAL_HIT_SHADOW:
      specs.push_back({0, BK::UBO});
      specs.push_back({1, BK::ACCELERATION_STRUCTURE});
      add_range(2, 5, BK::SSBO);
      add_range(16, 3, BK::SAMPLED);
      specs.push_back({40 + 3, BK::STORAGE_IMAGE});
      break;
    case NuruKernel::OIDN_PACK:
      specs.push_back({0, BK::UBO});
      add_range(2, 3, BK::SSBO);
      add_range(16, 3, BK::SAMPLED);
      break;
    case NuruKernel::OIDN_UNPACK:
      specs.push_back({0, BK::UBO});
      specs.push_back({2, BK::SSBO});
      specs.push_back({16, BK::SAMPLED});
      specs.push_back({40 + 1, BK::STORAGE_IMAGE});
      break;
    default:
      break;
  }
  return specs;
}

static VkDescriptorType to_vk_descriptor_type(const BindingKind kind)
{
  switch (kind) {
    case BindingKind::UBO:
      return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case BindingKind::ACCELERATION_STRUCTURE:
      return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    case BindingKind::SSBO:
      return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case BindingKind::SAMPLED:
      return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case BindingKind::STORAGE_IMAGE:
      return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  }
  return VK_DESCRIPTOR_TYPE_MAX_ENUM;
}

/* SPIR-V disk cache for the Nuru RT kernels. shaderc compilation of the large trace kernels
 * costs hundreds of milliseconds per session (every kernel recompiles on every Blender launch);
 * SPIR-V itself is portable, so cache it keyed by a hash of the GLSL source. Mirrors the
 * `vk-spirv-cache` pattern in `vk_shader_compiler.cc`. */
static std::optional<std::string> kernel_cache_dir_get()
{
  static std::optional<std::string> result = []() -> std::optional<std::string> {
    char tmp_dir_buffer[FILE_MAX] = {};
    /* Keep portable and isolated test sessions out of the real user cache. */
    if (const char *user_resources = std::getenv("BLENDER_USER_RESOURCES")) {
      BLI_path_join(tmp_dir_buffer, sizeof(tmp_dir_buffer), user_resources, "cache", SEP_STR);
    }
    else {
      BKE_appdir_folder_caches(tmp_dir_buffer, sizeof(tmp_dir_buffer));
    }
    if (tmp_dir_buffer[0] == '\0') {
      return std::nullopt;
    }
    std::string cache_dir = std::string(tmp_dir_buffer) + "vk-nuru-kernel-cache" + SEP_STR;
    BLI_dir_create_recursive(cache_dir.c_str());
    return cache_dir;
  }();
  return result;
}

static uint64_t kernel_cache_fnv1a64(const char *str)
{
  uint64_t hash = 0xCBF29CE484222325ull;
  for (const char *c = str; *c != '\0'; c++) {
    hash ^= uint64_t(uint8_t(*c));
    hash *= 0x100000001B3ull;
  }
  return hash;
}

static std::string kernel_cache_path(const NuruKernel kernel, const char *source)
{
  /* Salt with a cache version so loader ABI changes invalidate old blobs. */
  constexpr uint64_t cache_version = 1;
  const uint64_t source_hash = kernel_cache_fnv1a64(source);
  const uint64_t name_hash = kernel_cache_fnv1a64(kernel_name(kernel));
  char filename[64];
  SNPRINTF(filename,
           "%016llx%016llx.spv",
           (unsigned long long)(source_hash ^ (cache_version * 0x9E3779B97F4A7C15ull)),
           (unsigned long long)name_hash);
  return *kernel_cache_dir_get() + filename;
}

static bool kernel_spirv_read_cache(const std::string &path, std::vector<uint32_t> &r_spirv)
{
  if (!BLI_exists(path.c_str())) {
    return false;
  }
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::ate);
  if (!file.is_open()) {
    return false;
  }
  const std::streamsize size = file.tellg();
  if (size <= 0 || (size % 4) != 0) {
    return false;
  }
  file.seekg(0, std::ios::beg);
  r_spirv.resize(size_t(size) / 4);
  file.read(reinterpret_cast<char *>(r_spirv.data()), size);
  if (!file.good()) {
    r_spirv.clear();
    return false;
  }
  /* Minimal validity check: SPIR-V magic number. */
  if (r_spirv.empty() || r_spirv[0] != 0x07230203u) {
    r_spirv.clear();
    return false;
  }
  return true;
}

static void kernel_spirv_write_cache(const std::string &path, const std::vector<uint32_t> &spirv)
{
  std::fstream file(path, std::ios::binary | std::ios::out);
  if (!file.is_open()) {
    return;
  }
  file.write(reinterpret_cast<const char *>(spirv.data()),
             std::streamsize(spirv.size() * sizeof(uint32_t)));
}

static bool compile_kernel_spirv(const NuruKernel kernel, std::vector<uint32_t> &r_spirv)
{
  const char *source = kernel_glsl(kernel);
  if (source == nullptr || source[0] == '\0') {
    return false;
  }

  std::string cache_path;
  if (kernel_cache_dir_get().has_value()) {
    cache_path = kernel_cache_path(kernel, source);
    if (kernel_spirv_read_cache(cache_path, r_spirv)) {
      return true;
    }
  }

  shaderc::Compiler compiler;
  shaderc::CompileOptions options;
  options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
  options.SetOptimizationLevel(shaderc_optimization_level_performance);

  shaderc::SpvCompilationResult result = compiler.CompileGlslToSpv(
      source, shaderc_compute_shader, kernel_name(kernel), options);
  if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
    /* SAPPHIRE history: very large kernels can overflow SPIR-V id bounds under the optimizer.
     * Retry once with optimization disabled before giving up. */
    options.SetOptimizationLevel(shaderc_optimization_level_zero);
    result = compiler.CompileGlslToSpv(
        source, shaderc_compute_shader, kernel_name(kernel), options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
      fprintf(stderr,
              "Vulkan RT kernel %s compile failed: %s\n",
              kernel_name(kernel),
              result.GetErrorMessage().c_str());
      return false;
    }
  }
  r_spirv.assign(result.cbegin(), result.cend());
  if (!cache_path.empty()) {
    kernel_spirv_write_cache(cache_path, r_spirv);
  }
  return true;
}

static KernelPipeline g_kernel_pipelines[int(NuruKernel::KERNEL_MAX)];

static KernelPipeline &kernel_pipeline_get(const NuruKernel kernel)
{
  KernelPipeline &entry = g_kernel_pipelines[int(kernel)];
  if (entry.pipeline != VK_NULL_HANDLE || entry.failed) {
    return entry;
  }

  VKDevice &device = VKBackend::get().device;

  std::vector<uint32_t> spirv;
  if (!compile_kernel_spirv(kernel, spirv)) {
    entry.failed = true;
    return entry;
  }

  VkShaderModuleCreateInfo module_info = {};
  module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  module_info.codeSize = spirv.size() * sizeof(uint32_t);
  module_info.pCode = spirv.data();
  if (vkCreateShaderModule(device.vk_handle(), &module_info, nullptr, &entry.shader_module) !=
      VK_SUCCESS)
  {
    entry.failed = true;
    return entry;
  }

  entry.bindings = kernel_binding_specs(kernel);
  std::vector<VkDescriptorSetLayoutBinding> layout_bindings;
  layout_bindings.reserve(entry.bindings.size());
  for (const BindingSpec &spec : entry.bindings) {
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = spec.binding;
    binding.descriptorType = to_vk_descriptor_type(spec.kind);
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    layout_bindings.push_back(binding);
  }

  VkDescriptorSetLayoutCreateInfo layout_info = {};
  layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layout_info.bindingCount = uint32_t(layout_bindings.size());
  layout_info.pBindings = layout_bindings.data();
  if (vkCreateDescriptorSetLayout(
          device.vk_handle(), &layout_info, nullptr, &entry.descriptor_set_layout) != VK_SUCCESS)
  {
    entry.failed = true;
    return entry;
  }

  VkPipelineLayoutCreateInfo pipeline_layout_info = {};
  pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_info.setLayoutCount = 1;
  pipeline_layout_info.pSetLayouts = &entry.descriptor_set_layout;
  if (vkCreatePipelineLayout(
          device.vk_handle(), &pipeline_layout_info, nullptr, &entry.pipeline_layout) !=
      VK_SUCCESS)
  {
    entry.failed = true;
    return entry;
  }

  VkComputePipelineCreateInfo pipeline_info = {};
  pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipeline_info.stage.module = entry.shader_module;
  pipeline_info.stage.pName = "main";
  pipeline_info.layout = entry.pipeline_layout;
  if (vkCreateComputePipelines(device.vk_handle(),
                               VK_NULL_HANDLE,
                               1,
                               &pipeline_info,
                               nullptr,
                               &entry.pipeline) != VK_SUCCESS)
  {
    fprintf(stderr, "Vulkan RT kernel %s pipeline creation failed\n", kernel_name(kernel));
    entry.failed = true;
    return entry;
  }

  return entry;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Samplers
 * \{ */

static VkSampler g_raytrace_samplers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

static VkSampler raytrace_sampler_get(const bool linear)
{
  VkSampler &sampler = g_raytrace_samplers[linear ? 1 : 0];
  if (sampler != VK_NULL_HANDLE) {
    return sampler;
  }
  VKDevice &device = VKBackend::get().device;
  VkSamplerCreateInfo create_info = {};
  create_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  create_info.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  create_info.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  create_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  create_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  create_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  create_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  create_info.maxLod = VK_LOD_CLAMP_NONE;
  vkCreateSampler(device.vk_handle(), &create_info, nullptr, &sampler);
  return sampler;
}

static VkImageView g_dummy_array_view = VK_NULL_HANDLE;
static bool g_dummy_array_failed = false;
static VkImage g_dummy_array_image = VK_NULL_HANDLE;
static VmaAllocation g_dummy_array_allocation = VK_NULL_HANDLE;

/* 1x1 RGBA16F 2D-array texture in GENERAL layout, used as inert filler for optional sampler
 * bindings (e.g. the world probe when the environment contribution is disabled). */
static VkImageView raytrace_dummy_array_view_get()
{
  VkImageView &view = g_dummy_array_view;
  bool &failed = g_dummy_array_failed;
  if (view != VK_NULL_HANDLE || failed) {
    return view;
  }
  VKDevice &device = VKBackend::get().device;

  VkImageCreateInfo image_info = {};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  image_info.extent = {1, 1, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VmaAllocationCreateInfo alloc_info = {};
  alloc_info.usage = VMA_MEMORY_USAGE_AUTO;

  VkImage &image = g_dummy_array_image;
  VmaAllocation &allocation = g_dummy_array_allocation;
  if (vmaCreateImage(device.mem_allocator_get(),
                     &image_info,
                     &alloc_info,
                     &image,
                     &allocation,
                     nullptr) != VK_SUCCESS)
  {
    failed = true;
    return VK_NULL_HANDLE;
  }

  VkImageViewCreateInfo view_info = {};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  view_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(device.vk_handle(), &view_info, nullptr, &view) != VK_SUCCESS) {
    failed = true;
    view = VK_NULL_HANDLE;
    return VK_NULL_HANDLE;
  }

  /* One-time transition to GENERAL so descriptor layout expectations always hold. */
  VkCommandBuffer command_buffer = raytrace_command_buffer_alloc();
  if (command_buffer == VK_NULL_HANDLE) {
    failed = true;
    view = VK_NULL_HANDLE;
    return VK_NULL_HANDLE;
  }
  VkImageMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = 0;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(command_buffer,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       0,
                       0,
                       nullptr,
                       0,
                       nullptr,
                       1,
                       &barrier);
  raytrace_command_buffer_submit_and_wait(command_buffer);
  return view;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dispatch infrastructure
 * \{ */

struct ImageBinding {
  uint32_t binding;
  VKTexture *texture = nullptr;
  /* Used instead of `texture` for backend-owned views (dummy filler). Must already be in
   * VK_IMAGE_LAYOUT_GENERAL. */
  VkImageView raw_view = VK_NULL_HANDLE;
  bool sampled = false;
  bool linear_sampler = false;
  bool arrayed = false;
};

struct BufferBinding {
  uint32_t binding;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceSize size = VK_WHOLE_SIZE;
};

struct KernelArgs {
  const void *uniforms_data = nullptr;
  size_t uniforms_size = 0;
  VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
  std::vector<BufferBinding> ssbos;
  std::vector<ImageBinding> images;
  /* Direct dispatch. */
  int3 group_count = int3(0);
  /* Indirect dispatch (takes precedence when not VK_NULL_HANDLE). */
  VkBuffer indirect_buffer = VK_NULL_HANDLE;
};

}  // namespace blender::gpu::vulkan

namespace blender {

/* In-flight submission bookkeeping. Lives in `blender` namespace so the scene destructor can
 * reference it. */
struct NuruVKSubmission {
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  std::vector<VkDescriptorPool> descriptor_pools;
  std::vector<gpu::vulkan::RTBuffer> uniform_buffers;
  /* TEMPORARY DIAGNOSTIC: execution-time copies of the indirect dispatch args (one per
   * indirect dispatch recorded into this submission). Remove before completion. */
  std::vector<gpu::vulkan::RTBuffer> args_snapshots;
  std::vector<int> args_snapshot_kernels;
  /* END DIAGNOSTIC */
};

}  // namespace blender

namespace blender::gpu::vulkan {

static std::vector<NuruVKSubmission *> &in_flight_submissions()
{
  static std::vector<NuruVKSubmission *> submissions;
  return submissions;
}

static void submission_destroy(NuruVKSubmission *submission)
{
  VKDevice &device = VKBackend::get().device;
  /* TEMPORARY DIAGNOSTIC: report the execution-time indirect args captured in this submission.
   * Remove before completion. */
  if (!submission->args_snapshots.empty()) {
    const bool fence_ok = (submission->fence != VK_NULL_HANDLE) &&
                          (vkGetFenceStatus(device.vk_handle(), submission->fence) == VK_SUCCESS);
    for (size_t i = 0; i < submission->args_snapshots.size(); i++) {
      RTBuffer &snapshot = submission->args_snapshots[i];
      if (snapshot.mapped == nullptr) {
        continue;
      }
      const uint32_t *groups = static_cast<const uint32_t *>(snapshot.mapped);
      std::fprintf(stderr,
                   "EEVEE HWRT DIAG exec_args slot=%zu kernel=%d fence_ok=%d groups=%u,%u,%u\n",
                   i,
                   submission->args_snapshot_kernels[i],
                   fence_ok ? 1 : 0,
                   groups[0],
                   groups[1],
                   groups[2]);
      rt_buffer_free(snapshot);
    }
  }
  /* END DIAGNOSTIC */
  for (VkDescriptorPool pool : submission->descriptor_pools) {
    vkDestroyDescriptorPool(device.vk_handle(), pool, nullptr);
  }
  for (RTBuffer &buffer : submission->uniform_buffers) {
    rt_buffer_free(buffer);
  }
  if (submission->fence != VK_NULL_HANDLE) {
    vkDestroyFence(device.vk_handle(), submission->fence, nullptr);
  }
  if (submission->command_buffer != VK_NULL_HANDLE) {
    vkFreeCommandBuffers(
        device.vk_handle(), raytrace_command_pool_get(), 1, &submission->command_buffer);
  }
  delete submission;
}

static void collect_finished_submissions(const bool wait_all)
{
  VKDevice &device = VKBackend::get().device;
  std::vector<NuruVKSubmission *> &submissions = in_flight_submissions();
  for (auto it = submissions.begin(); it != submissions.end();) {
    NuruVKSubmission *submission = *it;
    VkResult status = vkGetFenceStatus(device.vk_handle(), submission->fence);
    if (status != VK_SUCCESS && wait_all) {
      vkWaitForFences(device.vk_handle(),
                      1,
                      &submission->fence,
                      VK_TRUE,
                      uint64_t(60) * 1000 * 1000 * 1000);
      status = vkGetFenceStatus(device.vk_handle(), submission->fence);
    }
    if (status == VK_SUCCESS) {
      submission_destroy(submission);
      it = submissions.erase(it);
    }
    else {
      ++it;
    }
  }
}

static NuruVKSubmission *submission_begin()
{
  /* Keep the in-flight list shallow; HWRT dispatches are heavyweight. */
  if (in_flight_submissions().size() > 8) {
    collect_finished_submissions(true);
  }
  else {
    collect_finished_submissions(false);
  }

  VkCommandBuffer command_buffer = raytrace_command_buffer_alloc();
  if (command_buffer == VK_NULL_HANDLE) {
    return nullptr;
  }
  NuruVKSubmission *submission = new NuruVKSubmission();
  submission->command_buffer = command_buffer;

  /* Order against all previously submitted GPU work (render graph included). */
  VkMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  vkCmdPipelineBarrier(command_buffer,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       0,
                       1,
                       &barrier,
                       0,
                       nullptr,
                       0,
                       nullptr);
  /* Nuru: wedge diagnostic (BLENDER_VULKAN_CHECKPOINTS=1). If this marker shows as
   * LAST_STARTED in a device-lost dump, the external RT batch passed its begin barrier;
   * if it never appears, the wedge sits in an earlier render-graph pass. */
  {
    const VKDevice &device = VKBackend::get().device;
    if (device.functions.vkCmdSetCheckpoint) {
      device.functions.vkCmdSetCheckpoint(command_buffer, "NURU_RT external begin");
    }
  }
  return submission;
}

static bool submission_end(NuruVKSubmission *submission, const bool wait)
{
  VKDevice &device = VKBackend::get().device;

  /* Nuru: wedge diagnostic (BLENDER_VULKAN_CHECKPOINTS=1). LAST_COMPLETED on this marker means
   * every kernel of the external batch finished on the GPU. */
  if (device.functions.vkCmdSetCheckpoint) {
    device.functions.vkCmdSetCheckpoint(submission->command_buffer, "NURU_RT external end");
  }

  /* Make compute writes visible to all later GPU work on this queue. */
  VkMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  vkCmdPipelineBarrier(submission->command_buffer,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       0,
                       1,
                       &barrier,
                       0,
                       nullptr,
                       0,
                       nullptr);
  vkEndCommandBuffer(submission->command_buffer);

  VkFenceCreateInfo fence_info = {};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (vkCreateFence(device.vk_handle(), &fence_info, nullptr, &submission->fence) != VK_SUCCESS) {
    submission_destroy(submission);
    return false;
  }

  /* Chain into the device timeline: wait for everything the render graph has claimed so far
   * and own the next slot so the following graph submission waits for this batch on the GPU.
   * Same-queue submissions may otherwise overlap execution; both Xid 109 checkpoint captures
   * show the GPU wedging on the first graph node right after an external RT batch completes
   * (a dispatch once, a plain buffer copy the next time), which fingerprints a cross-submission
   * resource hazard rather than any individual kernel. */
  TimelineValue chain_wait_value = 0;
  const TimelineValue chain_signal_value = device.external_timeline_chain_acquire(
      &chain_wait_value);
  /* Submission-order discipline; see `raytrace_command_buffer_submit_and_wait`. */
  device.wait_for_submission_timeline(chain_wait_value);
  VkSemaphore timeline_semaphore = device.timeline_semaphore_get();
  VkPipelineStageFlags chain_wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkTimelineSemaphoreSubmitInfo timeline_info = {};
  timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
  timeline_info.waitSemaphoreValueCount = 1;
  timeline_info.pWaitSemaphoreValues = &chain_wait_value;
  timeline_info.signalSemaphoreValueCount = 1;
  timeline_info.pSignalSemaphoreValues = &chain_signal_value;

  VkSubmitInfo submit_info = {};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.pNext = &timeline_info;
  submit_info.waitSemaphoreCount = 1;
  submit_info.pWaitSemaphores = &timeline_semaphore;
  submit_info.pWaitDstStageMask = &chain_wait_stage;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &submission->command_buffer;
  submit_info.signalSemaphoreCount = 1;
  submit_info.pSignalSemaphores = &timeline_semaphore;
  const bool submit_perf_logging = vulkan_raytrace_perf_logging_enabled();
  const double submit_start = submit_perf_logging ? BLI_time_now_seconds() : 0.0;
  {
    std::scoped_lock lock(device.queue_mutex_get());
    if (vkQueueSubmit(device.queue_get(), 1, &submit_info, submission->fence) != VK_SUCCESS) {
      /* Unblock the chain: later graph submissions already wait on `chain_signal_value`. */
      VkSemaphoreSignalInfo signal_info = {};
      signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
      signal_info.semaphore = timeline_semaphore;
      signal_info.value = chain_signal_value;
      vkSignalSemaphore(device.vk_handle(), &signal_info);
      device.external_timeline_chain_submitted(chain_signal_value);
      submission_destroy(submission);
      return false;
    }
  }
  device.external_timeline_chain_submitted(chain_signal_value);

  bool success = true;
  if (wait || vulkan_raytrace_force_sync()) {
    const double fence_wait_start = submit_perf_logging ? BLI_time_now_seconds() : 0.0;
    const VkResult wait_result = vkWaitForFences(
        device.vk_handle(), 1, &submission->fence, VK_TRUE, uint64_t(60) * 1000 * 1000 * 1000);
    if (submit_perf_logging) {
      /* With force-sync this attributes wall GPU time to each external batch. */
      const double now = BLI_time_now_seconds();
      std::fprintf(stderr,
                   "EEVEE HWRT perf external_wait submit_ms=%.2f wait_ms=%.2f\n",
                   (fence_wait_start - submit_start) * 1000.0,
                   (now - fence_wait_start) * 1000.0);
    }
    success = (wait_result == VK_SUCCESS);
    if (!success) {
      fprintf(stderr, "Vulkan RT submission wait failed with status=%d\n", int(wait_result));
      /* Nuru: wedge diagnostic. Covers both DEVICE_LOST and the 60s fence timeout; on timeout
       * the checkpoint state still names the graph node the GPU is spinning in. */
      device.diagnostic_checkpoints_dump();
    }
    submission_destroy(submission);
    if (success) {
      GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_SHADER_IMAGE_ACCESS);
    }
  }
  else {
    in_flight_submissions().push_back(submission);
  }
  return success;
}

static void transition_image_to_general(VkCommandBuffer command_buffer, VKTexture *texture)
{
  VKDevice &device = VKBackend::get().device;
  VkImage image = texture->vk_image_handle();
  if (image == VK_NULL_HANDLE) {
    return;
  }
  const VkImageLayout current_layout = device.resources.image_layout_get(image);
  if (current_layout == VK_IMAGE_LAYOUT_GENERAL) {
    return;
  }

  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  const TextureFormat format = texture->device_format_get();
  if (ELEM(format,
           TextureFormat::SFLOAT_32_DEPTH,
           TextureFormat::SFLOAT_32_DEPTH_UINT_8,
           TextureFormat::UNORM_16_DEPTH))
  {
    aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    /* Blender tracks one layout for the whole image and does not enable separate depth/stencil
     * layouts. Transition both aspects even when the sampled view exposes depth only. */
    if (format == TextureFormat::SFLOAT_32_DEPTH_UINT_8) {
      aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
  }

  VkImageMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.oldLayout = current_layout;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = aspect;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
  vkCmdPipelineBarrier(command_buffer,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       0,
                       0,
                       nullptr,
                       0,
                       nullptr,
                       1,
                       &barrier);
  device.resources.update_image_layout(image, VK_IMAGE_LAYOUT_GENERAL);
}

/* Record one kernel dispatch into the submission's command buffer. */
static bool record_kernel_dispatch(NuruVKSubmission *submission,
                                   const NuruKernel kernel,
                                   const KernelArgs &args)
{
  VKDevice &device = VKBackend::get().device;
  KernelPipeline &pipeline = kernel_pipeline_get(kernel);
  if (pipeline.pipeline == VK_NULL_HANDLE) {
    return false;
  }

  /* Transition every touched image to GENERAL so both sampled and storage access are valid. */
  for (const ImageBinding &image : args.images) {
    if (image.texture != nullptr) {
      transition_image_to_general(submission->command_buffer, image.texture);
    }
  }

  /* Uniform buffer. */
  RTBuffer uniform_buffer;
  if (args.uniforms_size > 0) {
    if (!rt_buffer_create(uniform_buffer,
                          args.uniforms_size,
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          /*host_visible=*/true))
    {
      return false;
    }
    std::memcpy(uniform_buffer.mapped, args.uniforms_data, args.uniforms_size);
    submission->uniform_buffers.push_back(uniform_buffer);
  }

  /* Descriptor pool + set. */
  std::vector<VkDescriptorPoolSize> pool_sizes;
  auto add_pool_size = [&pool_sizes](VkDescriptorType type, uint32_t count) {
    if (count > 0) {
      pool_sizes.push_back({type, count});
    }
  };
  uint32_t ubo_count = 0, as_count = 0, ssbo_count = 0, sampled_count = 0, storage_count = 0;
  for (const BindingSpec &spec : pipeline.bindings) {
    switch (spec.kind) {
      case BindingKind::UBO:
        ubo_count++;
        break;
      case BindingKind::ACCELERATION_STRUCTURE:
        as_count++;
        break;
      case BindingKind::SSBO:
        ssbo_count++;
        break;
      case BindingKind::SAMPLED:
        sampled_count++;
        break;
      case BindingKind::STORAGE_IMAGE:
        storage_count++;
        break;
    }
  }
  add_pool_size(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, ubo_count);
  add_pool_size(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, as_count);
  add_pool_size(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, ssbo_count);
  add_pool_size(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sampled_count);
  add_pool_size(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storage_count);

  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = uint32_t(pool_sizes.size());
  pool_info.pPoolSizes = pool_sizes.data();
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  if (vkCreateDescriptorPool(device.vk_handle(), &pool_info, nullptr, &descriptor_pool) !=
      VK_SUCCESS)
  {
    return false;
  }
  submission->descriptor_pools.push_back(descriptor_pool);

  VkDescriptorSetAllocateInfo set_alloc_info = {};
  set_alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  set_alloc_info.descriptorPool = descriptor_pool;
  set_alloc_info.descriptorSetCount = 1;
  set_alloc_info.pSetLayouts = &pipeline.descriptor_set_layout;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  if (vkAllocateDescriptorSets(device.vk_handle(), &set_alloc_info, &descriptor_set) !=
      VK_SUCCESS)
  {
    return false;
  }

  /* Descriptor writes. Keep backing storage alive until vkUpdateDescriptorSets returns. */
  std::vector<VkWriteDescriptorSet> writes;
  std::vector<VkDescriptorBufferInfo> buffer_infos;
  std::vector<VkDescriptorImageInfo> image_infos;
  buffer_infos.reserve(1 + args.ssbos.size());
  image_infos.reserve(args.images.size());
  writes.reserve(2 + args.ssbos.size() + args.images.size());

  VkWriteDescriptorSetAccelerationStructureKHR as_write = {};
  if (args.tlas != VK_NULL_HANDLE) {
    as_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    as_write.accelerationStructureCount = 1;
    as_write.pAccelerationStructures = &args.tlas;
    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.pNext = &as_write;
    write.dstSet = descriptor_set;
    write.dstBinding = 1;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes.push_back(write);
  }

  if (args.uniforms_size > 0) {
    buffer_infos.push_back({uniform_buffer.buffer, 0, VK_WHOLE_SIZE});
    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptor_set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &buffer_infos.back();
    writes.push_back(write);
  }

  for (const BufferBinding &ssbo : args.ssbos) {
    buffer_infos.push_back({ssbo.buffer, 0, ssbo.size});
    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptor_set;
    write.dstBinding = ssbo.binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer_infos.back();
    writes.push_back(write);
  }

  for (const ImageBinding &image : args.images) {
    VkImageView view_handle = image.raw_view;
    if (image.texture != nullptr) {
      const VKImageView &view = image.texture->image_view_get(
          image.arrayed ? VKImageViewArrayed::ARRAYED : VKImageViewArrayed::NOT_ARRAYED,
          VKImageViewFlags::DEFAULT);
      view_handle = view.vk_handle();
    }
    if (view_handle == VK_NULL_HANDLE) {
      return false;
    }
    VkDescriptorImageInfo info = {};
    info.imageView = view_handle;
    info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    info.sampler = image.sampled ? raytrace_sampler_get(image.linear_sampler) : VK_NULL_HANDLE;
    image_infos.push_back(info);
    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptor_set;
    write.dstBinding = image.binding;
    write.descriptorCount = 1;
    write.descriptorType = image.sampled ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
                                           VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &image_infos.back();
    writes.push_back(write);
  }

  vkUpdateDescriptorSets(
      device.vk_handle(), uint32_t(writes.size()), writes.data(), 0, nullptr);

  vkCmdBindPipeline(
      submission->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
  vkCmdBindDescriptorSets(submission->command_buffer,
                          VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipeline.pipeline_layout,
                          0,
                          1,
                          &descriptor_set,
                          0,
                          nullptr);
  if (args.indirect_buffer != VK_NULL_HANDLE) {
    /* TEMPORARY DIAGNOSTIC: snapshot the indirect args at execution time, inside this very
     * submission, so the values are exactly what the dispatch consumes (CPU readbacks serialize
     * the pipeline and mask the failure). Printed by collect_finished_submissions(). Remove
     * before completion. */
    if (env_flag_enabled("BLENDER_EEVEE_HWRT_SNAPSHOT_ARGS")) {
      RTBuffer snapshot;
      if (rt_buffer_create(snapshot,
                           sizeof(uint32_t) * 4,
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           /*host_visible=*/true,
                           /*host_cached=*/true))
      {
        /* Sentinel distinguishes "copy executed" from "submission never ran". */
        uint32_t *sentinel = static_cast<uint32_t *>(snapshot.mapped);
        sentinel[0] = sentinel[1] = sentinel[2] = 0xDEADBEEFu;
        VkBufferCopy copy_region = {};
        copy_region.srcOffset = 0;
        copy_region.dstOffset = 0;
        copy_region.size = sizeof(uint32_t) * 3;
        vkCmdCopyBuffer(submission->command_buffer,
                        args.indirect_buffer,
                        snapshot.buffer,
                        1,
                        &copy_region);
        submission->args_snapshots.push_back(snapshot);
        submission->args_snapshot_kernels.push_back(int(kernel));
      }
    }
    /* END DIAGNOSTIC */
    /* Nuru: per-kernel wedge-diagnostic checkpoint (BLENDER_VULKAN_CHECKPOINTS=1). Kernel names
     * are static literals, so they stay valid for a device-lost dump. */
    if (device.functions.vkCmdSetCheckpoint) {
      device.functions.vkCmdSetCheckpoint(submission->command_buffer, kernel_name(kernel));
    }
    vkCmdDispatchIndirect(submission->command_buffer, args.indirect_buffer, 0);
  }
  else {
    if (device.functions.vkCmdSetCheckpoint) {
      device.functions.vkCmdSetCheckpoint(submission->command_buffer, kernel_name(kernel));
    }
    vkCmdDispatch(submission->command_buffer,
                  uint32_t(std::max(args.group_count.x, 1)),
                  uint32_t(std::max(args.group_count.y, 1)),
                  uint32_t(std::max(args.group_count.z, 1)));
  }

  /* Barrier between consecutive dispatches in the same submission (shadow batches). */
  VkMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(submission->command_buffer,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       0,
                       1,
                       &barrier,
                       0,
                       nullptr,
                       0,
                       nullptr);
  return true;
}

/* Execute a kernel: either record into the scene's open shadow batch or do a one-shot
 * submission, mirroring the Metal `trace_command_buffer_for_shadow` flow. */
static bool dispatch_kernel(GPUHardwareRaytraceScene *scene,
                            const NuruKernel kernel,
                            const KernelArgs &args,
                            const bool allow_batch)
{
  VKContext *context = VKContext::get();
  if (context == nullptr) {
    return false;
  }
  std::scoped_lock lock(raytrace_mutex_get());
  /* TEMPORARY DIAGNOSTIC: allow forcing one submission per kernel to bisect a GPU hang.
   * Remove before completion. */
  const bool batch_disabled = env_flag_enabled("BLENDER_EEVEE_HWRT_NO_BATCH");
  /* END DIAGNOSTIC */
  const bool uses_batch = !batch_disabled && allow_batch && scene != nullptr &&
                          scene->shadow_batch_submission != nullptr;
  /* Submit all pending render-graph work first so input resources are produced and image
   * layouts settle before our externally recorded transitions. The graph submits from a
   * background runner thread, so additionally wait until everything enqueued so far has reached
   * vkQueueSubmit; otherwise our direct submit below can reach the queue first and e.g. the
   * visibility clears land on top of the trace results. Metal commits in CPU order; this
   * restores the same ordering contract.
   *
   * Batched dispatches skip this: `shadow_batch_begin` already flushed and the batch records
   * tracker state transitions that only become real at `shadow_batch_end`. Flushing here would
   * let graph submissions get built from tracker state describing not-yet-executed batch
   * transitions, executing before the batch with mismatching layouts. */
  if (!uses_batch) {
    const TimelineValue pending_timeline = context->flush_render_graph(
        RenderGraphFlushFlags::SUBMIT | RenderGraphFlushFlags::RENEW_RENDER_GRAPH);
    VKBackend::get().device.wait_for_submission_timeline(pending_timeline);
  }

  NuruVKSubmission *submission = uses_batch ? scene->shadow_batch_submission :
                                              submission_begin();
  if (submission == nullptr) {
    return false;
  }

  if (!record_kernel_dispatch(submission, kernel, args)) {
    if (!uses_batch) {
      /* Abandon the partially recorded one-shot submission. */
      vkEndCommandBuffer(submission->command_buffer);
      submission_destroy(submission);
    }
    return false;
  }

  if (uses_batch) {
    scene->shadow_batch_has_work = true;
    return true;
  }
  return submission_end(submission, /*wait=*/false);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public API
 * \{ */

GPUHardwareRaytraceScene *raytrace_scene_build(Span<GPUHardwareRaytraceSceneEntry> entries,
                                               GPUHardwareRaytraceSceneStats *r_stats)
{
  if (r_stats != nullptr) {
    *r_stats = {};
  }

  if (!GPU_hardware_raytracing_support()) {
    return nullptr;
  }
  VKContext *context = VKContext::get();
  if (context == nullptr) {
    return nullptr;
  }

  const bool perf_logging_enabled = vulkan_raytrace_perf_logging_enabled();
  const double build_start_time = perf_logging_enabled ? BLI_time_now_seconds() : 0.0;

  std::vector<SceneGeometryBuild> built_geometry;
  built_geometry.reserve(entries.size());

  /* Read back geometry first (this synchronizes with the render graph internally), then batch
   * all BLAS builds into one submission. */
  AccelerationStructureBuildBatch blas_build_batch;
  if (!begin_acceleration_structure_build_batch(blas_build_batch)) {
    return nullptr;
  }

  GPUHardwareRaytraceScene *scene = new GPUHardwareRaytraceScene();
  for (const GPUHardwareRaytraceSceneEntry &entry : entries) {
    SceneGeometryBuild geometry;
    const bool built = build_entry_blas(entry, geometry, blas_build_batch);
    if (!built) {
      /* Keep a 1:1 placeholder slot. `raytrace_scene_update()` requires per-entry arrays so
       * object transforms can refresh the TLAS instead of triggering a full scene rebuild
       * every frame (interactive transforms were ~400 ms/frame without this). Null AS slots
       * are skipped by the TLAS instance fill. */
      geometry = {};
      geometry.user_id = entry.user_id;
      geometry.instance_count = 0;
    }

    scene->bottom_level_acceleration_structures.push_back(geometry.acceleration_structure);
    scene->bottom_level_buffers.push_back(geometry.acceleration_buffer);
    scene->local_triangle_normals.push_back(geometry.triangle_normals);
    scene->local_triangle_smooth_normals.push_back(geometry.triangle_smooth_normals);
    scene->local_triangle_positions.push_back(geometry.triangle_local_positions);
    scene->geometry_buffers.push_back(geometry.vertex_buffer);
    scene->geometry_buffers.push_back(geometry.index_buffer);
    if (built) {
      scene->geometry_count++;
      scene->instance_count += int(geometry.instance_count);
    }
    built_geometry.push_back(std::move(geometry));
  }
  if (!commit_acceleration_structure_build_batch(blas_build_batch)) {
    delete scene;
    return nullptr;
  }

  if (r_stats != nullptr) {
    r_stats->geometry_count = scene->geometry_count;
    r_stats->instance_count = scene->instance_count;
    /* Placeholder slots carry a null AS; report only really built ones. */
    r_stats->built_blas_count = scene->geometry_count;
  }

  if (scene->bottom_level_acceleration_structures.empty() || scene->instance_count == 0) {
    delete scene;
    return nullptr;
  }

  if (!build_top_level_acceleration_structure(
          built_geometry, scene->top_level_acceleration_structure, scene->top_level_buffer))
  {
    delete scene;
    return nullptr;
  }

  std::vector<TriangleNormalRangeRecord> triangle_normal_ranges;
  if (!build_emissive_radiance_buffer(built_geometry, scene->emissive_radiance_buffer) ||
      !build_emissive_light_buffer(
          built_geometry, scene->emissive_light_buffer, scene->emissive_light_count) ||
      !build_diffuse_albedo_buffer(built_geometry, scene->diffuse_albedo_buffer) ||
      !build_material_proxy_buffer(built_geometry, scene->material_proxy_buffer) ||
      !build_triangle_normal_buffer(
          built_geometry, scene->triangle_normal_buffer, triangle_normal_ranges) ||
      !build_triangle_smooth_normal_buffer(built_geometry, scene->triangle_smooth_normal_buffer) ||
      !build_triangle_local_position_buffer(built_geometry,
                                            scene->triangle_local_position_buffer) ||
      !build_triangle_normal_range_buffer(triangle_normal_ranges,
                                          scene->triangle_normal_range_buffer))
  {
    delete scene;
    return nullptr;
  }

  if (r_stats != nullptr) {
    r_stats->emissive_light_count = scene->emissive_light_count;
    r_stats->emissive_energy_sum = scene_emissive_energy_sum(built_geometry);
    r_stats->built_scene = true;
  }
  if (perf_logging_enabled) {
    const double elapsed_ms = (BLI_time_now_seconds() - build_start_time) * 1000.0;
    std::fprintf(stderr,
                 "EEVEE HWRT perf rt_scene_build geometries=%d instances=%d built_blas=%d "
                 "emissive_lights=%d elapsed_ms=%.2f\n",
                 scene->geometry_count,
                 scene->instance_count,
                 int(scene->bottom_level_acceleration_structures.size()),
                 scene->emissive_light_count,
                 elapsed_ms);
  }
  return scene;
}

bool raytrace_scene_update(GPUHardwareRaytraceScene *scene,
                           Span<GPUHardwareRaytraceSceneEntry> entries,
                           const GPUHardwareRaytraceSceneUpdateParams &update_params,
                           GPUHardwareRaytraceSceneStats *r_stats)
{
  if (r_stats != nullptr) {
    *r_stats = {};
  }
  if (scene == nullptr || scene->bottom_level_acceleration_structures.size() != entries.size() ||
      scene->local_triangle_normals.size() != entries.size() ||
      scene->local_triangle_smooth_normals.size() != entries.size() ||
      scene->local_triangle_positions.size() != entries.size())
  {
    return false;
  }

  if (!GPU_hardware_raytracing_support()) {
    return false;
  }
  VKContext *context = VKContext::get();
  if (context == nullptr) {
    return false;
  }

  const bool perf_logging_enabled = vulkan_raytrace_perf_logging_enabled();
  const double update_start_time = perf_logging_enabled ? BLI_time_now_seconds() : 0.0;

  if (!update_params.update_tlas && !update_params.update_emissive_data &&
      !update_params.update_material_data && !update_params.update_world_geometry_data &&
      update_params.rebuild_blas_indices.is_empty())
  {
    if (r_stats != nullptr) {
      r_stats->geometry_count = scene->geometry_count;
      r_stats->instance_count = scene->instance_count;
      r_stats->built_blas_count = 0;
      r_stats->emissive_light_count = scene->emissive_light_count;
      r_stats->built_scene = true;
    }
    return true;
  }

  /* Selective BLAS rebuild: build replacement BLAS for the flagged entries first; old GPU
   * objects are only destroyed in the success-path swap below so a failed build leaves the
   * scene untouched. */
  std::vector<std::pair<int, SceneGeometryBuild>> rebuilt_blas;
  if (!update_params.rebuild_blas_indices.is_empty()) {
    AccelerationStructureBuildBatch blas_build_batch;
    if (!begin_acceleration_structure_build_batch(blas_build_batch)) {
      return false;
    }
    for (const int index : update_params.rebuild_blas_indices) {
      if (index < 0 || index >= entries.size()) {
        continue;
      }
      SceneGeometryBuild geometry;
      if (!build_entry_blas(entries[index], geometry, blas_build_batch)) {
        /* Keep a placeholder slot like `raytrace_scene_build` does. */
        geometry = {};
        geometry.user_id = uint32_t(index);
        geometry.instance_count = 0;
      }
      rebuilt_blas.emplace_back(index, std::move(geometry));
    }
    if (!commit_acceleration_structure_build_batch(blas_build_batch)) {
      for (auto &pair : rebuilt_blas) {
        destroy_acceleration_structure(pair.second.acceleration_structure,
                                       pair.second.acceleration_buffer);
        rt_buffer_free(pair.second.vertex_buffer);
        rt_buffer_free(pair.second.index_buffer);
      }
      return false;
    }
  }
  auto rebuilt_blas_for_index = [&rebuilt_blas](const int index) -> SceneGeometryBuild * {
    for (auto &pair : rebuilt_blas) {
      if (pair.first == index) {
        return &pair.second;
      }
    }
    return nullptr;
  };

  std::vector<SceneGeometryBuild> updated_geometry;
  updated_geometry.reserve(entries.size());
  int instance_count = 0;
  for (const int i : entries.index_range()) {
    const GPUHardwareRaytraceSceneEntry &entry = entries[i];
    const SceneGeometryBuild *rebuilt = rebuilt_blas_for_index(i);
    SceneGeometryBuild geometry;
    geometry.acceleration_structure = rebuilt ? rebuilt->acceleration_structure :
                                                scene->bottom_level_acceleration_structures[i];
    geometry.object_to_world = entry.object_to_world;
    geometry.instance_count = std::max(entry.instance_count, uint32_t(1));
    geometry.user_id = uint32_t(i);
    geometry.emissive_radiance = entry.emissive_radiance;
    geometry.diffuse_albedo = entry.diffuse_albedo;
    geometry.reflection_color = entry.reflection_color;
    geometry.reflection_roughness = entry.reflection_roughness;
    geometry.transmission_color = entry.transmission_color;
    geometry.transmission_roughness = entry.transmission_roughness;
    geometry.reflection_ior = entry.reflection_ior;
    geometry.refraction_ior = entry.refraction_ior;
    geometry.packed_thickness = entry.packed_thickness;
    geometry.alpha = entry.alpha;
    geometry.reflection_layer_coverage = entry.reflection_layer_coverage;
    geometry.closure_type = entry.closure_type;
    geometry.proxy_flags = entry.proxy_flags;
    geometry.triangle_normals = rebuilt ? rebuilt->triangle_normals :
                                          scene->local_triangle_normals[i];
    geometry.triangle_smooth_normals = rebuilt ? rebuilt->triangle_smooth_normals :
                                                 scene->local_triangle_smooth_normals[i];
    geometry.triangle_local_positions = rebuilt ? rebuilt->triangle_local_positions :
                                                  scene->local_triangle_positions[i];
    updated_geometry.push_back(std::move(geometry));
    instance_count += int(updated_geometry.back().instance_count);
  }

  if (updated_geometry.empty() || instance_count == 0) {
    return false;
  }

  VkAccelerationStructureKHR new_tlas = VK_NULL_HANDLE;
  RTBuffer new_tlas_buffer;
  if (update_params.update_tlas) {
    if (!build_top_level_acceleration_structure(updated_geometry, new_tlas, new_tlas_buffer)) {
      return false;
    }
  }

  int new_emissive_light_count = scene->emissive_light_count;
  RTBuffer new_emissive;
  RTBuffer new_emissive_lights;
  RTBuffer new_diffuse;
  RTBuffer new_proxy;
  RTBuffer new_triangle_normals;
  RTBuffer new_triangle_smooth_normals;
  RTBuffer new_triangle_local_positions;
  RTBuffer new_triangle_normal_ranges;
  auto cleanup_news = [&]() {
    destroy_acceleration_structure(new_tlas, new_tlas_buffer);
    rt_buffer_free(new_emissive);
    rt_buffer_free(new_emissive_lights);
    rt_buffer_free(new_diffuse);
    rt_buffer_free(new_proxy);
    rt_buffer_free(new_triangle_normals);
    rt_buffer_free(new_triangle_smooth_normals);
    rt_buffer_free(new_triangle_local_positions);
    rt_buffer_free(new_triangle_normal_ranges);
    for (auto &pair : rebuilt_blas) {
      destroy_acceleration_structure(pair.second.acceleration_structure,
                                     pair.second.acceleration_buffer);
      rt_buffer_free(pair.second.vertex_buffer);
      rt_buffer_free(pair.second.index_buffer);
    }
  };

  if (update_params.update_emissive_data) {
    if (!build_emissive_radiance_buffer(updated_geometry, new_emissive) ||
        !build_emissive_light_buffer(
            updated_geometry, new_emissive_lights, new_emissive_light_count))
    {
      cleanup_news();
      return false;
    }
  }
  if (update_params.update_material_data) {
    if (!build_diffuse_albedo_buffer(updated_geometry, new_diffuse) ||
        !build_material_proxy_buffer(updated_geometry, new_proxy))
    {
      cleanup_news();
      return false;
    }
  }
  if (update_params.update_world_geometry_data) {
    std::vector<TriangleNormalRangeRecord> triangle_normal_ranges;
    if (!build_triangle_normal_buffer(
            updated_geometry, new_triangle_normals, triangle_normal_ranges) ||
        !build_triangle_smooth_normal_buffer(updated_geometry, new_triangle_smooth_normals))
    {
      cleanup_news();
      return false;
    }
    if (!rebuilt_blas.empty()) {
      /* Rebuilt BLAS can change per-entry triangle counts, shifting the packed range table and
       * the local position stream; refresh both alongside the normals. */
      if (!build_triangle_local_position_buffer(updated_geometry, new_triangle_local_positions) ||
          !build_triangle_normal_range_buffer(triangle_normal_ranges, new_triangle_normal_ranges))
      {
        cleanup_news();
        return false;
      }
    }
  }

  /* Old GPU objects may still be referenced by in-flight submissions; drain them first. */
  collect_finished_submissions(true);

  if (!rebuilt_blas.empty()) {
    for (auto &pair : rebuilt_blas) {
      const int i = pair.first;
      SceneGeometryBuild &geometry = pair.second;
      destroy_acceleration_structure(scene->bottom_level_acceleration_structures[i],
                                     scene->bottom_level_buffers[i]);
      rt_buffer_free(scene->geometry_buffers[2 * i]);
      rt_buffer_free(scene->geometry_buffers[2 * i + 1]);
      scene->bottom_level_acceleration_structures[i] = geometry.acceleration_structure;
      scene->bottom_level_buffers[i] = geometry.acceleration_buffer;
      scene->geometry_buffers[2 * i] = geometry.vertex_buffer;
      scene->geometry_buffers[2 * i + 1] = geometry.index_buffer;
      scene->local_triangle_normals[i] = std::move(geometry.triangle_normals);
      scene->local_triangle_smooth_normals[i] = std::move(geometry.triangle_smooth_normals);
      scene->local_triangle_positions[i] = std::move(geometry.triangle_local_positions);
    }
    /* Ownership transferred into the scene; do not free in any later cleanup. */
    rebuilt_blas.clear();
    int built_count = 0;
    for (const VkAccelerationStructureKHR blas : scene->bottom_level_acceleration_structures) {
      built_count += (blas != VK_NULL_HANDLE) ? 1 : 0;
    }
    scene->geometry_count = built_count;
  }

  if (update_params.update_tlas) {
    destroy_acceleration_structure(scene->top_level_acceleration_structure,
                                   scene->top_level_buffer);
    scene->top_level_acceleration_structure = new_tlas;
    scene->top_level_buffer = new_tlas_buffer;
  }
  if (update_params.update_emissive_data) {
    rt_buffer_free(scene->emissive_radiance_buffer);
    rt_buffer_free(scene->emissive_light_buffer);
    scene->emissive_radiance_buffer = new_emissive;
    scene->emissive_light_buffer = new_emissive_lights;
    scene->emissive_light_count = new_emissive_light_count;
  }
  if (update_params.update_material_data) {
    rt_buffer_free(scene->diffuse_albedo_buffer);
    rt_buffer_free(scene->material_proxy_buffer);
    scene->diffuse_albedo_buffer = new_diffuse;
    scene->material_proxy_buffer = new_proxy;
  }
  if (update_params.update_world_geometry_data) {
    rt_buffer_free(scene->triangle_normal_buffer);
    rt_buffer_free(scene->triangle_smooth_normal_buffer);
    scene->triangle_normal_buffer = new_triangle_normals;
    scene->triangle_smooth_normal_buffer = new_triangle_smooth_normals;
    if (new_triangle_local_positions.buffer != VK_NULL_HANDLE) {
      rt_buffer_free(scene->triangle_local_position_buffer);
      scene->triangle_local_position_buffer = new_triangle_local_positions;
    }
    if (new_triangle_normal_ranges.buffer != VK_NULL_HANDLE) {
      rt_buffer_free(scene->triangle_normal_range_buffer);
      scene->triangle_normal_range_buffer = new_triangle_normal_ranges;
    }
  }
  scene->geometry_count = int(updated_geometry.size());
  scene->instance_count = instance_count;

  if (r_stats != nullptr) {
    r_stats->geometry_count = scene->geometry_count;
    r_stats->instance_count = scene->instance_count;
    r_stats->built_blas_count = 0;
    r_stats->emissive_light_count = scene->emissive_light_count;
    r_stats->emissive_energy_sum = scene_emissive_energy_sum(updated_geometry);
    r_stats->built_scene = true;
  }
  if (perf_logging_enabled) {
    const double elapsed_ms = (BLI_time_now_seconds() - update_start_time) * 1000.0;
    std::fprintf(stderr,
                 "EEVEE HWRT perf rt_scene_update tlas=%d emissive=%d material=%d world_geom=%d "
                 "geometries=%d instances=%d elapsed_ms=%.2f\n",
                 update_params.update_tlas ? 1 : 0,
                 update_params.update_emissive_data ? 1 : 0,
                 update_params.update_material_data ? 1 : 0,
                 update_params.update_world_geometry_data ? 1 : 0,
                 scene->geometry_count,
                 scene->instance_count,
                 elapsed_ms);
  }
  return true;
}

/* Helpers for argument marshaling. */
static VKTexture *texture_cast(gpu::Texture *texture)
{
  return static_cast<VKTexture *>(texture);
}

static bool storage_buffer_binding(BufferBinding &r_binding,
                                   const uint32_t binding,
                                   gpu::StorageBuf *storage_buf)
{
  if (storage_buf == nullptr) {
    return false;
  }
  VKStorageBuffer *vk_buf = static_cast<VKStorageBuffer *>(storage_buf);
  /* Ensure device-side allocation exists. */
  vk_buf->ensure_allocated();
  r_binding.binding = binding;
  r_binding.buffer = vk_buf->vk_handle();
  return r_binding.buffer != VK_NULL_HANDLE;
}

static BufferBinding rt_buffer_binding(const uint32_t binding, const RTBuffer &buffer)
{
  BufferBinding result;
  result.binding = binding;
  result.buffer = buffer.buffer;
  return result;
}

static ImageBinding sampled_binding(const uint32_t binding,
                                    gpu::Texture *texture,
                                    const bool arrayed = false,
                                    const bool linear = false)
{
  ImageBinding result;
  result.binding = binding;
  result.texture = texture_cast(texture);
  result.sampled = true;
  result.linear_sampler = linear;
  result.arrayed = arrayed;
  return result;
}

static ImageBinding storage_binding(const uint32_t binding,
                                    gpu::Texture *texture,
                                    const bool arrayed = false)
{
  ImageBinding result;
  result.binding = binding;
  result.texture = texture_cast(texture);
  result.sampled = false;
  result.arrayed = arrayed;
  return result;
}

static ImageBinding dummy_sampled_binding(const uint32_t binding)
{
  ImageBinding result;
  result.binding = binding;
  result.raw_view = raytrace_dummy_array_view_get();
  result.sampled = true;
  result.linear_sampler = true;
  return result;
}

bool raytrace_scene_trace(GPUHardwareRaytraceScene *scene,
                          const GPUHardwareRaytraceTraceParams &params)
{
  if (scene == nullptr || scene->top_level_acceleration_structure == VK_NULL_HANDLE ||
      params.ray_data_tx == nullptr || params.depth_tx == nullptr ||
      params.gbuf_header_tx == nullptr || params.gbuf_normal_tx == nullptr ||
      params.screen_continuation_tx == nullptr || params.ray_time_tx == nullptr ||
      params.ray_radiance_tx == nullptr || params.hit_albedo_tx == nullptr ||
      params.hit_throughput_tx == nullptr || params.hit_material_tx == nullptr ||
      params.hit_normal_tx == nullptr || params.hit_position_tx == nullptr ||
      params.hit_world_position_tx == nullptr || params.hit_identity_tx == nullptr ||
      params.hit_barycentric_tx == nullptr || params.layered_receiver_ray_time_tx == nullptr ||
      params.layered_receiver_ray_radiance_tx == nullptr ||
      params.layered_receiver_albedo_tx == nullptr ||
      params.layered_receiver_throughput_tx == nullptr ||
      params.layered_receiver_material_tx == nullptr ||
      params.layered_receiver_normal_tx == nullptr ||
      params.layered_receiver_position_tx == nullptr ||
      params.layered_receiver_world_position_tx == nullptr ||
      params.layered_receiver_identity_tx == nullptr ||
      params.layered_receiver_barycentric_tx == nullptr ||
      params.transmission_receiver_ray_time_tx == nullptr ||
      params.transmission_receiver_ray_radiance_tx == nullptr ||
      params.transmission_receiver_albedo_tx == nullptr ||
      params.transmission_receiver_throughput_tx == nullptr ||
      params.transmission_receiver_material_tx == nullptr ||
      params.transmission_receiver_normal_tx == nullptr ||
      params.transmission_receiver_position_tx == nullptr ||
      params.transmission_receiver_world_position_tx == nullptr ||
      params.transmission_receiver_identity_tx == nullptr ||
      params.transmission_receiver_barycentric_tx == nullptr || params.dispatch_buf == nullptr ||
      params.tiles_coord_buf == nullptr || params.hit_emission_replay_tx == nullptr ||
      params.layered_receiver_emission_replay_tx == nullptr ||
      params.transmission_receiver_emission_replay_tx == nullptr)
  {
    return false;
  }

  if (!GPU_hardware_raytracing_support()) {
    return false;
  }

  HardwareTraceUniforms uniforms = {};
  uniforms.viewinv = params.viewinv;
  uniforms.wininv = params.wininv;
  uniforms.full_resolution = params.full_resolution;
  uniforms.resolution_scale = std::max(params.resolution_scale, 1);
  uniforms.resolution_scale_denominator = std::max(params.resolution_scale_denominator, 1);
  uniforms.closure_index = std::max(params.closure_index, 0);
  uniforms.feature_mask = params.feature_mask;
  uniforms.hardware_trace_phase = params.hardware_trace_phase;
  uniforms.reflection_bounces = std::clamp(
      params.reflection_bounces, 1, GPU_HARDWARE_RAYTRACE_SPECULAR_MAX_BOUNCES);
  uniforms.refraction_bounces = std::clamp(
      params.refraction_bounces, 1, GPU_HARDWARE_RAYTRACE_SPECULAR_MAX_BOUNCES);
  uniforms._pad0 = 0;
  uniforms.resolution_bias = params.resolution_bias;
  uniforms.clamp_indirect = std::max(params.clamp_indirect, 0.0f);
  uniforms.world_probe_atlas_coord = params.world_probe_atlas_coord;
  uniforms.use_environment_pad = int4(
      (params.use_environment && params.world_probe_tx != nullptr) ? 1 : 0,
      std::max(scene->emissive_light_count, 0),
      std::max(params.gi_diffuse_sample_count, 1),
      (params.use_diffuse_environment && params.world_probe_tx != nullptr) ? 1 : 0);
  uniforms.light_count_pad = int4(
      std::max(params.light_count, 0), std::max(params.light_sample_count, 0), 0, 0);
  uniforms.sampling_rand = params.sampling_rand;

  KernelArgs args;
  args.uniforms_data = &uniforms;
  args.uniforms_size = sizeof(uniforms);
  args.tlas = scene->top_level_acceleration_structure;

  args.ssbos.push_back(rt_buffer_binding(2, scene->emissive_radiance_buffer));
  args.ssbos.push_back(rt_buffer_binding(3, scene->diffuse_albedo_buffer));
  args.ssbos.push_back(rt_buffer_binding(4, scene->material_proxy_buffer));
  args.ssbos.push_back(rt_buffer_binding(5, scene->triangle_normal_buffer));
  args.ssbos.push_back(rt_buffer_binding(6, scene->triangle_normal_range_buffer));
  BufferBinding tiles_binding;
  if (!storage_buffer_binding(tiles_binding, 7, params.tiles_coord_buf)) {
    return false;
  }
  args.ssbos.push_back(tiles_binding);
  args.ssbos.push_back(rt_buffer_binding(8, scene->triangle_smooth_normal_buffer));
  args.ssbos.push_back(rt_buffer_binding(9, scene->triangle_local_position_buffer));
  args.ssbos.push_back(rt_buffer_binding(10, scene->emissive_light_buffer));
  BufferBinding light_binding;
  if (params.light_buf != nullptr) {
    if (!storage_buffer_binding(light_binding, 11, params.light_buf)) {
      return false;
    }
  }
  else {
    /* The kernel requires a bound buffer; reuse the emissive light buffer as inert filler when
     * the caller has no light list (light_count is 0 in that case). */
    light_binding = rt_buffer_binding(11, scene->emissive_light_buffer);
  }
  args.ssbos.push_back(light_binding);

  args.images.push_back(sampled_binding(16 + 0, params.ray_data_tx));
  args.images.push_back(sampled_binding(16 + 1, params.depth_tx));
  args.images.push_back(sampled_binding(16 + 2, params.gbuf_header_tx, /*arrayed=*/true));
  args.images.push_back(sampled_binding(16 + 3, params.gbuf_normal_tx, /*arrayed=*/true));
  args.images.push_back(sampled_binding(16 + 4, params.screen_continuation_tx));
  if (params.world_probe_tx != nullptr) {
    args.images.push_back(
        sampled_binding(80, params.world_probe_tx, /*arrayed=*/true, /*linear=*/true));
  }
  else {
    /* The kernel declares sampler2DArray; bind the backend dummy as inert filler. The probe is
     * only sampled when the use_environment flags are set, which requires a real probe. */
    args.images.push_back(dummy_sampled_binding(80));
  }

  args.images.push_back(storage_binding(40 + 5, params.ray_time_tx));
  args.images.push_back(storage_binding(40 + 6, params.ray_radiance_tx));
  args.images.push_back(storage_binding(40 + 7, params.hit_albedo_tx));
  args.images.push_back(storage_binding(40 + 8, params.hit_material_tx));
  args.images.push_back(storage_binding(40 + 9, params.hit_normal_tx));
  args.images.push_back(storage_binding(40 + 10, params.hit_position_tx));
  args.images.push_back(storage_binding(40 + 11, params.hit_identity_tx));
  args.images.push_back(storage_binding(40 + 12, params.hit_barycentric_tx));
  args.images.push_back(storage_binding(40 + 13, params.hit_world_position_tx));
  args.images.push_back(storage_binding(40 + 14, params.hit_throughput_tx));
  args.images.push_back(storage_binding(40 + 15, params.layered_receiver_ray_time_tx));
  args.images.push_back(storage_binding(40 + 16, params.layered_receiver_ray_radiance_tx));
  args.images.push_back(storage_binding(40 + 17, params.layered_receiver_albedo_tx));
  args.images.push_back(storage_binding(40 + 18, params.layered_receiver_material_tx));
  args.images.push_back(storage_binding(40 + 19, params.layered_receiver_normal_tx));
  args.images.push_back(storage_binding(40 + 20, params.layered_receiver_position_tx));
  args.images.push_back(storage_binding(40 + 21, params.layered_receiver_identity_tx));
  args.images.push_back(storage_binding(40 + 22, params.layered_receiver_barycentric_tx));
  args.images.push_back(storage_binding(40 + 23, params.layered_receiver_world_position_tx));
  args.images.push_back(storage_binding(40 + 24, params.layered_receiver_throughput_tx));
  args.images.push_back(storage_binding(40 + 25, params.transmission_receiver_ray_time_tx));
  args.images.push_back(storage_binding(40 + 26, params.transmission_receiver_ray_radiance_tx));
  args.images.push_back(storage_binding(40 + 27, params.transmission_receiver_albedo_tx));
  args.images.push_back(storage_binding(40 + 28, params.transmission_receiver_material_tx));
  args.images.push_back(storage_binding(40 + 29, params.transmission_receiver_normal_tx));
  args.images.push_back(storage_binding(40 + 30, params.transmission_receiver_position_tx));
  args.images.push_back(storage_binding(40 + 31, params.transmission_receiver_identity_tx));
  args.images.push_back(storage_binding(40 + 32, params.transmission_receiver_barycentric_tx));
  args.images.push_back(
      storage_binding(40 + 33, params.transmission_receiver_world_position_tx));
  args.images.push_back(storage_binding(40 + 34, params.transmission_receiver_throughput_tx));
  args.images.push_back(storage_binding(75, params.hit_emission_replay_tx));
  args.images.push_back(storage_binding(76, params.layered_receiver_emission_replay_tx));
  args.images.push_back(storage_binding(77, params.transmission_receiver_emission_replay_tx));

  VKStorageBuffer *dispatch_ssbo = static_cast<VKStorageBuffer *>(params.dispatch_buf);
  dispatch_ssbo->ensure_allocated();
  args.indirect_buffer = dispatch_ssbo->vk_handle();
  if (args.indirect_buffer == VK_NULL_HANDLE) {
    return false;
  }

  return dispatch_kernel(scene, NuruKernel::TRACE, args, /*allow_batch=*/false);
}

bool raytrace_scene_shadow_batch_begin(GPUHardwareRaytraceScene *scene)
{
  if (scene == nullptr) {
    return false;
  }
  if (!GPU_hardware_raytracing_support()) {
    return false;
  }
  VKContext *context = VKContext::get();
  if (context == nullptr) {
    return false;
  }
  std::scoped_lock lock(raytrace_mutex_get());
  if (scene->shadow_batch_submission != nullptr) {
    return true;
  }
  /* Single flush for the whole batch: all graph work recorded before the batch (clears,
   * compacts) must be on the queue before the batch submission; see dispatch_kernel(). */
  const TimelineValue pending_timeline = context->flush_render_graph(
      RenderGraphFlushFlags::SUBMIT | RenderGraphFlushFlags::RENEW_RENDER_GRAPH);
  VKBackend::get().device.wait_for_submission_timeline(pending_timeline);
  scene->shadow_batch_submission = submission_begin();
  scene->shadow_batch_has_work = false;
  return scene->shadow_batch_submission != nullptr;
}

bool raytrace_scene_shadow_batch_end(GPUHardwareRaytraceScene *scene)
{
  if (scene == nullptr) {
    return false;
  }
  std::scoped_lock lock(raytrace_mutex_get());
  if (scene->shadow_batch_submission == nullptr) {
    return true;
  }
  NuruVKSubmission *submission = scene->shadow_batch_submission;
  scene->shadow_batch_submission = nullptr;
  scene->shadow_batch_has_work = false;
  return submission_end(submission, /*wait=*/false);
}

bool raytrace_scene_trace_directional_shadow(
    GPUHardwareRaytraceScene *scene, const GPUHardwareRaytraceDirectionalShadowParams &params)
{
  if (scene == nullptr || scene->top_level_acceleration_structure == VK_NULL_HANDLE ||
      params.depth_tx == nullptr || params.gbuf_header_tx == nullptr ||
      params.gbuf_normal_tx == nullptr || params.shadow_visibility_tx == nullptr)
  {
    return false;
  }

  if (!GPU_hardware_raytracing_support()) {
    return false;
  }

  HardwareShadowUniforms uniforms = {};
  uniforms.viewinv = params.viewinv;
  uniforms.wininv = params.wininv;
  uniforms.resolution_layer = int4(
      params.full_resolution.x, params.full_resolution.y, std::max(params.shadow_layer, 0), 0);
  uniforms.light_direction_bias = float4(params.light_direction,
                                         std::max(params.normal_bias, 0.0f));
  /* Nuru: `shadow_params.z` = Color Intensity (0..1), `.w` = Photons intensity (0..10). */
  uniforms.shadow_params = float4(std::max(params.shadow_angle, 0.0f),
                                  float(std::max(params.sample_count, 1)),
                                  std::clamp(params.color_intensity, 0.0f, 1.0f),
                                  std::max(params.photons_intensity, 0.0f));
  /* Nuru: `.y` = caustic toggle, `.w` = transparent-shadow blend (float bits). */
  uniforms.world_sun_slot_pad = int4(
      params.world_sun_slot,
      params.use_caustics ? 1 : 0,
      0,
      gpu_vulkan_shadow_transparency_bits(params.shadow_transparency));
  uniforms.sampling_rand = params.sampling_rand;

  KernelArgs args;
  args.uniforms_data = &uniforms;
  args.uniforms_size = sizeof(uniforms);
  args.tlas = scene->top_level_acceleration_structure;

  BufferBinding sunlight_binding;
  if (params.world_sunlight_direction_buf != nullptr) {
    if (!storage_buffer_binding(sunlight_binding, 2, params.world_sunlight_direction_buf)) {
      return false;
    }
  }
  else {
    if (params.world_sun_slot >= 0) {
      return false;
    }
    sunlight_binding = rt_buffer_binding(2, scene->emissive_radiance_buffer);
  }
  args.ssbos.push_back(sunlight_binding);
  args.ssbos.push_back(rt_buffer_binding(3, scene->material_proxy_buffer));
  args.ssbos.push_back(rt_buffer_binding(4, scene->triangle_normal_buffer));
  args.ssbos.push_back(rt_buffer_binding(5, scene->triangle_smooth_normal_buffer));
  args.ssbos.push_back(rt_buffer_binding(6, scene->triangle_normal_range_buffer));

  args.images.push_back(sampled_binding(16 + 0, params.depth_tx));
  args.images.push_back(sampled_binding(16 + 1, params.gbuf_header_tx, /*arrayed=*/true));
  args.images.push_back(sampled_binding(16 + 2, params.gbuf_normal_tx, /*arrayed=*/true));
  args.images.push_back(storage_binding(40 + 3, params.shadow_visibility_tx, /*arrayed=*/true));

  args.group_count = int3((std::max(params.full_resolution.x, 1) + 7) / 8,
                          (std::max(params.full_resolution.y, 1) + 7) / 8,
                          1);

  return dispatch_kernel(scene, NuruKernel::DIRECTIONAL_SHADOW, args, /*allow_batch=*/true);
}

bool raytrace_scene_trace_directional_hit_shadow(
    GPUHardwareRaytraceScene *scene, const GPUHardwareRaytraceDirectionalHitShadowParams &params)
{
  if (scene == nullptr || scene->top_level_acceleration_structure == VK_NULL_HANDLE ||
      params.hit_normal_tx == nullptr || params.hit_world_position_tx == nullptr ||
      params.hit_identity_tx == nullptr || params.shadow_visibility_tx == nullptr ||
      params.dispatch_buf == nullptr || params.tiles_coord_buf == nullptr)
  {
    return false;
  }

  if (!GPU_hardware_raytracing_support()) {
    return false;
  }

  HardwareShadowUniforms uniforms = {};
  uniforms.resolution_layer = int4(params.tracing_resolution.x,
                                   params.tracing_resolution.y,
                                   std::max(params.shadow_layer, 0),
                                   0);
  uniforms.light_direction_bias = float4(params.light_direction,
                                         std::max(params.normal_bias, 0.0f));
  uniforms.shadow_params = float4(std::max(params.shadow_angle, 0.0f),
                                  float(std::max(params.sample_count, 1)),
                                  std::clamp(params.color_intensity, 0.0f, 1.0f),
                                  std::max(params.photons_intensity, 0.0f));
  uniforms.world_sun_slot_pad = int4(
      params.world_sun_slot,
      params.use_caustics ? 1 : 0,
      0,
      gpu_vulkan_shadow_transparency_bits(params.shadow_transparency));
  uniforms.sampling_rand = params.sampling_rand;

  KernelArgs args;
  args.uniforms_data = &uniforms;
  args.uniforms_size = sizeof(uniforms);
  args.tlas = scene->top_level_acceleration_structure;

  BufferBinding sunlight_binding;
  if (params.world_sunlight_direction_buf != nullptr) {
    if (!storage_buffer_binding(sunlight_binding, 2, params.world_sunlight_direction_buf)) {
      return false;
    }
  }
  else {
    if (params.world_sun_slot >= 0) {
      return false;
    }
    sunlight_binding = rt_buffer_binding(2, scene->emissive_radiance_buffer);
  }
  args.ssbos.push_back(sunlight_binding);
  BufferBinding tiles_binding;
  if (!storage_buffer_binding(tiles_binding, 3, params.tiles_coord_buf)) {
    return false;
  }
  args.ssbos.push_back(tiles_binding);
  args.ssbos.push_back(rt_buffer_binding(4, scene->triangle_normal_buffer));
  args.ssbos.push_back(rt_buffer_binding(5, scene->triangle_normal_range_buffer));
  args.ssbos.push_back(rt_buffer_binding(6, scene->material_proxy_buffer));
  args.ssbos.push_back(rt_buffer_binding(7, scene->triangle_smooth_normal_buffer));

  args.images.push_back(sampled_binding(16 + 0, params.hit_normal_tx));
  args.images.push_back(sampled_binding(16 + 1, params.hit_world_position_tx));
  args.images.push_back(sampled_binding(16 + 2, params.hit_identity_tx));
  args.images.push_back(storage_binding(40 + 3, params.shadow_visibility_tx, /*arrayed=*/true));

  VKStorageBuffer *dispatch_ssbo = static_cast<VKStorageBuffer *>(params.dispatch_buf);
  dispatch_ssbo->ensure_allocated();
  args.indirect_buffer = dispatch_ssbo->vk_handle();
  if (args.indirect_buffer == VK_NULL_HANDLE) {
    return false;
  }

  return dispatch_kernel(scene, NuruKernel::DIRECTIONAL_HIT_SHADOW, args, /*allow_batch=*/true);
}

bool raytrace_scene_trace_environment_visibility(
    GPUHardwareRaytraceScene *scene, const GPUHardwareRaytraceEnvironmentVisibilityParams &params)
{
  if (scene == nullptr || scene->top_level_acceleration_structure == VK_NULL_HANDLE ||
      params.depth_tx == nullptr || params.gbuf_header_tx == nullptr ||
      params.gbuf_normal_tx == nullptr || params.environment_visibility_tx == nullptr)
  {
    return false;
  }

  if (!GPU_hardware_raytracing_support()) {
    return false;
  }

  HardwareEnvironmentVisibilityUniforms uniforms = {};
  uniforms.viewinv = params.viewinv;
  uniforms.wininv = params.wininv;
  uniforms.resolution_samples = int4(
      params.full_resolution.x, params.full_resolution.y, std::max(params.sample_count, 1), 0);
  uniforms.normal_bias_pad = float4(
      std::max(params.normal_bias, 0.0f), float(std::max(params.sample_count, 1)), 0.0f, 0.0f);
  uniforms.sampling_rand = params.sampling_rand;

  KernelArgs args;
  args.uniforms_data = &uniforms;
  args.uniforms_size = sizeof(uniforms);
  args.tlas = scene->top_level_acceleration_structure;

  args.images.push_back(sampled_binding(16 + 0, params.depth_tx));
  args.images.push_back(sampled_binding(16 + 1, params.gbuf_header_tx, /*arrayed=*/true));
  args.images.push_back(sampled_binding(16 + 2, params.gbuf_normal_tx, /*arrayed=*/true));
  args.images.push_back(storage_binding(40 + 3, params.environment_visibility_tx));

  args.group_count = int3((std::max(params.full_resolution.x, 1) + 7) / 8,
                          (std::max(params.full_resolution.y, 1) + 7) / 8,
                          1);

  return dispatch_kernel(scene, NuruKernel::ENVIRONMENT_VISIBILITY, args, /*allow_batch=*/false);
}

bool raytrace_scene_trace_hit_environment_visibility(
    GPUHardwareRaytraceScene *scene,
    const GPUHardwareRaytraceHitEnvironmentVisibilityParams &params)
{
  if (scene == nullptr || scene->top_level_acceleration_structure == VK_NULL_HANDLE ||
      params.hit_normal_tx == nullptr || params.hit_world_position_tx == nullptr ||
      params.environment_visibility_tx == nullptr || params.dispatch_buf == nullptr ||
      params.tiles_coord_buf == nullptr)
  {
    return false;
  }

  if (!GPU_hardware_raytracing_support()) {
    return false;
  }

  HardwareEnvironmentVisibilityUniforms uniforms = {};
  uniforms.resolution_samples = int4(params.tracing_resolution.x,
                                     params.tracing_resolution.y,
                                     std::max(params.sample_count, 1),
                                     0);
  uniforms.normal_bias_pad = float4(
      std::max(params.normal_bias, 0.0f), float(std::max(params.sample_count, 1)), 0.0f, 0.0f);
  uniforms.sampling_rand = params.sampling_rand;

  KernelArgs args;
  args.uniforms_data = &uniforms;
  args.uniforms_size = sizeof(uniforms);
  args.tlas = scene->top_level_acceleration_structure;

  BufferBinding tiles_binding;
  if (!storage_buffer_binding(tiles_binding, 2, params.tiles_coord_buf)) {
    return false;
  }
  args.ssbos.push_back(tiles_binding);

  args.images.push_back(sampled_binding(16 + 0, params.hit_normal_tx));
  args.images.push_back(sampled_binding(16 + 1, params.hit_world_position_tx));
  args.images.push_back(storage_binding(40 + 2, params.environment_visibility_tx));

  VKStorageBuffer *dispatch_ssbo = static_cast<VKStorageBuffer *>(params.dispatch_buf);
  dispatch_ssbo->ensure_allocated();
  args.indirect_buffer = dispatch_ssbo->vk_handle();
  if (args.indirect_buffer == VK_NULL_HANDLE) {
    return false;
  }

  return dispatch_kernel(
      scene, NuruKernel::HIT_ENVIRONMENT_VISIBILITY, args, /*allow_batch=*/false);
}

bool raytrace_scene_trace_local_shadow(GPUHardwareRaytraceScene *scene,
                                       const GPUHardwareRaytraceLocalShadowParams &params)
{
  if (scene == nullptr || scene->top_level_acceleration_structure == VK_NULL_HANDLE ||
      params.depth_tx == nullptr || params.gbuf_header_tx == nullptr ||
      params.gbuf_normal_tx == nullptr || params.shadow_visibility_tx == nullptr)
  {
    return false;
  }

  if (!GPU_hardware_raytracing_support()) {
    return false;
  }

  HardwareLocalShadowUniforms uniforms = {};
  uniforms.viewinv = params.viewinv;
  uniforms.wininv = params.wininv;
  uniforms.resolution_layer_type = int4(params.full_resolution.x,
                                        params.full_resolution.y,
                                        std::max(params.shadow_layer, 0),
                                        int(params.light_type));
  uniforms.light_position_radius = float4(params.light_position,
                                          std::max(params.shadow_radius, 0.0f));
  uniforms.light_x_axis_size_x = float4(params.light_x_axis, std::max(params.area_size_x, 0.0f));
  uniforms.light_y_axis_size_y = float4(params.light_y_axis, std::max(params.area_size_y, 0.0f));
  uniforms.shadow_offset_scale = float4(params.shadow_offset,
                                        std::max(params.area_shadow_scale, 0.0f));
  /* Nuru: `normal_bias_pad.z` carries the caustic-shadow toggle (0/1), `.w` the Color
   * Intensity slider (0..1). `caustic_params.x` carries the Photons intensity (0..10). */
  uniforms.normal_bias_pad = float4(std::max(params.normal_bias, 0.0f),
                                    float(std::max(params.sample_count, 1)),
                                    params.use_caustics ? 1.0f : 0.0f,
                                    std::clamp(params.color_intensity, 0.0f, 1.0f));
  uniforms.sampling_rand = params.sampling_rand;
  uniforms.caustic_params = float4(std::max(params.photons_intensity, 0.0f),
                                   std::clamp(params.shadow_transparency, 0.0f, 1.0f),
                                   0.0f,
                                   0.0f);

  KernelArgs args;
  args.uniforms_data = &uniforms;
  args.uniforms_size = sizeof(uniforms);
  args.tlas = scene->top_level_acceleration_structure;

  args.ssbos.push_back(rt_buffer_binding(2, scene->material_proxy_buffer));
  args.ssbos.push_back(rt_buffer_binding(3, scene->triangle_normal_buffer));
  args.ssbos.push_back(rt_buffer_binding(4, scene->triangle_smooth_normal_buffer));
  args.ssbos.push_back(rt_buffer_binding(5, scene->triangle_normal_range_buffer));

  args.images.push_back(sampled_binding(16 + 0, params.depth_tx));
  args.images.push_back(sampled_binding(16 + 1, params.gbuf_header_tx, /*arrayed=*/true));
  args.images.push_back(sampled_binding(16 + 2, params.gbuf_normal_tx, /*arrayed=*/true));
  args.images.push_back(storage_binding(40 + 3, params.shadow_visibility_tx, /*arrayed=*/true));

  args.group_count = int3((std::max(params.full_resolution.x, 1) + 7) / 8,
                          (std::max(params.full_resolution.y, 1) + 7) / 8,
                          1);

  return dispatch_kernel(scene, NuruKernel::LOCAL_SHADOW, args, /*allow_batch=*/true);
}

bool raytrace_scene_trace_local_hit_shadow(GPUHardwareRaytraceScene *scene,
                                           const GPUHardwareRaytraceLocalHitShadowParams &params)
{
  if (scene == nullptr || scene->top_level_acceleration_structure == VK_NULL_HANDLE ||
      params.hit_normal_tx == nullptr || params.hit_world_position_tx == nullptr ||
      params.hit_identity_tx == nullptr || params.shadow_visibility_tx == nullptr ||
      params.dispatch_buf == nullptr || params.tiles_coord_buf == nullptr)
  {
    return false;
  }

  if (!GPU_hardware_raytracing_support()) {
    return false;
  }

  HardwareLocalShadowUniforms uniforms = {};
  uniforms.resolution_layer_type = int4(params.tracing_resolution.x,
                                        params.tracing_resolution.y,
                                        std::max(params.shadow_layer, 0),
                                        int(params.light_type));
  uniforms.light_position_radius = float4(params.light_position,
                                          std::max(params.shadow_radius, 0.0f));
  uniforms.light_x_axis_size_x = float4(params.light_x_axis, std::max(params.area_size_x, 0.0f));
  uniforms.light_y_axis_size_y = float4(params.light_y_axis, std::max(params.area_size_y, 0.0f));
  uniforms.shadow_offset_scale = float4(params.shadow_offset,
                                        std::max(params.area_shadow_scale, 0.0f));
  uniforms.normal_bias_pad = float4(std::max(params.normal_bias, 0.0f),
                                    float(std::max(params.sample_count, 1)),
                                    params.use_caustics ? 1.0f : 0.0f,
                                    std::clamp(params.color_intensity, 0.0f, 1.0f));
  uniforms.sampling_rand = params.sampling_rand;
  uniforms.caustic_params = float4(std::max(params.photons_intensity, 0.0f),
                                   std::clamp(params.shadow_transparency, 0.0f, 1.0f),
                                   0.0f,
                                   0.0f);

  KernelArgs args;
  args.uniforms_data = &uniforms;
  args.uniforms_size = sizeof(uniforms);
  args.tlas = scene->top_level_acceleration_structure;

  BufferBinding tiles_binding;
  if (!storage_buffer_binding(tiles_binding, 2, params.tiles_coord_buf)) {
    return false;
  }
  args.ssbos.push_back(tiles_binding);
  args.ssbos.push_back(rt_buffer_binding(3, scene->triangle_normal_buffer));
  args.ssbos.push_back(rt_buffer_binding(4, scene->triangle_normal_range_buffer));
  args.ssbos.push_back(rt_buffer_binding(5, scene->material_proxy_buffer));
  args.ssbos.push_back(rt_buffer_binding(6, scene->triangle_smooth_normal_buffer));

  args.images.push_back(sampled_binding(16 + 0, params.hit_normal_tx));
  args.images.push_back(sampled_binding(16 + 1, params.hit_world_position_tx));
  args.images.push_back(sampled_binding(16 + 2, params.hit_identity_tx));
  args.images.push_back(storage_binding(40 + 3, params.shadow_visibility_tx, /*arrayed=*/true));

  VKStorageBuffer *dispatch_ssbo = static_cast<VKStorageBuffer *>(params.dispatch_buf);
  dispatch_ssbo->ensure_allocated();
  args.indirect_buffer = dispatch_ssbo->vk_handle();
  if (args.indirect_buffer == VK_NULL_HANDLE) {
    return false;
  }

  return dispatch_kernel(scene, NuruKernel::LOCAL_HIT_SHADOW, args, /*allow_batch=*/true);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name OIDN denoise
 * \{ */

#ifdef WITH_OPENIMAGEDENOISE

struct OIDNUniforms {
  uint32_t extent_x;
  uint32_t extent_y;
  uint32_t use_albedo;
  uint32_t use_normal;
};

struct OIDNInteropCache {
  OIDNDevice device = nullptr;
  OIDNFilter filter = nullptr;
  bool filter_use_albedo = false;
  bool filter_use_normal = false;
  int filter_quality = -1;
  int filter_prefilter = -1;
  bool device_is_cpu = false;
  RTBuffer color_buffer;
  RTBuffer albedo_buffer;
  RTBuffer normal_buffer;
  RTBuffer output_buffer;
  OIDNBuffer oidn_color = nullptr;
  OIDNBuffer oidn_albedo = nullptr;
  OIDNBuffer oidn_normal = nullptr;
  OIDNBuffer oidn_output = nullptr;
  size_t oidn_buffer_size = 0;
  /* True when the OIDN buffers are imports of the Vulkan allocations (zero copy). False means
   * separate OIDN staging buffers fed through mapped-memory copies. */
  bool buffers_shared = false;
};

static OIDNInteropCache &oidn_interop_cache()
{
  static OIDNInteropCache cache;
  return cache;
}

static bool oidn_report_error(OIDNDevice device, const char *context)
{
  const char *error_message = nullptr;
  if (oidnGetDeviceError(device, &error_message) != OIDN_ERROR_NONE) {
    fprintf(stderr,
            "EEVEE OIDN %s failed: %s\n",
            context,
            error_message != nullptr ? error_message : "unknown error");
    return true;
  }
  return false;
}

static bool oidn_perf_logging_enabled()
{
  return env_flag_enabled("BLENDER_EEVEE_HWRT_PERF");
}

static int oidn_quality_from_nuru(const int quality)
{
#  if OIDN_VERSION_MAJOR >= 2
  /* Metal-parity mapping of `RaytraceEEVEE.denoise_quality`:
   * HIGH = 1, BALANCED = 2 (default), FAST = 3. */
  switch (quality) {
    case 1:
      return OIDN_QUALITY_HIGH;
    case 3:
      return OIDN_QUALITY_FAST;
    case 2:
    default:
      return OIDN_QUALITY_BALANCED;
  }
#  else
  (void)quality;
  return 0;
#  endif
}

static void free_oidn_buffers(OIDNInteropCache &cache)
{
  for (OIDNBuffer *oidn_buffer :
       {&cache.oidn_color, &cache.oidn_albedo, &cache.oidn_normal, &cache.oidn_output})
  {
    if (*oidn_buffer != nullptr) {
      oidnReleaseBuffer(*oidn_buffer);
      *oidn_buffer = nullptr;
    }
  }
  rt_buffer_free(cache.color_buffer);
  rt_buffer_free(cache.albedo_buffer);
  rt_buffer_free(cache.normal_buffer);
  rt_buffer_free(cache.output_buffer);
  cache.oidn_buffer_size = 0;
}

static bool ensure_oidn_device(OIDNInteropCache &cache, const bool use_gpu)
{
  if (cache.device != nullptr && cache.device_is_cpu == !use_gpu) {
    return true;
  }
  if (cache.device != nullptr) {
    if (cache.filter != nullptr) {
      oidnReleaseFilter(cache.filter);
      cache.filter = nullptr;
    }
    /* Buffers (including the Vulkan side of shared imports) are tied to the device. */
    free_oidn_buffers(cache);
    oidnReleaseDevice(cache.device);
    cache.device = nullptr;
  }
  cache.device = oidnNewDevice(use_gpu ? OIDN_DEVICE_TYPE_DEFAULT : OIDN_DEVICE_TYPE_CPU);
  if (cache.device == nullptr) {
    return false;
  }
  oidnCommitDevice(cache.device);
  if (oidn_report_error(cache.device, "device commit")) {
    oidnReleaseDevice(cache.device);
    cache.device = nullptr;
    return false;
  }
  cache.device_is_cpu = !use_gpu;
  cache.filter_quality = -1;
  return true;
}

static bool ensure_oidn_filter(OIDNInteropCache &cache,
                               const bool use_albedo,
                               const bool use_normal,
                               const int quality,
                               const int prefilter)
{
  if (cache.filter != nullptr && cache.filter_use_albedo == use_albedo &&
      cache.filter_use_normal == use_normal && cache.filter_quality == quality &&
      cache.filter_prefilter == prefilter)
  {
    return true;
  }
  if (cache.filter != nullptr) {
    oidnReleaseFilter(cache.filter);
    cache.filter = nullptr;
  }
  cache.filter = oidnNewFilter(cache.device, "RT");
  if (cache.filter == nullptr) {
    return false;
  }
  oidnSetFilterBool(cache.filter, "hdr", true);
  oidnSetFilterBool(cache.filter, "srgb", false);
#  if OIDN_VERSION_MAJOR >= 2
  oidnSetFilterInt(cache.filter, "quality", oidn_quality_from_nuru(quality));
#  endif
  /* Metal-parity: guides are treated as clean unless the FAST prefilter (= 2) is selected. */
  oidnSetFilterBool(cache.filter, "cleanAux", prefilter != 2);
  cache.filter_use_albedo = use_albedo;
  cache.filter_use_normal = use_normal;
  cache.filter_quality = quality;
  cache.filter_prefilter = prefilter;
  return true;
}

/* Ensure a Vulkan buffer and its zero-copy OIDN import as one unit. */
static bool ensure_oidn_plane_shared(OIDNInteropCache &cache,
                                     RTBuffer &rt_buffer,
                                     OIDNBuffer &oidn_buffer,
                                     const size_t size)
{
  if (rt_buffer.buffer != VK_NULL_HANDLE && rt_buffer.external_memory != VK_NULL_HANDLE &&
      rt_buffer.size >= size && oidn_buffer != nullptr)
  {
    return true;
  }
  if (oidn_buffer != nullptr) {
    oidnReleaseBuffer(oidn_buffer);
    oidn_buffer = nullptr;
  }
  rt_buffer_free(rt_buffer);
  if (!rt_external_buffer_create(rt_buffer, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
    return false;
  }
#  ifdef _WIN32
  void *handle = rt_external_buffer_export_win32(rt_buffer);
  if (handle == nullptr) {
    rt_buffer_free(rt_buffer);
    return false;
  }
  oidn_buffer = oidnNewSharedBufferFromWin32Handle(cache.device,
                                                   OIDN_EXTERNAL_MEMORY_TYPE_FLAG_OPAQUE_WIN32,
                                                   handle,
                                                   /*name=*/nullptr,
                                                   rt_buffer.size);
  if (oidn_buffer == nullptr) {
    CloseHandle(handle);
    (void)oidn_report_error(cache.device, "shared buffer import");
    rt_buffer_free(rt_buffer);
    return false;
  }
  /* NT handles are not adopted by the importer; keep ours open for the import's lifetime and
   * close it in `rt_buffer_free`. */
  rt_buffer.external_handle = handle;
#  else
  const int fd = rt_external_buffer_export_fd(rt_buffer);
  if (fd < 0) {
    rt_buffer_free(rt_buffer);
    return false;
  }
  oidn_buffer = oidnNewSharedBufferFromFD(
      cache.device, OIDN_EXTERNAL_MEMORY_TYPE_FLAG_OPAQUE_FD, fd, rt_buffer.size);
  if (oidn_buffer == nullptr) {
    /* Import failed: ownership of the fd was not transferred. */
    close(fd);
    (void)oidn_report_error(cache.device, "shared buffer import");
    rt_buffer_free(rt_buffer);
    return false;
  }
#  endif
  return true;
}

static bool ensure_oidn_buffers(OIDNInteropCache &cache,
                                const size_t buffer_size,
                                const bool use_albedo,
                                const bool use_normal)
{
  /* Prefer importing the Vulkan allocations straight into OIDN's GPU device (CUDA on NVIDIA).
   * The filter then reads/writes the packed planes in place; the copy path below moves
   * ~4 x extent x 12 bytes across mapped memory every denoise call. On Windows the staging
   * path is doubly pathological: the pack kernel's GPU writes into snooped host-cached memory
   * measured ~53 ms per call at 480x270 (EMERALD perf evidence), so zero-copy import is the
   * required form on both Vulkan platforms. */
  const VKDevice &device = VKBackend::get().device;
  bool want_shared = !cache.device_is_cpu && device.extensions_get().external_memory;
  if (want_shared) {
    const int external_types = oidnGetDeviceInt(cache.device, "externalMemoryTypes");
    (void)oidnGetDeviceError(cache.device, nullptr); /* Clear error from unknown parameter. */
#  ifdef _WIN32
    want_shared = (external_types & OIDN_EXTERNAL_MEMORY_TYPE_FLAG_OPAQUE_WIN32) != 0;
#  else
    want_shared = (external_types & OIDN_EXTERNAL_MEMORY_TYPE_FLAG_OPAQUE_FD) != 0;
#  endif
  }

  if (want_shared != cache.buffers_shared) {
    free_oidn_buffers(cache);
    cache.buffers_shared = want_shared;
  }

  if (want_shared) {
    bool ok = ensure_oidn_plane_shared(cache, cache.color_buffer, cache.oidn_color, buffer_size) &&
              ensure_oidn_plane_shared(cache, cache.output_buffer, cache.oidn_output, buffer_size);
    if (ok && use_albedo) {
      ok = ensure_oidn_plane_shared(cache, cache.albedo_buffer, cache.oidn_albedo, buffer_size);
    }
    if (ok && use_normal) {
      ok = ensure_oidn_plane_shared(cache, cache.normal_buffer, cache.oidn_normal, buffer_size);
    }
    if (ok) {
      cache.oidn_buffer_size = buffer_size;
      return true;
    }
    /* Import not available after all: fall back to staging copies. */
    free_oidn_buffers(cache);
    cache.buffers_shared = false;
  }

  auto ensure_vk = [](RTBuffer &buffer, const size_t size) {
    if (buffer.buffer != VK_NULL_HANDLE && buffer.external_memory == VK_NULL_HANDLE &&
        buffer.size >= size)
    {
      return true;
    }
    rt_buffer_free(buffer);
    /* Host-cached: `oidnWriteBuffer` reads these mapped pointers back; write-combined memory
     * makes those reads pathologically slow. */
    return rt_buffer_create(buffer,
                            size,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            /*host_visible=*/true,
                            /*host_cached=*/true);
  };
  if (!ensure_vk(cache.color_buffer, buffer_size) || !ensure_vk(cache.output_buffer, buffer_size))
  {
    return false;
  }
  if (use_albedo && !ensure_vk(cache.albedo_buffer, buffer_size)) {
    return false;
  }
  if (use_normal && !ensure_vk(cache.normal_buffer, buffer_size)) {
    return false;
  }

  if (cache.oidn_buffer_size < buffer_size || cache.oidn_color == nullptr) {
    if (cache.oidn_color != nullptr) {
      oidnReleaseBuffer(cache.oidn_color);
    }
    if (cache.oidn_output != nullptr) {
      oidnReleaseBuffer(cache.oidn_output);
    }
    /* Albedo/normal must be resized together with color/output, otherwise a later
     * `oidnWriteBuffer(..., buffer_size, ...)` targets a stale smaller buffer. */
    if (cache.oidn_albedo != nullptr) {
      oidnReleaseBuffer(cache.oidn_albedo);
      cache.oidn_albedo = nullptr;
    }
    if (cache.oidn_normal != nullptr) {
      oidnReleaseBuffer(cache.oidn_normal);
      cache.oidn_normal = nullptr;
    }
    cache.oidn_color = oidnNewBuffer(cache.device, buffer_size);
    cache.oidn_output = oidnNewBuffer(cache.device, buffer_size);
    cache.oidn_buffer_size = buffer_size;
  }
  if (use_albedo && cache.oidn_albedo == nullptr) {
    cache.oidn_albedo = oidnNewBuffer(cache.device, buffer_size);
  }
  if (use_normal && cache.oidn_normal == nullptr) {
    cache.oidn_normal = oidnNewBuffer(cache.device, buffer_size);
  }
  return cache.oidn_color != nullptr && cache.oidn_output != nullptr &&
         (!use_albedo || cache.oidn_albedo != nullptr) &&
         (!use_normal || cache.oidn_normal != nullptr);
}

#endif /* WITH_OPENIMAGEDENOISE */

bool raytrace_denoise_oidn(const GPUHardwareRaytraceOIDNDenoiseParams &params)
{
#ifndef WITH_OPENIMAGEDENOISE
  (void)params;
  return false;
#else
  if (params.input_radiance_tx == nullptr || params.output_radiance_tx == nullptr ||
      params.extent.x <= 0 || params.extent.y <= 0)
  {
    return false;
  }
  VKContext *context = VKContext::get();
  if (context == nullptr) {
    return false;
  }

  const bool use_albedo = params.use_albedo && params.albedo_tx != nullptr;
  const bool use_normal = params.use_normal && params.normal_tx != nullptr;

  /* `BLENDER_EEVEE_HWRT_OIDN_CPU=1` forces the CPU device for diagnostics (used to isolate
   * the June 11 CUDA/Vulkan contention wedge). */
  const bool use_gpu_oidn = params.use_gpu && !env_flag_enabled("BLENDER_EEVEE_HWRT_OIDN_CPU");

  OIDNInteropCache &cache = oidn_interop_cache();
  if (!ensure_oidn_device(cache, use_gpu_oidn) ||
      !ensure_oidn_filter(cache, use_albedo, use_normal, params.quality, params.prefilter))
  {
    return false;
  }

  const size_t buffer_size = size_t(params.extent.x) * size_t(params.extent.y) *
                             sizeof(float) * 3;
  if (!ensure_oidn_buffers(cache, buffer_size, use_albedo, use_normal)) {
    return false;
  }

  const bool perf_logging_enabled = oidn_perf_logging_enabled();
  const double pack_start_time = perf_logging_enabled ? BLI_time_now_seconds() : 0.0;
  /* Pack-phase breakdown (perf logging only): flush, runner publish, record, fence wait. */
  double pack_flush_ms = 0.0;
  double pack_publish_ms = 0.0;
  double pack_record_ms = 0.0;
  double pack_gpu_ms = 0.0;

  /* Pack: GPU compute writes the radiance (and aux) into tightly packed float3 buffers. */
  {
    OIDNUniforms uniforms = {};
    uniforms.extent_x = uint32_t(params.extent.x);
    uniforms.extent_y = uint32_t(params.extent.y);
    uniforms.use_albedo = use_albedo ? 1u : 0u;
    uniforms.use_normal = use_normal ? 1u : 0u;

    KernelArgs args;
    args.uniforms_data = &uniforms;
    args.uniforms_size = sizeof(uniforms);
    args.ssbos.push_back(rt_buffer_binding(2, cache.color_buffer));
    args.ssbos.push_back(
        rt_buffer_binding(3, use_albedo ? cache.albedo_buffer : cache.color_buffer));
    args.ssbos.push_back(
        rt_buffer_binding(4, use_normal ? cache.normal_buffer : cache.color_buffer));
    args.images.push_back(sampled_binding(16 + 0, params.input_radiance_tx));
    args.images.push_back(
        sampled_binding(16 + 1, use_albedo ? params.albedo_tx : params.input_radiance_tx));
    args.images.push_back(
        sampled_binding(16 + 2, use_normal ? params.normal_tx : params.input_radiance_tx));
    args.group_count = int3((params.extent.x + 7) / 8, (params.extent.y + 7) / 8, 1);

    /* Pack must complete before the CPU reads the mapped buffers. The submission-timeline wait
     * keeps our direct queue submit ordered after the graph's background-thread submits. */
    std::scoped_lock lock(raytrace_mutex_get());
    double t_phase = perf_logging_enabled ? BLI_time_now_seconds() : 0.0;
    const TimelineValue pending_timeline = context->flush_render_graph(
        RenderGraphFlushFlags::SUBMIT | RenderGraphFlushFlags::RENEW_RENDER_GRAPH);
    if (perf_logging_enabled) {
      const double now = BLI_time_now_seconds();
      pack_flush_ms = (now - t_phase) * 1000.0;
      t_phase = now;
    }
    VKBackend::get().device.wait_for_submission_timeline(pending_timeline);
    if (perf_logging_enabled) {
      const double now = BLI_time_now_seconds();
      pack_publish_ms = (now - t_phase) * 1000.0;
      t_phase = now;
    }
    NuruVKSubmission *submission = submission_begin();
    if (submission == nullptr) {
      return false;
    }
    if (!record_kernel_dispatch(submission, NuruKernel::OIDN_PACK, args)) {
      vkEndCommandBuffer(submission->command_buffer);
      submission_destroy(submission);
      return false;
    }
    if (perf_logging_enabled) {
      const double now = BLI_time_now_seconds();
      pack_record_ms = (now - t_phase) * 1000.0;
      t_phase = now;
    }
    if (!submission_end(submission, /*wait=*/true)) {
      return false;
    }
    if (perf_logging_enabled) {
      pack_gpu_ms = (BLI_time_now_seconds() - t_phase) * 1000.0;
    }
  }
  const double pack_ms = perf_logging_enabled ?
                             (BLI_time_now_seconds() - pack_start_time) * 1000.0 :
                             0.0;

  /* Shared (imported) buffers: the pack kernel already wrote the planes OIDN will read.
   * Staging buffers: copy the mapped planes into the OIDN device. */
  if (!cache.buffers_shared) {
    oidnWriteBuffer(cache.oidn_color, 0, buffer_size, cache.color_buffer.mapped);
    if (use_albedo) {
      oidnWriteBuffer(cache.oidn_albedo, 0, buffer_size, cache.albedo_buffer.mapped);
    }
    if (use_normal) {
      oidnWriteBuffer(cache.oidn_normal, 0, buffer_size, cache.normal_buffer.mapped);
    }
    if (oidn_report_error(cache.device, "buffer upload")) {
      return false;
    }
  }

  const size_t width = size_t(params.extent.x);
  const size_t height = size_t(params.extent.y);

  /* OptiX denoiser branch (user-selected): same packed planes, same zero-copy allocations,
   * same pack/unpack pipeline - only the filter engine differs. Requires the shared (imported)
   * buffers; any unavailability or failure falls through to the OIDN filter below. The CUDA
   * burst runs under the same queue-mutex + drain contract as the GPU OIDN filter (Xid 109,
   * see the comment on that block). */
  bool used_optix = false;
  const double filter_start_time_optix = perf_logging_enabled ? BLI_time_now_seconds() : 0.0;
#  ifdef WITH_NURU_OPTIX_DENOISER
  if (params.use_optix_denoiser && cache.buffers_shared && use_gpu_oidn) {
    VKDevice &device = VKBackend::get().device;
    std::scoped_lock queue_lock(device.queue_mutex_get());
    device.wait_for_timeline(device.queue_submitted_timeline_value());

    nuru_optix::DenoiseParams optix_params;
    optix_params.color = {cache.color_buffer.external_handle, cache.color_buffer.size};
    optix_params.output = {cache.output_buffer.external_handle, cache.output_buffer.size};
    if (use_albedo) {
      optix_params.albedo = {cache.albedo_buffer.external_handle, cache.albedo_buffer.size};
    }
    if (use_normal) {
      optix_params.normal = {cache.normal_buffer.external_handle, cache.normal_buffer.size};
    }
    optix_params.width = params.extent.x;
    optix_params.height = params.extent.y;
    used_optix = nuru_optix::denoise(optix_params);
  }
#  endif

  if (!used_optix) {
    oidnSetFilterImage(
        cache.filter, "color", cache.oidn_color, OIDN_FORMAT_FLOAT3, width, height, 0, 0, 0);
    oidnSetFilterImage(
        cache.filter, "output", cache.oidn_output, OIDN_FORMAT_FLOAT3, width, height, 0, 0, 0);
    if (use_albedo) {
      oidnSetFilterImage(
          cache.filter, "albedo", cache.oidn_albedo, OIDN_FORMAT_FLOAT3, width, height, 0, 0, 0);
    }
    else {
      oidnUnsetFilterImage(cache.filter, "albedo");
    }
    if (use_normal) {
      oidnSetFilterImage(
          cache.filter, "normal", cache.oidn_normal, OIDN_FORMAT_FLOAT3, width, height, 0, 0, 0);
    }
    else {
      oidnUnsetFilterImage(cache.filter, "normal");
    }

    oidnCommitFilter(cache.filter);
    if (oidn_report_error(cache.device, "filter commit")) {
      return false;
    }
  }
  const double filter_start_time = used_optix ?
                                       filter_start_time_optix :
                                       (perf_logging_enabled ? BLI_time_now_seconds() : 0.0);
  if (used_optix) {
    /* Filter already ran above; fall through to the unpack. */
  }
  else if (use_gpu_oidn) {
    /* Hand the GPU to the CUDA filter alone. The filter's long non-preemptible kernels starve
     * concurrently executing Vulkan channels into NVRM Xid 109 CTX SWITCH TIMEOUT (NVIDIA
     * 595.71 / RTX 5090; June 11 isolation: async + GPU OIDN wedged within minutes while
     * force-sync + GPU OIDN and async + CPU OIDN were both stable through heavy interactive
     * stress). A drain alone is not enough — the submission runner can push NEW graph work
     * onto the queue while the filter runs (wedge #7 reproduced with the plain drain). Hold
     * the queue mutex across the filter so nothing reaches `vkQueueSubmit` until the CUDA
     * burst has fully synced, and wait for everything already on the queue under that lock
     * (safe: completion of submitted work needs no further submissions). */
    VKDevice &device = VKBackend::get().device;
    std::scoped_lock queue_lock(device.queue_mutex_get());
    device.wait_for_timeline(device.queue_submitted_timeline_value());
    oidnExecuteFilter(cache.filter);
    if (oidn_report_error(cache.device, "filter execution")) {
      return false;
    }
    oidnSyncDevice(cache.device);
    if (oidn_report_error(cache.device, "filter sync")) {
      return false;
    }
  }
  else {
    oidnExecuteFilter(cache.filter);
    if (oidn_report_error(cache.device, "filter execution")) {
      return false;
    }
    oidnSyncDevice(cache.device);
    if (oidn_report_error(cache.device, "filter sync")) {
      return false;
    }
  }
  const double filter_ms = perf_logging_enabled ?
                               (BLI_time_now_seconds() - filter_start_time) * 1000.0 :
                               0.0;

  if (!cache.buffers_shared) {
    oidnReadBuffer(cache.oidn_output, 0, buffer_size, cache.output_buffer.mapped);
    if (oidn_report_error(cache.device, "buffer readback")) {
      return false;
    }
  }

  /* Unpack: GPU compute writes the filtered radiance back, preserving alpha. */
  const double unpack_start_time = perf_logging_enabled ? BLI_time_now_seconds() : 0.0;
  bool unpack_submitted = false;
  {
    OIDNUniforms uniforms = {};
    uniforms.extent_x = uint32_t(params.extent.x);
    uniforms.extent_y = uint32_t(params.extent.y);

    KernelArgs args;
    args.uniforms_data = &uniforms;
    args.uniforms_size = sizeof(uniforms);
    args.ssbos.push_back(rt_buffer_binding(2, cache.output_buffer));
    args.images.push_back(sampled_binding(16 + 0, params.input_radiance_tx));
    args.images.push_back(storage_binding(40 + 1, params.output_radiance_tx));
    args.group_count = int3((params.extent.x + 7) / 8, (params.extent.y + 7) / 8, 1);

    std::scoped_lock lock(raytrace_mutex_get());
    const TimelineValue pending_timeline = context->flush_render_graph(
        RenderGraphFlushFlags::SUBMIT | RenderGraphFlushFlags::RENEW_RENDER_GRAPH);
    VKBackend::get().device.wait_for_submission_timeline(pending_timeline);
    NuruVKSubmission *submission = submission_begin();
    if (submission != nullptr) {
      if (record_kernel_dispatch(submission, NuruKernel::OIDN_UNPACK, args)) {
        unpack_submitted = submission_end(submission, /*wait=*/true);
      }
      else {
        vkEndCommandBuffer(submission->command_buffer);
        submission_destroy(submission);
      }
    }
  }

  if (perf_logging_enabled) {
    const double unpack_ms = (BLI_time_now_seconds() - unpack_start_time) * 1000.0;
    std::fprintf(stderr,
                 "EEVEE HWRT perf oidn_backend denoiser=%s gpu=%d shared_buffers=%d "
                 "extent=%dx%d albedo=%d "
                 "normal=%d pack_ms=%.2f (flush=%.2f publish=%.2f record=%.2f gpu_wait=%.2f) "
                 "filter_submit_ms=%.2f unpack_ms=%.2f unpacked=%d\n",
                 used_optix ? "optix" : "oidn",
                 use_gpu_oidn ? 1 : 0,
                 cache.buffers_shared ? 1 : 0,
                 params.extent.x,
                 params.extent.y,
                 use_albedo ? 1 : 0,
                 use_normal ? 1 : 0,
                 pack_ms,
                 pack_flush_ms,
                 pack_publish_ms,
                 pack_record_ms,
                 pack_gpu_ms,
                 filter_ms,
                 unpack_ms,
                 unpack_submitted ? 1 : 0);
  }
  return unpack_submitted;
#endif
}

void raytrace_scene_free(GPUHardwareRaytraceScene *scene)
{
  delete scene;
}

#ifdef WITH_OPENIMAGEDENOISE
static void oidn_interop_cache_free()
{
  OIDNInteropCache &cache = oidn_interop_cache();
  if (cache.filter != nullptr) {
    oidnReleaseFilter(cache.filter);
    cache.filter = nullptr;
  }
#  ifdef WITH_NURU_OPTIX_DENOISER
  /* Before the buffers: the OptiX path holds no per-call buffer references (imports are
   * released after every denoise), but the denoiser/context must go before device teardown. */
  nuru_optix::free_resources();
#  endif
  free_oidn_buffers(cache);
  if (cache.device != nullptr) {
    oidnReleaseDevice(cache.device);
    cache.device = nullptr;
  }
  cache.buffers_shared = false;
  cache.filter_quality = -1;
}
#endif

void raytrace_device_free()
{
  VKDevice &device = VKBackend::get().device;
  if (device.vk_handle() == VK_NULL_HANDLE) {
    return;
  }
  std::scoped_lock lock(raytrace_mutex_get());
  /* Wait for and release every in-flight submission (frees fences, descriptor pools, uniform
   * buffers and command buffers), then drain the queue so render-graph work that samples our
   * outputs cannot still be executing while pipelines/buffers are destroyed below. */
  collect_finished_submissions(true);
  device.wait_queue_idle();

#ifdef WITH_OPENIMAGEDENOISE
  oidn_interop_cache_free();
#endif

  for (KernelPipeline &pipeline : g_kernel_pipelines) {
    if (pipeline.pipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(device.vk_handle(), pipeline.pipeline, nullptr);
    }
    if (pipeline.pipeline_layout != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device.vk_handle(), pipeline.pipeline_layout, nullptr);
    }
    if (pipeline.descriptor_set_layout != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(device.vk_handle(), pipeline.descriptor_set_layout, nullptr);
    }
    if (pipeline.shader_module != VK_NULL_HANDLE) {
      vkDestroyShaderModule(device.vk_handle(), pipeline.shader_module, nullptr);
    }
    pipeline = {};
  }

  if (g_dummy_array_view != VK_NULL_HANDLE) {
    vkDestroyImageView(device.vk_handle(), g_dummy_array_view, nullptr);
    g_dummy_array_view = VK_NULL_HANDLE;
  }
  if (g_dummy_array_image != VK_NULL_HANDLE) {
    vmaDestroyImage(device.mem_allocator_get(), g_dummy_array_image, g_dummy_array_allocation);
    g_dummy_array_image = VK_NULL_HANDLE;
    g_dummy_array_allocation = VK_NULL_HANDLE;
  }
  g_dummy_array_failed = false;

  for (VkSampler &sampler : g_raytrace_samplers) {
    if (sampler != VK_NULL_HANDLE) {
      vkDestroySampler(device.vk_handle(), sampler, nullptr);
      sampler = VK_NULL_HANDLE;
    }
  }

  if (g_raytrace_command_pool != VK_NULL_HANDLE) {
    vkDestroyCommandPool(device.vk_handle(), g_raytrace_command_pool, nullptr);
    g_raytrace_command_pool = VK_NULL_HANDLE;
  }
}

/** \} */

}  // namespace blender::gpu::vulkan

namespace blender {

GPUHardwareRaytraceScene::~GPUHardwareRaytraceScene()
{
  using namespace gpu::vulkan;
  /* Submissions may still reference the acceleration structures and buffers. */
  if (shadow_batch_submission != nullptr) {
    /* Never submitted: finish recording and destroy directly. */
    vkEndCommandBuffer(shadow_batch_submission->command_buffer);
    gpu::vulkan::submission_destroy(shadow_batch_submission);
    shadow_batch_submission = nullptr;
  }
  gpu::vulkan::collect_finished_submissions(true);
  destroy_acceleration_structure(top_level_acceleration_structure, top_level_buffer);
  rt_buffer_free(emissive_radiance_buffer);
  rt_buffer_free(emissive_light_buffer);
  rt_buffer_free(diffuse_albedo_buffer);
  rt_buffer_free(material_proxy_buffer);
  rt_buffer_free(triangle_normal_buffer);
  rt_buffer_free(triangle_smooth_normal_buffer);
  rt_buffer_free(triangle_local_position_buffer);
  rt_buffer_free(triangle_normal_range_buffer);
  for (size_t i = 0; i < bottom_level_acceleration_structures.size(); i++) {
    RTBuffer &buffer = bottom_level_buffers[i];
    destroy_acceleration_structure(bottom_level_acceleration_structures[i], buffer);
  }
  for (nuru_vk::RTBuffer &geometry_buffer : geometry_buffers) {
    gpu::vulkan::rt_buffer_free(geometry_buffer);
  }
}

}  // namespace blender
