/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 *
 * Shader module that manage shader libraries, deferred compilation,
 * and static shader usage.
 */

#include "GPU_capabilities.hh"

#include "BKE_material.hh"
#include "BKE_node_runtime.hh"

#include "DNA_world_types.h"
#include "DNA_light_types.h"

#include "gpu_shader_create_info.hh"

#include "NOD_shader.h"

#include "eevee_shader.hh"

#include "eevee_shadow.hh"

#include "BLI_assert.h"
#include "BLI_math_bits.h"
#include "BLI_set.hh"

namespace blender::eevee {

static bool material_output_has_depth_offset(bNodeTree *nodetree)
{
  if (nodetree == nullptr) {
    return false;
  }
  nodetree->ensure_topology_cache();
  bNode *output = ntreeShaderOutputNode(nodetree, SHD_OUTPUT_EEVEE);
  if (output == nullptr) {
    return false;
  }
  const bNodeSocket *depth_offset = output->input_by_identifier("Depth Offset");
  return depth_offset != nullptr && depth_offset->is_available() &&
         depth_offset->is_directly_linked();
}

static bool material_depth_offset_affects_lighting(const blender::Material *material)
{
  return material == nullptr || material->depth_offset_affect_lighting != 0;
}

static bool material_pipeline_supports_depth_offset(eMaterialPipeline pipeline_type,
                                                    eMaterialGeometry geometry_type)
{
  return geometry_type_has_surface(geometry_type) &&
         ELEM(pipeline_type,
              MAT_PIPE_PREPASS_FORWARD_VELOCITY,
              MAT_PIPE_PREPASS_DEFERRED_VELOCITY,
              MAT_PIPE_PREPASS_OVERLAP,
              MAT_PIPE_PREPASS_FORWARD,
              MAT_PIPE_PREPASS_DEFERRED,
              MAT_PIPE_PREPASS_PLANAR,
              MAT_PIPE_DEFERRED,
              MAT_PIPE_DEFERRED_NPR,
              MAT_PIPE_FORWARD);
}

static const GPUMaterialGeneratedSource *material_generated_source_find(
    const GPUMaterial *gpumat, const StringRefNull filename)
{
  for (int i = 0; i < GPU_material_generated_source_count(gpumat); i++) {
    const GPUMaterialGeneratedSource *generated_source = GPU_material_generated_source_get(gpumat,
                                                                                           i);
    if (generated_source != nullptr && generated_source->filename == filename) {
      return generated_source;
    }
  }
  return nullptr;
}

static void material_generated_dependency_append(const GPUMaterial *gpumat,
                                                 const StringRefNull dependency_name,
                                                 Set<StringRefNull> &r_static_dependencies,
                                                 Set<StringRefNull> &r_emitted_generated_sources,
                                                 std::stringstream &r_generated_source_block)
{
  if (dependency_name.is_empty()) {
    return;
  }

  const GPUMaterialGeneratedSource *generated_source = material_generated_source_find(
      gpumat, dependency_name);
  if (generated_source == nullptr) {
    r_static_dependencies.add(dependency_name);
    return;
  }

  if (!r_emitted_generated_sources.add(dependency_name)) {
    return;
  }

  for (const std::string &generated_dependency : generated_source->dependencies) {
    material_generated_dependency_append(gpumat,
                                         StringRefNull(generated_dependency.c_str()),
                                         r_static_dependencies,
                                         r_emitted_generated_sources,
                                         r_generated_source_block);
  }

  r_generated_source_block << generated_source->content;
  if (!generated_source->content.empty() && generated_source->content.back() != '\n') {
    r_generated_source_block << "\n";
  }
  r_generated_source_block << "\n";
}

static void material_graph_dependencies_append(const GPUMaterial *gpumat,
                                               const Vector<StringRefNull> &dependencies,
                                               Set<StringRefNull> &r_static_dependencies,
                                               Set<StringRefNull> &r_emitted_generated_sources,
                                               std::stringstream &r_generated_source_block)
{
  for (const StringRefNull dependency_name : dependencies) {
    material_generated_dependency_append(gpumat,
                                         dependency_name,
                                         r_static_dependencies,
                                         r_emitted_generated_sources,
                                         r_generated_source_block);
  }
}

static Vector<StringRefNull> material_dependencies_finalize(const Set<StringRefNull> &dependencies)
{
  Vector<StringRefNull> result;
  result.reserve(dependencies.size());
  for (const StringRefNull dependency_name : dependencies) {
    result.append(dependency_name);
  }
  std::ranges::sort(result);
  return result;
}

static bool material_dependency_tree_contains(const GPUMaterial *gpumat,
                                              const StringRefNull dependency_name,
                                              const StringRefNull needle,
                                              Set<std::string> &visited)
{
  if (dependency_name == needle) {
    return true;
  }
  if (dependency_name.is_empty() || !visited.add(std::string(dependency_name.c_str()))) {
    return false;
  }
  const GPUMaterialGeneratedSource *generated_source = material_generated_source_find(
      gpumat, dependency_name);
  if (generated_source == nullptr) {
    return false;
  }
  for (const std::string &dependency : generated_source->dependencies) {
    if (material_dependency_tree_contains(
            gpumat, StringRefNull(dependency.c_str()), needle, visited))
    {
      return true;
    }
  }
  return false;
}

static bool material_graph_dependency_tree_contains(const GPUMaterial *gpumat,
                                                    const GPUGraphOutput &graph,
                                                    const StringRefNull needle)
{
  Set<std::string> visited;
  for (const StringRefNull dependency_name : graph.dependencies) {
    if (material_dependency_tree_contains(gpumat, dependency_name, needle, visited)) {
      return true;
    }
  }
  return false;
}

static bool material_dependency_tree_source_contains(const GPUMaterial *gpumat,
                                                     const StringRefNull dependency_name,
                                                     const StringRefNull needle,
                                                     Set<std::string> &visited)
{
  if (dependency_name.is_empty() || !visited.add(std::string(dependency_name.c_str()))) {
    return false;
  }
  const GPUMaterialGeneratedSource *generated_source = material_generated_source_find(
      gpumat, dependency_name);
  if (generated_source == nullptr) {
    return false;
  }
  if (generated_source->content.find(needle.c_str()) != std::string::npos) {
    return true;
  }
  for (const std::string &dependency : generated_source->dependencies) {
    if (material_dependency_tree_source_contains(
            gpumat, StringRefNull(dependency.c_str()), needle, visited))
    {
      return true;
    }
  }
  return false;
}

static bool material_graph_dependency_source_contains(const GPUMaterial *gpumat,
                                                      const GPUGraphOutput &graph,
                                                      const StringRefNull needle)
{
  Set<std::string> visited;
  for (const StringRefNull dependency_name : graph.dependencies) {
    if (material_dependency_tree_source_contains(gpumat, dependency_name, needle, visited)) {
      return true;
    }
  }
  return false;
}

static bool material_graph_uses_glsl_light_access(const GPUMaterial *gpumat,
                                                  const GPUGraphOutput &graph)
{
  return material_graph_dependency_tree_contains(
      gpumat, graph, "gpu_shader_material_glsl_light_access.glsl");
}

static bool material_graph_serialized_contains(const GPUGraphOutput &graph,
                                               const StringRefNull needle)
{
  return graph.serialized.find(needle.c_str()) != std::string::npos;
}

static bool material_depth_offset_graph_has_unsupported_dependencies(const GPUMaterial *gpumat,
                                                                     const GPUGraphOutput &graph)
{
  return material_graph_serialized_contains(graph, "node_shader_to_rgba(") ||
         material_graph_serialized_contains(graph, "node_shader_info(") ||
         material_graph_serialized_contains(graph, "node_scene_color_handle_only(") ||
         material_graph_serialized_contains(graph, "node_screenspace_info(") ||
         material_graph_serialized_contains(graph, "node_light_probe_color(") ||
         material_graph_dependency_source_contains(gpumat, graph, "glsl_ambient_lighting") ||
         material_graph_dependency_source_contains(gpumat, graph, "glsl_light_shadow");
}

static bool material_depth_offset_graph_uses_supported_light_access(
    const GPUMaterial *gpumat, const std::optional<GPUGraphOutput> &graph)
{
  return graph.has_value() &&
         !material_depth_offset_graph_has_unsupported_dependencies(gpumat, *graph) &&
         material_graph_uses_glsl_light_access(gpumat, *graph);
}

/* -------------------------------------------------------------------- */
/** \name Module
 *
 * \{ */

ShaderModule *ShaderModule::module_get()
{
  return &get_static_cache().get();
}

void ShaderModule::module_free()
{
  get_static_cache().release();
}

ShaderModule::ShaderModule()
{
  for (auto i : IndexRange(MAX_SHADER_TYPE)) {
    const char *name = static_shader_create_info_name_get(eShaderType(i));
#ifndef NDEBUG
    if (name == nullptr) {
      std::cerr << "EEVEE: Missing case for eShaderType(" << i
                << ") in static_shader_create_info_name_get().";
      BLI_assert(0);
    }
    const GPUShaderCreateInfo *create_info = GPU_shader_create_info_get(name);
    BLI_assert_msg(create_info != nullptr, "EEVEE: Missing create info for static shader.");
#endif
    shaders_[i] = StaticShader(name);
  }
}

ShaderModule::~ShaderModule()
{
  /* Cancel compilation to avoid asserts on exit at ShaderCompiler destructor. */

  /* Specializations first, to avoid releasing the base shader while the specialization compilation
   * is still in flight. */
  for (Vector<AsyncSpecializationHandle> &handles : specialization_handles_.values()) {
    for (AsyncSpecializationHandle &handle : handles) {
      if (handle) {
        GPU_shader_async_specialization_cancel(handle);
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Static shaders
 *
 * \{ */

ShaderGroups ShaderModule::static_shaders_load(const ShaderGroups request_bits,
                                               bool block_until_ready)
{
  std::lock_guard lock(mutex_);

  ShaderGroups ready = ShaderGroups::NONE;
  auto request = [&](ShaderGroups bit, Span<eShaderType> shader_types) {
    if (request_bits & bit) {
      bool all_loaded = true;
      for (eShaderType shader : shader_types) {
        if (shaders_[shader].is_ready()) {
          /* Noop. */
        }
        else if (block_until_ready) {
          shaders_[shader].get();
        }
        else {
          shaders_[shader].ensure_compile_async();
          all_loaded = false;
        }
      }
      if (all_loaded) {
        ready |= bit;
      }
    }
  };

#define AS_SPAN(arr) Span<eShaderType>(arr, ARRAY_SIZE(arr))
  {
    /* These are the slowest shaders by far. Submitting them first make sure they overlap with
     * other shaders compilation. */
    const eShaderType shader_list[] = {DEFERRED_LIGHT_TRIPLE,
                                       DEFERRED_LIGHT_SINGLE,
                                       DEFERRED_LIGHT_DOUBLE,
                                       DEFERRED_COMBINE,
                                       DEFERRED_AOV_CLEAR,
                                       DEFERRED_TILE_CLASSIFY,
                                       OUTLINE_DETECT,
                                       OUTLINE_JFA_INIT,
                                       OUTLINE_FACTOR_BLUR,
                                       OUTLINE_JFA_STEP,
                                       OUTLINE_RESOLVE,
                                       OUTLINE_FREESTYLE};
    request(DEFERRED_LIGHTING_SHADERS, AS_SPAN(shader_list));
  }
  {
    const eShaderType shader_list[] = {AMBIENT_OCCLUSION_PASS};
    request(AMBIENT_OCCLUSION_SHADERS, AS_SPAN(shader_list));
  }
  {
    const eShaderType shader_list[] = {RENDERPASS_CLEAR,
                                       RENDER_TEXTURE_EXTRACT_RGBA16F,
                                       RENDER_TEXTURE_EXTRACT_RGBA32F,
                                       RENDER_TEXTURE_EXTRACT_R16F,
                                       RENDER_TEXTURE_EXTRACT_R32F,
                                       NATIVE_POSTFX_OUTPUT_EXTRACT,
                                       NATIVE_POSTFX_OUTPUT_PACK_COLOR,
                                       NATIVE_POSTFX_OUTPUT_PACK_VALUE,
                                       FILM_COPY,
                                       FILM_COMP,
                                       FILM_CRYPTOMATTE_POST,
                                       FILM_FRAG,
                                       FILM_PASS_CONVERT_COMBINED,
                                       FILM_PASS_CONVERT_DEPTH,
                                       FILM_PASS_CONVERT_VALUE,
                                       FILM_PASS_CONVERT_COLOR,
                                       FILM_PASS_CONVERT_CRYPTOMATTE};
    request(FILM_SHADERS, AS_SPAN(shader_list));
  }
  {
    const eShaderType shader_list[] = {DEFERRED_CAPTURE_EVAL};
    request(DEFERRED_CAPTURE_SHADERS, AS_SPAN(shader_list));
  }
  {
    const eShaderType shader_list[] = {DEFERRED_PLANAR_EVAL};
    request(DEFERRED_PLANAR_SHADERS, AS_SPAN(shader_list));
  }
  {
    const eShaderType shader_list[] = {DOF_BOKEH_LUT,
                                       DOF_DOWNSAMPLE,
                                       DOF_FILTER,
                                       DOF_GATHER_BACKGROUND_LUT,
                                       DOF_GATHER_BACKGROUND,
                                       DOF_GATHER_FOREGROUND_LUT,
                                       DOF_GATHER_FOREGROUND,
                                       DOF_GATHER_HOLE_FILL,
                                       DOF_REDUCE,
                                       DOF_RESOLVE_LUT,
                                       DOF_RESOLVE,
                                       DOF_SCATTER,
                                       DOF_SETUP,
                                       DOF_STABILIZE,
                                       DOF_TILES_DILATE_MINABS,
                                       DOF_TILES_DILATE_MINMAX,
                                       DOF_TILES_FLATTEN};
    request(DEPTH_OF_FIELD_SHADERS, AS_SPAN(shader_list));
  }
  {
    const eShaderType shader_list[] = {HIZ_UPDATE, HIZ_UPDATE_LAYER};
    request(HIZ_SHADERS, AS_SPAN(shader_list));
  }
  {
    const eShaderType shader_list[] = {
        HORIZON_DENOISE, HORIZON_RESOLVE, HORIZON_SCAN, HORIZON_SETUP};
    request(HORIZON_SCAN_SHADERS, AS_SPAN(shader_list));
  }
  {
    const eShaderType shader_list[] = {LIGHT_CULLING_DEBUG,
                                       LIGHT_CULLING_SELECT,
                                       LIGHT_CULLING_SORT,
                                       LIGHT_CULLING_TILE,
                                       LIGHT_CULLING_ZBIN,
                                       LIGHT_SHADOW_SETUP};
    request(LIGHT_CULLING_SHADERS, AS_SPAN(shader_list));
  }
  {
    const eShaderType shader_list[] = {
        LIGHTPROBE_IRRADIANCE_BOUNDS, LIGHTPROBE_IRRADIANCE_OFFSET, LIGHTPROBE_IRRADIANCE_RAY};
    request(IRRADIANCE_BAKE_SHADERS, AS_SPAN(shader_list));
  }
  {
    const eShaderType shader_list[] = {MOTION_BLUR_GATHER,
                                       MOTION_BLUR_TILE_DILATE,
                                       MOTION_BLUR_TILE_FLATTEN_RGBA,
                                       MOTION_BLUR_TILE_FLATTEN_RG};
    request(MOTION_BLUR_SHADERS, AS_SPAN(shader_list));
  }
  {
    const eShaderType shader_list[] = {RAY_DENOISE_BILATERAL,
                                       RAY_DENOISE_SPATIAL,
                                       RAY_DENOISE_TEMPORAL,
                                       RAY_GENERATE,
                                       RAY_TILE_CLASSIFY,
                                       RAY_TILE_COMPACT,
                                       RAY_TRACE_FALLBACK,
                                       RAY_TRACE_PLANAR,
                                       RAY_TRACE_SCREEN};
    request(RAYTRACING_SHADERS, AS_SPAN(shader_list));
  }
  {
    const eShaderType shader_list[] = {SPHERE_PROBE_CONVOLVE,
                                       SPHERE_PROBE_IRRADIANCE,
                                       SPHERE_PROBE_REMAP,
                                       SPHERE_PROBE_SELECT,
                                       SPHERE_PROBE_SUNLIGHT};
    request(SPHERE_PROBE_SHADERS, AS_SPAN(shader_list));
  }
  {
    const eShaderType shader_list[] = {LIGHTPROBE_IRRADIANCE_WORLD, LIGHTPROBE_IRRADIANCE_LOAD};
    request(VOLUME_PROBE_SHADERS, AS_SPAN(shader_list));
  }
  {
    const eShaderType shader_list[] = {SHADOW_CLIPMAP_CLEAR,
                                       SHADOW_PAGE_ALLOCATE,
                                       SHADOW_PAGE_CLEAR,
                                       SHADOW_PAGE_DEFRAG,
                                       SHADOW_PAGE_FREE,
                                       SHADOW_PAGE_MASK,
                                       SHADOW_MASK_FILTER,
                                       SHADOW_MASK_FILTER_LAYERED,
                                       SHADOW_PAGE_TILE_CLEAR,
                                       SHADOW_PAGE_TILE_STORE,
                                       SHADOW_TILEMAP_AMEND,
                                       SHADOW_TILEMAP_BOUNDS,
                                       SHADOW_TILEMAP_FINALIZE,
                                       SHADOW_TILEMAP_RENDERMAP,
                                       SHADOW_TILEMAP_INIT,
                                       SHADOW_TILEMAP_TAG_UPDATE,
                                       SHADOW_TILEMAP_TAG_USAGE_BOUNDS,
                                       SHADOW_TILEMAP_TAG_USAGE_OPAQUE,
                                       SHADOW_TILEMAP_TAG_USAGE_TRANSPARENT,
                                       SHADOW_VIEW_VISIBILITY};
    request(SHADOW_SHADERS, AS_SPAN(shader_list));
  }
  {
    const eShaderType shader_list[] = {SUBSURFACE_CONVOLVE, SUBSURFACE_SETUP};
    request(SUBSURFACE_SHADERS, AS_SPAN(shader_list));
  }
  {
    const eShaderType shader_list[] = {SURFEL_CLUSTER_BUILD,
                                       SURFEL_LIGHT,
                                       SURFEL_LIST_BUILD,
                                       SURFEL_LIST_SORT,
                                       SHADOW_TILEMAP_TAG_USAGE_SURFELS,
                                       SURFEL_RAY};
    request(SURFEL_SHADERS, AS_SPAN(shader_list));
  }
  {
    const eShaderType shader_list[] = {VERTEX_COPY};
    request(VERTEX_COPY_SHADERS, AS_SPAN(shader_list));
  }
  {
    const eShaderType shader_list[] = {SHADOW_TILEMAP_TAG_USAGE_VOLUME,
                                       VOLUME_INTEGRATION,
                                       VOLUME_OCCUPANCY_CONVERT,
                                       VOLUME_RESOLVE,
                                       VOLUME_SCATTER,
                                       VOLUME_SCATTER_WITH_LIGHTS};
    request(VOLUME_EVAL_SHADERS, AS_SPAN(shader_list));
  }
#undef AS_SPAN
  return ready;
}

bool ShaderModule::request_specializations(bool block_until_ready,
                                           int render_buffers_shadow_id,
                                           int shadow_ray_count,
                                           int shadow_ray_step_count,
                                           bool use_split_indirect,
                                           bool use_lightprobe_eval)
{
  std::lock_guard lock(mutex_);

  Vector<AsyncSpecializationHandle> &handles = specialization_handles_.lookup_or_add_cb(
      {render_buffers_shadow_id,
       shadow_ray_count,
       shadow_ray_step_count,
       use_split_indirect,
       use_lightprobe_eval},
      [&]() {
        Vector<AsyncSpecializationHandle> handles;
        for (int i : IndexRange(3)) {
          gpu::Shader *shader = static_shader_get(eShaderType(DEFERRED_LIGHT_SINGLE + i));

          ShaderSpecialization specialization;
          specialization.shader = shader;
          gpu::shader::SpecializationConstants &constants = specialization.constants;
          constants = GPU_shader_get_default_constant_state(shader);

          auto set_value = [&](const char *name, auto value) {
            constants.set_value(GPU_shader_get_constant(shader, name), value);
          };

          for (bool use_transmission : {false, true}) {
            set_value("render_pass_shadow_id", render_buffers_shadow_id);
            set_value("use_split_indirect", use_split_indirect);
            set_value("use_lightprobe_eval", use_lightprobe_eval);
            set_value("use_transmission", use_transmission);
            set_value("shadow_ray_count", shadow_ray_count);
            set_value("shadow_ray_step_count", shadow_ray_step_count);
          }

          handles.append(GPU_shader_async_specialization(specialization));
        }

        return handles;
      });

  bool is_ready = true;
  for (AsyncSpecializationHandle &handle : handles) {
    while (!GPU_shader_async_specialization_is_ready(handle) && block_until_ready) {
      /* Block until ready. */
    }
    if (handle != 0) {
      is_ready = false;
      break;
    }
  }

  return is_ready;
}

const char *ShaderModule::static_shader_create_info_name_get(eShaderType shader_type)
{
  switch (shader_type) {
    case AMBIENT_OCCLUSION_PASS:
      return "eevee_ambient_occlusion_pass";
    case BAKE_LIGHT_SHADER_SURFACE:
      return "eevee_bake_light_shader_surface_mesh";
    case FILM_COPY:
      return "eevee_film_copy_frag";
    case FILM_COMP:
      return "eevee_film_comp";
    case FILM_CRYPTOMATTE_POST:
      return "eevee_film_cryptomatte_post";
    case FILM_FRAG:
      return "eevee_film_frag";
    case FILM_PASS_CONVERT_COMBINED:
      return "eevee_film_pass_convert_combined";
    case FILM_PASS_CONVERT_DEPTH:
      return "eevee_film_pass_convert_depth";
    case FILM_PASS_CONVERT_VALUE:
      return "eevee_film_pass_convert_value";
    case FILM_PASS_CONVERT_COLOR:
      return "eevee_film_pass_convert_color";
    case FILM_PASS_CONVERT_CRYPTOMATTE:
      return "eevee_film_pass_convert_cryptomatte";
    case DEFERRED_COMBINE:
      return "eevee_deferred_combine";
    case DEFERRED_LIGHT_SINGLE:
      return "eevee_deferred_light_single";
    case DEFERRED_LIGHT_DOUBLE:
      return "eevee_deferred_light_double";
    case DEFERRED_LIGHT_TRIPLE:
      return "eevee_deferred_light_triple";
    case OUTLINE_DETECT:
      return "eevee_outline_detect";
    case OUTLINE_JFA_INIT:
      return "eevee_outline_jfa_init";
    case OUTLINE_FACTOR_BLUR:
      return "eevee_outline_factor_blur";
    case OUTLINE_JFA_STEP:
      return "eevee_outline_jfa_step";
    case OUTLINE_RESOLVE:
      return "eevee_outline_resolve";
    case OUTLINE_FREESTYLE:
      return "eevee_outline_freestyle";
    case DEFERRED_AOV_CLEAR:
      return "eevee_deferred_aov_clear";
    case DEFERRED_CAPTURE_EVAL:
      return "eevee_deferred_capture_eval";
    case DEFERRED_PLANAR_EVAL:
      return "eevee_deferred_planar_eval";
    case DEFERRED_THICKNESS_AMEND:
      return "eevee_deferred_thickness_amend";
    case DEFERRED_TILE_CLASSIFY:
      return "eevee_deferred_tile_classify";
    case STENCIL_VALUE_VISUALIZE:
      return "eevee_stencil_value_visualize";
    case HIZ_DEBUG:
      return "eevee_hiz_debug";
    case HIZ_UPDATE:
      return "eevee_hiz_update";
    case HIZ_UPDATE_LAYER:
      return "eevee_hiz_update_layer";
    case HORIZON_DENOISE:
      return "eevee_horizon_denoise";
    case HORIZON_RESOLVE:
      return "eevee_horizon_resolve";
    case HORIZON_SCAN:
      return "eevee_horizon_scan";
    case HORIZON_SETUP:
      return "eevee_horizon_setup";
    case LOOKDEV_COPY_WORLD:
      return "eevee_lookdev_copy_world";
    case LOOKDEV_DISPLAY:
      return "eevee_lookdev_display";
    case MOTION_BLUR_GATHER:
      return "eevee_motion_blur_gather";
    case MOTION_BLUR_TILE_DILATE:
      return "eevee_motion_blur_tiles_dilate";
    case MOTION_BLUR_TILE_FLATTEN_RGBA:
      return "eevee_motion_blur_tiles_flatten_rgba";
    case MOTION_BLUR_TILE_FLATTEN_RG:
      return "eevee_motion_blur_tiles_flatten_rg";
    case NATIVE_POSTFX_OUTPUT_EXTRACT:
      return "eevee_native_postfx_output_extract";
    case NATIVE_POSTFX_OUTPUT_PACK_COLOR:
      return "eevee_native_postfx_output_pack_color";
    case NATIVE_POSTFX_OUTPUT_PACK_VALUE:
      return "eevee_native_postfx_output_pack_value";
    case DEBUG_SURFELS:
      return "eevee_debug_surfels";
    case DEBUG_IRRADIANCE_GRID:
      return "eevee_debug_irradiance_grid";
    case DEBUG_GBUFFER:
      return "eevee_debug_gbuffer";
    case DISPLAY_PROBE_VOLUME:
      return "eevee_display_lightprobe_volume";
    case DISPLAY_PROBE_SPHERE:
      return "eevee_display_lightprobe_sphere";
    case DISPLAY_PROBE_PLANAR:
      return "eevee_display_lightprobe_planar";
    case DOF_BOKEH_LUT:
      return "eevee_depth_of_field_bokeh_lut";
    case DOF_DOWNSAMPLE:
      return "eevee_depth_of_field_downsample";
    case DOF_FILTER:
      return "eevee_depth_of_field_filter";
    case DOF_GATHER_FOREGROUND_LUT:
      return "eevee_depth_of_field_gather_foreground_lut";
    case DOF_GATHER_FOREGROUND:
      return "eevee_depth_of_field_gather_foreground_no_lut";
    case DOF_GATHER_BACKGROUND_LUT:
      return "eevee_depth_of_field_gather_background_lut";
    case DOF_GATHER_BACKGROUND:
      return "eevee_depth_of_field_gather_background_no_lut";
    case DOF_GATHER_HOLE_FILL:
      return "eevee_depth_of_field_hole_fill";
    case DOF_REDUCE:
      return "eevee_depth_of_field_reduce";
    case DOF_RESOLVE:
      return "eevee_depth_of_field_resolve_no_lut";
    case DOF_RESOLVE_LUT:
      return "eevee_depth_of_field_resolve_lut";
    case DOF_SETUP:
      return "eevee_depth_of_field_setup";
    case DOF_SCATTER:
      return "eevee_depth_of_field_scatter";
    case DOF_STABILIZE:
      return "eevee_depth_of_field_stabilize";
    case DOF_TILES_DILATE_MINABS:
      return "eevee_depth_of_field_tiles_dilate_minabs";
    case DOF_TILES_DILATE_MINMAX:
      return "eevee_depth_of_field_tiles_dilate_minmax";
    case DOF_TILES_FLATTEN:
      return "eevee_depth_of_field_tiles_flatten";
    case LIGHT_CULLING_DEBUG:
      return "eevee_light_culling_debug";
    case LIGHT_CULLING_SELECT:
      return "eevee_light_culling_select";
    case LIGHT_CULLING_SORT:
      return "eevee_light_culling_sort";
    case LIGHT_CULLING_TILE:
      return "eevee_light_culling_tile";
    case LIGHT_CULLING_ZBIN:
      return "eevee_light_culling_zbin";
    case LIGHT_SHADOW_SETUP:
      return "eevee_light_shadow_setup";
    case RAY_DENOISE_SPATIAL:
      return "eevee_ray_denoise_spatial";
    case RAY_DENOISE_TEMPORAL:
      return "eevee_ray_denoise_temporal";
    case RAY_DENOISE_BILATERAL:
      return "eevee_ray_denoise_bilateral";
    case RAY_GENERATE:
      return "eevee_ray_generate";
    case RAY_TRACE_FALLBACK:
      return "eevee_ray_trace_fallback";
    case RAY_TRACE_PLANAR:
      return "eevee_ray_trace_planar";
    case RAY_TRACE_SCREEN:
      return "eevee_ray_trace_screen";
    case RAY_TILE_CLASSIFY:
      return "eevee_ray_tile_classify";
    case RAY_TILE_COMPACT:
      return "eevee_ray_tile_compact";
    case RENDERPASS_CLEAR:
      return "eevee_renderpass_clear";
    case RENDER_TEXTURE_EXTRACT_RGBA16F:
      return "eevee_render_texture_extract_rgba16f";
    case RENDER_TEXTURE_EXTRACT_RGBA32F:
      return "eevee_render_texture_extract_rgba32f";
    case RENDER_TEXTURE_EXTRACT_R16F:
      return "eevee_render_texture_extract_r16f";
    case RENDER_TEXTURE_EXTRACT_R32F:
      return "eevee_render_texture_extract_r32f";
    case LIGHTPROBE_IRRADIANCE_BOUNDS:
      return "eevee_lightprobe_volume_bounds";
    case LIGHTPROBE_IRRADIANCE_OFFSET:
      return "eevee_lightprobe_volume_offset";
    case LIGHTPROBE_IRRADIANCE_RAY:
      return "eevee_lightprobe_volume_ray";
    case LIGHTPROBE_IRRADIANCE_LOAD:
      return "eevee_lightprobe_volume_load";
    case LIGHTPROBE_IRRADIANCE_WORLD:
      return "eevee_lightprobe_volume_world";
    case SPHERE_PROBE_CONVOLVE:
      return "eevee_lightprobe_sphere_convolve";
    case SPHERE_PROBE_REMAP:
      return "eevee_lightprobe_sphere_remap";
    case SPHERE_PROBE_IRRADIANCE:
      return "eevee_lightprobe_sphere_irradiance";
    case SPHERE_PROBE_SELECT:
      return "eevee_lightprobe_sphere_select";
    case SPHERE_PROBE_SUNLIGHT:
      return "eevee_lightprobe_sphere_sunlight";
    case SHADOW_CLIPMAP_CLEAR:
      return "eevee_shadow_clipmap_clear";
    case SHADOW_DEBUG:
      return "eevee_shadow_debug";
    case SHADOW_MASK_FILTER:
      return "eevee_shadow_mask_filter";
    case SHADOW_MASK_FILTER_LAYERED:
      return "eevee_shadow_mask_filter_layered";
    case SHADOW_PAGE_ALLOCATE:
      return "eevee_shadow_page_allocate";
    case SHADOW_PAGE_CLEAR:
      return "eevee_shadow_page_clear";
    case SHADOW_PAGE_DEFRAG:
      return "eevee_shadow_page_defrag";
    case SHADOW_PAGE_FREE:
      return "eevee_shadow_page_free";
    case SHADOW_PAGE_MASK:
      return "eevee_shadow_page_mask";
    case SHADOW_TILEMAP_AMEND:
      return "eevee_shadow_tilemap_amend";
    case SHADOW_TILEMAP_BOUNDS:
      return "eevee_shadow_tilemap_bounds";
    case SHADOW_TILEMAP_FINALIZE:
      return "eevee_shadow_tilemap_finalize";
    case SHADOW_TILEMAP_RENDERMAP:
      return "eevee_shadow_tilemap_rendermap";
    case SHADOW_TILEMAP_INIT:
      return "eevee_shadow_tilemap_init";
    case SHADOW_TILEMAP_TAG_UPDATE:
      return "eevee_shadow_tag_update";
    case SHADOW_TILEMAP_TAG_USAGE_BOUNDS:
      return "eevee_shadow_tag_usage_bounds";
    case SHADOW_TILEMAP_TAG_USAGE_OPAQUE:
      return "eevee_shadow_tag_usage_opaque";
    case SHADOW_TILEMAP_TAG_USAGE_SURFELS:
      return "eevee_shadow_tag_usage_surfels";
    case SHADOW_TILEMAP_TAG_USAGE_TRANSPARENT:
      return "eevee_shadow_tag_usage_transparent";
    case SHADOW_PAGE_TILE_CLEAR:
      return "eevee_shadow_page_tile_clear";
    case SHADOW_PAGE_TILE_STORE:
      return "eevee_shadow_page_tile_store";
    case SHADOW_TILEMAP_TAG_USAGE_VOLUME:
      return "eevee_shadow_tag_usage_volume";
    case SHADOW_VIEW_VISIBILITY:
      return "eevee_shadow_view_visibility";
    case SUBSURFACE_CONVOLVE:
      return "eevee_subsurface_convolve";
    case SUBSURFACE_SETUP:
      return "eevee_subsurface_setup";
    case SURFEL_CLUSTER_BUILD:
      return "eevee_surfel_cluster_build";
    case SURFEL_LIGHT:
      return "eevee_surfel_light";
    case SURFEL_LIST_BUILD:
      return "eevee_surfel_list_build";
    case SURFEL_LIST_FLATTEN:
      return "eevee_surfel_list_flatten";
    case SURFEL_LIST_PREFIX:
      return "eevee_surfel_list_prefix";
    case SURFEL_LIST_PREPARE:
      return "eevee_surfel_list_prepare";
    case SURFEL_LIST_SORT:
      return "eevee_surfel_list_sort";
    case SURFEL_RAY:
      return "eevee_surfel_ray";
    case TRANSPARENCY_RESOLVE:
      return "eevee_transparency_resolve";
    case VERTEX_COPY:
      return "eevee_vertex_copy";
    case VOLUME_INTEGRATION:
      return "eevee_volume_integration";
    case VOLUME_OCCUPANCY_CONVERT:
      return "eevee_volume_occupancy_convert";
    case VOLUME_RESOLVE:
      return "eevee_volume_resolve";
    case VOLUME_SCATTER:
      return "eevee_volume_scatter";
    case VOLUME_SCATTER_WITH_LIGHTS:
      return "eevee_volume_scatter_with_lights";
    /* To avoid compiler warning about missing case. */
    case MAX_SHADER_TYPE:
      return "";
  }
  return "";
}

gpu::Shader *ShaderModule::static_shader_get(eShaderType shader_type)
{
  return shaders_[shader_type].get();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPU Materials
 *
 * \{ */

/* Helper class to get free sampler slots for materials. */
class SlotAllocator {
  uint64_t available_samplers_ = ~uint64_t(0u);
  uint32_t available_vertex_id_ = ~uint32_t(0u);
  bool sampler_overflow_ = false;
  bool vertex_id_overflow_ = false;

 public:
  void reserve_slots(const gpu::shader::ShaderCreateInfo &info)
  {
    using namespace blender::gpu::shader;
    for (const ShaderCreateInfo::VertIn &vert_in : info.vertex_inputs_) {
      available_vertex_id_ &= ~(uint32_t(1) << vert_in.index);
    }
    for (const ShaderCreateInfo::Resource &res : info.pass_resources_) {
      if (res.bind_type == ShaderCreateInfo::Resource::SAMPLER) {
        available_samplers_ &= ~(uint64_t(1) << res.slot);
      }
    }
    for (const ShaderCreateInfo::Resource &res : info.batch_resources_) {
      if (res.bind_type == ShaderCreateInfo::Resource::SAMPLER) {
        available_samplers_ &= ~(uint64_t(1) << res.slot);
      }
    }
    for (const ShaderCreateInfo::Resource &res : info.geometry_resources_) {
      if (res.bind_type == ShaderCreateInfo::Resource::SAMPLER) {
        available_samplers_ &= ~(uint64_t(1) << res.slot);
      }
    }
  }

  bool sampler_overflow() const
  {
    return sampler_overflow_;
  }

  bool vertex_id_overflow() const
  {
    return vertex_id_overflow_;
  }

  int get_next_sampler()
  {
    if (available_samplers_ == 0) {
      /* Should result in compilation failure. */
      sampler_overflow_ = true;
      return -1;
    }
    return bitscan_forward_clear_uint64(&available_samplers_);
  }

  void reserve_sampler_range(const int slot_first, const int slot_last)
  {
    if (slot_first < 0 || slot_last < slot_first) {
      return;
    }
    for (int slot = slot_first; slot <= slot_last; slot++) {
      if (slot < 64) {
        available_samplers_ &= ~(uint64_t(1) << slot);
      }
    }
  }

  void reserve_sampler(const int slot)
  {
    if (slot >= 0 && slot < 64) {
      available_samplers_ &= ~(uint64_t(1) << slot);
    }
  }

  void set_vertex_input(int index)
  {
    if ((available_vertex_id_ & 0xFFFFu) == 0) {
      /* Should result in compilation failure. */
      vertex_id_overflow_ = true;
    }
    available_vertex_id_ &= ~(uint32_t(1) << index);
  }
};

static bool create_info_contains_additional_info(
    const gpu::shader::ShaderCreateInfo &info,
    const StringRefNull create_info_name,
    Set<StringRefNull> &visited_infos)
{
  using namespace blender::gpu::shader;

  for (const ShaderCreateInfo::AdditionalInfo &additional_info : info.additional_infos_) {
    if (additional_info.name == create_info_name) {
      return true;
    }
    if (!visited_infos.add(additional_info.name)) {
      continue;
    }
    const ShaderCreateInfo *nested_info = reinterpret_cast<const ShaderCreateInfo *>(
        GPU_shader_create_info_get(additional_info.name.c_str()));
    if (nested_info != nullptr &&
        create_info_contains_additional_info(*nested_info, create_info_name, visited_infos))
    {
      return true;
    }
  }
  return false;
}

static bool create_info_contains_additional_info(const gpu::shader::ShaderCreateInfo &info,
                                                 const StringRefNull create_info_name)
{
  Set<StringRefNull> visited_infos;
  return create_info_contains_additional_info(info, create_info_name, visited_infos);
}

static void add_create_info_and_reserve(gpu::shader::ShaderCreateInfo &info,
                                        SlotAllocator &slots,
                                        StringRefNull create_info_name)
{
  using namespace blender::gpu::shader;

  if (create_info_name.is_empty()) {
    return;
  }

  const ShaderCreateInfo *create_info = reinterpret_cast<const ShaderCreateInfo *>(
      GPU_shader_create_info_get(create_info_name.c_str()));
  if (!create_info_contains_additional_info(info, create_info_name)) {
    info.additional_info(create_info_name);
  }
  slots.reserve_slots(*create_info);
}

static void reserve_deferred_npr_pass_samplers(SlotAllocator &slots)
{
  /* Keep this in sync with DeferredLayerBase::npr_pass_sync(). These pass-level bindings can
   * collide with material image textures even when the active material create-info does not
   * declare the corresponding sampler. */
  slots.reserve_sampler(RBUFS_UTILITY_TEX_SLOT);
  slots.reserve_sampler(HIZ_TEX_SLOT);
  slots.reserve_sampler(SHADOW_TILEMAPS_TEX_SLOT);
  slots.reserve_sampler(SHADOW_ATLAS_TEX_SLOT);
  slots.reserve_sampler(VOLUME_PROBE_TEX_SLOT);
  slots.reserve_sampler(SPHERE_PROBE_TEX_SLOT);
  slots.reserve_sampler(OBJECT_ID_TEX_SLOT);
  slots.reserve_sampler(PREPASS_NORMAL_TEX_SLOT);
  slots.reserve_sampler_range(GBUF_CLOSURE_TEX_SLOT, GBUF_HEADER_TEX_SLOT);
  slots.reserve_sampler(NPR_RADIANCE_TEX_SLOT);
  slots.reserve_sampler_range(DIRECT_RADIANCE_NPR_TX_SLOT_1, DIRECT_RADIANCE_NPR_TX_SLOT_1 + 2);
  slots.reserve_sampler_range(INDIRECT_RADIANCE_NPR_TX_SLOT_1,
                              INDIRECT_RADIANCE_NPR_TX_SLOT_1 + 2);
  slots.reserve_sampler(BACK_HIZ_TX_SLOT);
  slots.reserve_sampler(BACK_RADIANCE_TX_SLOT);
  slots.reserve_sampler_range(RENDER_TEXTURE_COLOR_TX_SLOT_0, RENDER_TEXTURE_HISTORY_TX_SLOT_3);
  slots.reserve_sampler(LIGHT_SHADER_NPR_TEX_SLOT);
  slots.reserve_sampler(SCENE_SHADOW_TEX_SLOT);
  slots.reserve_sampler(SHADOW_CASTER_ATLAS_TEX_SLOT);
}

static int material_texture_reserved_slot_last(const eMaterialPipeline pipeline_type,
                                               const eMaterialGeometry geometry_type)
{
  if (pipeline_type == MAT_PIPE_FILTER) {
    return MATERIAL_TEXTURE_RESERVED_SLOT_LAST_FILTER;
  }
  if (pipeline_type == MAT_PIPE_DEFERRED_NPR) {
    return MATERIAL_TEXTURE_RESERVED_SLOT_LAST_NPR;
  }
  if (geometry_type == MAT_GEOM_WORLD) {
    return MATERIAL_TEXTURE_RESERVED_SLOT_LAST_WORLD;
  }

  switch (pipeline_type) {
    case MAT_PIPE_DEFERRED:
      return MATERIAL_TEXTURE_RESERVED_SLOT_LAST_HYBRID;
    case MAT_PIPE_FORWARD:
      return MATERIAL_TEXTURE_RESERVED_SLOT_LAST_FORWARD;
    case MAT_PIPE_BAKE_COLOR:
      return MATERIAL_TEXTURE_RESERVED_SLOT_LAST_BAKE;
    case MAT_PIPE_PREPASS_FORWARD_VELOCITY:
    case MAT_PIPE_PREPASS_DEFERRED_VELOCITY:
    case MAT_PIPE_PREPASS_OVERLAP:
    case MAT_PIPE_PREPASS_FORWARD:
    case MAT_PIPE_PREPASS_DEFERRED:
    case MAT_PIPE_PREPASS_PLANAR:
    case MAT_PIPE_SHADOW:
    case MAT_PIPE_VOLUME_OCCUPANCY:
    case MAT_PIPE_VOLUME_MATERIAL:
    case MAT_PIPE_CAPTURE:
      return MATERIAL_TEXTURE_RESERVED_SLOT_LAST_NO_EVAL;
    case MAT_PIPE_FILTER:
    case MAT_PIPE_DEFERRED_NPR:
      break;
  }
  return -1;
}

static SlotAllocator add_pipeline_create_info(gpu::shader::ShaderCreateInfo &info,
                                              eMaterialPipeline pipeline_type,
                                              eMaterialGeometry geometry_type,
                                              const bool use_shader_to_rgba,
                                              const bool has_depth_offset)
{
  using namespace blender::gpu::shader;

  StringRefNull pipeline_info_name;
  StringRefNull additional_info_name;
  /* Pipeline Info. */
  switch (geometry_type) {
    case MAT_GEOM_WORLD:
      switch (pipeline_type) {
        case MAT_PIPE_VOLUME_MATERIAL:
          pipeline_info_name = "eevee_surf_volume";
          info.name_ += "_world_volume";
          break;
        case MAT_PIPE_FILTER:
          pipeline_info_name = "eevee_filter_material";
          info.name_ += "_world_filter";
          break;
        default:
          pipeline_info_name = "eevee_surf_world";
          info.name_ += "_world";
          break;
      }
      break;
    default:
      switch (pipeline_type) {
        case MAT_PIPE_PREPASS_FORWARD_VELOCITY:
        case MAT_PIPE_PREPASS_DEFERRED_VELOCITY:
          pipeline_info_name = "eevee_surf_depth";
          additional_info_name = "eevee_velocity_geom";
          info.name_ += "_depth_velocity";
          break;
        case MAT_PIPE_PREPASS_OVERLAP:
        case MAT_PIPE_PREPASS_FORWARD:
        case MAT_PIPE_PREPASS_DEFERRED:
          pipeline_info_name = "eevee_surf_depth";
          info.name_ += "_depth";
          if (pipeline_type == MAT_PIPE_PREPASS_OVERLAP) {
            info.define("MAT_OUTLINE_OCCLUSION");
          }
          break;
        case MAT_PIPE_PREPASS_PLANAR:
          pipeline_info_name = "eevee_surf_depth";
          additional_info_name = "eevee_clip_plane";
          info.name_ += "_depth_clip";
          break;
        case MAT_PIPE_SHADOW:
          /* Determine surface shadow shader depending on used update technique. */
          switch (ShadowModule::shadow_technique) {
            case ShadowTechnique::ATOMIC_RASTER: {
              pipeline_info_name = "eevee_surf_shadow_atomic";
            } break;
            case ShadowTechnique::TILE_COPY: {
              pipeline_info_name = "eevee_surf_shadow_tbdr";
            } break;
            default: {
              BLI_assert_unreachable();
            } break;
          }
          break;
        case MAT_PIPE_VOLUME_OCCUPANCY:
          pipeline_info_name = "eevee_surf_occupancy";
          info.name_ += "_occupancy";
          break;
        case MAT_PIPE_VOLUME_MATERIAL:
          pipeline_info_name = "eevee_surf_volume";
          info.name_ += "_volume";
          break;
        case MAT_PIPE_CAPTURE:
          pipeline_info_name = "eevee_surf_capture";
          info.name_ += "_capture";
          break;
        case MAT_PIPE_BAKE_COLOR:
          pipeline_info_name = "eevee_surf_bake_color";
          info.name_ += "_bake_color";
          break;
        case MAT_PIPE_DEFERRED:
          if (use_shader_to_rgba) {
            pipeline_info_name = has_depth_offset ? "eevee_surf_deferred_hybrid_depth_offset" :
                                                    "eevee_surf_deferred_hybrid";
            info.name_ += "_deferred_hybrid";
          }
          else {
            pipeline_info_name = has_depth_offset ? "eevee_surf_deferred_depth_offset" :
                                                    "eevee_surf_deferred";
            info.name_ += "_deferred";
          }
          break;
        case MAT_PIPE_DEFERRED_NPR:
          pipeline_info_name = has_depth_offset ? "eevee_surf_npr_depth_offset" :
                                                  "eevee_surf_npr";
          info.name_ += "_deferred_npr";
          break;
        case MAT_PIPE_FORWARD:
          pipeline_info_name = has_depth_offset ? "eevee_surf_forward_depth_offset" :
                                                  "eevee_surf_forward";
          info.name_ += "_forward";
          break;
        default:
          BLI_assert_unreachable();
          break;
      }
      break;
  }

  /* Geometry Info. */
  StringRefNull geometry_info_name;
  switch (geometry_type) {
    case MAT_GEOM_WORLD:
      geometry_info_name = "eevee_geom_world";
      info.name_ += "_world";
      break;
    case MAT_GEOM_CURVES:
      geometry_info_name = "eevee_geom_curves";
      info.name_ += "_curves";
      break;
    case MAT_GEOM_MESH:
      geometry_info_name = (pipeline_type == MAT_PIPE_BAKE_COLOR) ? "eevee_geom_bake_mesh" :
                                                                    "eevee_geom_mesh";
      info.name_ += (pipeline_type == MAT_PIPE_BAKE_COLOR) ? "_bake_mesh" : "_mesh";
      break;
    case MAT_GEOM_POINTCLOUD:
      geometry_info_name = "eevee_geom_pointcloud";
      info.name_ += "_pointcloud";
      break;
    case MAT_GEOM_VOLUME:
      geometry_info_name = "eevee_geom_volume";
      info.name_ += "_volume";
      break;
  }

  SlotAllocator available_slots;

  if (!pipeline_info_name.is_empty()) {
    add_create_info_and_reserve(info, available_slots, pipeline_info_name);
  }
  if (!additional_info_name.is_empty()) {
    add_create_info_and_reserve(info, available_slots, additional_info_name);
  }
  if (!geometry_info_name.is_empty()) {
    add_create_info_and_reserve(info, available_slots, geometry_info_name);
  }
  return available_slots;
}

void ShaderModule::material_create_info_amend(GPUMaterial *gpumat, GPUCodegenOutput *codegen_)
{
  using namespace blender::gpu::shader;

  uint64_t shader_uuid = GPU_material_uuid_get(gpumat);
  eMaterialPipeline pipeline_type;
  eMaterialGeometry geometry_type;
  eMaterialDisplacement displacement_type;
  eMaterialThickness thickness_type;
  eMaterialProbe probe_capture;
  bool transparent_shadows;
  bool use_outline;
  bool uuid_depth_offset_affect_lighting;
  material_type_from_shader_uuid(shader_uuid,
                                 pipeline_type,
                                 geometry_type,
                                 displacement_type,
                                 thickness_type,
                                 probe_capture,
                                 transparent_shadows,
                                 use_outline,
                                 uuid_depth_offset_affect_lighting);
  UNUSED_VARS(uuid_depth_offset_affect_lighting);

  GPUCodegenOutput &codegen = *codegen_;
  ShaderCreateInfo &info = *reinterpret_cast<ShaderCreateInfo *>(codegen.create_info);
  const bool use_shader_to_rgba = material_graph_serialized_contains(codegen.surface,
                                                                     "node_shader_to_rgba(");

  /* Material generated sources can use arbitrary per-material names, while the GPU dependency
   * resolver only knows startup-registered files. Inline the referenced generated blocks here and
   * keep only real library files in the dependency list. */
  for (int i = 0; i < GPU_material_generated_source_count(gpumat); i++) {
    const GPUMaterialGeneratedSource *generated_source = GPU_material_generated_source_get(gpumat,
                                                                                           i);
    if (generated_source == nullptr) {
      continue;
    }

    Vector<StringRefNull> dependencies;
    dependencies.reserve(generated_source->dependencies.size());
    for (const std::string &dependency : generated_source->dependencies) {
      dependencies.append(dependency.c_str());
    }

    info.generated_sources.append(
        {generated_source->filename.c_str(), dependencies, generated_source->content});
  }

  /* WORKAROUND: Add new ob attr buffer. */
  if (GPU_material_uniform_attributes(gpumat) != nullptr) {
    info.additional_info("draw_object_attributes");

    /* Search and remove the old object attribute UBO which would creating bind point collision. */
    for (auto &resource_info : info.batch_resources_) {
      if (resource_info.bind_type == ShaderCreateInfo::Resource::BindType::UNIFORM_BUFFER &&
          resource_info.uniformbuf.name == GPU_ATTRIBUTE_UBO_BLOCK_NAME "[512]")
      {
        info.batch_resources_.remove_first_occurrence_and_reorder(resource_info);
        break;
      }
    }
    /* Remove references to the UBO. */
    info.define("UNI_ATTR(a)", "float4(0.0)");
  }

  bool use_ao_node = false;

  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_AO) &&
      ELEM(pipeline_type,
           MAT_PIPE_FORWARD,
           MAT_PIPE_DEFERRED,
           MAT_PIPE_DEFERRED_NPR,
           MAT_PIPE_BAKE_COLOR) &&
      geometry_type_has_surface(geometry_type))
  {
    info.define("MAT_AMBIENT_OCCLUSION");
    use_ao_node = true;
  }

  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_TRANSPARENT)) {
    if (pipeline_type != MAT_PIPE_SHADOW || transparent_shadows) {
      info.define("MAT_TRANSPARENT");
    }
    /* Transparent material do not have any velocity specific pipeline. */
    if (pipeline_type == MAT_PIPE_PREPASS_FORWARD_VELOCITY) {
      pipeline_type = MAT_PIPE_PREPASS_FORWARD;
    }
  }

  const bool use_raycast = GPU_material_flag_get(gpumat, GPU_MATFLAG_RAYCAST) &&
                           ELEM(pipeline_type,
                                MAT_PIPE_DEFERRED,
                                MAT_PIPE_DEFERRED_NPR,
                                MAT_PIPE_FORWARD,
                                MAT_PIPE_BAKE_COLOR);

  if (probe_capture != MAT_PROBE_NONE) {
    info.define("MAT_PROBE_CAPTURE");
    switch (probe_capture) {
      case MAT_PROBE_REFLECTION:
        info.define("MAT_SPHERE_PROBE_CAPTURE");
        break;
      case MAT_PROBE_PLANAR:
        info.define("MAT_PLANAR_PROBE_CAPTURE");
        break;
      case MAT_PROBE_NONE:
        break;
    }
  }

  const blender::Material *blender_mat = GPU_material_get_material(gpumat);
  const bool depth_offset_affect_lighting = material_depth_offset_affects_lighting(blender_mat);
  const bool has_depth_offset = GPU_material_has_depth_offset_output(gpumat) &&
                                material_pipeline_supports_depth_offset(pipeline_type,
                                                                        geometry_type);
  const bool depth_offset_uses_light_access =
      has_depth_offset &&
      material_depth_offset_graph_uses_supported_light_access(gpumat, codegen_->depth_offset);
  const bool surface_graph_uses_glsl_light_access =
      ELEM(pipeline_type, MAT_PIPE_DEFERRED, MAT_PIPE_FORWARD, MAT_PIPE_BAKE_COLOR) &&
      material_graph_uses_glsl_light_access(gpumat, codegen.surface);
  const bool npr_graph_uses_glsl_light_access =
      ELEM(pipeline_type, MAT_PIPE_DEFERRED_NPR, MAT_PIPE_BAKE_COLOR) &&
      material_graph_uses_glsl_light_access(gpumat, codegen.npr);
  const bool surface_pass_uses_glsl_light_access = surface_graph_uses_glsl_light_access ||
                                                   npr_graph_uses_glsl_light_access;
  bool material_pass_uses_glsl_light_access =
      surface_pass_uses_glsl_light_access ||
      (ELEM(pipeline_type,
            MAT_PIPE_DEFERRED,
            MAT_PIPE_DEFERRED_NPR,
            MAT_PIPE_FORWARD,
            MAT_PIPE_BAKE_COLOR) &&
       (material_graph_uses_glsl_light_access(gpumat, codegen.filter) ||
        material_graph_uses_glsl_light_access(gpumat, codegen.thickness) ||
        material_graph_uses_glsl_light_access(gpumat, codegen.volume)));
  for (const GPUGraphOutput &graph : codegen.material_functions) {
    material_pass_uses_glsl_light_access |=
        material_graph_uses_glsl_light_access(gpumat, graph);
  }
  const bool uses_glsl_light_access = material_pass_uses_glsl_light_access ||
                                      depth_offset_uses_light_access;
  const bool surface_pass_uses_shader_info =
      GPU_material_flag_get(gpumat, GPU_MATFLAG_SHADER_INFO) &&
      ELEM(pipeline_type, MAT_PIPE_DEFERRED, MAT_PIPE_DEFERRED_NPR, MAT_PIPE_FORWARD);
  const bool use_shader_info_shadow_classification =
      GPU_material_has_shader_info_shadow_classification(gpumat) &&
      ELEM(pipeline_type, MAT_PIPE_DEFERRED, MAT_PIPE_DEFERRED_NPR, MAT_PIPE_FORWARD);
  const bool separate_depth_offset_lighting =
      has_depth_offset && !depth_offset_affect_lighting &&
      ELEM(pipeline_type, MAT_PIPE_DEFERRED, MAT_PIPE_DEFERRED_NPR);

  const bool use_front_light_shader_in_surface_pass =
      use_shader_to_rgba || surface_graph_uses_glsl_light_access || surface_pass_uses_shader_info;

  SlotAllocator slots = add_pipeline_create_info(
      info, pipeline_type, geometry_type, use_front_light_shader_in_surface_pass, has_depth_offset);
  if (pipeline_type == MAT_PIPE_DEFERRED_NPR) {
    reserve_deferred_npr_pass_samplers(slots);
  }
  else {
    slots.reserve_sampler_range(MATERIAL_TEXTURE_RESERVED_SLOT_FIRST,
                                material_texture_reserved_slot_last(pipeline_type, geometry_type));
  }
  if (ELEM(pipeline_type, MAT_PIPE_DEFERRED, MAT_PIPE_FORWARD)) {
    slots.reserve_sampler(LIGHT_SHADER_TEX_SLOT);
  }
  else if (pipeline_type == MAT_PIPE_BAKE_COLOR) {
    slots.reserve_sampler(LIGHT_SHADER_TEX_SLOT);
  }
  if (use_shader_info_shadow_classification) {
    slots.reserve_sampler(SHADOW_CASTER_ATLAS_TEX_SLOT);
  }
  if (has_depth_offset) {
    info.define("MAT_DEPTH_OFFSET");
    if (separate_depth_offset_lighting) {
      info.define("MAT_DEPTH_OFFSET_NO_LIGHTING");
    }
    if (pipeline_type != MAT_PIPE_SHADOW ||
        ShadowModule::shadow_technique == ShadowTechnique::TILE_COPY)
    {
      info.early_fragment_test(false);
      info.depth_write(DepthWrite::ANY);
    }
  }

  if (use_raycast) {
    add_create_info_and_reserve(info, slots, "eevee_raycast");
  }

  /* Deferred and forward materials write render passes here. NPR binds the in/out variant in its
   * base info. */
  if (ELEM(pipeline_type, MAT_PIPE_DEFERRED, MAT_PIPE_FORWARD)) {
    info.additional_info("eevee_render_pass_out");
    if (pipeline_type == MAT_PIPE_DEFERRED) {
      info.additional_info("eevee_cryptomatte_out");
    }
  }
  const bool has_outline_output = GPU_material_has_outline_output(gpumat);
  /* Outline is a screen-space main-view effect. Do not emit it into probe captures, or planar
   * and reflection probes will bake the outline overlay into their radiance textures. */
  const bool use_outline_support = use_outline && (probe_capture == MAT_PROBE_NONE);
  const bool clears_outline_output =
      pipeline_type == MAT_PIPE_FORWARD ||
      (pipeline_type == MAT_PIPE_DEFERRED && blender_mat != nullptr &&
       (blender_mat->blend_flag & MA_BL_SS_REFRACTION) != 0);
  if (use_outline_support &&
      (has_outline_output || clears_outline_output) &&
      ELEM(pipeline_type, MAT_PIPE_DEFERRED, MAT_PIPE_DEFERRED_NPR, MAT_PIPE_FORWARD))
  {
    info.additional_info((pipeline_type == MAT_PIPE_FORWARD) ? "eevee_surf_forward_outline_out" :
                                                              "eevee_outline_out");
    if (clears_outline_output) {
      info.define("MAT_OUTLINE_CLEAR");
    }
    if (has_outline_output) {
      info.define("MAT_OUTLINE_SUPPORT");
    }
  }

  if (use_shader_to_rgba) {
    info.define("MAT_SHADER_TO_RGBA");
  }
  if (geometry_type == MAT_GEOM_WORLD && GPU_material_flag_get(gpumat, GPU_MATFLAG_NPR)) {
    info.define("NPR_SHADER");
  }
  if (pipeline_type == MAT_PIPE_BAKE_COLOR && GPU_material_flag_get(gpumat, GPU_MATFLAG_NPR)) {
    info.define("NPR_SHADER");
  }

  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_RENDER_TEXTURE) &&
      pipeline_type == MAT_PIPE_DEFERRED_NPR)
  {
    add_create_info_and_reserve(info, slots, "eevee_render_texture_data");
  }

  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_NPR_REFRACTION) &&
      pipeline_type == MAT_PIPE_DEFERRED_NPR)
  {
    info.define("MAT_NPR_REFRACTION");
    add_create_info_and_reserve(info, slots, "eevee_surf_npr_refraction_data");
  }

