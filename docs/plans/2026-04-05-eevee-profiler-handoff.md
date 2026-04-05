# Eevee Profiler / Stage Debugger Handoff

## Current Goal

This branch is building two Eevee developer tools for the NPR port:

1. `Eevee Performance`
   - Goal: answer `where is time going?`
   - Current main UI: Outliner display mode `Eevee Performance`

2. `Stage Output Viewer`
   - Goal: answer `which pipeline stage starts producing the wrong image?`
   - Current main UI: Render Properties > Eevee > Performance > Stage Output Viewer

These two tools are supposed to share one stage-definition backbone.

---

## Branch / Binary

- Branch: `feature/eevee-performance-profiler`
- Install binary path:
  - `E:\blender_bulid_test\blender_npr_bulid\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe`
- Current installed binary reports:
  - `hash 250eb0d9e0c4`
  - `built 2026-04-04 20:59:20`

Important: the worktree is **dirty**. Nothing in this handoff has been committed yet.

---

## Files With Active Changes

Main areas touched:

- Eevee telemetry / sampling / profiler:
  - `source/blender/draw/engines/eevee/eevee_telemetry.hh`
  - `source/blender/draw/engines/eevee/eevee_telemetry.cc`
  - `source/blender/draw/engines/eevee/eevee_instance.hh`
  - `source/blender/draw/engines/eevee/eevee_instance.cc`
  - `source/blender/draw/engines/eevee/eevee_view.cc`
  - `source/blender/draw/engines/eevee/eevee_pipeline.cc`
  - `source/blender/draw/engines/eevee/eevee_volume.cc`
  - `source/blender/draw/engines/eevee/eevee_depth_of_field.cc`
  - `source/blender/draw/engines/eevee/eevee_sampling.hh`

- Outliner `Eevee Performance` mode:
  - `source/blender/editors/space_outliner/tree/tree_display_eevee_performance.cc`
  - `source/blender/editors/space_outliner/tree/tree_display.cc`
  - `source/blender/editors/space_outliner/tree/tree_display.hh`
  - `source/blender/editors/space_outliner/outliner_tree.cc`
  - `source/blender/editors/space_outliner/outliner_draw.cc`
  - `source/blender/editors/space_outliner/outliner_sync.cc`
  - `source/blender/editors/space_outliner/outliner_utils.cc`
  - `source/blender/editors/space_outliner/CMakeLists.txt`
  - `source/blender/makesdna/DNA_space_enums.h`
  - `source/blender/makesrna/intern/rna_space.cc`
  - `scripts/startup/bl_ui/space_outliner.py`

- RNA / scene properties:
  - `source/blender/makesdna/DNA_scene_types.h`
  - `source/blender/makesrna/intern/rna_scene.cc`

- Viewport overlay:
  - `source/blender/editors/space_view3d/view3d_draw.cc`

- Render UI:
  - `scripts/startup/bl_ui/properties_render.py`

- Planning docs:
  - `docs/plans/2026-04-05-eevee-profiler-and-stage-debugger.md`

There is also an unrelated user-facing release note file modified:

- `blender-5.1-npr-release-changelog.md`

Do not casually overwrite it.

---

## What Is Already Working

### 1. Shared stage registry exists

Telemetry stage labels and Outliner tree paths were consolidated into one source in:

- `eevee_telemetry.hh`
- `eevee_telemetry.cc`

`tree_display_eevee_performance.cc` now consumes telemetry stage metadata instead of keeping its own fully separate stage tree mapping.

### 2. Outliner mode exists

Outliner has a new display mode:

- `EEVEE_PERFORMANCE`

It builds a custom tree with:

- `Viewport`
- `Final Render`
- `Metadata`
- `Features`
- `Stages`
- `Hints`

### 3. CPU profiler drilldown exists

Profiler already has timing scopes for:

- top-level sync / capture / main view / final render readback
- `Deferred` child stages
- `Forward` child stages
- `Volume` child stages
- `DOF` child stages

### 4. Viewport display-only zeroing bug was addressed

When viewport accumulation had already finished and Eevee was only doing `film.display()`, telemetry was previously overwriting the last useful viewport record with near-zero timings.

