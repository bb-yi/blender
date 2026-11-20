/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "BLI_assert.hh"
#include "BLI_hash.hh"
#include "BLI_fileops.hh"
#include "BLI_hash_md5.hh"
#include "BLI_path_utils.hh"
#include "BLI_map.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"

#include "GPU_capabilities.hh"
#include "GPU_shader.hh"
#include "GPU_texture.hh"
#include "GPU_uniform_buffer.hh"

#include "DNA_color_types.h"
#include "DNA_node_types.h"
#include "DNA_packedFile_types.h"
#include "BKE_appdir.hh"
#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"

#include "gpu_shader_create_info.hh"

#include "COM_context.hh"
#include "COM_ocio_color_space_conversion_shader.hh"
#include "COM_result.hh"

#include "libocio_display_processor.hh"

#include "CLG_log.h"

#include <OpenColorIO/OpenColorIO.h>

namespace blender::compositor {

static CLG_LogRef LOG = {"compositor.gpu"};

/* --------------------------------------------------------------------
 * GPU Shader Creator.
 */

namespace OCIO = OCIO_NAMESPACE;
using namespace blender::gpu::shader;

/* A subclass of OCIO::GpuShaderCreator that constructs the shader using a ShaderCreateInfo. The
 * Create method should be used to construct the creator, then the extractGpuShaderInfo() method of
 * the appropriate OCIO::GPUProcessor should be called passing in the creator. After construction,
 * the constructed compute shader can be used by calling the bind_shader_and_resources() method,
 * followed by binding the input texture and output image using their names input_sampler_name()
 * and output_image_name(), following by dispatching the shader on the domain of the input, and
 * finally calling the unbind_shader_and_resources() method.
 *
 * Upon calling the extractGpuShaderInfo(), all the transforms in the GPU processor will add their
 * needed resources by calling the respective addUniform() and add[3D]Texture() methods. Then, the
 * shader code of all transforms will be generated and passed to the createShaderText() method,
 * generating the full code of the processor. Finally, the finalize() method will be called to
 * finally create the shader. */
class GPUShaderCreator : public OCIO::GpuShaderCreator {
 public:
  static std::shared_ptr<GPUShaderCreator> Create(ResultPrecision precision, bool lut = false)
  {
    std::shared_ptr<GPUShaderCreator> instance = std::make_shared<GPUShaderCreator>();
    instance->setLanguage(OCIO::GPU_LANGUAGE_GLSL_4_0);
    instance->precision_ = precision;
    instance->lut_ = lut;
    return instance;
  }

  /* Not used, but needs to be overridden, so return a nullptr. */
  OCIO::GpuShaderCreatorRcPtr clone() const override
  {
    return OCIO::GpuShaderCreatorRcPtr();
  }

  /* This is ignored since we query using our own GPU capabilities system. */
  void setTextureMaxWidth(uint /*max_width*/) override {}

  uint getTextureMaxWidth() const noexcept override
  {
    return GPU_max_texture_size();
  }

#if OCIO_VERSION_HEX >= 0x02030000
  void setAllowTexture1D(bool allowed) override
  {
    allow_texture_1D_ = allowed;
  }

  bool getAllowTexture1D() const override
  {
    return allow_texture_1D_;
  }
#endif

  bool addUniform(const char *name, const DoubleGetter &get_double) override
  {
    /* Check if a resource exists with the same name and assert if it is the case, returning false
     * indicates failure to add the uniform for the shader creator. */
    if (!resource_names_.add(std::make_unique<std::string>(name))) {
      BLI_assert_unreachable();
      return false;
    }

    /* Don't use the name argument directly since ShaderCreateInfo only stores references to
     * resource names, instead, use the name that is stored in resource_names_. */
    std::string &resource_name = *resource_names_[resource_names_.size() - 1];
    shader_create_info_.push_constant(Type::float_t, resource_name);

    float_uniforms_.add(resource_name, get_double);

    return true;
  }

  bool addUniform(const char *name, const BoolGetter &get_bool) override
  {
    /* Check if a resource exists with the same name and assert if it is the case, returning false
     * indicates failure to add the uniform for the shader creator. */
    if (!resource_names_.add(std::make_unique<std::string>(name))) {
      BLI_assert_unreachable();
      return false;
    }

    /* Don't use the name argument directly since ShaderCreateInfo only stores references to
     * resource names, instead, use the name that is stored in resource_names_. */
    const std::string &resource_name = *resource_names_[resource_names_.size() - 1];
    shader_create_info_.push_constant(Type::bool_t, resource_name);

    boolean_uniforms_.add(name, get_bool);

    return true;
  }

  bool addUniform(const char *name, const Float3Getter &get_float3) override
  {
    /* Check if a resource exists with the same name and assert if it is the case, returning false
     * indicates failure to add the uniform for the shader creator. */
    if (!resource_names_.add(std::make_unique<std::string>(name))) {
      BLI_assert_unreachable();
      return false;
    }

    /* Don't use the name argument directly since ShaderCreateInfo only stores references to
     * resource names, instead, use the name that is stored in resource_names_. */
    std::string &resource_name = *resource_names_[resource_names_.size() - 1];
    shader_create_info_.push_constant(Type::float3_t, resource_name);

    vector_uniforms_.add(resource_name, get_float3);

    return true;
  }

