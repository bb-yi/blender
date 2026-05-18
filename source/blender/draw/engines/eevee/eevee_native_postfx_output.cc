/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 */

#include "BLI_hash.h"
#include "BLI_listbase.h"
#include "BLI_string.h"

#include "GPU_texture.hh"
#include "GPU_state.hh"

#include "eevee_instance.hh"
#include "eevee_native_postfx_output.hh"

namespace blender::eevee {

static void velocity_work_texture_release(TextureFromPool &texture, gpu::TextureFormat format)
{
  if (format == gpu::TextureFormat::SFLOAT_16_16 && texture.is_valid()) {
    GPU_texture_swizzle_set(texture, "rgba");
  }
  texture.release();
}

enum eNativePostFXExtractSource : int {
  NATIVE_POSTFX_EXTRACT_DEPTH = 0,
  NATIVE_POSTFX_EXTRACT_VECTOR = 1,
  NATIVE_POSTFX_EXTRACT_COLOR_PASS = 2,
  NATIVE_POSTFX_EXTRACT_VALUE_PASS = 3,
  NATIVE_POSTFX_EXTRACT_OUTLINE = 4,
};

bool NativePostFXOutputModule::output_is_enabled_and_valid(
    const ViewLayerNativePostFXOutput &output)
{
  const int invalid_flags = VIEW_LAYER_NATIVE_POSTFX_OUTPUT_CONFLICT |
                            VIEW_LAYER_NATIVE_POSTFX_OUTPUT_SOURCE_INVALID;
  return (output.flag & VIEW_LAYER_NATIVE_POSTFX_OUTPUT_ENABLED) && (output.flag & invalid_flags) == 0;
}

eViewLayerEEVEEPassType NativePostFXOutputModule::source_pass_bit(const int source)
{
  switch (source) {
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_DEPTH:
      return EEVEE_RENDER_PASS_DEPTH;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_NORMAL:
      return EEVEE_RENDER_PASS_NORMAL;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_POSITION:
      return EEVEE_RENDER_PASS_POSITION;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_VECTOR:
      return EEVEE_RENDER_PASS_VECTOR;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_DIFFUSE_LIGHT:
      return EEVEE_RENDER_PASS_DIFFUSE_LIGHT;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_DIFFUSE_COLOR:
      return EEVEE_RENDER_PASS_DIFFUSE_COLOR;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_SPECULAR_LIGHT:
      return EEVEE_RENDER_PASS_SPECULAR_LIGHT;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_SPECULAR_COLOR:
      return EEVEE_RENDER_PASS_SPECULAR_COLOR;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_VOLUME_LIGHT:
      return EEVEE_RENDER_PASS_VOLUME_LIGHT;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_EMISSION:
      return EEVEE_RENDER_PASS_EMIT;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_ENVIRONMENT:
      return EEVEE_RENDER_PASS_ENVIRONMENT;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_SHADOW:
      return EEVEE_RENDER_PASS_SHADOW;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_AO:
      return EEVEE_RENDER_PASS_AO;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_TRANSPARENT:
      return EEVEE_RENDER_PASS_TRANSPARENT;
    default:
      return eViewLayerEEVEEPassType(0);
  }
}

ePassStorageType NativePostFXOutputModule::output_storage_type(
    const ViewLayerNativePostFXOutput &output, const ViewLayer *view_layer)
{
  if (output.source == VIEW_LAYER_NATIVE_POSTFX_SOURCE_AOV) {
    const ViewLayerAOV *aov = static_cast<const ViewLayerAOV *>(
        BLI_findstring(&view_layer->aovs, output.source_aov, offsetof(ViewLayerAOV, name)));
    return (aov != nullptr && aov->type == AOV_TYPE_VALUE) ? PASS_STORAGE_VALUE :
                                                             PASS_STORAGE_COLOR;
  }
  if (output.source == VIEW_LAYER_NATIVE_POSTFX_SOURCE_DEPTH) {
    return PASS_STORAGE_VALUE;
  }
  return PASS_STORAGE_COLOR;
}

void NativePostFXOutputModule::output_render_pass_info(
    const ViewLayerNativePostFXOutput &output,
    const ViewLayer *view_layer,
    int &r_channels,
    const char *&r_chan_id,
    eNodeSocketDatatype &r_socket_type)
{
  if (output_storage_type(output, view_layer) == PASS_STORAGE_VALUE) {
    r_channels = 1;
    r_chan_id = (output.source == VIEW_LAYER_NATIVE_POSTFX_SOURCE_DEPTH) ? "Z" : "X";
    r_socket_type = SOCK_FLOAT;
    return;
  }

  switch (output.source) {
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_NORMAL:
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_POSITION:
      r_channels = 3;
      r_chan_id = "XYZ";
      r_socket_type = SOCK_VECTOR;
      break;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_VECTOR:
      r_channels = 4;
      r_chan_id = "XYZW";
      r_socket_type = SOCK_VECTOR;
      break;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_TRANSPARENT:
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_OUTLINE:
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_AOV:
      r_channels = 4;
      r_chan_id = "RGBA";
      r_socket_type = SOCK_RGBA;
      break;
    default:
      r_channels = 3;
      r_chan_id = "RGB";
      r_socket_type = SOCK_RGBA;
      break;
  }
}

void NativePostFXOutputModule::init()
{
  outputs_.clear();
  color_len_ = 0;
  value_len_ = 0;
  outputs_hash_ = 0;
  requires_outline_source_ = false;

  int output_index = 0;
  for (ViewLayerNativePostFXOutput &output : inst_.view_layer->native_postfx_outputs) {
    if (!output_is_enabled_and_valid(output) || output_index >= output_max) {
      continue;
    }

    RuntimeOutput runtime;
    runtime.data = &output;
    runtime.storage_type = output_storage_type(output, inst_.view_layer);
    output_render_pass_info(output,
                            inst_.view_layer,
                            runtime.channels,
                            runtime.chan_id,
                            runtime.socket_type);
    if (runtime.storage_type == PASS_STORAGE_VALUE) {
      runtime.value_index = value_len_++;
    }
    else {
      runtime.color_index = color_len_++;
    }
    requires_outline_source_ |= output.source == VIEW_LAYER_NATIVE_POSTFX_SOURCE_OUTLINE;
    outputs_hash_ = (outputs_hash_ * 33u) ^ BLI_hash_string(output.name);
    outputs_hash_ = (outputs_hash_ * 33u) ^ uint64_t(output.source);
    outputs_hash_ = (outputs_hash_ * 33u) ^ uint64_t(output.effects);
    outputs_hash_ = (outputs_hash_ * 33u) ^ uint64_t(runtime.storage_type);
    if (output.source == VIEW_LAYER_NATIVE_POSTFX_SOURCE_AOV) {
      outputs_hash_ = (outputs_hash_ * 33u) ^ BLI_hash_string(output.source_aov);
    }
    outputs_.append(runtime);
    output_index++;
  }
}

eViewLayerEEVEEPassType NativePostFXOutputModule::required_passes_get() const
{
  eViewLayerEEVEEPassType result = eViewLayerEEVEEPassType(0);
  for (const RuntimeOutput &output : outputs_) {
    result |= source_pass_bit(output.data->source);
  }
  return result;
}

bool NativePostFXOutputModule::resolve_source(RuntimeOutput &output)
{
  RenderBuffersInfoData &rbuf_data = inst_.render_buffers.data;
  output.source_index = -1;
  output.source_is_value = false;

  auto set_color = [&](const int index) {
    output.source_index = index;
    output.source_is_value = false;
  };
  auto set_value = [&](const int index) {
    output.source_index = index;
    output.source_is_value = true;
  };

  switch (output.data->source) {
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_DEPTH:
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_VECTOR:
      return true;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_NORMAL:
      set_color(rbuf_data.normal_id);
      break;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_POSITION:
      set_color(rbuf_data.position_id);
      break;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_DIFFUSE_LIGHT:
      set_color(rbuf_data.diffuse_light_id);
      break;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_DIFFUSE_COLOR:
      set_color(rbuf_data.diffuse_color_id);
      break;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_SPECULAR_LIGHT:
      set_color(rbuf_data.specular_light_id);
      break;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_SPECULAR_COLOR:
      set_color(rbuf_data.specular_color_id);
      break;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_VOLUME_LIGHT:
      set_color(rbuf_data.volume_light_id);
      break;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_EMISSION:
      set_color(rbuf_data.emission_id);
      break;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_ENVIRONMENT:
      set_color(rbuf_data.environment_id);
      break;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_SHADOW:
      set_value(rbuf_data.shadow_id);
      break;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_AO:
      set_value(rbuf_data.ambient_occlusion_id);
      break;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_TRANSPARENT:
      set_color(rbuf_data.transparent_id);
      break;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_OUTLINE:
      return inst_.outline.resolved_texture() != nullptr;
    case VIEW_LAYER_NATIVE_POSTFX_SOURCE_AOV: {
      int color_index = 0;
      int value_index = 0;
      for (ViewLayerAOV &aov : inst_.view_layer->aovs) {
        if ((aov.flag & AOV_CONFLICT) != 0) {
          continue;
        }
        if (STREQ(aov.name, output.data->source_aov)) {
          if (aov.type == AOV_TYPE_VALUE) {
            set_value(rbuf_data.value_len + value_index);
          }
          else {
            set_color(rbuf_data.color_len + color_index);
          }
          break;
        }
        if (aov.type == AOV_TYPE_VALUE) {
          value_index++;
        }
        else {
          color_index++;
        }
      }
      break;
    }
    default:
      return false;
  }

  return output.source_index != -1;
}

void NativePostFXOutputModule::extract_source(const RuntimeOutput &output, gpu::Texture *output_tx)
{
  int source_kind = NATIVE_POSTFX_EXTRACT_COLOR_PASS;
  if (output.data->source == VIEW_LAYER_NATIVE_POSTFX_SOURCE_DEPTH) {
    source_kind = NATIVE_POSTFX_EXTRACT_DEPTH;
  }
  else if (output.data->source == VIEW_LAYER_NATIVE_POSTFX_SOURCE_VECTOR) {
    source_kind = NATIVE_POSTFX_EXTRACT_VECTOR;
  }
  else if (output.data->source == VIEW_LAYER_NATIVE_POSTFX_SOURCE_OUTLINE) {
    source_kind = NATIVE_POSTFX_EXTRACT_OUTLINE;
  }
  else if (output.source_is_value) {
    source_kind = NATIVE_POSTFX_EXTRACT_VALUE_PASS;
  }

  gpu::Texture *outline_tx = inst_.outline.resolved_texture();
  if (outline_tx == nullptr) {
    outline_tx = inst_.render_buffers.combined_tx;
  }

  extract_ps_.init();
  extract_ps_.shader_set(inst_.shaders.static_shader_get(NATIVE_POSTFX_OUTPUT_EXTRACT));
  extract_ps_.bind_texture("depth_tx", &inst_.render_buffers.depth_tx);
  extract_ps_.bind_texture("vector_tx", &inst_.render_buffers.vector_tx);
  extract_ps_.bind_texture("rp_color_tx", &inst_.render_buffers.rp_color_tx);
  extract_ps_.bind_texture("rp_value_tx", &inst_.render_buffers.rp_value_tx);
  extract_ps_.bind_texture("outline_tx", outline_tx);
  extract_ps_.bind_image("out_color_img", output_tx);
  extract_ps_.push_constant("source_kind", source_kind);
  extract_ps_.push_constant("source_layer", output.source_index);
  extract_ps_.dispatch(math::divide_ceil(inst_.film.render_extent_get(), int2(FILM_GROUP_SIZE)));
  extract_ps_.barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_SHADER_IMAGE_ACCESS);
  inst_.manager->submit(extract_ps_);
}

