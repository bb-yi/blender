# Filter Cryptomatte Mask Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a native Eevee `Filter`-domain mask node that can generate fast object masks in filter materials using Eevee's existing Cryptomatte data, with collection masks added as a second phase.

**Architecture:** Reuse Eevee's existing Cryptomatte render-pass generation instead of inventing a new masking buffer. V1 should add a dedicated `Filter Cryptomatte Mask` node for object masks, auto-request the needed Cryptomatte pass when any filter material uses the node, and sample the Cryptomatte texture in the filter shader. V2 should extend the same node to collections by expanding a collection into a list of object hashes on the CPU side and matching against the object Cryptomatte layer in the shader.

**Tech Stack:** Blender shader nodes in C++, Eevee render-pass plumbing, GLSL shader nodes, GPU material runtime flags/buffers, Python NPR tests.

---

## Current State Snapshot

- `Filter` materials already scan their node trees to auto-request some scene-derived inputs, but only for depth / normal / position. See `source/blender/draw/engines/eevee/eevee_filter_material.cc` and `source/blender/draw/engines/eevee/eevee_filter_material.hh`.
- The actual filter draw pass currently binds only `scene_color_tx`, `rp_color_tx`, `rp_value_tx`, `depth_tx`, and the filter object info buffer. It does not bind any Cryptomatte texture.
- Eevee already allocates and populates Cryptomatte data for object / asset / material passes. The render buffers and film system already contain the plumbing.
- `AOV Input` is already supported at shader/runtime level for `MAT_FILTER`, but the node UI entry was hidden by an overly strict poll function. There is currently one uncommitted local change in `source/blender/nodes/shader/nodes/node_shader_input_aov.cc` that exposes `AOV Input` to `Filter` trees.

## Recommendation

Implement this in phases instead of trying to clone the full compositor Cryptomatte feature set in one shot.

### Recommended V1

Add a `Filter Cryptomatte Mask` node that supports:

- Object mask
- Single float output `Mask`
- Optional invert toggle if needed later, but do not add it in the first pass unless it is essentially free

Why this is the best first step:

- It matches the user's immediate need: quickly masking specific objects in `Filter Materials`
- It reuses Eevee's existing Cryptomatte pass instead of introducing a new per-object mask buffer
- The shader cost is low for a single selected object
- It avoids the much more complex compositor-style UI and metadata flow

### Recommended V2

Extend the same node to support:

- Collection mask

Implementation strategy for collection:

- Expand the selected collection into object hashes on the CPU side
- Upload those hashes in a compact buffer
- Match the pixel's object Cryptomatte samples against that uploaded hash list

This is much simpler than inventing a dedicated collection Cryptomatte render pass.

### Not Recommended for First Pass

- Full compositor-like Cryptomatte picker / manifest UI in the Filter editor
- A brand new collection render pass
- Multi-layer material / asset / collection support in the very first implementation

Those are all valid future additions, but they are not the shortest path to a useful feature.

## Architectural Choice

There are two realistic implementation routes.

## Execution Stage Update

The recommended V1 execution point is now a dedicated Eevee filter stage placed after forward
rendering and before the post-processing chain. In code terms this is a new `Before PostFX` stage,
which runs before motion blur, depth of field, and final composite handling.

Why this stage is preferred:

- It sees a more complete scene result than `Before Volume Fog`
- It still keeps object-mask-driven color changes ahead of depth of field, so the final image can
  blur naturally instead of cutting with a sharp mask afterward
- It avoids depending on final film accumulation output, which is too late for per-sample mask work

### Route A: Fast Practical Route

Use Eevee's existing Cryptomatte render pass in the Filter shader and compare against selected hashes directly.

Pros:

- Smallest code surface
- Fast enough for object masks
- Easy to validate

Cons:

- Collection masks need an extra uploaded hash list
- Less feature-rich than the compositor Cryptomatte node UI

