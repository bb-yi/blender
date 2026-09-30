/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#include <climits>
#include <cstdlib>
#include <cfloat>
#include <algorithm>
#include <cmath>

#include "BLI_math_rotation.h"

#include "BLT_translation.hh"

#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_types.hh"
#include "rna_internal.hh"

#include "DNA_light_types.h"

#include "IMB_colormanagement.hh"

namespace blender {
const EnumPropertyItem rna_enum_light_shader_parameter_type_items[] = {
    {LIGHT_SHADER_PARAMETER_FLOAT, "FLOAT", 0, "Float", "Floating-point value"},
    {LIGHT_SHADER_PARAMETER_INT, "INT", 0, "Integer", "Exact integer between -16777216 and 16777216"},
    {LIGHT_SHADER_PARAMETER_BOOL, "BOOLEAN", 0, "Boolean", "True or false"},
    {LIGHT_SHADER_PARAMETER_VECTOR2, "VECTOR2", 0, "Vector 2D", "Two floating-point components"},
    {LIGHT_SHADER_PARAMETER_VECTOR3, "VECTOR3", 0, "Vector 3D", "Three floating-point components"},
    {LIGHT_SHADER_PARAMETER_VECTOR4, "VECTOR4", 0, "Vector 4D", "Four floating-point components"},
    {LIGHT_SHADER_PARAMETER_COLOR, "COLOR", 0, "Color", "Scene-linear RGBA color"},
    {0, nullptr, 0, nullptr, nullptr},
};
static const EnumPropertyItem light_parameter_subtypes[] = {
    {PROP_NONE, "NONE", 0, "Number", "Plain numeric values"},
    {PROP_FACTOR, "FACTOR", 0, "Factor", "Factor presentation; range is configured separately"},
    {PROP_ANGLE, "ANGLE", 0, "Angle", "Display angles in degrees; stored and read by shaders in radians"},
    {PROP_DISTANCE, "DISTANCE", 0, "Distance", "Display using scene units; keep the stored value unchanged"},
    {PROP_XYZ, "XYZ", 0, "XYZ", "Vector component presentation"},
    {0, nullptr, 0, nullptr, nullptr},
};
}

#ifdef RNA_RUNTIME

#  include "MEM_guardedalloc.h"

#  include "BLI_math_matrix_types.hh"
#  include "BLI_listbase.h"
#  include "BLI_string.h"
#  include "BLI_string_utf8.h"
#  include "BLI_math_vector.h"

#  include "BKE_context.hh"
#  include "BKE_global.hh"
#  include "BKE_light.h"
#  include "BKE_library.hh"
#  include "BKE_main.hh"
#  include "BKE_main_invariants.hh"
#  include "BKE_node.hh"
#  include "BKE_node_legacy_types.hh"
#  include "BKE_node_tree_update.hh"
#  include "BKE_report.hh"
#  include "BKE_lib_id.hh"
#  include "BKE_texture.h"

#  include "DNA_object_types.h"
#  include "DNA_node_types.h"

#  include "DEG_depsgraph.hh"
#  include "DEG_depsgraph_build.hh"

#  include "NOD_defaults.hh"

#  include "WM_api.hh"
#  include "WM_types.hh"


