/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "GPU_nuru_hardware_raytrace.hh"

namespace blender::gpu::vulkan {

GPUHardwareRaytraceScene *raytrace_scene_build(Span<GPUHardwareRaytraceSceneEntry> entries,
                                               GPUHardwareRaytraceSceneStats *r_stats);
bool raytrace_scene_update(GPUHardwareRaytraceScene *scene,
                           Span<GPUHardwareRaytraceSceneEntry> entries,
                           const GPUHardwareRaytraceSceneUpdateParams &update_params,
                           GPUHardwareRaytraceSceneStats *r_stats);
bool raytrace_scene_trace(GPUHardwareRaytraceScene *scene,
                          const GPUHardwareRaytraceTraceParams &params);
bool raytrace_scene_trace_directional_shadow(
    GPUHardwareRaytraceScene *scene, const GPUHardwareRaytraceDirectionalShadowParams &params);
bool raytrace_scene_trace_directional_hit_shadow(
    GPUHardwareRaytraceScene *scene, const GPUHardwareRaytraceDirectionalHitShadowParams &params);
bool raytrace_scene_trace_local_shadow(GPUHardwareRaytraceScene *scene,
                                       const GPUHardwareRaytraceLocalShadowParams &params);
bool raytrace_scene_trace_local_hit_shadow(GPUHardwareRaytraceScene *scene,
                                           const GPUHardwareRaytraceLocalHitShadowParams &params);
bool raytrace_scene_shadow_batch_begin(GPUHardwareRaytraceScene *scene);
bool raytrace_scene_shadow_batch_end(GPUHardwareRaytraceScene *scene);
bool raytrace_scene_trace_environment_visibility(
    GPUHardwareRaytraceScene *scene, const GPUHardwareRaytraceEnvironmentVisibilityParams &params);
bool raytrace_scene_trace_hit_environment_visibility(
    GPUHardwareRaytraceScene *scene,
    const GPUHardwareRaytraceHitEnvironmentVisibilityParams &params);
bool raytrace_denoise_oidn(const GPUHardwareRaytraceOIDNDenoiseParams &params);
void raytrace_scene_free(GPUHardwareRaytraceScene *scene);

/**
 * Free all device-lifetime objects owned by the ray-tracing backend (pipelines, samplers,
 * command pool, OIDN interop cache, in-flight submissions). Must be called from
 * `VKDevice::deinit` while the device and memory allocator are still alive.
 */
void raytrace_device_free();

}  // namespace blender::gpu::vulkan