### Route B: Full Compositor-Like Route

Expose Eevee film Cryptomatte accumulation layers to the Filter stage and reproduce compositor-style multi-rank matte accumulation behavior.

Pros:

- Closest behavior to compositor Cryptomatte
- Best future-proofing for complex selection cases

Cons:

- Significantly more plumbing
- Higher texture memory usage
- More shader work
- More likely to create regressions in the Filter pipeline

### Decision

Use **Route A** first.

If later we need near-compositor parity for multiple IDs or more exact behavior across many ranks, upgrade from Route A incrementally instead of starting there.

## File Map

### Files very likely to modify for V1

- Modify: `source/blender/draw/engines/eevee/eevee_filter_material.hh`
- Modify: `source/blender/draw/engines/eevee/eevee_filter_material.cc`
- Modify: `source/blender/draw/engines/eevee/eevee_film.cc`
- Modify: `source/blender/draw/engines/eevee/shaders/infos/eevee_filter_material_infos.hh`
- Modify: `source/blender/draw/engines/eevee/shaders/eevee_filter_material_frag.glsl`
- Modify: `source/blender/gpu/GPU_material.hh`
- Modify: `source/blender/gpu/intern/gpu_material.cc`
- Create: `source/blender/gpu/shaders/material/gpu_shader_material_filter_cryptomatte.glsl`
- Modify: `source/blender/nodes/shader/CMakeLists.txt`
- Modify: `source/blender/nodes/shader/node_shader_register.hh`
- Modify: `source/blender/nodes/shader/node_shader_register.cc`
- Create: `source/blender/nodes/shader/nodes/node_shader_filter_cryptomatte_mask.cc`
- Modify: `source/blender/blenkernel/BKE_node_legacy_types.hh`
- Modify: `source/blender/makesrna/intern/rna_nodetree.cc`

### Files likely to modify for V2 collection support

- Modify: `source/blender/draw/engines/eevee/eevee_filter_material_shared.hh`
- Modify: `source/blender/draw/engines/eevee/eevee_filter_material.hh`
- Modify: `source/blender/draw/engines/eevee/eevee_filter_material.cc`
- Modify: `source/blender/gpu/GPU_material.hh`
- Modify: `source/blender/gpu/intern/gpu_material.cc`

### Useful reference files

- `source/blender/nodes/shader/nodes/node_shader_filter_object_info.cc`
- `source/blender/draw/engines/eevee/eevee_filter_material.cc`
- `source/blender/compositor/shaders/compositor_cryptomatte_matte.glsl`
- `source/blender/draw/engines/eevee/eevee_renderbuffers.cc`
- `source/blender/draw/engines/eevee/eevee_film.cc`
- `source/blender/draw/engines/eevee/shaders/eevee_film_cryptomatte_post_comp.glsl`

## Task 1: Add the Node Skeleton

**Files:**

- Create: `source/blender/nodes/shader/nodes/node_shader_filter_cryptomatte_mask.cc`
- Modify: `source/blender/nodes/shader/CMakeLists.txt`
- Modify: `source/blender/nodes/shader/node_shader_register.hh`
- Modify: `source/blender/nodes/shader/node_shader_register.cc`
- Modify: `source/blender/blenkernel/BKE_node_legacy_types.hh`
- Modify: `source/blender/makesrna/intern/rna_nodetree.cc`

**Step 1: Write the failing API/UI smoke test**

- Add a Python NPR test that creates a `Filter` material and confirms the new node can be created in a filter node tree.

**Step 2: Register the node**

- Add a new shader node type, likely named `ShaderNodeFilterCryptomatteMask`
- Restrict it to `filter_eevee_shader_nodes_poll`
- Add it to the Add menu for Filter materials

**Step 3: Keep V1 node properties minimal**

- Source type: object only in the first pass
- Selected datablock pointer: `Object`
- Output: `Mask`

**Step 4: Run the smoke test**

