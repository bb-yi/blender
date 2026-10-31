/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <climits>
#include <sstream>

#include "BLI_fileops.h"
#include "BLI_hash_md5.hh"
#include "BLI_listbase.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"

#include "DNA_packedFile_types.h"
#include "DNA_space_types.h"

#include "BKE_context.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_main_invariants.hh"
#include "BKE_node.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_packedFile.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"
#include "GPU_shader.hh"
#include "IMB_imbuf.hh"
#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "WM_api.hh"
#include "WM_types.hh"

#include "COM_node_operation.hh"
#include "COM_ocio_color_space_conversion_shader.hh"
#include "COM_utilities.hh"
#include "NOD_eval_log.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_lut_cc {

NODE_STORAGE_FUNCS(NodeCompositorLUT)

static void packed_hash_update(NodeCompositorLUT &settings)
{
  settings.packed_hash[0] = '\0';
  if (settings.packed_file) {
    unsigned char digest[16];
    BLI_hash_md5_buffer(
        static_cast<const char *>(settings.packed_file->data), settings.packed_file->size, digest);
    BLI_hash_md5_to_hexdigest(digest, settings.packed_hash);
  }
}

static void file_info_update(NodeCompositorLUT &settings, const char *filepath)
{
  settings.file_info[0] = '\0';
  if (!BLI_path_extension_check(filepath, ".cube")) {
    ImBuf *image = IMB_load_image_from_filepath(
        filepath,
        ImBufFlags::Test | ImBufFlags::NoColorspaceConvert | ImBufFlags::AlphaChannelPacked);
    if (image) {
      const int size = compositor::ocio_lut_image_size(image->x, image->y);
      if (size) {
        SNPRINTF(settings.file_info,
                 "3D: %d x %d x %d; grid %d x %d",
                 size,
                 size,
                 size,
                 image->x / size,
                 image->y / size);
      }
      else {
        SNPRINTF(settings.file_info, "Image: %d x %d (invalid slice layout)", image->x, image->y);
      }
      IMB_freeImBuf(image);
    }
    return;
  }
  FILE *file = BLI_fopen(filepath, "rb");
  if (!file) {
    return;
  }
  char buffer[65536];
  const size_t size = fread(buffer, 1, sizeof(buffer), file);
  fclose(file);
  std::istringstream text(std::string(buffer, size));
  int one_d = 0, three_d = 0;
  std::string line;
  while (std::getline(text, line)) {
    std::istringstream entry(line);
    std::string keyword;
    entry >> keyword;
    if (keyword == "LUT_1D_SIZE") {
      entry >> one_d;
    }
    if (keyword == "LUT_3D_SIZE") {
      entry >> three_d;
    }
  }
  if (one_d && three_d) {
    SNPRINTF(settings.file_info, "1D (%d) + 3D (%d x %d x %d)", one_d, three_d, three_d, three_d);
  }
  else if (three_d) {
    SNPRINTF(settings.file_info, "3D: %d x %d x %d", three_d, three_d, three_d);
  }
  else if (one_d) {
    SNPRINTF(settings.file_info, "1D: %d", one_d);
  }
}

static void node_free(bNode *node)
{
  auto *settings = static_cast<NodeCompositorLUT *>(node->storage);
  if (settings->packed_file) {
    BKE_packedfile_free(settings->packed_file);
  }
  MEM_delete(settings);
}

static void node_copy(bNodeTree *, bNode *destination, const bNode *source)
{
  const auto &original = node_storage(*source);
  auto *settings = MEM_dupalloc(static_cast<NodeCompositorLUT *>(source->storage));
  settings->packed_file = original.packed_file ? BKE_packedfile_duplicate(original.packed_file) :
                                                 nullptr;
  destination->storage = settings;
}

static void node_blend_write(const bNodeTree &, const bNode &node, BlendWriter &writer)
{
  BKE_packedfile_blend_write(&writer, node_storage(node).packed_file);
}

