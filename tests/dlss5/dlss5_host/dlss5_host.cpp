#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "nvsdk_ngx.h"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

constexpr unsigned long long kApplicationId = 0x0876232Cull;
constexpr NVSDK_NGX_Version kSdkVersion = static_cast<NVSDK_NGX_Version>(0x15);
constexpr NVSDK_NGX_Feature kFeatureId = static_cast<NVSDK_NGX_Feature>(18);

void Check(HRESULT hr, const char *what)
{
  if (FAILED(hr)) {
    throw std::runtime_error(std::string(what) + " failed, HRESULT=0x" +
                             [&] {
                               char buf[16];
                               sprintf_s(buf, "%08lx", static_cast<unsigned long>(hr));
                               return std::string(buf);
                             }());
  }
}

uint16_t FloatToHalf(float value)
{
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
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
  memcpy(&result, &bits, sizeof(result));
  return result;
}

struct Image {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgb;
  std::vector<float> rgba;
};

Image ReadBmp(const fs::path &path)
{
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("Cannot open input BMP: " + path.string());
  }
  BITMAPFILEHEADER file_header{};
  BITMAPINFOHEADER info_header{};
  file.read(reinterpret_cast<char *>(&file_header), sizeof(file_header));
  file.read(reinterpret_cast<char *>(&info_header), sizeof(info_header));
  if (!file || file_header.bfType != 0x4d42 || info_header.biSize < sizeof(BITMAPINFOHEADER) ||
      info_header.biCompression != BI_RGB || (info_header.biBitCount != 24 && info_header.biBitCount != 32) ||
      info_header.biWidth <= 0 || info_header.biHeight == 0) {
    throw std::runtime_error("Input must be an uncompressed 24/32-bit BMP");
  }
  Image image;
  image.width = static_cast<uint32_t>(info_header.biWidth);
  image.height = static_cast<uint32_t>(std::abs(info_header.biHeight));
  const uint32_t bytes_per_pixel = info_header.biBitCount / 8;
  const uint32_t row_stride = (image.width * bytes_per_pixel + 3u) & ~3u;
  image.rgb.resize(size_t(image.width) * image.height * 3);
  file.seekg(file_header.bfOffBits, std::ios::beg);
  std::vector<uint8_t> row(row_stride);
  const bool bottom_up = info_header.biHeight > 0;
  for (uint32_t y = 0; y < image.height; ++y) {
    file.read(reinterpret_cast<char *>(row.data()), row.size());
    if (!file) {
      throw std::runtime_error("Truncated BMP input");
    }
    const uint32_t dst_y = bottom_up ? image.height - 1 - y : y;
    for (uint32_t x = 0; x < image.width; ++x) {
      const uint8_t *src = row.data() + size_t(x) * bytes_per_pixel;
      uint8_t *dst = image.rgb.data() + (size_t(dst_y) * image.width + x) * 3;
      dst[0] = src[2];
      dst[1] = src[1];
      dst[2] = src[0];
    }
  }
  return image;
}

Image ReadRgba32f(const fs::path &path)
{
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("Cannot open input RGBA32F: " + path.string());
  }
  std::array<char, 8> magic{};
  uint32_t width = 0;
  uint32_t height = 0;
  file.read(magic.data(), magic.size());
  file.read(reinterpret_cast<char *>(&width), sizeof(width));
  file.read(reinterpret_cast<char *>(&height), sizeof(height));
  if (!file || std::string(magic.data(), magic.size()) != "DLSS5F32" || width == 0 ||
      height == 0) {
    throw std::runtime_error("Input is not a DLSS5F32 RGBA32F file");
  }
  Image image;
  image.width = width;
  image.height = height;
  image.rgba.resize(size_t(width) * height * 4);
  file.read(reinterpret_cast<char *>(image.rgba.data()),
            static_cast<std::streamsize>(image.rgba.size() * sizeof(float)));
  if (!file) {
    throw std::runtime_error("Truncated RGBA32F input");
  }
  return image;
}

std::vector<float> ReadPass32f(const fs::path &path,
                               const char (&expected_magic)[9],
                               size_t channels,
                               uint32_t &width,
                               uint32_t &height)
{
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("Cannot open input pass: " + path.string());
  }
  std::array<char, 8> magic{};
  file.read(magic.data(), magic.size());
  file.read(reinterpret_cast<char *>(&width), sizeof(width));
  file.read(reinterpret_cast<char *>(&height), sizeof(height));
  if (!file || std::string(magic.data(), magic.size()) !=
                   std::string(expected_magic, magic.size()) ||
      width == 0 || height == 0) {
    throw std::runtime_error("Input pass has an unexpected format: " + path.string());
  }
  std::vector<float> values(size_t(width) * height * channels);
  file.read(reinterpret_cast<char *>(values.data()),
            static_cast<std::streamsize>(values.size() * sizeof(float)));
  if (!file) {
    throw std::runtime_error("Truncated input pass: " + path.string());
  }
  return values;
}

void WriteFloatRgba(const fs::path &path,
                    uint32_t width,
                    uint32_t height,
                    const std::vector<float> &rgba)
{
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("Cannot create raw output: " + path.string());
  }
  file.write("DLSS5F32", 8);
  file.write(reinterpret_cast<const char *>(&width), sizeof(width));
  file.write(reinterpret_cast<const char *>(&height), sizeof(height));
  file.write(reinterpret_cast<const char *>(rgba.data()),
             static_cast<std::streamsize>(rgba.size() * sizeof(float)));
}

