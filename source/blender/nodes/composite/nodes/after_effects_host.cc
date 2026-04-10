/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "after_effects_host.hh"
#include "after_effects_host_suites.hh"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "BKE_main.hh"

#include "BLI_fileops.h"
#include "BLI_ghash.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

#if __has_include("AE_Effect.h") && __has_include("AE_PluginData.h")
#  include "AE_Effect.h"
#  include "AE_EffectCB.h"
#  include "AE_EffectCBSuites.h"
#  include "AE_PluginData.h"
#  include "SP/SPBasic.h"
#  define WITH_AFTER_EFFECTS_SDK_HEADERS 1
#else
#  define WITH_AFTER_EFFECTS_SDK_HEADERS 0
#endif

namespace blender::nodes::after_effects {

namespace {

#if defined(_WIN32)
using DynamicLibraryHandle = HMODULE;
static constexpr DynamicLibraryHandle null_library = nullptr;

static DynamicLibraryHandle dynamic_library_open(const char *path)
{
  return ::LoadLibraryA(path);
}

static void *dynamic_library_find(DynamicLibraryHandle handle, const char *symbol)
{
  return reinterpret_cast<void *>(::GetProcAddress(handle, symbol));
}

static void dynamic_library_close(DynamicLibraryHandle handle)
{
  if (handle) {
    ::FreeLibrary(handle);
  }
}
#else
using DynamicLibraryHandle = void *;
static constexpr DynamicLibraryHandle null_library = nullptr;

static DynamicLibraryHandle dynamic_library_open(const char *path)
{
  return ::dlopen(path, RTLD_NOW);
}

static void *dynamic_library_find(DynamicLibraryHandle handle, const char *symbol)
{
  return ::dlsym(handle, symbol);
}

static void dynamic_library_close(DynamicLibraryHandle handle)
{
  if (handle) {
    ::dlclose(handle);
  }
}
#endif

class DynamicLibrary {
 private:
  DynamicLibraryHandle handle_ = null_library;

 public:
  explicit DynamicLibrary(const char *path) : handle_(dynamic_library_open(path)) {}

  ~DynamicLibrary()
  {
    dynamic_library_close(handle_);
  }

  DynamicLibrary(const DynamicLibrary &) = delete;
  DynamicLibrary &operator=(const DynamicLibrary &) = delete;

  explicit operator bool() const
  {
    return handle_ != null_library;
  }

  void *find_symbol(const char *symbol) const
  {
    if (!handle_) {
      return nullptr;
    }
    return dynamic_library_find(handle_, symbol);
  }
};

static void ae_host_debug_log(const char *format, ...)
{

  std::fprintf(stderr, "[AE Host] ");

  va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  va_end(args);

  std::fprintf(stderr, "\n");
}

static std::string make_identifier(StringRef source, const int fallback_index)
{
  std::string id;
  id.reserve(source.size() + 8);
  for (const char c : source) {
    if (std::isalnum(uchar(c)) || c == '_') {
      id.push_back(std::tolower(uchar(c)));
    }
    else if (c == ' ' || c == '-' || c == '.') {
      id.push_back('_');
    }
  }

  if (id.empty()) {
    id = "param_" + std::to_string(fallback_index);
  }

  if (!std::isalpha(uchar(id.front())) && id.front() != '_') {
    id.insert(id.begin(), 'p');
    id.insert(id.begin() + 1, '_');
  }

  return id;
}

static void resolve_absolute_path(const StringRef filepath, char r_path[FILE_MAX])
{
  const std::string filepath_string = filepath;
  BLI_strncpy(r_path, filepath_string.c_str(), FILE_MAX);
  BLI_path_abs(r_path, BKE_main_blendfile_path_from_global());
}

#if WITH_AFTER_EFFECTS_SDK_HEADERS

static float fixed_to_float(const PF_Fixed value)
{
  return float(value) / 65536.0f;
}

static std::vector<PopupItem> parse_popup_items(const char *names)
{
  std::vector<PopupItem> items;
  if (!names || names[0] == '\0') {
    return items;
  }

  int value = 1;
  std::string current;
  for (const char *it = names;; ++it) {
    const char c = *it;
    if (c == '\0' || c == '|' || c == '/') {
      if (!current.empty()) {
        PopupItem item;
        item.value = value++;
        item.name = current;
        item.identifier = make_identifier(current, item.value);
        items.push_back(std::move(item));
      }
      current.clear();
      if (c == '\0') {
        break;
      }
      continue;
    }
    current.push_back(c);
  }

  return items;
}

struct ParseState {
  PluginMetadata *metadata = nullptr;
  int add_param_call_index = 0;
  PF_ParamDef **params = nullptr;
  int num_params = 0;
};

static PF_Err parse_add_param(PF_ProgPtr effect_ref, PF_ParamIndex index, PF_ParamDefPtr def)
{
  if (effect_ref == nullptr || def == nullptr) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  ParseState *state = reinterpret_cast<ParseState *>(effect_ref);
  PluginMetadata &metadata = *state->metadata;

  PluginParameter parameter;
  parameter.index = (index < 0) ? int(metadata.parameters.size()) + 1 : int(index);
  parameter.disk_id = def->uu.id;
  parameter.ae_param_type = def->param_type;
  parameter.name = def->PF_DEF_NAME;
  parameter.identifier = make_identifier(
      parameter.name.empty() ? StringRef("param") : StringRef(parameter.name), state->add_param_call_index);
  parameter.expose_as_socket = true;
  state->add_param_call_index++;

  switch (def->param_type) {
    case PF_Param_LAYER:
      parameter.type = ParamType::Layer;
      parameter.layer_default_myself = (def->u.ld.dephault == PF_LayerDefault_MYSELF);
      break;
    case PF_Param_SLIDER:
      parameter.type = ParamType::Int;
      parameter.min_value = float(def->u.sd.valid_min);
      parameter.max_value = float(def->u.sd.valid_max);
      parameter.default_value = float(def->u.sd.dephault);
      break;
    case PF_Param_FIX_SLIDER:
      parameter.type = ParamType::Float;
      parameter.min_value = fixed_to_float(def->u.fd.valid_min);
      parameter.max_value = fixed_to_float(def->u.fd.valid_max);
      parameter.default_value = fixed_to_float(def->u.fd.dephault);
      break;
    case PF_Param_FLOAT_SLIDER:
      parameter.type = ParamType::Float;
      parameter.min_value = def->u.fs_d.valid_min;
      parameter.max_value = def->u.fs_d.valid_max;
      parameter.default_value = def->u.fs_d.dephault;
      break;
    case PF_Param_ANGLE:
      parameter.type = ParamType::Float;
      parameter.default_value = fixed_to_float(def->u.ad.dephault);
      parameter.min_value = -36000.0f;
      parameter.max_value = 36000.0f;
      break;
    case PF_Param_CHECKBOX:
      parameter.type = ParamType::Bool;
      parameter.default_value = float(def->u.bd.dephault ? 1 : 0);
      break;
    case PF_Param_COLOR:
      parameter.type = ParamType::Color;
      parameter.vector_default = float4(float(def->u.cd.dephault.red) / 255.0f,
                                        float(def->u.cd.dephault.green) / 255.0f,
                                        float(def->u.cd.dephault.blue) / 255.0f,
                                        1.0f);
      break;
    case PF_Param_POINT:
      parameter.type = ParamType::Vector2;
      parameter.vector_default = float4(fixed_to_float(def->u.td.x_dephault),
                                        fixed_to_float(def->u.td.y_dephault),
                                        0.0f,
                                        0.0f);
      break;
    case PF_Param_POINT_3D:
      parameter.type = ParamType::Vector3;
      parameter.vector_default = float4(
          def->u.point3d_d.x_dephault, def->u.point3d_d.y_dephault, def->u.point3d_d.z_dephault, 0.0f);
      break;
    case PF_Param_POPUP:
      parameter.type = ParamType::Menu;
      parameter.default_value = float(def->u.pd.dephault);
      parameter.min_value = 1.0f;
      parameter.max_value = std::max(1, int(def->u.pd.num_choices));
      parameter.popup_items = parse_popup_items(def->u.pd.u.namesptr);
      break;
    case PF_Param_GROUP_START:
      parameter.type = ParamType::GroupStart;
      parameter.expose_as_socket = false;
      break;
    case PF_Param_GROUP_END:
      parameter.type = ParamType::GroupEnd;
      parameter.expose_as_socket = false;
      break;
    case PF_Param_BUTTON:
    case PF_Param_PATH:
    case PF_Param_ARBITRARY_DATA:
    case PF_Param_CUSTOM:
    case PF_Param_NO_DATA:
      parameter.type = ParamType::Unsupported;
      parameter.expose_as_socket = false;
      metadata.has_unsupported_parameters = true;
      break;
    default:
      parameter.type = ParamType::Unsupported;
      parameter.expose_as_socket = false;
      metadata.has_unsupported_parameters = true;
      break;
  }

  metadata.parameters.push_back(std::move(parameter));
  return PF_Err_NONE;
}

static PF_Err parse_noop_abort(PF_ProgPtr /*effect_ref*/)
{
  return PF_Err_NONE;
}

static PF_Err parse_noop_new_world(PF_ProgPtr /*effect_ref*/,
                                   A_long /*width*/,
                                   A_long /*height*/,
                                   PF_NewWorldFlags /*flags*/,
                                   PF_EffectWorld *world)
{
  /* util_new_world expects a HostRenderState* via effect_ref, but during parameter
   * parsing effect_ref points to a ParseState. Plugins that allocate worlds during
   * GLOBAL_SETUP or PARAMS_SETUP would cause a type-confusion crash, so stub it out. */
  if (world) {
    std::memset(world, 0, sizeof(*world));
  }
  return PF_Err_BAD_CALLBACK_PARAM;
}

static PF_Err parse_noop_dispose_world(PF_ProgPtr /*effect_ref*/, PF_EffectWorld *world)
{
  if (world) {
    std::memset(world, 0, sizeof(*world));
  }
  return PF_Err_NONE;
}

static PF_Err parse_noop_progress(PF_ProgPtr /*effect_ref*/, A_long /*current*/, A_long /*total*/)
{
  return PF_Err_NONE;
}

static PF_Err parse_noop_register_ui(PF_ProgPtr /*effect_ref*/, PF_CustomUIInfo * /*cust_info*/)
{
  return PF_Err_NONE;
}

static PF_Err parse_noop_checkout_param(PF_ProgPtr effect_ref,
                                        PF_ParamIndex index,
                                        A_long /*what_time*/,
                                        A_long /*step*/,
                                        A_u_long /*scale*/,
                                        PF_ParamDef *param)
{
  if (effect_ref == nullptr || param == nullptr) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }
  ParseState *state = reinterpret_cast<ParseState *>(effect_ref);
  if (index < 0 || index >= state->num_params || state->params == nullptr || state->params[index] == nullptr) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }
  *param = *state->params[index];
  return PF_Err_NONE;
}

static PF_Err parse_noop_checkin_param(PF_ProgPtr /*effect_ref*/, PF_ParamDef * /*param*/)
{
  return PF_Err_NONE;
}

static PF_Err parse_noop_checkout_layer_audio(PF_ProgPtr /*effect_ref*/,
                                              PF_ParamIndex /*index*/,
                                              A_long /*start_time*/,
                                              A_long /*duration*/,
                                              A_u_long /*scale*/,
                                              PF_UFixed /*rate*/,
                                              A_long /*bytes_per_sample*/,
                                              A_long /*num_channels*/,
                                              A_long /*fmt_signed*/,
                                              PF_LayerAudio *audio)
{
  if (audio) {
    *audio = nullptr;
  }
  return PF_Err_NONE;
}

static PF_Err parse_noop_checkin_layer_audio(PF_ProgPtr /*effect_ref*/, PF_LayerAudio /*audio*/)
{
  return PF_Err_NONE;
}

static PF_Err parse_noop_get_audio_data(PF_ProgPtr /*effect_ref*/,
                                        PF_LayerAudio /*audio*/,
                                        PF_SndSamplePtr *data0,
                                        A_long *num_samples0,
                                        PF_UFixed *rate0,
                                        A_long *bytes_per_sample0,
                                        A_long *num_channels0,
                                        A_long *fmt_signed0)
{
  if (data0) {
    *data0 = nullptr;
  }
  if (num_samples0) {
    *num_samples0 = 0;
  }
  if (rate0) {
    *rate0 = 0;
  }
  if (bytes_per_sample0) {
    *bytes_per_sample0 = 0;
  }
  if (num_channels0) {
    *num_channels0 = 0;
  }
  if (fmt_signed0) {
    *fmt_signed0 = 0;
  }
  return PF_Err_NONE;
}

using PluginDataEntryFunction2Fn = PluginDataEntryFunction2Ptr;
using PluginDataEntryFunctionFn = PluginDataEntryFunctionPtr;
using EffectMainFn = PF_FilterProc;

static A_Err plugin_data_callback_2(PF_PluginDataPtr in_ptr,
                                    const A_u_char *in_name,
                                    const A_u_char *in_match_name,
                                    const A_u_char *in_category,
                                    const A_u_char *in_entry_point_name,
                                    A_long /*in_kind*/,
                                    A_long /*in_api_major*/,
                                    A_long /*in_api_minor*/,
                                    A_long /*in_reserved_info*/,
                                    const A_u_char * /*in_support_url*/)
{
  PluginMetadata *metadata = reinterpret_cast<PluginMetadata *>(in_ptr);
  if (!metadata) {
    return A_Err_GENERIC;
  }
  if (in_name) {
    metadata->effect_name = reinterpret_cast<const char *>(in_name);
  }
  if (in_match_name) {
    metadata->match_name = reinterpret_cast<const char *>(in_match_name);
  }
  if (in_category) {
    metadata->category = reinterpret_cast<const char *>(in_category);
  }
  if (in_entry_point_name) {
    metadata->entry_point = reinterpret_cast<const char *>(in_entry_point_name);
  }
  return A_Err_NONE;
}

