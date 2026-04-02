# GLSL Function Node Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

## Current Implementation Status

- V1 node registration, RNA, UI, dynamic sockets, and Eevee GPU path are implemented on `feature/glsl-function-node`.
- The original blocker was that custom wrapper functions were emitted into graph code, but their GLSL source never entered the final Eevee nodetree shader. The fix was to carry a material-local generated-source key through GPU codegen and inline the referenced GLSL blocks into `eevee_nodetree_vert_lib.glsl` / `eevee_nodetree_frag_lib.glsl` during `material_create_info_amend()`.
- Wrapper function names now use unsigned hexadecimal signature hashes so they always produce valid GLSL identifiers.
- Verified locally in `install_windows_x64_vc17_Release_5_1_port_clean` with:
  - a surface-path smoke render using a `float custom_gain(float value)` function
  - a displacement-path smoke render using a `float displacement_amount(float value)` function

**Goal:** Add an Eevee-first `GLSL Function` shader node that lets artists call user-authored GLSL functions from a Blender `Text` data-block inside material and NPR shader graphs without rebuilding Blender.

**Architecture:** V1 should avoid trying to globally register runtime GLSL into Blender's static GPU material library. Instead, the node stores a `Text` data-block reference plus a selected exported function, parses a restricted function signature, synchronizes node sockets from that signature, and injects only the selected function plus its helper closure into Eevee's per-material generated shader sources. This keeps user GLSL local to the compiling material, reduces global-state risk, and matches how Eevee already appends generated sources and graph sub-functions.

**Tech Stack:** C++, Blender DNA/RNA, Blender shader nodes, Blender `Text` datablocks, GPU material codegen, Eevee generated shader sources, GLSL, Python NPR smoke tests

---

## Current State Snapshot

- `GPU_stack_link()` cannot call arbitrary user function names at runtime. It resolves names from the static GPU material function library built during startup.
- `gpu_shader_dependency_init()` builds the global source/function dictionaries from baked shader source lists once. There is no clean existing public runtime path for registering user-authored material functions on demand.
- Eevee already supports per-material source injection through `generated_sources` and serializes graph-local helper functions through `material_functions`.
- `ShaderNodeScript` is OSL-oriented and is not a drop-in solution for Eevee GLSL material functions.
- The existing shader node system already supports dynamic socket visibility and storage-backed node UI, so the editor side of this feature is feasible once the codegen route is settled.

## Recommended V1 Scope

- Source comes from a Blender `Text` data-block only
- One selected exported function per node
- One return value output per node
- Input parameters are `in` only in V1
- Same-text helper function calls are allowed
- Supported boundary types in V1:
  - `float`
  - `vec2`
  - `vec3`
  - `vec4`
  - `color` as a node-facing alias mapped to GLSL `vec4`
- Supported targets in V1:
  - Eevee object materials
  - NPR trees that compile through the same GPU material path

## Explicitly Out of Scope for V1

- External file loading
- `out` and `inout` parameters
- Multiple return outputs
- Custom structs, arrays, and samplers as function boundary types
- Recursive or cyclic function graphs
- Cycles support
- Filter / World / Light tree specific polish

## Architectural Choice

There are two realistic implementation routes.

### Route A: Extend the Global GPU Dependency Library

Add runtime registration to `gpu_shader_dependency` so user GLSL functions become first-class GPU material library functions.

Pros:

- Closer to how built-in material nodes work
- Could make user functions callable through existing `GPU_stack_link()` patterns

Cons:

- Adds global mutable state to a startup-initialized system
- Harder to invalidate safely when `Text` datablocks change
- Higher risk of name collisions and lifetime bugs
- More invasive for a first implementation

### Route B: Inject User GLSL Per Material

Treat the selected function and its helper closure as per-material generated sources and wrappers, appended during Eevee material create-info generation.

Pros:

- Localizes user code to the material being compiled
- Avoids mutating the global startup shader dependency registry
- Fits Eevee's existing `generated_sources` and `material_functions` pipeline
- Easier to hash, isolate, and invalidate when a single node changes

Cons:

- Requires custom plumbing for the node instead of pure `GPU_stack_link()`
- Needs custom wrapper/function serialization

### Decision

Use **Route B** first.

If Route B proves too narrow later, Route A can be explored as a second-phase refactor after the user-facing behavior is stable.

## File Map

### Files very likely to modify

