/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <map>
#include <mutex>
#include <sstream>

#include "DNA_node_types.h"
#include "DNA_collection_types.h"
#include "DNA_scene_types.h"
#include "DNA_color_types.h"
#include "BLI_string.hh"
#include "BLI_rect.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_listbase.hh"
#include "BKE_context.hh"
#include "BKE_collection.hh"
#include "BKE_colortools.hh"
#include "BKE_main.hh"
#include "BKE_main_invariants.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BLT_translation.hh"
#include "BLO_read_write.hh"
#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"
#include "NOD_npr_line.hh"
#include "NOD_common.hh"
#include "NOD_socket.hh"
#include "NOD_socket_search_link.hh"
#include "RNA_access.hh"
#include "RNA_prototypes.hh"
#include "UI_interface_layout.hh"
#include "UI_interface_c.hh"
#include "UI_interface_icons.hh"
#include "UI_resources.hh"
#include "GPU_state.hh"
#include "WM_api.hh"
#include "WM_types.hh"
#include "node_common.h"
#include "node_shader_util.hh"
#include "node_util.hh"

namespace blender::nodes {

static CurveMapping *new_brush_curve()
{
  CurveMapping *curve = BKE_curvemapping_add(1, 0, 0, 1, 1);
  BKE_curvemap_reset(&curve->cm[0], &curve->clipr, CURVE_PRESET_BELL,
                    CurveMapSlopeType::Positive);
  curve->cm[0].curve[0].y = 0;
  curve->cm[0].curve[curve->cm[0].totpoint - 1].y = 0;
  BKE_curvemapping_changed(curve, false);
  return curve;
}

static void ensure_brush_curves(bNode &node)
{
  if (!ELEM(node.type_legacy, NPR_LINE_NODE_BRUSH, NPR_LINE_NODE_BRUSH_INPUT,
            NPR_LINE_NODE_BRUSH_OUTPUT, NPR_LINE_NODE_REDUCTION)) { return; }
  auto &data = *static_cast<NodeNPRLine *>(node.storage);
  if (node.type_legacy == NPR_LINE_NODE_REDUCTION) {
    if (!data.width_curve) {
      data.width_curve = BKE_curvemapping_add(1, 0, 0, 1, 1);
      CurveMap &curve = data.width_curve->cm[0];
      curve.curve[0].y = 1;
      curve.curve[curve.totpoint - 1].y = 0.1f;
      BKE_curvemap_insert(&curve, 0.2f, 0.4f);
      BKE_curvemapping_changed(data.width_curve, false);
    }
    return;
  }
  if (!data.width_curve) { data.width_curve = new_brush_curve(); }
  if (!data.opacity_curve) { data.opacity_curve = new_brush_curve(); }
}

static constexpr const char *edge_names[] = {"Outline", "Object Contour", "Intersection",
                                            "Smoothing Boundary", "Material Boundary",
                                            "Normal Angle", "Wire", "Marked Edge"};
static constexpr const char *edge_ids[] = {"outline", "object", "intersection", "smoothing",
                                          "material", "normal", "wire", "marked"};

static std::mutex solo_mutex;
static std::map<unsigned int, int> solo_nodes;
struct LineTiming {
  bool enabled = false;
  std::string text;
};
static std::map<unsigned int, LineTiming> timings;

bool npr_line_timing_enabled(const bNodeTree &tree)
{
  const ID *id = DEG_get_original_id(&tree.id);
  std::lock_guard lock(solo_mutex);
  auto found = timings.find(id->session_uid);
  return found != timings.end() && found->second.enabled;
}

void npr_line_set_timing_enabled(Main &bmain, bNodeTree &tree, bool enabled)
{
  {
    std::lock_guard lock(solo_mutex);
    std::erase_if(timings, [&](const auto &entry) {
      for (const bNodeTree &candidate : bmain.nodetrees) {
        if (candidate.id.session_uid == entry.first) { return false; }
      }
      return true;
    });
    timings[tree.id.session_uid].enabled = enabled;
  }
  npr_line_tag_tree_changed(bmain, tree);
}

void npr_line_set_timing(const bNodeTree &tree, const std::string &text)
{
  const ID *id = DEG_get_original_id(&tree.id);
  std::lock_guard lock(solo_mutex);
  auto found = timings.find(id->session_uid);
  if (found != timings.end() && found->second.enabled) { found->second.text = text; }
}

std::string npr_line_timing(const bNodeTree &tree)
{
  const ID *id = DEG_get_original_id(&tree.id);
  std::lock_guard lock(solo_mutex);
  auto found = timings.find(id->session_uid);
  return found == timings.end() ? std::string() : found->second.text;
}

int npr_line_solo_node(const bNodeTree &tree)
{
  const ID *id = DEG_get_original_id(&tree.id);
  std::lock_guard lock(solo_mutex);
  const auto found = solo_nodes.find(id->session_uid);
  return found == solo_nodes.end() ? 0 : found->second;
}

void npr_line_set_solo_node(Main &bmain, bNodeTree &tree, int node_identifier)
{
  {
    std::lock_guard lock(solo_mutex);
    /* Keep only IDs still present in Main; opening a file cannot inherit a previous solo. */
    std::erase_if(solo_nodes, [&](const auto &entry) {
      for (const bNodeTree &candidate : bmain.nodetrees) {
        if (candidate.id.session_uid == entry.first) {
          return false;
        }
      }
      return true;
    });
    if (node_identifier) {
      solo_nodes[tree.id.session_uid] = node_identifier;
    }
    else {
      solo_nodes.erase(tree.id.session_uid);
    }
  }
  npr_line_tag_tree_changed(bmain, tree);
}

void npr_line_tag_tree_changed(Main &bmain, bNodeTree &tree)
{
  BKE_main_ensure_invariants(bmain, tree.id);
  DEG_id_tag_update(&tree.id, ID_RECALC_SYNC_TO_EVAL);
  for (Scene &scene : bmain.scenes) {
    if (scene.eevee.geometry_line_tree) {
      DEG_id_tag_update(&scene.id, ID_RECALC_SYNC_TO_EVAL);
    }
  }
  WM_main_add_notifier(NC_NODE | NA_EDITED, &tree);
  WM_main_add_notifier(NC_SCENE | ND_RENDER_OPTIONS, nullptr);
}

static void tree_context(const bContext *C,
                         bke::bNodeTreeType *,
                         bNodeTree **r_tree,
                         ID **r_id,
                         ID **r_from)
{
  Scene *scene = CTX_data_scene(C);
  *r_tree = scene ? scene->eevee.geometry_line_tree : nullptr;
  *r_id = scene ? &scene->id : nullptr;
  *r_from = nullptr;
}

static void tree_update(bNodeTree *tree)
{
  bNode *first = nullptr;
  bNode *active = nullptr;
  for (bNode &node : tree->nodes) {
    if (node.type_legacy != NPR_LINE_NODE_OUTPUT) {
      continue;
    }
    if (!first) {
      first = &node;
    }
    if (node.flag & NODE_DO_OUTPUT) {
      if (active) {
        node.flag &= ~NODE_DO_OUTPUT;
      }
      else {
        active = &node;
      }
    }
  }
  if (!active && first) {
    first->flag |= NODE_DO_OUTPUT;
  }
  ntree_update_reroute_nodes(tree);
}

static bool tree_socket_valid(bke::bNodeTreeType *, bke::bNodeSocketType *type)
{
  return STRPREFIX(type->idname.c_str(), "NodeSocketNPRLine") ||
         ELEM(type->type, SOCK_FLOAT, SOCK_INT, SOCK_BOOLEAN, SOCK_RGBA,
              SOCK_VECTOR, SOCK_OBJECT, SOCK_COLLECTION, SOCK_MATERIAL, SOCK_IMAGE, SOCK_STRING);
}

static bool node_poll(const bke::bNodeType *, const bNodeTree *tree, const char **hint)
{
  if (tree->type != NTREE_NPR_LINE) {
    *hint = RPT_("Requires a Geometry Lines node tree");
    return false;
  }
  return true;
}

static void socket_color(const bke::bNodeSocketType *type, float *color)
{
  const float colors[][4] = {{0.25f, 0.7f, 0.35f, 1.0f}, {0.7f, 0.35f, 0.8f, 1.0f},
                            {0.1f, 0.65f, 0.85f, 1.0f}, {0.8f, 0.45f, 0.2f, 1.0f},
                            {0.85f, 0.65f, 0.2f, 1.0f}, {0.25f, 0.7f, 0.75f, 1.0f}};
  const int index = type->idname == npr_line_objects_socket ? 0 :
                    type->idname == npr_line_brush_socket ? 1 :
                    type->idname == npr_line_plan_socket ? 2 :
                    type->idname == npr_line_reduction_socket ? 4 :
                    type->idname == npr_line_map_socket ? 5 : 3;
  std::copy_n(colors[index], 4, color);
}

static void socket_draw(bContext *, ui::Layout *layout, PointerRNA *, PointerRNA *, StringRef text)
{
  layout->label(text, ICON_NONE);
}

static void register_socket(const char *idname, const char *interface_idname, const char *label)
{
  auto *type = MEM_new<bke::bNodeSocketType>(__func__);
  type->idname = UString(idname);
  type->label = label;
  type->type = SOCK_CUSTOM;
  type->ext_socket.srna = RNA_struct_find(idname);
  type->ext_interface.srna = RNA_struct_find(interface_idname);
  RNA_struct_blender_type_set(type->ext_socket.srna, type);
  RNA_struct_blender_type_set(type->ext_interface.srna, type);
  type->draw = socket_draw;
  type->draw_color_simple = socket_color;
  type->draw_color = [](bContext *, PointerRNA *ptr, PointerRNA *, float *color) {
    socket_color(ptr->data_as<bNodeSocket>()->typeinfo, color);
  };
  type->use_link_limits_of_type = true;
  type->input_link_limit = 1;
  type->output_link_limit = 0xFFF;
  type->free_self = [](bke::bNodeSocketType *self) { MEM_delete(self); };
  bke::node_register_socket_type(*type);
}

static const NodeNPRLine *settings(const NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  return node ? static_cast<const NodeNPRLine *>(node->storage) : nullptr;
}

static void mode_layout(DeclarationListBuilder &b)
{
  b.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
    layout.prop(ptr, "mode", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  });
}

