/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "BKE_global.hh"
#include "BLI_memory_utils.hh"
#include "BLI_mutex.hh"
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
  bool is_done = false;
};

/* Only used by isolated background regression processes. Never reset the physical GPU. */
static bool submission_test_failure(const char *operation)
{
  const char *failure = std::getenv("BLENDER_VULKAN_TEST_FAILURE");
  return G.background && failure && std::strcmp(failure, operation) == 0;
}

void VKDevice::submission_error_set(VkResult result, const char *operation, TimelineValue timeline)
{
  if (result == VK_SUCCESS) {
    return;
  }
  VkResult expected = VK_SUCCESS;
  if (submission_error_.compare_exchange_strong(expected, result)) {
    CLOG_ERROR(&LOG,
               "Vulkan GPU failure: %s [%s], timeline=%llu. Submissions stopped; restart Blender.",
               operation,
               to_string(result),
               static_cast<unsigned long long>(timeline));
  }
}

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
  /* Observe failed submissions before waiting on a semaphore they could not signal. Also retire
   * the CPU task before callers begin error cleanup. */
  wait_for_submission |= wait_for_completion;

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
    wait_condition.is_submitted_condition.wait(lock, [&] { return wait_condition.is_done; });
  }

  if (submission_failed() || (wait_for_completion && !wait_for_timeline(timeline))) {
    return 0;
  }
  return timeline;
}

bool VKDevice::wait_for_timeline(TimelineValue timeline)
{
  if (submission_failed()) {
    return false;
  }
  if (timeline == 0) {
    return true;
  }
  VkSemaphoreWaitInfo vk_semaphore_wait_info = {
      VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &vk_timeline_semaphore_, &timeline};
  while (!submission_failed()) {
    /* A submission on another thread can fail while this wait is pending. */
    VkResult result = vkWaitSemaphores(vk_device_, &vk_semaphore_wait_info, 50000000);
    if (result == VK_TIMEOUT) {
      continue;
    }
    if (result == VK_SUCCESS && submission_test_failure("wait")) {
      result = VK_ERROR_DEVICE_LOST;
    }
    submission_error_set(result, "vkWaitSemaphores", timeline);
    return result == VK_SUCCESS && !submission_failed();
  }
  return false;
}

bool VKDevice::wait_queue_idle()
{
  std::scoped_lock lock(*queue_mutex_);
  const VkResult result = vkQueueWaitIdle(vk_queue_);
  submission_error_set(result, "vkQueueWaitIdle");
  return result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST;
}

