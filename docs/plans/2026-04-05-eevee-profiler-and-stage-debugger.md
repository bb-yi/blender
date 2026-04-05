# Eevee Profiler And Stage Debugger Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build two Eevee developer tools for this NPR port: a performance profiler that answers "where is render time going?" and a stage debugger that answers "which pipeline stage starts producing the wrong image?"

**Architecture:** Keep the tools separate at the UI level, but make them share one Eevee stage-definition backbone. The profiler owns timing and scene-feature telemetry, while the stage debugger owns single-stage image capture. Both tools read the same ordered stage registry so stage names, hierarchy, and scope stay consistent.

**Tech Stack:** Blender C++, Eevee draw engine, RNA/DNA scene properties, Outliner tree display, Properties UI panels, viewport film override path

---

### Task 1: Shared Stage Registry And Hierarchy

**Files:**
- Modify: `source/blender/draw/engines/eevee/eevee_telemetry.hh`
- Modify: `source/blender/draw/engines/eevee/eevee_telemetry.cc`
- Modify: `source/blender/editors/space_outliner/CMakeLists.txt`
- Modify: `source/blender/editors/space_outliner/tree/tree_display_eevee_performance.cc`

**Step 1: Add a shared stage metadata type**

Add a small data description alongside `TelemetryStageId`:

```cpp
struct TelemetryStageInfo {
  TelemetryStageId id;
  const char *label;
  const char *tree_path;
};
```

Expose:

```cpp
static const TelemetryStageInfo &stage_info(TelemetryStageId stage);
static Span<const TelemetryStageInfo> stage_infos();
```

**Step 2: Move hard-coded stage labels into one table**

Replace the large `switch` in `stage_label()` with a single static table in `eevee_telemetry.cc`.

Example entries:

```cpp
{TelemetryStageId::MainDeferred, "Deferred", "Main View/Deferred"},
{TelemetryStageId::MainDeferredRaytrace, "Deferred.Raytrace", "Main View/Deferred/Raytrace"},
{TelemetryStageId::MainForwardTransparent,
 "Forward.Transparent",
 "Main View/Forward/Transparent"},
```

**Step 3: Make Outliner use the shared registry**

Remove the duplicated stage skeleton and path mapping from `tree_display_eevee_performance.cc`.

Instead:
- build the tree skeleton by iterating `TelemetryModule::stage_infos()`
- resolve parsed stage lines by matching `TelemetryModule::stage_label()`
- split the `tree_path` string to build the Outliner hierarchy

**Step 4: Build to verify the refactor**

Run:

```powershell
cmake --build e:\blender_bulid_test\blender_npr_bulid\build_windows_x64_vc17_Release_5_1_port_clean --config Release --target install -- /m:8
```

Expected:
- Build succeeds
- `Eevee Performance` Outliner mode still opens
- No stage hierarchy disappears because of duplicated mapping drift

**Step 5: Commit**

```bash
git add source/blender/draw/engines/eevee/eevee_telemetry.hh source/blender/draw/engines/eevee/eevee_telemetry.cc source/blender/editors/space_outliner/CMakeLists.txt source/blender/editors/space_outliner/tree/tree_display_eevee_performance.cc docs/plans/2026-04-05-eevee-profiler-and-stage-debugger.md
git commit -m "refactor: unify eevee profiler stage metadata"
```

### Task 2: Profiler Runtime Controls And Final Render Focus

**Files:**
- Modify: `source/blender/makesdna/DNA_scene_types.h`
- Modify: `source/blender/makesrna/intern/rna_scene.cc`
- Modify: `scripts/startup/bl_ui/properties_render.py`
- Modify: `source/blender/draw/engines/eevee/eevee_telemetry.hh`
- Modify: `source/blender/draw/engines/eevee/eevee_telemetry.cc`
- Modify: `source/blender/draw/engines/eevee/eevee_instance.cc`

**Step 1: Add explicit capture mode**

Add an enum/property for:
- `Off`
- `Continuous`
- `Next Frame`

The profiler should not rely only on a single boolean flag anymore.

**Step 2: Make final render the primary readout**

Keep viewport output compact, but make final render records complete:
- current frame number
- sample index
- total CPU
- full stage list
- feature snapshot
- hints