  bool addUniform(const char *name,
                  const SizeGetter &get_size,
                  const VectorFloatGetter &get_vector_float
#if OCIO_VERSION_HEX >= 0x02050000
                  ,
                  const uint /*maxSize*/
#endif
                  ) override
  {
    /* Check if a resource exists with the same name and assert if it is the case, returning false
     * indicates failure to add the uniform for the shader creator. */
    if (!resource_names_.add(std::make_unique<std::string>(name))) {
      BLI_assert_unreachable();
      return false;
    }

    /* Don't use the name argument directly since ShaderCreateInfo only stores references to
     * resource names, instead, use the name that is stored in resource_names_. */
    std::string &resource_name = *resource_names_[resource_names_.size() - 1];
    shader_create_info_.uniform_buf(buffers_sizes_.size(), "float", resource_name);

    float_buffers_.add(resource_name, get_vector_float);
    buffers_sizes_.add(resource_name, get_size);

    return true;
  }

  bool addUniform(const char *name,
                  const SizeGetter &get_size,
                  const VectorIntGetter &get_vector_int
#if OCIO_VERSION_HEX >= 0x02050000
                  ,
                  const uint /*maxSize*/
#endif
                  ) override
  {
    /* Check if a resource exists with the same name and assert if it is the case, returning false
     * indicates failure to add the uniform for the shader creator. */
    if (!resource_names_.add(std::make_unique<std::string>(name))) {
      BLI_assert_unreachable();
      return false;
    }

    /* Don't use the name argument directly since ShaderCreateInfo only stores references to
     * resource names, instead, use the name that is stored in resource_names_. */
    std::string &resource_name = *resource_names_[resource_names_.size() - 1];
    shader_create_info_.uniform_buf(buffers_sizes_.size(), "int", resource_name);

    int_buffers_.add(name, get_vector_int);
    buffers_sizes_.add(name, get_size);

    return true;
  }

#if OCIO_VERSION_HEX >= 0x02050000
  uint
#else
  void
#endif
  addTexture(const char *texture_name,
             const char *sampler_name,
             uint width,
             uint height,
             TextureType channel,
#if OCIO_VERSION_HEX >= 0x02030000
             OCIO::GpuShaderDesc::TextureDimensions dimensions,
#endif
             OCIO::Interpolation interpolation,
             const float *values) override
  {
    /* Check if a resource exists with the same name and assert if it is the case. */
    if (!resource_names_.add(std::make_unique<std::string>(sampler_name))) {
      BLI_assert_unreachable();
    }

    /* Don't use the name argument directly since ShaderCreateInfo only stores references to
     * resource names, instead, use the name that is stored in resource_names_. */
    const std::string &resource_name = *resource_names_[resource_names_.size() - 1];

    gpu::Texture *texture;
    const gpu::TextureFormat base_format = (channel == TEXTURE_RGB_CHANNEL) ?
                                               gpu::TextureFormat::SFLOAT_32_32_32 :
                                               gpu::TextureFormat::SFLOAT_32;
    const gpu::TextureFormat texture_format = Result::gpu_texture_format(base_format, precision_);
    /* A height of 1 indicates a 1D texture according to the OCIO API. */
#if OCIO_VERSION_HEX >= 0x02030000
    if (dimensions == OCIO::GpuShaderDesc::TEXTURE_1D)
#else
    if (height == 1)
#endif
    {
      texture = GPU_texture_create_1d(
          texture_name, width, 1, texture_format, GPU_TEXTURE_USAGE_SHADER_READ, values);
      shader_create_info_.sampler(
          textures_.size() + 1 + int(lut_), ImageType::Float1D, resource_name);
      if (lut_ && interpolation == OCIO::INTERP_LINEAR) {
        linear_samplers_.add(resource_name, 1);
      }
    }
    else {
      texture = GPU_texture_create_2d(
          texture_name, width, height, 1, texture_format, GPU_TEXTURE_USAGE_SHADER_READ, values);
      shader_create_info_.sampler(
          textures_.size() + 1 + int(lut_), ImageType::Float2D, resource_name);
      if (lut_ && interpolation == OCIO::INTERP_LINEAR) {
        linear_samplers_.add(resource_name, 2);
      }
    }
    GPU_texture_filter_mode(texture, interpolation != OCIO::INTERP_NEAREST);

    textures_.add(sampler_name, texture);
#if OCIO_VERSION_HEX >= 0x02050000
    return textures_.size() - 1;
#endif
  }

#if OCIO_VERSION_HEX >= 0x02050000
  uint
#else
  void
#endif
  add3DTexture(const char *texture_name,
               const char *sampler_name,
               uint size,
               OCIO::Interpolation interpolation,
               const float *values) override
  {
    /* Check if a resource exists with the same name and assert if it is the case. */
    if (!resource_names_.add(std::make_unique<std::string>(sampler_name))) {
      BLI_assert_unreachable();
    }

    /* Don't use the name argument directly since ShaderCreateInfo only stores references to
     * resource names, instead, use the name that is stored in resource_names_. */
    const std::string &resource_name = *resource_names_[resource_names_.size() - 1];
    shader_create_info_.sampler(
        textures_.size() + 1 + int(lut_), ImageType::Float3D, resource_name);
    if (lut_ && interpolation == OCIO::INTERP_LINEAR) {
      linear_samplers_.add(resource_name, 3);
    }

    gpu::Texture *texture = GPU_texture_create_3d(
        texture_name,
        size,
        size,
        size,
        1,
        Result::gpu_texture_format(gpu::TextureFormat::SFLOAT_32_32_32, precision_),
        GPU_TEXTURE_USAGE_SHADER_READ,
        values);
    GPU_texture_filter_mode(texture, interpolation != OCIO::INTERP_NEAREST);

    textures_.add(sampler_name, texture);
#if OCIO_VERSION_HEX >= 0x02050000
    return textures_.size() - 1;
#endif
  }

