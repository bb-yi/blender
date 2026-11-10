# Geometry Lines: Brush Closure

Add **Brush > Brush Closure** in the Geometry Lines editor. The two native zone boundaries
form a colored region. **Brush Parameters** supplies data on the left; **Brush Output**
shows a live preview and returns the existing `NodeSocketNPRLineBrush` on the right.
Connect it directly to Line Settings visible, hidden or per-type brush inputs.

| Output input | Default | Meaning |
| --- | --- | --- |
| Position Offset | (0, 0) | Two-dimensional pixels: X along the original contour, Y along its normal |
| Width | 2 px | Explicit width before rule/global multipliers |
| Color | Black | RGB, independent of color Alpha |
| Opacity | 1 | Explicit stroke opacity |

The output has exactly these four inputs, no effect panels and no legacy effect RNA.
An empty closure draws a continuous stroke, without implicit taper, wave, randomness,
dash mask, texture strength or copies. Width zero draws nothing. Image Alpha changes
opacity only through an explicit connection to Opacity.
The quick brush Color Map also blends RGB independently of image Alpha, matching
the measured Pencil behavior. Use the closure's explicit Opacity field when Alpha
should affect visibility, or an explicit Mix when it should weight a color blend.
Width uses the calibrated raster response of the default quick Simple brush, including
subpixel opacity for very thin strokes. No second width-response option is added.

## Parameters and editable effect groups

Brush Parameters has no Brush input and no brush settings. Distance, Factor, Length,
ID and Closed are always shown, without a visibility menu. All refer to the
original stroke; final offsets cannot feed back into them. Parameters belong to their
paired closure. Reuse an algorithm as a node group instead of borrowing another
closure's parameter outputs. Artist controls use ordinary values or group inputs.
Factor stays in [0, 1], including rounded endpoints; Position Offset.X can still
extend an open stroke beyond its original ends.

**Add > Brush > Brush Effects** creates ordinary editable node groups:

| Group | Inputs | Result |
| --- | --- | --- |
| Wave | Distance, Amplitude, Wavelength, Phase (cycles) | Position Offset |
| Dashes | Distance, Length, Gap, Phase (px) | Opacity |
| Taper | Factor, Width | Width through an editable Float Curve |
| Per Stroke Random | ID, Seed, Min, Max | Value through Blender White Noise |

Enter a group to edit its algorithm. Group instances share algorithms and have
independent input values. The output does not contain a second implementation of
these effects. Math, Vector Math, Float Curve, White Noise, Color Ramp, Mix, Map Range,
images, groups and reroutes can feed its fields.
The Taper group applies its curve in the same raster size domain as the quick brush;
a plain `Width * Factor` expression instead requests a directly varying width.

The Add menu groups nodes under Brush, Input, Math, Color and Vector, and Line Setup,
followed by the native Node Groups, Layout and Assets menus. Menu search uses the same
entries. Category names, node labels, effect names and descriptions use Blender i18n;
Simplified Chinese includes the brush-specific meaning of Taper.

The final local position is `(Distance,0)+Position Offset`, mapped through
`C(X)+N(X)*Y`. Open ends extrapolate along the tangent; closed paths wrap by original
length. X remapping updates source sampling; Y retains the source surface.
Connection gaps restrict the source path, while a valid Simple endpoint's round cap
can overlap one naturally. This matches the quick brush instead of cutting a slit
through the full width. Explicit opacity masks still use the projected path coordinate.
Shared Field Sample Length is under Geometry Line Output quality settings and
preserves original boundaries and non-drawable connection gaps.

## Sources

Use UV Map > Image Texture, Color Attribute, or Material AOV. UV and Point/Corner
Float/Byte colors come from the evaluated source mesh, retaining correspondence
instead of guessing a new surface at the displaced screen position. Image nodes
honor Blender color spaces and Closest/Linear/Cubic/Smart filters. UDIM and non-Flat
projection are not supported and produce diagnostics.

Material AOV reads the current completed visible Deferred surface before Geometry
Lines and later Filters. Register the exact name/type and write it in the material.
Object/material/depth identity is checked. Hidden, offscreen, unwritten, mismatched
and unsupported Forward sources use the configured fallback. Valid black and zero
remain values. Model UV and attributes do not require visible surface AOV data.

## Preview, sharing and compatibility

The top preview updates automatically. The sidebar provides a larger swatch and
preview length (256 px default). Both sizes share one GPU evaluation, preserve Render
Result and remove disposable scenes and node trees. Image content, curves and group
instance changes invalidate the appropriate cache. Fitting a swatch never changes
the evaluated wavelength. The local test stroke has no model UV, attributes or AOV;
verify actual source values in a rendered scene viewport.
Continuous edits keep the current swatch; after editing pauses, the latest result
updates once. Node movement reuses the swatch. Preview and scene drawing share
EEVEE's shader cache, so disposable previews do not recreate the line shader programs.

Native zones supply pairing, duplicate, group/ungroup, undo and save/reload behavior.
Copying a shared closure duplicates its parameter boundary and computation nodes.
Nested group algorithms and images retain Blender's normal sharing semantics.

Legacy experimental Brush Input/Set Position/Brush Output nodes remain registered
for existing files, outside the new add menu. Their evaluator preserves old semantics.
The original quick Line Brush retains its features and the same Brush socket. Neither
workflow is inherited as hidden effect state by the new closure.

The four fields define continuous strokes. Dashes here are opacity masks; independent
dash end caps/tapers, true tip stamping, Multiple dot layouts and topology copies are
separate drawing capabilities. This is not a claim of complete Pencil+4 parity.
The quick brush's calibrated Simple dashes retain round caps. Explicit width and
opacity curves also apply around closed quick-brush paths; legacy open-end tapers
remain disabled on loops.

## Validation

`tests/python/npr/test_geometry_line_brush_closure.py` covers the minimal interface,
real pixels, ordinary effect groups, sources, persistent viewport, preview and cold
reload. The Geometry Lines Release case includes it on OpenGL and Vulkan. Native
window checks cover add/duplicate/pairing, undo/redo, grouping, independent copies and
readable preview layout. Earlier experiment suites remain compatibility regressions.
`test_geometry_line_brush_closure_endpoints.py` checks 144 orthographic/perspective
views of a short-wavelength wave with an editable taper. Its coverage must remain
inside the same path drawn at the curve's actual maximum width.
