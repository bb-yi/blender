# Outline Control Shell Geometry Parity

## 2026-06-19 Update: Screen-Space Contour Parameter Removed

The original screen-space contour-parameter direction has been removed before distribution. There is
no compatibility socket or migration path because no production file should depend on this
prototype.

Current direction:

- Do not use screen-space contour seeds for shell parity.
- Use the real shell path and `Shell Width` for material-level parity with the production outline
  shell workflow.
- Keep `Depth Threshold` and `Normal Threshold` for the existing screen-space outline modes only.
- Store `Depth Threshold` directly in `outline_info.g`; the removed contour prototype no longer
  shares that channel.

## Problem

`Outline Control` currently writes per-pixel outline parameters and the Eevee outline detect pass
creates seeds from object/material ID differences, depth discontinuities, normal discontinuities,
and Freestyle marked edges. `Line Width` is only used later when resolving/expanding existing seeds.

This means a smooth view-dependent feature can disappear when it does not create a strong
neighboring depth or normal jump. A side-view nose bridge is one such case: the inverted-hull
geometry outline can show it because the shell geometry creates an actual contour, while the
screen-space detect pass may have no seed to expand.

## Original Goal

Add a controlled, material-driven view-contour detector to `Outline Control` so smooth internal
silhouette/ridge-like contours can produce seeds without requiring duplicated outline meshes.

The goal is to get closer to the old geometry outline behavior while preserving the existing NPR
screen-space pipeline and keeping old files unchanged by default.

The current production goal is stricter than this original plan: the material-level `Outline
Control` path should visually replace the existing outline-shell geometry workflow, which duplicates
the source mesh, offsets the copy along normals with vertex-color width control, flips faces, and
assigns a separate outline material. That target is not met by the current screen-space seed
implementation.

## Removed Screen-Space Prototype

The removed prototype tried to derive smooth view-dependent contours from the original surface in
`eevee_outline_detect_frag.glsl`. It was rejected because it could not provide shell parity: the
shell-geometry method creates real offset shell geometry, while the screen-space path can only
expand from source-surface seeds.

Implementation cleanup:

- Removed the temporary node socket from `ShaderNodeOutlineControl`.
- Removed the temporary material GLSL parameter and `output_outline()` argument.
- Removed contour threshold packing/unpacking from `outline_info.g`.
- Removed the optional view-contour seed branch from `eevee_outline_detect_frag.glsl`.
- Kept the previous image-slot-8 lesson: do not add another material image output for this feature.

## Current Workflow Inspection

Checked the representative outline-shell test file before the follow-up wiring. Initial inspection
found:

- The reference outline-shell workflow still duplicates geometry, offsets the copy from normals,
  flips faces, assigns a separate outline material, and mixes several vertex or attribute inputs
  before storing the width control attribute.
- The `Outline Control` node currently exposes Line Color, Line Alpha, Line Width, Depth
  Threshold, Normal Threshold, and Outline ID only.
- With vertex-color width control enabled, the material path still depends on screen-space source
  pixels having nonzero effective width and on the detector producing a seed on the original mesh.
- The geometry-shell path instead creates real offset geometry, so it can produce silhouettes that
  are not equivalent to any original-surface screen-space seed.

Follow-up inspection of the reference outline-shell workflow after the material shell experiments
found the core sequence:

- A named attribute stores the mixed vertex color used by the outline width logic.
- `Merge by Distance` welds the generated outline mesh with distance `0.0001` before expansion.
- `Set Position` offsets the welded mesh by a vector derived from the geometry normal, final width,
  and optional camera-follow offset.
- `Flip Faces` turns the shell into an inverted hull.
- `Set Material` assigns the dedicated outline material.

The outline objects in that workflow are separate mesh objects that share the source mesh data and
use a dedicated modifier. Their outline materials use backface culling and dithered/hashed alpha
settings, and the shell participates in the normal scene depth pipeline.

This explains why the geometry-node version does not produce the same face-internal extra strokes
as the renderer-side screen-space fixes:

- It does not dilate shell pixels in screen space.
- It welds small seams before offsetting the shell.
- Occlusion comes from real geometry and the normal depth buffer instead of a post-process
  `shell_color_tx` neighborhood fill.
- Small transparent gaps should therefore be fixed by improving shell geometry continuity or depth
  parity, not by screen-space 1-pixel hole filling. The `Fill small outline shell gaps` experiment
  was reverted because it reintroduced face-internal strokes.

Renderer follow-up:

- Added the existing mesh `VertexNormal` VBO to EEVEE mesh surface batches and exposed it to
  `eevee_geom_mesh_vert.glsl` as `outline_vnor`.
- The outline shell vertex expansion now uses `outline_vnor` instead of the corner/loop normal
  `nor`. This is closer to the shell workflow after `Merge by Distance`, because vertex
  normals are less affected by UV seams, split loop normals, and material boundary corners.
- This still is not a full GPU-side merge by distance, but it removes the most obvious source of
  shell cracks without screen-space dilation.

