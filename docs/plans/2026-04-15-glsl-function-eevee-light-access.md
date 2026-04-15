# GLSL Function Eevee Light Access Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Let `GLSL Function` access Eevee direct-light and shadow data in ordinary object materials without exposing raw Eevee internals as the public user API.

**Architecture:** Keep the implementation split across the existing Blender layers instead of turning this into a giant node-only patch. `node_shader_glsl_function.cc` detects and injects the helper workflow, `GPU_material.hh` carries a dedicated material flag, `eevee_shader.cc` exposes the required runtime resources only on supported pipelines, and a dedicated helper GLSL library translates the stable public helper API into Eevee's internal light/shadow functions.

**Tech Stack:** C++, Blender shader nodes, GPU material codegen, Eevee shader dependency system, Python node tests, Python render tests, install-tree Blender validation

---

## Summary

- Do **not** implement this as new sockets, new node properties, or direct public access to `LightData`, `light_buf`, or Eevee shader macros.
- First checkpoint the existing `sampler2D` closure-workflow cleanup on `npr-port-5.1`, then branch from that clean point.
- Support scope for V1:
  - ordinary object materials only
  - Eevee `Deferred + Forward`
  - direct light + shadow only
- Explicitly out of scope for V1:
  - `FILTER`
  - `NPR Tree`
  - `World`
  - probe / indirect / volume lighting

## Branch And Commit Order

### Task 0: Checkpoint Existing GLSL Function Cleanup

**Files:**
- Modify: `blender_5_1_port/blender-5.1-npr-features-and-usage.md`
- Modify: `blender_5_1_port/source/blender/makesrna/intern/rna_nodetree.cc`
- Modify: `blender_5_1_port/source/blender/nodes/intern/shader_nodes_inline.cc`
- Modify: `blender_5_1_port/source/blender/nodes/shader/nodes/node_shader_glsl_function.cc`

**Steps:**
1. Stage only the four files above.
2. Commit with:
   ```bash
   git -C blender_5_1_port commit -m "refactor: unify glsl function sampler2d closure workflow"
   ```
3. Confirm clean working tree:
   ```bash
   git -C blender_5_1_port status --short
   ```
4. Create the feature branch from that clean checkpoint:
   ```bash
   git -C blender_5_1_port switch -c feat/glsl-function-eevee-light-access
   ```

### Task 1: Add the Dedicated Eevee Light Access Flag

**Files:**
- Modify: `blender_5_1_port/source/blender/nodes/shader/nodes/node_shader_glsl_function.cc`
- Modify: `blender_5_1_port/source/blender/gpu/GPU_material.hh`
- Modify: `blender_5_1_port/source/blender/draw/engines/eevee/eevee_shader.cc`

**Required result:**
- `GLSL Function` recognizes the helper API as a special workflow.
- A new dedicated `GPU_MATFLAG_GLSL_LIGHT_ACCESS` is introduced.
- `eevee_light_data` and `eevee_shadow_data` are injected only for `MAT_PIPE_DEFERRED` and `MAT_PIPE_FORWARD`.
- Do **not** reuse `GPU_MATFLAG_SHADER_INFO`.

**Commit:**
```bash
git -C blender_5_1_port commit -m "feat: add glsl function eevee light access flag"
```

### Task 2: Add the Helper Library and Inject It Into GLSL Function

**Files:**
- Create: `blender_5_1_port/source/blender/gpu/shaders/material/gpu_shader_material_glsl_light_access.glsl`
- Modify: `blender_5_1_port/source/blender/gpu/CMakeLists.txt`
- Modify: `blender_5_1_port/source/blender/nodes/shader/nodes/node_shader_glsl_function.cc`
- Modify: `blender_5_1_port/source/blender/gpu/intern/gpu_shader_dependency.cc`

**Required result:**
- The public helper API is defined in a dedicated GLSL file.
- `GLSL Function` injects that helper source into generated library code.
- Helper source is resolved together with its Eevee dependencies instead of exposing internal names directly to users.
- `gpu_shader_dependency_get_source()` must never crash on a missing source entry; return an empty string after logging the failure.