  /* This gets called before the finalize() method to construct the shader code. We just
   * concatenate the code except for the declarations section. That's because the ShaderCreateInfo
   * will add the declaration itself. */
  void createShaderText(const char * /*parameter_declarations*/,
#if OCIO_VERSION_HEX >= 0x02050000
                        const char * /*texture_declarations*/,
#endif
                        const char *helper_methods,
                        const char *function_header,
                        const char *function_body,
                        const char *function_footer) override
  {
    shader_code_ += helper_methods;
    shader_code_ += function_header;
    shader_code_ += function_body;
    shader_code_ += function_footer;
  }

  /* This gets called when all resources were added using the respective addUniform() or
   * add[3D]Texture() methods and the shader code was generated using the createShaderText()
   * method. That is, we are ready to complete the ShaderCreateInfo and create a shader from it. */
  void finalize() override
  {
    GpuShaderCreator::finalize();

    shader_create_info_.local_group_size(16, 16);
    shader_create_info_.sampler(0, ImageType::Float2D, input_sampler_name());
    if (lut_) {
      shader_create_info_.define("OCIO_LUT");
      shader_create_info_.sampler(1, ImageType::Float2D, "factor_tx");
      use_precise_lut_sampling();
    }
    shader_create_info_.builtins(BuiltinBits::GLOBAL_INVOCATION_ID);
    shader_create_info_.push_constant(Type::bool_t, "premultiply_output");
    shader_create_info_.image(0,
                              Result::gpu_texture_format(ResultType::Color, precision_),
                              Qualifier::write,
                              ImageReadWriteType::Float2D,
                              output_image_name());
    shader_create_info_.compute_source("gpu_shader_compositor_ocio_processor.glsl");
    shader_create_info_.generated_sources.append(
        {"gpu_shader_compositor_ocio_processor_lib.glsl",
         {},
         GPU_shader_preprocess_source(shader_code_, shader_create_info_)});

    GPUShaderCreateInfo *info = reinterpret_cast<GPUShaderCreateInfo *>(&shader_create_info_);
    shader_ = GPU_shader_create_from_info(info);
  }

  gpu::Shader *bind_shader_and_resources()
  {
    if (!shader_) {
      return nullptr;
    }

    GPU_shader_bind(shader_);

    for (auto item : float_uniforms_.items()) {
      GPU_shader_uniform_1f(shader_, item.key.c_str(), item.value());
    }

    for (auto item : boolean_uniforms_.items()) {
      GPU_shader_uniform_1b(shader_, item.key.c_str(), item.value());
    }

    for (auto item : vector_uniforms_.items()) {
      GPU_shader_uniform_3fv(shader_, item.key.c_str(), item.value().data());
    }

    for (auto item : float_buffers_.items()) {
      gpu::UniformBuf *buffer = GPU_uniformbuf_create_ex(
          buffers_sizes_.lookup(item.key)(), item.value(), item.key.c_str());
      const int ubo_location = GPU_shader_get_ubo_binding(shader_, item.key.c_str());
      GPU_uniformbuf_bind(buffer, ubo_location);
      uniform_buffers_.append(buffer);
    }

    for (auto item : int_buffers_.items()) {
      gpu::UniformBuf *buffer = GPU_uniformbuf_create_ex(
          buffers_sizes_.lookup(item.key)(), item.value(), item.key.c_str());
      const int ubo_location = GPU_shader_get_ubo_binding(shader_, item.key.c_str());
      GPU_uniformbuf_bind(buffer, ubo_location);
      uniform_buffers_.append(buffer);
    }

    for (auto item : textures_.items()) {
      const int texture_image_unit = GPU_shader_get_sampler_binding(shader_, item.key.c_str());
      GPU_texture_bind(item.value, texture_image_unit);
    }

    return shader_;
  }

  void unbind_shader_and_resources()
  {
    for (gpu::UniformBuf *buffer : uniform_buffers_) {
      GPU_uniformbuf_unbind(buffer);
      GPU_uniformbuf_free(buffer);
    }
    uniform_buffers_.clear();

    for (gpu::Texture *texture : textures_.values()) {
      GPU_texture_unbind(texture);
    }

    GPU_shader_unbind();
  }

  const char *input_sampler_name()
  {
    return "input_tx";
  }

  const char *output_image_name()
  {
    return "output_img";
  }

  ~GPUShaderCreator() override
  {
    for (gpu::Texture *texture : textures_.values()) {
      GPU_texture_free(texture);
    }

    GPU_shader_free(shader_);
  }

