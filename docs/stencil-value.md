# Stencil Value and early markers

Stencil Value reads a snapshot of the current view's four user stencil bits. Its float output is
an exact integer from 0 to 15. It is available in Eevee surface shading, including Deferred,
Hybrid, Forward, refractive Deferred, and their NPR shader trees.

## Material setup

The material Stencil panel provides three modes:

- **Standard** keeps the existing stencil/prepass behavior.
- **Early Marker** adds an early stencil-only draw and keeps the regular surface, refraction,
  shadows, and outline shell.
- **Marker Only** contributes only the early marker for that material slot. It contributes no
  ordinary surface/depth, outline shell, volume, shadow, or probe draw. Turning Enabled off
  disables the marker without revealing the helper geometry.

Reference, Read Mask, Write Mask, Test, On Pass, On Fail, On ZFail and Order keep their existing
meanings. Early markers always disable color/depth/normal/ID/velocity writes. Their ZTest uses
the material's existing ZTest setting against the completed ordinary Deferred depth.

For a simple mark, enable stencil, choose Early Marker or Marker Only, set Reference to 1,
Write Mask to bit 0, Test to Always, On Pass to Replace, and both failure operations to Keep.
The normal material Depth Write setting does not need to be changed.

Conflicting overlapping writes must use distinct Order values. Lower orders execute first;
equal orders do not define a winner. Markers do not occlude each other by writing depth.
The four bits are shared by all users of the view; reserve different bits for independent effects.

Early modes support Keep, Zero, Replace, Invert, Increment Wrap and Decrement Wrap. Clamp is
unsupported because native clamp operations act on all eight stencil bits before applying the
write mask. An unsupported configuration skips its marker and reports an error. Standard keeps
its legacy native arithmetic; this feature does not change it into four-bit saturation.

## Node sampling

Add **Stencil Value** to the receiving material. Leave UV unconnected for the current pixel.
Connect Render Info's Frag Coord to UV for the same location, or modify that coordinate to
sample another pixel. UV uses XY and ignores Z. Values outside the actual snapshot texture
return zero. Sampling is nearest integer texel sampling, with no mipmaps or color conversion.
Interpolating raw labels would create labels which were never written, so linear filtering is
not offered. Extract the relevant bit before building any soft mask effect.

For example, the existing GLSL/math nodes can use:

```glsl
float mask = float((uint(value) & 1u) != 0u);
b = mix(b, min(b, SolidShadowStrength), mask);
```

This lets one face shader change its lighting input. Refractive hair can supply an early marker
and still sample the face's completed color in its later refraction pass. Offset helper geometry
can use Marker Only. This is screen-space raster coverage, not light-space projection or ray tracing.

## Timing and data source

Each render view/sample performs the following sequence:

1. Prepare ordinary Deferred depth and execute its Standard stencil draws.
2. Draw Early Marker / Marker Only surfaces in their own ordered stencil-only queue.
3. Read the native stencil aspect and convert `stencil & 0x0F` into an R8UI texture.
4. Shade ordinary Deferred surfaces, then the existing refraction and Forward stages.

The source depth/stencil is not attached to the conversion target. All consumers sample the
separate immutable R8UI texture. Later native stencil clears/writes do not update this snapshot.
There are no previous-frame reads or per-object snapshot chains. Render Texture captures and
the main view regenerate the snapshot independently, including when the opaque layer is empty.

The snapshot includes ordinary Deferred Standard writes and early markers. Later Standard
refraction/Forward writes are excluded; select Early Marker to contribute from those materials.
Those materials' later depth is also excluded from the marker ZTest. Existing refraction-layer
settings must still be enabled for scene-color refraction to work.

## Dependency restrictions

Stencil Value may control Mix Shader between two proven opaque branches, including nested
Mix/Add Shader branches. Principled BSDF and Principled NPR require a constant Alpha of 1;
Specular BSDF requires constant Transparency of 0. Constants folded when expanding the graph
are supported. Transparent, Holdout, missing shader inputs and unproven custom closures remain
conservative boundaries. Refraction in a BSDF does not by itself imply alpha transparency.

Stencil Value must not affect opacity, coverage-changing shader weights, displacement, tessellation
or depth offset, since those may be evaluated before the snapshot. Early marker geometry and opacity
must also be independent of later scene-color, refraction, ambient occlusion and lighting results. Expanded node
groups and closure branches are checked; arbitrary GLSL outputs conservatively depend on all
their input sockets. Keep opacity computation independent of the color/lighting branch.
Only the selected material/outline output is checked; switching outputs or changing Alpha
invalidates the old validation result. Cached passes retain the validation from before internal
BSDF weight expansion.

## Previewing the snapshot