  if (ELEM(pipeline_type, MAT_PIPE_DEFERRED, MAT_PIPE_FORWARD) &&
      (use_shader_to_rgba || GPU_material_flag_get(gpumat, GPU_MATFLAG_SCREENSPACE_INFO)))
  {
    add_create_info_and_reserve(info, slots, "eevee_hiz_prev_data");
    add_create_info_and_reserve(info, slots, "eevee_previous_layer_radiance");
  }

  if ((GPU_material_flag_get(gpumat, GPU_MATFLAG_SHADER_INFO) ||
       GPU_material_flag_get(gpumat, GPU_MATFLAG_NPR_FOREACH_LIGHT)) &&
      ELEM(pipeline_type,
           MAT_PIPE_DEFERRED,
           MAT_PIPE_DEFERRED_NPR,
           MAT_PIPE_FORWARD,
           MAT_PIPE_BAKE_COLOR))
  {
    add_create_info_and_reserve(info, slots, "eevee_light_data");
    add_create_info_and_reserve(info, slots, "eevee_shadow_data");
    if (use_shader_info_shadow_classification) {
      info.define("SHADOW_CASTER_CLASSIFY");
      add_create_info_and_reserve(info, slots, "eevee_shadow_caster_data");
    }
  }
  if (pipeline_type == MAT_PIPE_BAKE_COLOR) {
    info.define("LIGHT_ITER_FORCE_NO_CULLING");
  }
  if (uses_glsl_light_access) {
    info.define("MAT_GLSL_LIGHT_ACCESS");
    if (depth_offset_uses_light_access && pipeline_type != MAT_PIPE_BAKE_COLOR) {
      info.define("LIGHT_ITER_FORCE_NO_CULLING");
    }
    add_create_info_and_reserve(info, slots, "eevee_light_data");
    if (material_pass_uses_glsl_light_access) {
      info.define("MAT_GLSL_LIGHT_SHADOW_ACCESS");
      add_create_info_and_reserve(info, slots, "eevee_shadow_data");
    }
    if (surface_pass_uses_glsl_light_access) {
      info.define("MAT_GLSL_LIGHT_SHADER_EVAL");
      info.define("LIGHT_SHADER_TEXTURE_EVAL");
      GPU_material_glsl_light_shader_eval_set(gpumat);
    }
  }
  if ((GPU_material_flag_get(gpumat, GPU_MATFLAG_SHADER_INFO) ||
       GPU_material_flag_get(gpumat, GPU_MATFLAG_NPR_FOREACH_LIGHT) ||
       GPU_material_flag_get(gpumat, GPU_MATFLAG_LIGHTPROBE_ACCESS)) &&
      ELEM(pipeline_type,
           MAT_PIPE_DEFERRED,
           MAT_PIPE_DEFERRED_NPR,
           MAT_PIPE_FORWARD,
           MAT_PIPE_BAKE_COLOR))
  {
    add_create_info_and_reserve(info, slots, "eevee_lightprobe_data");
  }

