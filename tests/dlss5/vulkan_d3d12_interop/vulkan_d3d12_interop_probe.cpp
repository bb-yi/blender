/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#ifndef VK_USE_PLATFORM_WIN32_KHR
#  define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct ExtensionSet {
  std::set<std::string> names;

  bool contains(const char *name) const
  {
    return names.find(name) != names.end();
  }
};

ExtensionSet EnumerateDeviceExtensions(VkPhysicalDevice device)
{
  uint32_t count = 0;
  VkResult result = vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
  if (result != VK_SUCCESS) {
    throw std::runtime_error("vkEnumerateDeviceExtensionProperties failed");
  }
  std::vector<VkExtensionProperties> properties(count);
  result = vkEnumerateDeviceExtensionProperties(device, nullptr, &count, properties.data());
  if (result != VK_SUCCESS) {
    throw std::runtime_error("vkEnumerateDeviceExtensionProperties failed");
  }

  ExtensionSet extensions;
  for (const VkExtensionProperties &property : properties) {
    extensions.names.emplace(property.extensionName);
  }
  return extensions;
}

std::string VkResultName(VkResult result)
{
  switch (result) {
    case VK_SUCCESS:
      return "VK_SUCCESS";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
      return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
      return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE:
      return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    default:
      return "VkResult(" + std::to_string(static_cast<int>(result)) + ")";
  }
}

std::string HexLuid(const uint8_t *luid, size_t size)
{
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (size_t index = 0; index < size; ++index) {
    stream << std::setw(2) << static_cast<unsigned>(luid[index]);
  }
  return stream.str();
}

bool SameLuid(const uint8_t *vulkan_luid, const LUID &dxgi_luid)
{
  return std::memcmp(vulkan_luid, &dxgi_luid, sizeof(LUID)) == 0;
}

const char *MemoryHandleName(VkExternalMemoryHandleTypeFlagBits handle_type)
{
  switch (handle_type) {
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT:
      return "D3D12_HEAP";
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT:
      return "D3D12_RESOURCE";
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT:
      return "OPAQUE_WIN32";
    default:
      return "UNKNOWN_MEMORY_HANDLE";
  }
}

const char *SemaphoreHandleName(VkExternalSemaphoreHandleTypeFlagBits handle_type)
{
  switch (handle_type) {
    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT:
      return "D3D12_FENCE";
    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT:
      return "OPAQUE_WIN32";
    default:
      return "UNKNOWN_SEMAPHORE_HANDLE";
  }
}

bool HasExternalFeature(VkExternalMemoryFeatureFlags features, VkExternalMemoryFeatureFlagBits bit)
{
  return (features & bit) != 0;
}

struct ImageQuery {
  VkResult result = VK_ERROR_UNKNOWN;
  VkExternalMemoryFeatureFlags features = 0;
  VkExternalMemoryHandleTypeFlags compatible_handles = 0;
  VkExternalMemoryHandleTypeFlags export_from_imported_handles = 0;
};

