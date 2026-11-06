/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "BLI_assert.hh"
#include "BLI_enum_flags.hh"
#include "BLI_math_base_c.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"

#include "DNA_customdata_types.h" /* for eCustomDataType */
#include "DNA_image_types.h"
#include "DNA_listBase.h"

#include "GPU_shader.hh"  /* for GPUShaderCreateInfo */
#include "GPU_texture.hh" /* for GPUSamplerState */

namespace blender {

struct GHash;
struct GPUMaterial;
struct GPUInput;
struct GPUNodeLink;
struct GPUNodeStack;
struct GPUPass;
struct Material;
namespace gpu {
class Texture;
class UniformBuf;
}  // namespace gpu
struct Image;
struct ImageUser;
struct Main;
struct Material;
struct Object;
struct Scene;
struct bNode;
struct bNodeSocket;
struct bNodeTree;

/**
 * High level functions to create and use GPU materials.
 */

enum eGPUMaterialEngine {
  GPU_MAT_EEVEE,
  GPU_MAT_COMPOSITOR,
  GPU_MAT_ENGINE_MAX,
};

enum GPUMaterialStatus {
  GPU_MAT_FAILED = 0,
  GPU_MAT_QUEUED,
  GPU_MAT_SUCCESS,
};

/** Data lanes requested from an evaluated object by a material node. */
enum eGPUReferencedObjectDataFlag : uint32_t {
  GPU_REFERENCED_OBJECT_DATA_NONE = 0,
  GPU_REFERENCED_OBJECT_DATA_TRANSFORM = (1u << 0),
  GPU_REFERENCED_OBJECT_DATA_COLOR = (1u << 1),
  GPU_REFERENCED_OBJECT_DATA_VISIBILITY = (1u << 2),
  GPU_REFERENCED_OBJECT_DATA_TYPE = (1u << 3),
  GPU_REFERENCED_OBJECT_DATA_LIGHT = (1u << 4),
};
ENUM_OPERATORS(eGPUReferencedObjectDataFlag);

/** Original object identity retained by a GPUMaterial until Draw Manager sync. */
struct GPUReferencedObject {
  Object *object = nullptr;
  uint32_t session_uid = 0;
  eGPUReferencedObjectDataFlag flags = GPU_REFERENCED_OBJECT_DATA_NONE;
};

struct GPULightShaderParameterRequest {
  char name[64] = "";
  uint64_t key = 0;
  /* Zero requests the parameter for all lights (GLSL light iteration). */
  uint32_t object_uid = 0;
};

uint64_t GPU_light_shader_parameter_key(const char *name);
uint64_t GPU_material_light_shader_parameter_ensure(GPUMaterial *material,
                                                  const char *name,
                                                  Object *object = nullptr);
Span<GPULightShaderParameterRequest> GPU_material_light_shader_parameters(const GPUMaterial *material);

/* GPU_MAT_OPTIMIZATION_SKIP for cases where we do not
 * plan to perform optimization on a given material. */
enum eGPUMaterialOptimizationStatus {
  GPU_MAT_OPTIMIZATION_SKIP = 0,
  GPU_MAT_OPTIMIZATION_QUEUED,
  GPU_MAT_OPTIMIZATION_SUCCESS,
};

enum eGPUMaterialFlag : uint64_t {
  GPU_MATFLAG_DIFFUSE = (1 << 0),
  GPU_MATFLAG_SUBSURFACE = (1 << 1),
  GPU_MATFLAG_GLOSSY = (1 << 2),
  GPU_MATFLAG_REFRACT = (1 << 3),
  GPU_MATFLAG_EMISSION = (1 << 4),
  GPU_MATFLAG_TRANSPARENT = (1 << 5),
  GPU_MATFLAG_HOLDOUT = (1 << 6),
  GPU_MATFLAG_SHADER_TO_RGBA = (1 << 7),
  GPU_MATFLAG_AO = (1 << 8),
  /* Signals the presence of multiple reflection closures. */
  GPU_MATFLAG_COAT = (1 << 9),
  GPU_MATFLAG_TRANSLUCENT = (1 << 10),
  GPU_MATFLAG_RAYCAST = (1 << 11),
  GPU_MATFLAG_NPR = (1 << 12),
  GPU_MATFLAG_SHADER_INFO = (1 << 13),
  GPU_MATFLAG_NPR_REFRACTION = (1 << 14),
  GPU_MATFLAG_NPR_FOREACH_LIGHT = (1 << 15),
  GPU_MATFLAG_LIGHTING = (1ull << 32),
  GPU_MATFLAG_LIGHT_ATTRIBUTE = (1ull << 33),
  GPU_MATFLAG_SHADOW_OFFSET = (1ull << 34),

  GPU_MATFLAG_VOLUME_SCATTER = (1 << 16),
  GPU_MATFLAG_VOLUME_ABSORPTION = (1 << 17),

  GPU_MATFLAG_OBJECT_INFO = (1 << 18),
  GPU_MATFLAG_AOV = (1 << 19),