Current code cancels that frame instead of publishing garbage.

### 5. Viewport publish throttling exists

Viewport telemetry publishing is throttled in:

- `eevee_telemetry.hh`
- `eevee_telemetry.cc`

The code currently uses:

- `viewport_publish_interval_seconds_ = 0.25`

This is display throttling, not sampling throttling.

### 6. Sample progress text exists

The profiler metadata now exposes:

- `Sample Progress`
- `Sampling`

instead of only a confusing `Sample Index`.

This uses viewport accumulation progress from:

- `Sampling::viewport_sample_index()`

### 7. Stage output viewer is present

Stage output viewer already works through stage-specific texture capture and film override.

This path is separate from the profiler path.

---

## Bugs That Were Reproduced And Fixed During This Session

### Fixed: Outliner parser crash on truncated report strings

Crash stack pointed to:

- `tree_display_eevee_performance.cc:294`

Root cause:

- parser used `line.substr(2)` on a trimmed line while reading stage entries
- if the report buffer ended with a truncated line, `substr()` could throw `std::out_of_range`

Fix:

- parse stage lines from the original raw line with safe trimming

### Fixed: `Eevee Performance` tree not refreshing stale empty viewport root

Observed symptom:

- `Viewport -> No viewport timing captured yet`
- while scene-side viewport telemetry string already had data

Likely cause:

- Outliner was redrawing without rebuilding the tree in some paths

Fix applied:

- `outliner_tree.cc` skips the `RGN_DRAW_NO_REBUILD` short-circuit for `SO_EEVEE_PERFORMANCE`

This forces a rebuild for the custom profiler display mode.

### Fixed: enabling profiler after viewport convergence did not re-open accumulation

Observed symptom:

- viewport already converged
- user enabled profiler
- no new viewport report was ever generated

Fix applied:

- `Instance::init()` now resets sampling when profiler enable state changes
- it also resets sampling when stage output view mode changes

---

## Validation That Was Performed

### Repro/validation done in real startup environment

Several front-end automation scripts were run with the actual user startup and addons, not only `--factory-startup`.

Important findings:

- If `VIEW3D` is `SOLID`, enabling profiler yields no viewport report
- If `VIEW3D` is switched to `RENDERED`, enabling profiler produces a viewport report

This is expected, because viewport timing is only meaningful when Eevee is actually rendering the view.

### Current important caveat

Do **not** assume the user failed to switch the viewport correctly.

This became a trust issue during debugging.

If the next AI investigates a failure, it should verify state with code/logs, not by telling the user they probably clicked the wrong thing.

---

## Things That Are Still Broken / Incomplete

### 1. `Sort by Time` has no visible effect in `Eevee Performance`

Reason:

- telemetry text reports do support sorted stage indices
- but the Outliner tree is currently built from a fixed stage hierarchy, so sibling order stays registry order

Result:

- `Sort by Time` changes report generation order, but not the visible Outliner tree order in a meaningful way

This still needs a real tree ordering step.

### 2. `Average Window` default still shows as `0` in factory startup

Observed:

- `bpy.context.scene.eevee.performance_profiler_average_window` prints `0` in factory startup

Even though:

- DNA default is set
- RNA range clamps to `1..64`

Current runtime impact:

- telemetry code clamps it with `max_ii(..., 1)`, so runtime behavior is not broken
- but UI/default semantics are still wrong

Needs cleanup/versioning/default investigation.

### 3. UI design is mid-transition

Current state:

- `Performance Profiler` panel in Render Properties was removed
- controls moved toward `Eevee Performance` Outliner mode
- Render Properties still contains `Stage Output Viewer`

But the UI is not fully settled yet.

### 4. Stage debugger and profiler are not fully unified yet

There is now a shared stage registry concept, but not every tool path fully consumes it yet.

### 5. Full `install` builds are unreliable on this machine

See build section below.

---

## Build / Install Reality On This Machine

### Important

`cmake` from:

- `C:\Program Files\CMake\bin\cmake.exe`