## Current Result

As of the current renderer-side shell implementation, the result is visually close enough to keep
iterating on the material-level approach. It is still not a literal replacement for the geometry
node modifier, because the renderer does not actually duplicate, weld, and re-materialize a mesh in
the same way as the original outline-shell workflow.

Current practical status:

- Screen-space contour seeds have been removed.
- The material shell pass is active for mesh materials using `Outline Control`.
- Shell width is evaluated in the vertex shader from `Line Width` / `Shell Width`.
- Shell expansion uses `outline_vnor`, a vertex-normal VBO exposed to EEVEE mesh shaders, instead
  of the corner/loop normal. This reduces seam-related shell cracks compared to expanding with
  `nor`.
- The shell pass keeps `DRW_STATE_DEPTH_GREATER_EQUAL | DRW_STATE_CULL_FRONT`; switching to
  `DRW_STATE_DEPTH_LESS_EQUAL` was tested and reverted because it caused the whole source mesh to
  be covered by the outline color.
- Screen-space 1-pixel shell hole filling was tested and reverted because it reduced transparent
  cracks but reintroduced face-internal extra strokes.
- The mesh position VBO order is preserved for velocity: `CornerNormal`, `Position`,
  `VertexNormal`. This avoids the `eevee_velocity.hh:65` assertion in `pos_buf_get()`.

Remaining parity gaps:

- GPU shell expansion still does not perform a real `Merge by Distance(0.0001)` before offsetting.
- Dedicated outline objects and dedicated outline materials from the reference workflow are not
  created;
  instead, the result is composited through `resolved_outline_tx_`.
- Transparent cracks or small face-internal strokes can still appear in edge cases where topology,
  material boundaries, or alpha behavior differs from the old duplicated-shell object.
- Exact camera-follow offset parity has not been implemented.

## Revised Direction: True Shell Geometry Parity

The next implementation direction should stop treating the target as a screen-space contour
detection problem. The requirement is now full parity with the reference shell workflow:

- Submit outline-enabled mesh materials through a second shell pass.
- Evaluate the same material node tree so `Outline Control` keeps using the same color, alpha,
  width, ID, and vertex-color/attribute driven inputs.
- Expand shell vertices in the vertex stage using the same semantic width source as
  the reference outline-shell workflow.
- Render the shell with inverted-hull visibility behavior instead of deriving the result only from
  original-surface screen-space seeds.
- Composite the shell result through the existing outline result texture so combined/pass output,
  TAA history handling, and the Outline render pass stay on the current integration path.

This implies a new Eevee material pipeline, tentatively `MAT_PIPE_OUTLINE_SHELL`, rather than more
logic in `eevee_outline_detect_frag.glsl`.

The minimum useful implementation slice is:

- Add `Material::outline_shell` and register it only when `uses_outline_control` is true.
- Add a material create-info/pipeline state that compiles the surface graph for mesh geometry but
  outputs only outline-shell color/alpha.
- Add a shell vertex mode to `eevee_geom_mesh_vert.glsl` that offsets `interp.P` after
  displacement and attribute loading.
- Add `OutlineModule` shell framebuffer output before the current JFA resolve path. The first
  version can write directly to `resolved_outline_tx_`; later versions can decide whether shell
  pixels should also feed JFA seeds.
- Keep the current screen-space detect/JFA path as a fallback and for ID/depth/normal/Freestyle
  strokes that are not shell-parity strokes.

Current shell slice status:

- Added `MAT_PIPE_OUTLINE_SHELL`.
- Added `Material::outline_shell`.
- `MaterialModule` now creates an outline shell material pass for mesh materials that use
  `Outline Control`.
- `SyncModule::sync_mesh()` submits the shell draw call for each matching material slot.
- `OutlineModule` owns an `Outline.Shell` pass and composites shell fragments into
  `resolved_outline_tx_` after the existing JFA resolve.
- Added `eevee_surf_outline_shell_frag.glsl`, which evaluates the same material graph and uses the
  staged `Outline Control` color/alpha to output premultiplied shell color.
- Added a dedicated vertex-stage shell-width graph output. `Outline Control` now emits its
  `Line Width` / `Shell Width` inputs through a side-effect-free `node_outline_shell_width()`
  material node, so both linked attribute expressions and unlinked socket defaults can be
  serialized for the shell vertex shader. `Shell Width > 0` overrides shell expansion; `0` keeps
  using `Line Width` for backwards compatibility.
- Updated the `MAT_OUTLINE_SHELL` branch in `eevee_geom_mesh_vert.glsl` to offset vertices in
  object/geometry space by `Normal * max(Line Width, 0) * outline_shell_offset`, then transform the
  offset delta to world space. This better matches geometry-node `Set Position` semantics than a
  fixed world-space normalized-normal offset.
- Added `MAT_OUTLINE_STAGE_ONLY` for shell materials. The shell fragment shader needs the staged
  `Outline Control` values, but must not compile the normal forward/deferred image-store path that
  writes `outline_color_img` / `outline_info_img`.