ImageQuery QueryImage(VkPhysicalDevice device,
                      VkFormat format,
                      VkExternalMemoryHandleTypeFlagBits handle_type)
{
  VkPhysicalDeviceExternalImageFormatInfo external_info = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
  external_info.handleType = handle_type;

  VkPhysicalDeviceImageFormatInfo2 image_info = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
  image_info.pNext = &external_info;
  image_info.format = format;
  image_info.type = VK_IMAGE_TYPE_2D;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.flags = 0;

  VkExternalImageFormatProperties external_properties = {
      VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
  VkImageFormatProperties2 properties = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
  properties.pNext = &external_properties;

  ImageQuery query;
  query.result = vkGetPhysicalDeviceImageFormatProperties2(device, &image_info, &properties);
  if (query.result == VK_SUCCESS) {
    query.features = external_properties.externalMemoryProperties.externalMemoryFeatures;
    query.compatible_handles =
        external_properties.externalMemoryProperties.compatibleHandleTypes;
    query.export_from_imported_handles =
        external_properties.externalMemoryProperties.exportFromImportedHandleTypes;
  }
  return query;
}

struct SemaphoreQuery {
  VkExternalSemaphoreHandleTypeFlags export_from_imported_handles = 0;
  VkExternalSemaphoreHandleTypeFlags compatible_handles = 0;
  VkExternalSemaphoreFeatureFlags features = 0;
};

SemaphoreQuery QuerySemaphore(VkPhysicalDevice device,
                              VkExternalSemaphoreHandleTypeFlagBits handle_type)
{
  VkPhysicalDeviceExternalSemaphoreInfo info = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
  info.handleType = handle_type;

  VkExternalSemaphoreProperties properties = {
      VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
  vkGetPhysicalDeviceExternalSemaphoreProperties(device, &info, &properties);

  return {properties.exportFromImportedHandleTypes,
          properties.compatibleHandleTypes,
          properties.externalSemaphoreFeatures};
}

void PrintImageQuery(VkPhysicalDevice device,
                     VkFormat format,
                     const char *format_name,
                     VkExternalMemoryHandleTypeFlagBits handle_type)
{
  const ImageQuery query = QueryImage(device, format, handle_type);
  std::cout << "[interop] image format=" << format_name
            << " handle=" << MemoryHandleName(handle_type)
            << " result=" << VkResultName(query.result);
  if (query.result == VK_SUCCESS) {
    std::cout << " export="
              << HasExternalFeature(query.features, VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)
              << " import="
              << HasExternalFeature(query.features, VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)
              << " dedicated="
              << HasExternalFeature(query.features,
                                    VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT)
              << " compatible=0x" << std::hex << query.compatible_handles << std::dec;
  }
  std::cout << '\n';
}

void PrintSemaphoreQuery(VkPhysicalDevice device,
                         VkExternalSemaphoreHandleTypeFlagBits handle_type)
{
  const SemaphoreQuery query = QuerySemaphore(device, handle_type);
  std::cout << "[interop] semaphore handle=" << SemaphoreHandleName(handle_type)
            << " export=" << ((query.features & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) != 0)
            << " import=" << ((query.features & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) != 0)
            << " compatible=0x" << std::hex << query.compatible_handles << std::dec << '\n';
}

}  // namespace

int main()
{
  try {
    uint32_t instance_version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion != nullptr) {
      vkEnumerateInstanceVersion(&instance_version);
    }
    std::cout << "[interop] Vulkan instance version=" << VK_VERSION_MAJOR(instance_version) << '.'
              << VK_VERSION_MINOR(instance_version) << '.' << VK_VERSION_PATCH(instance_version)
              << '\n';

    VkApplicationInfo application_info = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application_info.pApplicationName = "DLSS5 NPR Vulkan-D3D12 Interop Probe";
    application_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    application_info.pEngineName = "Blender NPR";
    application_info.engineVersion = VK_MAKE_VERSION(5, 2, 0);
    application_info.apiVersion = std::min(instance_version, VK_API_VERSION_1_3);

    VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &application_info;
    VkInstance instance = VK_NULL_HANDLE;
    VkResult vulkan_result = vkCreateInstance(&instance_info, nullptr, &instance);
    if (vulkan_result != VK_SUCCESS) {
      std::cerr << "[interop] vkCreateInstance failed: " << VkResultName(vulkan_result) << '\n';
      return 2;
    }

    uint32_t device_count = 0;
    vulkan_result = vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    if (vulkan_result != VK_SUCCESS || device_count == 0) {
      std::cerr << "[interop] no Vulkan physical device: " << VkResultName(vulkan_result) << '\n';
      vkDestroyInstance(instance, nullptr);
      return 2;
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    vulkan_result = vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
    if (vulkan_result != VK_SUCCESS) {
      std::cerr << "[interop] vkEnumeratePhysicalDevices failed: "
                << VkResultName(vulkan_result) << '\n';
      vkDestroyInstance(instance, nullptr);
      return 2;
    }

    VkPhysicalDevice selected = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties selected_properties = {};
    VkPhysicalDeviceIDProperties selected_id = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    ExtensionSet selected_extensions;
    for (VkPhysicalDevice device : devices) {
      VkPhysicalDeviceProperties properties = {};
      vkGetPhysicalDeviceProperties(device, &properties);
      std::cout << "[interop] device name=\"" << properties.deviceName
                << "\" vendor=0x" << std::hex << properties.vendorID << std::dec << '\n';
      if (selected == VK_NULL_HANDLE && properties.vendorID == 0x10DE) {
        selected = device;
        selected_properties = properties;
        selected_extensions = EnumerateDeviceExtensions(device);
        VkPhysicalDeviceProperties2 properties2 = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties2.pNext = &selected_id;
        vkGetPhysicalDeviceProperties2(device, &properties2);
      }
    }

    if (selected == VK_NULL_HANDLE) {
      std::cerr << "[interop] NVIDIA Vulkan device not found\n";
      vkDestroyInstance(instance, nullptr);
      return 3;
    }

    std::cout << "[interop] selected=\"" << selected_properties.deviceName
              << "\" driver=" << selected_properties.driverVersion
              << " luid=" << HexLuid(selected_id.deviceLUID, VK_LUID_SIZE) << '\n';

    const std::array<const char *, 4> required_extensions = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
    };
    bool required_extensions_present = true;
    for (const char *extension : required_extensions) {
      const bool present = selected_extensions.contains(extension);
      std::cout << "[interop] extension " << extension << '=' << (present ? "yes" : "no")
                << '\n';
      required_extensions_present &= present;
    }

    PrintImageQuery(selected,
                    VK_FORMAT_R16G16B16A16_SFLOAT,
                    "R16G16B16A16_SFLOAT",
                    VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT);
    PrintImageQuery(selected,
                    VK_FORMAT_R16G16B16A16_SFLOAT,
                    "R16G16B16A16_SFLOAT",
                    VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT);
    PrintImageQuery(selected,
                    VK_FORMAT_R16G16B16A16_SFLOAT,
                    "R16G16B16A16_SFLOAT",
                    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT);
    PrintImageQuery(
        selected, VK_FORMAT_R32_SFLOAT, "R32_SFLOAT", VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT);
    PrintImageQuery(selected,
                    VK_FORMAT_R16G16_SFLOAT,
                    "R16G16_SFLOAT",
                    VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT);

    PrintSemaphoreQuery(selected, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT);
    PrintSemaphoreQuery(selected, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT);

    ComPtr<IDXGIFactory6> factory;
    const HRESULT dxgi_result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    bool matching_d3d12_adapter = false;
    if (SUCCEEDED(dxgi_result)) {
      for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapterByGpuPreference(
                index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) ==
            DXGI_ERROR_NOT_FOUND)
        {
          break;
        }
        DXGI_ADAPTER_DESC1 description = {};
        adapter->GetDesc1(&description);
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
          continue;
        }
        const bool same_luid = SameLuid(selected_id.deviceLUID, description.AdapterLuid);
        std::wcout << L"[interop] dxgi adapter=\"" << description.Description
                   << L"\" same_luid=" << (same_luid ? L"yes" : L"no") << L'\n';
        if (same_luid) {
          ComPtr<ID3D12Device> d3d12_device;
          const HRESULT d3d12_result = D3D12CreateDevice(
              adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d3d12_device));
          matching_d3d12_adapter = SUCCEEDED(d3d12_result);
          std::cout << "[interop] matching D3D12 device="
                    << (matching_d3d12_adapter ? "yes" : "no") << '\n';
        }
      }
    }

    const bool baseline_ready = required_extensions_present && matching_d3d12_adapter;
    std::cout << "[interop] required_extensions=" << (required_extensions_present ? "yes" : "no")
              << " matching_d3d12_adapter=" << (matching_d3d12_adapter ? "yes" : "no") << '\n';
    std::cout << "DLSS5_INTEROP_PROBE: PASS\n";
    std::cout << "DLSS5_INTEROP_BASELINE_READY: " << (baseline_ready ? "YES" : "NO") << '\n';

    vkDestroyInstance(instance, nullptr);
    return 0;
  }
  catch (const std::exception &error) {
    std::cerr << "[interop] error: " << error.what() << '\n';
    return 4;
  }
}