- Confirm the node is visible and constructible

## Task 2: Track Cryptomatte Usage in GPUMaterial

**Files:**

- Modify: `source/blender/gpu/GPU_material.hh`
- Modify: `source/blender/gpu/intern/gpu_material.cc`
- Modify: `source/blender/nodes/shader/nodes/node_shader_filter_cryptomatte_mask.cc`

**Step 1: Write the failing expectation**

- Add a small test or assertion path so we can verify the filter material knows it requires object Cryptomatte.

**Step 2: Add runtime tracking**

- Add a light-weight flag or descriptor list to `GPUMaterial` similar in spirit to `filter_object_infos`
- V1 only needs to record that the material requests object Cryptomatte

**Step 3: Connect the node GPU function**

- In the node GPU function, mark the material as using object Cryptomatte
- Pass the encoded target hash as a constant into the shader link

**Step 4: Verify no unrelated shaders regress**

- Rebuild just the affected target
- Confirm existing filter materials still compile

## Task 3: Auto-Enable Cryptomatte Passes for Filter Materials

**Files:**

- Modify: `source/blender/draw/engines/eevee/eevee_filter_material.hh`
- Modify: `source/blender/draw/engines/eevee/eevee_filter_material.cc`
- Modify: `source/blender/draw/engines/eevee/eevee_film.cc`

**Step 1: Add usage flags to `FilterMaterialModule`**

- Mirror the existing `uses_scene_depth_`, `uses_scene_normal_`, `uses_scene_position_` pattern
- Add at least `uses_cryptomatte_object_`

**Step 2: Fill those flags during `init()` / sync**

- Detect whether any valid filter material compiled with the new requirement

**Step 3: Auto-request the render pass**

- In `eevee_film.cc`, OR in `EEVEE_RENDER_PASS_CRYPTOMATTE_OBJECT` when any filter material needs it
- Keep this behavior symmetrical with the existing normal / position auto-enable logic

**Step 4: Verify the pass is allocated**

- Confirm the relevant texture allocation path becomes active

## Task 4: Bind Cryptomatte to the Filter Shader

**Files:**

- Modify: `source/blender/draw/engines/eevee/shaders/infos/eevee_filter_material_infos.hh`
- Modify: `source/blender/draw/engines/eevee/shaders/eevee_filter_material_frag.glsl`
- Modify: `source/blender/draw/engines/eevee/eevee_filter_material.cc`
- Create: `source/blender/gpu/shaders/material/gpu_shader_material_filter_cryptomatte.glsl`

**Step 1: Add the texture binding**

- Bind the needed Cryptomatte texture into the filter shader create-info and render pass setup

**Step 2: Implement the shader helper**

- Read the relevant Cryptomatte layer(s)
- Compare ranks against the encoded target hash
- Sum matched coverage into a single `Mask` output

**Step 3: Link the node**

- Expose a `node_filter_cryptomatte_mask` GLSL function through the regular GPU node stack

**Step 4: Clamp output behavior**

- Ensure the result is always in `[0, 1]`

## Task 5: Add a Real Filter Render Test

**Files:**

- Create: `tests/python/npr/test_filter_cryptomatte_mask.py`
- Optionally create: `tests/files/render/render_layer/...` only if a reference image is genuinely needed

**Step 1: Build a tiny scene in Python**

- Two objects with clearly different colors
- One filter material that isolates one object using the new mask

**Step 2: Render and assert**

- Check that the masked object differs from the unmasked object in a deterministic way

**Step 3: Add a negative case**

- Confirm a filter material without the node still renders normally

**Step 4: Manual validation**

- Verify in viewport render preview that the node updates live enough for practical use

## Task 6: V2 Collection Mask

**Files:**