  for (auto &resource : info.batch_resources_) {
    if (resource.bind_type == ShaderCreateInfo::Resource::BindType::SAMPLER) {
      resource.slot = slots.get_next_sampler();
    }
  }

  if ((GPU_material_flag_get(gpumat, GPU_MATFLAG_SHADER_INFO) ||
       GPU_material_flag_get(gpumat, GPU_MATFLAG_NPR_FOREACH_LIGHT)) &&
      ELEM(pipeline_type, MAT_PIPE_DEFERRED_NPR, MAT_PIPE_BAKE_COLOR))
  {
    info.define("MAT_NPR_LIGHTING");
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_SHADER_INFO) &&
      ELEM(pipeline_type, MAT_PIPE_DEFERRED_NPR, MAT_PIPE_BAKE_COLOR))
  {
    info.define("MAT_NPR_SHADER_INFO");
    info.define("LIGHT_SHADER_TEXTURE_EVAL");
  }

  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_DIFFUSE)) {
    info.define("MAT_DIFFUSE");
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_SUBSURFACE)) {
    info.define("MAT_SUBSURFACE");
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_REFRACT)) {
    info.define("MAT_REFRACTION");
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_TRANSLUCENT)) {
    info.define("MAT_TRANSLUCENT");
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_GLOSSY)) {
    info.define("MAT_REFLECTION");
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_COAT)) {
    info.define("MAT_CLEARCOAT");
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_REFLECTION_MAYBE_COLORED) == false) {
    info.define("MAT_REFLECTION_COLORLESS");
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_REFRACTION_MAYBE_COLORED) == false) {
    info.define("MAT_REFRACTION_COLORLESS");
  }

  const eClosureBits closure_bits = shader_closure_bits_from_flag(gpumat);

  int32_t closure_bin_count = to_gbuffer_bin_count(closure_bits);
  switch (closure_bin_count) {
    /* These need to be separated since the strings need to be static. */
    case 0:
    case 1:
      info.define("CLOSURE_BIN_COUNT", "1");
      break;
    case 2:
      info.define("CLOSURE_BIN_COUNT", "2");
      break;
    case 3:
      info.define("CLOSURE_BIN_COUNT", "3");
      break;
    default:
      BLI_assert_unreachable();
      break;
  }

  if (ELEM(pipeline_type, MAT_PIPE_DEFERRED, MAT_PIPE_DEFERRED_NPR)) {
    if (pipeline_type == MAT_PIPE_DEFERRED_NPR) {
      /* NPR reads the already-written material GBuffer, so it cannot be specialized to the
       * NPR tree's own closures. */
      info.define("GBUFFER_LAYER_MAX", "3");
    }
    else {
      switch (closure_bin_count) {
        /* These need to be separated since the strings need to be static. */
        case 0:
        case 1:
          info.define("GBUFFER_LAYER_MAX", "1");
          break;
        case 2:
          info.define("GBUFFER_LAYER_MAX", "2");
          break;
        case 3:
          info.define("GBUFFER_LAYER_MAX", "3");
          break;
        default:
          BLI_assert_unreachable();
          break;
      }
    }

    if (closure_bin_count == 2 && pipeline_type != MAT_PIPE_DEFERRED_NPR) {
      /* In a lot of cases, we can predict that we do not need the extra GBuffer layers. This
       * simplifies the shader code and improves compilation time (see #145347). */
      const bool colorless_reflection = !GPU_material_flag_get(
          gpumat, GPU_MATFLAG_REFLECTION_MAYBE_COLORED);
      const bool colorless_refraction = !GPU_material_flag_get(
          gpumat, GPU_MATFLAG_REFRACTION_MAYBE_COLORED);
      int closure_layer_count = 0;
      if (closure_bits & CLOSURE_DIFFUSE) {
        closure_layer_count += 1;
      }
      if (closure_bits & CLOSURE_SSS) {
        closure_layer_count += 2;
      }
      if (closure_bits & CLOSURE_REFLECTION) {
        closure_layer_count += colorless_reflection ? 1 : 2;
      }
      if (closure_bits & CLOSURE_REFRACTION) {
        closure_layer_count += colorless_refraction ? 1 : 2;
      }
      if (closure_bits & CLOSURE_TRANSLUCENT) {
        closure_layer_count += 1;
      }
      if (closure_bits & CLOSURE_CLEARCOAT) {
        closure_layer_count += colorless_reflection ? 1 : 2;
      }

      if (closure_layer_count <= 2) {
        info.define("GBUFFER_SIMPLE_CLOSURE_LAYOUT");
      }
    }
  }

  if (ELEM(pipeline_type, MAT_PIPE_FORWARD, MAT_PIPE_BAKE_COLOR) ||
      use_front_light_shader_in_surface_pass)
  {
    switch (closure_bin_count) {
      case 0:
        /* Define nothing. This will in turn define SKIP_LIGHT_EVAL. */
        break;
      /* These need to be separated since the strings need to be static. */
      case 1:
        info.define("LIGHT_CLOSURE_EVAL_COUNT", "1");
        break;
      case 2:
        info.define("LIGHT_CLOSURE_EVAL_COUNT", "2");
        break;
      case 3:
        info.define("LIGHT_CLOSURE_EVAL_COUNT", "3");
        break;
      default:
        BLI_assert_unreachable();
        break;
    }
  }

  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_BARYCENTRIC)) {
    switch (geometry_type) {
      case MAT_GEOM_MESH:
        /* Support using gpu builtin barycentrics. */
        info.define("USE_BARYCENTRICS");
        info.builtins(BuiltinBits::BARYCENTRIC_COORD);
        break;
      case MAT_GEOM_CURVES:
        /* Support using one float2 attribute. See #hair_get_barycentric(). */
        info.define("USE_BARYCENTRICS");
        break;
      default:
        /* No support */
        break;
    }
  }

  /* Allow to use Reverse-Z on OpenGL. Does nothing in other backend. */
  info.builtins(BuiltinBits::CLIP_CONTROL);

  std::stringstream global_vars;
  switch (geometry_type) {
    case MAT_GEOM_MESH:
      if (pipeline_type == MAT_PIPE_VOLUME_MATERIAL) {
        /* If mesh has a volume output, it can receive volume grid attributes from smoke
         * simulation modifier. But the vertex shader might still need access to the vertex
         * attribute for displacement. */
        /* TODO(fclem): Eventually, we could add support for loading both. For now, remove the
         * vertex inputs after conversion (avoid name collision). */
        for (auto &input : info.vertex_inputs_) {
          info.sampler(slots.get_next_sampler(), ImageType::Float3D, input.name, Frequency::BATCH);
        }
        info.vertex_inputs_.clear();
        /* Volume materials require these for loading the grid attributes from smoke sims. */
        info.additional_info("draw_volume_infos");
      }
      break;
    case MAT_GEOM_POINTCLOUD:
    case MAT_GEOM_CURVES:
      /** Hair attributes come from sampler buffer. Transfer attributes to sampler. */
      for (auto &input : info.vertex_inputs_) {
        if (input.name == "orco") {
          /** NOTE: Orco is generated from strand position for now. */
          global_vars << input.type << " " << input.name << ";\n";
        }
        else {
          info.sampler(
              slots.get_next_sampler(), ImageType::FloatBuffer, input.name, Frequency::BATCH);
        }
      }
      info.vertex_inputs_.clear();
      break;
    case MAT_GEOM_WORLD:
      if (pipeline_type == MAT_PIPE_VOLUME_MATERIAL) {
        /* Even if world do not have grid attributes, we use dummy texture binds to pass correct
         * defaults. So we have to replace all attributes as samplers. */
        for (auto &input : info.vertex_inputs_) {
          info.sampler(slots.get_next_sampler(), ImageType::Float3D, input.name, Frequency::BATCH);
        }
        info.vertex_inputs_.clear();
      }
      /**
       * Only orco layer is supported by world and it is procedurally generated. These are here to
       * make the attribs_load function calls valid.
       */
      for (auto &input : info.vertex_inputs_) {
        global_vars << input.type << " " << input.name << ";\n";
      }
      info.vertex_inputs_.clear();
      break;
    case MAT_GEOM_VOLUME:
      /** Volume grid attributes come from 3D textures. Transfer attributes to samplers. */
      for (auto &input : info.vertex_inputs_) {
        info.sampler(slots.get_next_sampler(), ImageType::Float3D, input.name, Frequency::BATCH);
      }
      info.vertex_inputs_.clear();
      break;
  }

  for (auto &vert_in : info.vertex_inputs_) {
    slots.set_vertex_input(vert_in.index);
  }

  const bool support_volume_attributes = ELEM(geometry_type, MAT_GEOM_MESH, MAT_GEOM_VOLUME);
  const bool do_vertex_attrib_load = !ELEM(geometry_type, MAT_GEOM_WORLD, MAT_GEOM_VOLUME) &&
                                     (pipeline_type != MAT_PIPE_VOLUME_MATERIAL ||
                                      !support_volume_attributes);

  if (!do_vertex_attrib_load && !info.vertex_out_interfaces_.is_empty()) {
    /* Codegen outputs only one interface. */
    const StageInterfaceInfo &iface = *info.vertex_out_interfaces_.first();
    /* Globals the attrib_load() can write to when it is in the fragment shader. */
    global_vars << "struct " << iface.name << " {\n";
    for (const auto &inout : iface.inouts) {
      global_vars << "  " << inout.type << " " << inout.name << ";\n";
    }
    global_vars << "};\n";
    global_vars << iface.name << " " << iface.instance_name << ";\n";

    info.vertex_out_interfaces_.clear();
  }

  const char *domain_type_frag = "";
  const char *domain_type_vert = "";
  switch (geometry_type) {
    case MAT_GEOM_MESH:
      domain_type_frag = (pipeline_type == MAT_PIPE_VOLUME_MATERIAL) ? "VolumePoint" :
                                                                       "MeshVertex";
      domain_type_vert = "MeshVertex";
      break;
    case MAT_GEOM_POINTCLOUD:
      domain_type_frag = (pipeline_type == MAT_PIPE_VOLUME_MATERIAL) ? "VolumePoint" :
                                                                       "PointCloudPoint";
      domain_type_vert = "PointCloudPoint";
      break;
    case MAT_GEOM_CURVES:
      domain_type_frag = (pipeline_type == MAT_PIPE_VOLUME_MATERIAL) ? "VolumePoint" :
                                                                       "CurvesPoint";
      domain_type_vert = "CurvesPoint";
      break;
    case MAT_GEOM_WORLD:
      domain_type_frag = (pipeline_type == MAT_PIPE_VOLUME_MATERIAL) ? "VolumePoint" :
                                                                       "WorldPoint";
      domain_type_vert = "WorldPoint";
      break;
    case MAT_GEOM_VOLUME:
      domain_type_frag = domain_type_vert = "VolumePoint";
      break;
  }

  std::stringstream attr_load;
  attr_load << "{\n";
  attr_load << (!codegen.attr_load.empty() ? codegen.attr_load : "");
  attr_load << "}\n\n";

  std::stringstream vert_gen, frag_gen;

  if (do_vertex_attrib_load) {
    vert_gen << global_vars.str() << "void attrib_load(" << domain_type_vert << " domain)"
             << attr_load.str();
    frag_gen << "void attrib_load(" << domain_type_frag << " domain) {}\n"; /* Placeholder. */
  }
  else {
    vert_gen << "void attrib_load(" << domain_type_vert << " domain) {}\n"; /* Placeholder. */
    frag_gen << global_vars.str() << "void attrib_load(" << domain_type_frag << " domain)"
             << attr_load.str();
  }

  /* Bit of a workaround. Make sure that the nodetree UBO is part of the eevee_node_tree
   * interface and not the interface with the shader name. */
  for (auto &res : info.batch_resources_) {
    res.info_name = "eevee_node_tree";
  }
  for (auto &res : info.pass_resources_) {
    res.info_name = "eevee_node_tree";
  }
  for (auto &res : info.geometry_resources_) {
    res.info_name = "eevee_node_tree";
  }

  std::string generated_resource_header = info.typedef_source_generated;
  /* Insert resource declaration after types declaration. */
  generated_resource_header += "#ifdef CREATE_INFO_RES_PASS_eevee_node_tree\n";
  generated_resource_header += "CREATE_INFO_RES_PASS_eevee_node_tree\n";
  generated_resource_header += "#endif\n";
  generated_resource_header += "#ifdef CREATE_INFO_RES_BATCH_eevee_node_tree\n";
  generated_resource_header += "CREATE_INFO_RES_BATCH_eevee_node_tree\n";
  generated_resource_header += "#endif\n";
  generated_resource_header += "#ifdef CREATE_INFO_RES_GEOMETRY_eevee_node_tree\n";
  generated_resource_header += "CREATE_INFO_RES_GEOMETRY_eevee_node_tree\n";
  generated_resource_header += "#endif\n";
  generated_resource_header += "\n";

  info.generated_sources.append({"eevee_nodetree_type_lib.glsl", {}, generated_resource_header});

  {
    const bool use_vertex_displacement = !codegen.displacement.empty() &&
                                         (displacement_type != MAT_DISPLACEMENT_BUMP) &&
                                         !ELEM(geometry_type, MAT_GEOM_WORLD, MAT_GEOM_VOLUME);

    Set<StringRefNull> generated_dependencies;
    Set<StringRefNull> emitted_generated_sources;
    std::stringstream generated_source_block;

    if (use_vertex_displacement) {
      generated_dependencies.add("eevee_geom_types_lib.glsl");
      generated_dependencies.add("eevee_nodetree_lib.glsl");
      material_graph_dependencies_append(gpumat,
                                         codegen.displacement.dependencies,
                                         generated_dependencies,
                                         emitted_generated_sources,
                                         generated_source_block);
    }

    vert_gen << generated_source_block.str();
    vert_gen << "float3 nodetree_displacement()\n";
    vert_gen << "{\n";
    vert_gen << ((use_vertex_displacement) ? codegen.displacement.serialized :
                                             "return float3(0);\n");
    vert_gen << "}\n\n";

    Vector<StringRefNull> dependencies = use_vertex_displacement ?
                                             material_dependencies_finalize(generated_dependencies) :
                                             Vector<StringRefNull>{};

    info.generated_sources.append({"eevee_nodetree_vert_lib.glsl", dependencies, vert_gen.str()});
  }

  if (pipeline_type != MAT_PIPE_VOLUME_OCCUPANCY) {
    Set<StringRefNull> dependencies_set;
    Set<StringRefNull> emitted_generated_sources;
    std::stringstream generated_source_block;
    if (use_ao_node) {
      dependencies_set.add("eevee_ambient_occlusion_lib.glsl");
    }
    const bool pipeline_uses_light_eval =
        (GPU_material_flag_get(gpumat, GPU_MATFLAG_SHADER_INFO) ||
         GPU_material_flag_get(gpumat, GPU_MATFLAG_NPR_FOREACH_LIGHT)) &&
        ELEM(pipeline_type,
             MAT_PIPE_DEFERRED,
             MAT_PIPE_DEFERRED_NPR,
             MAT_PIPE_FORWARD,
             MAT_PIPE_BAKE_COLOR);
    if (pipeline_uses_light_eval || material_pass_uses_glsl_light_access) {
      dependencies_set.add("eevee_light_eval_lib.glsl");
      dependencies_set.add("eevee_light_iter_lib.glsl");
      dependencies_set.add("eevee_light_lib.glsl");
      dependencies_set.add("eevee_shadow_tracing_lib.glsl");
    }
    if (uses_glsl_light_access) {
      dependencies_set.add("eevee_light_iter_lib.glsl");
      dependencies_set.add("eevee_light_lib.glsl");
    }
    if (material_pass_uses_glsl_light_access) {
      dependencies_set.add("eevee_shadow_tracing_lib.glsl");
    }
    if ((GPU_material_flag_get(gpumat, GPU_MATFLAG_SHADER_INFO) ||
         GPU_material_flag_get(gpumat, GPU_MATFLAG_NPR_FOREACH_LIGHT) ||
         GPU_material_flag_get(gpumat, GPU_MATFLAG_LIGHTPROBE_ACCESS)) &&
        ELEM(pipeline_type,
             MAT_PIPE_DEFERRED,
             MAT_PIPE_DEFERRED_NPR,
             MAT_PIPE_FORWARD,
             MAT_PIPE_BAKE_COLOR))
    {
      dependencies_set.add("eevee_lightprobe_eval_lib.glsl");
    }
    dependencies_set.add("eevee_geom_types_lib.glsl");
    dependencies_set.add("eevee_nodetree_lib.glsl");

    for (const auto &graph : codegen.material_functions) {
      material_graph_dependencies_append(
          gpumat, graph.dependencies, dependencies_set, emitted_generated_sources, generated_source_block);
    }
    if (!codegen.displacement.empty()) {
      material_graph_dependencies_append(gpumat,
                                         codegen.displacement.dependencies,
                                         dependencies_set,
                                         emitted_generated_sources,
                                         generated_source_block);
    }
    material_graph_dependencies_append(
        gpumat, codegen.surface.dependencies, dependencies_set, emitted_generated_sources, generated_source_block);
    material_graph_dependencies_append(
        gpumat, codegen.npr.dependencies, dependencies_set, emitted_generated_sources, generated_source_block);
    material_graph_dependencies_append(
        gpumat, codegen.filter.dependencies, dependencies_set, emitted_generated_sources, generated_source_block);
    material_graph_dependencies_append(
        gpumat, codegen.thickness.dependencies, dependencies_set, emitted_generated_sources, generated_source_block);
    if (has_depth_offset && codegen.depth_offset.has_value() &&
        !material_depth_offset_graph_has_unsupported_dependencies(gpumat, *codegen.depth_offset))
    {
      material_graph_dependencies_append(gpumat,
                                         codegen.depth_offset->dependencies,
                                         dependencies_set,
                                         emitted_generated_sources,
                                         generated_source_block);
    }
    material_graph_dependencies_append(
        gpumat, codegen.volume.dependencies, dependencies_set, emitted_generated_sources, generated_source_block);

    frag_gen << generated_source_block.str();
    for (const auto &graph : codegen.material_functions) {
      frag_gen << graph.serialized;
    }

    if (!codegen.displacement.empty()) {
      /* Bump displacement. Needed to recompute normals after displacement. */
      info.define("MAT_DISPLACEMENT_BUMP");

      frag_gen << "float3 nodetree_displacement()\n";
      frag_gen << "{\n";
      frag_gen << codegen.displacement.serialized;
      frag_gen << "}\n\n";
    }

    frag_gen << "Closure nodetree_surface(float closure_rand)\n";
    frag_gen << "{\n";
    frag_gen << "  closure_weights_reset(closure_rand);\n";
    frag_gen << codegen.surface.serialized_or_default("return Closure(0);\n");
    frag_gen << "}\n\n";

    frag_gen << "float4 nodetree_npr()\n";
    frag_gen << "{\n";
    frag_gen << codegen.npr.serialized_or_default("return float4(0.0f);\n");
    frag_gen << "}\n\n";

    frag_gen << "float4 nodetree_filter()\n";
    frag_gen << "{\n";
    frag_gen << codegen.filter.serialized_or_default("return float4(0.0f);\n");
    frag_gen << "}\n\n";

    /* TODO(fclem): Find a way to pass material parameters inside the material UBO. */
    info.define("thickness_mode", thickness_type == MAT_THICKNESS_SLAB ? "-1.0" : "1.0");

    frag_gen << "float nodetree_thickness()\n";
    frag_gen << "{\n";
    if (codegen.thickness.empty()) {
      /* Check presence of closure needing thickness to not add mandatory dependency on obinfos. */
      if (!GPU_material_flag_get(
              gpumat, GPU_MATFLAG_SUBSURFACE | GPU_MATFLAG_REFRACT | GPU_MATFLAG_TRANSLUCENT))
      {
        frag_gen << "return 0.0;\n";
      }
      else {
        if (info.additional_infos_.first_index_of_try({"draw_object_infos"}) == -1) {
          info.additional_info("draw_object_infos");
        }
        /* TODO(fclem): Should use `to_scale` but the gpu_shader_math_matrix_lib.glsl isn't
         * included everywhere yet. */
        frag_gen << "float3 ob_scale;\n";
        frag_gen << "ob_scale.x = length(drw_modelmat()[0].xyz);\n";
        frag_gen << "ob_scale.y = length(drw_modelmat()[1].xyz);\n";
        frag_gen << "ob_scale.z = length(drw_modelmat()[2].xyz);\n";
        frag_gen << "float3 ls_dimensions = safe_rcp(abs(drw_object_infos().orco_mul.xyz));\n";
        frag_gen << "float3 ws_dimensions = ob_scale * ls_dimensions;\n";
        /* Choose the minimum axis so that cuboids are better represented. */
        frag_gen << "return reduce_min(ws_dimensions);\n";
      }
    }
    else {
      frag_gen << codegen.thickness.serialized;
    }
    frag_gen << "}\n\n";

    if (has_depth_offset) {
      BLI_assert(codegen.depth_offset.has_value());
      frag_gen << "float nodetree_depth_offset()\n";
      frag_gen << "{\n";
      if (material_depth_offset_graph_has_unsupported_dependencies(gpumat, *codegen.depth_offset)) {
        frag_gen << "return 0.0f;\n";
      }
      else {
        frag_gen << codegen.depth_offset->serialized_or_default("return 0.0f;\n");
      }
      frag_gen << "}\n\n";
    }

    frag_gen << "Closure nodetree_volume()\n";
    frag_gen << "{\n";
    frag_gen << "  closure_weights_reset(0.0);\n";
    frag_gen << codegen.volume.serialized_or_default("return Closure(0);\n");
    frag_gen << "}\n\n";

    Vector<StringRefNull> dependencies = material_dependencies_finalize(dependencies_set);
    info.generated_sources.append({"eevee_nodetree_frag_lib.glsl", dependencies, frag_gen.str()});
  }

  const char *material_name = (info.name_.c_str() + 2);
  /* Make shaders that have as too many attributes fail compilation and have correct error
   * report instead of raising an error. */
  if (slots.vertex_id_overflow()) {
    std::cerr << "Error: EEVEE: Material " << material_name << " uses too many attributes."
              << std::endl;
    /* Avoid assert in ShaderCreateInfo::finalize. */
    info.vertex_inputs_.clear();
  }
  /* Make shaders that have as too many samplers fail compilation and have correct error
   * report instead of raising an error. */
  if (slots.sampler_overflow()) {
    /* We ran out of binding slots. Many systems inside the GPU backend assume a max amount of 64
     * samplers. */
    std::cerr << "Error: EEVEE: Material " << material_name << " uses too many samplers."
              << std::endl;
    /* Avoid assert in ShaderCreateInfo::finalize. */
    info.batch_resources_.clear();
  }

  material_create_info_pipelines_amend(geometry_type, pipeline_type, info);
}

