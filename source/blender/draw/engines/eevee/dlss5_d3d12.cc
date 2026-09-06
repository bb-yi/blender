/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "dlss5_d3d12.hh"

#include "CLG_log.h"

#include "GPU_context.hh"
#include "GPU_platform.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"

#ifdef _WIN32

#  include <Windows.h>
#  include <d3d12.h>
#  include <dxgi1_6.h>
#  include <wrl/client.h>

#  include <array>
#  include <memory>
#  include <mutex>
#  include <algorithm>
#  include <filesystem>
#  include <fstream>
#  include <limits>
#  include <cstdio>
#  include <string>
#  include <cstring>
#  include <vector>

namespace blender::eevee {

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

static CLG_LogRef LOG = {"eevee"};

constexpr unsigned long long kApplicationId = 0x0876232Cull;
constexpr int kSdkVersion = 0x15;
constexpr int kFeatureId = 18;
constexpr int kEngineTypeUnity = 2;
constexpr int kNgxSuccess = 0x1;

struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

using NgxResult = int;
using GetModuleFileNameWFn = DWORD(WINAPI *)(HMODULE, LPWSTR, DWORD);
using NgxAppLogCallback = void(__cdecl *)(const char *, int, int);
struct NgxPathListInfo {
  const wchar_t *const *path;
  unsigned int length;
};
struct NgxLoggingInfo {
  NgxAppLogCallback callback;
  int minimum;
  bool disable_other_sinks;
};
struct NgxFeatureCommonInfo {
  NgxPathListInfo path_list;
  void *internal_data;
  NgxLoggingInfo logging;
};
using NgxInitProjectFn = NgxResult(__cdecl *)(const char *,
                                               int,
                                               const char *,
                                               const wchar_t *,
                                               ID3D12Device *,
                                               const NgxFeatureCommonInfo *,
                                               int);
using NgxInitProjectLegacyFn = NgxResult(__cdecl *)(const char *,
                                                     int,
                                                     const char *,
                                                     const wchar_t *,
                                                     ID3D12Device *,
                                                     int,
                                                     const NgxFeatureCommonInfo *);
using NgxShutdownCoreFn = NgxResult(__cdecl *)(ID3D12Device *);
using NgxAllocateParametersFn = NgxResult(__cdecl *)(NVSDK_NGX_Parameter **);
using NgxDestroyParametersFn = NgxResult(__cdecl *)(NVSDK_NGX_Parameter *);
using NgxInitExtFn = NgxResult(__cdecl *)(unsigned long long,
                                           const wchar_t *,
                                           ID3D12Device *,
                                           int,
                                           const NVSDK_NGX_Parameter *);
using NgxShutdownFn = NgxResult(__cdecl *)(ID3D12Device *);
using NgxCreateFeatureFn = NgxResult(__cdecl *)(ID3D12GraphicsCommandList *,
                                                 int,
                                                 const NVSDK_NGX_Parameter *,
                                                 NVSDK_NGX_Handle **);
using NgxEvaluateFeatureFn = NgxResult(__cdecl *)(ID3D12GraphicsCommandList *,
                                                   const NVSDK_NGX_Handle *,
                                                   const NVSDK_NGX_Parameter *,
                                                   void *);
using NgxReleaseFeatureFn = NgxResult(__cdecl *)(NVSDK_NGX_Handle *);
using NgxGetCapabilityParametersFn = NgxResult(__cdecl *)(NVSDK_NGX_Parameter **);

HMODULE g_hooked_snippet_module = nullptr;
HMODULE g_hooked_core_module = nullptr;
GetModuleFileNameWFn g_original_get_module_file_name = nullptr;

bool WriteIat(void **slot, void *value)
{
  DWORD old_protect = 0;
  if (slot == nullptr || !VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old_protect)) {
    return false;
  }
  InterlockedExchangePointer(slot, value);
  DWORD ignored = 0;
  VirtualProtect(slot, sizeof(void *), old_protect, &ignored);
  FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
  return true;
}

DWORD WINAPI HookedGetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size)
{
  if (g_original_get_module_file_name == nullptr) {
    return 0;
  }
  if (g_hooked_snippet_module != nullptr && module == g_hooked_snippet_module) {
    return g_original_get_module_file_name(module, filename, size);
  }
  if (g_hooked_core_module != nullptr) {
    return g_original_get_module_file_name(g_hooked_core_module, filename, size);
  }

  if (filename == nullptr || size == 0) {
    return 0;
  }
  constexpr wchar_t fake_name[] = L"nvngx.dll";
  constexpr DWORD fake_length = ARRAYSIZE(fake_name) - 1;
  if (fake_length + 1 > size) {
    SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return size;
  }
  memcpy(filename, fake_name, sizeof(fake_name));
  return fake_length;
}

bool HookSnippetGetModuleFileNameW(HMODULE snippet_module,
                                   HMODULE core_module,
                                   void ***r_iat_slot)
{
  if (snippet_module == nullptr || r_iat_slot == nullptr) {
    return false;
  }

  auto *base = reinterpret_cast<BYTE *>(snippet_module);
  auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    return false;
  }
  auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    return false;
  }
  const auto &directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (directory.VirtualAddress == 0) {
    return false;
  }

  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  const FARPROC wanted = kernel32 ? GetProcAddress(kernel32, "GetModuleFileNameW") : nullptr;
  auto *descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + directory.VirtualAddress);
  for (; descriptor->Name != 0; ++descriptor) {
    const char *name = reinterpret_cast<const char *>(base + descriptor->Name);
    if (_stricmp(name, "KERNEL32.dll") != 0 && _stricmp(name, "KERNELBASE.dll") != 0) {
      continue;
    }
    auto *slots = reinterpret_cast<IMAGE_THUNK_DATA *>(base + descriptor->FirstThunk);
    auto *imports = descriptor->OriginalFirstThunk
                        ? reinterpret_cast<IMAGE_THUNK_DATA *>(base + descriptor->OriginalFirstThunk)
                        : nullptr;
    for (size_t index = 0; slots[index].u1.Function != 0; ++index) {
      bool match = false;
      if (imports != nullptr && imports[index].u1.AddressOfData != 0 &&
          !IMAGE_SNAP_BY_ORDINAL(imports[index].u1.Ordinal))
      {
        auto *entry = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(
            base + imports[index].u1.AddressOfData);
        match = strcmp(reinterpret_cast<const char *>(entry->Name), "GetModuleFileNameW") == 0;
      }
      else if (wanted && slots[index].u1.Function == reinterpret_cast<ULONG_PTR>(wanted)) {
        match = true;
      }
      if (!match) {
        continue;
      }

      auto *slot = reinterpret_cast<void **>(&slots[index].u1.Function);
      g_original_get_module_file_name = reinterpret_cast<GetModuleFileNameWFn>(*slot);
      g_hooked_snippet_module = snippet_module;
      g_hooked_core_module = core_module;
      if (!WriteIat(slot, reinterpret_cast<void *>(&HookedGetModuleFileNameW))) {
        g_original_get_module_file_name = nullptr;
        g_hooked_snippet_module = nullptr;
        g_hooked_core_module = nullptr;
        return false;
      }
      *r_iat_slot = slot;
      return true;
    }
  }
  return false;
}

void UnhookSnippetGetModuleFileNameW(void **iat_slot)
{
  if (iat_slot != nullptr && g_original_get_module_file_name != nullptr) {
    WriteIat(iat_slot, reinterpret_cast<void *>(g_original_get_module_file_name));
  }
  g_original_get_module_file_name = nullptr;
  g_hooked_snippet_module = nullptr;
  g_hooked_core_module = nullptr;
}