static A_Err plugin_data_callback_1(PF_PluginDataPtr in_ptr,
                                    const A_u_char *in_name,
                                    const A_u_char *in_match_name,
                                    const A_u_char *in_category,
                                    const A_u_char *in_entry_point_name,
                                    A_long /*in_kind*/,
                                    A_long /*in_api_major*/,
                                    A_long /*in_api_minor*/,
                                    A_long /*in_reserved_info*/)
{
  return plugin_data_callback_2(in_ptr,
                                in_name,
                                in_match_name,
                                in_category,
                                in_entry_point_name,
                                0,
                                0,
                                0,
                                0,
                                nullptr);
}

static bool safe_call_effect_main(const EffectMainFn effect_main,
                                  const PF_Cmd cmd,
                                  PF_InData *in_data,
                                  PF_OutData *out_data,
                                  PF_ParamDef *params[],
                                  PF_LayerDef *output,
                                  void *extra,
                                  PF_Err &r_err)
{
  if (!effect_main) {
    r_err = PF_Err_BAD_CALLBACK_PARAM;
    return false;
  }

#  if defined(_MSC_VER) && defined(_WIN32)
  __try {
    r_err = effect_main(cmd, in_data, out_data, params, output, extra);
    return true;
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    r_err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    return false;
  }
#  else
  r_err = effect_main(cmd, in_data, out_data, params, output, extra);
  return true;
#  endif
}

static void sync_persistent_plugin_data(PF_InData &in_data, const PF_OutData &out_data)
{
  /* Mirror AE host behavior where setup commands publish persistent handles through out_data
   * and later commands receive them through in_data. */
  in_data.global_data = out_data.global_data;
  in_data.sequence_data = out_data.sequence_data;
  in_data.frame_data = out_data.frame_data;
}

static void initialize_base_in_data(PF_InData &in_data,
                                    PF_ProgPtr effect_ref,
                                    const A_long width,
                                    const A_long height,
                                    const A_long num_params,
                                    const PF_RationalScale &pixel_aspect_ratio,
                                    PF_UtilCallbacks *utils,
                                    SPBasicSuite *pica_basic)
{
  std::memset(&in_data, 0, sizeof(in_data));
  in_data.effect_ref = effect_ref;
  in_data.quality = PF_Quality_HI;
  in_data.version.major = PF_AE_PLUG_IN_VERSION;
  in_data.version.minor = PF_AE_PLUG_IN_SUBVERS;
  in_data.appl_id = kAppID_AfterEffects;
  in_data.num_params = num_params;
  in_data.current_time = 0;
  in_data.time_step = 1;
  in_data.total_time = 1;
  in_data.local_time_step = 1;
  in_data.time_scale = 1;
  in_data.field = PF_Field_FRAME;
  in_data.shutter_angle = 0;
  in_data.shutter_phase = 0;
  in_data.width = width;
  in_data.height = height;
  in_data.extent_hint.left = 0;
  in_data.extent_hint.top = 0;
  in_data.extent_hint.right = width;
  in_data.extent_hint.bottom = height;
  in_data.output_origin_x = 0;
  in_data.output_origin_y = 0;
  in_data.downsample_x.num = 1;
  in_data.downsample_x.den = 1;
  in_data.downsample_y.num = 1;
  in_data.downsample_y.den = 1;
  in_data.pixel_aspect_ratio = pixel_aspect_ratio;
  in_data.pre_effect_source_origin_x = 0;
  in_data.pre_effect_source_origin_y = 0;
  in_data.utils = utils;
  in_data.pica_basicP = pica_basic;
}

static PF_Err util_iterate(PF_InData *in_data,
                           A_long progress_base,
                           A_long progress_final,
                           PF_EffectWorld *src,
                           const PF_Rect *area,
                           void *refcon,
                           PF_IteratePixel8Func pix_fn,
                           PF_EffectWorld *dst);
static PF_Err util_iterate_origin(PF_InData *in_data,
                                  A_long progress_base,
                                  A_long progress_final,
                                  PF_EffectWorld *src,
                                  const PF_Rect *area,
                                  const PF_Point *origin,
                                  void *refcon,
                                  PF_IteratePixel8Func pix_fn,
                                  PF_EffectWorld *dst);
static PF_Err util_iterate_lut(PF_InData *in_data,
                               A_long progress_base,
                               A_long progress_final,
                               PF_EffectWorld *src,
                               const PF_Rect *area,
                               A_u_char *a_lut0,
                               A_u_char *r_lut0,
                               A_u_char *g_lut0,
                               A_u_char *b_lut0,
                               PF_EffectWorld *dst);
static PF_Err util_iterate_origin_non_clip_src(PF_InData *in_data,
                                               A_long progress_base,
                                               A_long progress_final,
                                               PF_EffectWorld *src,
                                               const PF_Rect *area,
                                               const PF_Point *origin,
                                               void *refcon,
                                               PF_IteratePixel8Func pix_fn,
                                               PF_EffectWorld *dst);
static PF_Err util_iterate_generic(
    A_long iterationsL,
    void *refconPV,
    PF_Err (*fn_func)(void *refconPV, A_long thread_indexL, A_long i, A_long iterationsL));
static PF_Err util_new_world(PF_ProgPtr effect_ref,
                             A_long width,
                             A_long height,
                             PF_NewWorldFlags flags,
                             PF_EffectWorld *world);
static PF_Err util_dispose_world(PF_ProgPtr effect_ref, PF_EffectWorld *world);
static PF_Handle util_host_new_handle(A_u_longlong size);
static void *util_host_lock_handle(PF_Handle pf_handle);
static void util_host_unlock_handle(PF_Handle pf_handle);
static void util_host_dispose_handle(PF_Handle pf_handle);
static A_u_longlong util_host_get_handle_size(PF_Handle pf_handle);
static PF_Err util_host_resize_handle(A_u_longlong new_size, PF_Handle *handlePH);
static PF_Err util_get_pixel_data8(PF_EffectWorld *world, PF_PixelPtr pixels, PF_Pixel8 **pixPP);
static PF_Err util_get_pixel_data16(PF_EffectWorld *world, PF_PixelPtr pixels, PF_Pixel16 **pixPP);
static PF_Err util_get_pixel_data_float(PF_EffectWorld *world,
                                        PF_PixelPtr pixels,
                                        PF_PixelFloat **pixPP);
static void initialize_util_callbacks(PF_UtilCallbacks &utils);


/* PF_HandleSuite1 implementation - uses existing util callbacks */
static void initialize_handle_suite(PF_HandleSuite1 &handle_suite)
{
  handle_suite.host_new_handle = util_host_new_handle;
  handle_suite.host_lock_handle = util_host_lock_handle;
  handle_suite.host_unlock_handle = util_host_unlock_handle;
  handle_suite.host_dispose_handle = util_host_dispose_handle;
  handle_suite.host_get_handle_size = util_host_get_handle_size;
  handle_suite.host_resize_handle = util_host_resize_handle;
}

/* PF_ANSICallbacksSuite1 implementation */
static void initialize_ansi_suite(PF_ANSICallbacksSuite1 &ansi_suite)
{
  ansi_suite.atan = std::atan;
  ansi_suite.atan2 = std::atan2;
  ansi_suite.ceil = std::ceil;
  ansi_suite.cos = std::cos;
  ansi_suite.exp = std::exp;
  ansi_suite.fabs = std::fabs;
  ansi_suite.floor = std::floor;
  ansi_suite.fmod = std::fmod;
  ansi_suite.hypot = std::hypot;
  ansi_suite.log = std::log;
  ansi_suite.log10 = std::log10;
  ansi_suite.pow = std::pow;
  ansi_suite.sin = std::sin;
  ansi_suite.sqrt = std::sqrt;
  ansi_suite.tan = std::tan;
  ansi_suite.sprintf = std::sprintf;
  ansi_suite.strcpy = std::strcpy;
  ansi_suite.asin = std::asin;
  ansi_suite.acos = std::acos;
}

/* PF_PixelDataSuite1 implementation */
static void initialize_pixel_data_suite(PF_PixelDataSuite1 &pixel_data_suite)
{
  pixel_data_suite.get_pixel_data8 = util_get_pixel_data8;
  pixel_data_suite.get_pixel_data16 = util_get_pixel_data16;
  pixel_data_suite.get_pixel_data_float = util_get_pixel_data_float;
}

static PF_Err world_suite2_new_world(PF_ProgPtr effect_ref,
                                     A_long width,
                                     A_long height,
                                     PF_Boolean clear_pix,
                                     PF_PixelFormat pixel_format,
                                     PF_EffectWorld *world)
{
  PF_NewWorldFlags flags = PF_NewWorldFlags(0);
  if (clear_pix) {
    flags = PF_NewWorldFlags(flags | PF_NewWorldFlag_CLEAR_PIXELS);
  }

  switch (pixel_format) {
    case PF_PixelFormat_ARGB32:
      break;
    case PF_PixelFormat_ARGB64:
      flags = PF_NewWorldFlags(flags | PF_NewWorldFlag_DEEP_PIXELS);
      break;
    default:
      return PF_Err_BAD_CALLBACK_PARAM;
  }

  return util_new_world(effect_ref, width, height, flags, world);
}

static PF_Err world_suite2_dispose_world(PF_ProgPtr effect_ref, PF_EffectWorld *world)
{
  return util_dispose_world(effect_ref, world);
}

static PF_Err world_suite2_get_pixel_format(const PF_EffectWorld *world, PF_PixelFormat *pixel_format)
{
  if (!world || !pixel_format) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }
  *pixel_format = PF_WORLD_IS_DEEP(world) ? PF_PixelFormat_ARGB64 : PF_PixelFormat_ARGB32;
  return PF_Err_NONE;
}

static void initialize_world_suite1(PF_WorldSuite1 &world_suite)
{
  world_suite.new_world = util_new_world;
  world_suite.dispose_world = util_dispose_world;
}

static void initialize_world_suite2(PF_WorldSuite2 &world_suite)
{
  world_suite.PF_NewWorld = world_suite2_new_world;
  world_suite.PF_DisposeWorld = world_suite2_dispose_world;
  world_suite.PF_GetPixelFormat = world_suite2_get_pixel_format;
}

static void initialize_iterate8_suite1(PF_Iterate8Suite1 &iterate_suite)
{
  iterate_suite.iterate = util_iterate;
  iterate_suite.iterate_origin = util_iterate_origin;
  iterate_suite.iterate_lut = util_iterate_lut;
  iterate_suite.iterate_origin_non_clip_src = util_iterate_origin_non_clip_src;
  iterate_suite.iterate_generic = util_iterate_generic;
}

static void initialize_iterate8_suite2(PF_Iterate8Suite2 &iterate_suite)
{
  iterate_suite.iterate = util_iterate;
  iterate_suite.iterate_origin = util_iterate_origin;
  iterate_suite.iterate_lut = util_iterate_lut;
  iterate_suite.iterate_origin_non_clip_src = util_iterate_origin_non_clip_src;
  iterate_suite.iterate_generic = util_iterate_generic;
}

static PF_Err pixel_format_add_supported(PF_ProgPtr /*effect_ref*/, PF_PixelFormat pixel_format)
{
  switch (pixel_format) {
    case PF_PixelFormat_ARGB32:
    case PF_PixelFormat_ARGB64:
      return PF_Err_NONE;
    default:
      return PF_Err_BAD_CALLBACK_PARAM;
  }
}

static PF_Err pixel_format_clear_supported(PF_ProgPtr /*effect_ref*/)
{
  return PF_Err_NONE;
}

static void initialize_pixel_format_suite2(PF_PixelFormatSuite2 &pixel_format_suite)
{
  pixel_format_suite.PF_AddSupportedPixelFormat = pixel_format_add_supported;
  pixel_format_suite.PF_ClearSupportedPixelFormats = pixel_format_clear_supported;
}

static void register_common_suites(SuiteRegistry &registry,
                                   PF_HandleSuite1 &handle_suite,
                                   PF_ANSICallbacksSuite1 &ansi_suite,
                                   PF_PixelDataSuite1 &pixel_data_suite,
                                   PF_ColorCallbacksSuite1 &color_suite,
                                   PF_WorldSuite1 &world_suite_v1,
                                   PF_WorldSuite2 &world_suite_v2,
                                   PF_Iterate8Suite1 &iterate8_suite_v1,
                                   PF_Iterate8Suite2 &iterate8_suite_v2,
                                   PF_Iterate16Suite1 &iterate16_suite_v1,
                                   PF_Iterate16Suite2 &iterate16_suite_v2,
                                   PF_PixelFormatSuite2 &pixel_format_suite)
{
  registry.register_suite(kPFHandleSuite, kPFHandleSuiteVersion1, &handle_suite);
  registry.register_suite(kPFANSISuite, kPFANSISuiteVersion1, &ansi_suite);
  registry.register_suite(kPFPixelDataSuite, kPFPixelDataSuiteVersion1, &pixel_data_suite);
  registry.register_suite(kPFColorCallbacksSuite, kPFColorCallbacksSuiteVersion1, &color_suite);
  registry.register_suite(kPFWorldSuite, kPFWorldSuiteVersion1, &world_suite_v1);
  registry.register_suite(kPFWorldSuite, kPFWorldSuiteVersion2, &world_suite_v2);
  registry.register_suite(kPFIterate8Suite, kPFIterate8SuiteVersion1, &iterate8_suite_v1);
  registry.register_suite(kPFIterate8Suite, kPFIterate8SuiteVersion2, &iterate8_suite_v2);
  registry.register_suite(kPFIterate16Suite, kPFIterate16SuiteVersion1, &iterate16_suite_v1);
  registry.register_suite(kPFIterate16Suite, kPFIterate16SuiteVersion2, &iterate16_suite_v2);
  registry.register_suite(kPFPixelFormatSuite, kPFPixelFormatSuiteVersion2, &pixel_format_suite);
}

