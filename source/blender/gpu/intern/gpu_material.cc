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
#include <memory>
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
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "RNA_access.hh"
#include "RNA_path.hh"
#include "GPU_context.hh"

#include "NOD_shader.h"
#include "NOD_shader_glsl_bundle.hh"
#include "CLG_log.h"
#include "NOD_shader_nodes_inline.hh"

#include "GPU_material.hh"
#include "GPU_material_profile.hh"
#include "GPU_pass.hh"
#include "GPU_shader.hh"
#include "GPU_texture.hh"
#include "GPU_uniform_buffer.hh"

#include "DRW_engine.hh"

#include "gpu_node_graph.hh"

#include "atomic_ops.h"

namespace blender {

static CLG_LogRef LOG_BUNDLE = {"gpu.material.bundle"};

thread_local GPUMaterialProfile *gpu_material_profile_active = nullptr;

static void gpu_material_ramp_texture_build(GPUMaterial *mat);
static void gpu_material_sky_texture_build(GPUMaterial *mat);

static std::unordered_map<const Material *, int> g_material_recompile_serials;
static std::mutex g_material_recompile_serials_mutex;

static const Material *gpu_material_recompile_serial_key(const Material *material)
{
  return material->id.orig_id ? reinterpret_cast<const Material *>(material->id.orig_id) : material;
}

static std::weak_ptr<bke::ShaderNodeCompileContext> existing_expression_context(
    const bNodeTree &tree)
{
  const bNodeTree &original = tree.id.orig_id ?
                                  *reinterpret_cast<const bNodeTree *>(tree.id.orig_id) :
                                  tree;
  std::lock_guard lock(original.runtime->shader_node_errors_mutex);
  return original.runtime->shader_compile_context;
}

struct MaterialUniformBinding {
  nodes::ShaderUniformSource source;
  size_t byte_offset = 0;
  int components = 0;
};

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
  struct ExpressionOrigin {
    uint32_t tree_uid;
    int32_t node_id;
    std::string signature;
  };
  std::weak_ptr<bke::ShaderNodeCompileContext> expression_context;
  /* Used only while generating nodes, cleared before asynchronous compilation starts. */
  bNodeTree *expression_root = nullptr;
  std::vector<gpu::ShaderDiagnosticSource> expression_sources;
  Vector<ExpressionOrigin> expression_origins;
  Vector<bke::ShaderNodeCompileContext::Message> inline_diagnostics;
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
  bool uses_stencil_value = false;
  Vector<int> render_texture_ids;
  bool reads_stencil_value = false;
  std::string stencil_value_error;
  std::string stencil_marker_error;
  /** Fatal schema errors are retained independently of shader/pass replacement. */
  std::string inline_node_error;
  /* The engine type this material is compiled for. */
  eGPUMaterialEngine engine;
  /* Identify shader variations (shadow, probe, world background...) */
  uint64_t uuid = 0;
  /* Number of generated function. */
  int generated_function_len = 0;

  /* Source material, might be null for worlds and lights. */
  Material *source_material = nullptr;
  /* Reuse preprocessing between passes of the same evaluated material. Owned by one GPU
   * material, so normal material invalidation also discards animated values and topology. */
  const bNodeTree *surface_source_tree = nullptr;
  bNodeTree *surface_preprocessed_tree = nullptr;
  std::shared_ptr<const Vector<double>> animation_state;
  bool animation_state_valid = false;
  /* Pointer keys exist only while this material is being compiled. */
  std::unique_ptr<nodes::ShaderUniformTracking> uniform_tracking;
  const bNodeTree *uniform_tracking_tree = nullptr;
  Vector<nodes::ShaderUniformSource> uniform_origins;
  Vector<nodes::ShaderUniformSource> uniform_unsafe_sources;
  Vector<MaterialUniformBinding> uniform_bindings;
  Vector<uint8_t> uniform_cpu_data;
  bool uniform_tracking_supported = false;
  bool uniform_update_pending = false;
  bool is_world = false;
  /* The node graph is rooted at an Outline Shell Output node instead of the
   * regular material output. */
  bool is_outline_shell = false;
  bool outline_shell_has_alpha = false;
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
  Vector<GPULightShaderParameterRequest> light_shader_parameters;
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
  bool has_tessellation_output = false;
  bool has_filter_output = false;
  bool has_light_shader_output = false;
  bool has_glsl_light_shader_eval = false;
  bool has_shader_info_shadow_classification = false;
  bool has_node_soft_shadow = false;
  int node_shadow_cache_quality = 0;
  bool has_principled_npr_v2 = false;
  bool has_surface_diffusion = false;
  eGPUMaterialNPRFeature npr_features = GPU_MAT_NPR_FEATURE_NONE;
  bool uses_hiz_data = false;

  std::string name;

  GPUMaterial(eGPUMaterialEngine engine) : engine(engine) {};