  GPU_MATFLAG_BARYCENTRIC = (1 << 20),
  /* Signals that these specific closures might *not* be colorless.
   * If this flag is not set, all closures are ensured to not be tinted. */
  GPU_MATFLAG_REFLECTION_MAYBE_COLORED = (1 << 21),
  GPU_MATFLAG_REFRACTION_MAYBE_COLORED = (1 << 22),
  GPU_MATFLAG_TRANSPARENT_MAYBE_COLORED = (1 << 23),

  /* Set if the material uses the "Is Diffuse / Glossy Ray" output of the light path node. */
  GPU_MATFLAG_IS_DIFFUSE_OR_GLOSSY_RAY_FLAG = (1 << 24),
  GPU_MATFLAG_RENDER_TEXTURE = (1 << 25),
  GPU_MATFLAG_FILTER_MATERIAL = (1 << 26),
  GPU_MATFLAG_SCENE_COLOR = (1 << 27),
  GPU_MATFLAG_SCREENSPACE_INFO = (1 << 28),

  /* Tells the render engine the material was just compiled or updated. */
  GPU_MATFLAG_UPDATED = (1 << 29),
  /* Signals the material needs Eevee light-probe resources without enabling full lighting. */
  GPU_MATFLAG_LIGHTPROBE_ACCESS = (1 << 30),
  /* Signals the material needs Eevee direct-light resources for GLSL Function helper access. */
  GPU_MATFLAG_GLSL_LIGHT_ACCESS = (1u << 31),
};
ENUM_OPERATORS(eGPUMaterialFlag);

/** Principled NPR code needed by the consumed nodes in one material. */
enum eGPUMaterialNPRFeature : uint32_t {
  GPU_MAT_NPR_FEATURE_NONE = 0,
  GPU_MAT_NPR_FINITE_HIGHLIGHT = (1 << 0),
  GPU_MAT_NPR_REFERENCE_HIGHLIGHT = (1 << 1),
  GPU_MAT_NPR_SHADOW_STABLE = (1 << 2),
  GPU_MAT_NPR_SHADOW_TEMPORAL = (1 << 3),
  GPU_MAT_NPR_SHADOW_SOFT = (1 << 4),
  GPU_MAT_NPR_SHARED_ENERGY = (1 << 5),
  GPU_MAT_NPR_RIM_DEPTH = (1 << 6),
  GPU_MAT_NPR_MAP_PER_LIGHT = (1 << 8),
  GPU_MAT_NPR_MAP_COMBINED = (1 << 9),
  GPU_MAT_NPR_MAP_TOTAL = (1 << 10),
  GPU_MAT_NPR_DRIVEN_RAMP = (1 << 11),
};
ENUM_OPERATORS(eGPUMaterialNPRFeature);

enum eGPUCustomNodeDependencyFlag {
  GPU_CUSTOM_NODE_DEPENDENCY_NONE = 0,
  GPU_CUSTOM_NODE_DEPENDENCY_GLSL_GEOMETRY_HELPERS = (1 << 0),
  GPU_CUSTOM_NODE_DEPENDENCY_GLSL_LIGHTPROBE_HELPERS = (1 << 1),
  GPU_CUSTOM_NODE_DEPENDENCY_GLSL_MATRIX_HELPERS = (1 << 2),
  GPU_CUSTOM_NODE_DEPENDENCY_SHADING_CONTEXT = (1 << 3),
};
ENUM_OPERATORS(eGPUCustomNodeDependencyFlag);

inline constexpr const char *GPU_GLSL_FUNCTION_GEOMETRY_HELPER_FILENAME =
    "__glsl_function_geometry_helpers.glsl";
inline constexpr const char *GPU_GLSL_FUNCTION_LIGHTPROBE_HELPER_FILENAME =
    "__glsl_function_lightprobe_helpers.glsl";
inline constexpr const char *GPU_GLSL_FUNCTION_MATRIX_HELPER_FILENAME =
    "__glsl_function_matrix_helpers.glsl";

using GPUCodegenCallbackFn = void (*)(void *thunk,
                                      GPUMaterial *mat,
                                      struct GPUCodegenOutput *codegen);
/**
 * Should return an already compiled pass if it's functionally equivalent to the one being
 * compiled.
 */
using GPUMaterialPassReplacementCallbackFn = GPUPass *(*)(void *thunk, GPUMaterial *mat);

struct GPUMaterialFromNodeTreeResult {
  GPUMaterial *material = nullptr;

  /** Compatible with #NodeWarningType. */
  enum class WarningType {
    Error = 0,
    Warning = 1,
    Info = 2,
  };

  struct Error {
    const bNode *node;
    std::string message;
    WarningType type;
  };
  Vector<Error> errors;
};

std::string GPU_material_error_message(GPUMaterial *material);
void GPU_material_expression_source_add(GPUMaterial *material,
                                        const bNode &node,
                                        gpu::ShaderDiagnosticSource source,
                                        std::string signature);
const std::vector<gpu::ShaderDiagnosticSource> &GPU_material_expression_diagnostic_sources(
    const GPUMaterial *material);
void GPU_material_expression_diagnostics_publish(GPUMaterial *material,
                                                 StringRef context_key,
                                                 const std::shared_ptr<const void> &context_owner,
                                                 uint64_t sync_epoch);

/** WARNING: gpumaterials thread safety must be ensured by the caller. */
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
    GPUMaterialPassReplacementCallbackFn pass_replacement_cb = nullptr,
    bool outline_shell_root = false);