void NgxLogCallback(const char *message, int /*level*/, int /*source_component*/)
{
  if (message != nullptr && *message != '\0') {
    std::fprintf(stderr, "[DLSS5 NGX] %s\n", message);
  }
}

template<typename T> T Resolve(HMODULE module, const char *name)
{
  return reinterpret_cast<T>(GetProcAddress(module, name));
}

bool NgxSucceeded(NgxResult result)
{
  return result == kNgxSuccess;
}

void SetUll(NVSDK_NGX_Parameter *parameters, const char *name, unsigned long long value)
{
  using Fn = void(__cdecl *)(NVSDK_NGX_Parameter *, const char *, unsigned long long);
  reinterpret_cast<Fn>((*reinterpret_cast<void ***>(parameters))[0])(parameters, name, value);
}

void SetResource(NVSDK_NGX_Parameter *parameters, const char *name, ID3D12Resource *resource)
{
  using Fn = void(__cdecl *)(NVSDK_NGX_Parameter *, const char *, ID3D12Resource *);
  reinterpret_cast<Fn>((*reinterpret_cast<void ***>(parameters))[1])(parameters, name, resource);
}

void SetUi(NVSDK_NGX_Parameter *parameters, const char *name, unsigned int value)
{
  using Fn = void(__cdecl *)(NVSDK_NGX_Parameter *, const char *, unsigned int);
  reinterpret_cast<Fn>((*reinterpret_cast<void ***>(parameters))[3])(parameters, name, value);
}

void SetI(NVSDK_NGX_Parameter *parameters, const char *name, int value)
{
  using Fn = void(__cdecl *)(NVSDK_NGX_Parameter *, const char *, int);
  reinterpret_cast<Fn>((*reinterpret_cast<void ***>(parameters))[4])(parameters, name, value);
}

void SetF(NVSDK_NGX_Parameter *parameters, const char *name, float value)
{
  using Fn = void(__cdecl *)(NVSDK_NGX_Parameter *, const char *, float);
  reinterpret_cast<Fn>((*reinterpret_cast<void ***>(parameters))[6])(parameters, name, value);
}

NgxResult GetUi(NVSDK_NGX_Parameter *parameters, const char *name, unsigned int *value)
{
  using Fn = NgxResult(__cdecl *)(NVSDK_NGX_Parameter *, const char *, unsigned int *);
  return reinterpret_cast<Fn>((*reinterpret_cast<void ***>(parameters))[11])(
      parameters, name, value);
}

NgxResult GetI(NVSDK_NGX_Parameter *parameters, const char *name, int *value)
{
  using Fn = NgxResult(__cdecl *)(NVSDK_NGX_Parameter *, const char *, int *);
  return reinterpret_cast<Fn>((*reinterpret_cast<void ***>(parameters))[12])(parameters, name, value);
}

NgxResult ComputeScalingRatio(NVSDK_NGX_Parameter *parameters)
{
  unsigned int upscaling = 0;
  int input_width = 0;
  int output_width = 0;
  GetUi(parameters, "DLSSNR.Upscaling", &upscaling);
  GetI(parameters, "DLSSNR.InputWidth", &input_width);
  GetI(parameters, "DLSSNR.OutputWidth", &output_width);
  const float ratio = upscaling && input_width > 0 && output_width > 0 ?
                          float(input_width) / float(output_width) :
                          1.0f;
  SetF(parameters, "DLSSNR.ScalingRatio", ratio);
  return kNgxSuccess;
}

void SetRect(NVSDK_NGX_Parameter *parameters, const char *prefix, int width, int height)
{
  const std::string name(prefix);
  SetI(parameters, (name + "SubrectBaseX").c_str(), 0);
  SetI(parameters, (name + "SubrectBaseY").c_str(), 0);
  SetI(parameters, (name + "SubrectWidth").c_str(), width);
  SetI(parameters, (name + "SubrectHeight").c_str(), height);
}

fs::path RuntimeDirectory()
{
  const wchar_t *configured = _wgetenv(L"DLSS5_RUNTIME_DIR");
  if (configured && *configured && fs::is_directory(configured)) {
    return fs::absolute(configured);
  }

  wchar_t module_path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, module_path, ARRAYSIZE(module_path));
  if (length == 0 || length >= ARRAYSIZE(module_path)) {
    return {};
  }

  const fs::path executable = fs::path(module_path).parent_path();
  const std::vector<fs::path> candidates = {
      executable / "temp" / "dlss5_runtime",
      executable.parent_path() / "temp" / "dlss5_runtime",
      executable.parent_path().parent_path() / "temp" / "dlss5_runtime",
  };
  for (const fs::path &candidate : candidates) {
    if (fs::is_regular_file(candidate / "nvngx_dlssnr.dll")) {
      return fs::absolute(candidate);
    }
  }
  return {};
}

fs::path NgxCorePath(const fs::path &runtime_directory)
{
  const wchar_t *configured = _wgetenv(L"DLSS5_CORE_DLL");
  if (configured && *configured) {
    return fs::path(configured);
  }

  wchar_t windows_directory[MAX_PATH] = {};
  const UINT length = GetWindowsDirectoryW(windows_directory, ARRAYSIZE(windows_directory));
  if (length != 0 && length < ARRAYSIZE(windows_directory)) {
    const fs::path driver_store =
        fs::path(windows_directory) / "System32" / "DriverStore" / "FileRepository";
    std::error_code error;
    fs::path selected;
    fs::file_time_type selected_time;
    const fs::directory_options options = fs::directory_options::skip_permission_denied;
    for (const fs::directory_entry &entry : fs::directory_iterator(driver_store, options, error)) {
      error.clear();
      std::error_code entry_error;
      if (!entry.is_directory(entry_error) || entry_error) {
        continue;
      }
      const fs::path candidate = entry.path() / "_nvngx.dll";
      std::error_code candidate_error;
      if (!fs::is_regular_file(candidate, candidate_error) || candidate_error) {
        continue;
      }
      std::error_code time_error;
      const fs::file_time_type candidate_time = fs::last_write_time(candidate, time_error);
      if (!time_error && (selected.empty() || candidate_time > selected_time)) {
        selected = candidate;
        selected_time = candidate_time;
      }
    }
    if (!selected.empty()) {
      return fs::absolute(selected);
    }

    const fs::path system_candidate = fs::path(windows_directory) / "System32" / "_nvngx.dll";
    if (fs::is_regular_file(system_candidate, error)) {
      return fs::absolute(system_candidate);
    }
  }

  /* Keep the packaged shim as a development fallback when no NVIDIA driver Core is installed. */
  return runtime_directory / "nvngx.dll";
}

std::string HResultString(HRESULT result)
{
  char buffer[16];
  sprintf_s(buffer, "%08lx", static_cast<unsigned long>(result));
  return buffer;
}

}  // namespace

/* NGX DLL hooks and initialization are process-wide. A viewport and F12 must
 * not hook the same IAT twice or shut down each other's device. Keep the runtime
 * loaded for the process lifetime; only feature/history resources are per view. */
struct Dlss5NgxRuntime {
  ComPtr<ID3D12Device> device;
  HMODULE core_module = nullptr;
  HMODULE snippet_module = nullptr;
  void **snippet_iat_slot = nullptr;
  NgxInitProjectFn ngx_init_project = nullptr;
  NgxInitProjectLegacyFn ngx_init_project_legacy = nullptr;
  NgxShutdownCoreFn ngx_shutdown_core = nullptr;
  NgxAllocateParametersFn ngx_allocate_parameters = nullptr;
  NgxDestroyParametersFn ngx_destroy_parameters = nullptr;
  NgxInitExtFn ngx_init_ext = nullptr;
  NgxShutdownFn ngx_shutdown = nullptr;
  NgxCreateFeatureFn ngx_create_feature = nullptr;
  NgxEvaluateFeatureFn ngx_evaluate_feature = nullptr;
  NgxReleaseFeatureFn ngx_release_feature = nullptr;
  NgxGetCapabilityParametersFn ngx_get_capability_parameters = nullptr;
  fs::path runtime_directory;
  bool core_initialized = false;
  bool snippet_initialized = false;
  std::string status;
  /* Serialize the NGX host API, not GPU execution on the separate queues. */
  std::mutex mutex;