static bool extract_plugin_metadata_from_entrypoints(DynamicLibrary &library, PluginMetadata &metadata)
{
  if (void *symbol = library.find_symbol("PluginDataEntryFunction2")) {
    PluginDataEntryFunction2Fn entry = reinterpret_cast<PluginDataEntryFunction2Fn>(symbol);
    entry(reinterpret_cast<PF_PluginDataPtr>(&metadata),
          plugin_data_callback_2,
          nullptr,
          "Blender",
          "5.1");
    return true;
  }

  if (void *symbol = library.find_symbol("PluginDataEntryFunction")) {
    PluginDataEntryFunctionFn entry = reinterpret_cast<PluginDataEntryFunctionFn>(symbol);
    entry(reinterpret_cast<PF_PluginDataPtr>(&metadata),
          plugin_data_callback_1,
          nullptr,
          "Blender",
          "5.1");
    return true;
  }

  return false;
}

static bool parse_parameters_with_effect_main(DynamicLibrary &library,
                                              const PluginMetadata &metadata,
                                              PluginMetadata &r_metadata,
                                              std::string &r_error)
{
  std::vector<std::string> candidate_entry_points;
  if (!metadata.entry_point.empty()) {
    candidate_entry_points.push_back(metadata.entry_point);
  }
  candidate_entry_points.push_back("EffectMain");
  candidate_entry_points.push_back("EntryPointFunc");
  candidate_entry_points.push_back("main");

  EffectMainFn effect_main = nullptr;
  for (const std::string &entry_name : candidate_entry_points) {
    if (void *symbol = library.find_symbol(entry_name.c_str())) {
      effect_main = reinterpret_cast<EffectMainFn>(symbol);
      r_metadata.entry_point = entry_name;
      break;
    }
  }

  if (!effect_main) {
    std::string tried;
    for (const std::string &entry_name : candidate_entry_points) {
      if (!tried.empty()) {
        tried += ", ";
      }
      tried += entry_name;
    }
    r_error = "Plugin effect entry point not found (tried: " + tried + ")";
    return false;
  }

  ParseState parse_state;
  parse_state.metadata = &r_metadata;

  SPBasicSuite pica_basic{};
  PF_HandleSuite1 handle_suite{};
  PF_ANSICallbacksSuite1 ansi_suite{};
  PF_PixelDataSuite1 pixel_data_suite{};
  PF_ColorCallbacksSuite1 color_suite{};
  PF_WorldSuite1 world_suite_v1{};
  PF_WorldSuite2 world_suite_v2{};
  PF_Iterate8Suite1 iterate8_suite_v1{};
  PF_Iterate8Suite2 iterate8_suite_v2{};
  PF_Iterate16Suite1 iterate16_suite_v1{};
  PF_Iterate16Suite2 iterate16_suite_v2{};
  PF_PixelFormatSuite2 pixel_format_suite{};
  SuiteRegistry suite_registry;

  initialize_handle_suite(handle_suite);
  initialize_ansi_suite(ansi_suite);
  initialize_pixel_data_suite(pixel_data_suite);
  initialize_color_suite(color_suite);
  initialize_world_suite1(world_suite_v1);
  initialize_world_suite2(world_suite_v2);
  initialize_iterate8_suite1(iterate8_suite_v1);
  initialize_iterate8_suite2(iterate8_suite_v2);
  initialize_iterate16_suite1(iterate16_suite_v1);
  initialize_iterate16_suite2(iterate16_suite_v2);
  initialize_pixel_format_suite2(pixel_format_suite);
  register_common_suites(suite_registry,
                         handle_suite,
                         ansi_suite,
                         pixel_data_suite,
                         color_suite,
                         world_suite_v1,
                         world_suite_v2,
                         iterate8_suite_v1,
                         iterate8_suite_v2,
                         iterate16_suite_v1,
                         iterate16_suite_v2,
                         pixel_format_suite);

  initialize_pica_basic_suite(pica_basic);
  PF_UtilCallbacks parse_utils{};
  initialize_util_callbacks(parse_utils);
  /* Override world callbacks: util_new_world/util_dispose_world dereference effect_ref
   * as HostRenderState*, but here effect_ref is ParseState*. Use safe stubs instead. */
  parse_utils.new_world = parse_noop_new_world;
  parse_utils.dispose_world = parse_noop_dispose_world;

  PF_InData in_data{};
  initialize_base_in_data(in_data,
                          reinterpret_cast<PF_ProgPtr>(&parse_state),
                          1,
                          1,
                          1,
                          PF_RationalScale{1, 1},
                          &parse_utils,
                          &pica_basic);
  in_data.inter.add_param = parse_add_param;
  in_data.inter.abort = parse_noop_abort;
  in_data.inter.progress = parse_noop_progress;
  in_data.inter.register_ui = parse_noop_register_ui;
  in_data.inter.checkout_param = parse_noop_checkout_param;
  in_data.inter.checkin_param = parse_noop_checkin_param;
  in_data.inter.checkout_layer_audio = parse_noop_checkout_layer_audio;
  in_data.inter.checkin_layer_audio = parse_noop_checkin_layer_audio;
  in_data.inter.get_audio_data = parse_noop_get_audio_data;

  PF_OutData out_data{};
  out_data.num_params = 1;

  PF_ParamDef input_param{};
  input_param.param_type = PF_Param_LAYER;
  STRNCPY(input_param.PF_DEF_NAME, "Image");
  input_param.u.ld.dephault = PF_LayerDefault_MYSELF;

  PF_ParamDef *param_list[1] = {&input_param};
  parse_state.params = param_list;
  parse_state.num_params = 1;

  ScopedActiveSuiteRegistry active_registry_scope(suite_registry);

  PF_Err global_setup_err = PF_Err_NONE;
  const bool global_setup_called = safe_call_effect_main(effect_main,
                                                         PF_Cmd_GLOBAL_SETUP,
                                                         &in_data,
                                                         &out_data,
                                                         param_list,
                                                         nullptr,
                                                         nullptr,
                                                         global_setup_err);
  if (global_setup_called && global_setup_err == PF_Err_NONE) {
    sync_persistent_plugin_data(in_data, out_data);
  }

  PF_Err params_setup_err = PF_Err_NONE;
  if (!safe_call_effect_main(effect_main,
                             PF_Cmd_PARAMS_SETUP,
                             &in_data,
                             &out_data,
                             param_list,
                             nullptr,
                             nullptr,
                             params_setup_err) ||
      params_setup_err != PF_Err_NONE)
  {
    r_error = "Plugin failed during PF_Cmd_PARAMS_SETUP";
    return false;
  }
  sync_persistent_plugin_data(in_data, out_data);

  if (global_setup_called && global_setup_err == PF_Err_NONE) {
    PF_Err global_setdown_err = PF_Err_NONE;
    safe_call_effect_main(effect_main,
                          PF_Cmd_GLOBAL_SETDOWN,
                          &in_data,
                          &out_data,
                          param_list,
                          nullptr,
                          nullptr,
                          global_setdown_err);
  }
  else if (!global_setup_called || global_setup_err != PF_Err_NONE) {
    /* Some plugins still expose parameters without full global setup support in this host. */
    r_metadata.has_unsupported_parameters = true;
  }

  r_metadata.supports_deep_color = (out_data.out_flags & PF_OutFlag_DEEP_COLOR_AWARE) != 0;
  r_metadata.supports_smart_render = (out_data.out_flags2 & PF_OutFlag2_SUPPORTS_SMART_RENDER) != 0;
  r_metadata.supports_float_color = (out_data.out_flags2 & PF_OutFlag2_FLOAT_COLOR_AWARE) != 0;
  r_metadata.expands_buffer = (out_data.out_flags & PF_OutFlag_I_EXPAND_BUFFER) != 0;

  return true;
}

struct HostLayerBuffer {
  PF_LayerDef world{};
  std::vector<PF_Pixel> pixels;
};

struct HostRenderState {
  PF_InData in_data{};
  PF_OutData out_data{};
  PF_UtilCallbacks utils{};
  SPBasicSuite pica_basic{};
  SuiteRegistry suite_registry{};
  PF_HandleSuite1 handle_suite{};
  PF_ANSICallbacksSuite1 ansi_suite{};
  PF_PixelDataSuite1 pixel_data_suite{};
  PF_ColorCallbacksSuite1 color_suite{};
  PF_WorldSuite1 world_suite_v1{};
  PF_WorldSuite2 world_suite_v2{};
  PF_Iterate8Suite1 iterate8_suite_v1{};
  PF_Iterate8Suite2 iterate8_suite_v2{};
  PF_Iterate16Suite1 iterate16_suite_v1{};
  PF_Iterate16Suite2 iterate16_suite_v2{};
  PF_PixelFormatSuite2 pixel_format_suite{};
  std::vector<PF_ParamDef> params;
  std::vector<HostLayerBuffer> layer_buffers;
  std::unordered_set<void *> owned_world_data;
  int2 image_size = int2(1, 1);
  PF_RationalScale pixel_aspect_ratio = {1, 1};
};

static HostRenderState *render_state_from_effect_ref(PF_ProgPtr effect_ref)
{
  return reinterpret_cast<HostRenderState *>(effect_ref);
}

static int clamp_byte(float value)
{
  const float scaled = std::clamp(value, 0.0f, 1.0f) * 255.0f;
  return int(std::round(scaled));
}

static PF_Pixel to_ae_pixel(const compositor::Color &color)
{
  PF_Pixel pixel{};
  pixel.alpha = uchar(clamp_byte(color.a));
  pixel.red = uchar(clamp_byte(color.r));
  pixel.green = uchar(clamp_byte(color.g));
  pixel.blue = uchar(clamp_byte(color.b));
  return pixel;
}

static compositor::Color to_compositor_color(const PF_Pixel &pixel)
{
  return compositor::Color(float4(float(pixel.red) / 255.0f,
                                  float(pixel.green) / 255.0f,
                                  float(pixel.blue) / 255.0f,
                                  float(pixel.alpha) / 255.0f));
}

static void initialize_world(PF_LayerDef &world,
                             int2 size,
                             void *data,
                             const bool deep,
                             const PF_RationalScale &pixel_aspect_ratio)
{
  world.world_flags = PF_WorldFlag_WRITEABLE;
  if (deep) {
    world.world_flags |= PF_WorldFlag_DEEP;
  }
  world.data = reinterpret_cast<PF_PixelPtr>(data);
  world.rowbytes = size.x * int(deep ? sizeof(PF_Pixel16) : sizeof(PF_Pixel));
  world.width = size.x;
  world.height = size.y;
  world.extent_hint.left = 0;
  world.extent_hint.top = 0;
  world.extent_hint.right = size.x;
  world.extent_hint.bottom = size.y;
  world.pix_aspect_ratio = pixel_aspect_ratio;
  world.origin_x = 0;
  world.origin_y = 0;
  world.dephault = PF_LayerDefault_MYSELF;
}

static bool fill_layer_from_result(const compositor::Result &result,
                                   int2 size,
                                   const PF_RationalScale &pixel_aspect_ratio,
                                   HostLayerBuffer &r_layer)
{
  r_layer.pixels.resize(size.x * size.y);
  initialize_world(r_layer.world, size, r_layer.pixels.data(), false, pixel_aspect_ratio);

  if (result.is_single_value()) {
    const compositor::Color color = result.get_single_value_default<compositor::Color>();
    const PF_Pixel pixel = to_ae_pixel(color);
    std::fill(r_layer.pixels.begin(), r_layer.pixels.end(), pixel);
    return true;
  }

  for (int y = 0; y < size.y; y++) {
    for (int x = 0; x < size.x; x++) {
      const compositor::Color color = result.load_pixel_zero<compositor::Color>(int2(x, y));
      r_layer.pixels[y * size.x + x] = to_ae_pixel(color);
    }
  }
  return true;
}

static PF_Err render_checkout_param(PF_ProgPtr effect_ref,
                                    PF_ParamIndex index,
                                    A_long /*what_time*/,
                                    A_long /*step*/,
                                    A_u_long /*scale*/,
                                    PF_ParamDef *param)
{
  if (param == nullptr) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }
  HostRenderState *state = render_state_from_effect_ref(effect_ref);
  if (!state || index < 0 || index >= state->params.size()) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }
  *param = state->params[index];
  return PF_Err_NONE;
}

static PF_Err render_checkin_param(PF_ProgPtr /*effect_ref*/, PF_ParamDef * /*param*/)
{
  return PF_Err_NONE;
}

static PF_Err render_add_param(PF_ProgPtr effect_ref, PF_ParamIndex index, PF_ParamDefPtr def)
{
  HostRenderState *state = render_state_from_effect_ref(effect_ref);
  if (!state || !def || index < 0 || index >= state->params.size()) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }
  state->params[index] = *def;
  return PF_Err_NONE;
}

static PF_Err render_abort(PF_ProgPtr /*effect_ref*/)
{
  return PF_Err_NONE;
}

static PF_Err render_progress(PF_ProgPtr /*effect_ref*/, A_long /*current*/, A_long /*total*/)
{
  return PF_Err_NONE;
}

