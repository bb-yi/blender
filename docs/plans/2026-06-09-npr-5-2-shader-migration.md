# NPR 5.2 Shader Migration Implementation Plan

**Goal:** Complete the NPR shader migration on `merge/blender-5.2-beta-into-npr-port-5.1` so NPR nodes run on Blender 5.2's BSL/resource-table material architecture without losing the 5.1 NPR behavior.

**Architecture:** Stabilize the current mixed architecture first. Port missing BSL hooks for World NPR and light-probe access, keep the deferred NPR/filter/bake GLSL compatibility islands working, then migrate those islands toward resource-table BSL in later bounded steps.

**Tech Stack:** Blender EEVEE Next, BSL shader files, GPU material codegen, shader create-info resources, NPR shader nodes, Python release tests, Ninja+sccache install-tree validation

---

## Current State

- Branch: `merge/blender-5.2-beta-into-npr-port-5.1`.
- Source repo: `E:\blender_bulid_test\blender_npr_bulid\blender_5_1_port`.
- Build target: outer root `build_ninja_sccache_poll.bat install --no-pause`.
- Install tree: `install_windows_x64_vc17_Release_5_1_port_clean`.
- Latest validated install tree: Blender `5.2.0 LTS Beta`, branch
  `merge/blender-5.2-beta-into-npr-port-5.1 (modified)`, hash `da69e52071bd`,
  build time `2026-06-10 13:59:05`.

The merge has no text conflict markers. The current patch migrates the blocking NPR shader paths
needed by the 5.2 BSL/resource-table material architecture:

- Regular EEVEE world/deferred/forward material shaders use 5.2 BSL/resource-table style.
- NPR deferred, filter material, and bake color are still GLSL compatibility passes.
- `eevee_nodetree_lib.bsl.hh` has legacy `drw_*` shims so old NPR node GLSL can compile in BSL material passes.
- World NPR now applies `nodetree_npr()` in `eevee_surf_world.bsl.hh` and exposes world
  `TextureHandle` inputs for Combined Color, Position, and Normal.
- Light-probe access now uses conditional 5.2 resource-table bindings instead of forcing
  probe data onto every deferred material.
- GLSL Function ambient helper code now emits 5.2 light-probe/resource-table API names.
- Light Shader surfel/volume-probe bake now uses a BSL resource table for its SSBO-backed
  per-surfel Light Shader results instead of the old 5.1 global `LIGHT_SHADER_SURFEL_EVAL`
  GLSL path.

## Resolved Failing Evidence

Release cases that originally identified the migration gaps are now passing against the current
install tree:

- `npr-test_world_npr_tree_render`: pass, log
  `test\release\logs\20260609-235858\001_npr-test_world_npr_tree_render.log`.
- `npr-test_goo_light_probe_color_render`: pass, log
  `test\release\logs\20260609-235918\001_npr-test_goo_light_probe_color_render.log`.
- `npr-test_goo_light_probe_color_smoke`: pass, log
  `test\release\logs\20260609-235550\001_npr-test_goo_light_probe_color_smoke.log`.
- `eevee-light-shader`: pass, logs
  `test\release\logs\20260610-221147\001_eevee-light-shader-output-direct-volume.log`
  through `005_eevee-light-shader-output-regressions.log`.
