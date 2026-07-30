/* SPDX-FileCopyrightText: 2006 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Manages materials, lights and textures.
 */

#include <climits>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "BKE_lib_id.hh"
#include "MEM_guardedalloc.h"

#include "DNA_material_types.h"
#include "DNA_light_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_world_types.h"

#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_time.h"
#include "BLI_utildefines.h"
#include "BLI_array.hh"
#include "BLI_map.hh"
#include "BLI_vector.hh"

#include <utility>

#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"

#include "NOD_shader.h"
#include "NOD_shader_nodes_inline.hh"

#include "GPU_material.hh"
#include "GPU_pass.hh"
#include "GPU_shader.hh"
#include "GPU_texture.hh"
#include "GPU_uniform_buffer.hh"

#include "DRW_engine.hh"

#include "gpu_node_graph.hh"

#include "atomic_ops.h"

namespace blender {

static void gpu_material_ramp_texture_build(GPUMaterial *mat);
static void gpu_material_sky_texture_build(GPUMaterial *mat);

static std::unordered_map<const Material *, int> g_material_recompile_serials;
static std::mutex g_material_recompile_serials_mutex;

static const Material *gpu_material_recompile_serial_key(const Material *material)
{
  return material->id.orig_id ? reinterpret_cast<const Material *>(material->id.orig_id) : material;
}

/* Structs */
#define MAX_COLOR_BAND 128
#define MAX_GPU_SKIES 8

struct GPUColorBandBuilder {
  float pixels[MAX_COLOR_BAND][CM_TABLE + 1][4];
  int current_layer;
};

struct GPUSkyBuilder {
  float pixels[MAX_GPU_SKIES][GPU_SKY_WIDTH * GPU_SKY_HEIGHT][4];
  int current_layer;
};

struct GPUMaterialClosureCallbackInputStorage {
  int closure_output_node_id;
  std::string item_key;
  GPUType type;
  int function_input_index;
};

struct GPUMaterialClosureCallbackFrameStorage {
  Vector<GPUMaterialClosureCallbackInputStorage> inputs;
  std::string error;
};

struct GPUMaterial {
  /* Contains #gpu::Shader and source code for deferred compilation.
   * Can be shared between materials sharing same node-tree topology. */
  GPUPass *pass = nullptr;
  /* Optimized GPUPass, situationally compiled after initial pass for optimal realtime performance.
   * This shader variant bakes dynamic uniform data as constant. This variant will not use
   * the ubo, and instead bake constants directly into the shader source. */
  GPUPass *optimized_pass = nullptr;

  /* UBOs for this material parameters. */
  gpu::UniformBuf *ubo = nullptr;
  /* Some flags about the nodetree & the needed resources. */
  eGPUMaterialFlag flag = GPU_MATFLAG_UPDATED;
  bool is_time_dependent = false;
  /* The engine type this material is compiled for. */
  eGPUMaterialEngine engine;
  /* Identify shader variations (shadow, probe, world background...) */
  uint64_t uuid = 0;
  /* Number of generated function. */
  int generated_function_len = 0;

  /* Source material, might be null for worlds and lights. */
  Material *source_material = nullptr;
  bool is_world = false;
  /* 1D Texture array containing all color bands. */
  gpu::Texture *coba_tex = nullptr;
  /* Builder for coba_tex. */
  GPUColorBandBuilder *coba_builder = nullptr;
  /* 2D Texture array containing all sky textures. */
  gpu::Texture *sky_tex = nullptr;
  /* Builder for sky_tex. */
  GPUSkyBuilder *sky_builder = nullptr;
  /* Low level node graph(s). Also contains resources needed by the material. */
  GPUNodeGraph graph = {};
  bool uses_referenced_object_data = false;
  Vector<GPUReferencedObject> referenced_objects;
  Vector<Object *> filter_object_infos;
  Vector<Object *> filter_mask_objects;
  Vector<GPUMaterialGeneratedSource> generated_sources;
  Vector<std::string> closure_uv_source_stack;
  Vector<GPUType> closure_uv_source_type_stack;
  Vector<std::string> closure_uv_dx_source_stack;
  Vector<std::string> closure_uv_dy_source_stack;
  Vector<GPUMaterialClosureCallbackFrameStorage> closure_callback_input_frame_stack;

  bool has_surface_output = false;
  bool has_volume_output = false;
  bool has_displacement_output = false;
  bool has_depth_offset_output = false;
  bool has_filter_output = false;
  bool has_light_shader_output = false;
  bool has_glsl_light_shader_eval = false;
  bool has_shader_info_shadow_classification = false;
  bool uses_hiz_data = false;

  std::string name;

  GPUMaterial(eGPUMaterialEngine engine) : engine(engine) {};