/* A callback passed to GPU_material_from_callbacks to construct the material graph by adding and
 * linking the necessary GPU material nodes. */
using ConstructGPUMaterialFn = void (*)(void *thunk, GPUMaterial *material);

/* Construct a GPU material from a set of callbacks. See the callback types for more information.
 * The given thunk will be passed as the first parameter of each callback. The UUID identify a
 * possible variation for the same graph, for instance, a shader whose output is half precision or
 * full precision. */
GPUMaterial *GPU_material_from_callbacks(eGPUMaterialEngine engine,
                                         ConstructGPUMaterialFn construct_function_cb,
                                         GPUCodegenCallbackFn generate_code_function_cb,
                                         void *thunk,
                                         const uint64_t uuid);

void GPU_material_free_single(GPUMaterial *material);
void GPU_material_free(ListBaseT<LinkData> *gpumaterial);
bool GPU_material_animation_state_matches(ListBaseT<LinkData> *gpumaterial,
                                          Span<double> state,
                                          int *r_reject_reason = nullptr);
/** Queue validated CPU-only uniform changes; actual upload occurs during material sync. */
bool GPU_material_animation_uniform_update(Material *material, Span<double> state);
int GPU_material_uniform_source_register(GPUMaterial *material, const bNodeSocket *socket);
void GPU_material_uniform_capture_input(GPUMaterial *material,
                                        const GPUInput &input,
                                        size_t byte_offset);
void GPU_material_uniform_capture_data(GPUMaterial *material, const void *data, size_t size);

void GPU_materials_free(Main *bmain);

GPUPass *GPU_material_get_pass(GPUMaterial *material);
/** Return the most optimal shader configuration for the given material. */
gpu::Shader *GPU_material_get_shader(GPUMaterial *material);

const char *GPU_material_get_name(GPUMaterial *material);

/**
 * Return can be null if the GPU material was not compiled from a Material ID.
 */
Material *GPU_material_get_material(GPUMaterial *material);
bool GPU_material_is_world(const GPUMaterial *material);
bool GPU_material_is_outline_shell(const GPUMaterial *material);
/* Root Alpha only, not transparency used inside a Shader to RGB color branch. */
bool GPU_material_outline_shell_has_alpha(const GPUMaterial *material);
void GPU_material_outline_shell_alpha_set(GPUMaterial *material, bool has_alpha);
/**
 * Return true if the material compilation has not yet begin or begin.
 */
GPUMaterialStatus GPU_material_status(GPUMaterial *mat);

/**
 * Return status for asynchronous optimization jobs.
 */
eGPUMaterialOptimizationStatus GPU_material_optimization_status(GPUMaterial *mat);

uint64_t GPU_material_compilation_timestamp(GPUMaterial *mat);
double GPU_material_compilation_time(GPUMaterial *mat);
int GPU_material_recompile_serial_get(const GPUMaterial *mat);
void GPU_material_recompile_serial_increment(Material *material);
void GPU_material_recompile_serial_clear(const Material *material);

gpu::UniformBuf *GPU_material_uniform_buffer_get(GPUMaterial *material);
/**
 * Create dynamic UBO from parameters
 *
 * \param inputs: Items are #LinkData, data is #GPUInput (`BLI_genericNodeN(GPUInput)`).
 */
void GPU_material_uniform_buffer_create(GPUMaterial *material, ListBaseT<LinkData> *inputs);

bool GPU_material_has_surface_output(GPUMaterial *mat);
/* Separate metadata: eGPUMaterialFlag already occupies all 32 bits. */
void GPU_material_planar_probe_used_set(GPUMaterial *mat, bool used = true);
bool GPU_material_planar_probe_used(const GPUMaterial *mat);
void GPU_material_stencil_value_used_set(GPUMaterial *mat);
bool GPU_material_stencil_value_used(const GPUMaterial *mat);
void GPU_material_render_texture_register(GPUMaterial *mat, int uid);
Span<int> GPU_material_render_texture_ids(const GPUMaterial *mat);
void GPU_material_stencil_value_readable_set(GPUMaterial *mat);
bool GPU_material_stencil_value_readable(const GPUMaterial *mat);
void GPU_material_stencil_surface_used_set(GPUMaterial *mat);
bool GPU_material_stencil_surface_used(const GPUMaterial *mat);
void GPU_material_stencil_surface_readable_set(GPUMaterial *mat);
bool GPU_material_stencil_surface_readable(const GPUMaterial *mat);
StringRef GPU_material_stencil_marker_error(const GPUMaterial *mat);
bool GPU_material_has_volume_output(GPUMaterial *mat);
bool GPU_material_has_displacement_output(GPUMaterial *mat);
bool GPU_material_has_depth_offset_output(GPUMaterial *mat);
bool GPU_material_has_tessellation_output(GPUMaterial *mat);
bool GPU_material_has_filter_output(GPUMaterial *mat);
bool GPU_material_has_light_shader_output(GPUMaterial *mat);
bool GPU_material_has_glsl_light_shader_eval(const GPUMaterial *mat);
bool GPU_material_has_shader_info_shadow_classification(const GPUMaterial *mat);
bool GPU_material_has_node_soft_shadow(const GPUMaterial *mat);
bool GPU_material_has_node_shadow_cache(const GPUMaterial *mat);
int GPU_material_node_shadow_cache_quality(const GPUMaterial *mat);
bool GPU_material_principled_npr_v2_has(const GPUMaterial *mat);
bool GPU_material_surface_diffusion_has(const GPUMaterial *mat);
void GPU_material_surface_diffusion_set(GPUMaterial *mat);
eGPUMaterialNPRFeature GPU_material_npr_features_get(const GPUMaterial *mat);
bool GPU_material_uses_hiz_data(const GPUMaterial *mat);