float LinearToSrgb(float value)
{
  value = std::max(value, 0.0f);
  return value <= 0.0031308f ? value * 12.92f :
                               1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

void WritePpm(const fs::path &path,
              uint32_t width,
              uint32_t height,
              const std::vector<float> &rgba,
              bool srgb_encode)
{
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("Cannot create output PPM: " + path.string());
  }
  file << "P6\n" << width << ' ' << height << "\n255\n";
  std::vector<uint8_t> rgb(size_t(width) * height * 3);
  for (size_t i = 0, p = 0; i < size_t(width) * height; ++i) {
    for (int c = 0; c < 3; ++c) {
      const float encoded = srgb_encode ? LinearToSrgb(rgba[i * 4 + c]) :
                                          rgba[i * 4 + c];
      const float value = std::clamp(encoded, 0.0f, 1.0f);
      rgb[p++] = static_cast<uint8_t>(value * 255.0f + 0.5f);
    }
  }
  file.write(reinterpret_cast<const char *>(rgb.data()), rgb.size());
}

struct Texture {
  ComPtr<ID3D12Resource> resource;
  uint32_t width = 0;
  uint32_t height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

struct FrameData {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint16_t> color;
  std::vector<float> depth;
  std::vector<uint16_t> motion;
};

D3D12_RESOURCE_DESC TextureDesc(uint32_t width, uint32_t height, DXGI_FORMAT format,
                                D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = flags;
  return desc;
}

class D3D12Context {
 public:
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12InfoQueue> info_queue;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  HANDLE fence_event = nullptr;
  uint64_t fence_value = 0;
  std::vector<ComPtr<ID3D12Resource>> pending_uploads;

  D3D12Context()
  {
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
      debug->EnableDebugLayer();
    }
    ComPtr<IDXGIFactory6> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    for (UINT i = 0;; ++i) {
      ComPtr<IDXGIAdapter1> adapter;
      if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                              IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      DXGI_ADAPTER_DESC1 desc{};
      adapter->GetDesc1(&desc);
      if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
        continue;
      }
      if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                       IID_PPV_ARGS(&device)))) {
        std::wcout << L"[dlss5_host] adapter: " << desc.Description << L"\n";
        break;
      }
    }
    if (!device) {
      throw std::runtime_error("No D3D12 hardware device found");
    }
    device.As(&info_queue);

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&list)), "CreateCommandList");
    Check(list->Close(), "Close initial command list");
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event) {
      throw std::runtime_error("CreateEventW failed");
    }
  }

  ~D3D12Context()
  {
    if (queue && fence) {
      Wait();
    }
    if (fence_event) {
      CloseHandle(fence_event);
    }
  }

  void Begin()
  {
    Check(allocator->Reset(), "Reset command allocator");
    Check(list->Reset(allocator.Get(), nullptr), "Reset command list");
  }

  void ExecuteAndWait()
  {
    Check(list->Close(), "Close command list");
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ++fence_value;
    Check(queue->Signal(fence.Get(), fence_value), "Signal fence");
    Wait();
    pending_uploads.clear();
    DumpDebugMessages();
  }

  void DumpDebugMessages()
  {
    if (!info_queue) {
      return;
    }
    const UINT64 count = info_queue->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 i = 0; i < count; ++i) {
      SIZE_T size = 0;
      if (FAILED(info_queue->GetMessage(i, nullptr, &size))) {
        continue;
      }
      std::vector<uint8_t> storage(size);
      auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
      if (SUCCEEDED(info_queue->GetMessage(i, message, &size)) && message->pDescription) {
        std::cerr << "[d3d12] " << message->pDescription << '\n';
      }
    }
    info_queue->ClearStoredMessages();
  }

  void Wait()
  {
    if (fence->GetCompletedValue() < fence_value) {
      Check(fence->SetEventOnCompletion(fence_value, fence_event), "SetEventOnCompletion");
      WaitForSingleObject(fence_event, INFINITE);
    }
  }

  Texture CreateTexture(uint32_t width, uint32_t height, DXGI_FORMAT format,
                        D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags = {})
  {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    Texture texture{nullptr, width, height, format, state};
    const D3D12_RESOURCE_DESC desc = TextureDesc(width, height, format, flags);
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                                          &desc, state,
                                          nullptr, IID_PPV_ARGS(&texture.resource)),
          "CreateCommittedResource texture");
    return texture;
  }

  void Transition(Texture &texture,
                  D3D12_RESOURCE_STATES state_before,
                  D3D12_RESOURCE_STATES state_after)
  {
    if (state_before == state_after) {
      texture.state = state_after;
      return;
    }
    if (texture.state != state_before) {
      throw std::runtime_error("Unexpected D3D12 texture state before transition");
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.resource.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = state_before;
    barrier.Transition.StateAfter = state_after;
    list->ResourceBarrier(1, &barrier);
    texture.state = state_after;
  }

  void UploadTexture(Texture &texture, const void *data, uint32_t bytes_per_pixel)
  {
    if (texture.state != D3D12_RESOURCE_STATE_COPY_DEST) {
      throw std::runtime_error("UploadTexture requires COPY_DEST state");
    }
    const D3D12_RESOURCE_DESC desc = texture.resource->GetDesc();
    UINT64 upload_size = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_size = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_size, &upload_size);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = upload_size;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          IID_PPV_ARGS(&upload)),
          "CreateCommittedResource upload");
    void *mapped = nullptr;
    D3D12_RANGE range{0, 0};
    Check(upload->Map(0, &range, &mapped), "Map upload");
    const auto *src = static_cast<const uint8_t *>(data);
    auto *dst = static_cast<uint8_t *>(mapped) + footprint.Offset;
    const uint64_t src_pitch = uint64_t(texture.width) * bytes_per_pixel;
    for (UINT y = 0; y < rows; ++y) {
      memcpy(dst + uint64_t(y) * footprint.Footprint.RowPitch,
             src + uint64_t(y) * src_pitch, static_cast<size_t>(row_size));
    }
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = texture.resource.Get();
    target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    target.SubresourceIndex = 0;
    list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    pending_uploads.push_back(upload);
    Transition(texture,
               D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  void UploadFrame(Texture &color_texture,
                   Texture &depth_texture,
                   Texture &motion_texture,
                   const FrameData &frame)
  {
    if (color_texture.width != frame.width || color_texture.height != frame.height ||
        depth_texture.width != frame.width || depth_texture.height != frame.height ||
        motion_texture.width != frame.width || motion_texture.height != frame.height) {
      throw std::runtime_error("Frame dimensions do not match D3D12 input textures");
    }
    Transition(color_texture, color_texture.state, D3D12_RESOURCE_STATE_COPY_DEST);
    Transition(depth_texture, depth_texture.state, D3D12_RESOURCE_STATE_COPY_DEST);
    Transition(motion_texture, motion_texture.state, D3D12_RESOURCE_STATE_COPY_DEST);
    UploadTexture(color_texture, frame.color.data(), 8);
    UploadTexture(depth_texture, frame.depth.data(), 4);
    UploadTexture(motion_texture, frame.motion.data(), 4);
  }

  void CopyResource(Texture &destination, Texture &source)
  {
    if (destination.width != source.width || destination.height != source.height ||
        destination.format != source.format) {
      throw std::runtime_error("CopyResource requires matching texture dimensions and format");
    }
    Transition(source, source.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(destination, destination.state, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(destination.resource.Get(), source.resource.Get());
    Transition(source,
               D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  std::vector<float> ReadbackRgba16f(Texture &texture)
  {
    const D3D12_RESOURCE_DESC desc = texture.resource->GetDesc();
    UINT64 size = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_size = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_size, &size);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = size;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&readback)),
          "CreateCommittedResource readback");

    Transition(texture, texture.state, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = texture.resource.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = readback.Get();
    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint = footprint;
    list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    ExecuteAndWait();

    void *mapped = nullptr;
    D3D12_RANGE range{0, size};
    const HRESULT map_result = readback->Map(0, &range, &mapped);
    if (FAILED(map_result)) {
      const HRESULT removed_reason = device->GetDeviceRemovedReason();
      char map_buf[16];
      char removed_buf[16];
      sprintf_s(map_buf, "%08lx", static_cast<unsigned long>(map_result));
      sprintf_s(removed_buf, "%08lx", static_cast<unsigned long>(removed_reason));
      throw std::runtime_error(std::string("Map readback failed, HRESULT=0x") + map_buf +
                               ", device_removed_reason=0x" + removed_buf);
    }
    const auto *src = reinterpret_cast<const uint16_t *>(
        static_cast<const uint8_t *>(mapped) + footprint.Offset);
    std::vector<float> result(size_t(texture.width) * texture.height * 4);
    const size_t src_stride = footprint.Footprint.RowPitch / sizeof(uint16_t);
    for (uint32_t y = 0; y < texture.height; ++y) {
      for (uint32_t x = 0; x < texture.width; ++x) {
        const uint16_t *pixel = src + size_t(y) * src_stride + size_t(x) * 4;
        for (int c = 0; c < 4; ++c) {
          result[(size_t(y) * texture.width + x) * 4 + c] = HalfToFloat(pixel[c]);
        }
      }
    }
    readback->Unmap(0, nullptr);
    return result;
  }
};

using InitFn = NVSDK_NGX_Result(NVSDK_CONV *)(unsigned long long, const wchar_t *, ID3D12Device *,
                                                NVSDK_NGX_Version, const NVSDK_NGX_Parameter *);
using ShutdownFn = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D12Device *);
using CreateFn = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D12GraphicsCommandList *, NVSDK_NGX_Feature,
                                                const NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
