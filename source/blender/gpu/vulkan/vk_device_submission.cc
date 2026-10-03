/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <thread>

#include "BLI_mutex.hh"
#include "BLI_system.h"
#include "BLI_task.h"

#include "vk_device.hh"
#include "vk_to_string.hh"

#include "CLG_log.h"

namespace blender {

static CLG_LogRef LOG = {"gpu.vulkan"};

namespace gpu {

/* -------------------------------------------------------------------- */
/** \name Render graph
 * \{ */

struct VKRenderGraphWait {
  Mutex is_submitted_mutex;
  std::condition_variable_any is_submitted_condition;
  bool is_submitted;
};

struct VKRenderGraphSubmitTask {
  render_graph::VKRenderGraph *render_graph;
  uint64_t timeline;
  bool submit_to_device;
  VkPipelineStageFlags wait_dst_stage_mask;
  VkSemaphore wait_semaphore;
  VkSemaphore signal_semaphore;
  VkFence signal_fence;
  uint64_t wait_semaphore_value;
  uint64_t signal_semaphore_value;
  VKRenderGraphWait *wait_for_submission;
};

TimelineValue VKDevice::render_graph_submit(render_graph::VKRenderGraph *render_graph,
                                            VKDiscardPool &context_discard_pool,
                                            bool submit_to_device,
                                            bool wait_for_submission,
                                            bool wait_for_completion,
                                            VkPipelineStageFlags wait_dst_stage_mask,
                                            VkSemaphore wait_semaphore,
                                            VkSemaphore signal_semaphore,
                                            VkFence signal_fence,
                                            uint64_t wait_semaphore_value,
                                            uint64_t signal_semaphore_value)
{
  /* Syncing input flags. */
  /* When we wait for completion/submission we must submit to device. */
  submit_to_device |= wait_for_completion;
  submit_to_device |= wait_for_submission;
  /* We don't need to wait for submission when waiting for completion. */
  wait_for_submission &= !wait_for_completion;

  VKRenderGraphSubmitTask *submit_task = MEM_new<VKRenderGraphSubmitTask>(__func__);
  submit_task->render_graph = render_graph;
  submit_task->submit_to_device = submit_to_device;
  submit_task->wait_dst_stage_mask = wait_dst_stage_mask;
  submit_task->wait_semaphore = wait_semaphore;
  submit_task->signal_semaphore = signal_semaphore;
  submit_task->signal_fence = signal_fence;
  submit_task->wait_semaphore_value = wait_semaphore_value;
  submit_task->signal_semaphore_value = signal_semaphore_value;
  submit_task->wait_for_submission = nullptr;

  VKRenderGraphWait wait_condition{};
  if (wait_for_submission) {
    submit_task->wait_for_submission = &wait_condition;
  }
  TimelineValue timeline = 0;
  {
    std::scoped_lock lock(orphaned_data.mutex_get());
    timeline = submit_task->timeline = submit_to_device ? ++timeline_value_ : timeline_value_ + 1;
    orphaned_data.timeline_ = timeline;
    orphaned_data.move_data(context_discard_pool, timeline);
    BLI_thread_queue_push(
        submitted_render_graphs_, submit_task, BLI_THREAD_QUEUE_WORK_PRIORITY_NORMAL);
  }
  submit_task = nullptr;

  if (wait_for_submission) {
    std::unique_lock<Mutex> lock(wait_condition.is_submitted_mutex);
    wait_condition.is_submitted_condition.wait(lock, [&] { return wait_condition.is_submitted; });
  }

  if (wait_for_completion) {
    wait_for_timeline(timeline);
  }
  return timeline;
}

void VKDevice::wait_for_submission_timeline(TimelineValue timeline)
{
  if (timeline == 0 ||
      queue_submitted_timeline_.load(std::memory_order_acquire) >= timeline)
  {
    return;
  }
  std::unique_lock lock(queue_submitted_mutex_);
  queue_submitted_condition_.wait(lock, [&] {
    return queue_submitted_timeline_.load(std::memory_order_acquire) >= timeline;
  });
}

TimelineValue VKDevice::external_timeline_chain_acquire(TimelineValue *r_wait_value)
{
  std::scoped_lock lock(orphaned_data.mutex_get());
  *r_wait_value = timeline_value_;
  const TimelineValue signal_value = ++timeline_value_;
  orphaned_data.timeline_ = signal_value;
  return signal_value;
}

void VKDevice::external_timeline_chain_submitted(TimelineValue value)
{
  std::scoped_lock lock(queue_submitted_mutex_);
  /* Keep the published stream monotonic: the runner thread may publish a later graph value
   * before this external value lands. */
  if (value > queue_submitted_timeline_.load(std::memory_order_acquire)) {
    queue_submitted_timeline_.store(value, std::memory_order_release);
  }
  queue_submitted_condition_.notify_all();
}

void VKDevice::wait_for_timeline(TimelineValue timeline)
{
  if (timeline == 0) {
    return;
  }
  VkSemaphoreWaitInfo vk_semaphore_wait_info = {
      VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &vk_timeline_semaphore_, &timeline};
  /* Nuru: wait in finite slices instead of a single infinite wait. On an Xid-class GPU wedge
   * the NVIDIA driver can poll forever without returning VK_ERROR_DEVICE_LOST, which silenced
   * the checkpoint diagnostic. Dump the checkpoints on the first long stall (the queue state
   * does not change once wedged), then keep waiting so legitimate long GPU work still
   * completes. */
  constexpr uint64_t wait_slice_ns = 10ull * 1000ull * 1000ull * 1000ull;
  VkResult wait_result = VK_TIMEOUT;
  bool stall_reported = false;
  while ((wait_result = vkWaitSemaphores(vk_device_, &vk_semaphore_wait_info, wait_slice_ns)) ==
         VK_TIMEOUT)
  {
    if (!stall_reported) {
      stall_reported = true;
      CLOG_ERROR(&LOG,
                 "Vulkan: timeline wait exceeded %llu ms (value %llu); possible GPU wedge",
                 (unsigned long long)(wait_slice_ns / 1000000ull),
                 (unsigned long long)timeline);
      diagnostic_checkpoints_dump();
    }
  }
  if (wait_result != VK_SUCCESS) {
    CLOG_ERROR(
        &LOG, "Vulkan: failed to wait for synchronization timeline [%s]", to_string(wait_result));
    if (wait_result == VK_ERROR_DEVICE_LOST) {
      diagnostic_checkpoints_dump();
    }
  }
}

void VKDevice::diagnostic_checkpoints_dump()
{
  /* Nuru: wedge diagnostic. Print the timeline-chain state on every call: the gap between
   * `claimed`, `submitted`, and `gpu_signaled` classifies a stalled wait. A wait on a value
   * above `submitted` means the owner never reached `vkQueueSubmit` (CPU-side stall: runner
   * blocked or a claimed value dropped); a wait on a value at or below `submitted` but above
   * `gpu_signaled` means the submission sits on the queue blocked by an unsatisfied semaphore
   * dependency (GPU-side chain stall). */
  {
    TimelineValue gpu_signaled = 0;
    vkGetSemaphoreCounterValue(vk_device_, vk_timeline_semaphore_, &gpu_signaled);
    TimelineValue claimed = 0;
    {
      std::scoped_lock lock(orphaned_data.mutex_get());
      claimed = timeline_value_;
    }
    fprintf(stderr,
            "[NURU_VK_WEDGE] timeline state: claimed=%llu submitted=%llu gpu_signaled=%llu\n",
            (unsigned long long)claimed,
            (unsigned long long)queue_submitted_timeline_.load(std::memory_order_acquire),
            (unsigned long long)gpu_signaled);
    fflush(stderr);
  }

  /* All-thread backtraces, once per process: names the thread (and code site) every stuck
   * participant is blocked in. */
  {
    static std::atomic<bool> backtraced = false;
    if (!backtraced.exchange(true)) {
      fprintf(stderr, "[NURU_VK_WEDGE] all-thread backtrace begin\n");
      BLI_system_backtrace(stderr);
      fprintf(stderr, "[NURU_VK_WEDGE] all-thread backtrace end\n");
      fflush(stderr);
    }
  }

  /* Nuru: VK_NV_device_diagnostic_checkpoints wedge diagnostic (BLENDER_VULKAN_CHECKPOINTS=1).
   * Reports the last checkpoint markers that started/completed execution on the queue, naming
   * the render-graph node where the GPU wedged. Only the first caller dumps; afterwards the
   * queue state does not change anymore. */
  if (!extensions_.diagnostic_checkpoints || functions.vkGetQueueCheckpointData == nullptr) {
    return;
  }
  static std::atomic<bool> dumped = false;
  if (dumped.exchange(true)) {
    return;
  }

  uint32_t count = 0;
  functions.vkGetQueueCheckpointData(vk_queue_, &count, nullptr);
  Array<VkCheckpointDataNV> checkpoints(count);
  for (VkCheckpointDataNV &data : checkpoints) {
    data = {};
    data.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
  }
  functions.vkGetQueueCheckpointData(vk_queue_, &count, checkpoints.data());

  fprintf(stderr, "[NURU_VK_CHECKPOINTS] device-lost checkpoint dump, %u entries:\n", count);
  for (const VkCheckpointDataNV &data : checkpoints.as_span().take_front(count)) {
    const char *stage_name = "OTHER";
    if (data.stage == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT) {
      stage_name = "LAST_STARTED";
    }
    else if (data.stage == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT) {
      stage_name = "LAST_COMPLETED";
    }
    fprintf(stderr,
            "[NURU_VK_CHECKPOINTS]   stage=0x%08x (%s) marker=%s\n",
            uint32_t(data.stage),
            stage_name,
            data.pCheckpointMarker ? static_cast<const char *>(data.pCheckpointMarker) :
                                     "(none)");
  }
  fflush(stderr);
}

void VKDevice::wait_queue_idle()
{
  std::scoped_lock lock(*queue_mutex_);
  vkQueueWaitIdle(vk_queue_);
}

render_graph::VKRenderGraph *VKDevice::render_graph_new()
{
  render_graph::VKRenderGraph *render_graph = static_cast<render_graph::VKRenderGraph *>(
      BLI_thread_queue_pop_timeout(unused_render_graphs_, 0));
  if (render_graph) {
    return render_graph;
  }

  std::scoped_lock lock(resources.mutex);
  render_graph = MEM_new<render_graph::VKRenderGraph>(__func__, resources);
  render_graphs_.append(render_graph);
  return render_graph;
}

void VKDevice::submission_runner(TaskPool *__restrict pool, void *task_data)
{
  CLOG_TRACE(&LOG, "Submission runner has started");
  UNUSED_VARS(task_data);

  VKDevice *device = static_cast<VKDevice *>(BLI_task_pool_user_data(pool));
  VkCommandPool vk_command_pool = VK_NULL_HANDLE;
  VkCommandPoolCreateInfo vk_command_pool_create_info = {
      VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      nullptr,
      VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      device->vk_queue_family_};
  vkCreateCommandPool(device->vk_device_, &vk_command_pool_create_info, nullptr, &vk_command_pool);

  render_graph::VKScheduler scheduler;
  render_graph::VKCommandBuilder command_builder;
  Vector<VkCommandBuffer> command_buffers_unused;
  TimelineResources<VkCommandBuffer> command_buffers_in_use;
  VkCommandBuffer vk_command_buffer = VK_NULL_HANDLE;
  std::optional<render_graph::VKCommandBufferWrapper> command_buffer;
  uint64_t previous_gc_timeline = 0;
  uint64_t num_nodes = 0;

  CLOG_TRACE(&LOG, "Submission runner initialized");
  while (!BLI_task_pool_current_canceled(pool)) {
    VKRenderGraphSubmitTask *submit_task = static_cast<VKRenderGraphSubmitTask *>(
        BLI_thread_queue_pop_timeout(device->submitted_render_graphs_, 1));
    if (submit_task == nullptr) {
      continue;
    }
    uint64_t current_timeline = device->submission_finished_timeline_get();
    if (assign_if_different(previous_gc_timeline, current_timeline)) {
      device->orphaned_data.destroy_discarded_resources(*device, current_timeline);
    }

    if (!command_buffer.has_value()) {
      /* Check for completed command buffers that can be reused. */
      if (command_buffers_unused.is_empty()) {
        command_buffers_in_use.remove_old(current_timeline,
                                          [&](VkCommandBuffer vk_command_buffer) {
                                            command_buffers_unused.append(vk_command_buffer);
                                          });
      }

      /* Create new command buffers when there are no left to be reused. */
      if (command_buffers_unused.is_empty()) {
        command_buffers_unused.resize(10, VK_NULL_HANDLE);
        VkCommandBufferAllocateInfo vk_command_buffer_allocate_info = {
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            nullptr,
            vk_command_pool,
            VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            10};
        vkAllocateCommandBuffers(
            device->vk_device_, &vk_command_buffer_allocate_info, command_buffers_unused.data());
      };

      vk_command_buffer = command_buffers_unused.pop_last();
      command_buffer = std::make_optional<render_graph::VKCommandBufferWrapper>(
          vk_command_buffer, device->extensions_);
      command_buffer->begin_recording();
    }

    BLI_assert(vk_command_buffer != VK_NULL_HANDLE);

    render_graph::VKRenderGraph &render_graph = *submit_task->render_graph;
    Span<render_graph::NodeHandle> node_handles = scheduler.select_nodes(render_graph);
    {
      std::scoped_lock lock_resources(device->resources.mutex);
      command_builder.build_nodes(render_graph, *command_buffer, node_handles);
    }
    command_builder.record_commands(render_graph, *command_buffer, node_handles);
    num_nodes += node_handles.size();

    if (submit_task->submit_to_device) {
      /* Finalize current command buffer. */
      command_buffer->end_recording();

      uint32_t wait_semaphore_len = submit_task->wait_semaphore == VK_NULL_HANDLE ? 1 : 2;
      VkSemaphore wait_semaphores[2] = {device->vk_timeline_semaphore_,
                                        submit_task->wait_semaphore};
      uint64_t wait_semaphore_values[2] = {submit_task->timeline - 1,
                                           submit_task->wait_semaphore_value};

      uint32_t signal_semaphore_len = submit_task->signal_semaphore == VK_NULL_HANDLE ? 1 : 2;
      VkSemaphore signal_semaphores[2] = {device->vk_timeline_semaphore_,
                                          submit_task->signal_semaphore};
      uint64_t signal_semaphore_values[2] = {submit_task->timeline,
                                             submit_task->signal_semaphore_value};
      VkPipelineStageFlags pipeline_stage_flags[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                      submit_task->wait_dst_stage_mask};

      VkTimelineSemaphoreSubmitInfo vk_timeline_semaphore_submit_info = {
          VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
          nullptr,
          wait_semaphore_len,
          wait_semaphore_values,
          signal_semaphore_len,
          signal_semaphore_values};
      VkSubmitInfo vk_submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                     &vk_timeline_semaphore_submit_info,
                                     wait_semaphore_len,
                                     wait_semaphores,
                                     pipeline_stage_flags,
                                     1,
                                     &vk_command_buffer,
                                     signal_semaphore_len,
                                     signal_semaphores};

      CLOG_TRACE(&LOG, "Submitting %u render graph nodes to device.", uint32_t(num_nodes));
      num_nodes = 0;

      /* Nuru: submissions must reach the queue in timeline-claim order. This batch waits on
       * `timeline - 1`; if that value belongs to an external RT batch that claimed it but has
       * not reached `vkQueueSubmit` yet, submitting this batch first would park the queue
       * front-end on a wait whose signal is submitted later on the SAME queue - the queue then
       * never starts the signaling batch (observed deadlock: TLAS build batch stuck >60s, GPU
       * idle, no TDR, claimed=N+1 submitted=N gpu_signaled=N-1). Externals wait symmetrically
       * before their own submits, so the order forms a total chain and cannot cycle. */
      device->wait_for_submission_timeline(submit_task->timeline - 1);

      VkResult submit_result = VK_SUCCESS;
      {
        std::scoped_lock lock_queue(*device->queue_mutex_);
        submit_result = vkQueueSubmit(
            device->vk_queue_, 1, &vk_submit_info, submit_task->signal_fence);
      }
      if (submit_result != VK_SUCCESS) {
        /* Nuru: a dropped submission must still signal its claimed timeline value; every later
         * graph submission and external RT batch waits on it and would deadlock forever (the
         * external paths already CPU-signal on their own submit failures). */
        CLOG_ERROR(&LOG,
                   "Vulkan: render graph vkQueueSubmit failed [%s]; CPU-signaling timeline %llu "
                   "to unblock waiters",
                   to_string(submit_result),
                   (unsigned long long)submit_task->timeline);
        VkSemaphoreSignalInfo signal_info = {};
        signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
        signal_info.semaphore = device->vk_timeline_semaphore_;
        signal_info.value = submit_task->timeline;
        vkSignalSemaphore(device->vk_device_, &signal_info);
        device->diagnostic_checkpoints_dump();
      }
      {
        /* Nuru: publish queue-submission progress for external direct submitters. Monotonic:
         * an external batch with a higher claimed value may have published already, and a
         * regressing store would strand later `wait_for_submission_timeline` callers. */
        std::scoped_lock lock(device->queue_submitted_mutex_);
        if (submit_task->timeline >
            device->queue_submitted_timeline_.load(std::memory_order_acquire))
        {
          device->queue_submitted_timeline_.store(submit_task->timeline,
                                                  std::memory_order_release);
        }
        device->queue_submitted_condition_.notify_all();
      }
      if (submit_task->wait_for_submission != nullptr) {
        std::unique_lock<Mutex> lock(submit_task->wait_for_submission->is_submitted_mutex);
        submit_task->wait_for_submission->is_submitted = true;
        submit_task->wait_for_submission->is_submitted_condition.notify_one();
      }
      command_buffers_in_use.append_timeline(submit_task->timeline, vk_command_buffer);
      vk_command_buffer = VK_NULL_HANDLE;
      command_buffer.reset();
    }

    render_graph.reset();
    BLI_thread_queue_push(device->unused_render_graphs_,
                          std::move(submit_task->render_graph),
                          BLI_THREAD_QUEUE_WORK_PRIORITY_NORMAL);
    MEM_delete<VKRenderGraphSubmitTask>(submit_task);
  }
  CLOG_TRACE(&LOG, "Submission runner is being canceled");