gpu::Texture *NativePostFXOutputModule::apply_camera_fx(
    View &view,
    gpu::Texture *input_tx,
    gpu::Texture *output_tx,
    gpu::Texture *depth_tx,
    gpu::Texture *velocity_tx,
    DepthOfFieldBuffer &dof_buffer,
    const ViewLayerNativePostFXOutput &output)
{
  const bool use_motion_blur =
      (output.effects & VIEW_LAYER_NATIVE_POSTFX_OUTPUT_EFFECT_MOTION_BLUR) &&
      inst_.motion_blur.postfx_enabled();
  const bool use_dof = (output.effects & VIEW_LAYER_NATIVE_POSTFX_OUTPUT_EFFECT_DOF) &&
                       inst_.depth_of_field.postfx_enabled();
  if (!use_motion_blur && !use_dof) {
    return input_tx;
  }

  gpu::Texture *input = input_tx;
  gpu::Texture *output_work = output_tx;
  gpu::Texture *depth = (depth_tx != nullptr) ? depth_tx : inst_.render_buffers.depth_tx;
  gpu::Texture *velocity = velocity_tx;

  if (use_motion_blur) {
    inst_.motion_blur.render(view, &input, &output_work, depth, velocity);
  }
  if (use_dof) {
    inst_.depth_of_field.render(view, &input, &output_work, dof_buffer, depth, velocity);
  }
  return input;
}