static decl::FloatBuilder &number(DeclarationListBuilder &b, const char *label, const char *id,
                                 float value, float min, float max)
{
  return b.add_input<decl::Float>(UString(label), UString(id)).default_value(value).min(min).max(max);
}

static void boolean(DeclarationListBuilder &b, const char *label, const char *id, bool value)
{
  b.add_input<decl::Bool>(UString(label), UString(id)).default_value(value);
}

static auto &descriptor_input(DeclarationListBuilder &b,
                             const char *label,
                             const char *id,
                             const char *type)
{
  return b.add_input<decl::Custom>(UString(label), UString(id)).idname(type).hide_value();
}

static void descriptor_output(DeclarationListBuilder &b, const char *label, const char *type)
{
  b.add_output<decl::Custom>(UString(label), "result"_ustr).idname(type);
}

static void source_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  descriptor_output(b, "Objects", npr_line_objects_socket);
  mode_layout(b);
  const NodeNPRLine *data = settings(b);
  b.add_input<decl::Collection>("Collection"_ustr, "collection"_ustr).available(!data || data->mode == 1);
  b.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
    const bNode &node = *ptr->data_as<bNode>();
    const auto &data = *static_cast<const NodeNPRLine *>(node.storage);
    if (data.mode == 0) {
      layout.label(IFACE_("Current view layer objects"), ICON_SCENE_DATA);
      return;
    }
    int count = 0;
    Collection *collection = nullptr;
    for (const bNodeSocket &socket : node.inputs) {
      if (data.mode == 2 && STRPREFIX(socket.identifier, "object_") &&
          ((socket.flag & SOCK_IS_LINKED) ||
           static_cast<const bNodeSocketValueObject *>(socket.default_value)->value)) {
        count++;
      }
      if (data.mode == 1 && STREQ(socket.identifier, "collection") &&
          ((socket.flag & SOCK_IS_LINKED) ||
           static_cast<const bNodeSocketValueCollection *>(socket.default_value)->value)) {
        count++;
        collection = static_cast<const bNodeSocketValueCollection *>(socket.default_value)->value;
      }
    }
    if (collection) {
      const auto object_cache = BKE_collection_object_cache_get(collection);
      const int objects = BLI_listbase_count(&object_cache);
      layout.label(std::string(collection->id.name + 2) + " · " + std::to_string(objects) + " " +
                       IFACE_("objects"), ICON_OUTLINER_COLLECTION);
    }
    else {
      layout.label(count ? std::to_string(count) + " " + IFACE_("object references") :
                           IFACE_("No line objects assigned"), count ? ICON_CHECKMARK : ICON_INFO);
    }
  });
  auto &objects = b.add_panel("Objects"_ustr).default_closed(false);
  objects.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
    PointerRNA op = layout.op("node.npr_line_add_selected", IFACE_("Add Selected"), ICON_ADD);
    RNA_string_set(&op, "node_name", ptr->data_as<bNode>()->name);
  });
  for (int i = 0; i < (data ? data->item_count : 1); i++) {
    objects.add_input<decl::Object>(UString("Object " + std::to_string(i + 1)),
                                    UString("object_" + std::to_string(i))).available(!data || data->mode == 2);
  }
  auto &exclude = b.add_panel("Exclude"_ustr).default_closed(true);
  exclude.add_input<decl::Collection>("Collection"_ustr, "exclude_collection"_ustr);
  exclude.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
    PointerRNA op = layout.op("node.npr_line_add_selected", IFACE_("Exclude Selected"), ICON_ADD);
    RNA_string_set(&op, "node_name", ptr->data_as<bNode>()->name);
    RNA_boolean_set(&op, "exclude", true);
  });
  for (int i = 0; i < (data ? data->exclude_count : 1); i++) {
    exclude.add_input<decl::Object>(UString("Object " + std::to_string(i + 1)),
                                    UString("exclude_" + std::to_string(i)));
  }
  auto &options = b.add_panel("Scope Options"_ustr).default_closed(true);
  boolean(options, "Include Child Collections", "children", true);
  boolean(options, "Include Instances", "instances", true);
  options.add_input<decl::Bool>("Connect Coincident Seams"_ustr, "repair_seams"_ustr).default_value(false)
      .description("Use virtual adjacency for an unambiguous pair of exactly coincident, oppositely wound boundary edges in the same mesh; keeps the original mesh unchanged");
}