int GPU_material_filter_object_info_ensure(GPUMaterial *material, Object *object);
int GPU_material_filter_object_info_count(const GPUMaterial *material);
Object *GPU_material_filter_object_info_get(const GPUMaterial *material, int index);
uint32_t GPU_material_referenced_object_ensure(
    GPUMaterial *material, Object *object, eGPUReferencedObjectDataFlag flags);
bool GPU_material_uses_referenced_object_data(const GPUMaterial *material);
int GPU_material_referenced_object_count(const GPUMaterial *material);
const GPUReferencedObject *GPU_material_referenced_object_get(const GPUMaterial *material,
                                                              int index);
int GPU_material_filter_mask_object_append(GPUMaterial *material, Object *object);
int GPU_material_filter_mask_object_count(const GPUMaterial *material);
Object *GPU_material_filter_mask_object_get(const GPUMaterial *material, int index);

struct GPUMaterialGeneratedSource {
  std::string filename;
  Vector<std::string> dependencies;
  std::string content;
};

void GPU_material_generated_source_add(GPUMaterial *material,
                                       StringRefNull filename,
                                       Span<StringRefNull> dependencies,
                                       StringRefNull content);
int GPU_material_generated_source_count(const GPUMaterial *material);
const GPUMaterialGeneratedSource *GPU_material_generated_source_get(const GPUMaterial *material,
                                                                    int index);

bool GPU_material_flag_get(const GPUMaterial *mat, eGPUMaterialFlag flag);
bool GPU_material_has_outline_output(const GPUMaterial *mat);

uint64_t GPU_material_uuid_get(GPUMaterial *mat);

struct GPULayerAttr {
  GPULayerAttr *next, *prev;

  /* Meaningful part of the attribute set key. */
  char name[256]; /* Multiple MAX_CUSTOMDATA_LAYER_NAME */
  /** Hash of `name[68]`. */
  uint32_t hash_code;

  /* Helper fields used by code generation. */
  int users;
};

const ListBaseT<GPULayerAttr> *GPU_material_layer_attributes(const GPUMaterial *material);

/* Requested Material Attributes and Textures */

enum GPUType {
  GPU_NONE,
  GPU_FLOAT,
  GPU_VEC2,
  GPU_VEC3,
  GPU_VEC4,
  GPU_MAT3,
  GPU_MAT4,

  GPU_INT,
  GPU_INT2,
  GPU_INT3,
  GPU_INT4,
  GPU_BOOL,
  GPU_TEX1D_ARRAY,
  GPU_TEX2D,
  GPU_TEX2D_ARRAY,
  GPU_TEX3D,
  GPU_TEXCUBE,
  GPU_TEX_HANDLE = 1008,

  /* Struct types. */
  GPU_CLOSURE,
  GPU_KERNEL_GLOBALS,
  GPU_SHADING_DATA,

  /* Vertex Attributes. */
  GPU_ATTR,
};

/** Non-owning callback input description copied by the frame push API. */
struct GPUMaterialClosureCallbackInput {
  int closure_output_node_id = 0;
  StringRef item_key;
  GPUType type = GPU_NONE;
  int function_input_index = -1;
};

struct GPUMaterialFunctionOutput {
  GPUType type = GPU_NONE;
  GPUNodeLink **link = nullptr;
};

/* Element count of GPU_MAT4. */
constexpr int GPU_MAX_CONSTANT_DATA = 16;

constexpr int gpu_type_element_count(const GPUType type)
{
  switch (type) {
    case GPU_FLOAT:
    case GPU_INT:
    case GPU_BOOL:
      return 1;
    case GPU_VEC2:
    case GPU_INT2:
      return 2;
    case GPU_VEC3:
    case GPU_INT3:
      return 3;
    case GPU_VEC4:
    case GPU_INT4:
      return 4;
    case GPU_MAT3:
      return 9;
    case GPU_MAT4:
      return 16;
    case GPU_NONE:
    case GPU_TEX1D_ARRAY:
    case GPU_TEX2D:
    case GPU_TEX2D_ARRAY:
    case GPU_TEX3D:
    case GPU_TEXCUBE:
    case GPU_CLOSURE:
    case GPU_TEX_HANDLE:
    case GPU_ATTR:
    case GPU_KERNEL_GLOBALS:
    case GPU_SHADING_DATA:
      break;
  }

  BLI_assert_unreachable();
  return 0;
}