 private:
  /* Keep OCIO's coordinates and transforms, but interpolate LUTs in float rather than using
   * hardware weights. Short 1D shapers otherwise visibly diverge from the CPU processor. */
  void use_precise_lut_sampling()
  {
    bool dimensions[4] = {};
    for (const auto item : linear_samplers_.items()) {
      dimensions[item.value] = true;
      const std::string call = "texture(" + item.key + ",";
      size_t position = 0;
      while ((position = shader_code_.find(call, position)) != std::string::npos) {
        shader_code_.replace(position, 7, "ocio_lut_linear");
        position += 15;
      }
    }
    std::string helpers;
    if (dimensions[1]) {
      helpers += R"GLSL(
float4 ocio_lut_linear(sampler1D tx, float coordinate)
{
  int extent = textureSize(tx, 0);
  float position = coordinate * float(extent) - 0.5f;
  int first = int(floor(position));
  float weight = position - float(first);
  return mix(texelFetch(tx, clamp(first, 0, extent - 1), 0),
             texelFetch(tx, clamp(first + 1, 0, extent - 1), 0), weight);
}
)GLSL";
    }
    if (dimensions[2]) {
      helpers += R"GLSL(
float4 ocio_lut_linear(sampler2D tx, float2 coordinate)
{
  int2 extent = textureSize(tx, 0);
  float2 position = coordinate * float2(extent) - 0.5f;
  int2 first = int2(floor(position));
  float2 weight = position - float2(first);
  int2 last = extent - 1;
  float4 low = mix(texelFetch(tx, clamp(first, int2(0), last), 0),
                   texelFetch(tx, clamp(first + int2(1, 0), int2(0), last), 0), weight.x);
  float4 high = mix(texelFetch(tx, clamp(first + int2(0, 1), int2(0), last), 0),
                    texelFetch(tx, clamp(first + int2(1, 1), int2(0), last), 0), weight.x);
  return mix(low, high, weight.y);
}
)GLSL";
    }
    if (dimensions[3]) {
      helpers += R"GLSL(
float4 ocio_lut_linear(sampler3D tx, float3 coordinate)
{
  int3 extent = textureSize(tx, 0);
  float3 position = coordinate * float3(extent) - 0.5f;
  int3 first = int3(floor(position));
  float3 weight = position - float3(first);
  int3 last = extent - 1;
  float4 low_y0 = mix(texelFetch(tx, clamp(first, int3(0), last), 0),
                      texelFetch(tx, clamp(first + int3(1, 0, 0), int3(0), last), 0), weight.x);
  float4 low_y1 = mix(texelFetch(tx, clamp(first + int3(0, 1, 0), int3(0), last), 0),
                      texelFetch(tx, clamp(first + int3(1, 1, 0), int3(0), last), 0), weight.x);
  float4 high_y0 = mix(texelFetch(tx, clamp(first + int3(0, 0, 1), int3(0), last), 0),
                       texelFetch(tx, clamp(first + int3(1, 0, 1), int3(0), last), 0), weight.x);
  float4 high_y1 = mix(texelFetch(tx, clamp(first + int3(0, 1, 1), int3(0), last), 0),
                       texelFetch(tx, clamp(first + int3(1, 1, 1), int3(0), last), 0), weight.x);
  return mix(mix(low_y0, low_y1, weight.y), mix(high_y0, high_y1, weight.y), weight.z);
}
)GLSL";
    }
    shader_code_ = helpers + shader_code_;
  }

  Map<std::string, int> linear_samplers_;

  /* The processor shader and the ShaderCreateInfo used to construct it. Constructed and
   * initialized in the finalize() method. */
  gpu::Shader *shader_ = nullptr;
  ShaderCreateInfo shader_create_info_ = ShaderCreateInfo("OCIO_Processor");

  /* Stores the generated OCIOMain function as well as a number of helper functions. Initialized in
   * the createShaderText() method. */
  std::string shader_code_;

  /* Maps that associates the name of a uniform with a getter function that returns its value.
   * Initialized in the respective addUniform() methods. */
  Map<std::string, DoubleGetter> float_uniforms_;
  Map<std::string, BoolGetter> boolean_uniforms_;
  Map<std::string, Float3Getter> vector_uniforms_;

  /* Maps that associates the name of uniform buffer objects with a getter function that returns
   * its values. Initialized in the respective addUniform() methods. */
  Map<std::string, VectorFloatGetter> float_buffers_;
  Map<std::string, VectorIntGetter> int_buffers_;

  /* A map that associates the name of uniform buffer objects with a getter functions that returns
   * its number of elements. Initialized in the respective addUniform() methods. */
  Map<std::string, SizeGetter> buffers_sizes_;

  /* A map that associates the name of a sampler with its corresponding texture. Initialized in the
   * addTexture() and add3DTexture() methods. */
  Map<std::string, gpu::Texture *> textures_;

  /* A vector set that stores the names of all the resources used by the shader. This is used to:
   *   1. Check for name collisions when adding new resources.
   *   2. Store the resource names throughout the construction of the shader since the
   *      ShaderCreateInfo class only stores references to resources names. */
  VectorSet<std::unique_ptr<std::string>> resource_names_;

  /* A vectors that stores the created uniform buffers when bind_shader_and_resources() is called,
   * so that they can be properly unbound and freed in the unbind_shader_and_resources() method. */
  Vector<gpu::UniformBuf *> uniform_buffers_;

