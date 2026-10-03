/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Nuru: Backend-neutral hardware ray-tracing API dispatcher. */

/** \file
 * \ingroup gpu
 */

#include "GPU_context.hh"
#include "GPU_nuru_hardware_raytrace.hh"

#ifdef WITH_VULKAN_BACKEND
#  include "vulkan/vk_nuru_raytrace_acceleration.hh"
#endif

namespace blender {

GPUHardwareRaytraceScene *GPU_hardware_raytrace_scene_build(Span<GPUHardwareRaytraceSceneEntry> entries,
                                                      GPUHardwareRaytraceSceneStats *r_stats)
{
  if (r_stats != nullptr) {
    *r_stats = {};
  }

#ifdef WITH_VULKAN_BACKEND
  if (GPU_backend_get_type() == GPU_BACKEND_VULKAN) {
    return gpu::vulkan::raytrace_scene_build(entries, r_stats);
  }
#endif

  (void)entries;
  return nullptr;
}

bool GPU_hardware_raytrace_scene_update(GPUHardwareRaytraceScene *scene,
                                     Span<GPUHardwareRaytraceSceneEntry> entries,
                                     const GPUHardwareRaytraceSceneUpdateParams &update_params,
                                     GPUHardwareRaytraceSceneStats *r_stats)
{
  if (r_stats != nullptr) {
    *r_stats = {};
  }
#ifdef WITH_VULKAN_BACKEND
  if (scene != nullptr && GPU_backend_get_type() == GPU_BACKEND_VULKAN) {
    return gpu::vulkan::raytrace_scene_update(scene, entries, update_params, r_stats);
  }
#endif
  (void)scene;
  (void)entries;
  (void)update_params;
  return false;
}

bool GPU_hardware_raytrace_scene_trace(GPUHardwareRaytraceScene *scene,
                                    const GPUHardwareRaytraceTraceParams &params)
{
#ifdef WITH_VULKAN_BACKEND
  if (scene != nullptr && GPU_backend_get_type() == GPU_BACKEND_VULKAN) {
    return gpu::vulkan::raytrace_scene_trace(scene, params);
  }
#endif
  (void)scene;
  (void)params;
  return false;
}

bool GPU_hardware_raytrace_scene_trace_directional_shadow(
    GPUHardwareRaytraceScene *scene, const GPUHardwareRaytraceDirectionalShadowParams &params)
{
#ifdef WITH_VULKAN_BACKEND
  if (scene != nullptr && GPU_backend_get_type() == GPU_BACKEND_VULKAN) {
    return gpu::vulkan::raytrace_scene_trace_directional_shadow(scene, params);
  }
#endif
  (void)scene;
  (void)params;
  return false;
}

bool GPU_hardware_raytrace_scene_trace_directional_hit_shadow(
    GPUHardwareRaytraceScene *scene, const GPUHardwareRaytraceDirectionalHitShadowParams &params)
{
#ifdef WITH_VULKAN_BACKEND
  if (scene != nullptr && GPU_backend_get_type() == GPU_BACKEND_VULKAN) {
    return gpu::vulkan::raytrace_scene_trace_directional_hit_shadow(scene, params);
  }
#endif
  (void)scene;
  (void)params;
  return false;
}

bool GPU_hardware_raytrace_scene_trace_local_shadow(GPUHardwareRaytraceScene *scene,
                                                 const GPUHardwareRaytraceLocalShadowParams &params)
{
#ifdef WITH_VULKAN_BACKEND
  if (scene != nullptr && GPU_backend_get_type() == GPU_BACKEND_VULKAN) {
    return gpu::vulkan::raytrace_scene_trace_local_shadow(scene, params);
  }
#endif
  (void)scene;
  (void)params;
  return false;
}

bool GPU_hardware_raytrace_scene_trace_local_hit_shadow(
    GPUHardwareRaytraceScene *scene, const GPUHardwareRaytraceLocalHitShadowParams &params)
{
#ifdef WITH_VULKAN_BACKEND
  if (scene != nullptr && GPU_backend_get_type() == GPU_BACKEND_VULKAN) {
    return gpu::vulkan::raytrace_scene_trace_local_hit_shadow(scene, params);
  }
#endif
  (void)scene;
  (void)params;
  return false;
}

bool GPU_hardware_raytrace_scene_shadow_batch_begin(GPUHardwareRaytraceScene *scene)
{
#ifdef WITH_VULKAN_BACKEND
  if (scene != nullptr && GPU_backend_get_type() == GPU_BACKEND_VULKAN) {
    return gpu::vulkan::raytrace_scene_shadow_batch_begin(scene);
  }
#endif
  (void)scene;
  return false;
}

bool GPU_hardware_raytrace_scene_shadow_batch_end(GPUHardwareRaytraceScene *scene)
{
#ifdef WITH_VULKAN_BACKEND
  if (scene != nullptr && GPU_backend_get_type() == GPU_BACKEND_VULKAN) {
    return gpu::vulkan::raytrace_scene_shadow_batch_end(scene);
  }
#endif
  (void)scene;
  return false;
}

bool GPU_hardware_raytrace_scene_trace_environment_visibility(
    GPUHardwareRaytraceScene *scene, const GPUHardwareRaytraceEnvironmentVisibilityParams &params)
{
#ifdef WITH_VULKAN_BACKEND
  if (scene != nullptr && GPU_backend_get_type() == GPU_BACKEND_VULKAN) {
    return gpu::vulkan::raytrace_scene_trace_environment_visibility(scene, params);
  }
#endif
  (void)scene;
  (void)params;
  return false;
}

bool GPU_hardware_raytrace_scene_trace_hit_environment_visibility(
    GPUHardwareRaytraceScene *scene,
    const GPUHardwareRaytraceHitEnvironmentVisibilityParams &params)
{
#ifdef WITH_VULKAN_BACKEND
  if (scene != nullptr && GPU_backend_get_type() == GPU_BACKEND_VULKAN) {
    return gpu::vulkan::raytrace_scene_trace_hit_environment_visibility(scene, params);
  }
#endif
  (void)scene;
  (void)params;
  return false;
}

bool GPU_hardware_raytrace_denoise_oidn(const GPUHardwareRaytraceOIDNDenoiseParams &params)
{
#ifdef WITH_VULKAN_BACKEND
  if (GPU_backend_get_type() == GPU_BACKEND_VULKAN) {
    return gpu::vulkan::raytrace_denoise_oidn(params);
  }
#endif
  (void)params;
  return false;
}

void GPU_hardware_raytrace_scene_free(GPUHardwareRaytraceScene *scene)
{
#ifdef WITH_VULKAN_BACKEND
  if (scene != nullptr && GPU_backend_get_type() == GPU_BACKEND_VULKAN) {
    gpu::vulkan::raytrace_scene_free(scene);
    return;
  }
#endif
  (void)scene;
}

}  // namespace blender