is problematic here because it is `4.1.0-rc4` and rejects the old VS instance unless a portable instance version is passed explicitly.

### Working toolchain

Use the old VS bundle here:

- VS root:
  - `D:\rj\zhuanye\vstudio`
- bundled CMake:
  - `D:\rj\zhuanye\vstudio\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
- MSBuild:
  - `D:\rj\zhuanye\vstudio\MSBuild\Current\Bin\amd64\MSBuild.exe`
- vcvars:
  - `D:\rj\zhuanye\vstudio\VC\Auxiliary\Build\vcvars64.bat`

### If CMake reconfigure is needed

Use:

```powershell
cmd /c "\"D:\rj\zhuanye\vstudio\VC\Auxiliary\Build\vcvars64.bat\" >nul && \"D:\rj\zhuanye\vstudio\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe\" -S e:\blender_bulid_test\blender_npr_bulid\blender_5_1_port -B e:\blender_bulid_test\blender_npr_bulid\build_windows_x64_vc17_Release_5_1_port_clean -G \"Visual Studio 17 2022\" -DCMAKE_GENERATOR_INSTANCE=D:/rj/zhuanye/vstudio,version=17.14.37012.4"
```

### Why not use normal `cmake --build` right now

It may fail because:

- current system CMake re-runs configure and rejects the old portable VS instance
- full solution install sometimes trips over unrelated oneAPI / external toolchain issues

### Reliable workaround used in this session

For many iterations, targeted rebuilds were used:

```powershell
$env:VCTargetsPath='D:\rj\zhuanye\vstudio\MSBuild\Microsoft\VC\v170\'
$env:VSINSTALLDIR='D:\rj\zhuanye\vstudio\'
& 'D:\rj\zhuanye\vstudio\MSBuild\Current\Bin\amd64\MSBuild.exe' <project>.vcxproj /p:Configuration=Release /m
```

Then copy the rebuilt binary manually:

```powershell
Copy-Item build_windows_x64_vc17_Release_5_1_port_clean\bin\Release\blender.exe install_windows_x64_vc17_Release_5_1_port_clean\blender.exe -Force
Copy-Item build_windows_x64_vc17_Release_5_1_port_clean\source\creator\Release\blender_private.pdb install_windows_x64_vc17_Release_5_1_port_clean\blender.pdb -Force
```

And copy modified startup Python scripts manually when needed.

### Do not violate AGENTS instructions

- Use only:
  - `build_windows_x64_vc17_Release_5_1_port_clean`
  - `install_windows_x64_vc17_Release_5_1_port_clean`
- Do not create a new build directory
- Do not kill the user's running Blender in `install` or `release`

If install files are locked, stop and tell the user which process/path is blocking.

---

## Suggested Next Steps For The Next AI

### High priority

1. Fix `Sort by Time` in the Outliner tree for real
   - sort sibling `PerfNode` children by current `time_ms` when the flag is enabled
   - keep top-level grouping stable

2. Fix `Average Window` default showing `0`
   - investigate DNA default / RNA generation / versioning interaction

3. Improve empty state text
   - when viewport report is empty, show explicit cause:
     - no Eevee rendered viewport
     - profiler disabled
     - no sample captured yet

### Medium priority

4. Continue profiler drilldown
   - `Deferred / Forward` deeper
   - `Shadow`
   - `Raytrace`

5. Continue unifying stage debugger with shared stage registry

### Low priority

6. Clean up old render-properties profiler remnants / release notes once feature stabilizes

---

## Recommended First Action For The Next AI

If taking over immediately, start with:

1. Re-read:
   - `docs/plans/2026-04-05-eevee-profiler-and-stage-debugger.md`
   - `source/blender/draw/engines/eevee/eevee_telemetry.hh`
   - `source/blender/draw/engines/eevee/eevee_telemetry.cc`
   - `source/blender/editors/space_outliner/tree/tree_display_eevee_performance.cc`

2. Verify install binary version:
   - `E:\blender_bulid_test\blender_npr_bulid\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe --version`

3. Implement real sibling sorting for `Sort by Time`

That is the most obvious user-visible unfinished behavior right now.