bool VKDevice::submission_finished_timeline_get(TimelineValue &r_timeline)
{
  r_timeline = 0;
  if (submission_failed()) {
    return false;
  }
  TimelineValue current_timeline = 0;
  VkResult result = vkGetSemaphoreCounterValue(
      vk_device_, vk_timeline_semaphore_, &current_timeline);
  if (result == VK_SUCCESS && submission_test_failure("counter")) {
    result = VK_ERROR_DEVICE_LOST;
  }
  if (submission_test_failure("counter_max")) {
    current_timeline = UINT64_MAX;
  }
  /* Some drivers return SUCCESS and UINT64_MAX during a reset. Never treat it as completion. */
  if (result == VK_SUCCESS && current_timeline == UINT64_MAX) {
    result = VK_ERROR_DEVICE_LOST;
  }
  if (result == VK_SUCCESS) {
    std::scoped_lock lock(orphaned_data.mutex_get());
    if (current_timeline > timeline_value_) {
      result = VK_ERROR_DEVICE_LOST;
    }
  }
  submission_error_set(result, "vkGetSemaphoreCounterValue", current_timeline);
  if (result != VK_SUCCESS) {
    return false;
  }
  r_timeline = current_timeline;
  return true;
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
  if (!device->submission_failed()) {
    device->submission_error_set(
        vkCreateCommandPool(
            device->vk_device_, &vk_command_pool_create_info, nullptr, &vk_command_pool),
        "vkCreateCommandPool");
  }

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
    BLI_SCOPED_DEFER([&]() {
      submit_task->render_graph->reset();
      BLI_thread_queue_push(device->unused_render_graphs_,
                            submit_task->render_graph,
                            BLI_THREAD_QUEUE_WORK_PRIORITY_NORMAL);
      if (submit_task->wait_for_submission != nullptr) {
        std::unique_lock<Mutex> lock(submit_task->wait_for_submission->is_submitted_mutex);
        submit_task->wait_for_submission->is_done = true;
        submit_task->wait_for_submission->is_submitted_condition.notify_one();
      }
      MEM_delete<VKRenderGraphSubmitTask>(submit_task);
    });
    uint64_t current_timeline = 0;
    if (!device->submission_finished_timeline_get(current_timeline)) {
      continue;
    }
    if (current_timeline < previous_gc_timeline) {
      device->submission_error_set(
          VK_ERROR_DEVICE_LOST, "timeline moved backwards", current_timeline);
      continue;
    }
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
        const VkResult result = vkAllocateCommandBuffers(
            device->vk_device_, &vk_command_buffer_allocate_info, command_buffers_unused.data());
        device->submission_error_set(result, "vkAllocateCommandBuffers", submit_task->timeline);
        if (result != VK_SUCCESS) {
          command_buffers_unused.clear();
          continue;
        }
      };

      vk_command_buffer = command_buffers_unused.pop_last();
      command_buffer = std::make_optional<render_graph::VKCommandBufferWrapper>(
          vk_command_buffer, device->extensions_);
      command_buffer->begin_recording();
      if (device->submission_failed()) {
        continue;
      }
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
      if (device->submission_failed()) {
        continue;
      }

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

      {
        std::scoped_lock lock_queue(*device->queue_mutex_);
        if (device->submission_failed()) {
          continue;
        }
        const bool inject_failure = submission_test_failure("submit") ||
                                    (submit_task->signal_semaphore != VK_NULL_HANDLE &&
                                     submission_test_failure("external_signal"));
        const VkResult result = inject_failure ? VK_ERROR_DEVICE_LOST :
                                                 vkQueueSubmit(device->vk_queue_,
                                                               1,
                                                               &vk_submit_info,
                                                               submit_task->signal_fence);
        device->submission_error_set(result, "vkQueueSubmit", submit_task->timeline);
        if (result != VK_SUCCESS) {
          CLOG_ERROR(&LOG,
                     "Failed submission: external wait=%llu signal=%llu, nodes=%llu",
                     static_cast<unsigned long long>(submit_task->wait_semaphore_value),
                     static_cast<unsigned long long>(submit_task->signal_semaphore_value),
                     static_cast<unsigned long long>(node_handles.size()));
          continue;
        }
      }
      command_buffers_in_use.append_timeline(submit_task->timeline, vk_command_buffer);
      vk_command_buffer = VK_NULL_HANDLE;
      command_buffer.reset();
    }
  }
  CLOG_TRACE(&LOG, "Submission runner is being canceled");

  /* Clear command buffers and pool */
  {
    std::scoped_lock lock(*device->queue_mutex_);
    device->submission_error_set(vkDeviceWaitIdle(device->vk_device_), "vkDeviceWaitIdle");
  }
  command_buffers_in_use.remove_old(UINT64_MAX, [&](VkCommandBuffer vk_command_buffer) {
    command_buffers_unused.append(vk_command_buffer);
  });
  if (vk_command_pool != VK_NULL_HANDLE) {
    vkFreeCommandBuffers(device->vk_device_,
                         vk_command_pool,
                         command_buffers_unused.size(),
                         command_buffers_unused.data());
    vkDestroyCommandPool(device->vk_device_, vk_command_pool, nullptr);
  }
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
  submission_error_.store(VK_SUCCESS);
  submission_error_set(
      vkCreateSemaphore(vk_device_, &vk_semaphore_create_info, nullptr, &vk_timeline_semaphore_),
      "vkCreateSemaphore");

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