static void brush_preview_layout(NodeDeclarationBuilder &b)
{
  b.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
    PropertyRNA *property = RNA_struct_find_property(ptr, "brush_preview_icon");
    const int icon = property ? RNA_property_int_get(ptr, property) : 0;
    if (icon) {
      /* The ordinary icon template fits into a square. Draw the wide swatch
       * directly so it fills the node without reserving a square of empty space. */
      const int width = std::max(layout.width(), int(8 * UI_UNIT_X));
      ui::Block *block = layout.absolute().block();
      uiDefBut(block, ui::ButtonType::Extra, "", 0, 0, width, width / 3,
               nullptr, 0, 0, std::nullopt);
      ui::button_func_drawextra_set(block, [icon](const bContext *, rcti *rect) {
        const int size = BLI_rcti_size_x(rect);
        GPU_blend(GPU_BLEND_ALPHA);
        ui::icon_draw_preview(rect->xmin, BLI_rcti_cent_y(rect) - size / 2,
                              icon, 1.0f, 1.0f, size);
        GPU_blend(GPU_BLEND_NONE);
      });
    }
  });
}

static void brush_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  descriptor_output(b, "Brush", npr_line_brush_socket);
  const NodeNPRLine *data = settings(b);
  const bool output = b.node_or_null() &&
                      b.node_or_null()->type_legacy == NPR_LINE_NODE_BRUSH_OUTPUT;
  const bool textured = !data || data->mode != 0;
  brush_preview_layout(b);
  mode_layout(b);
  const bool legacy_input = b.node_or_null() &&
                            b.node_or_null()->type_legacy == NPR_LINE_NODE_BRUSH_INPUT;
  if (output && data && !(data->curve_flags & (1 << 7))) {
    /* Read old experimental files until the load handler converts their chain to fields. */
    descriptor_input(b, "Legacy Brush", "brush", npr_line_brush_socket);
  }
  if (!legacy_input) {
    if (output) {
      b.add_input<decl::Vector>("Position Offset"_ustr, "position_offset"_ustr)
          .description("Local offset in output pixels: X advances along the source contour, "
                       "Y moves along its normal; unlinked is zero");
    }
    b.add_input<decl::Color>("Color"_ustr, "color"_ustr).default_value({0, 0, 0, 1});
    number(b, "Width (px)", "width", 2, output ? 0.0f : 0.001f, 1000)
        .description("Stroke width in output pixels before global and per-type multipliers");
    number(b, "Opacity", "opacity", 1, 0, 1);
  }
  b.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
    layout.prop(ptr, "raster_size_response", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    auto &tree = *reinterpret_cast<bNodeTree *>(ptr->owner_id);
    tree.ensure_topology_cache();
    const bNode &node = *ptr->data_as<bNode>();
    const auto *output = static_cast<const bNodeSocket *>(node.outputs.first_);
    const int count = output ? output->directly_linked_links().size() : 0;
    layout.label(std::to_string(count) + " " + IFACE_("brush connections"), ICON_LINKED);
    for (const bNodeSocket &socket : node.inputs) {
      if ((STREQ(socket.identifier, "width_map") || STREQ(socket.identifier, "color_map") ||
           STREQ(socket.identifier, "distortion_map")) && (socket.flag & SOCK_IS_LINKED)) {
        layout.label(IFACE_("View surface maps in the scene"), ICON_INFO);
        break;
      }
    }
    PointerRNA op = layout.op("node.npr_line_brush_preview", IFACE_("Preview Brush"), ICON_BRUSH_DATA);
    RNA_string_set(&op, "node_name", node.name);
  });
  auto &tip = b.add_panel("Tip and Texture"_ustr).default_closed(true);
  tip.add_input<decl::Image>("Image"_ustr, "image"_ustr).available(textured);
  descriptor_input(tip, "Brush Map", "brush_map", npr_line_map_socket)
      .description("Multiply tip coverage by the map RGB average; images use brush-local coordinates")
      .available(textured);
  number(tip, "Brush Map Opacity", "brush_map_opacity", 1, 0, 1)
      .description("Blend the map with the untextured tip; image alpha does not change this mask")
      .available(textured);
  number(tip, "Spacing", "spacing", 0, 0, 10)
      .description("Extra spacing as a fraction of brush width; zero draws a continuous stroke").available(textured);
  number(tip, "Aspect Ratio", "stretch", 1, 0.01f, 100)
      .description("Tip width divided by height, before rotation; one is circular").available(textured);
  number(tip, "Rotation", "angle", 0, -360, 360).available(textured);
  number(tip, "Softness", "softness", 0, 0, 1).available(textured);
  number(tip, "Groove", "groove", 0, 0, 1).available(textured);
  tip.add_input<decl::Int>("Groove Count"_ustr, "groove_count"_ustr)
      .default_value(5).min(3).max(20).available(textured);
  number(tip, "Tip Fade Start", "tip_reduction_start", 1, 0, 1)
      .description("Normalized radius where the brush tip begins to fade; separate from stroke start and end")
      .available(textured);
  number(tip, "Tip Fade End", "tip_reduction_end", 1, 0, 1).available(textured);
  tip.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
    if (static_cast<const NodeNPRLine *>(ptr->data_as<bNode>()->storage)->mode == 0) {
      layout.label(IFACE_("Choose Normal or Multiple for texture tips"), ICON_INFO);
    }
    else {
      layout.prop(ptr, "stroke_mode", ui::ITEM_R_SPLIT_EMPTY_NAME, IFACE_("Orientation"), ICON_NONE);
    }
  });
  auto &taper = b.add_panel("Start and End"_ustr).default_closed(true);
  taper.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
    layout.prop(ptr, "use_width_curve", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    if (static_cast<const NodeNPRLine *>(ptr->data_as<bNode>()->storage)->curve_flags & 1) {
      ui::template_curve_mapping(&layout, ptr, "width_curve", 's', false, false, false, false, false);
    }
  });
  number(taper, "Start Width", "taper_start", 0, 0, 0.5f)
      .description("Fraction of the stroke length used to fade the starting width from zero")
      .available(!data || !(data->curve_flags & 1));
  number(taper, "End Width", "taper_end", 0, 0, 0.5f)
      .description("Fraction of the stroke length used to fade the ending width to zero")
      .available(!data || !(data->curve_flags & 1));
  taper.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
    layout.prop(ptr, "use_opacity_curve", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    if (static_cast<const NodeNPRLine *>(ptr->data_as<bNode>()->storage)->curve_flags & 2) {
      ui::template_curve_mapping(&layout, ptr, "opacity_curve", 's', false, false, false, false, false);
    }
  });
  number(taper, "Start Opacity", "alpha_start", 0, 0, 0.5f)
      .description("Fraction of the stroke length used to fade in opacity")
      .available(!data || !(data->curve_flags & 2));
  number(taper, "End Opacity", "alpha_end", 0, 0, 0.5f)
      .description("Fraction of the stroke length used to fade out opacity")
      .available(!data || !(data->curve_flags & 2));
  auto &dash = b.add_panel("Dashed Line"_ustr).default_closed(true);
  boolean(dash, "Enabled", "dashed", false);
  number(dash, "Length", "dash_length", 8, 0.01f, 10000);
  number(dash, "Space", "dash_space", 5, 0, 10000);
  number(dash, "Phase", "dash_phase", 0, -10000, 10000);
  auto &random = b.add_panel("Variation"_ustr).default_closed(true);
  number(random, "Width Random", "width_random", 0, 0, 1);
  number(random, "Rotation Random", "angle_random", 0, 0, 360).available(textured);
  number(random, "Grain", "grain", 0, 0, 1);
  random.add_input<decl::Int>("Seed"_ustr, "seed"_ustr).default_value(0).min(0);
  auto &deform = b.add_panel("Path Deformation"_ustr).default_closed(true);
  boolean(deform, "Wave and Map", "distortion_enabled", true);
  descriptor_input(deform, "Distortion Map", "distortion_map", npr_line_map_socket);
  number(deform, "Map Amount (px)", "distortion_map_amount", 5, 0, 1000)
      .description("Offset the path along its normal by the map RGB average, in output pixels");
  number(deform, "Extend", "extend", 0, 0, 1000);
  number(deform, "Normal Offset", "offset", 0, -1000, 1000);
  number(deform, "Normal Offset Random (px)", "offset_random", 0, 0, 1000);
  number(deform, "X Offset", "shift_x", 0, -1000, 1000);
  number(deform, "Y Offset", "shift_y", 0, -1000, 1000);
  deform.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
    layout.prop(ptr, "wave_units", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  });
  const bool pixel_wave = !data || (data->curve_flags & (1 << 6));
  number(deform, "Wave", "wave", 0, 0, 1000);
  number(deform, "Wavelength (px)", "wave_length", 100, 5, 10000).available(pixel_wave);
  deform.add_input<decl::Float>("Wave Phase"_ustr, "wave_phase"_ustr).default_value(0)
      .min(-30 * float(M_PI)).max(30 * float(M_PI)).subtype(PROP_ANGLE).available(pixel_wave);
  number(deform, "Wave Amount Random", "wave_random", 0, 0, 1).available(pixel_wave);
  number(deform, "Wave Period Random", "wave_period_random", 0, 0, 1).available(pixel_wave);
  number(deform, "Wave Phase Random", "wave_phase_random", 0, 0, 1).available(pixel_wave);
  number(deform, "Cycles", "cycles", 3, 0, 1000).available(!pixel_wave);
  number(deform, "Noise", "noise", 0, 0, 1000).available(!pixel_wave);
  deform.add_input<decl::Int>("Copies"_ustr, "copies"_ustr).default_value(1).min(1).max(32);
  auto &dots = b.add_panel("Multiple Tip"_ustr).default_closed(true);
  const bool multiple = !data || data->mode == 2;
  number(dots, "Dot Size (%)", "dot_size", 16, 0.1f, 100)
      .description("Size of each point in the reusable tip pattern; independent of the stroke width")
      .available(multiple);
  number(dots, "Dot Size Random", "dot_size_random", 0.8f, 0, 1).available(multiple);
  number(dots, "Dot Antialiasing", "dot_antialiasing", 0.5f, 0, 10).available(multiple);
  number(dots, "Horizontal Space", "dot_space_x", 0.1f, 0, 1).available(multiple);
  number(dots, "Horizontal Random", "dot_space_random_x", 1, 0, 1).available(multiple);
  number(dots, "Vertical Space", "dot_space_y", 0.1f, 0, 1).available(multiple);
  number(dots, "Vertical Random", "dot_space_random_y", 1, 0, 1).available(multiple);
  auto &stroke = b.add_panel("Stroke Connection"_ustr).default_closed(true);
  stroke.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
    layout.prop(ptr, "override_stroke_connection", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    if (!(static_cast<const NodeNPRLine *>(ptr->data_as<bNode>()->storage)->curve_flags & (1 << 5))) {
      layout.label(IFACE_("Uses rule connection settings"), ICON_INFO);
    }
  });
  const bool custom_stroke = data && (data->curve_flags & (1 << 5));
  number(stroke, "Intersection Join", "stroke_intersection_join", 1, 0, 100).available(custom_stroke);
  number(stroke, "Gap Join", "stroke_gap_join", 2, 0, 100).available(custom_stroke);
  number(stroke, "Split Angle", "stroke_split_angle", 90, 0, 180).available(custom_stroke);
  number(stroke, "Minimum Stroke", "stroke_minimum_length", 0, 0, 10000).available(custom_stroke);
  number(stroke, "Direction", "stroke_direction", -30, -180, 180).available(custom_stroke);
  stroke.add_input<decl::Bool>("Clockwise Loops"_ustr, "stroke_clockwise"_ustr).default_value(true).available(custom_stroke);
  auto &maps = b.add_panel("Surface Maps"_ustr).default_closed(true);
  descriptor_input(maps, "Width Map", "width_map", npr_line_map_socket);
  number(maps, "Width Map Strength", "width_map_strength", 1, 0, 1);
  descriptor_input(maps, "Color Map", "color_map", npr_line_map_socket);
  number(maps, "Color Map Strength", "color_map_strength", 1, 0, 1)
      .description("Blend image RGB over the base color; image alpha does not change the color strength or stroke opacity");
  if (output) {
    auto &sampling = b.add_panel("Field Sampling"_ustr).default_closed(true);
    number(sampling, "Sample Length (px)", "sample_length", 0.9f, 0.1f, 1000)
        .description("Maximum local field sample interval; source boundaries and connection gaps are preserved");
  }
}