struct LightShaderPipelineInfo {
  eLightShaderPipeline pipeline_type;
  const char *create_info_name;
  const char *name_suffix;
  const char *error_label;
  uint64_t shader_uuid;
};

static const LightShaderPipelineInfo &light_shader_pipeline_info_get(
    const eLightShaderPipeline pipeline_type)
{
  static constexpr LightShaderPipelineInfo infos[] = {
      {eLightShaderPipeline::Surface,
       "eevee_light_shader",
       "_light_shader",
       "Light shader",
       0xEEAA0001u},
      {eLightShaderPipeline::Front,
       "eevee_light_shader_front",
       "_light_shader_front",
       "Front-layer light shader",
       0xEEAA0005u},
      {eLightShaderPipeline::Bake,
       "eevee_light_shader_bake",
       "_light_shader_bake",
       "Bake light shader",
       0xEEAA0006u},
      {eLightShaderPipeline::Volume,
       "eevee_light_shader_volume",
       "_light_shader_volume",
       "Volume light shader",
       0xEEAA0002u},
      {eLightShaderPipeline::Surfel,
       "eevee_light_shader_surfel",
       "_light_shader_surfel",
       "Surfel light shader",
       0xEEAA0003u},
      {eLightShaderPipeline::Uniform,
       "eevee_light_shader_uniform",
       "_light_shader_uniform",
       "Uniform light shader",
       0xEEAA0004u},
  };
  const int index = int(pipeline_type);
  BLI_assert(index >= 0 && index < ARRAY_SIZE(infos));
  if (index < 0 || index >= ARRAY_SIZE(infos)) {
    return infos[0];
  }
  BLI_assert(infos[index].pipeline_type == pipeline_type);
  return infos[index];
}

