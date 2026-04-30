/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

 /** \file
  * \ingroup shdnodes
  */

#include <cctype>
#include <cstdlib>
#include <sstream>

#include "node_exec.hh"
#include "node_shader_util.hh"

#include "BLO_read_write.hh"

#include "BKE_image.hh"
#include "BKE_node_runtime.hh"
#include "BKE_text.h"

#include "DNA_material_types.h"

#include "BLI_fileops.h"
#include "BLI_ghash.h"
#include "BLI_map.hh"
#include "BLI_memory_utils.hh"
#include "BLI_path_utils.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "CLG_log.h"

#include "GPU_capabilities.hh"

#include "IMB_imbuf_types.hh"

#include "intern/gpu_node_graph.hh"

#include "NOD_sync_sockets.hh"

#include "UI_interface_layout.hh"
#include "UI_interface_c.hh"
#include "UI_resources.hh"

namespace blender
{

  namespace nodes::node_shader_glsl_function_cc
  {

    NODE_STORAGE_FUNCS(NodeShaderGLSLFunction)

    static CLG_LogRef LOG = { "node.shader.glsl_function" };
    static thread_local Set<std::string> active_closure_helper_keys;
    static thread_local Vector<const bNode*> active_closure_helper_nodes;

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
      if (storage->packed_source != nullptr)
      {
        MEM_delete(storage->packed_source);
      }
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
      if (src_storage->packed_source != nullptr)
      {
        dst_storage->packed_source = BLI_strdup(src_storage->packed_source);
      }
      dest_node->storage = dst_storage;
    }

    static void node_storage_blend_write(const bNodeTree& /*tree*/,
                                         const bNode& node,
                                         BlendWriter& writer)
    {
      const NodeShaderGLSLFunction& storage = node_storage(node);
      BLO_write_string(&writer, storage.packed_source);
    }

    static void node_storage_blend_read(bNodeTree& /*tree*/,
                                        bNode& node,
                                        BlendDataReader& reader)
    {
      NodeShaderGLSLFunction& storage = node_storage(node);
      BLO_read_string(&reader, &storage.packed_source);
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

      struct Meta
      {
        bool has_default_value = false;
        float4 default_value = float4(0.0f);
        std::optional<std::string> default_expression;
        std::optional<std::string> panel_name;
        std::optional<std::string> description;
        bool has_min = false;
        float min_value = 0.0f;
        bool has_max = false;
        float max_value = 0.0f;
        bool hide_value = false;
        std::optional<PropertySubType> subtype;

        bool has_any() const
        {
          return has_default_value || default_expression.has_value() || panel_name.has_value() ||
            description.has_value() || has_min || has_max || hide_value || subtype.has_value();
        }
      };

      GLSLBoundaryType type = GLSLBoundaryType::Unsupported;
      Qualifier qualifier = Qualifier::In;
      std::string type_name;
      std::string name;
      std::string identifier;
      int dimensions = 0;
      int64_t type_source_start = -1;
      int64_t type_source_end = -1;
      Meta meta;
    };

    struct GLSLPanelMeta
    {
      std::string name;
      bool default_closed = true;
    };

    struct GLSLFunctionDefinition
    {
      std::string name;
      std::string return_type_name;
      GLSLBoundaryType return_type = GLSLBoundaryType::Unsupported;
      Vector<GLSLFunctionParam> params;
      Vector<GLSLPanelMeta> panels;
      int body_token_start = -1;
      int body_token_end = -1;
    };

    struct GLSLParseResult
    {
      bool ok = false;
      bool keep_existing_sockets = false;
      bool used_first_function = false;
      bool uses_geometry_access = false;
      bool uses_lightprobe_access = false;
      bool uses_eevee_light_access = false;

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
      GLSLFunctionDefinition function;
      int signature_hash = 0;
      int meta_hash = 0;
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

    enum class GLSLSample2DSourceKind : uint8_t
    {
      None = 0,
      ImageToClosure,
      ClosureOutput,
      GLSLFunction,
      Unsupported,
    };

    struct GLSLRawParamMeta
    {
      std::optional<std::string> default_value;
      std::optional<std::string> min_value;
      std::optional<std::string> max_value;
      std::optional<std::string> hide_value;
      std::optional<std::string> subtype;
      std::optional<std::string> panel_name;
      std::optional<std::string> description;

      bool has_any() const
      {
        return default_value.has_value() || min_value.has_value() || max_value.has_value() ||
          hide_value.has_value() || subtype.has_value() || panel_name.has_value() ||
          description.has_value();
      }

      bool has_sampler_unsupported_meta() const
      {
        return default_value.has_value() || min_value.has_value() || max_value.has_value() ||
          hide_value.has_value() || subtype.has_value();
      }
    };

    struct GLSLClosureSampleHelper
    {
      std::string param_name;
      std::string helper_name;
      std::string uv_global_name;
      std::string sub_function_name;
      GPUType return_type = GPU_VEC4;
    };

    struct GLSLTokenRange
    {
      int start = -1;
      int end = -1;
    };

    struct GLSLClosureSample2DDowngradeInfo
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

    static Vector<GLSLToken> tokenize_glsl_source(const StringRef source);
    static const bNodeSocket* find_node_input_socket_by_identifier(const bNode& node,
      const StringRef identifier);
    static bNodeSocket* find_node_input_socket_by_identifier(bNode& node, const StringRef identifier);
    static const bNodeSocket* find_closure_output_socket_by_name(const bNode& node, const StringRef name);
    static bool closure_output_has_required_sample2d_signature(const bNode& closure_output_node,
      std::string& r_error);
    static bool glsl_param_has_input_socket(const GLSLFunctionParam& param);
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
    static bool is_sample2d_sampling_function_name(const StringRef name);
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