static void brush_closure_input_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.add_output<decl::Float>("Distance"_ustr, "Distance"_ustr)
      .description("Original pixel distance along the source contour");
  b.add_output<decl::Float>("Factor"_ustr, "Factor"_ustr)
      .description("Original distance divided by the complete stroke length");
  b.add_output<decl::Float>("Length"_ustr, "Length"_ustr)
      .description("Original complete stroke length in pixels");
  b.add_output<decl::Float>("ID"_ustr, "ID"_ustr)
      .description("Stable per-stroke identity for procedural variation");
  b.add_output<decl::Bool>("Closed"_ustr, "Closed"_ustr)
      .description("Whether the source stroke is a loop");
}

static void brush_closure_output_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  descriptor_output(b, "Brush", npr_line_brush_socket);
  brush_preview_layout(b);
  b.add_input<decl::Vector>("Position Offset"_ustr, "position_offset"_ustr)
      .dimensions(2)
      .description("Local pixel offset: X advances along the original contour, Y follows its normal");
  number(b, "Width (px)", "width", 2, 0, 1000);
  b.add_input<decl::Color>("Color"_ustr, "color"_ustr).default_value({0, 0, 0, 1});
  number(b, "Opacity", "opacity", 1, 0, 1);
}

static void material_filter(DeclarationListBuilder &b, const NodeNPRLine *data)
{
  auto &panel = b.add_panel("Material Filter"_ustr).default_closed(true);
  panel.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
    layout.prop(ptr, "material_count", ui::ITEM_R_SPLIT_EMPTY_NAME, IFACE_("Slots"), ICON_NONE);
  });
  for (int i = 0; i < (data ? data->material_count : 1); i++) {
    panel.add_input<decl::Material>(UString("Material " + std::to_string(i + 1)),
                                    UString("material_" + std::to_string(i)));
  }
  boolean(panel, "Exclude Materials", "exclude_materials", false);
}