static void light_create_info_amend(GPUMaterial *gpumat,
                                    GPUCodegenOutput *codegen_,
                                    const LightShaderPipelineInfo &pipeline_info)
{
  using namespace blender::gpu::shader;

  GPUCodegenOutput &codegen = *codegen_;
  ShaderCreateInfo &info = *reinterpret_cast<ShaderCreateInfo *>(codegen.create_info);

  info.additional_info(pipeline_info.create_info_name);
  info.name_ += pipeline_info.name_suffix;

  SlotAllocator slots;
  const ShaderCreateInfo *light_shader_info = reinterpret_cast<const ShaderCreateInfo *>(
      GPU_shader_create_info_get(pipeline_info.create_info_name));
  slots.reserve_slots(*light_shader_info);
  slots.reserve_sampler_range(MATERIAL_TEXTURE_RESERVED_SLOT_FIRST,
                              MATERIAL_TEXTURE_RESERVED_SLOT_LAST_NO_EVAL);
  slots.reserve_sampler(LIGHT_SHADER_TEX_SLOT);

  for (auto &resource : info.batch_resources_) {
    if (resource.bind_type == ShaderCreateInfo::Resource::BindType::SAMPLER) {
      resource.slot = slots.get_next_sampler();
    }
  }

  for (auto &res : info.batch_resources_) {
    res.info_name = "eevee_node_tree";
  }
  for (auto &res : info.pass_resources_) {
    res.info_name = "eevee_node_tree";
  }
  for (auto &res : info.geometry_resources_) {
    res.info_name = "eevee_node_tree";
  }

  std::string generated_resource_header = info.typedef_source_generated;
  generated_resource_header += "#ifdef CREATE_INFO_RES_PASS_eevee_node_tree\n";
  generated_resource_header += "CREATE_INFO_RES_PASS_eevee_node_tree\n";
  generated_resource_header += "#endif\n";
  generated_resource_header += "#ifdef CREATE_INFO_RES_BATCH_eevee_node_tree\n";
  generated_resource_header += "CREATE_INFO_RES_BATCH_eevee_node_tree\n";
  generated_resource_header += "#endif\n";
  generated_resource_header += "#ifdef CREATE_INFO_RES_GEOMETRY_eevee_node_tree\n";
  generated_resource_header += "CREATE_INFO_RES_GEOMETRY_eevee_node_tree\n";
  generated_resource_header += "#endif\n";
  generated_resource_header += "\n";
  info.generated_sources.append({"eevee_nodetree_type_lib.glsl", {}, generated_resource_header});

  Set<StringRefNull> dependencies_set;
  Set<StringRefNull> emitted_generated_sources;
  std::stringstream generated_source_block;
  dependencies_set.add("eevee_geom_types_lib.glsl");
  dependencies_set.add("eevee_attributes_world_lib.glsl");
  dependencies_set.add("eevee_light_lib.glsl");
  dependencies_set.add("eevee_nodetree_lib.glsl");
  if (codegen.light_shader.has_value()) {
    material_graph_dependencies_append(gpumat,
                                       codegen.light_shader->dependencies,
                                       dependencies_set,
                                       emitted_generated_sources,
                                       generated_source_block);
  }

  std::stringstream comp_gen;
  comp_gen << "void attrib_load(WorldPoint domain) {}\n\n";
  comp_gen << generated_source_block.str();
  comp_gen << "float4 nodetree_light_shader()\n";
  comp_gen << "{\n";
  comp_gen << (codegen.light_shader.has_value() ?
                   codegen.light_shader->serialized_or_default("return float4(1.0f);\n") :
                   "return float4(1.0f);\n");
  comp_gen << "}\n\n";

  Vector<StringRefNull> dependencies = material_dependencies_finalize(dependencies_set);
  info.generated_sources.append({"eevee_nodetree_frag_lib.glsl", dependencies, comp_gen.str()});

  const char *material_name = (info.name_.c_str() + 2);
  if (slots.sampler_overflow()) {
    std::cerr << "Error: EEVEE: " << pipeline_info.error_label << " " << material_name
              << " uses too many samplers." << std::endl;
    info.batch_resources_.clear();
  }
}

