/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "vk_query.hh"
#include "vk_backend.hh"
#include "vk_context.hh"

#include "GPU_debug.hh"

namespace blender::gpu {

VKQueryPool::~VKQueryPool()
{
  VKBackend &backend = VKBackend::get();
  const VKDevice &device = backend.device;

  while (!vk_query_pools_.is_empty()) {
    VkQueryPool vk_query_pool = vk_query_pools_.pop_last();
    if (timestamp_bits_ > 0) {
      VKDiscardPool::discard_pool_get().discard_query_pool(vk_query_pool);
    }
    else {
      device.functions.vkDestroyQueryPool(device.vk_handle(), vk_query_pool, nullptr);
    }
  }
}

uint32_t VKQueryPool::query_index_in_pool() const
{
  return queries_issued_ - (vk_query_pools_.size() - 1) * query_chunk_len_;
}

int VKQueryPool::timestamp_write()
{
  const VKDevice &device = VKBackend::get().device;
  if (timestamp_bits_ == -1) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device.physical_device_get(), &count, nullptr);
    Vector<VkQueueFamilyProperties> properties(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device.physical_device_get(), &count, properties.data());
    timestamp_bits_ = properties[device.queue_family_get()].timestampValidBits;
  }
  if (!timestamp_bits_) {
    return -1;
  }
  const uint32_t index = queries_issued_ % query_chunk_len_;
  auto &graph = VKContext::get()->render_graph();
  if (index == 0) {
    VkQueryPoolCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = query_chunk_len_;
    VkQueryPool pool;
    if (device.functions.vkCreateQueryPool(device.vk_handle(), &info, nullptr, &pool) != VK_SUCCESS) {
      return -1;
    }
    vk_query_pools_.append(pool);
    graph.add_node(render_graph::VKResetQueryPoolNode::Data{pool, 0, query_chunk_len_});
  }
  graph.add_node(render_graph::VKWriteTimestampNode::Data{vk_query_pools_.last(), index});
  return queries_issued_++;
}

void VKQueryPool::timestamp_resolve()
{
  const VKDevice &device = VKBackend::get().device;
  timestamps_.resize(queries_issued_, {0, 0});
  for (int pool : vk_query_pools_.index_range()) {
    const uint32_t first = pool * query_chunk_len_;
    const uint32_t count = std::min(query_chunk_len_, queries_issued_ - first);
    auto *data = &timestamps_[first];
    const VkResult result = device.functions.vkGetQueryPoolResults(
        device.vk_handle(), vk_query_pools_[pool], 0, count, count * sizeof(*data), data,
        sizeof(*data), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (result != VK_SUCCESS && result != VK_NOT_READY) {
      timestamps_.clear();
      return;
    }
  }
}

bool VKQueryPool::timestamp_duration(int begin, int end, double &ms) const
{
  if (end >= timestamps_.size() || !timestamps_[begin][1] || !timestamps_[end][1]) {
    return false;
  }
  const uint64_t mask = UINT64_MAX >> (64 - timestamp_bits_);
  ms = double((timestamps_[end][0] - timestamps_[begin][0]) & mask) *
       VKBackend::get().device.physical_device_properties_get().limits.timestampPeriod * 1e-6;
  return true;
}

void VKQueryPool::init(GPUQueryType type)
{
  BLI_assert(vk_query_pools_.is_empty());
  queries_issued_ = 0;
  vk_query_type_ = to_vk_query_type(type);
}

void VKQueryPool::begin_query()
{
  VKBackend &backend = VKBackend::get();
  const VKDevice &device = backend.device;

  uint32_t pool_index = queries_issued_ / query_chunk_len_;
  bool is_new_pool = (queries_issued_ % query_chunk_len_) == 0;

  if (pool_index == vk_query_pools_.size()) {
    BLI_assert(is_new_pool);

    VkQueryPoolCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    create_info.queryType = vk_query_type_;
    create_info.queryCount = query_chunk_len_;

    VkQueryPool vk_query_pool = VK_NULL_HANDLE;
    device.functions.vkCreateQueryPool(device.vk_handle(), &create_info, nullptr, &vk_query_pool);
    vk_query_pools_.append(vk_query_pool);
  }
  BLI_assert(pool_index < vk_query_pools_.size());

  /* When using a new query pool make sure to reset it before first usage. */
  VKContext &context = *VKContext::get();
  VkQueryPool vk_query_pool = vk_query_pools_[pool_index];
  if (is_new_pool) {
    render_graph::VKResetQueryPoolNode::Data reset_query_pool = {};
    reset_query_pool.vk_query_pool = vk_query_pool;
    reset_query_pool.first_query = 0;
    reset_query_pool.query_count = query_chunk_len_;
    context.render_graph().add_node(reset_query_pool);
  }

  render_graph::VKBeginQueryNode::Data begin_query = {};
  begin_query.vk_query_pool = vk_query_pool;
  begin_query.query_index = query_index_in_pool();
  context.render_graph().add_node(begin_query);
}

void VKQueryPool::end_query()
{
  VKContext &context = *VKContext::get();
  render_graph::VKEndQueryNode::Data end_query = {};
  end_query.vk_query_pool = vk_query_pools_.last();
  end_query.query_index = query_index_in_pool();
  context.render_graph().add_node(end_query);
  queries_issued_ += 1;
}

void VKQueryPool::get_occlusion_result(MutableSpan<uint32_t> r_values)
{
  VKContext &context = *VKContext::get();
  /* During selection the frame buffer is still rendering. It needs to finish the render scope to
   * ensure the END_RENDERING node */
  context.rendering_end();
  const TimelineValue timeline = context.flush_render_graph(
      RenderGraphFlushFlags::SUBMIT | RenderGraphFlushFlags::WAIT_FOR_COMPLETION |
      RenderGraphFlushFlags::RENEW_RENDER_GRAPH);
  if (timeline == 0) {
    r_values.fill(0);
    return;
  }

  int queries_left = queries_issued_;
  int pool_index = 0;
  VKBackend &backend = VKBackend::get();
  VKDevice &device = backend.device;
  while (queries_left) {
    VkQueryPool vk_query_pool = vk_query_pools_[pool_index];
    uint32_t *r_values_chunk = &r_values[pool_index * query_chunk_len_];
    uint32_t values_chunk_size = min_ii(queries_left, query_chunk_len_);
    const VkResult result = device.functions.vkGetQueryPoolResults(device.vk_handle(),
                                                  vk_query_pool,
                                                  0,
                                                  values_chunk_size,
                                                  values_chunk_size * sizeof(uint32_t),
                                                  r_values_chunk,
                                                  sizeof(uint32_t),
                                                  VK_QUERY_RESULT_WAIT_BIT);
    device.submission_error_set(result, "vkGetQueryPoolResults");
    if (result != VK_SUCCESS) {
      r_values.fill(0);
      return;
    }

    queries_left = max_ii(queries_left - query_chunk_len_, 0);
    pool_index += 1;
  }
}

}  // namespace blender::gpu