using EvaluateFn = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D12GraphicsCommandList *,
                                                  const NVSDK_NGX_Handle *,
                                                  const NVSDK_NGX_Parameter *,
                                                  PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFn = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Handle *);
using GetModuleFileNameWFn = DWORD(WINAPI *)(HMODULE, LPWSTR, DWORD);

struct Runtime {
  HMODULE module = nullptr;
  InitFn init = nullptr;
  ShutdownFn shutdown = nullptr;
  CreateFn create = nullptr;
  EvaluateFn evaluate = nullptr;
  ReleaseFn release = nullptr;
  GetModuleFileNameWFn original_get_module_file_name = nullptr;
  void **iat_slot = nullptr;
};
Runtime runtime;
HMODULE snippet_module = nullptr;
GetModuleFileNameWFn original_get_module_file_name = nullptr;

void NVSDK_CONV NgxLogCallback(const char *message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
  if (message && *message) {
    std::cerr << "[ngx] " << message << '\n';
  }
}

DWORD WINAPI HookedGetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size)
{
  if (!original_get_module_file_name) {
    return 0;
  }
  if (snippet_module && module == snippet_module) {
    return original_get_module_file_name(module, filename, size);
  }
  HMODULE core = GetModuleHandleW(L"_nvngx.dll");
  if (!core) {
    core = GetModuleHandleW(L"nvngx.dll");
  }
  if (core) {
    return original_get_module_file_name(core, filename, size);
  }
  const wchar_t fake[] = L"nvngx.dll";
  const DWORD length = static_cast<DWORD>(std::size(fake) - 1);
  if (!filename || size == 0) {
    return 0;
  }
  if (length + 1 > size) {
    SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return size;
  }
  memcpy(filename, fake, sizeof(fake));
  return length;
}

bool WriteIat(void **slot, void *value)
{
  DWORD old_protect = 0;
  if (!slot || !VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old_protect)) {
    return false;
  }
  InterlockedExchangePointer(slot, value);
  DWORD ignored = 0;
  VirtualProtect(slot, sizeof(void *), old_protect, &ignored);
  FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
  return true;
}