constexpr GPUType gpu_float_type_from_element_count(const int count)
{
  switch (count) {
    case 1:
      return GPU_FLOAT;
    case 2:
      return GPU_VEC2;
    case 3:
      return GPU_VEC3;
    case 4:
      return GPU_VEC4;
    case 9:
      return GPU_MAT3;
    case 16:
      return GPU_MAT4;
  }

  BLI_assert_unreachable();
  return GPU_NONE;
}

constexpr GPUType gpu_int_type_from_element_count(const int count)
{
  switch (count) {
    case 1:
      return GPU_INT;
    case 2:
      return GPU_INT2;
    case 3:
      return GPU_INT3;
    case 4:
      return GPU_INT4;
  }

  BLI_assert_unreachable();
  return GPU_NONE;
}

enum GPUDefaultValue {
  GPU_DEFAULT_0 = 0,
  GPU_DEFAULT_1,
};

struct GPUMaterialAttribute {
  GPUMaterialAttribute *next, *prev;
  int type; /* eCustomDataType */
  char name[/*MAX_CUSTOMDATA_LAYER_NAME*/ 68];
  char input_name[/*GPU_MAX_SAFE_ATTR_NAME + 1*/ 12 + 1];
  GPUType gputype;
  GPUDefaultValue default_value; /* Only for volumes attributes. */
  int id;
  int users;
  /**
   * If true, the corresponding attribute is the specified default color attribute on the mesh,
   * if it exists. In that case the type and name data can vary per geometry, so it will not be
   * valid here.
   */
  bool is_default_color;
  /**
   * If true, the attribute is the length of hair particles and curves.
   */
  bool is_hair_length;
  /**
   * If true, the attribute is the intercept of hair particles and curves.
   */
  bool is_hair_intercept;
};

struct GPUMaterialTexture {
  GPUMaterialTexture *next = nullptr;
  GPUMaterialTexture *prev = nullptr;
  Image *ima = nullptr;
  ImageUser iuser = {};
  bool iuser_available = false;
  bool use_3d_lut_strip = false;
  bool use_cube_strip = false;
  int lut_3d_width = 0;
  int lut_3d_height = 0;
  int lut_3d_depth = 0;
  gpu::Texture **colorband = nullptr;
  gpu::Texture **sky = nullptr;
  char sampler_name[32] = {};       /* Name of sampler in GLSL. */
  char tiled_mapping_name[32] = {}; /* Name of tile mapping sampler in GLSL. */
  int users = 0;
  GPUSamplerState sampler_state = GPUSamplerState::default_sampler();
};

ListBaseT<GPUMaterialAttribute> GPU_material_attributes(const GPUMaterial *material);
ListBaseT<GPUMaterialTexture> GPU_material_textures(GPUMaterial *material);

struct GPUUniformAttr {
  GPUUniformAttr *next, *prev;

  /* Meaningful part of the attribute set key. */
  char name[/*MAX_CUSTOMDATA_LAYER_NAME*/ 68];
  /** Hash of `name[MAX_CUSTOMDATA_LAYER_NAME] + use_dupli`. */
  uint32_t hash_code;
  bool use_dupli;

  /* Helper fields used by code generation. */
  short id;
  int users;
};

struct GPUUniformAttrList {
  ListBaseT<GPUUniformAttr> list;

  /* List length and hash code precomputed for fast lookup and comparison. */
  unsigned int count, hash_code;
};

const GPUUniformAttrList *GPU_material_uniform_attributes(const GPUMaterial *material);

/* Functions to create GPU Materials nodes. */
/* TODO: Move to its own header. */

/**
 * Held type matches GPUType when the socket stores a constant.
 * Otherwise, std::monostate is used as a placeholder.
 */
using GPUNodeStackValue = std::variant<std::monostate,
                                       float,
                                       float2,
                                       float3,
                                       float4,
                                       float3x3,
                                       float4x4,
                                       int,
                                       int2,
                                       int3,
                                       int4,
                                       bool>;

/**
 * Returns the GPUNodeStackValue which corresponds to the given type, zero-initialized.
 */