- Latest targeted NPR/GLSL shader reruns:
  - `glsl-function-node`: pass, log
    `test\release\logs\20260610-221307\001_glsl-function-node.log`.
  - `npr-test_glsl_function_light_access_render`: pass, log
    `test\release\logs\20260610-221347\001_npr-test_glsl_function_light_access_render.log`.
  - `npr-test_filter_glsl_function_render`: pass, log
    `test\release\logs\20260610-221418\001_npr-test_filter_glsl_function_render.log`.
  - `npr-test_world_npr_tree_render`: pass, log
    `test\release\logs\20260610-221508\001_npr-test_world_npr_tree_render.log`.
  - `npr-test_goo_light_probe_color_render` and `npr-test_goo_light_probe_color_smoke`: pass,
    logs under `test\release\logs\20260610-221534\`.

## Migration Strategy

Keep this order. Do not start with a broad BSL rewrite of every NPR pass.

1. Restore World NPR semantics in `eevee_surf_world.bsl.hh`.
2. Add world/light-probe resources when World NPR or GLSL light-probe nodes need them.
3. Port GLSL Function ambient/light-probe helper output to the 5.2 resource-table API.
4. Update the 5.2 menu smoke shim after shader-side behavior is fixed.
5. Only then consider migrating deferred NPR/filter/bake GLSL compatibility islands to BSL.

## Implemented Architecture Notes

- World NPR is now a BSL-native hook inside `eevee_surf_world.bsl.hh`. The world pass first
  computes the normal background into `g_world_combined_color`, then calls `nodetree_npr()` when
  `NPR_SHADER` is defined. Offset sampling re-evaluates the world surface for the shifted view
  direction and restores material globals afterward.
- Material sampler assignment in `eevee_shader.cc` is delayed with `reserve_slots(info)` so
  conditional resources can be added before generated material samplers are assigned. This avoids
  world/NPR sampler overflow and missing generated `samp*` resources.
- Deferred BSL keeps the ordinary `surf_deferred()` entrypoint unchanged for normal materials and
  adds `surf_deferred_lightprobe()` only for materials that need `LightprobeRenderData`. Both
  entrypoints share `surf_deferred_impl(...)`.
- `use_lightprobe_data` is computed once in `eevee_shader.cc` and gates both
  `eevee_lightprobe_data` and the `eevee_lightprobe.bsl.hh` dependency.
- `eevee_lightprobe_infos.hh` provides BSL type macros plus resource-pass aliases for sphere,
  volume, and planar probe resources. `eevee_sampling_infos.hh` only provides
  `CREATE_INFO_eevee_Sampling`; do not add a `CREATE_INFO_RES_PASS_eevee_Sampling` alias because
  the legacy NPR pass already declares `sampling_buf`.
- Deferred GBuffer sync binds sphere and volume probe resources so the lightprobe-only deferred
  entrypoint has valid runtime resources.
- `Light Probe Color` GLSL calls the 5.2 namespaced
  `eevee::lightprobe::sphere::roughness_to_lod(...)`.
- The remaining deferred NPR, filter material, and bake-color GLSL files are intentionally left as
  compatibility islands for this patch. They should be migrated in separate bounded changes after
  the blocking 5.2 BSL/resource-table behavior is stable.

## Task 1: Restore World NPR BSL Hook

Status: Done.

**Files:**

- Modify: `source/blender/draw/engines/eevee/shaders/eevee_surf_world.bsl.hh`

**Required behavior:**

- Define World NPR `TextureHandle` IDs for Combined Color, Position, and Normal.
- Populate `NPR Input` with world handles.
- Implement `TextureHandle_eval()` for world handles.
- Preserve the old 5.1 offset sampling behavior by re-evaluating the world surface for a shifted view direction.
- Save and restore material global state while re-evaluating an offset direction.
- Set `g_world_combined_color` to the normal world background before NPR evaluation.
- Replace final world background with `nodetree_npr()` when `NPR_SHADER` is defined.

**Validation:**

Run:

```bat
build_ninja_sccache_poll.bat install --no-pause
```

Then:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\run_release_tests.ps1 -BlenderExe .\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe -Name npr-test_world_npr_tree_render -ContinueOnFailure
```

Expected: World NPR center pixel is controlled by the NPR output, not the base world background.

## Task 2: Align Light-Probe Resources for World NPR

Status: Done, with a narrower architecture than originally proposed. Light-probe data is not added
to every deferred material; it is only added for material variants that need light-probe access.

**Files:**

- Modify: `source/blender/draw/engines/eevee/eevee_shader.cc`
- Inspect: `source/blender/draw/engines/eevee/shaders/infos/eevee_lightprobe_infos.hh`

**Required behavior:**

