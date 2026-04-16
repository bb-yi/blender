# GLSL Function Friendly Light Struct Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Redefine `GLSLLight` as a single friendly per-light API for custom lighting models, while keeping per-light iteration and shadow queries intact.

**Architecture:** Keep the existing cross-layer wiring for Eevee light/shadow access, but change the public helper surface from raw internal light terms to a model-neutral friendly struct. `gpu_shader_material_glsl_light_access.glsl` owns the new struct semantics, `node_shader_glsl_function.cc` narrows the allowed helper names and rejects removed high-level helpers, and Python tests/docs are updated to match the new public contract.

**Tech Stack:** C++, Blender shader nodes, GPU material helper GLSL, Eevee light evaluation helpers, Python node tests, Python render tests, install-tree Blender validation

---

## Summary

- Replace the current raw-style `GLSLLight` fields with a single friendly struct aimed at custom Lambert/toon/Blinn-Phong style per-light workflows.
- Keep only `glsl_light_count()`, `glsl_light_get(i)`, and `glsl_light_shadow(i, shading_normal)` as the public per-light helper API.
- Remove `glsl_light_diffuse_attenuation(...)` and `glsl_light_specular_attenuation(...)` as public helpers and reject them during parse with an explicit migration message.
- Update docs and render tests so examples use `diffuse_color/specular_color/attenuation/shadow` directly instead of old `diffuse_power/specular_power` scaling hacks.

## Public API

`GLSLLight` becomes:

```glsl
struct GLSLLight {
  bool valid;
  uint index;
  int type;
  vec3 vector;
  vec3 position;
  vec3 direction;
  float distance;
  vec3 diffuse_color;
  vec3 specular_color;
  float attenuation;
};
```

Semantics:

- `vector`: normalized direction from the shaded point to the light center
- `position`, `direction`, `distance`, `type`, `index`: unchanged from the previous helper
- `diffuse_color`: a custom-lighting-friendly diffuse color term that converts Eevee's internal surface-radiance weighting into a more point-like channel energy, avoiding extreme brightness when local lights have a very small radius
- `specular_color`: the matching custom-lighting-friendly specular color term, using the same conversion idea for hand-written highlight models
- `attenuation = light_point_light(light, is_directional, light_vector) * light_attenuation_surface(light, is_directional, light_vector)`

`attenuation` excludes:

- `NdotL`, half-lambert, toon ramps, Blinn-Phong, GGX, or any other model term
- `light_attenuation_facing(...)`
- `light_ltc(...)`
- shadow
- material-side Fresnel / IOR / metallic / tint / roughness shaping

Public helper surface after the change:

```glsl
int glsl_light_count()
GLSLLight glsl_light_get(int light_index)
float glsl_light_shadow(int light_index, vec3 shading_normal)
```

Removed public members / helpers:

- `GLSLLight.color`
- `GLSLLight.diffuse_power`
- `GLSLLight.specular_power`
- old raw `GLSLLight.attenuation`
- `glsl_light_diffuse_attenuation(...)`
- `glsl_light_specular_attenuation(...)`

## Implementation Tasks

### Task 1: Save the new plan and switch helper semantics

**Files:**
- Add: `blender_5_1_port/docs/plans/2026-04-16-glsl-function-friendly-light-struct.md`
- Modify: `blender_5_1_port/source/blender/gpu/shaders/material/gpu_shader_material_glsl_light_access.glsl`

**Required result:**

- `GLSLLight` uses the new friendly field set.
- `glsl_light_default()` initializes only the new public fields.
- `glsl_light_build(...)` fills `diffuse_color`, `specular_color`, and the new `attenuation`.
- Remove unused helper code that only existed for the deleted diffuse/specular alignment helpers.
- Keep `glsl_light_count()`, `glsl_light_get(...)`, and `glsl_light_shadow(...)` intact.

### Task 2: Narrow parse-time helper recognition and hard-reject removed helpers

**Files:**
- Modify: `blender_5_1_port/source/blender/nodes/shader/nodes/node_shader_glsl_function.cc`

**Required result:**

- Light-access identifier detection only treats these names as live API:
  - `GLSLLight`
  - `glsl_light_count`
  - `glsl_light_get`
  - `glsl_light_shadow`
- `glsl_light_diffuse_attenuation` and `glsl_light_specular_attenuation` move into the deprecated helper list.
- Parse-time error text explains the new contract:
  - use `light.diffuse_color`
  - use `light.specular_color`
  - use `light.attenuation`
  - combine with `glsl_light_shadow(...)` manually
- Do not add field-name heuristics for removed members; old field access should fail naturally at GLSL compile/member lookup time.

### Task 3: Update user docs to the single-layer friendly API

**Files:**
- Modify: `blender_5_1_port/docs/glsl-function-node-conversion-guide.md`
- Modify: `blender_5_1_port/blender-5.1-npr-features-and-usage.md`

**Required result:**

- Remove documentation for `glsl_light_diffuse_attenuation(...)` and `glsl_light_specular_attenuation(...)`.
- Describe `GLSLLight.diffuse_color`, `GLSLLight.specular_color`, and the new model-neutral `GLSLLight.attenuation`.
- Provide one recommended custom-lighting pattern:
  - `light.diffuse_color * light.attenuation * max(dot(N, light.vector), 0.0) * glsl_light_shadow(...)`
  - `light.specular_color * light.attenuation * custom_spec_term * glsl_light_shadow(...)`
- State clearly that the helper is intended for per-light custom models, not a direct proxy for Eevee final BSDF output.

### Task 4: Update tests to the new public contract

**Files:**
- Modify: `blender_5_1_port/tests/python/bl_node_glsl_function.py`
- Add: `blender_5_1_port/tests/python/npr/test_glsl_function_light_access_render.py`

**Required result:**

- Add a node-level parse test that removed helpers are rejected with the new migration message.
- Add render coverage for the new friendly `GLSLLight` semantics:
  - single-light visible response without hand-tuned `0.0002` / `0.00005` scale factors
  - multi-light independent accumulation
  - shadow response through `glsl_light_shadow(...)`
  - type / position / direction field stability
- Remove old render expectations that depended on `diffuse_power/specular_power` or on matching Eevee `Diffuse BSDF` / `Glossy BSDF` output order of magnitude.

## Verification

Run the repo node smoke test and the new render test with the install-tree Blender:

```bat
E:\blender_bulid_test\blender_npr_bulid\build_ninja_sccache_poll.bat install --no-pause
E:\blender_bulid_test\blender_npr_bulid\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe --background --factory-startup --python blender_5_1_port\tests\python\bl_node_glsl_function.py
E:\blender_bulid_test\blender_npr_bulid\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe --background --factory-startup --python blender_5_1_port\tests\python\npr\test_glsl_function_light_access_render.py
```

If the helper GLSL change does not propagate, verify the generated GPU source list and force the owning unity target to rebuild before trusting the install tree.

## Assumptions

- This is an intentionally breaking public GLSL API change.
- No raw fallback struct or compatibility alias layer is kept.
- `GLSL_LIGHT_TYPE_*` constants remain unchanged.
- `glsl_light_count()` still iterates the fragment-local visible light list, not a global scene light list.
- `glsl_light_shadow(...)` remains the only higher-level retained helper because shadow depends on the current shading normal and runtime shadow tracing path.