inline GPUNodeStackValue GPU_node_stack_default_value(const GPUType type)
{
  switch (type) {
    case GPU_FLOAT:
      return 0.0f;
    case GPU_VEC2:
      return float2(0.0f);
    case GPU_VEC3:
      return float3(0.0f);
    case GPU_VEC4:
      return float4(0.0f);
    case GPU_MAT3:
      return float3x3::zero();
    case GPU_MAT4:
      return float4x4::zero();
    case GPU_INT:
      return 0;
    case GPU_INT2:
      return int2(0);
    case GPU_INT3:
      return int3(0);
    case GPU_INT4:
      return int4(0);
    case GPU_BOOL:
      return false;
    case GPU_NONE:
    case GPU_TEX1D_ARRAY:
    case GPU_TEX2D:
    case GPU_TEX2D_ARRAY:
    case GPU_TEX3D:
    case GPU_TEXCUBE:
    case GPU_CLOSURE:
    case GPU_TEX_HANDLE:
    case GPU_ATTR:
    case GPU_KERNEL_GLOBALS:
    case GPU_SHADING_DATA:
      break;
  }

  return std::monostate{};
}

struct GPUNodeStack {
  GPUType type = GPU_NONE;
  GPUNodeStackValue value{std::monostate{}};
  GPUNodeLink *link = nullptr;
  bool hasinput = false;
  bool hasoutput = false;
  short sockettype = 0;
  bool end = false;

  /* Return true if the socket might contain a polychromatic value.
   * This is a conservative heuristic that allows for optimization. */
  bool might_be_tinted() const
  {
    if (this->link) {
      return true;
    }
    switch (this->type) {
      case GPU_VEC3: {
        const float3 &vec = std::get<float3>(this->value);
        return (vec[0] != vec[1]) || (vec[1] != vec[2]);
      }
      case GPU_VEC4: {
        const float4 &vec = std::get<float4>(this->value);
        return (vec[0] != vec[1]) || (vec[1] != vec[2]);
      }
      default:
        break;
    }
    BLI_assert_unreachable();
    return true;
  }

  bool socket_not_zero() const
  {
    if (this->link) {
      return true;
    }
    switch (this->type) {
      case GPU_FLOAT:
        return saturate_f(std::get<float>(this->value)) > near_zero;
      case GPU_INT:
        return std::get<int>(this->value) != 0;
      case GPU_BOOL:
        return std::get<bool>(this->value);
      default:
        break;
    }
    BLI_assert_unreachable();
    return true;
  }

  bool socket_not_one() const
  {
    if (this->link) {
      return true;
    }
    switch (this->type) {
      case GPU_FLOAT:
        return saturate_f(std::get<float>(this->value)) < near_one;
      case GPU_INT:
        return std::get<int>(this->value) != 1;
      case GPU_BOOL:
        return !std::get<bool>(this->value);
      default:
        break;
    }
    BLI_assert_unreachable();
    return true;
  }

  bool socket_not_black() const
  {
    if (this->link) {
      return true;
    }
    switch (this->type) {
      case GPU_VEC3: {
        const float3 &vec = std::get<float3>(this->value);
        return saturate_f(vec[0]) > near_zero || saturate_f(vec[1]) > near_zero ||
               saturate_f(vec[2]) > near_zero;
      }
      case GPU_VEC4: {
        const float4 &vec = std::get<float4>(this->value);
        return saturate_f(vec[0]) > near_zero || saturate_f(vec[1]) > near_zero ||
               saturate_f(vec[2]) > near_zero;
      }
      default:
        break;
    }
    BLI_assert_unreachable();
    return true;
  }

  bool socket_not_white() const
  {
    if (this->link) {
      return true;
    }
    switch (this->type) {
      case GPU_VEC3: {
        const float3 &vec = std::get<float3>(this->value);
        return saturate_f(vec[0]) < near_one || saturate_f(vec[1]) < near_one ||
               saturate_f(vec[2]) < near_one;
      }
      case GPU_VEC4: {
        const float4 &vec = std::get<float4>(this->value);
        return saturate_f(vec[0]) < near_one || saturate_f(vec[1]) < near_one ||
               saturate_f(vec[2]) < near_one;
      }
      default:
        break;
    }
    BLI_assert_unreachable();
    return true;
  }

 private:
  static constexpr float near_zero = 1e-5f;
  static constexpr float near_one = 1.0f - 1e-5f;
  float saturate_f(const float f) const
  {
    return clamp_f(f, 0.0f, 1.0f);
  }
};

struct GPUGraphOutput {
  std::string serialized;
  Vector<StringRefNull> dependencies;

  bool empty() const
  {
    return serialized.empty();
  }

  std::string serialized_or_default(std::string value) const
  {
    return serialized.empty() ? value : serialized;
  }
};

struct GPUCodegenOutput {
  std::string attr_load;
  /* Node-tree functions calls. */
  GPUGraphOutput displacement;
  GPUGraphOutput surface;
  GPUGraphOutput volume;
  GPUGraphOutput thickness;
  std::optional<GPUGraphOutput> depth_offset;
  std::optional<GPUGraphOutput> tessellation;
  GPUGraphOutput npr;
  GPUGraphOutput filter;
  Vector<int> filter_output_identifiers;
  Vector<GPUGraphOutput> filter_outputs;
  std::optional<GPUGraphOutput> light_shader;
  GPUGraphOutput composite;
  Vector<GPUGraphOutput> material_functions;

  GPUShaderCreateInfo *create_info;
};