static void settings_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  const NodeNPRLine *data = settings(b);
  descriptor_output(b, "Line Plan", npr_line_plan_socket);
  b.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
    layout.prop(ptr, "enabled", ui::ITEM_R_SPLIT_EMPTY_NAME, IFACE_("Enabled"), ICON_NONE);
  });
  descriptor_input(b, "Objects", "objects", npr_line_objects_socket);
  descriptor_input(b, "Visible Brush", "visible_brush", npr_line_brush_socket);
  descriptor_input(b, "Hidden Brush", "hidden_brush", npr_line_brush_socket);
  b.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
    const bNode &node = *ptr->data_as<bNode>();
    for (const bNodeSocket &socket : node.inputs) {
      if (STREQ(socket.identifier, "hidden_brush") && !(socket.flag & SOCK_IS_LINKED)) {
        layout.label(IFACE_("Unlinked hidden brush uses visible brush"), ICON_INFO);
      }
      if (ELEM(StringRef(socket.identifier), "objects", "visible_brush") &&
          !(socket.flag & SOCK_IS_LINKED)) {
        layout.label(STREQ(socket.identifier, "objects") ? IFACE_("Connect Line Objects") :
                                                          IFACE_("Connect a visible brush"), ICON_ERROR);
      }
    }
  });
  b.add_layout([](ui::Layout &layout, bContext *C, PointerRNA *ptr) {
    auto &tree = *reinterpret_cast<bNodeTree *>(ptr->owner_id);
    tree.ensure_topology_cache();
    const bNode &node = *ptr->data_as<bNode>();
    for (const char *identifier : {"visible_brush", "hidden_brush"}) {
      const bNodeSocket *socket = bke::node_find_socket(node, SOCK_IN, UString(identifier));
      if (!socket || socket->directly_linked_links().is_empty()) {
        continue;
      }
      const bNodeLink &link = *socket->directly_linked_links().first();
      if (ELEM(link.fromnode->type_legacy, NPR_LINE_NODE_BRUSH, NPR_LINE_NODE_BRUSH_OUTPUT,
               NPR_LINE_NODE_BRUSH_CLOSURE_OUTPUT) &&
          link.fromsock->directly_linked_links().size() > 1) {
        PointerRNA op = layout.op("node.npr_line_make_brush_unique",
                                  STREQ(identifier, "visible_brush") ? IFACE_("Copy Visible Brush") :
                                                                      IFACE_("Copy Hidden Brush"),
                                  ICON_DUPLICATE);
        RNA_string_set(&op, "node_name", node.name);
        RNA_string_set(&op, "socket_identifier", identifier);
      }
    }
    if (CTX_data_scene(C)->eevee.geometry_line_tree == &tree) {
      PointerRNA op = layout.op("node.npr_line_solo", IFACE_("Solo Preview"), ICON_SOLO_ON);
      RNA_string_set(&op, "node_name", node.name);
    }
  });
  for (int hidden = 0; hidden < 2; hidden++) {
    auto &panel = b.add_panel(hidden ? "Hidden Lines"_ustr : "Visible Lines"_ustr)
                      .default_closed(hidden != 0);
    if (hidden) {
      boolean(panel, "Enabled", "hidden_enabled", false);
      panel.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
        PointerRNA op = layout.op("node.npr_line_copy_visible", IFACE_("Copy Visible Settings"), ICON_COPYDOWN);
        RNA_string_set(&op, "node_name", ptr->data_as<bNode>()->name);
      });
    }
    for (int i = 0; i < 8; i++) {
      const std::string id = std::string(hidden ? "hidden_" : "visible_") + edge_ids[i];
      boolean(panel, edge_names[i], id.c_str(), i < 2);
    }
  }
  auto &types = b.add_panel("Per-Type Style"_ustr).default_closed(true);
  for (int i = 0; i < 8; i++) {
    number(types, edge_names[i], ("scale_" + std::string(edge_ids[i])).c_str(), 1, 0, 100);
  }
  types.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
    layout.prop(ptr, "visible_overrides", ui::ITEM_R_SPLIT_EMPTY_NAME, IFACE_("Visible Overrides"), ICON_NONE);
    layout.prop(ptr, "hidden_overrides", ui::ITEM_R_SPLIT_EMPTY_NAME, IFACE_("Hidden Overrides"), ICON_NONE);
  });
  for (int hidden = 0; hidden < 2; hidden++) {
    const int mask = data ? (hidden ? data->hidden_overrides : data->visible_overrides) : 0;
    for (int i = 0; i < 8; i++) {
      const std::string id = std::string(hidden ? "hidden_override_" : "visible_override_") + edge_ids[i];
      auto &input = types.add_input<decl::Custom>(
          UString(std::string(hidden ? "Hidden " : "Visible ") + edge_names[i]), UString(id))
                        .idname(npr_line_brush_socket).hide_value();
      if (!(mask & (1 << i))) {
        input.available(false);
      }
    }
  }
  auto &connect = b.add_panel("Stroke Connection"_ustr).default_closed(true);
  number(connect, "Intersection Join", "intersection_join", 1, 0, 100);
  number(connect, "Gap Join", "gap_join", 2, 0, 100);
  connect.add_input<decl::Bool>("Screen Space Joins"_ustr, "screen_space_join"_ustr).default_value(true)
      .description("Join small gaps on the screen plane; disable to also apply the depth tolerance");
  number(connect, "Depth Tolerance", "join_depth", 0.08f, 0, 10000)
      .description("Maximum camera-depth difference for intersection joins, and for gap joins when Screen Space Joins is disabled, in scene units");
  number(connect, "Split Angle", "split_angle", 70, 0, 180);
  number(connect, "Minimum Length", "minimum_length", 0, 0, 10000);
  number(connect, "Direction", "direction", 0, -360, 360);
  boolean(connect, "Clockwise Loops", "clockwise", true);
  boolean(connect, "Connect Objects", "connect_objects", false);
  connect.add_input<decl::Bool>("Join Coincident Endpoints"_ustr, "connect_coincident"_ustr).default_value(false)
      .description("Join exactly overlapping stroke endpoints within one object, including split vertices; does not modify surfaces or mesh topology");
  auto &detect = b.add_panel("Detection"_ustr).default_closed(true);
  detect.add_input<decl::Bool>("Contour Priority"_ustr, "contour_priority"_ustr).default_value(false)
      .description("Keep silhouette boundaries in Outline or Object categories; disabling a contour type also suppresses its wire and marked-edge fallback");
  boolean(detect, "Open Edges", "open_edges", true);
  detect.add_input<decl::Bool>("Per-Object Contours"_ustr, "object_contours"_ustr).default_value(false)
      .description("Classify outer and inner contours within each object; other objects still occlude lines. Disable to classify against the whole scene");
  boolean(detect, "Self Intersection", "self_intersection", true);
  auto &open_options = b.add_panel("Open Edge Options"_ustr).default_closed(true);
  boolean(open_options, "Visible Outline", "visible_open_outline", true);
  boolean(open_options, "Visible Inner Contour", "visible_open_object", true);
  boolean(open_options, "Hidden Outline", "hidden_open_outline", true);
  boolean(open_options, "Hidden Inner Contour", "hidden_open_object", true);
  number(detect, "Minimum Angle", "normal_min", 30, 0, 180);
  number(detect, "Maximum Angle", "normal_max", 180, 0, 180);
  material_filter(b, data);
  auto &distance = b.add_panel("Distance Reduction"_ustr).default_closed(true);
  descriptor_input(distance, "Visible Width Reduction", "width_reduction", npr_line_reduction_socket);
  descriptor_input(distance, "Visible Opacity Reduction", "opacity_reduction", npr_line_reduction_socket);
  descriptor_input(distance, "Hidden Width Reduction", "hidden_width_reduction", npr_line_reduction_socket);
  descriptor_input(distance, "Hidden Opacity Reduction", "hidden_opacity_reduction", npr_line_reduction_socket);
}

static void reduction_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  const NodeNPRLine *data = settings(b);
  descriptor_output(b, "Reduction", npr_line_reduction_socket);
  mode_layout(b);
  number(b, "Start Distance", "start", 1, 0, 100000).subtype(PROP_DISTANCE);
  number(b, "End Distance", "end", 10, 0, 100000).subtype(PROP_DISTANCE);
  b.add_input<decl::Object>("Reference Object"_ustr, "object"_ustr).available(data && data->mode == 1)
      .description("Use the evaluated object origin as the distance reference");
  b.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
    const bNode &node = *ptr->data_as<bNode>();
    const auto &data = *static_cast<const NodeNPRLine *>(node.storage);
    if (data.mode == 1) {
      for (const bNodeSocket &socket : node.inputs) {
        if (STREQ(socket.identifier, "object") && !(socket.flag & SOCK_IS_LINKED) &&
            !static_cast<const bNodeSocketValueObject *>(socket.default_value)->value) {
          layout.label(IFACE_("Choose a reference object; reduction is inactive"), ICON_INFO);
        }
      }
    }
    ui::template_curve_mapping(&layout, ptr, "curve", 's', false, false, false, false, false);
  });
}

