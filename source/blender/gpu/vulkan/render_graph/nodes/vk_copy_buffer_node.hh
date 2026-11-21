/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "vk_node_info.hh"

namespace blender::gpu::render_graph {
/**
 * Information stored inside the render graph node. See `VKRenderGraphNode`.
 */
struct VKCopyBufferData {
  VKResourceWithHandle<VkBuffer> src_buffer;
  VKResourceWithHandle<VkBuffer> dst_buffer;
  VkBufferCopy region;
  bool dst_host_read = false;
};

class VKCopyBufferNode : public VKNodeInfo<VKNodeType::COPY_BUFFER,
                                           VKCopyBufferData,
                                           VKCopyBufferData,
                                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                                           VKResourceType::BUFFER> {
 public:
  /**
   * Update the node data with the data inside create_info.
   *
   * Has been implemented as a template to ensure all node specific data
   * (`VK*Data`/`VK*CreateInfo`) types can be included in the same header file as the logic. The
   * actual node data (`VKRenderGraphNode` includes all header files.)
   */
  template<typename Node, typename Storage>
  static void set_node_data(Node &node, Storage & /* storage */, const CreateInfo &create_info)
  {
    node.copy_buffer = create_info;
  }

  /**
   * Extract read/write resource dependencies from `create_info` and add them to `node_links`.
   */
  void build_links(VKResourceStateTracker &resources,
                   VKRenderGraphLinks &links,
                   const CreateInfo &create_info) override
  {
    ResourceWithStamp src_resource = resources.get_buffer(create_info.src_buffer);
    ResourceWithStamp dst_resource = resources.get_buffer_and_increase_stamp(
        create_info.dst_buffer);
    links.buffers.append({src_resource, VK_ACCESS_TRANSFER_READ_BIT});
    links.buffers.append({dst_resource, VK_ACCESS_TRANSFER_WRITE_BIT});
  }

  /**
   * Build the commands and add them to the command_buffer.
   */
  void build_commands(VKCommandBufferInterface &command_buffer,
                      Data &data,
                      Span<uint8_t> /*storage_push_constants*/,
                      VKBoundPipelines & /*r_bound_pipelines*/) override
  {
    command_buffer.copy_buffer(
        data.src_buffer.vk_handle, data.dst_buffer.vk_handle, 1, &data.region);
    if (data.dst_host_read) {
      /* Make the copy available to the host before the submission timeline is signaled. Keep
       * the barrier in this node so it follows the copy even if the graph moves the node. */
      const VkBufferMemoryBarrier barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                            nullptr,
                                            VK_ACCESS_TRANSFER_WRITE_BIT,
                                            VK_ACCESS_HOST_READ_BIT,
                                            VK_QUEUE_FAMILY_IGNORED,
                                            VK_QUEUE_FAMILY_IGNORED,
                                            data.dst_buffer.vk_handle,
                                            data.region.dstOffset,
                                            data.region.size};
      command_buffer.pipeline_barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      VK_PIPELINE_STAGE_HOST_BIT,
                                      0,
                                      0,
                                      nullptr,
                                      1,
                                      &barrier,
                                      0,
                                      nullptr);
    }
  }
};
}  // namespace blender::gpu::render_graph