bool HookSnippet()
{
  auto *base = reinterpret_cast<BYTE *>(snippet_module);
  auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    return false;
  }
  auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
  const auto &directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!directory.VirtualAddress) {
    return false;
  }
  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  const FARPROC wanted = kernel32 ? GetProcAddress(kernel32, "GetModuleFileNameW") : nullptr;
  auto *descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + directory.VirtualAddress);
  for (; descriptor->Name; ++descriptor) {
    const char *name = reinterpret_cast<const char *>(base + descriptor->Name);
    if (_stricmp(name, "KERNEL32.dll") != 0 && _stricmp(name, "KERNELBASE.dll") != 0) {
      continue;
    }
    auto *slots = reinterpret_cast<IMAGE_THUNK_DATA *>(base + descriptor->FirstThunk);
    auto *imports = descriptor->OriginalFirstThunk
                        ? reinterpret_cast<IMAGE_THUNK_DATA *>(base + descriptor->OriginalFirstThunk)
                        : nullptr;
    for (size_t i = 0; slots[i].u1.Function; ++i) {
      bool match = false;
      if (imports && imports[i].u1.AddressOfData &&
          !IMAGE_SNAP_BY_ORDINAL(imports[i].u1.Ordinal)) {
        auto *entry = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + imports[i].u1.AddressOfData);
        match = strcmp(reinterpret_cast<const char *>(entry->Name), "GetModuleFileNameW") == 0;
      }
      else if (wanted && slots[i].u1.Function == reinterpret_cast<ULONG_PTR>(wanted)) {
        match = true;
      }
      if (match) {
        runtime.iat_slot = reinterpret_cast<void **>(&slots[i].u1.Function);
        runtime.original_get_module_file_name =
            reinterpret_cast<GetModuleFileNameWFn>(slots[i].u1.Function);
        original_get_module_file_name = runtime.original_get_module_file_name;
        snippet_module = runtime.module;
        return WriteIat(runtime.iat_slot, reinterpret_cast<void *>(&HookedGetModuleFileNameW));
      }
    }
  }
  return false;
}

void UnhookSnippet()
{
  if (runtime.iat_slot && runtime.original_get_module_file_name) {
    WriteIat(runtime.iat_slot, reinterpret_cast<void *>(runtime.original_get_module_file_name));
  }
  runtime.iat_slot = nullptr;
  runtime.original_get_module_file_name = nullptr;
  original_get_module_file_name = nullptr;
  snippet_module = nullptr;
}

template<typename T> T Resolve(const char *name)
{
  auto result = reinterpret_cast<T>(GetProcAddress(runtime.module, name));
  if (!result) {
    throw std::runtime_error(std::string("Missing snippet export: ") + name);
  }
  return result;
}

void SetUll(NVSDK_NGX_Parameter *parameters, const char *name, unsigned long long value)
{
  using Fn = void(NVSDK_CONV *)(NVSDK_NGX_Parameter *, const char *, unsigned long long);
  reinterpret_cast<Fn>((*reinterpret_cast<void ***>(parameters))[0])(parameters, name, value);
}
void SetResource(NVSDK_NGX_Parameter *parameters, const char *name, ID3D12Resource *resource)
{
  using Fn = void(NVSDK_CONV *)(NVSDK_NGX_Parameter *, const char *, ID3D12Resource *);
  reinterpret_cast<Fn>((*reinterpret_cast<void ***>(parameters))[1])(parameters, name, resource);
}
void SetUi(NVSDK_NGX_Parameter *parameters, const char *name, unsigned int value)
{
  using Fn = void(NVSDK_CONV *)(NVSDK_NGX_Parameter *, const char *, unsigned int);
  reinterpret_cast<Fn>((*reinterpret_cast<void ***>(parameters))[3])(parameters, name, value);
}
void SetI(NVSDK_NGX_Parameter *parameters, const char *name, int value)
{
  using Fn = void(NVSDK_CONV *)(NVSDK_NGX_Parameter *, const char *, int);
  reinterpret_cast<Fn>((*reinterpret_cast<void ***>(parameters))[4])(parameters, name, value);
}
void SetF(NVSDK_NGX_Parameter *parameters, const char *name, float value)
{
  using Fn = void(NVSDK_CONV *)(NVSDK_NGX_Parameter *, const char *, float);
  reinterpret_cast<Fn>((*reinterpret_cast<void ***>(parameters))[6])(parameters, name, value);
}
NVSDK_NGX_Result GetUi(NVSDK_NGX_Parameter *parameters, const char *name, unsigned int *value)
{
  using Fn = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Parameter *, const char *, unsigned int *);
  return reinterpret_cast<Fn>((*reinterpret_cast<void ***>(parameters))[11])(parameters, name, value);
}
NVSDK_NGX_Result GetI(NVSDK_NGX_Parameter *parameters, const char *name, int *value)
{
  using Fn = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Parameter *, const char *, int *);
  return reinterpret_cast<Fn>((*reinterpret_cast<void ***>(parameters))[12])(parameters, name, value);
}

NVSDK_NGX_Result NVSDK_CONV ComputeScalingRatio(NVSDK_NGX_Parameter *parameters)
{
  unsigned int upscaling = 0;
  GetUi(parameters, "DLSSNR.Upscaling", &upscaling);
  int input_width = 0;
  int output_width = 0;
  GetI(parameters, "DLSSNR.InputWidth", &input_width);
  GetI(parameters, "DLSSNR.OutputWidth", &output_width);
  const float ratio = upscaling && input_width > 0 && output_width > 0 ?
                          float(input_width) / float(output_width) :
                          1.0f;
  SetF(parameters, "DLSSNR.ScalingRatio", ratio);
  return NVSDK_NGX_Result_Success;
}

void SetRect(NVSDK_NGX_Parameter *parameters, const char *prefix, uint32_t width, uint32_t height)
{
  const std::string name(prefix);
  SetI(parameters, (name + "SubrectBaseX").c_str(), 0);
  SetI(parameters, (name + "SubrectBaseY").c_str(), 0);
  SetI(parameters, (name + "SubrectWidth").c_str(), width);
  SetI(parameters, (name + "SubrectHeight").c_str(), height);
}

class DlssNrSession {
 public:
  D3D12Context context;
  NVSDK_NGX_Parameter *parameters = nullptr;
  NVSDK_NGX_Handle *handle = nullptr;
  bool ngx_initialized = false;
  HMODULE core_module = nullptr;