static void map_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  const NodeNPRLine *data = settings(b);
  descriptor_output(b, "Map", npr_line_map_socket);
  b.add_input<decl::Image>("Image"_ustr, "image"_ustr).available(!data || data->mode != 2);
  b.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
    if (static_cast<const NodeNPRLine *>(ptr->data_as<bNode>()->storage)->mode == 2) { return; }
    for (const bNodeSocket &socket : ptr->data_as<bNode>()->inputs) {
      if (STREQ(socket.identifier, "image") && !(socket.flag & SOCK_IS_LINKED) &&
          !static_cast<const bNodeSocketValueImage *>(socket.default_value)->value) {
        layout.label(IFACE_("Choose an image to enable the map"), ICON_INFO);
      }
    }
  });
  mode_layout(b);
  b.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
    if (static_cast<const NodeNPRLine *>(ptr->data_as<bNode>()->storage)->mode == 0) {
      layout.prop(ptr, "uv_selection", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    }
    else if (static_cast<const NodeNPRLine *>(ptr->data_as<bNode>()->storage)->mode == 2) {
      layout.prop(ptr, "attribute_selection", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
      layout.label(IFACE_("Color attributes use RGB; alpha is ignored"), ICON_INFO);
      layout.label(IFACE_("Preview color attributes in the scene"), ICON_INFO);
    }
  });
  b.add_input<decl::Int>("UV Index"_ustr, "uv_index"_ustr).default_value(0).min(0).max(7)
      .available(!data || (data->mode == 0 && data->uv_selection == 0));
  b.add_input<decl::String>("UV Name"_ustr, "uv_name"_ustr).default_value("UVMap")
      .available(data && data->mode == 0 && data->uv_selection == 1);
  b.add_input<decl::Int>("Attribute Index"_ustr, "attribute_index"_ustr).default_value(0).min(0).max(7)
      .available(data && data->mode == 2 && data->curve_flags == 0);
  b.add_input<decl::String>("Attribute Name"_ustr, "attribute_name"_ustr).default_value("Color")
      .available(data && data->mode == 2 && data->curve_flags != 0);
  b.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
    if (static_cast<const NodeNPRLine *>(ptr->data_as<bNode>()->storage)->mode == 2) { return; }
    layout.prop(ptr, "wrap_u", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    layout.prop(ptr, "wrap_v", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    layout.prop(ptr, "filter_mode", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  });
  number(b, "Tiling U", "tiling_u", 1, -100000, 100000).available(!data || data->mode != 2);
  number(b, "Tiling V", "tiling_v", 1, -100000, 100000).available(!data || data->mode != 2);
  number(b, "Offset U", "offset_u", 0, -100000, 100000).available(!data || data->mode != 2);
  number(b, "Offset V", "offset_v", 0, -100000, 100000).available(!data || data->mode != 2);
  number(b, "Rotation", "rotation", 0, -3600, 3600).available(!data || data->mode != 2);
}

static void combine_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  descriptor_output(b, "Line Plan", npr_line_plan_socket);
  mode_layout(b);
  b.add_input<decl::Custom>("Plans (Top First)"_ustr, "plans"_ustr)
      .idname(npr_line_plan_socket).hide_value().multi_input();
  b.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
    auto &tree = *reinterpret_cast<bNodeTree *>(ptr->owner_id);
    tree.ensure_topology_cache();
    const bNode &node = *ptr->data_as<bNode>();
    const auto *socket = static_cast<const bNodeSocket *>(node.inputs.first_);
    if (!socket) { return; }
    for (const bNodeLink *link : socket->directly_linked_links()) {
      ui::Layout &row = layout.row(true);
      row.label(link->fromnode->label[0] ? link->fromnode->label : link->fromnode->name, ICON_NODE);
      for (int direction : {-1, 1}) {
        PointerRNA op = row.op("node.npr_line_move_rule", "", direction < 0 ? ICON_TRIA_UP : ICON_TRIA_DOWN);
        RNA_string_set(&op, "node_name", node.name);
        RNA_int_set(&op, "sort_id", link->multi_input_sort_id);
        RNA_int_set(&op, "direction", direction);
      }
    }
  });
}

static void occlusion_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  descriptor_output(b, "Occlusion", npr_line_occlusion_socket);
  mode_layout(b);
  descriptor_input(b, "Objects", "objects", npr_line_objects_socket);
  const NodeNPRLine *data = settings(b);
  const bool mask = !data || data->mode == 2;
  b.add_input<decl::Image>("Alpha Image"_ustr, "image"_ustr).available(mask);
  b.add_input<decl::String>("UV Map"_ustr, "uv_map"_ustr).available(mask);
  number(b, "Threshold", "threshold", 0.5f, 0, 1).available(mask);
  b.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
    const bNode &node = *ptr->data_as<bNode>();
    const bNodeSocket *objects = bke::node_find_socket(node, SOCK_IN, "objects"_ustr);
    if (!(objects->flag & SOCK_IS_LINKED)) {
      layout.label(IFACE_("Connect Line Objects"), ICON_ERROR);
    }
    if (static_cast<const NodeNPRLine *>(node.storage)->mode == 2) {
      const bNodeSocket *image = bke::node_find_socket(node, SOCK_IN, "image"_ustr);
      if (!(image->flag & SOCK_IS_LINKED) &&
          !static_cast<const bNodeSocketValueImage *>(image->default_value)->value) {
        layout.label(IFACE_("Assign an alpha mask image"), ICON_ERROR);
      }
    }
  });
  material_filter(b, settings(b));
}

static void output_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
    layout.prop(ptr, "is_active_output", ui::ITEM_R_SPLIT_EMPTY_NAME, IFACE_("Active Output"), ICON_NONE);
    const bNode &node = *ptr->data_as<bNode>();
    const bNodeSocket *plan = bke::node_find_socket(node, SOCK_IN, "plan"_ustr);
    if (!(plan->flag & SOCK_IS_LINKED)) {
      layout.label(IFACE_("Connect a line plan"), ICON_ERROR);
    }
  });
  descriptor_input(b, "Line Plan", "plan", npr_line_plan_socket);
  b.add_input<decl::Custom>("Occlusion Rules (Top First)"_ustr, "occlusion"_ustr)
      .idname(npr_line_occlusion_socket).hide_value().multi_input();
  boolean(b, "Composite", "composite", true);
  mode_layout(b);
  number(b, "Width Scale", "width_scale", 1, 0, 100);
  auto &quality = b.add_panel("Output"_ustr).default_closed(true);
  boolean(quality, "Relative Width", "relative_width", false);
  number(quality, "Reference Width", "reference_width", 1920, 1, 32768);
  boolean(quality, "Depth of Field", "depth_of_field", false);
  boolean(quality, "Motion Blur", "motion_blur", false);
  number(quality, "Antialiasing", "antialiasing", 1, 0, 4);
  number(quality, "Field Sample Length (px)", "field_sample_length", 0.9f, 0.1f, 1000)
      .description("Shared Brush Closure sampling quality; preserves source boundaries and connection gaps");
  number(quality, "Offscreen Stroke Margin", "offscreen_margin", 0, 0, 1000)
      .description("Continue stroke detection and joining outside the image border in pixels; zero uses the automatic brush margin");
  quality.add_input<decl::Int>("Over Sampling"_ustr, "samples"_ustr).default_value(1).min(1).max(4)
      .description("Samples per pixel axis; two evaluates a 2 by 2 subpixel grid");
  auto &timing = b.add_panel("Performance"_ustr).default_closed(false);
  timing.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
    auto &tree = *reinterpret_cast<bNodeTree *>(ptr->owner_id);
    PointerRNA tree_ptr = RNA_id_pointer_create(&tree.id);
    layout.prop(&tree_ptr, "show_performance", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_TIME);
    if (npr_line_timing_enabled(tree)) {
      const std::string text = npr_line_timing(tree);
      std::istringstream lines(text);
      std::string line;
      while (std::getline(lines, line)) { layout.label(line, ICON_NONE); }
      if (text.empty()) { layout.label(IFACE_("Render the viewport to measure"), ICON_INFO); }
    }
  });
}