static PF_Err render_register_ui(PF_ProgPtr /*effect_ref*/, PF_CustomUIInfo * /*cust_info*/)
{
  return PF_Err_NONE;
}

static PF_Err render_checkout_layer_audio(PF_ProgPtr /*effect_ref*/,
                                          PF_ParamIndex /*index*/,
                                          A_long /*start_time*/,
                                          A_long /*duration*/,
                                          A_u_long /*time_scale*/,
                                          PF_UFixed /*rate*/,
                                          A_long /*bytes_per_sample*/,
                                          A_long /*num_channels*/,
                                          A_long /*fmt_signed*/,
                                          PF_LayerAudio * /*audio*/)
{
  return PF_Err_BAD_CALLBACK_PARAM;
}

static PF_Err render_checkin_layer_audio(PF_ProgPtr /*effect_ref*/, PF_LayerAudio /*audio*/)
{
  return PF_Err_NONE;
}

static PF_Err render_get_audio_data(PF_ProgPtr /*effect_ref*/,
                                    PF_LayerAudio /*audio*/,
                                    PF_SndSamplePtr * /*data0*/,
                                    A_long * /*num_samples0*/,
                                    PF_UFixed * /*rate0*/,
                                    A_long * /*bytes_per_sample0*/,
                                    A_long * /*num_channels0*/,
                                    A_long * /*fmt_signed0*/)
{
  return PF_Err_BAD_CALLBACK_PARAM;
}

static PF_Pixel *pixel8_at(PF_EffectWorld *world, int x, int y)
{
  return reinterpret_cast<PF_Pixel *>(reinterpret_cast<char *>(world->data) + y * world->rowbytes) + x;
}

static const PF_Pixel *pixel8_at(const PF_EffectWorld *world, int x, int y)
{
  return reinterpret_cast<const PF_Pixel *>(reinterpret_cast<const char *>(world->data) +
                                            y * world->rowbytes) +
         x;
}

static PF_Pixel16 *pixel16_at(PF_EffectWorld *world, int x, int y)
{
  return reinterpret_cast<PF_Pixel16 *>(reinterpret_cast<char *>(world->data) + y * world->rowbytes) + x;
}

static const PF_Pixel16 *pixel16_at(const PF_EffectWorld *world, int x, int y)
{
  return reinterpret_cast<const PF_Pixel16 *>(reinterpret_cast<const char *>(world->data) +
                                              y * world->rowbytes) +
         x;
}

static PF_Err util_copy(PF_ProgPtr /*effect_ref*/,
                        PF_EffectWorld *src,
                        PF_EffectWorld *dst,
                        PF_Rect *src_r,
                        PF_Rect *dst_r)
{
  if (!src || !dst || !src->data || !dst->data) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  const PF_Rect src_rect = src_r ? *src_r : PF_Rect{0, 0, src->width, src->height};
  const PF_Rect dst_rect = dst_r ? *dst_r : PF_Rect{0, 0, dst->width, dst->height};
  const int width = std::min(int(src_rect.right - src_rect.left), int(dst_rect.right - dst_rect.left));
  const int height = std::min(int(src_rect.bottom - src_rect.top), int(dst_rect.bottom - dst_rect.top));

  for (int y = 0; y < height; y++) {
    PF_Pixel *src_row = reinterpret_cast<PF_Pixel *>(
        reinterpret_cast<char *>(src->data) + (src_rect.top + y) * src->rowbytes);
    PF_Pixel *dst_row = reinterpret_cast<PF_Pixel *>(
        reinterpret_cast<char *>(dst->data) + (dst_rect.top + y) * dst->rowbytes);
    std::memcpy(dst_row + dst_rect.left, src_row + src_rect.left, sizeof(PF_Pixel) * width);
  }

  return PF_Err_NONE;
}