    static const bNodeSocket* find_linked_sample2d_input_socket(const bNode& node,
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

    static bool glsl_function_input_is_directly_linked(const bNode& node,
      const GLSLFunctionParam& param)
    {
      const bNodeSocket* socket = find_glsl_function_input_socket(node, param);
      return socket != nullptr && socket->is_directly_linked();
    }

    static GLSLSample2DSourceKind resolve_sample2d_source_kind(const bNode& node,
      const GLSLFunctionParam& param,
      const bNodeLink*& r_link,
      const bNode* fallback_node = nullptr)
    {
      const bNodeSocket* sample_socket = find_linked_sample2d_input_socket(node, param);
      if (sample_socket == nullptr && fallback_node != nullptr)
      {
        if (fallback_node != &node && fallback_node != node.runtime->original)
        {
          sample_socket = find_linked_sample2d_input_socket(*fallback_node, param);
        }
      }
      if (sample_socket == nullptr)
      {
        r_link = nullptr;
        return GLSLSample2DSourceKind::None;
      }

      r_link = find_used_direct_link(*sample_socket);
      if (r_link == nullptr || r_link->fromnode == nullptr)
      {
        return GLSLSample2DSourceKind::None;
      }
      if (r_link->fromnode->is_type("ShaderNodeImageToClosure"))
      {
        return GLSLSample2DSourceKind::ImageToClosure;
      }
      if (r_link->fromnode->is_type("NodeClosureOutput"))
      {
        return GLSLSample2DSourceKind::ClosureOutput;
      }
      if (r_link->fromnode->is_type("ShaderNodeGLSLFunction"))
      {
        return GLSLSample2DSourceKind::GLSLFunction;
      }
      return GLSLSample2DSourceKind::Unsupported;
    }

    static GLSLSample2DSourceKind resolve_nested_sample2d_source_kind(const GLSLFunctionParam& param,
      const bNodeLink& link,
      const bNodeLink*& r_link)
    {
      const bNode* from_node = link.fromnode;
      if (from_node == nullptr || !from_node->is_type("ShaderNodeGLSLFunction"))
      {
        r_link = nullptr;
        return GLSLSample2DSourceKind::Unsupported;
      }

      const bNode* logical_node = from_node->runtime->original ? from_node->runtime->original : from_node;
      return resolve_sample2d_source_kind(*from_node, param, r_link, logical_node);
    }

    static Image* resolve_image_to_closure_image(const bNodeLink& link)
    {
      if (link.fromnode == nullptr || !link.fromnode->is_type("ShaderNodeImageToClosure"))
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

    static Image* image_to_closure_image(const bNode& node)
    {
      return id_cast<Image*>(node.id);
    }

    static int image_to_closure_texture_type(const bNode& node)
    {
      const Image* image = image_to_closure_image(node);
      return image ? image->image_to_closure_texture_type : IMA_IMAGE_TO_CLOSURE_TEXTURE_2D;
    }

    static int image_to_closure_texture_size_mode(const bNode& node)
    {
      const Image* image = image_to_closure_image(node);
      return image ? image->image_to_closure_texture_size_mode :
                     IMA_IMAGE_TO_CLOSURE_3D_LUT_SIZE_AUTO;
    }

    static int image_to_closure_interpolation(const bNode& node)
    {
      const Image* image = image_to_closure_image(node);
      return image ? image->image_to_closure_interpolation : SHD_INTERP_LINEAR;
    }

    static int image_to_closure_extension(const bNode& node)
    {
      const Image* image = image_to_closure_image(node);
      return image ? image->image_to_closure_extension : SHD_IMAGE_EXTENSION_REPEAT;
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
        const int lut_width = image.image_to_closure_texture_width;
        const int lut_height = image.image_to_closure_texture_height;
        const int lut_depth = image.image_to_closure_texture_depth;
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
                                    GPU_SAMPLER_FILTERING_ANISOTROPIC |
                                      GPU_SAMPLER_FILTERING_LINEAR | GPU_SAMPLER_FILTERING_MIPMAP;
      }

      return sampler_state;
    }

    static Map<std::string, GLSLSample2DSourceKind> resolve_sample2d_source_kinds(
      const bNode& node, const GLSLFunctionDefinition& function)
    {
      Map<std::string, GLSLSample2DSourceKind> source_kinds;
      const bNode* logical_node = node.runtime->original ? node.runtime->original : &node;
      for (const GLSLFunctionParam& param : function.params)
      {
        if (!glsl_param_has_input_socket(param) || !glsl_boundary_type_is_sample2d(param.type))
        {
          continue;
        }
        const bNodeLink* used_link = nullptr;
        source_kinds.add_new(param.name,
          resolve_sample2d_source_kind(node, param, used_link, logical_node));
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
          r_error = "Closure Output sampler2D identifier '" + identifier_key + "' in function '" +
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

    static bool build_closure_sample2d_bindings_by_function(
      const Vector<GLSLToken>& tokens,
      const Span<GLSLFunctionDefinition> functions,
      const GLSLFunctionDefinition& entry_function,
      const Map<std::string, GLSLSample2DSourceKind>& entry_source_kinds,
      Vector<GLSLClosureFunctionBindings>& r_bindings_by_function,
      std::string& r_error)
    {
      r_bindings_by_function.clear();
      r_bindings_by_function.append({ entry_function.name, {} });

      bool changed = false;
      GLSLClosureFunctionBindings& entry_bindings = r_bindings_by_function.last();
      for (const GLSLFunctionParam& param : entry_function.params)
      {
        if (!glsl_param_has_input_socket(param) || !glsl_boundary_type_is_sample2d(param.type))
        {
          continue;
        }
        const GLSLSample2DSourceKind source_kind = entry_source_kinds.lookup_default(
          param.name, GLSLSample2DSourceKind::None);
        if (source_kind != GLSLSample2DSourceKind::ClosureOutput)
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
                !glsl_boundary_type_is_sample2d(callee_param.type))
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

    static bool validate_closure_sample2d_bindings(
      const Vector<GLSLToken>& tokens,
      const Span<GLSLFunctionDefinition> functions,
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

          if (is_sample2d_sampling_function_name(token.text))
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
              !glsl_boundary_type_is_sample2d(callee_param.type))
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

          r_error = "Closure Output driven sampler2D identifier '" + token.text + "' in function '" +
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

    static int make_panel_identifier(const StringRef name)
    {
      const std::string key = "glsl_meta_panel_" + std::string(name);
      return int(BLI_ghashutil_strhash_p(key.c_str()) & 0x7fffffff);
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

    static bool is_sample2d_sampling_function_name(const StringRef name)
    {
      return name == "texture" || name == "textureLod" || name == "textureGrad" ||
        name == "textureSize" || name == "texelFetch" || name == "textureGather";
    }

    static std::string make_split_vec4_w_socket_identifier(const StringRef identifier)
    {
      return std::string(identifier) + "_w";
    }

    static std::string make_split_vec4_w_socket_name(const StringRef name)
    {
      return std::string(name) + " W";
    }

    static std::string make_wrapper_vec4_temp_name(const StringRef name)
    {
      return std::string(name) + "_vec4_tmp";
    }

    static std::string make_wrapper_temp_name(const StringRef name)
    {
      return std::string(name) + "_tmp";
    }

    static bool glsl_param_uses_color_socket(const GLSLFunctionParam& param)
    {
      return ELEM(param.type, GLSLBoundaryType::Vec3, GLSLBoundaryType::Vec4) &&
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
      const Map<std::string, GLSLSample2DSourceKind> source_kinds = resolve_sample2d_source_kinds(node,
        function);
      bool has_closure_output_sample2d = false;

      for (const GLSLFunctionParam& param : function.params)
      {
        if (!glsl_param_has_input_socket(param) || !glsl_boundary_type_is_sampler(param.type))
        {
          continue;
        }

        const bNodeSocket* socket = find_linked_sample2d_input_socket(node, param);
        if (socket == nullptr && node.runtime->original)
        {
          socket = find_linked_sample2d_input_socket(*node.runtime->original, param);
        }
        if (socket != nullptr)
        {
          const bNodeLink* used_link = nullptr;
          const bNode* logical_node = node.runtime->original ? node.runtime->original : &node;
          GLSLSample2DSourceKind source_kind = resolve_sample2d_source_kind(
            node, param, used_link, logical_node);
          if (source_kind == GLSLSample2DSourceKind::GLSLFunction && used_link != nullptr)
          {
            source_kind = resolve_nested_sample2d_source_kind(param, *used_link, used_link);
          }
          if (source_kind == GLSLSample2DSourceKind::ClosureOutput)
          {
            if (glsl_boundary_type_is_sample3d(param.type))
            {
              r_error = "Closure Output connected to sampler3D parameter '" + param.name +
                "' is not supported; use an Image to Closure node set to 3D LUT Strip";
              return false;
            }
            std::string closure_error;
            if (!closure_output_has_required_sample2d_signature(*used_link->fromnode, closure_error))
            {
              r_error = "Closure Output connected to sampler2D parameter '" + param.name +
                "' is missing required closure items: " + closure_error;
              return false;
            }
            has_closure_output_sample2d = true;
            continue;
          }
          if (source_kind != GLSLSample2DSourceKind::ImageToClosure || used_link == nullptr)
          {
            r_error = std::string(sampler_type_name(param.type).c_str()) + " parameter '" + param.name +
              "' currently only supports Image to Closure" +
              (glsl_boundary_type_is_sample2d(param.type) ? " or Closure Output links" : " links");
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

      if (!has_closure_output_sample2d)
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
      if (!build_closure_sample2d_bindings_by_function(
        tokens, all_functions, function, source_kinds, bindings_by_function, r_error))
      {
        return false;
      }
      if (!validate_closure_sample2d_bindings(tokens, all_functions, bindings_by_function, r_error))
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
        if ((i + 1) < source.size() && source[i] == '/' && source[i + 1] == '/')
        {
          i += 2;
          while (i < source.size() && source[i] != '\n')
          {
            i++;
          }
          continue;
        }
        if ((i + 1) < source.size() && source[i] == '/' && source[i + 1] == '*')
        {
          i += 2;
          while ((i + 1) < source.size() && !(source[i] == '*' && source[i + 1] == '/'))
          {
            i++;
          }
          i = std::min<int64_t>(i + 2, source.size());
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

    static bool parse_glsl_meta_int_literal(const StringRef text, int& r_value, std::string& r_error)
    {
      const std::string trimmed = trim_copy(text);
      if (trimmed.empty())
      {
        r_error = "GLSL meta integer value cannot be empty";
        return false;
      }

      char* end = nullptr;
      const long value = std::strtol(trimmed.c_str(), &end, 10);
      if (end == trimmed.c_str() || trim_copy(end).size() != 0)
      {
        r_error = "Could not parse GLSL meta integer value '" + trimmed + "'";
        return false;
      }
      r_value = int(value);
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
        else if (key == "description")
        {
          if (!assign_once(r_meta.description, key, value))
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
      const float value = std::strtof(trimmed.c_str(), &end);
      if (end == trimmed.c_str() || trim_copy(end).size() != 0)
      {
        r_error = "Could not parse GLSL meta float value '" + trimmed + "'";
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
        r_error = "GLSL meta only supports input parameters";
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
        if (raw_meta.panel_name.has_value())
        {
          r_param.meta.panel_name = *raw_meta.panel_name;
        }
        return true;
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
          if (parse_glsl_meta_int_literal(*raw_meta.default_value, literal_default, r_error))
          {
            r_param.meta.default_value.x = float(literal_default);
            r_param.meta.has_default_value = true;
            parsed_literal_default = true;
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

      if (raw_meta.min_value.has_value())
      {
        if (r_param.type == GLSLBoundaryType::Int)
        {
          int min_value = 0;
          if (!parse_glsl_meta_int_literal(*raw_meta.min_value, min_value, r_error))
          {
            return false;
          }
          r_param.meta.min_value = float(min_value);
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
          r_param.meta.max_value = float(max_value);
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

      if (r_param.meta.has_min && r_param.meta.has_max &&
        r_param.meta.min_value > r_param.meta.max_value)
      {
        r_error = "GLSL meta min cannot be greater than max";
        return false;
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
          for (const int i : IndexRange(std::max(param.dimensions, 1)))
          {
            if (i != 0)
            {
              ss << ',';
            }
            ss << param.meta.default_value[i];
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
        if (param.meta.has_min)
        {
          ss << "min=" << param.meta.min_value << ';';
        }
        if (param.meta.has_max)
        {
          ss << "max=" << param.meta.max_value << ';';
        }
        if (param.meta.hide_value)
        {
          ss << "hide_value=1;";
        }
        if (param.meta.subtype.has_value())
        {
          ss << "subtype=" << int(*param.meta.subtype) << ';';
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

    static bool parse_glsl_parameter_tokens(const Span<GLSLToken> tokens,
      GLSLFunctionParam& r_param,
      std::string& r_error)
    {
      if (tokens.is_empty())
      {
        r_error = "Empty parameter declaration";
        return false;
      }

      Vector<StringRef> identifiers;
      bool has_out_qualifier = false;
      bool has_inout_qualifier = false;
      bool has_unsupported_punctuation = false;

      for (const GLSLToken& token : tokens)
      {
        if (token.kind == GLSLToken::Kind::Identifier)
        {
          identifiers.append(token.text);
          has_out_qualifier |= token.text == "out";
          has_inout_qualifier |= token.text == "inout";
        }
        else if (!ELEM(token.punctuation, '[', ']'))
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
      if (identifiers.size() == 1 && identifiers[0] == "void")
      {
        r_param = {};
        return true;
      }
      if (identifiers.size() < 2)
      {
        r_error = "Each parameter needs a type and a name";
        return false;
      }

      const StringRef type_name = identifiers[identifiers.size() - 2];
      const StringRef param_name = identifiers.last();
      const GLSLToken& type_token = tokens[tokens.size() - 2];
      const GLSLBoundaryType boundary_type = glsl_boundary_type_from_name(type_name);
      if (!ELEM(boundary_type,
        GLSLBoundaryType::Float,
        GLSLBoundaryType::Int,
        GLSLBoundaryType::Bool,
        GLSLBoundaryType::Vec2,
        GLSLBoundaryType::Vec3,
        GLSLBoundaryType::Vec4,
        GLSLBoundaryType::Sample2D,
        GLSLBoundaryType::Sample3D))
      {
        r_error =
          "Supported parameter types are float, int, bool, vec2, vec3, vec4, sampler2D, and "
          "sampler3D";
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
        GLSLBoundaryType::Vec4))
      {
        r_error = "Supported return types are void, float, int, bool, vec2, vec3, and vec4";
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
      if (!node.is_type("NodeClosureOutput"))
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

    static const bNode* find_localized_copy_of_original_node(const bNodeTree& tree, const bNode& original_node)
    {
      for (const bNode* node : tree.all_nodes())
      {
        if (node->runtime->original == &original_node)
        {
          return node;
        }
      }
      return nullptr;
    }

    static bool closure_output_has_required_sample2d_signature(const bNode& closure_output_node,
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
        r_error = "Closure Output must expose a Vector input item named 'UV' for sampler2D";
        return false;
      }

      const bNodeSocket* color_socket = find_closure_output_socket_by_name(closure_output_node, "Color");
      if (color_socket == nullptr || !ELEM(color_socket->type, SOCK_FLOAT, SOCK_VECTOR, SOCK_RGBA))
      {
        r_error = "Closure Output must expose a Float, Vector, or RGBA output item named 'Color' for sampler2D";
        return false;
      }
      return true;
    }

    static void mark_node_upstream_for_closure_helper(const bNode& node, Set<const bNode*>& r_visited_nodes);

    static void mark_socket_upstream_for_closure_helper(const bNodeSocket& socket,
      Set<const bNode*>& r_visited_nodes)
    {
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
      const bNode* node_to_mark = &node;
      const bNode* node_original = node.runtime->original ? node.runtime->original : &node;
      if (const bNode* localized_node = find_localized_copy_of_original_node(node.owner_tree(),
        *node_original))
      {
        node_to_mark = localized_node;
      }

      if (!r_visited_nodes.add(node_to_mark))
      {
        return;
      }
      const_cast<bNode&>(*node_to_mark).runtime->need_exec = 1;
      for (const bNodeSocket* input_socket : node_to_mark->input_sockets())
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
      if (const bNode* localized_node = find_localized_copy_of_original_node(node.owner_tree(),
        *logical_node))
      {
        eval_node = localized_node;
      }

      const std::string helper_key = std::to_string(reinterpret_cast<uintptr_t>(eval_node)) + ":" +
        param.name;
      if (!active_closure_helper_keys.add(helper_key))
      {
        r_error = "Recursive nested Closure Output sampler2D helper dependency detected on node '" +
          std::string(logical_node->name) + "' parameter '" + param.name + "'";
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
      if (resolve_sample2d_source_kind(*eval_node, param, used_link, logical_node) !=
        GLSLSample2DSourceKind::ClosureOutput ||
        used_link == nullptr || used_link->fromnode == nullptr)
      {
        r_error = "sampler2D parameter '" + param.name + "' is missing a Closure Output source";
        return false;
      }

      const bNode* closure_output_node = used_link->fromnode;
      const bNode* closure_output_original = closure_output_node->runtime->original ?
        closure_output_node->runtime->original :
        closure_output_node;
      if (const bNode* localized_node = find_localized_copy_of_original_node(node.owner_tree(),
        *closure_output_original))
      {
        closure_output_node = localized_node;
      }

      std::string closure_error;
      if (!closure_output_has_required_sample2d_signature(*closure_output_node, closure_error))
      {
        r_error = "Closure Output connected to '" + param.name + "' is missing required closure items: " +
          closure_error;
        return false;
      }
      const bNodeSocket* color_socket = find_closure_output_socket_by_name(*closure_output_node, "Color");
      const bNodeLink* color_source_link = find_any_direct_link(*color_socket);
      const bNodeSocket* sample_socket = color_socket;

      bNodeExecContext context = {};
      bNodeTree* helper_tree = const_cast<bNodeTree*>(&closure_output_node->owner_tree());
      bNodeTreeExec* exec = ntreeShaderBeginExecTree_internal(&context, helper_tree, bke::NODE_INSTANCE_KEY_BASE);
      if (exec == nullptr)
      {
        r_error = "Could not build a helper shader tree for sampler2D parameter '" + param.name + "'";
        return false;
      }
      BLI_SCOPED_DEFER([&]() { ntreeShaderEndExecTree_internal(exec); });

      Vector<int> previous_need_exec;
      for (bNode& tree_node : helper_tree->nodes)
      {
        previous_need_exec.append(tree_node.runtime->need_exec);
        tree_node.runtime->need_exec = 0;
      }
      BLI_SCOPED_DEFER([&]()
        {
          int node_index = 0;
          for (bNode& tree_node : helper_tree->nodes)
          {
            tree_node.runtime->need_exec = previous_need_exec[node_index++];
          }
        });
      Set<const bNode*> visited_nodes;
      if (color_source_link != nullptr && color_source_link->fromnode != nullptr)
      {
        mark_node_upstream_for_closure_helper(*color_source_link->fromnode, visited_nodes);
      }
      else
      {
        mark_socket_upstream_for_closure_helper(*color_socket, visited_nodes);
      }

      GPU_material_closure_uv_source_push(mat, StringRefNull(uv_global_name));
      BLI_SCOPED_DEFER([&]() { GPU_material_closure_uv_source_pop(mat); });

      ntreeExecGPUNodes(exec, mat, nullptr);

      bNodeStack* color_stack = node_get_socket_stack(exec->stack, const_cast<bNodeSocket*>(sample_socket));
      GPUNodeLink* color_link = (color_stack != nullptr) ? static_cast<GPUNodeLink*>(color_stack->data) :
        nullptr;
      if (color_link == nullptr && color_stack != nullptr)
      {
        color_link = GPU_constant(color_stack->vec);
      }
      if (color_link == nullptr)
      {
        r_error = "Could not evaluate Closure Output Color for sampler2D parameter '" + param.name + "'";
        return false;
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

      release_active_helper();
      active_helper_released = true;

      const char* sub_function_name = GPU_material_split_sub_function(
        mat, helper_return_type, &color_link, StringRefNull(source_filename));
      r_helper.param_name = param.name;
      r_helper.helper_name = helper_name;
      r_helper.uv_global_name = uv_global_name;
      r_helper.sub_function_name = sub_function_name;
      r_helper.return_type = helper_return_type;
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

    static bool rewrite_glsl_source_for_sample2d(
      const StringRef source,
      const Vector<GLSLToken>& tokens,
      const Span<GLSLFunctionDefinition> functions,
      const Span<GLSLClosureFunctionBindings> bindings_by_function,
      const Span<GLSLClosureSampleHelper> closure_helpers,
      std::string& r_rewritten_source,
      GLSLClosureSample2DDowngradeInfo& r_downgrade_info,
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
          if (!glsl_boundary_type_is_sample2d(param.type) || param.type_source_start < 0 ||
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
            !is_sample2d_sampling_function_name(token.text) ||
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
            r_error = "Could not resolve a Closure Output helper for sampler2D parameter '" +
              *root_param_name + "'";
            return false;
          }

          r_downgrade_info.root_param_names.add(*root_param_name);
          std::string replacement_text;

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

    static void log_closure_sample2d_downgrades(GPUMaterial* mat,
      const bNode& node,
      const GLSLParseResult& parse_result,
      const GLSLClosureSample2DDowngradeInfo& info)
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

    static std::string build_sample2d_closure_helper_block(
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
        ss << "vec2 " << helper.uv_global_name << " = vec2(0.0);\n";
      }
      ss << "\n";
      for (const GLSLClosureSampleHelper& helper : closure_helpers)
      {
        ss << "vec4 " << helper.helper_name << "(vec2 uv)\n{\n";
        ss << "  " << helper.uv_global_name << " = uv;\n";
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
#if defined(GPU_FRAGMENT_SHADER) && (defined(MAT_DEFERRED) || defined(MAT_FORWARD) || defined(NPR_SHADER))
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
vec3 glsl_helper_ambient_safe_direction(vec3 value, vec3 fallback)
{
  float value_len_squared = dot(value, value);
  if (value_len_squared > 1e-16) {
    return value * inversesqrt(value_len_squared);
  }

  float fallback_len_squared = dot(fallback, fallback);
  if (fallback_len_squared > 1e-16) {
    return fallback * inversesqrt(fallback_len_squared);
  }

  return vec3(0.0, 0.0, 1.0);
}

vec3 glsl_ambient_lighting()
{
#ifdef SPHERE_PROBE
  vec3 shading_normal = glsl_helper_ambient_safe_direction(g_data.N, g_data.Ng);
  vec3 probe_bias_normal = glsl_helper_ambient_safe_direction(g_data.Ni, g_data.Ng);
  vec3 view_vector = drw_world_incident_vector(g_data.P);

  LightProbeSample probe_sample = lightprobe_load(g_data.P, probe_bias_normal, view_vector);
  probe_sample.volume_irradiance = spherical_harmonics_clamp(probe_sample.volume_irradiance,
                                                             uniform_buf.clamp.surface_indirect);
  return max(spherical_harmonics_evaluate_lambert(shading_normal, probe_sample.volume_irradiance),
             vec3(0.0));
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
      const StringRef source,
      const Span<GLSLClosureSampleHelper> closure_helpers)
    {
      std::stringstream ss;
      ss << build_sample2d_closure_helper_block(closure_helpers);
      ss << "#define sample2D sampler2D\n";
      for (const std::string& name : global_names)
      {
        ss << "#define " << name << " " << prefix << name << "\n";
      }
      for (const std::string& name : function_names)
      {
        ss << "#define " << name << " " << prefix << name << "\n";
      }
      ss << "\n" << source;
      if (!source.is_empty() && source[source.size() - 1] != '\n')
      {
        ss << "\n";
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
      const GLSLSample2DSourceKind sample2d_source_kind)
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
      if (glsl_boundary_type_is_sample3d(param.type))
      {
        return StringRefNull("sampler3D");
      }
      return (sample2d_source_kind == GLSLSample2DSourceKind::ClosureOutput) ? StringRefNull("float") :
                                                                               StringRefNull("sampler2D");
    }

    static StringRefNull emitted_type_name(const GLSLBoundaryType type, const StringRefNull type_name)
    {
      return glsl_boundary_type_uses_float_transport(type) ? StringRefNull("float") : type_name;
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
      return ELEM(type, GLSLBoundaryType::Vec4, GLSLBoundaryType::Int, GLSLBoundaryType::Bool);
    }

    static std::string build_wrapper_glsl_source(
      const bNode& node,
      const GLSLParseResult& parse_result,
      const Map<std::string, GLSLSample2DSourceKind>& sample2d_source_kinds)
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
      for (const GLSLFunctionParam& param : function.params)
      {
        if (!glsl_param_has_input_socket(param))
        {
          continue;
        }
        const GLSLSample2DSourceKind source_kind = sample2d_source_kinds.lookup_ptr(param.name) ?
          *sample2d_source_kinds.lookup_ptr(param.name) :
          GLSLSample2DSourceKind::None;
        append_wrapper_input(emitted_type_name(param, source_kind), make_wrapper_argument_name("in", param.name));
      }
      if (function.return_type != GLSLBoundaryType::Void)
      {
        if (function.return_type == GLSLBoundaryType::Vec4)
        {
          append_wrapper_output("vec3", "out_result");
          append_wrapper_output("float", "out_result_w");
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
          append_wrapper_output("vec3", make_wrapper_argument_name("out", param.name));
          append_wrapper_output("float", make_split_vec4_w_socket_identifier(
            make_wrapper_argument_name("out", param.name)));
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
        else if (glsl_boundary_type_uses_float_transport(function.return_type))
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
          else if (glsl_boundary_type_uses_float_transport(param.type))
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
        ss << "  out_result = " << make_wrapper_vec4_temp_name("out_result") << ".xyz;\n";
        ss << "  out_result_w = " << make_wrapper_vec4_temp_name("out_result") << ".w;\n";
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
            ss << "  " << output_name << " = " << temp_name << ".xyz;\n";
            ss << "  " << make_split_vec4_w_socket_identifier(output_name) << " = " << temp_name
              << ".w;\n";
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

    static bool build_specialized_glsl_sources(GPUMaterial* mat,
      const bNode& node,
      const GLSLParseResult& parse_result,
      std::string& r_library_source,
      std::string& r_wrapper_source,
      std::string& r_error)
    {
      const std::string stripped_source = strip_glsl_comments(parse_result.source);
      const Vector<GLSLToken> tokens = tokenize_glsl_source(stripped_source);
      const Map<std::string, GLSLSample2DSourceKind> source_kinds = resolve_sample2d_source_kinds(
        node, parse_result.function);
      Vector<GLSLFunctionDefinition> all_functions;
      if (!find_all_top_level_glsl_function_definitions(
        tokens, parse_result.function_names, all_functions, r_error))
      {
        return false;
      }

      Vector<GLSLClosureFunctionBindings> bindings_by_function;
      if (!build_closure_sample2d_bindings_by_function(
        tokens, all_functions, parse_result.function, source_kinds, bindings_by_function, r_error))
      {
        return false;
      }

      Vector<GLSLClosureSampleHelper> closure_helpers;
      for (const GLSLFunctionParam& param : parse_result.function.params)
      {
        const GLSLSample2DSourceKind* source_kind = source_kinds.lookup_ptr(param.name);
        if (source_kind == nullptr || *source_kind != GLSLSample2DSourceKind::ClosureOutput)
        {
          continue;
        }

        GLSLClosureSampleHelper helper;
        const std::string helper_name = "glsl_sample2d_" + std::to_string(node.identifier) + "_" +
          param.identifier;
        const std::string uv_global_name = helper_name + "_uv";
        if (!build_closure_sample_helper(
          mat, node, param, parse_result.source_filename, helper_name, uv_global_name, helper, r_error))
        {
          return false;
        }
        closure_helpers.append(helper);
      }

      std::string rewritten_source;
      GLSLClosureSample2DDowngradeInfo downgrade_info;
      if (!rewrite_glsl_source_for_sample2d(
        stripped_source,
        tokens,
        all_functions,
        bindings_by_function,
        closure_helpers,
        rewritten_source,
        downgrade_info,
        r_error))
      {
        return false;
      }
      log_closure_sample2d_downgrades(mat, node, parse_result, downgrade_info);

      r_library_source = build_namespaced_glsl_source(
        parse_result.source_prefix,
        parse_result.function_names,
        parse_result.global_names,
        rewritten_source,
        closure_helpers);
      r_wrapper_source = build_wrapper_glsl_source(node, parse_result, source_kinds);

      return true;
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
      if (trim_copy(source).empty())
      {
        result.error = "The GLSL source is empty";
        return result;
      }

      Map<std::string, GLSLRawParamMeta> meta_by_key;
      Map<std::string, Vector<GLSLPanelMeta>> panels_by_function;
      if (!extract_glsl_meta(source, meta_by_key, panels_by_function, result.error))
      {
        return result;
      }

      const std::string stripped_source = strip_glsl_comments(source);
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

      const NodeShaderGLSLFunction& storage = node_storage(node);
      const StringRef requested_function_name = storage.function_name;
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
      if (!validate_sampler_inputs(node, tokens, result.function, result.error))
      {
        return result;
      }

      result.ok = true;
      result.source = source;
      result.resolved_function_name = result.function.name;
      result.used_first_function = false;

      const std::string signature_key = build_function_signature_key(result.function);
      const uint signature_hash = BLI_ghashutil_strhash_p(signature_key.c_str());
      result.signature_hash = int(signature_hash);

      const uint source_hash = BLI_ghashutil_strhash_p(source.c_str());
      char source_hash_hex[16];
      char signature_hash_hex[16];
      SNPRINTF(source_hash_hex, "%08x", source_hash);
      SNPRINTF(signature_hash_hex, "%08x", signature_hash);
      result.source_hash_hex = source_hash_hex;
      const std::string node_id_suffix = std::to_string(node.identifier);
      result.source_prefix = "glsl_src_" + result.source_hash_hex + "_" + node_id_suffix + "_";
      result.source_filename =
        "glsl_function_source_" + result.source_hash_hex + "_" + node_id_suffix + ".glsl";
      result.wrapper_name = "glsl_fn_" + std::to_string(node.identifier) + "_" + signature_hash_hex;
      result.wrapper_filename = result.wrapper_name + ".glsl";
      result.library_source = build_namespaced_glsl_source(
        result.source_prefix,
        result.function_names,
        result.global_names,
        result.source,
        Span<GLSLClosureSampleHelper>());
      result.wrapper_source = build_wrapper_glsl_source(
        node, result, Map<std::string, GLSLSample2DSourceKind>());
      return result;
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

    static void add_glsl_socket_declaration(DeclarationListBuilder& b,
      const GLSLFunctionParam& param,
      const bool is_output,
      const StringRef socket_name,
      const StringRef socket_identifier)
    {
      auto apply_input_description = [&](auto& decl) {
        if (!is_output && param.meta.description.has_value())
        {
          decl.description(*param.meta.description);
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
          auto& decl = b.add_input<decl::Int>(socket_name, socket_identifier)
            .min(param.meta.has_min ? int(param.meta.min_value) : -10000)
            .max(param.meta.has_max ? int(param.meta.max_value) : 10000);
          if (param.meta.has_default_value)
          {
            decl.default_value(int(param.meta.default_value.x));
          }
          if (param.meta.hide_value)
          {
            decl.hide_value();
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

      const bool use_color_socket = !is_output && glsl_param_uses_color_socket(param);
      if (use_color_socket)
      {
        auto& decl = b.add_input<decl::Color>(socket_name, socket_identifier);
        if (param.meta.has_default_value)
        {
          decl.default_value(ColorGeometry4f(param.meta.default_value.x,
            param.meta.default_value.y,
            param.meta.default_value.z,
            param.type == GLSLBoundaryType::Vec4 ? param.meta.default_value.w :
            1.0f));
        }
        if (param.meta.hide_value)
        {
          decl.hide_value();
        }
        apply_input_description(decl);
        return;
      }

      auto configure_vector_decl = [&](auto& decl)
        {
          decl.dimensions(param.dimensions)
            .min(param.meta.has_min ? param.meta.min_value : -10000.0f)
            .max(param.meta.has_max ? param.meta.max_value : 10000.0f);
          if (param.meta.has_default_value)
          {
            switch (param.dimensions)
            {
            case 2:
              decl.default_value(float2(param.meta.default_value.x, param.meta.default_value.y));
              break;
            case 3:
              decl.default_value(
                float3(param.meta.default_value.x, param.meta.default_value.y, param.meta.default_value.z));
              break;
            case 4:
              decl.default_value(param.meta.default_value);
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

      if (is_output)
      {
        if (glsl_output_is_split_vec4(param))
        {
          auto& decl = b.add_output<decl::Vector>(socket_name, socket_identifier);
          decl.dimensions(3);
          b.add_output<decl::Float>(make_split_vec4_w_socket_name(socket_name),
            make_split_vec4_w_socket_identifier(socket_identifier));
          return;
        }
        auto& decl = b.add_output<decl::Vector>(socket_name, socket_identifier);
        configure_vector_decl(decl);
      }
      else
      {
        auto& decl = b.add_input<decl::Vector>(socket_name, socket_identifier);
        configure_vector_decl(decl);
      }
    }

    static void sync_glsl_meta_defaults(bNode& node, const GLSLParseResult& parse_result)
    {
      NodeShaderGLSLFunction& storage = node_storage(node);
      if (!parse_result.ok)
      {
        storage.meta_hash = 0;
        return;
      }
      if (storage.meta_hash == parse_result.meta_hash)
      {
        return;
      }

      /* Socket declarations already initialize defaults for newly created sockets. Re-applying
       * GLSL meta defaults here would overwrite user-edited values on refresh for unchanged
       * parameters with the same identifier. */
      storage.meta_hash = parse_result.meta_hash;
    }

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

      const bool has_panels = !parse_result.function.panels.is_empty();
      if (has_panels)
      {
        b.use_custom_socket_order();
      }

      auto add_output_declarations = [&]() {
        if (parse_result.function.return_type != GLSLBoundaryType::Void)
        {
          GLSLFunctionParam output_param;
          output_param.type = parse_result.function.return_type;
          output_param.type_name = parse_result.function.return_type_name;
          output_param.dimensions = glsl_boundary_dimensions(parse_result.function.return_type);
          add_glsl_socket_declaration(
            b, output_param, true, "Result", result_socket_identifier);
        }

        for (const GLSLFunctionParam& param : parse_result.function.params)
        {
          if (glsl_param_has_output_socket(param))
          {
            add_glsl_socket_declaration(
              b, param, true, param.name, make_socket_identifier("Out", param.name));
          }
        }
      };

      if (has_panels)
      {
        add_output_declarations();
        b.add_default_layout();
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
          b.add_panel(panel.name, make_panel_identifier(panel.name)).default_closed(
            panel.default_closed);
        panel_builders.add(panel.name, &panel_builder);
        return &panel_builder;
      };

      for (const GLSLFunctionParam& param : parse_result.function.params)
      {
        if (glsl_param_has_input_socket(param))
        {
          if (param.meta.panel_name.has_value())
          {
            if (PanelDeclarationBuilder* panel_builder = ensure_panel_builder(
                  *param.meta.panel_name))
            {
              add_glsl_socket_declaration(
                *panel_builder, param, false, param.name, param.identifier);
              continue;
            }
          }
          add_glsl_socket_declaration(b, param, false, param.name, param.identifier);
        }
      }

      if (!has_panels)
      {
        add_output_declarations();
      }
    }

    static void draw_node_layout_content(ui::Layout& layout, bContext* C, PointerRNA* ptr)
    {
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
          field_row.op("node.glsl_function_refresh", "", ICON_FILE_REFRESH);
        }
        else
        {
          ui::Layout& row = layout.row(true);
          row.use_property_split_set(false);
          row.use_property_decorate_set(false);

          if (is_internal)
          {
            if (C != nullptr)
            {
              template_id(&row, C, ptr, "script", nullptr, nullptr, nullptr);
            }
            else
            {
              row.prop(ptr, "script", UI_ITEM_NONE, "", ICON_NONE);
            }
          }
          else
          {
            row.prop(ptr, "filepath", UI_ITEM_NONE, "", ICON_NONE);
          }
          row.op("node.glsl_function_refresh", "", ICON_FILE_REFRESH);
        }
      }

      layout.prop(ptr, "function_name", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);

      bNode& node = *static_cast<bNode*>(ptr->data);
      const NodeShaderGLSLFunction& storage = node_storage(node);
      const GLSLParseResult parse_result = parse_glsl_for_node(node);
      cache_parse_status(node, parse_result);
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
    }

    static void node_layout(ui::Layout& layout, bContext* C, PointerRNA* ptr)
    {
      draw_node_layout_content(layout, C, ptr);
    }

    static void node_layout_ex(ui::Layout& layout, bContext* C, PointerRNA* ptr)
    {
      draw_node_layout_content(layout, C, ptr);
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
          GLSLSample2DSourceKind source_kind = resolve_sample2d_source_kind(node, param, used_link);
          if (source_kind == GLSLSample2DSourceKind::GLSLFunction && used_link != nullptr)
          {
            source_kind = resolve_nested_sample2d_source_kind(param, *used_link, used_link);
          }
          if (source_kind != GLSLSample2DSourceKind::ImageToClosure || used_link == nullptr)
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
          GLSLSample2DSourceKind source_kind = resolve_sample2d_source_kind(node, param, used_link);
          if (source_kind == GLSLSample2DSourceKind::GLSLFunction && used_link != nullptr)
          {
            source_kind = resolve_nested_sample2d_source_kind(param, *used_link, used_link);
          }
          if (source_kind == GLSLSample2DSourceKind::ImageToClosure && used_link != nullptr &&
            used_link->fromnode != nullptr)
          {
            return sampler_state_from_image_to_closure_node(*used_link->fromnode);
          }
          return GPUSamplerState::default_sampler();
        };

      auto resolve_image_to_closure_link = [&](const GLSLFunctionParam& param) -> const bNodeLink*
        {
          const bNodeLink* used_link = nullptr;
          GLSLSample2DSourceKind source_kind = resolve_sample2d_source_kind(node, param, used_link);
          if (source_kind == GLSLSample2DSourceKind::GLSLFunction && used_link != nullptr)
          {
            source_kind = resolve_nested_sample2d_source_kind(param, *used_link, used_link);
          }
          if (source_kind != GLSLSample2DSourceKind::ImageToClosure || used_link == nullptr)
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
          if (glsl_boundary_type_is_sample2d(param.type))
          {
            const bNodeLink* used_link = nullptr;
            GLSLSample2DSourceKind source_kind = resolve_sample2d_source_kind(node, param, used_link);
            if (source_kind == GLSLSample2DSourceKind::GLSLFunction && used_link != nullptr)
            {
              source_kind = resolve_nested_sample2d_source_kind(param, *used_link, used_link);
            }
            if (source_kind == GLSLSample2DSourceKind::ClosureOutput)
            {
              stack->type = GPU_FLOAT;
              stack->link = GPU_constant(zero_value);
              continue;
            }
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
      sync_glsl_meta_defaults(*node, parse_result);
    }

    static bool node_insert_link(bke::NodeInsertLinkParams& params)
    {
      if (params.link.tonode != &params.node || params.link.tosock->type != SOCK_CLOSURE ||
        params.link.fromnode == nullptr || !params.link.fromnode->is_type("NodeClosureOutput"))
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
      if (!active_closure_helper_nodes.is_empty() &&
        !GPU_material_closure_uv_source_get(mat).is_empty())
      {
        const bNode* logical_node = node->runtime->original ? node->runtime->original : node;
        for (const bNode* active_logical_node : active_closure_helper_nodes)
        {
          if (active_logical_node == logical_node)
          {
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
      sync_glsl_meta_defaults(*node, parse_result);

      if (!parse_result.ok)
      {
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
        GPU_material_generated_source_add(
          mat, GPU_GLSL_FUNCTION_LIGHTPROBE_HELPER_FILENAME, {}, lightprobe_helper_source.c_str());
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

      return GPU_stack_link_custom(mat,
        node,
        parse_result.wrapper_name.c_str(),
        parse_result.wrapper_filename.c_str(),
        dependency_flags,
        in,
        out);
    }

  }  // namespace nodes::node_shader_glsl_function_cc

  void register_node_type_sh_glsl_function()
  {
    namespace file_ns = nodes::node_shader_glsl_function_cc;

    static bke::bNodeType ntype;

    sh_node_type_base(&ntype, "ShaderNodeGLSLFunction", SH_NODE_GLSL_FUNCTION);
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
