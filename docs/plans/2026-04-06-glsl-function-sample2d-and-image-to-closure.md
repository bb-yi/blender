# GLSL Function `sample2D` / `Image to Closure` Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Extend `GLSL Function` so a single `sample2D` parameter can accept either images or procedural texture logic through shader closures, while keeping user-authored code close to normal GLSL.

**Architecture:** Add one new boundary type, `sample2D`, and map it to a `SOCK_CLOSURE` input on the Blender node. `Image to Closure` adapts image resources into the same closure-backed sampling path. The compiler classifies each connected `sample2D` source at compile time: `Image to Closure` gets image-capable helper generation, while ordinary `Closure Output` sources get generic closure sampling with explicit downgrade/error rules for image-only functions.

**Tech Stack:** C++, Blender DNA/RNA, shader nodes, shader closure inline pipeline, GPU material codegen, Eevee material-local generated GLSL sources

---

## Final Product Decision

Do **not** split the user-facing API into `sampler2D` and `sample2D`.

Use a single new sampling boundary type:

- `sample2D`

The user-visible rule becomes:

- images and procedural textures both enter GLSL through `sample2D`
- user code still looks like ordinary GLSL function code
- ordinary sampling continues to use `texture(tex, uv)`

Example target usage:

```glsl
vec4 stylize(vec2 uv, float strength, sample2D tex)
{
  return texture(tex, uv) * strength;
}
```

On the Blender node side:

- `sample2D` maps to one `Closure` input socket
- that `Closure` socket can connect:
  - `Image to Closure`
  - `Closure Output`

This keeps the API simple for users and avoids exposing the implementation split directly in the
GLSL surface language.

## Why This Version Is Better

This version is better than:

- remapping `sampler2D` directly to closures
- forcing users to place `Evaluate Closure`
- introducing two different user-facing sample types

Because it:

- gives the user only one sample-source concept to learn
- keeps the function body close to existing GLSL expectations
- allows images and procedural sources to share one input shape
- still leaves room for compile-time specialization based on the actual source node

## User-Facing Rules

### 1. New Boundary Type

Add `sample2D` as a supported `GLSL Function` input parameter type.

Supported V2 boundary types:

- input:
  - `float`
  - `vec2`
  - `vec3`
  - `vec4`
  - `sample2D`
- output:
  - `float`
  - `vec2`
  - `vec3`
  - `vec4`
- return:
  - `void`
  - `float`
  - `vec2`
  - `vec3`
  - `vec4`

`sampler2D` is no longer the recommended boundary type for the new workflow.

### 2. Node UI Mapping

For each `sample2D` parameter:

- the node shows a `Closure` input socket
- the socket accepts:
  - `Image to Closure`
  - `Closure Output`

There is no image picker for `sample2D` directly on the `GLSL Function` node.

### 3. `Image to Closure`

Add a dedicated shader node:

- name: `Image to Closure`
- output:
  - `Closure`

Purpose:

- adapt a real image into the same sampling pipeline used by procedural closures
- mark the connected source as "image-capable" during `GLSL Function` compilation

Suggested UI:

- image datablock picker
- interpolation
- extension
- optional image-specific controls if already available in existing image node code

### 4. Procedural Workflow

Procedural sampling uses the existing shader closure nodes:

- `Closure Input`
- `Closure Output`

`Closure Input` must provide at least:

- `UV`

`Closure Output` must publish at least:

- `Color`

Optional published outputs for later use:

- `Alpha`
- `Value`

Users should connect `Closure Output` directly into the `GLSL Function` `sample2D` input.
They should not need to place `Evaluate Closure` themselves for this workflow.

## GLSL Surface Semantics

### 1. Primary Sampling Form

For `sample2D`, the supported canonical form is:

```glsl
texture(tex, uv)
```

To reduce friction, do not invent a custom function like `sample(tex, uv)` for V1 unless it turns
out to be required by the parser implementation. The preferred goal is to let artists keep using
the standard `texture()` spelling.

### 2. Advanced Sampling on `Image to Closure`

If the connected source is `Image to Closure`, the compiler may support:

- `textureLod`
- `textureGrad`
- `textureSize`
- `texelFetch`
- `textureGather`

These should only be enabled when the source is explicitly an `Image to Closure` node.

### 3. Procedural Closure Degradation Rules

If the connected source is a generic `Closure Output`, then:

- `texture(tex, uv)` is supported
- `textureLod(tex, uv, lod)` is downgraded to `texture(tex, uv)`
- `textureGrad(tex, uv, dx, dy)` is downgraded to `texture(tex, uv)`

But these should be rejected with clear node errors:

- `textureSize`
- `texelFetch`
- `textureGather`

Rationale:

- `textureLod` and `textureGrad` still conceptually mean "sample at uv", so a fallback to the base
  sample is an acceptable approximation for closure-backed sources
- the others depend on real texel grids or texture metadata and do not have a trustworthy
  procedural equivalent

### 4. Compile-Time Classification

Do not make this a runtime shader branch.

Instead, classify each `sample2D` input at compile time into one of two categories:

- `ImageSampleSource`
  - only when the direct connected source is `Image to Closure`
- `ClosureSampleSource`
  - for ordinary `Closure Output`

The first implementation should **not** attempt deep graph inference such as "this closure
eventually contains an image texture, therefore enable image semantics too". That would add too
much ambiguity and complexity.

## Lowering Strategy

For each `sample2D` parameter, generate helper functions bound to that parameter name.

Example input:

```glsl
vec4 stylize(vec2 uv, float strength, sample2D tex)
{
  return texture(tex, uv) * strength;
}
```

Conceptual lowering:

```glsl
vec4 __sample_tex(vec2 uv);
vec4 __sample_tex_lod(vec2 uv, float lod);
vec4 __sample_tex_grad(vec2 uv, vec2 dx, vec2 dy);
```

Then rewrite supported calls:

- `texture(tex, uv)` -> `__sample_tex(uv)`
- `textureLod(tex, uv, lod)`:
  - image source -> `__sample_tex_lod(uv, lod)`
  - closure source -> `__sample_tex(uv)`
- `textureGrad(tex, uv, dx, dy)`:
  - image source -> `__sample_tex_grad(uv, dx, dy)`
  - closure source -> `__sample_tex(uv)`

Invalid calls for closure-backed sources should fail validation before Eevee shader compilation.

## Internal Implementation Shape

### Route A: Expose `Evaluate Closure` to Users

Pros:

- lower implementation cost
- minimal new compiler plumbing

Cons:

- poor UX
- noisy node graphs
- makes the GLSL workflow feel like a graph trick instead of a native feature

### Route B: `GLSL Function` Becomes a Specialized Closure Consumer

Pros:

- best UX
- direct `Closure Output -> GLSL Function(sample2D)` connection
- hidden use of closure evaluation mechanics

Cons:

- requires dedicated helper emission and source rewriting

### Decision

Use **Route B**.

The `GLSL Function` node should internally evaluate closure-backed `sample2D` parameters without
requiring users to place `Evaluate Closure`.

## File Map

### Very likely to modify

- `source/blender/nodes/shader/nodes/node_shader_glsl_function.cc`
- `source/blender/makesdna/DNA_node_types.h`
- `source/blender/makesrna/intern/rna_nodetree.cc`
- `source/blender/nodes/shader/node_shader_register.hh`
- `source/blender/nodes/shader/node_shader_register.cc`
- `source/blender/nodes/shader/CMakeLists.txt`
- `scripts/startup/bl_ui/node_add_menu_shader.py`
- `docs/glsl-function-node-conversion-guide.md`

### Likely new file

- `source/blender/nodes/shader/nodes/node_shader_image_to_closure.cc`

### Likely infrastructure changes

- `source/blender/nodes/intern/shader_nodes_inline.cc`
- `source/blender/gpu/GPU_material.hh`
- `source/blender/gpu/intern/gpu_material.cc`
- `source/blender/gpu/intern/gpu_codegen.cc`
- `source/blender/draw/engines/eevee/eevee_shader.cc`

## Implementation Stages

### Stage 1: Surface Language and Validation

- add `sample2D` parsing support to `GLSL Function`
- map `sample2D` parameters to `SOCK_CLOSURE`
- add source-call analysis for:
  - `texture`
  - `textureLod`
  - `textureGrad`
  - `textureSize`
  - `texelFetch`
  - `textureGather`
- emit validation errors and downgrade notices based on connected source class

### Stage 2: `Image to Closure`

- add the `Image to Closure` shader node
- make it output a closure-backed image sampling source
- ensure the node is discoverable in the shader add menu

### Stage 3: `sample2D` Helper Emission

- generate per-parameter helper functions for `sample2D`
- support:
  - image-backed helper emission
  - closure-backed helper emission
- rewrite relevant texture calls in generated GLSL

### Stage 4: Direct Procedural Closure Support

- support `Closure Output -> GLSL Function(sample2D)` directly
- inject `Closure Input.UV` as the procedural sampling coordinate
- keep `Evaluate Closure` as an implementation detail, not a required user node

### Stage 5: Docs and Error UX

- update the conversion guide
- document `sample2D`
- document downgrade rules
- document when advanced image sampling requires `Image to Closure`

## Risks

### 1. Rewriting GLSL calls too narrowly

Mitigation:

- explicitly support a small known set of sampling functions first
- reject everything else with clear messages

### 2. Closure graphs can become too general

Mitigation:

- only support closure outputs that publish simple scalar/vector/color values
- reject unsupported structures early

### 3. Hidden downgrades may surprise users

Mitigation:

- emit node warnings when `textureLod` or `textureGrad` are downgraded for closure-backed sources

## Recommended First Vertical Slice

The smallest good slice is:

1. Add `sample2D`
2. Add `Image to Closure`
3. Support `texture(tex, uv)` through `Image to Closure`
4. Reject all advanced sampling calls at first

Then second slice:

1. Add direct `Closure Output` support
2. Allow `textureLod` / `textureGrad` downgrade for closure-backed sources

This keeps the first implementation small while still matching the final architecture.
