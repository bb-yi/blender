# SDF Nodes Design

**Goal:** Port Goo Engine's `Sdf Primitive` and `Sdf Operator` shader nodes into the Blender 5.1 NPR port with behavior that is as close to Goo as practical.

**Scope**
- Add `ShaderNodeSdfPrimitive` with Goo's full primitive mode set.
- Add `ShaderNodeSdfOp` with Goo's full operator mode set.
- Port the shared GPU shader support used by both nodes.
- Expose both nodes in the shader add menu.

**Architecture**
- Keep the user-facing behavior close to Goo, but implement the nodes in the modern 5.1 node style already used in this branch.
- Use DNA/RNA storage for enum-backed node settings so the nodes are saved in `.blend` files and exposed cleanly in the UI.
- Use GPU-only evaluation for the first port, matching the existing Eevee-focused custom node pattern in this branch.

**Data Model**
- `NodeSdfPrimitive`
  - Inherits `NodeTexBase` so it can reuse texture mapping and default coordinate handling.
  - Stores `mode` and `invert`.
- `NodeSdfOp`
  - Stores `operation` and `invert`.

**Node Behavior**
- `Sdf Primitive`
  - One node hosts all primitive variants.
  - Uses a shared socket layout and updates socket visibility and labels based on the selected primitive.
  - Outputs a single float distance value.
- `Sdf Operator`
  - One node hosts all operator variants.
  - Uses shared float inputs and dynamically relabels sockets for the active operation.
  - Outputs a single float distance value, except for Goo-compatible value-style operations that still reuse the same output socket.

**Files to Touch**
- DNA/RNA and IDs:
  - `source/blender/makesdna/DNA_node_types.h`
  - `source/blender/blenkernel/BKE_node_legacy_types.hh`
  - `source/blender/makesrna/RNA_enum_items.hh`
  - `source/blender/makesrna/intern/rna_nodetree.cc`
- Node registration:
  - `source/blender/nodes/shader/node_shader_register.hh`
  - `source/blender/nodes/shader/node_shader_register.cc`
  - `source/blender/nodes/shader/CMakeLists.txt`
- Node implementations:
  - `source/blender/nodes/shader/nodes/node_shader_sdf_primitive.cc`
  - `source/blender/nodes/shader/nodes/node_shader_sdf_op.cc`
- GPU shaders:
  - `source/blender/gpu/CMakeLists.txt`
  - `source/blender/gpu/shaders/material/gpu_shader_material_sdf_util.glsl`
  - `source/blender/gpu/shaders/material/gpu_shader_material_sdf_primitive.glsl`
  - `source/blender/gpu/shaders/material/gpu_shader_material_sdf_op.glsl`
- UI:
  - `scripts/startup/bl_ui/node_add_menu_shader.py`

**Compatibility Notes**
- Reuse Goo's node IDs where possible because they are currently unused in this branch.
- Keep Goo's enum ordering for storage stability and easier future diffing against Goo.

**Validation**
- Build incrementally in `build_windows_x64_vc17_Release_5_1_port_clean`.
- Verify both nodes appear in the shader editor add menu.
- Verify several representative primitive and operator modes compile and evaluate without shader errors.
