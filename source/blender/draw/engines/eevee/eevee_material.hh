/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 */

#pragma once

#include "DNA_material_types.h"

#include "DRW_render.hh"

#include "BLI_enum_flags.hh"
#include "BLI_hash.hh"
#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"

#include "GPU_material.hh"

#include "draw_pass.hh"

#include "eevee_material_shared.hh"
#include "eevee_shader.hh"
#include "eevee_sync.hh"

namespace blender {

struct bNodeSocketValueFloat;
struct bNodeSocketValueRGBA;

namespace eevee {

class Instance;

/* -------------------------------------------------------------------- */
/** \name MaterialKey
 *
 * \{ */

static inline bool geometry_type_has_surface(eMaterialGeometry geometry_type)
{
  return geometry_type < MAT_GEOM_VOLUME;
}

enum eMaterialDisplacement {
  MAT_DISPLACEMENT_BUMP = 0,
  MAT_DISPLACEMENT_VERTEX_WITH_BUMP,
};

static inline eMaterialDisplacement to_displacement_type(int displacement_method)
{
  switch (displacement_method) {
    case MA_DISPLACEMENT_DISPLACE:
      /* Currently unsupported. Revert to vertex displacement + bump. */
      ATTR_FALLTHROUGH;
    case MA_DISPLACEMENT_BOTH:
      return MAT_DISPLACEMENT_VERTEX_WITH_BUMP;
    default:
      return MAT_DISPLACEMENT_BUMP;
  }
}

enum eMaterialThickness {
  /* These maps directly to thickness mode. */
  MAT_THICKNESS_SPHERE = 0,
  MAT_THICKNESS_SLAB,
};

static inline eMaterialThickness to_thickness_type(int thickness_mode)
{
  switch (thickness_mode) {
    case MA_THICKNESS_SLAB:
      return MAT_THICKNESS_SLAB;
    default:
      return MAT_THICKNESS_SPHERE;
  }
}

static inline void material_type_from_shader_uuid(uint64_t shader_uuid,
                                                  eMaterialPipeline &pipeline_type,
                                                  eMaterialGeometry &geometry_type,
                                                  eMaterialDisplacement &displacement_type,
                                                  eMaterialThickness &thickness_type,
                                                  eMaterialProbe &probe_capture,
                                                  bool &transparent_shadows,
                                                  bool &use_outline,
                                                  bool &depth_offset_affect_lighting)
{
  const uint64_t geometry_mask = ((1u << 4u) - 1u);
  const uint64_t pipeline_mask = ((1u << 4u) - 1u);
  const uint64_t thickness_mask = ((1u << 1u) - 1u);
  const uint64_t displacement_mask = ((1u << 1u) - 1u);
  const uint64_t probe_mask = ((1u << 2u) - 1u);
  geometry_type = static_cast<eMaterialGeometry>(shader_uuid & geometry_mask);
  pipeline_type = static_cast<eMaterialPipeline>((shader_uuid >> 4u) & pipeline_mask);
  displacement_type = static_cast<eMaterialDisplacement>((shader_uuid >> 8u) & displacement_mask);
  thickness_type = static_cast<eMaterialThickness>((shader_uuid >> 9u) & thickness_mask);
  probe_capture = static_cast<eMaterialProbe>((shader_uuid >> 10u) & probe_mask);
  transparent_shadows = (shader_uuid >> 12u) & 1u;
  use_outline = (shader_uuid >> 13u) & 1u;
  depth_offset_affect_lighting = (shader_uuid >> 14u) & 1u;
}

static inline uint64_t shader_uuid_from_material_type(
    eMaterialPipeline pipeline_type,
    eMaterialGeometry geometry_type,
    eMaterialDisplacement displacement_type = MAT_DISPLACEMENT_BUMP,
    eMaterialThickness thickness_type = MAT_THICKNESS_SPHERE,
    eMaterialProbe probe_capture = MAT_PROBE_NONE,
    char blend_flags = 0,
    bool use_outline = true,
    bool depth_offset_affect_lighting = false)
{
  BLI_assert(int64_t(displacement_type) < (1 << 1));
  BLI_assert(int64_t(thickness_type) < (1 << 1));
  BLI_assert(int64_t(probe_capture) < (1 << 2));
  BLI_assert(int64_t(geometry_type) < (1 << 4));
  BLI_assert(int64_t(pipeline_type) < (1 << 4));
  uint64_t transparent_shadows = blend_flags & MA_BL_TRANSPARENT_SHADOW ? 1 : 0;

  uint64_t uuid;
  uuid = geometry_type;
  uuid |= pipeline_type << 4;
  uuid |= displacement_type << 8;
  uuid |= thickness_type << 9;
  uuid |= uint64_t(probe_capture) << 10;
  uuid |= transparent_shadows << 12;
  uuid |= uint64_t(use_outline) << 13;
  uuid |= uint64_t(depth_offset_affect_lighting) << 14;
  return uuid;
}

enum eClosureBits : uint32_t {
  CLOSURE_NONE = 0u,
  CLOSURE_DIFFUSE = (1u << 0u),
  CLOSURE_SSS = (1u << 1u),
  CLOSURE_REFLECTION = (1u << 2u),
  CLOSURE_REFRACTION = (1u << 3u),
  CLOSURE_TRANSLUCENT = (1u << 4u),
  CLOSURE_TRANSPARENCY = (1u << 8u),
  CLOSURE_EMISSION = (1u << 9u),
  CLOSURE_HOLDOUT = (1u << 10u),
  CLOSURE_VOLUME = (1u << 11u),
  CLOSURE_AMBIENT_OCCLUSION = (1u << 12u),
  CLOSURE_SHADER_TO_RGBA = (1u << 13u),
  CLOSURE_CLEARCOAT = (1u << 14u),

