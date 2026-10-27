/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 *
 * Converts the different renderable object types to draw-calls.
 */

#include <cmath>
#include <limits>

#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"

#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "DNA_curves_types.h"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_particle_types.h"
#include "DNA_pointcloud_types.h"
#include "DNA_volume_types.h"

#include "NOD_shader.h"

#include "draw_cache.hh"
#include "draw_common.hh"
#include "draw_sculpt.hh"

#include "eevee_instance.hh"

namespace blender::eevee {

static Vector<GPUMaterial *> non_null_materials(Span<GPUMaterial *> materials)
{
  Vector<GPUMaterial *> filtered;
  filtered.reserve(materials.size());
  for (GPUMaterial *material : materials) {
    if (material != nullptr) {
      filtered.append(material);
    }
  }
  return filtered;
}

static void append_npr_and_probe_materials(Vector<GPUMaterial *> &materials,
                                           Span<Material *> npr_materials)
{
  for (const Material *material : npr_materials) {
    for (GPUMaterial *gpumat : {material->npr.gpumat,
                               material->planar_probe_npr.gpumat,
                               material->lightprobe_sphere_npr.gpumat,
                               material->outline_shell_prepass.gpumat,
                               material->outline_shell_shading.gpumat,
                               material->outline_shell_shadow.gpumat})
    {
      if (gpumat != nullptr) {
        materials.append(gpumat);
      }
    }
  }
}

static bool material_array_uses_outline_control(const MaterialArray &material_array,
                                                Span<gpu::Batch *> mat_geom)
{
  for (const int i : material_array.materials.index_range()) {
    if (i >= mat_geom.size() || mat_geom[i] == nullptr) {
      continue;
    }

    if (material_array.materials[i].uses_outline_control) {
      return true;
    }
  }
  return false;
}

/* -------------------------------------------------------------------- */
/** \name Recalc
 *
 * \{ */

ObjectHandle SyncModule::sync_object(const ObjectRef &ob_ref,
                                     const ResourceHandleRange &res_handle,
                                     uint sub_key)
{
  return ObjectHandle(ob_ref, res_handle, inst_.get_recalc_flags(ob_ref), sub_key);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Common
 * \{ */

/**
 * Return the subdivision level used by the shader of a material pass. Zero if the shader doesn't
 * tessellate, which is the case of placeholder materials used during shader compilation.
 */
static int material_pass_tessellation_level(const Material &material, GPUMaterial *gpumat)
{
  if (material.tessellation_level == 0 || gpumat == nullptr) {
    return 0;
  }
  constexpr uint64_t geometry_mask = (1u << 4u) - 1u;
  const eMaterialGeometry geometry_type = eMaterialGeometry(GPU_material_uuid_get(gpumat) &
                                                            geometry_mask);
  if (geometry_type != MAT_GEOM_MESH_TESS) {
    return 0;
  }
  /* Without the settings, the shader defaults to a single level. */
  return GPU_material_has_tessellation_output(gpumat) ? material.tessellation_level : 1;
}

void SyncModule::geometry_call(PassMain::Sub *sub_pass,
                               gpu::Batch *geom,
                               ResourceHandleRange resource_handle,
                               int tessellation_level)
{
  if (sub_pass == nullptr || geom == nullptr) {
    return;
  }
  if (tessellation_level > 0) {
    sub_pass = tessellation_passes_.lookup_or_add_cb({sub_pass, tessellation_level}, [&]() {
      PassMain::Sub &tess_pass = sub_pass->sub("Tessellation budget");
      tess_pass.push_constant("tessellation_expand", tessellation_level);
      return &tess_pass;
    });
    /* Each triangle is subdivided into level^2 triangles inside the vertex shader.
     * The batch content is only generated after object sync, so the resulting vertex count is
     * unknown here. #GPU_batch_draw_expanded_parameter_get skips overflowing draw-calls. */
    sub_pass->draw_expand(
        geom, GPU_PRIM_TRIS, tessellation_level * tessellation_level, 1, resource_handle);
    return;
  }
  sub_pass->draw(geom, resource_handle);
}

/** Bound every original edge's LOD without changing its fractional partitioning.
 * Evaluated bounds enclose endpoints and their midpoints. All instances share the largest budget.
 * Displacement and Phong smoothing happen after LOD, so they do not enlarge these bounds. */
static int material_tessellation_camera_budget(const ObjectHandle &ob_handle,
                                               const Material &material,
                                               const ShaderTessellationSettings &settings,
                                               const CameraData &camera,
                                               const int2 extent)
{
  /* Use the same evaluated mesh as DRW_cache_mesh_surface_shaded_get (also in edit mode). */
  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(*ob_handle.object);
  const std::optional<Bounds<float3>> bounds = mesh.bounds_min_max();
  if (!bounds) {
    return material.tessellation_level;
  }
  int budget = 1;
  for (const int instance : IndexRange(ob_handle.instances_count())) {
    const float4x4 &model = ob_handle.object_to_world(instance);
    float3 world_min(std::numeric_limits<float>::max());
    float3 world_max(std::numeric_limits<float>::lowest());
    float2 screen_min(std::numeric_limits<float>::max());
    float2 screen_max(std::numeric_limits<float>::lowest());
    for (int corner = 0; corner < 8; corner++) {
      const float3 local((corner & 1) ? bounds->max.x : bounds->min.x,
                         (corner & 2) ? bounds->max.y : bounds->min.y,
                         (corner & 4) ? bounds->max.z : bounds->min.z);
      const float3 world = math::transform_point(model, local);
      if (!std::isfinite(world.x) || !std::isfinite(world.y) || !std::isfinite(world.z)) {
        return material.tessellation_level;
      }
      world_min = math::min(world_min, world);
      world_max = math::max(world_max, world);
      if (settings.mode == SHD_TESSELLATION_SCREEN_EDGE) {
        const float4 clip = camera.persmat * float4(world, 1.0f);
        /* A box crossing/behind the camera plane has no finite projected diameter bound. */
        if (!(clip.w > 1e-5f)) {
          return material.tessellation_level;
        }
        const float2 pixel = (float2(clip.x, clip.y) / clip.w) * 0.5f * float2(extent);
        if (!std::isfinite(pixel.x) || !std::isfinite(pixel.y)) {
          return material.tessellation_level;
        }
        screen_min = math::min(screen_min, pixel);
        screen_max = math::max(screen_max, pixel);
      }
    }
    float factor;
    if (settings.mode == SHD_TESSELLATION_DISTANCE) {
      const float3 camera_position = camera.viewinv.location();
      const float3 closest = math::clamp(camera_position, world_min, world_max);
      /* Round toward MORE work to cover CPU/GPU transform and distance roundoff. */
      const float epsilon = 1e-4f * std::max({1.0f,
                                              math::length(camera_position),
                                              math::length(world_min),
                                              math::length(world_max)});
      const float distance = std::max(0.0f, math::distance(camera_position, closest) - epsilon);
      const float fac = math::clamp(
          (distance - settings.near_distance) /
              std::max(settings.far_distance - settings.near_distance, 1e-6f),
          0.0f,
          1.0f);
      factor = (1.0f - fac) * settings.max_level + fac * settings.min_level;
      if (fac > 0.0f && fac < 1.0f) {
        factor += 1e-4f;
      }
    }
    else {
      factor = math::distance(screen_min, screen_max) / std::max(settings.edge_length, 1.0f);
      factor += 1e-4f * std::max(1.0f, factor);
      factor = math::clamp(factor, float(settings.min_level), float(settings.max_level));
    }
    if (!std::isfinite(factor)) {
      return material.tessellation_level;
    }
    budget = std::max(budget, int(std::ceil(factor)) | 1);
    if (budget >= material.tessellation_level) {
      return material.tessellation_level;
    }
  }
  return std::min(budget, material.tessellation_level);
}

static int material_tessellation_budget(Instance &inst,
                                        const ObjectHandle &object,
                                        const Material &material)
{
  if (material.tessellation_level <= 1 || material.shading.gpumat == nullptr) {
    return material.tessellation_level;
  }
  const MaterialNodeTreeInfo &info = inst.materials.node_tree_info_get(
      GPU_material_get_material(material.shading.gpumat));
  const ShaderTessellationSettings &settings = info.tessellation;
  if (!info.has_tessellation || settings.mode == SHD_TESSELLATION_FIXED ||
      settings.min_level == settings.max_level)
  {
    return material.tessellation_level;
  }
  int budget = material_tessellation_camera_budget(
      object, material, settings, inst.camera.data_get(), inst.film.render_extent_get());
  /* Demand is finalized after object sync. Bound every enabled camera, including a newly
   * referenced capture, rather than truncating its geometry or submitting the full lattice. */
  const bool valid = inst.render_textures.foreach_enabled_camera([&](const CameraData &camera) {
    if (budget < material.tessellation_level) {
      /* Captures override CameraData, but tess_vertex_get still reads film.render_extent.
       * Match that shader input even when the capture output has a different resolution. */
      budget = std::max(budget,
                        material_tessellation_camera_budget(
                            object, material, settings, camera, inst.film.render_extent_get()));
    }
  });
  CLOG_INFO(&Instance::log,
            "RenderTexture tessellation budget=%d full=%d",
            valid ? budget : material.tessellation_level,
            material.tessellation_level);
  return valid ? budget : material.tessellation_level;
}

static inline void geometry_volume_call(PassMain::Sub *pass,
                                        GPUMaterial *gpumat,
                                        Scene *scene,
                                        Object *ob,
                                        gpu::Batch *geom,
                                        ResourceHandle res_handle)
{
  BLI_assert(ob->type != OB_VOLUME);
  if (pass != nullptr) {
    PassMain::Sub *object_pass = volume_sub_pass(*pass, scene, ob, gpumat);
    if (object_pass != nullptr) {
      object_pass->draw(geom, {ResourceID(res_handle)});
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Common Helpers
 * \{ */

void SyncModule::sync_common_passes(const Material &material,
                                    FunctionRef<void(const MaterialPass &)> sync_cb)
{
  sync_cb(material.capture);
  sync_cb(material.stencil);
  sync_cb(material.shadow);
  sync_cb(material.prepass);
  sync_cb(material.shading);
  sync_cb(material.npr);
  sync_cb(material.outline_occlusion);

  sync_cb(material.lightprobe_sphere_prepass);
  sync_cb(material.lightprobe_sphere_shading);
  sync_cb(material.lightprobe_sphere_npr);
  sync_cb(material.planar_probe_prepass);
  sync_cb(material.planar_probe_shading);
  sync_cb(material.planar_probe_npr);
}

void SyncModule::sync_volume_passes(const ObjectHandle &ob_handle,
                                    const Material &material,
                                    FunctionRef<void(const MaterialPass &, int)> sync_cb)
{
  if (material.volume_occupancy.gpumat == nullptr || material.volume_material.gpumat == nullptr) {
    return;
  }
  if (GPU_material_status(material.volume_occupancy.gpumat) != GPU_MAT_SUCCESS ||
      GPU_material_status(material.volume_material.gpumat) != GPU_MAT_SUCCESS)
  {
    return;
  }

  blender::Material *blender_mat = GPU_material_get_material(material.volume_material.gpumat);

  for (int instance : IndexRange(ob_handle.instances_count())) {
    /* TODO(fclem): This is against design. Sync shouldn't depend on view properties (camera). */
    VolumeLayer *layer = inst_.pipelines.volume.register_and_get_layer(
        VolumeObjectBounds(inst_.camera, ob_handle, instance));

    if (!layer) {
      continue;
    }

    sync_cb(MaterialPass{material.volume_occupancy.gpumat,
                         layer->occupancy_add(
                             ob_handle.object, blender_mat, material.volume_occupancy.gpumat)},
            instance);

    sync_cb(MaterialPass{material.volume_material.gpumat,
                         layer->material_add(
                             ob_handle.object, blender_mat, material.volume_material.gpumat)},
            instance);
  }
}

void SyncModule::sync_alpha_blended_passes(const ObjectHandle &ob_handle,
                                           const Material &material,
                                           FunctionRef<void(const MaterialPass &, int)> sync_cb,
                                           bool outline_shell)
{
  const bool hide_on_camera = ob_handle.object->visibility_flag & OB_HIDE_CAMERA;
  if (hide_on_camera || !(outline_shell ? material.is_outline_shell_blended :
                                         material.is_alpha_blend_transparent))
  {
    return;
  }

  const MaterialPass &shading = outline_shell ? material.outline_shell_shading : material.shading;
  if (shading.gpumat == nullptr) {
    return;
  }
  blender::Material *blender_mat = GPU_material_get_material(shading.gpumat);
  const SurfaceDrawState shell_state = surface_draw_state_outline_shell(*blender_mat);

  for (int instance : IndexRange(ob_handle.instances_count())) {
    PassMain::Sub *prepass = nullptr;
    PassMain::Sub *matpass = nullptr;

    inst_.pipelines.forward.transparent_add(ob_handle.object,
                                            ob_handle.object_to_world(instance).location(),
                                            blender_mat,
                                            shading.gpumat,
                                            prepass,
                                            matpass,
                                            outline_shell ? &shell_state : nullptr);

    if (prepass != nullptr) {
      sync_cb(MaterialPass{material.overlap_masking.gpumat, prepass}, instance);
    }
    sync_cb(MaterialPass{shading.gpumat, matpass}, instance);
  }
}

void SyncModule::sync_outline_occlusion_passes(const ObjectHandle &ob_handle,
                                               const Material &material,
                                               FunctionRef<void(const MaterialPass &)> sync_cb)
{
  const bool hide_on_camera = ob_handle.object->visibility_flag & OB_HIDE_CAMERA;
  if (hide_on_camera || material.outline_occlusion.gpumat == nullptr) {
    return;
  }

  blender::Material *blender_mat = GPU_material_get_material(material.outline_occlusion.gpumat);

  /* Unlike alpha blending this pass has no per-instance sorting or bindings. Keep the
   * object/slot submission order, but use a single draw over its resource handle range. */
  PassMain::Sub *pass = inst_.pipelines.forward.outline_occlusion_add(
      blender_mat, material.outline_occlusion.gpumat);
  sync_cb(MaterialPass{material.outline_occlusion.gpumat, pass});
}

void SyncModule::sync_common(const ObjectHandle &ob_handle,
                             Span<Material *> materials,
                             Span<GPUMaterial *> gpu_materials)
{
  bool has_volume = false;
  bool is_alpha_blend = false;
  bool has_transparent_shadows = false;
  bool has_time_dependent_shadows = false;
  bool has_view_dependent_shadows = false;
  bool has_position_dependent_shadows = false;
  float inflate_bounds = 0.0f;

  for (const Material *material : materials) {
    has_volume |= material->has_volume;
    if (material->has_volume && !material->has_surface) {
      continue;
    }

    GPUMaterial *gpu_material = material->shading.gpumat;
    /* Marker Only has no surface shader while its marker is compiling/failed, or when baking.
     * Curves and point clouds still reach this shared sync path in those states. */
    if (gpu_material == nullptr) {
      continue;
    }
    blender::Material *bl_material = GPU_material_get_material(gpu_material);

    const bool has_displacement = GPU_material_has_displacement_output(gpu_material) &&
                                  (bl_material->displacement_method != MA_DISPLACEMENT_BUMP);
    const bool has_time_node = GPU_material_is_time_dependent(gpu_material);

    is_alpha_blend |= material->is_alpha_blend_transparent || material->is_outline_shell_blended;
    has_transparent_shadows |= material->has_transparent_shadows ||
                               material->has_outline_shell_transparent_shadows;
    has_time_dependent_shadows |= has_time_node &&
                                  (material->has_transparent_shadows || has_displacement);
    /* View dependent tessellation changes the shadow caster geometry with the camera. */
    has_view_dependent_shadows |= material->tessellation_view_dependent;
    has_position_dependent_shadows |= material->tessellation_position_dependent;
    has_time_dependent_shadows |= material->outline_shell_shadow.gpumat != nullptr &&
                                  GPU_material_is_time_dependent(
                                      material->outline_shell_shadow.gpumat);

    if (has_displacement) {
      inflate_bounds = math::max(inflate_bounds, bl_material->inflate_bounds);
    }
    if (material->outline_shell_shading.gpumat != nullptr ||
        material->outline_shell_shadow.gpumat != nullptr)
    {
      inflate_bounds = math::max(
          inflate_bounds,
          math::max(material->outline_shell_bounds_inflation, bl_material->inflate_bounds));
    }

    inst_.cryptomatte.sync_material(bl_material);
  }

  inst_.cryptomatte.sync_object(ob_handle);

  inst_.shadows.sync_object(ob_handle,
                            is_alpha_blend,
                            has_transparent_shadows,
                            has_time_dependent_shadows,
                            has_view_dependent_shadows,
                            has_position_dependent_shadows);

  if (has_volume) {
    inst_.volume.object_sync(ob_handle);
  }

  if (inflate_bounds != 0.0f) {
    inst_.manager->update_handle_bounds(ob_handle.res_handle, ob_handle, inflate_bounds);
  }

  Vector<GPUMaterial *> attribute_materials = non_null_materials(gpu_materials);
  attribute_materials.reserve(attribute_materials.size() + materials.size() * 3);
  append_npr_and_probe_materials(attribute_materials, materials);
  const Vector<const GPUUniformAttr *> *requests = object_attribute_requests_.lookup_ptr(
      attribute_materials);
  if (requests != nullptr) {
    object_attribute_request_hits_++;
  }
  else {
    requests = &object_attribute_requests_.lookup_or_add_cb(attribute_materials, [&]() {
      Map<uint32_t, const GPUUniformAttr *, 4> attributes;
      for (GPUMaterial *gpumat : attribute_materials) {
        if (const GPUUniformAttrList *list = GPU_material_uniform_attributes(gpumat)) {
          for (const GPUUniformAttr &attribute : list->list) {
            attributes.add(attribute.hash_code, &attribute);
          }
        }
      }
      Vector<const GPUUniformAttr *> result;
      for (const GPUUniformAttr *attribute : attributes.values()) {
        result.append(attribute);
      }
      return result;
    });
  }
  inst_.manager->extract_object_attributes(ob_handle.res_handle, ob_handle, requests->as_span());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mesh
 * \{ */

void SyncModule::sync_mesh(const ObjectRef &ob_ref)
{
  if (!inst_.use_surfaces) {
    return;
  }

  if ((ob_ref.object->dt < OB_SOLID) &&
      (inst_.is_viewport() && inst_.v3d->shading.type != OB_RENDER))
  {
    /** Do not render objects with display type lower than solid when in material preview mode. */
    return;
  }

  ObjectHandle ob_handle = sync_object(ob_ref, inst_.manager->unique_handle(ob_ref));

  bool has_motion = inst_.velocity.step_object_sync(ob_handle);

  MaterialArray &material_array = inst_.materials.material_array_get(ob_handle, has_motion);

  VectorSet<GPUMaterial *> attribute_materials;
  for (const Span<GPUMaterial *> pass_materials :
       {material_array.gpu_materials.as_span(),
        material_array.gpu_materials_npr.as_span(),
        material_array.gpu_materials_outline_shell.as_span()})
  {
    for (GPUMaterial *gpumat : pass_materials) {
      if (gpumat != nullptr) {
        attribute_materials.add(gpumat);
      }
    }
  }
  Span<gpu::Batch *> mat_geom = DRW_cache_object_surface_material_get(
      ob_handle.object, attribute_materials.as_span());
  mesh_attribute_requests_++;
  if (mat_geom.is_empty()) {
    return;
  }

  if (material_array_uses_outline_control(material_array, mat_geom)) {
    inst_.outline.sync_object(ob_handle.object, ob_handle.res_handle);
  }

  Vector<Material *, 8> synced_materials;

  for (auto i : material_array.gpu_materials.index_range()) {
    gpu::Batch *geom = mat_geom[i];
    if (geom == nullptr || material_array.gpu_materials[i] == nullptr) {
      continue;
    }

    Material &material = material_array.materials[i];
    material.tessellation_level = material_tessellation_budget(inst_, ob_handle, material);
    synced_materials.append(&material);

    if (material.has_volume) {
      sync_volume_passes(ob_handle, material, [&](const MaterialPass &pass, int instance) {
        geometry_volume_call(pass.sub_pass,
                             pass.gpumat,
                             inst_.scene,
                             ob_handle.object,
                             geom,
                             ob_handle.res_handle.sub_handle(instance));
      });

      /* Do not render surface if we are rendering a volume object
       * and do not have a surface closure. */
      if (!material.has_surface) {
        continue;
      }
    }

    sync_common_passes(material, [&](const MaterialPass &pass) {
      geometry_call(pass.sub_pass,
                    geom,
                    ob_handle.res_handle,
                    material_pass_tessellation_level(material, pass.gpumat));
    });

    geometry_call(material.outline_shell_prepass.sub_pass, geom, ob_handle.res_handle);
    geometry_call(material.outline_shell_shading.sub_pass, geom, ob_handle.res_handle);
    geometry_call(material.outline_shell_shadow.sub_pass, geom, ob_handle.res_handle);

    sync_outline_occlusion_passes(ob_handle, material, [&](const MaterialPass &pass) {
      geometry_call(pass.sub_pass,
                    geom,
                    ob_handle.res_handle,
                    material_pass_tessellation_level(material, pass.gpumat));
    });

    sync_alpha_blended_passes(
        ob_handle,
        material,
        [&](const MaterialPass &pass, int instance) {
          geometry_call(pass.sub_pass, geom, ob_handle.res_handle.sub_handle(instance));
        },
        true);
  }

  /* Stable equal-depth sorting: all shell slots precede all body slots for this object. */
  for (auto i : material_array.gpu_materials.index_range()) {
    gpu::Batch *geom = mat_geom[i];
    Material &material = material_array.materials[i];
    if (geom == nullptr || material_array.gpu_materials[i] == nullptr ||
        (material.has_volume && !material.has_surface))
    {
      continue;
    }
    sync_alpha_blended_passes(ob_handle, material, [&](const MaterialPass &pass, int instance) {
      /* The overlap masking pre-pass uses the shading material shader. */
      GPUMaterial *gpumat = pass.gpumat ? pass.gpumat : material.shading.gpumat;
      geometry_call(pass.sub_pass,
                    geom,
                    ob_handle.res_handle.sub_handle(instance),
                    material_pass_tessellation_level(material, gpumat));
    });
  }

  sync_common(ob_handle, synced_materials.as_span(), material_array.gpu_materials);
}

bool SyncModule::sync_sculpt(const ObjectRef &ob_ref)
{
  if (!inst_.use_surfaces) {
    return false;
  }

  bool pbvh_draw = BKE_sculptsession_use_pbvh_draw(ob_ref.object, inst_.rv3d) &&
                   !inst_.is_image_render;
  if (!pbvh_draw) {
    return false;
  }

  ObjectHandle ob_handle = sync_object(ob_ref, inst_.manager->unique_handle_for_sculpt(ob_ref));

  bool has_motion = false;
  MaterialArray &material_array = inst_.materials.material_array_get(
      ob_handle, has_motion, false);
  Vector<SculptBatch> batches_npr = sculpt_batches_per_material_get(
      ob_ref.object, material_array.gpu_materials_npr);

  Vector<Material *, 8> synced_materials;

  for (SculptBatch &batch :
       sculpt_batches_per_material_get(ob_ref.object, material_array.gpu_materials))
  {
    gpu::Batch *geom = batch.batch;
    if (geom == nullptr) {
      continue;
    }

    Material &material = material_array.materials[batch.material_slot];
    gpu::Batch *geom_npr = nullptr;
    for (const SculptBatch &batch_npr : batches_npr) {
      if (batch_npr.material_slot == batch.material_slot) {
        geom_npr = batch_npr.batch;
        break;
      }
    }
    synced_materials.append(&material);

    if (material.has_volume) {
      sync_volume_passes(ob_handle, material, [&](const MaterialPass &pass, int instance) {
        geometry_volume_call(pass.sub_pass,
                             pass.gpumat,
                             inst_.scene,
                             ob_handle.object,
                             geom,
                             ob_handle.res_handle.sub_handle(instance));
      });

      /* Do not render surface if we are rendering a volume object
       * and do not have a surface closure. */
      if (!material.has_surface) {
        continue;
      }
    }

    sync_common_passes(material, [&](const MaterialPass &pass) {
      const bool use_npr_geom = pass.gpumat == material.npr.gpumat ||
                                pass.gpumat == material.planar_probe_npr.gpumat ||
                                pass.gpumat == material.lightprobe_sphere_npr.gpumat;
      geometry_call(pass.sub_pass, use_npr_geom ? geom_npr : geom, ob_handle.res_handle);
    });

    sync_outline_occlusion_passes(ob_handle, material, [&](const MaterialPass &pass) {
      geometry_call(pass.sub_pass, geom, ob_handle.res_handle);
    });

    sync_alpha_blended_passes(ob_handle, material, [&](const MaterialPass &pass, int instance) {
      geometry_call(pass.sub_pass, geom, ob_handle.res_handle.sub_handle(instance));
    });
  }

  sync_common(ob_handle, synced_materials.as_span(), material_array.gpu_materials);

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Point Cloud
 * \{ */

void SyncModule::sync_pointcloud(const ObjectRef &ob_ref)
{
  const int material_slot = POINTCLOUD_MATERIAL_NR;

  ObjectHandle ob_handle = sync_object(ob_ref, inst_.manager->unique_handle(ob_ref));

  bool has_motion = inst_.velocity.step_object_sync(ob_handle);

  Material material = inst_.materials.material_get(
      ob_handle, has_motion, material_slot - 1, MAT_GEOM_POINTCLOUD);

  auto drawcall_add = [&](const MaterialPass &matpass, bool dual_sided = false) {
    if (matpass.sub_pass == nullptr) {
      return;
    }
    PassMain::Sub &object_pass = matpass.sub_pass->sub("Point Cloud Sub Pass");
    gpu::Batch *geometry = pointcloud_sub_pass_setup(
        object_pass, ob_handle.object, matpass.gpumat);
    if (dual_sided) {
      /* WORKAROUND: Hack to generate backfaces. Should also be baked into the Index Buf too at
       * some point in the future. */
      object_pass.push_constant("ptcloud_backface", false);
      object_pass.draw(geometry, ob_handle.res_handle);
      object_pass.push_constant("ptcloud_backface", true);
      object_pass.draw(geometry, ob_handle.res_handle);
    }
    else {
      object_pass.push_constant("ptcloud_backface", false);
      object_pass.draw(geometry, ob_handle.res_handle);
    }
  };

  if (material.has_volume) {
    /* Volumes not supported for now. */
    /* NOTE: Point Cloud volumes used to work at some point,
     * but we didn't catch the regression due to the test being disabled. :( */
    if (!material.has_surface) {
      /* Do not render surface if we are rendering a volume object
       * and do not have a surface closure. */
      return;
    }
  }

  sync_common_passes(material, [&](const MaterialPass &pass) { drawcall_add(pass); });

  sync_outline_occlusion_passes(
      ob_handle, material, [&](const MaterialPass &pass) { drawcall_add(pass); });

  sync_alpha_blended_passes(ob_handle, material, [&](const MaterialPass &pass, int /*instance*/) {
    drawcall_add(pass);
  });

  sync_common(ob_handle, {&material}, {material.shading.gpumat});
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Volume Objects
 * \{ */

void SyncModule::sync_volume(const ObjectRef &ob_ref)
{
  if (!inst_.use_volumes) {
    return;
  }

  ObjectHandle ob_handle = sync_object(ob_ref, inst_.manager->unique_handle(ob_ref));

  const int material_slot = VOLUME_MATERIAL_NR;

  /* Motion is not supported on volumes yet. */
  const bool has_motion = false;

  Material material = inst_.materials.material_get(
      ob_handle, has_motion, material_slot - 1, MAT_GEOM_VOLUME);

  if (!material.has_volume) {
    return;
  }

  /* Do not render the object if there is no attribute used in the volume.
   * This mimic Cycles behavior (see #124061). */
  ListBaseT<GPUMaterialAttribute> attr_list = GPU_material_attributes(
      material.volume_material.gpumat);
  if (attr_list.is_empty()) {
    return;
  }

  /* Use bounding box tag empty spaces. */
  gpu::Batch *geom = inst_.volume.unit_cube_batch_get();
  bool is_rendered = false;

  sync_volume_passes(ob_handle, material, [&](const MaterialPass &pass, int /*instance*/) {
    if (pass.sub_pass == nullptr) {
      return;
    }
    BLI_assert_msg(!ob_handle.is_range(),
                   "volume_object_grids_init pass setup is object/instance specific.");
    PassMain::Sub *object_pass = volume_sub_pass(
        *pass.sub_pass, inst_.scene, ob_handle.object, pass.gpumat);
    if (object_pass != nullptr) {
      object_pass->draw(geom, ob_handle.res_handle);
      is_rendered = true;
    }
  });

  if (!is_rendered) {
    return;
  }

  inst_.volume.object_sync(ob_handle);

  inst_.manager->extract_object_attributes(
      ob_handle.res_handle, ob_handle, material.volume_material.gpumat);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Hair
 * \{ */

void SyncModule::sync_curves(const ObjectRef &ob_ref, HairParticleInfo const *hair_particle)
{
  if (!inst_.use_curves) {
    return;
  }

  int mat_nr = CURVES_MATERIAL_NR;
  if (hair_particle != nullptr) {
    mat_nr = hair_particle->psys.part->omat;
  }

  ObjectHandle ob_handle = hair_particle ?
                               ObjectHandle(ob_ref,
                                            inst_.manager->resource_handle_for_psys(
                                                ob_ref, ob_ref.object_to_world()),
                                            hair_particle->recalc_flags,
                                            hair_particle->sub_key) :
                               sync_object(ob_ref, inst_.manager->unique_handle(ob_ref));

  bool has_motion = inst_.velocity.step_object_sync(ob_handle, hair_particle);
  Material material = inst_.materials.material_get(
      ob_handle, has_motion, mat_nr - 1, MAT_GEOM_CURVES);

  auto drawcall_add = [&](const MaterialPass &matpass) {
    if (matpass.sub_pass == nullptr) {
      return;
    }
    if (hair_particle != nullptr) {
      PassMain::Sub &sub_pass = matpass.sub_pass->sub("Hair SubPass");
      gpu::Batch *geometry = hair_sub_pass_setup(
          sub_pass, inst_.scene, ob_ref, &hair_particle->psys, &hair_particle->md, matpass.gpumat);
      sub_pass.draw(geometry, ob_handle.res_handle);
    }
    else {
      PassMain::Sub &sub_pass = matpass.sub_pass->sub("Curves SubPass");
      const char *error = nullptr;
      gpu::Batch *geometry = curves_sub_pass_setup(
          sub_pass, inst_.scene, ob_ref.object, error, matpass.gpumat);
      if (error) {
        inst_.info_append(error);
      }
      sub_pass.draw(geometry, ob_handle.res_handle);
    }
  };

  if (material.has_volume) {
    /* Volumes not supported for now. */
    if (!material.has_surface) {
      /* Do not render surface if we are rendering a volume object
       * and do not have a surface closure. */
      return;
    }
  }

  sync_common_passes(material, [&](const MaterialPass &pass) { drawcall_add(pass); });

  sync_outline_occlusion_passes(
      ob_handle, material, [&](const MaterialPass &pass) { drawcall_add(pass); });

  sync_alpha_blended_passes(ob_handle, material, [&](const MaterialPass &pass, int /*instance*/) {
    drawcall_add(pass);
  });

  sync_common(ob_handle, {&material}, {material.shading.gpumat});
}

/** \} */

void foreach_hair_particle(Instance &inst, ObjectRef &ob_ref, HairHandleCallback callback)
{
  uint sub_key = 1;

  for (ModifierData &md : ob_ref.object->modifiers) {
    if (md.type == eModifierType_ParticleSystem) {
      ParticleSystem *particle_sys = reinterpret_cast<ParticleSystemModifierData *>(&md)->psys;
      ParticleSettings *part_settings = particle_sys->part;
      /* Only use the viewport drawing mode for material preview. */
      const int draw_as = (part_settings->draw_as == PART_DRAW_REND || !inst.is_viewport()) ?
                              part_settings->ren_as :
                              part_settings->draw_as;
      if (draw_as != PART_DRAW_PATH ||
          !DRW_object_is_visible_psys_in_active_context(ob_ref.object, particle_sys))
      {
        continue;
      }
      callback({md, *particle_sys, uint(particle_sys->recalc), sub_key++});
    }
  }
}

}  // namespace blender::eevee