  void Init(const fs::path &runtime_dir)
  {
    const fs::path runtime_absolute = fs::absolute(runtime_dir);
    SetDllDirectoryW(runtime_absolute.c_str());
    const wchar_t *core_override = _wgetenv(L"DLSS5_CORE_DLL");
    const fs::path core_path = core_override ? fs::path(core_override) : runtime_absolute / L"nvngx.dll";
    core_module = LoadLibraryW(core_path.c_str());
    if (!core_module) {
      throw std::runtime_error("LoadLibraryW nvngx.dll failed, Win32=" +
                               std::to_string(GetLastError()));
    }
    const fs::path snippet_path = runtime_absolute / L"nvngx_dlssnr.dll";
    runtime.module = LoadLibraryExW(snippet_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!runtime.module) {
      throw std::runtime_error("LoadLibraryExW nvngx_dlssnr.dll failed, Win32=" +
                               std::to_string(GetLastError()));
    }
    snippet_module = runtime.module;
    runtime.init = Resolve<InitFn>("NVSDK_NGX_D3D12_Init_Ext");
    runtime.shutdown = Resolve<ShutdownFn>("NVSDK_NGX_D3D12_Shutdown1");
    runtime.create = Resolve<CreateFn>("NVSDK_NGX_D3D12_CreateFeature");
    runtime.evaluate = Resolve<EvaluateFn>("NVSDK_NGX_D3D12_EvaluateFeature");
    runtime.release = Resolve<ReleaseFn>("NVSDK_NGX_D3D12_ReleaseFeature");
    if (!HookSnippet()) {
      throw std::runtime_error("Could not install snippet GetModuleFileNameW IAT hook");
    }
    const fs::path data_dir = runtime_absolute / L"ngx-data";
    fs::create_directories(data_dir);
    const wchar_t *feature_paths[] = {runtime_absolute.c_str()};
    NVSDK_NGX_FeatureCommonInfo common_info{};
    common_info.PathListInfo.Path = feature_paths;
    common_info.PathListInfo.Length = 1;
    common_info.LoggingInfo.LoggingCallback = &NgxLogCallback;
    common_info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
    const NVSDK_NGX_Result ngx_result = NVSDK_NGX_D3D12_Init_with_ProjectID(
        "e40890d0-da76-467b-8130-12f3ada8d7c8", NVSDK_NGX_ENGINE_TYPE_UNITY, "6000.3",
        data_dir.c_str(), context.device.Get(), &common_info, kSdkVersion);
    if (NVSDK_NGX_FAILED(ngx_result)) {
      throw std::runtime_error("NVSDK_NGX_D3D12_Init_with_ProjectID failed, result=0x" +
                               [&] {
                                 char buf[16];
                                 sprintf_s(buf, "%08x", static_cast<unsigned>(ngx_result));
                                 return std::string(buf);
                               }());
    }
    ngx_initialized = true;
    const NVSDK_NGX_Result init_result = runtime.init(kApplicationId, data_dir.c_str(),
                                                       context.device.Get(), kSdkVersion, nullptr);
    if (NVSDK_NGX_FAILED(init_result)) {
      throw std::runtime_error("NVSDK_NGX_D3D12_Init_Ext failed, result=0x" +
                               [&] {
                                 char buf[16];
                                 sprintf_s(buf, "%08x", static_cast<unsigned>(init_result));
                                 return std::string(buf);
                               }());
    }
    NVSDK_NGX_Result params_result = NVSDK_NGX_D3D12_AllocateParameters(&parameters);
    if (NVSDK_NGX_FAILED(params_result) || !parameters) {
      params_result = NVSDK_NGX_D3D12_GetCapabilityParameters(&parameters);
    }
    if (NVSDK_NGX_FAILED(params_result) || !parameters) {
      throw std::runtime_error("NVSDK_NGX_D3D12_AllocateParameters failed, result=0x" +
                               [&] {
                                 char buf[16];
                                 sprintf_s(buf, "%08x", static_cast<unsigned>(params_result));
                                 return std::string(buf);
                               }());
    }
  }

  void CreateFeature(uint32_t input_width, uint32_t input_height,
                     uint32_t output_width, uint32_t output_height, bool upscale)
  {
    const float scaling_ratio = upscale ? float(input_width) / float(output_width) : 1.0f;
    SetI(parameters, "Width", input_width);
    SetI(parameters, "Height", input_height);
    SetI(parameters, "OutWidth", output_width);
    SetI(parameters, "OutHeight", output_height);
    SetI(parameters, "DLSSNR.Width", output_width);
    SetI(parameters, "DLSSNR.Height", output_height);
    SetI(parameters, "DLSSNR.InputWidth", input_width);
    SetI(parameters, "DLSSNR.InputHeight", input_height);
    SetI(parameters, "DLSSNR.OutputWidth", output_width);
    SetI(parameters, "DLSSNR.OutputHeight", output_height);
    SetI(parameters, "DLSSNR.Output.Width", output_width);
    SetI(parameters, "DLSSNR.Output.Height", output_height);
    SetI(parameters, "DLSSNR.Hint.Render.Preset", 0);
    SetI(parameters, "CreationNodeMask", 1);
    SetI(parameters, "VisibilityNodeMask", 1);
    SetUi(parameters, "DLSS.Output.Subrect.Base.X", 0);
    SetUi(parameters, "DLSS.Output.Subrect.Base.Y", 0);
    SetUi(parameters, "DLSSNR.Upscaling", upscale ? 1u : 0u);
    SetF(parameters, "DLSSNR.Scale", scaling_ratio);
    SetF(parameters, "DLSSNR.ScalingRatio", scaling_ratio);
    SetUll(parameters, "DLSSNRComputeScalingRatioCallback",
           reinterpret_cast<unsigned long long>(&ComputeScalingRatio));
    const NVSDK_NGX_Result result = runtime.create(context.list.Get(), kFeatureId, parameters, &handle);
    if (NVSDK_NGX_FAILED(result) || !handle) {
      throw std::runtime_error("NVSDK_NGX_D3D12_CreateFeature failed, result=0x" +
                               [&] {
                                 char buf[16];
                                 sprintf_s(buf, "%08x", static_cast<unsigned>(result));
                                 return std::string(buf);
                               }());
    }
  }