**Step 3: Make Next Frame self-reset**

When `Next Frame` completes one record:
- keep the captured data
- automatically fall back to `Off`

**Step 4: Verify**

Run:

```powershell
e:\blender_bulid_test\blender_npr_bulid\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe --factory-startup --background --python-expr "import bpy; print(bpy.context.scene.eevee.performance_profiler_capture_mode)"
```

Then do a normal install build and verify the property appears in UI.

**Step 5: Commit**

```bash
git add source/blender/makesdna/DNA_scene_types.h source/blender/makesrna/intern/rna_scene.cc scripts/startup/bl_ui/properties_render.py source/blender/draw/engines/eevee/eevee_telemetry.hh source/blender/draw/engines/eevee/eevee_telemetry.cc source/blender/draw/engines/eevee/eevee_instance.cc
git commit -m "feat: add eevee profiler capture modes"
```

### Task 3: Deferred, Forward, Volume, DOF Drilldown

**Files:**
- Modify: `source/blender/draw/engines/eevee/eevee_view.cc`
- Modify: `source/blender/draw/engines/eevee/eevee_pipeline.cc`
- Modify: `source/blender/draw/engines/eevee/eevee_volume.cc`
- Modify: `source/blender/draw/engines/eevee/eevee_depth_of_field.cc`
- Modify: `source/blender/draw/engines/eevee/eevee_subsurface.cc`
- Modify: `source/blender/draw/engines/eevee/eevee_telemetry.hh`
- Modify: `source/blender/draw/engines/eevee/eevee_telemetry.cc`

**Step 1: Keep the top-level pipeline groups stable**

Always emit these top-level groups in the registry:
- `Sync`
- `Capture`
- `Render Textures`
- `Main View`
- `Deferred`
- `Volume`
- `Forward`
- `PostFX`
- `Film`
- `Lookdev`
- `Read Result`

**Step 2: Add the highest-value child timings**

Required child stages:
- `Deferred.Prepass`
- `Deferred.HiZUpdate`
- `Deferred.GBufferPass`
- `Deferred.Raytrace`
- `Deferred.EvalLight`
- `Deferred.Subsurface`
- `Deferred.Combine`
- `Deferred.NPR`
- `Forward.HiZUpdate`
- `Forward.TransparencySetup`
- `Forward.Prepass`
- `Forward.Opaque`
- `Forward.Transparent`
- `Forward.Resolve`
- `Volume.ComputeSetup`
- `Volume.Scatter`
- `Volume.Integration`
- `Volume.Resolve.HiZ`
- `Volume.Resolve.Composite`
- `DOF.Setup`
- `DOF.TilePrepare`
- `DOF.Background`
- `DOF.Foreground`
- `DOF.HoleFill`
- `DOF.Resolve`

**Step 3: Verify**

Use a heavy Eevee final render and confirm the profiler shows non-zero values for the new child stages.

**Step 4: Commit**

```bash
git add source/blender/draw/engines/eevee/eevee_view.cc source/blender/draw/engines/eevee/eevee_pipeline.cc source/blender/draw/engines/eevee/eevee_volume.cc source/blender/draw/engines/eevee/eevee_depth_of_field.cc source/blender/draw/engines/eevee/eevee_subsurface.cc source/blender/draw/engines/eevee/eevee_telemetry.hh source/blender/draw/engines/eevee/eevee_telemetry.cc
git commit -m "feat: expand eevee profiler drilldown stages"
```

### Task 4: Shadow And Raytrace Deep Drilldown

**Files:**
- Modify: `source/blender/draw/engines/eevee/eevee_shadow.cc`
- Modify: `source/blender/draw/engines/eevee/eevee_raytrace.cc`
- Modify: `source/blender/draw/engines/eevee/eevee_telemetry.hh`
- Modify: `source/blender/draw/engines/eevee/eevee_telemetry.cc`

**Step 1: Split shadow work**

Target stages:
- `Shadow.TilemapSetup`
- `Shadow.CasterUpdate`
- `Shadow.TransparentCasterUpdate`
- `Shadow.TilemapUsage`
- `Shadow.TilemapUpdate`
- `Shadow.Surface`

