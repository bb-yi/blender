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
  NVSDK_NGX_Parameter *parameters = nullptr;
  NVSDK_NGX_Handle *feature = nullptr;
  fs::path runtime_directory;
  bool core_initialized = false;
  bool snippet_initialized = false;
  bool initialized = false;
  bool parameters_reported = false;
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
    wait_for_completion();
    if (GPU_context_active_get() != nullptr) {
      GPU_finish();
    }

    GPU_vulkan_external_semaphore_free(d3d12_to_vulkan_semaphore);
    d3d12_to_vulkan_semaphore = nullptr;
    GPU_vulkan_external_semaphore_free(vulkan_to_d3d12_semaphore);
    vulkan_to_d3d12_semaphore = nullptr;
    external_sync = false;

    if (feature != nullptr && ngx_release_feature != nullptr) {
      ngx_release_feature(feature);
      feature = nullptr;
    }
    if (parameters != nullptr && ngx_destroy_parameters != nullptr) {
      ngx_destroy_parameters(parameters);
      parameters = nullptr;
    }
    if (snippet_initialized && ngx_shutdown != nullptr && device) {
      ngx_shutdown(device.Get());
      snippet_initialized = false;
    }
    if (core_initialized && ngx_shutdown_core != nullptr && device) {
      ngx_shutdown_core(device.Get());
      core_initialized = false;
    }

    UnhookSnippetGetModuleFileNameW(snippet_iat_slot);
    snippet_iat_slot = nullptr;

    color.reset();
    depth.reset();
    velocity.reset();
    output.reset();

    if (completion_event != nullptr) {
      CloseHandle(completion_event);
      completion_event = nullptr;
    }
    if (snippet_module != nullptr) {
      FreeLibrary(snippet_module);
      snippet_module = nullptr;
    }
    if (core_module != nullptr) {
      FreeLibrary(core_module);
      core_module = nullptr;
    }
    for (auto &command_list : command_lists) {
      command_list.Reset();
    }
    for (auto &allocator : allocators) {
      allocator.Reset();
    }
    command_list_completion_values = {};
    command_list_index = -1;
    queue.Reset();
    completion_fence.Reset();
    vulkan_to_d3d12_fence.Reset();
    d3d12_to_vulkan_fence.Reset();
    completion_value = 0;
    vulkan_to_d3d12_value = 0;
    d3d12_to_vulkan_value = 0;
    device.Reset();
    initialized = false;
    parameters_reported = false;
  }

  void wait_for_completion()
  {
    if (completion_fence == nullptr || completion_value == 0 ||
        completion_fence->GetCompletedValue() >= completion_value)
    {
      return;
    }
    if (completion_event == nullptr) {
      return;
    }
    if (FAILED(completion_fence->SetEventOnCompletion(completion_value, completion_event))) {
      return;
    }
    WaitForSingleObject(completion_event, INFINITE);
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
    ComPtr<IDXGIFactory6> factory;
    HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
      set_error("CreateDXGIFactory1 failed: 0x" + HResultString(result));
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
      if (SUCCEEDED(D3D12CreateDevice(
              adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
      {
        selected_adapter = adapter;
        break;
      }
    }
    if (!selected_adapter || !device) {
      set_error("No NVIDIA D3D12 device found");
      return false;
    }

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
    completion_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (completion_event == nullptr) {
      set_error("CreateEventW failed");
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

    result = ngx_allocate_parameters(&parameters);
    if (!NgxSucceeded(result) || parameters == nullptr) {
      if (ngx_get_capability_parameters == nullptr ||
          !NgxSucceeded(ngx_get_capability_parameters(&parameters)))
      {
        set_error("NVSDK_NGX_D3D12_AllocateParameters failed: 0x" +
                  HResultString(static_cast<HRESULT>(result)));
        return false;
      }
    }
    if (parameters == nullptr) {
      set_error("DLSS5 NGX returned a null parameter interface");
      return false;
    }
    return true;
  }

  bool create_feature(int2 extent)
  {
    SetI(parameters, "Width", extent.x);
    SetI(parameters, "Height", extent.y);
    SetI(parameters, "OutWidth", extent.x);
    SetI(parameters, "OutHeight", extent.y);
    SetI(parameters, "DLSSNR.Width", extent.x);
    SetI(parameters, "DLSSNR.Height", extent.y);
    SetI(parameters, "DLSSNR.InputWidth", extent.x);
    SetI(parameters, "DLSSNR.InputHeight", extent.y);
    SetI(parameters, "DLSSNR.OutputWidth", extent.x);
    SetI(parameters, "DLSSNR.OutputHeight", extent.y);
    SetI(parameters, "DLSSNR.Output.Width", extent.x);
    SetI(parameters, "DLSSNR.Output.Height", extent.y);
    SetI(parameters, "DLSSNR.Hint.Render.Preset", 0);
    SetI(parameters, "CreationNodeMask", 1);
    SetI(parameters, "VisibilityNodeMask", 1);
    SetUi(parameters, "DLSS.Output.Subrect.Base.X", 0);
    SetUi(parameters, "DLSS.Output.Subrect.Base.Y", 0);
    SetUi(parameters, "DLSSNR.Upscaling", 0);
    SetF(parameters, "DLSSNR.Scale", 1.0f);
    SetF(parameters, "DLSSNR.ScalingRatio", 1.0f);
    SetUll(parameters,
           "DLSSNRComputeScalingRatioCallback",
           reinterpret_cast<unsigned long long>(&ComputeScalingRatio));

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
    if (slot_completion != 0 && completion_fence->GetCompletedValue() < slot_completion) {
      if (FAILED(completion_fence->SetEventOnCompletion(slot_completion, completion_event))) {
        set_error("D3D12 command allocator completion event failed (create_feature)");
        return false;
      }
      WaitForSingleObject(completion_event, INFINITE);
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

    const NgxResult ngx_result =
        ngx_create_feature(command_list, kFeatureId, parameters, &feature);
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

    /* CreateFeature must retire on the GPU before the handle is usable. */
    if (completion_fence->GetCompletedValue() < completion_value) {
      if (FAILED(completion_fence->SetEventOnCompletion(completion_value, completion_event))) {
        set_error("D3D12 create_feature completion event failed");
        return false;
      }
      WaitForSingleObject(completion_event, INFINITE);
    }
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
    texture.shared_handle = nullptr;
    texture.extent = extent;
    texture.format = format;
    return true;
  }

  bool ensure_resources(int2 input_extent, int2 output_extent)
  {
    if (input_extent.x < 32 || input_extent.y < 32 || input_extent != output_extent) {
      set_error("realtime DLSSNR currently requires matching extents >= 32px");
      return false;
    }
    if (initialized && color.extent == input_extent && output.extent == output_extent) {
      return true;
    }

    reset();
    if (GPU_backend_get_type() != GPU_BACKEND_VULKAN) {
      set_error("realtime DLSSNR requires the Vulkan GPU backend");
      return false;
    }
    if (!initialize_device()) {
      reset();
      return false;
    }
    external_sync = initialize_external_sync();
    if (external_sync) {
      CLOG_INFO(&LOG, "DLSS5 synchronization: external semaphore");
    }
    else {
      CLOG_WARN(&LOG,
                "DLSS5 synchronization: CPU fallback (%s)",
                status.c_str());
    }
    if (!initialize_ngx()) {
      reset();
      return false;
    }
    if (!create_shared_texture(
            color, "DLSS5.Color", input_extent, gpu::TextureFormat::SFLOAT_16_16_16_16) ||
        !create_shared_texture(depth, "DLSS5.Depth", input_extent, gpu::TextureFormat::SFLOAT_32) ||
        !create_shared_texture(velocity, "DLSS5.Velocity", input_extent, gpu::TextureFormat::SFLOAT_16_16) ||
        !create_shared_texture(
            output, "DLSS5.Output", output_extent, gpu::TextureFormat::SFLOAT_16_16_16_16))
    {
      reset();
      return false;
    }
    if (!create_feature(input_extent)) {
      reset();
      return false;
    }
    initialized = true;
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
    SetRect(parameters, "DLSSNR.Color", frame.input_extent.x, frame.input_extent.y);
    SetRect(parameters, "DLSSNR.MVec", frame.input_extent.x, frame.input_extent.y);
    SetRect(parameters, "DLSSNR.Depth", frame.input_extent.x, frame.input_extent.y);
    SetRect(parameters, "DLSSNR.Output", frame.output_extent.x, frame.output_extent.y);
    SetF(parameters, "DLSSNR.MVecScaleX", frame.velocity_is_pixel_space ? 1.0f :
                                                         -float(frame.input_extent.x));
    SetF(parameters, "DLSSNR.MVecScaleY", frame.velocity_is_pixel_space ? 1.0f :
                                                         -float(frame.input_extent.y));
    SetUi(parameters, "DLSSNR.DepthInverted", frame.depth_is_reverse_z ? 1u : 0u);
    SetUi(parameters, "DLSSNR.Enabled", 1);
    SetUi(parameters, "DLSSNR.Reset", frame.reset_history ? 1u : 0u);
    SetF(parameters, "DLSSNR.Intensity", frame.settings.intensity);
    SetF(parameters, "DLSSNR.LocalToneStrength", frame.settings.local_tone_strength);
    SetF(parameters, "DLSSNR.LocalStructureStrength", frame.settings.local_structure_strength);
    SetF(parameters, "DLSSNR.SkinStructureStrength", frame.settings.skin_structure_strength);
    SetUi(parameters, "DLSSNR.UseAutoMask", frame.settings.use_auto_mask ? 1u : 0u);
    SetI(parameters, "DLSSNR.Style", 0);
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
    if (slot_completion != 0 && completion_fence->GetCompletedValue() < slot_completion) {
      if (FAILED(completion_fence->SetEventOnCompletion(slot_completion, completion_event))) {
        set_error("D3D12 command allocator completion event failed");
        return false;
      }
      WaitForSingleObject(completion_event, INFINITE);
    }

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

    const NgxResult evaluate_result =
        ngx_evaluate_feature(command_list, feature, parameters, nullptr);
    if (!NgxSucceeded(evaluate_result)) {
      command_list->Close();
      set_error("NVSDK_NGX_D3D12_EvaluateFeature failed: 0x" +
                HResultString(static_cast<HRESULT>(evaluate_result)));
      return false;
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

    if (external_sync) {
      ++d3d12_to_vulkan_value;
      result = queue->Signal(d3d12_to_vulkan_fence.Get(), d3d12_to_vulkan_value);
      if (FAILED(result)) {
        set_error("D3D12 output signal failed: 0x" + HResultString(result));
        return false;
      }
    }
    else if (completion_fence->GetCompletedValue() < completion_value) {
      result = completion_fence->SetEventOnCompletion(completion_value, completion_event);
      if (FAILED(result)) {
        set_error("D3D12 completion event failed: 0x" + HResultString(result));
        return false;
      }
      WaitForSingleObject(completion_event, INFINITE);
    }
    return true;
  }

  bool wait_for_output()
  {
    if (!external_sync || d3d12_to_vulkan_value == 0) {
      return true;
    }
    return GPU_vulkan_external_semaphore_wait(
        d3d12_to_vulkan_semaphore, d3d12_to_vulkan_value);
  }
};

Dlss5D3D12Session::Dlss5D3D12Session() : impl_(new Impl()) {}

Dlss5D3D12Session::~Dlss5D3D12Session()
{
  delete impl_;
}

bool Dlss5D3D12Session::ensure_resources(const int2 input_extent, const int2 output_extent)
{
  return impl_->ensure_resources(input_extent, output_extent);
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
      GPU_texture_width(frame.velocity) != frame.input_extent.x ||
      GPU_texture_height(frame.velocity) != frame.input_extent.y)
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
  return impl_->evaluate(frame);
}

bool Dlss5D3D12Session::wait_for_output()
{
  return impl_->wait_for_output();
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
bool Dlss5D3D12Session::ensure_resources(int2, int2)
{
  return false;
}
void Dlss5D3D12Session::reset() {}
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