void NativePostFXOutputModule::acquire_velocity_work_texture(TextureFromPool &texture,
                                                            int2 extent)
{
  constexpr eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE;
  const gpu::TextureFormat format = inst_.render_buffers.vector_tx_format();
  texture.acquire(extent, format, usage);
  if (format == gpu::TextureFormat::SFLOAT_16_16) {
    GPU_texture_swizzle_set(texture, "rgrg");
  }
}

uint64_t NativePostFXOutputModule::output_signature(const RuntimeOutput &output)
{
  const uint source_hash = output.data->source == VIEW_LAYER_NATIVE_POSTFX_SOURCE_AOV ?
                               BLI_hash_string(output.data->source_aov) :
                               uint(output.data->source);
  return uint64_t(source_hash) ^ (uint64_t(output.data->effects) << 32) ^
         (uint64_t(output.storage_type) << 48);
}

void NativePostFXOutputModule::pack_output(const RuntimeOutput &output, gpu::Texture *input_tx)
{
  PassSimple &pack_ps = output.storage_type == PASS_STORAGE_VALUE ? pack_value_ps_ : pack_color_ps_;
  pack_ps.init();
  pack_ps.shader_set(inst_.shaders.static_shader_get(
      output.storage_type == PASS_STORAGE_VALUE ? NATIVE_POSTFX_OUTPUT_PACK_VALUE :
                                                  NATIVE_POSTFX_OUTPUT_PACK_COLOR));
  pack_ps.bind_texture("input_tx", input_tx);
  if (output.storage_type == PASS_STORAGE_VALUE) {
    const int output_layer = inst_.render_buffers.data.value_len +
                             inst_.render_buffers.data.aovs.value_len + output.value_index;
    pack_ps.bind_image("output_img", &inst_.render_buffers.rp_value_tx);
    pack_ps.push_constant("output_layer", output_layer);
  }
  else {
    const int output_layer = inst_.render_buffers.data.color_len +
                             inst_.render_buffers.data.aovs.color_len + output.color_index;
    pack_ps.bind_image("output_img", &inst_.render_buffers.rp_color_tx);
    pack_ps.push_constant("output_layer", output_layer);
  }
  pack_ps.dispatch(math::divide_ceil(inst_.film.render_extent_get(), int2(FILM_GROUP_SIZE)));
  pack_ps.barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_SHADER_IMAGE_ACCESS);
  inst_.manager->submit(pack_ps);
}

