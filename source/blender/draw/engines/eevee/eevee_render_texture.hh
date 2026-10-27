/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 */

#pragma once

#include <algorithm>
#include <array>
#include <memory>

#include "BLI_set.hh"
#include "BLI_span.hh"
#include "DNA_scene_types.h"

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
  enum SlotTextureRecreate : uint8_t {
    SLOT_TEXTURE_CURRENT_RECREATED = 1u << 0u,
    SLOT_TEXTURE_PREVIOUS_RECREATED = 1u << 1u,
  };

  struct RuntimeSlot {
    int uid = -1;
    Object *camera = nullptr;
    Object *camera_orig = nullptr;
    int2 extent = int2(1);
    int source = 0;
    int format = 0;
    int update_mode = SCE_EEVEE_RENDER_TEXTURE_UPDATE_EVERY_FRAME;
    int manual_update = 0;
    bool capture_pending = true;
    bool active = false;
    bool used = false;
    bool history_reset_pending = true;
    bool has_camera_data = false;
    CameraData camera_data = {};
    SwapChain<Texture, 2> color_tx;
    std::unique_ptr<RayTraceBuffer> rt_buffer_opaque;
    std::unique_ptr<RayTraceBuffer> rt_buffer_refract;
  };

  Instance &inst_;

  RenderTextureDataBuf data_ = {"render_texture_buf"};
  std::array<RuntimeSlot, RENDER_TEXTURE_SLOT_MAX> slots_;
  Set<int> used_uids_;
  bool capture_has_texture_dependencies_ = false;
  bool requires_full_capture_ = false;
  uint64_t last_evaluation_ = 0;
  float last_time_ = 0.0f;
  bool has_evaluation_ = false;

  Framebuffer prepass_fb_ = {"RenderTexture.Prepass"};
  Framebuffer combined_fb_ = {"RenderTexture.Combined"};
  Framebuffer transparent_fb_ = {"RenderTexture.Transparent"};
  Framebuffer gbuffer_fb_ = {"RenderTexture.GBuffer"};
  PassSimple extract_ps_ = {"RenderTexture.Extract"};

 public:
  RenderTextureModule(Instance &inst) : inst_(inst) {}

  void init();
  void begin_sync();
  void render();
  void end_sync();
  void material_sync(GPUMaterial *material, bool is_capture_material = true);

  bool has_active() const
  {
    return std::any_of(slots_.begin(), slots_.end(), [](const RuntimeSlot &slot) {
      return slot.active && slot.used;
    });
  }

  /* Object sync happens before material demand is finalized. Include every enabled camera in
   * conservative geometry budgets so newly referenced captures cannot lose geometry. */
  bool has_enabled() const
  {
    return std::any_of(slots_.begin(), slots_.end(), [](const RuntimeSlot &slot) {
      return slot.active;
    });
  }

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
  uint8_t slot_ensure_textures(int slot_index);
  void slot_extract(int slot_index, RenderBuffers &rbufs);
  void slot_capture(Span<int> capture_slots);
  bool slot_should_capture(const RuntimeSlot &slot) const;
};

}  // namespace blender::eevee