static PF_Err util_composite_rect(PF_ProgPtr /*effect_ref*/,
                                  PF_Rect *src_rect,
                                  A_long src_opacity,
                                  PF_EffectWorld *source_wld,
                                  A_long dest_x,
                                  A_long dest_y,
                                  PF_Field /*field_rdr*/,
                                  PF_XferMode xfer_mode,
                                  PF_EffectWorld *dest_wld)
{
  if (!source_wld || !dest_wld || !source_wld->data || !dest_wld->data || PF_WORLD_IS_DEEP(source_wld) ||
      PF_WORLD_IS_DEEP(dest_wld))
  {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  const PF_Rect src = src_rect ? *src_rect : PF_Rect{0, 0, source_wld->width, source_wld->height};
  const float opacity = std::clamp(float(src_opacity) / 255.0f, 0.0f, 1.0f);

  for (int sy = src.top; sy < src.bottom; sy++) {
    const int dy = dest_y + (sy - src.top);
    if (dy < 0 || dy >= dest_wld->height) {
      continue;
    }
    for (int sx = src.left; sx < src.right; sx++) {
      const int dx = dest_x + (sx - src.left);
      if (dx < 0 || dx >= dest_wld->width) {
        continue;
      }

      const PF_Pixel src_px = *pixel8_at(source_wld, sx, sy);
      PF_Pixel &dst_px = *pixel8_at(dest_wld, dx, dy);

      if (xfer_mode == PF_Xfer_COPY) {
        dst_px = src_px;
        continue;
      }

      const float src_alpha = (float(src_px.alpha) / 255.0f) * opacity;
      const float dst_alpha = float(dst_px.alpha) / 255.0f;
      const float out_alpha = std::clamp(src_alpha + dst_alpha * (1.0f - src_alpha), 0.0f, 1.0f);

      /* AE expects straight (non-premultiplied) alpha, so colors are already straight. */
      const float src_r = float(src_px.red) / 255.0f;
      const float src_g = float(src_px.green) / 255.0f;
      const float src_b = float(src_px.blue) / 255.0f;
      const float dst_r = float(dst_px.red) / 255.0f;
      const float dst_g = float(dst_px.green) / 255.0f;
      const float dst_b = float(dst_px.blue) / 255.0f;

      /* Standard over operation with straight alpha. */
      const float out_r = src_r * src_alpha + dst_r * dst_alpha * (1.0f - src_alpha);
      const float out_g = src_g * src_alpha + dst_g * dst_alpha * (1.0f - src_alpha);
      const float out_b = src_b * src_alpha + dst_b * dst_alpha * (1.0f - src_alpha);

      dst_px.red = A_u_char(clamp_byte(out_r));
      dst_px.green = A_u_char(clamp_byte(out_g));
      dst_px.blue = A_u_char(clamp_byte(out_b));
      dst_px.alpha = A_u_char(clamp_byte(out_alpha));
    }
  }

  return PF_Err_NONE;
}

static PF_Err util_fill(PF_ProgPtr /*effect_ref*/,
                        const PF_Pixel *color,
                        const PF_Rect *dst_rect,
                        PF_EffectWorld *world)
{
  if (!world || !world->data) {
    ae_host_debug_log("util_fill: invalid world/data world=%p data=%p", world, world ? world->data : nullptr);
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  const PF_Rect raw_rect = dst_rect ? *dst_rect : PF_Rect{0, 0, world->width, world->height};
  PF_Rect rect = raw_rect;
  rect.left = std::max<A_long>(0, rect.left);
  rect.top = std::max<A_long>(0, rect.top);
  rect.right = std::min<A_long>(world->width, rect.right);
  rect.bottom = std::min<A_long>(world->height, rect.bottom);
  if (rect.left >= rect.right || rect.top >= rect.bottom) {
    ae_host_debug_log("util_fill: empty/clipped rect l=%ld t=%ld r=%ld b=%ld -> l=%ld t=%ld r=%ld b=%ld",
                      raw_rect.left,
                      raw_rect.top,
                      raw_rect.right,
                      raw_rect.bottom,
                      rect.left,
                      rect.top,
                      rect.right,
                      rect.bottom);
    return PF_Err_NONE;
  }

  if (PF_WORLD_IS_DEEP(world)) {
    const PF_Pixel16 fill = color ? PF_Pixel16{A_u_short(int(color->alpha) * 128),
                                               A_u_short(int(color->red) * 128),
                                               A_u_short(int(color->green) * 128),
                                               A_u_short(int(color->blue) * 128)} :
                                  PF_Pixel16{};
    for (int y = rect.top; y < rect.bottom; y++) {
      for (int x = rect.left; x < rect.right; x++) {
        *pixel16_at(world, x, y) = fill;
      }
    }
    return PF_Err_NONE;
  }

  const PF_Pixel fill = color ? *color : PF_Pixel{};
  for (int y = rect.top; y < rect.bottom; y++) {
    PF_Pixel *row = reinterpret_cast<PF_Pixel *>(reinterpret_cast<char *>(world->data) + y * world->rowbytes);
    for (int x = rect.left; x < rect.right; x++) {
      row[x] = fill;
    }
  }
  return PF_Err_NONE;
}

static PF_Err util_fill16(PF_ProgPtr /*effect_ref*/,
                          const PF_Pixel16 *color,
                          const PF_Rect *dst_rect,
                          PF_EffectWorld *world)
{
  if (!world || !world->data || !PF_WORLD_IS_DEEP(world)) {
    ae_host_debug_log("util_fill16: invalid world/data/depth world=%p data=%p deep=%d",
                      world,
                      world ? world->data : nullptr,
                      world ? (PF_WORLD_IS_DEEP(world) ? 1 : 0) : 0);
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  const PF_Pixel16 fill = color ? *color : PF_Pixel16{};

  const PF_Rect raw_rect = dst_rect ? *dst_rect : PF_Rect{0, 0, world->width, world->height};
  PF_Rect rect = raw_rect;
  rect.left = std::max<A_long>(0, rect.left);
  rect.top = std::max<A_long>(0, rect.top);
  rect.right = std::min<A_long>(world->width, rect.right);
  rect.bottom = std::min<A_long>(world->height, rect.bottom);
  if (rect.left >= rect.right || rect.top >= rect.bottom) {
    ae_host_debug_log("util_fill16: empty/clipped rect l=%ld t=%ld r=%ld b=%ld -> l=%ld t=%ld r=%ld b=%ld",
                      raw_rect.left,
                      raw_rect.top,
                      raw_rect.right,
                      raw_rect.bottom,
                      rect.left,
                      rect.top,
                      rect.right,
                      rect.bottom);
    return PF_Err_NONE;
  }

  for (int y = rect.top; y < rect.bottom; y++) {
    for (int x = rect.left; x < rect.right; x++) {
      *pixel16_at(world, x, y) = fill;
    }
  }
  return PF_Err_NONE;
}

static PF_Err util_blend(PF_ProgPtr /*effect_ref*/,
                         const PF_EffectWorld *src1,
                         const PF_EffectWorld *src2,
                         PF_Fixed ratio,
                         PF_EffectWorld *dst)
{
  if (!src1 || !src2 || !dst || !src1->data || !src2->data || !dst->data) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  const float t = std::clamp(float(ratio) / 65536.0f, 0.0f, 1.0f);
  const int width = std::min({src1->width, src2->width, dst->width});
  const int height = std::min({src1->height, src2->height, dst->height});

  for (int y = 0; y < height; y++) {
    PF_Pixel *row1 = reinterpret_cast<PF_Pixel *>(reinterpret_cast<char *>(src1->data) + y * src1->rowbytes);
    PF_Pixel *row2 = reinterpret_cast<PF_Pixel *>(reinterpret_cast<char *>(src2->data) + y * src2->rowbytes);
    PF_Pixel *rowd = reinterpret_cast<PF_Pixel *>(reinterpret_cast<char *>(dst->data) + y * dst->rowbytes);
    for (int x = 0; x < width; x++) {
      const float4 c1 = float4(float(row1[x].red),
                               float(row1[x].green),
                               float(row1[x].blue),
                               float(row1[x].alpha));
      const float4 c2 = float4(float(row2[x].red),
                               float(row2[x].green),
                               float(row2[x].blue),
                               float(row2[x].alpha));
      const float4 c = math::interpolate(c1, c2, t);
      rowd[x].red = uchar(std::clamp(c.x, 0.0f, 255.0f));
      rowd[x].green = uchar(std::clamp(c.y, 0.0f, 255.0f));
      rowd[x].blue = uchar(std::clamp(c.z, 0.0f, 255.0f));
      rowd[x].alpha = uchar(std::clamp(c.w, 0.0f, 255.0f));
    }
  }

  return PF_Err_NONE;
}

static PF_Err util_iterate(PF_InData *in_data,
                           A_long /*progress_base*/,
                           A_long /*progress_final*/,
                           PF_EffectWorld *src,
                           const PF_Rect *area,
                           void *refcon,
                           PF_IteratePixel8Func pix_fn,
                           PF_EffectWorld *dst)
{
  if (!in_data || !dst || !pix_fn || !dst->data || PF_WORLD_IS_DEEP(dst)) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  if (src && (!src->data || PF_WORLD_IS_DEEP(src))) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  PF_Rect rect = area ? *area : PF_Rect{0, 0, dst->width, dst->height};
  rect.left = std::max<A_long>(0, rect.left);
  rect.top = std::max<A_long>(0, rect.top);
  rect.right = std::min<A_long>(dst->width, rect.right);
  rect.bottom = std::min<A_long>(dst->height, rect.bottom);

  if (src) {
    rect.left = std::max<A_long>(rect.left, 0);
    rect.top = std::max<A_long>(rect.top, 0);
    rect.right = std::min<A_long>(rect.right, src->width);
    rect.bottom = std::min<A_long>(rect.bottom, src->height);
  }

  if (rect.left >= rect.right || rect.top >= rect.bottom) {
    return PF_Err_NONE;
  }

  for (int y = rect.top; y < rect.bottom; y++) {
    PF_Pixel *dst_row = reinterpret_cast<PF_Pixel *>(reinterpret_cast<char *>(dst->data) + y * dst->rowbytes);
    PF_Pixel *src_row = src ? reinterpret_cast<PF_Pixel *>(reinterpret_cast<char *>(src->data) + y * src->rowbytes) :
                             dst_row;
    for (int x = rect.left; x < rect.right; x++) {
      PF_Err err = pix_fn(refcon, x, y, &src_row[x], &dst_row[x]);
      if (err != PF_Err_NONE) {
        return err;
      }
    }
  }

  return PF_Err_NONE;
}

static PF_Err util_iterate_origin(PF_InData *in_data,
                                  A_long /*progress_base*/,
                                  A_long /*progress_final*/,
                                  PF_EffectWorld *src,
                                  const PF_Rect *area,
                                  const PF_Point *origin,
                                  void *refcon,
                                  PF_IteratePixel8Func pix_fn,
                                  PF_EffectWorld *dst)
{
  if (!in_data || !src || !dst || !src->data || !dst->data || !pix_fn || PF_WORLD_IS_DEEP(src) ||
      PF_WORLD_IS_DEEP(dst))
  {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  const A_long ox = origin ? origin->h : 0;
  const A_long oy = origin ? origin->v : 0;
  PF_Rect rect = area ? *area : PF_Rect{0, 0, dst->width, dst->height};
  rect.left = std::max<A_long>(0, rect.left);
  rect.top = std::max<A_long>(0, rect.top);
  rect.right = std::min<A_long>(dst->width, rect.right);
  rect.bottom = std::min<A_long>(dst->height, rect.bottom);

  for (A_long y = rect.top; y < rect.bottom; y++) {
    const A_long sy = y + oy;
    if (sy < 0 || sy >= src->height) {
      continue;
    }
    PF_Pixel *dst_row = reinterpret_cast<PF_Pixel *>(reinterpret_cast<char *>(dst->data) + y * dst->rowbytes);
    PF_Pixel *src_row = reinterpret_cast<PF_Pixel *>(reinterpret_cast<char *>(src->data) + sy * src->rowbytes);
    for (A_long x = rect.left; x < rect.right; x++) {
      const A_long sx = x + ox;
      if (sx < 0 || sx >= src->width) {
        continue;
      }
      const PF_Err err = pix_fn(refcon, x, y, &src_row[sx], &dst_row[x]);
      if (err != PF_Err_NONE) {
        return err;
      }
    }
  }

  return PF_Err_NONE;
}

static PF_Err util_convolve(PF_ProgPtr effect_ref,
                            PF_EffectWorld *src,
                            const PF_Rect *area,
                            PF_KernelFlags /*flags*/,
                            A_long /*kernel_size*/,
                            void * /*a_kernel*/,
                            void * /*r_kernel*/,
                            void * /*g_kernel*/,
                            void * /*b_kernel*/,
                            PF_EffectWorld *dst)
{
  /* Conservative fallback to keep filters functional when they request host convolution. */
  PF_Rect src_rect;
  PF_Rect dst_rect;
  if (area) {
    src_rect = *area;
    dst_rect = *area;
    return util_copy(effect_ref, src, dst, &src_rect, &dst_rect);
  }
  return util_copy(effect_ref, src, dst, nullptr, nullptr);
}

static PF_Err util_gaussian_kernel(PF_ProgPtr /*effect_ref*/,
                                   A_FpLong kRadius,
                                   PF_KernelFlags flags,
                                   A_FpLong multiplier,
                                   A_long *diameter,
                                   void *kernel)
{
  if (!diameter || !kernel) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  const int radius = std::max(0, int(std::ceil(kRadius)));
  const int d = radius * 2 + 1;
  *diameter = d;

  const bool use_long = (flags & (PF_KernelFlag_USE_CHAR | PF_KernelFlag_USE_FIXED)) == 0;
  const bool use_char = (flags & PF_KernelFlag_USE_CHAR) != 0;
  const bool use_fixed = (flags & PF_KernelFlag_USE_FIXED) != 0;

  std::vector<double> values(size_t(d) * size_t(d));
  const double sigma = std::max(0.5, double(kRadius));
  const double two_sigma_sq = 2.0 * sigma * sigma;
  double sum = 0.0;
  for (int y = -radius; y <= radius; y++) {
    for (int x = -radius; x <= radius; x++) {
      const double v = std::exp(-(double(x * x + y * y)) / two_sigma_sq);
      values[size_t(y + radius) * size_t(d) + size_t(x + radius)] = v;
      sum += v;
    }
  }
  if (sum <= 0.0) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  for (double &v : values) {
    v = (v / sum) * double(multiplier);
  }

  if (use_long) {
    auto *out = reinterpret_cast<A_long *>(kernel);
    for (int i = 0; i < d * d; i++) {
      out[i] = A_long(std::round(values[size_t(i)] * 255.0));
    }
  }
  else if (use_char) {
    auto *out = reinterpret_cast<A_u_char *>(kernel);
    for (int i = 0; i < d * d; i++) {
      out[i] = A_u_char(std::clamp(std::round(values[size_t(i)] * 255.0), 0.0, 255.0));
    }
  }
  else if (use_fixed) {
    auto *out = reinterpret_cast<PF_Fixed *>(kernel);
    for (int i = 0; i < d * d; i++) {
      out[i] = PF_Fixed(std::round(values[size_t(i)] * 65536.0));
    }
  }

  return PF_Err_NONE;
}

static PF_Err util_iterate_lut(PF_InData *in_data,
                               A_long /*progress_base*/,
                               A_long /*progress_final*/,
                               PF_EffectWorld *src,
                               const PF_Rect *area,
                               A_u_char *a_lut0,
                               A_u_char *r_lut0,
                               A_u_char *g_lut0,
                               A_u_char *b_lut0,
                               PF_EffectWorld *dst)
{
  if (!in_data || !src || !dst || !src->data || !dst->data || PF_WORLD_IS_DEEP(src) || PF_WORLD_IS_DEEP(dst)) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  const PF_Rect rect = area ? *area : PF_Rect{0, 0, src->width, src->height};
  for (int y = rect.top; y < rect.bottom; y++) {
    for (int x = rect.left; x < rect.right; x++) {
      const PF_Pixel in = *pixel8_at(src, x, y);
      PF_Pixel out = in;
      if (a_lut0) {
        out.alpha = a_lut0[in.alpha];
      }
      if (r_lut0) {
        out.red = r_lut0[in.red];
      }
      if (g_lut0) {
        out.green = g_lut0[in.green];
      }
      if (b_lut0) {
        out.blue = b_lut0[in.blue];
      }
      *pixel8_at(dst, x, y) = out;
    }
  }

  return PF_Err_NONE;
}

static PF_Err util_premultiply(PF_ProgPtr /*effect_ref*/, A_long forward, PF_EffectWorld *dst)
{
  if (!dst || !dst->data) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  if (PF_WORLD_IS_DEEP(dst)) {
    for (int y = 0; y < dst->height; y++) {
      for (int x = 0; x < dst->width; x++) {
        PF_Pixel16 &pixel = *pixel16_at(dst, x, y);
        const float alpha = float(pixel.alpha) / float(PF_MAX_CHAN16);
        if (forward) {
          pixel.red = A_u_short(std::round(float(pixel.red) * alpha));
          pixel.green = A_u_short(std::round(float(pixel.green) * alpha));
          pixel.blue = A_u_short(std::round(float(pixel.blue) * alpha));
        }
        else if (pixel.alpha > 0) {
          const float inv_alpha = float(PF_MAX_CHAN16) / float(pixel.alpha);
          pixel.red = A_u_short(std::clamp(std::round(float(pixel.red) * inv_alpha), 0.0f, float(PF_MAX_CHAN16)));
          pixel.green = A_u_short(
              std::clamp(std::round(float(pixel.green) * inv_alpha), 0.0f, float(PF_MAX_CHAN16)));
          pixel.blue = A_u_short(
              std::clamp(std::round(float(pixel.blue) * inv_alpha), 0.0f, float(PF_MAX_CHAN16)));
        }
      }
    }
    return PF_Err_NONE;
  }

  for (int y = 0; y < dst->height; y++) {
    for (int x = 0; x < dst->width; x++) {
      PF_Pixel &pixel = *pixel8_at(dst, x, y);
      const float alpha = float(pixel.alpha) / float(PF_MAX_CHAN8);
      if (forward) {
        pixel.red = A_u_char(std::round(float(pixel.red) * alpha));
        pixel.green = A_u_char(std::round(float(pixel.green) * alpha));
        pixel.blue = A_u_char(std::round(float(pixel.blue) * alpha));
      }
      else if (pixel.alpha > 0) {
        const float inv_alpha = float(PF_MAX_CHAN8) / float(pixel.alpha);
        pixel.red = A_u_char(std::clamp(std::round(float(pixel.red) * inv_alpha), 0.0f, float(PF_MAX_CHAN8)));
        pixel.green = A_u_char(
            std::clamp(std::round(float(pixel.green) * inv_alpha), 0.0f, float(PF_MAX_CHAN8)));
        pixel.blue = A_u_char(std::clamp(std::round(float(pixel.blue) * inv_alpha), 0.0f, float(PF_MAX_CHAN8)));
      }
    }
  }

  return PF_Err_NONE;
}

static PF_Err util_premultiply_color(PF_ProgPtr /*effect_ref*/,
                                     PF_EffectWorld *src,
                                     const PF_Pixel *color,
                                     A_long forward,
                                     PF_EffectWorld *dst)
{
  if (!src || !dst || !src->data || !dst->data || src->width != dst->width || src->height != dst->height ||
      PF_WORLD_IS_DEEP(src) != PF_WORLD_IS_DEEP(dst))
  {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  if (PF_WORLD_IS_DEEP(src)) {
    const PF_Pixel16 matte = color ? PF_Pixel16{A_u_short(int(color->alpha) * 128),
                                                A_u_short(int(color->red) * 128),
                                                A_u_short(int(color->green) * 128),
                                                A_u_short(int(color->blue) * 128)} :
                                   PF_Pixel16{};
    for (int y = 0; y < src->height; y++) {
      for (int x = 0; x < src->width; x++) {
        const PF_Pixel16 in = *pixel16_at(src, x, y);
        PF_Pixel16 &out = *pixel16_at(dst, x, y);
        const float alpha = float(in.alpha) / float(PF_MAX_CHAN16);
        out.alpha = in.alpha;
        if (forward) {
          out.red = A_u_short(std::round(float(in.red) * alpha + float(matte.red) * (1.0f - alpha)));
          out.green = A_u_short(std::round(float(in.green) * alpha + float(matte.green) * (1.0f - alpha)));
          out.blue = A_u_short(std::round(float(in.blue) * alpha + float(matte.blue) * (1.0f - alpha)));
        }
        else if (in.alpha > 0) {
          const float inv_alpha = float(PF_MAX_CHAN16) / float(in.alpha);
          out.red = A_u_short(std::clamp(std::round((float(in.red) - float(matte.red) * (1.0f - alpha)) * inv_alpha),
                                         0.0f,
                                         float(PF_MAX_CHAN16)));
          out.green = A_u_short(std::clamp(
              std::round((float(in.green) - float(matte.green) * (1.0f - alpha)) * inv_alpha),
              0.0f,
              float(PF_MAX_CHAN16)));
          out.blue = A_u_short(std::clamp(std::round((float(in.blue) - float(matte.blue) * (1.0f - alpha)) * inv_alpha),
                                          0.0f,
                                          float(PF_MAX_CHAN16)));
        }
      }
    }
    return PF_Err_NONE;
  }

  const PF_Pixel matte = color ? *color : PF_Pixel{};
  for (int y = 0; y < src->height; y++) {
    for (int x = 0; x < src->width; x++) {
      const PF_Pixel in = *pixel8_at(src, x, y);
      PF_Pixel &out = *pixel8_at(dst, x, y);
      const float alpha = float(in.alpha) / float(PF_MAX_CHAN8);
      out.alpha = in.alpha;
      if (forward) {
        out.red = A_u_char(std::round(float(in.red) * alpha + float(matte.red) * (1.0f - alpha)));
        out.green = A_u_char(std::round(float(in.green) * alpha + float(matte.green) * (1.0f - alpha)));
        out.blue = A_u_char(std::round(float(in.blue) * alpha + float(matte.blue) * (1.0f - alpha)));
      }
      else if (in.alpha > 0) {
        const float inv_alpha = float(PF_MAX_CHAN8) / float(in.alpha);
        out.red = A_u_char(std::clamp(std::round((float(in.red) - float(matte.red) * (1.0f - alpha)) * inv_alpha),
                                      0.0f,
                                      float(PF_MAX_CHAN8)));
        out.green = A_u_char(std::clamp(
            std::round((float(in.green) - float(matte.green) * (1.0f - alpha)) * inv_alpha),
            0.0f,
            float(PF_MAX_CHAN8)));
        out.blue = A_u_char(std::clamp(std::round((float(in.blue) - float(matte.blue) * (1.0f - alpha)) * inv_alpha),
                                       0.0f,
                                       float(PF_MAX_CHAN8)));
      }
    }
  }

  return PF_Err_NONE;
}

static PF_Err util_premultiply_color16(PF_ProgPtr /*effect_ref*/,
                                       PF_EffectWorld *src,
                                       const PF_Pixel16 *color,
                                       A_long forward,
                                       PF_EffectWorld *dst)
{
  if (!src || !dst || !src->data || !dst->data || src->width != dst->width || src->height != dst->height ||
      !PF_WORLD_IS_DEEP(src) || !PF_WORLD_IS_DEEP(dst))
  {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  const PF_Pixel16 matte = color ? *color : PF_Pixel16{};
  for (int y = 0; y < src->height; y++) {
    for (int x = 0; x < src->width; x++) {
      const PF_Pixel16 in = *pixel16_at(src, x, y);
      PF_Pixel16 &out = *pixel16_at(dst, x, y);
      const float alpha = float(in.alpha) / float(PF_MAX_CHAN16);
      out.alpha = in.alpha;
      if (forward) {
        out.red = A_u_short(std::round(float(in.red) * alpha + float(matte.red) * (1.0f - alpha)));
        out.green = A_u_short(std::round(float(in.green) * alpha + float(matte.green) * (1.0f - alpha)));
        out.blue = A_u_short(std::round(float(in.blue) * alpha + float(matte.blue) * (1.0f - alpha)));
      }
      else if (in.alpha > 0) {
        const float inv_alpha = float(PF_MAX_CHAN16) / float(in.alpha);
        out.red = A_u_short(std::clamp(std::round((float(in.red) - float(matte.red) * (1.0f - alpha)) * inv_alpha),
                                       0.0f,
                                       float(PF_MAX_CHAN16)));
        out.green = A_u_short(std::clamp(
            std::round((float(in.green) - float(matte.green) * (1.0f - alpha)) * inv_alpha),
            0.0f,
            float(PF_MAX_CHAN16)));
        out.blue = A_u_short(std::clamp(std::round((float(in.blue) - float(matte.blue) * (1.0f - alpha)) * inv_alpha),
                                        0.0f,
                                        float(PF_MAX_CHAN16)));
      }
    }
  }

  return PF_Err_NONE;
}

static PF_Err util_new_world(PF_ProgPtr effect_ref,
                             A_long width,
                             A_long height,
                             PF_NewWorldFlags flags,
                             PF_EffectWorld *world)
{
  HostRenderState *state = render_state_from_effect_ref(effect_ref);
  if (!state || !world) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  if (width <= 0 || height <= 0) {
    ae_host_debug_log("util_new_world: clamping invalid size %ldx%ld to 1x1 (flags=%ld)",
                      width,
                      height,
                      long(flags));
    width = std::max<A_long>(1, width);
    height = std::max<A_long>(1, height);
  }

  const bool deep = (flags & PF_NewWorldFlag_DEEP_PIXELS) != 0;
  const size_t element_size = deep ? sizeof(PF_Pixel16) : sizeof(PF_Pixel);
  const size_t allocation_size = size_t(width) * size_t(height) * element_size;

  void *data = std::malloc(allocation_size);
  if (!data) {
    return PF_Err_OUT_OF_MEMORY;
  }
  if (flags & PF_NewWorldFlag_CLEAR_PIXELS) {
    std::memset(data, 0, allocation_size);
  }

  initialize_world(*world, int2(width, height), data, deep, state->pixel_aspect_ratio);
  state->owned_world_data.insert(data);
  return PF_Err_NONE;
}

static PF_Err util_dispose_world(PF_ProgPtr effect_ref, PF_EffectWorld *world)
{
  HostRenderState *state = render_state_from_effect_ref(effect_ref);
  if (!state || !world) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  if (world->data && state->owned_world_data.contains(world->data)) {
    std::free(world->data);
    state->owned_world_data.erase(world->data);
  }
  std::memset(world, 0, sizeof(*world));
  return PF_Err_NONE;
}

struct HostHandleHeader {
  A_u_longlong size = 0;
};

static HostHandleHeader *host_handle_header_from_data(void *data)
{
  return reinterpret_cast<HostHandleHeader *>(reinterpret_cast<char *>(data) - sizeof(HostHandleHeader));
}

static PF_Handle util_host_new_handle(A_u_longlong size)
{
  const size_t alloc_size = sizeof(HostHandleHeader) + size_t(size);
  auto *header = reinterpret_cast<HostHandleHeader *>(std::malloc(alloc_size));
  if (!header) {
    return nullptr;
  }
  header->size = size;
  void *data = reinterpret_cast<void *>(reinterpret_cast<char *>(header) + sizeof(HostHandleHeader));

  PF_Handle handle = reinterpret_cast<PF_Handle>(std::malloc(sizeof(void *)));
  if (!handle) {
    std::free(header);
    return nullptr;
  }
  *handle = data;
  return handle;
}

static void *util_host_lock_handle(PF_Handle pf_handle)
{
  if (!pf_handle) {
    return nullptr;
  }
  return *pf_handle;
}

static void util_host_unlock_handle(PF_Handle /*pf_handle*/) {}

static void util_host_dispose_handle(PF_Handle pf_handle)
{
  if (!pf_handle) {
    return;
  }
  if (*pf_handle) {
    std::free(host_handle_header_from_data(*pf_handle));
  }
  std::free(pf_handle);
}

static A_u_longlong util_host_get_handle_size(PF_Handle pf_handle)
{
  if (!pf_handle || !*pf_handle) {
    return 0;
  }
  return host_handle_header_from_data(*pf_handle)->size;
}

static PF_Err util_host_resize_handle(A_u_longlong new_size, PF_Handle *handlePH)
{
  if (!handlePH || !*handlePH) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }
  PF_Handle handle = *handlePH;
  HostHandleHeader *header = *handle ? host_handle_header_from_data(*handle) : nullptr;
  const size_t alloc_size = sizeof(HostHandleHeader) + size_t(new_size);
  auto *resized = reinterpret_cast<HostHandleHeader *>(std::realloc(header, alloc_size));
  if (!resized && alloc_size != 0) {
    return PF_Err_OUT_OF_MEMORY;
  }
  resized->size = new_size;
  *handle = reinterpret_cast<void *>(reinterpret_cast<char *>(resized) + sizeof(HostHandleHeader));
  return PF_Err_NONE;
}

static PF_Err util_begin_sampling(PF_ProgPtr /*effect_ref*/,
                                  PF_Quality /*qual*/,
                                  PF_ModeFlags /*mf*/,
                                  PF_SampPB * /*params*/)
{
  return PF_Err_NONE;
}

static PF_Err util_end_sampling(PF_ProgPtr /*effect_ref*/,
                                PF_Quality /*qual*/,
                                PF_ModeFlags /*mf*/,
                                PF_SampPB * /*params*/)
{
  return PF_Err_NONE;
}

static PF_Err util_subpixel_sample(PF_ProgPtr /*effect_ref*/,
                                   PF_Fixed x,
                                   PF_Fixed y,
                                   const PF_SampPB *params,
                                   PF_Pixel *dst_pixel)
{
  if (!params || !params->src || !params->src->data || !dst_pixel || PF_WORLD_IS_DEEP(params->src)) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  const int px = std::clamp(int(std::round(float(x) / 65536.0f)), 0, params->src->width - 1);
  const int py = std::clamp(int(std::round(float(y) / 65536.0f)), 0, params->src->height - 1);
  *dst_pixel = *pixel8_at(params->src, px, py);
  return PF_Err_NONE;
}

static PF_Err util_area_sample(PF_ProgPtr effect_ref,
                               PF_Fixed x,
                               PF_Fixed y,
                               const PF_SampPB *params,
                               PF_Pixel *dst_pixel)
{
  return util_subpixel_sample(effect_ref, x, y, params, dst_pixel);
}

static PF_Err util_get_pixel_data8(PF_EffectWorld *world, PF_PixelPtr pixels, PF_Pixel8 **pixPP)
{
  if (!pixPP || !world) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }
  if (PF_WORLD_IS_DEEP(world)) {
    *pixPP = nullptr;
    return PF_Err_NONE;
  }
  *pixPP = pixels ? reinterpret_cast<PF_Pixel8 *>(pixels) : reinterpret_cast<PF_Pixel8 *>(world->data);
  return PF_Err_NONE;
}

static PF_Err util_get_pixel_data16(PF_EffectWorld *world, PF_PixelPtr pixels, PF_Pixel16 **pixPP)
{
  if (!pixPP || !world) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }
  if (!PF_WORLD_IS_DEEP(world)) {
    *pixPP = nullptr;
    return PF_Err_NONE;
  }
  *pixPP = pixels ? reinterpret_cast<PF_Pixel16 *>(pixels) : reinterpret_cast<PF_Pixel16 *>(world->data);
  return PF_Err_NONE;
}

static PF_Err util_get_pixel_data_float(PF_EffectWorld *world,
                                        PF_PixelPtr /*pixels*/,
                                        PF_PixelFloat **pixPP)
{
  if (!pixPP || !world) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }
  /* Float worlds are not fully hosted yet; report depth mismatch with a null pointer. */
  *pixPP = nullptr;
  return PF_Err_NONE;
}

static PF_Err util_transfer_rect(PF_ProgPtr /*effect_ref*/,
                                 PF_Quality /*quality*/,
                                 PF_ModeFlags /*m_flags*/,
                                 PF_Field /*field*/,
                                 const PF_Rect *src_rec,
                                 const PF_EffectWorld *src_world,
                                 const PF_CompositeMode * /*comp_mode*/,
                                 const PF_MaskWorld * /*mask_world0*/,
                                 A_long dest_x,
                                 A_long dest_y,
                                 PF_EffectWorld *dst_world)
{
  if (!src_world || !dst_world) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }
  PF_Rect src = src_rec ? *src_rec : PF_Rect{0, 0, src_world->width, src_world->height};
  PF_Rect dst = {dest_x, dest_y, dest_x + (src.right - src.left), dest_y + (src.bottom - src.top)};
  return util_copy(nullptr,
                   const_cast<PF_EffectWorld *>(src_world),
                   dst_world,
                   &src,
                   &dst);
}

static PF_Err util_transform_world(PF_ProgPtr effect_ref,
                                   PF_Quality quality,
                                   PF_ModeFlags m_flags,
                                   PF_Field field,
                                   const PF_EffectWorld *src_world,
                                   const PF_CompositeMode *comp_mode,
                                   const PF_MaskWorld *mask_world0,
                                   const PF_FloatMatrix * /*matrices*/,
                                   A_long /*num_matrices*/,
                                   PF_Boolean /*src2dst_matrix*/,
                                   const PF_Rect *dest_rect,
                                   PF_EffectWorld *dst_world)
{
  /* Fallback: copy into destination region without geometric transform. */
  return util_transfer_rect(
      effect_ref, quality, m_flags, field, dest_rect, src_world, comp_mode, mask_world0, 0, 0, dst_world);
}

static PF_Err util_get_callback_addr(PF_ProgPtr /*effect_ref*/,
                                     PF_Quality /*quality*/,
                                     PF_ModeFlags /*mode_flags*/,
                                     PF_CallbackID /*which_callback*/,
                                     PF_CallbackFunc *fn_ptr)
{
  if (!fn_ptr) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }
  *fn_ptr = nullptr;
  return PF_Err_INVALID_CALLBACK;
}

