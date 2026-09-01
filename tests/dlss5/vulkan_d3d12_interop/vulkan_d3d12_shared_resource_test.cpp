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

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 64;
constexpr uint64_t kSignalValue = 1;

void CheckHr(HRESULT result, const char *what)
{
  if (FAILED(result)) {
    throw std::runtime_error(std::string(what) + " failed, HRESULT=0x" +
                             [&] {
                               char buffer[16];
                               sprintf_s(buffer, "%08lx", static_cast<unsigned long>(result));
                               return std::string(buffer);
                             }());
  }
}

void CheckVk(VkResult result, const char *what)
{
  if (result != VK_SUCCESS) {
    throw std::runtime_error(std::string(what) + " failed, VkResult=" +
                             std::to_string(static_cast<int>(result)));
  }
}

uint16_t FloatToHalf(float value)
{
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint32_t sign = (bits >> 16) & 0x8000u;
  int exponent = int((bits >> 23) & 0xffu) - 127 + 15;
  uint32_t mantissa = bits & 0x7fffffu;
  if (exponent <= 0) {
    if (exponent < -10) {
      return static_cast<uint16_t>(sign);
    }
    mantissa = (mantissa | 0x800000u) >> (1 - exponent);
    return static_cast<uint16_t>(sign | ((mantissa + 0x1000u) >> 13));
  }
  if (exponent >= 31) {
    return static_cast<uint16_t>(sign | 0x7c00u);
  }
  return static_cast<uint16_t>(sign | (uint32_t(exponent) << 10) |
                               ((mantissa + 0x1000u) >> 13));
}

float HalfToFloat(uint16_t value)
{
  const uint32_t sign = uint32_t(value & 0x8000u) << 16;
  uint32_t exponent = (value >> 10) & 0x1fu;
  uint32_t mantissa = value & 0x3ffu;
  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    }
    else {
      exponent = 1;
      while ((mantissa & 0x400u) == 0) {
        mantissa <<= 1;
        --exponent;
      }
      mantissa &= 0x3ffu;
      bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    }
  }
  else if (exponent == 31) {
    bits = sign | 0x7f800000u | (mantissa << 13);
  }
  else {
    bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
  }
  float result;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

bool SameLuid(const uint8_t *vulkan_luid, const LUID &dxgi_luid)
{
  return std::memcmp(vulkan_luid, &dxgi_luid, sizeof(LUID)) == 0;
}

uint32_t FindMemoryType(VkPhysicalDevice physical_device,
                        uint32_t memory_type_bits,
                        VkMemoryPropertyFlags required)
{
  VkPhysicalDeviceMemoryProperties properties = {};
  vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
  for (uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
    if ((memory_type_bits & (1u << index)) != 0 &&
        (properties.memoryTypes[index].propertyFlags & required) == required)
    {
      return index;
    }
  }
  throw std::runtime_error("No compatible Vulkan memory type");
}

struct D3D12State {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> shared_fence;
  ComPtr<ID3D12Fence> completion_fence;
  ComPtr<ID3D12Resource> resource;
  HANDLE completion_event = nullptr;
  HANDLE resource_handle = nullptr;
  uint64_t completion_value = 0;

  ~D3D12State()
  {
    if (completion_event) {
      CloseHandle(completion_event);
    }
    if (resource_handle) {
      CloseHandle(resource_handle);
    }
  }

  void begin()
  {
    CheckHr(allocator->Reset(), "Reset D3D12 allocator");
    CheckHr(list->Reset(allocator.Get(), nullptr), "Reset D3D12 command list");
  }

  void submit_and_wait()
  {
    CheckHr(list->Close(), "Close D3D12 command list");
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ++completion_value;
    CheckHr(queue->Signal(completion_fence.Get(), completion_value),
            "Signal D3D12 completion fence");
    if (completion_fence->GetCompletedValue() < completion_value) {
      CheckHr(completion_fence->SetEventOnCompletion(completion_value, completion_event),
              "Set D3D12 completion event");
      WaitForSingleObject(completion_event, INFINITE);
    }
  }
};

struct VulkanState {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t queue_family = 0;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkSemaphore timeline = VK_NULL_HANDLE;