  /* Clear command buffers and pool */
  {
    std::scoped_lock lock(*device->queue_mutex_);
    vkDeviceWaitIdle(device->vk_device_);
  }
  command_buffers_in_use.remove_old(UINT64_MAX, [&](VkCommandBuffer vk_command_buffer) {
    command_buffers_unused.append(vk_command_buffer);
  });
  vkFreeCommandBuffers(device->vk_device_,
                       vk_command_pool,
                       command_buffers_unused.size(),
                       command_buffers_unused.data());
  vkDestroyCommandPool(device->vk_device_, vk_command_pool, nullptr);
  CLOG_TRACE(&LOG, "Submission runner finished");
}

void VKDevice::init_submission_pool()
{
  CLOG_TRACE(&LOG, "Create submission pool");
  submission_pool_ = BLI_task_pool_create_background_serial(this, TASK_PRIORITY_HIGH);
  submitted_render_graphs_ = BLI_thread_queue_init();
  unused_render_graphs_ = BLI_thread_queue_init();

  VkSemaphoreTypeCreateInfo vk_semaphore_type_create_info = {
      VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE, 0};
  VkSemaphoreCreateInfo vk_semaphore_create_info = {
      VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &vk_semaphore_type_create_info, 0};
  vkCreateSemaphore(vk_device_, &vk_semaphore_create_info, nullptr, &vk_timeline_semaphore_);

  BLI_task_pool_push(submission_pool_, VKDevice::submission_runner, nullptr, false, nullptr);
}

void VKDevice::deinit_submission_pool()
{
  CLOG_TRACE(&LOG, "Cancelling submission pool");
  BLI_task_pool_cancel(submission_pool_);
  CLOG_TRACE(&LOG, "Waiting for completion");
  BLI_task_pool_work_and_wait(submission_pool_);
  CLOG_TRACE(&LOG, "Freeing submission pool");
  BLI_task_pool_free(submission_pool_);
  submission_pool_ = nullptr;

  while (!BLI_thread_queue_is_empty(submitted_render_graphs_)) {
    VKRenderGraphSubmitTask *submit_task = static_cast<VKRenderGraphSubmitTask *>(
        BLI_thread_queue_pop(submitted_render_graphs_));
    MEM_delete<VKRenderGraphSubmitTask>(submit_task);
  }
  BLI_thread_queue_free(submitted_render_graphs_);
  submitted_render_graphs_ = nullptr;
  BLI_thread_queue_free(unused_render_graphs_);
  unused_render_graphs_ = nullptr;

  vkDestroySemaphore(vk_device_, vk_timeline_semaphore_, nullptr);
  vk_timeline_semaphore_ = VK_NULL_HANDLE;
}

/** \} */

}  // namespace gpu
}  // namespace blender