namespace blender {

static void rna_Light_update_and_notify(Main * /*bmain*/, Light *la, const uint notifier)
{
  DEG_id_tag_update(&la->id, 0);
  WM_main_add_notifier(notifier, la);
}

static StructRNA *rna_Light_refine(PointerRNA *ptr)
{
  Light *la = static_cast<Light *>(ptr->data);

  switch (la->type) {
    case LA_LOCAL:
      return RNA_PointLight;
    case LA_SUN:
      return RNA_SunLight;
    case LA_SPOT:
      return RNA_SpotLight;
    case LA_AREA:
      return RNA_AreaLight;
    default:
      return RNA_Light;
  }
}

static void rna_Light_update(Main *bmain, Scene * /*scene*/, PointerRNA *ptr)
{
  Light *la = id_cast<Light *>(ptr->owner_id);
  rna_Light_update_and_notify(bmain, la, NC_LAMP | ND_LIGHTING);
}

static void rna_Light_draw_update(Main *bmain, Scene * /*scene*/, PointerRNA *ptr)
{
  Light *la = id_cast<Light *>(ptr->owner_id);
  rna_Light_update_and_notify(bmain, la, NC_LAMP | ND_LIGHTING_DRAW);
}

static bool rna_Light_use_nodes_get(PointerRNA * /*ptr*/)
{
  /* #use_nodes is deprecated. All lights now use nodes. */
  return true;
}

static void rna_Light_use_nodes_set(PointerRNA * /*ptr*/, bool /*new_value*/)
{
  /* #use_nodes is deprecated. Setting the property has no effect.
   * Note: Users will get a warning through the RNA deprecation warning, so no need to log a
   * warning here. */
  return;
}

static void rna_Light_temperature_color_get(PointerRNA *ptr, float *color)
{
  Light *la = static_cast<Light *>(ptr->data);

  if (la->mode & LA_USE_TEMPERATURE) {
    float rgb[4];
    IMB_colormanagement_blackbody_temperature_to_rgb(rgb, la->temperature);

    color[0] = rgb[0];
    color[1] = rgb[1];
    color[2] = rgb[2];
  }
  else {
    copy_v3_fl(color, 1.0f);
  }
}

static float rna_Light_area(Light *light, const float matrix_world[16])
{
  float4x4 mat(matrix_world);
  return BKE_light_area(*light, mat);
}

static void light_parameter_layout_update(Main *bmain, Light *light)
{
  if (bmain) {
    FOREACH_NODETREE_BEGIN (bmain, tree, owner) {
      for (bNode &node : tree->nodes) {
        if (node.type_legacy != SH_NODE_LIGHT_INFO || !node.id) {
          continue;
        }
        const Object *object = reinterpret_cast<const Object *>(node.id);
        if (object->type == OB_LAMP && object->data == &light->id) {
          BKE_ntree_update_tag_node_property(tree, &node);
        }
      }
    }
    FOREACH_NODETREE_END;
    BKE_main_ensure_invariants(*bmain);
  }
  rna_Light_update_and_notify(bmain, light, NC_LAMP | ND_LIGHTING);
}

static void rna_LightShaderParameter_layout_update(Main *bmain, Scene *, PointerRNA *ptr)
{
  light_parameter_layout_update(bmain, id_cast<Light *>(ptr->owner_id));
}

static StructRNA *rna_LightShaderParameter_refine(PointerRNA *ptr)
{
  const auto &p = *static_cast<const LightShaderParameter *>(ptr->data);
  if (p.type == LIGHT_SHADER_PARAMETER_FLOAT) {
    switch (p.subtype) {
      case PROP_FACTOR: return RNA_LightShaderParameterFactor;
      case PROP_ANGLE: return RNA_LightShaderParameterAngle;
      case PROP_DISTANCE: return RNA_LightShaderParameterDistance;
    }
  }
  if (ELEM(p.type, LIGHT_SHADER_PARAMETER_VECTOR2, LIGHT_SHADER_PARAMETER_VECTOR3,
           LIGHT_SHADER_PARAMETER_VECTOR4) && p.subtype == PROP_XYZ)
  {
    return RNA_LightShaderParameterVector;
  }
  return RNA_LightShaderParameterNumber;
}

static const EnumPropertyItem *rna_LightShaderParameter_subtype_itemf(
    bContext *, PointerRNA *ptr, PropertyRNA *, bool *r_free)
{
  const auto &p = *static_cast<const LightShaderParameter *>(ptr->data);
  EnumPropertyItem *items = nullptr;
  int count = 0;
  RNA_enum_items_add_value(&items, &count, light_parameter_subtypes, PROP_NONE);
  if (p.type == LIGHT_SHADER_PARAMETER_FLOAT) {
    for (int subtype : {PROP_FACTOR, PROP_ANGLE, PROP_DISTANCE}) {
      RNA_enum_items_add_value(&items, &count, light_parameter_subtypes, subtype);
    }
  }
  else if (ELEM(p.type, LIGHT_SHADER_PARAMETER_VECTOR2, LIGHT_SHADER_PARAMETER_VECTOR3,
                LIGHT_SHADER_PARAMETER_VECTOR4))
  {
    RNA_enum_items_add_value(&items, &count, light_parameter_subtypes, PROP_XYZ);
  }
  RNA_enum_item_end(&items, &count);
  *r_free = true;
  return items;
}

static void rna_LightShaderParameter_type_set(PointerRNA *ptr, int value)
{
  auto &p = *static_cast<LightShaderParameter *>(ptr->data);
  p.type = eLightShaderParameterType(value);
  if (!((p.type == LIGHT_SHADER_PARAMETER_FLOAT && ELEM(p.subtype, PROP_FACTOR, PROP_ANGLE, PROP_DISTANCE)) ||
        (ELEM(p.type, LIGHT_SHADER_PARAMETER_VECTOR2, LIGHT_SHADER_PARAMETER_VECTOR3,
              LIGHT_SHADER_PARAMETER_VECTOR4) && p.subtype == PROP_XYZ))) {
    p.subtype = PROP_NONE;
  }
}

static void rna_LightShaderParameter_float_range(
    PointerRNA *ptr, float *min, float *max, float *softmin, float *softmax)
{
  const auto &p = *static_cast<const LightShaderParameter *>(ptr->data);
  const float lower = p.type == LIGHT_SHADER_PARAMETER_COLOR ? 0.0f : -FLT_MAX;
  *min = p.use_hard_limits ? std::max(lower, p.range_min) : lower;
  *max = p.use_hard_limits ? std::max(*min, p.range_max) : FLT_MAX;
  *softmin = std::clamp(p.range_min, *min, *max);
  *softmax = std::clamp(p.range_max, *softmin, *max);
}

static void rna_LightShaderParameter_int_range(
    PointerRNA *ptr, int *min, int *max, int *softmin, int *softmax)
{
  const auto &p = *static_cast<const LightShaderParameter *>(ptr->data);
  const int lo = int(std::ceil(std::clamp(p.range_min, -16777216.0f, 16777216.0f)));
  const int hi = std::max(lo, int(std::floor(std::clamp(p.range_max, -16777216.0f, 16777216.0f))));
  *min = p.use_hard_limits ? lo : -16777216;
  *max = p.use_hard_limits ? hi : 16777216;
  *softmin = lo;
  *softmax = hi;
}

static const char *rna_LightShaderParameter_value_description(
    const PointerRNA *ptr, const PropertyRNA *, bool)
{
  const auto &p = *static_cast<const LightShaderParameter *>(ptr->data);
  return p.description[0] ? p.description : "Light shader parameter value";
}

static void rna_LightShaderParameter_min_set(PointerRNA *ptr, float value)
{
  if (!std::isfinite(value)) { return; }
  auto &p = *static_cast<LightShaderParameter *>(ptr->data);
  p.range_min = value;
  p.range_max = std::max(value, p.range_max);
}

static void rna_LightShaderParameter_max_set(PointerRNA *ptr, float value)
{
  if (!std::isfinite(value)) { return; }
  auto &p = *static_cast<LightShaderParameter *>(ptr->data);
  p.range_max = value;
  p.range_min = std::min(value, p.range_min);
}

static void rna_LightShaderParameter_limits_update(Main *bmain, Scene *scene, PointerRNA *ptr)
{
  auto &p = *static_cast<LightShaderParameter *>(ptr->data);
  if (p.use_hard_limits && p.type != LIGHT_SHADER_PARAMETER_BOOL) {
    float lo, hi, softlo, softhi;
    rna_LightShaderParameter_float_range(ptr, &lo, &hi, &softlo, &softhi);
    switch (p.type) {
      case LIGHT_SHADER_PARAMETER_FLOAT: p.value_float = std::clamp(p.value_float, lo, hi); break;
      case LIGHT_SHADER_PARAMETER_INT: {
        int il, ih, sl, sh;
        rna_LightShaderParameter_int_range(ptr, &il, &ih, &sl, &sh);
        p.value_int = std::clamp(p.value_int, il, ih);
        break;
      }
      case LIGHT_SHADER_PARAMETER_VECTOR2:
        for (float &v : p.value_vector2) { v = std::clamp(v, lo, hi); } break;
      case LIGHT_SHADER_PARAMETER_VECTOR3:
        for (float &v : p.value_vector3) { v = std::clamp(v, lo, hi); } break;
      case LIGHT_SHADER_PARAMETER_VECTOR4:
        for (float &v : p.value_vector4) { v = std::clamp(v, lo, hi); } break;
      case LIGHT_SHADER_PARAMETER_COLOR:
        for (float &v : p.value_color) { v = std::clamp(v, lo, hi); } break;
      default: break;
    }
  }
  rna_LightShaderParameter_layout_update(bmain, scene, ptr);
}

static std::optional<std::string> rna_LightShaderParameter_path(const PointerRNA *ptr)
{
  const auto *parameter = static_cast<const LightShaderParameter *>(ptr->data);
  return "shader_parameters[\"" + std::string(parameter->identifier) + "\"]";
}

static void rna_LightShaderParameter_name_set(PointerRNA *ptr, const char *value)
{
  Light *light = id_cast<Light *>(ptr->owner_id);
  auto *parameter = static_cast<LightShaderParameter *>(ptr->data);
  char name[sizeof(parameter->name)];
  BLI_strncpy_utf8(name, value, sizeof(name));
  if (name[0] == '\0') {
    WM_global_report(RPT_ERROR, "Shader parameter names cannot be empty");
    return;
  }
  for (const LightShaderParameter &other : light->shader_parameters) {
    if (&other != parameter && STREQ(other.name, name)) {
      WM_global_report(RPT_ERROR, "A shader parameter with this name already exists");
      return;
    }
  }
  BLI_strncpy_utf8(parameter->name, name, sizeof(parameter->name));
}

/* String collection keys are immutable identifiers, not display/shader names. */
static bool rna_Light_shader_parameters_lookupstring(PointerRNA *ptr,
                                                     const char *key,
                                                     PointerRNA *r_ptr)
{
  auto *light = static_cast<Light *>(ptr->data);
  for (LightShaderParameter &parameter : light->shader_parameters) {
    if (STREQ(parameter.identifier, key)) {
      *r_ptr = RNA_pointer_create_with_parent(*ptr, RNA_LightShaderParameter, &parameter);
      return true;
    }
  }
  return false;
}

static bool rna_Light_shader_parameters_editable(Light *light, ReportList *reports)
{
  if (!ID_IS_EDITABLE(light) || ID_IS_OVERRIDE_LIBRARY(light)) {
    BKE_report(reports, RPT_ERROR, "Shader parameter layout requires a local Light data-block");
    return false;
  }
  return true;
}

static LightShaderParameter *rna_Light_shader_parameter_new(Light *light,
                                                           ReportList *reports,
                                                           const char *name,
                                                           int type)
{
  if (!rna_Light_shader_parameters_editable(light, reports)) {
    return nullptr;
  }
  LightShaderParameter *parameter = BKE_light_shader_parameter_add(*light, name, type);
  if (!parameter) {
    BKE_report(reports, RPT_ERROR, "Cannot allocate another shader parameter identifier");
    return nullptr;
  }
  DEG_relations_tag_update(G_MAIN);
  light_parameter_layout_update(G_MAIN, light);
  return parameter;
}

static void rna_Light_shader_parameter_remove(Light *light,
                                              ReportList *reports,
                                              PointerRNA *parameter_ptr)
{
  if (!rna_Light_shader_parameters_editable(light, reports)) {
    return;
  }
  auto *parameter = static_cast<LightShaderParameter *>(parameter_ptr->data);
  if (BLI_findindex(&light->shader_parameters, parameter) < 0) {
    BKE_report(reports, RPT_ERROR, "Shader parameter does not belong to this light");
    return;
  }
  BKE_light_shader_parameter_remove(*light, *parameter);
  parameter_ptr->invalidate();
  DEG_relations_tag_update(G_MAIN);
  light_parameter_layout_update(G_MAIN, light);
}

static LightShaderParameter *rna_Light_shader_parameter_duplicate(Light *light,
                                                                 ReportList *reports,
                                                                 LightShaderParameter *source)
{
  if (BLI_findindex(&light->shader_parameters, source) < 0) {
    BKE_report(reports, RPT_ERROR, "Shader parameter does not belong to this light");
    return nullptr;
  }
  LightShaderParameter *parameter = rna_Light_shader_parameter_new(
      light, reports, source->name, source->type);
  if (parameter) {
    parameter->value_float = source->value_float;
    parameter->value_int = source->value_int;
    parameter->value_bool = source->value_bool;
    copy_v2_v2(parameter->value_vector2, source->value_vector2);
    copy_v3_v3(parameter->value_vector3, source->value_vector3);
    copy_v4_v4(parameter->value_vector4, source->value_vector4);
    copy_v4_v4(parameter->value_color, source->value_color);
    STRNCPY(parameter->description, source->description);
    parameter->subtype = source->subtype;
    parameter->range_min = source->range_min;
    parameter->range_max = source->range_max;
    parameter->use_hard_limits = source->use_hard_limits;
    light_parameter_layout_update(G_MAIN, light);
  }
  return parameter;
}

static void rna_Light_shader_parameter_move(Light *light, ReportList *reports, int from, int to)
{
  if (!rna_Light_shader_parameters_editable(light, reports)) {
    return;
  }
  auto *parameter = static_cast<LightShaderParameter *>(BLI_findlink(&light->shader_parameters, from));
  auto *target = static_cast<LightShaderParameter *>(BLI_findlink(&light->shader_parameters, to));
  if (!parameter || !target) {
    BKE_report(reports, RPT_ERROR, "Shader parameter index out of range");
    return;
  }
  if (from != to) {
    BLI_remlink(&light->shader_parameters, parameter);
    if (from < to) {
      BLI_insertlinkafter(&light->shader_parameters, target, parameter);
    }
    else {
      BLI_insertlinkbefore(&light->shader_parameters, target, parameter);
    }
  }
  light->active_shader_parameter_index = to;
  light_parameter_layout_update(G_MAIN, light);
}

}  // namespace blender

