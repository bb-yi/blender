/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 */

#pragma once

#include <array>

#include "DRW_render.hh"

#include "eevee_camera.hh"
#include "eevee_raytrace.hh"
#include "eevee_render_texture_shared.hh"

namespace blender::eevee {

class Instance;
class RenderBuffers;

using RenderTextureDataBuf = draw::StorageArrayBuffer<RenderTextureData, RENDER_TEXTURE_SLOT_MAX>;

class RenderTextureModule {
 private:
  struct RuntimeSlot {
    int uid = -1;
    Object *camera = nullptr;
    int2 extent = int2(1);
    int source = 0;
    int format = 0;
    bool active = false;
    SwapChain<Texture, 2> color_tx;
  };

  Instance &inst_;

  RenderTextureDataBuf data_ = {"render_texture_buf"};
  std::array<RuntimeSlot, RENDER_TEXTURE_SLOT_MAX> slots_;

  Framebuffer prepass_fb_ = {"RenderTexture.Prepass"};
  Framebuffer combined_fb_ = {"RenderTexture.Combined"};
  Framebuffer transparent_fb_ = {"RenderTexture.Transparent"};
  Framebuffer gbuffer_fb_ = {"RenderTexture.GBuffer"};
  RayTraceBuffer rt_buffer_opaque_;
  RayTraceBuffer rt_buffer_refract_;
  PassSimple extract_ps_ = {"RenderTexture.Extract"};

 public:
  RenderTextureModule(Instance &inst) : inst_(inst) {}

  void init();
  void begin_sync();
  void render();
  void end_sync() {}

  template<typename PassType> void bind_resources(PassType &pass)
  {
    pass.bind_ssbo(RENDER_TEXTURE_BUF_SLOT, &data_);
    for (int slot_index = 0; slot_index < RENDER_TEXTURE_SLOT_MAX; slot_index++) {
      pass.bind_texture(RENDER_TEXTURE_COLOR_TX_SLOT_0 + slot_index,
                        &slots_[slot_index].color_tx.current());
      pass.bind_texture(RENDER_TEXTURE_HISTORY_TX_SLOT_0 + slot_index,
                        &slots_[slot_index].color_tx.previous());
    }
  }

 private:
  static RenderTextureData slot_default_data();
  void slot_reset(int slot_index);
  void slot_ensure_textures(int slot_index);
  void slot_extract(int slot_index, RenderBuffers &rbufs);
  void slot_capture(int slot_index);
};

}  // namespace blender::eevee