  ~VulkanState()
  {
    if (device != VK_NULL_HANDLE) {
      vkDeviceWaitIdle(device);
    }
    if (timeline != VK_NULL_HANDLE) {
      vkDestroySemaphore(device, timeline, nullptr);
    }
    if (image != VK_NULL_HANDLE) {
      vkDestroyImage(device, image, nullptr);
    }
    if (memory != VK_NULL_HANDLE) {
      vkFreeMemory(device, memory, nullptr);
    }
    if (command_pool != VK_NULL_HANDLE) {
      vkDestroyCommandPool(device, command_pool, nullptr);
    }
    if (device != VK_NULL_HANDLE) {
      vkDestroyDevice(device, nullptr);
    }
    if (instance != VK_NULL_HANDLE) {
      vkDestroyInstance(instance, nullptr);
    }
  }
};

void CreateD3D12Objects(D3D12State &state, const uint8_t *vulkan_luid)
{
  ComPtr<IDXGIFactory6> factory;
  CheckHr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
  ComPtr<IDXGIAdapter1> selected_adapter;
  for (UINT index = 0;; ++index) {
    ComPtr<IDXGIAdapter1> adapter;
    const HRESULT result = factory->EnumAdapterByGpuPreference(
        index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    if (result == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    CheckHr(result, "EnumAdapterByGpuPreference");
    DXGI_ADAPTER_DESC1 description = {};
    CheckHr(adapter->GetDesc1(&description), "GetAdapterDesc1");
    if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
        SameLuid(vulkan_luid, description.AdapterLuid))
    {
      selected_adapter = adapter;
      break;
    }
  }
  if (!selected_adapter) {
    throw std::runtime_error("Matching DXGI adapter was not found");
  }

  CheckHr(D3D12CreateDevice(
              selected_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&state.device)),
          "D3D12CreateDevice");

  D3D12_COMMAND_QUEUE_DESC queue_desc = {};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  CheckHr(state.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&state.queue)),
          "CreateCommandQueue");
  CheckHr(state.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&state.allocator)),
          "CreateCommandAllocator");
  CheckHr(state.device->CreateCommandList(0,
                                           D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           state.allocator.Get(),
                                           nullptr,
                                           IID_PPV_ARGS(&state.list)),
          "CreateCommandList");
  CheckHr(state.list->Close(), "Close initial D3D12 command list");
  CheckHr(state.device->CreateFence(0,
                                    D3D12_FENCE_FLAG_SHARED,
                                    IID_PPV_ARGS(&state.shared_fence)),
          "CreateSharedFence");
  CheckHr(state.device->CreateFence(
              0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state.completion_fence)),
          "CreateCompletionFence");
  state.completion_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!state.completion_event) {
    throw std::runtime_error("CreateEventW failed");
  }

  D3D12_RESOURCE_DESC resource_desc = {};
  resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  resource_desc.Width = kWidth;
  resource_desc.Height = kHeight;
  resource_desc.DepthOrArraySize = 1;
  resource_desc.MipLevels = 1;
  resource_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  resource_desc.SampleDesc.Count = 1;
  resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  D3D12_HEAP_PROPERTIES heap_properties = {};
  heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  CheckHr(state.device->CreateCommittedResource(&heap_properties,
                                                D3D12_HEAP_FLAG_SHARED,
                                                &resource_desc,
                                                D3D12_HEAP_TYPE_DEFAULT == heap_properties.Type ?
                                                    D3D12_RESOURCE_STATE_COMMON :
                                                    D3D12_RESOURCE_STATE_COMMON,
                                                nullptr,
                                                IID_PPV_ARGS(&state.resource)),
          "Create shared D3D12 resource");
  CheckHr(state.device->CreateSharedHandle(
              state.resource.Get(), nullptr, GENERIC_ALL, nullptr, &state.resource_handle),
          "Create shared D3D12 resource handle");
}