#else

namespace blender {

/* NOTE(@dingto): Don't define icons here,
 * so they don't show up in the Light UI (properties editor). */

const EnumPropertyItem rna_enum_light_type_items[] = {
    {LA_LOCAL, "POINT", 0, "Point", "Omnidirectional point light source"},
    {LA_SUN, "SUN", 0, "Sun", "Constant direction parallel ray light source"},
    {LA_SPOT, "SPOT", 0, "Spot", "Directional cone light source"},
    {LA_AREA, "AREA", 0, "Area", "Directional area light source"},
    {0, nullptr, 0, nullptr, nullptr},
};

static void rna_def_light_api(StructRNA *srna)
{
  FunctionRNA *func = RNA_def_function(srna, "area", "rna_Light_area");
  RNA_def_function_ui_description(func,
                                  "Compute light area based on type and shape. The normalize "
                                  "option divides light intensity by this area");
  PropertyRNA *parm = RNA_def_property(func, "matrix_world", PROP_FLOAT, PROP_MATRIX);
  RNA_def_property_multi_array(parm, 2, rna_matrix_dimsize_4x4);
  RNA_def_property_ui_text(parm, "", "Object to world space transformation matrix");
  parm = RNA_def_property(func, "area", PROP_FLOAT, PROP_NONE);
  RNA_def_function_return(func, parm);
}

static void rna_def_light_shader_parameter(BlenderRNA *brna)
{
  StructRNA *srna = RNA_def_struct(brna, "LightShaderParameter", nullptr);
  RNA_def_struct_ui_text(srna, "Light Shader Parameter", "Typed parameter for material shaders");
  RNA_def_struct_path_func(srna, "rna_LightShaderParameter_path");
  RNA_def_struct_refine_func(srna, "rna_LightShaderParameter_refine");
  PropertyRNA *prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop, nullptr, nullptr, "rna_LightShaderParameter_name_set");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_struct_name_property(srna, prop);
  RNA_def_property_ui_text(prop, "Name", "Unique, case-sensitive name used by material shaders");
  RNA_def_property_update(prop, 0, "rna_LightShaderParameter_layout_update");
  prop = RNA_def_property(srna, "identifier", PROP_STRING, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE | PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop, "Identifier", "Stable collection key used by animation paths");
  prop = RNA_def_property(srna, "type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_light_shader_parameter_type_items);
  RNA_def_property_enum_funcs(prop, nullptr, "rna_LightShaderParameter_type_set", nullptr);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop, "Type", "Shader value type; changing type preserves stored values");
  RNA_def_property_update(prop, 0, "rna_LightShaderParameter_layout_update");