#if OCIO_VERSION_HEX >= 0x02030000
  /* Allow creating 1D textures, or only use 2D textures. */
  bool allow_texture_1D_ = true;
#endif

  /* The precision of the OCIO resources as well as the output image. */
  ResultPrecision precision_;
  bool lut_ = false;
};

/* --------------------------------------------------------------------
 * File LUT.
 */

int ocio_lut_image_size(const int width, const int height)
{
  if (width <= 0 || height <= 0) {
    return 0;
  }
  const int64_t pixels = int64_t(width) * height;
  /* OCIO's maximum 3D LUT grid size is 129. */
  for (int size = 2; size <= 129; size++) {
    if (int64_t(size) * size * size == pixels && width % size == 0 && height % size == 0) {
      return size;
    }
  }
  return 0;
}

static OCIO::Lut3DTransformRcPtr image_lut_transform(const NodeCompositorLUT &settings,
                                                     const char *data,
                                                     const size_t size,
                                                     const char *filepath,
                                                     std::string &error)
{
  /* LUT texels are encoded table values. Never apply image gamma or alpha premultiplication. */
  const ImBufFlags flags = ImBufFlags::ByteData | ImBufFlags::NoColorspaceConvert |
                           ImBufFlags::AlphaChannelPacked;
  ColorManagedColorspaceSettings colorspace;
  IMB_colormanagement_colorspace_settings_set(&colorspace, "Non-Color");
  const auto *bytes = reinterpret_cast<const unsigned char *>(data);
  std::unique_ptr<ImBuf, decltype(&IMB_freeImBuf)> image(
      IMB_load_image_from_memory(
          bytes, size, flags | ImBufFlags::Test, filepath, filepath, &colorspace),
      IMB_freeImBuf);
  if (!image) {
    error = "LUT: could not read the image";
    return nullptr;
  }
  const int grid_size = ocio_lut_image_size(image->x, image->y);
  if (!grid_size) {
    error =
        "LUT: image must contain N square slices of N x N pixels (N = 2..129), without padding";
    return nullptr;
  }
  const int width = image->x, height = image->y;
  image.reset(IMB_load_image_from_memory(bytes, size, flags, filepath, filepath, &colorspace));
  if (!image || image->x != width || image->y != height ||
      (!image->float_data() && !image->byte_data()))
  {
    error = "LUT: could not decode the image pixels";
    return nullptr;
  }
  const int channels = image->channels ? image->channels : 4;
  if (image->float_data() && channels < 3) {
    error = "LUT: the image must have RGB channels";
    return nullptr;
  }
  const int columns = width / grid_size;
  auto lut = OCIO::Lut3DTransform::Create(grid_size);
  for (int b = 0; b < grid_size; b++) {
    for (int g = 0; g < grid_size; g++) {
      const int green = (settings.image_flags & CMP_NODE_LUT_IMAGE_FLIP_GREEN) ?
                            grid_size - 1 - g :
                            g;
      const int y = (b / columns) * grid_size + green;
      /* ImBuf rows start at the bottom. Default atlas ordering starts at the file's top-left. */
      const int row = (settings.image_flags & CMP_NODE_LUT_IMAGE_FLIP_Y) ? y : height - 1 - y;
      for (int r = 0; r < grid_size; r++) {
        const int x = (b % columns) * grid_size + r;
        const int64_t index = int64_t(row) * width + x;
        float rgb[3];
        for (int c = 0; c < 3; c++) {
          rgb[c] = image->float_data() ? image->float_data()[index * channels + c] :
                                         image->byte_data()[index * 4 + c] / 255.0f;
          if (!std::isfinite(rgb[c])) {
            error = "LUT: the image contains non-finite RGB values";
            return nullptr;
          }
        }
        lut->setValue(r, g, b, rgb[0], rgb[1], rgb[2]);
      }
    }
  }
  return lut;
}

struct OCIOLUT::Impl {
  OCIO::ConstCPUProcessorRcPtr cpu;
  OCIO::ConstGPUProcessorRcPtr gpu;
  std::shared_ptr<GPUShaderCreator> shader;
  ResultPrecision precision;
  std::string error;
};