**Step 2: Split raytrace work**

Target stages:
- `Raytrace.TileCompact`
- `Raytrace.Generate`
- `Raytrace.Trace.Planar`
- `Raytrace.Trace.Screen`
- `Raytrace.Trace.Fallback`
- `Raytrace.Denoise.Spatial`
- `Raytrace.Denoise.Temporal`
- `Raytrace.Denoise.Bilateral`
- `Raytrace.Horizon.Schedule`
- `Raytrace.Horizon.Setup`
- `Raytrace.Horizon.Scan`
- `Raytrace.Horizon.Denoise`
- `Raytrace.Horizon.Resolve`

**Step 3: Verify**

Compile, install, and run a final render scene with raytrace + shadows enabled.

Expected:
- `Deferred.Raytrace` is no longer a black box
- shadow-related spikes can be traced to a concrete sub-stage

**Step 4: Commit**

```bash
git add source/blender/draw/engines/eevee/eevee_shadow.cc source/blender/draw/engines/eevee/eevee_raytrace.cc source/blender/draw/engines/eevee/eevee_telemetry.hh source/blender/draw/engines/eevee/eevee_telemetry.cc
git commit -m "feat: add eevee shadow and raytrace drilldown"
```

### Task 5: Stage Debugger Core

**Files:**
- Modify: `source/blender/draw/engines/eevee/eevee_instance.hh`
- Modify: `source/blender/draw/engines/eevee/eevee_instance.cc`
- Modify: `source/blender/draw/engines/eevee/eevee_view.cc`
- Modify: `source/blender/draw/engines/eevee/eevee_film.cc`
- Modify: `source/blender/makesdna/DNA_scene_types.h`
- Modify: `source/blender/makesrna/intern/rna_scene.cc`
- Modify: `scripts/startup/bl_ui/properties_render.py`

**Step 1: Keep the single-stage capture model**

Do not retain every intermediate texture.

Only:
- choose one stage
- copy that stage output immediately
- show it instead of the final viewport result

**Step 2: Make the debugger read the same stage registry**

Only expose stages marked as image-viewable.

V1 image stages:
- `Final`
- `Background`
- `Deferred`
- `Filter.BeforeVolumeFog`
- `Volume.Resolve`
- `AmbientOcclusion`
- `Forward`
- `Filter.BeforePostFX`
- `MotionBlur`
- `Filter.BeforeDepthOfField`
- `DepthOfField`
- `Filter.BeforeComposite`

**Step 3: Verify**

Switch the stage dropdown and confirm:
- image changes
- no crash
- final render output is unaffected

**Step 4: Commit**

```bash
git add source/blender/draw/engines/eevee/eevee_instance.hh source/blender/draw/engines/eevee/eevee_instance.cc source/blender/draw/engines/eevee/eevee_view.cc source/blender/draw/engines/eevee/eevee_film.cc source/blender/makesdna/DNA_scene_types.h source/blender/makesrna/intern/rna_scene.cc scripts/startup/bl_ui/properties_render.py
git commit -m "feat: add eevee stage debugger"
```

### Task 6: Verification, Docs, And Release Packaging

**Files:**
- Modify: `docs/eevee-render-pipeline-reference.md`
- Modify: `blender-5.1-npr-features-and-usage.md`
- Modify: `blender-5.1-npr-release-changelog.md`

**Step 1: Update docs**

Document:
- profiler purpose
- stage debugger purpose
- where the UI lives
- how to interpret the main groups

**Step 2: Verify install build**

Run:

```powershell
cmake --build e:\blender_bulid_test\blender_npr_bulid\build_windows_x64_vc17_Release_5_1_port_clean --config Release --target install -- /m:8
```

**Step 3: Manual smoke checklist**

- Launch install build
- Open a heavy Eevee scene
- Run final render once
- Check `Eevee Performance` Outliner mode
- Check stage debugger viewport switch
- Confirm startup is not blocked by these tools when they are disabled

**Step 4: Commit**

```bash
git add docs/eevee-render-pipeline-reference.md blender-5.1-npr-features-and-usage.md blender-5.1-npr-release-changelog.md
git commit -m "docs: document eevee profiler and debugger"
```
