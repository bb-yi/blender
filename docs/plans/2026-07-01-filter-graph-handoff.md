# Eevee Filter Graph Handoff

Date: 2026-07-01
Branch: `feat/filter-graph-image-handles`
Repo: `E:\blender_bulid_test\blender_npr_bulid\blender_5_1_port`

## Current Goal

Replace the old linear Eevee filter-material stack with a scene-level Filter Graph that passes image handles through a DAG. A `Filter Pass` graph node invokes a filter-domain material. The material uses `Pass Input` nodes to read the graph invocation inputs.

The current implementation is a functional first pass, not ready for merge. It compiles, installs, and passes the targeted smoke test described below.

## Implemented

- Added `EeveeFilterGraphNodeTree` and `Scene.eevee.filter_graph`.
- Added graph node types:
  - `Scene Color`
  - `AOV Input`
  - `Filter Pass`
  - `Stage Output`
- `Stage Output` uses shader-output style active semantics:
  - `custom1` stores execution stage.
  - `NODE_DO_OUTPUT` marks the active output for that stage.
  - Multiple outputs may exist; only the active one is executed.
- Added Filter Graph Shift+A menu groups:
  - Input
  - Pass
  - Output
- Added `Pass Input` shader node (`ShaderNodeFilterGraphInput`) for filter-domain materials.
- Added dynamic material interface items using `NodeEeveeFilterGraphSocketItem`.
- Implemented bidirectional interface mirroring between material `Pass Input` and all graph `Filter Pass` nodes using the same material.
- Added `Filter Pass` UX operators:
  - `node.filter_pass_new_material`
  - `node.filter_pass_clear_material`
  - `NODE_OT_filter_pass_edit_material`
- Added editor navigation:
  - Double-click / Edit on `Filter Pass` opens the filter material node tree.
  - Ctrl+Tab returns to the scene Eevee Filter Graph.
- Scene properties panel now shows Filter Graph entry instead of the old linear filter material list.
- `render_stage()` executes the graph path when `scene.eevee.filter_graph` exists.
- Legacy filter stack still has a compatibility path when no graph exists.

## Runtime Details

- Graph edges pass image handles, not colors.
- `Scene Color.Color Image` is the current stage input.
- `Scene Color.Depth/Normal/Position Image` resolve from render buffers.
- `Filter Pass` material invocations build an input handle table.
- `TEX_HANDLE_FILTER_GRAPH_INPUT` resolves through `filter_graph_input_buf`.
- Graph intermediate textures use `TEX_HANDLE_FILTER_GRAPH_TEXTURE`.
- Backend input cap is currently `FILTER_GRAPH_INPUT_MAX = 32`.
- First version keeps intermediate textures full resolution and uses the stage input format.
- Filter material output remains single-main-image only; no MRT/multi-output material support yet.

## Recent Bug Fixes

### Two Filter Passes Chained Rendered Black

Cause:

`prepare_filter_graph_inputs()` copied a 2D intermediate texture into a `sampler2DArray` layer using:

```cpp
GPU_texture_copy(graph_input_tx_.layer_view(layer), texture_inputs[layer]);
```

That path did not reliably populate the layer used by the next pass, so the second pass sampled black.

Fix:

- Added `eevee_filter_graph_input_copy_frag.glsl`.
- Added static shader type `FILTER_GRAPH_INPUT_COPY`.
- Explicitly renders a fullscreen copy into each `graph_input_tx_` layer framebuffer.
- `two_pass_chain` smoke now matches passthrough and one-pass output.

### Depth Direct Output Was Pure White

Cause:

The resolve shader used a hand-written reverse-Z conversion.

Fix:

- `eevee_filter_graph_resolve_frag.glsl` now includes `eevee_reverse_z_lib.glsl`.
- Depth visualization uses `reverse_z::read()` and `-drw_depth_screen_to_view()`.

## Validation Done

Commands run from outer root:

```powershell
python -m py_compile blender_5_1_port\scripts\startup\bl_operators\node.py blender_5_1_port\scripts\startup\bl_ui\space_node.py blender_5_1_port\scripts\startup\bl_ui\properties_scene.py blender_5_1_port\scripts\startup\bl_ui\node_add_menu_shader.py
git -C blender_5_1_port diff --check
.\build_ninja_sccache_poll.bat install --no-pause
install_windows_x64_vc17_Release_5_1_port_clean\blender.exe --factory-startup --background --python temp\scripts\filter_graph_smoke.py
```

Build result:

- Default install succeeded.
- Install tree: `E:\blender_bulid_test\blender_npr_bulid\install_windows_x64_vc17_Release_5_1_port_clean`
- `blender.exe` installed at about `2026-07-01 16:19`.

Smoke output:

- `passthrough`: non-black, alpha mean 1.0
- `one_pass`: matches passthrough
- `two_pass_chain`: matches passthrough and one pass
- `depth_direct`: non-black and non-flat
- `normal_direct`: non-black and non-flat
- `position_direct`: non-black and non-flat
- `alpha_opaque`: alpha mean 1.0
- `alpha_transparent`: alpha mean 0.0
- `filter_pass_has_material_property`: true
- operators `new`, `clear`, `edit`: true

Smoke files are intentionally outside the source repo:

- Script: `E:\blender_bulid_test\blender_npr_bulid\temp\scripts\filter_graph_smoke.py`
- Output: `E:\blender_bulid_test\blender_npr_bulid\temp\filter_graph_smoke\`

Note: The background smoke still printed a tiny unfreed-memory message on exit:

```text
Error: Not freed memory blocks: 1, total unfreed memory 0.000259 MB
```

This needs follow-up if it persists.

## Known Issues And Risks

- User previously reported the `Filter Pass` node body UI still appeared empty in the interactive editor. The code now registers `draw_buttons` and `b.add_default_layout()`, and smoke confirms the RNA material property/operators exist, but this still needs visual interactive verification in Blender.
- Filter material header UI is not fully polished. It currently shows the filter material name when editing a filter material, but it is not yet the same as Blender's standard material ID selector UX.
- Alpha: user-side `Filter Output.Alpha` should be opacity (`1 = visible`, `0 = transparent`). Smoke confirms final image alpha follows that. Do not remove the final transmittance conversion in `eevee_filter_material_frag.glsl` without checking EEVEE combined/film semantics.
- Direct `Normal` output currently appears high mean in smoke but has variance. It is useful for debug visualization, not color-managed art output.
- Graph executor validation is minimal. It handles cycle/invalid-node failure by black output and info messages, but node-level UI error reporting is not complete.
- Legacy stack is not fully unified into synthesized graph execution. There are still two paths in `render_stage()`.
- AOV writer conflict handling is not fully productized for graph UX.
- No release tests were run by request. Only current-branch targeted smoke was run.
- `docs/glsl-function-node-conversion-guide.md` has a large unrelated-looking dirty change in this commit. Confirm whether it should stay before merge.
- `source/blender/draw/engines/eevee/shaders/eevee_light_shadow_setup_comp.glsl` has a small compile-oriented change (`exp2(float(level))`). Confirm whether to keep or split before merge.

## Suggested Next Steps

1. Open the installed Blender interactively and verify:
   - Filter Graph appears in the scene properties panel.
   - Node editor can select and display `Eevee Filter Graph`.
   - Shift+A menu lists `Scene Color`, `AOV Input`, `Filter Pass`, `Stage Output`.
   - `Filter Pass` node body shows material selector and New/Clear/Edit/Sync controls.
   - Double-click `Filter Pass` opens its filter material tree.
   - Ctrl+Tab returns to the Eevee Filter Graph.
2. Reproduce the user's original interactive chain:
   - `Scene Color -> Filter Pass -> Filter Pass -> Stage Output`
   - Confirm viewport is not black.
3. Stress dynamic interface sync:
   - Add/rename/delete/move inputs in `Pass Input`.
   - Confirm all graph `Filter Pass` nodes using the same material mirror exactly.
   - Repeat edits from graph side.
4. Add a repo-local automated test or formal release case later. Current smoke is in `temp` and not committed.
5. Clean up product polish before merge:
   - Better node/header UI.
   - Node-level graph compile error reporting.
   - Decide whether to keep full dynamic interface or enforce cap/UI warnings.
   - Decide whether to unify legacy stack through synthesized graph execution.
6. Before final merge:
   - Re-run default install.
   - Run current smoke.
   - Run release tests only when feature is ready for broader validation.

## Important Files

- `source/blender/draw/engines/eevee/eevee_filter_material.cc`
- `source/blender/draw/engines/eevee/eevee_filter_material.hh`
- `source/blender/draw/engines/eevee/shaders/eevee_filter_material_frag.glsl`
- `source/blender/draw/engines/eevee/shaders/eevee_filter_graph_resolve_frag.glsl`
- `source/blender/draw/engines/eevee/shaders/eevee_filter_graph_input_copy_frag.glsl`
- `source/blender/draw/engines/eevee/shaders/infos/eevee_filter_material_infos.hh`
- `source/blender/nodes/shader/nodes/node_filter_graph_nodes.cc`
- `source/blender/nodes/shader/nodes/node_shader_filter_graph_input.cc`
- `source/blender/nodes/shader/node_filter_graph_tree.cc`
- `source/blender/nodes/intern/filter_graph.cc`
- `scripts/startup/bl_ui/properties_scene.py`
- `scripts/startup/bl_ui/space_node.py`
- `scripts/startup/bl_operators/node.py`