static PF_Err util_unsupported(PF_ProgPtr /*effect_ref*/)
{
  return PF_Err_BAD_CALLBACK_PARAM;
}

static PF_Err util_unsupported_app(PF_ProgPtr /*effect_ref*/, A_long /*arg*/, ...)
{
  return PF_Err_BAD_CALLBACK_PARAM;
}

static PF_Err util_unsupported_sampling(PF_ProgPtr /*effect_ref*/,
                                        PF_Fixed /*x*/,
                                        PF_Fixed /*y*/,
                                        const PF_SampPB * /*params*/,
                                        PF_Pixel16 * /*dst_pixel*/)
{
  return PF_Err_BAD_CALLBACK_PARAM;
}

static PF_Err util_subpixel_sample16(PF_ProgPtr /*effect_ref*/,
                                     PF_Fixed x,
                                     PF_Fixed y,
                                     const PF_SampPB *params,
                                     PF_Pixel16 *dst_pixel)
{
  if (!params || !params->src || !params->src->data || !dst_pixel || !PF_WORLD_IS_DEEP(params->src)) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  const int px = std::clamp(int(std::round(float(x) / 65536.0f)), 0, params->src->width - 1);
  const int py = std::clamp(int(std::round(float(y) / 65536.0f)), 0, params->src->height - 1);
  *dst_pixel = *pixel16_at(params->src, px, py);
  return PF_Err_NONE;
}

static PF_Err util_area_sample16(PF_ProgPtr effect_ref,
                                 PF_Fixed x,
                                 PF_Fixed y,
                                 const PF_SampPB *params,
                                 PF_Pixel16 *dst_pixel)
{
  return util_subpixel_sample16(effect_ref, x, y, params, dst_pixel);
}

static PF_Err util_unsupported_get_platform_data(PF_ProgPtr /*effect_ref*/,
                                                 PF_PlatDataID /*which*/,
                                                 void * /*data*/)
{
  return PF_Err_BAD_CALLBACK_PARAM;
}