- If `GPU_MATFLAG_LIGHTPROBE_ACCESS` is set, add `eevee_lightprobe_data` for material variants
  that actually need probe resources.
- Keep slot reservation compatible with world material texture slots.
- Add `eevee_lightprobe.bsl.hh` dependency for light-probe access graphs.

**Validation:**

Run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\run_release_tests.ps1 -BlenderExe .\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe -Name npr-test_goo_light_probe_color_render -ContinueOnFailure
```

Expected: Light Probe Color returns the scene probe/world color instead of black.

## Task 3: Port GLSL Function Light-Probe Helper

Status: Done.

**Files:**

- Modify: `source/blender/nodes/shader/nodes/node_shader_glsl_function.cc`

**Required behavior:**

- Replace generated helper use of old `lightprobe_load`, `spherical_harmonics_clamp`, and `spherical_harmonics_evaluate_lambert`.
- Use 5.2 BSL/resource-table APIs:
  - `view_matrices_get()`
  - `resource_table_get(eevee::LightprobeRenderData)`
  - `LightprobeRenderData::load(...)`
  - `spherical_harmonics::clamp_energy(...)`
  - `SphericalHarmonicL1::evaluate_lambert(...)`
- Keep the public GLSL Function helper API as `vec3 glsl_ambient_lighting()`.

**Validation:**

Run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\run_release_tests.ps1 -BlenderExe .\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe -Name glsl-function-node -ContinueOnFailure
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\run_release_tests.ps1 -BlenderExe .\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe -Name npr-test_glsl_function_light_access_render -ContinueOnFailure
```

Expected: GLSL Function parser/runtime tests still pass, and helper code compiles under 5.2 BSL material passes.

## Task 4: Update Blender 5.2 Menu Smoke Shim

Status: Done.

**Files:**

- Modify: `tests/python/npr/test_goo_light_probe_color_smoke.py`

**Required behavior:**

- Add the fake menu methods/fields required by Blender 5.2 `node_add_menu_shader.py`
  (`separator`, `draw_menu`, and `space_data.id_from`).
- Keep the smoke assertion focused on Light Probe Color node registration and menu exposure.

**Validation:**

Run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\run_release_tests.ps1 -BlenderExe .\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe -Name npr-test_goo_light_probe_color_smoke -ContinueOnFailure
```

Expected: no fake-menu API exception, and Light Probe Color menu exposure remains asserted.

## Task 5: Compatibility Island Review

Status: Done for this patch scope. Compatibility islands remain intentionally in place; nearby
regression tests passed.

**Files:**

- Inspect: `source/blender/draw/engines/eevee/shaders/eevee_surf_deferred_npr_frag.glsl`
- Inspect: `source/blender/draw/engines/eevee/shaders/eevee_filter_material_frag.glsl`
- Inspect: `source/blender/draw/engines/eevee/shaders/eevee_surf_bake_color_frag.glsl`
- Inspect: `source/blender/draw/engines/eevee/shaders/infos/eevee_surf_deferred_infos.hh`

**Required behavior:**

- Keep these passes working while Task 1-4 stabilize runtime.
- Do not rewrite them to BSL in the same patch unless a concrete test failure requires it.
- Record any remaining fixed sampler-slot or legacy GLSL dependencies before converting them.

**Validation:**

Run nearby regression tests:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\run_release_tests.ps1 -BlenderExe .\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe -Name npr-test_filter_glsl_function_render -ContinueOnFailure
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\run_release_tests.ps1 -BlenderExe .\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe -Name eevee-light-shader-output-regressions -ContinueOnFailure
```

Expected: compatibility passes keep their existing behavior.