GPUNodeLink *GPU_shading_data();
GPUNodeLink *GPU_kernel_globals();
GPUNodeLink *GPU_constant(const float *num);
GPUNodeLink *GPU_uniform(const float *num);
GPUNodeLink *GPU_uniform_from_socket(GPUMaterial *material,
                                     const bNodeSocket *socket,
                                     const float *num);
GPUNodeLink *GPU_uniform_from_socket(GPUMaterial *material,
                                     const bNodeSocket *socket,
                                     const GPUNodeStack &stack);
GPUNodeLink *GPU_function_call(StringRefNull function_call);
GPUNodeLink *GPU_constant(const int *num);
GPUNodeLink *GPU_uniform(const int *num);
GPUNodeLink *GPU_constant(const bool *num);
GPUNodeLink *GPU_uniform(const bool *num);
GPUNodeLink *GPU_constant(const GPUNodeStack &stack);
GPUNodeLink *GPU_uniform(const GPUNodeStack &stack);
GPUNodeLink *GPU_attribute(GPUMaterial *mat, eCustomDataType type, const char *name);
/**
 * Add a GPU attribute that refers to the default color attribute on a geometry.
 * The name, type, and domain are unknown and do not depend on the material.
 */
GPUNodeLink *GPU_attribute_default_color(GPUMaterial *mat);
/**
 * Add a GPU attribute that refers to the approximate length of curves/hairs.
 */
GPUNodeLink *GPU_attribute_hair_length(GPUMaterial *mat);
GPUNodeLink *GPU_attribute_hair_intercept(GPUMaterial *mat);
GPUNodeLink *GPU_attribute_with_default(GPUMaterial *mat,
                                        eCustomDataType type,
                                        const char *name,
                                        GPUDefaultValue default_value);
GPUNodeLink *GPU_uniform_attribute(GPUMaterial *mat,
                                   const char *name,
                                   bool use_dupli,
                                   uint32_t *r_hash);
GPUNodeLink *GPU_layer_attribute(GPUMaterial *mat, const char *name);
GPUNodeLink *GPU_image(GPUMaterial *mat,
                       Image *ima,
                       ImageUser *iuser,
                       GPUSamplerState sampler_state);
GPUNodeLink *GPU_image_cube_strip(GPUMaterial *mat,
                                  Image *ima,
                                  ImageUser *iuser,
                                  GPUSamplerState sampler_state);
GPUNodeLink *GPU_image_3d_lut_strip(GPUMaterial *mat,
                                    Image *ima,
                                    ImageUser *iuser,
                                    int width,
                                    int height,
                                    int depth,
                                    GPUSamplerState sampler_state);
void GPU_image_tiled(GPUMaterial *mat,
                     Image *ima,
                     ImageUser *iuser,
                     GPUSamplerState sampler_state,
                     GPUNodeLink **r_image_tiled_link,
                     GPUNodeLink **r_image_tiled_mapping_link);
GPUNodeLink *GPU_image_sky(GPUMaterial *mat,
                           int width,
                           int height,
                           const float *pixels,
                           float *layer,
                           GPUSamplerState sampler_state);
GPUNodeLink *GPU_color_band(GPUMaterial *mat, int size, float *pixels, float *r_row);

/**
 * Create an implementation defined differential calculation of a float function.
 * The given function should return a float.
 * The result will be a vec2 containing dFdx and dFdy result of that function.
 */
GPUNodeLink *GPU_differentiate_float_function(const char *function_name, const float filter_width);

void GPU_material_closure_uv_source_push(GPUMaterial *material, StringRefNull source);
void GPU_material_closure_uv_source_push(GPUMaterial *material,
                                         StringRefNull source,
                                         GPUType source_type);
void GPU_material_closure_uv_source_pop(GPUMaterial *material);
StringRefNull GPU_material_closure_uv_source_get(const GPUMaterial *material);
GPUType GPU_material_closure_uv_source_type_get(const GPUMaterial *material);
void GPU_material_closure_uv_gradient_source_push(GPUMaterial *material,
                                                  StringRefNull dx_source,
                                                  StringRefNull dy_source);
void GPU_material_closure_uv_gradient_source_pop(GPUMaterial *material);
void GPU_material_closure_uv_gradient_source_get(const GPUMaterial *material,
                                                 StringRefNull &r_dx_source,
                                                 StringRefNull &r_dy_source);
/** Pushes an owned copy. Lookup searches frames from the newest to the oldest. */
void GPU_material_closure_callback_input_frame_push(GPUMaterial *material,
                                                    Span<GPUMaterialClosureCallbackInput> inputs);
void GPU_material_closure_callback_input_frame_pop(GPUMaterial *material);
bool GPU_material_closure_callback_input_find(const GPUMaterial *material,
                                              int closure_output_node_id,
                                              StringRef item_key,
                                              GPUType &r_type,
                                              int &r_function_input_index,
                                              bool &r_is_ancestor_capture);
bool GPU_material_closure_callback_input_frame_error_set(GPUMaterial *material, StringRef error);
bool GPU_material_closure_callback_input_frame_error_get(const GPUMaterial *material,
                                                         std::string &r_error);