  CLOSURE_TRANSMISSION = CLOSURE_SSS | CLOSURE_REFRACTION | CLOSURE_TRANSLUCENT,
};
ENUM_OPERATORS(eClosureBits)

static inline eClosureBits shader_closure_bits_from_flag(const GPUMaterial *gpumat)
{
  eClosureBits closure_bits = eClosureBits(0);
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_DIFFUSE)) {
    closure_bits |= CLOSURE_DIFFUSE;
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_TRANSPARENT)) {
    closure_bits |= CLOSURE_TRANSPARENCY;
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_TRANSLUCENT)) {
    closure_bits |= CLOSURE_TRANSLUCENT;
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_EMISSION)) {
    closure_bits |= CLOSURE_EMISSION;
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_GLOSSY)) {
    closure_bits |= CLOSURE_REFLECTION;
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_COAT)) {
    closure_bits |= CLOSURE_CLEARCOAT;
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_SUBSURFACE)) {
    closure_bits |= CLOSURE_SSS;
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_REFRACT)) {
    closure_bits |= CLOSURE_REFRACTION;
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_HOLDOUT)) {
    closure_bits |= CLOSURE_HOLDOUT;
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_AO)) {
    closure_bits |= CLOSURE_AMBIENT_OCCLUSION;
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_SHADER_TO_RGBA)) {
    closure_bits |= CLOSURE_SHADER_TO_RGBA;
  }
  return closure_bits;
}

/* Count the number of closure bins required for the given combination of closure types. */
static inline int to_gbuffer_bin_count(const eClosureBits closure_bits)
{
  int closure_data_slots = 0;
  if (closure_bits & CLOSURE_DIFFUSE) {
    if ((closure_bits & CLOSURE_TRANSLUCENT) && !(closure_bits & CLOSURE_CLEARCOAT)) {
      /* Special case to allow translucent with diffuse without noise.
       * Revert back to noise if clear coat is present. */
      closure_data_slots |= (1 << 2);
    }
    else {
      closure_data_slots |= (1 << 0);
    }
  }
  if (closure_bits & CLOSURE_SSS) {
    closure_data_slots |= (1 << 0);
  }
  if (closure_bits & CLOSURE_REFRACTION) {
    closure_data_slots |= (1 << 0);
  }
  if (closure_bits & CLOSURE_TRANSLUCENT) {
    closure_data_slots |= (1 << 0);
  }
  if (closure_bits & CLOSURE_REFLECTION) {
    closure_data_slots |= (1 << 1);
  }
  if (closure_bits & CLOSURE_CLEARCOAT) {
    closure_data_slots |= (1 << 2);
  }
  return count_bits_i(closure_data_slots);
};

static inline eMaterialGeometry to_material_geometry(const Object *ob)
{
  switch (ob->type) {
    case OB_CURVES:
      return MAT_GEOM_CURVES;
    case OB_VOLUME:
      return MAT_GEOM_VOLUME;
    case OB_POINTCLOUD:
      return MAT_GEOM_POINTCLOUD;
    default:
      return MAT_GEOM_MESH;
  }
}

static inline bool pipeline_uses_material_ztest(eMaterialPipeline pipeline_type)
{
  return ELEM(pipeline_type,
              MAT_PIPE_PREPASS_DEFERRED,
              MAT_PIPE_PREPASS_DEFERRED_VELOCITY,
              MAT_PIPE_PREPASS_FORWARD,
              MAT_PIPE_PREPASS_FORWARD_VELOCITY,
              MAT_PIPE_PREPASS_PLANAR,
              MAT_PIPE_PREPASS_OVERLAP,
              MAT_PIPE_FORWARD);
}

