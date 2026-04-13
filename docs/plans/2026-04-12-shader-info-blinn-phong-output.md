# Shader Info Blinn-Phong Output Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Extend `Shader Info` so it can output a stable NPR-friendly `Blinn-Phong Factor` without introducing a separate node type.

**Architecture:** Reuse the existing `Shader Info` light loop and node registration path. Add one float input socket, `Exponent`, and one float output socket, `Blinn-Phong Factor`, then compute a weighted unshadowed Blinn-Phong highlight factor inside the existing GLSL function using the same light filtering rules already used by `Diffuse Shading`, `Shadow`, and `Half-Lambert Factor`.

**Tech Stack:** C++, Blender shader nodes, Eevee GPU material shaders, Python NPR regression tests, install-tree Blender validation

---

## Final Product Decision

Do **not** create a new node for V1.

Ship the first version as an extension of the existing `Shader Info` node:

- new input: `Exponent`
- new output: `Blinn-Phong Factor`

Keep the new output deliberately narrow:

- it is a float factor, not a color output
- it is evaluated from the same direct scene lights already filtered by `Shader Info`
- it stays independent from the `Shadow` output, matching the current node pattern where light signals and shadow signals are exposed separately
- it excludes world sun in the same way as the existing outputs

Use a regular input socket instead of a node property:

- no new DNA storage
- no new RNA property
- no node migration/versioning work
- users can drive the exponent per material with ordinary node links

Suggested V1 default:

- `Exponent` default value: `32.0`
- minimum clamp in shader: `1.0`

## Scope

### In Scope

- extend `Shader Info`
- add one float input and one float output
- compute weighted Blinn-Phong response in the existing light loop
- add smoke, render, and viewport coverage
- update user-facing docs

### Out of Scope

- new standalone node type
- colored specular output
- roughness-to-exponent conversion UI
- exact Eevee microfacet/specular parity
- new menu items
- DNA/RNA storage changes

## Semantics To Implement

### Node Interface

`Shader Info` becomes:

- inputs:
  - `World Position`
  - `Normal`
  - `Exponent`
- outputs:
  - `Diffuse Shading`
  - `Shadow`
  - `Ambient Lighting`
  - `Half-Lambert Factor`
  - `Blinn-Phong Factor`

### Blinn-Phong Formula

For each accepted light:

```glsl
float3 H = safe_normalize(L + V);
float nh = saturate(dot(N, H));
float specular = pow(nh, exponent);
```

Use the light's specular channel for weighting, not the diffuse channel:

```glsl
float specular_power = light_power_get(light, LIGHT_SPECULAR);
float specular_weight = specular_power * shader_info_max_component(light.color);
```

Aggregate as a weighted average:

```glsl
specular_sum += specular * specular_weight;
specular_weight_sum += specular_weight;
blinn_phong_factor = (specular_weight_sum > 1e-8f) ?
    saturate(specular_sum / specular_weight_sum) :
    0.0f;
```

This keeps the output:

- in a predictable `0..1` range
- independent from absolute lamp energy
- consistent with how `Half-Lambert Factor` is already treated as a normalized light response signal

## Task 1: Lock the Node Interface in the Smoke Test

**Files:**
- Modify: `blender_5_1_port/tests/python/npr/test_goo_shader_info_smoke.py`

**Step 1: Write the failing smoke assertions**

Update the socket lists so the test expects:

```python
assert [socket.name for socket in node.inputs] == [
    "World Position",
    "Normal",
    "Exponent",
]
assert [socket.name for socket in node.outputs] == [
    "Diffuse Shading",
    "Shadow",
    "Ambient Lighting",
    "Half-Lambert Factor",
    "Blinn-Phong Factor",
]
```

**Step 2: Run the smoke test to verify it fails**

Run:

```powershell
.\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe --background --factory-startup --python blender_5_1_port\tests\python\npr\test_goo_shader_info_smoke.py
```

Expected:

- assertion failure mentioning the missing `Exponent` input or `Blinn-Phong Factor` output

**Step 3: Commit only the test change**