Additional validation:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\run_release_tests.ps1 -BlenderExe .\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe -Name glsl-function-node -ContinueOnFailure
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\run_release_tests.ps1 -BlenderExe .\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe -Name eevee-light-shader-output-regressions -ContinueOnFailure
```

Passed logs:

- `test\release\logs\20260609-235643\001_glsl-function-node.log`
- `test\release\logs\20260609-235718\001_npr-test_glsl_function_light_access_render.log`
- `test\release\logs\20260609-235753\001_npr-test_filter_glsl_function_render.log`
- `test\release\logs\20260609-235816\001_eevee-light-shader-output-regressions.log`

## Task 6: Port Light Shader Surfel Bake Eval to 5.2 BSL Resources

Status: Done.

**Files:**

- Modify: `source/blender/draw/engines/eevee/shaders/eevee_light_data.bsl.hh`
- Modify: `source/blender/draw/engines/eevee/shaders/eevee_light_eval.bsl.hh`
- Modify: `source/blender/draw/engines/eevee/shaders/eevee_surfel_light.bsl.hh`
- Modify: outer release test
  `test/release/cases/142-eevee-light-shader-probe-reflection/run.py`

**Required behavior:**

- Keep deferred/forward Light Shader texture evaluation on the existing 5.2
  `LightShaderEvalData` sampler/index/uniform resource table.
- Add `LightShaderSurfelEvalData` for the surfel bake path, using the existing
  `LIGHT_SHADER_SURFEL_INDEX_BUF_SLOT`, `LIGHT_SHADER_SURFEL_BUF_SLOT`, and
  `LIGHT_SHADER_UNIFORM_BUF_SLOT` bindings.
- Use distinct surfel field names (`surfel_light_shader_*`) so BSL does not generate two
  `_light_shader_index_buf` resources with different SSBO bindings.
- Add `use_light_shader_surfel_eval` as a static resource-table gate on `LightEvalData`.
- Pass `light_shader_surfel_index` and `light_shader_surfel_len` through `EvalCtx` and evaluate
  per-surfel results as `light_shader_index * surfel_len + surfel_index`.
- Preserve uniform Light Shader results for surfel bake through the shared uniform SSBO.
- Keep `bind_light_shader_resources()` on the front/prepass light-shader cache for deferred
  direct-light evaluation; the surfel path uses `bind_surfel_light_shader_resources()`.
- Update the sphere-probe reflection release test to compare checker high-frequency variation
  with a second horizontal difference. Blender 5.2's smooth disabled reflection can have enough
  first-order gradient to make the old 5.1 threshold brittle even when the custom checker is
  visually and numerically present.

**Validation:**

Build/install:

```bat
build_ninja_sccache_poll.bat install --no-pause
```

Final build log:

- `temp\codex-build-logs\build-ninja-install-20260610-215844.log`

Install-tree version:

- Blender `5.2.0 LTS Beta`, branch
  `merge/blender-5.2-beta-into-npr-port-5.1 (modified)`, hash `da69e52071bd`,
  build time `2026-06-10 13:59:05`.

Targeted release tests:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\run_release_tests.ps1 -BlenderExe .\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe -Name eevee-light-shader-volume-probe-bake -ContinueOnFailure
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\run_release_tests.ps1 -BlenderExe .\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe -Name eevee-light-shader -ContinueOnFailure
```

Passed logs:

- `test\release\logs\20260610-220009\001_eevee-light-shader-volume-probe-bake.log`
- `test\release\logs\20260610-221147\001_eevee-light-shader-output-direct-volume.log`
- `test\release\logs\20260610-221147\002_eevee-light-shader-probe-reflection.log`
- `test\release\logs\20260610-221147\003_eevee-light-shader-volume-probe-bake.log`
- `test\release\logs\20260610-221147\004_eevee-light-shader-fast-path.log`
- `test\release\logs\20260610-221147\005_eevee-light-shader-output-regressions.log`

## Completion Criteria

- Install-tree Blender reports the current branch/hash.
- Targeted cases pass:
  - `npr-test_world_npr_tree_render`
  - `npr-test_goo_light_probe_color_render`
  - `npr-test_goo_light_probe_color_smoke`
  - `glsl-function-node`
  - `npr-test_filter_glsl_function_render`
  - `npr-test_glsl_function_light_access_render`
  - `eevee-light-shader`
  - `eevee-light-shader-output-regressions`
- Git status is clean or contains only the planned migration edits.