static inline bool pipeline_uses_material_write_state(eMaterialPipeline pipeline_type)
{
  return ELEM(pipeline_type,
              MAT_PIPE_PREPASS_DEFERRED,
              MAT_PIPE_PREPASS_DEFERRED_VELOCITY,
              MAT_PIPE_PREPASS_FORWARD,
              MAT_PIPE_PREPASS_FORWARD_VELOCITY,
              MAT_PIPE_PREPASS_PLANAR,
              MAT_PIPE_PREPASS_OVERLAP,
              MAT_PIPE_DEFERRED,
              MAT_PIPE_DEFERRED_NPR,
              MAT_PIPE_FORWARD);
}

/**
 * Unique key to identify each material in the hash-map.
 * This is above the shader binning.
 */
struct MaterialKey {
  blender::Material *mat;
  uint64_t options;
  uint32_t stencil_options;
  short stencil_order;

  MaterialKey(blender::Material *mat_,
              eMaterialGeometry geometry,
              eMaterialPipeline pipeline,
              short visibility_flags,
              short refraction_layer,
              bool use_outline)
      : mat(mat_)
  {
    options = shader_uuid_from_material_type(pipeline,
                                             geometry,
                                             to_displacement_type(mat_->displacement_method),
                                             to_thickness_type(mat_->thickness_mode),
                                             MAT_PROBE_NONE,
                                             mat_->blend_flag,
                                             use_outline,
                                             mat_->depth_offset_affect_lighting != 0);
    options = (options << 1) | (visibility_flags & OB_HIDE_CAMERA ? 0 : 1);
    options = (options << 1) | (visibility_flags & OB_HIDE_SHADOW ? 0 : 1);
    options = (options << 1) | (visibility_flags & OB_HIDE_PROBE_CUBEMAP ? 0 : 1);
    options = (options << 1) | (visibility_flags & OB_HIDE_PROBE_PLANAR ? 0 : 1);
    options = (options << 16) | uint16_t(refraction_layer);
    options = (options << 3) | uint64_t(material_ztest_mode_get(*mat_));
    options = (options << 1) | uint64_t(material_color_write_get(*mat_));
    options = (options << 1) | uint64_t(material_depth_write_get(*mat_));
    const uint64_t stencil_enabled = mat_->stencil_enabled != 0;
    stencil_order = stencil_enabled ? mat_->stencil_order : 0;
    stencil_options = uint32_t(stencil_enabled);
    stencil_options = (stencil_options << 4) |
                      (stencil_enabled ? uint32_t(mat_->stencil_reference & 0x0F) : 0);
    stencil_options = (stencil_options << 4) |
                      (stencil_enabled ? uint32_t(mat_->stencil_read_mask & 0x0F) : 0);
    stencil_options = (stencil_options << 4) |
                      (stencil_enabled ? uint32_t(mat_->stencil_write_mask & 0x0F) : 0);
    stencil_options = (stencil_options << 3) |
                      (stencil_enabled ? uint32_t(mat_->stencil_test & 0x07) : 0);
    stencil_options = (stencil_options << 3) |
                      (stencil_enabled ? uint32_t(mat_->stencil_pass_op & 0x07) : 0);
    stencil_options = (stencil_options << 3) |
                      (stencil_enabled ? uint32_t(mat_->stencil_fail_op & 0x07) : 0);
    stencil_options = (stencil_options << 3) |
                      (stencil_enabled ? uint32_t(mat_->stencil_zfail_op & 0x07) : 0);
  }

  uint64_t hash() const
  {
    return blender::get_default_hash(mat, options, stencil_options, stencil_order);
  }