struct CallbackThunk {
  ShaderModule *shader_module;
  blender::Material *default_mat;
};

/* WATCH: This can be called from another thread! Needs to not touch the shader module in any
 * thread unsafe manner. */
static void codegen_callback(void *void_thunk, GPUMaterial *mat, GPUCodegenOutput *codegen)
{
  CallbackThunk *thunk = static_cast<CallbackThunk *>(void_thunk);
  thunk->shader_module->material_create_info_amend(mat, codegen);
}

static void light_codegen_callback(void *void_thunk, GPUMaterial *mat, GPUCodegenOutput *codegen)
{
  const LightShaderPipelineInfo *pipeline_info = static_cast<const LightShaderPipelineInfo *>(
      void_thunk);
  light_create_info_amend(mat, codegen, *pipeline_info);
}

static GPUPass *pass_replacement_cb(void *void_thunk, GPUMaterial *mat)
{
  using namespace blender::gpu::shader;

  CallbackThunk *thunk = static_cast<CallbackThunk *>(void_thunk);

  const blender::Material *blender_mat = GPU_material_get_material(mat);

  uint64_t shader_uuid = GPU_material_uuid_get(mat);

  eMaterialPipeline pipeline_type;
  eMaterialGeometry geometry_type;
  eMaterialDisplacement displacement_type;
  eMaterialThickness thickness_type;
  eMaterialProbe probe_capture;
  bool transparent_shadows;
  bool use_outline;
  bool depth_offset_affect_lighting;
  material_type_from_shader_uuid(shader_uuid,
                                 pipeline_type,
                                 geometry_type,
                                 displacement_type,
                                 thickness_type,
                                 probe_capture,
                                 transparent_shadows,
                                 use_outline,
                                 depth_offset_affect_lighting);
  UNUSED_VARS(depth_offset_affect_lighting);

  bool is_shadow_pass = pipeline_type == eMaterialPipeline::MAT_PIPE_SHADOW;
  bool is_prepass = ELEM(pipeline_type,
                         eMaterialPipeline::MAT_PIPE_PREPASS_DEFERRED,
                         eMaterialPipeline::MAT_PIPE_PREPASS_DEFERRED_VELOCITY,
                         eMaterialPipeline::MAT_PIPE_PREPASS_OVERLAP,
                         eMaterialPipeline::MAT_PIPE_PREPASS_FORWARD,
                         eMaterialPipeline::MAT_PIPE_PREPASS_FORWARD_VELOCITY,
                         eMaterialPipeline::MAT_PIPE_PREPASS_PLANAR);

  bool has_vertex_displacement = GPU_material_has_displacement_output(mat) &&
                                 displacement_type != eMaterialDisplacement::MAT_DISPLACEMENT_BUMP;
  bool has_transparency = GPU_material_flag_get(mat, GPU_MATFLAG_TRANSPARENT);
  bool has_shadow_transparency = has_transparency && transparent_shadows;
  bool has_raytraced_transmission = blender_mat && (blender_mat->blend_flag & MA_BL_SS_REFRACTION);
  bool has_raycast = GPU_material_flag_get(mat, GPU_MATFLAG_RAYCAST);
  bool has_depth_offset = GPU_material_has_depth_offset_output(mat);

  bool can_use_default = (is_shadow_pass &&
                          (!has_vertex_displacement && !has_shadow_transparency)) ||
                         (is_prepass && (!has_vertex_displacement && !has_transparency &&
                                         !has_raytraced_transmission && !has_raycast &&
                                         !has_depth_offset));
  if (can_use_default) {
    GPUMaterial *mat = thunk->shader_module->material_shader_get(thunk->default_mat,
                                                                 thunk->default_mat->nodetree,
                                                                 pipeline_type,
                                                                 geometry_type,
                                                                 probe_capture,
                                                                 false,
                                                                 nullptr,
                                                                 use_outline);
    return GPU_material_get_pass(mat);
  }

  return nullptr;
}