  prop = RNA_def_property(srna, "description", PROP_STRING, PROP_NONE);
  RNA_def_property_ui_text(prop, "Description", "Tooltip for the value and dynamic Light Info output");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_LightShaderParameter_layout_update");
  prop = RNA_def_property(srna, "subtype", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, light_parameter_subtypes);
  RNA_def_property_enum_funcs(prop, nullptr, nullptr, "rna_LightShaderParameter_subtype_itemf");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop, "Subtype", "Presentation and units only; animation paths and stored values stay unchanged");
  RNA_def_property_update(prop, 0, "rna_LightShaderParameter_layout_update");
  prop = RNA_def_property(srna, "use_hard_limits", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "use_hard_limits", 1);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop, "Enforce Limits", "Also limit numeric input and evaluated shader values; enabling clamps the active value");
  RNA_def_property_update(prop, 0, "rna_LightShaderParameter_limits_update");

  /* All value properties live on leaf types: no duplicate inherited keys or UI-only aliases.
   * Every leaf exposes the same persistent values and canonical animation paths. */
  const char *type_names[] = {"LightShaderParameterNumber", "LightShaderParameterFactor",
                             "LightShaderParameterAngle", "LightShaderParameterDistance",
                             "LightShaderParameterVector"};
  const PropertySubType subtypes[] = {PROP_NONE, PROP_FACTOR, PROP_ANGLE, PROP_DISTANCE, PROP_NONE};
  for (int i = 0; i < 5; i++) {
    StructRNA *variant = RNA_def_struct(brna, type_names[i], "LightShaderParameter");
    RNA_def_struct_sdna(variant, "LightShaderParameter");
    RNA_def_struct_ui_text(variant, "Light Shader Parameter", "Instance-specific numeric presentation");
    for (int bound = 0; bound < 2; bound++) {
      prop = RNA_def_property(variant, bound == 0 ? "range_min" : "range_max", PROP_FLOAT, subtypes[i]);
      RNA_def_property_float_funcs(prop, nullptr,
          bound == 0 ? "rna_LightShaderParameter_min_set" : "rna_LightShaderParameter_max_set", nullptr);
      RNA_def_property_range(prop, -FLT_MAX, FLT_MAX);
      RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
      RNA_def_property_ui_text(prop, bound == 0 ? "Minimum" : "Maximum", "Range in the same units as Value");
      RNA_def_property_update(prop, 0, "rna_LightShaderParameter_limits_update");
    }
    prop = RNA_def_property(variant, "value_float", PROP_FLOAT, subtypes[i]);
    RNA_def_property_float_funcs(prop, nullptr, nullptr, "rna_LightShaderParameter_float_range");
    RNA_def_property_ui_description_func(prop, "rna_LightShaderParameter_value_description");
    RNA_def_property_range(prop, -FLT_MAX, FLT_MAX);
    RNA_def_property_ui_text(prop, "Value", "Floating-point value");
    RNA_def_property_update(prop, 0, "rna_Light_update");
    prop = RNA_def_property(variant, "value_int", PROP_INT, PROP_NONE);
    RNA_def_property_int_funcs(prop, nullptr, nullptr, "rna_LightShaderParameter_int_range");
    RNA_def_property_ui_description_func(prop, "rna_LightShaderParameter_value_description");
    RNA_def_property_range(prop, -16777216, 16777216);
    RNA_def_property_ui_text(prop, "Value", "Integer exactly representable by material sockets");
    RNA_def_property_update(prop, 0, "rna_Light_update");
    prop = RNA_def_property(variant, "value_bool", PROP_BOOLEAN, PROP_NONE);
    RNA_def_property_boolean_sdna(prop, nullptr, "value_bool", 1);
    RNA_def_property_ui_description_func(prop, "rna_LightShaderParameter_value_description");
    RNA_def_property_ui_text(prop, "Value", "Boolean value");
    RNA_def_property_update(prop, 0, "rna_Light_update");
    const char *names[] = {"value_vector2", "value_vector3", "value_vector4", "value_color"};
    for (int component = 0; component < 4; component++) {
      prop = RNA_def_property(variant, names[component], PROP_FLOAT,
          component == 3 ? PROP_COLOR : i == 4 ? PROP_XYZ : PROP_NONE);
      RNA_def_property_array(prop, component == 3 ? 4 : component + 2);
      RNA_def_property_range(prop, component == 3 ? 0.0f : -FLT_MAX, FLT_MAX);
      RNA_def_property_float_funcs(prop, nullptr, nullptr, "rna_LightShaderParameter_float_range");
      RNA_def_property_ui_description_func(prop, "rna_LightShaderParameter_value_description");
      RNA_def_property_ui_text(prop, component == 3 ? "Color" : "Value", "Shader parameter components");
      RNA_def_property_update(prop, 0, "rna_Light_update");
    }
  }
}