  bool operator==(const MaterialKey &k) const
  {
    return (mat == k.mat) && (options == k.options) && (stencil_options == k.stencil_options) &&
           (stencil_order == k.stencil_order);
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name ShaderKey
 *
 * \{ */

/**
 * Key used to find the sub-pass that already renders objects with the same shader.
 * This avoids the cost associated with shader switching.
 * This is below the material binning.
 * Should only include pipeline options that are not baked in the shader itself.
 */
struct ShaderKey {
  gpu::Shader *shader;
  uint64_t options;

  ShaderKey(GPUMaterial *gpumat,
            blender::Material *blender_mat,
            eMaterialPipeline pipeline_type,
            eMaterialProbe probe_capture,
            short refraction_layer)
  {
    shader = GPU_material_get_shader(gpumat);
    options = uint64_t(shader_closure_bits_from_flag(gpumat));
    options = (options << 8) | blender_mat->blend_flag;
    options = (options << 2) | uint64_t(material_surface_cull_method_get(*blender_mat));
    options = (options << 3) |
              uint64_t(pipeline_uses_material_ztest(pipeline_type) ?
                           material_ztest_mode_get(*blender_mat) :
                           MA_ZTEST_LESS_EQUAL);
    const bool use_material_write_state = (probe_capture == MAT_PROBE_NONE) &&
                                          pipeline_uses_material_write_state(pipeline_type);
    options = (options << 1) |
              uint64_t(use_material_write_state ? material_color_write_get(*blender_mat) : true);
    options = (options << 1) |
              uint64_t(use_material_write_state ? material_depth_write_get(*blender_mat) : true);
    options = (options << 2) | uint64_t(probe_capture);
    options = (options << 16) | uint16_t(refraction_layer);
  }

  uint64_t hash() const
  {
    return uint64_t(shader) + options;
  }

  bool operator==(const ShaderKey &k) const
  {
    return (shader == k.shader) && (options == k.options);
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Material
 *
 * \{ */

struct MaterialPass {
  GPUMaterial *gpumat;
  PassMain::Sub *sub_pass;
};

struct Material {
  bool is_alpha_blend_transparent;
  bool has_transparent_shadows;
  bool has_surface;
  bool has_volume;
  bool uses_outline_control;
  MaterialPass shadow;
  MaterialPass shading;
  MaterialPass npr;
  MaterialPass prepass;
  MaterialPass stencil;
  MaterialPass overlap_masking;
  MaterialPass outline_occlusion;
  MaterialPass capture;
  MaterialPass lightprobe_sphere_prepass;
  MaterialPass lightprobe_sphere_shading;
  MaterialPass lightprobe_sphere_npr;
  MaterialPass planar_probe_prepass;
  MaterialPass planar_probe_shading;
  MaterialPass planar_probe_npr;
  MaterialPass volume_occupancy;
  MaterialPass volume_material;
};

struct MaterialArray {
  Vector<Material> materials;
  Vector<GPUMaterial *> gpu_materials;
  Vector<GPUMaterial *> gpu_materials_npr;
};

class MaterialModule {
 public:
  blender::Material *diffuse_mat;
  blender::Material *metallic_mat;
  blender::Material *default_surface;
  blender::Material *default_volume;

  blender::Material *material_override = nullptr;

  int64_t queued_shaders_count = 0;
  int64_t queued_textures_count = 0;
  int64_t queued_optimize_shaders_count = 0;

 private:
  Instance &inst_;
  bool has_time_dependent_materials_ = false;

  Map<MaterialKey, Material> material_map_;
  Map<ShaderKey, PassMain::Sub *> shader_map_;
  Set<blender::Material *> depth_offset_shadow_disabled_mats_;
  Set<blender::Material *> current_depth_offset_shadow_disabled_mats_;

  MaterialArray material_array_;

  blender::Material *error_mat_;

  uint64_t gpu_pass_last_update_ = 0;
  uint64_t gpu_pass_next_update_ = 0;

  Vector<GPUMaterialTexture *> texture_loading_queue_;

 public:
  MaterialModule(Instance &inst);
  ~MaterialModule();

  void begin_sync();
  void end_sync();
  bool has_time_dependent_materials() const
  {
    return has_time_dependent_materials_;
  }
  int npr_material_count() const;
  int raycast_material_count() const;
  int glsl_function_material_count() const;
  bool has_visible_outline_materials() const;
  bool material_uses_outline_control(const blender::Material *material) const;

  /**
   * Returned Material references are valid until the next call to this function or material_get().
   */
  MaterialArray &material_array_get(Object *ob, bool has_motion);
  /**
   * Returned Material references are valid until the next call to this function or
   * material_array_get().
   */
  Material &material_get(Object *ob, bool has_motion, int mat_nr, eMaterialGeometry geometry_type);

  /* Request default materials and return DEFAULT_MATERIALS if they are compiled. */
  ShaderGroups default_materials_load_async()
  {
    return default_materials_load(false);
  }
  ShaderGroups default_materials_wait_ready()
  {
    return default_materials_load(true);
  }

 private:
  Material &material_sync(Object *ob,
                          blender::Material *blender_mat,
                          eMaterialGeometry geometry_type,
                          bool has_motion);

  /** Return correct material or empty default material if slot is empty. */
  blender::Material *material_from_slot(Object *ob, int slot);
  MaterialPass material_pass_get(Object *ob,
                                 blender::Material *blender_mat,
                                 eMaterialPipeline pipeline_type,
                                 eMaterialGeometry geometry_type,
                                 eMaterialProbe probe_capture = MAT_PROBE_NONE,
                                 bool register_pass = true);

  /* Push unloaded texture used by this material to the texture loading queue. */
  void queue_texture_loading(GPUMaterial *material);

  ShaderGroups default_materials_load(bool block_until_ready = false);
};

/** \} */

}  // namespace eevee
}  // namespace blender