void NativePostFXOutputModule::render(View &view)
{
  if (outputs_.is_empty()) {
    return;
  }

  const int2 extent = inst_.film.render_extent_get();
  constexpr eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE;

  for (const int index : outputs_.index_range()) {
    RuntimeOutput &output = outputs_[index];
    const bool source_valid = resolve_source(output);

    const uint64_t signature = output_signature(output);
    if (dof_signatures_[index] != signature) {
      dof_buffers_[index].stabilize_history_tx_.release();
      dof_signatures_[index] = signature;
    }

    source_tx_.acquire(extent, gpu::TextureFormat::SFLOAT_16_16_16_16, usage);
    if (source_valid) {
      extract_source(output, source_tx_.gpu_texture());
    }
    else {
      source_tx_.clear(float4(0.0f));
    }

    const bool use_motion_blur =
        (output.data->effects & VIEW_LAYER_NATIVE_POSTFX_OUTPUT_EFFECT_MOTION_BLUR) &&
        inst_.motion_blur.postfx_enabled();
    const bool use_dof = (output.data->effects & VIEW_LAYER_NATIVE_POSTFX_OUTPUT_EFFECT_DOF) &&
                         inst_.depth_of_field.postfx_enabled();
    const bool uses_effects = source_valid && (use_motion_blur || use_dof);
    gpu::Texture *final_tx = source_tx_.gpu_texture();
    if (uses_effects) {
      effect_tx_.acquire(extent, gpu::TextureFormat::SFLOAT_16_16_16_16, usage);
      gpu::Texture *depth_tx = inst_.render_buffers.depth_tx;
      gpu::Texture *velocity_tx = nullptr;
      gpu::Texture *source_velocity_tx = inst_.render_buffers.vector_tx;
      if (output.data->source == VIEW_LAYER_NATIVE_POSTFX_SOURCE_OUTLINE) {
        if (inst_.outline.resolved_depth_texture() != nullptr) {
          depth_tx = inst_.outline.resolved_depth_texture();
        }
        if (inst_.outline.resolved_velocity_texture() != nullptr) {
          source_velocity_tx = inst_.outline.resolved_velocity_texture();
        }
      }
      if (use_motion_blur) {
        acquire_velocity_work_texture(velocity_tx_, extent);
        GPU_texture_copy(velocity_tx_.gpu_texture(), source_velocity_tx);
        GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE | GPU_BARRIER_TEXTURE_FETCH |
                           GPU_BARRIER_SHADER_IMAGE_ACCESS);
        velocity_tx = velocity_tx_.gpu_texture();
      }
      else if (use_dof && output.data->source == VIEW_LAYER_NATIVE_POSTFX_SOURCE_OUTLINE) {
        velocity_tx = source_velocity_tx;
      }
      final_tx = apply_camera_fx(view,
                                 source_tx_.gpu_texture(),
                                 effect_tx_.gpu_texture(),
                                 depth_tx,
                                 velocity_tx,
                                 dof_buffers_[index],
                                 *output.data);
    }

    pack_output(output, final_tx);

    velocity_work_texture_release(velocity_tx_, inst_.render_buffers.vector_tx_format());
    effect_tx_.release();
    source_tx_.release();
  }
}