In the viewport shading popover, select **Render Pass → Stencil Value**. Its **Source** setting
offers **Material Snapshot** (the default) and **Final Stencil**. Both reuse the existing colored
labels and repeated numbers from 0 to 15.

Material Snapshot displays the same R8UI data that surface Stencil Value nodes read. Selecting
this preview generates a current-view snapshot even if there are no reader nodes or ordinary
opaque objects. Marker Only regions remain visible even when they have no rendered surface/depth.
Each viewport keeps its own source choice, which is saved in the blend file; older files default
to Material Snapshot.

Final Stencil retains the original native-attachment preview after Forward rendering. Later
clears or writes can make it differ from the earlier material snapshot. Changing the preview
source does not change when materials perform stencil tests or writes.

For a grayscale preview in a receiving surface material, connect Stencil Value to a Math Divide
node with divisor 15, then to Emission Color and Material Output Surface. To preview one bit as
black/white with Math nodes, use `floor(Value / 2^bit) modulo 2` instead. Comparing Value with an
integer previews an exact label. Use Emission to keep lighting from changing the preview.

Marker is a drawing mode; Reference is the value used by Replace, limited by Write Mask.
Other stencil operations modify the existing value instead. The preview reads the completed
snapshot, including overlapping markers and their Order, not simply each object's Reference.
Stencil testing only answers pass/fail and cannot display all four bits as a value.
The node-based grayscale/bit preview is visible on its receiving geometry. Use Material Snapshot
in the viewport to inspect the full frame without changing materials.

Unavailable contexts such as prepass, shadows, probes, baking, World, Volume, Filter and other
engines return zero. This does not make a cyclic surface-opacity dependency valid. Invalid
dependencies are diagnosed. Compiling/failed marker shaders do not write an opaque placeholder.
Curves and point clouds also skip surface-dependent synchronization while a marker shader is
unavailable. Marker Only suppresses native VDB volume draws as well as mesh volume outputs.

Snapshot storage is acquired when the early snapshot stage actually executes. This includes
Render Texture surface captures inside an offline DLSS NR guide instance; the separate
depth/velocity-only guide view does not generate or read a snapshot.

HETA's RT3 payload mixing is a separate operation; sampling its region marker alone does not
reproduce that mixing.

## Cost and verification

The full-size snapshot is allocated only when a supported reader or Material Snapshot preview
uses it. R8UI payload is one
byte per internal render pixel: approximately 1.98 MiB at 1920x1080 and 7.91 MiB at 3840x2160.
Additional costs are the selected marker draws, opacity evaluation, one full-screen conversion
per view/sample, and texture reads in consuming shaders. Actual GPU timings must be measured.

The independent regression entry is `tests/python/npr/test_stencil_value.py`. Run with isolated
Blender user directories and both `--gpu-backend opengl` and `--gpu-backend vulkan`.
Use `--profile-gpu` for the OpenGL benchmark phase; GPU trace entries use pid 1 and microsecond
durations. The current Vulkan profiler emits CPU groups only; use external GPU counters for
Vulkan timings, and distinguish replay action durations from live timestamp intervals.
Compilation, node registration and CPU render time are not substitutes for pixel or GPU timing tests.

`test_stencil_value_regressions.py` covers marker compilation states, native VDB suppression,
probe baking and offline NR with Render Texture. Run it through the `stencil-value-regressions`
Release case or a launcher which also rejects missing GPU bindings in `--debug-gpu` logs.
`test_stencil_value_async.py --geometry CURVES` (or `POINTCLOUD`) checks actual queued/failed
window frames and recovery. OpenGL tests NR's existing unsupported-backend fallback; actual NR
execution requires Vulkan.

`test_stencil_value_opaque_mix.py` compares stencil-driven opaque mixes with constant-factor
pixel references, including preprocessed-tree reuse, nested Mix/Add and group/Bundle graphs.
It also checks transparent/unknown coverage rejection, Shader to RGB paths into early inputs,
Alpha animation invalidation, Forward/refraction/marker modes and a 0–15 snapshot preview.

`test_stencil_value_preview.py` checks both viewport sources without material reader nodes,
all user values and marker modes, empty scenes, frame changes, resizing, independent viewports,
and persistence/older-file defaults. Native final-stencil frames provide the existing color and
number references. The preview uses a color-only target while sampling the snapshot and depth.

`test_stencil_value_review.py` adds non-square Render Texture, crop, overscan, resizing and
default/Frag Coord sampling comparisons, mixed image samplers, cached material copies,
sampler exhaustion/recovery and an actual Cycles zero-fallback render. Its optional integration
phase checks Bundle and Image to Closure together with Stencil in an integrated build.
The snapshot sampler uses the existing material slot allocator, after reserving engine resources.
OpenGL's combined texture unit indices remain distinct from its per-stage sampler count limit.