static PF_Err util_iterate_origin_non_clip_src(PF_InData *in_data,
                                               A_long /*progress_base*/,
                                               A_long /*progress_final*/,
                                               PF_EffectWorld *src,
                                               const PF_Rect *area,
                                               const PF_Point *origin,
                                               void *refcon,
                                               PF_IteratePixel8Func pix_fn,
                                               PF_EffectWorld *dst)
{
  if (!in_data || !src || !dst || !src->data || !dst->data || !pix_fn || PF_WORLD_IS_DEEP(src) ||
      PF_WORLD_IS_DEEP(dst))
  {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  const A_long ox = origin ? origin->h : 0;
  const A_long oy = origin ? origin->v : 0;
  PF_Rect rect = area ? *area : PF_Rect{0, 0, dst->width, dst->height};
  rect.left = std::max<A_long>(0, rect.left);
  rect.top = std::max<A_long>(0, rect.top);
  rect.right = std::min<A_long>(dst->width, rect.right);
  rect.bottom = std::min<A_long>(dst->height, rect.bottom);

  const PF_Pixel zero_pixel{};
  for (A_long y = rect.top; y < rect.bottom; y++) {
    PF_Pixel *dst_row = reinterpret_cast<PF_Pixel *>(reinterpret_cast<char *>(dst->data) + y * dst->rowbytes);
    const A_long sy = y + oy;
    const bool sy_valid = (sy >= 0 && sy < src->height);
    PF_Pixel *src_row = sy_valid ? reinterpret_cast<PF_Pixel *>(reinterpret_cast<char *>(src->data) + sy * src->rowbytes) :
                                  nullptr;
    for (A_long x = rect.left; x < rect.right; x++) {
      const A_long sx = x + ox;
      const bool in_bounds = sy_valid && (sx >= 0 && sx < src->width);
      PF_Pixel *in_pixel = in_bounds ? &src_row[sx] : const_cast<PF_Pixel *>(&zero_pixel);
      const PF_Err err = pix_fn(refcon, x, y, in_pixel, &dst_row[x]);
      if (err != PF_Err_NONE) {
        return err;
      }
    }
  }

  return PF_Err_NONE;
}

static PF_Err util_iterate16(PF_InData * /*in_data*/,
                             A_long /*progress_base*/,
                             A_long /*progress_final*/,
                             PF_EffectWorld *src,
                             const PF_Rect *area,
                             void *refcon,
                             PF_IteratePixel16Func pix_fn,
                             PF_EffectWorld *dst)
{
  if (!src || !dst || !src->data || !dst->data || !pix_fn || !PF_WORLD_IS_DEEP(src) || !PF_WORLD_IS_DEEP(dst)) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  const PF_Rect rect = area ? *area : PF_Rect{0, 0, src->width, src->height};
  for (int y = rect.top; y < rect.bottom; y++) {
    for (int x = rect.left; x < rect.right; x++) {
      PF_Err err = pix_fn(refcon, x, y, pixel16_at(src, x, y), pixel16_at(dst, x, y));
      if (err != PF_Err_NONE) {
        return err;
      }
    }
  }

  return PF_Err_NONE;
}

static PF_Err util_iterate_origin16(PF_InData *in_data,
                                    A_long progress_base,
                                    A_long progress_final,
                                    PF_EffectWorld *src,
                                    const PF_Rect *area,
                                    const PF_Point * /*origin*/,
                                    void *refcon,
                                    PF_IteratePixel16Func pix_fn,
                                    PF_EffectWorld *dst)
{
  return util_iterate16(in_data, progress_base, progress_final, src, area, refcon, pix_fn, dst);
}

static PF_Err util_iterate_origin_non_clip_src16(PF_InData *in_data,
                                                 A_long progress_base,
                                                 A_long progress_final,
                                                 PF_EffectWorld *src,
                                                 const PF_Rect *area,
                                                 const PF_Point *origin,
                                                 void *refcon,
                                                 PF_IteratePixel16Func pix_fn,
                                                 PF_EffectWorld *dst)
{
  return util_iterate_origin16(
      in_data, progress_base, progress_final, src, area, origin, refcon, pix_fn, dst);
}

static PF_Err util_iterate_generic(A_long iterationsL,
                                   void *refconPV,
                                   PF_Err (*fn_func)(void *refconPV,
                                                     A_long thread_indexL,
                                                     A_long i,
                                                     A_long iterationsL))
{
  if (!fn_func) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  A_long iterations = std::max<A_long>(iterationsL, 0);
  if (iterationsL == PF_Iterations_ONCE_PER_PROCESSOR) {
    const unsigned int hw_threads = std::max(1u, std::thread::hardware_concurrency());
    iterations = A_long(hw_threads);
  }

  for (A_long i = 0; i < iterations; i++) {
    PF_Err err = fn_func(refconPV, 0, i, iterations);
    if (err != PF_Err_NONE) {
      return err;
    }
  }
  return PF_Err_NONE;
}

static std::optional<int2> find_render_size(const compositor::Context &context,
                                            const std::vector<RenderValue> &values)
{
  for (const RenderValue &value : values) {
    if (value.image && !value.image->is_single_value()) {
      return value.image->domain().data_size;
    }
  }

  const int2 compositing_size = context.get_compositing_domain().data_size;
  if (compositing_size.x > 0 && compositing_size.y > 0) {
    return compositing_size;
  }

  for (const RenderValue &value : values) {
    if (value.image) {
      return value.image->domain().data_size;
    }
  }
  return std::nullopt;
}

static const RenderValue *find_value(const std::vector<RenderValue> &values, const StringRef identifier)
{
  for (const RenderValue &value : values) {
    if (value.identifier == identifier) {
      return &value;
    }
  }
  return nullptr;
}

static PF_RationalScale pixel_aspect_ratio_from_render_data(const RenderData &render_data)
{
  const double xasp = std::max(0.00001, double(render_data.xasp));
  const double yasp = std::max(0.00001, double(render_data.yasp));

  PF_RationalScale ratio{};
  const long long x_scaled = std::llround(xasp * 100000.0);
  const long long y_scaled = std::llround(yasp * 100000.0);
  ratio.num = A_long(std::clamp(x_scaled, 1LL, 1000000000LL));
  ratio.den = A_long(std::clamp(y_scaled, 1LL, 1000000000LL));
  return ratio;
}

static void initialize_util_callbacks(PF_UtilCallbacks &utils)
{
  std::memset(&utils, 0, sizeof(utils));
  utils.begin_sampling = util_begin_sampling;
  utils.subpixel_sample = util_subpixel_sample;
  utils.area_sample = util_area_sample;
  utils.end_sampling = util_end_sampling;
  utils.copy = util_copy;
  utils.composite_rect = util_composite_rect;
  utils.fill = util_fill;
  utils.blend = util_blend;
  utils.convolve = util_convolve;
  utils.gaussian_kernel = util_gaussian_kernel;
  utils.iterate = util_iterate;
  utils.premultiply = util_premultiply;
  utils.premultiply_color = util_premultiply_color;
  utils.new_world = util_new_world;
  utils.dispose_world = util_dispose_world;
  utils.iterate_origin = util_iterate_origin;
  utils.iterate_lut = util_iterate_lut;
  utils.transfer_rect = util_transfer_rect;
  utils.transform_world = util_transform_world;
  utils.host_new_handle = util_host_new_handle;
    utils.get_callback_addr = util_get_callback_addr;
  utils.host_lock_handle = util_host_lock_handle;
  utils.host_unlock_handle = util_host_unlock_handle;
  utils.host_dispose_handle = util_host_dispose_handle;
  utils.get_platform_data = util_unsupported_get_platform_data;
  utils.host_get_handle_size = util_host_get_handle_size;
  utils.iterate_origin_non_clip_src = util_iterate_origin_non_clip_src;
  utils.iterate_generic = util_iterate_generic;
  utils.host_resize_handle = util_host_resize_handle;
  utils.subpixel_sample16 = util_subpixel_sample16;
  utils.area_sample16 = util_area_sample16;
  utils.fill16 = util_fill16;
  utils.premultiply_color16 = util_premultiply_color16;
  utils.iterate16 = util_iterate16;
  utils.iterate_origin16 = util_iterate_origin16;
  utils.iterate_origin_non_clip_src16 = util_iterate_origin_non_clip_src16;
  utils.get_pixel_data8 = util_get_pixel_data8;
  utils.get_pixel_data16 = util_get_pixel_data16;
  utils.app = util_unsupported_app;

  utils.ansi.atan = std::atan;
  utils.ansi.atan2 = std::atan2;
  utils.ansi.ceil = std::ceil;
  utils.ansi.cos = std::cos;
  utils.ansi.exp = std::exp;
  utils.ansi.fabs = std::fabs;
  utils.ansi.floor = std::floor;
  utils.ansi.fmod = std::fmod;
  utils.ansi.hypot = std::hypot;
  utils.ansi.log = std::log;
  utils.ansi.log10 = std::log10;
  utils.ansi.pow = std::pow;
  utils.ansi.sin = std::sin;
  utils.ansi.sqrt = std::sqrt;
  utils.ansi.tan = std::tan;
  utils.ansi.sprintf = std::sprintf;
  utils.ansi.strcpy = std::strcpy;
  utils.ansi.asin = std::asin;
  utils.ansi.acos = std::acos;
}
#endif

}  // namespace

bool host_is_available()
{
#if WITH_AFTER_EFFECTS_SDK_HEADERS
  return true;
#else
  return false;
#endif
}

PluginMetadata parse_plugin_metadata(const StringRef filepath)
{
  PluginMetadata metadata;
  metadata.host_available = host_is_available();

  if (filepath.is_empty()) {
    metadata.error = "No plugin path selected";
    return metadata;
  }

  char absolute_path[FILE_MAX];
  resolve_absolute_path(filepath, absolute_path);
  metadata.absolute_path = absolute_path;

  if (!BLI_exists(absolute_path)) {
    metadata.error = "Plugin file does not exist";
    return metadata;
  }

  DynamicLibrary library(absolute_path);
  if (!library) {
    metadata.error = "Failed to load plugin library";
    return metadata;
  }

#if WITH_AFTER_EFFECTS_SDK_HEADERS
  extract_plugin_metadata_from_entrypoints(library, metadata);
  std::string parse_error;
  if (!parse_parameters_with_effect_main(library, metadata, metadata, parse_error)) {
    metadata.error = parse_error.empty() ? "Failed to parse plugin parameters" : parse_error;
    return metadata;
  }

  metadata.signature_hash = BLI_ghashutil_strhash_p(metadata.absolute_path.c_str());
  metadata.success = true;
  return metadata;
#else
  metadata.error = "After Effects SDK headers are unavailable at build time";
  return metadata;
#endif
}