static void store_node_tree_errors(GPUMaterialFromNodeTreeResult &material_from_tree)
{
  Depsgraph *depsgraph = DRW_context_get()->depsgraph;
  if (!depsgraph) {
    return;
  }
  if (!DEG_is_active(depsgraph)) {
    return;
  }
  for (const GPUMaterialFromNodeTreeResult::Error &error : material_from_tree.errors) {
    const bNodeTree &tree = error.node->owner_tree();
    if (const bNodeTree *tree_orig = DEG_get_original(&tree)) {
      std::lock_guard lock(tree_orig->runtime->shader_node_errors_mutex);
      tree_orig->runtime->shader_node_errors.lookup_or_add_default(error.node->identifier)
          .add(error.message);
    }
  }
}

GPUMaterial *ShaderModule::material_shader_get(blender::Material *blender_mat,
                                               bNodeTree *nodetree,
                                               eMaterialPipeline pipeline_type,
                                               eMaterialGeometry geometry_type,
                                               eMaterialProbe probe_capture,
                                               bool deferred_compilation,
                                               blender::Material *default_mat,
                                               bool use_outline)
{
  eMaterialDisplacement displacement_type = to_displacement_type(blender_mat->displacement_method);
  eMaterialThickness thickness_type = to_thickness_type(blender_mat->thickness_mode);
  const bool has_depth_offset_output = material_output_has_depth_offset(nodetree) &&
                                       material_pipeline_supports_depth_offset(pipeline_type,
                                                                               geometry_type);
  const bool compile_npr_graph = (pipeline_type == MAT_PIPE_DEFERRED_NPR) ||
                                 (pipeline_type == MAT_PIPE_BAKE_COLOR &&
                                  npr_tree_get(nodetree) != nullptr);
  const bool needs_npr_vertex_displacement = compile_npr_graph &&
                                             (displacement_type != MAT_DISPLACEMENT_BUMP);
  const bool needs_npr_depth_offset = compile_npr_graph && has_depth_offset_output;
  /* NPR passes normally only need the attached NPR tree, but true displacement lives on the
   * primary material output. Depth Offset is also a material output and must use the same custom
   * depth for DEPTH_EQUAL and screen-space position reads. */
  const bool compile_surface_graph = (pipeline_type != MAT_PIPE_DEFERRED_NPR) ||
                                     needs_npr_vertex_displacement || needs_npr_depth_offset;

  uint64_t shader_uuid = shader_uuid_from_material_type(
      pipeline_type,
      geometry_type,
      displacement_type,
      thickness_type,
      probe_capture,
      blender_mat->blend_flag,
      use_outline,
      material_depth_offset_affects_lighting(blender_mat));

  bool is_default_material = default_mat == nullptr;
  BLI_assert(blender_mat != default_mat);

  CallbackThunk thunk = {this, default_mat};

  GPUMaterialFromNodeTreeResult material_from_tree = GPU_material_from_nodetree(
      blender_mat,
      nodetree,
      &blender_mat->gpumaterial,
      blender_mat->id.name,
      GPU_MAT_EEVEE,
      shader_uuid,
      compile_surface_graph,
      compile_npr_graph,
      false,
      false,
      deferred_compilation,
      codegen_callback,
      &thunk,
      is_default_material ? nullptr : pass_replacement_cb);
  store_node_tree_errors(material_from_tree);
  return material_from_tree.material;
}

GPUMaterial *ShaderModule::world_shader_get(blender::World *blender_world,
                                            bNodeTree *nodetree,
                                            eMaterialPipeline pipeline_type,
                                            bool deferred_compilation)
{
  const bool compile_npr_graph = pipeline_type == MAT_PIPE_DEFERRED &&
                                 npr_tree_get(nodetree) != nullptr;
  uint64_t shader_uuid = shader_uuid_from_material_type(pipeline_type,
                                                        MAT_GEOM_WORLD,
                                                        MAT_DISPLACEMENT_BUMP,
                                                        MAT_THICKNESS_SPHERE,
                                                        MAT_PROBE_NONE,
                                                        0,
                                                        true,
                                                        compile_npr_graph);

  CallbackThunk thunk = {this, nullptr};

  GPUMaterialFromNodeTreeResult material_from_tree = GPU_material_from_nodetree(
      nullptr,
      nodetree,
      &blender_world->gpumaterial,
      blender_world->id.name,
      GPU_MAT_EEVEE,
      shader_uuid,
      true,
      compile_npr_graph,
      false,
      compile_npr_graph,
      deferred_compilation,
      codegen_callback,
      &thunk);
  store_node_tree_errors(material_from_tree);
  return material_from_tree.material;
}

