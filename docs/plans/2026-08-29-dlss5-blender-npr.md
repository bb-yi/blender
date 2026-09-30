# DLSS5 Blender NPR Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add an optional, pass-through-safe DLSS5 integration boundary to the NPR EEVEE branch and validate the real D3D12 runtime through an isolated offline executor before attempting live GPU interop.

**Architecture:** EEVEE supplies Color/Depth/MotionVector/jitter metadata to a `Dlss5Module` between post effects and Film accumulation. The first module is a no-op when the optional runtime is absent. A separate D3D12 executor owns `nvngx_dlssnr.dll` and is used for offline CPU-staging quality tests; Vulkan-D3D12 sharing is a later phase.

**Tech Stack:** Blender C++/CMake, EEVEE GPU abstraction, optional Windows D3D12/NGX host, NVIDIA NGX SDK headers/import library, Python test harness.

---

## Task 1: Record baseline and branch boundary

**Files:**
- Modify: none
- Test: `git -C blender_npr_post status --short --branch`

**Step 1:** Confirm base tag and commit.

Run:
```powershell
git -C blender_npr_post rev-parse v5.2.0-npr-port-win64-fd9fabb4f531^{commit}
```
Expected: `fd9fabb4f531...`.

**Step 2:** Confirm worktree.

Run:
```powershell
git -C blender_npr_post worktree list
```
Expected: `temp/worktrees/dlss5-blender-npr` on `feat/dlss5-blender-npr`.

**Step 3:** Commit only design documents after review.

```powershell
git -C temp/worktrees/dlss5-blender-npr add docs/architecture docs/plans
 git -C temp/worktrees/dlss5-blender-npr commit -m "docs: design DLSS5 NPR integration boundary"
```

## Task 2: Add the EEVEE adapter API

**Files:**
- Create: `source/blender/draw/engines/eevee/eevee_dlss5.hh`
- Create: `source/blender/draw/engines/eevee/eevee_dlss5.cc`
- Modify: `source/blender/draw/CMakeLists.txt`
- Modify: `source/blender/draw/engines/eevee/eevee_instance.hh`
- Modify: `source/blender/draw/engines/eevee/eevee_instance.cc`
- Modify: `source/blender/draw/engines/eevee/eevee_view.cc`

**Step 1:** Define a small `Dlss5Module` that accepts:
- `gpu::Texture *color`, `*depth`, `*velocity`;
- input/output extents;
- jitter and a reset flag;
- viewport/final-render mode.

**Step 2:** Implement the default behavior as pass-through:
- return `color` unchanged;
- expose `available() == false` until an executor is wired;
- emit one diagnostic per session only when explicitly enabled;
- do not add a hard dependency on NGX headers or DLLs.

**Step 3:** Add the module to `Instance` and call it immediately after `render_postfx` and before `film.accumulate`.

**Step 4:** Add source/header entries to `source/blender/draw/CMakeLists.txt`.

**Step 5:** Compile the affected target with the fixed Ninja build tree.

Expected: build succeeds; normal EEVEE output remains pass-through.

## Task 3: Validate EEVEE input contract

**Files:**
- Create: `tests/dlss5/eevee_dlss5_contract_test.cc` or use an existing EEVEE test target
- Create: `tests/dlss5/README.md`
- Create: `tests/dlss5/fixtures/` only if a small text fixture is needed

**Step 1:** Add a contract test for matching Color/Depth/Velocity extents and reset propagation.

**Step 2:** Add a test for viewport vector format handling:
- RG16F viewport vector path;
- RGBA16F final-render vector path;
- explicit swizzle resolution.

**Step 3:** Run the narrow test target and record output in `temp/logs/`.

Expected: the module returns the original color texture and does not mutate EEVEE resources.

## Task 4: Implement the isolated D3D12 executor

**Files:**
- Create: `tests/dlss5/dlss5_host/` sources and CMake project
- Reuse/reference: `RenderingPlugin/Source/DlssNrRuntime.cpp` and `NgxRuntime.cpp` from UnityDLSSNR

**Step 1:** Add dependency discovery for the NGX SDK headers/import library as an optional external build input. Do not make the Blender configure step download vendor SDKs.

**Step 2:** Implement D3D12 device, queue, staging textures and fence synchronization.

**Step 3:** Load `nvngx_dlssnr.dll`, resolve `Init_Ext`, `Shutdown1`, `CreateFeature`, `EvaluateFeature`, `ReleaseFeature`, and perform the caller-module compatibility hook only inside the isolated host.

**Step 4:** Implement parameter allocation through the NGX core and set Color/Depth/MV/subrect/output/exposure parameters from the test manifest.

**Step 5:** Implement readback and a small float output format first; add PNG/EXR preview conversion separately.

**Step 6:** Add deterministic exit codes and logs for missing DLL, missing exports, unsupported driver, invalid format and evaluation errors.

Expected: host can initialize and report a clear result without being linked into Blender.

## Task 5: Add Blender offline harness

**Files:**
- Create: `tests/dlss5/scripts/render_passes.py`
- Create: `tests/dlss5/cases/s01_basic/{case.json,run.py,README.md}`
- Create: `tests/dlss5/cases/s05_temporal/{case.json,run.py,README.md}`

**Step 1:** Render Color, Z and Vector at input resolution and a full-resolution ground truth.

**Step 2:** Convert Blender-native float passes to the host manifest format.

**Step 3:** Invoke the external host and collect logs.

**Step 4:** Compare DLSS5 output against ground truth and a bilinear control using PSNR/SSIM plus an orientation marker.

**Step 5:** Add a multi-frame camera-motion case; project Blender velocity into screen-space pixels before sending it to NGX.

Expected: s01 establishes runtime/format correctness; s05 establishes temporal input correctness.

## Task 6: Decide whether live interop is worth implementing

**Files:**
- Modify: `docs/architecture/adr-001-dlss5-npr-integration-boundary.md`
- Create: `docs/architecture/adr-002-dlss5-vulkan-d3d12-interop.md` if the decision proceeds

**Step 1:** Measure CPU staging cost and quality.

**Step 2:** If offline quality is valid, prototype Vulkan image/fence export in a separate backend-specific test, not in the default EEVEE path.

**Step 3:** Compare three options: CPU staging, Vulkan-D3D12 sharing, full D3D12 backend.

**Step 4:** Update the ADR with a go/no-go decision based on measured frame time, synchronization correctness and maintenance cost.

Expected: no live interop code is merged without a measured benefit and a reliable fallback.