void CreateVulkanObjects(VulkanState &state,
                         D3D12State &d3d12,
                         const VkPhysicalDeviceIDProperties &required_id)
{
  uint32_t instance_version = VK_API_VERSION_1_0;
  if (vkEnumerateInstanceVersion != nullptr) {
    CheckVk(vkEnumerateInstanceVersion(&instance_version), "vkEnumerateInstanceVersion");
  }

  VkApplicationInfo application_info = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  application_info.pApplicationName = "DLSS5 NPR Vulkan-D3D12 Shared Resource Test";
  application_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  application_info.pEngineName = "Blender NPR";
  application_info.engineVersion = VK_MAKE_VERSION(5, 2, 0);
  application_info.apiVersion = std::min(instance_version, VK_API_VERSION_1_3);

  VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instance_info.pApplicationInfo = &application_info;
  CheckVk(vkCreateInstance(&instance_info, nullptr, &state.instance), "vkCreateInstance");

  uint32_t device_count = 0;
  CheckVk(vkEnumeratePhysicalDevices(state.instance, &device_count, nullptr),
          "vkEnumeratePhysicalDevices count");
  std::vector<VkPhysicalDevice> devices(device_count);
  CheckVk(vkEnumeratePhysicalDevices(state.instance, &device_count, devices.data()),
          "vkEnumeratePhysicalDevices");

  VkPhysicalDeviceIDProperties selected_id = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
  for (VkPhysicalDevice device : devices) {
    VkPhysicalDeviceProperties2 properties = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    VkPhysicalDeviceIDProperties id = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    properties.pNext = &id;
    vkGetPhysicalDeviceProperties2(device, &properties);
    if (properties.properties.vendorID == 0x10DE &&
        std::memcmp(id.deviceLUID, required_id.deviceLUID, VK_LUID_SIZE) == 0)
    {
      state.physical_device = device;
      selected_id = id;
      std::cout << "[shared] Vulkan device=\"" << properties.properties.deviceName << "\"\n";
      break;
    }
  }
  if (state.physical_device == VK_NULL_HANDLE) {
    throw std::runtime_error("Matching NVIDIA Vulkan device was not found");
  }

  uint32_t family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(state.physical_device, &family_count, nullptr);
  std::vector<VkQueueFamilyProperties> families(family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(state.physical_device, &family_count, families.data());
  for (uint32_t index = 0; index < family_count; ++index) {
    if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
      state.queue_family = index;
      break;
    }
  }

  const float queue_priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue_info.queueFamilyIndex = state.queue_family;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &queue_priority;

  const std::array<const char *, 4> extensions = {
      VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
      VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
  };
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
  timeline.timelineSemaphore = VK_TRUE;
  VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  device_info.pNext = &timeline;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  device_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  device_info.ppEnabledExtensionNames = extensions.data();
  CheckVk(vkCreateDevice(state.physical_device, &device_info, nullptr, &state.device),
          "vkCreateDevice");
  vkGetDeviceQueue(state.device, state.queue_family, 0, &state.queue);

  VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = state.queue_family;
  CheckVk(vkCreateCommandPool(state.device, &pool_info, nullptr, &state.command_pool),
          "vkCreateCommandPool");

  VkExternalMemoryImageCreateInfo external_memory = {
      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  external_memory.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
  VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.pNext = &external_memory;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  image_info.extent = {kWidth, kHeight, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  CheckVk(vkCreateImage(state.device, &image_info, nullptr, &state.image), "vkCreateImage");

  VkMemoryRequirements memory_requirements = {};
  vkGetImageMemoryRequirements(state.device, state.image, &memory_requirements);

  VkImportMemoryWin32HandleInfoKHR import_info = {
      VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
  import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
  import_info.handle = d3d12.resource_handle;
  VkMemoryDedicatedAllocateInfo dedicated_info = {
      VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dedicated_info.image = state.image;
  import_info.pNext = &dedicated_info;

  VkMemoryAllocateInfo allocate_info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate_info.pNext = &import_info;
  allocate_info.allocationSize = memory_requirements.size;
  allocate_info.memoryTypeIndex = FindMemoryType(
      state.physical_device, memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  CheckVk(vkAllocateMemory(state.device, &allocate_info, nullptr, &state.memory),
          "vkAllocateMemory imported D3D12 resource");
  d3d12.resource_handle = nullptr;
  CheckVk(vkBindImageMemory(state.device, state.image, state.memory, 0), "vkBindImageMemory");

  VkSemaphoreTypeCreateInfo semaphore_type = {
      VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
  semaphore_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  semaphore_type.initialValue = 0;
  VkSemaphoreCreateInfo semaphore_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  semaphore_info.pNext = &semaphore_type;
  CheckVk(vkCreateSemaphore(state.device, &semaphore_info, nullptr, &state.timeline),
          "vkCreateSemaphore");

  HANDLE fence_handle = nullptr;
  CheckHr(d3d12.device->CreateSharedHandle(
              d3d12.shared_fence.Get(), nullptr, GENERIC_ALL, nullptr, &fence_handle),
          "Create shared D3D12 fence handle");
  VkImportSemaphoreWin32HandleInfoKHR import_semaphore = {
      VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
  import_semaphore.semaphore = state.timeline;
  import_semaphore.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
  import_semaphore.handle = fence_handle;
  const auto import_semaphore_fn =
      reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
          vkGetDeviceProcAddr(state.device, "vkImportSemaphoreWin32HandleKHR"));
  if (import_semaphore_fn == nullptr) {
    CloseHandle(fence_handle);
    throw std::runtime_error("vkImportSemaphoreWin32HandleKHR is unavailable");
  }
  CheckVk(import_semaphore_fn(state.device, &import_semaphore),
          "vkImportSemaphoreWin32HandleKHR");
}

void RunVulkanClear(VulkanState &state)
{
  VkCommandBufferAllocateInfo allocate_info = {
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocate_info.commandPool = state.command_pool;
  allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate_info.commandBufferCount = 1;
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  CheckVk(vkAllocateCommandBuffers(state.device, &allocate_info, &command_buffer),
          "vkAllocateCommandBuffers");

  VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  CheckVk(vkBeginCommandBuffer(command_buffer, &begin_info), "vkBeginCommandBuffer");

  VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = 0;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = state.image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(command_buffer,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                       0,
                       0,
                       nullptr,
                       0,
                       nullptr,
                       1,
                       &barrier);

  VkClearColorValue clear_value = {};
  clear_value.float32[0] = 0.125f;
  clear_value.float32[1] = 0.25f;
  clear_value.float32[2] = 0.5f;
  clear_value.float32[3] = 1.0f;
  VkImageSubresourceRange range = {};
  range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  range.levelCount = 1;
  range.layerCount = 1;
  vkCmdClearColorImage(
      command_buffer, state.image, VK_IMAGE_LAYOUT_GENERAL, &clear_value, 1, &range);
  CheckVk(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer");

  const uint64_t signal_value = kSignalValue;
  VkTimelineSemaphoreSubmitInfo timeline_info = {
      VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
  timeline_info.signalSemaphoreValueCount = 1;
  timeline_info.pSignalSemaphoreValues = &signal_value;
  VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit_info.pNext = &timeline_info;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &command_buffer;
  submit_info.signalSemaphoreCount = 1;
  submit_info.pSignalSemaphores = &state.timeline;
  CheckVk(vkQueueSubmit(state.queue, 1, &submit_info, VK_NULL_HANDLE), "vkQueueSubmit");
}

std::array<float, 4> ReadD3D12Resource(D3D12State &state)
{
  const D3D12_RESOURCE_DESC resource_desc = state.resource->GetDesc();
  UINT64 readback_size = 0;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  UINT rows = 0;
  UINT64 row_size = 0;
  state.device->GetCopyableFootprints(
      &resource_desc, 0, 1, 0, &footprint, &rows, &row_size, &readback_size);

  D3D12_HEAP_PROPERTIES readback_heap = {};
  readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC buffer_desc = {};
  buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer_desc.Width = readback_size;
  buffer_desc.Height = 1;
  buffer_desc.DepthOrArraySize = 1;
  buffer_desc.MipLevels = 1;
  buffer_desc.SampleDesc.Count = 1;
  buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> readback;
  CheckHr(state.device->CreateCommittedResource(&readback_heap,
                                                 D3D12_HEAP_FLAG_NONE,
                                                 &buffer_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr,
                                                 IID_PPV_ARGS(&readback)),
          "Create D3D12 readback buffer");

  state.begin();
  CheckHr(state.queue->Wait(state.shared_fence.Get(), kSignalValue),
          "D3D12 queue wait on imported Vulkan signal");

  D3D12_RESOURCE_BARRIER to_copy = {};
  to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  to_copy.Transition.pResource = state.resource.Get();
  to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
  to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  state.list->ResourceBarrier(1, &to_copy);

  D3D12_TEXTURE_COPY_LOCATION source = {};
  source.pResource = state.resource.Get();
  source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  source.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION target = {};
  target.pResource = readback.Get();
  target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  target.PlacedFootprint = footprint;
  state.list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);

  D3D12_RESOURCE_BARRIER to_common = to_copy;
  to_common.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  to_common.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
  state.list->ResourceBarrier(1, &to_common);
  state.submit_and_wait();

  void *mapped = nullptr;
  D3D12_RANGE range = {0, readback_size};
  CheckHr(readback->Map(0, &range, &mapped), "Map D3D12 readback buffer");
  const auto *bytes = static_cast<const uint8_t *>(mapped) + footprint.Offset;
  const auto *half = reinterpret_cast<const uint16_t *>(bytes);
  std::array<float, 4> result = {
      HalfToFloat(half[0]), HalfToFloat(half[1]), HalfToFloat(half[2]), HalfToFloat(half[3])};
  readback->Unmap(0, nullptr);
  return result;
}

}  // namespace

int main()
{
  try {
    VulkanState vulkan;
    uint32_t instance_version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion != nullptr) {
      CheckVk(vkEnumerateInstanceVersion(&instance_version), "vkEnumerateInstanceVersion");
    }

    VkApplicationInfo application_info = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application_info.pApplicationName = "DLSS5 NPR Vulkan-D3D12 Shared Resource Test";
    application_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    application_info.pEngineName = "Blender NPR";
    application_info.engineVersion = VK_MAKE_VERSION(5, 2, 0);
    application_info.apiVersion = std::min(instance_version, VK_API_VERSION_1_3);
    VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &application_info;
    CheckVk(vkCreateInstance(&instance_info, nullptr, &vulkan.instance), "vkCreateInstance");

    uint32_t device_count = 0;
    CheckVk(vkEnumeratePhysicalDevices(vulkan.instance, &device_count, nullptr),
            "vkEnumeratePhysicalDevices count");
    std::vector<VkPhysicalDevice> devices(device_count);
    CheckVk(vkEnumeratePhysicalDevices(vulkan.instance, &device_count, devices.data()),
            "vkEnumeratePhysicalDevices");

    VkPhysicalDeviceIDProperties selected_id = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    for (VkPhysicalDevice device : devices) {
      VkPhysicalDeviceProperties2 properties = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      VkPhysicalDeviceIDProperties id = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
      properties.pNext = &id;
      vkGetPhysicalDeviceProperties2(device, &properties);
      if (properties.properties.vendorID == 0x10DE) {
        vulkan.physical_device = device;
        selected_id = id;
        std::cout << "[shared] selected Vulkan device=\"" << properties.properties.deviceName
                  << "\"\n";
        break;
      }
    }
    if (vulkan.physical_device == VK_NULL_HANDLE) {
      throw std::runtime_error("NVIDIA Vulkan device not found");
    }

    D3D12State d3d12;
    CreateD3D12Objects(d3d12, selected_id.deviceLUID);
    CreateVulkanObjects(vulkan, d3d12, selected_id);
    RunVulkanClear(vulkan);
    const std::array<float, 4> result = ReadD3D12Resource(d3d12);
    std::cout << "[shared] readback="
              << result[0] << ',' << result[1] << ',' << result[2] << ',' << result[3] << '\n';

    const std::array<float, 4> expected = {0.125f, 0.25f, 0.5f, 1.0f};
    bool matches = true;
    for (int channel = 0; channel < 4; ++channel) {
      matches &= std::abs(result[channel] - expected[channel]) < 0.01f;
    }
    std::cout << "DLSS5_VULKAN_D3D12_SHARED_RESOURCE: " << (matches ? "PASS" : "FAIL")
              << '\n';
    return matches ? 0 : 5;
  }
  catch (const std::exception &error) {
    std::cerr << "[shared] error: " << error.what() << '\n';
    return 2;
  }
}