OCIOLUT::OCIOLUT(Context &context, const NodeCompositorLUT &settings, const std::string &filepath)
    : impl_(std::make_unique<Impl>())
{
  impl_->precision = context.get_precision();
  constexpr size_t max_size = 64 * 1024 * 1024;
  if (!settings.packed_file &&
      !BLI_path_extension_check_n(
          filepath.c_str(), ".cube", ".png", ".exr", ".tif", ".tiff", nullptr))
  {
    impl_->error = "LUT: select a .cube, PNG, TIFF, or EXR file";
    return;
  }
  const size_t size = settings.packed_file ? size_t(settings.packed_file->size) :
                                             BLI_file_size(filepath.c_str());
  if (size == 0 || size > max_size) {
    impl_->error = "LUT: file is missing, empty, or exceeds 64 MiB";
    return;
  }

  size_t read_size = 0;
  auto free_data = [](char *data) {
    if (data) {
      MEM_delete(data);
    }
  };
  std::unique_ptr<char, decltype(free_data)> owned_data(
      settings.packed_file ?
          nullptr :
          static_cast<char *>(BLI_file_read_binary_as_mem(filepath.c_str(), 0, &read_size)),
      free_data);
  const char *data = settings.packed_file ? static_cast<const char *>(settings.packed_file->data) :
                                            owned_data.get();
  if (!data || (!settings.packed_file && read_size != size)) {
    impl_->error = "LUT: could not read the file";
    return;
  }

  const eImbFileType type = IMB_test_image_type_from_memory(
      reinterpret_cast<const unsigned char *>(data), size);
  const bool is_image = ELEM(type, IMB_FTYPE_PNG, IMB_FTYPE_TIF, IMB_FTYPE_OPENEXR);
  if ((!is_image && type != IMB_FTYPE_NONE) ||
      (!is_image && !settings.packed_file && !BLI_path_extension_check(filepath.c_str(), ".cube")))
  {
    impl_->error = "LUT: invalid or unsupported image; use PNG, TIFF, or flat EXR";
    return;
  }

  /* OCIO caches file contents by path. Immutable content-addressed snapshots allow both reload
   * and packed files without flushing global caches or changing the user's OCIO configuration. */
  char snapshot[FILE_MAX] = "";
  if (!is_image) {
    unsigned char digest[16];
    char hex[33];
    BLI_hash_md5_buffer(data, size, digest);
    BLI_hash_md5_to_hexdigest(digest, hex);
    BLI_path_join(snapshot,
                  sizeof(snapshot),
                  BKE_tempdir_session(),
                  "compositor_lut",
                  (std::string(hex) + ".cube").c_str());
    {
      static std::mutex snapshot_mutex;
      std::lock_guard lock(snapshot_mutex);
      if (!BLI_exists(snapshot)) {
        if (!BLI_file_ensure_parent_dir_exists(snapshot)) {
          impl_->error = "LUT: could not create the temporary cache directory";
          return;
        }
        FILE *file = BLI_fopen(snapshot, "wb");
        if (!file) {
          impl_->error = "LUT: could not create the temporary file";
          return;
        }
        const bool written = fwrite(data, 1, size, file) == size;
        const bool closed = fclose(file) == 0;
        if (!written || !closed) {
          BLI_delete(snapshot, false, false);
          impl_->error = "LUT: could not write the temporary file";
          return;
        }
      }
    }
  }
  try {
    const OCIO::ConstConfigRcPtr config = OCIO::GetCurrentConfig();
    OCIO::GroupTransformRcPtr group = OCIO::GroupTransform::Create();
    const bool raw = settings.preset == CMP_NODE_LUT_RAW;
    const char *input_space = settings.preset == CMP_NODE_LUT_CUSTOM ? settings.input_color_space :
                              settings.preset == CMP_NODE_LUT_SRGB   ? "sRGB" :
                                                                       "scene_linear";
    const char *output_space = settings.preset == CMP_NODE_LUT_CUSTOM &&
                                       !settings.output_matches_input ?
                                   settings.output_color_space :
                                   input_space;
    if (settings.direction) {
      std::swap(input_space, output_space);
    }
    if (!raw) {
      auto conversion = OCIO::ColorSpaceTransform::Create();
      conversion->setSrc("scene_linear");
      conversion->setDst(input_space);
      group->appendTransform(conversion);
    }
    const OCIO::Interpolation interpolation[] = {
        OCIO::INTERP_BEST, OCIO::INTERP_LINEAR, OCIO::INTERP_TETRAHEDRAL};
    const auto method = interpolation[std::clamp(settings.interpolation, 0, 2)];
    const auto direction = settings.direction ? OCIO::TRANSFORM_DIR_INVERSE :
                                                OCIO::TRANSFORM_DIR_FORWARD;
    OCIO::TransformRcPtr lut;
    if (is_image) {
      auto image_lut = image_lut_transform(settings, data, size, filepath.c_str(), impl_->error);
      if (!image_lut) {
        return;
      }
      image_lut->setInterpolation(method);
      image_lut->setDirection(direction);
      lut = image_lut;
    }
    else {
      auto file_lut = OCIO::FileTransform::Create();
      file_lut->setSrc(snapshot);
      file_lut->setInterpolation(method);
      file_lut->setDirection(direction);
      lut = file_lut;
    }
    group->appendTransform(lut);
    if (!raw) {
      auto conversion = OCIO::ColorSpaceTransform::Create();
      conversion->setSrc(output_space);
      conversion->setDst("scene_linear");
      group->appendTransform(conversion);
    }
    const auto processor = config->getProcessor(group);
    impl_->cpu = processor->getDefaultCPUProcessor();
    impl_->gpu = processor->getDefaultGPUProcessor();
  }
  catch (const OCIO::Exception &error) {
    impl_->error = "LUT: " + std::string(error.what());
  }
}

OCIOLUT::~OCIOLUT() = default;

const std::string &OCIOLUT::error() const
{
  return impl_->error;
}