  void set_error(const std::string &message) { status = message; }

  ~Dlss5NgxRuntime()
  {
    if (snippet_initialized) { ngx_shutdown(device.Get()); }
    if (core_initialized) { ngx_shutdown_core(device.Get()); }
    UnhookSnippetGetModuleFileNameW(snippet_iat_slot);
    if (snippet_module) { FreeLibrary(snippet_module); }
    if (core_module) { FreeLibrary(core_module); }
  }
  bool initialize_device()
  {
    ComPtr<IDXGIFactory6> factory;
    HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
      set_error("CreateDXGIFactory1 failed: 0x" + HResultString(result));
      return false;
    }

    const Span<uint8_t> luid = GPU_platform_luid();
    if (luid.size() != sizeof(LUID)) {
      set_error("Vulkan device does not expose a Windows LUID");
      return false;
    }
    ComPtr<IDXGIAdapter1> selected_adapter;
    for (UINT index = 0;; ++index) {
      ComPtr<IDXGIAdapter1> adapter;
      result = factory->EnumAdapterByGpuPreference(
          index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
      if (result == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      if (FAILED(result)) {
        continue;
      }
      DXGI_ADAPTER_DESC1 description = {};
      adapter->GetDesc1(&description);
      if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || description.VendorId != 0x10DE) {
        continue;
      }
      if (memcmp(luid.data(), &description.AdapterLuid, sizeof(LUID)) != 0) {
        continue;
      }
      if (SUCCEEDED(D3D12CreateDevice(
              adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
      {
        selected_adapter = adapter;
        break;
      }
    }
    if (!selected_adapter || !device) {
      set_error("No NVIDIA D3D12 adapter matches the active Vulkan device");
      return false;
    }

    return true;
  }
  bool initialize_ngx()
  {
    runtime_directory = RuntimeDirectory();
    if (runtime_directory.empty()) {
      set_error("DLSS5 runtime directory not found");
      return false;
    }

    const fs::path core_path = NgxCorePath(runtime_directory);
    if (!fs::is_regular_file(core_path)) {
      set_error("DLSS5 NGX Core not found: " + core_path.string());
      return false;
    }
    core_module = LoadLibraryExW(core_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (core_module == nullptr) {
      set_error("LoadLibraryExW NGX Core failed: " + core_path.string() + " Win32=" +
                std::to_string(GetLastError()));
      return false;
    }
    CLOG_INFO(&LOG, "DLSS5 NGX Core loaded from %ls", core_path.c_str());
    const fs::path snippet_path = runtime_directory / "nvngx_dlssnr.dll";
    snippet_module = LoadLibraryExW(snippet_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (snippet_module == nullptr) {
      set_error("LoadLibraryExW nvngx_dlssnr.dll failed: " + std::to_string(GetLastError()));
      return false;
    }
    if (!HookSnippetGetModuleFileNameW(snippet_module, core_module, &snippet_iat_slot)) {
      set_error("DLSS5 GetModuleFileNameW compatibility hook failed");
      return false;
    }

    ngx_init_project = Resolve<NgxInitProjectFn>(core_module,
                                                 "NVSDK_NGX_D3D12_Init_with_ProjectID");
    if (ngx_init_project == nullptr) {
      ngx_init_project_legacy = Resolve<NgxInitProjectLegacyFn>(
          core_module, "NVSDK_NGX_D3D12_Init_ProjectID");
    }
    ngx_shutdown_core = Resolve<NgxShutdownCoreFn>(core_module, "NVSDK_NGX_D3D12_Shutdown1");
    ngx_allocate_parameters = Resolve<NgxAllocateParametersFn>(
        core_module, "NVSDK_NGX_D3D12_AllocateParameters");
    ngx_destroy_parameters = Resolve<NgxDestroyParametersFn>(
        core_module, "NVSDK_NGX_D3D12_DestroyParameters");
    ngx_init_ext = Resolve<NgxInitExtFn>(snippet_module, "NVSDK_NGX_D3D12_Init_Ext");
    ngx_shutdown = Resolve<NgxShutdownFn>(snippet_module, "NVSDK_NGX_D3D12_Shutdown1");
    ngx_create_feature = Resolve<NgxCreateFeatureFn>(
        snippet_module, "NVSDK_NGX_D3D12_CreateFeature");
    ngx_evaluate_feature = Resolve<NgxEvaluateFeatureFn>(
        snippet_module, "NVSDK_NGX_D3D12_EvaluateFeature");
    ngx_release_feature = Resolve<NgxReleaseFeatureFn>(
        snippet_module, "NVSDK_NGX_D3D12_ReleaseFeature");
    ngx_get_capability_parameters = Resolve<NgxGetCapabilityParametersFn>(
        core_module, "NVSDK_NGX_D3D12_GetCapabilityParameters");
    if ((ngx_init_project == nullptr && ngx_init_project_legacy == nullptr) ||
        ngx_shutdown_core == nullptr ||
        ngx_allocate_parameters == nullptr || ngx_destroy_parameters == nullptr ||
        ngx_init_ext == nullptr || ngx_shutdown == nullptr || ngx_create_feature == nullptr ||
        ngx_evaluate_feature == nullptr || ngx_release_feature == nullptr)
    {
      set_error("DLSS5 NGX exports are incomplete");
      return false;
    }

    const fs::path data_directory = runtime_directory / "ngx-data";
    fs::create_directories(data_directory);
    const wchar_t *feature_paths[] = {runtime_directory.c_str()};
    NgxFeatureCommonInfo common_info = {};
    common_info.path_list.path = feature_paths;
    common_info.path_list.length = 1;
    common_info.logging.callback = &NgxLogCallback;
    common_info.logging.minimum = 2;
    common_info.logging.disable_other_sinks = false;
    uint16_t legacy_internal_data = 0;
    if (ngx_init_project == nullptr) {
      common_info.internal_data = &legacy_internal_data;
    }
    NgxResult result = ngx_init_project != nullptr ?
                           ngx_init_project("e40890d0-da76-467b-8130-12f3ada8d7c8",
                                            kEngineTypeUnity,
                                            "6000.3",
                                            data_directory.c_str(),
                                            device.Get(),
                                            &common_info,
                                            kSdkVersion) :
                           ngx_init_project_legacy("e40890d0-da76-467b-8130-12f3ada8d7c8",
                                                   kEngineTypeUnity,
                                                   "6000.3",
                                                   data_directory.c_str(),
                                                   device.Get(),
                                                   kSdkVersion,
                                                   &common_info);
    if (!NgxSucceeded(result)) {
      set_error("NVSDK_NGX_D3D12_Init_with_ProjectID failed: 0x" +
                HResultString(static_cast<HRESULT>(result)));
      return false;
    }
    core_initialized = true;

    result = ngx_init_ext(kApplicationId, data_directory.c_str(), device.Get(), kSdkVersion, nullptr);
    if (!NgxSucceeded(result)) {
      set_error("NVSDK_NGX_D3D12_Init_Ext failed: 0x" +
                HResultString(static_cast<HRESULT>(result)));
      return false;
    }
    snippet_initialized = true;

    return true;
  }
};

static std::shared_ptr<Dlss5NgxRuntime> acquire_ngx_runtime(std::string &error)
{
  static std::mutex mutex;
  static std::shared_ptr<Dlss5NgxRuntime> cached;
  std::lock_guard lock(mutex);
  if (cached) { return cached; }
  auto runtime = std::make_shared<Dlss5NgxRuntime>();
  if (!runtime->initialize_device() || !runtime->initialize_ngx()) {
    error = runtime->status;
    return nullptr;
  }
  cached = runtime;
  return runtime;
}

struct Dlss5D3D12Session::Impl {
  struct SharedTexture {
    ComPtr<ID3D12Resource> d3d12;
    HANDLE shared_handle = nullptr;
    gpu::Texture *vulkan = nullptr;
    int2 extent = int2(0);
    gpu::TextureFormat format = gpu::TextureFormat::Invalid;

    void reset()
    {
      if (vulkan != nullptr) {
        GPU_texture_free(vulkan);
        vulkan = nullptr;
      }
      if (shared_handle != nullptr) {
        CloseHandle(shared_handle);
        shared_handle = nullptr;
      }
      d3d12.Reset();
      extent = int2(0);
      format = gpu::TextureFormat::Invalid;
    }
  };

  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  static constexpr int kCommandListCount = 3;
  std::array<ComPtr<ID3D12CommandAllocator>, kCommandListCount> allocators;
  std::array<ComPtr<ID3D12GraphicsCommandList>, kCommandListCount> command_lists;
  std::array<uint64_t, kCommandListCount> command_list_completion_values = {};
  int command_list_index = -1;
  ComPtr<ID3D12QueryHeap> timestamp_heap;
  ComPtr<ID3D12Resource> timestamp_readback;
  uint64_t timestamp_frequency = 0;
  std::array<bool, kCommandListCount> timestamp_valid = {};
  std::array<uint64_t, kCommandListCount> timestamp_serial = {};
  double last_gpu_time_ms = -1.0;
  uint64_t evaluate_count = 0;
  uint64_t feature_create_count = 0;
  ComPtr<ID3D12Fence> completion_fence;
  ComPtr<ID3D12Fence> vulkan_to_d3d12_fence;
  ComPtr<ID3D12Fence> d3d12_to_vulkan_fence;
  GPUVulkanExternalSemaphore *vulkan_to_d3d12_semaphore = nullptr;
  GPUVulkanExternalSemaphore *d3d12_to_vulkan_semaphore = nullptr;
  HANDLE completion_event = nullptr;
  uint64_t completion_value = 0;
  uint64_t vulkan_to_d3d12_value = 0;
  uint64_t d3d12_to_vulkan_value = 0;
  bool external_sync = false;
  bool textures_released = false;

  std::shared_ptr<Dlss5NgxRuntime> runtime;
  NVSDK_NGX_Parameter *parameters = nullptr;
  NVSDK_NGX_Handle *feature = nullptr;
  bool initialized = false;
  bool parameters_reported = false;
  bool feature_settings_valid = false;
  bool feature_color_is_scene_linear = true;
  bool feature_depth_is_reverse_z = true;
  std::string status = "not initialized";

  SharedTexture color;
  SharedTexture depth;
  SharedTexture velocity;
  SharedTexture output;

  ~Impl()
  {
    reset();
  }

  void set_error(const std::string &message)
  {
    status = message;
  }

  void reset()
  {
    if (!wait_for_fence_value(completion_value, INFINITE)) {
      return;
    }
    /* Only stall the GPU if we actually have imported shared textures to free.
     * Calling GPU_finish() on first enable waits for the whole EEVEE frame and
     * freezes heavy scenes when the user just ticks DLSSNR. */
    const bool has_shared_textures = color.vulkan != nullptr || depth.vulkan != nullptr ||
                                     velocity.vulkan != nullptr || output.vulkan != nullptr;
    if (has_shared_textures && GPU_context_active_get() != nullptr) {
      GPU_finish();
    }

    GPU_vulkan_external_semaphore_free(d3d12_to_vulkan_semaphore);
    d3d12_to_vulkan_semaphore = nullptr;
    GPU_vulkan_external_semaphore_free(vulkan_to_d3d12_semaphore);
    vulkan_to_d3d12_semaphore = nullptr;
    external_sync = false;
    textures_released = false;

    if (feature != nullptr && runtime != nullptr) {
      std::lock_guard lock(runtime->mutex);
      runtime->ngx_release_feature(feature);
      feature = nullptr;
    }
    if (parameters != nullptr && runtime != nullptr) {
      std::lock_guard lock(runtime->mutex);
      runtime->ngx_destroy_parameters(parameters);
      parameters = nullptr;
    }
    color.reset();
    depth.reset();
    velocity.reset();
    output.reset();
    feature_settings_valid = false;
    feature_color_is_scene_linear = true;
    feature_depth_is_reverse_z = true;

    if (completion_event != nullptr) {
      CloseHandle(completion_event);
      completion_event = nullptr;
    }
    for (auto &command_list : command_lists) {
      command_list.Reset();
    }
    for (auto &allocator : allocators) {
      allocator.Reset();
    }
    command_list_completion_values = {};
    command_list_index = -1;
    timestamp_heap.Reset();
    timestamp_readback.Reset();
    timestamp_frequency = 0;
    timestamp_valid = {};
    last_gpu_time_ms = -1.0;
    queue.Reset();
    completion_fence.Reset();
    vulkan_to_d3d12_fence.Reset();
    d3d12_to_vulkan_fence.Reset();
    completion_value = 0;
    vulkan_to_d3d12_value = 0;
    d3d12_to_vulkan_value = 0;
    device.Reset();
    runtime.reset();
    initialized = false;
    parameters_reported = false;
  }

  bool wait_for_fence_value(const uint64_t value, const DWORD timeout_ms)
  {
    if (completion_fence == nullptr || value == 0) { return true; }
    const ULONGLONG start = GetTickCount64();
    for (;;) {
      const uint64_t completed = completion_fence->GetCompletedValue();
      if (completed == UINT64_MAX) {
        set_error("DLSS5 D3D12 device removed");
        /* The device is no longer executing commands, so teardown is safe. */
        return FAILED(device->GetDeviceRemovedReason());
      }
      if (completed >= value) { return true; }
      if (timeout_ms == 0 || completion_event == nullptr) { return false; }
      ResetEvent(completion_event);
      if (FAILED(completion_fence->SetEventOnCompletion(value, completion_event))) {
        set_error("DLSS5 SetEventOnCompletion failed");
        return false;
      }
      const ULONGLONG elapsed = GetTickCount64() - start;
      if (timeout_ms != INFINITE && elapsed >= timeout_ms) { return false; }
      const DWORD remaining = timeout_ms == INFINITE ? INFINITE : timeout_ms - DWORD(elapsed);
      if (WaitForSingleObject(completion_event, remaining) != WAIT_OBJECT_0) { return false; }
    }
  }

  bool initialize_external_sync()
  {
    auto import_fence = [&](ID3D12Fence *fence,
                            GPUVulkanExternalSemaphore **r_semaphore) {
      HANDLE handle = nullptr;
      HRESULT result = device->CreateSharedHandle(
          fence, nullptr, GENERIC_ALL, nullptr, &handle);
      if (FAILED(result)) {
        set_error("CreateSharedHandle fence failed: 0x" + HResultString(result));
        return false;
      }
      *r_semaphore = GPU_vulkan_external_semaphore_create_from_d3d12_fence(
          reinterpret_cast<uint64_t>(handle));
      if (*r_semaphore == nullptr) {
        CloseHandle(handle);
        set_error("Vulkan import of D3D12 fence failed");
        return false;
      }
      CloseHandle(handle); /* Vulkan imports the payload, not ownership of this NT handle. */
      return true;
    };

    if (!import_fence(vulkan_to_d3d12_fence.Get(), &vulkan_to_d3d12_semaphore) ||
        !import_fence(d3d12_to_vulkan_fence.Get(), &d3d12_to_vulkan_semaphore))
    {
      GPU_vulkan_external_semaphore_free(d3d12_to_vulkan_semaphore);
      d3d12_to_vulkan_semaphore = nullptr;
      GPU_vulkan_external_semaphore_free(vulkan_to_d3d12_semaphore);
      vulkan_to_d3d12_semaphore = nullptr;
      return false;
    }
    external_sync = true;
    return true;
  }

  bool initialize_device()
  {
    runtime = acquire_ngx_runtime(status);
    if (!runtime) { return false; }
    device = runtime->device;
    HRESULT result;
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    result = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (FAILED(result)) {
      set_error("CreateCommandQueue failed: 0x" + HResultString(result));
      return false;
    }
    for (int index = 0; index < kCommandListCount; index++) {
      result = device->CreateCommandAllocator(
          D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[index]));
      if (FAILED(result)) {
        set_error("CreateCommandAllocator failed: 0x" + HResultString(result));
        return false;
      }
      result = device->CreateCommandList(0,
                                         D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         allocators[index].Get(),
                                         nullptr,
                                         IID_PPV_ARGS(&command_lists[index]));
      if (FAILED(result)) {
        set_error("CreateCommandList failed: 0x" + HResultString(result));
        return false;
      }
      command_lists[index]->Close();
    }
    result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completion_fence));
    if (FAILED(result)) {
      set_error("CreateFence failed: 0x" + HResultString(result));
      return false;
    }
    result = device->CreateFence(
        0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&vulkan_to_d3d12_fence));
    if (FAILED(result)) {
      set_error("Create Vulkan-to-D3D12 fence failed: 0x" + HResultString(result));
      return false;
    }
    result = device->CreateFence(
        0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&d3d12_to_vulkan_fence));
    if (FAILED(result)) {
      set_error("Create D3D12-to-Vulkan fence failed: 0x" + HResultString(result));
      return false;
    }
    initialize_timestamps();
    completion_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (completion_event == nullptr) {
      set_error("CreateEventW failed");
      return false;
    }
    return true;
  }