```powershell
git -C blender_5_1_port add tests/python/npr/test_goo_shader_info_smoke.py
git -C blender_5_1_port commit -m "test: lock shader info blinn-phong interface"
```

## Task 2: Add Failing Render Coverage for the New Output

**Files:**
- Modify: `blender_5_1_port/tests/python/npr/test_goo_shader_info_render.py`

**Step 1: Add exponent wiring support in the test material builder**

Extend the helper so the material can optionally drive `Exponent`:

```python
def make_shader_info_material(name, output_name, exponent=32.0):
    material = bpy.data.materials.new(name)
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    exponent_value = nodes.new("ShaderNodeValue")
    exponent_value.outputs[0].default_value = exponent
    shader_info = nodes.new("ShaderNodeShaderInfo")

    links.new(exponent_value.outputs[0], shader_info.inputs["Exponent"])
    links.new(shader_info.outputs[output_name], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material
```

**Step 2: Add failing assertions for the new output**

Add coverage that checks:

- light on: `Blinn-Phong Factor` becomes visible
- no light: it stays black
- world sun only: it stays black
- blocker shadow does not directly darken the factor

Add:

```python
assert_unshadowed_response("Blinn-Phong Factor", min_value=0.05, tolerance=0.08)
assert_no_light_black("Blinn-Phong Factor")
assert_world_sun_black("Blinn-Phong Factor")
```

**Step 3: Add one shape test that proves it is actually view/light dependent**

Use a sphere and compare highlight center vs off-center:

```python
def assert_blinn_phong_gradient_on_sphere():
    clear_scene()
    configure_scene()
    make_camera()
    make_sphere(make_shader_info_material("BlinnPhongSphereMaterial", "Blinn-Phong Factor"))
    make_light()
    pixels, width, height = render_image()
    center = sample_world_point(pixels, width, height, -1.4, 0.0)
    edge = sample_world_point(pixels, width, height, 1.4, 0.0)
    assert center[0] > edge[0] + 0.1, (
        f"Blinn-Phong highlight should peak near the facing region, got center={center} edge={edge}"
    )
```

Call it near the existing `Half-Lambert` shape checks.

**Step 4: Run the render test to verify it fails**

Run:

```powershell
.\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe --background --factory-startup --python blender_5_1_port\tests\python\npr\test_goo_shader_info_render.py
```

Expected:

- missing socket error for `Exponent` or `Blinn-Phong Factor`

**Step 5: Commit only the render-test change**

```powershell
git -C blender_5_1_port add tests/python/npr/test_goo_shader_info_render.py
git -C blender_5_1_port commit -m "test: add shader info blinn-phong render coverage"
```

## Task 3: Add Failing Viewport Coverage

**Files:**
- Modify: `blender_5_1_port/tests/python/npr/test_goo_shader_info_viewport.py`

**Step 1: Reuse the same interface change in the viewport helper**

Mirror the `Exponent` link setup used in the render test helper so the viewport test can build the new node graph.

**Step 2: Add the new viewport assertion**

Extend `run_tests()` with:

```python
assert_unshadowed_viewport("Blinn-Phong Factor", min_value=0.05, tolerance=COLOR_EPSILON)
```

**Step 3: Run the viewport test to verify it fails**

Run only if no release Blender is already open. Close only the test Blender launched by this command:

```powershell
.\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe --factory-startup --python blender_5_1_port\tests\python\npr\test_goo_shader_info_viewport.py
```

Expected:

- missing socket error or new assertion failure before implementation

**Step 4: Commit only the viewport-test change**

```powershell
git -C blender_5_1_port add tests/python/npr/test_goo_shader_info_viewport.py
git -C blender_5_1_port commit -m "test: add shader info blinn-phong viewport coverage"
```

## Task 4: Extend the Node Declaration and GPU Stack Wiring

**Files:**
- Modify: `blender_5_1_port/source/blender/nodes/shader/nodes/node_shader_shader_info.cc`

**Step 1: Add the new socket declarations**

Change the node declaration to:

```cpp
b.add_input<decl::Vector>("World Position").hide_value();
b.add_input<decl::Vector>("Normal").hide_value();
b.add_input<decl::Float>("Exponent")
    .default_value(32.0f)
    .min(1.0f)
    .max(512.0f);

b.add_output<decl::Color>("Diffuse Shading");
b.add_output<decl::Float>("Shadow");
b.add_output<decl::Color>("Ambient Lighting");
b.add_output<decl::Float>("Half-Lambert Factor");
b.add_output<decl::Float>("Blinn-Phong Factor");
```

**Step 2: Pass the new input through `GPU_stack_link()`**

Do not add RNA or storage. Keep `shadow_mode`, `stable_shadow_samples`, and `lightgroup_hash` as extra GPU constants after the regular input stack.

**Step 3: Update the node description text**

Make the tooltip mention direct light, shadow, ambient, half-lambert, and blinn-phong signals.

**Step 4: Build the install tree incrementally**

Run from the workspace root:

```powershell
.\build_ninja_sccache.bat install
```

Expected:

- incremental build only
- updated binaries copied into `install_windows_x64_vc17_Release_5_1_port_clean`

**Step 5: Re-run the smoke test**

Run:

```powershell
.\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe --background --factory-startup --python blender_5_1_port\tests\python\npr\test_goo_shader_info_smoke.py
```

Expected:

- smoke test still fails because GLSL output is not implemented yet, or passes if the interface is fully wired first

**Step 6: Commit the declaration change**

```powershell
git -C blender_5_1_port add source/blender/nodes/shader/nodes/node_shader_shader_info.cc
git -C blender_5_1_port commit -m "feat: extend shader info node interface"
```

## Task 5: Implement the Blinn-Phong Output in GLSL

**Files:**
- Modify: `blender_5_1_port/source/blender/gpu/shaders/material/gpu_shader_material_shader_info.glsl`

**Step 1: Extend the shader function signature**

Change the function signature so it accepts `Exponent` and returns `Blinn-Phong Factor`:

```glsl
void node_shader_info(float3 position,
                      float3 normal_in,
                      float exponent,
                      float shadow_mode,
                      float stable_shadow_samples,
                      float lightgroup_hash_value,
                      out float4 diffuse_shading,
                      out float shadow,
                      out float4 ambient_lighting,
                      out float half_lambert_factor,
                      out float blinn_phong_factor)
```

**Step 2: Add the new accumulators**

Add:

```glsl
float safe_exponent = max(exponent, 1.0f);
float blinn_phong_sum = 0.0f;
float blinn_phong_weight_sum = 0.0f;
```

**Step 3: Accumulate the new response inside the existing light loop**

Use the same accepted-light branch already filtered by:

- zero light color rejection
- light linking
- matching lightgroup
- non-world-sun rejection

Add:

```glsl
float specular_power = light_power_get(light, LIGHT_SPECULAR);
if (specular_power >= LIGHT_ATTENUATION_THRESHOLD) {
  float3 half_vector = safe_normalize(lv.L + view_vector);
  float nh = saturate(dot(shading_normal, half_vector));
  float blinn_phong = pow(nh, safe_exponent);
  float specular_weight = specular_power * shader_info_max_component(light.color);
  blinn_phong_sum += blinn_phong * specular_weight;
  blinn_phong_weight_sum += specular_weight;
}
```

**Step 4: Write the final output**

At the end of the function:

```glsl
blinn_phong_factor = (blinn_phong_weight_sum > 1e-8f) ?
                         saturate(blinn_phong_sum / blinn_phong_weight_sum) :
                         0.0f;
```

And in the fallback branch:

```glsl
blinn_phong_factor = 0.0f;
```

**Step 5: Build the install tree incrementally again**

Run:

```powershell
.\build_ninja_sccache.bat install
```

**Step 6: Run the smoke and render tests**

Run:

```powershell
.\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe --background --factory-startup --python blender_5_1_port\tests\python\npr\test_goo_shader_info_smoke.py
.\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe --background --factory-startup --python blender_5_1_port\tests\python\npr\test_goo_shader_info_render.py
```

Expected:

- both pass

**Step 7: Commit the shader implementation**

```powershell
git -C blender_5_1_port add source/blender/gpu/shaders/material/gpu_shader_material_shader_info.glsl
git -C blender_5_1_port commit -m "feat: add shader info blinn-phong output"
```