void OCIOLUT::apply_rgb(float rgb[3]) const
{
  impl_->cpu->applyRGB(rgb);
}

gpu::Shader *OCIOLUT::bind_shader_and_resources()
{
  if (!impl_->error.empty()) {
    return nullptr;
  }
  if (!impl_->shader) {
    try {
      impl_->shader = GPUShaderCreator::Create(impl_->precision, true);
      auto ocio_shader_creator = std::static_pointer_cast<OCIO::GpuShaderCreator>(impl_->shader);
      impl_->gpu->extractGpuShaderInfo(ocio_shader_creator);
    }
    catch (const OCIO::Exception &error) {
      impl_->error = "LUT GPU: " + std::string(error.what());
      return nullptr;
    }
  }
  gpu::Shader *shader = impl_->shader->bind_shader_and_resources();
  if (!shader) {
    impl_->error = "LUT: GPU shader compilation failed";
  }
  return shader;
}

void OCIOLUT::unbind_shader_and_resources()
{
  impl_->shader->unbind_shader_and_resources();
}

void OCIOLUTContainer::reset()
{
  map_.remove_if([](auto item) { return !item.value->needed; });
  for (auto &value : map_.values()) {
    value->needed = false;
  }
}

OCIOLUT &OCIOLUTContainer::get(Context &context,
                               const NodeCompositorLUT &settings,
                               const std::string &filepath)
{
  BLI_stat_t stat = {};
  if (!settings.packed_file) {
    if (BLI_stat(filepath.c_str(), &stat) != 0) {
      stat = {};
    }
  }
  std::string key;
  auto append = [&](const std::string &value) {
    key.append(value);
    key.push_back('\0');
  };
  append(filepath);
  append(settings.packed_file ? settings.packed_hash : "external");
  append(std::to_string(stat.st_mtime));
  append(std::to_string(stat.st_size));
  append(std::to_string(settings.reload_version));
  append(std::to_string(settings.preset));
  append(std::to_string(settings.interpolation));
  append(std::to_string(settings.direction));
  append(settings.input_color_space);
  append(settings.output_color_space);
  append(std::to_string(settings.output_matches_input));
  append(std::to_string(settings.image_flags));
  append(OCIO::GetCurrentConfig()->getCacheID());
  append(std::to_string(int(context.get_precision())));
  OCIOLUT &resource = *map_.lookup_or_add_cb(
      key, [&]() { return std::make_unique<OCIOLUT>(context, settings, filepath); });
  resource.needed = true;
  return resource;
}

/* ------------------------------------------------------------------------------------------------
 * OCIO Color Space Conversion Shader Key.
 */

OCIOColorSpaceConversionShaderKey::OCIOColorSpaceConversionShaderKey(
    const std::string &source, const std::string &target, const std::string &config_cache_id)
    : source(source), target(target), config_cache_id(config_cache_id)
{
}

uint64_t OCIOColorSpaceConversionShaderKey::hash() const
{
  return get_default_hash(source, target, config_cache_id);
}

bool operator==(const OCIOColorSpaceConversionShaderKey &a,
                const OCIOColorSpaceConversionShaderKey &b)
{
  return a.source == b.source && a.target == b.target && a.config_cache_id == b.config_cache_id;
}

/* --------------------------------------------------------------------
 * OCIO Color Space Conversion Shader.
 */

OCIOColorSpaceConversionShader::OCIOColorSpaceConversionShader(Context &context,
                                                               std::string source,
                                                               std::string target)
{
  /* Create a GPU shader creator and construct it based on the transforms in the default GPU
   * processor. */
  shader_creator_ = GPUShaderCreator::Create(context.get_precision());

  /* Get a GPU processor that transforms the source color space to the target color space. */
  try {
    OCIO::ConstConfigRcPtr config = OCIO::GetCurrentConfig();
    OCIO::ConstProcessorRcPtr processor = config->getProcessor(source.c_str(), target.c_str());
    OCIO::ConstGPUProcessorRcPtr gpu_processor = processor->getDefaultGPUProcessor();

    auto ocio_shader_creator = std::static_pointer_cast<OCIO::GpuShaderCreator>(shader_creator_);
    gpu_processor->extractGpuShaderInfo(ocio_shader_creator);
  }
  catch (const OCIO::Exception &e) {
    CLOG_ERROR(&LOG, "Failed to create OpenColorIO shader: %s", e.what());
  }
}

gpu::Shader *OCIOColorSpaceConversionShader::bind_shader_and_resources()
{
  return shader_creator_->bind_shader_and_resources();
}

void OCIOColorSpaceConversionShader::unbind_shader_and_resources()
{
  shader_creator_->unbind_shader_and_resources();
}

const char *OCIOColorSpaceConversionShader::input_sampler_name()
{
  return shader_creator_->input_sampler_name();
}

const char *OCIOColorSpaceConversionShader::output_image_name()
{
  return shader_creator_->output_image_name();
}

/* --------------------------------------------------------------------
 * OCIO Color Space Conversion Shader Container.
 */

void OCIOColorSpaceConversionShaderContainer::reset()
{
  /* First, delete all resources that are no longer needed. */
  map_.remove_if([](auto item) { return !item.value->needed; });

  /* Second, reset the needed status of the remaining resources to false to ready them to track
   * their needed status for the next evaluation. */
  for (auto &value : map_.values()) {
    value->needed = false;
  }
}