static void position_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  descriptor_output(b, "Brush", npr_line_brush_socket);
  descriptor_input(b, "Brush", "brush", npr_line_brush_socket);
  boolean(b, "Selection", "selection", true);
  b.add_input<decl::Vector>("Position"_ustr, "position"_ustr).hide_value()
      .description("Unlinked uses the incoming position; X is source arc length in pixels, Y is normal offset");
  b.add_input<decl::Vector>("Offset"_ustr, "offset"_ustr);
}

static void info_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  const NodeNPRLine *data = settings(b);
  if (data && !(data->curve_flags & (1 << 7))) {
    descriptor_input(b, "Legacy Brush", "brush", npr_line_brush_socket);
  }
  b.add_output<decl::Vector>("Position"_ustr, "position"_ustr);
  for (const char *name : {"Distance", "Length", "Factor", "ID"}) {
    b.add_output<decl::Float>(UString(name), UString(name));
  }
  for (const char *name : {"Start", "End", "Closed"}) {
    b.add_output<decl::Bool>(UString(name), UString(name));
  }
}

static void resample_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  descriptor_output(b, "Brush", npr_line_brush_socket);
  descriptor_input(b, "Brush", "brush", npr_line_brush_socket);
  number(b, "Length (px)", "length", 1, 0.1f, 1000);
}

static void source_uv_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("Name"_ustr, "name"_ustr);
  b.add_output<decl::Vector>("UV"_ustr, "UV"_ustr);
  b.add_output<decl::Bool>("Valid"_ustr, "Valid"_ustr);
}

static void source_color_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("Name"_ustr, "name"_ustr);
  b.add_output<decl::Color>("Color"_ustr, "Color"_ustr);
  b.add_output<decl::Float>("Alpha"_ustr, "Alpha"_ustr);
  b.add_output<decl::Bool>("Valid"_ustr, "Valid"_ustr);
}

static void source_aov_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  mode_layout(b);
  b.add_input<decl::String>("Name"_ustr, "name"_ustr).default_value("LineColor");
  b.add_input<decl::Color>("Fallback Color"_ustr, "color"_ustr).default_value({0, 0, 0, 1});
  number(b, "Fallback Value", "value", 0, -100000, 100000);
  b.add_output<decl::Color>("Color"_ustr, "Color"_ustr);
  b.add_output<decl::Float>("Value"_ustr, "Value"_ustr);
  b.add_output<decl::Bool>("Valid"_ustr, "Valid"_ustr);
}

static void node_init(bNodeTree *tree, bNode *node)
{
  node->storage = MEM_new<NodeNPRLine>(__func__);
  if (ELEM(node->type_legacy, NPR_LINE_NODE_BRUSH, NPR_LINE_NODE_BRUSH_INPUT,
           NPR_LINE_NODE_BRUSH_OUTPUT)) {
    static_cast<NodeNPRLine *>(node->storage)->curve_flags |= (1 << 6) | 4;
  }
  if (ELEM(node->type_legacy, NPR_LINE_NODE_BRUSH_OUTPUT, NPR_LINE_NODE_STROKE_INFO)) {
    static_cast<NodeNPRLine *>(node->storage)->curve_flags |= (1 << 7);
  }
  ensure_brush_curves(*node);
  node->width = 250;
  if (node->type_legacy == NPR_LINE_NODE_OUTPUT) {
    for (bNode &other : tree->nodes) {
      if (other.type_legacy == NPR_LINE_NODE_OUTPUT) {
        other.flag &= ~NODE_DO_OUTPUT;
      }
    }
    node->flag |= NODE_DO_OUTPUT;
  }
}

static bool insert_link(bke::NodeInsertLinkParams &params)
{
  if (params.link.fromsock && params.link.tosock &&
      params.link.fromnode->type_legacy != NODE_REROUTE && params.link.tonode->type_legacy != NODE_REROUTE &&
      (params.link.fromsock->type == SOCK_CUSTOM || params.link.tosock->type == SOCK_CUSTOM) &&
      !STREQ(params.link.fromsock->idname, params.link.tosock->idname))
  {
    return false;
  }
  return node_insert_link_default(params);
}

