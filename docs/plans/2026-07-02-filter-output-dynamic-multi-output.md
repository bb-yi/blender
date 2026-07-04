# Filter Output Dynamic Multi-Output Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Let filter materials expose a user-defined number of named image outputs from the existing `Filter Output` node, while keeping AOV inputs read-only and AOV writes explicit at the Filter Graph boundary.

**Architecture:** `Filter Output` becomes a dynamic output-interface node. Each output item creates one `Color` input and one `Alpha` input in the filter material, and one matching `Image` output socket on every `Filter Pass` graph node using that material. A filter pass invocation executes once and publishes its reachable outputs as graph image handles; final AOV publishing remains a separate graph-level `AOV Output` operation.

**Tech Stack:** Blender DNA/RNA node storage, socket-items declarations, GPU material codegen, EEVEE fullscreen filter material pass, EEVEE Filter Graph executor.

---

## Evaluation

This is the right direction. It solves the user's main workflow issue: a single filter material can manage one algorithm and export multiple intermediate results without splitting everything into separate materials.

The important boundary is that multi-output is not multi-pass. Outputs from the same `Filter Output` node are simultaneous exports from one shader invocation. Output B cannot sample Output A as a generated texture in the same pass. If a workflow needs "render output A, then sample output A as an image", it must remain two Filter Pass nodes.

The AOV rule should stay simple:

- `AOV Input` is a read-only graph source for the stage's base AOV data.
- `Filter Output` exports named graph image handles.
- `AOV Output` writes one graph image handle to a named final AOV.
- Filter materials must not write final AOVs through hidden `Output AOV` side effects in graph mode.

## Decisions

- Use dynamic output items, not fixed output names or fixed visible sockets.
- Default new filter materials get one output named `Image`; the user can rename it and add more.
- Keep at least one output item to avoid empty, non-rendering filter materials.
- Initial backend cap: `FILTER_GRAPH_OUTPUT_MAX = 32`.
- Output UI remains dynamic; the cap is a compile/runtime validation limit, not a fixed UI layout.
- Use stable integer identifiers for outputs; links follow identifiers, not display names.
- Enforce unique output display names in one active `Filter Output` node by auto-suffixing or validation.
- Keep one active `Filter Output` node per filter material, reusing `NODE_DO_OUTPUT` semantics.
- First implementation stores all output textures at full stage resolution and stage format.
- Per-output resolution scale, format, and typed value sockets are later extensions.

## Risks and Mitigations

- **GPU codegen risk:** Current GPU material code has one `outlink_filter` and one `GPUGraphOutput filter`. Replace this with a filter-output list while preserving the old single-output path as the first/default output.
- **Shader output risk:** MRT would be limited by framebuffer color attachments, often 8. Prefer texture-array/image-store output for up to 32 outputs. Verify fragment-stage image writes across OpenGL/Vulkan/Metal. If image-only fullscreen passes are not supported, keep layer 0 as the framebuffer color attachment and write remaining layers as images.
- **Memory risk:** One RGBA16F 4K output is about 66 MB. Allocate only outputs reachable from active graph roots where practical; otherwise warn that many connected outputs are expensive.
- **Usability risk:** A node with 32 outputs means 64 material input sockets. The cap can be 32, but the comfortable workflow is likely 4-12 outputs. Later add compact interface editing if needed.
- **Alpha risk:** User-facing `Alpha` remains opacity. Internal storage may still use transmittance; apply the same conversion to every output.
- **AOV side-effect risk:** Existing `Output AOV` inside filter materials can reintroduce hidden global writes. In Filter Graph mode, compile this as an error or disable it in the filter material add menu.
- **Sync risk:** Output interfaces must sync from the active material `Filter Output` to all graph `Filter Pass` nodes using that material without breaking existing links.
- **Reuse risk:** The same material can be used by multiple `Filter Pass` graph nodes. Each graph node must have independent output textures and input tables.

## Implementation Plan

### Task 1: Add Filter Output Interface Storage

**Files:**
- Modify: `source/blender/makesdna/DNA_node_types.h`
- Modify: `source/blender/makesrna/intern/rna_nodetree.cc`
- Modify: `source/blender/nodes/shader/nodes/node_shader_output_filter.cc`

**Steps:**
1. Add a storage struct for `ShaderNodeOutputFilter` containing `NodeEeveeFilterGraphSocketItem *outputs`, `outputs_num`, `active_index`, and `next_identifier`.
2. Add blend write/read/free/copy support for the new storage.
3. Update `Filter Output` declaration to create one `Color_<id>` and one `Alpha_<id>` input pair per output item.
4. Add dynamic add/remove/move/rename RNA support using the existing socket-items pattern.
5. Initialize old/no-storage `Filter Output` nodes with one output item named `Image`.

### Task 2: Mirror Material Outputs to Filter Pass Graph Nodes

**Files:**
- Modify: `source/blender/makesdna/DNA_node_types.h`
- Modify: `source/blender/nodes/shader/nodes/node_filter_graph_nodes.cc`
- Modify: `source/blender/nodes/shader/nodes/node_shader_filter_graph_input.cc`

**Steps:**
1. Extend `NodeEeveeFilterGraphFilterMaterial` with a second item list for output items.
2. Keep input items and output items separate; do not reuse `items` for both directions.
3. Generate dynamic `Image_<id>` output sockets on `Filter Pass` from the mirrored output item list.
4. When the material pointer changes, copy the active `Filter Output` interface into the graph node.
5. When the material output interface changes, sync all `Filter Pass` nodes referencing that material.
6. Preserve socket identifiers during rename and reorder; delete links only when the corresponding output item is removed.