  ~GPUMaterial()
  {
    gpu_node_graph_free(&graph);
    if (surface_preprocessed_tree != nullptr) {
      BKE_id_free(nullptr, &surface_preprocessed_tree->id);
    }

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

static bool gpu_material_can_cache_surface_tree(const bNodeTree &tree)
{
  for (const bNode *node : tree.all_nodes()) {
    if (node->is_type("ShaderNodeGLSLFunction"_ustr) ||
        node->is_type("GeometryNodeRepeatOutput"_ustr) ||
        node->is_type("ShaderNodeForeachLightOutput"_ustr))
    {
      return false;
    }
    for (const bNodeSocket &socket : node->inputs) {
      if (ELEM(socket.type, SOCK_CLOSURE, SOCK_BUNDLE)) {
        return false;
      }
    }
  }
  return true;
}

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


/** Publish through the same revision/owner mechanism as asynchronous compiler diagnostics. */
static bool gpu_material_bundle_error_record(GPUMaterial *material, const bNode &node, StringRef message)
{
  CLOG_ERROR(&LOG_BUNDLE, "GLSL Bundle '%s': %.*s", material->name.c_str(), int(message.size()), message.data());
  if (!ELEM(node.type_legacy, SH_NODE_GLSL_FUNCTION, NODE_SEPARATE_BUNDLE)) { return false; }
  const int previous_count = material->expression_origins.size();
  GPU_material_expression_source_add(material, node,
      {"glsl_bundle_inline_" + std::to_string(node.identifier), "", ""},
      nodes::shader_glsl_bundle_error_signature(node));
  if (material->expression_origins.size() != previous_count) {
    const auto &origin = material->expression_origins.last();
    material->inline_diagnostics.append({origin.tree_uid, origin.node_id, origin.signature, std::string(message)});
  }
  /* An expired revision must not fall back to the unversioned warning map. */
  return true;
}

/* Public API */

static bool uniform_tracking_animation_safe(const Material &material,
                                            Span<double> state,
                                            const nodes::ShaderUniformTracking &tracking);

/* Validate the expanded graph before closure/weight preprocessing. Arbitrary function outputs
 * conservatively depend on every input; no GLSL semantic analysis is required. */
static void gpu_material_validate_stencil_dependencies(GPUMaterial &mat, bNodeTree &tree)
{
  if (mat.engine != GPU_MAT_EEVEE || mat.source_material == nullptr) {
    return;
  }
  bool needs_validation = mat.source_material->stencil_mode != MA_STENCIL_STANDARD;
  for (const bNode &node : tree.nodes) {
    needs_validation |= node.type_legacy == SH_NODE_STENCIL_VALUE;
  }
  if (!needs_validation) {
    return;
  }

  /* Prove opacity before propagating dependencies: an initially unknown branch must not leave
   * permanent coverage taint after a later iteration proves it opaque. Work on the authored
   * closure graph, before Mix factors are inverted into internal BSDF Weight inputs. */
  Set<const bNode *> opaque_closures;
  auto constant_input_is = [](const bNode &node, StringRef name, float value) {
    for (const bNodeSocket &input : node.inputs) {
      if (StringRef(input.name) == name) {
        return input.type == SOCK_FLOAT && !input.link && input.default_value &&
               input.default_value_typed<bNodeSocketValueFloat>()->value == value;
      }
    }
    return false;
  };
  bool opacity_changed = true;
  while (opacity_changed) {
    opacity_changed = false;
    for (const bNode &node : tree.nodes) {
      if (opaque_closures.contains(&node)) {
        continue;
      }
      bool opaque = ELEM(node.type_legacy,
                         SH_NODE_EMISSION,
                         SH_NODE_BSDF_DIFFUSE,
                         SH_NODE_BSDF_GLOSSY,
                         SH_NODE_BSDF_REFRACTION,
                         SH_NODE_BSDF_GLASS,
                         SH_NODE_BSDF_TOON,
                         SH_NODE_BSDF_TRANSLUCENT,
                         SH_NODE_BSDF_SHEEN,
                         SH_NODE_SUBSURFACE_SCATTERING,
                         SH_NODE_BSDF_HAIR,
                         SH_NODE_BSDF_HAIR_PRINCIPLED,
                         SH_NODE_BSDF_METALLIC);
      if (ELEM(node.type_legacy, SH_NODE_BSDF_PRINCIPLED, SH_NODE_PRINCIPLED_NPR)) {
        opaque = constant_input_is(node, "Alpha", 1.0f);
      }
      else if (node.type_legacy == SH_NODE_EEVEE_SPECULAR) {
        opaque = constant_input_is(node, "Transparency", 0.0f);
      }
      else if (ELEM(node.type_legacy,
                    SH_NODE_MIX_SHADER,
                    SH_NODE_ADD_SHADER,
                    SH_NODE_NPR_SURFACE_DIFFUSION))
      {
        opaque = true;
        for (const bNodeSocket &input : node.inputs) {
          if (input.type == SOCK_SHADER) {
            opaque &= input.link && input.link->fromsock->type == SOCK_SHADER &&
                      opaque_closures.contains(input.link->fromnode);
          }
        }
      }
      for (const bNodeSocket &input : node.inputs) {
        /* Authored weights are not the internal, unconnected defaults initialized later. */
        if (StringRef(input.name) == "Weight" && input.link) {
          opaque = false;
        }
      }
      if (opaque) {
        opaque_closures.add_new(&node);
        opacity_changed = true;
      }
    }
  }

  enum { SNAPSHOT = 1, LATE_SURFACE = 2 };
  Map<const bNode *, int> source_dependencies;
  for (const bNode &node : tree.nodes) {
    std::string source;
    if (node.type_legacy == SH_NODE_GLSL_FUNCTION) {
      std::string error;
      if (!node_shader_glsl_function_source_get(node, source, error)) {
        source_dependencies.add(&node, LATE_SURFACE);
        continue;
      }
    }
    else if (node.type_legacy == SH_NODE_SCRIPT_EXPRESSION && node.storage != nullptr) {
      source = static_cast<const NodeShaderScriptExpression *>(node.storage)->expression;
    }
    int dependency = 0;
    for (const char *symbol : {"node_stencil_value", "stencil_value_tx"}) {
      if (source.find(symbol) != std::string::npos) {
        dependency |= SNAPSHOT;
      }
    }
    /* Keep early uniform light parameters usable; only evaluated lighting/screen resources
     * are late. This deliberately conservatively includes helpers present in callback sources. */
    for (const char *symbol : {"glsl_ambient_lighting",
                               "glsl_light_shadow",
                               "glsl_light_eval",
                               "glsl_light_probe_color",
                               "closure_to_rgba",
                               "node_scene_color",
                               "npr_refraction",
                               "ambient_occlusion_eval",
                               "node_ambient_occlusion",
                               "hiz_tx",
                               "gbuf_",
                               "radiance_"})
    {
      if (source.find(symbol) != std::string::npos) {
        dependency |= LATE_SURFACE;
      }
    }
    if (dependency != 0) {
      source_dependencies.add(&node, dependency);
    }
  }
  struct Dependency {
    int value = 0;
    int coverage = 0;
  };
  Map<const bNodeSocket *, Dependency> dependencies;
  auto input_dependency = [&](const bNodeSocket &input) {
    return input.link ? dependencies.lookup_default(input.link->fromsock, Dependency{}) :
                        Dependency{};
  };

  bool changed = true;
  while (changed) {
    changed = false;
    for (const bNode &node : tree.nodes) {
      int value = source_dependencies.lookup_default(&node, 0);
      int coverage = 0;
      for (const bNodeSocket &input : node.inputs) {
        const Dependency dep = input_dependency(input);
        value |= dep.value;
        if (input.type == SOCK_SHADER) {
          coverage |= dep.coverage;
        }
        if (ELEM(StringRef(input.name), "Alpha", "Weight", "Transparency") ||
            (node.type_legacy == SH_NODE_MIX_SHADER && input.type != SOCK_SHADER &&
             !opaque_closures.contains(&node)) ||
            node.type_legacy == SH_NODE_BSDF_TRANSPARENT)
        {
          coverage |= dep.value;
        }
      }
      if (node.type_legacy == SH_NODE_STENCIL_VALUE) {
        value |= SNAPSHOT;
      }
      if (ELEM(node.type_legacy,
               SH_NODE_SCENE_COLOR,
               SH_NODE_SCREENSPACE_INFO,
               SH_NODE_SHADER_INFO,
               SH_NODE_NPR_REFRACTION,
               SH_NODE_RAYCAST,
               SH_NODE_NPR_INPUT,
               SH_NODE_INPUT_AOV,
               SH_NODE_LIGHT_PROBE_COLOR,
               SH_NODE_AMBIENT_OCCLUSION))
      {
        value |= LATE_SURFACE;
      }
      const bool known_closure = ELEM(node.type_legacy,
                                      SH_NODE_BSDF_PRINCIPLED,
                                      SH_NODE_PRINCIPLED_NPR,
                                      SH_NODE_ADD_SHADER,
                                      SH_NODE_MIX_SHADER,
                                      SH_NODE_BSDF_TRANSPARENT,
                                      SH_NODE_EMISSION,
                                      SH_NODE_HOLDOUT,
                                      SH_NODE_BSDF_DIFFUSE,
                                      SH_NODE_BSDF_GLOSSY,
                                      SH_NODE_BSDF_REFRACTION,
                                      SH_NODE_BSDF_GLASS,
                                      SH_NODE_BSDF_TOON,
                                      SH_NODE_BSDF_TRANSLUCENT) ||
                                 ELEM(node.type_legacy,
                                      SH_NODE_BSDF_SHEEN,
                                      SH_NODE_EEVEE_SPECULAR,
                                      SH_NODE_SUBSURFACE_SCATTERING,
                                      SH_NODE_NPR_SURFACE_DIFFUSION,
                                      SH_NODE_BSDF_HAIR,
                                      SH_NODE_BSDF_HAIR_PRINCIPLED,
                                      SH_NODE_BSDF_METALLIC);
      for (const bNodeSocket &output : node.outputs) {
        Dependency &dep = dependencies.lookup_or_add_default(&output);
        int output_value = value;
        if (node.type_legacy == SH_NODE_SHADERTORGB) {
          output_value = StringRef(output.name) == "Alpha" ? coverage : value | LATE_SURFACE;
        }
        if (node.type_legacy == SH_NODE_PRINCIPLED_NPR && StringRef(output.name) == "Alpha") {
          output_value = coverage;
        }
        const int output_coverage = output.type == SOCK_SHADER ?
                                        (known_closure ? coverage : value) :
                                        0;
        changed |= (dep.value | output_value) != dep.value ||
                   (dep.coverage | output_coverage) != dep.coverage;
        dep.value |= output_value;
        dep.coverage |= output_coverage;
      }
    }
  }

  /* The inliner retains inactive outputs until preprocessing. Use the same active/engine
   * selection as execution, including the output flags it establishes for the outline shell. */
  const bNode *material_output = ntreeShaderOutputNode(&tree, SHD_OUTPUT_EEVEE);
  int early_dependency = 0;
  for (const bNode &node : tree.nodes) {
    if (node.type_legacy == SH_NODE_OUTPUT_MATERIAL && &node != material_output) {
      continue;
    }
    if (node.type_legacy == SH_NODE_OUTPUT_OUTLINE_SHELL &&
        (!(node.flag & NODE_DO_OUTPUT) ||
         mat.source_material->stencil_mode == MA_STENCIL_MARKER_ONLY))
    {
      continue;
    }
    if (ELEM(node.type_legacy, SH_NODE_OUTPUT_MATERIAL, SH_NODE_OUTPUT_OUTLINE_SHELL)) {
      for (const bNodeSocket &input : node.inputs) {
        const Dependency dep = input_dependency(input);
        const StringRef name(input.name);
        if (name == "Surface") {
          early_dependency |= dep.coverage;
        }
        else if (ELEM(name,
                      "Displacement",
                      "Depth Offset",
                      "Tessellation",
                      "Alpha",
                      "Offset",
                      "Strength",
                      "Vertex Offset"))
        {
          early_dependency |= dep.value;
        }
      }
    }
  }
  if (early_dependency & SNAPSHOT) {
    mat.stencil_value_error =
        "Stencil Value cannot affect opacity, transparent shader mixing, displacement, "
        "tessellation or depth offset: those must be evaluated before the stencil snapshot";
    mat.stencil_marker_error = mat.stencil_value_error;
  }
  else if (early_dependency & LATE_SURFACE) {
    mat.stencil_marker_error =
        "Early stencil marker coverage and geometry cannot depend on scene color, refraction, "
        "screen raycasts or lighting results";
  }
}

static void gpu_material_stencil_errors_append(const GPUMaterial &mat,
                                               const bNodeTree &source_tree,
                                               GPUMaterialFromNodeTreeResult &result)
{
  const bool marker = mat.source_material &&
                      mat.source_material->stencil_mode != MA_STENCIL_STANDARD;
  const std::string &message = marker ? mat.stencil_marker_error : mat.stencil_value_error;
  if (message.empty()) {
    return;
  }
  for (const bNode &node : source_tree.nodes) {
    if ((node.flag & NODE_DO_OUTPUT) &&
        ELEM(node.type_legacy, SH_NODE_OUTPUT_MATERIAL, SH_NODE_OUTPUT_OUTLINE_SHELL))
    {
      result.errors.append({&node, message});
    }
  }
}

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
    GPUMaterialPassReplacementCallbackFn pass_replacement_cb,
    bool outline_shell_root)
{
  /* Search if this material is not already compiled. */
  const bool cache_surface = ma != nullptr && ma->eevee_domain == MA_EEVEE_DOMAIN_SURFACE &&
                             engine == GPU_MAT_EEVEE && compile_surface_graph &&
                             !compile_light_shader_graph && !outline_shell_root;
  bNodeTree *cached_surface = nullptr;
  const GPUMaterial *cached_surface_owner = nullptr;
  const GPUMaterial *animation_owner = nullptr;
  for (LinkData &link : *gpumaterials) {
    GPUMaterial *mat = static_cast<GPUMaterial *>(link.data);
    if (!animation_owner && mat->engine == engine && mat->source_material == ma) {
      animation_owner = mat;
    }
    if (mat->uuid == shader_uuid && mat->engine == engine) {
      if (mat->expression_sources.empty()) {
        mat->expression_context = existing_expression_context(*ntree);
      }
      GPU_material_profile_count(GPUMaterialProfileCounter::MaterialHits);
      if (mat->uniform_update_pending) {
        BLI_assert(mat->ubo && !mat->uniform_cpu_data.is_empty());
        GPU_uniformbuf_update(mat->ubo, mat->uniform_cpu_data.data());
        mat->uniform_update_pending = false;
        mat->flag |= GPU_MATFLAG_UPDATED;
        GPU_material_profile_count(GPUMaterialProfileCounter::UniformUpdates);
      }
      if (!deferred_compilation) {
        GPUMaterialProfileTimer profile(GPUMaterialProfileStage::LookupWait);
        GPU_pass_ensure_its_ready(mat->pass);
      }
      GPUMaterialFromNodeTreeResult cached_result{mat};
      gpu_material_stencil_errors_append(*mat, *ntree, cached_result);
      return cached_result;
    }
    if (cache_surface && mat->surface_source_tree == ntree) {
      cached_surface = mat->surface_preprocessed_tree;
      cached_surface_owner = mat;
    }
  }

  GPUMaterialFromNodeTreeResult result;

  GPUMaterial *mat = MEM_new<GPUMaterial>(__func__, engine);
  GPU_material_profile_count(GPUMaterialProfileCounter::MaterialBuilds);
  mat->source_material = ma;
  if (ma != nullptr && engine == GPU_MAT_EEVEE) {
    GPUMaterialProfileTimer profile(GPUMaterialProfileStage::AnimationState);
    if (animation_owner) {
      mat->animation_state = animation_owner->animation_state;
      mat->animation_state_valid = animation_owner->animation_state_valid;
    }
    else {
      auto state = std::make_shared<Vector<double>>();
      mat->animation_state_valid = BKE_material_animation_state_get(*ma, *state);
      mat->animation_state = std::move(state);
    }
  }
  mat->is_world = (ma == nullptr) && compile_surface_graph && !compile_light_shader_graph;
  mat->is_outline_shell = outline_shell_root;
  mat->uuid = shader_uuid;
  mat->name = name;
  bNodeTree *expression_root = ntree->id.orig_id ?
                                   reinterpret_cast<bNodeTree *>(ntree->id.orig_id) :
                                   ntree;
  mat->expression_root = expression_root;
  mat->expression_context = existing_expression_context(*expression_root);
  result.material = mat;

  bNodeTree *npr_localtree = nullptr;
  bNodeTree *localtree = nullptr;
  bool owns_localtree = true;
  const bool track_uniforms = ma != nullptr && mat->animation_state_valid &&
                              engine == GPU_MAT_EEVEE && compile_surface_graph &&
                              !compile_light_shader_graph && !compile_npr_graph &&
                              !outline_shell_root &&
                              (!animation_owner || animation_owner->uniform_tracking_supported) &&
                              GPU_backend_get_type() != GPU_BACKEND_METAL;
  if (track_uniforms) {
    /* Every pass needs a source map for its own UBO; do not reuse a tree without provenance. */
    cached_surface = nullptr;
    mat->uniform_tracking = std::make_unique<nodes::ShaderUniformTracking>();
  }
  if (cached_surface != nullptr) {
    localtree = cached_surface;
    /* Preprocessing has inverted Mix factors into BSDF weights. Reuse the validation of the
     * original closure graph, not the rewritten weights, which no longer express coverage. */
    mat->stencil_value_error = cached_surface_owner->stencil_value_error;
    mat->stencil_marker_error = cached_surface_owner->stencil_marker_error;
    owns_localtree = false;
    /* Begin/EndExecTree rebuild and discard the per-pass stacks. Preprocessing is the only
     * step shared here; GPU nodes, resources, flags and uniforms still belong to this mat. */
    GPUMaterialProfileTimer profile(GPUMaterialProfileStage::Execute);
    ntreeGPUMaterialNodes(localtree, mat, true);
  }
  else if (compile_surface_graph || compile_light_shader_graph) {
    /* Localize tree to create links for reroute and mute. */
    localtree = bke::node_tree_add_tree(
        nullptr, (StringRef(ntree->id.name) + " Inlined").c_str(), ntree->idname);
    localtree->flag |= NTREE_IS_GPU_SHADER_INTERNAL;
    mat->uniform_tracking_tree = track_uniforms ? localtree : nullptr;
    nodes::InlineShaderNodeTreeParams inline_params;
    inline_params.allow_preserving_repeat_zones = true;
    inline_params.target_engine_ = engine == GPU_MAT_EEVEE ? SHD_OUTPUT_EEVEE : SHD_OUTPUT_ALL;
    inline_params.r_uniform_tracking = mat->uniform_tracking.get();
    bool inline_success;
    {
      GPUMaterialProfileTimer profile(GPUMaterialProfileStage::Inline);
      inline_success = nodes::inline_shader_node_tree(*ntree, *localtree, inline_params);
    }

    std::string callback_inline_failure_helper;
    bool fatal_inline_error = false;
    if (track_uniforms) {
      mat->uniform_tracking_supported = inline_success &&
                                        inline_params.r_error_messages.is_empty() &&
                                        !mat->uniform_tracking->unsupported_context &&
                                        uniform_tracking_animation_safe(
                                            *ma, mat->animation_state->as_span(),
                                            *mat->uniform_tracking);
      /* These consumers duplicate closure branches during preprocessing. Their copied sockets
       * do not carry provenance, so a partially tracked branch must never be updated in place. */
      for (const bNode &node : localtree->nodes) {
        if (node.is_type("ShaderNodeShaderToRGB"_ustr) ||
            node.is_type("ShaderNodeNPRSurfaceDiffusion"_ustr))
        {
          mat->uniform_tracking_supported = false;
        }
      }
      mat->uniform_unsafe_sources = mat->uniform_tracking->unsafe_sources;
    }
    for (nodes::InlineShaderNodeTreeParams::ErrorMessage &error : inline_params.r_error_messages) {
      fatal_inline_error |= error.fatal;
      if (error.fatal && mat->inline_node_error.empty()) {
        mat->inline_node_error = error.message;
      }
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
      if (!error.fatal || !gpu_material_bundle_error_record(mat, *error.node, error.message)) {
        result.errors.append({error.node, std::move(error.message)});
      }
    }

    if (compile_surface_graph) {
      gpu_material_validate_stencil_dependencies(*mat, *localtree);
      {
        GPUMaterialProfileTimer profile(GPUMaterialProfileStage::Preprocess);
        ntreeGPUMaterialNodesPreprocess(localtree, mat);
      }
      if (cache_surface && !mat->uniform_tracking_supported && inline_success &&
          result.errors.is_empty() && mat->inline_node_error.empty() && gpu_material_can_cache_surface_tree(*localtree))
      {
        mat->surface_source_tree = ntree;
        mat->surface_preprocessed_tree = localtree;
        owns_localtree = false;
      }
      GPUMaterialProfileTimer profile(GPUMaterialProfileStage::Execute);
      ntreeGPUMaterialNodes(localtree, mat, true);
    }
    if (compile_light_shader_graph) {
      ntreeGPULightShaderNodes(localtree, mat);
    }
    if (!fatal_inline_error && !inline_success && !callback_inline_failure_helper.empty()) {
      gpu_material_force_glsl_closure_callback_inline_error(
          mat, callback_inline_failure_helper);
    }
  }
  if (compile_npr_graph) {
    if (force_npr_graph || npr_tree_get(ntree) != nullptr) {
      GPU_material_flag_set(mat, GPU_MATFLAG_NPR);
    }
    if (GPU_material_flag_get(mat, GPU_MATFLAG_NPR)) {
      GPUMaterialProfileTimer profile(GPUMaterialProfileStage::NPR);
      nodes::InlineShaderNodeTreeParams inline_params;
      npr_localtree = ntreeGPUNPRNodes(ntree, mat, inline_params);
      for (auto &error : inline_params.r_error_messages) {
        if (error.fatal && mat->inline_node_error.empty()) {
          mat->inline_node_error = error.message;
        }
        if (!error.fatal || !gpu_material_bundle_error_record(mat, *error.node, error.message)) {
          result.errors.append({error.node, std::move(error.message)});
        }
      }
    }
  }

  mat->expression_root = nullptr;
  {
    GPUMaterialProfileTimer profile(GPUMaterialProfileStage::Textures);
    gpu_material_ramp_texture_build(mat);
    gpu_material_sky_texture_build(mat);
  }

  /* Use default material pass when possible. */
  if (!mat->inline_node_error.empty()) {
    /* A valid fallback pass must not hide an invalid authored Bundle interface. */
    mat->pass = nullptr;
  }
  else if (GPUPass *default_pass = pass_replacement_cb ? pass_replacement_cb(thunk, mat) : nullptr) {
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
  mat->uniform_tracking.reset();
  mat->uniform_tracking_tree = nullptr;
  mat->uniform_origins.clear();
  /* Only free after GPU_pass_shader_get where gpu::UniformBuf read data from the local
   * tree. */
  if (localtree != nullptr && owns_localtree) {
    GPUMaterialProfileTimer profile(GPUMaterialProfileStage::Cleanup);
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

  gpu_material_stencil_errors_append(*mat, *ntree, result);
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

int GPU_material_uniform_source_register(GPUMaterial *material, const bNodeSocket *socket)
{
  if (!material || !material->uniform_tracking || !material->uniform_tracking_supported ||
      &socket->owner_tree() != material->uniform_tracking_tree)
  {
    return 0;
  }
  const nodes::ShaderUniformTarget *target = material->uniform_tracking->sources.lookup_ptr(
      socket);
  if (!target) {
    return 0;
  }
  const bNode &node = socket->owner_node();
  if (node.identifier != target->node_identifier || node.name != target->node_name ||
      socket->identifier != target->socket_identifier || socket->in_out != target->in_out ||
      socket->type != target->socket_type)
  {
    /* Preprocessing may delete/replace a socket and reuse its address. */
    return 0;
  }
  for (const int i : material->uniform_origins.index_range()) {
    if (material->uniform_origins[i] == target->source) {
      return i + 1;
    }
  }
  material->uniform_origins.append(target->source);
  return material->uniform_origins.size();
}

void GPU_material_uniform_capture_input(GPUMaterial *material,
                                        const GPUInput &input,
                                        size_t byte_offset)
{
  if (!input.uniform_source_index || !material->uniform_tracking_supported) {
    return;
  }
  const auto &source = material->uniform_origins[input.uniform_source_index - 1];
  if (int(input.type) != source.components || !ELEM(input.type, GPU_FLOAT, GPU_VEC3, GPU_VEC4)) {
    material->uniform_unsafe_sources.append(source);
    return;
  }
  material->uniform_bindings.append({source, byte_offset, int(input.type)});
}

void GPU_material_uniform_capture_data(GPUMaterial *material, const void *data, size_t size)
{
  if (!material->uniform_bindings.is_empty()) {
    material->uniform_cpu_data.resize(size);
    memcpy(material->uniform_cpu_data.data(), data, size);
  }
}

static uint32_t uniform_tree_uid(const bNodeTree &tree)
{
  return tree.id.orig_id ? tree.id.orig_id->session_uid : tree.id.session_uid;
}

static const bNodeSocket *uniform_source_resolve(const Material &material,
                                                 const nodes::ShaderUniformSource &source)
{
  const bNodeTree *tree = material.nodetree;
  if (!tree) {
    return nullptr;
  }
  for (const auto &step : source.group_path) {
    if (uniform_tree_uid(*tree) != step.parent_tree_uid) {
      return nullptr;
    }
    const bNode *group = tree->node_by_id(step.group_node_identifier);
    if (!group || !group->is_group() || !group->id) {
      return nullptr;
    }
    tree = reinterpret_cast<const bNodeTree *>(group->id);
    if (uniform_tree_uid(*tree) != step.child_tree_uid) {
      return nullptr;
    }
  }
  if (uniform_tree_uid(*tree) != source.tree_uid) {
    return nullptr;
  }
  const bNode *node = tree->node_by_id(source.node_identifier);
  if (!node) {
    return nullptr;
  }
  const auto &sockets = source.in_out == SOCK_IN ? node->inputs : node->outputs;
  for (const bNodeSocket &socket : sockets) {
    if (socket.identifier == source.socket_identifier && socket.type == source.socket_type) {
      return &socket;
    }
  }
  return nullptr;
}

struct AnimatedUniformChange {
  uint32_t tree_uid;
  const bNodeSocket *socket;
  int component;
  uint32_t bits;
};

static bool uniform_change_matches(const AnimatedUniformChange &change,
                                   const nodes::ShaderUniformSource &source)
{
  return change.tree_uid == source.tree_uid &&
         change.socket->owner_node().identifier == source.node_identifier &&
         change.socket->identifier == source.socket_identifier &&
         change.socket->in_out == source.in_out && change.socket->type == source.socket_type;
}

/* The existing exact snapshot includes every supported Action curve and its full RNA path.
 * Only the final value word of a curve may differ; every identity word must still match. */
static bool animation_uniform_changes(const Material &material,
                                      Span<double> old_state,
                                      Span<double> state,
                                      Vector<AnimatedUniformChange> &changes,
                                      bool include_unchanged = false)
{
  if (old_state.size() != state.size() || !material.nodetree) {
    return false;
  }
  Map<uint32_t, const bNodeTree *> trees;
  StructRNA *socket_rna = RNA_struct_find("NodeSocket");
  if (!socket_rna) {
    return false;
  }
  const auto visit = [&](const auto &self, const bNodeTree &tree) -> void {
    if (trees.contains(uniform_tree_uid(tree))) {
      return;
    }
    trees.add_new(uniform_tree_uid(tree), &tree);
    /* Copy-on-evaluation replaces socket runtimes. Rebuild owner links before resolving
     * stable source identities; never consult an owner pointer from a previous evaluation. */
    tree.ensure_topology_cache();
    for (const bNode &node : tree.nodes) {
      if (node.id && GS(node.id->name) == ID_NT) {
        self(self, *reinterpret_cast<const bNodeTree *>(node.id));
      }
    }
  };
  visit(visit, *material.nodetree);
  int64_t index = 0;
  const auto same = [&]() {
    if (index >= state.size() || state[index] != old_state[index]) {
      return false;
    }
    index++;
    return true;
  };
  while (index < state.size()) {
    const uint32_t uid = uint32_t(state[index]);
    if (!same() || !same() || !same() || index >= state.size()) {
      return false;
    }
    const int count = int(state[index]);
    if (!same() || count < 0 || count > state.size() - index) {
      return false;
    }
    const bNodeTree *tree = trees.lookup_default(uid, nullptr);
    if (!tree) {
      return false;
    }
    for (int curve = 0; curve < count; curve++) {
      if (index >= state.size()) {
        return false;
      }
      const int length = int(state[index]);
      if (!same() || length < 0 || length > state.size() - index - 2) {
        return false;
      }
      std::string path;
      path.reserve(length);
      for (int c = 0; c < length; c++) {
        path.push_back(char(uint8_t(state[index])));
        if (!same()) {
          return false;
        }
      }
      const int component = int(state[index]);
      if (!same() || index >= state.size()) {
        return false;
      }
      const uint32_t bits = uint32_t(state[index]);
      if (include_unchanged || bits != uint32_t(old_state[index])) {
        PointerRNA owner = RNA_id_pointer_create(const_cast<ID *>(&tree->id));
        PointerRNA ptr;
        PropertyRNA *property = nullptr;
        if (!RNA_path_resolve_property(&owner, path.c_str(), &ptr, &property) ||
            !RNA_struct_is_a(ptr.type, socket_rna))
        {
          return false;
        }
        changes.append({uid, static_cast<const bNodeSocket *>(ptr.data), component, bits});
      }
      index++;
    }
  }
  return !changes.is_empty();
}

static bool uniform_tracking_animation_safe(const Material &material,
                                            Span<double> state,
                                            const nodes::ShaderUniformTracking &tracking)
{
  if (tracking.unsafe_sources.is_empty()) {
    return true;
  }
  Vector<AnimatedUniformChange> animated;
  if (!animation_uniform_changes(material, state, state, animated, true)) {
    return false;
  }
  for (const auto &source : tracking.unsafe_sources) {
    for (const auto &value : animated) {
      if (uniform_change_matches(value, source)) {
        return false;
      }
    }
  }
  return true;
}

bool GPU_material_animation_uniform_update(Material *material, Span<double> state)
{
  if (!material || material->gpumaterial.is_empty()) {
    return false;
  }
  GPUMaterial *first = static_cast<GPUMaterial *>(
      static_cast<LinkData *>(material->gpumaterial.first)->data);
  if (!first->uniform_tracking_supported || !first->animation_state_valid ||
      !first->animation_state)
  {
    return false;
  }
  Vector<AnimatedUniformChange> changes;
  if (!animation_uniform_changes(*material, first->animation_state->as_span(), state, changes)) {
    return false;
  }
  Vector<bool> covered(changes.size(), false);
  struct Pending {
    GPUMaterial *material;
    Vector<uint8_t> data;
  };
  Vector<Pending> pending;
  /* Validate every pass before changing any shadow UBO. No GPU calls in this evaluation path. */
  for (LinkData &link : material->gpumaterial) {
    GPUMaterial *mat = static_cast<GPUMaterial *>(link.data);
    if (!mat->uniform_tracking_supported || !mat->animation_state_valid || !mat->animation_state ||
        mat->animation_state->as_span() != first->animation_state->as_span() ||
        mat->engine != GPU_MAT_EEVEE || GPU_material_status(mat) != GPU_MAT_SUCCESS ||
        mat->optimized_pass || mat->uses_referenced_object_data ||
        !mat->generated_sources.is_empty())
    {
      return false;
    }
    for (const auto &unsafe : mat->uniform_unsafe_sources) {
      for (const auto &change : changes) {
        if (uniform_change_matches(change, unsafe)) {
          return false;
        }
      }
    }
    Pending item{mat, mat->uniform_cpu_data};
    bool changed = false;
    for (const MaterialUniformBinding &binding : mat->uniform_bindings) {
      const bNodeSocket *socket = uniform_source_resolve(*material, binding.source);
      if (!socket || !mat->ubo ||
          binding.byte_offset + binding.components * sizeof(float) > item.data.size())
      {
        return false;
      }
      for (const int i : changes.index_range()) {
        const auto &change = changes[i];
        if (uniform_change_matches(change, binding.source)) {
          if (change.component < 0 || change.component >= binding.components ||
              socket != change.socket)
          {
            return false;
          }
          memcpy(item.data.data() + binding.byte_offset + change.component * sizeof(float),
                 &change.bits,
                 sizeof(float));
          covered[i] = true;
          changed = true;
        }
      }
    }
    if (changed) {
      pending.append(std::move(item));
    }
  }
  for (const bool found : covered) {
    if (!found) {
      return false;
    }
  }
  auto new_state = std::make_shared<Vector<double>>(state);
  for (Pending &item : pending) {
    item.material->uniform_cpu_data = std::move(item.data);
    item.material->uniform_update_pending = true;
  }
  for (LinkData &link : material->gpumaterial) {
    GPUMaterial *mat = static_cast<GPUMaterial *>(link.data);
    mat->animation_state = new_state;
    /* Never retain evaluated node pointers through another depsgraph evaluation. */
    if (mat->surface_preprocessed_tree) {
      BKE_id_free(nullptr, &mat->surface_preprocessed_tree->id);
      mat->surface_preprocessed_tree = nullptr;
      mat->surface_source_tree = nullptr;
    }
  }
  return true;
}

bool GPU_material_animation_state_matches(ListBaseT<LinkData> *gpumaterial,
                                          Span<double> state,
                                          int *r_reject_reason)
{
  const auto reject = [&](const int reason) {
    if (r_reject_reason) {
      *r_reject_reason = reason;
    }
    return false;
  };
  if (gpumaterial->is_empty()) {
    return reject(13);
  }
  for (LinkData &link : *gpumaterial) {
    GPUMaterial *mat = static_cast<GPUMaterial *>(link.data);
    if (!mat->animation_state_valid || !mat->animation_state) {
      return reject(2);
    }
    if (mat->engine != GPU_MAT_EEVEE || GPU_material_status(mat) != GPU_MAT_SUCCESS) {
      return reject(12);
    }
    if (mat->uses_referenced_object_data) {
      return reject(11);
    }
    if (!mat->generated_sources.is_empty()) {
      return reject(10);
    }
    if (mat->animation_state->as_span() != state) {
      return reject(9);
    }
  }
  /* Evaluated node trees may have been replaced during animation evaluation. Keep GPU data,
   * but discard preprocessing trees holding references to the previous evaluated nodes. */
  for (LinkData &link : *gpumaterial) {
    GPUMaterial *mat = static_cast<GPUMaterial *>(link.data);
    if (mat->surface_preprocessed_tree) {
      BKE_id_free(nullptr, &mat->surface_preprocessed_tree->id);
      mat->surface_preprocessed_tree = nullptr;
      mat->surface_source_tree = nullptr;
    }
  }
  return true;
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

bool GPU_material_is_outline_shell(const GPUMaterial *material)
{
  return material->is_outline_shell;
}

bool GPU_material_outline_shell_has_alpha(const GPUMaterial *material)
{
  return material->outline_shell_has_alpha;
}

void GPU_material_outline_shell_alpha_set(GPUMaterial *material, bool has_alpha)
{
  material->outline_shell_has_alpha = has_alpha;
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
  if (!mat->stencil_value_error.empty() || !mat->inline_node_error.empty()) {
    return GPU_MAT_FAILED;
  }
  switch (GPU_pass_status(mat->pass)) {
    case GPU_PASS_SUCCESS:
      return GPU_MAT_SUCCESS;
    case GPU_PASS_QUEUED:
      return GPU_MAT_QUEUED;
    default:
      return GPU_MAT_FAILED;
  }
}

std::string GPU_material_error_message(GPUMaterial *material)
{
  if (!material->inline_node_error.empty()) {
    return material->inline_node_error;
  }
  if (!material->stencil_value_error.empty()) {
    return material->stencil_value_error;
  }
  return GPU_pass_error_message(material->pass);
}

void GPU_material_expression_source_add(GPUMaterial *material,
                                        const bNode &node,
                                        gpu::ShaderDiagnosticSource source,
                                        std::string signature)
{
  auto context = material->expression_context.lock();
  if (!context && material->expression_root) {
    context = bke::shader_node_compile_context_get(*material->expression_root);
    material->expression_context = context;
  }
  if (!context) {
    return;
  }
  const bNode &original = node.runtime->original ? *node.runtime->original : node;
  const bNodeTree &owner = original.owner_tree();
  bNodeTree &tree = owner.id.orig_id ? *reinterpret_cast<bNodeTree *>(owner.id.orig_id) :
                                       const_cast<bNodeTree &>(owner);
  {
    std::lock_guard lock(context->mutex);
    if (!context->valid) {
      return;
    }
    const uint64_t origin_key = (uint64_t(tree.id.session_uid) << 32) |
                                uint32_t(original.identifier);
    context->signatures.add_overwrite(origin_key, signature);
  }
  bke::shader_node_compile_reference_add(tree, original.identifier, context);
  material->expression_sources.push_back(std::move(source));
  material->expression_origins.append(
      {tree.id.session_uid, original.identifier, std::move(signature)});
}

const std::vector<gpu::ShaderDiagnosticSource> &GPU_material_expression_diagnostic_sources(
    const GPUMaterial *material)
{
  return material->expression_sources;
}

void GPU_material_expression_diagnostics_publish(GPUMaterial *material,
                                                 const StringRef context_key,
                                                 const std::shared_ptr<const void> &context_owner,
                                                 const uint64_t sync_epoch)
{
  auto context = material->expression_context.lock();
  if (!context) {
    return;
  }
  /* Copy under the pass lock, release it before taking the root revision lock. */
  const auto errors = material->pass ? GPU_pass_expression_errors(material->pass) :
                                      std::vector<gpu::ShaderDiagnostic>{};
  Vector<bke::ShaderNodeCompileContext::Message> messages = material->inline_diagnostics;
  if (GPU_material_status(material) == GPU_MAT_FAILED) {
    for (const auto &error : errors) {
      for (size_t i = 0; i < material->expression_sources.size(); i++) {
        if (material->expression_sources[i].function_name == error.function_name) {
          const auto &origin = material->expression_origins[i];
          const auto &source = material->expression_sources[i];
          const std::string message = source.constructor.empty() && !source.evaluation_target.empty() ?
              source.evaluation_target + ": " + error.message : error.message;
          messages.append({origin.tree_uid, origin.node_id, origin.signature, message});
        }
      }
    }
  }
  const std::string variant = std::to_string(int(material->engine)) + "/" +
                              std::to_string(material->uuid);
  std::lock_guard lock(context->mutex);
  if (!context->valid) {
    return;
  }
  messages.remove_if([&](const auto &message) {
    const uint64_t origin_key = (uint64_t(message.tree_uid) << 32) | uint32_t(message.node_id);
    const std::string *signature = context->signatures.lookup_ptr(origin_key);
    return signature == nullptr || *signature != message.signature;
  });
  context->snapshots.remove_if([](const auto &item) { return item.value.owner.expired(); });
  auto &snapshot = context->snapshots.lookup_or_add_default(std::string(context_key));
  if (sync_epoch < snapshot.sync_epoch) {
    return;
  }
  if (sync_epoch != snapshot.sync_epoch) {
    snapshot.variants.clear();
    snapshot.sync_epoch = sync_epoch;
  }
  snapshot.owner = context_owner;
  snapshot.variants.add_overwrite(variant, std::move(messages));
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

void GPU_material_stencil_value_used_set(GPUMaterial *mat)
{
  mat->uses_stencil_value = true;
}

void GPU_material_render_texture_register(GPUMaterial *mat, const int uid)
{
  mat->render_texture_ids.append_non_duplicates(uid);
}

Span<int> GPU_material_render_texture_ids(const GPUMaterial *mat)
{
  return mat->render_texture_ids;
}

bool GPU_material_stencil_value_used(const GPUMaterial *mat)
{
  return mat != nullptr && mat->uses_stencil_value;
}

void GPU_material_stencil_value_readable_set(GPUMaterial *mat)
{
  mat->reads_stencil_value = true;
}

bool GPU_material_stencil_value_readable(const GPUMaterial *mat)
{
  return mat != nullptr && mat->reads_stencil_value;
}

StringRef GPU_material_stencil_marker_error(const GPUMaterial *mat)
{
  return mat ? StringRef(mat->stencil_marker_error) : StringRef();
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

bool GPU_material_has_tessellation_output(GPUMaterial *mat)
{
  return mat->has_tessellation_output;
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

bool GPU_material_has_node_soft_shadow(const GPUMaterial *mat)
{
  return mat != nullptr && mat->has_node_soft_shadow;
}

bool GPU_material_has_node_shadow_cache(const GPUMaterial *mat)
{
  return mat != nullptr && mat->node_shadow_cache_quality != 0;
}

int GPU_material_node_shadow_cache_quality(const GPUMaterial *mat)
{
  return mat != nullptr ? mat->node_shadow_cache_quality : 0;
}

bool GPU_material_principled_npr_v2_has(const GPUMaterial *mat)
{
  return mat != nullptr && mat->has_principled_npr_v2;
}

bool GPU_material_surface_diffusion_has(const GPUMaterial *mat)
{
  return mat != nullptr && mat->has_surface_diffusion;
}

void GPU_material_surface_diffusion_set(GPUMaterial *mat)
{
  mat->has_surface_diffusion = true;
}

eGPUMaterialNPRFeature GPU_material_npr_features_get(const GPUMaterial *mat)
{
  return mat != nullptr ? mat->npr_features : GPU_MAT_NPR_FEATURE_NONE;
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

uint64_t GPU_light_shader_parameter_key(const char *name)
{
  /* Stable FNV-1a key. Draw Manager checks full names for collisions before upload. */
  uint64_t key = 14695981039346656037ull;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(name); *p; p++) {
    key = (key ^ *p) * 1099511628211ull;
  }
  return key;
}

uint64_t GPU_material_light_shader_parameter_ensure(GPUMaterial *material,
                                                   const char *name,
                                                   Object *object)
{
  GPULightShaderParameterRequest request;
  STRNCPY(request.name, name);
  request.key = GPU_light_shader_parameter_key(request.name);
  if (!material) {
    return request.key;
  }
  material->uses_referenced_object_data = true;
  if (object) {
    request.object_uid = GPU_material_referenced_object_ensure(
        material, object, GPU_REFERENCED_OBJECT_DATA_LIGHT);
    if (request.object_uid == 0) {
      return request.key;
    }
  }
  for (const auto &existing : material->light_shader_parameters) {
    if (existing.object_uid == request.object_uid && STREQ(existing.name, request.name)) {
      return request.key;
    }
  }
  material->light_shader_parameters.append(request);
  return request.key;
}

Span<GPULightShaderParameterRequest> GPU_material_light_shader_parameters(const GPUMaterial *material)
{
  return material ? material->light_shader_parameters.as_span() : Span<GPULightShaderParameterRequest>();
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
  material->ubo = GPU_uniformbuf_create_from_list(inputs, material->name.c_str(), material);
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

void GPU_material_output_tessellation(GPUMaterial *material, GPUNodeLink *link)
{
  if (!material->graph.outlink_tessellation) {
    material->graph.outlink_tessellation = link;
    material->has_tessellation_output = true;
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

void GPU_material_node_soft_shadow_set(GPUMaterial *material)
{
  if (material != nullptr) {
    material->has_node_soft_shadow = true;
  }
}

void GPU_material_node_shadow_cache_set(GPUMaterial *material, int quality)
{
  if (material != nullptr) {
    material->node_shadow_cache_quality = material->node_shadow_cache_quality == 0 ?
                                              quality :
                                              std::min(material->node_shadow_cache_quality,
                                                       quality);
  }
}

void GPU_material_principled_npr_v2_set(GPUMaterial *material)
{
  if (material != nullptr) {
    material->has_principled_npr_v2 = true;
  }
}

void GPU_material_npr_features_add(GPUMaterial *material, eGPUMaterialNPRFeature features)
{
  if (material != nullptr) {
    material->npr_features |= features;
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