bool render_plugin(compositor::Context &context,
                   const StringRef filepath,
                   const std::vector<RenderValue> &values,
                   compositor::Result &r_output,
                   compositor::Domain &r_output_domain,
                   std::string &r_error)
{
#if WITH_AFTER_EFFECTS_SDK_HEADERS
  if (values.empty()) {
    r_error = "No input values provided";
    return false;
  }

  const std::optional<int2> size = find_render_size(context, values);
  if (!size) {
    r_error = "No image input provided";
    return false;
  }
  ae_host_debug_log("chosen render size: %dx%d", size->x, size->y);

  char absolute_path[FILE_MAX];
  resolve_absolute_path(filepath, absolute_path);
  ae_host_debug_log("render begin: path=%s", absolute_path);
  DynamicLibrary library(absolute_path);
  if (!library) {
    ae_host_debug_log("library load failed: path=%s", absolute_path);
    r_error = "Failed to load plugin library";
    return false;
  }

  PluginMetadata metadata = parse_plugin_metadata(filepath);
  if (!metadata.success) {
    ae_host_debug_log("metadata parse failed: %s", metadata.error.c_str());
    r_error = metadata.error;
    return false;
  }
  ae_host_debug_log("metadata ok: effect=%s entry=%s params=%zu smart=%d float=%d deep=%d",
                    metadata.effect_name.c_str(),
                    metadata.entry_point.c_str(),
                    metadata.parameters.size(),
                    metadata.supports_smart_render ? 1 : 0,
                    metadata.supports_float_color ? 1 : 0,
                    metadata.supports_deep_color ? 1 : 0);

  EffectMainFn effect_main = nullptr;
  std::vector<std::string> candidate_entry_points;
  if (!metadata.entry_point.empty()) {
    candidate_entry_points.push_back(metadata.entry_point);
    effect_main = reinterpret_cast<EffectMainFn>(library.find_symbol(metadata.entry_point.c_str()));
  }
  if (!effect_main) {
    candidate_entry_points.push_back("EffectMain");
    effect_main = reinterpret_cast<EffectMainFn>(library.find_symbol("EffectMain"));
  }
  if (!effect_main) {
    candidate_entry_points.push_back("EntryPointFunc");
    effect_main = reinterpret_cast<EffectMainFn>(library.find_symbol("EntryPointFunc"));
  }
  if (!effect_main) {
    candidate_entry_points.push_back("main");
    effect_main = reinterpret_cast<EffectMainFn>(library.find_symbol("main"));
  }
  if (!effect_main) {
    std::string tried;
    for (const std::string &entry_name : candidate_entry_points) {
      if (!tried.empty()) {
        tried += ", ";
      }
      tried += entry_name;
    }
    r_error = "Plugin effect entry point not found (tried: " + tried + ")";
    return false;
  }
  ae_host_debug_log("selected entry point: %s", metadata.entry_point.c_str());

  HostRenderState state;
  state.image_size = *size;
  state.pixel_aspect_ratio = pixel_aspect_ratio_from_render_data(context.get_render_data());
  initialize_util_callbacks(state.utils);
  initialize_pica_basic_suite(state.pica_basic);
  initialize_handle_suite(state.handle_suite);
  initialize_ansi_suite(state.ansi_suite);
  initialize_pixel_data_suite(state.pixel_data_suite);
  initialize_color_suite(state.color_suite);
  initialize_world_suite1(state.world_suite_v1);
  initialize_world_suite2(state.world_suite_v2);
  initialize_iterate8_suite1(state.iterate8_suite_v1);
  initialize_iterate8_suite2(state.iterate8_suite_v2);
  initialize_iterate16_suite1(state.iterate16_suite_v1);
  initialize_iterate16_suite2(state.iterate16_suite_v2);
  initialize_pixel_format_suite2(state.pixel_format_suite);
  register_common_suites(state.suite_registry,
                         state.handle_suite,
                         state.ansi_suite,
                         state.pixel_data_suite,
                         state.color_suite,
                         state.world_suite_v1,
                         state.world_suite_v2,
                         state.iterate8_suite_v1,
                         state.iterate8_suite_v2,
                         state.iterate16_suite_v1,
                         state.iterate16_suite_v2,
                         state.pixel_format_suite);

  int max_param_index = 0;
  for (const PluginParameter &parameter : metadata.parameters) {
    max_param_index = std::max(max_param_index, parameter.index);
  }

  state.params.resize(max_param_index + 1);
  state.layer_buffers.resize(max_param_index + 1);

  const RenderValue *main_input_value = find_value(values, "IMAGE");
  if (!main_input_value || !main_input_value->image) {
    r_error = "Main image input is missing";
    return false;
  }

  /* Fill the source layer (params[0]) immediately.
   * This MUST happen before output_layer is initialised (it copies from layer_buffers[0])
   * and before GLOBAL_SETUP (some plugins inspect params[0] there). */
  {
    PF_ParamDef &input_param = state.params[0];
    std::memset(&input_param, 0, sizeof(input_param));
    input_param.param_type = PF_Param_LAYER;
    STRNCPY(input_param.PF_DEF_NAME, "Image");
    fill_layer_from_result(
        *main_input_value->image, state.image_size, state.pixel_aspect_ratio, state.layer_buffers[0]);
    input_param.u.ld = state.layer_buffers[0].world;
  }

  std::vector<PF_ParamDef *> param_ptrs(state.params.size());
  for (int i = 0; i < state.params.size(); i++) {
    param_ptrs[i] = &state.params[i];
  }

  HostLayerBuffer output_layer;
  output_layer.pixels.resize(state.image_size.x * state.image_size.y);
  initialize_world(
      output_layer.world, state.image_size, output_layer.pixels.data(), false, state.pixel_aspect_ratio);

  /* Start from input copy so effects that only touch regions remain valid. */
  output_layer.pixels = state.layer_buffers[0].pixels;
  output_layer.world.data = reinterpret_cast<PF_PixelPtr>(output_layer.pixels.data());

  initialize_base_in_data(state.in_data,
                          reinterpret_cast<PF_ProgPtr>(&state),
                          state.image_size.x,
                          state.image_size.y,
                          state.params.size(),
                          state.pixel_aspect_ratio,
                          &state.utils,
                          &state.pica_basic);
  state.in_data.inter.checkout_param = render_checkout_param;
  state.in_data.inter.checkin_param = render_checkin_param;
  state.in_data.inter.add_param = render_add_param;
  state.in_data.inter.abort = render_abort;
  state.in_data.inter.progress = render_progress;
  state.in_data.inter.register_ui = render_register_ui;
  state.in_data.inter.checkout_layer_audio = render_checkout_layer_audio;
  state.in_data.inter.checkin_layer_audio = render_checkin_layer_audio;
  state.in_data.inter.get_audio_data = render_get_audio_data;

  state.out_data.num_params = state.params.size();
  PF_Err command_err = PF_Err_NONE;
  ScopedActiveSuiteRegistry active_registry_scope(state.suite_registry);

  auto call_command =
      [&](const PF_Cmd cmd, const char *name, const bool fatal, const bool allow_bad_callback_param) -> bool {
    const bool called = safe_call_effect_main(effect_main,
                                              cmd,
                                              &state.in_data,
                                              &state.out_data,
                                              param_ptrs.data(),
                                              &output_layer.world,
                                              nullptr,
                                              command_err);
    const bool ok = called && command_err == PF_Err_NONE;
    const bool tolerated_bad_callback = called && allow_bad_callback_param &&
                                        command_err == PF_Err_BAD_CALLBACK_PARAM;
    ae_host_debug_log("cmd=%s called=%d err=%d fatal=%d tolerated_bad_callback=%d",
                      name,
                      called ? 1 : 0,
                      int(command_err),
                      fatal ? 1 : 0,
                      tolerated_bad_callback ? 1 : 0);
    if (ok) {
      sync_persistent_plugin_data(state.in_data, state.out_data);
      return true;
    }
    if (tolerated_bad_callback) {
      return true;
    }
    if (fatal) {
      r_error = std::string("Plugin failed during ") + name + " (err " + std::to_string(int(command_err)) + ")";
      return false;
    }
    return false;
  };

  const bool global_setup_ok = call_command(
      PF_Cmd_GLOBAL_SETUP, "PF_Cmd_GLOBAL_SETUP", false, true);
  const bool params_setup_ok = call_command(
      PF_Cmd_PARAMS_SETUP, "PF_Cmd_PARAMS_SETUP", false, true);
  ae_host_debug_log("setup summary: global_setup_ok=%d params_setup_ok=%d",
                    global_setup_ok ? 1 : 0,
                    params_setup_ok ? 1 : 0);

  /* Apply user values to params[1..N] AFTER PARAMS_SETUP.
   * render_add_param (called by the plugin during PARAMS_SETUP) resets these to plugin
   * defaults; we must override them here so RENDER sees the actual user-supplied values.
   * params[0] (source layer) was already set above; re-assert it here too in case the
   * plugin's PARAMS_SETUP unexpectedly called render_add_param(0, ...). */
  {
    /* Re-assert source layer in case PARAMS_SETUP clobbered it. */
    PF_ParamDef &input_param = state.params[0];
    std::memset(&input_param, 0, sizeof(input_param));
    input_param.param_type = PF_Param_LAYER;
    STRNCPY(input_param.PF_DEF_NAME, "Image");
    input_param.u.ld = state.layer_buffers[0].world;

    for (const PluginParameter &parameter : metadata.parameters) {
      if (parameter.index <= 0 || parameter.index >= int(state.params.size())) {
        continue;
      }

      if (parameter.type == ParamType::Unsupported) {
        ae_host_debug_log("skipping unsupported param: index=%d name=%s ae_type=%d",
                          parameter.index,
                          parameter.name.c_str(),
                          parameter.ae_param_type);
        continue;
      }

      PF_ParamDef &param = state.params[parameter.index];
      std::memset(&param, 0, sizeof(param));
      param.uu.id = parameter.disk_id;
      param.param_type = parameter.ae_param_type;
      STRNCPY(param.PF_DEF_NAME, parameter.name.c_str());

      const RenderValue *value = find_value(values, parameter.identifier);

      switch (parameter.ae_param_type) {
        case PF_Param_LAYER:
          if (value && value->image) {
            fill_layer_from_result(*value->image,
                                   state.image_size,
                                   state.pixel_aspect_ratio,
                                   state.layer_buffers[parameter.index]);
            param.u.ld = state.layer_buffers[parameter.index].world;
          }
          else {
            param.u.ld.dephault = PF_LayerDefault_NONE;
            param.u.ld.data = nullptr;
          }
          break;
        case PF_Param_SLIDER:
          param.u.sd.value = value ? value->integer : int(parameter.default_value);
          param.u.sd.dephault = int(parameter.default_value);
          param.u.sd.valid_min = int(parameter.min_value);
          param.u.sd.valid_max = int(parameter.max_value);
          break;
        case PF_Param_FIX_SLIDER:
          param.u.fd.value = PF_Fixed((value ? value->scalar : parameter.default_value) * 65536.0f);
          param.u.fd.dephault = PF_Fixed(parameter.default_value * 65536.0f);
          param.u.fd.valid_min = PF_Fixed(parameter.min_value * 65536.0f);
          param.u.fd.valid_max = PF_Fixed(parameter.max_value * 65536.0f);
          break;
        case PF_Param_FLOAT_SLIDER:
          param.u.fs_d.value = value ? value->scalar : parameter.default_value;
          param.u.fs_d.dephault = parameter.default_value;
          param.u.fs_d.valid_min = parameter.min_value;
          param.u.fs_d.valid_max = parameter.max_value;
          break;
        case PF_Param_ANGLE:
          param.u.ad.value = PF_Fixed((value ? value->scalar : parameter.default_value) * 65536.0f);
          param.u.ad.dephault = PF_Fixed(parameter.default_value * 65536.0f);
          break;
        case PF_Param_CHECKBOX:
          param.u.bd.value = value ? (value->boolean ? 1 : 0) : int(parameter.default_value >= 0.5f);
          param.u.bd.dephault = param.u.bd.value ? 1 : 0;
          break;
        case PF_Param_COLOR: {
          const float4 color = value ? value->vector : parameter.vector_default;
          param.u.cd.value.red = uchar(clamp_byte(color.x));
          param.u.cd.value.green = uchar(clamp_byte(color.y));
          param.u.cd.value.blue = uchar(clamp_byte(color.z));
          param.u.cd.value.alpha = uchar(clamp_byte(color.w));
          param.u.cd.dephault = param.u.cd.value;
          break;
        }
        case PF_Param_POINT: {
          const float4 point = value ? value->vector : parameter.vector_default;
          param.u.td.x_value = PF_Fixed(point.x * 65536.0f);
          param.u.td.y_value = PF_Fixed(point.y * 65536.0f);
          param.u.td.x_dephault = param.u.td.x_value;
          param.u.td.y_dephault = param.u.td.y_value;
          break;
        }
        case PF_Param_POINT_3D: {
          const float4 point = value ? value->vector : parameter.vector_default;
          param.u.point3d_d.x_value = point.x;
          param.u.point3d_d.y_value = point.y;
          param.u.point3d_d.z_value = point.z;
          param.u.point3d_d.x_dephault = point.x;
          param.u.point3d_d.y_dephault = point.y;
          param.u.point3d_d.z_dephault = point.z;
          break;
        }
        case PF_Param_POPUP:
          param.u.pd.value = value ? value->integer : int(parameter.default_value);
          param.u.pd.dephault = int(parameter.default_value);
          param.u.pd.num_choices = std::max(1, int(parameter.popup_items.size()));
          break;
        default:
          break;
      }
    }
  }

  const bool sequence_setup_ok =
      call_command(PF_Cmd_SEQUENCE_SETUP, "PF_Cmd_SEQUENCE_SETUP", false, false);
  const bool frame_setup_ok = call_command(PF_Cmd_FRAME_SETUP, "PF_Cmd_FRAME_SETUP", false, false);

  if (!call_command(PF_Cmd_RENDER, "PF_Cmd_RENDER", true, false)) {
    return false;
  }

  if (frame_setup_ok) {
    safe_call_effect_main(effect_main,
                          PF_Cmd_FRAME_SETDOWN,
                          &state.in_data,
                          &state.out_data,
                          param_ptrs.data(),
                          &output_layer.world,
                          nullptr,
                          command_err);
    ae_host_debug_log("cmd=PF_Cmd_FRAME_SETDOWN called=1 err=%d", int(command_err));
  }
  if (sequence_setup_ok) {
    safe_call_effect_main(effect_main,
                          PF_Cmd_SEQUENCE_SETDOWN,
                          &state.in_data,
                          &state.out_data,
                          param_ptrs.data(),
                          &output_layer.world,
                          nullptr,
                          command_err);
    ae_host_debug_log("cmd=PF_Cmd_SEQUENCE_SETDOWN called=1 err=%d", int(command_err));
  }
  if (global_setup_ok) {
    safe_call_effect_main(effect_main,
                          PF_Cmd_GLOBAL_SETDOWN,
                          &state.in_data,
                          &state.out_data,
                          param_ptrs.data(),
                          &output_layer.world,
                          nullptr,
                          command_err);
    ae_host_debug_log("cmd=PF_Cmd_GLOBAL_SETDOWN called=1 err=%d", int(command_err));
  }

  uint64_t input_hash = 1469598103934665603ULL;
  uint64_t output_hash = 1469598103934665603ULL;
  const size_t hash_count = std::min<size_t>(state.layer_buffers[0].pixels.size(), 1024);
  for (size_t i = 0; i < hash_count; i++) {
    const PF_Pixel &p = state.layer_buffers[0].pixels[i];
    input_hash ^= uint64_t(p.red) | (uint64_t(p.green) << 8) | (uint64_t(p.blue) << 16) |
                  (uint64_t(p.alpha) << 24);
    input_hash *= 1099511628211ULL;
  }
  const size_t out_hash_count = std::min<size_t>(output_layer.pixels.size(), 1024);
  for (size_t i = 0; i < out_hash_count; i++) {
    const PF_Pixel &p = output_layer.pixels[i];
    output_hash ^= uint64_t(p.red) | (uint64_t(p.green) << 8) | (uint64_t(p.blue) << 16) |
                   (uint64_t(p.alpha) << 24);
    output_hash *= 1099511628211ULL;
  }
  ae_host_debug_log("output hash: input=%llu output=%llu identical=%d",
                    static_cast<unsigned long long>(input_hash),
                    static_cast<unsigned long long>(output_hash),
                    input_hash == output_hash ? 1 : 0);

  const compositor::Domain output_domain(state.image_size);
  r_output.allocate_texture(output_domain, true, compositor::ResultStorageType::CPU);
  for (int y = 0; y < state.image_size.y; y++) {
    for (int x = 0; x < state.image_size.x; x++) {
      const PF_Pixel &pixel = output_layer.pixels[y * state.image_size.x + x];
      r_output.store_pixel<compositor::Color>(int2(x, y), to_compositor_color(pixel));
    }
  }

  r_output_domain = output_domain;
  return true;
#else
  if (filepath.is_empty()) {
    r_error = "No plugin path selected";
    return false;
  }

  for (const RenderValue &value : values) {
    if (value.image == nullptr) {
      continue;
    }
    r_output.share_data(*value.image);
    r_output_domain = value.image->domain();
    return true;
  }

  r_error = "Plugin render fallback needs an input image";
  return false;
#endif
}

}  // namespace blender::nodes::after_effects