- Modify: `source/blender/makesdna/DNA_node_types.h`
- Modify: `source/blender/blenkernel/BKE_node_legacy_types.hh`
- Modify: `source/blender/makesrna/RNA_enum_items.hh`
- Modify: `source/blender/makesrna/intern/rna_nodetree.cc`
- Modify: `source/blender/nodes/shader/CMakeLists.txt`
- Modify: `source/blender/nodes/shader/node_shader_register.hh`
- Modify: `source/blender/nodes/shader/node_shader_register.cc`
- Create: `source/blender/nodes/shader/nodes/node_shader_glsl_function.cc`
- Modify: `scripts/startup/bl_ui/node_add_menu_shader.py`
- Modify: `source/blender/gpu/GPU_material.hh`
- Modify: `source/blender/gpu/intern/gpu_material.cc`
- Modify: `source/blender/gpu/intern/gpu_codegen.hh`
- Modify: `source/blender/gpu/intern/gpu_codegen.cc`
- Modify: `source/blender/draw/engines/eevee/eevee_shader.cc`

### Files that may need changes if the first route is insufficient

- Modify: `source/blender/gpu/intern/gpu_node_graph.hh`
- Modify: `source/blender/gpu/intern/gpu_node_graph.cc`
- Modify: `source/blender/gpu/intern/gpu_shader_dependency.cc`
- Modify: `source/blender/gpu/intern/gpu_shader_dependency_private.hh`

### Tests to add locally first

- Create: `tests/python/npr/test_glsl_function_smoke.py`
- Create: `tests/python/npr/test_glsl_function_render.py`

### Reference files

- `source/blender/nodes/shader/nodes/node_shader_script.cc`
- `source/blender/nodes/shader/nodes/node_shader_sdf_vector_op.cc`
- `source/blender/nodes/shader/nodes/node_shader_vector_math.cc`
- `source/blender/gpu/intern/gpu_node_graph.cc`
- `source/blender/gpu/intern/gpu_material.cc`
- `source/blender/gpu/intern/gpu_codegen.cc`
- `source/blender/draw/engines/eevee/eevee_shader.cc`

## Task 1: Prove the Material-Local GLSL Injection Path

**Files:**

- Modify: `source/blender/gpu/GPU_material.hh`
- Modify: `source/blender/gpu/intern/gpu_material.cc`
- Modify: `source/blender/gpu/intern/gpu_codegen.hh`
- Modify: `source/blender/gpu/intern/gpu_codegen.cc`
- Modify: `source/blender/draw/engines/eevee/eevee_shader.cc`

**Step 1:** Add a minimal per-material generated-source carrier that can survive from node compilation to Eevee shader create-info generation.

**Step 2:** Inject a trivial hardcoded GLSL helper into one material as a spike.

**Step 3:** Confirm Eevee can compile and call that helper without global dependency registration.

**Step 4:** Keep the abstraction, remove the hardcoded spike behavior, and preserve only the reusable plumbing.

## Task 2: Lock the Node ID and Persistent Storage

**Files:**

- Modify: `source/blender/makesdna/DNA_node_types.h`
- Modify: `source/blender/blenkernel/BKE_node_legacy_types.hh`

**Step 1:** Reserve a shader node ID for `GLSL Function`.

**Step 2:** Add a storage struct for node-owned metadata such as:
- selected function name
- source mode
- cached parse status / flags
- stable socket signature hash or version fields if needed

**Step 3:** Reuse `node->id` for the `Text` datablock pointer so the feature follows Blender's existing datablock ownership patterns.

## Task 3: Expose RNA and Node Properties

**Files:**

- Modify: `source/blender/makesrna/RNA_enum_items.hh`
- Modify: `source/blender/makesrna/intern/rna_nodetree.cc`

**Step 1:** Add RNA for the node's source mode and function-selection state.

**Step 2:** Expose the `Text` datablock selector and selected function name.

**Step 3:** Add a lightweight refresh/reparse trigger path so the node can rebuild its sockets when the source changes.

**Step 4:** Add readonly parse-status text or enum state if it helps the UI report errors cleanly.

## Task 4: Register the Node and Add Menu Entry

**Files:**

- Modify: `source/blender/nodes/shader/CMakeLists.txt`
- Modify: `source/blender/nodes/shader/node_shader_register.hh`
- Modify: `source/blender/nodes/shader/node_shader_register.cc`
- Create: `source/blender/nodes/shader/nodes/node_shader_glsl_function.cc`
- Modify: `scripts/startup/bl_ui/node_add_menu_shader.py`

**Step 1:** Create the new shader node source file and basic node type registration.

**Step 2:** Add node init, copy, free, draw-buttons, and update callbacks.

**Step 3:** Expose the node in the shader add menu so it is discoverable in normal workflows.