OCIOColorSpaceConversionShader &OCIOColorSpaceConversionShaderContainer::get(Context &context,
                                                                             std::string source,
                                                                             std::string target)
{
  /* Use the config cache ID in the cache key in case the configuration changed at runtime. */
  std::string config_cache_id = OCIO::GetCurrentConfig()->getCacheID();

  const OCIOColorSpaceConversionShaderKey key(source, target, config_cache_id);

  OCIOColorSpaceConversionShader &shader = *map_.lookup_or_add_cb(key, [&]() {
    return std::make_unique<OCIOColorSpaceConversionShader>(context, source, target);
  });

  shader.needed = true;
  return shader;
}

/* ------------------------------------------------------------------------------------------------
 * OCIO To Display Shader Key.
 */

OCIOToDisplayShaderKey::OCIOToDisplayShaderKey(const ColorManagedDisplaySettings &display_settings,
                                               const ColorManagedViewSettings &view_settings,
                                               const bool inverse,
                                               const std::string &config_cache_id)
    : display_device(display_settings.display_device),
      view_transform(view_settings.view_transform),
      look(view_settings.look),
      inverse(inverse),
      config_cache_id(config_cache_id)
{
}

uint64_t OCIOToDisplayShaderKey::hash() const
{
  return get_default_hash(
      get_default_hash(display_device, view_transform, look, (inverse) ? "inverse" : "forward"),
      config_cache_id);
}

bool operator==(const OCIOToDisplayShaderKey &a, const OCIOToDisplayShaderKey &b)
{
  return a.display_device == b.display_device && a.view_transform == b.view_transform &&
         a.look == b.look && a.inverse == b.inverse && a.config_cache_id == b.config_cache_id;
}

/* --------------------------------------------------------------------
 * OCIO To Display Shader.
 */

OCIOToDisplayShader::OCIOToDisplayShader(Context &context,
                                         const ColorManagedDisplaySettings &display_settings,
                                         const ColorManagedViewSettings &view_settings,
                                         const bool inverse)
{
  /* Create a GPU shader creator and construct it based on the transforms in the default GPU
   * processor. */
  shader_creator_ = GPUShaderCreator::Create(context.get_precision());

  /* Get a GPU processor that transforms the display_device color space to the view_transform color
   * space. */
  try {
    OCIO::ConstConfigRcPtr config = OCIO::GetCurrentConfig();

    OCIO::TransformRcPtr group = ocio::create_ocio_display_transform(
        config,
        display_settings.display_device,
        view_settings.view_transform,
        view_settings.look,
        "scene_linear");

    if (inverse) {
      group->setDirection(OCIO::TRANSFORM_DIR_INVERSE);
    }

    OCIO::ConstProcessorRcPtr processor = config->getProcessor(group);
    OCIO::ConstGPUProcessorRcPtr gpu_processor = processor->getDefaultGPUProcessor();

    auto ocio_shader_creator = std::static_pointer_cast<OCIO::GpuShaderCreator>(shader_creator_);
    gpu_processor->extractGpuShaderInfo(ocio_shader_creator);
  }
  catch (const OCIO::Exception &e) {
    CLOG_ERROR(&LOG, "Failed to create OpenColorIO shader: %s", e.what());
  }
}

gpu::Shader *OCIOToDisplayShader::bind_shader_and_resources()
{
  gpu::Shader *shader = shader_creator_->bind_shader_and_resources();
  if (shader) {
    GPU_shader_uniform_1b(shader, "premultiply_output", false);
  }
  return shader;
}

void OCIOToDisplayShader::unbind_shader_and_resources()
{
  shader_creator_->unbind_shader_and_resources();
}

const char *OCIOToDisplayShader::input_sampler_name()
{
  return shader_creator_->input_sampler_name();
}

const char *OCIOToDisplayShader::output_image_name()
{
  return shader_creator_->output_image_name();
}

/* --------------------------------------------------------------------
 * OCIO To Display Shader Container.
 */

void OCIOToDisplayShaderContainer::reset()
{
  /* First, delete all resources that are no longer needed. */
  map_.remove_if([](auto item) { return !item.value->needed; });

  /* Second, reset the needed status of the remaining resources to false to ready them to
   * track their needed status for the next evaluation. */
  for (auto &value : map_.values()) {
    value->needed = false;
  }
}

OCIOToDisplayShader &OCIOToDisplayShaderContainer::get(
    Context &context,
    const ColorManagedDisplaySettings &display_settings,
    const ColorManagedViewSettings &view_settings,
    const bool inverse)
{
  /* Use the config cache ID in the cache key in case the configuration changed at runtime. */
  std::string config_cache_id = OCIO::GetCurrentConfig()->getCacheID();

  const OCIOToDisplayShaderKey key(display_settings, view_settings, inverse, config_cache_id);

  OCIOToDisplayShader &shader = *map_.lookup_or_add_cb(key, [&]() {
    return std::make_unique<OCIOToDisplayShader>(
        context, display_settings, view_settings, inverse);
  });

  shader.needed = true;
  return shader;
}

}  // namespace blender::compositor