bool GPU_link(GPUMaterial *mat, const char *name, ...);
bool GPU_stack_link(GPUMaterial *mat,
                    const bNode *node,
                    const char *name,
                    GPUNodeStack *in,
                    GPUNodeStack *out,
                    ...);
bool GPU_stack_link_custom(GPUMaterial *material,
                           const bNode *bnode,
                           StringRefNull name,
                           StringRefNull dependency_name,
                           eGPUCustomNodeDependencyFlag dependency_flags,
                           GPUNodeStack *in,
                           GPUNodeStack *out);

bool GPU_stack_link_zone(GPUMaterial *material,
                         const bNode *bnode,
                         const char *name,
                         GPUNodeStack *in,
                         GPUNodeStack *out,
                         int zone_index,
                         bool is_zone_end,
                         int in_argument_count,
                         int out_argument_count);

void GPU_material_output_surface(GPUMaterial *material, GPUNodeLink *link);
void GPU_material_output_volume(GPUMaterial *material, GPUNodeLink *link);
void GPU_material_output_displacement(GPUMaterial *material, GPUNodeLink *link);
void GPU_material_output_thickness(GPUMaterial *material, GPUNodeLink *link);
void GPU_material_output_depth_offset(GPUMaterial *material, GPUNodeLink *link);
/** \param link: Packed tessellation parameters (see `node_shader_tessellation_control.cc`). */
void GPU_material_output_tessellation(GPUMaterial *material, GPUNodeLink *link);
void GPU_material_output_npr(GPUMaterial *material, GPUNodeLink *link);
void GPU_material_output_filter(GPUMaterial *material, GPUNodeLink *link);
void GPU_material_output_filter_item(GPUMaterial *material, int identifier, GPUNodeLink *link);
void GPU_material_output_light_shader(GPUMaterial *material, GPUNodeLink *link);
void GPU_material_glsl_light_shader_eval_set(GPUMaterial *material);
void GPU_material_shader_info_shadow_classification_set(GPUMaterial *material);
void GPU_material_node_soft_shadow_set(GPUMaterial *material);
void GPU_material_node_shadow_cache_set(GPUMaterial *material, int quality);
void GPU_material_principled_npr_v2_set(GPUMaterial *material);
void GPU_material_npr_features_add(GPUMaterial *material, eGPUMaterialNPRFeature features);
void GPU_material_hiz_data_set(GPUMaterial *material);

void GPU_material_add_output_link_aov(GPUMaterial *material, GPUNodeLink *link, int hash);
void GPU_material_add_output_link_outline(GPUMaterial *material, GPUNodeLink *link);

void GPU_material_add_output_link_composite(GPUMaterial *material, GPUNodeLink *link);

/**
 * Wrap a part of the material graph into a function. You need then need to call the function by
 * using something like #GPU_differentiate_float_function.
 * \note This replace the link by a constant to break the link with the main graph.
 * \param return_type: sub function return type. Output is cast to this type.
 * \param link: link to use as the sub function output.
 * \return the name of the generated function.
 */
char *GPU_material_split_sub_function(GPUMaterial *material,
                                      GPUType return_type,
                                      GPUNodeLink **link,
                                      StringRefNull dependency_name,
                                      bool preserve_shading_state = false);

/**
 * Wrap a part of the material graph into a void function with typed inputs and outputs.
 * Each output is cast to its declared type and all output dependency graphs are serialized once.
 */
char *GPU_material_split_sub_function_multi(GPUMaterial *material,
                                            Span<GPUType> input_types,
                                            Span<GPUMaterialFunctionOutput> outputs,
                                            StringRefNull dependency_name);

void GPU_material_flag_set(GPUMaterial *mat, eGPUMaterialFlag flag);
eGPUMaterialFlag GPU_material_flag(const GPUMaterial *mat);
void GPU_material_set_time_dependent(GPUMaterial *mat);
bool GPU_material_is_time_dependent(const GPUMaterial *mat);

GHash *GPU_uniform_attr_list_hash_new(const char *info);
void GPU_uniform_attr_list_copy(GPUUniformAttrList *dest, const GPUUniformAttrList *src);
void GPU_uniform_attr_list_free(GPUUniformAttrList *set);

/* Returns the GPU node stack of the input with the given identifier in the given node within the
 * given inputs stack array. */
GPUNodeStack &GPU_node_get_input(const bNode &node, GPUNodeStack inputs[], StringRef identifier);

/* Returns the GPU node stack of the output with the given identifier in the given node within the
 * given output stack array. */
GPUNodeStack &GPU_node_get_output(const bNode &node, GPUNodeStack outputs[], StringRef identifier);

/* Returns the GPU node link of the input with the given identifier in the given node within the
 * given inputs stack array, if the input is not linked, a uniform link carrying the value of the
 * input will be created and returned. It is expected that the caller will use the returned link in
 * a GPU material, otherwise, the link may not be properly freed. */
GPUNodeLink *GPU_node_get_input_link(const bNode &node,
                                     GPUNodeStack inputs[],
                                     StringRef identifier);

}  // namespace blender