### Task 3: Extend GPU Material Filter Codegen

**Files:**
- Modify: `source/blender/gpu/GPU_material.hh`
- Modify: `source/blender/gpu/intern/gpu_node_graph.hh`
- Modify: `source/blender/gpu/intern/gpu_node_graph.cc`
- Modify: `source/blender/gpu/intern/gpu_codegen.cc`
- Modify: `source/blender/gpu/intern/gpu_material.cc`
- Modify: `source/blender/draw/engines/eevee/eevee_shader.cc`

**Steps:**
1. Replace or supplement `outlink_filter` with a list of filter output links keyed by output identifier.
2. Add `GPU_material_output_filter_item(GPUMaterial *, int identifier, GPUNodeLink *)`.
3. Make `ShaderNodeOutputFilter` call this once per output item.
4. Generate one serialized `GPUGraphOutput` per filter output.
5. In EEVEE codegen, emit `nodetree_filter_output_<index>()` functions instead of only `nodetree_filter()`.
6. Keep the old `nodetree_filter()` wrapper for one-output compatibility if useful.

### Task 4: Add Multi-Output Shader Storage

**Files:**
- Modify: `source/blender/draw/engines/eevee/eevee_defines.hh`
- Modify: `source/blender/draw/engines/eevee/eevee_filter_material_shared.hh`
- Modify: `source/blender/draw/engines/eevee/shaders/infos/eevee_filter_material_infos.hh`
- Modify: `source/blender/draw/engines/eevee/shaders/eevee_filter_material_frag.glsl`

**Steps:**
1. Add `FILTER_GRAPH_OUTPUT_MAX 32`.
2. Add output metadata for enabled outputs and target layers if needed.
3. Bind a writable output texture array or equivalent output storage to the filter material shader.
4. Write each reachable output result into its assigned output layer.
5. Apply the same opacity-to-internal-alpha conversion for every output.
6. Ensure unconnected/default output inputs produce predictable black/opaque or existing default values.

### Task 5: Extend Filter Graph Executor

**Files:**
- Modify: `source/blender/draw/engines/eevee/eevee_filter_material.hh`
- Modify: `source/blender/draw/engines/eevee/eevee_filter_material.cc`

**Steps:**
1. Collect reachable output sockets from active `Stage Output` and future `AOV Output` roots.
2. Validate output count against `FILTER_GRAPH_OUTPUT_MAX`.
3. For each Filter Pass invocation, allocate one output texture array with enough layers for needed outputs.
4. Execute the material once per graph node, not once per output.
5. Map each output socket to a `FilterGraphImageHandle` pointing to its output layer or layer view.
6. Keep graph texture lifetime alive until all downstream consumers and final resolves finish.
7. Ensure reused materials in different graph nodes get independent output storage.

### Task 6: AOV Boundary Cleanup

**Files:**
- Modify: `source/blender/draw/engines/eevee/eevee_filter_material.cc`
- Modify: `source/blender/nodes/shader/nodes/node_shader_output_aov.cc`
- Modify: `scripts/startup/bl_ui/node_add_menu_shader.py`

**Steps:**
1. Treat graph-level `AOV Input` as a read-only source.
2. Add or finalize graph-level `AOV Output` as the only path for writing final AOVs from Filter Graph.
3. In Filter Graph mode, report a clear compile error when a filter material contains `ShaderNodeOutputAOV`.
4. Remove or hide `Output AOV` from filter material add menus if the active tree is a graph-driven filter material.

### Task 7: Tests and Validation

**Test scenarios:**
1. Create a filter material with 1, 2, 8, 16, and 32 custom outputs; save, reload, undo, redo.
2. Rename and reorder outputs; existing graph links stay attached to the same identifiers.
3. Delete a middle output; only links to that output are removed.
4. One Filter Pass with two outputs connected to two downstream Filter Pass nodes executes once and produces correct independent images.
5. Same material used by two Filter Pass nodes with different inputs produces independent outputs.
6. Per-output alpha: Alpha 1 is visible/opaque for every output, Alpha 0 is transparent.
7. AOV Input remains unchanged when graph outputs are written to AOV Output later in the graph.
8. Filter material with internal `Output AOV` in graph mode reports a compile error and does not crash.
9. Exceeding 32 outputs reports a node/graph error and does not render a broken graph.
10. Stress test 16 and 32 connected outputs at 1080p and 4K to confirm memory behavior and no GPU validation errors.

**Validation commands after implementation:**
- `git -C blender_5_1_port diff --check`
- Targeted Python smoke scripts under `temp/scripts/` or `test/filter_graph/scripts/`
- From workspace root: `.\build_ninja_sccache_poll.bat install --no-pause`
- Run targeted current-branch Filter Graph smoke tests only; do not run full release tests unless requested.

## Open Questions

- Whether the first implementation computes all declared outputs or only graph-reachable outputs. Recommended: start with graph-reachable allocation; compute masking can be added if codegen complexity is acceptable.
- Whether output layer views can be used everywhere a graph texture handle is consumed. If not, add an explicit layer index to `FilterGraphImageHandle`.
- Whether fragment shader image writes are stable enough across all supported backends. If not, fall back to a small MRT cap or a hybrid first-output framebuffer plus image-store additional outputs.

## Acceptance Criteria

- Users can add arbitrary named outputs to `Filter Output` without fixed names or fixed visible socket count.
- `Filter Pass` exposes matching dynamic image outputs.
- A graph can connect different outputs from one Filter Pass to different downstream passes and AOV outputs.
- The same Filter Pass invocation is not rerun per output.
- AOV input remains read-only and AOV write remains explicit at graph boundary.
- Existing one-output filter materials keep working.
