/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_dlss5.hh"

#include "CLG_log.h"

#include "GPU_context.hh"
#include "GPU_texture.hh"

#include "eevee_instance.hh"

namespace blender::eevee {

gpu::Texture *Dlss5Module::process(const Dlss5FrameInputs &inputs)
{
  if (!reported_) {
    CLOG_INFO(&Instance::log,
              "DLSS5 adapter pass-through: backend=%s input=%dx%d output=%dx%d viewport=%d",
              GPU_backend_get_name(),
              inputs.input_extent.x,
              inputs.input_extent.y,
              inputs.output_extent.x,
              inputs.output_extent.y,
              inputs.is_viewport);
    CLOG_INFO(&Instance::log,
              "DLSS5 contract: color=%s depth=%s velocity=%s color_scene_linear=%d "
              "depth_reverse_z=%d velocity_packed=%d velocity_pixel_space=%d exposure_scale=%f",
              GPU_texture_format_name(GPU_texture_format(inputs.color)),
              GPU_texture_format_name(GPU_texture_format(inputs.depth)),
              GPU_texture_format_name(GPU_texture_format(inputs.velocity)),
              inputs.color_is_scene_linear,
              inputs.depth_is_reverse_z,
              inputs.velocity_is_packed,
              inputs.velocity_is_pixel_space,
              inputs.exposure_scale);
    reported_ = true;
  }

  /* Keep the normal EEVEE path unchanged until a real executor is available. */
  return inputs.color;
}

}  // namespace blender::eevee