gpu::Texture *NativePostFXOutputModule::render_outline_for_combined(View &view,
                                                                    gpu::Texture *outline_tx)
{
  if (outline_tx == nullptr) {
    return nullptr;
  }

  ViewLayerNativePostFXOutput output = {};
  output.flag = VIEW_LAYER_NATIVE_POSTFX_OUTPUT_ENABLED;
  output.source = VIEW_LAYER_NATIVE_POSTFX_SOURCE_OUTLINE;
  output.effects = VIEW_LAYER_NATIVE_POSTFX_OUTPUT_EFFECT_MOTION_BLUR |
                   VIEW_LAYER_NATIVE_POSTFX_OUTPUT_EFFECT_DOF;

  const bool uses_effects = (inst_.motion_blur.postfx_enabled() || inst_.depth_of_field.postfx_enabled());
  if (!uses_effects) {
    return outline_tx;
  }

  const int2 extent = inst_.film.render_extent_get();
  constexpr eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE;
  default_outline_tx_.acquire(extent, gpu::TextureFormat::SFLOAT_16_16_16_16, usage);
  GPU_texture_copy(default_outline_tx_.gpu_texture(), outline_tx);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE | GPU_BARRIER_TEXTURE_FETCH);
  default_outline_effect_tx_.acquire(extent, gpu::TextureFormat::SFLOAT_16_16_16_16, usage);

  gpu::Texture *velocity_tx = nullptr;
  gpu::Texture *depth_tx = inst_.outline.resolved_depth_texture();
  if (depth_tx == nullptr) {
    depth_tx = inst_.render_buffers.depth_tx;
  }
  if (inst_.motion_blur.postfx_enabled()) {
    gpu::Texture *source_velocity_tx = inst_.outline.resolved_velocity_texture();
    if (source_velocity_tx == nullptr) {
      source_velocity_tx = inst_.render_buffers.vector_tx;
    }
    acquire_velocity_work_texture(default_outline_velocity_tx_, extent);
    GPU_texture_copy(default_outline_velocity_tx_.gpu_texture(), source_velocity_tx);
    GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE | GPU_BARRIER_TEXTURE_FETCH |
                       GPU_BARRIER_SHADER_IMAGE_ACCESS);
    velocity_tx = default_outline_velocity_tx_.gpu_texture();
  }
  else if (inst_.depth_of_field.postfx_enabled()) {
    velocity_tx = inst_.outline.resolved_velocity_texture();
  }

  gpu::Texture *final_tx = apply_camera_fx(view,
                                           default_outline_tx_.gpu_texture(),
                                           default_outline_effect_tx_.gpu_texture(),
                                           depth_tx,
                                           velocity_tx,
                                           default_outline_dof_buffer_,
                                           output);
  if (final_tx == default_outline_effect_tx_.gpu_texture()) {
    default_outline_tx_.release();
  }
  else if (final_tx == default_outline_tx_.gpu_texture()) {
    default_outline_effect_tx_.release();
  }
  else {
    default_outline_tx_.release();
    default_outline_effect_tx_.release();
  }

  velocity_work_texture_release(default_outline_velocity_tx_, inst_.render_buffers.vector_tx_format());
  return final_tx;
}

void NativePostFXOutputModule::release()
{
  source_tx_.release();
  effect_tx_.release();
  velocity_work_texture_release(velocity_tx_, inst_.render_buffers.vector_tx_format());
  default_outline_tx_.release();
  default_outline_effect_tx_.release();
  velocity_work_texture_release(default_outline_velocity_tx_, inst_.render_buffers.vector_tx_format());
}

}  // namespace blender::eevee
