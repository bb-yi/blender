/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

 /** \file
  * \ingroup shdnodes
  */

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "node_exec.hh"
#include "node_shader_util.hh"

#include "BLO_read_write.hh"

#include "BKE_image.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main_invariants.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_text.h"

#include "DNA_material_types.h"
#include "DNA_text_types.h"

#include "BLI_fileops.h"
#include "BLI_ghash.h"
#include "BLI_hash.hh"
#include "BLI_map.hh"
#include "BLI_memory_utils.hh"
#include "BLI_path_utils.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "CLG_log.h"

#include "GPU_capabilities.hh"
#include "GPU_context.hh"
#include "GPU_shader.hh"

#include "IMB_imbuf_types.hh"

#include "MEM_guardedalloc.h"

#include "intern/gpu_node_graph.hh"
#include "intern/gpu_shader_create_info.hh"

#include "NOD_sync_sockets.hh"
#include "NOD_geometry_nodes_closure_signature.hh"
#include "NOD_shader.h"
#include "NOD_socket.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "BKE_context.hh"
#include "BKE_main.hh"

#include "DNA_space_types.h"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender
{

  namespace nodes::node_shader_glsl_function_cc
  {

    NODE_STORAGE_FUNCS(NodeShaderGLSLFunction)

    static CLG_LogRef LOG = { "node.shader.glsl_function" };
    static thread_local Set<std::string> active_closure_helper_keys;
    static thread_local Vector<const bNode*> active_closure_helper_nodes;

    static void copy_glsl_define_value(NodeShaderGLSLDefineValue& dst,
      const NodeShaderGLSLDefineValue& src)
    {
      STRNCPY(dst.name, src.name);
      dst.type = src.type;
      dst.value = src.value;
    }

    static const char* glsl_function_material_name(GPUMaterial* mat)
    {
      if (mat == nullptr)
      {
        return "<unknown>";
      }
      if (Material* material = GPU_material_get_material(mat))
      {
        return material->id.name + 2;
      }
      const char* gpu_material_name = GPU_material_get_name(mat);
      return gpu_material_name != nullptr ? gpu_material_name : "<unknown>";
    }

    static void node_storage_free(bNode* node)
    {
      NodeShaderGLSLFunction* storage = static_cast<NodeShaderGLSLFunction*>(node->storage);
      if (storage == nullptr)
      {
        return;
      }
      for (bNodeSocket* socket : node->input_sockets())
      {
        RNA_node_socket_glsl_int_choices_unregister(socket);
      }
      MEM_SAFE_DELETE(storage->packed_source);
      MEM_SAFE_DELETE(storage->edit_source);
      RNA_shader_node_glsl_define_value_choices_unregister(storage->define_values,
                                                           storage->define_values_num);
      MEM_SAFE_DELETE(storage->define_values);
      MEM_delete(storage);
      node->storage = nullptr;
    }

    static void node_storage_copy(bNodeTree* /*dst_ntree*/, bNode* dest_node, const bNode* src_node)
    {
      const NodeShaderGLSLFunction* src_storage = static_cast<const NodeShaderGLSLFunction*>(
        src_node->storage);
      if (src_storage == nullptr)
      {
        dest_node->storage = nullptr;
        return;
      }

      NodeShaderGLSLFunction* dst_storage = MEM_dupalloc(src_storage);
      dst_storage->define_values = nullptr;
      if (src_storage->packed_source != nullptr)
      {
        dst_storage->packed_source = BLI_strdup(src_storage->packed_source);
      }
      if (src_storage->edit_source != nullptr)
      {
        dst_storage->edit_source = BLI_strdup(src_storage->edit_source);
      }
      if (src_storage->define_values != nullptr && src_storage->define_values_num > 0)
      {
        dst_storage->define_values = MEM_new_array<NodeShaderGLSLDefineValue>(
          src_storage->define_values_num, __func__);
        for (int i = 0; i < src_storage->define_values_num; i++)
        {
          copy_glsl_define_value(dst_storage->define_values[i], src_storage->define_values[i]);
        }
      }
      else
      {
        dst_storage->define_values_num = 0;
      }
      dest_node->storage = dst_storage;
    }

    static void node_storage_blend_write(const bNodeTree& /*tree*/,
                                         const bNode& node,
                                         BlendWriter& writer)
    {
      const NodeShaderGLSLFunction& storage = node_storage(node);
      writer.write_string(storage.packed_source);
      writer.write_string(storage.edit_source);
      writer.write_struct_array(storage.define_values_num, storage.define_values);
    }

    static void node_storage_blend_read(bNodeTree& /*tree*/,
                                        bNode& node,
                                        BlendDataReader& reader)
    {
      static constexpr int legacy_text_edit_baseline_flag = 1 << 2;
      NodeShaderGLSLFunction& storage = node_storage(node);
      BLO_read_string(&reader, &storage.packed_source);
      BLO_read_string(&reader, &storage.edit_source);
      if ((storage.flags & SHD_GLSL_FUNCTION_EDIT_FUNCTION) != 0)
      {
        STRNCPY(storage.function_name, storage.edit_function_name);
      }
      storage.flags &= ~SHD_GLSL_FUNCTION_EDIT_FUNCTION;
      storage.edit_function_name[0] = '\0';
      storage.edit_source_session_uid = 0;
      storage.edit_source_hash = 0;
      if ((storage.flags & legacy_text_edit_baseline_flag) != 0)
      {
        MEM_SAFE_DELETE(storage.edit_source);
        storage.flags &= ~legacy_text_edit_baseline_flag;
      }
      BLO_read_array_and_validate_size(&reader,
                                       &storage.define_values,
                                       &storage.define_values_num);
      if (storage.define_values_num <= 0)
      {
        storage.define_values_num = 0;
        storage.define_values = nullptr;
      }
    }

    enum class GLSLBoundaryType
    {
      Unsupported = 0,
      Float,
      Int,
      Bool,
      Vec2,
      Vec3,
      Vec4,
      Mat2,
      Mat3,
      Mat4,
      Sample2D,
      Sample3D,
      Void,
    };

    struct GLSLToken
    {
      enum class Kind
      {
        Identifier,
        Punctuation,
      };

      Kind kind;
      std::string text;
      char punctuation = '\0';
      int64_t source_start = 0;
      int64_t source_end = 0;
    };

    struct GLSLFunctionParam
    {
      enum class Qualifier
      {
        In,
        Out,
        InOut,
      };

      struct IntChoiceItem
      {
        int value = 0;
        std::string label;
      };

      struct Meta
      {
        bool has_default_value = false;
        float4 default_value = float4(0.0f);
        std::optional<int> int_default_value;
        std::optional<std::string> default_expression;
        std::optional<std::string> panel_name;
        std::optional<std::string> description;
        std::optional<std::string> label;
        bool has_min = false;
        float min_value = 0.0f;
        std::optional<int> int_min_value;
        bool has_max = false;
        float max_value = 0.0f;
        std::optional<int> int_max_value;
        bool hide_value = false;
        std::optional<PropertySubType> subtype;
        Vector<IntChoiceItem> int_choices;
        bool int_choices_show_label = false;

        bool has_any() const
        {
          return has_default_value || default_expression.has_value() || panel_name.has_value() ||
            description.has_value() || label.has_value() || has_min || has_max || hide_value ||
            subtype.has_value() || !int_choices.is_empty() || int_choices_show_label;
        }
      };

      GLSLBoundaryType type = GLSLBoundaryType::Unsupported;
      Qualifier qualifier = Qualifier::In;
      std::string type_name;
      std::string name;
      std::string identifier;
      int dimensions = 0;
      bool is_array = false;
      int64_t type_source_start = -1;
      int64_t type_source_end = -1;
      Meta meta;
    };

    struct GLSLPanelMeta
    {
      std::string name;
      bool default_closed = true;
    };

    struct GLSLDefineMeta
    {
      std::string name;
      int type = SHD_GLSL_FUNCTION_DEFINE_BOOL;
      int default_value = 0;
      std::optional<int> min_value;
      std::optional<int> max_value;
      std::optional<std::string> label;
      std::optional<std::string> description;
      Vector<GLSLFunctionParam::IntChoiceItem> int_choices;
      bool int_choices_show_label = false;
    };

    struct GLSLDefineValueState
    {
      char name[64] = "";
      int type = SHD_GLSL_FUNCTION_DEFINE_BOOL;
      int value = 0;
    };

    struct GLSLSourceRange
    {
      int64_t start = 0;
      int64_t end = 0;
    };

    struct GLSLFunctionDefinition
    {
      std::string name;
      std::string return_type_name;
      GLSLBoundaryType return_type = GLSLBoundaryType::Unsupported;
      GLSLFunctionParam::Meta return_meta;
      Vector<GLSLFunctionParam> params;
      Vector<GLSLPanelMeta> panels;
      int body_token_start = -1;
      int body_token_end = -1;
      int64_t body_source_start = -1;
      int64_t body_source_end = -1;
    };

    struct GLSLClosureCallback
    {
      GLSLFunctionDefinition function;
      std::optional<std::string> label;
      std::optional<std::string> description;
    };

    struct GLSLParseResult
    {
      bool ok = false;
      bool keep_existing_sockets = false;
      bool used_first_function = false;
      bool uses_geometry_access = false;
      bool uses_lightprobe_access = false;
      bool uses_eevee_light_access = false;
      bool defines_parsed = false;

      std::string error;
      std::string source;
      std::string source_hash_hex;
      std::string source_filename;
      std::string source_prefix;
      std::string wrapper_filename;
      std::string wrapper_name;
      std::string resolved_function_name;
      std::string library_source;
      std::string wrapper_source;

      Vector<std::string> function_names;
      Vector<std::string> global_names;
      Vector<GLSLDefineMeta> defines;
      Vector<GLSLClosureCallback> closure_callbacks;
      GLSLFunctionDefinition function;
      bool defines_panel_default_closed = false;
      int signature_hash = 0;
      int meta_hash = 0;
      int define_hash = 0;
    };

    struct GLSLSample2DUsage
    {
      int texture_calls = 0;
      bool uses_texture_bias = false;
      bool uses_texture_lod = false;
      bool uses_texture_grad = false;
      bool uses_texture_size = false;
      bool uses_texel_fetch = false;
      bool uses_texture_gather = false;
    };

    enum class GLSLSamplerSourceKind : uint8_t
    {
      None = 0,
      ImageToClosure,
      ClosureOutput,
      GLSLFunction,
      GroupInput,
      Unsupported,
    };

    static bool node_is_group_input(const bNode& node)
    {
      return node.is_group_input() || node.is_type("NodeGroupInput"_ustr);
    }

    struct GLSLRawParamMeta
    {
      std::optional<std::string> default_value;
      std::optional<std::string> min_value;
      std::optional<std::string> max_value;
      std::optional<std::string> hide_value;
      std::optional<std::string> subtype;
      std::optional<std::string> items;
      std::optional<std::string> show_label;
      std::optional<std::string> panel_name;
      std::optional<std::string> description;
      std::optional<std::string> label;

      bool has_any() const
      {
        return default_value.has_value() || min_value.has_value() || max_value.has_value() ||
          hide_value.has_value() || subtype.has_value() || items.has_value() ||
          show_label.has_value() || panel_name.has_value() || description.has_value() ||
          label.has_value();
      }

      bool has_sampler_unsupported_meta() const
      {
        return default_value.has_value() || min_value.has_value() || max_value.has_value() ||
          hide_value.has_value() || subtype.has_value() || items.has_value() ||
          show_label.has_value();
      }

      bool has_only_output_supported_meta() const
      {
        return label.has_value() && !default_value.has_value() && !min_value.has_value() &&
          !max_value.has_value() && !hide_value.has_value() && !subtype.has_value() &&
          !items.has_value() && !show_label.has_value() && !panel_name.has_value() &&
          !description.has_value();
      }
    };

    struct GLSLRawClosureMeta
    {
      std::optional<std::string> label;
      std::optional<std::string> description;
      Map<std::string, GLSLRawParamMeta> items;
    };

    struct GLSLClosureSampleHelper
    {
      std::string param_name;
      std::string helper_name;
      std::string uv_global_name;
      std::string sub_function_name;
      GPUType coordinate_type = GPU_VEC2;
      GPUType return_type = GPU_VEC4;
    };

    struct GLSLClosureCallbackHelper
    {
      std::string function_name;
      std::string sub_function_name;
      Vector<GPUType> input_types;
      Vector<GPUType> output_types;
    };

    struct GLSLTokenRange
    {
      int start = -1;
      int end = -1;
    };

    struct GLSLClosureSamplerDowngradeInfo
    {
      Set<std::string> root_param_names;
      bool uses_texture_bias = false;
      bool uses_texture_lod = false;
      bool uses_texture_grad = false;
      bool uses_texture_size = false;
      bool uses_texel_fetch = false;
      bool uses_texture_gather = false;
    };

    struct GLSLClosureFunctionBindings
    {
      std::string function_name;
      Map<std::string, std::string> bindings;
    };

    static constexpr const char* result_socket_identifier = "Result";
    static constexpr int closure_output_virtual_texture_size = 1024;
    static constexpr const char* glsl_light_access_helper_filename =
      "gpu_shader_material_glsl_light_access.glsl";
    static constexpr const char* glsl_light_probe_color_helper_filename =
      "gpu_shader_material_light_probe_color.glsl";

    static Vector<GLSLToken> tokenize_glsl_source(const StringRef source);
    static const bNodeSocket* find_node_input_socket_by_identifier(const bNode& node,
      const StringRef identifier);
    static bNodeSocket* find_node_input_socket_by_identifier(bNode& node, const StringRef identifier);
    static const bNodeSocket* find_closure_output_socket_by_name(const bNode& node, const StringRef name);
    static bool closure_output_has_required_sampler_signature(const bNode& closure_output_node,
      GLSLBoundaryType sampler_type,
      std::string& r_error);
    static bool glsl_param_has_input_socket(const GLSLFunctionParam& param);
    static ClosureSignature build_glsl_closure_signature(const GLSLClosureCallback& callback);
    static Vector<std::string> find_top_level_glsl_function_names(const Vector<GLSLToken>& tokens);
    static bool validate_top_level_glsl_declarations(const Vector<GLSLToken>& tokens,
      std::string& r_error);
    static Vector<std::string> find_top_level_glsl_global_names(
      const Vector<GLSLToken>& tokens, const Span<std::string> function_names);
    static bool glsl_source_uses_geometry_access(const Vector<GLSLToken>& tokens);
    static bool glsl_source_uses_lightprobe_access(const Vector<GLSLToken>& tokens);
    static bool glsl_source_uses_eevee_light_access(const Vector<GLSLToken>& tokens);
    static bool glsl_function_meta_uses_geometry_access(const GLSLFunctionDefinition& function);
    static bool glsl_function_meta_uses_lightprobe_access(const GLSLFunctionDefinition& function);
    static bool glsl_function_meta_uses_eevee_light_access(const GLSLFunctionDefinition& function);
    static bool find_glsl_function_definition(const Vector<GLSLToken>& tokens,
      const StringRef function_name,
      GLSLFunctionDefinition& r_function,
      std::string& r_error);
    static bool is_sampler_sampling_function_name(const StringRef name);
    static std::string make_split_vec4_w_socket_identifier(const StringRef identifier);
    static std::string make_split_matrix_column_socket_identifier(const StringRef identifier,
                                                                  const int column);
    static GPUType gpu_node_link_output_type(const GPUNodeLink& link);

    static bool is_identifier_start(const char c)
    {
      return std::isalpha(uchar(c)) || c == '_';
    }

    static bool is_identifier_continue(const char c)
    {
      return std::isalnum(uchar(c)) || c == '_';
    }

    static GLSLBoundaryType glsl_boundary_type_from_name(const StringRef type_name)
    {
      if (type_name == "float")
      {
        return GLSLBoundaryType::Float;
      }
      if (type_name == "int")
      {
        return GLSLBoundaryType::Int;
      }
      if (type_name == "bool")
      {
        return GLSLBoundaryType::Bool;
      }
      if (type_name == "vec2")
      {
        return GLSLBoundaryType::Vec2;
      }
      if (type_name == "vec3")
      {
        return GLSLBoundaryType::Vec3;
      }
      if (type_name == "vec4")
      {
        return GLSLBoundaryType::Vec4;
      }
      if (type_name == "mat2")
      {
        return GLSLBoundaryType::Mat2;
      }
      if (type_name == "mat3")
      {
        return GLSLBoundaryType::Mat3;
      }
      if (type_name == "mat4")
      {
        return GLSLBoundaryType::Mat4;
      }
      if (type_name == "sampler2D")
      {
        return GLSLBoundaryType::Sample2D;
      }
      if (type_name == "sampler3D")
      {
        return GLSLBoundaryType::Sample3D;
      }
      if (type_name == "void")
      {
        return GLSLBoundaryType::Void;
      }
      return GLSLBoundaryType::Unsupported;
    }

    static int glsl_boundary_dimensions(const GLSLBoundaryType type)
    {
      switch (type)
      {
      case GLSLBoundaryType::Vec2:
        return 2;
      case GLSLBoundaryType::Vec3:
        return 3;
      case GLSLBoundaryType::Vec4:
        return 4;
      default:
        return 0;
      }
    }

    static bool glsl_boundary_type_is_matrix(const GLSLBoundaryType type)
    {
      return ELEM(type, GLSLBoundaryType::Mat2, GLSLBoundaryType::Mat3, GLSLBoundaryType::Mat4);
    }

    static int glsl_matrix_columns(const GLSLBoundaryType type)
    {
      switch (type)
      {
      case GLSLBoundaryType::Mat2:
        return 2;
      case GLSLBoundaryType::Mat3:
        return 3;
      case GLSLBoundaryType::Mat4:
        return 4;
      default:
        return 0;
      }
    }

    static int glsl_matrix_column_dimensions(const GLSLBoundaryType type)
    {
      return glsl_matrix_columns(type);
    }

    static StringRefNull glsl_vector_type_name(const int dimensions)
    {
      switch (dimensions)
      {
      case 2:
        return StringRefNull("vec2");
      case 3:
        return StringRefNull("vec3");
      case 4:
        return StringRefNull("vec4");
      default:
        BLI_assert_unreachable();
        return StringRefNull("vec3");
      }
    }

    static bool glsl_boundary_type_is_sampler(const GLSLBoundaryType type)
    {
      return ELEM(type, GLSLBoundaryType::Sample2D, GLSLBoundaryType::Sample3D);
    }

    static bool glsl_boundary_type_uses_float_transport(const GLSLBoundaryType type)
    {
      return ELEM(type, GLSLBoundaryType::Int, GLSLBoundaryType::Bool);
    }

    static bool glsl_boundary_type_is_sample2d(const GLSLBoundaryType type)
    {
      return type == GLSLBoundaryType::Sample2D;
    }

    static bool glsl_boundary_type_is_sample3d(const GLSLBoundaryType type)
    {
      return type == GLSLBoundaryType::Sample3D;
    }

    static StringRefNull sampler_type_name(const GLSLBoundaryType type)
    {
      return glsl_boundary_type_is_sample3d(type) ? StringRefNull("sampler3D") :
                                                    StringRefNull("sampler2D");
    }

    static const bNodeLink* find_used_direct_link(const bNodeSocket& socket)
    {
      for (const bNodeLink* link : socket.directly_linked_links())
      {
        if (link->is_used())
        {
          return link;
        }
      }
      return nullptr;
    }

    static const bNodeLink* find_any_direct_link(const bNodeSocket& socket)
    {
      for (const bNodeLink* link : socket.directly_linked_links())
      {
        if (link->fromnode != nullptr)
        {
          return link;
        }
      }
      return nullptr;
    }

    static const bNodeSocket* find_linked_sampler_input_socket(const bNode& node,
      const GLSLFunctionParam& param)
    {
      const bNodeSocket* sample_socket = find_node_input_socket_by_identifier(node, param.identifier);
      if ((sample_socket == nullptr || !sample_socket->is_directly_linked()) && node.runtime->original)
      {
        sample_socket = find_node_input_socket_by_identifier(*node.runtime->original, param.identifier);
      }
      if (sample_socket != nullptr && sample_socket->is_directly_linked())
      {
        return sample_socket;
      }
      return nullptr;
    }

    static const bNodeSocket* find_glsl_function_input_socket(const bNode& node,
      const GLSLFunctionParam& param)
    {
      const bNodeSocket* socket = find_node_input_socket_by_identifier(node, param.identifier);
      if ((socket == nullptr || !socket->is_directly_linked()) && node.runtime->original)
      {
        socket = find_node_input_socket_by_identifier(*node.runtime->original, param.identifier);
      }
      return socket;
    }

    static bool glsl_input_socket_is_directly_linked(const bNode& node, const StringRef identifier)
    {
      const bNodeSocket* socket = find_node_input_socket_by_identifier(node, identifier);
      if ((socket == nullptr || !socket->is_directly_linked()) && node.runtime->original)
      {
        socket = find_node_input_socket_by_identifier(*node.runtime->original, identifier);
      }
      return socket != nullptr && socket->is_directly_linked();
    }

    static bool glsl_function_input_is_directly_linked(const bNode& node,
      const GLSLFunctionParam& param)
    {
      if (param.type == GLSLBoundaryType::Vec4)
      {
        if (glsl_input_socket_is_directly_linked(node, param.identifier))
        {
          return true;
        }
        return glsl_input_socket_is_directly_linked(
          node, make_split_vec4_w_socket_identifier(param.identifier));
      }
      if (glsl_boundary_type_is_matrix(param.type))
      {
        for (const int column : IndexRange(glsl_matrix_columns(param.type)))
        {
          const std::string column_identifier = make_split_matrix_column_socket_identifier(
            param.identifier, column);
          if (glsl_input_socket_is_directly_linked(node, column_identifier))
          {
            return true;
          }
          if (glsl_matrix_column_dimensions(param.type) == 4 &&
              glsl_input_socket_is_directly_linked(
                node, make_split_vec4_w_socket_identifier(column_identifier)))
          {
            return true;
          }
        }
        return false;
      }

      const bNodeSocket* socket = find_glsl_function_input_socket(node, param);
      return socket != nullptr && socket->is_directly_linked();
    }

    static GLSLSamplerSourceKind resolve_sampler_source_kind(const bNode& node,
      const GLSLFunctionParam& param,
      const bNodeLink*& r_link,
      const bNode* fallback_node = nullptr)
    {
      const bNodeSocket* sample_socket = find_linked_sampler_input_socket(node, param);
      if (sample_socket == nullptr && fallback_node != nullptr)
      {
        if (fallback_node != &node && fallback_node != node.runtime->original)
        {
          sample_socket = find_linked_sampler_input_socket(*fallback_node, param);
        }
      }
      if (sample_socket == nullptr)
      {
        r_link = nullptr;
        return GLSLSamplerSourceKind::None;
      }

      r_link = find_used_direct_link(*sample_socket);
      if (r_link == nullptr)
      {
        const bNodeLink* direct_link = find_any_direct_link(*sample_socket);
        if (direct_link != nullptr && direct_link->fromnode != nullptr &&
            node_is_group_input(*direct_link->fromnode))
        {
          r_link = direct_link;
        }
      }
      if (r_link == nullptr || r_link->fromnode == nullptr)
      {
        return GLSLSamplerSourceKind::None;
      }
      if (r_link->fromnode->is_type("ShaderNodeImageToClosure"_ustr))
      {
        return GLSLSamplerSourceKind::ImageToClosure;
      }
      if (r_link->fromnode->is_type("NodeClosureOutput"_ustr))
      {
        return GLSLSamplerSourceKind::ClosureOutput;
      }
      if (r_link->fromnode->is_type("ShaderNodeGLSLFunction"_ustr))
      {
        return GLSLSamplerSourceKind::GLSLFunction;
      }
      if (node_is_group_input(*r_link->fromnode))
      {
        return GLSLSamplerSourceKind::GroupInput;
      }
      return GLSLSamplerSourceKind::Unsupported;
    }

    static GLSLSamplerSourceKind resolve_nested_sampler_source_kind(const GLSLFunctionParam& param,
      const bNodeLink& link,
      const bNodeLink*& r_link)
    {
      const bNode* from_node = link.fromnode;
      if (from_node == nullptr || !from_node->is_type("ShaderNodeGLSLFunction"_ustr))
      {
        r_link = nullptr;
        return GLSLSamplerSourceKind::Unsupported;
      }

      const bNode* logical_node = from_node->runtime->original ? from_node->runtime->original : from_node;
      return resolve_sampler_source_kind(*from_node, param, r_link, logical_node);
    }

    static Image* resolve_image_to_closure_image(const bNodeLink& link)
    {
      if (link.fromnode == nullptr || !link.fromnode->is_type("ShaderNodeImageToClosure"_ustr))
      {
        return nullptr;
      }
      return id_cast<Image*>(link.fromnode->id);
    }

    struct ResolvedLut3DStripDimensions
    {
      int width = 0;
      int height = 0;
      int depth = 0;
    };

    static const NodeShaderImageToClosure* image_to_closure_storage(const bNode& node)
    {
      return static_cast<const NodeShaderImageToClosure*>(node.storage);
    }

    static int image_to_closure_texture_type(const bNode& node)
    {
      const NodeShaderImageToClosure* storage = image_to_closure_storage(node);
      return storage ? storage->texture_type : IMA_IMAGE_TO_CLOSURE_TEXTURE_2D;
    }

    static int image_to_closure_texture_size_mode(const bNode& node)
    {
      const NodeShaderImageToClosure* storage = image_to_closure_storage(node);
      return storage ? storage->texture_size_mode : IMA_IMAGE_TO_CLOSURE_3D_LUT_SIZE_AUTO;
    }

    static int image_to_closure_interpolation(const bNode& node)
    {
      const NodeShaderImageToClosure* storage = image_to_closure_storage(node);
      return storage ? storage->interpolation : SHD_INTERP_LINEAR;
    }

    static int image_to_closure_extension(const bNode& node)
    {
      const NodeShaderImageToClosure* storage = image_to_closure_storage(node);
      return storage ? storage->extension : SHD_IMAGE_EXTENSION_REPEAT;
    }

    static bool resolve_3d_lut_strip_dimensions(const bNode& image_to_closure_node,
      const Image& image,
      ResolvedLut3DStripDimensions& r_dimensions,
      std::string& r_error)
    {
      if (image.source == IMA_SRC_TILED)
      {
        r_error = "3D LUT Strip does not support UDIM tiled images";
        return false;
      }

      ImageUser iuser = {};
      BKE_imageuser_default(&iuser);
      void* lock = nullptr;
      ImBuf* ibuf = BKE_image_acquire_ibuf(const_cast<Image*>(&image), &iuser, &lock);
      if (ibuf == nullptr)
      {
        BKE_image_release_ibuf(const_cast<Image*>(&image), ibuf, lock);
        r_error = "Could not load the 3D LUT Strip image";
        return false;
      }

      const int width = ibuf->x;
      const int height = ibuf->y;
      const int max_size = GPU_max_texture_3d_size();
      BKE_image_release_ibuf(const_cast<Image*>(&image), ibuf, lock);

      if (image_to_closure_texture_size_mode(image_to_closure_node) ==
          IMA_IMAGE_TO_CLOSURE_3D_LUT_SIZE_MANUAL)
      {
        const NodeShaderImageToClosure* storage = image_to_closure_storage(image_to_closure_node);
        const int lut_width = storage ? storage->texture_width : 16;
        const int lut_height = storage ? storage->texture_height : 16;
        const int lut_depth = storage ? storage->texture_depth : 16;
        if (lut_width <= 0 || lut_height <= 0 || lut_depth <= 0)
        {
          r_error = "Manual 3D LUT Strip dimensions must be greater than zero";
          return false;
        }
        if (int64_t(width) != int64_t(lut_width) * int64_t(lut_depth) ||
            height != lut_height)
        {
          r_error =
            "Manual 3D LUT Strip dimensions require source width = Width * Depth and source "
            "height = Height";
          return false;
        }
        if (max_size > 0 &&
            (lut_width > max_size || lut_height > max_size || lut_depth > max_size))
        {
          r_error = "Manual 3D LUT Strip dimensions exceed the GPU 3D texture limit";
          return false;
        }
        r_dimensions.width = lut_width;
        r_dimensions.height = lut_height;
        r_dimensions.depth = lut_depth;
        return true;
      }

      if (height <= 0 || int64_t(width) != int64_t(height) * int64_t(height))
      {
        r_error =
          "Auto 3D LUT Strip images must be horizontal cubic strips where width = height * height";
        return false;
      }
      if (max_size > 0 && height > max_size)
      {
        r_error = "3D LUT Strip size exceeds the GPU 3D texture limit";
        return false;
      }
      r_dimensions.width = height;
      r_dimensions.height = height;
      r_dimensions.depth = height;
      return true;
    }

    static GPUSamplerState sampler_state_from_image_to_closure_node(const bNode& node)
    {
      GPUSamplerState sampler_state = GPUSamplerState::default_sampler();
      const bool use_3d_lut_strip =
        image_to_closure_texture_type(node) == IMA_IMAGE_TO_CLOSURE_TEXTURE_3D_LUT_STRIP;

      switch (image_to_closure_extension(node))
      {
      case SHD_IMAGE_EXTENSION_EXTEND:
        sampler_state.extend_x = GPU_SAMPLER_EXTEND_MODE_EXTEND;
        sampler_state.extend_yz = GPU_SAMPLER_EXTEND_MODE_EXTEND;
        break;
      case SHD_IMAGE_EXTENSION_REPEAT:
        sampler_state.extend_x = GPU_SAMPLER_EXTEND_MODE_REPEAT;
        sampler_state.extend_yz = GPU_SAMPLER_EXTEND_MODE_REPEAT;
        break;
      case SHD_IMAGE_EXTENSION_CLIP:
        sampler_state.extend_x = GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER;
        sampler_state.extend_yz = GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER;
        break;
      case SHD_IMAGE_EXTENSION_MIRROR:
        sampler_state.extend_x = GPU_SAMPLER_EXTEND_MODE_MIRRORED_REPEAT;
        sampler_state.extend_yz = GPU_SAMPLER_EXTEND_MODE_MIRRORED_REPEAT;
        break;
      default:
        break;
      }

      if (image_to_closure_interpolation(node) != SHD_INTERP_CLOSEST)
      {
        sampler_state.filtering = use_3d_lut_strip ?
                                    GPU_SAMPLER_FILTERING_LINEAR :
                                    GPU_SAMPLER_FILTERING_ANISOTROPIC_ENABLE |
                                      GPU_SAMPLER_FILTERING_LINEAR | GPU_SAMPLER_FILTERING_MIPMAP;
      }

      return sampler_state;
    }

    static Map<std::string, GLSLSamplerSourceKind> resolve_sampler_source_kinds(
      const bNode& node, const GLSLFunctionDefinition& function)
    {
      Map<std::string, GLSLSamplerSourceKind> source_kinds;
      const bNode* logical_node = node.runtime->original ? node.runtime->original : &node;
      for (const GLSLFunctionParam& param : function.params)
      {
        if (!glsl_param_has_input_socket(param) || !glsl_boundary_type_is_sampler(param.type))
        {
          continue;
        }
        const bNodeLink* used_link = nullptr;
        source_kinds.add_new(param.name,
          resolve_sampler_source_kind(node, param, used_link, logical_node));
      }
      return source_kinds;
    }

    static const GLSLFunctionDefinition* find_glsl_function_definition_by_name(
      const Span<GLSLFunctionDefinition> functions, const StringRef name)
    {
      for (const GLSLFunctionDefinition& function : functions)
      {
        if (function.name == name)
        {
          return &function;
        }
      }
      return nullptr;
    }

    static GLSLClosureFunctionBindings* find_glsl_closure_function_bindings(
      Vector<GLSLClosureFunctionBindings>& bindings_by_function, const StringRef function_name)
    {
      for (GLSLClosureFunctionBindings& bindings : bindings_by_function)
      {
        if (bindings.function_name == function_name)
        {
          return &bindings;
        }
      }
      return nullptr;
    }

    static const GLSLClosureFunctionBindings* find_glsl_closure_function_bindings(
      const Span<GLSLClosureFunctionBindings> bindings_by_function, const StringRef function_name)
    {
      for (const GLSLClosureFunctionBindings& bindings : bindings_by_function)
      {
        if (bindings.function_name == function_name)
        {
          return &bindings;
        }
      }
      return nullptr;
    }

    static bool ensure_closure_binding(Map<std::string, std::string>& r_bindings,
      const StringRef identifier,
      const StringRef root_param_name,
      const StringRef function_name,
      std::string& r_error,
      bool& r_changed)
    {
      const std::string identifier_key = std::string(identifier);
      if (const std::string* existing_root = r_bindings.lookup_ptr(identifier_key))
      {
        if (*existing_root != root_param_name)
        {
          r_error = "Closure Output sampler identifier '" + identifier_key + "' in function '" +
            std::string(function_name) + "' is bound to multiple inputs";
          return false;
        }
        return true;
      }
      r_bindings.add_new(identifier_key, std::string(root_param_name));
      r_changed = true;
      return true;
    }

    static bool token_range_is_wrapped_by_parens(const Vector<GLSLToken>& tokens,
      const int start,
      const int end)
    {
      if (start >= end || tokens[start].kind != GLSLToken::Kind::Punctuation ||
        tokens[start].punctuation != '(' || tokens[end].kind != GLSLToken::Kind::Punctuation ||
        tokens[end].punctuation != ')')
      {
        return false;
      }

      int paren_depth = 0;
      for (int i = start; i <= end; i++)
      {
        const GLSLToken& token = tokens[i];
        if (token.kind != GLSLToken::Kind::Punctuation)
        {
          continue;
        }
        if (token.punctuation == '(')
        {
          paren_depth++;
        }
        else if (token.punctuation == ')')
        {
          paren_depth--;
          if (paren_depth == 0 && i < end)
          {
            return false;
          }
        }
      }
      return paren_depth == 0;
    }

    static bool parse_single_identifier_token_range(const Vector<GLSLToken>& tokens,
      int start,
      int end,
      std::string& r_identifier,
      int& r_identifier_token_index)
    {
      if (start > end)
      {
        return false;
      }
      while (token_range_is_wrapped_by_parens(tokens, start, end))
      {
        start++;
        end--;
      }
      if (start == end && tokens[start].kind == GLSLToken::Kind::Identifier)
      {
        r_identifier = tokens[start].text;
        r_identifier_token_index = start;
        return true;
      }
      return false;
    }

    static Vector<GLSLTokenRange> collect_function_statement_ranges(const Vector<GLSLToken>& tokens,
      const GLSLFunctionDefinition& function)
    {
      Vector<GLSLTokenRange> ranges;
      int statement_start = function.body_token_start;
      int paren_depth = 0;

      for (int i = function.body_token_start; i <= function.body_token_end; i++)
      {
        const GLSLToken& token = tokens[i];
        if (token.kind == GLSLToken::Kind::Punctuation)
        {
          if (token.punctuation == '(')
          {
            paren_depth++;
          }
          else if (token.punctuation == ')')
          {
            paren_depth = max_ii(paren_depth - 1, 0);
          }
          if (paren_depth == 0 && ELEM(token.punctuation, ';', '{', '}'))
          {
            if (statement_start <= (i - 1))
            {
              ranges.append({ statement_start, i - 1 });
            }
            statement_start = i + 1;
          }
        }
      }
      if (statement_start <= function.body_token_end)
      {
        ranges.append({ statement_start, function.body_token_end });
      }
      return ranges;
    }

    static bool parse_call_argument_ranges(const Vector<GLSLToken>& tokens,
      const int open_paren_index,
      const int max_token_index,
      Vector<GLSLTokenRange>& r_argument_ranges,
      int& r_closing_paren_index)
    {
      r_argument_ranges.clear();
      r_closing_paren_index = -1;
      if ((open_paren_index + 1) > max_token_index)
      {
        return false;
      }

      int arg_start = open_paren_index + 1;
      int paren_depth = 1;
      for (int i = open_paren_index + 1; i <= max_token_index; i++)
      {
        const GLSLToken& token = tokens[i];
        if (token.kind != GLSLToken::Kind::Punctuation)
        {
          continue;
        }
        if (token.punctuation == '(')
        {
          paren_depth++;
        }
        else if (token.punctuation == ')')
        {
          paren_depth--;
          if (paren_depth == 0)
          {
            if (arg_start <= (i - 1))
            {
              r_argument_ranges.append({ arg_start, i - 1 });
            }
            r_closing_paren_index = i;
            return true;
          }
        }
        else if (token.punctuation == ',' && paren_depth == 1)
        {
          if (arg_start <= (i - 1))
          {
            r_argument_ranges.append({ arg_start, i - 1 });
          }
          else
          {
            r_argument_ranges.append({});
          }
          arg_start = i + 1;
        }
      }
      return false;
    }

    static bool find_all_top_level_glsl_function_definitions(
      const Vector<GLSLToken>& tokens,
      const Span<std::string> function_names,
      Vector<GLSLFunctionDefinition>& r_functions,
      std::string& r_error)
    {
      r_functions.clear();
      for (const std::string& function_name : function_names)
      {
        GLSLFunctionDefinition function;
        if (!find_glsl_function_definition(tokens, function_name, function, r_error))
        {
          return false;
        }
        r_functions.append(function);
      }
      return true;
    }

    static bool parse_direct_identifier_assignment_statement(const Vector<GLSLToken>& tokens,
      const GLSLTokenRange& statement_range,
      std::string& r_lhs_identifier,
      int& r_lhs_token_index,
      std::string& r_rhs_identifier,
      int& r_rhs_token_index)
    {
      int equals_index = -1;
      int equals_count = 0;
      for (int i = statement_range.start; i <= statement_range.end; i++)
      {
        if (tokens[i].kind == GLSLToken::Kind::Punctuation && tokens[i].punctuation == '=')
        {
          equals_count++;
          if (equals_index == -1)
          {
            equals_index = i;
          }
        }
      }
      if (equals_count != 1 || equals_index <= statement_range.start || equals_index >= statement_range.end)
      {
        return false;
      }

      r_lhs_token_index = -1;
      for (int i = statement_range.start; i < equals_index; i++)
      {
        if (tokens[i].kind == GLSLToken::Kind::Identifier)
        {
          r_lhs_token_index = i;
        }
      }
      if (r_lhs_token_index == -1)
      {
        return false;
      }

      if (!parse_single_identifier_token_range(
        tokens, equals_index + 1, statement_range.end, r_rhs_identifier, r_rhs_token_index))
      {
        return false;
      }

      r_lhs_identifier = tokens[r_lhs_token_index].text;
      return true;
    }

    static bool build_function_local_closure_bindings(const Vector<GLSLToken>& tokens,
      const GLSLFunctionDefinition& function,
      const Map<std::string, std::string>& seed_bindings,
      Map<std::string, std::string>& r_bindings,
      std::string& r_error)
    {
      r_bindings = seed_bindings;
      const Vector<GLSLTokenRange> statements = collect_function_statement_ranges(tokens, function);

      bool changed = true;
      while (changed)
      {
        changed = false;
        for (const GLSLTokenRange& statement : statements)
        {
          std::string lhs_identifier;
          std::string rhs_identifier;
          int lhs_token_index = -1;
          int rhs_token_index = -1;
          if (!parse_direct_identifier_assignment_statement(
            tokens, statement, lhs_identifier, lhs_token_index, rhs_identifier, rhs_token_index))
          {
            continue;
          }
          const std::string* root_param_name = r_bindings.lookup_ptr(rhs_identifier);
          if (root_param_name == nullptr)
          {
            continue;
          }
          if (!ensure_closure_binding(
            r_bindings, lhs_identifier, *root_param_name, function.name, r_error, changed))
          {
            return false;
          }
        }
      }
      return true;
    }

    static const GLSLFunctionParam* find_glsl_function_param_by_name(
      const GLSLFunctionDefinition& function, const StringRef name)
    {
      for (const GLSLFunctionParam& param : function.params)
      {
        if (param.name == name)
        {
          return &param;
        }
      }
      return nullptr;
    }

    static bool build_closure_sampler_bindings_by_function(
      const Vector<GLSLToken>& tokens,
      const Span<GLSLFunctionDefinition> functions,
      const GLSLFunctionDefinition& entry_function,
      const Map<std::string, GLSLSamplerSourceKind>& entry_source_kinds,
      Vector<GLSLClosureFunctionBindings>& r_bindings_by_function,
      std::string& r_error)
    {
      r_bindings_by_function.clear();
      r_bindings_by_function.append({ entry_function.name, {} });

      bool changed = false;
      GLSLClosureFunctionBindings& entry_bindings = r_bindings_by_function.last();
      for (const GLSLFunctionParam& param : entry_function.params)
      {
        if (!glsl_param_has_input_socket(param) || !glsl_boundary_type_is_sampler(param.type))
        {
          continue;
        }
        const GLSLSamplerSourceKind source_kind = entry_source_kinds.lookup_default(
          param.name, GLSLSamplerSourceKind::None);
        if (source_kind != GLSLSamplerSourceKind::ClosureOutput)
        {
          continue;
        }
        if (!ensure_closure_binding(
          entry_bindings.bindings, param.name, param.name, entry_function.name, r_error, changed))
        {
          return false;
        }
      }

      changed = true;
      while (changed)
      {
        changed = false;
        for (const GLSLFunctionDefinition& function : functions)
        {
          const GLSLClosureFunctionBindings* seed_bindings = find_glsl_closure_function_bindings(
            r_bindings_by_function, function.name);
          if (seed_bindings == nullptr || seed_bindings->bindings.is_empty())
          {
            continue;
          }

          Map<std::string, std::string> local_bindings;
          if (!build_function_local_closure_bindings(
            tokens, function, seed_bindings->bindings, local_bindings, r_error))
          {
            return false;
          }

          for (int i = function.body_token_start; i <= function.body_token_end; i++)
          {
            const GLSLToken& token = tokens[i];
            if (token.kind != GLSLToken::Kind::Identifier || (i + 1) > function.body_token_end ||
              tokens[i + 1].kind != GLSLToken::Kind::Punctuation || tokens[i + 1].punctuation != '(')
            {
              continue;
            }

            const GLSLFunctionDefinition* callee = find_glsl_function_definition_by_name(functions,
              token.text);
            if (callee == nullptr)
            {
              continue;
            }

            Vector<GLSLTokenRange> argument_ranges;
            int closing_paren_index = -1;
            if (!parse_call_argument_ranges(
              tokens, i + 1, function.body_token_end, argument_ranges, closing_paren_index))
            {
              continue;
            }

            for (int param_index = 0;
              param_index < callee->params.size() && param_index < argument_ranges.size();
              param_index++)
            {
              const GLSLFunctionParam& callee_param = callee->params[param_index];
              if (!glsl_param_has_input_socket(callee_param) ||
                !glsl_boundary_type_is_sampler(callee_param.type))
              {
                continue;
              }

              std::string argument_identifier;
              int argument_identifier_token_index = -1;
              if (!parse_single_identifier_token_range(tokens,
                argument_ranges[param_index].start,
                argument_ranges[param_index].end,
                argument_identifier,
                argument_identifier_token_index))
              {
                continue;
              }
              if (argument_identifier_token_index < 0)
              {
                continue;
              }

              const std::string* root_param_name = local_bindings.lookup_ptr(argument_identifier);
              if (root_param_name == nullptr)
              {
                continue;
              }

              const GLSLFunctionParam* root_param = find_glsl_function_param_by_name(
                entry_function, *root_param_name);
              if (root_param == nullptr || root_param->type != callee_param.type)
              {
                const StringRefNull root_type = root_param ? sampler_type_name(root_param->type) :
                                                            StringRefNull("sampler");
                r_error = "Closure Output driven " + std::string(root_type.c_str()) +
                  " parameter '" + *root_param_name + "' cannot be passed to " +
                  std::string(sampler_type_name(callee_param.type).c_str()) + " parameter '" +
                  callee_param.name + "' in function '" + callee->name + "'";
                return false;
              }

              GLSLClosureFunctionBindings* callee_bindings = find_glsl_closure_function_bindings(
                r_bindings_by_function, callee->name);
              if (callee_bindings == nullptr)
              {
                r_bindings_by_function.append({ callee->name, {} });
                callee_bindings = &r_bindings_by_function.last();
              }
              if (!ensure_closure_binding(callee_bindings->bindings,
                callee_param.name,
                *root_param_name,
                callee->name,
                r_error,
                changed))
              {
                return false;
              }
            }
          }
        }
      }

      return true;
    }

    static bool validate_closure_sampler_bindings(
      const Vector<GLSLToken>& tokens,
      const Span<GLSLFunctionDefinition> functions,
      const GLSLFunctionDefinition& entry_function,
      const Span<GLSLClosureFunctionBindings> bindings_by_function,
      std::string& r_error)
    {
      for (const GLSLFunctionDefinition& function : functions)
      {
        const GLSLClosureFunctionBindings* seed_bindings = find_glsl_closure_function_bindings(
          bindings_by_function, function.name);
        if (seed_bindings == nullptr || seed_bindings->bindings.is_empty())
        {
          continue;
        }

        Map<std::string, std::string> local_bindings;
        if (!build_function_local_closure_bindings(
          tokens, function, seed_bindings->bindings, local_bindings, r_error))
        {
          return false;
        }

        Set<int> allowed_identifier_token_indices;
        const Vector<GLSLTokenRange> statements = collect_function_statement_ranges(tokens, function);
        for (const GLSLTokenRange& statement : statements)
        {
          std::string lhs_identifier;
          std::string rhs_identifier;
          int lhs_token_index = -1;
          int rhs_token_index = -1;
          if (!parse_direct_identifier_assignment_statement(
            tokens, statement, lhs_identifier, lhs_token_index, rhs_identifier, rhs_token_index))
          {
            continue;
          }
          if (local_bindings.contains(rhs_identifier))
          {
            allowed_identifier_token_indices.add(rhs_token_index);
          }
          if (local_bindings.contains(lhs_identifier))
          {
            allowed_identifier_token_indices.add(lhs_token_index);
          }
        }

        for (int i = function.body_token_start; i <= function.body_token_end; i++)
        {
          const GLSLToken& token = tokens[i];
          if (token.kind != GLSLToken::Kind::Identifier || (i + 1) > function.body_token_end ||
            tokens[i + 1].kind != GLSLToken::Kind::Punctuation || tokens[i + 1].punctuation != '(')
          {
            continue;
          }

          Vector<GLSLTokenRange> argument_ranges;
          int closing_paren_index = -1;
          if (!parse_call_argument_ranges(
            tokens, i + 1, function.body_token_end, argument_ranges, closing_paren_index))
          {
            continue;
          }

          if (is_sampler_sampling_function_name(token.text))
          {
            if (!argument_ranges.is_empty())
            {
              std::string sampled_identifier;
              int sampled_identifier_token_index = -1;
              if (parse_single_identifier_token_range(tokens,
                argument_ranges[0].start,
                argument_ranges[0].end,
                sampled_identifier,
                sampled_identifier_token_index) &&
                local_bindings.contains(sampled_identifier))
              {
                allowed_identifier_token_indices.add(sampled_identifier_token_index);
                const std::string* root_param_name = local_bindings.lookup_ptr(sampled_identifier);
                const GLSLFunctionParam* root_param = root_param_name ?
                  find_glsl_function_param_by_name(entry_function, *root_param_name) :
                  nullptr;
                if (root_param != nullptr && glsl_boundary_type_is_sample3d(root_param->type) &&
                    (token.text != "texture" || argument_ranges.size() != 2))
                {
                  r_error = "Closure Output driven sampler3D parameter '" + *root_param_name +
                    "' only supports texture(sampler3D, vec3); '" + token.text +
                    "' is not supported";
                  return false;
                }
              }
            }
            continue;
          }

          const GLSLFunctionDefinition* callee = find_glsl_function_definition_by_name(functions,
            token.text);
          if (callee == nullptr)
          {
            continue;
          }
          for (int param_index = 0;
            param_index < callee->params.size() && param_index < argument_ranges.size();
            param_index++)
          {
            const GLSLFunctionParam& callee_param = callee->params[param_index];
            if (!glsl_param_has_input_socket(callee_param) ||
              !glsl_boundary_type_is_sampler(callee_param.type))
            {
              continue;
            }

            std::string argument_identifier;
            int argument_identifier_token_index = -1;
            if (parse_single_identifier_token_range(tokens,
              argument_ranges[param_index].start,
              argument_ranges[param_index].end,
              argument_identifier,
              argument_identifier_token_index) &&
              local_bindings.contains(argument_identifier))
            {
              allowed_identifier_token_indices.add(argument_identifier_token_index);
            }
          }
        }

        for (int i = function.body_token_start; i <= function.body_token_end; i++)
        {
          const GLSLToken& token = tokens[i];
          if (token.kind != GLSLToken::Kind::Identifier || !local_bindings.contains(token.text) ||
            allowed_identifier_token_indices.contains(i))
          {
            continue;
          }

          r_error = "Closure Output driven sampler identifier '" + token.text + "' in function '" +
            function.name +
            "' must be used through texture-family sampling, direct function passthrough, or "
            "direct alias assignment";
          return false;
        }
      }

      return true;
    }

    static std::string make_socket_identifier(const StringRef prefix, const StringRef name)
    {
      std::string identifier;
      identifier.reserve(prefix.size() + name.size() + 8);
      identifier.append(prefix);
      identifier.push_back('_');

      if (name.is_empty())
      {
        identifier.append("value");
      }
      else
      {
        for (const char c : name)
        {
          identifier.push_back(is_identifier_continue(c) ? c : '_');
        }
      }

      if (!identifier.empty() && std::isdigit(uchar(identifier.back())))
      {
        identifier.push_back('_');
      }
      return identifier;
    }

    static std::string make_derived_socket_identifier(const StringRef base_identifier,
                                                      const StringRef suffix)
    {
      std::string identifier = std::string(base_identifier);
      if (identifier.size() >= 2 && identifier.back() == '_' &&
          std::isdigit(uchar(identifier[identifier.size() - 2])))
      {
        identifier.pop_back();
      }
      identifier.append(suffix);
      return identifier;
    }

    static int make_panel_identifier(const StringRef name)
    {
      const std::string key = "glsl_meta_panel_" + std::string(name);
      return int(BLI_ghashutil_strhash_p(key.c_str()) & 0x7fffffff);
    }

    static int make_defines_panel_identifier(const bool default_closed)
    {
      const char* key = default_closed ? "glsl_defines_panel_closed" : "glsl_defines_panel";
      return int(BLI_ghashutil_strhash_p(key) & 0x7fffffff);
    }

    static int make_closures_panel_identifier()
    {
      return int(BLI_ghashutil_strhash_p("glsl_closures_panel") & 0x7fffffff);
    }

    static int make_code_interface_panel_identifier()
    {
      return int(BLI_ghashutil_strhash_p("glsl_code_interface_panel") & 0x7fffffff);
    }

    static std::string make_wrapper_argument_name(const StringRef prefix, const StringRef name)
    {
      std::string identifier = make_socket_identifier(prefix, name);
      return identifier;
    }

    static bool glsl_param_has_input_socket(const GLSLFunctionParam& param)
    {
      return ELEM(param.qualifier,
        GLSLFunctionParam::Qualifier::In,
        GLSLFunctionParam::Qualifier::InOut);
    }

    static bool is_sampler_sampling_function_name(const StringRef name)
    {
      return name == "texture" || name == "textureLod" || name == "textureGrad" ||
        name == "textureSize" || name == "texelFetch" || name == "textureGather";
    }

    static std::string make_split_vec4_w_socket_identifier(const StringRef identifier)
    {
      return make_derived_socket_identifier(identifier, "_w");
    }

    static std::string make_split_vec4_w_socket_name(const StringRef name)
    {
      return std::string(name) + " W";
    }

    static std::string make_split_matrix_column_socket_identifier(const StringRef identifier,
                                                                  const int column)
    {
      return make_derived_socket_identifier(identifier, "_c" + std::to_string(column));
    }

    static std::string make_split_matrix_column_socket_name(const StringRef name, const int column)
    {
      return std::string(name) + " C" + std::to_string(column + 1);
    }

    static std::string make_wrapper_vec4_temp_name(const StringRef name)
    {
      return make_derived_socket_identifier(name, "_vec4_tmp");
    }

    static std::string make_wrapper_temp_name(const StringRef name)
    {
      return make_derived_socket_identifier(name, "_tmp");
    }

    static bool glsl_param_uses_color_socket(const GLSLFunctionParam& param)
    {
      return param.type == GLSLBoundaryType::Vec3 &&
        param.meta.subtype.has_value() && *param.meta.subtype == PROP_COLOR;
    }

    static bool glsl_param_has_output_socket(const GLSLFunctionParam& param)
    {
      return ELEM(param.qualifier,
        GLSLFunctionParam::Qualifier::Out,
        GLSLFunctionParam::Qualifier::InOut);
    }

    static int glsl_function_output_count(const GLSLFunctionDefinition& function)
    {
      int count = function.return_type == GLSLBoundaryType::Void ? 0 : 1;
      for (const GLSLFunctionParam& param : function.params)
      {
        if (glsl_param_has_output_socket(param))
        {
          count++;
        }
      }
      return count;
    }

    static bool glsl_function_has_sampler_inputs(const GLSLFunctionDefinition& function)
    {
      for (const GLSLFunctionParam& param : function.params)
      {
        if (glsl_param_has_input_socket(param) && glsl_boundary_type_is_sampler(param.type))
        {
          return true;
        }
      }
      return false;
    }

    static const bNodeSocket* find_node_input_socket_by_identifier(const bNode& node,
      const StringRef identifier)
    {
      for (const bNodeSocket* socket : node.input_sockets())
      {
        if (identifier == socket->identifier)
        {
          return socket;
        }
      }
      return nullptr;
    }

    static bNodeSocket* find_node_input_socket_by_identifier(bNode& node, const StringRef identifier)
    {
      for (bNodeSocket* socket : node.input_sockets())
      {
        if (identifier == socket->identifier)
        {
          return socket;
        }
      }
      return nullptr;
    }

    static bool validate_sampler_inputs(const bNode& node,
      const Vector<GLSLToken>& tokens,
      const GLSLFunctionDefinition& function,
      std::string& r_error)
    {
      const Map<std::string, GLSLSamplerSourceKind> source_kinds = resolve_sampler_source_kinds(node,
        function);
      bool has_closure_output_sampler = false;

      for (const GLSLFunctionParam& param : function.params)
      {
        if (!glsl_param_has_input_socket(param) || !glsl_boundary_type_is_sampler(param.type))
        {
          continue;
        }

        const bNodeSocket* socket = find_linked_sampler_input_socket(node, param);
        if (socket == nullptr && node.runtime->original)
        {
          socket = find_linked_sampler_input_socket(*node.runtime->original, param);
        }
        if (socket != nullptr)
        {
          if (const bNodeLink* direct_link = find_any_direct_link(*socket))
          {
            if (direct_link->fromnode != nullptr && node_is_group_input(*direct_link->fromnode))
            {
              continue;
            }
          }

          const bNodeLink* used_link = nullptr;
          const bNode* logical_node = node.runtime->original ? node.runtime->original : &node;
          GLSLSamplerSourceKind source_kind = resolve_sampler_source_kind(
            node, param, used_link, logical_node);
          if (source_kind == GLSLSamplerSourceKind::GLSLFunction && used_link != nullptr)
          {
            source_kind = resolve_nested_sampler_source_kind(param, *used_link, used_link);
          }
          if (source_kind == GLSLSamplerSourceKind::GroupInput)
          {
            continue;
          }
          if (source_kind == GLSLSamplerSourceKind::ClosureOutput)
          {
            std::string closure_error;
            if (!closure_output_has_required_sampler_signature(
                  *used_link->fromnode, param.type, closure_error))
            {
              r_error = "Closure Output connected to " +
                std::string(sampler_type_name(param.type).c_str()) + " parameter '" + param.name +
                "' is missing required closure items: " + closure_error;
              return false;
            }
            has_closure_output_sampler = true;
            continue;
          }
          if (source_kind != GLSLSamplerSourceKind::ImageToClosure || used_link == nullptr)
          {
            r_error = std::string(sampler_type_name(param.type).c_str()) + " parameter '" + param.name +
              "' currently only supports Image to Closure or Closure Output links";
            return false;
          }

          if (used_link->fromnode != nullptr)
          {
            const int texture_type = image_to_closure_texture_type(*used_link->fromnode);
            if (glsl_boundary_type_is_sample2d(param.type) &&
                texture_type != IMA_IMAGE_TO_CLOSURE_TEXTURE_2D)
            {
              r_error = "sampler2D parameter '" + param.name +
                "' requires an Image to Closure node set to 2D Image";
              return false;
            }
            if (glsl_boundary_type_is_sample3d(param.type) &&
                texture_type != IMA_IMAGE_TO_CLOSURE_TEXTURE_3D_LUT_STRIP)
            {
              r_error = "sampler3D parameter '" + param.name +
                "' requires an Image to Closure node set to 3D LUT Strip";
              return false;
            }
          }
          else
          {
            r_error = std::string(sampler_type_name(param.type).c_str()) + " parameter '" +
              param.name + "' is missing an Image to Closure source";
            return false;
          }

          Image* image = resolve_image_to_closure_image(*used_link);
          if (image == nullptr)
          {
            r_error = "Choose an image on the Image to Closure node connected to '" + param.name + "'";
            return false;
          }
          if (glsl_boundary_type_is_sample3d(param.type))
          {
            std::string lut_error;
            ResolvedLut3DStripDimensions dimensions;
            if (!resolve_3d_lut_strip_dimensions(
                  *used_link->fromnode, *image, dimensions, lut_error))
            {
              r_error = "sampler3D parameter '" + param.name + "': " + lut_error;
              return false;
            }
            continue;
          }
          if (image->source == IMA_SRC_TILED)
          {
            r_error = "sampler2D parameter '" + param.name +
              "' does not support UDIM tiled images yet";
            return false;
          }
          continue;
        }
      }

      if (!has_closure_output_sampler)
      {
        return true;
      }

      const Vector<std::string> function_names = find_top_level_glsl_function_names(tokens);
      Vector<GLSLFunctionDefinition> all_functions;
      if (!find_all_top_level_glsl_function_definitions(tokens, function_names, all_functions, r_error))
      {
        return false;
      }

      Vector<GLSLClosureFunctionBindings> bindings_by_function;
      if (!build_closure_sampler_bindings_by_function(
        tokens, all_functions, function, source_kinds, bindings_by_function, r_error))
      {
        return false;
      }
      if (!validate_closure_sampler_bindings(
            tokens, all_functions, function, bindings_by_function, r_error))
      {
        return false;
      }

      return true;
    }

    static std::string trim_copy(const StringRef text)
    {
      int64_t start = 0;
      int64_t end = text.size();
      while (start < end && std::isspace(uchar(text[start])))
      {
        start++;
      }
      while (end > start && std::isspace(uchar(text[end - 1])))
      {
        end--;
      }
      return text.substr(start, end - start);
    }

    static std::string strip_glsl_comments(StringRef source)
    {
      std::string stripped;
      stripped.reserve(source.size());

      for (int64_t i = 0; i < source.size();)
      {
        if (ELEM(source[i], '\"', '\''))
        {
          const char quote = source[i];
          stripped.push_back(source[i++]);
          while (i < source.size())
          {
            const char c = source[i];
            stripped.push_back(c);
            i++;
            if (c == '\\' && i < source.size())
            {
              stripped.push_back(source[i++]);
              continue;
            }
            if (c == quote)
            {
              break;
            }
          }
          continue;
        }
        if ((i + 1) < source.size() && source[i] == '/' && source[i + 1] == '/')
        {
          stripped.append("  ");
          i += 2;
          while (i < source.size() && source[i] != '\n')
          {
            stripped.push_back(source[i] == '\r' ? '\r' : ' ');
            i++;
          }
          continue;
        }
        if ((i + 1) < source.size() && source[i] == '/' && source[i + 1] == '*')
        {
          stripped.append("  ");
          i += 2;
          while ((i + 1) < source.size() && !(source[i] == '*' && source[i + 1] == '/'))
          {
            stripped.push_back(ELEM(source[i], '\r', '\n') ? source[i] : ' ');
            i++;
          }
          if ((i + 1) < source.size())
          {
            stripped.append("  ");
            i += 2;
          }
          else if (i < source.size())
          {
            stripped.push_back(source[i++]);
          }
          continue;
        }
        stripped.push_back(source[i]);
        i++;
      }

      return stripped;
    }

    static std::string make_glsl_meta_key(const StringRef function_name, const StringRef param_name)
    {
      std::string key;
      key.reserve(function_name.size() + param_name.size() + 1);
      key.append(function_name);
      key.push_back('\x1f');
      key.append(param_name);
      return key;
    }

    static void split_glsl_meta_key(const StringRef key,
      StringRef& r_function_name,
      StringRef& r_param_name)
    {
      const int64_t separator = key.find('\x1f');
      if (separator == StringRef::not_found)
      {
        r_function_name = "";
        r_param_name = key;
        return;
      }
      r_function_name = key.substr(0, separator);
      r_param_name = key.substr(separator + 1);
    }

    static std::string lowercase_copy(StringRef text)
    {
      std::string result(text);
      for (char& c : result)
      {
        c = std::tolower(uchar(c));
      }
      return result;
    }

    static bool parse_glsl_meta_bool_literal(const StringRef text, bool& r_value, std::string& r_error)
    {
      const std::string normalized = lowercase_copy(trim_copy(text));
      if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on")
      {
        r_value = true;
        return true;
      }
      if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off")
      {
        r_value = false;
        return true;
      }
      r_error = "Expected a GLSL meta boolean literal";
      return false;
    }

    static bool parse_glsl_meta_int_literal(const StringRef text,
      int& r_value,
      std::string& r_error,
      bool* r_out_of_range = nullptr)
    {
      if (r_out_of_range != nullptr)
      {
        *r_out_of_range = false;
      }
      const std::string trimmed = trim_copy(text);
      if (trimmed.empty())
      {
        r_error = "GLSL meta integer value cannot be empty";
        return false;
      }

      char* end = nullptr;
      errno = 0;
      const long long value = std::strtoll(trimmed.c_str(), &end, 10);
      if (end == trimmed.c_str() || *end != '\0')
      {
        r_error = "Could not parse GLSL meta integer value '" + trimmed + "'";
        return false;
      }
      if (errno == ERANGE || value < INT_MIN || value > INT_MAX)
      {
        if (r_out_of_range != nullptr)
        {
          *r_out_of_range = true;
        }
        r_error = "GLSL meta integer value '" + trimmed + "' is outside the int32 range";
        return false;
      }
      r_value = int(value);
      return true;
    }

    static bool glsl_int_choice_contains_value(
      const Span<GLSLFunctionParam::IntChoiceItem> choices,
      const int value)
    {
      for (const GLSLFunctionParam::IntChoiceItem& item : choices)
      {
        if (item.value == value)
        {
          return true;
        }
      }
      return false;
    }

    static bool parse_glsl_int_choice_items(const StringRef text,
      Vector<GLSLFunctionParam::IntChoiceItem>& r_items,
      std::string& r_error)
    {
      const std::string trimmed = trim_copy(text);
      if (trimmed.empty())
      {
        r_error = "GLSL int items list cannot be empty";
        return false;
      }

      r_items.clear();
      Set<int> values;
      int64_t item_start = 0;
      while (item_start <= trimmed.size())
      {
        const int64_t separator = StringRef(trimmed).find(';', item_start);
        const int64_t item_end = separator == StringRef::not_found ? trimmed.size() : separator;
        const std::string item_text = trim_copy(StringRef(trimmed).substr(item_start,
          item_end - item_start));
        if (item_text.empty())
        {
          r_error = "GLSL int items list contains an empty item";
          return false;
        }

        const int64_t label_separator = StringRef(item_text).find(':');
        if (label_separator == StringRef::not_found)
        {
          r_error = "GLSL int items must use 'value:Label' entries";
          return false;
        }

        int value = 0;
        if (!parse_glsl_meta_int_literal(StringRef(item_text).substr(0, label_separator),
              value,
              r_error))
        {
          return false;
        }

        std::string label = trim_copy(StringRef(item_text).substr(label_separator + 1));
        if (label.empty())
        {
          r_error = "GLSL int item label cannot be empty";
          return false;
        }
        if (!values.add(value))
        {
          r_error = "GLSL int items list contains duplicate value '" + std::to_string(value) + "'";
          return false;
        }

        r_items.append({ value, std::move(label) });
        if (separator == StringRef::not_found)
        {
          break;
        }
        item_start = separator + 1;
      }

      if (r_items.is_empty())
      {
        r_error = "GLSL int items list cannot be empty";
        return false;
      }
      return true;
    }

    static bool parse_glsl_meta_quoted_string(const StringRef text,
      int64_t& r_index,
      std::string& r_value,
      std::string& r_error)
    {
      BLI_assert(r_index < text.size() && text[r_index] == '"');
      r_index++;
      while (r_index < text.size())
      {
        const char c = text[r_index];
        r_index++;
        if (c == '"')
        {
          return true;
        }
        if (c == '\\')
        {
          if (r_index >= text.size())
          {
            r_error = "Unterminated escape sequence in GLSL meta quoted string";
            return false;
          }
          const char escaped = text[r_index];
          r_index++;
          if (ELEM(escaped, '"', '\\'))
          {
            r_value.push_back(escaped);
            continue;
          }
          r_error = "Unsupported escape sequence in GLSL meta quoted string";
          return false;
        }
        r_value.push_back(c);
      }

      r_error = "Unterminated GLSL meta quoted string";
      return false;
    }

    static bool parse_glsl_meta_assignment_list(const StringRef text,
      Map<std::string, std::string>& r_assignments,
      std::string& r_error)
    {
      for (int64_t i = 0; i < text.size();)
      {
        while (i < text.size() && std::isspace(uchar(text[i])))
        {
          i++;
        }
        if (i >= text.size())
        {
          break;
        }
        if (!is_identifier_start(text[i]))
        {
          r_error = "Malformed GLSL meta attribute list";
          return false;
        }

        const int64_t key_start = i;
        i++;
        while (i < text.size() && is_identifier_continue(text[i]))
        {
          i++;
        }
        const std::string key = std::string(text.substr(key_start, i - key_start));

        while (i < text.size() && std::isspace(uchar(text[i])))
        {
          i++;
        }
        if (i >= text.size() || text[i] != '=')
        {
          r_error = "GLSL meta attributes must use key=value syntax";
          return false;
        }
        i++;

        while (i < text.size() && std::isspace(uchar(text[i])))
        {
          i++;
        }
        if (i >= text.size())
        {
          r_error = "GLSL meta attribute is missing a value";
          return false;
        }

        std::string value;
        if (text[i] == '"')
        {
          if (!parse_glsl_meta_quoted_string(text, i, value, r_error))
          {
            return false;
          }
          if (i < text.size() && !std::isspace(uchar(text[i])))
          {
            r_error = "GLSL meta quoted attribute values must be followed by whitespace";
            return false;
          }
        }
        else
        {
          const int64_t value_start = i;
          int paren_depth = 0;
          while (i < text.size())
          {
            const char c = text[i];
            if (c == '(')
            {
              paren_depth++;
            }
            else if (c == ')')
            {
              paren_depth = std::max(paren_depth - 1, 0);
            }
            else if (paren_depth == 0 && std::isspace(uchar(c)))
            {
              break;
            }
            i++;
          }
          value = std::string(text.substr(value_start, i - value_start));
        }

        value = trim_copy(value);
        if (value.empty())
        {
          r_error = "GLSL meta attribute is missing a value";
          return false;
        }
        if (r_assignments.contains(key))
        {
          r_error = "Duplicate GLSL meta attribute '" + key + "'";
          return false;
        }
        r_assignments.add(key, value);
      }

      return true;
    }

    static bool parse_glsl_meta_panel_directive(const StringRef text,
      GLSLPanelMeta& r_panel,
      std::string& r_error)
    {
      int64_t i = 0;
      while (i < text.size() && std::isspace(uchar(text[i])))
      {
        i++;
      }
      if (i >= text.size())
      {
        r_error = "GLSL meta panel name cannot be empty";
        return false;
      }

      if (text[i] == '"')
      {
        i++;
        std::string name;
        bool found_end_quote = false;
        while (i < text.size())
        {
          const char c = text[i];
          if (c == '"')
          {
            found_end_quote = true;
            i++;
            break;
          }
          name.push_back(c);
          i++;
        }
        if (!found_end_quote)
        {
          r_error = "Unterminated GLSL meta panel name";
          return false;
        }
        r_panel.name = trim_copy(name);
      }
      else
      {
        const int64_t name_start = i;
        while (i < text.size() && !std::isspace(uchar(text[i])))
        {
          i++;
        }
        r_panel.name = trim_copy(text.substr(name_start, i - name_start));
      }

      if (r_panel.name.empty())
      {
        r_error = "GLSL meta panel name cannot be empty";
        return false;
      }

      const std::string attributes_text = trim_copy(text.substr(i));
      if (attributes_text.empty())
      {
        r_panel.default_closed = true;
        return true;
      }

      Map<std::string, std::string> assignments;
      if (!parse_glsl_meta_assignment_list(attributes_text, assignments, r_error))
      {
        return false;
      }

      for (const auto& item : assignments.items())
      {
        const StringRef key = item.key;
        const StringRef value = item.value;
        if (key != "closed")
        {
          r_error = "Unsupported GLSL meta panel attribute '" + std::string(key) + "'";
          return false;
        }
        if (!parse_glsl_meta_bool_literal(value, r_panel.default_closed, r_error))
        {
          return false;
        }
      }

      return true;
    }

    static bool assign_glsl_raw_meta_panel(GLSLRawParamMeta& r_meta,
      const StringRef panel_name,
      const StringRef param_name,
      std::string& r_error)
    {
      if (r_meta.panel_name.has_value())
      {
        if (*r_meta.panel_name != panel_name)
        {
          r_error = "GLSL meta parameter '" + std::string(param_name) +
            "' cannot belong to multiple panels";
          return false;
        }
        return true;
      }
      r_meta.panel_name = std::string(panel_name);
      return true;
    }

    static bool merge_glsl_raw_param_meta(GLSLRawParamMeta& r_meta,
      const Map<std::string, std::string>& assignments,
      std::string& r_error)
    {
      auto assign_once = [&](std::optional<std::string>& slot,
        const StringRef key,
        const StringRef value) -> bool
        {
          if (slot.has_value())
          {
            r_error = "Duplicate GLSL meta attribute '" + std::string(key) + "'";
            return false;
          }
          slot = std::string(value);
          return true;
        };

      for (const auto& item : assignments.items())
      {
        const StringRef key = item.key;
        const StringRef value = item.value;
        if (key == "default")
        {
          if (!assign_once(r_meta.default_value, key, value))
          {
            return false;
          }
        }
        else if (key == "min")
        {
          if (!assign_once(r_meta.min_value, key, value))
          {
            return false;
          }
        }
        else if (key == "max")
        {
          if (!assign_once(r_meta.max_value, key, value))
          {
            return false;
          }
        }
        else if (key == "hide_value")
        {
          if (!assign_once(r_meta.hide_value, key, value))
          {
            return false;
          }
        }
        else if (key == "subtype")
        {
          if (!assign_once(r_meta.subtype, key, value))
          {
            return false;
          }
        }
        else if (key == "items")
        {
          if (!assign_once(r_meta.items, key, value))
          {
            return false;
          }
        }
        else if (key == "show_label")
        {
          if (!assign_once(r_meta.show_label, key, value))
          {
            return false;
          }
        }
        else if (key == "description")
        {
          if (!assign_once(r_meta.description, key, value))
          {
            return false;
          }
        }
        else if (key == "label")
        {
          if (!assign_once(r_meta.label, key, value))
          {
            return false;
          }
        }
        else
        {
          r_error = "Unsupported GLSL meta attribute '" + std::string(key) + "'";
          return false;
        }
      }

      return true;
    }

    static bool parse_glsl_meta_block(const StringRef comment,
      Map<std::string, GLSLRawParamMeta>& r_param_meta_by_name,
      Vector<GLSLPanelMeta>& r_panels,
      bool& r_is_meta_block,
      std::string& r_error)
    {
      r_is_meta_block = false;
      std::stringstream stream{ std::string(comment) };
      std::string line;
      bool header_seen = false;
      std::optional<std::string> active_panel_name;
      Set<std::string> panel_names;

      while (std::getline(stream, line))
      {
        std::string normalized = trim_copy(line);
        if (!normalized.empty() && normalized[0] == '*')
        {
          normalized = trim_copy(StringRef(normalized).drop_prefix(1));
        }
        if (normalized.empty())
        {
          continue;
        }

        if (!header_seen)
        {
          if (!StringRef(normalized).startswith("@glsl_meta"))
          {
            return true;
          }
          header_seen = true;
          r_is_meta_block = true;
          continue;
        }

        if (StringRef(normalized).startswith("@panel") &&
            (normalized.size() == 6 || std::isspace(uchar(normalized[6]))))
        {
          if (active_panel_name.has_value())
          {
            r_error = "Nested GLSL meta panels are not supported";
            return false;
          }

          GLSLPanelMeta panel;
          if (!parse_glsl_meta_panel_directive(StringRef(normalized).drop_prefix(6), panel, r_error))
          {
            return false;
          }
          if (!panel_names.add(panel.name))
          {
            r_error = "Duplicate GLSL meta panel '" + panel.name + "'";
            return false;
          }
          active_panel_name = panel.name;
          r_panels.append(panel);
          continue;
        }

        if (StringRef(normalized).startswith("@end_panel"))
        {
          if (normalized != "@end_panel")
          {
            r_error = "GLSL meta @end_panel does not support attributes";
            return false;
          }
          if (!active_panel_name.has_value())
          {
            r_error = "GLSL meta @end_panel has no matching @panel";
            return false;
          }
          active_panel_name.reset();
          continue;
        }

        if (StringRef(normalized).startswith("@"))
        {
          r_error = "Unsupported GLSL meta directive '" + normalized + "'";
          return false;
        }

        if (StringRef(normalized).startswith("function "))
        {
          r_error =
            "GLSL meta blocks now belong to the function directly below them; remove the "
            "'function ...' line";
          return false;
        }

        const int64_t separator = StringRef(normalized).find(':');
        if (separator == StringRef::not_found)
        {
          r_error = "GLSL meta lines must use 'name: key=value' syntax";
          return false;
        }

        std::string target = trim_copy(StringRef(normalized).substr(0, separator));
        const std::string attributes_text = trim_copy(StringRef(normalized).substr(separator + 1));
        if (target.empty() || (attributes_text.empty() && !active_panel_name.has_value()))
        {
          r_error = "GLSL meta lines must define a target and at least one attribute";
          return false;
        }

        const int64_t dot_index = StringRef(target).find('.');
        if (dot_index != StringRef::not_found)
        {
          r_error =
            "GLSL meta blocks now belong to the function directly below them; use plain parameter "
            "names inside the block";
          return false;
        }
        const std::string param_name = target;
        if (param_name.empty())
        {
          r_error = "GLSL meta parameter target cannot be empty";
          return false;
        }

        Map<std::string, std::string> assignments;
        if (!parse_glsl_meta_assignment_list(attributes_text, assignments, r_error))
        {
          return false;
        }

        GLSLRawParamMeta meta;
        if (const GLSLRawParamMeta* existing = r_param_meta_by_name.lookup_ptr(param_name))
        {
          meta = *existing;
        }

        if (active_panel_name.has_value())
        {
          if (!assign_glsl_raw_meta_panel(meta, *active_panel_name, param_name, r_error))
          {
            return false;
          }
        }

        if (!merge_glsl_raw_param_meta(meta, assignments, r_error))
        {
          return false;
        }

        r_param_meta_by_name.add_overwrite(param_name, meta);
      }

      if (active_panel_name.has_value())
      {
        r_error = "GLSL meta panel '" + *active_panel_name + "' must be closed with @end_panel";
        return false;
      }

      return true;
    }

    static bool find_glsl_meta_target_function_name(const StringRef source_after_comment,
      std::string& r_function_name,
      std::string& r_error)
    {
      const std::string stripped_source = strip_glsl_comments(source_after_comment);
      const Vector<GLSLToken> tokens = tokenize_glsl_source(stripped_source);
      if (tokens.is_empty())
      {
        r_error = "GLSL meta block must be placed directly above a function definition";
        return false;
      }

      int brace_depth = 0;
      for (int i = 0; i < tokens.size(); i++)
      {
        const GLSLToken& token = tokens[i];
        if (token.kind != GLSLToken::Kind::Punctuation)
        {
          continue;
        }

        if (token.punctuation == '{')
        {
          if (brace_depth == 0)
          {
            r_error = "GLSL meta block must be placed directly above a function definition";
            return false;
          }
          brace_depth++;
          continue;
        }
        if (token.punctuation == '}')
        {
          brace_depth = max_ii(0, brace_depth - 1);
          continue;
        }
        if (brace_depth != 0)
        {
          continue;
        }
        if (token.punctuation == ';')
        {
          r_error = "GLSL meta block must be placed directly above a function definition";
          return false;
        }
        if (token.punctuation != '(' || i < 2 || tokens[i - 1].kind != GLSLToken::Kind::Identifier ||
          tokens[i - 2].kind != GLSLToken::Kind::Identifier)
        {
          continue;
        }

        int paren_depth = 1;
        int closing_paren_index = -1;
        for (int j = i + 1; j < tokens.size(); j++)
        {
          if (tokens[j].kind != GLSLToken::Kind::Punctuation)
          {
            continue;
          }
          if (tokens[j].punctuation == '(')
          {
            paren_depth++;
          }
          else if (tokens[j].punctuation == ')')
          {
            paren_depth--;
            if (paren_depth == 0)
            {
              closing_paren_index = j;
              break;
            }
          }
        }

        if (closing_paren_index != -1 && (closing_paren_index + 1) < tokens.size() &&
          tokens[closing_paren_index + 1].kind == GLSLToken::Kind::Punctuation &&
          tokens[closing_paren_index + 1].punctuation == '{')
        {
          r_function_name = tokens[i - 1].text;
          return true;
        }

        r_error = "GLSL meta block must be placed directly above a function definition";
        return false;
      }

      r_error = "GLSL meta block must be placed directly above a function definition";
      return false;
    }

    static bool extract_glsl_meta(const StringRef source,
      Map<std::string, GLSLRawParamMeta>& r_meta_by_key,
      Map<std::string, Vector<GLSLPanelMeta>>& r_panels_by_function,
      std::string& r_error)
    {
      Set<std::string> functions_with_meta;
      for (int64_t i = 0; (i + 1) < source.size();)
      {
        if (source[i] == '/' && source[i + 1] == '*')
        {
          const int64_t body_start = i + 2;
          int64_t body_end = source.size();
          bool found_end = false;
          for (int64_t j = body_start; (j + 1) < source.size(); j++)
          {
            if (source[j] == '*' && source[j + 1] == '/')
            {
              body_end = j;
              i = j + 2;
              found_end = true;
              break;
            }
          }
          if (!found_end)
          {
            r_error = "Unterminated GLSL block comment";
            return false;
          }

          Map<std::string, GLSLRawParamMeta> param_meta_by_name;
          Vector<GLSLPanelMeta> panels;
          bool is_meta_block = false;
          if (!parse_glsl_meta_block(source.substr(body_start, body_end - body_start),
            param_meta_by_name,
            panels,
            is_meta_block,
            r_error))
          {
            return false;
          }
          if (!is_meta_block)
          {
            continue;
          }

          std::string function_name;
          if (!find_glsl_meta_target_function_name(source.substr(i), function_name, r_error))
          {
            return false;
          }
          if (functions_with_meta.contains(function_name))
          {
            r_error = "Only one GLSL meta block is supported per function";
            return false;
          }
          functions_with_meta.add(function_name);

          for (const auto& item : param_meta_by_name.items())
          {
            r_meta_by_key.add(make_glsl_meta_key(function_name, item.key), item.value);
          }
          if (!panels.is_empty())
          {
            r_panels_by_function.add(function_name, panels);
          }
          continue;
        }
        i++;
      }

      return true;
    }

    static bool parse_glsl_closure_block(const StringRef comment,
      GLSLRawClosureMeta& r_closure,
      bool& r_is_closure_block,
      std::string& r_error)
    {
      r_is_closure_block = false;
      std::stringstream stream{std::string(comment)};
      std::string line;
      bool header_seen = false;

      while (std::getline(stream, line))
      {
        std::string normalized = trim_copy(line);
        if (!normalized.empty() && normalized[0] == '*')
        {
          normalized = trim_copy(StringRef(normalized).drop_prefix(1));
        }
        if (normalized.empty())
        {
          continue;
        }

        if (!header_seen)
        {
          if (!StringRef(normalized).startswith("@glsl_closure"))
          {
            return true;
          }
          const StringRef suffix = StringRef(normalized).drop_prefix(13);
          const std::string options = trim_copy(suffix);
          if (options.size() < 2 || !StringRef(options).startswith("v1") ||
              (options.size() > 2 && !std::isspace(uchar(options[2]))))
          {
            r_error = "GLSL closure header must start with '@glsl_closure v1'";
            return false;
          }

          Map<std::string, std::string> assignments;
          if (!parse_glsl_meta_assignment_list(StringRef(options).drop_prefix(2), assignments, r_error))
          {
            return false;
          }
          for (const auto& item : assignments.items())
          {
            if (item.key == "label")
            {
              r_closure.label = item.value;
            }
            else if (item.key == "description")
            {
              r_closure.description = item.value;
            }
            else
            {
              r_error = "Unsupported GLSL closure header attribute '" + item.key + "'";
              return false;
            }
          }
          header_seen = true;
          r_is_closure_block = true;
          continue;
        }

        if (StringRef(normalized).startswith("@"))
        {
          r_error = "GLSL closure blocks do not support nested directives";
          return false;
        }

        const int64_t separator = StringRef(normalized).find(':');
        if (separator == StringRef::not_found)
        {
          r_error = "GLSL closure lines must use 'name: key=value' syntax";
          return false;
        }
        const std::string target = trim_copy(StringRef(normalized).substr(0, separator));
        const std::string attributes_text = trim_copy(StringRef(normalized).substr(separator + 1));
        if (target.empty() || attributes_text.empty())
        {
          r_error = "GLSL closure lines must define a target and at least one attribute";
          return false;
        }
        if (StringRef(target).find('.') != StringRef::not_found)
        {
          r_error = "GLSL closure item targets must use plain parameter names";
          return false;
        }

        Map<std::string, std::string> assignments;
        if (!parse_glsl_meta_assignment_list(attributes_text, assignments, r_error))
        {
          return false;
        }
        GLSLRawParamMeta meta;
        if (const GLSLRawParamMeta* existing = r_closure.items.lookup_ptr(target))
        {
          meta = *existing;
        }
        if (!merge_glsl_raw_param_meta(meta, assignments, r_error))
        {
          return false;
        }
        r_closure.items.add_overwrite(target, std::move(meta));
      }

      return true;
    }

    static bool extract_glsl_closures(const StringRef source,
      Map<std::string, GLSLRawClosureMeta>& r_closures_by_function,
      std::string& r_error)
    {
      Set<std::string> annotated_functions;
      for (int64_t i = 0; (i + 1) < source.size();)
      {
        if (source[i] == '/' && source[i + 1] == '/')
        {
          i += 2;
          while (i < source.size() && source[i] != '\n')
          {
            i++;
          }
          continue;
        }
        if (source[i] != '/' || source[i + 1] != '*')
        {
          i++;
          continue;
        }

        const int64_t body_start = i + 2;
        int64_t body_end = source.size();
        bool found_end = false;
        for (int64_t j = body_start; (j + 1) < source.size(); j++)
        {
          if (source[j] == '*' && source[j + 1] == '/')
          {
            body_end = j;
            i = j + 2;
            found_end = true;
            break;
          }
        }
        if (!found_end)
        {
          r_error = "Unterminated GLSL block comment";
          return false;
        }

        GLSLRawClosureMeta closure;
        bool is_closure_block = false;
        if (!parse_glsl_closure_block(source.substr(body_start, body_end - body_start),
                                      closure,
                                      is_closure_block,
                                      r_error))
        {
          return false;
        }
        if (!is_closure_block)
        {
          continue;
        }

        std::string function_name;
        if (!find_glsl_meta_target_function_name(source.substr(i), function_name, r_error))
        {
          r_error = "GLSL closure block must be placed directly above a function definition";
          return false;
        }
        if (!annotated_functions.add(function_name))
        {
          r_error = "Only one GLSL closure block is supported per function";
          return false;
        }
        r_closures_by_function.add(function_name, std::move(closure));
      }
      return true;
    }

    static bool parse_glsl_define_directive(const StringRef text,
      GLSLDefineMeta& r_define,
      std::string& r_error)
    {
      int64_t i = 0;
      while (i < text.size() && std::isspace(uchar(text[i])))
      {
        i++;
      }
      if (i >= text.size() || !is_identifier_start(text[i]))
      {
        r_error = "GLSL define name must be a valid identifier";
        return false;
      }
      const int64_t name_start = i;
      i++;
      while (i < text.size() && is_identifier_continue(text[i]))
      {
        i++;
      }
      r_define.name = std::string(text.substr(name_start, i - name_start));
      if (r_define.name.size() >= sizeof(NodeShaderGLSLDefineValue::name))
      {
        r_error = "GLSL define name '" + r_define.name + "' exceeds the " +
          std::to_string(sizeof(NodeShaderGLSLDefineValue::name) - 1) + " character limit";
        return false;
      }
      if (StringRef(r_define.name).startswith("gl_"))
      {
        r_error = "GLSL define name '" + r_define.name + "' uses the reserved 'gl_' prefix";
        return false;
      }

      while (i < text.size() && std::isspace(uchar(text[i])))
      {
        i++;
      }
      if (i >= text.size() || !is_identifier_start(text[i]))
      {
        r_error = "GLSL define '" + r_define.name + "' is missing its type";
        return false;
      }
      const int64_t type_start = i;
      i++;
      while (i < text.size() && is_identifier_continue(text[i]))
      {
        i++;
      }
      const std::string type_name = std::string(text.substr(type_start, i - type_start));
      if (type_name == "bool")
      {
        r_define.type = SHD_GLSL_FUNCTION_DEFINE_BOOL;
      }
      else if (type_name == "int")
      {
        r_define.type = SHD_GLSL_FUNCTION_DEFINE_INT;
      }
      else
      {
        r_error = "Unsupported GLSL define type '" + type_name + "'";
        return false;
      }

      const std::string attributes_text = trim_copy(text.substr(i));
      if (attributes_text.empty())
      {
        r_error = "GLSL define '" + r_define.name + "' must specify default=...";
        return false;
      }

      Map<std::string, std::string> assignments;
      if (!parse_glsl_meta_assignment_list(attributes_text, assignments, r_error))
      {
        return false;
      }

      bool has_default = false;
      bool has_show_label = false;
      for (const auto& item : assignments.items())
      {
        const StringRef key = item.key;
        const StringRef value = item.value;
        if (key == "default")
        {
          has_default = true;
          if (r_define.type == SHD_GLSL_FUNCTION_DEFINE_BOOL)
          {
            bool bool_value = false;
            if (!parse_glsl_meta_bool_literal(value, bool_value, r_error))
            {
              return false;
            }
            r_define.default_value = bool_value ? 1 : 0;
          }
          else
          {
            if (!parse_glsl_meta_int_literal(value, r_define.default_value, r_error))
            {
              return false;
            }
          }
        }
        else if (key == "min")
        {
          if (r_define.type != SHD_GLSL_FUNCTION_DEFINE_INT)
          {
            r_error = "GLSL bool define '" + r_define.name + "' does not support min";
            return false;
          }
          int value_int = 0;
          if (!parse_glsl_meta_int_literal(value, value_int, r_error))
          {
            return false;
          }
          r_define.min_value = value_int;
        }
        else if (key == "max")
        {
          if (r_define.type != SHD_GLSL_FUNCTION_DEFINE_INT)
          {
            r_error = "GLSL bool define '" + r_define.name + "' does not support max";
            return false;
          }
          int value_int = 0;
          if (!parse_glsl_meta_int_literal(value, value_int, r_error))
          {
            return false;
          }
          r_define.max_value = value_int;
        }
        else if (key == "items")
        {
          if (r_define.type != SHD_GLSL_FUNCTION_DEFINE_INT)
          {
            r_error = "GLSL bool define '" + r_define.name + "' does not support items";
            return false;
          }
          if (!parse_glsl_int_choice_items(value, r_define.int_choices, r_error))
          {
            return false;
          }
        }
        else if (key == "show_label")
        {
          has_show_label = true;
          if (r_define.type != SHD_GLSL_FUNCTION_DEFINE_INT)
          {
            r_error = "GLSL bool define '" + r_define.name + "' does not support show_label";
            return false;
          }
          if (!parse_glsl_meta_bool_literal(value, r_define.int_choices_show_label, r_error))
          {
            return false;
          }
        }
        else if (key == "label")
        {
          r_define.label = std::string(value);
        }
        else if (key == "description")
        {
          r_define.description = std::string(value);
        }
        else
        {
          r_error = "Unsupported GLSL define attribute '" + std::string(key) + "'";
          return false;
        }
      }

      if (!has_default)
      {
        r_error = "GLSL define '" + r_define.name + "' must specify default=...";
        return false;
      }
      if (r_define.type == SHD_GLSL_FUNCTION_DEFINE_BOOL)
      {
        r_define.default_value = r_define.default_value != 0 ? 1 : 0;
      }
      else
      {
        if (has_show_label && r_define.int_choices.is_empty())
        {
          r_error = "GLSL int define '" + r_define.name + "' show_label requires items";
          return false;
        }
        if (!r_define.int_choices.is_empty())
        {
          if (r_define.min_value.has_value() || r_define.max_value.has_value())
          {
            r_error = "GLSL int define '" + r_define.name +
              "' cannot combine items with min or max";
            return false;
          }
          if (!glsl_int_choice_contains_value(r_define.int_choices, r_define.default_value))
          {
            r_error = "GLSL int define '" + r_define.name +
              "' default must be one of its items";
            return false;
          }
        }
        else if (r_define.min_value.has_value() && r_define.max_value.has_value() &&
          *r_define.min_value > *r_define.max_value)
        {
          r_error = "GLSL define '" + r_define.name + "' has min greater than max";
          return false;
        }
        if (r_define.int_choices.is_empty() && r_define.min_value.has_value())
        {
          r_define.default_value = std::max(r_define.default_value, *r_define.min_value);
        }
        if (r_define.int_choices.is_empty() && r_define.max_value.has_value())
        {
          r_define.default_value = std::min(r_define.default_value, *r_define.max_value);
        }
      }
      return true;
    }

    static bool parse_glsl_defines_header_options(const StringRef text,
      bool& r_panel_default_closed,
      bool& r_has_panel_default_closed,
      std::string& r_error)
    {
      const std::string header_text = trim_copy(text);
      if (header_text.empty())
      {
        return true;
      }

      std::string options_text = header_text;
      int64_t first_token_end = 0;
      while (first_token_end < options_text.size() &&
             !std::isspace(uchar(options_text[first_token_end])))
      {
        first_token_end++;
      }

      const StringRef first_token = StringRef(options_text).substr(0, first_token_end);
      if (first_token.startswith("v"))
      {
        const std::string version_text = std::string(first_token);
        if (version_text != "v1")
        {
          r_error = "Unsupported GLSL defines metadata version '" + version_text + "'";
          return false;
        }
        options_text = trim_copy(StringRef(options_text).substr(first_token_end));
      }

      if (options_text.empty())
      {
        return true;
      }

      Map<std::string, std::string> assignments;
      if (!parse_glsl_meta_assignment_list(options_text, assignments, r_error))
      {
        return false;
      }

      for (const auto& item : assignments.items())
      {
        const StringRef key = item.key;
        const StringRef value = item.value;
        if (key != "closed")
        {
          r_error = "Unsupported GLSL defines attribute '" + std::string(key) + "'";
          return false;
        }
        if (!parse_glsl_meta_bool_literal(value, r_panel_default_closed, r_error))
        {
          return false;
        }
        r_has_panel_default_closed = true;
      }

      return true;
    }

    static bool parse_glsl_defines_block(const StringRef comment,
      Vector<GLSLDefineMeta>& r_defines,
      bool& r_is_defines_block,
      bool& r_panel_default_closed,
      bool& r_has_panel_default_closed,
      std::string& r_error)
    {
      r_is_defines_block = false;
      r_panel_default_closed = false;
      r_has_panel_default_closed = false;
      std::stringstream stream{ std::string(comment) };
      std::string line;
      bool header_seen = false;

      while (std::getline(stream, line))
      {
        std::string normalized = trim_copy(line);
        if (!normalized.empty() && normalized[0] == '*')
        {
          normalized = trim_copy(StringRef(normalized).drop_prefix(1));
        }
        if (normalized.empty())
        {
          continue;
        }

        if (!header_seen)
        {
          if (!StringRef(normalized).startswith("@glsl_defines") ||
              (normalized.size() > 13 && !std::isspace(uchar(normalized[13]))))
          {
            return true;
          }
          if (!parse_glsl_defines_header_options(StringRef(normalized).drop_prefix(13),
                r_panel_default_closed,
                r_has_panel_default_closed,
                r_error))
          {
            return false;
          }
          header_seen = true;
          r_is_defines_block = true;
          continue;
        }

        if (StringRef(normalized).startswith("@define") &&
          (normalized.size() == 7 || std::isspace(uchar(normalized[7]))))
        {
          GLSLDefineMeta define;
          if (!parse_glsl_define_directive(StringRef(normalized).drop_prefix(7), define, r_error))
          {
            return false;
          }
          r_defines.append(define);
          continue;
        }

        if (StringRef(normalized).startswith("@"))
        {
          r_error = "Unsupported GLSL defines directive '" + normalized + "'";
          return false;
        }

        r_error = "GLSL defines lines must use '@define NAME bool|int key=value' syntax";
        return false;
      }

      return true;
    }

    static bool extract_glsl_defines(const StringRef source,
      Vector<GLSLDefineMeta>& r_defines,
      bool& r_panel_default_closed,
      std::string& r_error)
    {
      Set<std::string> define_names;
      r_panel_default_closed = false;
      std::optional<bool> explicit_panel_default_closed;
      for (int64_t i = 0; (i + 1) < source.size();)
      {
        if (source[i] == '/' && source[i + 1] == '*')
        {
          const int64_t body_start = i + 2;
          int64_t body_end = source.size();
          bool found_end = false;
          for (int64_t j = body_start; (j + 1) < source.size(); j++)
          {
            if (source[j] == '*' && source[j + 1] == '/')
            {
              body_end = j;
              i = j + 2;
              found_end = true;
              break;
            }
          }
          if (!found_end)
          {
            r_error = "Unterminated GLSL block comment";
            return false;
          }

          Vector<GLSLDefineMeta> block_defines;
          bool is_defines_block = false;
          bool block_panel_default_closed = false;
          bool block_has_panel_default_closed = false;
          if (!parse_glsl_defines_block(
                source.substr(body_start, body_end - body_start),
                block_defines,
                is_defines_block,
                block_panel_default_closed,
                block_has_panel_default_closed,
                r_error))
          {
            return false;
          }
          if (!is_defines_block)
          {
            continue;
          }
          if (block_has_panel_default_closed)
          {
            if (explicit_panel_default_closed.has_value() &&
                *explicit_panel_default_closed != block_panel_default_closed)
            {
              r_error = "Conflicting GLSL defines panel 'closed' values";
              return false;
            }
            explicit_panel_default_closed = block_panel_default_closed;
            r_panel_default_closed = block_panel_default_closed;
          }
          for (const GLSLDefineMeta& define : block_defines)
          {
            if (!define_names.add(define.name))
            {
              r_error = "Duplicate GLSL define '" + define.name + "'";
              return false;
            }
            r_defines.append(define);
          }
          continue;
        }
        i++;
      }
      return true;
    }

    static int clamp_glsl_define_value(const GLSLDefineMeta& define, const int value)
    {
      if (define.type == SHD_GLSL_FUNCTION_DEFINE_BOOL)
      {
        return value != 0 ? 1 : 0;
      }
      if (!define.int_choices.is_empty())
      {
        return glsl_int_choice_contains_value(define.int_choices, value) ? value :
                                                                           define.default_value;
      }
      int result = value;
      if (define.min_value.has_value())
      {
        result = std::max(result, *define.min_value);
      }
      if (define.max_value.has_value())
      {
        result = std::min(result, *define.max_value);
      }
      return result;
    }

    static const NodeShaderGLSLDefineValue* find_glsl_define_value(
      const NodeShaderGLSLFunction& storage,
      const GLSLDefineMeta& define)
    {
      for (const int i : IndexRange(storage.define_values_num))
      {
        const NodeShaderGLSLDefineValue& value = storage.define_values[i];
        if (value.type == define.type && define.name == value.name)
        {
          return &value;
        }
      }
      return nullptr;
    }

    static int glsl_define_value_or_default(const bNode& node, const GLSLDefineMeta& define)
    {
      const NodeShaderGLSLFunction& storage = node_storage(node);
      if (storage.define_values != nullptr)
      {
        if (const NodeShaderGLSLDefineValue* value = find_glsl_define_value(storage, define))
        {
          return clamp_glsl_define_value(define, value->value);
        }
      }
      return clamp_glsl_define_value(define, define.default_value);
    }

    static bool glsl_define_values_equal(const NodeShaderGLSLFunction& storage,
      const Span<GLSLDefineValueState> new_values)
    {
      if (storage.define_values_num != new_values.size())
      {
        return false;
      }
      if (storage.define_values_num > 0 && storage.define_values == nullptr)
      {
        return false;
      }
      for (const int i : new_values.index_range())
      {
        const NodeShaderGLSLDefineValue& current = storage.define_values[i];
        const GLSLDefineValueState& expected = new_values[i];
        if (!STREQ(current.name, expected.name) || current.type != expected.type ||
          current.value != expected.value)
        {
          return false;
        }
      }
      return true;
    }

    static void register_glsl_define_int_choices(
      NodeShaderGLSLDefineValue& value,
      const Span<GLSLFunctionParam::IntChoiceItem> choices)
    {
      if (choices.is_empty())
      {
        RNA_shader_node_glsl_define_value_choices_unregister(&value, 1);
        return;
      }

      Vector<int> values;
      Vector<const char*> labels;
      values.reserve(choices.size());
      labels.reserve(choices.size());
      for (const GLSLFunctionParam::IntChoiceItem& choice : choices)
      {
        values.append(choice.value);
        labels.append(choice.label.c_str());
      }
      RNA_shader_node_glsl_define_value_choices_register(
        &value, values.data(), labels.data(), int(choices.size()));
    }

    static void sync_glsl_define_choice_items(NodeShaderGLSLFunction& storage,
                                              const GLSLParseResult& parse_result)
    {
      if (storage.define_values == nullptr)
      {
        return;
      }
      for (const GLSLDefineMeta& define : parse_result.defines)
      {
        NodeShaderGLSLDefineValue* value = nullptr;
        for (const int i : IndexRange(storage.define_values_num))
        {
          NodeShaderGLSLDefineValue& candidate = storage.define_values[i];
          if (candidate.type == define.type && define.name == candidate.name)
          {
            value = &candidate;
            break;
          }
        }
        if (value == nullptr)
        {
          continue;
        }
        if (define.type == SHD_GLSL_FUNCTION_DEFINE_INT)
        {
          register_glsl_define_int_choices(*value, define.int_choices);
        }
        else
        {
          RNA_shader_node_glsl_define_value_choices_unregister(value, 1);
        }
      }
    }

    static void sync_glsl_define_values(bNode& node, const GLSLParseResult& parse_result)
    {
      NodeShaderGLSLFunction& storage = node_storage(node);
      if (!parse_result.defines_parsed)
      {
        RNA_shader_node_glsl_define_value_choices_unregister(storage.define_values,
                                                             storage.define_values_num);
        return;
      }

      Vector<GLSLDefineValueState> new_values;
      new_values.reserve(parse_result.defines.size());
      for (const GLSLDefineMeta& define : parse_result.defines)
      {
        GLSLDefineValueState value;
        STRNCPY(value.name, define.name.c_str());
        value.type = define.type;
        value.value = define.default_value;
        if (storage.define_values != nullptr)
        {
          if (const NodeShaderGLSLDefineValue* existing = find_glsl_define_value(storage, define))
          {
            value.value = existing->value;
          }
        }
        value.value = clamp_glsl_define_value(define, value.value);
        new_values.append(value);
      }

      if (glsl_define_values_equal(storage, new_values))
      {
        sync_glsl_define_choice_items(storage, parse_result);
        return;
      }

      RNA_shader_node_glsl_define_value_choices_unregister(storage.define_values,
                                                           storage.define_values_num);
      MEM_SAFE_DELETE(storage.define_values);
      storage.define_values_num = new_values.size();
      if (!new_values.is_empty())
      {
        storage.define_values = MEM_new_array<NodeShaderGLSLDefineValue>(
          storage.define_values_num, __func__);
        for (const int i : new_values.index_range())
        {
          NodeShaderGLSLDefineValue& dst = storage.define_values[i];
          const GLSLDefineValueState& src = new_values[i];
          STRNCPY(dst.name, src.name);
          dst.type = src.type;
          dst.value = src.value;
        }
      }
      else
      {
        storage.define_values = nullptr;
      }
      sync_glsl_define_choice_items(storage, parse_result);
    }

    static bool reset_glsl_define_values_to_defaults(bNode& node,
                                                     const GLSLParseResult& parse_result)
    {
      NodeShaderGLSLFunction& storage = node_storage(node);
      if (!parse_result.defines_parsed || parse_result.defines.is_empty())
      {
        return false;
      }

      sync_glsl_define_values(node, parse_result);

      bool changed = false;
      for (const GLSLDefineMeta& define : parse_result.defines)
      {
        NodeShaderGLSLDefineValue* value = nullptr;
        for (const int i : IndexRange(storage.define_values_num))
        {
          NodeShaderGLSLDefineValue& candidate = storage.define_values[i];
          if (candidate.type == define.type && define.name == candidate.name)
          {
            value = &candidate;
            break;
          }
        }
        if (value == nullptr)
        {
          continue;
        }

        const int default_value = clamp_glsl_define_value(define, define.default_value);
        if (value->value != default_value)
        {
          value->value = default_value;
          changed = true;
        }
        if (define.type == SHD_GLSL_FUNCTION_DEFINE_INT)
        {
          register_glsl_define_int_choices(*value, define.int_choices);
        }
        else
        {
          RNA_shader_node_glsl_define_value_choices_unregister(value, 1);
        }
      }
      return changed;
    }

    static bool set_glsl_int_socket_default_from_meta(bNodeSocket& socket,
      const int default_value)
    {
      if (socket.type != SOCK_INT || socket.default_value == nullptr)
      {
        return false;
      }
      bNodeSocketValueInt& value = *static_cast<bNodeSocketValueInt*>(socket.default_value);
      if (value.value == default_value)
      {
        return false;
      }
      value.value = default_value;
      return true;
    }

    static bool set_glsl_socket_default_from_meta(bNodeSocket& socket, const float4 default_value)
    {
      if (socket.default_value == nullptr)
      {
        return false;
      }

      switch (socket.type)
      {
        case SOCK_FLOAT: {
          bNodeSocketValueFloat& value = *static_cast<bNodeSocketValueFloat*>(
            socket.default_value);
          if (value.value == default_value.x)
          {
            return false;
          }
          value.value = default_value.x;
          return true;
        }
        case SOCK_BOOLEAN: {
          bNodeSocketValueBoolean& value = *static_cast<bNodeSocketValueBoolean*>(
            socket.default_value);
          const char default_bool = default_value.x != 0.0f;
          if (value.value == default_bool)
          {
            return false;
          }
          value.value = default_bool;
          return true;
        }
        case SOCK_VECTOR: {
          bNodeSocketValueVector& value = *static_cast<bNodeSocketValueVector*>(
            socket.default_value);
          const int dimensions = std::min(value.dimensions, 4);
          bool changed = false;
          for (const int i : IndexRange(dimensions))
          {
            if (value.value[i] != default_value[i])
            {
              value.value[i] = default_value[i];
              changed = true;
            }
          }
          return changed;
        }
        case SOCK_RGBA: {
          bNodeSocketValueRGBA& value = *static_cast<bNodeSocketValueRGBA*>(
            socket.default_value);
          const float4 color_default(default_value.x, default_value.y, default_value.z, 1.0f);
          bool changed = false;
          for (const int i : IndexRange(4))
          {
            if (value.value[i] != color_default[i])
            {
              value.value[i] = color_default[i];
              changed = true;
            }
          }
          return changed;
        }
        default:
          break;
      }

      return false;
    }

    static bool reset_glsl_input_defaults_to_meta(bNode& node, const GLSLParseResult& parse_result)
    {
      bool changed = false;
      for (const GLSLFunctionParam& param : parse_result.function.params)
      {
        if (!glsl_param_has_input_socket(param) || !param.meta.has_default_value ||
            glsl_boundary_type_is_sampler(param.type) || glsl_boundary_type_is_matrix(param.type))
        {
          continue;
        }

        if (bNodeSocket* socket = find_node_input_socket_by_identifier(node, param.identifier))
        {
          if (param.type == GLSLBoundaryType::Int)
          {
            BLI_assert(param.meta.int_default_value.has_value());
            changed |= set_glsl_int_socket_default_from_meta(
              *socket, param.meta.int_default_value.value_or(0));
          }
          else
          {
            changed |= set_glsl_socket_default_from_meta(*socket, param.meta.default_value);
          }
        }

        if (param.type == GLSLBoundaryType::Vec4)
        {
          if (bNodeSocket* w_socket = find_node_input_socket_by_identifier(
                node, make_split_vec4_w_socket_identifier(param.identifier)))
          {
            changed |= set_glsl_socket_default_from_meta(
              *w_socket, float4(param.meta.default_value.w));
          }
        }
      }
      return changed;
    }

    static std::string build_glsl_define_signature_key(const bNode& node,
      const Span<GLSLDefineMeta> defines)
    {
      std::stringstream ss;
      for (const GLSLDefineMeta& define : defines)
      {
        ss << define.name << ':' << define.type << '=' << glsl_define_value_or_default(node, define)
          << ';';
      }
      return ss.str();
    }

    static std::string build_glsl_define_meta_signature_key(
      const Span<GLSLDefineMeta> defines,
      const bool defines_panel_default_closed)
    {
      std::stringstream ss;
      if (!defines.is_empty())
      {
        ss << "defines_panel_closed=" << int(defines_panel_default_closed) << ';';
      }
      for (const GLSLDefineMeta& define : defines)
      {
        ss << define.name << '{';
        ss << "type=" << define.type << ';';
        ss << "default=" << define.default_value << ';';
        if (define.min_value.has_value())
        {
          ss << "min=" << *define.min_value << ';';
        }
        if (define.max_value.has_value())
        {
          ss << "max=" << *define.max_value << ';';
        }
        if (define.label.has_value())
        {
          ss << "label=" << *define.label << ';';
        }
        if (define.description.has_value())
        {
          ss << "description=" << *define.description << ';';
        }
        if (!define.int_choices.is_empty())
        {
          ss << "items=";
          for (const GLSLFunctionParam::IntChoiceItem& item : define.int_choices)
          {
            ss << item.value << ':' << item.label << '|';
          }
          ss << ';';
          ss << "show_label=" << int(define.int_choices_show_label) << ';';
        }
        ss << '}';
      }
      return ss.str();
    }

    static std::string build_glsl_define_block(const bNode& node,
      const Span<GLSLDefineMeta> defines)
    {
      std::stringstream ss;
      for (const GLSLDefineMeta& define : defines)
      {
        const int value = glsl_define_value_or_default(node, define);
        if (define.type == SHD_GLSL_FUNCTION_DEFINE_BOOL)
        {
          if (value != 0)
          {
            ss << "#define " << define.name << " 1\n";
          }
        }
        else
        {
          ss << "#define " << define.name << ' ' << value << "\n";
        }
      }
      return ss.str();
    }

    static bool validate_glsl_define_name_collisions(const Span<GLSLDefineMeta> defines,
      const Span<std::string> function_names,
      const Span<std::string> global_names,
      std::string& r_error)
    {
      Set<std::string> top_level_names;
      for (const std::string& name : function_names)
      {
        top_level_names.add(name);
      }
      for (const std::string& name : global_names)
      {
        top_level_names.add(name);
      }
      for (const GLSLDefineMeta& define : defines)
      {
        if (define.name == "sample2D")
        {
          r_error = "GLSL define name 'sample2D' is reserved by GLSL Function";
          return false;
        }
        if (top_level_names.contains(define.name))
        {
          r_error = "GLSL define '" + define.name +
            "' conflicts with a top-level function or global name";
          return false;
        }
      }
      return true;
    }

    static bool parse_glsl_meta_float_literal(const StringRef text,
      float& r_value,
      std::string& r_error)
    {
      const std::string trimmed = trim_copy(text);
      if (trimmed.empty())
      {
        r_error = "GLSL meta float value cannot be empty";
        return false;
      }

      char* end = nullptr;
      errno = 0;
      const float value = std::strtof(trimmed.c_str(), &end);
      if (end == trimmed.c_str() || trim_copy(end).size() != 0)
      {
        r_error = "Could not parse GLSL meta float value '" + trimmed + "'";
        return false;
      }
      if (errno == ERANGE || !std::isfinite(value))
      {
        r_error = "GLSL meta float value '" + trimmed + "' is outside the finite float range";
        return false;
      }
      r_value = value;
      return true;
    }

    static bool parse_glsl_meta_vector_default(const StringRef text,
      const int dimensions,
      float4& r_value,
      std::string& r_error)
    {
      const std::string trimmed = trim_copy(text);
      const std::string prefix = "vec" + std::to_string(dimensions);
      if (!StringRef(trimmed).startswith(prefix) || !StringRef(trimmed).endswith(")"))
      {
        r_error = "GLSL meta vector defaults must use " + prefix + "(...)";
        return false;
      }

      const StringRef args_text = StringRef(trimmed).substr(prefix.size());
      if (args_text.size() < 2 || args_text[0] != '(' || args_text[args_text.size() - 1] != ')')
      {
        r_error = "Malformed GLSL meta vector constructor";
        return false;
      }

      Vector<std::string> args;
      int paren_depth = 0;
      int64_t arg_start = 1;
      for (int64_t i = 1; i < args_text.size() - 1; i++)
      {
        const char c = args_text[i];
        if (c == '(')
        {
          paren_depth++;
        }
        else if (c == ')')
        {
          paren_depth = std::max(paren_depth - 1, 0);
        }
        else if (c == ',' && paren_depth == 0)
        {
          args.append(trim_copy(args_text.substr(arg_start, i - arg_start)));
          arg_start = i + 1;
        }
      }
      args.append(trim_copy(args_text.substr(arg_start, args_text.size() - 1 - arg_start)));

      if (!(args.size() == 1 || args.size() == dimensions))
      {
        r_error = "GLSL meta vector defaults must provide either one scalar or " +
          std::to_string(dimensions) + " scalars";
        return false;
      }

      r_value = float4(0.0f);
      if (args.size() == 1)
      {
        float scalar = 0.0f;
        if (!parse_glsl_meta_float_literal(args[0], scalar, r_error))
        {
          return false;
        }
        for (const int i : IndexRange(dimensions))
        {
          r_value[i] = scalar;
        }
        return true;
      }

      for (const int i : IndexRange(dimensions))
      {
        if (!parse_glsl_meta_float_literal(args[i], r_value[i], r_error))
        {
          return false;
        }
      }
      return true;
    }

    static bool parse_glsl_meta_subtype(const StringRef text,
      const GLSLBoundaryType type,
      PropertySubType& r_subtype,
      std::string& r_error)
    {
      std::string name = lowercase_copy(trim_copy(text));
      if (StringRef(name).startswith("prop_"))
      {
        name = name.substr(5);
      }

      if (type == GLSLBoundaryType::Float)
      {
        if (name == "none")
        {
          r_subtype = PROP_NONE;
        }
        else if (name == "unsigned")
        {
          r_subtype = PROP_UNSIGNED;
        }
        else if (name == "percentage")
        {
          r_subtype = PROP_PERCENTAGE;
        }
        else if (name == "factor")
        {
          r_subtype = PROP_FACTOR;
        }
        else if (name == "mass")
        {
          r_subtype = PROP_MASS;
        }
        else if (name == "angle")
        {
          r_subtype = PROP_ANGLE;
        }
        else if (name == "time")
        {
          r_subtype = PROP_TIME;
        }
        else if (name == "time_absolute")
        {
          r_subtype = PROP_TIME_ABSOLUTE;
        }
        else if (name == "distance")
        {
          r_subtype = PROP_DISTANCE;
        }
        else if (name == "wavelength")
        {
          r_subtype = PROP_WAVELENGTH;
        }
        else
        {
          r_error = "Unsupported GLSL meta float subtype '" + std::string(text) + "'";
          return false;
        }
        return true;
      }

      if (!ELEM(type, GLSLBoundaryType::Vec2, GLSLBoundaryType::Vec3, GLSLBoundaryType::Vec4))
      {
        r_error = "GLSL meta subtype is only supported for float and vec* inputs";
        return false;
      }

      if (name == "none")
      {
        r_subtype = PROP_NONE;
      }
      else if (name == "factor")
      {
        r_subtype = PROP_FACTOR;
      }
      else if (name == "percentage")
      {
        r_subtype = PROP_PERCENTAGE;
      }
      else if (name == "translation")
      {
        r_subtype = PROP_TRANSLATION;
      }
      else if (name == "direction")
      {
        r_subtype = PROP_DIRECTION;
      }
      else if (name == "velocity")
      {
        r_subtype = PROP_VELOCITY;
      }
      else if (name == "acceleration")
      {
        r_subtype = PROP_ACCELERATION;
      }
      else if (name == "euler")
      {
        r_subtype = PROP_EULER;
      }
      else if (name == "xyz")
      {
        r_subtype = PROP_XYZ;
      }
      else if (name == "color")
      {
        if (!ELEM(type, GLSLBoundaryType::Vec3, GLSLBoundaryType::Vec4))
        {
          r_error = "GLSL meta vector subtype 'color' requires vec3 or vec4";
          return false;
        }
        r_subtype = PROP_COLOR;
      }
      else
      {
        r_error = "Unsupported GLSL meta vector subtype '" + std::string(text) + "'";
        return false;
      }
      return true;
    }

    static bool apply_glsl_meta_to_param(const GLSLRawParamMeta& raw_meta,
      GLSLFunctionParam& r_param,
      std::string& r_error)
    {
      if (!raw_meta.has_any())
      {
        return true;
      }
      if (!glsl_param_has_input_socket(r_param))
      {
        if (glsl_param_has_output_socket(r_param) && raw_meta.has_only_output_supported_meta())
        {
          r_param.meta.label = *raw_meta.label;
          return true;
        }
        r_error = "GLSL meta only supports input parameters, except label on output parameters";
        return false;
      }
      if (glsl_boundary_type_is_sampler(r_param.type))
      {
        if (raw_meta.has_sampler_unsupported_meta())
        {
          r_error =
            "GLSL meta default, min, max, hide_value, and subtype are not supported for "
            "sampler parameters";
          return false;
        }
        if (raw_meta.description.has_value())
        {
          r_param.meta.description = *raw_meta.description;
        }
        if (raw_meta.label.has_value())
        {
          r_param.meta.label = *raw_meta.label;
        }
        if (raw_meta.panel_name.has_value())
        {
          r_param.meta.panel_name = *raw_meta.panel_name;
        }
        return true;
      }

      if (raw_meta.items.has_value())
      {
        if (r_param.type != GLSLBoundaryType::Int)
        {
          r_error = "GLSL meta items is only supported for int inputs";
          return false;
        }
        if (raw_meta.min_value.has_value() || raw_meta.max_value.has_value())
        {
          r_error = "GLSL meta int items cannot be combined with min or max";
          return false;
        }
        if (!parse_glsl_int_choice_items(*raw_meta.items, r_param.meta.int_choices, r_error))
        {
          return false;
        }
      }

      if (raw_meta.show_label.has_value())
      {
        if (!raw_meta.items.has_value())
        {
          r_error = "GLSL meta show_label requires int items";
          return false;
        }
        if (r_param.type != GLSLBoundaryType::Int)
        {
          r_error = "GLSL meta show_label is only supported for int items";
          return false;
        }
        if (!parse_glsl_meta_bool_literal(
              *raw_meta.show_label, r_param.meta.int_choices_show_label, r_error))
        {
          return false;
        }
      }

      if (raw_meta.default_value.has_value())
      {
        bool parsed_literal_default = false;
        if (r_param.type == GLSLBoundaryType::Float)
        {
          float literal_default = 0.0f;
          if (parse_glsl_meta_float_literal(*raw_meta.default_value, literal_default, r_error))
          {
            r_param.meta.default_value.x = literal_default;
            r_param.meta.has_default_value = true;
            parsed_literal_default = true;
          }
        }
        else if (r_param.type == GLSLBoundaryType::Int)
        {
          int literal_default = 0;
          bool out_of_range = false;
          if (parse_glsl_meta_int_literal(
                *raw_meta.default_value, literal_default, r_error, &out_of_range))
          {
            r_param.meta.int_default_value = literal_default;
            r_param.meta.has_default_value = true;
            parsed_literal_default = true;
          }
          else if (out_of_range)
          {
            return false;
          }
        }
        else if (r_param.type == GLSLBoundaryType::Bool)
        {
          bool literal_default = false;
          if (parse_glsl_meta_bool_literal(*raw_meta.default_value, literal_default, r_error))
          {
            r_param.meta.default_value.x = literal_default ? 1.0f : 0.0f;
            r_param.meta.has_default_value = true;
            parsed_literal_default = true;
          }
        }
        else
        {
          float4 literal_default = float4(0.0f);
          if (parse_glsl_meta_vector_default(
            *raw_meta.default_value, r_param.dimensions, literal_default, r_error))
          {
            r_param.meta.default_value = literal_default;
            r_param.meta.has_default_value = true;
            parsed_literal_default = true;
          }
        }
        if (!parsed_literal_default)
        {
          if (raw_meta.items.has_value())
          {
            r_error = "GLSL meta int items require an integer literal default";
            return false;
          }
          r_param.meta.default_expression = trim_copy(*raw_meta.default_value);
          if (r_param.meta.default_expression->empty())
          {
            r_error = "GLSL meta default expression cannot be empty";
            return false;
          }
          /* Expression defaults are runtime values, so the inline socket value must stay hidden
           * and the wrapper injects the expression only when the input socket is not linked. */
          r_param.meta.hide_value = true;
          r_error.clear();
        }
      }

      if (raw_meta.items.has_value())
      {
        if (!r_param.meta.has_default_value || !r_param.meta.int_default_value.has_value())
        {
          r_error = "GLSL meta int items require default=...";
          return false;
        }
        if (!glsl_int_choice_contains_value(r_param.meta.int_choices,
              *r_param.meta.int_default_value))
        {
          r_error = "GLSL meta int default must be one of its items";
          return false;
        }
      }

      if (raw_meta.min_value.has_value())
      {
        if (r_param.type == GLSLBoundaryType::Int)
        {
          int min_value = 0;
          if (!parse_glsl_meta_int_literal(*raw_meta.min_value, min_value, r_error))
          {
            return false;
          }
          r_param.meta.int_min_value = min_value;
        }
        else if (r_param.type == GLSLBoundaryType::Bool)
        {
          r_error = "GLSL meta min is not supported for bool inputs";
          return false;
        }
        else if (!parse_glsl_meta_float_literal(*raw_meta.min_value, r_param.meta.min_value, r_error))
        {
          return false;
        }
        r_param.meta.has_min = true;
      }

      if (raw_meta.max_value.has_value())
      {
        if (r_param.type == GLSLBoundaryType::Int)
        {
          int max_value = 0;
          if (!parse_glsl_meta_int_literal(*raw_meta.max_value, max_value, r_error))
          {
            return false;
          }
          r_param.meta.int_max_value = max_value;
        }
        else if (r_param.type == GLSLBoundaryType::Bool)
        {
          r_error = "GLSL meta max is not supported for bool inputs";
          return false;
        }
        else if (!parse_glsl_meta_float_literal(*raw_meta.max_value, r_param.meta.max_value, r_error))
        {
          return false;
        }
        r_param.meta.has_max = true;
      }

      if (raw_meta.hide_value.has_value())
      {
        if (!parse_glsl_meta_bool_literal(*raw_meta.hide_value, r_param.meta.hide_value, r_error))
        {
          return false;
        }
      }

      if (r_param.meta.has_min && r_param.meta.has_max)
      {
        const bool invalid_range = r_param.type == GLSLBoundaryType::Int ?
          r_param.meta.int_min_value.value_or(0) > r_param.meta.int_max_value.value_or(0) :
          r_param.meta.min_value > r_param.meta.max_value;
        if (invalid_range)
        {
          r_error = "GLSL meta min cannot be greater than max";
          return false;
        }
      }

      if (raw_meta.subtype.has_value())
      {
        PropertySubType subtype = PROP_NONE;
        if (!parse_glsl_meta_subtype(*raw_meta.subtype, r_param.type, subtype, r_error))
        {
          return false;
        }
        r_param.meta.subtype = subtype;
      }

      if (raw_meta.description.has_value())
      {
        r_param.meta.description = *raw_meta.description;
      }

      if (raw_meta.label.has_value())
      {
        r_param.meta.label = *raw_meta.label;
      }

      if (raw_meta.panel_name.has_value())
      {
        r_param.meta.panel_name = *raw_meta.panel_name;
      }

      return true;
    }

    static std::string build_glsl_meta_signature_key(const GLSLFunctionDefinition& function)
    {
      std::stringstream ss;
      for (const GLSLPanelMeta& panel : function.panels)
      {
        ss << "panel{" << panel.name << ";closed=" << int(panel.default_closed) << '}';
      }
      for (const GLSLFunctionParam& param : function.params)
      {
        if (!param.meta.has_any())
        {
          continue;
        }
        ss << param.name << '{';
        if (param.meta.has_default_value)
        {
          ss << "default=";
          if (param.type == GLSLBoundaryType::Int)
          {
            BLI_assert(param.meta.int_default_value.has_value());
            ss << param.meta.int_default_value.value_or(0);
          }
          else
          {
            for (const int i : IndexRange(std::max(param.dimensions, 1)))
            {
              if (i != 0)
              {
                ss << ',';
              }
              ss << param.meta.default_value[i];
            }
          }
          ss << ';';
        }
        if (param.meta.default_expression.has_value())
        {
          ss << "default_expr=" << *param.meta.default_expression << ';';
        }
        if (param.meta.panel_name.has_value())
        {
          ss << "panel=" << *param.meta.panel_name << ';';
        }
        if (param.meta.description.has_value())
        {
          ss << "description=" << *param.meta.description << ';';
        }
        if (param.meta.label.has_value())
        {
          ss << "label=" << *param.meta.label << ';';
        }
        if (param.meta.has_min)
        {
          ss << "min=";
          if (param.type == GLSLBoundaryType::Int)
          {
            BLI_assert(param.meta.int_min_value.has_value());
            ss << param.meta.int_min_value.value_or(0);
          }
          else
          {
            ss << param.meta.min_value;
          }
          ss << ';';
        }
        if (param.meta.has_max)
        {
          ss << "max=";
          if (param.type == GLSLBoundaryType::Int)
          {
            BLI_assert(param.meta.int_max_value.has_value());
            ss << param.meta.int_max_value.value_or(0);
          }
          else
          {
            ss << param.meta.max_value;
          }
          ss << ';';
        }
        if (param.meta.hide_value)
        {
          ss << "hide_value=1;";
        }
        if (param.meta.subtype.has_value())
        {
          ss << "subtype=" << int(*param.meta.subtype) << ';';
        }
        if (!param.meta.int_choices.is_empty())
        {
          ss << "items=";
          for (const GLSLFunctionParam::IntChoiceItem& item : param.meta.int_choices)
          {
            ss << item.value << ':' << item.label << '|';
          }
          ss << ';';
          ss << "show_label=" << int(param.meta.int_choices_show_label) << ';';
        }
        ss << '}';
      }
      return ss.str();
    }

    static bool apply_glsl_meta_to_function(const Map<std::string, GLSLRawParamMeta>& meta_by_key,
      const Map<std::string, Vector<GLSLPanelMeta>>& panels_by_function,
      GLSLFunctionDefinition& r_function,
      int& r_meta_hash,
      std::string& r_error)
    {
      if (const Vector<GLSLPanelMeta>* panels = panels_by_function.lookup_ptr(r_function.name))
      {
        r_function.panels = *panels;
      }

      Set<std::string> param_names;
      for (const GLSLFunctionParam& param : r_function.params)
      {
        param_names.add(param.name);
      }

      for (const auto& item : meta_by_key.items())
      {
        StringRef target_function;
        StringRef target_param;
        split_glsl_meta_key(item.key, target_function, target_param);
        if (!target_function.is_empty() && target_function != r_function.name)
        {
          continue;
        }
        if (!param_names.contains(std::string(target_param)))
        {
          r_error = "GLSL meta parameter '" + std::string(target_param) +
            "' was not found in function '" + r_function.name + "'";
          return false;
        }
      }

      for (GLSLFunctionParam& param : r_function.params)
      {
        if (const GLSLRawParamMeta* function_meta = meta_by_key.lookup_ptr(
          make_glsl_meta_key(r_function.name, param.name)))
        {
          if (!apply_glsl_meta_to_param(*function_meta, param, r_error))
          {
            if (!r_error.empty())
            {
              r_error = "For parameter '" + param.name + "': " + r_error;
            }
            return false;
          }
        }
      }

      const std::string meta_signature = build_glsl_meta_signature_key(r_function);
      r_meta_hash = meta_signature.empty() ? 0 : int(BLI_ghashutil_strhash_p(meta_signature.c_str()));
      return true;
    }

    static bool validate_glsl_closure_socket_text(const StringRef value,
      const StringRef field,
      const StringRef helper_name,
      std::string& r_error)
    {
      constexpr int max_socket_text_bytes = sizeof(((bNodeSocket*)nullptr)->identifier) - 1;
      if (value.size() <= max_socket_text_bytes)
      {
        return true;
      }
      r_error = "GLSL closure " + std::string(field) + " for helper '" +
        std::string(helper_name) + "' exceeds the " + std::to_string(max_socket_text_bytes) +
        " byte socket limit";
      return false;
    }

    static bool glsl_closure_boundary_type_supported(const GLSLBoundaryType type)
    {
      return ELEM(type,
        GLSLBoundaryType::Float,
        GLSLBoundaryType::Int,
        GLSLBoundaryType::Bool,
        GLSLBoundaryType::Vec2,
        GLSLBoundaryType::Vec3,
        GLSLBoundaryType::Vec4);
    }

    /* Scalar GPU node sockets use float transport. Every integer in this inclusive range is
     * represented exactly by that transport. */
    static constexpr int glsl_closure_exact_int_limit = 1 << 24;

    static bool apply_glsl_closure_item_meta(const GLSLRawParamMeta& raw_meta,
      GLSLFunctionParam& r_param,
      const bool is_output,
      std::string& r_error)
    {
      if (raw_meta.items.has_value() || raw_meta.show_label.has_value() ||
          raw_meta.panel_name.has_value())
      {
        r_error = "GLSL closure items, show_label, and nested panels are not supported";
        return false;
      }

      if (!is_output)
      {
        if (raw_meta.default_value.has_value() || raw_meta.min_value.has_value() ||
            raw_meta.max_value.has_value() || raw_meta.hide_value.has_value())
        {
          r_error = "GLSL closure inputs only support label, description, and subtype=color";
          return false;
        }
      }
      else
      {
        if (raw_meta.default_value.has_value() || raw_meta.min_value.has_value() ||
            raw_meta.max_value.has_value() || raw_meta.hide_value.has_value())
        {
          r_error = "GLSL closure outputs only support label, description, and subtype";
          return false;
        }
      }

      if (raw_meta.subtype.has_value())
      {
        PropertySubType subtype = PROP_NONE;
        if (!parse_glsl_meta_subtype(*raw_meta.subtype, r_param.type, subtype, r_error))
        {
          return false;
        }
        if (!is_output && subtype != PROP_COLOR)
        {
          r_error = "GLSL closure inputs only support subtype=color";
          return false;
        }
        r_param.meta.subtype = subtype;
      }
      r_param.meta.label = raw_meta.label;
      r_param.meta.description = raw_meta.description;

      if (r_param.type == GLSLBoundaryType::Vec4 && r_param.meta.subtype.has_value())
      {
        r_error = "GLSL closure vec4 items do not support subtype in v1";
        return false;
      }
      return true;
    }

    static bool apply_glsl_closure_meta_to_function(const GLSLRawClosureMeta& raw_closure,
      GLSLFunctionDefinition& r_function,
      std::string& r_error)
    {
      if (!validate_glsl_closure_socket_text(
            "closure." + r_function.name, "identifier", r_function.name, r_error) ||
          (raw_closure.label.has_value() &&
           !validate_glsl_closure_socket_text(
             *raw_closure.label, "label", r_function.name, r_error)) ||
          (raw_closure.description.has_value() &&
           !validate_glsl_closure_socket_text(
             *raw_closure.description, "description", r_function.name, r_error)))
      {
        return false;
      }

      Set<std::string> item_names;
      if (r_function.return_type != GLSLBoundaryType::Void)
      {
        item_names.add("Result");
      }
      for (const GLSLFunctionParam& param : r_function.params)
      {
        if (!item_names.add(param.name))
        {
          r_error = "GLSL closure helper '" + r_function.name +
            "' has duplicate parameter name '" + param.name + "'";
          return false;
        }
        if (param.name == "Result")
        {
          r_error = "GLSL closure helper '" + r_function.name +
            "' uses reserved parameter name 'Result'";
          return false;
        }
        if (StringRef(param.name).startswith("npr_closure_callback_out_"))
        {
          r_error = "GLSL closure helper '" + r_function.name + "' parameter '" + param.name +
            "' uses the reserved callback wrapper prefix";
          return false;
        }
      }

      for (const auto& item : raw_closure.items.items())
      {
        if (!item_names.contains(item.key))
        {
          r_error = "GLSL closure item '" + item.key + "' was not found in helper '" +
            r_function.name + "'";
          return false;
        }
      }

      if (r_function.return_type != GLSLBoundaryType::Void)
      {
        if (!glsl_closure_boundary_type_supported(r_function.return_type))
        {
          r_error = "GLSL closure helper '" + r_function.name + "' has unsupported return type '" +
            r_function.return_type_name + "'";
          return false;
        }
        GLSLFunctionParam result_param;
        result_param.name = "Result";
        result_param.type = r_function.return_type;
        result_param.type_name = r_function.return_type_name;
        result_param.dimensions = glsl_boundary_dimensions(r_function.return_type);
        result_param.qualifier = GLSLFunctionParam::Qualifier::Out;
        if (const GLSLRawParamMeta* result_meta = raw_closure.items.lookup_ptr("Result"))
        {
          if (!apply_glsl_closure_item_meta(*result_meta, result_param, true, r_error))
          {
            r_error = "For GLSL closure helper '" + r_function.name + "' Result: " + r_error;
            return false;
          }
        }
        r_function.return_meta = std::move(result_param.meta);
        if ((r_function.return_type == GLSLBoundaryType::Vec4 &&
             !validate_glsl_closure_socket_text(
               "Result.__w", "item key", r_function.name, r_error)) ||
            (r_function.return_meta.label.has_value() &&
             !validate_glsl_closure_socket_text(
               *r_function.return_meta.label, "item label", r_function.name, r_error)) ||
            (r_function.return_type == GLSLBoundaryType::Vec4 &&
             r_function.return_meta.label.has_value() &&
             !validate_glsl_closure_socket_text(
               make_split_vec4_w_socket_name(*r_function.return_meta.label),
               "generated item label",
               r_function.name,
               r_error)) ||
            (r_function.return_meta.description.has_value() &&
             !validate_glsl_closure_socket_text(
               *r_function.return_meta.description, "item description", r_function.name, r_error)))
        {
          return false;
        }
      }

      int output_count = r_function.return_type == GLSLBoundaryType::Void ? 0 : 1;
      for (GLSLFunctionParam& param : r_function.params)
      {
        if (param.is_array)
        {
          r_error = "GLSL closure helper '" + r_function.name + "' parameter '" + param.name +
            "' uses an unsupported array type";
          return false;
        }
        if (!glsl_closure_boundary_type_supported(param.type))
        {
          r_error = "GLSL closure helper '" + r_function.name + "' parameter '" + param.name +
            "' has unsupported type '" + param.type_name + "'";
          return false;
        }
        const bool is_output = param.qualifier == GLSLFunctionParam::Qualifier::Out;
        output_count += is_output ? 1 : 0;
        if (const GLSLRawParamMeta* item_meta = raw_closure.items.lookup_ptr(param.name))
        {
          if (!apply_glsl_closure_item_meta(*item_meta, param, is_output, r_error))
          {
            r_error = "For GLSL closure helper '" + r_function.name + "' item '" + param.name +
              "': " + r_error;
            return false;
          }
        }

        const std::string key_w = param.name + ".__w";
        if (!validate_glsl_closure_socket_text(param.name, "item key", r_function.name, r_error) ||
            (param.type == GLSLBoundaryType::Vec4 &&
             !validate_glsl_closure_socket_text(key_w, "item key", r_function.name, r_error)) ||
            (param.meta.label.has_value() &&
             !validate_glsl_closure_socket_text(
               *param.meta.label, "item label", r_function.name, r_error)) ||
            (param.type == GLSLBoundaryType::Vec4 && param.meta.label.has_value() &&
             !validate_glsl_closure_socket_text(make_split_vec4_w_socket_name(*param.meta.label),
                                                "generated item label",
                                                r_function.name,
                                                r_error)) ||
            (param.meta.description.has_value() &&
             !validate_glsl_closure_socket_text(
               *param.meta.description, "item description", r_function.name, r_error)))
        {
          return false;
        }
      }
      if (output_count == 0)
      {
        r_error = "GLSL closure helper '" + r_function.name + "' must have a return value or out parameter";
        return false;
      }
      return true;
    }

    static Vector<GLSLToken> tokenize_glsl_source(const StringRef source)
    {
      Vector<GLSLToken> tokens;
      bool beginning_of_line = true;

      for (int64_t i = 0; i < source.size();)
      {
        const char c = source[i];

        if (c == '\n')
        {
          beginning_of_line = true;
          i++;
          continue;
        }
        if (std::isspace(uchar(c)))
        {
          i++;
          continue;
        }
        if (beginning_of_line && c == '#')
        {
          while (i < source.size() && source[i] != '\n')
          {
            i++;
          }
          continue;
        }

        beginning_of_line = false;

        if (ELEM(c, '\"', '\''))
        {
          const char quote = c;
          i++;
          while (i < source.size())
          {
            if (source[i] == '\\' && (i + 1) < source.size())
            {
              i += 2;
              continue;
            }
            if (source[i++] == quote)
            {
              break;
            }
          }
          continue;
        }

        if (is_identifier_start(c))
        {
          const int64_t start = i;
          i++;
          while (i < source.size() && is_identifier_continue(source[i]))
          {
            i++;
          }
          tokens.append(
            { GLSLToken::Kind::Identifier, std::string(source.substr(start, i - start)), '\0', start, i });
          continue;
        }

        if (strchr("(){}[],;=", c) != nullptr)
        {
          tokens.append({ GLSLToken::Kind::Punctuation, std::string(1, c), c, i, i + 1 });
        }
        i++;
      }

      return tokens;
    }

    static bool validate_glsl_closure_function_like_macros(
      const StringRef source,
      const Map<std::string, GLSLRawClosureMeta>& raw_closures,
      std::string& r_error)
    {
      if (raw_closures.is_empty())
      {
        return true;
      }

      struct MacroDefinition
      {
        std::string name;
        Set<std::string> parameter_names;
        Vector<std::string> replacement_identifiers;
      };
      Vector<MacroDefinition> macro_definitions;

      const auto parse_logical_line = [&](const StringRef line) {
        int64_t i = 0;
        while (i < line.size() && std::isspace(uchar(line[i])))
        {
          i++;
        }
        if (i >= line.size() || line[i] != '#')
        {
          return true;
        }
        i++;
        while (i < line.size() && std::isspace(uchar(line[i])))
        {
          i++;
        }
        const int64_t directive_start = i;
        while (i < line.size() && is_identifier_continue(line[i]))
        {
          i++;
        }
        if (line.substr(directive_start, i - directive_start) != "define")
        {
          return true;
        }
        while (i < line.size() && std::isspace(uchar(line[i])))
        {
          i++;
        }
        if (i >= line.size() || !is_identifier_start(line[i]))
        {
          return true;
        }
        const int64_t macro_name_start = i;
        i++;
        while (i < line.size() && is_identifier_continue(line[i]))
        {
          i++;
        }
        const std::string macro_name(line.substr(macro_name_start, i - macro_name_start));
        Set<std::string> parameter_names;
        if (i < line.size() && line[i] == '(')
        {
          const int64_t parameters_start = i + 1;
          int paren_depth = 1;
          i++;
          while (i < line.size() && paren_depth > 0)
          {
            if (line[i] == '(')
            {
              paren_depth++;
            }
            else if (line[i] == ')')
            {
              paren_depth--;
            }
            i++;
          }
          if (paren_depth != 0)
          {
            return true;
          }
          for (const GLSLToken& token :
               tokenize_glsl_source(line.substr(parameters_start, i - parameters_start - 1)))
          {
            if (token.kind == GLSLToken::Kind::Identifier)
            {
              parameter_names.add(token.text);
            }
          }
        }

        const StringRef replacement = line.substr(i);
        if (replacement.find("##") != StringRef::not_found)
        {
          r_error = "GLSL macro '" + macro_name +
            "' uses token pasting, which is not supported with GLSL closure callbacks";
          return false;
        }

        MacroDefinition definition;
        definition.name = macro_name;
        definition.parameter_names = std::move(parameter_names);
        for (const GLSLToken& token : tokenize_glsl_source(line.substr(i)))
        {
          if (token.kind == GLSLToken::Kind::Identifier &&
              !definition.parameter_names.contains(token.text))
          {
            definition.replacement_identifiers.append(token.text);
          }
        }
        macro_definitions.append(std::move(definition));
        return true;
      };

      std::stringstream stream{std::string(source)};
      std::string physical_line;
      std::string logical_line;
      while (std::getline(stream, physical_line))
      {
        int64_t content_end = physical_line.size();
        if (content_end > 0 && physical_line[content_end - 1] == '\r')
        {
          content_end--;
        }
        const bool continues = content_end > 0 && physical_line[content_end - 1] == '\\';
        const int64_t append_end = continues ? content_end - 1 : physical_line.size();
        logical_line.append(physical_line, 0, append_end);
        if (continues)
        {
          continue;
        }
        if (!parse_logical_line(logical_line))
        {
          return false;
        }
        logical_line.clear();
      }
      if (!logical_line.empty() && !parse_logical_line(logical_line))
      {
        return false;
      }

      const auto find_referenced_helper = [&](const MacroDefinition& root,
                                              std::string& r_helper_name) {
        Set<std::string> visited_macros;
        Vector<const MacroDefinition*> to_visit;
        to_visit.append(&root);
        while (!to_visit.is_empty())
        {
          const MacroDefinition& definition = *to_visit.pop_last();
          if (!visited_macros.add(definition.name))
          {
            continue;
          }
          for (const std::string& identifier : definition.replacement_identifiers)
          {
            if (raw_closures.contains(identifier))
            {
              r_helper_name = identifier;
              return true;
            }
            for (const MacroDefinition& candidate : macro_definitions)
            {
              if (candidate.name == identifier)
              {
                to_visit.append(&candidate);
              }
            }
          }
        }
        return false;
      };

      for (const MacroDefinition& definition : macro_definitions)
      {
        std::string helper_name;
        if (find_referenced_helper(definition, helper_name))
        {
          r_error = "GLSL macro '" + definition.name + "' references GLSL closure helper '" +
            helper_name + "', which is not supported";
          return false;
        }
      }
      return true;
    }

    static bool parse_glsl_preprocessor_directive_name(const StringRef line,
      std::string& r_directive)
    {
      int64_t i = 0;
      while (i < line.size() && std::isspace(uchar(line[i])))
      {
        i++;
      }
      if (i >= line.size() || line[i] != '#')
      {
        return false;
      }
      i++;
      while (i < line.size() && std::isspace(uchar(line[i])))
      {
        i++;
      }
      const int64_t directive_start = i;
      while (i < line.size() && is_identifier_continue(line[i]))
      {
        i++;
      }
      if (i == directive_start)
      {
        return false;
      }
      r_directive = std::string(line.substr(directive_start, i - directive_start));
      return true;
    }

    static bool glsl_preprocessor_directive_starts_conditional(const StringRef directive)
    {
      return directive == "if" || directive == "ifdef" || directive == "ifndef";
    }

    static Vector<GLSLSourceRange> find_top_level_conditional_preprocessor_ranges(
      const StringRef source)
    {
      Vector<GLSLSourceRange> ranges;
      Vector<int64_t> conditional_starts;
      int brace_depth = 0;

      for (int64_t line_start = 0; line_start < source.size();)
      {
        int64_t line_end = line_start;
        while (line_end < source.size() && source[line_end] != '\n')
        {
          line_end++;
        }

        const StringRef line = source.substr(line_start, line_end - line_start);
        std::string directive;
        const bool is_preprocessor_line = parse_glsl_preprocessor_directive_name(line, directive);
        if (is_preprocessor_line)
        {
          if (glsl_preprocessor_directive_starts_conditional(directive))
          {
            if (brace_depth == 0)
            {
              conditional_starts.append(line_start);
            }
          }
          else if (directive == "endif")
          {
            if (brace_depth == 0 && !conditional_starts.is_empty())
            {
              ranges.append({ conditional_starts.pop_last(), line_end });
            }
          }
        }
        else
        {
          for (int64_t i = line_start; i < line_end; i++)
          {
            if (source[i] == '{')
            {
              brace_depth++;
            }
            else if (source[i] == '}')
            {
              brace_depth = max_ii(0, brace_depth - 1);
            }
          }
        }

        line_start = line_end < source.size() ? line_end + 1 : line_end;
      }

      for (const int64_t start : conditional_starts)
      {
        ranges.append({ start, source.size() });
      }
      return ranges;
    }

    static bool glsl_source_position_is_in_ranges(const int64_t position,
      const Span<GLSLSourceRange> ranges)
    {
      for (const GLSLSourceRange& range : ranges)
      {
        if (position >= range.start && position < range.end)
        {
          return true;
        }
      }
      return false;
    }

    static bool validate_no_top_level_conditional_glsl_code(const StringRef source,
      const Vector<GLSLToken>& tokens,
      std::string& r_error)
    {
      const Vector<GLSLSourceRange> conditional_ranges =
        find_top_level_conditional_preprocessor_ranges(source);
      if (conditional_ranges.is_empty())
      {
        return true;
      }

      int brace_depth = 0;
      for (const GLSLToken& token : tokens)
      {
        if (brace_depth == 0 && glsl_source_position_is_in_ranges(token.source_start, conditional_ranges))
        {
          r_error =
            "Top-level GLSL declarations cannot be wrapped in #if/#ifdef blocks; move the macro "
            "branch inside a function body";
          return false;
        }
        if (token.kind != GLSLToken::Kind::Punctuation)
        {
          continue;
        }
        if (token.punctuation == '{')
        {
          brace_depth++;
        }
        else if (token.punctuation == '}')
        {
          brace_depth = max_ii(0, brace_depth - 1);
        }
      }

      return true;
    }

    static bool parse_glsl_parameter_tokens(const Span<GLSLToken> tokens,
      GLSLFunctionParam& r_param,
      std::string& r_error)
    {
      if (tokens.is_empty())
      {
        r_error = "Empty parameter declaration";
        return false;
      }

      Vector<const GLSLToken*> identifiers;
      bool has_out_qualifier = false;
      bool has_inout_qualifier = false;
      bool has_array_declarator = false;
      bool has_unsupported_punctuation = false;
      int array_bracket_depth = 0;

      for (const GLSLToken& token : tokens)
      {
        if (token.kind == GLSLToken::Kind::Punctuation && token.punctuation == '[')
        {
          has_array_declarator = true;
          array_bracket_depth++;
        }
        else if (token.kind == GLSLToken::Kind::Punctuation && token.punctuation == ']')
        {
          has_array_declarator = true;
          array_bracket_depth = std::max(array_bracket_depth - 1, 0);
        }
        else if (token.kind == GLSLToken::Kind::Identifier && array_bracket_depth == 0)
        {
          identifiers.append(&token);
          has_out_qualifier |= token.text == "out";
          has_inout_qualifier |= token.text == "inout";
        }
        else if (token.kind == GLSLToken::Kind::Punctuation)
        {
          has_unsupported_punctuation = true;
        }
      }

      if (has_out_qualifier && has_inout_qualifier)
      {
        r_error = "A parameter cannot be both 'out' and 'inout'";
        return false;
      }
      if (has_unsupported_punctuation)
      {
        r_error = "Unsupported GLSL parameter syntax";
        return false;
      }
      if (identifiers.size() == 1 && identifiers[0]->text == "void")
      {
        r_param = {};
        return true;
      }
      if (identifiers.size() < 2)
      {
        r_error = "Each parameter needs a type and a name";
        return false;
      }

      const GLSLToken& type_token = *identifiers[identifiers.size() - 2];
      const StringRef type_name = type_token.text;
      const StringRef param_name = identifiers.last()->text;
      const GLSLBoundaryType boundary_type = glsl_boundary_type_from_name(type_name);
      if (!ELEM(boundary_type,
        GLSLBoundaryType::Float,
        GLSLBoundaryType::Int,
        GLSLBoundaryType::Bool,
        GLSLBoundaryType::Vec2,
        GLSLBoundaryType::Vec3,
        GLSLBoundaryType::Vec4,
        GLSLBoundaryType::Mat2,
        GLSLBoundaryType::Mat3,
        GLSLBoundaryType::Mat4,
        GLSLBoundaryType::Sample2D,
        GLSLBoundaryType::Sample3D))
      {
        r_error =
          "Supported parameter types are float, int, bool, vec2, vec3, vec4, mat2, mat3, mat4, "
          "sampler2D, and sampler3D";
        return false;
      }

      r_param.type = boundary_type;
      if (has_inout_qualifier)
      {
        r_error = "The 'inout' qualifier is not supported yet";
        return false;
      }
      if (has_out_qualifier && glsl_boundary_type_is_sampler(boundary_type))
      {
        r_error = "sampler parameters only support input qualifiers";
        return false;
      }
      r_param.qualifier = has_out_qualifier ? GLSLFunctionParam::Qualifier::Out :
        GLSLFunctionParam::Qualifier::In;
      r_param.type_name = type_name;
      r_param.name = std::string(param_name);
      r_param.identifier = make_socket_identifier(has_out_qualifier ? "Out" : "In", param_name);
      r_param.dimensions = glsl_boundary_dimensions(boundary_type);
      r_param.is_array = has_array_declarator;
      r_param.type_source_start = type_token.source_start;
      r_param.type_source_end = type_token.source_end;
      return true;
    }

    static bool parse_glsl_function_definition(const Vector<GLSLToken>& tokens,
      const int paren_index,
      const int closing_paren_index,
      GLSLFunctionDefinition& r_function,
      std::string& r_error)
    {
      if (paren_index < 2 || tokens[paren_index].punctuation != '(' ||
        tokens[closing_paren_index].punctuation != ')')
      {
        r_error = "Malformed GLSL function declaration";
        return false;
      }

      const GLSLToken& name_token = tokens[paren_index - 1];
      const GLSLToken& type_token = tokens[paren_index - 2];
      if (name_token.kind != GLSLToken::Kind::Identifier ||
        type_token.kind != GLSLToken::Kind::Identifier)
      {
        r_error = "Could not resolve function name and return type";
        return false;
      }

      r_function.name = name_token.text;
      r_function.return_type_name = type_token.text;
      r_function.return_type = glsl_boundary_type_from_name(type_token.text);
      if (!ELEM(r_function.return_type,
        GLSLBoundaryType::Void,
        GLSLBoundaryType::Float,
        GLSLBoundaryType::Int,
        GLSLBoundaryType::Bool,
        GLSLBoundaryType::Vec2,
        GLSLBoundaryType::Vec3,
        GLSLBoundaryType::Vec4,
        GLSLBoundaryType::Mat2,
        GLSLBoundaryType::Mat3,
        GLSLBoundaryType::Mat4))
      {
        r_error =
          "Supported return types are void, float, int, bool, vec2, vec3, vec4, mat2, mat3, and "
          "mat4";
        return false;
      }

      Vector<GLSLToken> parameter_tokens;
      int parameter_depth = 0;
      for (int i = paren_index + 1; i < closing_paren_index; i++)
      {
        const GLSLToken& token = tokens[i];
        if (token.kind == GLSLToken::Kind::Punctuation && token.punctuation == ',' &&
          parameter_depth == 0)
        {
          GLSLFunctionParam parameter;
          if (!parse_glsl_parameter_tokens(parameter_tokens, parameter, r_error))
          {
            return false;
          }
          if (parameter.type != GLSLBoundaryType::Unsupported)
          {
            r_function.params.append(parameter);
          }
          parameter_tokens.clear();
          continue;
        }
        if (token.kind == GLSLToken::Kind::Punctuation)
        {
          if (token.punctuation == '(')
          {
            parameter_depth++;
          }
          else if (token.punctuation == ')')
          {
            parameter_depth--;
          }
        }
        parameter_tokens.append(token);
      }

      if (!parameter_tokens.is_empty())
      {
        GLSLFunctionParam parameter;
        if (!parse_glsl_parameter_tokens(parameter_tokens, parameter, r_error))
        {
          return false;
        }
        if (parameter.type != GLSLBoundaryType::Unsupported)
        {
          r_function.params.append(parameter);
        }
      }

      if (r_function.return_type == GLSLBoundaryType::Void && glsl_function_output_count(r_function) == 0)
      {
        r_error = "The selected function does not expose any node outputs";
        return false;
      }

      const int opening_brace_index = closing_paren_index + 1;
      if (opening_brace_index >= tokens.size() || tokens[opening_brace_index].kind != GLSLToken::Kind::Punctuation ||
        tokens[opening_brace_index].punctuation != '{')
      {
        r_error = "Could not resolve the GLSL function body";
        return false;
      }

      int brace_depth = 1;
      int closing_brace_index = -1;
      for (int i = opening_brace_index + 1; i < tokens.size(); i++)
      {
        const GLSLToken& token = tokens[i];
        if (token.kind != GLSLToken::Kind::Punctuation)
        {
          continue;
        }
        if (token.punctuation == '{')
        {
          brace_depth++;
        }
        else if (token.punctuation == '}')
        {
          brace_depth--;
          if (brace_depth == 0)
          {
            closing_brace_index = i;
            break;
          }
        }
      }
      if (closing_brace_index == -1)
      {
        r_error = "Could not resolve the GLSL function body";
        return false;
      }
      r_function.body_token_start = opening_brace_index + 1;
      r_function.body_token_end = closing_brace_index - 1;
      r_function.body_source_start = tokens[opening_brace_index].source_end;
      r_function.body_source_end = tokens[closing_brace_index].source_start;

      return true;
    }

    static Vector<std::string> find_top_level_glsl_function_names(const Vector<GLSLToken>& tokens)
    {
      Vector<std::string> names;
      int brace_depth = 0;

      brace_depth = 0;
      for (int i = 0; i < tokens.size(); i++)
      {
        const GLSLToken& token = tokens[i];
        if (token.kind == GLSLToken::Kind::Punctuation)
        {
          if (token.punctuation == '{')
          {
            brace_depth++;
          }
          else if (token.punctuation == '}')
          {
            brace_depth = max_ii(0, brace_depth - 1);
          }
          else if (token.punctuation == '(' && brace_depth == 0 && i >= 2 &&
            tokens[i - 1].kind == GLSLToken::Kind::Identifier &&
            tokens[i - 2].kind == GLSLToken::Kind::Identifier)
          {
            int paren_depth = 1;
            int closing_paren_index = -1;
            for (int j = i + 1; j < tokens.size(); j++)
            {
              if (tokens[j].kind == GLSLToken::Kind::Punctuation)
              {
                if (tokens[j].punctuation == '(')
                {
                  paren_depth++;
                }
                else if (tokens[j].punctuation == ')')
                {
                  paren_depth--;
                  if (paren_depth == 0)
                  {
                    closing_paren_index = j;
                    break;
                  }
                }
              }
            }
            if (closing_paren_index == -1)
            {
              continue;
            }
            const int next_index = closing_paren_index + 1;
            if (next_index < tokens.size() && tokens[next_index].kind == GLSLToken::Kind::Punctuation &&
              tokens[next_index].punctuation == '{')
            {
              names.append(tokens[i - 1].text);
            }
          }
        }
      }
      return names;
    }

    static bool validate_top_level_glsl_declarations(const Vector<GLSLToken>& tokens,
      std::string& r_error)
    {
      int brace_depth = 0;
      Vector<std::string> statement_identifiers;

      auto reset_statement = [&]() { statement_identifiers.clear(); };
      auto validate_statement = [&]() -> bool
        {
          if (statement_identifiers.is_empty())
          {
            return true;
          }

          const StringRef first_identifier = statement_identifiers.first();
          if (first_identifier == "precision")
          {
            r_error = "Top-level precision declarations are not supported";
            return false;
          }
          if (first_identifier == "uniform")
          {
            r_error = "Top-level uniform declarations are not supported; expose them as function parameters";
            return false;
          }
          if (first_identifier == "layout")
          {
            r_error = "Top-level layout-qualified declarations are not supported";
            return false;
          }
          if (ELEM(first_identifier, "in", "out", "attribute", "varying", "buffer"))
          {
            r_error = "Top-level shader interface declarations are not supported";
            return false;
          }
          if (ELEM(first_identifier, "flat", "smooth", "noperspective", "centroid", "sample") &&
            (statement_identifiers.contains("in") || statement_identifiers.contains("out")))
          {
            r_error = "Top-level shader interface declarations are not supported";
            return false;
          }
          return true;
        };

      auto flush_statement = [&]() -> bool
        {
          if (!validate_statement())
          {
            return false;
          }
          reset_statement();
          return true;
        };

      for (int i = 0; i < tokens.size(); i++)
      {
        const GLSLToken& token = tokens[i];
        if (token.kind == GLSLToken::Kind::Punctuation)
        {
          if (token.punctuation == '{')
          {
            brace_depth++;
            reset_statement();
          }
          else if (token.punctuation == '}')
          {
            brace_depth = max_ii(0, brace_depth - 1);
          }
          else if (token.punctuation == ';' && brace_depth == 0)
          {
            if (!flush_statement())
            {
              return false;
            }
          }
          continue;
        }

        if (brace_depth == 0 && token.kind == GLSLToken::Kind::Identifier)
        {
          if (i >= 2 && tokens[i - 1].kind == GLSLToken::Kind::Identifier &&
            tokens[i].kind == GLSLToken::Kind::Identifier && (i + 1) < tokens.size() &&
            tokens[i + 1].kind == GLSLToken::Kind::Punctuation && tokens[i + 1].punctuation == '(' &&
            !statement_identifiers.is_empty())
          {
            if (!flush_statement())
            {
              return false;
            }
          }
          statement_identifiers.append(token.text);
        }
      }

      if (brace_depth == 0 && !validate_statement())
      {
        return false;
      }

      return true;
    }

    static Vector<std::string> find_top_level_glsl_global_names(
      const Vector<GLSLToken>& tokens, const Span<std::string> function_names)
    {
      Set<std::string> function_name_set;
      for (const std::string& function_name : function_names)
      {
        function_name_set.add(function_name);
      }

      Set<std::string> global_name_set;
      Vector<std::string> global_names;
      int brace_depth = 0;
      int paren_depth = 0;
      int bracket_depth = 0;
      std::string last_identifier;
      bool last_identifier_is_function_name = false;

      auto flush_identifier = [&]()
        {
          if (last_identifier.empty())
          {
            return;
          }
          if (!last_identifier_is_function_name && !function_name_set.contains(last_identifier) &&
            global_name_set.add(last_identifier))
          {
            global_names.append(last_identifier);
          }
          last_identifier.clear();
          last_identifier_is_function_name = false;
        };

      for (int i = 0; i < tokens.size(); i++)
      {
        const GLSLToken& token = tokens[i];
        if (token.kind == GLSLToken::Kind::Punctuation)
        {
          switch (token.punctuation)
          {
          case '{':
            brace_depth++;
            flush_identifier();
            break;
          case '}':
            brace_depth = max_ii(0, brace_depth - 1);
            flush_identifier();
            break;
          case '(':
            if (brace_depth == 0 && paren_depth == 0 && bracket_depth == 0 && !last_identifier.empty())
            {
              last_identifier_is_function_name = true;
            }
            paren_depth++;
            break;
          case ')':
            paren_depth = max_ii(0, paren_depth - 1);
            break;
          case '[':
            bracket_depth++;
            break;
          case ']':
            bracket_depth = max_ii(0, bracket_depth - 1);
            break;
          case ',':
          case '=':
          case ';':
            if (brace_depth == 0 && paren_depth == 0 && bracket_depth == 0)
            {
              flush_identifier();
            }
            break;
          default:
            break;
          }
          continue;
        }

        if (brace_depth == 0 && paren_depth == 0 && bracket_depth == 0 &&
          token.kind == GLSLToken::Kind::Identifier)
        {
          last_identifier = token.text;
          last_identifier_is_function_name = false;
        }
      }

      return global_names;
    }

    static bool is_glsl_geometry_access_identifier(const StringRef identifier)
    {
      return ELEM(identifier,
        "glsl_position",
        "glsl_normal",
        "glsl_true_normal",
        "glsl_incoming");
    }

    static bool glsl_expression_uses_identifier(const StringRef expression,
      bool (*predicate)(const StringRef identifier))
    {
      const Vector<GLSLToken> tokens = tokenize_glsl_source(expression);
      for (const GLSLToken& token : tokens)
      {
        if (token.kind == GLSLToken::Kind::Identifier && predicate(StringRef(token.text)))
        {
          return true;
        }
      }
      return false;
    }

    static bool glsl_source_uses_geometry_access(const Vector<GLSLToken>& tokens)
    {
      for (const GLSLToken& token : tokens)
      {
        if (token.kind == GLSLToken::Kind::Identifier &&
            is_glsl_geometry_access_identifier(token.text))
        {
          return true;
        }
      }
      return false;
    }

    static bool is_glsl_lightprobe_access_identifier(const StringRef identifier)
    {
      return identifier == "glsl_ambient_lighting";
    }

    static bool glsl_source_uses_lightprobe_access(const Vector<GLSLToken>& tokens)
    {
      for (const GLSLToken& token : tokens)
      {
        if (token.kind == GLSLToken::Kind::Identifier &&
            is_glsl_lightprobe_access_identifier(token.text))
        {
          return true;
        }
      }
      return false;
    }

    static bool is_glsl_light_access_identifier(const StringRef identifier)
    {
      return identifier == "GLSLLight" || identifier == "glsl_light_count" ||
        identifier == "glsl_light_get" || identifier == "glsl_light_shadow";
    }

    static bool is_glsl_light_access_deprecated_identifier(const StringRef identifier)
    {
      return identifier == "GLSL_LIGHT_FOREACH_BEGIN" || identifier == "GLSL_LIGHT_FOREACH_END" ||
        identifier == "glsl_light_color" || identifier == "glsl_light_vector" ||
        identifier == "glsl_light_distance" || identifier == "glsl_light_diffuse_power" ||
        identifier == "glsl_light_specular_power" || identifier == "glsl_light_surface_attenuation" ||
        identifier == "glsl_light_shadow_visibility" ||
        identifier == "glsl_light_diffuse_attenuation" ||
        identifier == "glsl_light_specular_attenuation";
    }

    static bool glsl_source_uses_eevee_light_access(const Vector<GLSLToken>& tokens)
    {
      for (const GLSLToken& token : tokens)
      {
        if (token.kind == GLSLToken::Kind::Identifier &&
          is_glsl_light_access_identifier(token.text))
        {
          return true;
        }
      }
      return false;
    }

    static bool glsl_function_meta_uses_geometry_access(const GLSLFunctionDefinition& function)
    {
      for (const GLSLFunctionParam& param : function.params)
      {
        if (param.meta.default_expression.has_value() &&
          glsl_expression_uses_identifier(*param.meta.default_expression,
            is_glsl_geometry_access_identifier))
        {
          return true;
        }
      }
      return false;
    }

    static bool glsl_function_meta_uses_lightprobe_access(const GLSLFunctionDefinition& function)
    {
      for (const GLSLFunctionParam& param : function.params)
      {
        if (param.meta.default_expression.has_value() &&
          glsl_expression_uses_identifier(*param.meta.default_expression,
            is_glsl_lightprobe_access_identifier))
        {
          return true;
        }
      }
      return false;
    }

    static bool glsl_function_meta_uses_eevee_light_access(const GLSLFunctionDefinition& function)
    {
      for (const GLSLFunctionParam& param : function.params)
      {
        if (param.meta.default_expression.has_value() &&
          glsl_expression_uses_identifier(*param.meta.default_expression,
            is_glsl_light_access_identifier))
        {
          return true;
        }
      }
      return false;
    }

    static bool glsl_source_uses_deprecated_eevee_light_access(const Vector<GLSLToken>& tokens,
      std::string& r_identifier)
    {
      for (const GLSLToken& token : tokens)
      {
        if (token.kind == GLSLToken::Kind::Identifier &&
          is_glsl_light_access_deprecated_identifier(token.text))
        {
          r_identifier = token.text;
          return true;
        }
      }
      return false;
    }

    static bool glsl_function_meta_uses_deprecated_eevee_light_access(
      const GLSLFunctionDefinition& function, std::string& r_identifier)
    {
      for (const GLSLFunctionParam& param : function.params)
      {
        if (!param.meta.default_expression.has_value())
        {
          continue;
        }
        const Vector<GLSLToken> tokens = tokenize_glsl_source(*param.meta.default_expression);
        for (const GLSLToken& token : tokens)
        {
          if (token.kind == GLSLToken::Kind::Identifier &&
            is_glsl_light_access_deprecated_identifier(token.text))
          {
            r_identifier = token.text;
            return true;
          }
        }
      }
      return false;
    }

    static bool find_glsl_function_definition(const Vector<GLSLToken>& tokens,
      const StringRef function_name,
      GLSLFunctionDefinition& r_function,
      std::string& r_error)
    {
      int brace_depth = 0;
      bool found_first_function = false;

      for (int i = 0; i < tokens.size(); i++)
      {
        const GLSLToken& token = tokens[i];

        if (token.kind == GLSLToken::Kind::Punctuation && token.punctuation == '{')
        {
          brace_depth++;
          continue;
        }
        if (token.kind == GLSLToken::Kind::Punctuation && token.punctuation == '}')
        {
          brace_depth = max_ii(0, brace_depth - 1);
          continue;
        }

        if (brace_depth != 0 || token.kind != GLSLToken::Kind::Punctuation || token.punctuation != '(' ||
          i < 2 || tokens[i - 1].kind != GLSLToken::Kind::Identifier ||
          tokens[i - 2].kind != GLSLToken::Kind::Identifier)
        {
          continue;
        }

        int paren_depth = 1;
        int closing_paren_index = -1;
        for (int j = i + 1; j < tokens.size(); j++)
        {
          if (tokens[j].kind == GLSLToken::Kind::Punctuation)
          {
            if (tokens[j].punctuation == '(')
            {
              paren_depth++;
            }
            else if (tokens[j].punctuation == ')')
            {
              paren_depth--;
              if (paren_depth == 0)
              {
                closing_paren_index = j;
                break;
              }
            }
          }
        }

        if (closing_paren_index == -1 || (closing_paren_index + 1) >= tokens.size() ||
          tokens[closing_paren_index + 1].kind != GLSLToken::Kind::Punctuation ||
          tokens[closing_paren_index + 1].punctuation != '{')
        {
          continue;
        }

        const StringRef candidate_name = tokens[i - 1].text;
        if (!function_name.is_empty() && candidate_name != function_name)
        {
          found_first_function = true;
          continue;
        }

        if (!parse_glsl_function_definition(tokens, i, closing_paren_index, r_function, r_error))
        {
          return false;
        }
        return true;
      }

      if (function_name.is_empty())
      {
        r_error = found_first_function ? "Could not parse the first GLSL function definition" :
          "No GLSL function definition was found";
      }
      else
      {
        r_error = "The selected function was not found in the source";
      }
      return false;
    }

    static bool collect_reachable_glsl_functions(const Vector<GLSLToken>& tokens,
      const Span<GLSLFunctionDefinition> functions,
      const GLSLFunctionDefinition& function,
      Set<std::string>& r_reachable,
      Set<std::string>& r_visiting,
      std::string& r_error)
    {
      if (r_visiting.contains(function.name))
      {
        r_error = "Recursive GLSL closure call graph detected at helper '" + function.name + "'";
        return false;
      }
      if (!r_reachable.add(function.name))
      {
        return true;
      }

      r_visiting.add(function.name);
      BLI_SCOPED_DEFER([&]() { r_visiting.remove(function.name); });
      for (int i = function.body_token_start; i <= function.body_token_end; i++)
      {
        if (tokens[i].kind != GLSLToken::Kind::Identifier || (i + 1) > function.body_token_end ||
            tokens[i + 1].kind != GLSLToken::Kind::Punctuation ||
            tokens[i + 1].punctuation != '(')
        {
          continue;
        }
        const GLSLFunctionDefinition* callee = find_glsl_function_definition_by_name(
          functions, tokens[i].text);
        if (callee == nullptr)
        {
          continue;
        }
        if (r_visiting.contains(callee->name))
        {
          r_error = "Recursive GLSL closure call graph detected between helper '" + function.name +
            "' and '" + callee->name + "'";
          return false;
        }
        if (!collect_reachable_glsl_functions(
              tokens, functions, *callee, r_reachable, r_visiting, r_error))
        {
          return false;
        }
      }
      return true;
    }

    static bool build_glsl_closure_callbacks(const Vector<GLSLToken>& tokens,
      const Span<std::string> function_names,
      const Map<std::string, GLSLRawClosureMeta>& raw_closures,
      const GLSLFunctionDefinition& entry_function,
      Vector<GLSLClosureCallback>& r_callbacks,
      std::string& r_error)
    {
      r_callbacks.clear();
      if (raw_closures.is_empty())
      {
        return true;
      }

      Map<std::string, int> definition_counts;
      for (const std::string& function_name : function_names)
      {
        definition_counts.add_or_modify(
          function_name, [](int* count) { *count = 1; }, [](int* count) { (*count)++; });
      }
      for (const std::string& function_name : function_names)
      {
        if (definition_counts.lookup(function_name) > 1)
        {
          r_error = "GLSL function '" + function_name +
            "' is overloaded or defined more than once";
          return false;
        }
      }
      for (const auto& item : raw_closures.items())
      {
        const int definition_count = definition_counts.lookup_default(item.key, 0);
        if (definition_count == 0)
        {
          r_error = "GLSL closure helper '" + item.key + "' was not found";
          return false;
        }
      }
      if (raw_closures.contains(entry_function.name))
      {
        r_error = "The exported GLSL function '" + entry_function.name +
          "' cannot be annotated as a closure helper";
        return false;
      }

      Vector<GLSLFunctionDefinition> functions;
      if (!find_all_top_level_glsl_function_definitions(
            tokens, function_names, functions, r_error))
      {
        return false;
      }

      Set<std::string> reachable;
      Set<std::string> visiting;
      const GLSLFunctionDefinition* parsed_entry = find_glsl_function_definition_by_name(
        functions, entry_function.name);
      if (parsed_entry == nullptr ||
          !collect_reachable_glsl_functions(
            tokens, functions, *parsed_entry, reachable, visiting, r_error))
      {
        if (r_error.empty())
        {
          r_error = "Could not build the GLSL closure call graph";
        }
        return false;
      }

      for (GLSLFunctionDefinition function : functions)
      {
        const GLSLRawClosureMeta* raw_closure = raw_closures.lookup_ptr(function.name);
        if (raw_closure == nullptr || !reachable.contains(function.name))
        {
          continue;
        }
        if (!apply_glsl_closure_meta_to_function(*raw_closure, function, r_error))
        {
          return false;
        }
        r_callbacks.append(
          {std::move(function), raw_closure->label, raw_closure->description});
      }
      return true;
    }

    static std::string build_function_signature_key(const GLSLFunctionDefinition& function)
    {
      std::stringstream ss;
      ss << function.return_type_name << ' ' << function.name << '(';
      for (const int index : function.params.index_range())
      {
        const GLSLFunctionParam& param = function.params[index];
        if (index != 0)
        {
          ss << ", ";
        }
        switch (param.qualifier)
        {
        case GLSLFunctionParam::Qualifier::In:
          ss << "in ";
          break;
        case GLSLFunctionParam::Qualifier::Out:
          ss << "out ";
          break;
        case GLSLFunctionParam::Qualifier::InOut:
          ss << "inout ";
          break;
        }
        ss << param.type_name << ' ' << param.name;
      }
      ss << ')';
      return ss.str();
    }

    static std::string build_glsl_closure_signature_key(
      const Span<GLSLClosureCallback> callbacks)
    {
      std::stringstream ss;
      for (const GLSLClosureCallback& callback : callbacks)
      {
        ss << "\nclosure{" << build_function_signature_key(callback.function) << '}';
      }
      return ss.str();
    }

    static bool load_glsl_source(const bNode& node, std::string& r_source, std::string& r_error)
    {
      const NodeShaderGLSLFunction& storage = node_storage(node);

      if (storage.source_mode == SHD_GLSL_FUNCTION_SOURCE_INTERNAL)
      {
        Text* text = reinterpret_cast<Text*>(node.id);
        if (text == nullptr)
        {
          r_error = "Choose a Text datablock to use as the GLSL source";
          return false;
        }
        size_t buffer_len = 0;
        char* buffer = txt_to_buf(text, &buffer_len);
        BLI_SCOPED_DEFER([&]() { MEM_delete(buffer); });
        r_source.assign(buffer, buffer_len);
        return true;
      }

      if (storage.filepath[0] == '\0')
      {
        r_error = "Choose a GLSL file path";
        return false;
      }

      char absolute_path[FILE_MAX];
      STRNCPY(absolute_path, storage.filepath);
      if (!BLI_path_is_abs_from_cwd(absolute_path) && node.runtime->owner_tree != nullptr)
      {
        BLI_path_abs(absolute_path, ID_BLEND_PATH_FROM_GLOBAL(&node.runtime->owner_tree->id));
      }

      size_t buffer_len = 0;
      char* buffer = BLI_file_read_text_as_mem(absolute_path, 0, &buffer_len);
      if (buffer == nullptr)
      {
        if (storage.packed_source != nullptr)
        {
          r_source = storage.packed_source;
          return true;
        }

        r_error = "Could not open the GLSL file";
        return false;
      }
      BLI_SCOPED_DEFER([&]() { MEM_delete(buffer); });
      r_source.assign(buffer, buffer_len);
      return true;
    }

    static const bNodeSocket* find_closure_output_socket_by_name(const bNode& node, const StringRef name)
    {
      if (!node.is_type("NodeClosureOutput"_ustr))
      {
        return nullptr;
      }
      const auto& storage = *static_cast<const NodeClosureOutput*>(node.storage);
      for (const int i : IndexRange(storage.output_items.items_num))
      {
        const NodeClosureOutputItem& item = storage.output_items.items[i];
        if (item.name != nullptr && name == item.name)
        {
          return &node.input_socket(i);
        }
      }
      return nullptr;
    }

    static bool closure_output_has_required_sampler_signature(const bNode& closure_output_node,
      const GLSLBoundaryType sampler_type,
      std::string& r_error)
    {
      const auto& storage = *static_cast<const NodeClosureOutput*>(closure_output_node.storage);

      bool has_uv = false;
      for (const int i : IndexRange(storage.input_items.items_num))
      {
        const NodeClosureInputItem& item = storage.input_items.items[i];
        if (item.name != nullptr && STREQ(item.name, "UV") && item.socket_type == SOCK_VECTOR)
        {
          has_uv = true;
          break;
        }
      }
      if (!has_uv)
      {
        r_error = "Closure Output must expose a Vector input item named 'UV' for " +
          std::string(sampler_type_name(sampler_type).c_str());
        return false;
      }

      const bNodeSocket* color_socket = find_closure_output_socket_by_name(closure_output_node, "Color");
      if (color_socket == nullptr || !ELEM(color_socket->type, SOCK_FLOAT, SOCK_VECTOR, SOCK_RGBA))
      {
        r_error = "Closure Output must expose a Float, Vector, or RGBA output item named 'Color' for " +
          std::string(sampler_type_name(sampler_type).c_str());
        return false;
      }
      for (const int i : IndexRange(storage.output_items.items_num))
      {
        const NodeClosureOutputItem& item = storage.output_items.items[i];
        if (item.name != nullptr && STREQ(item.name, "Alpha") && item.socket_type != SOCK_FLOAT)
        {
          r_error = "Closure Output item named 'Alpha' must be a Float for " +
            std::string(sampler_type_name(sampler_type).c_str());
          return false;
        }
      }
      return true;
    }

    static void mark_node_upstream_for_closure_helper(const bNode& node, Set<const bNode*>& r_visited_nodes);

    static void mark_socket_upstream_for_closure_helper(const bNodeSocket& socket,
      Set<const bNode*>& r_visited_nodes)
    {
      if (socket.type == SOCK_CLOSURE)
      {
        /* A nested closure callback zone is compiled by its own helper with its own
         * callback frame. Marking it here would compile the nested Closure Input under
         * the outer frame, which has no bindings for it. */
        return;
      }
      for (const bNodeLink* link : socket.directly_linked_links())
      {
        if (!link->is_used() || link->fromnode == nullptr)
        {
          continue;
        }
        mark_node_upstream_for_closure_helper(*link->fromnode, r_visited_nodes);
      }
    }

    static void mark_node_upstream_for_closure_helper(const bNode& node, Set<const bNode*>& r_visited_nodes)
    {
      if (!r_visited_nodes.add(&node))
      {
        return;
      }
      const_cast<bNode&>(node).runtime->need_exec = 1;
      for (const bNodeSocket* input_socket : node.input_sockets())
      {
        mark_socket_upstream_for_closure_helper(*input_socket, r_visited_nodes);
      }
    }

    static bool build_closure_sample_helper(GPUMaterial* mat,
      const bNode& node,
      const GLSLFunctionParam& param,
      const StringRef source_filename,
      const StringRef helper_name,
      const StringRef uv_global_name,
      GLSLClosureSampleHelper& r_helper,
      std::string& r_error)
    {
      const bNode* logical_node = node.runtime->original ? node.runtime->original : &node;
      const bNode* eval_node = &node;

      const std::string helper_key = std::to_string(reinterpret_cast<uintptr_t>(eval_node)) + ":" +
        param.name;
      if (!active_closure_helper_keys.add(helper_key))
      {
        r_error = "Recursive nested Closure Output " +
          std::string(sampler_type_name(param.type).c_str()) +
          " helper dependency detected on node '" + std::string(logical_node->name) +
          "' parameter '" + param.name + "'";
        return false;
      }
      active_closure_helper_nodes.append(logical_node);
      const auto release_active_helper = [&]()
        {
          active_closure_helper_keys.remove(helper_key);
          active_closure_helper_nodes.pop_last();
        };
      bool active_helper_released = false;
      BLI_SCOPED_DEFER([&]()
        {
          if (!active_helper_released)
          {
            release_active_helper();
          }
        });

      const bNodeLink* used_link = nullptr;
      if (resolve_sampler_source_kind(*eval_node, param, used_link, logical_node) !=
        GLSLSamplerSourceKind::ClosureOutput ||
        used_link == nullptr || used_link->fromnode == nullptr)
      {
        r_error = std::string(sampler_type_name(param.type).c_str()) + " parameter '" +
          param.name + "' is missing a Closure Output source";
        return false;
      }

      const bNode* closure_output_node = used_link->fromnode;

      std::string closure_error;
      if (!closure_output_has_required_sampler_signature(
            *closure_output_node, param.type, closure_error))
      {
        r_error = "Closure Output connected to '" + param.name + "' is missing required closure items: " +
          closure_error;
        return false;
      }
      const bNodeSocket* color_socket = find_closure_output_socket_by_name(*closure_output_node, "Color");
      const bNodeLink* color_source_link = find_any_direct_link(*color_socket);
      const bNodeSocket* sample_socket = color_source_link != nullptr ? color_source_link->fromsock :
                                                                        color_socket;
      const bNodeSocket* alpha_socket = find_closure_output_socket_by_name(*closure_output_node, "Alpha");
      const bNodeLink* alpha_source_link = alpha_socket != nullptr ? find_any_direct_link(*alpha_socket) :
                                                                    nullptr;
      const bNodeSocket* alpha_sample_socket = alpha_source_link != nullptr ?
        alpha_source_link->fromsock :
        alpha_socket;

      bNodeExecContext context = {};
      bNodeTree* helper_tree = const_cast<bNodeTree*>(&closure_output_node->owner_tree());

      /* Snapshot need_exec BEFORE building the helper exec tree (see the typed callback helper
       * for why restoring an all-1 snapshot corrupts the calling compile). */
      Vector<int> previous_need_exec;
      for (bNode& tree_node : helper_tree->nodes)
      {
        previous_need_exec.append(tree_node.runtime->need_exec);
      }
      BLI_SCOPED_DEFER([&]()
        {
          int node_index = 0;
          for (bNode& tree_node : helper_tree->nodes)
          {
            tree_node.runtime->need_exec = previous_need_exec[node_index++];
          }
        });

      bNodeTreeExec* exec = ntreeShaderBeginExecTree_internal(&context, helper_tree, bke::NODE_INSTANCE_KEY_BASE);
      if (exec == nullptr)
      {
        r_error = "Could not build a helper shader tree for " +
          std::string(sampler_type_name(param.type).c_str()) + " parameter '" + param.name + "'";
        return false;
      }
      BLI_SCOPED_DEFER([&]() { ntreeShaderEndExecTree_internal(exec); });

      for (bNode& tree_node : helper_tree->nodes)
      {
        tree_node.runtime->need_exec = 0;
      }
      Set<const bNode*> visited_nodes;
      if (color_source_link != nullptr && color_source_link->fromnode != nullptr)
      {
        mark_node_upstream_for_closure_helper(*color_source_link->fromnode, visited_nodes);
      }
      else
      {
        mark_socket_upstream_for_closure_helper(*color_socket, visited_nodes);
      }
      if (alpha_socket != nullptr)
      {
        if (alpha_source_link != nullptr && alpha_source_link->fromnode != nullptr)
        {
          mark_node_upstream_for_closure_helper(*alpha_source_link->fromnode, visited_nodes);
        }
        else
        {
          mark_socket_upstream_for_closure_helper(*alpha_socket, visited_nodes);
        }
      }

      const GPUType coordinate_type = glsl_boundary_type_is_sample3d(param.type) ? GPU_VEC3 :
                                                                                GPU_VEC2;
      GPU_material_closure_uv_source_push(mat, StringRefNull(uv_global_name), coordinate_type);
      BLI_SCOPED_DEFER([&]() { GPU_material_closure_uv_source_pop(mat); });

      ntreeExecGPUNodes(exec, mat, nullptr);

      bNodeStack* color_stack = node_get_socket_stack(exec->stack, const_cast<bNodeSocket*>(sample_socket));
      GPUNodeLink* color_link = (color_stack != nullptr) ? static_cast<GPUNodeLink*>(color_stack->data) :
        nullptr;
      if (color_link == nullptr && color_source_link != nullptr)
      {
        r_error = "Closure Output Color source node '" +
          std::string(color_source_link->fromnode->name) + "' could not compile for " +
          std::string(sampler_type_name(param.type).c_str()) + " parameter '" + param.name + "'";
        return false;
      }
      if (color_link == nullptr && color_stack != nullptr)
      {
        color_link = GPU_uniform(color_stack->vec);
      }
      if (color_link == nullptr)
      {
        r_error = "Could not evaluate Closure Output Color for " +
          std::string(sampler_type_name(param.type).c_str()) + " parameter '" + param.name + "'";
        return false;
      }

      GPUNodeLink* alpha_link = nullptr;
      if (alpha_sample_socket != nullptr)
      {
        bNodeStack* alpha_stack = node_get_socket_stack(
          exec->stack, const_cast<bNodeSocket*>(alpha_sample_socket));
        alpha_link = (alpha_stack != nullptr) ? static_cast<GPUNodeLink*>(alpha_stack->data) : nullptr;
        if (alpha_link == nullptr && alpha_source_link != nullptr)
        {
          r_error = "Closure Output Alpha source node '" +
            std::string(alpha_source_link->fromnode->name) + "' could not compile for " +
            std::string(sampler_type_name(param.type).c_str()) + " parameter '" + param.name + "'";
          return false;
        }
        if (alpha_link == nullptr && alpha_stack != nullptr)
        {
          alpha_link = GPU_uniform(alpha_stack->vec);
        }
        if (alpha_link == nullptr)
        {
          r_error = "Could not evaluate Closure Output Alpha for " +
            std::string(sampler_type_name(param.type).c_str()) + " parameter '" + param.name + "'";
          return false;
        }
      }

      GPUType helper_return_type = GPU_VEC4;
      if (color_link != nullptr)
      {
        const GPUType output_type = gpu_node_link_output_type(*color_link);
        if (output_type == GPU_FLOAT)
        {
          helper_return_type = GPU_FLOAT;
        }
        else if (ELEM(output_type, GPU_VEC2, GPU_VEC3))
        {
          helper_return_type = GPU_VEC3;
        }
        else if (output_type == GPU_NONE)
        {
          if (sample_socket->type == SOCK_FLOAT)
          {
            helper_return_type = GPU_FLOAT;
          }
          else if (sample_socket->type == SOCK_VECTOR)
          {
            helper_return_type = GPU_VEC3;
          }
        }
      }
      else if (sample_socket->type == SOCK_FLOAT)
      {
        helper_return_type = GPU_FLOAT;
      }
      else if (sample_socket->type == SOCK_VECTOR)
      {
        helper_return_type = GPU_VEC3;
      }

      if (alpha_link != nullptr)
      {
        GPUNodeLink* rgba_link = nullptr;
        if (!GPU_link(mat, "multiply_rgba_alpha", color_link, alpha_link, &rgba_link))
        {
          r_error = "Could not combine Closure Output Color and Alpha for " +
            std::string(sampler_type_name(param.type).c_str()) + " parameter '" + param.name + "'";
          return false;
        }
        color_link = rgba_link;
        helper_return_type = GPU_VEC4;
      }

      release_active_helper();
      active_helper_released = true;

      const char* sub_function_name = GPU_material_split_sub_function(
        mat, helper_return_type, &color_link, StringRefNull(source_filename));
      r_helper.param_name = param.name;
      r_helper.helper_name = helper_name;
      r_helper.uv_global_name = uv_global_name;
      r_helper.sub_function_name = sub_function_name;
      r_helper.coordinate_type = coordinate_type;
      r_helper.return_type = helper_return_type;
      return true;
    }

    static GPUType glsl_closure_callback_item_gpu_type(const ClosureSignature::Item& item)
    {
      if (item.type == nullptr)
      {
        return GPU_NONE;
      }
      switch (item.type->type)
      {
      case SOCK_FLOAT:
      case SOCK_INT:
      case SOCK_BOOLEAN:
        return GPU_FLOAT;
      case SOCK_VECTOR:
        return GPU_VEC3;
      case SOCK_RGBA:
        return GPU_VEC4;
      default:
        return GPU_NONE;
      }
    }

    static bool glsl_closure_callback_signatures_have_same_abi(
      const ClosureSignature& expected,
      const ClosureSignature& actual,
      const StringRef helper_name,
      std::string& r_error)
    {
      const auto items_have_same_abi = [](const ClosureSignature::Item& expected_item,
                                          const ClosureSignature::Item& actual_item) {
        if (expected_item.has_same_abi(actual_item))
        {
          return true;
        }
        if (expected_item.key != actual_item.key ||
            expected_item.structure_type != actual_item.structure_type ||
            expected_item.type == nullptr || actual_item.type == nullptr)
        {
          return false;
        }
        const bool expected_is_vector3 = expected_item.type->type == SOCK_VECTOR &&
          expected_item.dimensions == 3;
        const bool actual_is_vector3 = actual_item.type->type == SOCK_VECTOR &&
          actual_item.dimensions == 3;
        return (expected_item.type->type == SOCK_RGBA && actual_is_vector3) ||
          (actual_item.type->type == SOCK_RGBA && expected_is_vector3);
      };

      auto compare_items = [&](const auto& expected_items,
                               const auto& actual_items,
                               const StringRef direction) {
        if (expected_items.size() != actual_items.size())
        {
          r_error = "GLSL closure callback '" + std::string(helper_name) + "' expects " +
            std::to_string(expected_items.size()) + " " + std::string(direction) +
            " items, but the connected Closure provides " +
            std::to_string(actual_items.size());
          return false;
        }
        for (const int i : expected_items.index_range())
        {
          if (!items_have_same_abi(expected_items[i], actual_items[i]))
          {
            r_error = "GLSL closure callback '" + std::string(helper_name) + "' " +
              std::string(direction) + " item '" + expected_items[i].key +
              "' does not match the connected Closure ABI";
            return false;
          }
        }
        return true;
      };

      return compare_items(expected.inputs, actual.inputs, "input") &&
        compare_items(expected.outputs, actual.outputs, "output");
    }

    static bool build_glsl_closure_callback_helper(GPUMaterial* mat,
      const bNode& node,
      const GLSLClosureCallback& callback,
      const StringRef source_filename,
      GLSLClosureCallbackHelper& r_helper,
      std::string& r_error)
    {
      const std::string socket_identifier = "closure." + callback.function.name;
      const bNodeSocket* callback_socket = find_node_input_socket_by_identifier(node,
        socket_identifier);
      const bNodeLink* callback_link = callback_socket != nullptr ?
        find_used_direct_link(*callback_socket) :
        nullptr;
      if (callback_link == nullptr || callback_link->fromnode == nullptr ||
          !callback_link->fromnode->is_type("NodeClosureOutput"_ustr))
      {
        r_error = "GLSL closure callback '" + callback.function.name +
          "' is connected but has no localized Closure Output source";
        return false;
      }

      const bNode* logical_node = node.runtime->original ? node.runtime->original : &node;
      const std::string helper_key = "callback:" +
        std::to_string(reinterpret_cast<uintptr_t>(&node)) + ":" + callback.function.name;
      if (!active_closure_helper_keys.add(helper_key))
      {
        r_error = "Recursive GLSL closure callback dependency detected for helper '" +
          callback.function.name + "'";
        return false;
      }
      active_closure_helper_nodes.append(logical_node);
      const auto release_active_helper = [&]() {
        active_closure_helper_keys.remove(helper_key);
        active_closure_helper_nodes.pop_last();
      };
      bool active_helper_released = false;
      BLI_SCOPED_DEFER([&]() {
        if (!active_helper_released)
        {
          release_active_helper();
        }
      });

      const bNode& closure_output_node = *callback_link->fromnode;
      const ClosureSignature expected_signature = build_glsl_closure_signature(callback);
      const ClosureSignature actual_signature = ClosureSignature::from_closure_output_node(
        closure_output_node, false);
      if (!glsl_closure_callback_signatures_have_same_abi(
            expected_signature, actual_signature, callback.function.name, r_error))
      {
        return false;
      }

      Vector<GPUType> input_types;
      Vector<GPUMaterialClosureCallbackInput> callback_inputs;
      input_types.reserve(actual_signature.inputs.size());
      callback_inputs.reserve(actual_signature.inputs.size());
      for (const ClosureSignature::Item& item : actual_signature.inputs)
      {
        const GPUType gpu_type = glsl_closure_callback_item_gpu_type(item);
        if (gpu_type == GPU_NONE)
        {
          r_error = "GLSL closure callback '" + callback.function.name + "' input item '" +
            item.key + "' has no GPU transport type";
          return false;
        }
        const int function_input_index = input_types.size();
        input_types.append(gpu_type);
        callback_inputs.append({closure_output_node.identifier,
                                StringRef(item.key),
                                gpu_type,
                                function_input_index});
      }

      struct OutputBinding
      {
        const ClosureSignature::Item* item = nullptr;
        const bNodeSocket* closure_socket = nullptr;
        const bNodeLink* source_link = nullptr;
        const bNodeSocket* sample_socket = nullptr;
        GPUType type = GPU_NONE;
        GPUNodeLink* link = nullptr;
      };
      Vector<OutputBinding> output_bindings;
      output_bindings.reserve(actual_signature.outputs.size());
      for (const ClosureSignature::Item& item : actual_signature.outputs)
      {
        const bNodeSocket* closure_socket = find_closure_output_socket_by_name(
          closure_output_node, item.key);
        if (closure_socket == nullptr)
        {
          r_error = "GLSL closure callback '" + callback.function.name + "' output item '" +
            item.key + "' was not found on the localized Closure Output";
          return false;
        }
        const GPUType gpu_type = glsl_closure_callback_item_gpu_type(item);
        if (gpu_type == GPU_NONE)
        {
          r_error = "GLSL closure callback '" + callback.function.name + "' output item '" +
            item.key + "' has no GPU transport type";
          return false;
        }
        const bNodeLink* source_link = find_any_direct_link(*closure_socket);
        output_bindings.append({&item,
                                closure_socket,
                                source_link,
                                source_link != nullptr ? source_link->fromsock : closure_socket,
                                gpu_type,
                                nullptr});
      }

      bNodeExecContext context = {};
      bNodeTree* helper_tree = const_cast<bNodeTree*>(&closure_output_node.owner_tree());

      /* Snapshot need_exec BEFORE building the helper exec tree: ntreeShaderBeginExecTree_internal
       * resets every node's need_exec to 1, so a snapshot taken after it would restore all-1s and
       * make the calling compile (e.g. an outer callback helper) recompile every remaining node,
       * including nested closure zones under the wrong frame. */
      Vector<int> previous_need_exec;
      for (bNode& tree_node : helper_tree->nodes)
      {
        previous_need_exec.append(tree_node.runtime->need_exec);
      }
      BLI_SCOPED_DEFER([&]() {
        int node_index = 0;
        for (bNode& tree_node : helper_tree->nodes)
        {
          tree_node.runtime->need_exec = previous_need_exec[node_index++];
        }
      });

      bNodeTreeExec* exec = ntreeShaderBeginExecTree_internal(
        &context, helper_tree, bke::NODE_INSTANCE_KEY_BASE);
      if (exec == nullptr)
      {
        r_error = "Could not build the localized shader tree for GLSL closure callback '" +
          callback.function.name + "'";
        return false;
      }
      BLI_SCOPED_DEFER([&]() { ntreeShaderEndExecTree_internal(exec); });

      for (bNode& tree_node : helper_tree->nodes)
      {
        tree_node.runtime->need_exec = 0;
      }

      Set<const bNode*> visited_nodes;
      for (const OutputBinding& binding : output_bindings)
      {
        if (binding.source_link != nullptr && binding.source_link->fromnode != nullptr)
        {
          mark_node_upstream_for_closure_helper(*binding.source_link->fromnode, visited_nodes);
        }
        else
        {
          mark_socket_upstream_for_closure_helper(*binding.closure_socket, visited_nodes);
        }
      }

      GPU_material_closure_callback_input_frame_push(mat, callback_inputs);
      BLI_SCOPED_DEFER([&]() { GPU_material_closure_callback_input_frame_pop(mat); });
      ntreeExecGPUNodes(exec, mat, nullptr);

      std::string callback_gpu_error;
      if (GPU_material_closure_callback_input_frame_error_get(mat, callback_gpu_error))
      {
        r_error = "GLSL closure callback '" + callback.function.name +
          "' GPU compilation failed: " + callback_gpu_error;
        return false;
      }

      for (OutputBinding& binding : output_bindings)
      {
        bNodeStack* stack = node_get_socket_stack(
          exec->stack, const_cast<bNodeSocket*>(binding.sample_socket));
        binding.link = stack != nullptr ? static_cast<GPUNodeLink*>(stack->data) : nullptr;
        if (binding.link == nullptr && binding.source_link != nullptr)
        {
          r_error = "GLSL closure callback '" + callback.function.name + "' output item '" +
            binding.item->key + "' could not compile from node '" +
            std::string(binding.source_link->fromnode->name) + "'";
          return false;
        }
        if (binding.link == nullptr && stack != nullptr)
        {
          binding.link = GPU_constant(stack->vec);
        }
        if (binding.link == nullptr)
        {
          r_error = "Could not evaluate output item '" + binding.item->key +
            "' for GLSL closure callback '" + callback.function.name + "'";
          return false;
        }
      }

      Vector<GPUMaterialFunctionOutput> function_outputs;
      function_outputs.reserve(output_bindings.size());
      for (OutputBinding& binding : output_bindings)
      {
        function_outputs.append({binding.type, &binding.link});
      }

      release_active_helper();
      active_helper_released = true;
      const char* sub_function_name = GPU_material_split_sub_function_multi(
        mat, input_types, function_outputs, StringRefNull(source_filename));
      if (sub_function_name == nullptr)
      {
        r_error = "Could not create the multi-output GPU function for GLSL closure callback '" +
          callback.function.name + "'";
        return false;
      }

      r_helper.function_name = callback.function.name;
      r_helper.sub_function_name = sub_function_name;
      r_helper.input_types = std::move(input_types);
      r_helper.output_types.reserve(output_bindings.size());
      for (const OutputBinding& binding : output_bindings)
      {
        r_helper.output_types.append(binding.type);
      }
      return true;
    }

    static std::string trim_source_range(const StringRef source, const int64_t start, const int64_t end)
    {
      if (end <= start)
      {
        return "";
      }
      return trim_copy(source.substr(start, end - start));
    }

    static const GLSLClosureSampleHelper* find_closure_sample_helper(
      const Span<GLSLClosureSampleHelper> helpers, const StringRef param_name)
    {
      for (const GLSLClosureSampleHelper& helper : helpers)
      {
        if (param_name == helper.param_name)
        {
          return &helper;
        }
      }
      return nullptr;
    }

    static GPUType gpu_node_link_output_type(const GPUNodeLink& link)
    {
      return (link.link_type == GPU_NODE_LINK_OUTPUT && link.output != nullptr) ? link.output->type :
        GPU_NONE;
    }

    static std::string make_closure_texel_fetch_expression(const GLSLClosureSampleHelper& helper,
      const StringRef coord_expression)
    {
      std::stringstream ss;
      ss << helper.helper_name << "((vec2(" << coord_expression << ") + vec2(0.5)) / vec2(" <<
        float(closure_output_virtual_texture_size) << ", " <<
        float(closure_output_virtual_texture_size) << "))";
      return ss.str();
    }

    static std::string make_closure_gather_expression(const GLSLClosureSampleHelper& helper,
      const StringRef uv_expression,
      const StringRef component_expression)
    {
      const std::string texel = "vec2(1.0 / " + std::to_string(closure_output_virtual_texture_size) +
        ".0, 1.0 / " + std::to_string(closure_output_virtual_texture_size) +
        ".0)";
      const std::string component = component_expression.is_empty() ? "0" :
        std::string(component_expression);

      std::stringstream ss;
      ss << "vec4(";
      ss << helper.helper_name << "(" << uv_expression << " + vec2(-0.5, 0.5) * " << texel << ")["
        << component << "], ";
      ss << helper.helper_name << "(" << uv_expression << " + vec2(0.5, 0.5) * " << texel << ")["
        << component << "], ";
      ss << helper.helper_name << "(" << uv_expression << " + vec2(0.5, -0.5) * " << texel << ")["
        << component << "], ";
      ss << helper.helper_name << "(" << uv_expression << " + vec2(-0.5, -0.5) * " << texel << ")["
        << component << "])";
      return ss.str();
    }

    static bool rewrite_glsl_source_for_closure_samplers(
      const StringRef source,
      const Vector<GLSLToken>& tokens,
      const Span<GLSLFunctionDefinition> functions,
      const Span<GLSLClosureFunctionBindings> bindings_by_function,
      const Span<GLSLClosureSampleHelper> closure_helpers,
      std::string& r_rewritten_source,
      GLSLClosureSamplerDowngradeInfo& r_downgrade_info,
      std::string& r_error)
    {
      struct Replacement
      {
        int64_t start = 0;
        int64_t end = 0;
        std::string text;
      };

      Vector<Replacement> replacements;

      for (const GLSLFunctionDefinition& function : functions)
      {
        const GLSLClosureFunctionBindings* seed_bindings = find_glsl_closure_function_bindings(
          bindings_by_function, function.name);
        if (seed_bindings == nullptr || seed_bindings->bindings.is_empty())
        {
          continue;
        }

        for (const GLSLFunctionParam& param : function.params)
        {
          if (!glsl_boundary_type_is_sampler(param.type) || param.type_source_start < 0 ||
            param.type_source_end <= param.type_source_start || !seed_bindings->bindings.contains(param.name))
          {
            continue;
          }
          replacements.append({ param.type_source_start, param.type_source_end, "float" });
        }

        Map<std::string, std::string> local_bindings;
        if (!build_function_local_closure_bindings(
          tokens, function, seed_bindings->bindings, local_bindings, r_error))
        {
          return false;
        }

        for (int i = function.body_token_start; i <= function.body_token_end; i++)
        {
          const GLSLToken& token = tokens[i];
          if (token.kind != GLSLToken::Kind::Identifier ||
            !is_sampler_sampling_function_name(token.text) ||
            (i + 1) > function.body_token_end || tokens[i + 1].kind != GLSLToken::Kind::Punctuation ||
            tokens[i + 1].punctuation != '(')
          {
            continue;
          }

          Vector<GLSLTokenRange> argument_ranges;
          int closing_paren_index = -1;
          if (!parse_call_argument_ranges(
            tokens, i + 1, function.body_token_end, argument_ranges, closing_paren_index) ||
            argument_ranges.is_empty())
          {
            continue;
          }

          std::string sampled_identifier;
          int sampled_identifier_token_index = -1;
          if (!parse_single_identifier_token_range(tokens,
            argument_ranges[0].start,
            argument_ranges[0].end,
            sampled_identifier,
            sampled_identifier_token_index))
          {
            continue;
          }
          if (sampled_identifier_token_index < 0)
          {
            continue;
          }

          const std::string* root_param_name = local_bindings.lookup_ptr(sampled_identifier);
          if (root_param_name == nullptr)
          {
            continue;
          }

          const GLSLClosureSampleHelper* helper = find_closure_sample_helper(closure_helpers,
            *root_param_name);
          if (helper == nullptr)
          {
            r_error = "Could not resolve a Closure Output helper for sampler parameter '" +
              *root_param_name + "'";
            return false;
          }

          r_downgrade_info.root_param_names.add(*root_param_name);
          std::string replacement_text;

          if (helper->coordinate_type == GPU_VEC3)
          {
            if (token.text != "texture" || argument_ranges.size() != 2)
            {
              r_error = "Closure Output driven sampler3D parameter '" + *root_param_name +
                "' only supports texture(sampler3D, vec3); '" + token.text +
                "' is not supported";
              return false;
            }
            const std::string coord_expression = trim_source_range(source,
              tokens[argument_ranges[1].start].source_start,
              tokens[argument_ranges[1].end].source_end);
            replacement_text = helper->helper_name + "(" + coord_expression + ")";
            replacements.append(
              { token.source_start, tokens[closing_paren_index].source_end, replacement_text });
            continue;
          }

          if (token.text == "texture")
          {
            if (argument_ranges.size() < 2)
            {
              r_error = "sampler2D parameter '" + *root_param_name +
                "' could not rewrite texture(...) from Closure Output";
              return false;
            }
            const std::string uv_expression = trim_source_range(source,
              tokens[argument_ranges[1].start].source_start,
              tokens[argument_ranges[1].end].source_end);
            replacement_text = helper->helper_name + "(" + uv_expression + ")";
            r_downgrade_info.uses_texture_bias |= argument_ranges.size() > 2;
          }
          else if (token.text == "textureLod")
          {
            if (argument_ranges.size() < 3)
            {
              r_error = "sampler2D parameter '" + *root_param_name +
                "' could not rewrite textureLod(...) from Closure Output";
              return false;
            }
            const std::string uv_expression = trim_source_range(source,
              tokens[argument_ranges[1].start].source_start,
              tokens[argument_ranges[1].end].source_end);
            replacement_text = helper->helper_name + "(" + uv_expression + ")";
            r_downgrade_info.uses_texture_lod = true;
          }
          else if (token.text == "textureGrad")
          {
            if (argument_ranges.size() < 4)
            {
              r_error = "sampler2D parameter '" + *root_param_name +
                "' could not rewrite textureGrad(...) from Closure Output";
              return false;
            }
            const std::string uv_expression = trim_source_range(source,
              tokens[argument_ranges[1].start].source_start,
              tokens[argument_ranges[1].end].source_end);
            replacement_text = helper->helper_name + "(" + uv_expression + ")";
            r_downgrade_info.uses_texture_grad = true;
          }
          else if (token.text == "textureSize")
          {
            replacement_text = "ivec2(" + std::to_string(closure_output_virtual_texture_size) + ", " +
              std::to_string(closure_output_virtual_texture_size) + ")";
            r_downgrade_info.uses_texture_size = true;
          }
          else if (token.text == "texelFetch")
          {
            if (argument_ranges.size() < 3)
            {
              r_error = "sampler2D parameter '" + *root_param_name +
                "' could not rewrite texelFetch(...) from Closure Output";
              return false;
            }
            const std::string coord_expression = trim_source_range(source,
              tokens[argument_ranges[1].start].source_start,
              tokens[argument_ranges[1].end].source_end);
            replacement_text = make_closure_texel_fetch_expression(*helper, coord_expression);
            r_downgrade_info.uses_texel_fetch = true;
          }
          else if (token.text == "textureGather")
          {
            if (argument_ranges.size() < 2)
            {
              r_error = "sampler2D parameter '" + *root_param_name +
                "' could not rewrite textureGather(...) from Closure Output";
              return false;
            }
            const std::string uv_expression = trim_source_range(source,
              tokens[argument_ranges[1].start].source_start,
              tokens[argument_ranges[1].end].source_end);
            const std::string component_expression =
              argument_ranges.size() >= 3 ?
              trim_source_range(source,
                tokens[argument_ranges[2].start].source_start,
                tokens[argument_ranges[2].end].source_end) :
              "";
            replacement_text = make_closure_gather_expression(*helper, uv_expression, component_expression);
            r_downgrade_info.uses_texture_gather = true;
          }

          replacements.append({ token.source_start, tokens[closing_paren_index].source_end, replacement_text });
        }
      }

      r_rewritten_source = std::string(source);
      for (int i = replacements.size() - 1; i >= 0; i--)
      {
        const Replacement& replacement = replacements[i];
        r_rewritten_source.replace(
          replacement.start, replacement.end - replacement.start, replacement.text);
      }
      return true;
    }

    static void log_closure_sampler_downgrades(GPUMaterial* mat,
      const bNode& node,
      const GLSLParseResult& parse_result,
      const GLSLClosureSamplerDowngradeInfo& info)
    {
      if (!info.uses_texture_bias && !info.uses_texture_lod && !info.uses_texture_grad &&
        !info.uses_texture_size && !info.uses_texel_fetch && !info.uses_texture_gather)
      {
        return;
      }

      std::stringstream ss;
      ss << "GLSL Function material '" << glsl_function_material_name(mat) << "' node '"
         << node.name << "' function '" << parse_result.function.name
         << "': Closure Output sampler2D downgraded ";
      bool need_separator = false;
      auto append_item = [&](const StringRef text)
        {
          if (need_separator)
          {
            ss << ", ";
          }
          ss << text;
          need_separator = true;
        };

      if (info.uses_texture_bias)
      {
        append_item("texture(..., bias)->texture");
      }
      if (info.uses_texture_lod)
      {
        append_item("textureLod->texture");
      }
      if (info.uses_texture_grad)
      {
        append_item("textureGrad->texture");
      }
      if (info.uses_texture_size)
      {
        append_item("textureSize->ivec2(1024, 1024)");
      }
      if (info.uses_texel_fetch)
      {
        append_item("texelFetch->texture using virtual 1024x1024 size");
      }
      if (info.uses_texture_gather)
      {
        append_item("textureGather->approximate 4-tap texture using virtual 1024x1024 size");
      }
      if (!info.root_param_names.is_empty())
      {
        ss << " for parameters ";
        bool need_param_separator = false;
        for (const std::string& param_name : info.root_param_names)
        {
          if (need_param_separator)
          {
            ss << ", ";
          }
          ss << "'" << param_name << "'";
          need_param_separator = true;
        }
      }

      CLOG_WARN(&LOG, "%s", ss.str().c_str());
    }

    static const GLSLClosureCallbackHelper* find_glsl_closure_callback_helper(
      const Span<GLSLClosureCallbackHelper> helpers, const StringRef function_name)
    {
      for (const GLSLClosureCallbackHelper& helper : helpers)
      {
        if (helper.function_name == function_name)
        {
          return &helper;
        }
      }
      return nullptr;
    }

    static const GLSLClosureCallback* find_glsl_closure_callback(
      const Span<GLSLClosureCallback> callbacks, const StringRef function_name)
    {
      for (const GLSLClosureCallback& callback : callbacks)
      {
        if (callback.function.name == function_name)
        {
          return &callback;
        }
      }
      return nullptr;
    }

    static const char* glsl_closure_callback_gpu_type_name(const GPUType type)
    {
      switch (type)
      {
      case GPU_FLOAT:
        return "float";
      case GPU_VEC3:
        return "vec3";
      case GPU_VEC4:
        return "vec4";
      default:
        BLI_assert_unreachable();
        return "float";
      }
    }

    static std::string build_glsl_closure_callback_helper_block(
      const Span<GLSLClosureCallbackHelper> helpers)
    {
      if (helpers.is_empty())
      {
        return "";
      }

      std::stringstream ss;
      for (const GLSLClosureCallbackHelper& helper : helpers)
      {
        ss << "void " << helper.sub_function_name << "(";
        bool needs_separator = false;
        for (const int i : helper.input_types.index_range())
        {
          if (needs_separator)
          {
            ss << ", ";
          }
          ss << glsl_closure_callback_gpu_type_name(helper.input_types[i]) << " in" << i;
          needs_separator = true;
        }
        for (const int i : helper.output_types.index_range())
        {
          if (needs_separator)
          {
            ss << ", ";
          }
          ss << "out " << glsl_closure_callback_gpu_type_name(helper.output_types[i]) << " out"
             << i;
          needs_separator = true;
        }
        ss << ");\n";
      }
      ss << "\n";
      return ss.str();
    }

    static void append_glsl_closure_callback_input_expressions(
      const GLSLFunctionParam& param,
      const Span<GPUType> input_types,
      int& r_input_index,
      Vector<std::string>& r_expressions)
    {
      switch (param.type)
      {
      case GLSLBoundaryType::Int:
      {
        BLI_assert(input_types[r_input_index] == GPU_FLOAT);
        r_input_index++;
        r_expressions.append("float(clamp(" + param.name + ", -16777216, 16777216))");
        break;
      }
      case GLSLBoundaryType::Bool:
      {
        BLI_assert(input_types[r_input_index] == GPU_FLOAT);
        r_input_index++;
        r_expressions.append("(" + param.name + " ? 1.0 : 0.0)");
        break;
      }
      case GLSLBoundaryType::Vec2:
      {
        BLI_assert(input_types[r_input_index] == GPU_VEC3);
        r_input_index++;
        r_expressions.append("vec3(" + param.name + ", 0.0)");
        break;
      }
      case GLSLBoundaryType::Vec3:
      {
        const GPUType transport_type = input_types[r_input_index++];
        BLI_assert(ELEM(transport_type, GPU_VEC3, GPU_VEC4));
        r_expressions.append(transport_type == GPU_VEC4 ?
                               "vec4(" + param.name + ", 1.0)" : param.name);
        break;
      }
      case GLSLBoundaryType::Vec4:
      {
        BLI_assert(input_types[r_input_index] == GPU_VEC3);
        BLI_assert(input_types[r_input_index + 1] == GPU_FLOAT);
        r_input_index += 2;
        r_expressions.append(param.name + ".xyz");
        r_expressions.append(param.name + ".w");
        break;
      }
      case GLSLBoundaryType::Float:
      default:
      {
        BLI_assert(input_types[r_input_index] == GPU_FLOAT);
        r_input_index++;
        r_expressions.append(param.name);
        break;
      }
      }
    }

    static std::string glsl_closure_callback_output_expression(
      const GLSLFunctionParam& param,
      const Span<GPUType> output_types,
      int& r_output_index)
    {
      const int value_index = r_output_index++;
      const GPUType transport_type = output_types[value_index];
      const std::string value_name = "npr_closure_callback_out_" +
        std::to_string(value_index);
      switch (param.type)
      {
      case GLSLBoundaryType::Int:
        BLI_assert(transport_type == GPU_FLOAT);
        return "int(round(clamp(" + value_name + ", -16777216.0, 16777216.0)))";
      case GLSLBoundaryType::Bool:
        BLI_assert(transport_type == GPU_FLOAT);
        return "(" + value_name + " != 0.0)";
      case GLSLBoundaryType::Vec2:
        BLI_assert(transport_type == GPU_VEC3);
        return value_name + ".xy";
      case GLSLBoundaryType::Vec3:
        BLI_assert(ELEM(transport_type, GPU_VEC3, GPU_VEC4));
        return transport_type == GPU_VEC4 ? value_name + ".rgb" : value_name;
      case GLSLBoundaryType::Vec4:
      {
        BLI_assert(transport_type == GPU_VEC3);
        BLI_assert(output_types[r_output_index] == GPU_FLOAT);
        const std::string w_name = "npr_closure_callback_out_" +
          std::to_string(r_output_index++);
        return "vec4(" + value_name + ".xyz, " + w_name + ")";
      }
      case GLSLBoundaryType::Float:
      default:
        BLI_assert(transport_type == GPU_FLOAT);
        return value_name;
      }
    }

    static std::string build_glsl_closure_callback_body(
      const GLSLClosureCallback& callback, const GLSLClosureCallbackHelper& helper)
    {
      std::stringstream ss;
      for (const int i : helper.output_types.index_range())
      {
        ss << "\n  " << glsl_closure_callback_gpu_type_name(helper.output_types[i])
           << " npr_closure_callback_out_" << i << ";";
      }

      Vector<std::string> call_arguments;
      int input_index = 0;
      for (const GLSLFunctionParam& param : callback.function.params)
      {
        if (param.qualifier != GLSLFunctionParam::Qualifier::Out)
        {
          append_glsl_closure_callback_input_expressions(
            param, helper.input_types, input_index, call_arguments);
        }
      }
      BLI_assert(input_index == helper.input_types.size());
      for (const int i : helper.output_types.index_range())
      {
        call_arguments.append("npr_closure_callback_out_" + std::to_string(i));
      }

      ss << "\n  " << helper.sub_function_name << "(";
      for (const int i : call_arguments.index_range())
      {
        if (i != 0)
        {
          ss << ", ";
        }
        ss << call_arguments[i];
      }
      ss << ");";

      int output_index = 0;
      std::optional<std::string> return_expression;
      if (callback.function.return_type != GLSLBoundaryType::Void)
      {
        GLSLFunctionParam result;
        result.name = "Result";
        result.type = callback.function.return_type;
        result.type_name = callback.function.return_type_name;
        result.dimensions = glsl_boundary_dimensions(result.type);
        result.qualifier = GLSLFunctionParam::Qualifier::Out;
        result.meta = callback.function.return_meta;
        return_expression = glsl_closure_callback_output_expression(
          result, helper.output_types, output_index);
      }
      for (const GLSLFunctionParam& param : callback.function.params)
      {
        if (param.qualifier == GLSLFunctionParam::Qualifier::Out)
        {
          ss << "\n  " << param.name << " = " <<
            glsl_closure_callback_output_expression(param, helper.output_types, output_index) <<
            ";";
        }
      }
      BLI_assert(output_index == helper.output_types.size());
      if (return_expression.has_value())
      {
        ss << "\n  return " << *return_expression << ";";
      }
      ss << "\n";
      return ss.str();
    }

    static bool rewrite_glsl_source_for_closure_callbacks(
      const StringRef source,
      const Span<std::string> function_names,
      const Span<GLSLClosureCallback> callbacks,
      const Span<GLSLClosureCallbackHelper> helpers,
      std::string& r_rewritten_source,
      std::string& r_error)
    {
      if (helpers.is_empty())
      {
        r_rewritten_source = source;
        return true;
      }

      const Vector<GLSLToken> tokens = tokenize_glsl_source(source);
      Vector<GLSLFunctionDefinition> functions;
      if (!find_all_top_level_glsl_function_definitions(
            tokens, function_names, functions, r_error))
      {
        return false;
      }

      struct Replacement
      {
        int64_t start;
        int64_t end;
        std::string body;
      };
      Vector<Replacement> replacements;
      for (const GLSLClosureCallbackHelper& helper : helpers)
      {
        const GLSLClosureCallback* callback = find_glsl_closure_callback(callbacks,
          helper.function_name);
        const GLSLFunctionDefinition* function = find_glsl_function_definition_by_name(
          functions, helper.function_name);
        if (callback == nullptr || function == nullptr || function->body_source_start < 0 ||
            function->body_source_end < function->body_source_start)
        {
          r_error = "Could not locate the function body for connected GLSL closure callback '" +
            helper.function_name + "'";
          return false;
        }
        replacements.append({function->body_source_start,
                             function->body_source_end,
                             build_glsl_closure_callback_body(*callback, helper)});
      }

      std::sort(replacements.begin(),
                replacements.end(),
                [](const Replacement& a, const Replacement& b) { return a.start > b.start; });
      r_rewritten_source = source;
      for (const Replacement& replacement : replacements)
      {
        r_rewritten_source.replace(replacement.start,
                                   replacement.end - replacement.start,
                                   replacement.body);
      }
      return true;
    }

    static std::string build_closure_sampler_helper_block(
      const Span<GLSLClosureSampleHelper> closure_helpers)
    {
      if (closure_helpers.is_empty())
      {
        return "";
      }

      std::stringstream ss;
      for (const GLSLClosureSampleHelper& helper : closure_helpers)
      {
        switch (helper.return_type)
        {
        case GPU_FLOAT:
          ss << "float " << helper.sub_function_name << "();\n";
          break;
        case GPU_VEC3:
          ss << "vec3 " << helper.sub_function_name << "();\n";
          break;
        case GPU_VEC4:
        default:
          ss << "vec4 " << helper.sub_function_name << "();\n";
          break;
        }
      }
      ss << "\n";
      for (const GLSLClosureSampleHelper& helper : closure_helpers)
      {
        const char* coordinate_type = helper.coordinate_type == GPU_VEC3 ? "vec3" : "vec2";
        ss << coordinate_type << " " << helper.uv_global_name << " = " << coordinate_type
           << "(0.0);\n";
      }
      ss << "\n";
      for (const GLSLClosureSampleHelper& helper : closure_helpers)
      {
        const char* coordinate_type = helper.coordinate_type == GPU_VEC3 ? "vec3" : "vec2";
        const char* coordinate_name = helper.coordinate_type == GPU_VEC3 ? "coord" : "uv";
        ss << "vec4 " << helper.helper_name << "(" << coordinate_type << " " << coordinate_name
           << ")\n{\n";
        ss << "  " << helper.uv_global_name << " = " << coordinate_name << ";\n";
        switch (helper.return_type)
        {
        case GPU_FLOAT:
          ss << "  return vec4(vec3(" << helper.sub_function_name << "()), 1.0);\n";
          break;
        case GPU_VEC3:
          ss << "  return vec4(" << helper.sub_function_name << "(), 1.0);\n";
          break;
        case GPU_VEC4:
        default:
          ss << "  return " << helper.sub_function_name << "();\n";
          break;
        }
        ss << "}\n\n";
      }
      return ss.str();
    }

    static std::string build_glsl_geometry_helper_block()
    {
      return R"GLSL(
#if defined(GPU_FRAGMENT_SHADER) && (defined(MAT_DEFERRED) || defined(MAT_FORWARD) || defined(MAT_DEPTH) || defined(MAT_SHADOW) || defined(NPR_SHADER))
vec3 glsl_position()
{
  return g_data.P;
}

vec3 glsl_normal()
{
  return g_data.N;
}

vec3 glsl_true_normal()
{
  return g_data.Ng;
}

vec3 glsl_incoming()
{
  return coordinate_incoming(g_data.P);
}
#else
vec3 glsl_position()
{
  return vec3(0.0);
}

vec3 glsl_normal()
{
  return vec3(0.0);
}

vec3 glsl_true_normal()
{
  return vec3(0.0);
}

vec3 glsl_incoming()
{
  return vec3(0.0);
}
#endif

)GLSL";
    }

    static std::string build_glsl_lightprobe_helper_block()
    {
      return R"GLSL(
#if defined(GPU_FRAGMENT_SHADER) && (defined(MAT_DEFERRED) || defined(MAT_FORWARD) || defined(NPR_SHADER))
vec3 glsl_ambient_lighting()
{
#if defined(CREATE_INFO_eevee_LightprobeRenderData)
  vec4 reflection;
  vec4 irradiance;
  vec4 combined;
  node_light_probe_color(vec3(0.0), 1.0, reflection, irradiance, combined);
  return max(irradiance.rgb, vec3(0.0));
#else
  return vec3(0.0);
#endif
}
#else
vec3 glsl_ambient_lighting()
{
  return vec3(0.0);
}
#endif

)GLSL";
    }

    static std::string build_namespaced_glsl_source(const StringRef prefix,
      const Span<std::string> function_names,
      const Span<std::string> global_names,
      const Span<GLSLDefineMeta> defines,
      const StringRef define_block,
      const StringRef source,
      const Span<GLSLClosureSampleHelper> closure_helpers,
      const Span<GLSLClosureCallbackHelper> callback_helpers)
    {
      std::stringstream ss;
      ss << build_glsl_closure_callback_helper_block(callback_helpers);
      ss << build_closure_sampler_helper_block(closure_helpers);
      ss << "#define sample2D sampler2D\n";
      for (const std::string& name : global_names)
      {
        ss << "#define " << name << " " << prefix << name << "\n";
      }
      for (const std::string& name : function_names)
      {
        ss << "#define " << name << " " << prefix << name << "\n";
      }
      if (!define_block.is_empty())
      {
        ss << define_block;
      }
      ss << "\n" << source;
      if (!source.is_empty() && source[source.size() - 1] != '\n')
      {
        ss << "\n";
      }
      for (const GLSLDefineMeta& define : defines)
      {
        ss << "#undef " << define.name << "\n";
      }
      for (const std::string& name : function_names)
      {
        ss << "#undef " << name << "\n";
      }
      for (const std::string& name : global_names)
      {
        ss << "#undef " << name << "\n";
      }
      ss << "#undef sample2D\n";
      return ss.str();
    }

    static StringRefNull emitted_type_name(const GLSLFunctionParam& param,
      const GLSLSamplerSourceKind sampler_source_kind)
    {
      if (glsl_param_uses_color_socket(param))
      {
        return StringRefNull("vec4");
      }
      if (glsl_boundary_type_uses_float_transport(param.type))
      {
        return StringRefNull("float");
      }
      if (!glsl_boundary_type_is_sampler(param.type))
      {
        return param.type_name;
      }
      if (sampler_source_kind == GLSLSamplerSourceKind::ClosureOutput)
      {
        return StringRefNull("float");
      }
      return glsl_boundary_type_is_sample3d(param.type) ? StringRefNull("sampler3D") :
                                                          StringRefNull("sampler2D");
    }

    static StringRefNull emitted_type_name(const GLSLBoundaryType type, const StringRefNull type_name)
    {
      return glsl_boundary_type_uses_float_transport(type) ? StringRefNull("float") : type_name;
    }

    static std::string make_wrapper_split_vec4_expression(const StringRef vector_name)
    {
      const std::string name = std::string(vector_name);
      return "vec4(" + name + ", " + make_split_vec4_w_socket_identifier(name) + ")";
    }

    static std::string make_wrapper_matrix_column_name(const StringRef matrix_name,
                                                       const int column)
    {
      return make_split_matrix_column_socket_identifier(matrix_name, column);
    }

    static std::string make_wrapper_matrix_column_expression(const GLSLBoundaryType type,
                                                             const StringRef matrix_name,
                                                             const int column)
    {
      const std::string column_name = make_wrapper_matrix_column_name(matrix_name, column);
      if (glsl_matrix_column_dimensions(type) == 4)
      {
        return make_wrapper_split_vec4_expression(column_name);
      }
      return column_name;
    }

    static std::string make_wrapper_matrix_constructor_expression(const GLSLBoundaryType type,
                                                                  const StringRef type_name,
                                                                  const StringRef matrix_name)
    {
      std::string expression = std::string(type_name) + "(";
      for (const int column : IndexRange(glsl_matrix_columns(type)))
      {
        if (column != 0)
        {
          expression += ", ";
        }
        expression += make_wrapper_matrix_column_expression(type, matrix_name, column);
      }
      expression += ")";
      return expression;
    }

    static void append_wrapper_split_vec4_assignment(std::stringstream& ss,
                                                     const StringRef output_name,
                                                     const StringRef value_name)
    {
      ss << "  " << output_name << " = " << value_name << ".xyz;\n";
      ss << "  " << make_split_vec4_w_socket_identifier(output_name) << " = " << value_name
         << ".w;\n";
    }

    static void append_wrapper_matrix_column_assignment(std::stringstream& ss,
                                                        const GLSLBoundaryType type,
                                                        const StringRef output_name,
                                                        const StringRef value_name,
                                                        const int column)
    {
      const std::string column_output = make_wrapper_matrix_column_name(output_name, column);
      std::string column_value = std::string(value_name) + "[" + std::to_string(column) + "]";
      if (glsl_matrix_column_dimensions(type) == 4)
      {
        append_wrapper_split_vec4_assignment(ss, column_output, column_value);
      }
      else
      {
        ss << "  " << column_output << " = " << column_value << ";\n";
      }
    }

    static void append_wrapper_matrix_assignment(std::stringstream& ss,
                                                 const GLSLBoundaryType type,
                                                 const StringRef output_name,
                                                 const StringRef value_name)
    {
      for (const int column : IndexRange(glsl_matrix_columns(type)))
      {
        append_wrapper_matrix_column_assignment(ss, type, output_name, value_name, column);
      }
    }

    static std::string wrapper_argument_expression(const bNode& node, const GLSLFunctionParam& param)
    {
      if (param.meta.default_expression.has_value() &&
        !glsl_function_input_is_directly_linked(node, param))
      {
        return *param.meta.default_expression;
      }
      const std::string argument_name = make_wrapper_argument_name("in", param.name);
      if (glsl_param_uses_color_socket(param) && param.type == GLSLBoundaryType::Vec3)
      {
        return argument_name + ".rgb";
      }
      if (param.type == GLSLBoundaryType::Vec4)
      {
        return make_wrapper_split_vec4_expression(argument_name);
      }
      if (glsl_boundary_type_is_matrix(param.type))
      {
        return make_wrapper_matrix_constructor_expression(param.type, param.type_name, argument_name);
      }
      if (param.type == GLSLBoundaryType::Int)
      {
        return "int(round(" + argument_name + "))";
      }
      if (param.type == GLSLBoundaryType::Bool)
      {
        return "(" + argument_name + " != 0.0)";
      }
      return argument_name;
    }

    static bool glsl_output_is_split_vec4(const GLSLFunctionParam& param)
    {
      return param.type == GLSLBoundaryType::Vec4;
    }

    static bool glsl_boundary_type_uses_output_temp(const GLSLBoundaryType type)
    {
      return ELEM(type, GLSLBoundaryType::Vec4, GLSLBoundaryType::Int, GLSLBoundaryType::Bool) ||
        glsl_boundary_type_is_matrix(type);
    }

    static std::string build_wrapper_glsl_source(
      const bNode& node,
      const GLSLParseResult& parse_result,
      const Map<std::string, GLSLSamplerSourceKind>& sampler_source_kinds)
    {
      const GLSLFunctionDefinition& function = parse_result.function;
      const bool uses_expression_defaults = [&]()
        {
          for (const GLSLFunctionParam& param : function.params)
          {
            if (param.meta.default_expression.has_value())
            {
              return true;
            }
          }
          return false;
        }();

      std::stringstream ss;
      if (uses_expression_defaults)
      {
        for (const std::string& name : parse_result.global_names)
        {
          ss << "#define " << name << " " << parse_result.source_prefix << name << "\n";
        }
        for (const std::string& name : parse_result.function_names)
        {
          ss << "#define " << name << " " << parse_result.source_prefix << name << "\n";
        }
        ss << "\n";
      }
      ss << "void " << parse_result.wrapper_name << "(";
      bool need_comma = false;
      auto append_wrapper_input = [&](const StringRefNull type_name, const StringRef name)
        {
          if (need_comma)
          {
            ss << ", ";
          }
          ss << type_name << " " << name;
          need_comma = true;
        };
      auto append_wrapper_output = [&](const StringRefNull type_name, const StringRef name)
        {
          if (need_comma)
          {
            ss << ", ";
          }
          ss << "out " << type_name << " " << name;
          need_comma = true;
        };
      auto append_wrapper_split_vec4_input = [&](const StringRef name)
        {
          append_wrapper_input("vec3", name);
          append_wrapper_input("float", make_split_vec4_w_socket_identifier(name));
        };
      auto append_wrapper_split_vec4_output = [&](const StringRef name)
        {
          append_wrapper_output("vec3", name);
          append_wrapper_output("float", make_split_vec4_w_socket_identifier(name));
        };
      auto append_wrapper_matrix_input = [&](const GLSLBoundaryType type, const StringRef name)
        {
          const int column_dimensions = glsl_matrix_column_dimensions(type);
          for (const int column : IndexRange(glsl_matrix_columns(type)))
          {
            const std::string column_name = make_wrapper_matrix_column_name(name, column);
            if (column_dimensions == 4)
            {
              append_wrapper_split_vec4_input(column_name);
            }
            else
            {
              append_wrapper_input(glsl_vector_type_name(column_dimensions), column_name);
            }
          }
        };
      auto append_wrapper_matrix_output = [&](const GLSLBoundaryType type, const StringRef name)
        {
          const int column_dimensions = glsl_matrix_column_dimensions(type);
          for (const int column : IndexRange(glsl_matrix_columns(type)))
          {
            const std::string column_name = make_wrapper_matrix_column_name(name, column);
            if (column_dimensions == 4)
            {
              append_wrapper_split_vec4_output(column_name);
            }
            else
            {
              append_wrapper_output(glsl_vector_type_name(column_dimensions), column_name);
            }
          }
        };
      for (const GLSLFunctionParam& param : function.params)
      {
        if (!glsl_param_has_input_socket(param))
        {
          continue;
        }
        const std::string input_name = make_wrapper_argument_name("in", param.name);
        if (param.type == GLSLBoundaryType::Vec4)
        {
          append_wrapper_split_vec4_input(input_name);
          continue;
        }
        if (glsl_boundary_type_is_matrix(param.type))
        {
          append_wrapper_matrix_input(param.type, input_name);
          continue;
        }
        const GLSLSamplerSourceKind source_kind = sampler_source_kinds.lookup_ptr(param.name) ?
          *sampler_source_kinds.lookup_ptr(param.name) :
          GLSLSamplerSourceKind::None;
        append_wrapper_input(emitted_type_name(param, source_kind), input_name);
      }
      if (function.return_type != GLSLBoundaryType::Void)
      {
        if (function.return_type == GLSLBoundaryType::Vec4)
        {
          append_wrapper_split_vec4_output("out_result");
        }
        else if (glsl_boundary_type_is_matrix(function.return_type))
        {
          append_wrapper_matrix_output(function.return_type, "out_result");
        }
        else
        {
          append_wrapper_output(emitted_type_name(function.return_type, function.return_type_name.c_str()),
            "out_result");
        }
      }
      for (const GLSLFunctionParam& param : function.params)
      {
        if (!glsl_param_has_output_socket(param))
        {
          continue;
        }
        if (glsl_output_is_split_vec4(param))
        {
          append_wrapper_split_vec4_output(make_wrapper_argument_name("out", param.name));
        }
        else if (glsl_boundary_type_is_matrix(param.type))
        {
          append_wrapper_matrix_output(param.type, make_wrapper_argument_name("out", param.name));
        }
        else
        {
          append_wrapper_output(emitted_type_name(param.type, param.type_name.c_str()),
            make_wrapper_argument_name("out", param.name));
        }
      }
      ss << ")\n{\n";
      if (glsl_boundary_type_uses_output_temp(function.return_type))
      {
        if (function.return_type == GLSLBoundaryType::Vec4)
        {
          ss << "  vec4 " << make_wrapper_vec4_temp_name("out_result") << ";\n";
        }
        else
        {
          ss << "  " << function.return_type_name << " " << make_wrapper_temp_name("out_result")
            << ";\n";
        }
      }
      for (const GLSLFunctionParam& param : function.params)
      {
        if (glsl_param_has_output_socket(param) && glsl_boundary_type_uses_output_temp(param.type))
        {
          if (glsl_output_is_split_vec4(param))
          {
            ss << "  vec4 "
              << make_wrapper_vec4_temp_name(make_wrapper_argument_name("out", param.name))
              << ";\n";
          }
          else
          {
            ss << "  " << param.type_name << " "
              << make_wrapper_temp_name(make_wrapper_argument_name("out", param.name)) << ";\n";
          }
        }
      }
      ss << "  ";
      if (function.return_type != GLSLBoundaryType::Void)
      {
        if (function.return_type == GLSLBoundaryType::Vec4)
        {
          ss << make_wrapper_vec4_temp_name("out_result") << " = ";
        }
        else if (glsl_boundary_type_uses_output_temp(function.return_type))
        {
          ss << make_wrapper_temp_name("out_result") << " = ";
        }
        else
        {
          ss << "out_result = ";
        }
      }
      ss << parse_result.source_prefix << function.name << "(";
      need_comma = false;
      for (const GLSLFunctionParam& param : function.params)
      {
        if (need_comma)
        {
          ss << ", ";
        }
        if (glsl_param_has_output_socket(param))
        {
          if (glsl_output_is_split_vec4(param))
          {
            ss << make_wrapper_vec4_temp_name(make_wrapper_argument_name("out", param.name));
          }
          else if (glsl_boundary_type_uses_output_temp(param.type))
          {
            ss << make_wrapper_temp_name(make_wrapper_argument_name("out", param.name));
          }
          else
          {
            ss << make_wrapper_argument_name("out", param.name);
          }
        }
        else
        {
          ss << wrapper_argument_expression(node, param);
        }
        need_comma = true;
      }
      ss << ");\n";
      if (function.return_type == GLSLBoundaryType::Vec4)
      {
        append_wrapper_split_vec4_assignment(ss, "out_result", make_wrapper_vec4_temp_name("out_result"));
      }
      else if (glsl_boundary_type_is_matrix(function.return_type))
      {
        append_wrapper_matrix_assignment(
          ss, function.return_type, "out_result", make_wrapper_temp_name("out_result"));
      }
      else if (function.return_type == GLSLBoundaryType::Int)
      {
        ss << "  out_result = float(" << make_wrapper_temp_name("out_result") << ");\n";
      }
      else if (function.return_type == GLSLBoundaryType::Bool)
      {
        ss << "  out_result = " << make_wrapper_temp_name("out_result") << " ? 1.0 : 0.0;\n";
      }
      for (const GLSLFunctionParam& param : function.params)
      {
        if (glsl_param_has_output_socket(param) && glsl_boundary_type_uses_output_temp(param.type))
        {
          const std::string output_name = make_wrapper_argument_name("out", param.name);
          if (glsl_output_is_split_vec4(param))
          {
            const std::string temp_name = make_wrapper_vec4_temp_name(output_name);
            append_wrapper_split_vec4_assignment(ss, output_name, temp_name);
          }
          else if (glsl_boundary_type_is_matrix(param.type))
          {
            append_wrapper_matrix_assignment(
              ss, param.type, output_name, make_wrapper_temp_name(output_name));
          }
          else if (param.type == GLSLBoundaryType::Int)
          {
            ss << "  " << output_name << " = float(" << make_wrapper_temp_name(output_name)
              << ");\n";
          }
          else if (param.type == GLSLBoundaryType::Bool)
          {
            ss << "  " << output_name << " = " << make_wrapper_temp_name(output_name)
              << " ? 1.0 : 0.0;\n";
          }
        }
      }
      ss << "}\n";
      if (uses_expression_defaults)
      {
        for (const std::string& name : parse_result.function_names)
        {
          ss << "#undef " << name << "\n";
        }
        for (const std::string& name : parse_result.global_names)
        {
          ss << "#undef " << name << "\n";
        }
      }
      return ss.str();
    }

    static bool add_unique_generated_identifier(Set<std::string>& identifiers,
                                                const std::string& identifier,
                                                const StringRef scope,
                                                std::string& r_error)
    {
      if (identifiers.add(identifier))
      {
        return true;
      }
      r_error = "Generated GLSL Function " + std::string(scope) + " identifier '" + identifier +
        "' is duplicated after splitting vec4 or matrix boundary sockets; rename parameters that "
        "end with generated suffixes such as _w, _c0, or _tmp";
      return false;
    }

    static bool add_generated_boundary_identifiers(Set<std::string>& identifiers,
                                                   const GLSLBoundaryType type,
                                                   const StringRef base_identifier,
                                                   const StringRef scope,
                                                   std::string& r_error)
    {
      if (type == GLSLBoundaryType::Void)
      {
        return true;
      }
      if (type == GLSLBoundaryType::Vec4)
      {
        return add_unique_generated_identifier(
                 identifiers, std::string(base_identifier), scope, r_error) &&
          add_unique_generated_identifier(
                 identifiers, make_split_vec4_w_socket_identifier(base_identifier), scope, r_error);
      }
      if (glsl_boundary_type_is_matrix(type))
      {
        for (const int column : IndexRange(glsl_matrix_columns(type)))
        {
          const std::string column_identifier = make_split_matrix_column_socket_identifier(
            base_identifier, column);
          if (!add_unique_generated_identifier(identifiers, column_identifier, scope, r_error))
          {
            return false;
          }
          if (glsl_matrix_column_dimensions(type) == 4 &&
              !add_unique_generated_identifier(identifiers,
                                               make_split_vec4_w_socket_identifier(
                                                 column_identifier),
                                               scope,
                                               r_error))
          {
            return false;
          }
        }
        return true;
      }
      return add_unique_generated_identifier(
        identifiers, std::string(base_identifier), scope, r_error);
    }

    static bool validate_generated_glsl_function_identifiers(
      const GLSLFunctionDefinition& function, std::string& r_error)
    {
      Set<std::string> input_socket_identifiers;
      Set<std::string> output_socket_identifiers;
      Set<std::string> wrapper_identifiers;

      for (const GLSLFunctionParam& param : function.params)
      {
        if (glsl_param_has_input_socket(param) &&
            !add_generated_boundary_identifiers(input_socket_identifiers,
                                                param.type,
                                                param.identifier,
                                                "input socket",
                                                r_error))
        {
          return false;
        }
        if (glsl_param_has_output_socket(param))
        {
          const std::string output_identifier = make_socket_identifier("Out", param.name);
          if (!add_generated_boundary_identifiers(output_socket_identifiers,
                                                 param.type,
                                                 output_identifier,
                                                 "output socket",
                                                 r_error))
          {
            return false;
          }
        }
      }

      if (!add_generated_boundary_identifiers(output_socket_identifiers,
                                             function.return_type,
                                             result_socket_identifier,
                                             "output socket",
                                             r_error))
      {
        return false;
      }

      for (const GLSLFunctionParam& param : function.params)
      {
        if (glsl_param_has_input_socket(param))
        {
          const std::string input_name = make_wrapper_argument_name("in", param.name);
          if (!add_generated_boundary_identifiers(
                wrapper_identifiers, param.type, input_name, "wrapper", r_error))
          {
            return false;
          }
        }
        if (glsl_param_has_output_socket(param))
        {
          const std::string output_name = make_wrapper_argument_name("out", param.name);
          if (!add_generated_boundary_identifiers(
                wrapper_identifiers, param.type, output_name, "wrapper", r_error))
          {
            return false;
          }
          if (glsl_output_is_split_vec4(param) &&
              !add_unique_generated_identifier(wrapper_identifiers,
                                               make_wrapper_vec4_temp_name(output_name),
                                               "wrapper",
                                               r_error))
          {
            return false;
          }
          if (param.type != GLSLBoundaryType::Vec4 &&
              glsl_boundary_type_uses_output_temp(param.type) &&
              !add_unique_generated_identifier(wrapper_identifiers,
                                               make_wrapper_temp_name(output_name),
                                               "wrapper",
                                               r_error))
          {
            return false;
          }
        }
      }

      if (!add_generated_boundary_identifiers(
            wrapper_identifiers, function.return_type, "out_result", "wrapper", r_error))
      {
        return false;
      }
      if (function.return_type == GLSLBoundaryType::Vec4)
      {
        return add_unique_generated_identifier(wrapper_identifiers,
                                               make_wrapper_vec4_temp_name("out_result"),
                                               "wrapper",
                                               r_error);
      }
      if (glsl_boundary_type_uses_output_temp(function.return_type))
      {
        return add_unique_generated_identifier(wrapper_identifiers,
                                               make_wrapper_temp_name("out_result"),
                                               "wrapper",
                                               r_error);
      }
      return true;
    }

    static bool build_specialized_glsl_sources(GPUMaterial* mat,
      const bNode& node,
      const GLSLParseResult& parse_result,
      std::string& r_library_source,
      std::string& r_wrapper_source,
      std::string& r_error)
    {
      const std::string stripped_source = strip_glsl_comments(parse_result.source);
      const Vector<GLSLToken> tokens = tokenize_glsl_source(stripped_source);
      const Map<std::string, GLSLSamplerSourceKind> source_kinds = resolve_sampler_source_kinds(
        node, parse_result.function);
      Vector<GLSLFunctionDefinition> all_functions;
      if (!find_all_top_level_glsl_function_definitions(
        tokens, parse_result.function_names, all_functions, r_error))
      {
        return false;
      }

      Vector<GLSLClosureFunctionBindings> bindings_by_function;
      if (!build_closure_sampler_bindings_by_function(
        tokens, all_functions, parse_result.function, source_kinds, bindings_by_function, r_error))
      {
        return false;
      }

      Vector<GLSLClosureSampleHelper> closure_helpers;
      for (const GLSLFunctionParam& param : parse_result.function.params)
      {
        const GLSLSamplerSourceKind* source_kind = source_kinds.lookup_ptr(param.name);
        if (source_kind == nullptr || *source_kind != GLSLSamplerSourceKind::ClosureOutput)
        {
          continue;
        }

        GLSLClosureSampleHelper helper;
        const std::string sampler_dimension = glsl_boundary_type_is_sample3d(param.type) ? "3d" :
                                                                                        "2d";
        const std::string helper_name = "glsl_sampler" + sampler_dimension + "_" +
          std::to_string(node.identifier) + "_" + param.identifier;
        const std::string uv_global_name = helper_name +
          (glsl_boundary_type_is_sample3d(param.type) ? "_coord" : "_uv");
        if (!build_closure_sample_helper(
          mat, node, param, parse_result.source_filename, helper_name, uv_global_name, helper, r_error))
        {
          return false;
        }
        closure_helpers.append(helper);
      }

      Vector<GLSLClosureCallbackHelper> callback_helpers;
      for (const GLSLClosureCallback& callback : parse_result.closure_callbacks)
      {
        const std::string socket_identifier = "closure." + callback.function.name;
        const bNodeSocket* callback_socket = find_node_input_socket_by_identifier(
          node, socket_identifier);
        const bNodeLink* callback_link = callback_socket != nullptr ?
          find_used_direct_link(*callback_socket) :
          nullptr;
        if (callback_link == nullptr)
        {
          continue;
        }

        GLSLClosureCallbackHelper helper;
        if (!build_glsl_closure_callback_helper(
              mat, node, callback, parse_result.source_filename, helper, r_error))
        {
          return false;
        }
        const bool collides_with_user_name =
          std::find(parse_result.function_names.begin(),
                    parse_result.function_names.end(),
                    helper.sub_function_name) != parse_result.function_names.end() ||
          std::find(parse_result.global_names.begin(),
                    parse_result.global_names.end(),
                    helper.sub_function_name) != parse_result.global_names.end() ||
          std::any_of(parse_result.defines.begin(),
                      parse_result.defines.end(),
                      [&](const GLSLDefineMeta& define) {
                        return define.name == helper.sub_function_name;
                      });
        if (collides_with_user_name)
        {
          r_error = "Generated GPU function name '" + helper.sub_function_name +
            "' conflicts with GLSL source while compiling closure callback '" +
            callback.function.name + "'";
          return false;
        }
        callback_helpers.append(std::move(helper));
      }

      std::string sampler_rewritten_source;
      GLSLClosureSamplerDowngradeInfo downgrade_info;
      if (!rewrite_glsl_source_for_closure_samplers(
        stripped_source,
        tokens,
        all_functions,
        bindings_by_function,
        closure_helpers,
        sampler_rewritten_source,
        downgrade_info,
        r_error))
      {
        return false;
      }
      log_closure_sampler_downgrades(mat, node, parse_result, downgrade_info);

      std::string callback_rewritten_source;
      if (!rewrite_glsl_source_for_closure_callbacks(sampler_rewritten_source,
                                                     parse_result.function_names,
                                                     parse_result.closure_callbacks,
                                                     callback_helpers,
                                                     callback_rewritten_source,
                                                     r_error))
      {
        return false;
      }

      const std::string define_block = build_glsl_define_block(node, parse_result.defines);
      r_library_source = build_namespaced_glsl_source(
        parse_result.source_prefix,
        parse_result.function_names,
        parse_result.global_names,
        parse_result.defines,
        define_block,
        callback_rewritten_source,
        closure_helpers,
        callback_helpers);
      r_wrapper_source = build_wrapper_glsl_source(node, parse_result, source_kinds);

      return true;
    }

    static GLSLParseResult parse_glsl_source_for_node(const bNode& node,
                                                      const std::string& source,
                                                      const StringRef function_name)
    {
      GLSLParseResult result;
      result.keep_existing_sockets = true;

      if (trim_copy(source).empty())
      {
        result.error = "The GLSL source is empty";
        return result;
      }

      Map<std::string, GLSLRawParamMeta> meta_by_key;
      Map<std::string, Vector<GLSLPanelMeta>> panels_by_function;
      Map<std::string, GLSLRawClosureMeta> raw_closures;
      if (!extract_glsl_meta(source, meta_by_key, panels_by_function, result.error))
      {
        return result;
      }
      if (!extract_glsl_closures(source, raw_closures, result.error))
      {
        return result;
      }
      if (!extract_glsl_defines(
            source, result.defines, result.defines_panel_default_closed, result.error))
      {
        return result;
      }
      result.defines_parsed = true;

      const std::string stripped_source = strip_glsl_comments(source);
      if (!validate_glsl_closure_function_like_macros(
            stripped_source, raw_closures, result.error))
      {
        return result;
      }
      const Vector<GLSLToken> tokens = tokenize_glsl_source(stripped_source);
      std::string deprecated_identifier;
      if (glsl_source_uses_deprecated_eevee_light_access(tokens, deprecated_identifier))
      {
        result.error = "Deprecated Eevee light helper '" + deprecated_identifier +
          "' was removed. Use glsl_light_get() to read GLSLLight.diffuse_color, "
          "GLSLLight.specular_color, and GLSLLight.attenuation, then combine them with "
          "glsl_light_shadow() manually.";
        return result;
      }
      result.uses_geometry_access = glsl_source_uses_geometry_access(tokens);
      result.uses_lightprobe_access = glsl_source_uses_lightprobe_access(tokens);
      result.uses_eevee_light_access = glsl_source_uses_eevee_light_access(tokens);
      if (!validate_no_top_level_conditional_glsl_code(stripped_source, tokens, result.error))
      {
        return result;
      }
      result.function_names = find_top_level_glsl_function_names(tokens);
      if (result.function_names.is_empty())
      {
        result.error = "No top-level GLSL function definition was found";
        return result;
      }
      if (!validate_top_level_glsl_declarations(tokens, result.error))
      {
        return result;
      }
      result.global_names = find_top_level_glsl_global_names(tokens, result.function_names);
      if (!validate_glsl_define_name_collisions(
        result.defines, result.function_names, result.global_names, result.error))
      {
        return result;
      }

      const StringRef requested_function_name = function_name;
      if (requested_function_name.is_empty())
      {
        return result;
      }
      if (!find_glsl_function_definition(tokens, requested_function_name, result.function, result.error))
      {
        return result;
      }
      if (!apply_glsl_meta_to_function(
        meta_by_key, panels_by_function, result.function, result.meta_hash, result.error))
      {
        return result;
      }
      if (!build_glsl_closure_callbacks(tokens,
                                        result.function_names,
                                        raw_closures,
                                        result.function,
                                        result.closure_callbacks,
                                        result.error))
      {
        return result;
      }
      const std::string define_meta_signature_key = build_glsl_define_meta_signature_key(
        result.defines, result.defines_panel_default_closed);
      if (!define_meta_signature_key.empty())
      {
        const std::string meta_hash_key = std::to_string(result.meta_hash) +
          "\n\x1fglsl_define_meta\x1f" + define_meta_signature_key;
        result.meta_hash = int(BLI_ghashutil_strhash_p(meta_hash_key.c_str()));
      }
      if (!result.closure_callbacks.is_empty())
      {
        const std::string meta_hash_key = std::to_string(result.meta_hash) +
          "\n\x1fglsl_closure_meta\x1f" + source;
        result.meta_hash = int(BLI_ghashutil_strhash_p(meta_hash_key.c_str()));
      }
      if (glsl_function_meta_uses_deprecated_eevee_light_access(result.function, deprecated_identifier))
      {
        result.error = "Deprecated Eevee light helper '" + deprecated_identifier +
          "' was removed. Use glsl_light_get() to read GLSLLight.diffuse_color, "
          "GLSLLight.specular_color, and GLSLLight.attenuation, then combine them with "
          "glsl_light_shadow() manually.";
        return result;
      }
      result.uses_geometry_access |= glsl_function_meta_uses_geometry_access(result.function);
      result.uses_lightprobe_access |= glsl_function_meta_uses_lightprobe_access(result.function);
      result.uses_eevee_light_access |= glsl_function_meta_uses_eevee_light_access(result.function);
      if (!validate_generated_glsl_function_identifiers(result.function, result.error))
      {
        return result;
      }
      if (!validate_sampler_inputs(node, tokens, result.function, result.error))
      {
        return result;
      }

      result.ok = true;
      result.source = source;
      result.resolved_function_name = result.function.name;
      result.used_first_function = false;

      const std::string signature_key = build_function_signature_key(result.function) +
        build_glsl_closure_signature_key(result.closure_callbacks);
      const uint signature_hash = BLI_ghashutil_strhash_p(signature_key.c_str());
      result.signature_hash = int(signature_hash);

      const std::string define_signature_key = build_glsl_define_signature_key(node, result.defines);
      const uint define_hash = define_signature_key.empty() ?
        0 :
        BLI_ghashutil_strhash_p(define_signature_key.c_str());
      result.define_hash = int(define_hash);

      std::string source_hash_key = source;
      if (!define_signature_key.empty())
      {
        source_hash_key += "\n\x1fglsl_defines\x1f";
        source_hash_key += define_signature_key;
      }
      const uint source_hash = BLI_ghashutil_strhash_p(source_hash_key.c_str());
      char source_hash_hex[16];
      char signature_hash_hex[16];
      char define_hash_hex[16];
      SNPRINTF(source_hash_hex, "%08x", source_hash);
      SNPRINTF(signature_hash_hex, "%08x", signature_hash);
      SNPRINTF(define_hash_hex, "%08x", define_hash);
      result.source_hash_hex = source_hash_hex;
      const std::string node_id_suffix = std::to_string(node.identifier);
      result.source_prefix = "glsl_src_" + result.source_hash_hex + "_" + node_id_suffix + "_";
      result.source_filename =
        "glsl_function_source_" + result.source_hash_hex + "_" + node_id_suffix + ".glsl";
      result.wrapper_name = "glsl_fn_" + std::to_string(node.identifier) + "_" + signature_hash_hex;
      if (!define_signature_key.empty())
      {
        result.wrapper_name += "_";
        result.wrapper_name += define_hash_hex;
      }
      result.wrapper_filename = result.wrapper_name + ".glsl";
      const std::string define_block = build_glsl_define_block(node, result.defines);
      result.library_source = build_namespaced_glsl_source(
        result.source_prefix,
        result.function_names,
        result.global_names,
        result.defines,
        define_block,
        result.source,
        Span<GLSLClosureSampleHelper>(),
        Span<GLSLClosureCallbackHelper>());
      result.wrapper_source = build_wrapper_glsl_source(
        node, result, Map<std::string, GLSLSamplerSourceKind>());
      return result;
    }

    static GLSLParseResult parse_glsl_for_node(const bNode& node)
    {
      GLSLParseResult result;
      result.keep_existing_sockets = true;

      std::string source;
      if (!load_glsl_source(node, source, result.error))
      {
        return result;
      }
      return parse_glsl_source_for_node(node, source, node_storage(node).function_name);
    }

    static bool validate_glsl_candidate_with_gpu(const GLSLParseResult& parse_result,
                                                 std::string& r_error)
    {
      if (GPU_context_active_get() == nullptr)
      {
        /* Background automation has no running GPU material to preserve. The structural parser
         * validation above still prevents invalid function signatures and metadata from applying. */
        r_error.clear();
        return true;
      }

      static constexpr char validation_preamble[] = R"GLSL(
#define GLSL_LIGHT_TYPE_INVALID 0
#define GLSL_LIGHT_TYPE_SUN 1
#define GLSL_LIGHT_TYPE_POINT 2
#define GLSL_LIGHT_TYPE_SPOT 3
#define GLSL_LIGHT_TYPE_AREA_RECT 4
#define GLSL_LIGHT_TYPE_AREA_ELLIPSE 5

struct GLSLLight {
  bool valid;
  uint index;
  int type;
  int lightgroup_id;
  vec3 vector;
  vec3 position;
  vec3 direction;
  float distance;
  vec3 diffuse_color;
  vec3 specular_color;
  float attenuation;
};

GLSLLight glsl_light_default()
{
  GLSLLight light;
  light.valid = false;
  light.index = 0u;
  light.type = GLSL_LIGHT_TYPE_INVALID;
  light.lightgroup_id = 0;
  light.vector = vec3(0.0, 0.0, 1.0);
  light.position = vec3(0.0);
  light.direction = vec3(0.0);
  light.distance = 0.0;
  light.diffuse_color = vec3(0.0);
  light.specular_color = vec3(0.0);
  light.attenuation = 0.0;
  return light;
}

int glsl_light_count() { return 0; }
GLSLLight glsl_light_get(int light_ordinal) { return glsl_light_default(); }
float glsl_light_shadow(int light_ordinal, vec3 shading_normal) { return 0.0; }
vec3 glsl_position() { return vec3(0.0); }
vec3 glsl_normal() { return vec3(0.0); }
vec3 glsl_true_normal() { return vec3(0.0); }
vec3 glsl_incoming() { return vec3(0.0); }
vec3 glsl_ambient_lighting() { return vec3(0.0); }
)GLSL";

      gpu::shader::ShaderCreateInfo create_info("pyGPU_Shader");
      create_info.fragment_out(0, gpu::shader::Type::float4_t, "frag_out");
      create_info.vertex_source_generated = "void main() { gl_Position = vec4(0.0); }";
      create_info.fragment_source_generated = validation_preamble;
      create_info.fragment_source_generated += parse_result.library_source;
      create_info.fragment_source_generated += parse_result.wrapper_source;
      create_info.fragment_source_generated += "\nvoid main() { frag_out = vec4(0.0); }\n";

      gpu::Shader* shader = GPU_shader_create_from_info_python(
        reinterpret_cast<GPUShaderCreateInfo*>(&create_info));
      if (shader == nullptr)
      {
        r_error = "The GPU shader compiler rejected the current GLSL source";
        return false;
      }
      GPU_shader_free(shader);
      r_error.clear();
      return true;
    }

    static void cache_parse_status(bNode& node, const GLSLParseResult& parse_result)
    {
      NodeShaderGLSLFunction& storage = node_storage(node);
      if (parse_result.ok)
      {
        storage.parse_status = SHD_GLSL_FUNCTION_PARSE_READY;
        storage.signature_hash = parse_result.signature_hash;
      }
      else if (parse_result.error.empty())
      {
        storage.parse_status = SHD_GLSL_FUNCTION_PARSE_DIRTY;
        storage.signature_hash = 0;
      }
      else
      {
        storage.parse_status = SHD_GLSL_FUNCTION_PARSE_ERROR;
        storage.signature_hash = 0;
      }
    }

    static std::string glsl_socket_display_name(const GLSLFunctionParam& param)
    {
      return param.meta.label.value_or(param.name);
    }

    static void register_glsl_param_int_choices(
      bNodeSocket& socket,
      const Span<GLSLFunctionParam::IntChoiceItem> choices);

    static void add_glsl_socket_declaration(DeclarationListBuilder& b,
      const GLSLFunctionParam& param,
      const bool is_output,
      const UString socket_name,
      const UString socket_identifier)
    {
      auto apply_input_description = [&](auto& decl) {
        if (!is_output && param.meta.description.has_value())
        {
          decl.description(*param.meta.description);
        }
      };
      auto configure_float_input_decl = [&](auto& decl, const float default_value)
        {
          decl.min(param.meta.has_min ? param.meta.min_value : -10000.0f)
            .max(param.meta.has_max ? param.meta.max_value : 10000.0f);
          if (param.meta.has_default_value)
          {
            decl.default_value(default_value);
          }
          if (param.meta.hide_value)
          {
            decl.hide_value();
          }
          apply_input_description(decl);
        };
      auto configure_vector_decl = [&](auto& decl, const int dimensions, const float4 default_value)
        {
          decl.dimensions(dimensions)
            .min(param.meta.has_min ? param.meta.min_value : -10000.0f)
            .max(param.meta.has_max ? param.meta.max_value : 10000.0f);
          if (param.meta.has_default_value)
          {
            switch (dimensions)
            {
            case 2:
              decl.default_value(float2(default_value.x, default_value.y));
              break;
            case 3:
              decl.default_value(float3(default_value.x, default_value.y, default_value.z));
              break;
            default:
              break;
            }
          }
          if (param.meta.subtype.has_value())
          {
            decl.subtype(*param.meta.subtype);
          }
          if (param.meta.hide_value)
          {
            decl.hide_value();
          }
          apply_input_description(decl);
        };
      auto add_split_vec4_declarations =
        [&](const UString name, const UString identifier, const float4 default_value)
        {
          if (is_output)
          {
            auto& vector_decl = b.add_output<decl::Vector>(name, identifier);
            vector_decl.dimensions(3);
            b.add_output<decl::Float>(UString(make_split_vec4_w_socket_name(name.c_str())),
                                      UString(make_split_vec4_w_socket_identifier(
                                        identifier.c_str())));
            return;
          }

          auto& vector_decl = b.add_input<decl::Vector>(name, identifier);
          configure_vector_decl(vector_decl, 3, default_value);
          auto& w_decl = b.add_input<decl::Float>(
            UString(make_split_vec4_w_socket_name(name.c_str())),
            UString(make_split_vec4_w_socket_identifier(identifier.c_str())));
          configure_float_input_decl(w_decl, default_value.w);
        };
      auto add_matrix_declarations =
        [&](const UString name, const UString identifier)
        {
          const int column_dimensions = glsl_matrix_column_dimensions(param.type);
          for (const int column : IndexRange(glsl_matrix_columns(param.type)))
          {
            const std::string column_name = make_split_matrix_column_socket_name(name.c_str(),
                                                                                column);
            const std::string column_identifier = make_split_matrix_column_socket_identifier(
              identifier.c_str(), column);
            if (column_dimensions == 4)
            {
              add_split_vec4_declarations(UString(column_name),
                                          UString(column_identifier),
                                          float4(0.0f));
              continue;
            }
            if (is_output)
            {
              auto& vector_decl = b.add_output<decl::Vector>(UString(column_name),
                                                             UString(column_identifier));
              vector_decl.dimensions(column_dimensions);
            }
            else
            {
              auto& vector_decl = b.add_input<decl::Vector>(UString(column_name),
                                                            UString(column_identifier));
              configure_vector_decl(vector_decl, column_dimensions, float4(0.0f));
            }
          }
        };

      if (glsl_boundary_type_is_sampler(param.type))
      {
        BLI_assert(!is_output);
        auto& decl = b.add_input<decl::Closure>(socket_name, socket_identifier);
        apply_input_description(decl);
        return;
      }

      if (param.type == GLSLBoundaryType::Float)
      {
        if (is_output)
        {
          b.add_output<decl::Float>(socket_name, socket_identifier);
        }
        else
        {
          auto& decl = b.add_input<decl::Float>(socket_name, socket_identifier)
            .min(param.meta.has_min ? param.meta.min_value : -10000.0f)
            .max(param.meta.has_max ? param.meta.max_value : 10000.0f);
          if (param.meta.has_default_value)
          {
            decl.default_value(param.meta.default_value.x);
          }
          if (param.meta.hide_value)
          {
            decl.hide_value();
          }
          if (param.meta.subtype.has_value())
          {
            decl.subtype(*param.meta.subtype);
          }
          apply_input_description(decl);
        }
        return;
      }

      if (param.type == GLSLBoundaryType::Int)
      {
        if (is_output)
        {
          b.add_output<decl::Int>(socket_name, socket_identifier);
        }
        else
        {
          BLI_assert(!param.meta.has_default_value || param.meta.int_default_value.has_value());
          BLI_assert(!param.meta.has_min || param.meta.int_min_value.has_value());
          BLI_assert(!param.meta.has_max || param.meta.int_max_value.has_value());
          auto& decl = b.add_input<decl::Int>(socket_name, socket_identifier)
            .min(param.meta.has_min ? param.meta.int_min_value.value_or(-10000) : -10000)
            .max(param.meta.has_max ? param.meta.int_max_value.value_or(10000) : 10000);
          if (param.meta.has_default_value)
          {
            decl.default_value(param.meta.int_default_value.value_or(0));
          }
          if (param.meta.hide_value)
          {
            decl.hide_value();
          }
          if (!param.meta.int_choices.is_empty())
          {
            Vector<GLSLFunctionParam::IntChoiceItem> choices = param.meta.int_choices;
            const bool show_label = param.meta.int_choices_show_label;
            decl.custom_draw([choices, show_label](CustomSocketDrawParams& params) {
              if (params.socket.is_logically_linked() || (params.socket.flag & SOCK_HIDE_VALUE))
              {
                params.draw_standard(params.layout);
                return;
              }
              register_glsl_param_int_choices(params.socket, choices);
              params.layout.prop(&params.socket_ptr,
                                 "glsl_int_choice_value",
                                 UI_ITEM_NONE,
                                 show_label ? params.label : "",
                                 ICON_NONE);
            });
          }
          apply_input_description(decl);
        }
        return;
      }

      if (param.type == GLSLBoundaryType::Bool)
      {
        if (is_output)
        {
          b.add_output<decl::Bool>(socket_name, socket_identifier);
        }
        else
        {
          auto& decl = b.add_input<decl::Bool>(socket_name, socket_identifier);
          if (param.meta.has_default_value)
          {
            decl.default_value(param.meta.default_value.x != 0.0f);
          }
          if (param.meta.hide_value)
          {
            decl.hide_value();
          }
          apply_input_description(decl);
        }
        return;
      }

      if (param.type == GLSLBoundaryType::Vec4)
      {
        add_split_vec4_declarations(socket_name, socket_identifier, param.meta.default_value);
        return;
      }

      if (glsl_boundary_type_is_matrix(param.type))
      {
        add_matrix_declarations(socket_name, socket_identifier);
        return;
      }

      const bool use_color_socket = !is_output && glsl_param_uses_color_socket(param);
      if (use_color_socket)
      {
        auto& decl = b.add_input<decl::Color>(socket_name, socket_identifier);
        if (param.meta.has_default_value)
        {
          decl.default_value(ColorGeometry4f(param.meta.default_value.x,
            param.meta.default_value.y,
            param.meta.default_value.z,
            1.0f));
        }
        if (param.meta.hide_value)
        {
          decl.hide_value();
        }
        apply_input_description(decl);
        return;
      }

      if (is_output)
      {
        auto& decl = b.add_output<decl::Vector>(socket_name, socket_identifier);
        configure_vector_decl(decl, param.dimensions, param.meta.default_value);
      }
      else
      {
        auto& decl = b.add_input<decl::Vector>(socket_name, socket_identifier);
        configure_vector_decl(decl, param.dimensions, param.meta.default_value);
      }
    }

    static ClosureSignature::ItemUIData glsl_closure_item_ui_data(
      const GLSLFunctionParam& param,
      const bool w_component)
    {
      ClosureSignature::ItemUIData ui;
      if (param.meta.label.has_value())
      {
        ui.label = w_component ? make_split_vec4_w_socket_name(*param.meta.label) :
                               *param.meta.label;
      }
      ui.description = param.meta.description;
      if (!w_component)
      {
        ui.subtype = param.meta.subtype;
      }
      if (param.type == GLSLBoundaryType::Int)
      {
        ui.min_value = ClosureSignature::ItemMinMaxValue(-glsl_closure_exact_int_limit);
        ui.max_value = ClosureSignature::ItemMinMaxValue(glsl_closure_exact_int_limit);
      }
      return ui;
    }

    static void add_glsl_closure_signature_item(ClosureSignature& signature,
      const GLSLFunctionParam& param,
      const StringRef key,
      const bool is_output,
      const bool w_component = false)
    {
      eNodeSocketDatatype socket_type = SOCK_FLOAT;
      int dimensions = 0;
      if (!w_component)
      {
        switch (param.type)
        {
        case GLSLBoundaryType::Int:
          socket_type = SOCK_INT;
          break;
        case GLSLBoundaryType::Bool:
          socket_type = SOCK_BOOLEAN;
          break;
        case GLSLBoundaryType::Vec2:
          socket_type = SOCK_VECTOR;
          dimensions = 2;
          break;
        case GLSLBoundaryType::Vec3:
          if (glsl_param_uses_color_socket(param))
          {
            socket_type = SOCK_RGBA;
          }
          else
          {
            socket_type = SOCK_VECTOR;
            dimensions = 3;
          }
          break;
        case GLSLBoundaryType::Vec4:
          socket_type = SOCK_VECTOR;
          dimensions = 3;
          break;
        case GLSLBoundaryType::Float:
        default:
          socket_type = SOCK_FLOAT;
          break;
        }
      }

      ClosureSignature::Item item;
      item.key = key;
      item.type = bke::node_socket_type_find_static(socket_type);
      item.structure_type = NodeSocketInterfaceStructureType::Single;
      item.dimensions = dimensions;
      item.ui = glsl_closure_item_ui_data(param, w_component);
      if (is_output)
      {
        signature.outputs.add(std::move(item));
      }
      else
      {
        signature.inputs.add(std::move(item));
      }
    }

    static ClosureSignature build_glsl_closure_signature(const GLSLClosureCallback& callback)
    {
      ClosureSignature signature;
      auto add_param = [&](const GLSLFunctionParam& param,
                           const StringRef key,
                           const bool is_output) {
        add_glsl_closure_signature_item(signature, param, key, is_output);
        if (param.type == GLSLBoundaryType::Vec4)
        {
          add_glsl_closure_signature_item(
            signature, param, std::string(key) + ".__w", is_output, true);
        }
      };

      if (callback.function.return_type != GLSLBoundaryType::Void)
      {
        GLSLFunctionParam result;
        result.name = "Result";
        result.type = callback.function.return_type;
        result.type_name = callback.function.return_type_name;
        result.dimensions = glsl_boundary_dimensions(result.type);
        result.qualifier = GLSLFunctionParam::Qualifier::Out;
        result.meta = callback.function.return_meta;
        add_param(result, "Result", true);
      }
      for (const GLSLFunctionParam& param : callback.function.params)
      {
        add_param(param, param.name, param.qualifier == GLSLFunctionParam::Qualifier::Out);
      }
      return signature;
    }

    static void add_glsl_closure_declarations(DeclarationListBuilder& builder,
      const GLSLParseResult& parse_result)
    {
      for (const GLSLClosureCallback& callback : parse_result.closure_callbacks)
      {
        const ClosureSignature signature = build_glsl_closure_signature(callback);
        auto& decl = builder.add_input<decl::Closure>(
          UString(callback.label.value_or(callback.function.name)),
          UString("closure." + callback.function.name));
        if (callback.description.has_value())
        {
          decl.description(*callback.description);
        }
        decl.create_signature([signature](const bNode& /*node*/) { return signature; });
      }
    }

    static void unregister_glsl_param_int_choices(bNode& node)
    {
      for (bNodeSocket* socket : node.input_sockets())
      {
        if (socket->type != SOCK_INT)
        {
          continue;
        }
        RNA_node_socket_glsl_int_choices_unregister(socket);
      }
    }

    static void register_glsl_param_int_choices(
      bNodeSocket& socket,
      const Span<GLSLFunctionParam::IntChoiceItem> choices)
    {
      if (choices.is_empty())
      {
        RNA_node_socket_glsl_int_choices_unregister(&socket);
        return;
      }

      Vector<int> values;
      Vector<const char*> labels;
      values.reserve(choices.size());
      labels.reserve(choices.size());
      for (const GLSLFunctionParam::IntChoiceItem& choice : choices)
      {
        values.append(choice.value);
        labels.append(choice.label.c_str());
      }
      RNA_node_socket_glsl_int_choices_register(
        &socket, values.data(), labels.data(), int(choices.size()));
    }

    static void sync_glsl_param_int_choices(bNode& node, const GLSLParseResult& parse_result)
    {
      unregister_glsl_param_int_choices(node);
      if (!parse_result.ok)
      {
        return;
      }

      for (const GLSLFunctionParam& param : parse_result.function.params)
      {
        if (param.type != GLSLBoundaryType::Int || param.meta.int_choices.is_empty())
        {
          continue;
        }
        bNodeSocket* socket = find_node_input_socket_by_identifier(node, param.identifier);
        if (socket == nullptr || socket->type != SOCK_INT)
        {
          continue;
        }
        register_glsl_param_int_choices(*socket, param.meta.int_choices);
      }
    }

    static void sync_glsl_meta_defaults(bNode& node, const GLSLParseResult& parse_result)
    {
      NodeShaderGLSLFunction& storage = node_storage(node);
      if (!parse_result.ok)
      {
        unregister_glsl_param_int_choices(node);
        storage.meta_hash = 0;
        return;
      }
      sync_glsl_param_int_choices(node, parse_result);
      for (const GLSLClosureCallback& callback : parse_result.closure_callbacks)
      {
        bNodeSocket* socket = find_node_input_socket_by_identifier(
          node, "closure." + callback.function.name);
        if (socket == nullptr || socket->type != SOCK_CLOSURE)
        {
          continue;
        }
        if (callback.description.has_value())
        {
          BLI_strncpy_utf8(
            socket->description, callback.description->c_str(), sizeof(socket->description));
        }
        else
        {
          socket->description[0] = '\0';
        }
      }
      if (storage.meta_hash == parse_result.meta_hash)
      {
        return;
      }

      /* Socket declarations already initialize defaults for newly created sockets. Re-applying
       * GLSL meta defaults here would overwrite user-edited values on refresh for unchanged
       * parameters with the same identifier. */
      for (const GLSLFunctionParam& param : parse_result.function.params)
      {
        if (param.type != GLSLBoundaryType::Int || param.meta.int_choices.is_empty())
        {
          continue;
        }
        bNodeSocket* socket = find_node_input_socket_by_identifier(node, param.identifier);
        if (socket == nullptr || socket->type != SOCK_INT || socket->default_value == nullptr)
        {
          continue;
        }
        bNodeSocketValueInt& value = *static_cast<bNodeSocketValueInt*>(socket->default_value);
        if (!glsl_int_choice_contains_value(param.meta.int_choices, value.value))
        {
          BLI_assert(param.meta.int_default_value.has_value());
          value.value = param.meta.int_default_value.value_or(0);
        }
      }
      storage.meta_hash = parse_result.meta_hash;
    }

    static void draw_glsl_define_settings(ui::Layout& layout,
                                          PointerRNA* ptr,
                                          const GLSLParseResult& parse_result);

    static void node_declare(NodeDeclarationBuilder& b)
    {
      const bNode* node = b.node_or_null();
      if (node == nullptr)
      {
        return;
      }

      const GLSLParseResult parse_result = parse_glsl_for_node(*node);
      if (!parse_result.ok)
      {
        b.declaration().skip_updating_sockets = parse_result.keep_existing_sockets;
        return;
      }

      const bool code_mode = (node_storage(*node).flags & SHD_GLSL_FUNCTION_CODE_MODE) != 0;
      const bool has_parameter_panels = !parse_result.function.panels.is_empty();
      const bool has_define_panel = !parse_result.defines.is_empty();
      const bool has_closure_panel = !parse_result.closure_callbacks.is_empty();
      const bool use_declaration_panels = has_parameter_panels || has_define_panel ||
        has_closure_panel;
      if (code_mode || use_declaration_panels)
      {
        b.use_custom_socket_order();
        b.allow_any_socket_order();
      }

      auto add_output_declarations = [&](DeclarationListBuilder& builder) {
        if (parse_result.function.return_type != GLSLBoundaryType::Void)
        {
          GLSLFunctionParam output_param;
          output_param.type = parse_result.function.return_type;
          output_param.type_name = parse_result.function.return_type_name;
          output_param.dimensions = glsl_boundary_dimensions(parse_result.function.return_type);
          add_glsl_socket_declaration(
            builder, output_param, true, "Result"_ustr, UString(result_socket_identifier));
        }

        for (const GLSLFunctionParam& param : parse_result.function.params)
        {
          if (glsl_param_has_output_socket(param))
          {
            const UString socket_name(glsl_socket_display_name(param));
            const UString socket_identifier(make_socket_identifier("Out", param.name));
            add_glsl_socket_declaration(
              builder, param, true, socket_name, socket_identifier);
          }
        }
      };

      auto add_closure_panel = [&]() {
        if (!has_closure_panel)
        {
          return;
        }
        PanelDeclarationBuilder& closures_panel =
          b.add_panel("Closures"_ustr, make_closures_panel_identifier());
        add_glsl_closure_declarations(closures_panel, parse_result);
      };

      if (code_mode)
      {
        b.add_default_layout();
        PanelDeclarationBuilder& interface_panel =
          b.add_panel("Interface"_ustr, make_code_interface_panel_identifier())
            .default_closed(true);
        add_output_declarations(interface_panel);
        for (const GLSLFunctionParam& param : parse_result.function.params)
        {
          if (!glsl_param_has_input_socket(param))
          {
            continue;
          }
          add_glsl_socket_declaration(interface_panel,
                                      param,
                                      false,
                                      UString(glsl_socket_display_name(param)),
                                      UString(param.identifier));
        }
        add_closure_panel();
        return;
      }

      if (use_declaration_panels)
      {
        add_output_declarations(b);
        b.add_default_layout();
      }

      if (has_define_panel)
      {
        PanelDeclarationBuilder& defines_panel =
          b.add_panel("Defines"_ustr,
                      make_defines_panel_identifier(parse_result.defines_panel_default_closed))
            .default_closed(parse_result.defines_panel_default_closed);
        defines_panel.add_layout([parse_result](ui::Layout& layout,
                                                bContext* /*C*/,
                                                PointerRNA* ptr) {
          draw_glsl_define_settings(layout, ptr, parse_result);
        });
      }

      Map<std::string, int> panel_indices;
      for (const int panel_index : parse_result.function.panels.index_range())
      {
        const GLSLPanelMeta& panel = parse_result.function.panels[panel_index];
        panel_indices.add(panel.name, panel_index);
      }
      Map<std::string, PanelDeclarationBuilder*> panel_builders;
      auto ensure_panel_builder = [&](const std::string& panel_name) -> PanelDeclarationBuilder* {
        if (PanelDeclarationBuilder** existing_builder = panel_builders.lookup_ptr(panel_name))
        {
          return *existing_builder;
        }
        const int* panel_index = panel_indices.lookup_ptr(panel_name);
        if (panel_index == nullptr)
        {
          return nullptr;
        }
        const GLSLPanelMeta& panel = parse_result.function.panels[*panel_index];
        PanelDeclarationBuilder& panel_builder =
          b.add_panel(UString(panel.name), make_panel_identifier(panel.name)).default_closed(
            panel.default_closed);
        panel_builders.add(panel.name, &panel_builder);
        return &panel_builder;
      };

      for (const GLSLFunctionParam& param : parse_result.function.params)
      {
        if (glsl_param_has_input_socket(param))
        {
          const UString socket_name(glsl_socket_display_name(param));
          const UString socket_identifier(param.identifier);
          if (param.meta.panel_name.has_value())
          {
            if (PanelDeclarationBuilder* panel_builder = ensure_panel_builder(
                  *param.meta.panel_name))
            {
              add_glsl_socket_declaration(
                *panel_builder, param, false, socket_name, socket_identifier);
              continue;
            }
          }
          add_glsl_socket_declaration(b, param, false, socket_name, socket_identifier);
        }
      }

      add_closure_panel();

      if (!use_declaration_panels)
      {
        add_output_declarations(b);
      }
    }

    static void draw_display_mode_switch(ui::Layout& layout, PointerRNA* ptr)
    {
      ui::Layout& row = layout.row(true);
      row.prop(ptr, "display_mode", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
    }

    static GLSLParseResult draw_node_layout_content(ui::Layout& layout, bContext* C, PointerRNA* ptr)
    {
      bNode& node = *static_cast<bNode*>(ptr->data);
      layout.use_property_split_set(false);
      layout.use_property_decorate_set(false);
      draw_display_mode_switch(layout, ptr);

      layout.use_property_split_set(true);
      layout.use_property_decorate_set(false);

      {
        ui::Layout& row = layout.row(false);
        row.prop(
          ptr, "source_mode", ui::ITEM_R_SPLIT_EMPTY_NAME | ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
      }

      {
        const bool is_internal = RNA_enum_get(ptr, "source_mode") == SHD_GLSL_FUNCTION_SOURCE_INTERNAL;
        const bool has_script = RNA_pointer_get(ptr, "script").data != nullptr;

        if (is_internal && !has_script)
        {
          ui::Layout &split = layout.split(0.4f, false);
          split.use_property_split_set(false);
          split.use_property_decorate_set(false);

          ui::Layout &label_row = split.row(false);
          label_row.use_property_split_set(false);
          label_row.use_property_decorate_set(false);
          label_row.label("", ICON_NONE);

          ui::Layout &field_row = split.row(true);
          field_row.use_property_split_set(false);
          field_row.use_property_decorate_set(false);
          field_row.op("node.glsl_function_new_text", "", ICON_ADD);
          if (C != nullptr)
          {
            template_id(&field_row, C, ptr, "script", nullptr, nullptr, nullptr);
          }
          else
          {
            field_row.prop(ptr, "script", UI_ITEM_NONE, "", ICON_NONE);
          }
          field_row.op("node.glsl_function_reset_defaults", "", ICON_LOOP_BACK);
          field_row.op("node.glsl_function_refresh", "", ICON_FILE_REFRESH);
        }
        else
        {
          ui::Layout& row = layout.row(true);
          row.use_property_split_set(false);
          row.use_property_decorate_set(false);

          ui::Layout& source_row = row.row(true);
          if (is_internal)
          {
            if (C != nullptr)
            {
              template_id(&source_row, C, ptr, "script", nullptr, nullptr, nullptr);
            }
            else
            {
              source_row.prop(ptr, "script", UI_ITEM_NONE, "", ICON_NONE);
            }
          }
          else
          {
            source_row.prop(ptr, "filepath", UI_ITEM_NONE, "", ICON_NONE);
          }
          row.op("node.glsl_function_reset_defaults", "", ICON_LOOP_BACK);
          row.op("node.glsl_function_refresh", "", ICON_FILE_REFRESH);
        }
      }

      ui::Layout& function_row = layout.row(false);
      function_row.prop(ptr, "function_name", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);

      const NodeShaderGLSLFunction& storage = node_storage(node);
      const GLSLParseResult parse_result = parse_glsl_for_node(node);
      cache_parse_status(node, parse_result);
      sync_glsl_define_values(node, parse_result);
      sync_glsl_meta_defaults(node, parse_result);

      if (parse_result.ok)
      {
        std::string label = "Using function: " + parse_result.resolved_function_name;
        layout.label(label.c_str(), ICON_NONE);
      }
      else if (storage.function_name[0] == '\0' && !parse_result.function_names.is_empty())
      {
        layout.label(IFACE_("Choose a function"), ICON_NONE);
      }
      else if (!parse_result.error.empty())
      {
        layout.label(parse_result.error.c_str(), ICON_ERROR);
      }

      return parse_result;
    }

    static void draw_code_layout_content(ui::Layout& layout, bContext* C, PointerRNA* ptr)
    {
      layout.use_property_split_set(false);
      layout.use_property_decorate_set(false);

      bNode& node = *static_cast<bNode*>(ptr->data);
      const NodeShaderGLSLFunction& storage = node_storage(node);
      const bool is_internal = storage.source_mode == SHD_GLSL_FUNCTION_SOURCE_INTERNAL;
      const bool has_script = node.id != nullptr;
      draw_display_mode_switch(layout, ptr);

      ui::Layout& toolbar = layout.row(true);
      ui::Layout& function_field = toolbar.row(true);
      function_field.enabled_set(!is_internal || has_script);
      const GLSLParseResult parse_result = parse_glsl_for_node(node);
      function_field.red_alert_set(!parse_result.ok && storage.function_name[0] != '\0');
      function_field.prop(ptr, "function_name", UI_ITEM_NONE, "", ICON_NONE);
      toolbar.op("node.glsl_function_refresh", "", ICON_FILE_REFRESH);

      ui::Layout& editor = layout.column(false);
      editor.enabled_set(is_internal && has_script && ID_IS_EDITABLE(node.id));
      if (C != nullptr)
      {
        editor.textbox(C, ptr, "source_code", std::nullopt, 10);
      }
      else
      {
        editor.prop(ptr, "source_code", UI_ITEM_NONE, "", ICON_NONE);
      }

    }

    static void draw_glsl_define_settings(ui::Layout& layout,
                                          PointerRNA* ptr,
                                          const GLSLParseResult& parse_result)
    {
      if (parse_result.defines.is_empty())
      {
        return;
      }

      bNode& node = *static_cast<bNode*>(ptr->data);
      NodeShaderGLSLFunction& storage = node_storage(node);
      if (storage.define_values == nullptr)
      {
        return;
      }

      layout.use_property_split_set(true);
      layout.use_property_decorate_set(false);

      for (const GLSLDefineMeta& define : parse_result.defines)
      {
        NodeShaderGLSLDefineValue* value = nullptr;
        for (const int i : IndexRange(storage.define_values_num))
        {
          NodeShaderGLSLDefineValue& candidate = storage.define_values[i];
          if (candidate.type == define.type && define.name == candidate.name)
          {
            value = &candidate;
            break;
          }
        }
        if (value == nullptr)
        {
          continue;
        }

        PointerRNA value_ptr = RNA_pointer_create_discrete(
          ptr->owner_id, RNA_ShaderNodeGLSLDefineValue, value);
        const std::string label = define.label.value_or(define.name);
        if (define.type == SHD_GLSL_FUNCTION_DEFINE_INT && !define.int_choices.is_empty())
        {
          register_glsl_define_int_choices(*value, define.int_choices);
          layout.prop(&value_ptr,
                      "choice_value",
                      UI_ITEM_NONE,
                      define.int_choices_show_label ? label.c_str() : "",
                      ICON_NONE);
        }
        else
        {
          RNA_shader_node_glsl_define_value_choices_unregister(value, 1);
          const char* prop_name = define.type == SHD_GLSL_FUNCTION_DEFINE_BOOL ? "bool_value" :
                                                                                  "int_value";
          layout.prop(&value_ptr, prop_name, UI_ITEM_NONE, label.c_str(), ICON_NONE);
        }
        if (define.description.has_value() && !define.description->empty())
        {
          ui::Layout& description_row = layout.row(false);
          description_row.use_property_split_set(false);
          description_row.active_set(false);
          description_row.label(define.description->c_str(), ICON_NONE);
        }
      }
    }

    static void draw_glsl_define_panel(ui::Layout& layout,
                                       bContext* C,
                                       PointerRNA* ptr,
                                       const GLSLParseResult& parse_result,
                                       const char* panel_id)
    {
      if (C == nullptr || !parse_result.ok || parse_result.defines.is_empty())
      {
        return;
      }
      if (ui::Layout* panel = layout.panel(
            C, panel_id, parse_result.defines_panel_default_closed, IFACE_("Defines")))
      {
        draw_glsl_define_settings(*panel, ptr, parse_result);
      }
    }

    static void node_layout(ui::Layout& layout, bContext* C, PointerRNA* ptr)
    {
      const bNode& node = *static_cast<const bNode*>(ptr->data);
      if ((node_storage(node).flags & SHD_GLSL_FUNCTION_CODE_MODE) != 0)
      {
        draw_code_layout_content(layout, C, ptr);
        return;
      }
      draw_node_layout_content(layout, C, ptr);
    }

    static void node_layout_ex(ui::Layout& layout, bContext* C, PointerRNA* ptr)
    {
      const bNode& node = *static_cast<const bNode*>(ptr->data);
      if ((node_storage(node).flags & SHD_GLSL_FUNCTION_CODE_MODE) != 0)
      {
        draw_code_layout_content(layout, C, ptr);
        return;
      }
      const GLSLParseResult parse_result = draw_node_layout_content(layout, C, ptr);
      draw_glsl_define_panel(layout,
                             C,
                             ptr,
                             parse_result,
                             parse_result.defines_panel_default_closed ?
                               "glsl_function_defines_closed" :
                               "glsl_function_defines");
    }

    static void node_init(bNodeTree* /*ntree*/, bNode* node)
    {
      node->storage = MEM_new<NodeShaderGLSLFunction>(__func__);
    }

    static bool prepare_sampler_input_bindings(GPUMaterial* mat,
      const bNode& node,
      const GLSLFunctionDefinition& function,
      GPUNodeStack* in)
    {
      static float zero_value[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

      if (in == nullptr || !glsl_function_has_sampler_inputs(function))
      {
        return true;
      }

      auto find_input_stack_by_identifier = [&](const StringRef identifier) -> GPUNodeStack*
        {
          int input_index = 0;
          for (const bNodeSocket* socket : node.input_sockets())
          {
            if (identifier == socket->identifier)
            {
              return &in[input_index];
            }
            input_index++;
          }
          return nullptr;
        };

      auto resolve_sample2d_image = [&](const GLSLFunctionParam& param) -> Image*
        {
          const bNodeLink* used_link = nullptr;
          GLSLSamplerSourceKind source_kind = resolve_sampler_source_kind(node, param, used_link);
          if (source_kind == GLSLSamplerSourceKind::GLSLFunction && used_link != nullptr)
          {
            source_kind = resolve_nested_sampler_source_kind(param, *used_link, used_link);
          }
          if (source_kind != GLSLSamplerSourceKind::ImageToClosure || used_link == nullptr)
          {
            return nullptr;
          }
          if (used_link->fromnode == nullptr ||
              image_to_closure_texture_type(*used_link->fromnode) != IMA_IMAGE_TO_CLOSURE_TEXTURE_2D)
          {
            return nullptr;
          }
          return resolve_image_to_closure_image(*used_link);
        };

      auto resolve_sample2d_sampler_state = [&](const GLSLFunctionParam& param) -> GPUSamplerState
        {
          const bNodeLink* used_link = nullptr;
          GLSLSamplerSourceKind source_kind = resolve_sampler_source_kind(node, param, used_link);
          if (source_kind == GLSLSamplerSourceKind::GLSLFunction && used_link != nullptr)
          {
            source_kind = resolve_nested_sampler_source_kind(param, *used_link, used_link);
          }
          if (source_kind == GLSLSamplerSourceKind::ImageToClosure && used_link != nullptr &&
            used_link->fromnode != nullptr)
          {
            return sampler_state_from_image_to_closure_node(*used_link->fromnode);
          }
          return GPUSamplerState::default_sampler();
        };

      auto resolve_image_to_closure_link = [&](const GLSLFunctionParam& param) -> const bNodeLink*
        {
          const bNodeLink* used_link = nullptr;
          GLSLSamplerSourceKind source_kind = resolve_sampler_source_kind(node, param, used_link);
          if (source_kind == GLSLSamplerSourceKind::GLSLFunction && used_link != nullptr)
          {
            source_kind = resolve_nested_sampler_source_kind(param, *used_link, used_link);
          }
          if (source_kind != GLSLSamplerSourceKind::ImageToClosure || used_link == nullptr)
          {
            return nullptr;
          }
          return used_link;
        };

      for (const GLSLFunctionParam& param : function.params)
      {
        if (!glsl_param_has_input_socket(param))
        {
          continue;
        }

        if (glsl_boundary_type_is_sampler(param.type))
        {
          const std::string socket_identifier = param.identifier;
          GPUNodeStack* stack = find_input_stack_by_identifier(socket_identifier);
          if (stack == nullptr)
          {
            return false;
          }

          Image* image = nullptr;
          const bNodeLink* sampler_link = nullptr;
          GLSLSamplerSourceKind source_kind = resolve_sampler_source_kind(
            node, param, sampler_link);
          if (source_kind == GLSLSamplerSourceKind::GLSLFunction && sampler_link != nullptr)
          {
            source_kind = resolve_nested_sampler_source_kind(param, *sampler_link, sampler_link);
          }
          if (source_kind == GLSLSamplerSourceKind::ClosureOutput)
          {
            stack->type = GPU_FLOAT;
            stack->link = GPU_constant(zero_value);
            continue;
          }

          if (glsl_boundary_type_is_sample2d(param.type))
          {
            image = resolve_sample2d_image(param);
          }
          else if (glsl_boundary_type_is_sample3d(param.type))
          {
            const bNodeLink* used_link = resolve_image_to_closure_link(param);
            if (used_link == nullptr || used_link->fromnode == nullptr ||
                image_to_closure_texture_type(*used_link->fromnode) !=
                    IMA_IMAGE_TO_CLOSURE_TEXTURE_3D_LUT_STRIP)
            {
              return false;
            }
            image = resolve_image_to_closure_image(*used_link);
            if (image == nullptr || image->source == IMA_SRC_TILED)
            {
              return false;
            }
            ResolvedLut3DStripDimensions dimensions;
            std::string lut_error;
            if (!resolve_3d_lut_strip_dimensions(
                  *used_link->fromnode, *image, dimensions, lut_error))
            {
              return false;
            }
            stack->type = GPU_TEX3D;
            stack->link = GPU_image_3d_lut_strip(mat,
              image,
              nullptr,
              dimensions.width,
              dimensions.height,
              dimensions.depth,
              sampler_state_from_image_to_closure_node(*used_link->fromnode));
            continue;
          }

          if (image == nullptr || image->source == IMA_SRC_TILED)
          {
            return false;
          }

          stack->type = GPU_TEX2D;
          stack->link = GPU_image(mat, image, nullptr, resolve_sample2d_sampler_state(param));
        }
      }

      return true;
    }

    static void node_update(bNodeTree* /*ntree*/, bNode* node)
    {
      const GLSLParseResult parse_result = parse_glsl_for_node(*node);
      cache_parse_status(*node, parse_result);
      sync_glsl_define_values(*node, parse_result);
      sync_glsl_meta_defaults(*node, parse_result);
    }

    static std::optional<std::string> find_connected_glsl_closure_callback_name(
      const bNode& node, const StringRef error)
    {
      const bNode* original_node = node.runtime->original;
      Vector<std::string> connected_helpers;
      const auto gather_connected_helpers = [&](const bNode& candidate_node) {
        for (const bNodeSocket* socket : candidate_node.input_sockets())
        {
          const StringRef identifier(socket->identifier);
          if (socket->type != SOCK_CLOSURE || !identifier.startswith("closure.") ||
              find_used_direct_link(*socket) == nullptr)
          {
            continue;
          }
          const std::string helper_name = identifier.drop_prefix(8);
          if (std::find(connected_helpers.begin(), connected_helpers.end(), helper_name) ==
              connected_helpers.end())
          {
            connected_helpers.append(helper_name);
          }
        }
      };

      gather_connected_helpers(node);
      if (original_node != nullptr && original_node != &node)
      {
        gather_connected_helpers(*original_node);
      }
      for (const std::string& helper_name : connected_helpers)
      {
        const std::string quoted_helper = "'" + helper_name + "'";
        if (error.find(quoted_helper) != StringRef::not_found)
        {
          return helper_name;
        }
      }
      return connected_helpers.is_empty() ? std::nullopt :
                                            std::make_optional(connected_helpers.first());
    }

    static int fail_glsl_closure_callback_material_compile(
      GPUMaterial* mat,
      bNode* node,
      const StringRef helper_name,
      const StringRef error,
      GPUNodeStack* out)
    {
      CLOG_ERROR(&LOG,
                 "GLSL Function material '%s' node '%s' closure callback '%s' failed: %.*s",
                 glsl_function_material_name(mat),
                 node->name,
                 std::string(helper_name).c_str(),
                 int(error.size()),
                 error.data());

      /* Keep a custom node call in the graph and make its generated dependency fail shader
       * compilation explicitly. This also works when parsing failed and no regular wrapper exists. */
      const std::string failure_name = "glsl_closure_callback_failure_" +
        std::to_string(node->identifier);
      const std::string failure_filename = failure_name + ".glsl";
      const std::string error_source = "#error GLSL_closure_callback_" +
        std::string(helper_name) + "_failed\n";
      GPU_material_generated_source_add(
        mat, failure_filename.c_str(), {}, error_source.c_str());
      return GPU_stack_link_custom(mat,
                                   node,
                                   failure_name.c_str(),
                                   failure_filename.c_str(),
                                   GPU_CUSTOM_NODE_DEPENDENCY_NONE,
                                   nullptr,
                                   out);
    }

    static bool node_insert_link(bke::NodeInsertLinkParams& params)
    {
      if (params.link.tonode != &params.node || params.link.tosock->type != SOCK_CLOSURE ||
        params.link.fromnode == nullptr || !params.link.fromnode->is_type("NodeClosureOutput"_ustr))
      {
        return true;
      }
      if (params.C == nullptr)
      {
        return true;
      }
      SpaceNode* snode = CTX_wm_space_node(params.C);
      if (snode == nullptr || snode->edittree != &params.ntree)
      {
        return true;
      }

      bNode& closure_output_node = *params.link.fromnode;
      const bke::bNodeZoneType* closure_zone_type = bke::zone_type_by_node_type(NODE_CLOSURE_OUTPUT);
      if (closure_zone_type == nullptr)
      {
        return true;
      }
      bNode* closure_input_node = closure_zone_type->get_corresponding_input(params.ntree,
        closure_output_node);
      if (closure_input_node == nullptr)
      {
        return true;
      }

      sync_sockets_closure(*snode, *closure_input_node, closure_output_node, nullptr, params.link.fromsock);
      return true;
    }

    static int gpu_shader_glsl_function(GPUMaterial* mat,
      bNode* node,
      bNodeExecData* /*execdata*/,
      GPUNodeStack* in,
      GPUNodeStack* out)
    {
      if (!active_closure_helper_nodes.is_empty())
      {
        const bNode* logical_node = node->runtime->original ? node->runtime->original : node;
        for (const bNode* active_logical_node : active_closure_helper_nodes)
        {
          if (active_logical_node == logical_node)
          {
            if (GPU_material_closure_uv_source_get(mat).is_empty())
            {
              GPU_material_closure_callback_input_frame_error_set(
                mat,
                "Recursive GLSL Function dependency reached node '" +
                  std::string(logical_node->name) + "'");
              CLOG_ERROR(&LOG,
                         "Recursive GLSL closure callback dependency reached node '%s'",
                         logical_node->name);
              return 0;
            }
            static float zero_value[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            if (out != nullptr)
            {
              for (int i = 0; !out[i].end; i++)
              {
                if (out[i].type != GPU_NONE)
                {
                  out[i].link = GPU_constant(zero_value);
                }
              }
            }
            return 1;
          }
        }
      }

      const GLSLParseResult parse_result = parse_glsl_for_node(*node);
      cache_parse_status(*node, parse_result);
      sync_glsl_define_values(*node, parse_result);
      sync_glsl_meta_defaults(*node, parse_result);

      if (!parse_result.ok)
      {
        if (const std::optional<std::string> helper_name =
              find_connected_glsl_closure_callback_name(*node, parse_result.error))
        {
          return fail_glsl_closure_callback_material_compile(
            mat, node, *helper_name, parse_result.error, out);
        }
        if (GPU_material_closure_callback_input_frame_error_set(
              mat,
              "GLSL Function node '" + std::string(node->name) +
                "' could not parse: " + parse_result.error))
        {
          return 0;
        }
        static float zero_value[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        if (out != nullptr)
        {
          for (int i = 0; !out[i].end; i++)
          {
            if (out[i].type != GPU_NONE)
            {
              out[i].link = GPU_constant(zero_value);
            }
          }
        }
        return 1;
      }
      if (parse_result.uses_eevee_light_access)
      {
        GPU_material_flag_set(mat, GPU_MATFLAG_GLSL_LIGHT_ACCESS);
      }
      if (parse_result.uses_lightprobe_access)
      {
        GPU_material_flag_set(mat, GPU_MATFLAG_LIGHTPROBE_ACCESS);
      }
      if (!prepare_sampler_input_bindings(mat, *node, parse_result.function, in))
      {
        if (const std::optional<std::string> helper_name =
              find_connected_glsl_closure_callback_name(*node, {}))
        {
          return fail_glsl_closure_callback_material_compile(
            mat, node, *helper_name, "sampler input preparation failed", out);
        }
        if (GPU_material_closure_callback_input_frame_error_set(
              mat,
              "GLSL Function node '" + std::string(node->name) +
                "' sampler input preparation failed"))
        {
          return 0;
        }
        CLOG_WARN(&LOG,
          "GLSL Function material '%s' node '%s' sampler preparation failed",
          glsl_function_material_name(mat),
          node->name);
        static float zero_value[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        if (out != nullptr)
        {
          for (int i = 0; !out[i].end; i++)
          {
            if (out[i].type != GPU_NONE)
            {
              out[i].link = GPU_constant(zero_value);
            }
          }
        }
        return 1;
      }

      std::string library_source = parse_result.library_source;
      std::string wrapper_source = parse_result.wrapper_source;
      std::string specialized_error;
      if (!build_specialized_glsl_sources(
        mat, *node, parse_result, library_source, wrapper_source, specialized_error))
      {
        if (const std::optional<std::string> helper_name =
              find_connected_glsl_closure_callback_name(*node, specialized_error))
        {
          return fail_glsl_closure_callback_material_compile(
            mat, node, *helper_name, specialized_error, out);
        }
        if (GPU_material_closure_callback_input_frame_error_set(
              mat,
              "GLSL Function node '" + std::string(node->name) +
                "' specialization failed: " + specialized_error))
        {
          return 0;
        }
        CLOG_WARN(&LOG,
          "GLSL Function material '%s' node '%s' specialized source build failed: %s",
          glsl_function_material_name(mat),
          node->name,
          specialized_error.c_str());
        static float zero_value[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        if (out != nullptr)
        {
          for (int i = 0; !out[i].end; i++)
          {
            if (out[i].type != GPU_NONE)
            {
              out[i].link = GPU_constant(zero_value);
            }
          }
        }
        return 1;
      }
      if (parse_result.uses_geometry_access)
      {
        const std::string geometry_helper_source = build_glsl_geometry_helper_block();
        GPU_material_generated_source_add(
          mat, GPU_GLSL_FUNCTION_GEOMETRY_HELPER_FILENAME, {}, geometry_helper_source.c_str());
      }
      if (parse_result.uses_lightprobe_access)
      {
        const std::string lightprobe_helper_source = build_glsl_lightprobe_helper_block();
        Vector<StringRefNull> lightprobe_helper_dependencies;
        lightprobe_helper_dependencies.append(glsl_light_probe_color_helper_filename);
        GPU_material_generated_source_add(
          mat,
          GPU_GLSL_FUNCTION_LIGHTPROBE_HELPER_FILENAME,
          lightprobe_helper_dependencies,
          lightprobe_helper_source.c_str());
      }
      GPU_material_generated_source_add(
        mat,
        parse_result.source_filename.c_str(),
        {},
        library_source.c_str());

      Vector<StringRefNull> dependencies;
      if (parse_result.uses_eevee_light_access)
      {
        /* Share one helper library across all GLSL Function nodes in the material and let the
         * dependency resolver deduplicate Eevee light/shadow support code. */
        dependencies.append(glsl_light_access_helper_filename);
      }
      dependencies.append(parse_result.source_filename.c_str());
      eGPUCustomNodeDependencyFlag dependency_flags = GPU_CUSTOM_NODE_DEPENDENCY_NONE;
      if (parse_result.uses_geometry_access)
      {
        dependency_flags = eGPUCustomNodeDependencyFlag(
          dependency_flags | GPU_CUSTOM_NODE_DEPENDENCY_GLSL_GEOMETRY_HELPERS);
      }
      if (parse_result.uses_lightprobe_access)
      {
        dependency_flags = eGPUCustomNodeDependencyFlag(
          dependency_flags | GPU_CUSTOM_NODE_DEPENDENCY_GLSL_LIGHTPROBE_HELPERS);
      }
      GPU_material_generated_source_add(
        mat,
        parse_result.wrapper_filename.c_str(),
        dependencies,
        wrapper_source.c_str());

      const int link_result = GPU_stack_link_custom(mat,
                                                    node,
                                                    parse_result.wrapper_name.c_str(),
                                                    parse_result.wrapper_filename.c_str(),
                                                    dependency_flags,
                                                    in,
                                                    out);
      if (link_result == 0)
      {
        if (const std::optional<std::string> helper_name =
              find_connected_glsl_closure_callback_name(*node, {}))
        {
          return fail_glsl_closure_callback_material_compile(
            mat, node, *helper_name, "generated wrapper graph link failed", out);
        }
        GPU_material_closure_callback_input_frame_error_set(
          mat,
          "GLSL Function node '" + std::string(node->name) +
            "' generated wrapper graph link failed");
      }
      return link_result;
    }

  }  // namespace nodes::node_shader_glsl_function_cc

  bool node_shader_glsl_function_source_get(const bNode& node,
                                            std::string& r_source,
                                            std::string& r_error)
  {
    return nodes::node_shader_glsl_function_cc::load_glsl_source(node, r_source, r_error);
  }

  bool node_shader_glsl_function_code_source_ensure(Main& bmain,
                                                    bNode& node,
                                                    bool& r_changed,
                                                    std::string& r_error)
  {
    static constexpr char default_source[] =
      "/* @glsl_meta v1\n"
      "color: label=\"Color\" default=vec4(1.0) subtype=color\n"
      "*/\n"
      "vec4 glsl_function(vec4 color)\n"
      "{\n"
      "  return color;\n"
      "}\n";

    r_changed = false;
    NodeShaderGLSLFunction* storage = static_cast<NodeShaderGLSLFunction*>(node.storage);
    if (storage == nullptr || storage->source_mode != SHD_GLSL_FUNCTION_SOURCE_INTERNAL)
    {
      r_error.clear();
      return true;
    }

    Text* text = reinterpret_cast<Text*>(node.id);
    if (text == nullptr)
    {
      text = BKE_text_add(&bmain, "GLSL Function.glsl");
      node.id = &text->id;
      id_us_plus(&text->id);
      r_changed = true;
    }

    if (storage->edit_source != nullptr)
    {
      if (!ID_IS_EDITABLE(&text->id))
      {
        r_error = "The GLSL Text data-block is not editable";
        return false;
      }
      BKE_text_clear(text);
      BKE_text_write(text, storage->edit_source, int(strlen(storage->edit_source)));
      MEM_SAFE_DELETE(storage->edit_source);
      storage->edit_source_session_uid = 0;
      storage->edit_source_hash = 0;
      storage->parse_status = SHD_GLSL_FUNCTION_PARSE_DIRTY;
      storage->signature_hash = 0;
      node_shader_glsl_function_tag_text_users_dirty(bmain, node);
      r_changed = true;
      r_error.clear();
      return true;
    }

    size_t source_len = 0;
    char* source = txt_to_buf(text, &source_len);
    BLI_SCOPED_DEFER([&]() { MEM_delete(source); });
    if (source_len != 0)
    {
      r_error.clear();
      return true;
    }
    if (!ID_IS_EDITABLE(&text->id))
    {
      r_error.clear();
      return true;
    }

    BKE_text_write(text, default_source, int(sizeof(default_source) - 1));
    STRNCPY(storage->function_name, "glsl_function");
    storage->parse_status = SHD_GLSL_FUNCTION_PARSE_DIRTY;
    storage->signature_hash = 0;
    node_shader_glsl_function_tag_text_users_dirty(bmain, node);
    r_changed = true;
    r_error.clear();
    return true;
  }

  bool node_shader_glsl_function_edit_source_get(const bNode& node,
                                                 std::string& r_source,
                                                 std::string& r_error)
  {
    const NodeShaderGLSLFunction* storage = static_cast<const NodeShaderGLSLFunction*>(
      node.storage);
    if (storage != nullptr && storage->edit_source != nullptr)
    {
      r_source = storage->edit_source;
      r_error.clear();
      return true;
    }
    return node_shader_glsl_function_source_get(node, r_source, r_error);
  }

  void node_shader_glsl_function_edit_source_set(bNode& node, const char* source)
  {
    NodeShaderGLSLFunction* storage = static_cast<NodeShaderGLSLFunction*>(node.storage);
    if (storage == nullptr || source == nullptr ||
        storage->source_mode != SHD_GLSL_FUNCTION_SOURCE_INTERNAL || node.id == nullptr)
    {
      return;
    }

    std::string current_source;
    std::string error;
    if (!node_shader_glsl_function_edit_source_get(node, current_source, error) ||
        current_source == source || !ID_IS_EDITABLE(node.id))
    {
      return;
    }

    Text& text = *reinterpret_cast<Text*>(node.id);
    BKE_text_clear(&text);
    BKE_text_write(&text, source, int(strlen(source)));
    MEM_SAFE_DELETE(storage->edit_source);
    storage->edit_source_session_uid = 0;
    storage->edit_source_hash = 0;
    storage->parse_status = SHD_GLSL_FUNCTION_PARSE_DIRTY;
    storage->signature_hash = 0;
  }

  struct GLSLTextUser
  {
    ID* owner_id;
    bNodeTree* tree;
    bNode* node;
  };

  static Vector<GLSLTextUser> node_shader_glsl_function_text_users(Main& bmain,
                                                                   const bNode& node)
  {
    Vector<GLSLTextUser> users;
    if (node.id == nullptr)
    {
      return users;
    }

    FOREACH_NODETREE_BEGIN (&bmain, user_tree, owner_id)
    {
      for (bNode& user_node : user_tree->nodes)
      {
        if (user_node.type_legacy == SH_NODE_GLSL_FUNCTION && user_node.storage != nullptr &&
            user_node.id == node.id)
        {
          users.append({owner_id, user_tree, &user_node});
        }
      }
    }
    FOREACH_NODETREE_END;
    return users;
  }

  void node_shader_glsl_function_tag_text_users_dirty(Main& bmain, const bNode& node)
  {
    for (const GLSLTextUser& user : node_shader_glsl_function_text_users(bmain, node))
    {
      NodeShaderGLSLFunction& user_storage =
        *static_cast<NodeShaderGLSLFunction*>(user.node->storage);
      user_storage.parse_status = SHD_GLSL_FUNCTION_PARSE_DIRTY;
      user_storage.signature_hash = 0;
    }
  }

  bool node_shader_glsl_function_refresh_text_users(Main& bmain,
                                                     bNodeTree& ntree,
                                                     bNode& node,
                                                     std::string& r_error)
  {
    namespace file_ns = nodes::node_shader_glsl_function_cc;
    NodeShaderGLSLFunction* storage = static_cast<NodeShaderGLSLFunction*>(node.storage);
    if (storage == nullptr)
    {
      r_error = "Invalid GLSL Function node storage";
      return false;
    }

    std::string source;
    if (!node_shader_glsl_function_source_get(node, source, r_error))
    {
      return false;
    }

    Vector<GLSLTextUser> users;
    if (storage->source_mode == SHD_GLSL_FUNCTION_SOURCE_INTERNAL && node.id != nullptr)
    {
      users = node_shader_glsl_function_text_users(bmain, node);
    }
    if (users.is_empty())
    {
      users.append({&ntree.id, &ntree, &node});
    }

    std::string current_error;
    Set<bNodeTree*> changed_trees;
    for (const GLSLTextUser& user : users)
    {
      NodeShaderGLSLFunction& user_storage =
        *static_cast<NodeShaderGLSLFunction*>(user.node->storage);
      const file_ns::GLSLParseResult parse_result = file_ns::parse_glsl_source_for_node(
        *user.node, source, user_storage.function_name);
      file_ns::cache_parse_status(*user.node, parse_result);
      std::string error = parse_result.error;
      if (parse_result.ok && !file_ns::validate_glsl_candidate_with_gpu(parse_result, error))
      {
        user_storage.parse_status = SHD_GLSL_FUNCTION_PARSE_ERROR;
        user_storage.signature_hash = 0;
      }
      if (!parse_result.ok || !error.empty())
      {
        if (user.node == &node)
        {
          current_error = error.empty() ? "GLSL Function did not parse" : std::move(error);
        }
        continue;
      }
      if (!ID_IS_EDITABLE(user.owner_id))
      {
        continue;
      }

      user_storage.parse_status = SHD_GLSL_FUNCTION_PARSE_DIRTY;
      user_storage.signature_hash = 0;
      BKE_ntree_update_tag_node_property(user.tree, user.node);
      nodes::update_node_declaration_and_sockets(*user.tree, *user.node);
      changed_trees.add(user.tree);
    }
    for (bNodeTree* changed_tree : changed_trees)
    {
      if (GS(changed_tree->id.name) == ID_NT)
      {
        changed_tree->tree_interface.tag_items_changed_generic();
      }
    }
    BKE_main_ensure_invariants(bmain);
    if (!current_error.empty())
    {
      const Text* text = node.id != nullptr ? reinterpret_cast<const Text*>(node.id) : nullptr;
      r_error = "Cannot refresh GLSL Function \"" + std::string(storage->function_name) + "\"";
      if (text != nullptr)
      {
        r_error += " from Text \"" + std::string(text->id.name + 2) + "\"";
      }
      r_error += ": " + current_error;
      return false;
    }
    r_error.clear();
    return true;
  }

  bool node_shader_glsl_function_reset_defaults(bNode& node, std::string& r_error)
  {
    namespace file_ns = nodes::node_shader_glsl_function_cc;
    if (node.type_legacy != SH_NODE_GLSL_FUNCTION)
    {
      r_error = "Active node is not a GLSL Function";
      return false;
    }

    const file_ns::GLSLParseResult parse_result = file_ns::parse_glsl_for_node(node);
    file_ns::cache_parse_status(node, parse_result);
    if (!parse_result.ok)
    {
      r_error = parse_result.error.empty() ? "GLSL Function did not parse" : parse_result.error;
      return false;
    }

    file_ns::sync_glsl_meta_defaults(node, parse_result);
    file_ns::reset_glsl_input_defaults_to_meta(node, parse_result);
    file_ns::reset_glsl_define_values_to_defaults(node, parse_result);
    r_error.clear();
    return true;
  }

  void register_node_type_sh_glsl_function()
  {
    namespace file_ns = nodes::node_shader_glsl_function_cc;

    static bke::bNodeType ntype;

    sh_node_type_base(&ntype, "ShaderNodeGLSLFunction"_ustr, SH_NODE_GLSL_FUNCTION);
    ntype.ui_name = "GLSL Function";
    ntype.ui_description =
      "Call a user-authored GLSL function from a Text datablock or external GLSL file";
    ntype.enum_name_legacy = "GLSL_FUNCTION";
    ntype.nclass = NODE_CLASS_SCRIPT;
    ntype.declare = file_ns::node_declare;
    ntype.draw_buttons = file_ns::node_layout;
    ntype.draw_buttons_ex = file_ns::node_layout_ex;
    ntype.initfunc = file_ns::node_init;
    ntype.updatefunc = file_ns::node_update;
    ntype.insert_link = file_ns::node_insert_link;
    ntype.gpu_fn = file_ns::gpu_shader_glsl_function;
    ntype.add_ui_poll = object_filter_or_npr_eevee_shader_nodes_poll;
    ntype.blend_write_storage_content = file_ns::node_storage_blend_write;
    ntype.blend_data_read_storage_content = file_ns::node_storage_blend_read;

    bke::node_type_storage(
      ntype, "NodeShaderGLSLFunction", file_ns::node_storage_free, file_ns::node_storage_copy);

    bke::node_register_type(ntype);
  }

}  // namespace blender