## Task 6: Run Viewport Validation Against the Installed Blender

**Files:**
- Test: `blender_5_1_port/tests/python/npr/test_goo_shader_info_viewport.py`

**Step 1: Launch the viewport test**

Run:

```powershell
.\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe --factory-startup --python blender_5_1_port\tests\python\npr\test_goo_shader_info_viewport.py
```

Expected:

- the script prints `PASS`
- the test Blender closes itself

**Step 2: If it fails, tighten the test before touching the implementation**

Only adjust tolerances if the rendered signal is correct and the failure is clearly due to viewport noise. Do not weaken the light/no-light/world-sun semantic checks.

**Step 3: Commit the passing viewport validation update**

```powershell
git -C blender_5_1_port add tests/python/npr/test_goo_shader_info_viewport.py
git -C blender_5_1_port commit -m "test: validate shader info blinn-phong viewport output"
```

## Task 7: Update Docs

**Files:**
- Modify: `blender_5_1_port/blender-5.1-npr-features-and-usage.md`

**Step 1: Update the Shader Info docs**

In the `Shader Info` section:

- add `Exponent` to the input list
- add `Blinn-Phong Factor` to the output list
- explain that it is an unshadowed normalized highlight factor from the current scene lights
- state that it excludes world sun like the other `Shader Info` direct-light outputs

Suggested wording:

```markdown
- `Blinn-Phong Factor`
  - 每个灯光的布林冯高光因子按镜面通道加权求平均, 再钳制到 0-1
  - 默认不直接乘阴影, 需要时请与 `Shadow` 输出自行组合
```

**Step 2: Mention the new `Exponent` input**

Suggested wording:

```markdown
- `Exponent`
  - 控制布林冯高光的锐度, 数值越高高光越集中
```

**Step 3: Commit the docs change**

```powershell
git -C blender_5_1_port add blender-5.1-npr-features-and-usage.md
git -C blender_5_1_port commit -m "docs: document shader info blinn-phong output"
```

## Task 8: Final Validation and Integration Commit

**Files:**
- Modify: all files above

**Step 1: Rebuild and install one final time**

Run:

```powershell
.\build_ninja_sccache.bat install
```

**Step 2: Run the full relevant validation set**

Run:

```powershell
.\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe --background --factory-startup --python blender_5_1_port\tests\python\npr\test_goo_shader_info_smoke.py
.\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe --background --factory-startup --python blender_5_1_port\tests\python\npr\test_goo_shader_info_render.py
.\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe --factory-startup --python blender_5_1_port\tests\python\npr\test_goo_shader_info_viewport.py
```

Also verify the installed binary timestamp for user confirmation.

**Step 3: Review the final diff**

Review only the intended files:

```powershell
git -C blender_5_1_port diff -- source/blender/nodes/shader/nodes/node_shader_shader_info.cc source/blender/gpu/shaders/material/gpu_shader_material_shader_info.glsl tests/python/npr/test_goo_shader_info_smoke.py tests/python/npr/test_goo_shader_info_render.py tests/python/npr/test_goo_shader_info_viewport.py blender-5.1-npr-features-and-usage.md
```

**Step 4: Create the integration commit**

```powershell
git -C blender_5_1_port add source/blender/nodes/shader/nodes/node_shader_shader_info.cc source/blender/gpu/shaders/material/gpu_shader_material_shader_info.glsl tests/python/npr/test_goo_shader_info_smoke.py tests/python/npr/test_goo_shader_info_render.py tests/python/npr/test_goo_shader_info_viewport.py blender-5.1-npr-features-and-usage.md
git -C blender_5_1_port commit -m "feat: add shader info blinn-phong factor"
```

## Notes for the Implementer

- Do not split this into a new node unless the user explicitly changes direction.
- Do not add node storage or RNA unless a plain input socket proves insufficient.
- Keep the signal unshadowed in V1. The existing node design already exposes `Shadow` separately.
- Prefer `safe_normalize()` over `normalize()` for the half vector.
- Treat exact Eevee specular parity as a separate future task.