  void initialize_timestamps()
  {
    if (FAILED(queue->GetTimestampFrequency(&timestamp_frequency))) { return; }
    D3D12_QUERY_HEAP_DESC desc = {};
    desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    desc.Count = kCommandListCount * 2;
    if (FAILED(device->CreateQueryHeap(&desc, IID_PPV_ARGS(&timestamp_heap)))) { return; }
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = desc.Count * sizeof(uint64_t);
    buffer.Height = 1;
    buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&timestamp_readback))))
    {
      timestamp_heap.Reset();
    }
  }

  void read_timestamp(const int slot)
  {
    if (!timestamp_valid[slot] || timestamp_readback == nullptr || timestamp_frequency == 0) { return; }
    if (completion_fence->GetCompletedValue() < command_list_completion_values[slot]) { return; }
    const SIZE_T offset = SIZE_T(slot) * 2 * sizeof(uint64_t);
    D3D12_RANGE range = {offset, offset + 2 * sizeof(uint64_t)};
    void *mapped = nullptr;
    if (SUCCEEDED(timestamp_readback->Map(0, &range, &mapped))) {
      const uint64_t *ticks = static_cast<const uint64_t *>(mapped) + slot * 2;
      if (ticks[1] >= ticks[0]) {
        last_gpu_time_ms = double(ticks[1] - ticks[0]) * 1000.0 / double(timestamp_frequency);
        if (_wgetenv(L"BLENDER_DLSS5_PROFILE") != nullptr) {
          CLOG_INFO(&LOG, "DLSS5_PROFILE sample=%llu nr_gpu_ms=%.6f creates=%llu size=%dx%d",
                    static_cast<unsigned long long>(timestamp_serial[slot]), last_gpu_time_ms,
                    static_cast<unsigned long long>(feature_create_count), output.extent.x,
                    output.extent.y);
        }
      }
      D3D12_RANGE written = {0, 0};
      timestamp_readback->Unmap(0, &written);
    }
    timestamp_valid[slot] = false;
  }

  bool initialize_ngx()
  {
    std::lock_guard lock(runtime->mutex);
    const NgxResult result = runtime->ngx_allocate_parameters(&parameters);
    if (!NgxSucceeded(result) || parameters == nullptr) {
      set_error("NVSDK_NGX_D3D12_AllocateParameters failed");
      return false;
    }
    return true;
  }

  bool create_feature(const int2 input_extent,
                      const int2 output_extent,
                      const int2 guide_extent,
                      const Dlss5NRSettings &settings,
                      const bool color_is_scene_linear,
                      const bool depth_is_reverse_z)
  {
    const unsigned int create_flags =
        (color_is_scene_linear ? 0x01u : 0u) | (depth_is_reverse_z ? 0x08u : 0u) |
        (guide_extent != input_extent ? 0x02u : 0u);
    SetResource(parameters, "DLSSNR.Color", color.d3d12.Get());
    SetResource(parameters, "DLSSNR.Output", output.d3d12.Get());
    SetResource(parameters, "DLSSNR.Depth", depth.d3d12.Get());
    SetResource(parameters, "DLSSNR.MVec", velocity.d3d12.Get());
    SetUi(parameters, "DLSSNR.Enabled", 1);
    SetI(parameters, "Width", input_extent.x);
    SetI(parameters, "Height", input_extent.y);
    SetI(parameters, "OutWidth", output_extent.x);
    SetI(parameters, "OutHeight", output_extent.y);
    SetUi(parameters, "DLSSNR.Width", input_extent.x);
    SetUi(parameters, "DLSSNR.Height", input_extent.y);
    SetUi(parameters, "DLSSNR.DepthInverted", depth_is_reverse_z ? 1u : 0u);
    SetUi(parameters, "DLSSNR.Reset", 1);
    SetUi(parameters, "DLSSNR.ColorSubrectBaseX", 0);
    SetUi(parameters, "DLSSNR.ColorSubrectBaseY", 0);
    SetUi(parameters, "DLSSNR.ColorSubrectWidth", input_extent.x);
    SetUi(parameters, "DLSSNR.ColorSubrectHeight", input_extent.y);
    SetUi(parameters, "DLSSNR.OutputSubrectBaseX", 0);
    SetUi(parameters, "DLSSNR.OutputSubrectBaseY", 0);
    SetUi(parameters, "DLSSNR.OutputSubrectWidth", output_extent.x);
    SetUi(parameters, "DLSSNR.OutputSubrectHeight", output_extent.y);
    SetUi(parameters, "DLSSNR.DepthSubrectBaseX", 0);
    SetUi(parameters, "DLSSNR.DepthSubrectBaseY", 0);
    SetUi(parameters, "DLSSNR.DepthSubrectWidth", guide_extent.x);
    SetUi(parameters, "DLSSNR.DepthSubrectHeight", guide_extent.y);
    SetUi(parameters, "DLSSNR.MVecSubrectBaseX", 0);
    SetUi(parameters, "DLSSNR.MVecSubrectBaseY", 0);
    SetUi(parameters, "DLSSNR.MVecSubrectWidth", guide_extent.x);
    SetUi(parameters, "DLSSNR.MVecSubrectHeight", guide_extent.y);
    SetF(parameters, "DLSSNR.MVecScaleX", -float(guide_extent.x));
    SetF(parameters, "DLSSNR.MVecScaleY", -float(guide_extent.y));
    SetUi(parameters, "DLSS.Feature.Create.Flags", create_flags);
    SetF(parameters, "DLSSNR.Intensity", settings.intensity);
    SetUi(parameters, "DLSSNR.Style", unsigned(std::max(0, std::min(2, settings.style))));
    SetF(parameters, "DLSSNR.LocalStructureStrength", settings.local_structure_strength);
    SetF(parameters, "DLSSNR.LocalToneStrength", settings.local_tone_strength);
    SetF(parameters, "DLSSNR.SkinStructureStrength", settings.skin_structure_strength);
    SetUi(parameters, "DLSSNR.UseAutoMask", settings.use_auto_mask ? 1u : 0u);
    SetUi(parameters, "DLSSNR.UICorrection", settings.ui_correction ? 1u : 0u);
    SetI(parameters, "CreationNodeMask", 1);
    SetI(parameters, "VisibilityNodeMask", 1);

    /* NGX requires an open (Reset) command list to record CreateFeature into,
     * and the recorded commands must be closed, executed and waited on before
     * the feature handle is considered initialized (see dlss5_host.cpp
     * Begin/ExecuteAndWait around CreateFeature). The previous implementation
     * passed a closed command list (closed at the end of initialize_device)
     * and never submitted it, so CreateFeature either failed outright or left
     * the feature in an uninitialized state, causing every subsequent
     * EvaluateFeature to operate on garbage. */
    command_list_index = (command_list_index + 1) % kCommandListCount;
    const uint64_t slot_completion = command_list_completion_values[command_list_index];
    if (slot_completion != 0 && !wait_for_fence_value(slot_completion, 8000)) {
      return false;
    }

    ID3D12CommandAllocator *allocator = allocators[command_list_index].Get();
    ID3D12GraphicsCommandList *command_list = command_lists[command_list_index].Get();
    HRESULT reset_result = allocator->Reset();
    if (FAILED(reset_result)) {
      set_error("D3D12 allocator reset failed (create_feature): 0x" +
                HResultString(reset_result));
      return false;
    }
    reset_result = command_list->Reset(allocator, nullptr);
    if (FAILED(reset_result)) {
      set_error("D3D12 command list reset failed (create_feature): 0x" +
                HResultString(reset_result));
      return false;
    }

    NgxResult ngx_result;
    {
      std::lock_guard lock(runtime->mutex);
      ngx_result = runtime->ngx_create_feature(command_list, kFeatureId, parameters, &feature);
    }
    if (!NgxSucceeded(ngx_result) || feature == nullptr) {
      command_list->Close();
      set_error("NVSDK_NGX_D3D12_CreateFeature failed: 0x" +
                HResultString(static_cast<HRESULT>(ngx_result)));
      return false;
    }

    reset_result = command_list->Close();
    if (FAILED(reset_result)) {
      set_error("D3D12 command list close failed (create_feature): 0x" +
                HResultString(reset_result));
      return false;
    }
    ID3D12CommandList *lists[] = {command_list};
    queue->ExecuteCommandLists(1, lists);
    ++completion_value;
    reset_result = queue->Signal(completion_fence.Get(), completion_value);
    if (FAILED(reset_result)) {
      set_error("D3D12 completion signal failed (create_feature): 0x" +
                HResultString(reset_result));
      return false;
    }
    command_list_completion_values[command_list_index] = completion_value;

    return true;
  }

  bool create_shared_texture(SharedTexture &texture,
                             const char *name,
                             int2 extent,
                             gpu::TextureFormat format)
  {
    D3D12_RESOURCE_DESC description = {};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = extent.x;
    description.Height = extent.y;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = format == gpu::TextureFormat::SFLOAT_32 ? DXGI_FORMAT_R32_FLOAT :
                                                                    format == gpu::TextureFormat::SFLOAT_16_16 ?
                                                                    DXGI_FORMAT_R16G16_FLOAT :
                                                                    DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    HRESULT result = device->CreateCommittedResource(&heap,
                                                      D3D12_HEAP_FLAG_SHARED,
                                                      &description,
                                                      D3D12_RESOURCE_STATE_COMMON,
                                                      nullptr,
                                                      IID_PPV_ARGS(&texture.d3d12));
    if (FAILED(result)) {
      set_error(std::string("Create shared texture failed: 0x") + HResultString(result));
      return false;
    }
    result = device->CreateSharedHandle(
        texture.d3d12.Get(), nullptr, GENERIC_ALL, nullptr, &texture.shared_handle);
    if (FAILED(result)) {
      set_error(std::string("CreateSharedHandle failed: 0x") + HResultString(result));
      return false;
    }

    GPUExternalTextureHandle external;
    external.type = GPUExternalTextureType::D3D12_RESOURCE;
    external.handle = reinterpret_cast<uint64_t>(texture.shared_handle);
    texture.vulkan = GPU_texture_create_2d_from_external(
        name,
        extent.x,
        extent.y,
        format,
        GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE |
            GPU_TEXTURE_USAGE_ATTACHMENT,
        external);
    if (texture.vulkan == nullptr) {
      set_error(std::string("Vulkan import failed for ") + name);
      return false;
    }
    CloseHandle(texture.shared_handle);
    texture.shared_handle = nullptr;
    texture.extent = extent;
    texture.format = format;
    return true;
  }

  void release_feature()
  {
    if (feature != nullptr && runtime != nullptr) {
      std::lock_guard lock(runtime->mutex);
      runtime->ngx_release_feature(feature);
      feature = nullptr;
    }
    feature_settings_valid = false;
  }

  bool ensure_resources(const int2 input_extent,
                        const int2 output_extent,
                        const int2 guide_extent,
                        const Dlss5NRSettings &settings,
                        const bool color_is_scene_linear,
                        const bool depth_is_reverse_z)
  {
    if (input_extent.x < 32 || input_extent.y < 32 || output_extent.x < 32 ||
        output_extent.y < 32 || guide_extent.x < 32 || guide_extent.y < 32 ||
        input_extent != output_extent)
    {
      set_error("DLSSNR requires matching color/output extents >= 32px");
      return false;
    }
    const bool same_size = initialized && color.extent == input_extent &&
                           output.extent == output_extent && depth.extent == guide_extent &&
                           velocity.extent == guide_extent;
    const bool same_settings = initialized && feature_settings_valid &&
                               feature_color_is_scene_linear == color_is_scene_linear &&
                               feature_depth_is_reverse_z == depth_is_reverse_z;
    if (same_size && same_settings) {
      return true;
    }
    if (GPU_backend_get_type() != GPU_BACKEND_VULKAN) {
      set_error("realtime DLSSNR requires the Vulkan GPU backend");
      return false;
    }

    /* OptiScaler keeps NGX/device loaded and only ReleaseFeature + CreateFeature
     * when size or tuning changes. Reloading nvngx_dlssnr.dll (165MB) on every
     * slider tick is what made enable feel like a hang. */
    if (device == nullptr) {
      if (!initialize_device()) {
        reset();
        return false;
      }
      external_sync = initialize_external_sync();
      if (!external_sync) { return false; }
      if (external_sync) {
        CLOG_INFO(&LOG, "DLSS5 synchronization: external semaphore");
      }
      else {
        CLOG_WARN(&LOG, "DLSS5 synchronization: CPU fallback (%s)", status.c_str());
      }
    }
    if (parameters == nullptr) {
      if (!initialize_ngx()) {
        reset();
        return false;
      }
    }

    /* Pending Vulkan reads of the previous output must retire before resize. */
    if (!same_size && initialized) { GPU_finish(); }
    if (!wait_for_fence_value(completion_value, 30000)) {
      set_error("DLSS5 resize deferred: GPU work has not completed");
      return false;
    }
    release_feature();

    if (!same_size) {
      color.reset();
      depth.reset();
      velocity.reset();
      output.reset();
      if (!create_shared_texture(
              color, "DLSS5.Color", input_extent, gpu::TextureFormat::SFLOAT_16_16_16_16) ||
          !create_shared_texture(depth, "DLSS5.Depth", guide_extent, gpu::TextureFormat::SFLOAT_32) ||
          !create_shared_texture(
              velocity, "DLSS5.Velocity", guide_extent, gpu::TextureFormat::SFLOAT_16_16) ||
          !create_shared_texture(
              output, "DLSS5.Output", output_extent, gpu::TextureFormat::SFLOAT_16_16_16_16))
      {
        reset();
        return false;
      }
    }

    if (!create_feature(input_extent,
                        output_extent,
                        guide_extent,
                        settings,
                        color_is_scene_linear,
                        depth_is_reverse_z))
    {
      initialized = false;
      return false;
    }
    initialized = true;
    ++feature_create_count;
    CLOG_INFO(&LOG, "DLSS5 feature created: count=%llu size=%dx%d",
              static_cast<unsigned long long>(feature_create_count), output_extent.x, output_extent.y);
    feature_settings_valid = true;
    feature_color_is_scene_linear = color_is_scene_linear;
    feature_depth_is_reverse_z = depth_is_reverse_z;
    status = external_sync ? "ready (external sync)" : "ready (CPU synchronization)";
    return true;
  }

  void transition(ID3D12GraphicsCommandList *command_list,
                  ID3D12Resource *resource,
                  D3D12_RESOURCE_STATES before,
                  D3D12_RESOURCE_STATES after)
  {
    if (before == after) {
      return;
    }
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    command_list->ResourceBarrier(1, &barrier);
  }

  bool evaluate(const Dlss5D3D12Frame &frame)
  {
    if (!initialized || feature == nullptr) {
      return false;
    }
    SetResource(parameters, "DLSSNR.Color", color.d3d12.Get());
    SetResource(parameters, "DLSSNR.Output", output.d3d12.Get());
    SetResource(parameters, "DLSSNR.MVec", velocity.d3d12.Get());
    SetResource(parameters, "DLSSNR.Depth", depth.d3d12.Get());
    SetI(parameters, "Width", frame.input_extent.x);
    SetI(parameters, "Height", frame.input_extent.y);
    SetI(parameters, "OutWidth", frame.output_extent.x);
    SetI(parameters, "OutHeight", frame.output_extent.y);
    SetUi(parameters, "DLSSNR.Enabled", 1);
    SetUi(parameters, "DLSSNR.Width", frame.input_extent.x);
    SetUi(parameters, "DLSSNR.Height", frame.input_extent.y);
    SetUi(parameters, "DLSSNR.DepthInverted", frame.depth_is_reverse_z ? 1u : 0u);
    SetUi(parameters, "DLSSNR.Reset", frame.reset_history ? 1u : 0u);
    SetRect(parameters, "DLSSNR.Color", frame.input_extent.x, frame.input_extent.y);
    SetRect(parameters, "DLSSNR.MVec", frame.guide_extent.x, frame.guide_extent.y);
    SetRect(parameters, "DLSSNR.Depth", frame.guide_extent.x, frame.guide_extent.y);
    SetRect(parameters, "DLSSNR.Output", frame.output_extent.x, frame.output_extent.y);
    SetF(parameters, "DLSSNR.MVecScaleX", frame.velocity_is_pixel_space ? 1.0f :
                                                         -float(frame.guide_extent.x));
    SetF(parameters, "DLSSNR.MVecScaleY", frame.velocity_is_pixel_space ? 1.0f :
                                                         -float(frame.guide_extent.y));
    SetF(parameters, "DLSSNR.Intensity", frame.settings.intensity);
    SetF(parameters, "DLSSNR.LocalToneStrength", frame.settings.local_tone_strength);
    SetF(parameters, "DLSSNR.LocalStructureStrength", frame.settings.local_structure_strength);
    SetF(parameters, "DLSSNR.SkinStructureStrength", frame.settings.skin_structure_strength);
    SetUi(parameters, "DLSSNR.UseAutoMask", frame.settings.use_auto_mask ? 1u : 0u);
    SetI(parameters, "DLSSNR.Style", std::max(0, std::min(2, frame.settings.style)));
    SetUi(parameters, "DLSSNR.UICorrection", frame.settings.ui_correction ? 1u : 0u);
    if (!parameters_reported) {
      CLOG_INFO(&LOG,
                "DLSS5 NR parameters: intensity=%f local_tone=%f local_structure=%f "
                "skin_structure=%f auto_mask=%d ui_correction=%d exposure_scale=%f",
                frame.settings.intensity,
                frame.settings.local_tone_strength,
                frame.settings.local_structure_strength,
                frame.settings.skin_structure_strength,
                frame.settings.use_auto_mask,
                frame.settings.ui_correction,
                frame.exposure_scale);
      parameters_reported = true;
    }

    command_list_index = (command_list_index + 1) % kCommandListCount;
    const uint64_t slot_completion = command_list_completion_values[command_list_index];
    /* Do not CPU-stall the viewport if the previous NR is still on the GPU.
     * OptiScaler/dlss5-bridge keep Evaluate on the queue and never WaitForSingleObject. */
    if (slot_completion != 0 &&
        completion_fence != nullptr &&
        completion_fence->GetCompletedValue() < slot_completion)
    {
      set_error("DLSSNR GPU busy");
      return false;
    }

    read_timestamp(command_list_index);
    ID3D12CommandAllocator *allocator = allocators[command_list_index].Get();
    ID3D12GraphicsCommandList *command_list = command_lists[command_list_index].Get();
    HRESULT result = allocator->Reset();
    if (FAILED(result)) {
      set_error("D3D12 allocator reset failed: 0x" + HResultString(result));
      return false;
    }
    result = command_list->Reset(allocator, nullptr);
    if (FAILED(result)) {
      set_error("D3D12 command list reset failed: 0x" + HResultString(result));
      return false;
    }

    if (external_sync && vulkan_to_d3d12_value != 0) {
      result = queue->Wait(vulkan_to_d3d12_fence.Get(), vulkan_to_d3d12_value);
      if (FAILED(result)) {
        set_error("D3D12 wait for Vulkan submission failed: 0x" + HResultString(result));
        return false;
      }
    }

    transition(command_list,
               color.d3d12.Get(),
               D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(command_list,
               depth.d3d12.Get(),
               D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(command_list,
               velocity.d3d12.Get(),
               D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(command_list,
               output.d3d12.Get(),
               D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (timestamp_heap != nullptr) {
      command_list->EndQuery(timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, command_list_index * 2);
    }
    NgxResult evaluate_result;
    {
      std::lock_guard lock(runtime->mutex);
      evaluate_result = runtime->ngx_evaluate_feature(command_list, feature, parameters, nullptr);
    }
    if (!NgxSucceeded(evaluate_result)) {
      command_list->Close();
      set_error("NVSDK_NGX_D3D12_EvaluateFeature failed: 0x" +
                HResultString(static_cast<HRESULT>(evaluate_result)));
      return false;
    }

    if (timestamp_heap != nullptr) {
      const UINT index = command_list_index * 2;
      command_list->EndQuery(timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index + 1);
      command_list->ResolveQueryData(timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                    index, 2, timestamp_readback.Get(), index * sizeof(uint64_t));
      timestamp_valid[command_list_index] = true;
      timestamp_serial[command_list_index] = evaluate_count + 1;
    }
    transition(command_list,
               color.d3d12.Get(),
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COMMON);
    transition(command_list,
               depth.d3d12.Get(),
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COMMON);
    transition(command_list,
               velocity.d3d12.Get(),
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COMMON);
    transition(command_list,
               output.d3d12.Get(),
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COMMON);
    result = command_list->Close();
    if (FAILED(result)) {
      set_error("D3D12 command list close failed: 0x" + HResultString(result));
      return false;
    }
    ID3D12CommandList *lists[] = {command_list};
    queue->ExecuteCommandLists(1, lists);
    ++completion_value;
    result = queue->Signal(completion_fence.Get(), completion_value);
    if (FAILED(result)) {
      set_error("D3D12 completion signal failed: 0x" + HResultString(result));
      return false;
    }
    command_list_completion_values[command_list_index] = completion_value;
    ++evaluate_count;

    if (external_sync) {
      ++d3d12_to_vulkan_value;
      result = queue->Signal(d3d12_to_vulkan_fence.Get(), d3d12_to_vulkan_value);
      if (FAILED(result)) {
        set_error("D3D12 output signal failed: 0x" + HResultString(result));
        return false;
      }
    }
    else if (!wait_for_fence_value(completion_value, 3000)) {
      return false;
    }
    return true;
  }

  bool acquire_textures()
  {
    if (!textures_released) { return true; }
    gpu::Texture *textures[] = {color.vulkan, depth.vulkan, velocity.vulkan, output.vulkan};
    if (!GPU_vulkan_external_textures_transfer(textures, 4, true)) { return false; }
    textures_released = false;
    return true;
  }

  bool wait_for_output()
  {
    if (!external_sync || d3d12_to_vulkan_value == 0) {
      return true;
    }
    if (!GPU_vulkan_external_semaphore_wait(
            d3d12_to_vulkan_semaphore, d3d12_to_vulkan_value))
    {
      set_error("Vulkan wait for DLSS5 output failed");
      return false;
    }
    return acquire_textures();
  }

  bool warmup()
  {
    if (GPU_backend_get_type() != GPU_BACKEND_VULKAN) {
      set_error("realtime DLSSNR requires the Vulkan GPU backend");
      return false;
    }
    if (device == nullptr) {
      if (!initialize_device()) {
        return false;
      }
      external_sync = initialize_external_sync();
      if (!external_sync) { return false; }
      if (external_sync) {
        CLOG_INFO(&LOG, "DLSS5 synchronization: external semaphore");
      }
      else {
        CLOG_WARN(&LOG, "DLSS5 synchronization: CPU fallback (%s)", status.c_str());
      }
    }
    if (parameters == nullptr) {
      if (!initialize_ngx()) {
        return false;
      }
      CLOG_INFO(&LOG, "DLSS5 NGX warmed up (dll kept loaded)");
    }
    return true;
  }
};

Dlss5D3D12Session::Dlss5D3D12Session() : impl_(new Impl()) {}

Dlss5D3D12Session::~Dlss5D3D12Session()
{
  delete impl_;
}

bool Dlss5D3D12Session::ensure_resources(const int2 input_extent,
                                          const int2 output_extent,
                                          const int2 guide_extent,
                                          const Dlss5NRSettings &settings,
                                          const bool color_is_scene_linear,
                                          const bool depth_is_reverse_z)
{
  return impl_->ensure_resources(
      input_extent, output_extent, guide_extent, settings, color_is_scene_linear, depth_is_reverse_z);
}

void Dlss5D3D12Session::reset()
{
  impl_->reset();
}

bool Dlss5D3D12Session::copy_inputs_and_evaluate(const Dlss5D3D12Frame &frame,
                                                 const bool copy_color,
                                                 const bool copy_velocity)
{
  if (!impl_->initialized || frame.color == nullptr || frame.depth == nullptr ||
      frame.velocity == nullptr)
  {
    return false;
  }
  if (GPU_texture_width(frame.color) != frame.input_extent.x ||
      GPU_texture_height(frame.color) != frame.input_extent.y ||
      GPU_texture_width(frame.depth) != frame.guide_extent.x ||
      GPU_texture_height(frame.depth) != frame.guide_extent.y ||
      GPU_texture_width(frame.velocity) != frame.guide_extent.x ||
      GPU_texture_height(frame.velocity) != frame.guide_extent.y)
  {
    impl_->set_error("DLSS5 input texture extent mismatch");
    return false;
  }

  if (copy_color) {
    GPU_texture_copy(impl_->color.vulkan, frame.color);
  }
  if (copy_velocity) {
    GPU_texture_copy(impl_->velocity.vulkan, frame.velocity);
  }
  if (impl_->external_sync) {
    gpu::Texture *textures[] = {impl_->color.vulkan, impl_->depth.vulkan,
                               impl_->velocity.vulkan, impl_->output.vulkan};
    if (!GPU_vulkan_external_textures_transfer(textures, 4, false)) {
      impl_->set_error("Vulkan shared texture release failed");
      return false;
    }
    impl_->textures_released = true;
    ++impl_->vulkan_to_d3d12_value;
    if (!GPU_vulkan_external_semaphore_signal(impl_->vulkan_to_d3d12_semaphore,
                                              impl_->vulkan_to_d3d12_value))
    {
      impl_->set_error("Vulkan signal for D3D12 submission failed");
      return false;
    }
  }
  else {
    GPU_finish();
  }
  if (!impl_->evaluate(frame)) {
    /* Failed recording does not submit writes, but the input release was already
     * queued. Reclaim ownership before any later conversion can reuse textures. */
    impl_->wait_for_output();
    impl_->acquire_textures();
    return false;
  }
  return true;
}

bool Dlss5D3D12Session::wait_for_output()
{
  return impl_->wait_for_output();
}

bool Dlss5D3D12Session::warmup()
{
  return impl_->warmup();
}

gpu::Texture *Dlss5D3D12Session::color_texture() const
{
  return impl_->color.vulkan;
}

gpu::Texture *Dlss5D3D12Session::depth_texture() const
{
  return impl_->depth.vulkan;
}

gpu::Texture *Dlss5D3D12Session::velocity_texture() const
{
  return impl_->velocity.vulkan;
}

gpu::Texture *Dlss5D3D12Session::output_texture() const
{
  return impl_->output.vulkan;
}

double Dlss5D3D12Session::gpu_time_ms() const
{
  if (impl_->command_list_index >= 0) { impl_->read_timestamp(impl_->command_list_index); }
  return impl_->last_gpu_time_ms;
}

bool Dlss5D3D12Session::available() const
{
  return impl_->initialized;
}

const char *Dlss5D3D12Session::status() const
{
  return impl_->status.c_str();
}

}  // namespace blender::eevee

#else

namespace blender::eevee {

struct Dlss5D3D12Session::Impl {};

Dlss5D3D12Session::Dlss5D3D12Session() : impl_(new Impl()) {}
Dlss5D3D12Session::~Dlss5D3D12Session()
{
  delete impl_;
}
bool Dlss5D3D12Session::ensure_resources(int2,
                                          int2,
                                          int2,
                                          const Dlss5NRSettings &,
                                          bool,
                                          bool)
{
  return false;
}
void Dlss5D3D12Session::reset() {}
bool Dlss5D3D12Session::warmup()
{
  return false;
}
bool Dlss5D3D12Session::copy_inputs_and_evaluate(const Dlss5D3D12Frame &, bool, bool)
{
  return false;
}
bool Dlss5D3D12Session::wait_for_output()
{
  return false;
}
gpu::Texture *Dlss5D3D12Session::color_texture() const
{
  return nullptr;
}
gpu::Texture *Dlss5D3D12Session::depth_texture() const
{
  return nullptr;
}
gpu::Texture *Dlss5D3D12Session::velocity_texture() const
{
  return nullptr;
}
gpu::Texture *Dlss5D3D12Session::output_texture() const
{
  return nullptr;
}
double Dlss5D3D12Session::gpu_time_ms() const { return -1.0; }

bool Dlss5D3D12Session::available() const
{
  return false;
}
const char *Dlss5D3D12Session::status() const
{
  return "D3D12 is only available on Windows";
}

}  // namespace blender::eevee

#endif