static void rna_def_light_shader_parameters(BlenderRNA *brna, PropertyRNA *cprop)
{
  RNA_def_property_srna(cprop, "LightShaderParameters");
  StructRNA *srna = RNA_def_struct(brna, "LightShaderParameters", nullptr);
  RNA_def_struct_sdna(srna, "Light");
  RNA_def_struct_ui_text(srna, "Light Shader Parameters", "Dedicated shader parameter collection");
  FunctionRNA *func = RNA_def_function(srna, "new", "rna_Light_shader_parameter_new");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  PropertyRNA *parm = RNA_def_string(func, "name", "Parameter", 64, "Name", "Shader parameter name");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_enum(func, "type", rna_enum_light_shader_parameter_type_items,
               LIGHT_SHADER_PARAMETER_FLOAT, "Type", "Parameter type");
  parm = RNA_def_pointer(func, "parameter", "LightShaderParameter", "", "New parameter");
  RNA_def_function_return(func, parm);
  func = RNA_def_function(srna, "remove", "rna_Light_shader_parameter_remove");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_pointer(func, "parameter", "LightShaderParameter", "", "Parameter to remove");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, ParameterFlag(0));
  func = RNA_def_function(srna, "duplicate", "rna_Light_shader_parameter_duplicate");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  RNA_def_function_ui_description(func, "Duplicate parameter values without copying animation");
  parm = RNA_def_pointer(func, "source", "LightShaderParameter", "", "Parameter to duplicate");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_pointer(func, "parameter", "LightShaderParameter", "", "New parameter");
  RNA_def_function_return(func, parm);
  func = RNA_def_function(srna, "move", "rna_Light_shader_parameter_move");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_int(func, "from_index", 0, 0, INT_MAX, "From", "Source index", 0, INT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_int(func, "to_index", 0, 0, INT_MAX, "To", "Destination index", 0, INT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
}

static void rna_def_light(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;
  static const float default_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};

  srna = RNA_def_struct(brna, "Light", "ID");
  RNA_def_struct_sdna(srna, "Light");
  RNA_def_struct_refine_func(srna, "rna_Light_refine");
  RNA_def_struct_ui_text(srna, "Light", "Light data-block for lighting a scene");
  RNA_def_struct_translation_context(srna, BLT_I18NCONTEXT_ID_LIGHT);
  RNA_def_struct_ui_icon(srna, ICON_LIGHT_DATA);

  prop = RNA_def_property(srna, "shader_parameters", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, nullptr, "shader_parameters", nullptr);
  RNA_def_property_struct_type(prop, "LightShaderParameter");
  RNA_def_property_collection_funcs(prop, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                     "rna_Light_shader_parameters_lookupstring", nullptr);
  RNA_def_property_ui_text(prop, "Shader Parameters", "Typed values read by Light Info and GLSL shaders");
  rna_def_light_shader_parameters(brna, prop);
  prop = RNA_def_property(srna, "active_shader_parameter_index", PROP_INT, PROP_NONE);
  RNA_def_property_range(prop, 0, INT_MAX);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop, "Active Shader Parameter", "Selected parameter in the list");

  prop = RNA_def_property(srna, "type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_light_type_items);
  RNA_def_property_ui_text(prop, "Type", "Type of light");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_LIGHT);
  RNA_def_property_update(prop, 0, "rna_Light_draw_update");

  RNA_define_lib_overridable(true);

  prop = RNA_def_property(srna, "use_temperature", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "mode", LA_USE_TEMPERATURE);
  RNA_def_property_ui_text(
      prop, "Use Temperature", "Use blackbody temperature to define a natural light color");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_LIGHT);
  RNA_def_property_update(prop, 0, "rna_Light_draw_update");

  prop = RNA_def_property(srna, "color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_float_sdna(prop, nullptr, "r");
  RNA_def_property_array(prop, 3);
  RNA_def_property_float_array_default(prop, default_color);
  RNA_def_property_ui_text(prop, "Color", "Light color");
  RNA_def_property_update(prop, 0, "rna_Light_draw_update");

  prop = RNA_def_property(srna, "lightgroup_id", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_int_sdna(prop, nullptr, "lightgroup_id");
  RNA_def_property_range(prop, 0, INT_MAX);
  RNA_def_property_ui_text(
      prop,
      "Lightgroup ID",
      "Numeric lightgroup used by Shader Info node filtering; lights default to group 0");
  RNA_def_property_update(prop, 0, "rna_Light_draw_update");

  prop = RNA_def_property(srna, "temperature", PROP_FLOAT, PROP_COLOR_TEMPERATURE);
  RNA_def_property_float_sdna(prop, nullptr, "temperature");
  RNA_def_property_range(prop, 800.0f, 20000.0f);
  RNA_def_property_ui_range(prop, 800.0f, 20000.0f, 400.0f, 1);
  RNA_def_property_ui_text(prop, "Temperature", "Light color temperature in Kelvin");
  RNA_def_property_update(prop, 0, "rna_Light_update");

  RNA_define_lib_overridable(false);

  /* Excluded from being overridable because it's a read-only property with a
   * dynamically-generated value based on the 'temperature' property above. */
  prop = RNA_def_property(srna, "temperature_color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_array(prop, 3);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_float_funcs(prop, "rna_Light_temperature_color_get", nullptr, nullptr);
  RNA_def_property_ui_text(prop, "Temperature Color", "Color from Temperature");
  RNA_def_property_update(prop, 0, "rna_Light_draw_update");

  RNA_define_lib_overridable(true);

  prop = RNA_def_property(srna, "specular_factor", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "spec_fac");
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.01, 2);
  RNA_def_property_ui_text(prop, "Specular Factor", "Specular reflection multiplier");
  RNA_def_property_update(prop, 0, "rna_Light_update");

  prop = RNA_def_property(srna, "diffuse_factor", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "diff_fac");
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.01, 2);
  RNA_def_property_ui_text(prop, "Diffuse Factor", "Diffuse reflection multiplier");
  RNA_def_property_update(prop, 0, "rna_Light_update");

  prop = RNA_def_property(srna, "transmission_factor", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "transmission_fac");
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.01, 2);
  RNA_def_property_ui_text(prop, "Transmission Factor", "Transmission light multiplier");
  RNA_def_property_update(prop, 0, "rna_Light_update");

  prop = RNA_def_property(srna, "volume_factor", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "volume_fac");
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.01, 2);
  RNA_def_property_ui_text(prop, "Volume Factor", "Volume light multiplier");
  RNA_def_property_update(prop, 0, "rna_Light_update");

  prop = RNA_def_property(srna, "use_custom_distance", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "mode", LA_CUSTOM_ATTENUATION);
  RNA_def_property_ui_text(prop,
                           "Custom Attenuation",
                           "Use custom attenuation distance instead of global light threshold");
  RNA_def_property_update(prop, 0, "rna_Light_update");

  prop = RNA_def_property(srna, "cutoff_distance", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_float_sdna(prop, nullptr, "att_dist");
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.01f, 100.0f, 1.0, 2);
  RNA_def_property_ui_text(
      prop, "Cutoff Distance", "Distance at which the light influence will be set to 0");
  RNA_def_property_update(prop, 0, "rna_Light_update");

  prop = RNA_def_property(srna, "use_shadow", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "mode", LA_SHADOW);
  RNA_def_property_update(prop, 0, "rna_Light_draw_update");

  prop = RNA_def_property(srna, "exposure", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_default(prop, 0.0f);
  RNA_def_property_range(prop, -32.0f, 32.0f);
  RNA_def_property_ui_range(prop, -10.0f, 10.0f, 1, 3);
  RNA_def_property_ui_text(
      prop,
      "Exposure",
      "Scales the power of the light exponentially, multiplying the intensity by 2^exposure");
  RNA_def_property_update(prop, 0, "rna_Light_update");

  prop = RNA_def_property(srna, "normalize", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_negative_sdna(prop, nullptr, "mode", LA_UNNORMALIZED);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop,
                           "Normalize",
                           "Normalize intensity by light area, for consistent total light "
                           "output regardless of size and shape");
  RNA_def_property_update(prop, 0, "rna_Light_draw_update");

  RNA_define_lib_overridable(false);

  /* nodes */
  prop = RNA_def_property(srna, "node_tree", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "nodetree");
  RNA_def_property_clear_flag(prop, PROP_PTR_NO_OWNERSHIP);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_ui_text(prop, "Node Tree", "Node tree for node based lights");

  prop = RNA_def_property(srna, "use_nodes", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "use_nodes", 1);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop, "Use Nodes", "Use shader nodes to render the light");
  RNA_def_property_boolean_funcs(prop, "rna_Light_use_nodes_get", "rna_Light_use_nodes_set");
  RNA_def_property_deprecated(prop,
                              "Unused but kept for compatibility reasons. Setting the property "
                              "has no effect, and getting it always returns True.",
                              510,
                              600);

  /* common */
  rna_def_animdata_common(srna);
  rna_def_light_api(srna);
}

