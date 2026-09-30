/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <cstdlib>
#include <algorithm>
#include <climits>
#include <cmath>
#include <optional>

#include "MEM_guardedalloc.h"

/* Allow using deprecated functionality for .blend file I/O. */
#define DNA_DEPRECATED_ALLOW

#include "DNA_light_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "BLI_listbase.h"
#include "BLI_math_base.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_utildefines.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_string_utils.hh"

#include "BKE_icons.hh"
#include "BKE_animsys.h"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_light.h"
#include "BKE_node.hh"
#include "BKE_preview_image.hh"

#include "GPU_material.hh"

#include "BLT_translation.hh"

#include "DEG_depsgraph.hh"

#include "IMB_colormanagement.hh"

#include "BLO_read_write.hh"

#include "NOD_defaults.hh"

namespace blender {

static void light_init_data(ID *id)
{
  Light *la = id_cast<Light *>(id);
  INIT_DEFAULT_STRUCT_AFTER(la, id);
}

/**
 * Only copy internal data of Light ID from source
 * to already allocated/initialized destination.
 * You probably never want to use that directly,
 * use #BKE_id_copy or #BKE_id_copy_ex for typical needs.
 *
 * WARNING! This function will not handle ID user count!
 *
 * \param flag: Copying options (see BKE_lib_id.hh's LIB_ID_COPY_... flags for more).
 */
static void light_copy_data(Main *bmain,
                            std::optional<Library *> owner_library,
                            ID *id_dst,
                            const ID *id_src,
                            const int flag)
{
  Light *la_dst = id_cast<Light *>(id_dst);
  const Light *la_src = id_cast<const Light *>(id_src);

  BLI_duplicatelist(&la_dst->shader_parameters, &la_src->shader_parameters);

  const bool is_localized = (flag & LIB_ID_CREATE_LOCAL) != 0;
  /* We always need allocation of our private ID data.
   * User reference-counting is also handled by calling code,
   * so the duplication calls for embedded data should _never_ handle it from here. */
  const int flag_embedded_id_data = (flag & ~LIB_ID_CREATE_NO_ALLOCATE) |
                                    LIB_ID_CREATE_NO_USER_REFCOUNT;

  if (la_src->nodetree) {
    if (is_localized) {
      la_dst->nodetree = bke::node_tree_localize(la_src->nodetree, &la_dst->id);
    }
    else {
      BKE_id_copy_in_lib(bmain,
                         owner_library,
                         &la_src->nodetree->id,
                         &la_dst->id,
                         reinterpret_cast<ID **>(&la_dst->nodetree),
                         flag_embedded_id_data);
    }
  }

  if ((flag & LIB_ID_COPY_NO_PREVIEW) == 0) {
    BKE_previewimg_id_copy(&la_dst->id, &la_src->id);
  }
  else {
    la_dst->preview = nullptr;
  }

  BLI_listbase_clear(&la_dst->gpumaterial);
}

static void light_free_data(ID *id)
{
  Light *la = id_cast<Light *>(id);

  GPU_material_free(&la->gpumaterial);
  BLI_freelistN(&la->shader_parameters);

  /* is no lib link block, but light extension */
  if (la->nodetree) {
    bke::node_tree_free_embedded_tree(la->nodetree);
    MEM_delete(la->nodetree);
    la->nodetree = nullptr;
  }

  BKE_previewimg_id_free(&la->id);
  BKE_icon_id_delete(&la->id);
  la->id.icon_id = 0;
}

static void light_foreach_id(ID *id, LibraryForeachIDData *data)
{
  Light *lamp = reinterpret_cast<Light *>(id);

  if (lamp->nodetree) {
    /* nodetree **are owned by IDs**, treat them as mere sub-data and not real ID! */
    BKE_LIB_FOREACHID_PROCESS_FUNCTION_CALL(
        data, BKE_library_foreach_ID_embedded(data, (ID **)&lamp->nodetree));
  }
}

static void light_foreach_working_space_color(ID *id, const IDTypeForeachColorFunctionCallback &fn)
{
  Light *la = id_cast<Light *>(id);

  fn.single(&la->r);
  for (LightShaderParameter &parameter : la->shader_parameters) {
    fn.single(parameter.value_color);
  }
}

static void light_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  Light *la = id_cast<Light *>(id);

  /* Forward compatibility for energy. */
  la->energy_deprecated = la->energy * exp2f(la->exposure);
  if (la->type == LA_AREA) {
    la->energy_deprecated /= M_PI_4;
  }