  void Evaluate(Texture &color, Texture &depth, Texture &motion, Texture &output,
                bool reset, uint32_t input_width, uint32_t input_height,
                uint32_t output_width, uint32_t output_height,
                float motion_scale_x, float motion_scale_y, bool depth_inverted)
  {
    SetResource(parameters, "DLSSNR.Color", color.resource.Get());
    SetResource(parameters, "DLSSNR.Output", output.resource.Get());
    SetResource(parameters, "DLSSNR.MVec", motion.resource.Get());
    SetResource(parameters, "DLSSNR.Depth", depth.resource.Get());
    SetI(parameters, "Width", input_width);
    SetI(parameters, "Height", input_height);
    SetI(parameters, "OutWidth", output_width);
    SetI(parameters, "OutHeight", output_height);
    SetRect(parameters, "DLSSNR.Color", input_width, input_height);
    SetRect(parameters, "DLSSNR.MVec", input_width, input_height);
    SetRect(parameters, "DLSSNR.Depth", input_width, input_height);
    SetRect(parameters, "DLSSNR.Output", output_width, output_height);
    SetF(parameters, "DLSSNR.MVecScaleX", motion_scale_x);
    SetF(parameters, "DLSSNR.MVecScaleY", motion_scale_y);
    SetUi(parameters, "DLSSNR.DepthInverted", depth_inverted ? 1u : 0u);
    SetUi(parameters, "DLSSNR.Enabled", 1);
    SetUi(parameters, "DLSSNR.Reset", reset ? 1u : 0u);
    SetF(parameters, "DLSSNR.Intensity", 1.0f);
    SetF(parameters, "DLSSNR.LocalToneStrength", 1.0f);
    SetF(parameters, "DLSSNR.LocalStructureStrength", 1.0f);
    SetF(parameters, "DLSSNR.SkinStructureStrength", -1.0f);
    SetUi(parameters, "DLSSNR.UseAutoMask", 0);
    SetI(parameters, "DLSSNR.Style", 0);
    SetUi(parameters, "DLSSNR.UICorrection", 0);
    const NVSDK_NGX_Result result = runtime.evaluate(context.list.Get(), handle, parameters, nullptr);
    if (NVSDK_NGX_FAILED(result)) {
      throw std::runtime_error("NVSDK_NGX_D3D12_EvaluateFeature failed, result=0x" +
                               [&] {
                                 char buf[16];
                                 sprintf_s(buf, "%08x", static_cast<unsigned>(result));
                                 return std::string(buf);
                               }());
    }
  }

  ~DlssNrSession()
  {
    if (context.queue) {
      context.Wait();
    }
    if (handle && runtime.release) {
      runtime.release(handle);
    }
    if (parameters) {
      NVSDK_NGX_D3D12_DestroyParameters(parameters);
    }
    if (runtime.shutdown && context.device) {
      runtime.shutdown(context.device.Get());
    }
    if (ngx_initialized && context.device) {
      NVSDK_NGX_D3D12_Shutdown1(context.device.Get());
    }
    UnhookSnippet();
    if (runtime.module) {
      FreeLibrary(runtime.module);
    }
    if (core_module) {
      FreeLibrary(core_module);
    }
    runtime = {};
  }
};

struct Options {
  fs::path input;
  fs::path depth_input;
  fs::path motion_input;
  fs::path second_input;
  fs::path second_depth_input;
  fs::path second_motion_input;
  fs::path output;
  fs::path raw_output;
  fs::path runtime_dir;
  uint32_t output_width = 0;
  uint32_t output_height = 0;
  bool input_f32 = false;
  bool second_input_f32 = false;
  bool second_reset = false;
  std::string output_encoding = "raw";
  float motion_scale_x = std::numeric_limits<float>::quiet_NaN();
  float motion_scale_y = std::numeric_limits<float>::quiet_NaN();
  bool depth_inverted = true;
  bool upscale = false;
  bool control_copy = false;
  bool control_upload = false;
  std::string color_encoding = "srgb";
};