static void rna_def_light_energy(StructRNA *srna, const short light_type)
{
  PropertyRNA *prop;

  RNA_define_lib_overridable(true);

  switch (light_type) {
    case LA_SUN: {
      /* Distant light strength has no unit defined,
       * it's proportional to 'watt/m^2' and is not sensitive to scene unit scale. */
      prop = RNA_def_property(srna, "energy", PROP_FLOAT, PROP_NONE);
      RNA_def_property_ui_range(prop, 0.0f, 10.0f, 1, 3);
      RNA_def_property_ui_text(
          prop, "Strength", "Sunlight strength in watts per meter squared (W/m²)");
      RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_LIGHT);
      RNA_def_property_update(prop, 0, "rna_Light_draw_update");
      break;
    }
    case LA_SPOT: {
      /* Lights with a location have radiometric power in Watts,
       * which is sensitive to scene unit scale. */
      prop = RNA_def_property(srna, "energy", PROP_FLOAT, PROP_NONE);
      RNA_def_property_ui_range(prop, 0.0f, 1000000.0f, 10, 3);
      RNA_def_property_ui_text(
          prop,
          "Power",
          "The energy this light would emit over its entire area "
          "if it wasn't limited by the spot angle, in units of radiant power (W)");
      RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_LIGHT);
      RNA_def_property_update(prop, 0, "rna_Light_draw_update");
      break;
    }
    default: {
      /* Lights with a location have radiometric power in Watts,
       * which is sensitive to scene unit scale. */
      prop = RNA_def_property(srna, "energy", PROP_FLOAT, PROP_NONE);
      RNA_def_property_ui_range(prop, 0.0f, 1000000.0f, 10, 3);
      RNA_def_property_ui_text(prop,
                               "Power",
                               "Light energy emitted over the entire area of the light in all "
                               "directions, in units of radiant power (W)");
      RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_LIGHT);
      RNA_def_property_update(prop, 0, "rna_Light_draw_update");
      break;
    }
  }

  RNA_define_lib_overridable(false);
}