- Modify: `source/blender/nodes/shader/nodes/node_shader_filter_cryptomatte_mask.cc`
- Modify: `source/blender/gpu/GPU_material.hh`
- Modify: `source/blender/gpu/intern/gpu_material.cc`
- Modify: `source/blender/draw/engines/eevee/eevee_filter_material_shared.hh`
- Modify: `source/blender/draw/engines/eevee/eevee_filter_material.hh`
- Modify: `source/blender/draw/engines/eevee/eevee_filter_material.cc`
- Modify: `source/blender/gpu/shaders/material/gpu_shader_material_filter_cryptomatte.glsl`

**Step 1: Extend node source mode**

- Add `Collection` mode

**Step 2: Expand collection members**

- Resolve the chosen collection to object members during sync
- Convert members to encoded object hashes

**Step 3: Upload compact lookup data**

- Keep the initial implementation bounded
- Prefer a fixed-size or capped buffer over unbounded dynamic allocations in the first version

**Step 4: Shader matching**

- Compare the pixel's object Cryptomatte ranks against the uploaded hash list
- Return the summed coverage for any match

**Step 5: Test small and medium collections**

- Include one case with a few objects
- Include one case that verifies behavior when the collection is empty

## Performance Notes

### V1 Object Mask

- Cost is low and should be acceptable for Filter materials
- The dominant cost is a few extra texture fetches from the Cryptomatte texture plus a handful of comparisons
- This should be far cheaper than introducing a separate object-ID masking render pipeline

### V2 Collection Mask

- Cost scales with the number of objects in the collection
- Tens of objects should be fine
- Hundreds may still be acceptable in many scenes
- Thousands will become a poor fit for a per-pixel post-process loop

### Memory Impact

- Enabling Cryptomatte has a real memory cost because Eevee stores floating-point ID/coverage data
- The raw render buffer is already `RGBA32F`
- Higher `cryptomatte_levels` increases the size of the accumulated array texture
- This is still preferable to inventing a second parallel masking system

## Risks

- The main risk is choosing the wrong Cryptomatte texture stage for Filter sampling
- If the filter shader samples too early, results may differ from compositor expectations
- If collection support is added without a cap, shader cost can grow too much
- If the auto-enable logic is incomplete, users will get confusing all-black masks

## Validation Checklist

- New node appears in `Filter` Add menu
- Existing filter materials still compile
- Object mask works without manually enabling Cryptomatte in the View Layer UI
- Toggling the node changes the filtered result immediately enough for workflow use
- No regression in existing `Filter Object Info`, `Scene Color`, or `AOV Input`

## Local Context To Preserve

- There is one uncommitted adjacent fix in `source/blender/nodes/shader/nodes/node_shader_input_aov.cc` that changes `AOV Input` from `npr_shader_nodes_poll` to `filter_or_npr_eevee_shader_nodes_poll`
- That fix is not the Cryptomatte feature itself, but it is relevant because AOVs are a current workaround path for some filter masks

## Suggested Commit Breakdown

- `feat(filter): add filter cryptomatte mask node skeleton`
- `feat(filter): auto-enable cryptomatte pass for filter materials`
- `feat(filter): sample cryptomatte in filter shaders`
- `test(filter): add filter cryptomatte mask coverage tests`
- `feat(filter): add collection mode for filter cryptomatte mask`

## Suggested Test Commands

- `ctest -C Release -R filter_object_info_updates --test-dir build_windows_x64_vc17_Release_5_1_port_clean -V`
- `ctest -C Release -R goo_scene_color_position_filter --test-dir build_windows_x64_vc17_Release_5_1_port_clean -V`
- `ctest -C Release -R filter_cryptomatte_mask --test-dir build_windows_x64_vc17_Release_5_1_port_clean -V`

## Handoff

When resuming this work:

1. Decide whether to ship V1 as object-only or object+collection in one go. Object-only is recommended.
2. Keep the current uncommitted `AOV Input` UI fix in mind and either commit it separately or leave it out of the Cryptomatte change-set.
3. Implement the node first, then pass auto-enable, then shader sampling, then tests.
