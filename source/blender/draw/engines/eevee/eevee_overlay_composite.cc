/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 */

#include "BKE_image.hh"

#include "GPU_framebuffer.hh"
#include "GPU_texture.hh"

#include "eevee_instance.hh"

namespace blender::eevee {

static bool overlay_input_is_valid(const SceneOverlayInput *overlay_input)
{
  return overlay_input != nullptr && overlay_input->enabled && overlay_input->color_image != nullptr &&
         overlay_input->opacity > 0.0f;
}

void OverlayCompositeModule::begin_sync()
{
  entries_.clear();

  for (SceneOverlayInput *overlay_input = static_cast<SceneOverlayInput *>(
           inst_.scene->eevee.overlay_inputs.first);
       overlay_input != nullptr;
       overlay_input = overlay_input->next)
  {
    if (!overlay_input_is_valid(overlay_input)) {
      continue;
    }

    gpu::Texture *color_tx = BKE_image_get_gpu_texture(overlay_input->color_image, nullptr);
    if (color_tx == nullptr) {
      continue;
    }

    OverlayPassEntry entry;
    entry.scene_input = overlay_input;
    entry.color_image = overlay_input->color_image;
    entry.color_tx = color_tx;
    entries_.append(entry);
  }
}

void OverlayCompositeModule::render(draw::View &view, gpu::Texture **input_tx, gpu::Texture **output_tx)
{
  if (entries_.is_empty() || *input_tx == nullptr) {
    return;
  }

  const int2 extent = {GPU_texture_width(*input_tx), GPU_texture_height(*input_tx)};
  ping_tx_.ensure_2d(GPU_texture_format(*input_tx), extent, GPU_TEXTURE_USAGE_GENERAL);
  pong_tx_.ensure_2d(GPU_texture_format(*input_tx), extent, GPU_TEXTURE_USAGE_GENERAL);

  gpu::Texture *source_tx = *input_tx;

  for (const int entry_index : entries_.index_range()) {
    Texture &target_tx = ((entry_index & 1) == 0) ? ping_tx_ : pong_tx_;
    SceneOverlayInput &overlay_input = *entries_[entry_index].scene_input;
    gpu::Texture *scene_color_tx = source_tx;
    gpu::Texture *overlay_color_tx = entries_[entry_index].color_tx;
    const int alpha_mode = overlay_input.alpha_mode;
    const int blend_mode = overlay_input.blend_mode;
    const float opacity = overlay_input.opacity;
    const float2 offset = float2(overlay_input.offset[0], overlay_input.offset[1]);
    const float2 scale = float2((overlay_input.scale[0] > 1e-8f) ? overlay_input.scale[0] :
                                                                     1e-8f,
                                (overlay_input.scale[1] > 1e-8f) ? overlay_input.scale[1] :
                                                                     1e-8f);

    framebuffer_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(target_tx));

    PassSimple pass = {"OverlayComposite.Pass"};
    pass.state_set(DRW_STATE_WRITE_COLOR);
    pass.framebuffer_set(&framebuffer_);
    pass.shader_set(inst_.shaders.static_shader_get(OVERLAY_COMPOSITE));
    pass.bind_resources(inst_.uniform_data);
    pass.bind_texture("scene_color_tx", &scene_color_tx);
    pass.bind_texture("overlay_color_tx", &overlay_color_tx);
    pass.push_constant("overlay_alpha_mode", &alpha_mode, 1);
    pass.push_constant("overlay_blend_mode", &blend_mode, 1);
    pass.push_constant("overlay_opacity", &opacity, 1);
    pass.push_constant("overlay_offset", offset);
    pass.push_constant("overlay_scale", scale);
    pass.draw_procedural(GPU_PRIM_TRIS, 1, 3);

    GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
    inst_.manager->submit(pass, view);
    GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);

    source_tx = target_tx;
  }

  *input_tx = source_tx;
  *output_tx = (source_tx == ping_tx_.gpu_texture()) ? pong_tx_.gpu_texture() :
                                                       ping_tx_.gpu_texture();
}

}  // namespace blender::eevee