  /* Forward compatibility for Use Nodes. */
  la->use_nodes = true;

  /* Clean up runtime data, important in undo case to reduce false detection of changed
   * datablocks. */
  BLI_listbase_clear(&la->gpumaterial);

  /* write LibData */
  writer->write_id_struct(id_address, la);
  BKE_id_blend_write(writer, &la->id);
  writer->write_struct_list(&la->shader_parameters);

  /* Node-tree is integral part of lights, no libdata. */
  if (la->nodetree) {
    BLO_Write_IDBuffer temp_embedded_id_buffer{la->nodetree->id, writer};
    writer->write_struct_at_address_cast<bNodeTree>(la->nodetree, temp_embedded_id_buffer.get());
    bke::node_tree_blend_write(writer,
                               reinterpret_cast<bNodeTree *>(temp_embedded_id_buffer.get()));
  }

  BKE_previewimg_blend_write(writer, la->preview);
}

static void light_blend_read_data(BlendDataReader *reader, ID *id)
{
  Light *la = id_cast<Light *>(id);

  BLO_read_struct(reader, PreviewImage, &la->preview);
  BLO_read_struct_list(reader, LightShaderParameter, &la->shader_parameters);
  BKE_previewimg_blend_read(reader, la->preview);
  BLI_listbase_clear(&la->gpumaterial);
}

IDTypeInfo IDType_ID_LA = {
    .id_code = Light::id_type,
    .id_filter = FILTER_ID_LA,
    .dependencies_id_types = FILTER_ID_TE,
    .main_listbase_index = INDEX_ID_LA,
    .struct_size = sizeof(Light),
    .name = "Light",
    .name_plural = N_("lights"),
    .translation_context = BLT_I18NCONTEXT_ID_LIGHT,
    .flags = IDTYPE_FLAGS_APPEND_IS_REUSABLE,
    .asset_type_info = nullptr,

    .init_data = light_init_data,
    .copy_data = light_copy_data,
    .free_data = light_free_data,
    .make_local = nullptr,
    .foreach_id = light_foreach_id,
    .foreach_cache = nullptr,
    .foreach_path = nullptr,
    .foreach_working_space_color = light_foreach_working_space_color,
    .owner_pointer_get = nullptr,

    .blend_write = light_blend_write,
    .blend_read_data = light_blend_read_data,
    .blend_read_after_liblink = nullptr,

    .blend_read_undo_preserve = nullptr,

    .lib_override_apply_post = nullptr,
};

LightShaderParameter *BKE_light_shader_parameter_add(Light &light, const char *name, int type)
{
  if (light.next_shader_parameter_identifier == INT_MAX ||
      type < LIGHT_SHADER_PARAMETER_FLOAT || type > LIGHT_SHADER_PARAMETER_COLOR)
  {
    return nullptr;
  }
  auto *parameter = MEM_new<LightShaderParameter>(__func__);
  parameter->type = eLightShaderParameterType(type);
  BLI_strncpy_utf8(parameter->name, name[0] ? name : "Parameter", sizeof(parameter->name));
  SNPRINTF(parameter->identifier, "parameter_%d", ++light.next_shader_parameter_identifier);
  BLI_addtail(&light.shader_parameters, parameter);
  BLI_uniquename(&light.shader_parameters, parameter, "Parameter", '.',
                 offsetof(LightShaderParameter, name), sizeof(parameter->name));
  light.active_shader_parameter_index = BLI_listbase_count(&light.shader_parameters) - 1;
  return parameter;
}

void BKE_light_shader_parameter_remove(Light &light, LightShaderParameter &parameter)
{
  const std::string path = "shader_parameters[\"" + std::string(parameter.identifier) + "\"]";
  /* Actions (including NLA actions) may be shared with another Light. Do not edit their curves.
   * Identifiers are never reused, so an obsolete curve cannot animate a newly added parameter. */
  BKE_animdata_driver_path_remove(&light.id, path.c_str());
  BLI_remlink(&light.shader_parameters, &parameter);
  MEM_delete(&parameter);
  light.active_shader_parameter_index = std::clamp(
      light.active_shader_parameter_index, 0, max_ii(0, BLI_listbase_count(&light.shader_parameters) - 1));
}