  ~GPUMaterial()
  {
    gpu_node_graph_free(&graph);

    if (optimized_pass != nullptr) {
      GPU_pass_release(optimized_pass);
    }
    if (pass != nullptr) {
      GPU_pass_release(pass);
    }
    if (ubo != nullptr) {
      GPU_uniformbuf_free(ubo);
    }
    if (coba_builder != nullptr) {
      MEM_delete(coba_builder);
    }
    if (coba_tex != nullptr) {
      GPU_texture_free(coba_tex);
    }
    if (sky_tex != nullptr) {
      GPU_texture_free(sky_tex);
    }
  }
};

static void gpu_material_force_glsl_closure_callback_inline_error(
    GPUMaterial *material, const StringRef helper_name)
{
  static constexpr StringRefNull failure_name = "glsl_closure_callback_inline_failure";
  static constexpr StringRefNull failure_filename =
      "glsl_closure_callback_inline_failure.glsl";
  const std::string source = "#error GLSL_closure_callback_" + std::string(helper_name) +
                             "_inline_failed\n";
  GPU_material_generated_source_add(material, failure_filename, {}, source);

  GPUNodeStack failure_output[2] = {};
  failure_output[0].type = GPU_CLOSURE;
  failure_output[0].hasoutput = true;
  failure_output[1].end = true;
  if (GPU_stack_link_custom(material,
                            nullptr,
                            failure_name,
                            failure_filename,
                            GPU_CUSTOM_NODE_DEPENDENCY_NONE,
                            nullptr,
                            failure_output))
  {
    GPU_material_output_surface(material, failure_output[0].link);
  }
}

/* Public API */

GPUMaterialFromNodeTreeResult GPU_material_from_nodetree(
    Material *ma,
    bNodeTree *ntree,
    ListBaseT<LinkData> *gpumaterials,
    const char *name,
    eGPUMaterialEngine engine,
    uint64_t shader_uuid,
    bool compile_surface_graph,
    bool compile_npr_graph,
    bool compile_light_shader_graph,
    bool force_npr_graph,
    bool deferred_compilation,
    GPUCodegenCallbackFn callback,
    void *thunk,
    GPUMaterialPassReplacementCallbackFn pass_replacement_cb)
{
  /* Search if this material is not already compiled. */
  for (LinkData &link : *gpumaterials) {
    GPUMaterial *mat = static_cast<GPUMaterial *>(link.data);
    if (mat->uuid == shader_uuid && mat->engine == engine) {
      if (!deferred_compilation) {
        GPU_pass_ensure_its_ready(mat->pass);
      }
      return {mat};
    }
  }

  GPUMaterialFromNodeTreeResult result;

  GPUMaterial *mat = MEM_new<GPUMaterial>(__func__, engine);
  mat->source_material = ma;
  mat->is_world = (ma == nullptr) && compile_surface_graph && !compile_light_shader_graph;
  mat->uuid = shader_uuid;
  mat->name = name;
  result.material = mat;

  bNodeTree *npr_localtree = nullptr;
  bNodeTree *localtree = nullptr;
  if (compile_surface_graph || compile_light_shader_graph) {
    /* Localize tree to create links for reroute and mute. */
    localtree = bke::node_tree_add_tree(
        nullptr, (StringRef(ntree->id.name) + " Inlined").c_str(), ntree->idname);
    localtree->flag |= NTREE_IS_GPU_SHADER_INTERNAL;
    nodes::InlineShaderNodeTreeParams inline_params;
    inline_params.allow_preserving_repeat_zones = true;
    inline_params.target_engine_ = engine == GPU_MAT_EEVEE ? SHD_OUTPUT_EEVEE : SHD_OUTPUT_ALL;
    const bool inline_success = nodes::inline_shader_node_tree(*ntree, *localtree, inline_params);

    std::string callback_inline_failure_helper;
    for (nodes::InlineShaderNodeTreeParams::ErrorMessage &error : inline_params.r_error_messages) {
      if (!inline_success && callback_inline_failure_helper.empty()) {
        const StringRef message(error.message);
        static constexpr StringRef prefix = "GLSL closure callback '";
        if (message.startswith(prefix)) {
          const StringRef helper_and_suffix = message.drop_prefix(prefix.size());
          const int64_t helper_end = helper_and_suffix.find("'");
          if (helper_end != StringRef::not_found) {
            callback_inline_failure_helper = helper_and_suffix.substr(0, helper_end);
          }
        }
      }
      result.errors.append({error.node, std::move(error.message)});
    }

    if (compile_surface_graph) {
      ntreeGPUMaterialNodes(localtree, mat);
    }
    if (compile_light_shader_graph) {
      ntreeGPULightShaderNodes(localtree, mat);
    }
    if (!inline_success && !callback_inline_failure_helper.empty()) {
      gpu_material_force_glsl_closure_callback_inline_error(
          mat, callback_inline_failure_helper);
    }
  }
  if (compile_npr_graph) {
    if (force_npr_graph || npr_tree_get(ntree) != nullptr) {
      GPU_material_flag_set(mat, GPU_MATFLAG_NPR);
    }
    if (GPU_material_flag_get(mat, GPU_MATFLAG_NPR)) {
      npr_localtree = ntreeGPUNPRNodes(ntree, mat);
    }
  }

  gpu_material_ramp_texture_build(mat);
  gpu_material_sky_texture_build(mat);

  /* Use default material pass when possible. */
  if (GPUPass *default_pass = pass_replacement_cb ? pass_replacement_cb(thunk, mat) : nullptr) {
    mat->pass = default_pass;
    GPU_pass_acquire(mat->pass);
    /** WORKAROUND:
     * The node tree code is never executed in default replaced passes,
     * but the GPU validation will still complain if the node tree UBO is not bound.
     * So we create a dummy UBO with (at least) the size of the default material one (192 bytes).
     * We allocate 256 bytes to leave some room for future changes. */
    mat->ubo = GPU_uniformbuf_create_ex(256, nullptr, "Dummy UBO");
  }
  else {
    /* Create source code and search pass cache for an already compiled version. */
    mat->pass = GPU_generate_pass(
        mat, &mat->graph, mat->name.c_str(), engine, deferred_compilation, callback, thunk, false);
  }

  /* Determine whether we should generate an optimized variant of the graph.
   * Heuristic is based on complexity of default material pass and shader node graph. */
  if (GPU_pass_should_optimize(mat->pass)) {
    mat->optimized_pass = GPU_generate_pass(
        mat, &mat->graph, mat->name.c_str(), engine, true, callback, thunk, true);
  }

  gpu_node_graph_free_nodes(&mat->graph);
  /* Only free after GPU_pass_shader_get where gpu::UniformBuf read data from the local
   * tree. */
  if (localtree != nullptr) {
    BKE_id_free(nullptr, &localtree->id);
  }
  if (npr_localtree != nullptr) {
    BKE_id_free(nullptr, &npr_localtree->id);
  }

  /* Note that even if building the shader fails in some way, we want to keep
   * it to avoid trying to compile again and again, and simply do not use
   * the actual shader on drawing. */
  LinkData *link = MEM_new_zeroed<LinkData>("GPUMaterialLink");
  link->data = mat;
  BLI_addtail(gpumaterials, link);

  return result;
}

GPUMaterial *GPU_material_from_callbacks(eGPUMaterialEngine engine,
                                         ConstructGPUMaterialFn construct_function_cb,
                                         GPUCodegenCallbackFn generate_code_function_cb,
                                         void *thunk)
{
  /* Allocate a new material and its material graph. */
  GPUMaterial *material = MEM_new<GPUMaterial>(__func__, engine);

  /* Construct the material graph by adding and linking the necessary GPU material nodes. */
  construct_function_cb(thunk, material);

  /* Create and initialize the texture storing color bands used by Ramp and Curve nodes. */
  gpu_material_ramp_texture_build(material);

  /* Lookup an existing pass in the cache or generate a new one. */
  material->pass = GPU_generate_pass(material,
                                     &material->graph,
                                     __func__,
                                     engine,
                                     false,
                                     generate_code_function_cb,
                                     thunk,
                                     false);

  /* Determine whether we should generate an optimized variant of the graph.
   * Heuristic is based on complexity of default material pass and shader node graph. */
  if (GPU_pass_should_optimize(material->pass)) {
    material->optimized_pass = GPU_generate_pass(material,
                                                 &material->graph,
                                                 __func__,
                                                 engine,
                                                 true,
                                                 generate_code_function_cb,
                                                 thunk,
                                                 true);
  }

  gpu_node_graph_free_nodes(&material->graph);

  return material;
}

void GPU_material_free_single(GPUMaterial *material)
{
  MEM_delete(material);
}

void GPU_material_free(ListBaseT<LinkData> *gpumaterial)
{
  for (LinkData &link : *gpumaterial) {
    GPUMaterial *material = static_cast<GPUMaterial *>(link.data);
    GPU_material_free_single(material);
  }
  gpumaterial->free_no_destruct();
}

void GPU_materials_free(Main *bmain)
{
  for (Material &ma : bmain->materials) {
    GPU_material_free(&ma.gpumaterial);
  }

  for (World &wo : bmain->worlds) {
    GPU_material_free(&wo.gpumaterial);
  }

  for (Light &la : bmain->lights) {
    GPU_material_free(&la.gpumaterial);
  }

  BKE_material_defaults_free_gpu();
}

const char *GPU_material_get_name(GPUMaterial *material)
{
  return material->name.c_str();
}

uint64_t GPU_material_uuid_get(GPUMaterial *mat)
{
  return mat->uuid;
}

Material *GPU_material_get_material(GPUMaterial *material)
{
  return material->source_material;
}

bool GPU_material_is_world(const GPUMaterial *material)
{
  return material->is_world;
}

GPUPass *GPU_material_get_pass(GPUMaterial *material)
{
  /* If an optimized pass variant is available, and optimization is
   * flagged as complete, we use this one instead. */
  return GPU_material_optimization_status(material) == GPU_MAT_OPTIMIZATION_SUCCESS ?
             material->optimized_pass :
             material->pass;
}

gpu::Shader *GPU_material_get_shader(GPUMaterial *material)
{
  return GPU_pass_shader_get(GPU_material_get_pass(material));
}

GPUMaterialStatus GPU_material_status(GPUMaterial *mat)
{
  switch (GPU_pass_status(mat->pass)) {
    case GPU_PASS_SUCCESS:
      return GPU_MAT_SUCCESS;
    case GPU_PASS_QUEUED:
      return GPU_MAT_QUEUED;
    default:
      return GPU_MAT_FAILED;
  }
}

eGPUMaterialOptimizationStatus GPU_material_optimization_status(GPUMaterial *mat)
{
  if (!GPU_pass_should_optimize(mat->pass)) {
    return GPU_MAT_OPTIMIZATION_SKIP;
  }

  switch (GPU_pass_status(mat->optimized_pass)) {
    case GPU_PASS_SUCCESS:
      return GPU_MAT_OPTIMIZATION_SUCCESS;
    case GPU_PASS_QUEUED:
      return GPU_MAT_OPTIMIZATION_QUEUED;
    default:
      BLI_assert_unreachable();
      return GPU_MAT_OPTIMIZATION_SKIP;
  }
}

uint64_t GPU_material_compilation_timestamp(GPUMaterial *mat)
{
  return GPU_pass_compilation_timestamp(mat->pass);
}

double GPU_material_compilation_time(GPUMaterial *mat)
{
  return GPU_pass_compilation_time(GPU_material_get_pass(mat));
}

int GPU_material_recompile_serial_get(const GPUMaterial *mat)
{
  if (mat->source_material == nullptr) {
    return 0;
  }

  const Material *source_material = gpu_material_recompile_serial_key(mat->source_material);
  std::lock_guard lock(g_material_recompile_serials_mutex);
  const auto serial = g_material_recompile_serials.find(source_material);
  return serial == g_material_recompile_serials.end() ? 0 : serial->second;
}

void GPU_material_recompile_serial_increment(Material *material)
{
  if (material == nullptr) {
    return;
  }

  const Material *source_material = gpu_material_recompile_serial_key(material);
  std::lock_guard lock(g_material_recompile_serials_mutex);
  int &serial = g_material_recompile_serials[source_material];
  serial = (serial == INT_MAX) ? 1 : serial + 1;
}

void GPU_material_recompile_serial_clear(const Material *material)
{
  if (material == nullptr) {
    return;
  }

  std::lock_guard lock(g_material_recompile_serials_mutex);
  g_material_recompile_serials.erase(material);
}

bool GPU_material_has_surface_output(GPUMaterial *mat)
{
  return mat->has_surface_output;
}

bool GPU_material_has_volume_output(GPUMaterial *mat)
{
  return mat->has_volume_output;
}

bool GPU_material_has_displacement_output(GPUMaterial *mat)
{
  return mat->has_displacement_output;
}

bool GPU_material_has_depth_offset_output(GPUMaterial *mat)
{
  return mat->has_depth_offset_output;
}

bool GPU_material_has_filter_output(GPUMaterial *mat)
{
  return mat->has_filter_output;
}

bool GPU_material_has_light_shader_output(GPUMaterial *mat)
{
  return mat != nullptr && mat->has_light_shader_output;
}

bool GPU_material_has_glsl_light_shader_eval(const GPUMaterial *mat)
{
  return mat != nullptr && mat->has_glsl_light_shader_eval;
}

bool GPU_material_has_shader_info_shadow_classification(const GPUMaterial *mat)
{
  return mat != nullptr && mat->has_shader_info_shadow_classification;
}

bool GPU_material_uses_hiz_data(const GPUMaterial *mat)
{
  return mat != nullptr && mat->uses_hiz_data;
}

int GPU_material_filter_object_info_ensure(GPUMaterial *material, Object *object)
{
  if (material == nullptr || object == nullptr) {
    return -1;
  }

  const int existing_index = material->filter_object_infos.first_index_of_try(object);
  if (existing_index != -1) {
    return existing_index;
  }

  material->filter_object_infos.append(object);
  return material->filter_object_infos.size() - 1;
}

static Object *gpu_material_referenced_object_original(Object *object)
{
  if (object == nullptr) {
    return nullptr;
  }
  if (object->id.orig_id != nullptr) {
    return reinterpret_cast<Object *>(object->id.orig_id);
  }
  return object;
}

uint32_t GPU_material_referenced_object_ensure(GPUMaterial *material,
                                               Object *object,
                                               eGPUReferencedObjectDataFlag flags)
{
  if (material == nullptr) {
    return 0;
  }
  material->uses_referenced_object_data = true;

  Object *original = gpu_material_referenced_object_original(object);
  if (original == nullptr) {
    return 0;
  }

  /* Temporary Main IDs intentionally do not receive session UIDs. Referenced-object requests
   * from runtime-only graphs must degrade to the shader default instead of tripping the UID assert. */
  if (original->id.tag & ID_TAG_TEMP_MAIN) {
    return 0;
  }
  if (original->id.session_uid == 0) {
    BKE_lib_libblock_session_uid_ensure(&original->id);
  }
  const uint32_t session_uid = original->id.session_uid;
  if (session_uid == 0) {
    return 0;
  }

  for (GPUReferencedObject &entry : material->referenced_objects) {
    if (entry.session_uid == session_uid) {
      entry.flags |= flags;
      return session_uid;
    }
  }

  GPUReferencedObject entry;
  entry.object = original;
  entry.session_uid = session_uid;
  entry.flags = flags;
  material->referenced_objects.append(entry);
  return session_uid;
}

bool GPU_material_uses_referenced_object_data(const GPUMaterial *material)
{
  return material != nullptr && material->uses_referenced_object_data;
}

int GPU_material_referenced_object_count(const GPUMaterial *material)
{
  return (material != nullptr) ? material->referenced_objects.size() : 0;
}

const GPUReferencedObject *GPU_material_referenced_object_get(const GPUMaterial *material,
                                                              int index)
{
  if (material == nullptr || index < 0 || index >= material->referenced_objects.size()) {
    return nullptr;
  }
  return &material->referenced_objects[index];
}

int GPU_material_filter_object_info_count(const GPUMaterial *material)
{
  return (material != nullptr) ? material->filter_object_infos.size() : 0;
}

Object *GPU_material_filter_object_info_get(const GPUMaterial *material, int index)
{
  if (material == nullptr || index < 0 || index >= material->filter_object_infos.size()) {
    return nullptr;
  }
  return material->filter_object_infos[index];
}

int GPU_material_filter_mask_object_append(GPUMaterial *material, Object *object)
{
  if (material == nullptr || object == nullptr) {
    return -1;
  }

  material->filter_mask_objects.append(object);
  return material->filter_mask_objects.size() - 1;
}

int GPU_material_filter_mask_object_count(const GPUMaterial *material)
{
  return (material != nullptr) ? material->filter_mask_objects.size() : 0;
}

Object *GPU_material_filter_mask_object_get(const GPUMaterial *material, int index)
{
  if (material == nullptr || index < 0 || index >= material->filter_mask_objects.size()) {
    return nullptr;
  }
  return material->filter_mask_objects[index];
}

void GPU_material_generated_source_add(GPUMaterial *material,
                                       StringRefNull filename,
                                       Span<StringRefNull> dependencies,
                                       StringRefNull content)
{
  if (material == nullptr || filename.is_empty() || content.is_empty()) {
    return;
  }

  GPUMaterialGeneratedSource generated_source;
  generated_source.filename = filename;
  generated_source.content = content;
  generated_source.dependencies.reserve(dependencies.size());
  for (const StringRefNull dependency : dependencies) {
    if (!dependency.is_empty()) {
      generated_source.dependencies.append(dependency);
    }
  }

  for (GPUMaterialGeneratedSource &existing_source : material->generated_sources) {
    if (existing_source.filename == generated_source.filename) {
      existing_source.dependencies = std::move(generated_source.dependencies);
      existing_source.content = std::move(generated_source.content);
      return;
    }
  }

  material->generated_sources.append(generated_source);
}

int GPU_material_generated_source_count(const GPUMaterial *material)
{
  return (material != nullptr) ? material->generated_sources.size() : 0;
}

const GPUMaterialGeneratedSource *GPU_material_generated_source_get(const GPUMaterial *material,
                                                                    int index)
{
  if (material == nullptr || index < 0 || index >= material->generated_sources.size()) {
    return nullptr;
  }
  return &material->generated_sources[index];
}

bool GPU_material_flag_get(const GPUMaterial *mat, eGPUMaterialFlag flag)
{
  return (mat->flag & flag) != 0;
}

bool GPU_material_has_outline_output(const GPUMaterial *mat)
{
  return mat != nullptr && !BLI_listbase_is_empty(&mat->graph.outlink_outlines);
}

eGPUMaterialFlag GPU_material_flag(const GPUMaterial *mat)
{
  return mat->flag;
}

void GPU_material_flag_set(GPUMaterial *mat, eGPUMaterialFlag flag)
{
  if ((flag & GPU_MATFLAG_GLOSSY) && (mat->flag & GPU_MATFLAG_GLOSSY)) {
    /* Tag material using multiple glossy BSDF as using clear coat. */
    mat->flag |= GPU_MATFLAG_COAT;
  }
  mat->flag |= flag;
}

void GPU_material_set_time_dependent(GPUMaterial *mat)
{
  mat->is_time_dependent = true;
}

bool GPU_material_is_time_dependent(const GPUMaterial *mat)
{
  return mat->is_time_dependent;
}

void GPU_material_uniform_buffer_create(GPUMaterial *material, ListBaseT<LinkData> *inputs)
{
  material->ubo = GPU_uniformbuf_create_from_list(inputs, material->name.c_str());
}

gpu::UniformBuf *GPU_material_uniform_buffer_get(GPUMaterial *material)
{
  return material->ubo;
}

ListBaseT<GPUMaterialAttribute> GPU_material_attributes(const GPUMaterial *material)
{
  return material->graph.attributes;
}

ListBaseT<GPUMaterialTexture> GPU_material_textures(GPUMaterial *material)
{
  return material->graph.textures;
}

const GPUUniformAttrList *GPU_material_uniform_attributes(const GPUMaterial *material)
{
  const GPUUniformAttrList *attrs = &material->graph.uniform_attrs;
  return attrs->count > 0 ? attrs : nullptr;
}

const ListBaseT<GPULayerAttr> *GPU_material_layer_attributes(const GPUMaterial *material)
{
  const ListBaseT<GPULayerAttr> *attrs = &material->graph.layer_attrs;
  return !attrs->is_empty() ? attrs : nullptr;
}

GPUNodeGraph *gpu_material_node_graph(GPUMaterial *material)
{
  return &material->graph;
}

void GPU_material_closure_uv_source_push(GPUMaterial *material, StringRefNull source)
{
  GPU_material_closure_uv_source_push(material, source, GPU_VEC2);
}

void GPU_material_closure_uv_source_push(GPUMaterial *material,
                                         StringRefNull source,
                                         GPUType source_type)
{
  if (material == nullptr) {
    return;
  }
  material->closure_uv_source_stack.append(std::string(source));
  material->closure_uv_source_type_stack.append(source_type);
}

void GPU_material_closure_uv_source_pop(GPUMaterial *material)
{
  if (material == nullptr || material->closure_uv_source_stack.is_empty()) {
    return;
  }
  material->closure_uv_source_stack.pop_last();
  if (!material->closure_uv_source_type_stack.is_empty()) {
    material->closure_uv_source_type_stack.pop_last();
  }
}

StringRefNull GPU_material_closure_uv_source_get(const GPUMaterial *material)
{
  if (material == nullptr || material->closure_uv_source_stack.is_empty()) {
    return {};
  }
  return material->closure_uv_source_stack.last();
}

GPUType GPU_material_closure_uv_source_type_get(const GPUMaterial *material)
{
  if (material == nullptr || material->closure_uv_source_type_stack.is_empty()) {
    return GPU_NONE;
  }
  return material->closure_uv_source_type_stack.last();
}

void GPU_material_closure_uv_gradient_source_push(GPUMaterial *material,
                                                  StringRefNull dx_source,
                                                  StringRefNull dy_source)
{
  if (material == nullptr) {
    return;
  }
  material->closure_uv_dx_source_stack.append(std::string(dx_source));
  material->closure_uv_dy_source_stack.append(std::string(dy_source));
}

void GPU_material_closure_uv_gradient_source_pop(GPUMaterial *material)
{
  if (material == nullptr || material->closure_uv_dx_source_stack.is_empty() ||
      material->closure_uv_dy_source_stack.is_empty())
  {
    return;
  }
  material->closure_uv_dx_source_stack.pop_last();
  material->closure_uv_dy_source_stack.pop_last();
}

void GPU_material_closure_uv_gradient_source_get(const GPUMaterial *material,
                                                 StringRefNull &r_dx_source,
                                                 StringRefNull &r_dy_source)
{
  r_dx_source = {};
  r_dy_source = {};
  if (material == nullptr || material->closure_uv_dx_source_stack.is_empty() ||
      material->closure_uv_dy_source_stack.is_empty())
  {
    return;
  }
  r_dx_source = material->closure_uv_dx_source_stack.last();
  r_dy_source = material->closure_uv_dy_source_stack.last();
}

void GPU_material_closure_callback_input_frame_push(GPUMaterial *material,
                                                    Span<GPUMaterialClosureCallbackInput> inputs)
{
  if (material == nullptr) {
    return;
  }

  GPUMaterialClosureCallbackFrameStorage frame;
  frame.inputs.reserve(inputs.size());
  for (const GPUMaterialClosureCallbackInput &input : inputs) {
    GPUMaterialClosureCallbackInputStorage stored_input;
    stored_input.closure_output_node_id = input.closure_output_node_id;
    stored_input.item_key = std::string(input.item_key.data(), size_t(input.item_key.size()));
    stored_input.type = input.type;
    stored_input.function_input_index = input.function_input_index;
    frame.inputs.append(std::move(stored_input));
  }
  material->closure_callback_input_frame_stack.append(std::move(frame));
}

void GPU_material_closure_callback_input_frame_pop(GPUMaterial *material)
{
  if (material == nullptr || material->closure_callback_input_frame_stack.is_empty()) {
    return;
  }
  material->closure_callback_input_frame_stack.pop_last();
}

bool GPU_material_closure_callback_input_find(const GPUMaterial *material,
                                              const int closure_output_node_id,
                                              const StringRef item_key,
                                              GPUType &r_type,
                                              int &r_function_input_index,
                                              bool &r_is_ancestor_capture)
{
  r_type = GPU_NONE;
  r_function_input_index = -1;
  r_is_ancestor_capture = false;
  if (material == nullptr || material->closure_callback_input_frame_stack.is_empty()) {
    return false;
  }

  const auto &frames = material->closure_callback_input_frame_stack;
  for (const GPUMaterialClosureCallbackInputStorage &input : frames.last().inputs) {
    if (input.closure_output_node_id == closure_output_node_id &&
        StringRef(input.item_key) == item_key)
    {
      r_type = input.type;
      r_function_input_index = input.function_input_index;
      return true;
    }
  }

  /* Inputs from an outer callback function are not in scope in the nested function. Treat them as
   * an unsupported capture instead of aliasing their inN index to the nested frame. */
  for (int64_t frame_index = frames.size() - 1; frame_index-- > 0;) {
    for (const GPUMaterialClosureCallbackInputStorage &input : frames[frame_index].inputs) {
      if (input.closure_output_node_id == closure_output_node_id &&
          StringRef(input.item_key) == item_key)
      {
        r_is_ancestor_capture = true;
        return false;
      }
    }
  }
  return false;
}

bool GPU_material_closure_callback_input_frame_error_set(GPUMaterial *material,
                                                         const StringRef error)
{
  if (material == nullptr || material->closure_callback_input_frame_stack.is_empty()) {
    return false;
  }
  GPUMaterialClosureCallbackFrameStorage &frame =
      material->closure_callback_input_frame_stack.last();
  if (frame.error.empty()) {
    frame.error = std::string(error);
  }
  return true;
}

bool GPU_material_closure_callback_input_frame_error_get(const GPUMaterial *material,
                                                         std::string &r_error)
{
  r_error.clear();
  if (material == nullptr || material->closure_callback_input_frame_stack.is_empty()) {
    return false;
  }
  r_error = material->closure_callback_input_frame_stack.last().error;
  return !r_error.empty();
}

/* Resources */

gpu::Texture **gpu_material_sky_texture_layer_set(
    GPUMaterial *mat, int width, int height, const float *pixels, float *row)
{
  /* In order to put all sky textures into one 2D array texture,
   * we need them to be the same size. */
  BLI_assert(width == GPU_SKY_WIDTH);
  BLI_assert(height == GPU_SKY_HEIGHT);
  UNUSED_VARS_NDEBUG(width, height);

  if (mat->sky_builder == nullptr) {
    mat->sky_builder = MEM_new_uninitialized<GPUSkyBuilder>("GPUSkyBuilder");
    mat->sky_builder->current_layer = 0;
  }

  int layer = mat->sky_builder->current_layer;
  *row = float(layer);

  if (*row == MAX_GPU_SKIES) {
    printf("Too many sky textures in shader!\n");
  }
  else {
    float *dst = reinterpret_cast<float *>(mat->sky_builder->pixels[layer]);
    memcpy(dst, pixels, sizeof(float) * GPU_SKY_WIDTH * GPU_SKY_HEIGHT * 4);
    mat->sky_builder->current_layer += 1;
  }

  return &mat->sky_tex;
}

gpu::Texture **gpu_material_ramp_texture_row_set(GPUMaterial *mat,
                                                 int size,
                                                 const float *pixels,
                                                 float *r_row)
{
  /* In order to put all the color-bands into one 1D array texture,
   * we need them to be the same size. */
  BLI_assert(size == CM_TABLE + 1);
  UNUSED_VARS_NDEBUG(size);

  if (mat->coba_builder == nullptr) {
    mat->coba_builder = MEM_new_uninitialized<GPUColorBandBuilder>("GPUColorBandBuilder");
    mat->coba_builder->current_layer = 0;
  }

  int layer = mat->coba_builder->current_layer;
  *r_row = float(layer);

  if (*r_row == MAX_COLOR_BAND) {
    printf("Too many color band in shader! Remove some Curve, Black Body or Color Ramp Node.\n");
  }
  else {
    float *dst = reinterpret_cast<float *>(mat->coba_builder->pixels[layer]);
    memcpy(dst, pixels, sizeof(float) * (CM_TABLE + 1) * 4);
    mat->coba_builder->current_layer += 1;
  }

  return &mat->coba_tex;
}

static void gpu_material_ramp_texture_build(GPUMaterial *mat)
{
  if (mat->coba_builder == nullptr) {
    return;
  }

  GPUColorBandBuilder *builder = mat->coba_builder;

  mat->coba_tex = GPU_texture_create_1d_array("mat_ramp",
                                              CM_TABLE + 1,
                                              builder->current_layer,
                                              1,
                                              gpu::TextureFormat::SFLOAT_16_16_16_16,
                                              GPU_TEXTURE_USAGE_SHADER_READ,
                                              reinterpret_cast<float *>(builder->pixels));

  MEM_delete(builder);
  mat->coba_builder = nullptr;
}

static void gpu_material_sky_texture_build(GPUMaterial *mat)
{
  if (mat->sky_builder == nullptr) {
    return;
  }

  mat->sky_tex = GPU_texture_create_2d_array("mat_sky",
                                             GPU_SKY_WIDTH,
                                             GPU_SKY_HEIGHT,
                                             mat->sky_builder->current_layer,
                                             1,
                                             gpu::TextureFormat::SFLOAT_32_32_32_32,
                                             GPU_TEXTURE_USAGE_SHADER_READ,
                                             reinterpret_cast<float *>(mat->sky_builder->pixels));

  MEM_delete(mat->sky_builder);
  mat->sky_builder = nullptr;
}

/* Code generation */

void GPU_material_output_surface(GPUMaterial *material, GPUNodeLink *link)
{
  if (!material->graph.outlink_surface) {
    material->graph.outlink_surface = link;
    material->has_surface_output = true;
  }
}

void GPU_material_output_volume(GPUMaterial *material, GPUNodeLink *link)
{
  if (!material->graph.outlink_volume) {
    material->graph.outlink_volume = link;
    material->has_volume_output = true;
  }
}

void GPU_material_output_displacement(GPUMaterial *material, GPUNodeLink *link)
{
  if (!material->graph.outlink_displacement) {
    material->graph.outlink_displacement = link;
    material->has_displacement_output = true;
  }
}

void GPU_material_output_thickness(GPUMaterial *material, GPUNodeLink *link)
{
  if (!material->graph.outlink_thickness) {
    material->graph.outlink_thickness = link;
  }
}

void GPU_material_output_depth_offset(GPUMaterial *material, GPUNodeLink *link)
{
  if (!material->graph.outlink_depth_offset) {
    material->graph.outlink_depth_offset = link;
    material->has_depth_offset_output = true;
  }
}

void GPU_material_output_npr(GPUMaterial *material, GPUNodeLink *link)
{
  if (!material->graph.outlink_npr) {
    material->graph.outlink_npr = link;
  }
}

void GPU_material_output_filter(GPUMaterial *material, GPUNodeLink *link)
{
  GPU_material_output_filter_item(material, 0, link);
}

void GPU_material_output_filter_item(GPUMaterial *material, int identifier, GPUNodeLink *link)
{
  if (link != nullptr && !material->graph.outlink_filter) {
    material->graph.outlink_filter = link;
    material->has_filter_output = true;
  }
  if (link == nullptr) {
    return;
  }
  GPUNodeGraphOutputLink *filter_link = MEM_new<GPUNodeGraphOutputLink>(__func__);
  filter_link->hash = identifier;
  filter_link->outlink = link;
  BLI_addtail(&material->graph.outlink_filters, filter_link);
  material->has_filter_output = true;
}

void GPU_material_output_light_shader(GPUMaterial *material, GPUNodeLink *link)
{
  if (link != nullptr && !material->graph.outlink_light_shader) {
    material->graph.outlink_light_shader = link;
    material->has_light_shader_output = true;
  }
}

void GPU_material_glsl_light_shader_eval_set(GPUMaterial *material)
{
  if (material != nullptr) {
    material->has_glsl_light_shader_eval = true;
  }
}

void GPU_material_shader_info_shadow_classification_set(GPUMaterial *material)
{
  if (material != nullptr) {
    material->has_shader_info_shadow_classification = true;
  }
}

void GPU_material_hiz_data_set(GPUMaterial *material)
{
  if (material != nullptr) {
    material->uses_hiz_data = true;
  }
}

void GPU_material_add_output_link_aov(GPUMaterial *material, GPUNodeLink *link, int hash)
{
  GPUNodeGraphOutputLink *aov_link = MEM_new_zeroed<GPUNodeGraphOutputLink>(__func__);
  aov_link->outlink = link;
  aov_link->hash = hash;
  BLI_addtail(&material->graph.outlink_aovs, aov_link);
}

void GPU_material_add_output_link_outline(GPUMaterial *material, GPUNodeLink *link)
{
  GPUNodeGraphOutputLink *outline_link = MEM_new_zeroed<GPUNodeGraphOutputLink>(__func__);
  outline_link->outlink = link;
  BLI_addtail(&material->graph.outlink_outlines, outline_link);
}

void GPU_material_add_output_link_composite(GPUMaterial *material, GPUNodeLink *link)
{
  GPUNodeGraphOutputLink *compositor_link = MEM_new_zeroed<GPUNodeGraphOutputLink>(__func__);
  compositor_link->outlink = link;
  BLI_addtail(&material->graph.outlink_compositor, compositor_link);
}

static bool gpu_node_link_uses_closure_callback_input(const GPUNodeGraph &graph,
                                                      const GPUNodeLink *root_link)
{
  if (root_link == nullptr) {
    return false;
  }
  if (root_link->link_type == GPU_NODE_LINK_FUNCTION_CALL) {
    return root_link->function_call != nullptr &&
           StringRef(root_link->function_call).startswith("$OUT = in");
  }
  if (root_link->link_type != GPU_NODE_LINK_OUTPUT || root_link->output == nullptr) {
    return false;
  }

  Set<const GPUNode *> visited_nodes;
  Vector<const GPUNode *> nodes_to_visit;
  nodes_to_visit.append(root_link->output->node);
  while (!nodes_to_visit.is_empty()) {
    const GPUNode *node = nodes_to_visit.pop_last();
    if (node == nullptr || !visited_nodes.add(node)) {
      continue;
    }
    for (const GPUInput &input : node->inputs) {
      if (input.source == GPU_SOURCE_FUNCTION_CALL && input.function_call != nullptr &&
          StringRef(input.function_call).startswith("$OUT = in"))
      {
        return true;
      }
      if (input.source == GPU_SOURCE_OUTPUT && input.link != nullptr &&
          input.link->output != nullptr)
      {
        nodes_to_visit.append(input.link->output->node);
      }
    }

    /* Zone start nodes are implicit dependencies of their matching zone end nodes. */
    if (node->is_zone_end) {
      for (const GPUNode &zone_node : graph.nodes) {
        if (zone_node.zone_index == node->zone_index && !zone_node.is_zone_end) {
          nodes_to_visit.append(&zone_node);
        }
      }
    }
  }
  return false;
}

char *GPU_material_split_sub_function(GPUMaterial *material,
                                      GPUType return_type,
                                      GPUNodeLink **link,
                                      StringRefNull dependency_name)
{
  if (!material->closure_callback_input_frame_stack.is_empty() &&
      gpu_node_link_uses_closure_callback_input(material->graph,
                                                link != nullptr ? *link : nullptr))
  {
    GPU_material_closure_callback_input_frame_error_set(
        material,
        "Callback input-dependent graph cannot be captured by a legacy zero-input GPU "
        "sub-function");
  }

  /* Force cast to return type. */
  switch (return_type) {
    case GPU_FLOAT:
      GPU_link(material, "set_value", *link, link);
      break;
    case GPU_VEC3:
      GPU_link(material, "set_rgb", *link, link);
      break;
    case GPU_VEC4:
      GPU_link(material, "set_rgba", *link, link);
      break;
    default:
      BLI_assert(0);
      break;
  }

  GPUNodeGraphFunctionLink *func_link = MEM_new_zeroed<GPUNodeGraphFunctionLink>(__func__);
  func_link->mode = GPU_NODE_GRAPH_FUNCTION_LEGACY;
  func_link->outlink = *link;
  func_link->return_type = return_type;
  SNPRINTF(func_link->name, "ntree_fn%d", material->generated_function_len++);
  if (!dependency_name.is_empty()) {
    BLI_strncpy(func_link->dependency_name, dependency_name.c_str(), sizeof(func_link->dependency_name));
  }
  BLI_addtail(&material->graph.material_functions, func_link);

  return func_link->name;
}

static bool gpu_material_sub_function_type_supported(const GPUType type)
{
  return ELEM(type, GPU_FLOAT, GPU_VEC3, GPU_VEC4);
}

char *GPU_material_split_sub_function_multi(GPUMaterial *material,
                                            const Span<GPUType> input_types,
                                            const Span<GPUMaterialFunctionOutput> outputs,
                                            const StringRefNull dependency_name)
{
  BLI_assert(material != nullptr);
  BLI_assert(!outputs.is_empty());
  BLI_assert(input_types.size() <= INT_MAX && outputs.size() <= INT_MAX);
  if (material == nullptr || outputs.is_empty() || input_types.size() > INT_MAX ||
      outputs.size() > INT_MAX)
  {
    return nullptr;
  }

  for (const GPUType type : input_types) {
    BLI_assert(gpu_material_sub_function_type_supported(type));
    if (!gpu_material_sub_function_type_supported(type)) {
      return nullptr;
    }
  }
  for (const GPUMaterialFunctionOutput &output : outputs) {
    BLI_assert(gpu_material_sub_function_type_supported(output.type));
    BLI_assert(output.link != nullptr && *output.link != nullptr);
    if (!gpu_material_sub_function_type_supported(output.type) || output.link == nullptr ||
        *output.link == nullptr)
    {
      return nullptr;
    }
  }

  /* Force every graph output to its declared function output type. Outputs that share the same
   * source link and type are serialized once so a shared value (e.g. one nested callback result
   * feeding several closure outputs) is not duplicated. */
  Map<std::pair<GPUNodeLink *, GPUType>, GPUNodeLink *> shared_serialized;
  Array<GPUNodeLink *> serialized_outlinks(outputs.size());
  for (const int64_t index : outputs.index_range()) {
    const GPUMaterialFunctionOutput &output = outputs[index];
    GPUNodeLink *source = *output.link;
    const std::pair<GPUNodeLink *, GPUType> key(source, output.type);
    if (GPUNodeLink *const *found = shared_serialized.lookup_ptr(key)) {
      serialized_outlinks[index] = *found;
      if (source->link_type != GPU_NODE_LINK_OUTPUT) {
        gpu_node_link_discard(source);
      }
      continue;
    }
    GPUNodeLink *serialized = nullptr;
    bool linked = false;
    switch (output.type) {
      case GPU_FLOAT:
        linked = GPU_link(material, "set_value", source, &serialized);
        break;
      case GPU_VEC3:
        linked = GPU_link(material, "set_rgb", source, &serialized);
        break;
      case GPU_VEC4:
        linked = GPU_link(material, "set_rgba", source, &serialized);
        break;
      default:
        BLI_assert_unreachable();
        break;
    }
    if (!linked) {
      return nullptr;
    }
    shared_serialized.add_new(key, serialized);
    serialized_outlinks[index] = serialized;
  }

  GPUNodeGraphFunctionLink *func_link = MEM_new_zeroed<GPUNodeGraphFunctionLink>(__func__);
  func_link->mode = GPU_NODE_GRAPH_FUNCTION_MULTI_IO;
  func_link->input_types_len = int(input_types.size());
  if (!input_types.is_empty()) {
    func_link->input_types = MEM_new_array<GPUType>(input_types.size(), __func__);
    for (const int64_t index : input_types.index_range()) {
      func_link->input_types[index] = input_types[index];
    }
  }
  func_link->outputs_len = int(outputs.size());
  func_link->outputs = MEM_new_array<GPUNodeGraphFunctionOutput>(outputs.size(), __func__);
  for (const int64_t index : outputs.index_range()) {
    func_link->outputs[index].type = outputs[index].type;
    func_link->outputs[index].outlink = serialized_outlinks[index];
  }
  SNPRINTF(func_link->name, "ntree_fn%d", material->generated_function_len++);
  if (!dependency_name.is_empty()) {
    BLI_strncpy(
        func_link->dependency_name, dependency_name.c_str(), sizeof(func_link->dependency_name));
  }
  BLI_addtail(&material->graph.material_functions, func_link);

  return func_link->name;
}

}  // namespace blender