GPUMaterial *ShaderModule::light_shader_get(blender::Light *blender_light,
                                            bNodeTree *nodetree,
                                            const eLightShaderPipeline pipeline_type,
                                            bool deferred_compilation)
{
  const LightShaderPipelineInfo &pipeline_info = light_shader_pipeline_info_get(pipeline_type);

  GPUMaterialFromNodeTreeResult material_from_tree = GPU_material_from_nodetree(
      nullptr,
      nodetree,
      &blender_light->gpumaterial,
      blender_light->id.name,
      GPU_MAT_EEVEE,
      pipeline_info.shader_uuid,
      false,
      false,
      true,
      false,
      deferred_compilation,
      light_codegen_callback,
      const_cast<LightShaderPipelineInfo *>(&pipeline_info));
  store_node_tree_errors(material_from_tree);
  return material_from_tree.material;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pipeline states
 *
 * \{ */

void ShaderModule::material_create_info_pipelines_amend(eMaterialGeometry geometry_type,
                                                        eMaterialPipeline pipeline_type,
                                                        gpu::shader::ShaderCreateInfo &r_info)
{
  /* Pipeline states to compile during shader compilation. */
  if (geometry_type == MAT_GEOM_WORLD) {
    switch (pipeline_type) {
      case MAT_PIPE_VOLUME_MATERIAL: {
        /* World Volume Pipeline */
        r_info.pipeline_state()
            .primitive(GPU_PRIM_TRIS)
            .state(GPU_WRITE_COLOR,
                   GPU_BLEND_NONE,
                   GPU_CULL_NONE,
                   GPU_DEPTH_NONE,
                   GPU_STENCIL_NONE,
                   GPU_STENCIL_OP_NONE,
                   GPU_VERTEX_LAST)
            .viewports(1)
            .depth_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8)
            .stencil_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8);

        break;
      }
      case MAT_PIPE_FILTER: {
        /* Filter fullscreen pipeline. */
        r_info.pipeline_state()
            .primitive(GPU_PRIM_TRIS)
            .state(GPU_WRITE_COLOR,
                   GPU_BLEND_NONE,
                   GPU_CULL_NONE,
                   GPU_DEPTH_NONE,
                   GPU_STENCIL_NONE,
                   GPU_STENCIL_OP_NONE,
                   GPU_VERTEX_LAST)
            .viewports(1)
            .color_format(gpu::TextureTargetFormat::SFLOAT_16_16_16_16);

        break;
      }

      default: {
        /* World Pipeline */
        r_info.pipeline_state()
            .primitive(GPU_PRIM_TRIS)
            .state(GPU_WRITE_COLOR,
                   GPU_BLEND_NONE,
                   GPU_CULL_NONE,
                   GPU_DEPTH_ALWAYS,
                   GPU_STENCIL_NONE,
                   GPU_STENCIL_OP_NONE,
                   GPU_VERTEX_LAST)
            .viewports(1)
            .color_format(gpu::TextureTargetFormat::SFLOAT_16_16_16_16);

        /* Background Pipeline */
        r_info.pipeline_state()
            .primitive(GPU_PRIM_TRIS)
            .state(GPU_WRITE_COLOR,
                   GPU_BLEND_NONE,
                   GPU_CULL_NONE,
                   GPU_DEPTH_EQUAL,
                   GPU_STENCIL_NONE,
                   GPU_STENCIL_OP_NONE,
                   GPU_VERTEX_LAST)
            .viewports(1)
            .depth_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8)
            .stencil_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8)
            .color_format(gpu::TextureTargetFormat::SFLOAT_16_16_16_16);

        break;
      }
    }

    return;
  }

  /* Determine primitive type base on the geometry type. */
  /* TODO: For curves we should use the correct one based on the scene settings. Currently it will
   * assume it is set to strip or cylinder. */
  constexpr GPUPrimType prim_type = GPU_PRIM_TRIS;

  switch (pipeline_type) {
    case MAT_PIPE_PREPASS_DEFERRED: {
      /* DeferredLayer pipeline. */
      r_info.pipeline_state()
          .primitive(prim_type)
          .state(GPU_WRITE_DEPTH | GPU_WRITE_STENCIL,
                 GPU_BLEND_NONE,
                 GPU_CULL_NONE,
                 GPU_DEPTH_GREATER_EQUAL,
                 GPU_STENCIL_ALWAYS,
                 GPU_STENCIL_OP_REPLACE,
                 GPU_VERTEX_LAST)
          .viewports(1)
          .depth_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8)
          .stencil_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8)
          .color_format(gpu::TextureTargetFormat::SFLOAT_16_16);

      /* Deferred probe pipeline */
      r_info.pipeline_state()
          .primitive(prim_type)
          .state(GPU_WRITE_DEPTH,
                 GPU_BLEND_NONE,
                 GPU_CULL_NONE,
                 GPU_DEPTH_GREATER_EQUAL,
                 GPU_STENCIL_NONE,
                 GPU_STENCIL_OP_NONE,
                 GPU_VERTEX_LAST)
          .viewports(1)
          .depth_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8)
          .stencil_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8)
          .color_format(gpu::TextureTargetFormat::SFLOAT_16_16);

      break;
    }

    case MAT_PIPE_PREPASS_DEFERRED_VELOCITY: {
      r_info.pipeline_state()
          .primitive(prim_type)
          .state(GPU_WRITE_COLOR | GPU_WRITE_DEPTH | GPU_WRITE_STENCIL,
                 GPU_BLEND_NONE,
                 GPU_CULL_NONE,
                 GPU_DEPTH_GREATER_EQUAL,
                 GPU_STENCIL_ALWAYS,
                 GPU_STENCIL_OP_REPLACE,
                 GPU_VERTEX_LAST)
          .viewports(1)
          .depth_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8)
          .stencil_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8)
          .color_format(gpu::TextureTargetFormat::SFLOAT_16_16);
      break;
    }
    case MAT_PIPE_DEFERRED: {
      r_info.pipeline_state()
          .primitive(prim_type)
          .state(GPU_WRITE_COLOR | GPU_WRITE_STENCIL,
                 GPU_BLEND_NONE,
                 GPU_CULL_NONE,
                 GPU_DEPTH_EQUAL,
                 GPU_STENCIL_ALWAYS,
                 GPU_STENCIL_OP_REPLACE,
                 GPU_VERTEX_LAST)
          .viewports(1)
          .depth_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8)
          .stencil_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8)
          .color_format(gpu::TextureTargetFormat::SFLOAT_16_16_16_16)
          .color_format(gpu::TextureTargetFormat::UINT_32)
          .color_format(gpu::TextureTargetFormat::UNORM_16_16)
          .color_format(gpu::TextureTargetFormat::UNORM_10_10_10_2)
          .color_format(gpu::TextureTargetFormat::UNORM_10_10_10_2);

      /* Planar probe */
      r_info.pipeline_state()
          .primitive(prim_type)
          .state(GPU_WRITE_COLOR | GPU_WRITE_STENCIL,
                 GPU_BLEND_NONE,
                 GPU_CULL_NONE,
                 GPU_DEPTH_EQUAL,
                 GPU_STENCIL_NONE,
                 GPU_STENCIL_OP_NONE,
                 GPU_VERTEX_LAST)
          .viewports(1)
          .depth_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH)
          .color_format(gpu::TextureTargetFormat::UFLOAT_11_11_10)
          .color_format(gpu::TextureTargetFormat::UINT_32)
          .color_format(gpu::TextureTargetFormat::UNORM_16_16)
          .color_format(gpu::TextureTargetFormat::UNORM_10_10_10_2)
          .color_format(gpu::TextureTargetFormat::UNORM_10_10_10_2);

      break;
    }
    case MAT_PIPE_DEFERRED_NPR: {
      r_info.pipeline_state()
          .primitive(prim_type)
          .state(GPU_WRITE_COLOR,
                 GPU_BLEND_NONE,
                 GPU_CULL_NONE,
                 GPU_DEPTH_EQUAL,
                 GPU_STENCIL_NONE,
                 GPU_STENCIL_OP_NONE,
                 GPU_VERTEX_LAST)
          .viewports(1)
          .depth_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8)
          .stencil_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8)
          .color_format(gpu::TextureTargetFormat::SFLOAT_16_16_16_16);
      break;
    }

    case MAT_PIPE_BAKE_COLOR: {
      r_info.pipeline_state()
          .primitive(prim_type)
          .state(GPU_WRITE_COLOR,
                 GPU_BLEND_NONE,
                 GPU_CULL_NONE,
                 GPU_DEPTH_NONE,
                 GPU_STENCIL_NONE,
                 GPU_STENCIL_OP_NONE,
                 GPU_VERTEX_LAST)
          .viewports(1)
          .color_format(gpu::TextureTargetFormat::SFLOAT_16_16_16_16);
      break;
    }

    case MAT_PIPE_PREPASS_FORWARD: {
      r_info.pipeline_state()
          .primitive(prim_type)
          .state(GPU_WRITE_DEPTH,
                 GPU_BLEND_NONE,
                 GPU_CULL_NONE,
                 GPU_DEPTH_GREATER_EQUAL,
                 GPU_STENCIL_NONE,
                 GPU_STENCIL_OP_NONE,
                 GPU_VERTEX_LAST)
          .viewports(1)
          .depth_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8)
          .stencil_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8)
          .color_format(gpu::TextureTargetFormat::SFLOAT_16_16);
      break;
    }
    case MAT_PIPE_PREPASS_OVERLAP: {
      r_info.pipeline_state()
          .primitive(prim_type)
          .state(GPU_WRITE_DEPTH,
                 GPU_BLEND_NONE,
                 GPU_CULL_NONE,
                 GPU_DEPTH_GREATER_EQUAL,
                 GPU_STENCIL_NONE,
                 GPU_STENCIL_OP_NONE,
                 GPU_VERTEX_LAST)
          .viewports(1)
          .depth_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8)
          .stencil_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8)
          .color_format(gpu::TextureTargetFormat::SFLOAT_16_16);
      break;
    }
    case MAT_PIPE_PREPASS_FORWARD_VELOCITY: {
      r_info.pipeline_state()
          .primitive(prim_type)
          .state(GPU_WRITE_COLOR | GPU_WRITE_DEPTH,
                 GPU_BLEND_NONE,
                 GPU_CULL_NONE,
                 GPU_DEPTH_GREATER_EQUAL,
                 GPU_STENCIL_NONE,
                 GPU_STENCIL_OP_NONE,
                 GPU_VERTEX_LAST)
          .viewports(1)
          .depth_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8)
          .stencil_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8)
          .color_format(gpu::TextureTargetFormat::SFLOAT_16_16);
      break;
    }
    case MAT_PIPE_FORWARD: {
      r_info.pipeline_state()
          .primitive(prim_type)
          .state(GPU_WRITE_COLOR,
                 GPU_BLEND_NONE,
                 GPU_CULL_NONE,
                 GPU_DEPTH_EQUAL,
                 GPU_STENCIL_NONE,
                 GPU_STENCIL_OP_NONE,
                 GPU_VERTEX_LAST)
          .viewports(1)
          .depth_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8)
          .stencil_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8)
          .color_format(gpu::TextureTargetFormat::SFLOAT_16_16_16_16);
      break;
    }

    case MAT_PIPE_SHADOW: {
      r_info.pipeline_state()
          .primitive(prim_type)
          .state(GPU_WRITE_COLOR | GPU_WRITE_DEPTH,
                 GPU_BLEND_NONE,
                 GPU_CULL_NONE,
                 GPU_DEPTH_LESS,
                 GPU_STENCIL_NONE,
                 GPU_STENCIL_OP_NONE,
                 GPU_VERTEX_LAST)
          .viewports(16);
      break;
    }

    case MAT_PIPE_PREPASS_PLANAR: {
      r_info.pipeline_state()
          .primitive(GPU_PRIM_TRIS)
          .state(GPU_WRITE_DEPTH,
                 GPU_BLEND_NONE,
                 GPU_CULL_NONE,
                 GPU_DEPTH_GREATER_EQUAL,
                 GPU_STENCIL_NONE,
                 GPU_STENCIL_OP_NONE,
                 GPU_VERTEX_LAST)
          .viewports(1)
          .depth_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH)
          .color_format(gpu::TextureTargetFormat::UFLOAT_11_11_10)
          .color_format(gpu::TextureTargetFormat::UINT_32)
          .color_format(gpu::TextureTargetFormat::UNORM_16_16)
          .color_format(gpu::TextureTargetFormat::UNORM_10_10_10_2)
          .color_format(gpu::TextureTargetFormat::UNORM_10_10_10_2);
      break;
    }

    case MAT_PIPE_VOLUME_MATERIAL: {
      /* Volume Material Pipeline */
      r_info.pipeline_state()
          .primitive(prim_type)
          .state(GPU_WRITE_STENCIL,
                 GPU_BLEND_NONE,
                 GPU_CULL_NONE,
                 GPU_DEPTH_NONE,
                 GPU_STENCIL_NEQUAL,
                 GPU_STENCIL_OP_REPLACE,
                 GPU_VERTEX_LAST)
          .viewports(1)
          .depth_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8)
          .stencil_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8);

      break;
    }
    case MAT_PIPE_VOLUME_OCCUPANCY: {
      /* Volume Occupancy Pipeline */
      r_info.pipeline_state()
          .primitive(prim_type)
          .state(GPU_WRITE_DEPTH,
                 GPU_BLEND_NONE,
                 GPU_CULL_NONE,
                 GPU_DEPTH_NONE,
                 GPU_STENCIL_NONE,
                 GPU_STENCIL_OP_NONE,
                 GPU_VERTEX_LAST)
          .viewports(1)
          .depth_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8)
          .stencil_format(gpu::TextureTargetFormat::SFLOAT_32_DEPTH_UINT_8);
      break;
    }

    default:
      break;
  }
}

/** \} */

}  // namespace blender::eevee