static void rna_def_light_shadow(StructRNA *srna, bool sun)
{
  PropertyRNA *prop;

  RNA_define_lib_overridable(true);

  prop = RNA_def_property(srna, "shadow_buffer_clip_start", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_float_sdna(prop, nullptr, "clipsta");
  RNA_def_property_range(prop, 1e-6f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.001f, FLT_MAX, 10, 3);
  RNA_def_property_ui_text(prop,
                           "Shadow Buffer Clip Start",
                           "Shadow map clip start, below which objects will not generate shadows");
  RNA_def_property_update(prop, 0, "rna_Light_draw_update");

  prop = RNA_def_property(srna, "shadow_soft_size", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_float_sdna(prop, nullptr, "radius");
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0, 100, 0.1, 3);
  RNA_def_property_ui_text(
      prop, "Shadow Soft Size", "Light size for ray shadow sampling (Raytraced shadows)");
  RNA_def_property_update(prop, 0, "rna_Light_draw_update");

  /* Eevee */
  prop = RNA_def_property(srna, "shadow_filter_radius", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.0f, 5.0f, 1.0f, 2);
  RNA_def_property_ui_text(
      prop, "Shadow Filter Radius", "Blur shadow aliasing using Percentage Closer Filtering");
  RNA_def_property_update(prop, 0, "rna_Light_update");

  prop = RNA_def_property(srna, "shadow_maximum_resolution", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.0001f, 0.020f, 0.05f, 4);
  RNA_def_property_ui_text(prop,
                           "Shadows Resolution Limit",
                           "Minimum size of a shadow map pixel. Higher values use less memory at "
                           "the cost of shadow quality.");
  RNA_def_property_update(prop, 0, "rna_Light_update");

  prop = RNA_def_property(srna, "use_shadow_jitter", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "mode", LA_SHADOW_JITTER);
  RNA_def_property_ui_text(
      prop,
      "Shadow Jitter",
      "Enable jittered soft shadows to increase shadow precision (disabled in viewport unless "
      "enabled in the render settings). Has a high performance impact.");
  RNA_def_property_update(prop, 0, "rna_Light_update");

  prop = RNA_def_property(srna, "shadow_jitter_overblur", PROP_FLOAT, PROP_PERCENTAGE);
  RNA_def_property_range(prop, 0.0f, 100.0f);
  RNA_def_property_ui_range(prop, 0.0f, 20.0f, 10.0f, 0);
  RNA_def_property_ui_text(
      prop,
      "Shadow Jitter Overblur",
      "Apply shadow tracing to each jittered sample to reduce under-sampling artifacts");
  RNA_def_property_update(prop, 0, "rna_Light_update");

  if (sun) {
    prop = RNA_def_property(srna, "shadow_cascade_max_distance", PROP_FLOAT, PROP_DISTANCE);
    RNA_def_property_float_sdna(prop, nullptr, "cascade_max_dist");
    RNA_def_property_range(prop, 0.0f, FLT_MAX);
    RNA_def_property_ui_text(prop,
                             "Cascade Max Distance",
                             "End distance of the cascaded shadow map (only in perspective view)");
    RNA_def_property_update(prop, 0, "rna_Light_update");

    prop = RNA_def_property(srna, "shadow_cascade_count", PROP_INT, PROP_NONE);
    RNA_def_property_int_sdna(prop, nullptr, "cascade_count");
    RNA_def_property_range(prop, 1, 4);
    RNA_def_property_ui_text(
        prop, "Cascade Count", "Number of texture used by the cascaded shadow map");
    RNA_def_property_update(prop, 0, "rna_Light_update");

    prop = RNA_def_property(srna, "shadow_cascade_exponent", PROP_FLOAT, PROP_FACTOR);
    RNA_def_property_float_sdna(prop, nullptr, "cascade_exponent");
    RNA_def_property_range(prop, 0.0f, 1.0f);
    RNA_def_property_ui_text(prop,
                             "Exponential Distribution",
                             "Higher value increase resolution towards the viewpoint");
    RNA_def_property_update(prop, 0, "rna_Light_update");

    prop = RNA_def_property(srna, "shadow_cascade_fade", PROP_FLOAT, PROP_FACTOR);
    RNA_def_property_float_sdna(prop, nullptr, "cascade_fade");
    RNA_def_property_range(prop, 0.0f, 1.0f);
    RNA_def_property_ui_text(
        prop, "Cascade Fade", "How smooth is the transition between each cascade");
    RNA_def_property_update(prop, 0, "rna_Light_update");
  }
  else {
    prop = RNA_def_property(srna, "use_absolute_resolution", PROP_BOOLEAN, PROP_NONE);
    RNA_def_property_boolean_sdna(prop, nullptr, "mode", LA_SHAD_RES_ABSOLUTE);
    RNA_def_property_ui_text(prop,
                             "Absolute Resolution Limit",
                             "Limit the resolution at 1 unit from the light origin instead of "
                             "relative to the shadowed pixel");
    RNA_def_property_update(prop, 0, "rna_Light_update");
  }

  RNA_define_lib_overridable(false);
}

static void rna_def_point_light(BlenderRNA *brna)
{
  StructRNA *srna;

  srna = RNA_def_struct(brna, "PointLight", "Light");
  RNA_def_struct_sdna(srna, "Light");
  RNA_def_struct_ui_text(srna, "Point Light", "Omnidirectional point Light");
  RNA_def_struct_ui_icon(srna, ICON_LIGHT_POINT);

  PropertyRNA *prop;
  prop = RNA_def_property(srna, "use_soft_falloff", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "mode", LA_USE_SOFT_FALLOFF);
  RNA_def_property_ui_text(
      prop,
      "Soft Falloff",
      "Apply falloff to avoid sharp edges when the light geometry intersects with other objects");
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_update(prop, 0, "rna_Light_draw_update");

  rna_def_light_energy(srna, LA_LOCAL);
  rna_def_light_shadow(srna, false);
}

static void rna_def_area_light(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  static const EnumPropertyItem prop_areashape_items[] = {
      {LA_AREA_SQUARE, "SQUARE", 0, "Square", ""},
      {LA_AREA_RECT, "RECTANGLE", 0, "Rectangle", ""},
      {LA_AREA_DISK, "DISK", 0, "Disk", ""},
      {LA_AREA_ELLIPSE, "ELLIPSE", 0, "Ellipse", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  srna = RNA_def_struct(brna, "AreaLight", "Light");
  RNA_def_struct_sdna(srna, "Light");
  RNA_def_struct_ui_text(srna, "Area Light", "Directional area Light");
  RNA_def_struct_ui_icon(srna, ICON_LIGHT_AREA);

  rna_def_light_energy(srna, LA_AREA);
  rna_def_light_shadow(srna, false);

  RNA_define_lib_overridable(true);

  prop = RNA_def_property(srna, "shape", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "area_shape");
  RNA_def_property_enum_items(prop, prop_areashape_items);
  RNA_def_property_ui_text(prop, "Shape", "Shape of the area Light");
  RNA_def_property_update(prop, 0, "rna_Light_draw_update");

  prop = RNA_def_property(srna, "size", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_float_sdna(prop, nullptr, "area_size");
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0, 100, 0.1, 3);
  RNA_def_property_ui_text(
      prop, "Size", "Size of the area of the area light, X direction size for rectangle shapes");
  RNA_def_property_update(prop, 0, "rna_Light_draw_update");

  prop = RNA_def_property(srna, "size_y", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_float_sdna(prop, nullptr, "area_sizey");
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0, 100, 0.1, 3);
  RNA_def_property_ui_text(
      prop,
      "Size Y",
      "Size of the area of the area light in the Y direction for rectangle shapes");
  RNA_def_property_update(prop, 0, "rna_Light_draw_update");

  prop = RNA_def_property(srna, "spread", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_float_sdna(prop, nullptr, "area_spread");
  RNA_def_property_range(prop, DEG2RADF(0.0f), DEG2RADF(180.0f));
  RNA_def_property_ui_text(
      prop,
      "Spread",
      "How widely the emitted light fans out, as in the case of a gridded softbox");
  RNA_def_property_update(prop, 0, "rna_Light_draw_update");

  RNA_define_lib_overridable(false);
}

static void rna_def_spot_light(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "SpotLight", "Light");
  RNA_def_struct_sdna(srna, "Light");
  RNA_def_struct_ui_text(srna, "Spot Light", "Directional cone Light");
  RNA_def_struct_ui_icon(srna, ICON_LIGHT_SPOT);

  rna_def_light_energy(srna, LA_SPOT);
  rna_def_light_shadow(srna, false);

  RNA_define_lib_overridable(true);

  prop = RNA_def_property(srna, "use_square", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "mode", LA_SQUARE);
  RNA_def_property_ui_text(prop, "Square", "Cast a square spot light shape");
  RNA_def_property_update(prop, 0, "rna_Light_draw_update");

  prop = RNA_def_property(srna, "spot_blend", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "spotblend");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(prop, "Spot Blend", "The softness of the spotlight edge");
  RNA_def_property_update(prop, 0, "rna_Light_draw_update");

  prop = RNA_def_property(srna, "spot_size", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_float_sdna(prop, nullptr, "spotsize");
  RNA_def_property_range(prop, DEG2RADF(1.0f), DEG2RADF(180.0f));
  RNA_def_property_ui_text(prop, "Beam Angle", "Angular diameter of the spotlight beam");
  RNA_def_property_update(prop, 0, "rna_Light_draw_update");

  prop = RNA_def_property(srna, "show_cone", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "mode", LA_SHOW_CONE);
  RNA_def_property_ui_text(
      prop,
      "Show Cone",
      "Display transparent cone in 3D view to visualize which objects are contained in it");
  RNA_def_property_update(prop, 0, "rna_Light_draw_update");

  prop = RNA_def_property(srna, "use_soft_falloff", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "mode", LA_USE_SOFT_FALLOFF);
  RNA_def_property_ui_text(
      prop,
      "Soft Falloff",
      "Apply falloff to avoid sharp edges when the light geometry intersects with other objects");
  RNA_def_property_update(prop, 0, "rna_Light_draw_update");

  RNA_define_lib_overridable(false);
}

static void rna_def_sun_light(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "SunLight", "Light");
  RNA_def_struct_sdna(srna, "Light");
  RNA_def_struct_ui_text(srna, "Sun Light", "Constant direction parallel ray Light");
  RNA_def_struct_ui_icon(srna, ICON_LIGHT_SUN);

  prop = RNA_def_property(srna, "angle", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_float_sdna(prop, nullptr, "sun_angle");
  RNA_def_property_range(prop, DEG2RADF(0.0f), DEG2RADF(180.0f));
  RNA_def_property_ui_text(prop, "Angle", "Angular diameter of the Sun as seen from the Earth");
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_update(prop, 0, "rna_Light_update");

  rna_def_light_energy(srna, LA_SUN);
  rna_def_light_shadow(srna, true);
}

void RNA_def_light(BlenderRNA *brna)
{
  rna_def_light_shader_parameter(brna);
  rna_def_light(brna);
  rna_def_point_light(brna);
  rna_def_area_light(brna);
  rna_def_spot_light(brna);
  rna_def_sun_light(brna);
}

}  // namespace blender

#endif
