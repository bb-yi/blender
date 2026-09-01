# Vulkan / D3D12 interop probe

This directory contains the first real-time preview gate for the DLSS5 NPR
path. The probe is intentionally independent from Blender's build and does
not modify a running Blender instance.

## Build

Run from the workspace root:

```powershell
cmake -S blender_npr_post_mainfix/tests/dlss5/vulkan_d3d12_interop `
  -B temp/build/dlss5_vulkan_d3d12_interop_probe `
  -G Ninja `
  -DVULKAN_ROOT=blender_npr_post_mainfix/lib/windows_x64/vulkan
cmake --build temp/build/dlss5_vulkan_d3d12_interop_probe --parallel 4
```

## Capability probe

Run with the Vulkan loader directory first on `PATH`:

```powershell
$env:Path = "$(Resolve-Path blender_npr_post_mainfix/lib/windows_x64/vulkan/bin);$env:Path"
.\temp\build\dlss5_vulkan_d3d12_interop_probe\dlss5_vulkan_d3d12_interop_probe.exe
```

The probe prints image external-handle features, semaphore handle features,
the Vulkan/DXGI adapter LUID match, and the final
`DLSS5_INTEROP_BASELINE_READY` marker.

## Shared resource smoke

The shared-resource test creates a D3D12-owned
`R16G16B16A16_FLOAT` resource and shared `D3D12_FENCE`, imports both into
Vulkan, clears the image from Vulkan, signals the imported timeline semaphore,
then waits and reads the same resource from D3D12.

Expected marker:

```text
DLSS5_VULKAN_D3D12_SHARED_RESOURCE: PASS
```

The test proves the direction required by the current Windows driver:
D3D12 owns the shared resources and Blender Vulkan imports them. It does not
yet run NGX or touch EEVEE render buffers.