bool BKE_light_shader_parameter_value(const LightShaderParameter &parameter, float4 &value)
{
  value = float4(0.0f);
  switch (parameter.type) {
    case LIGHT_SHADER_PARAMETER_FLOAT:
      value.x = parameter.value_float;
      break;
    case LIGHT_SHADER_PARAMETER_INT:
      if (parameter.value_int < -16777216 || parameter.value_int > 16777216) {
        return false;
      }
      value.x = float(parameter.value_int);
      break;
    case LIGHT_SHADER_PARAMETER_BOOL:
      value.x = parameter.value_bool != 0 ? 1.0f : 0.0f;
      break;
    case LIGHT_SHADER_PARAMETER_VECTOR2:
      value.x = parameter.value_vector2[0];
      value.y = parameter.value_vector2[1];
      break;
    case LIGHT_SHADER_PARAMETER_VECTOR3:
      value = float4(float3(parameter.value_vector3), 0.0f);
      break;
    case LIGHT_SHADER_PARAMETER_VECTOR4:
      value = float4(parameter.value_vector4);
      break;
    case LIGHT_SHADER_PARAMETER_COLOR:
      value = float4(parameter.value_color);
      break;
    default:
      return false;
  }
  if (!(std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
        std::isfinite(value.w))) {
    return false;
  }
  if (parameter.use_hard_limits && parameter.type != LIGHT_SHADER_PARAMETER_BOOL) {
    float lo = parameter.range_min;
    float hi = parameter.range_max;
    if (!std::isfinite(lo) || !std::isfinite(hi)) {
      return false;
    }
    hi = std::max(lo, hi);
    if (parameter.type == LIGHT_SHADER_PARAMETER_INT) {
      lo = std::ceil(std::clamp(lo, -16777216.0f, 16777216.0f));
      hi = std::max(lo, std::floor(std::clamp(hi, -16777216.0f, 16777216.0f)));
    }
    if (parameter.type == LIGHT_SHADER_PARAMETER_COLOR) {
      lo = std::max(lo, 0.0f);
      hi = std::max(lo, hi);
    }
    const int components = parameter.type <= LIGHT_SHADER_PARAMETER_BOOL ? 1 :
                           parameter.type == LIGHT_SHADER_PARAMETER_VECTOR2 ? 2 :
                           parameter.type == LIGHT_SHADER_PARAMETER_VECTOR3 ? 3 : 4;
    for (int i = 0; i < components; i++) {
      value[i] = std::clamp(value[i], lo, hi);
    }
  }
  return true;
}

Light *BKE_light_add(Main *bmain, const char *name)
{
  Light *la;

  la = BKE_id_new<Light>(bmain, name);

  nodes::node_tree_shader_default(nullptr, bmain, &la->id);

  return la;
}

void BKE_light_eval(Depsgraph *depsgraph, Light *la)
{
  DEG_debug_print_eval(depsgraph, __func__, la->id.name, la);
}

float BKE_light_power(const Light &light)
{
  return light.energy * exp2f(light.exposure);
}

float3 BKE_light_color(const Light &light)
{
  float3 color(&light.r);

  if (light.mode & LA_USE_TEMPERATURE) {
    float temperature_color[4];
    IMB_colormanagement_blackbody_temperature_to_rgb(temperature_color, light.temperature);
    color *= float3(temperature_color);
  }

  return color;
}

float BKE_light_area(const Light &light, const float4x4 &object_to_world)
{
  /* Make illumination power constant. */
  switch (light.type) {
    case LA_AREA: {
      /* Rectangle area. */
      const float3x3 scalemat = object_to_world.view<3, 3>();
      const float3 scale = math::to_scale(scalemat);

      const float size_x = light.area_size * scale.x;
      const float size_y = (ELEM(light.area_shape, LA_AREA_RECT, LA_AREA_ELLIPSE) ?
                                light.area_sizey :
                                light.area_size) *
                           scale.y;

      float area = size_x * size_y;
      /* Scale for smaller area of the ellipse compared to the surrounding rectangle. */
      if (ELEM(light.area_shape, LA_AREA_DISK, LA_AREA_ELLIPSE)) {
        area *= float(M_PI / 4.0f);
      }
      return area;
    }
    case LA_LOCAL:
    case LA_SPOT: {
      /* Sphere area. For legacy reasons object scale is not taken into account
       * here, even though logically it should be. */
      const float radius = light.radius;
      return (radius > 0.0f) ? float(4.0f * M_PI) * math::square(radius) : 4.0f;
    }
    case LA_SUN: {
      /* Sun disk area. */
      const float angle = light.sun_angle / 2.0f;
      return (angle > 0.0f) ? float(M_PI) * math::square(sinf(angle)) : 1.0f;
    }
  }

  BLI_assert_unreachable();
  return 1.0f;
}

}  // namespace blender
