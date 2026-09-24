# DLSS5 NPR Realtime Preview Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the offline Blender DLSSNR comparison path with an optional
real-time EEVEE viewport path that preserves the normal render path when the
NVIDIA runtime or cross-API synchronization is unavailable.

**Architecture:** Keep the existing EEVEE `Dlss5Module` insertion point after
post effects and before Film accumulation. Add a Windows Vulkan/D3D12
interop layer that exports compatible render resources and synchronization
objects, then run an in-process D3D12 NGX session with lazy feature recreation
on extent or mode changes. The official Blender `dlss` branch is used for
session lifetime, capability checks, jitter/reset propagation and failure
fallback patterns; its Cycles CUDA denoiser is not copied into EEVEE.

**Tech Stack:** Blender EEVEE C++, Vulkan GPU backend, Windows external memory
and semaphore extensions, Direct3D 12, NVIDIA NGX/DLSSNR, Ninja, and focused
GPU smoke tests.

---

### Task 1: Preserve the offline baseline

**Files:**
- Modify: `docs/architecture/adr-001-dlss5-npr-integration-boundary.md`
- Test: existing Blender add-on and `tests/dlss5/dlss5_host`

**Step 1:** Record the current offline host and Blender UI result as the
fallback baseline.

**Step 2:** Keep the current add-on usable while the real-time path is
disabled or unavailable.

**Step 3:** Verify the current mainfix installation before changing the GPU
backend.

### Task 2: Add a Vulkan/D3D12 capability probe

**Files:**
- Create: `tests/dlss5/vulkan_d3d12_interop/CMakeLists.txt`
- Create: `tests/dlss5/vulkan_d3d12_interop/vulkan_d3d12_interop_probe.cpp`

**Step 1:** Enumerate the NVIDIA Vulkan device and required external memory,
semaphore and fence extensions.

**Step 2:** Query external image support for
`R16G16B16A16_SFLOAT`, `R32_SFLOAT`, and `R16G16_SFLOAT` using
`D3D12_HEAP`, `D3D12_RESOURCE`, and Win32 handle types.

**Step 3:** Query external semaphore support for `D3D12_FENCE` and Win32
handles, and print exact Vulkan result codes and compatible handle types.

**Step 4:** Build and run the probe without changing Blender or the current
install tree.

**Observed result (2026-08-30):** The local RTX 4080 has all four required
external memory/semaphore extensions and a matching D3D12 adapter. Vulkan can
import `D3D12_RESOURCE` images and `D3D12_FENCE` semaphores, while the queried
D3D12 handle types are not Vulkan-exportable on this driver.

### Task 3: Add explicit native Vulkan interop handles

**Files:**
- Modify: `source/blender/gpu/vulkan/vk_memory_pool.hh`
- Modify: `source/blender/gpu/vulkan/vk_memory_pool.cc`
- Modify: `source/blender/gpu/vulkan/vk_texture.hh`
- Modify: `source/blender/gpu/vulkan/vk_texture.cc`
- Modify: `source/blender/gpu/vulkan/vk_device.hh`
- Modify: `source/blender/gpu/vulkan/vk_device.cc`
- Modify: `source/blender/gpu/vulkan/vk_context.hh`
- Modify: `source/blender/gpu/vulkan/vk_context.cc`

**Step 1:** Add an explicit D3D12-compatible export mode instead of reusing
the current opaque Win32 allocation mode.

**Step 2:** Add resource metadata for Vulkan image format, extent, tiling,
allocation offset, and ownership/lifetime.

**Step 3:** Add a synchronization API that returns a submission value or
imports/exports a shared fence without exposing backend internals to EEVEE.

**Step 4:** Add backend tests for unsupported handles and non-exportable
textures, with no assertion in normal fallback operation.

**Completed first gate:** An independent smoke created a D3D12-owned
`R16G16B16A16_FLOAT` resource and fence, imported them into Vulkan, cleared the
image in Vulkan, and read back `0.125, 0.25, 0.5, 1.0` from D3D12.

### Task 4: Refactor the D3D12 NGX host into a reusable in-process session

**Files:**
- Create: `source/blender/draw/engines/eevee/dlss5_d3d12.hh`
- Create: `source/blender/draw/engines/eevee/dlss5_d3d12.cc`
- Modify: `source/blender/draw/engines/eevee/eevee_dlss5.hh`
- Modify: `source/blender/draw/engines/eevee/eevee_dlss5.cc`
- Reuse/reference: `tests/dlss5/dlss5_host/dlss5_host.cpp`

**Step 1:** Move dynamic NGX loading, caller-module compatibility handling,
parameter setup, feature creation and release into an RAII session.

**Step 2:** Recreate the feature only when input/output extent, format,
upscaling mode or backend device changes.

**Step 3:** Pass color, depth, motion vectors, jitter, exposure and reset for
each frame.

**Step 4:** Return the original EEVEE color texture on every runtime,
interop, resource-state or evaluation failure.

### Task 5: Implement a GPU-only viewport preview path

**Files:**
- Modify: `source/blender/draw/engines/eevee/eevee_view.cc`
- Modify: `source/blender/draw/engines/eevee/eevee_instance.hh`
- Modify: `source/blender/draw/engines/eevee/eevee_instance.cc`
- Modify: `scripts/startup/bl_ui/space_view3d.py`
- Modify: `source/blender/makesrna/intern/rna_space.cc`

**Step 1:** Add a viewport-only opt-in setting with `Off`, `DLSSNR`, and
`Auto` states.

**Step 2:** Render at the selected input scale while keeping display extent
and Film accumulation ownership explicit.

**Step 3:** Insert the DLSS output at the correct display-resolution boundary
without feeding a display-resolution result back through the low-resolution
Film resampler.

**Step 4:** Reset history on camera changes, scene changes, resolution changes,
mode changes, and explicit viewport reset.

### Task 6: Validate interaction and fallback

**Files:**
- Create: `tests/dlss5/vulkan_d3d12_interop/README.md`
- Create: focused GPU/viewport smoke scripts under `temp/scripts/`
- Modify: `tests/dlss5/README.md`
- Modify: `docs/architecture/adr-001-dlss5-npr-integration-boundary.md`

**Step 1:** Verify the probe on the local RTX 4080 and record supported handle
types.

**Step 2:** Verify a static viewport frame, camera motion, history reset and
resolution change.

**Step 3:** Verify that disabling the runtime, using a non-Vulkan backend,
or forcing an unsupported format restores the original EEVEE image.

**Step 4:** Measure GPU synchronization overhead and compare against CPU
staging before enabling the feature by default.

**Step 5:** Build the fixed mainfix tree, run targeted tests, and leave the
existing offline UI test as a regression case.
