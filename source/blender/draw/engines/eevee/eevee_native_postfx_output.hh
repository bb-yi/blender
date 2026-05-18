/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 */

#pragma once

#include <array>

#include "DNA_layer_types.h"
#include "DNA_node_types.h"

#include "BLI_vector.hh"

#include "DRW_render.hh"

#include "eevee_depth_of_field.hh"
#include "eevee_film_shared.hh"

namespace blender::eevee {

class Instance;

class NativePostFXOutputModule {
 public:
  static constexpr int output_max = 32;

  struct RuntimeOutput {
    ViewLayerNativePostFXOutput *data = nullptr;
    ePassStorageType storage_type = PASS_STORAGE_COLOR;
    int source_index = -1;
    bool source_is_value = false;
    int color_index = -1;
    int value_index = -1;
    int channels = 4;
    const char *chan_id = "RGBA";
    eNodeSocketDatatype socket_type = SOCK_RGBA;
  };

 private:
  Instance &inst_;

  Vector<RuntimeOutput> outputs_;
  int color_len_ = 0;
  int value_len_ = 0;
  uint64_t outputs_hash_ = 0;
  bool requires_outline_source_ = false;

  TextureFromPool source_tx_ = {"native_postfx_source"};
  TextureFromPool effect_tx_ = {"native_postfx_effect"};
  TextureFromPool velocity_tx_ = {"native_postfx_velocity"};

  TextureFromPool default_outline_tx_ = {"outline_camera_fx"};
  TextureFromPool default_outline_effect_tx_ = {"outline_camera_fx_effect"};
  TextureFromPool default_outline_velocity_tx_ = {"outline_camera_fx_velocity"};

  PassSimple extract_ps_ = {"NativePostFXOutput.Extract"};
  PassSimple pack_color_ps_ = {"NativePostFXOutput.PackColor"};
  PassSimple pack_value_ps_ = {"NativePostFXOutput.PackValue"};

  std::array<DepthOfFieldBuffer, output_max> dof_buffers_;
  std::array<uint64_t, output_max> dof_signatures_ = {};
  DepthOfFieldBuffer default_outline_dof_buffer_;

 public:
  NativePostFXOutputModule(Instance &inst) : inst_(inst) {}

  void init();
  eViewLayerEEVEEPassType required_passes_get() const;

  int color_len_get() const
  {
    return color_len_;
  }

  int value_len_get() const
  {
    return value_len_;
  }

  uint64_t outputs_hash_get() const
  {
    return outputs_hash_;
  }

  Span<RuntimeOutput> outputs_get()
  {
    return outputs_.as_span();
  }

  Span<const RuntimeOutput> outputs_get() const
  {
    return Span<const RuntimeOutput>(outputs_.data(), outputs_.size());
  }

  bool requires_outline_source() const
  {
    return requires_outline_source_;
  }

  void render(View &view);
  gpu::Texture *render_outline_for_combined(View &view, gpu::Texture *outline_tx);
  void release();

  static ePassStorageType output_storage_type(const ViewLayerNativePostFXOutput &output,
                                              const ViewLayer *view_layer);
  static void output_render_pass_info(const ViewLayerNativePostFXOutput &output,
                                      const ViewLayer *view_layer,
                                      int &r_channels,
                                      const char *&r_chan_id,
                                      eNodeSocketDatatype &r_socket_type);

 private:
  static bool output_is_enabled_and_valid(const ViewLayerNativePostFXOutput &output);
  static eViewLayerEEVEEPassType source_pass_bit(int source);
  static uint64_t output_signature(const RuntimeOutput &output);

  bool resolve_source(RuntimeOutput &output);
  void extract_source(const RuntimeOutput &output, gpu::Texture *output_tx);
  gpu::Texture *apply_camera_fx(View &view,
                                gpu::Texture *input_tx,
                                gpu::Texture *output_tx,
                                gpu::Texture *velocity_tx,
                                DepthOfFieldBuffer &dof_buffer,
                                const ViewLayerNativePostFXOutput &output);
  void pack_output(const RuntimeOutput &output, gpu::Texture *input_tx);
};

}  // namespace blender::eevee
