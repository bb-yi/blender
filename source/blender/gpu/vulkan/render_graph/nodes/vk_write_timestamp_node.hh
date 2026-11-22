#pragma once

#include "vk_node_info.hh"

namespace blender::gpu::render_graph {

struct VKWriteTimestampData {
  VkQueryPool pool;
  uint32_t index;
};

class VKWriteTimestampNode : public VKNodeInfo<VKNodeType::WRITE_TIMESTAMP,
                                              VKWriteTimestampData,
                                              VKWriteTimestampData,
                                              VK_PIPELINE_STAGE_NONE,
                                              VKResourceType::NONE> {
 public:
  template<typename Node, typename Storage>
  void set_node_data(Node &node, Storage &, const CreateInfo &info)
  {
    node.write_timestamp = info;
  }
  void build_links(VKResourceStateTracker &, VKRenderGraphLinks &, const CreateInfo &) override {}
  void build_commands(VKCommandBufferInterface &commands, Data &data, Span<uint8_t>,
                      VKBoundPipelines &) override
  {
    commands.write_timestamp(data.pool, data.index);
  }
};

}  // namespace blender::gpu::render_graph