Options ParseArgs(int argc, wchar_t **argv)
{
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg(argv[i]);
    auto next = [&]() -> fs::path {
      if (++i >= argc) {
        throw std::runtime_error("Missing value after " + std::string(arg.begin(), arg.end()));
      }
      return argv[i];
    };
    if (arg == L"--input") options.input = next();
    else if (arg == L"--depth-f32") options.depth_input = next();
    else if (arg == L"--motion-f32") options.motion_input = next();
    else if (arg == L"--second-input") options.second_input = next();
    else if (arg == L"--second-depth-f32") options.second_depth_input = next();
    else if (arg == L"--second-motion-f32") options.second_motion_input = next();
    else if (arg == L"--input-f32") options.input_f32 = true;
    else if (arg == L"--second-input-f32") options.second_input_f32 = true;
    else if (arg == L"--second-reset") options.second_reset = true;
    else if (arg == L"--output") options.output = next();
    else if (arg == L"--raw-output") options.raw_output = next();
    else if (arg == L"--runtime-dir") options.runtime_dir = next();
    else if (arg == L"--output-encoding") {
      options.output_encoding = next().string();
      if (options.output_encoding != "raw" && options.output_encoding != "srgb") {
        throw std::runtime_error("Output encoding must be raw or srgb");
      }
    }
    else if (arg == L"--motion-scale-x") {
      options.motion_scale_x = std::stof(next().string());
    }
    else if (arg == L"--motion-scale-y") {
      options.motion_scale_y = std::stof(next().string());
    }
    else if (arg == L"--depth-not-inverted") {
      options.depth_inverted = false;
    }
    else if (arg == L"--upscale") options.upscale = true;
    else if (arg == L"--output-width") {
      options.output_width = std::stoul(next().string());
    }
    else if (arg == L"--output-height") {
      options.output_height = std::stoul(next().string());
    }
    else if (arg == L"--control-copy") options.control_copy = true;
    else if (arg == L"--control-upload") options.control_upload = true;
    else if (arg == L"--color-encoding") {
      const fs::path value = next();
      options.color_encoding = value.string();
      if (options.color_encoding != "srgb" && options.color_encoding != "linear" &&
          options.color_encoding != "linear4") {
        throw std::runtime_error("Color encoding must be srgb, linear or linear4");
      }
    }
    else if (arg == L"--help") {
      std::wcout
          << L"dlss5_host --input low.bmp --output result.ppm --runtime-dir runtime "
             L"[--input-f32] [--depth-f32 depth.dlss5p32] [--motion-f32 motion.dlss5v32] "
             L"[--second-input frame2.bmp --second-depth-f32 depth2.dlss5p32 "
             L"--second-motion-f32 motion2.dlss5v32 --second-input-f32 --second-reset] "
             L"[--motion-scale-x X --motion-scale-y Y] [--depth-not-inverted] "
             L"[--output-encoding raw|srgb] "
             L"[--upscale] [--output-width W --output-height H] [--control-copy|--control-upload] "
             L"[--color-encoding srgb|linear|linear4] "
             L"[--raw-output result.rgba32f]\n";
      std::exit(0);
    }
    else throw std::runtime_error("Unknown argument");
  }
  if (options.input.empty() || options.output.empty() ||
      (!options.control_copy && !options.control_upload && options.runtime_dir.empty())) {
      throw std::runtime_error(options.control_copy || options.control_upload ?
                                 "Required: --input, --output" :
                                 "Required: --input, --output, --runtime-dir");
  }
  if (options.second_input.empty() &&
      (!options.second_depth_input.empty() || !options.second_motion_input.empty())) {
    throw std::runtime_error("Second depth/motion inputs require --second-input");
  }
  return options;
}

float SrgbToLinear(float value)
{
  return value <= 0.04045f ? value / 12.92f :
                             std::pow((value + 0.055f) / 1.055f, 2.4f);
}

void PrintChannelStats(const std::vector<float> &rgba)
{
  constexpr const char *kChannelNames[] = {"R", "G", "B", "A"};
  for (int channel = 0; channel < 4; ++channel) {
    float min_value = std::numeric_limits<float>::infinity();
    float max_value = -std::numeric_limits<float>::infinity();
    double sum = 0.0;
    size_t finite_count = 0;
    for (size_t i = channel; i < rgba.size(); i += 4) {
      const float value = rgba[i];
      if (!std::isfinite(value)) {
        continue;
      }
      min_value = std::min(min_value, value);
      max_value = std::max(max_value, value);
      sum += value;
      ++finite_count;
    }
    std::cout << "[dlss5_host] " << kChannelNames[channel] << " min=" << min_value
              << " max=" << max_value << " mean="
              << (finite_count ? sum / finite_count : std::numeric_limits<double>::quiet_NaN())
              << " finite=" << finite_count << "\n";
  }
}

size_t CountSentinelPixels(const std::vector<float> &rgba)
{
  size_t count = 0;
  for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
    if (std::abs(rgba[i + 0] - 1.0f) < 0.001f &&
        std::abs(rgba[i + 1] - 0.0f) < 0.001f &&
        std::abs(rgba[i + 2] - 1.0f) < 0.001f &&
        std::abs(rgba[i + 3] - 1.0f) < 0.001f) {
      ++count;
    }
  }
  return count;
}

FrameData LoadFrame(const fs::path &input,
                    bool input_f32,
                    const fs::path &depth_input,
                    const fs::path &motion_input,
                    const std::string &color_encoding)
{
  const Image image = input_f32 ? ReadRgba32f(input) : ReadBmp(input);
  if (image.width < 2 || image.height < 2) {
    throw std::runtime_error("Input resolution must be at least 2x2");
  }

  FrameData frame;
  frame.width = image.width;
  frame.height = image.height;
  frame.color.resize(size_t(image.width) * image.height * 4);
  frame.depth.assign(size_t(image.width) * image.height, 0.5f);
  frame.motion.assign(size_t(image.width) * image.height * 2, FloatToHalf(0.0f));

  if (!depth_input.empty()) {
    uint32_t depth_width = 0;
    uint32_t depth_height = 0;
    frame.depth = ReadPass32f(depth_input, "DLSS5P32", 1, depth_width, depth_height);
    if (depth_width != image.width || depth_height != image.height) {
      throw std::runtime_error("Depth pass dimensions do not match Color input");
    }
  }
  if (!motion_input.empty()) {
    uint32_t motion_width = 0;
    uint32_t motion_height = 0;
    const std::vector<float> motion_values =
        ReadPass32f(motion_input, "DLSS5V32", 2, motion_width, motion_height);
    if (motion_width != image.width || motion_height != image.height) {
      throw std::runtime_error("Motion pass dimensions do not match Color input");
    }
    for (size_t i = 0; i < size_t(image.width) * image.height; ++i) {
      frame.motion[i * 2 + 0] = FloatToHalf(motion_values[i * 2 + 0]);
      frame.motion[i * 2 + 1] = FloatToHalf(motion_values[i * 2 + 1]);
    }
  }

  for (size_t i = 0; i < size_t(image.width) * image.height; ++i) {
    for (int c = 0; c < 3; ++c) {
      float value;
      if (!image.rgba.empty()) {
        value = image.rgba[i * 4 + c];
      }
      else {
        value = float(image.rgb[i * 3 + c]) / 255.0f;
        if (color_encoding == "linear" || color_encoding == "linear4") {
          value = SrgbToLinear(value);
          if (color_encoding == "linear4") {
            value *= 4.0f;
          }
        }
      }
      frame.color[i * 4 + c] = FloatToHalf(value);
    }
    frame.color[i * 4 + 3] = FloatToHalf(image.rgba.empty() ? 1.0f : image.rgba[i * 4 + 3]);
  }
  return frame;
}

}  // namespace