**Step 4:** Restrict or label unsupported tree types clearly if V1 is not enabled everywhere.

## Task 5: Implement the Restricted GLSL Signature Parser

**Files:**

- Modify: `source/blender/nodes/shader/nodes/node_shader_glsl_function.cc`

**Step 1:** Parse a restricted exported-function signature from the selected `Text` datablock.

**Step 2:** Support V1 boundary syntax only:
- function return type
- function name
- comma-separated `in` parameters
- supported scalar/vector/color boundary types

**Step 3:** Reject unsupported syntax early with clear node errors instead of allowing obscure shader compile failures later.

**Step 4:** Normalize the parsed signature into a small node-local metadata representation.

## Task 6: Synchronize Dynamic Sockets from the Parsed Signature

**Files:**

- Modify: `source/blender/nodes/shader/nodes/node_shader_glsl_function.cc`

**Step 1:** Create sockets from the parsed signature:
- one output socket from the function return type
- one input socket per supported parameter

**Step 2:** Generate stable socket identifiers so links survive harmless source edits when parameter order and meaning do not change.

**Step 3:** Rebuild sockets safely when the signature changes.

**Step 4:** Hide or remove invalid sockets and publish a clear node warning when the function cannot currently be represented.

## Task 7: Build the Helper-Closure and Cycle Checks

**Files:**

- Modify: `source/blender/nodes/shader/nodes/node_shader_glsl_function.cc`
- Modify if needed: `source/blender/gpu/GPU_material.hh`
- Modify if needed: `source/blender/gpu/intern/gpu_material.cc`

**Step 1:** Scan the selected text for same-source helper function definitions.

**Step 2:** Build a dependency closure for the selected exported function.

**Step 3:** Reject recursion and cyclic helper graphs before shader compilation.

**Step 4:** Include only the needed helper subset in the final generated source to keep material code size under control.

## Task 8: Emit the Node's GPU Call Path

**Files:**

- Modify: `source/blender/nodes/shader/nodes/node_shader_glsl_function.cc`
- Modify: `source/blender/gpu/GPU_material.hh`
- Modify: `source/blender/gpu/intern/gpu_material.cc`
- Modify: `source/blender/gpu/intern/gpu_codegen.hh`
- Modify: `source/blender/gpu/intern/gpu_codegen.cc`
- Modify: `source/blender/draw/engines/eevee/eevee_shader.cc`

**Step 1:** Add a material-local record that stores:
- unique generated function/library names
- serialized GLSL source text
- dependency names if needed
- signature metadata needed for wrapper emission

**Step 2:** In the node GPU function, gather input links and register the selected GLSL function with the material-local generated-source list.

**Step 3:** Emit a unique wrapper call that adapts Blender socket values to the selected GLSL function signature and writes the return value back into the node graph.

**Step 4:** Ensure name hashing prevents collisions between multiple `GLSL Function` nodes inside the same material.

## Task 9: Add User-Facing Validation and Safe Fallbacks

**Files:**

- Modify: `source/blender/nodes/shader/nodes/node_shader_glsl_function.cc`
- Modify if needed: `source/blender/gpu/intern/gpu_material.cc`

**Step 1:** Publish clear node errors for:
- missing text datablock
- missing function
- unsupported type
- recursive helper graph
- empty source

**Step 2:** Make invalid nodes fall back to a safe zero value instead of poisoning the whole graph when possible.

**Step 3:** Keep shader compile failures readable by preserving the generated function names and source boundaries in emitted GLSL.

## Task 10: Add Smoke Tests and a Render Test

**Files:**

- Create: `tests/python/npr/test_glsl_function_smoke.py`
- Create: `tests/python/npr/test_glsl_function_render.py`

**Step 1:** Add a smoke test that creates the node, assigns a `Text` datablock, selects a function, and confirms sockets update as expected.

**Step 2:** Add a render test with a trivial function such as a scalar multiply or color invert and verify the material compiles in Eevee.

**Step 3:** Add at least one negative test for unsupported syntax or a recursive helper graph.

**Step 4:** Keep these tests local until the feature stabilizes enough to decide what should be committed.

## Task 11: Incremental Build and Install Verification

**Files:**

- Modify if needed based on compile errors

**Step 1:** Build only in `build_windows_x64_vc17_Release_5_1_port_clean`.

**Step 2:** Fix compile, RNA registration, or shader-generation errors.

**Step 3:** Copy the verified build output to `install_windows_x64_vc17_Release_5_1_port_clean`.

**Step 4:** Verify the node appears in the add menu, parses a sample function, and compiles in Blender without a full clean rebuild.