**Public helper API for V1:**
```glsl
struct GLSLLight {
  bool valid;
  uint index;
  int type;
  vec3 color;
  vec3 vector;
  vec3 position;
  vec3 direction;
  float distance;
  float diffuse_power;
  float specular_power;
  float attenuation;
};

int glsl_light_count()
GLSLLight glsl_light_get(int light_index)
float glsl_light_shadow(int light_index, vec3 shading_normal)
float glsl_light_diffuse_attenuation(int light_index, vec3 shading_normal, vec3 view_vector)
float glsl_light_specular_attenuation(
    int light_index, vec3 shading_normal, vec3 view_vector, float roughness)

// Public type constants:
GLSL_LIGHT_TYPE_INVALID
GLSL_LIGHT_TYPE_SUN
GLSL_LIGHT_TYPE_POINT
GLSL_LIGHT_TYPE_SPOT
GLSL_LIGHT_TYPE_AREA_RECT
GLSL_LIGHT_TYPE_AREA_ELLIPSE
```

**Commit:**
```bash
git -C blender_5_1_port commit -m "feat: add glsl function eevee light helper library"
```

### Task 3: Add Test Coverage and Lock the Interface

**Files:**
- Modify: `blender_5_1_port/tests/python/bl_node_glsl_function.py`
- Modify: `blender_5_1_port/tests/python/npr/test_filter_glsl_function_render.py`
- Create: `blender_5_1_port/tests/python/npr/test_glsl_function_light_access_render.py`

**Required result:**
- Existing `GLSL Function` Python tests match the current `sampler2D` boundary model.
- The new helper API is covered by a node smoke test.
- Render coverage includes:
  - deferred direct-light response
  - forward direct-light response
  - multi-light accumulation
  - shadow visibility response
- Keep current `FILTER` render coverage passing after the `sampler2D` cleanup.

**Commit:**
```bash
git -C blender_5_1_port commit -m "test: cover glsl function eevee light access"
```

## Verification

Run verification after each major phase:

```bash
cmd /c build_ninja_sccache.bat
cmd /c build_ninja_sccache.bat install
```

Use the install-tree Blender only:

```bash
E:\blender_bulid_test\blender_npr_bulid\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe --background --factory-startup --python blender_5_1_port\tests\python\bl_node_glsl_function.py
E:\blender_bulid_test\blender_npr_bulid\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe --background --factory-startup --python blender_5_1_port\tests\python\npr\test_glsl_function_light_access_render.py
```

If Windows Ninja unity misses included source changes:

- identify the owning unity object
- force that unity object to rebuild before trusting the install result

## Documentation Finish

### Task 4: Document the Helper Workflow

**Files:**
- Modify: `blender_5_1_port/docs/glsl-function-node-conversion-guide.md`
- Modify: `blender_5_1_port/blender-5.1-npr-features-and-usage.md`

**Required result:**
- Document the supported helper API names.
- Document that `GLSLLight.attenuation` is still the raw surface attenuation term, while `glsl_light_diffuse_attenuation(...)` is the diffuse-aligned aggregate helper for users who want brightness closer to Eevee's default `Diffuse BSDF`.
- Document that `glsl_light_specular_attenuation(...)` is the specular-aligned aggregate helper for users who want highlight intensity closer to Eevee's default GGX reflection path, while material-side Fresnel / IOR / tint / metallic remain outside the helper.
- State clearly that V1 only supports ordinary object materials.
- State clearly that `FILTER`, `NPR Tree`, `World`, probe lighting, and volume lighting are unsupported.
- Keep the description narrow: this is Eevee direct-light helper access, not public `LightData` access.

**Commit:**
```bash
git -C blender_5_1_port commit -m "docs: document glsl function eevee light helpers"
```

## Assumptions

- Checkpoint commit is only for cleaning the worktree and preserving the current `sampler2D` cleanup.
- The feature branch is created from the local clean checkpoint, not rebased to the remote tip first.
- V1 helper support stays object-material-only even if helper stubs compile on unsupported domains.
- Temporary debug prints added during bring-up should be removed before the final doc commit unless they are still required to diagnose an unresolved blocker.