int wmain(int argc, wchar_t **argv)
{
  try {
    const Options options = ParseArgs(argc, argv);
    const FrameData first_frame = LoadFrame(options.input,
                                            options.input_f32,
                                            options.depth_input,
                                            options.motion_input,
                                            options.color_encoding);
    const bool has_second_frame = !options.second_input.empty();
    FrameData second_frame;
    if (has_second_frame) {
      if (options.control_copy || options.control_upload) {
        throw std::runtime_error("Control modes do not support a second frame");
      }
      second_frame = LoadFrame(options.second_input,
                               options.second_input_f32,
                               options.second_depth_input,
                               options.second_motion_input,
                               options.color_encoding);
      if (second_frame.width != first_frame.width || second_frame.height != first_frame.height) {
        throw std::runtime_error("Second frame dimensions must match the first frame");
      }
    }
    if ((options.output_width == 0) != (options.output_height == 0)) {
      throw std::runtime_error("--output-width and --output-height must be provided together");
    }
    const uint32_t output_width = options.output_width ?
                                      options.output_width :
                                      (options.upscale ? first_frame.width * 2 : first_frame.width);
    const uint32_t output_height = options.output_height ?
                                       options.output_height :
                                       (options.upscale ? first_frame.height * 2 : first_frame.height);
    if (output_width < 2 || output_height < 2) {
      throw std::runtime_error("Output resolution must be at least 2x2");
    }
    const float default_motion_scale_x = options.motion_input.empty() ?
                                             -static_cast<float>(first_frame.width) :
                                             1.0f;
    const float default_motion_scale_y = options.motion_input.empty() ?
                                             -static_cast<float>(first_frame.height) :
                                             1.0f;

    DlssNrSession session;
    if (!options.control_copy && !options.control_upload) {
      session.Init(options.runtime_dir);
    }
    session.context.Begin();
    Texture color_texture = session.context.CreateTexture(
        first_frame.width, first_frame.height, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    Texture depth_texture = session.context.CreateTexture(
        first_frame.width, first_frame.height, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    Texture motion_texture = session.context.CreateTexture(
        first_frame.width, first_frame.height, DXGI_FORMAT_R16G16_FLOAT,
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    Texture output_texture = session.context.CreateTexture(
        output_width, output_height, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    session.context.UploadFrame(color_texture, depth_texture, motion_texture, first_frame);
    if (options.control_copy || options.control_upload) {
      if (options.upscale) {
        throw std::runtime_error("control mode requires matching input/output resolution");
      }
      if (options.control_upload) {
        session.context.UploadTexture(output_texture, first_frame.color.data(), 8);
      }
      else {
        session.context.CopyResource(output_texture, color_texture);
      }
    }
    else {
      std::vector<uint16_t> sentinel(size_t(output_width) * output_height * 4);
      for (size_t i = 0; i < size_t(output_width) * output_height; ++i) {
        sentinel[i * 4 + 0] = FloatToHalf(1.0f);
        sentinel[i * 4 + 1] = FloatToHalf(0.0f);
        sentinel[i * 4 + 2] = FloatToHalf(1.0f);
        sentinel[i * 4 + 3] = FloatToHalf(1.0f);
      }
      session.context.UploadTexture(output_texture, sentinel.data(), 8);
      session.context.Transition(output_texture,
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      session.CreateFeature(
          first_frame.width, first_frame.height, output_width, output_height, options.upscale);
      session.Evaluate(
          color_texture,
          depth_texture,
          motion_texture,
          output_texture,
          true,
          first_frame.width,
          first_frame.height,
          output_width,
          output_height,
          std::isfinite(options.motion_scale_x) ? options.motion_scale_x :
                                                   default_motion_scale_x,
          std::isfinite(options.motion_scale_y) ? options.motion_scale_y :
                                                   default_motion_scale_y,
          options.depth_inverted);
    }
    session.context.ExecuteAndWait();
    if (has_second_frame) {
      session.context.Begin();
      session.context.UploadFrame(color_texture, depth_texture, motion_texture, second_frame);
      session.Evaluate(
          color_texture,
          depth_texture,
          motion_texture,
          output_texture,
          options.second_reset,
          second_frame.width,
          second_frame.height,
          output_width,
          output_height,
          std::isfinite(options.motion_scale_x) ? options.motion_scale_x :
                                                   (options.second_motion_input.empty() ?
                                                        -static_cast<float>(second_frame.width) :
                                                        1.0f),
          std::isfinite(options.motion_scale_y) ? options.motion_scale_y :
                                                   (options.second_motion_input.empty() ?
                                                        -static_cast<float>(second_frame.height) :
                                                        1.0f),
          options.depth_inverted);
      session.context.ExecuteAndWait();
    }
    session.context.Begin();
    const std::vector<float> result = session.context.ReadbackRgba16f(output_texture);
    PrintChannelStats(result);
    std::cout << "[dlss5_host] sentinel_pixels=" << CountSentinelPixels(result)
              << " total_pixels=" << size_t(output_width) * output_height << "\n";
    if (!options.raw_output.empty()) {
      WriteFloatRgba(options.raw_output, output_width, output_height, result);
    }
    WritePpm(options.output,
             output_width,
             output_height,
             result,
             options.output_encoding == "srgb");
    std::cout << "[dlss5_host] success frames=" << (has_second_frame ? 2 : 1)
              << " second_reset=" << (has_second_frame && options.second_reset ? 1 : 0)
              << " input=" << first_frame.width << 'x' << first_frame.height
              << " output=" << output_width << 'x' << output_height
              << " file=" << options.output.string() << "\n";
    return 0;
  }
  catch (const std::exception &error) {
    std::cerr << "[dlss5_host] error: " << error.what() << '\n';
    return 2;
  }
}