static void node_blend_read(bNodeTree &, bNode &node, BlendDataReader &reader)
{
  auto &settings = node_storage(node);
  BKE_packedfile_blend_read(&reader, &settings.packed_file, settings.filepath);
  packed_hash_update(settings);
}

static const EnumPropertyItem file_actions[] = {
    {0, "OPEN", 0, "Open", "Select a .cube or sliced image LUT file"},
    {1, "RELOAD", 0, "Reload", "Reload the LUT from disk, including packed data"},
    {2, "PACK", 0, "Pack", "Store the LUT file inside this blend file"},
    {3, "UNPACK", 0, "Unpack", "Restore the original file and use the external LUT"},
    {4, "CLEAR", 0, "Clear", "Remove the LUT from this node"},
    {5, "RELATIVE", 0, "Make Relative", "Make the LUT path relative to this blend file"},
    {0, nullptr, 0, nullptr, nullptr},
};

static PointerRNA lut_pointer(const bContext *C)
{
  return CTX_data_pointer_get_type(C, "node", RNA_CompositorNodeLUT);
}

static bool file_poll(bContext *C)
{
  const PointerRNA ptr = lut_pointer(C);
  return ptr.data && ptr.owner_id && ID_IS_EDITABLE(ptr.owner_id);
}

static wmOperatorStatus file_exec(bContext *C, wmOperator *op)
{
  PointerRNA ptr = lut_pointer(C);
  char tree_name[MAX_ID_NAME];
  RNA_string_get(op->ptr, "node_tree_name", tree_name);
  if (tree_name[0]) {
    ptr = {};
    const int identifier = RNA_int_get(op->ptr, "node_identifier");
    for (bNodeTree &tree : CTX_data_main(C)->nodetrees) {
      if (STREQ(tree.id.name, tree_name) && ID_IS_EDITABLE(&tree.id)) {
        for (bNode &node : tree.nodes) {
          if (node.identifier == identifier && node.type_legacy == CMP_NODE_LUT) {
            ptr = RNA_pointer_create_discrete(&tree.id, RNA_CompositorNodeLUT, &node);
            break;
          }
        }
        break;
      }
    }
  }
  if (!ptr.data || !ptr.owner_id || !ID_IS_EDITABLE(ptr.owner_id)) {
    BKE_report(op->reports, RPT_ERROR, "The LUT node is no longer available for editing");
    return OPERATOR_CANCELLED;
  }
  bNode &node = *ptr.data_as<bNode>();
  bNodeTree &tree = *id_cast<bNodeTree *>(ptr.owner_id);
  auto &settings = node_storage(node);
  const char *base = BKE_main_blendfile_path(CTX_data_main(C));
  char filepath[FILE_MAX];
  STRNCPY(filepath, settings.filepath);
  const int action = RNA_enum_get(op->ptr, "action");
  if (action == 0) {
    RNA_string_get(op->ptr, "filepath", filepath);
  }
  BLI_path_abs(filepath, base);

  if (ELEM(action, 0, 1, 2)) {
    const size_t size = BLI_file_size(filepath);
    if (!BLI_path_extension_check_n(filepath, ".cube", ".png", ".exr", ".tif", ".tiff", nullptr) ||
        size == 0 || size > 64 * 1024 * 1024)
    {
      BKE_report(op->reports,
                 RPT_ERROR,
                 "Select a non-empty .cube, PNG, TIFF, or EXR LUT smaller than 64 MiB");
      return OPERATOR_CANCELLED;
    }
    PackedFile *packed = nullptr;
    if (action == 2 || (action == 1 && settings.packed_file)) {
      packed = BKE_packedfile_new(op->reports, filepath, base);
      if (!packed) {
        return OPERATOR_CANCELLED;
      }
    }
    if (settings.packed_file) {
      BKE_packedfile_free(settings.packed_file);
    }
    settings.packed_file = packed;
    if (action == 0) {
      if (RNA_boolean_get(op->ptr, "relative_path") && base[0]) {
        BLI_path_rel(filepath, base);
      }
      STRNCPY(settings.filepath, filepath);
    }
    char absolute_path[FILE_MAX];
    STRNCPY(absolute_path, filepath);
    BLI_path_abs(absolute_path, base);
    file_info_update(settings, absolute_path);
  }
  else if (action == 3 && settings.packed_file) {
    if (!BLI_path_extension_check_n(filepath, ".cube", ".png", ".exr", ".tif", ".tiff", nullptr)) {
      BKE_report(op->reports,
                 RPT_ERROR,
                 "Choose a .cube, PNG, TIFF, or EXR path before unpacking the LUT");
      return OPERATOR_CANCELLED;
    }
    if (BLI_exists(filepath) && BKE_packedfile_compare_to_file(
                                    base, settings.filepath, settings.packed_file) != PF_CMP_EQUAL)
    {
      BKE_report(
          op->reports,
          RPT_ERROR,
          "The external LUT differs from the packed LUT; choose a new file path before unpacking");
      return OPERATOR_CANCELLED;
    }
    if (!BLI_exists(filepath) &&
        BKE_packedfile_write_to_file(op->reports, base, settings.filepath, settings.packed_file) !=
            RET_OK)
    {
      return OPERATOR_CANCELLED;
    }
    BKE_packedfile_free(settings.packed_file);
    settings.packed_file = nullptr;
  }
  else if (action == 4) {
    if (settings.packed_file) {
      BKE_packedfile_free(settings.packed_file);
      settings.packed_file = nullptr;
    }
    settings.filepath[0] = '\0';
    settings.file_info[0] = '\0';
  }
  else if (action == 5) {
    if (!base[0]) {
      BKE_report(
          op->reports, RPT_ERROR, "Save the blend file before making the LUT path relative");
      return OPERATOR_CANCELLED;
    }
    BLI_path_rel(filepath, base);
    STRNCPY(settings.filepath, filepath);
  }
  packed_hash_update(settings);
  settings.reload_version = settings.reload_version == INT_MAX ? 0 : settings.reload_version + 1;
  BKE_ntree_update_tag_node_property(&tree, &node);
  BKE_main_ensure_invariants(*CTX_data_main(C));
  DEG_id_tag_update(&tree.id, ID_RECALC_NTREE_OUTPUT);
  WM_event_add_notifier(C, NC_NODE | NA_EDITED, &tree.id);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus file_invoke(bContext *C, wmOperator *op, const wmEvent *)
{
  if (RNA_enum_get(op->ptr, "action") != 0) {
    return file_exec(C, op);
  }
  if (!RNA_struct_property_is_set(op->ptr, "filepath")) {
    RNA_string_set(op->ptr, "filepath", node_storage(*lut_pointer(C).data_as<bNode>()).filepath);
  }
  const PointerRNA ptr = lut_pointer(C);
  RNA_string_set(op->ptr, "node_tree_name", ptr.owner_id->name);
  RNA_int_set(op->ptr, "node_identifier", ptr.data_as<bNode>()->identifier);
  WM_event_add_fileselect(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static void register_operators()
{
  WM_operatortype_append([](wmOperatorType *ot) {
    ot->name = "LUT File";
    ot->idname = "NODE_OT_lut_file";
    ot->description = "Manage the LUT file for the current compositor node";
    ot->poll = file_poll;
    ot->exec = file_exec;
    ot->invoke = file_invoke;
    ot->flag = OPTYPE_UNDO | OPTYPE_INTERNAL;
    RNA_def_enum(ot->srna, "action", file_actions, 0, "Action", "File operation");
    WM_operator_properties_filesel(ot,
                                   FILE_TYPE_FOLDER,
                                   FILE_SPECIAL,
                                   FILE_OPENFILE,
                                   WM_FILESEL_FILEPATH | WM_FILESEL_RELPATH,
                                   FILE_DEFAULTDISPLAY,
                                   FILE_SORT_DEFAULT);
    PropertyRNA *prop = RNA_def_string(
        ot->srna, "filter_glob", "*.cube;*.png;*.exr;*.tif;*.tiff", 0, "", "");
    RNA_def_property_flag(prop, PROP_HIDDEN);
    prop = RNA_def_string(
        ot->srna, "node_tree_name", nullptr, MAX_ID_NAME, "Node Tree", "Target node group");
    RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
    prop = RNA_def_int(ot->srna, "node_identifier", 0, INT_MIN, INT_MAX, "", "", INT_MIN, INT_MAX);
    RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  });
}

static void file_button(ui::Layout &layout, const char *label, int icon, int action)
{
  PointerRNA props = layout.op("NODE_OT_lut_file", label, icon);
  RNA_enum_set(&props, "action", action);
}

static void draw_buttons(ui::Layout &layout, bContext *, PointerRNA *ptr)
{
  layout.context_ptr_set("node", ptr);
  const auto &settings = node_storage(*ptr->data_as<bNode>());
  ui::Layout &row = layout.row(true);
  /* Keep one file selector, using the LUT operator to replace packed contents and update metadata.
   */
  uiDefButR(row.block(),
            ui::ButtonType::Text,
            "",
            0,
            0,
            10 * UI_UNIT_X,
            UI_UNIT_Y,
            ptr,
            "filepath",
            -1,
            0,
            0,
            std::nullopt);
  file_button(row, "", ICON_FILE_FOLDER, 0);
  file_button(row, "", ICON_FILE_REFRESH, 1);
  file_button(row, "", ICON_X, 4);
  layout.label(settings.packed_file ? IFACE_("Packed LUT") :
               settings.filepath[0] ? IFACE_("External LUT") :
                                      IFACE_("No LUT selected"),
               settings.packed_file ? ICON_PACKAGE : ICON_NONE);
  if (settings.file_info[0]) {
    layout.label(settings.file_info, ICON_NONE);
  }
  layout.prop(ptr, "preset", UI_ITEM_NONE, IFACE_("Preset"), ICON_NONE);
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_output<decl::Color>("Image"_ustr).structure_type(StructureType::Dynamic);
  b.add_default_layout();
  b.add_panel("Advanced"_ustr)
      .default_closed(true)
      .add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
        if (RNA_enum_get(ptr, "preset") == CMP_NODE_LUT_CUSTOM) {
          layout.prop_with_menu(ptr,
                                "input_color_space",
                                UI_ITEM_NONE,
                                std::nullopt,
                                ICON_NONE,
                                "UI_MT_color_space_select");
          layout.prop(ptr, "use_same_color_space", UI_ITEM_NONE, std::nullopt, ICON_NONE);
          if (!RNA_boolean_get(ptr, "use_same_color_space")) {
            layout.prop_with_menu(ptr,
                                  "output_color_space",
                                  UI_ITEM_NONE,
                                  std::nullopt,
                                  ICON_NONE,
                                  "UI_MT_color_space_select");
          }
        }
        layout.prop(ptr, "interpolation", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        layout.prop(ptr, "direction", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        const auto &settings = node_storage(*ptr->data_as<bNode>());
        if (BLI_path_extension_check_n(
                settings.filepath, ".png", ".exr", ".tif", ".tiff", nullptr)) {
          layout.label(IFACE_("Image Slice Orientation"), ICON_IMAGE_DATA);
          layout.prop(ptr, "image_flip_y", UI_ITEM_NONE, std::nullopt, ICON_NONE);
          layout.prop(ptr, "image_flip_green", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        }
      });
  b.add_panel("File"_ustr)
      .default_closed(true)
      .add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
        layout.context_ptr_set("node", ptr);
        const bool packed = node_storage(*ptr->data_as<bNode>()).packed_file != nullptr;
        file_button(layout,
                    packed ? IFACE_("Unpack LUT") : IFACE_("Pack LUT"),
                    ICON_PACKAGE,
                    packed ? 3 : 2);
        file_button(layout, IFACE_("Make Path Relative"), ICON_FILE_PARENT, 5);
      });
  b.add_input<decl::Color>("Image"_ustr).hide_value().structure_type(StructureType::Dynamic);
  b.add_input<decl::Float>("Strength"_ustr, "Fac"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .structure_type(StructureType::Dynamic)
      .compositor_domain_priority(1);
}

using namespace blender::compositor;

class LUTOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &input = get_input("Image");
    const Result &factor = get_input("Fac");
    Result &output = get_result("Image");
    const auto &settings = node_storage(node());
    if ((!settings.filepath[0] && !settings.packed_file) ||
        (factor.is_single_value() && factor.get_single_value<float>() <= 0.0f))
    {
      output.share_data(input);
      return;
    }
    char filepath[FILE_MAX];
    STRNCPY(filepath, settings.filepath[0] ? settings.filepath : "packed.cube");
    BLI_path_abs(filepath, ID_BLEND_PATH(&context().get_main(), &node().owner_tree().id));
    OCIOLUT &lut = context().cache_manager().ocio_luts.get(context(), settings, filepath);
    if (!lut.error().empty()) {
      report_error(lut.error());
      output.share_data(input);
      return;
    }
    if (input.is_single_value() && factor.is_single_value()) {
      output.allocate_single_value();
      output.set_single_value(
          grade(lut, input.get_single_value<Color>(), factor.get_single_value<float>()));
      return;
    }
    const Domain domain = compute_domain();
    if (context().use_gpu()) {
      gpu::Shader *shader = lut.bind_shader_and_resources();
      if (!shader) {
        report_error(lut.error());
        output.share_data(input);
        return;
      }
      input.bind_as_texture(shader, "input_tx");
      factor.bind_as_texture(shader, "factor_tx");
      output.allocate_texture(domain);
      output.bind_as_image(shader, "output_img");
      compute_dispatch_threads_at_least(shader, domain.data_size);
      input.unbind_as_texture();
      factor.unbind_as_texture();
      output.unbind_as_image();
      lut.unbind_shader_and_resources();
    }
    else {
      output.allocate_texture(domain);
      parallel_for(domain.data_size, [&](const int2 texel) {
        output.store_pixel(texel,
                           grade(lut,
                                 input.load_pixel<Color, true>(texel),
                                 factor.load_pixel<float, true>(texel)));
      });
    }
  }

 private:
  void report_error(const std::string &message)
  {
    context().set_info_message(message);
    if (auto *log = context().nodes_evaluation_log()) {
      auto &logger = log->get_local_tree_logger(get_compute_context());
      logger.node_warnings.append(*logger.allocator,
                                  {node().identifier, {NodeWarningType::Error, message}});
    }
  }

  static Color grade(const OCIOLUT &lut, const Color &input, float factor)
  {
    factor = std::clamp(factor, 0.0f, 1.0f);
    if (factor == 0.0f || input.a == 0.0f) {
      return input;
    }
    float rgb[3] = {input.r / input.a, input.g / input.a, input.b / input.a};
    lut.apply_rgb(rgb);
    return Color(input.r + factor * (rgb[0] * input.a - input.r),
                 input.g + factor * (rgb[1] * input.a - input.g),
                 input.b + factor * (rgb[2] * input.a - input.b),
                 input.a);
  }
};

static void node_register()
{
  static bke::bNodeType ntype;
  cmp_node_type_base(&ntype, "CompositorNodeLUT"_ustr, CMP_NODE_LUT);
  ntype.ui_name = "LUT Color Grade";
  ntype.ui_description =
      "Apply a .cube or sliced image color lookup table, with color management and a mask";
  ntype.enum_name_legacy = "LUT";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.default_width = 280.0f;
  ntype.declare = node_declare;
  ntype.draw_buttons = draw_buttons;
  ntype.initfunc = [](bNodeTree *, bNode *node) {
    node->storage = MEM_new<NodeCompositorLUT>(__func__);
  };
  bke::node_type_storage(ntype, "NodeCompositorLUT", node_free, node_copy);
  ntype.blend_write_storage_content = node_blend_write;
  ntype.blend_data_read_storage_content = node_blend_read;
  ntype.register_operators = register_operators;
  ntype.get_compositor_operation = [](Context &context, const bNode &node) -> NodeOperation * {
    return new LUTOperation(context, node);
  };
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_composite_lut_cc
