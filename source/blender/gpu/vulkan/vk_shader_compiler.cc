/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "BKE_appdir.hh"

#include "BLI_fileops.hh"
#include "BLI_path_utils.hh"
#include "BLI_simd.hh"
#include "GPU_capabilities.hh"
#ifdef _WIN32
#  include "BLI_winstuff.hh"
#endif

#include "BLI_system.hh"
#include BLI_SYSTEM_PID_H

#include "vk_backend.hh"
#include "vk_device.hh"
#include "vk_shader.hh"
#include "vk_shader_compiler.hh"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <string>

#include "CLG_log.h"

#include "spirv/unified1/spirv.h"

#ifdef WITH_GPU_BACKEND_TESTS
#  if 0
#    include "spirv-tools/libspirv.h"
#  endif
#endif

namespace blender::gpu {

VKShaderCompiler::VKShaderCompiler()
    : ShaderCompiler(GPU_max_parallel_compilations(), GPUWorker::ContextType::Main, true)
{
}

void VKShaderCompiler::specialize_shader(const ShaderSpecialization &specialization)
{
  BLI_assert(specialization.graphics_pipeline);
  shader::PipelineState pipeline = *specialization.graphics_pipeline;
  pipeline.specialization_constants_ = specialization.constants.values;
  auto &vk_shader = *static_cast<VKShader *>(specialization.shader);
  vk_shader.ensure_graphics_pipelines(Span<shader::PipelineState>(&pipeline, 1), false);
}

static CLG_LogRef LOG = {"gpu.vulkan"};

static std::optional<std::string> cache_dir_get()
{
  static std::optional<std::string> result = []() -> std::optional<std::string> {
    static char tmp_dir_buffer[FILE_MAX];
    /* Shader builder doesn't return the correct appdir. */
    BKE_appdir_folder_caches(tmp_dir_buffer, sizeof(tmp_dir_buffer));

    std::string cache_dir = std::string(tmp_dir_buffer) + "vk-spirv-cache" + SEP_STR;
    BLI_dir_create_recursive(cache_dir.c_str());
    return cache_dir;
  }();

  return result;
}

/* -------------------------------------------------------------------- */
/** \name SPIR-V disk cache
 * \{ */

struct SPIRVSidecar {
  /** Size of the SPIRV binary. */
  uint64_t spirv_size;
  /** SPIR-V version. */
  uint32_t spirv_version;
};

static constexpr int64_t SPIRV_HEADER_WORD_COUNT = 5;

static uint32_t spirv_target_version_get()
{
  const VKDevice &device = VKBackend::get().device;
  return device.extensions_get().spirv_1_4 ? shaderc_spirv_version_1_4 : shaderc_spirv_version_1_3;
}

static bool read_spirv_from_disk(VKShaderModule &shader_module, StringRef hash_extra)
{
  if (G.debug & G_DEBUG_GPU_SHADER_DEBUG_INFO) {
    /* Debug information is not part of the cached SPIR-V, so don't use the cache in this case. */
    return false;
  }
  if (!cache_dir_get().has_value()) {
    return false;
  }
  shader_module.build_sources_hash(hash_extra);
  std::string spirv_path = (*cache_dir_get()) + SEP_STR + shader_module.sources_hash + ".spv";
  std::string sidecar_path = (*cache_dir_get()) + SEP_STR + shader_module.sources_hash +
                             ".sidecar.bin";

  /* Read sidecar. */
  fstream sidecar_file(sidecar_path, std::ios::binary | std::ios::in | std::ios::ate);
  SPIRVSidecar sidecar = {};
  if (sidecar_file.tellg() != sizeof(sidecar) ||
      !sidecar_file.seekg(0).read(reinterpret_cast<char *>(&sidecar), sizeof(sidecar)))
  {
    return false;
  }
  sidecar_file.close();

  if (sidecar.spirv_version != spirv_target_version_get()) {
    CLOG_TRACE(
        &LOG,
        "Recompiling SPIR-V cache entry for %s. Cached SPIR-V version %08x, but %08x requested.",
        spirv_path.c_str(),
        sidecar.spirv_version,
        spirv_target_version_get());
    return false;
  }

  /* Read the SPIR-V binary and reject incomplete or invalid cache entries. */
  fstream spirv_file(spirv_path, std::ios::binary | std::ios::in | std::ios::ate);
  constexpr std::streamsize word_size = sizeof(uint32_t);
  const std::streamsize size = spirv_file.tellg();
  if (size <= SPIRV_HEADER_WORD_COUNT * word_size || size % word_size != 0 ||
      uint64_t(size) != sidecar.spirv_size)
  {
    return false;
  }
  Vector<uint32_t> binary(size / word_size);
  if (!spirv_file.seekg(0).read(reinterpret_cast<char *>(binary.data()), size) ||
      binary[0] != SpvMagicNumber || binary[1] != sidecar.spirv_version || binary[3] == 0 ||
      binary[4] != 0)
  {
    CLOG_DEBUG(&LOG, "Ignoring invalid SPIR-V cache entry %s", spirv_path.c_str());
    return false;
  }
  spirv_file.close();

  const auto now = std::filesystem::file_time_type::clock::now();
  std::error_code error;
  std::filesystem::last_write_time(std::u8string(spirv_path.begin(), spirv_path.end()), now, error);
  std::filesystem::last_write_time(
      std::u8string(sidecar_path.begin(), sidecar_path.end()), now, error);

  shader_module.spirv_binary = std::move(binary);
  CLOG_TRACE(&LOG, "reading SpirV from disk %s", spirv_path.c_str());
  return true;
}

static bool write_cache_file(const std::string &path, const char *data, std::streamsize size)
{
  static std::atomic<uint64_t> next_write_id = 0;
  const std::string tmp_path = path + ".tmp-" + std::to_string(getpid()) + "-" +
                               std::to_string(next_write_id.fetch_add(1, std::memory_order_relaxed));
  fstream file(tmp_path, std::ios::binary | std::ios::out);
  if (!file.is_open()) {
    return false;
  }
  file.write(data, size);
  file.close();
  if (file && BLI_rename_overwrite(tmp_path.c_str(), path.c_str()) == 0) {
    return true;
  }
  BLI_delete(tmp_path.c_str(), false, false);
  return false;
}

static void write_spirv_to_disk(VKShaderModule &shader_module)
{
  if (G.debug & G_DEBUG_GPU_SHADER_DEBUG_INFO) {
    /* Debug information is not part of the cached SPIR-V, so don't use the cache in this case. */
    return;
  }
  if (!cache_dir_get().has_value()) {
    return;
  }

  /* Write the SPIR-V binary. Note that spirv_binary is used instead of compilation_result so
   * that any post-processing (e.g. the injected OpString shader name) is stored in the cache and
   * does not have to be redone every time the cache entry is read back. */
  std::string spirv_path = (*cache_dir_get()) + SEP_STR + shader_module.sources_hash + ".spv";
  const uint64_t size = shader_module.spirv_binary.size() * sizeof(uint32_t);
  if (!write_cache_file(
          spirv_path, reinterpret_cast<const char *>(shader_module.spirv_binary.data()), size))
  {
    CLOG_DEBUG(&LOG, "Could not write SPIR-V cache entry %s", spirv_path.c_str());
    return;
  }

  /* Write the sidecar */
  const SPIRVSidecar sidecar = {size, spirv_target_version_get()};
  std::string sidecar_path = (*cache_dir_get()) + SEP_STR + shader_module.sources_hash +
                             ".sidecar.bin";
  if (!write_cache_file(sidecar_path, reinterpret_cast<const char *>(&sidecar), sizeof(sidecar))) {
    CLOG_DEBUG(&LOG, "Could not write SPIR-V cache metadata %s", sidecar_path.c_str());
    return;
  }
  CLOG_TRACE(&LOG, "write SpirV to disk %s", spirv_path.c_str());
}

void VKShaderCompiler::cache_dir_clear_old()
{
  if (!cache_dir_get().has_value()) {
    return;
  }

  direntry *entries = nullptr;
  uint32_t dir_len = BLI_filelist_dir_contents(cache_dir_get()->c_str(), &entries);
  for (int i : IndexRange(dir_len)) {
    direntry entry = entries[i];
    if (S_ISDIR(entry.s.st_mode)) {
      continue;
    }
    const time_t ts_now = time(nullptr);
    const time_t delete_threshold = 60 /*seconds*/ * 60 /*minutes*/ * 24 /*hours*/ * 30 /*days*/;
    if (entry.s.st_mtime + delete_threshold < ts_now) {
      BLI_delete(entry.path, false, false);
    }
  }
  BLI_filelist_free(entries, dir_len);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Compilation
 * \{ */

static StringRef to_stage_name(shaderc_shader_kind stage)
{
  switch (stage) {
    case shaderc_vertex_shader:
      return "vertex";
    case shaderc_geometry_shader:
      return "geometry";
    case shaderc_fragment_shader:
      return "fragment";
    case shaderc_compute_shader:
      return "compute";

    default:
      BLI_assert_msg(false, "Do not know how to convert shaderc_shader_kind to stage name.");
      break;
  }
  return "unknown stage";
}

/**
 * Injects an "OpString" holding the shader name into the SPIR-V binary, and references it via an
 * "OpSource" instruction. This is not necessary when G_DEBUG_GPU_SHADER_DEBUG_INFO is used, as
 * then shaderc/glslang will generate this OpString together with an OpSource instruction that
 * contains the full GLSL source code.
 *
 * This way, the human-readable shader name can be found/referenced when inspecting the binary or
 * debugging even when not using the flag G_DEBUG_GPU_SHADER_DEBUG_INFO.
 */
static void spirv_inject_op_string(const Span<uint32_t> &spirv_in,
                                   Vector<uint32_t> &spirv_out,
                                   StringRef name,
                                   uint32_t glsl_version)
{
  struct SpirvHeader {
    uint32_t magic_number;
    uint32_t version;
    uint32_t generator;
    uint32_t bound; /* Indicates the next available result ID. */
    uint32_t schema;
  };
  static_assert(sizeof(SpirvHeader) == SPIRV_HEADER_WORD_COUNT * sizeof(uint32_t));
  const SpirvHeader *header_in = reinterpret_cast<const SpirvHeader *>(spirv_in.data());
  if (spirv_in.size() < SPIRV_HEADER_WORD_COUNT || header_in->magic_number != SpvMagicNumber) {
    spirv_out.extend(spirv_in); /* pass through unmodified */
    BLI_assert_msg(false, "Invalid SPIR-V header");
    return;
  }

  /* Insertion point = start of the debug section: After all OpCapability / OpExtension /
   * OpExtInstImport / OpMemoryModel / OpEntryPoint / OpExecutionMode instructions. */
  int64_t pos_inject = SPIRV_HEADER_WORD_COUNT;
  while (pos_inject < spirv_in.size()) {
    const uint32_t opcode = spirv_in[pos_inject] & SpvOpCodeMask;
    const uint32_t length = spirv_in[pos_inject] >> SpvWordCountShift;
    if (length == 0) {
      /* Invalid SPIR-V binary. */
      spirv_out.extend(spirv_in); /* pass through unmodified */
      BLI_assert_msg(false, "Invalid SPIR-V code word length");
      return;
    }
    const bool pre_debug = ELEM(opcode,
                                SpvOpCapability,
                                SpvOpExtension,
                                SpvOpExtInstImport,
                                SpvOpMemoryModel,
                                SpvOpEntryPoint,
                                SpvOpExecutionMode,
                                SpvOpExecutionModeId);
    if (!pre_debug) {
      break;
    }
    pos_inject += length;
  }

  /* Literal string: UTF-8, null-terminated, packed 4 bytes/word, zero-padded. */
  const size_t byte_len = name.size() + 1; /* +1 for null terminator. */
  const uint32_t num_string_words = divide_ceil_u(byte_len, 4);

  /* Compute the number of words to inject; OpString (2 + string) + OpSource (4). */
  const uint32_t num_words_inject = 2u + num_string_words + 4u;

  /* Compute the size of the SPIR-V output binary and add the part until the injection point. */
  spirv_out.reserve(spirv_in.size() + num_words_inject);
  spirv_out.extend(spirv_in.take_front(pos_inject));

  /* Inject OpString holding the shader name. */
  const uint32_t op_string_word_count = 2u + num_string_words;
  spirv_out.append((op_string_word_count << SpvWordCountShift) | SpvOpString);
  spirv_out.append(header_in->bound);
  const int64_t string_offset = spirv_out.size();
  spirv_out.resize(spirv_out.size() + num_string_words, 0u);
  memcpy(spirv_out.data() + string_offset, name.data(), name.size());

  /* Inject OpSource referencing the OpString above as the source file so tools can associate the
   * shader name with this module. */
  constexpr uint32_t op_source_word_count = 4u;
  spirv_out.append((op_source_word_count << SpvWordCountShift) | SpvOpSource);
  spirv_out.append(SpvSourceLanguageGLSL);
  spirv_out.append(glsl_version);
  spirv_out.append(header_in->bound);

  /* Add the part from the source binary after the injection position to the output. */
  spirv_out.extend(spirv_in.drop_front(pos_inject));

  /* Raise the result ID bound in the output binary by one due to adding OpString. */
  SpirvHeader *header_out = reinterpret_cast<SpirvHeader *>(spirv_out.data());
  header_out->bound = header_in->bound + 1u;
}

#ifdef WITH_GPU_BACKEND_TESTS
/* Validate the SPIR-V binary using SPIRV-Tools. Only compiled into test builds since validation
 * is expensive. Used to catch breakage of spirv_inject_op_string with future SPIR-V versions.
 * \todo Enable the code below once SPIRV-Tools static library is included in the dependencies. */
#  if 0
static bool spirv_validate(const Span<uint32_t> &spirv, StringRef name)
{
  spv_context context = spvContextCreate(SPV_ENV_VULKAN_1_2);
  spv_diagnostic diagnostic = nullptr;
  const spv_result_t result = spvValidateBinary(context, spirv.data(), spirv.size(), &diagnostic);
  const bool valid = result == SPV_SUCCESS;
  if (!valid) {
    CLOG_ERROR(&LOG,
               "SPIR-V validation failed for %s: %s",
               std::string(name).c_str(),
               (diagnostic && diagnostic->error) ? diagnostic->error : "unknown error");
  }
  spvDiagnosticDestroy(diagnostic);
  spvContextDestroy(context);
  return valid;
}
#  endif
#endif

static std::string patch_line_directives(std::string source)
{
  /* Patch line directives so that we can make error reporting consistent. */
  size_t start_pos = 0;
  while ((start_pos = source.find("#line ", start_pos)) != std::string::npos) {
    source[start_pos] = '/';
    source[start_pos + 1] = '/';
  }
  return source;
}

static bool compile_ex(shaderc::Compiler &compiler,
                       VKShader &shader,
                       shaderc_shader_kind stage,
                       VKShaderModule &shader_module)
{
  std::string full_name = shader.name_get() + "_" + to_stage_name(stage);

  shader_module.original_sources = std::move(shader_module.combined_sources);

  Shader::dump_source_to_disk(
      shader.name_get(), full_name, ".glsl", shader_module.original_sources);

  if (!shader.skip_preprocessor) {
    shader_module.combined_sources = Shader::run_preprocessor(shader_module.original_sources,
                                                              G.debug & G_DEBUG_GPU_SHADER_NO_DCE);

    Shader::dump_source_to_disk(
        shader.name_get(), full_name + ".expanded", ".glsl", shader_module.combined_sources);
  }
  else {
    shader_module.combined_sources = shader_module.original_sources;
  }

  bool do_optimize = true;
  if (G.debug & G_DEBUG_GPU_RENDERDOC) {
    do_optimize = false;
  }
  /* WORKAROUND: Qualcomm driver can crash when handling optimized SPIR-V. */
  if (GPU_type_matches(GPU_DEVICE_QUALCOMM, GPU_OS_ANY, GPU_DRIVER_ANY)) {
    do_optimize = false;
  }
  /* The SPIR-V optimizer crashes when optimizing probe lighting with the shared shadow tracer.
   * The planar variant faults in shaderc; the sphere variant takes minutes to optimize. Keep
   * their generated shader and shadow sampling intact, and let the driver optimize the SPIR-V.
   * Other material and lighting shaders still use the normal offline optimization pipeline. */
  if (stage == shaderc_fragment_shader &&
      ELEM(shader.name_get(), "eevee_deferred_sphere_eval", "eevee_deferred_planar_eval"))
  {
    do_optimize = false;
  }

  /* The cached SPIR-V binary contains an injected OpString, which depends on the shader name.
   * Include the name in the cache key so an entry is only reused for an identical result. This
   * also invalidates older caches that stored the SPIR-V without the injected OpString.
   * Keep unoptimized results separate so an old optimized probe binary cannot bypass the
   * workaround above. */
  const std::string hash_extra = std::string("op_string:") + full_name +
                                 (do_optimize ? "" : ":unoptimized-v1");

  if (read_spirv_from_disk(shader_module, hash_extra)) {
    return true;
  }

  shaderc::CompileOptions options;
  if (VKBackend::get().device.extensions_get().spirv_1_4) {
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    options.SetTargetSpirv(shaderc_spirv_version_1_4);
  }
  else {
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
  }
  options.SetOptimizationLevel(do_optimize ? shaderc_optimization_level_performance :
                                             shaderc_optimization_level_zero);

  /* Increase the max id bound.
   *
   * SPIR-V has a default max id bound set to 0x3fffff which is the minimum amount of ids that
   * needs to be supported by any platform. However during optimization the max id bound can
   * increase very fast and lowered at the end. As glslang uses max id bound in their internal
   * structures to allocate arrays out of bound errors can occur.
   *
   * Increasing the max id bound to a larger number to increase the internal arrays of the
   * compiler to work around the compiler crash.
   *
   * NOTE: Test-files in #144614 and #143516 would surpass the default limit during compilation.
   * The final optimized SPIR-V is far less than the default so be fine to be used on platforms
   * with minimum spec.
   *
   * https://registry.khronos.org/SPIR-V/specs/1.0/SPIRV.html#_a_id_limits_a_universal_limits
   */
  options.SetMaxIdBound(0xffffff);

  /* Should always be called after setting the optimization level. Setting optimization level
   * resets all previous passes. */
  if (G.debug & G_DEBUG_GPU_SHADER_DEBUG_INFO) {
    options.SetGenerateDebugInfo();
  }

  /* Removes line directive. */
  shader_module.combined_sources = patch_line_directives(
      std::move(shader_module.combined_sources));
  const std::string &sources = shader_module.combined_sources;

  /* The task scheduler enables flush-to-zero. Some platforms convert floating point literals using
   * floating point instructions, which would then turn denormal literals into zero. That happens
   * for example with the `1.17549435e-38` guard inside OCIO generated for the Filmic view
   * transform. */
  {
    ScopedNoFlushToZero scoped_no_flush_to_zero;
    shader_module.compilation_result = compiler.CompileGlslToSpv(
        sources, stage, full_name.c_str(), options);
  }
  bool compilation_succeeded = shader_module.compilation_result.GetCompilationStatus() ==
                               shaderc_compilation_status_success;
  if (compilation_succeeded) {
    /* Copy the compiled SPIR-V code into spirv_binary so it can be post-processed.
     * The function VKShaderModule::finalize prefers spirv_binary when it is not empty. */
    const uint32_t *begin = shader_module.compilation_result.begin();
    const uint32_t *end = shader_module.compilation_result.end();
    Span<uint32_t> compilation_result_span(begin, end - begin);
    if ((G.debug & G_DEBUG_GPU_SHADER_DEBUG_INFO) == 0) {
      const VKDevice &device = VKBackend::get().device;
      const bool stage_use_ray_query = stage != shaderc_geometry_shader &&
                                       shader.use_ray_query_get();
      const uint32_t glsl_version = device.glsl_patch_version_get(stage_use_ray_query);
      spirv_inject_op_string(
          compilation_result_span, shader_module.spirv_binary, full_name, glsl_version);
    }
    else {
      shader_module.spirv_binary = Vector<uint32_t>(compilation_result_span);
    }

#ifdef WITH_GPU_BACKEND_TESTS
    /* TODO(@chrismile): enable the code below once SPIRV-Tools library is available. */
#  if 0
    if (!spirv_validate(shader_module.spirv_binary, full_name)) {
      return false;
    }
#  endif
#endif

    write_spirv_to_disk(shader_module);
  }
  return compilation_succeeded;
}

bool VKShaderCompiler::compile_module(VKShader &shader,
                                      shaderc_shader_kind stage,
                                      VKShaderModule &shader_module)
{
  shaderc::Compiler compiler;
  return compile_ex(compiler, shader, stage, shader_module);
}

/** \} */

}  // namespace blender::gpu