- Fixed shell subpass registration to call `material_set(*manager, gpumat, true)`. Without this,
  the pass can record geometry calls with no shader bound and crash at draw submission.
- Removed the shell contour-facing gate. It suppressed legitimate self-occluding shell contours
  such as a nose bridge at three-quarter views. The shell pass keeps the current
  `DRW_STATE_DEPTH_GREATER_EQUAL` state because switching to `DRW_STATE_DEPTH_LESS_EQUAL` caused the
  expanded shell to cover the whole source mesh with the outline color.
- Changed shell output back to visible shell-band compositing after boundary-only extraction missed
  self-occluding shell contours. `Outline.Shell` writes thresholded candidate shell color into
  `Outline.ShellColor`, then `Outline.ShellBoundary` composites that visible shell band into
  `resolved_outline_tx_`. Boundary-only extraction left transparent gaps when `Shell Width` was
  exaggerated and could miss the nose bridge when adjacent shell candidates stayed connected.
- Reverted the 1-pixel `Outline.ShellBoundary` hole-fill experiment because it created extra face
  internal lines.
- Added `outline_vnor` for shell expansion and kept `Position` as the second VBO in mesh surface
  batches so velocity continues to read the expected `pos` buffer.

Important limitation of the current shell slice:

- The shell pass proves the renderer can submit a real second geometry pass for outline-enabled
  materials, but it is not full shell-workflow parity yet.
- The shell pass now has a vertex-accessible `Line Width` / `Shell Width` expression, but
  `outline_shell_offset` is still a global scale factor. Its unit convention must be matched
  against the reference outline-shell workflow; the vertex offset now uses geometry-space semantics
  so object scale should behave closer to the duplicate shell.
- To match the reference outline-shell workflow, the shell pass needs a vertex-accessible
  shell-width source equivalent to the width mix built from multiple vertex and attribute inputs.

Next implementation step for real parity:

- Validate the current `outline_vnor` result across side, three-quarter, and front views.
- If internal leakage or cracks remain, the next fix should be topology/depth aware, not
  screen-space dilation or another global view-facing threshold.
- The shell vertex offset should use:
  - material displacement first,
  - transformed vertex normal from `outline_vnor`,
  - shell-width value in the same unit convention as the reference outline workflow,
  - optional per-vertex width suppression from the same attribute expression used by the reference
    workflow.
- Only after this exists should the side-face nose bridge case be judged against the reference
  duplicate-shell outline object.

Acceptance criteria for this revised target:

- With the reference duplicate-shell outline object hidden, the material-only result matches the
  shell outline on the side view, including the nose bridge case with vertex-color width control
  enabled.
- Setting every contour-specific screen-space value to disabled does not remove shell silhouettes.
- Vertex-color width suppression behaves like the reference outline-shell workflow, because the
  shell pass uses the same attribute-driven width before vertex expansion.
- The implementation must not require image slot 8 or any new per-pixel material output buffer
  beyond the existing outline textures.

Deferred or explicitly non-goals for the first shell slice:

- Exact topology edge extraction for internal line art beyond what the shell geometry produces.
- Parity for curves, point clouds, volumes, or non-mesh object types.
- Replacing the existing JFA-based outline expansion for all outline modes.

The temporary screen-space fallback is no longer part of the parity direction. If exact shell
parity remains mandatory, the material-only screen-space path is the wrong abstraction; the renderer
needs the actual shell/expanded-depth path or a mesh/topology-derived contour source.

## Verification

- Static `git diff --check` passed on the modified NPR source files.
- `output_outline()` call sites were checked after the signature change.
- Per current workflow, compile commands are handed to the user instead of being run by Codex.
- Runtime issues found during shell workflow testing:
  - `outline_extra_img` undeclared in `eevee_deferred_aov_clear`, fixed by synchronizing shader
    declarations before the extra-buffer approach was removed.
  - `outline_color_img` / `outline_info_img` undeclared in
    `MADefault_Surface_outline_shell_mesh`, fixed by splitting shell staged outline evaluation from
    the forward/deferred image-store clear/flush path.
  - Silent crash after switching viewport modes, likely from `Outline.Shell` subpasses missing
    `material_set()` and therefore submitting draws without a shader.
  - Vulkan `BLI_vector` assertion from image slot 8 overflow, fixed by removing `outline_extra_tx`
    and packing into `outline_info.g`.
  - `MAT_PIPE_PREPASS_OVERLAP` assertion, fixed by keeping overlap prepass registration on the
    forward outline-occlusion path.
  - Whole-mesh outline color after switching shell depth state to `DRW_STATE_DEPTH_LESS_EQUAL`,
    fixed by reverting that depth-state experiment.
  - Face-internal strokes after 1-pixel shell hole filling, fixed by reverting the screen-space
    fill experiment.
  - `eevee_velocity.hh:65` assertion in `pos_buf_get()` after adding `VertexNormal` to surface
    batches, fixed by preserving `Position` as `verts_(1)`.