void register_npr_line_nodes()
{
  register_socket(npr_line_objects_socket, "NodeTreeInterfaceSocketNPRLineObjects", N_("Line Objects"));
  register_socket(npr_line_brush_socket, "NodeTreeInterfaceSocketNPRLineBrush", N_("Line Brush"));
  register_socket(npr_line_plan_socket, "NodeTreeInterfaceSocketNPRLinePlan", N_("Line Plan"));
  register_socket(npr_line_occlusion_socket, "NodeTreeInterfaceSocketNPRLineOcclusion", N_("Line Occlusion"));
  register_socket(npr_line_reduction_socket, "NodeTreeInterfaceSocketNPRLineReduction", N_("Distance Reduction"));
  register_socket(npr_line_map_socket, "NodeTreeInterfaceSocketNPRLineMap", N_("Line Map"));

  auto *tree = MEM_new<bke::bNodeTreeType>(__func__);
  tree->type = NTREE_NPR_LINE;
  tree->idname = UString(npr_line_tree_idname);
  tree->group_idname = "NPRLineNodeGroup"_ustr;
  tree->ui_name = N_("Geometry Lines");
  tree->ui_description = N_("Scene-wide geometry line rules and reusable brushes");
  tree->ui_icon = ICON_GREASEPENCIL;
  tree->get_from_context = tree_context;
  tree->update = tree_update;
  tree->valid_socket_type = tree_socket_valid;
  tree->validate_link = [](eNodeSocketDatatype a, eNodeSocketDatatype b) {
    const auto numeric = [](eNodeSocketDatatype type) {
      return ELEM(type, SOCK_FLOAT, SOCK_INT, SOCK_BOOLEAN, SOCK_VECTOR, SOCK_RGBA);
    };
    return a == b || (numeric(a) && numeric(b));
  };
  tree->rna_ext.srna = RNA_struct_find(npr_line_tree_idname);
  bke::node_tree_type_add(*tree);

  struct Definition {
    const char *idname;
    const char *label;
    int legacy;
    int nclass;
    void (*declare)(NodeDeclarationBuilder &);
  };
  const Definition definitions[] = {
      {"NPRLineNodeObjects", "Line Objects", NPR_LINE_NODE_OBJECTS, NODE_CLASS_INPUT, source_declare},
      {"NPRLineNodeBrush", "Line Brush", NPR_LINE_NODE_BRUSH, NODE_CLASS_INPUT, brush_declare},
      {"NPRLineNodeSettings", "Line Settings", NPR_LINE_NODE_SETTINGS, NODE_CLASS_OP_FILTER, settings_declare},
      {"NPRLineNodeOutput", "Geometry Line Output", NPR_LINE_NODE_OUTPUT, NODE_CLASS_OUTPUT, output_declare},
      {"NPRLineNodeCombine", "Combine Lines", NPR_LINE_NODE_COMBINE, NODE_CLASS_OP_FILTER, combine_declare},
      {"NPRLineNodeOcclusion", "Line Occlusion", NPR_LINE_NODE_OCCLUSION, NODE_CLASS_OP_FILTER, occlusion_declare},
      {"NPRLineNodeReduction", "Distance Reduction", NPR_LINE_NODE_REDUCTION, NODE_CLASS_OP_FILTER, reduction_declare},
      {"NPRLineNodeMap", "Line Map", NPR_LINE_NODE_MAP, NODE_CLASS_INPUT, map_declare},
      {"NPRLineNodeBrushInput", "Brush Input", NPR_LINE_NODE_BRUSH_INPUT, NODE_CLASS_INPUT, brush_declare},
      {"NPRLineNodeBrushOutput", "Legacy Brush Output", NPR_LINE_NODE_BRUSH_OUTPUT, NODE_CLASS_OP_FILTER, brush_declare},
      {"NPRLineNodeBrushClosureInput", "Brush Parameters", NPR_LINE_NODE_BRUSH_CLOSURE_INPUT, NODE_CLASS_INTERFACE, brush_closure_input_declare},
      {"NPRLineNodeBrushClosureOutput", "Brush Output", NPR_LINE_NODE_BRUSH_CLOSURE_OUTPUT, NODE_CLASS_INTERFACE, brush_closure_output_declare},
      {"NPRLineNodeSetPosition", "Set Position", NPR_LINE_NODE_SET_POSITION, NODE_CLASS_OP_FILTER, position_declare},
      {"NPRLineNodeStrokeInfo", "Stroke Info", NPR_LINE_NODE_STROKE_INFO, NODE_CLASS_INPUT, info_declare},
      {"NPRLineNodeResample", "Resample Stroke", NPR_LINE_NODE_RESAMPLE, NODE_CLASS_OP_FILTER, resample_declare},
      {"NPRLineNodeUVMap", "UV Map", NPR_LINE_NODE_UV, NODE_CLASS_INPUT, source_uv_declare},
      {"NPRLineNodeColorAttribute", "Color Attribute", NPR_LINE_NODE_COLOR_ATTRIBUTE, NODE_CLASS_INPUT, source_color_declare},
      {"NPRLineNodeMaterialAOV", "Material AOV", NPR_LINE_NODE_AOV, NODE_CLASS_INPUT, source_aov_declare},
  };
  for (const Definition &definition : definitions) {
    auto *type = MEM_new<bke::bNodeType>(__func__);
    bke::node_type_base(*type, UString(definition.idname), definition.legacy);
    type->ui_name = definition.label;
    type->nclass = definition.nclass;
    type->poll = node_poll;
    if (ELEM(definition.legacy, NPR_LINE_NODE_BRUSH_INPUT, NPR_LINE_NODE_BRUSH_OUTPUT,
             NPR_LINE_NODE_STROKE_INFO, NPR_LINE_NODE_SET_POSITION, NPR_LINE_NODE_RESAMPLE)) {
      /* Old experiments remain readable, but are not a second authoring workflow. */
      type->add_ui_poll = [](const bContext *) { return false; };
    }
    if (ELEM(definition.legacy, NPR_LINE_NODE_BRUSH_CLOSURE_INPUT,
             NPR_LINE_NODE_BRUSH_CLOSURE_OUTPUT)) {
      type->no_muting = true;
    }
    if (definition.legacy == NPR_LINE_NODE_BRUSH_CLOSURE_INPUT) {
      /* Parameters are created with their closure, never as an unpaired data source. */
      type->add_ui_poll = [](const bContext *) { return false; };
    }
    type->declare = definition.declare;
    type->initfunc = node_init;
    type->insert_link = insert_link;
    type->gather_link_search_ops = search_link_ops_for_basic_node;
    if (definition.legacy == NPR_LINE_NODE_BRUSH_CLOSURE_OUTPUT) {
      type->gather_link_search_ops = [](GatherLinkSearchOpParams &params) {
        if (params.other_socket().in_out != SOCK_IN ||
            !STREQ(params.other_socket().idname, npr_line_brush_socket)) { return; }
        params.add_item_full_name(IFACE_("Brush Closure"), [](LinkSearchOpParams &p) {
          bNode &start = p.add_node("NPRLineNodeBrushClosureInput"_ustr);
          bNode &end = p.add_node("NPRLineNodeBrushClosureOutput"_ustr);
          static_cast<NodeNPRLine *>(start.storage)->output_node_id = end.identifier;
          start.location[0] = -300;
          end.location[0] = 300;
          p.connect_available_socket(end, "Brush"_ustr);
        });
      };
    }
    type->default_width = 250;
    type->free_self = [](bke::bNodeType *self) { MEM_delete(self); };
    bke::node_type_storage(*type, "NodeNPRLine",
      [](bNode *node) {
        auto *data = static_cast<NodeNPRLine *>(node->storage);
        if (data->width_curve) { BKE_curvemapping_free(data->width_curve); }
        if (data->opacity_curve) { BKE_curvemapping_free(data->opacity_curve); }
        MEM_delete(data);
      },
      [](bNodeTree *, bNode *target, const bNode *source) {
        const auto &data = *static_cast<const NodeNPRLine *>(source->storage);
        auto *copy = MEM_new<NodeNPRLine>(__func__, data);
        copy->width_curve = data.width_curve ? BKE_curvemapping_copy(data.width_curve) : nullptr;
        copy->opacity_curve = data.opacity_curve ? BKE_curvemapping_copy(data.opacity_curve) : nullptr;
        target->storage = copy;
      });
    type->blend_write_storage_content = [](const bNodeTree &, const bNode &node, BlendWriter &writer) {
      const auto &data = *static_cast<const NodeNPRLine *>(node.storage);
      if (data.width_curve) { BKE_curvemapping_blend_write(&writer, data.width_curve); }
      if (data.opacity_curve) { BKE_curvemapping_blend_write(&writer, data.opacity_curve); }
    };
    type->blend_data_read_storage_content = [](bNodeTree &, bNode &node, BlendDataReader &reader) {
      auto &data = *static_cast<NodeNPRLine *>(node.storage);
      BLO_read_struct(&reader, CurveMapping, &data.width_curve);
      BLO_read_struct(&reader, CurveMapping, &data.opacity_curve);
      if (data.width_curve) { BKE_curvemapping_blend_read(&reader, data.width_curve); }
      if (data.opacity_curve) { BKE_curvemapping_blend_read(&reader, data.opacity_curve); }
      ensure_brush_curves(node);
    };
    bke::node_register_type(*type);
  }
  static bke::bNodeType group;
  bke::node_type_base_custom(group, "NPRLineNodeGroup", "Group", "GROUP", NODE_CLASS_GROUP);
  group.type_legacy = NODE_GROUP;
  group.poll = node_poll;
  group.poll_instance = node_group_poll_instance;
  group.insert_link = insert_link;
  group.ui_description_fn = node_group_ui_description;
  group.rna_ext.srna = RNA_struct_find("NPRLineNodeGroup");
  RNA_struct_blender_type_set(group.rna_ext.srna, &group);
  group.labelfunc = node_group_label;
  group.declare = node_group_declare;
  bke::node_register_type(group);
}

}  // namespace blender::nodes
