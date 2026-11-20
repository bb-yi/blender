# Draw Info

`Draw Info` is an Eevee material input node. All 22 context tests are exposed as
separate outputs, like Light Path. Each has Boolean semantics represented by a
float: `1` when the context is active, `0` otherwise. The node is available in the shader
editor's **Add > Input** menu and supports nested, shared shader node groups.
Old single-Result nodes retain their connections on the output corresponding to
their saved Query when the file is opened.

## Queries

| Query | Meaning |
| --- | --- |
| Is Outline Shell | The material graph is rooted at Outline Shell Output. This also applies to that shell's depth and shadow draws. Merely having an Outline Shell Output in the material does not make the body's evaluation return 1. |
| Is Shadow Pass | The current material variant draws a shadow map. This does not indicate whether the visible surface is in shadow. |
| Is Render Texture Capture | The current draw belongs to a Render Texture camera capture. Nested planar captures retain this outer capture context. |
| Is Probe Capture | A reflection probe capture or an irradiance surfel capture is being evaluated. |
| Is Image Render | The host task renders a final image or a viewport image. |
| Is Color Bake | The current material variant performs Eevee color baking. |
| Is Viewport | The host task is a viewport draw, including viewport image rendering. |
| Is Depth Prepass | The current variant evaluates a depth prepass. Shadow maps are excluded. |
| Is Deferred | A deferred GBuffer, hybrid, or NPR shading variant is being evaluated. Depth prepasses are excluded. |
| Is Forward | A forward shading variant is being evaluated. This does not by itself mean the surface is transparent. |
| Is NPR Evaluation | The graph is evaluated in an NPR shader context, including NPR world or bake evaluation when applicable. |
| Is Filter Pass | The graph is evaluated as a full-screen Filter material. |
| Is World | The graph is evaluated as a World shader. Filter materials are excluded even though their implementation uses full-screen world geometry. |
| Is Vertex Evaluation | The current expression runs in the vertex shader. |
| Is Fragment Evaluation | The current expression runs in the fragment shader. |
| Is Volume Evaluation | The material graph evaluates volume properties. |
| Is Sphere Probe Capture | The draw captures a spherical reflection probe, including the shared World shader. |
| Is Planar Probe Capture | The draw captures a planar reflection probe, including the shared World shader. |
| Is Irradiance Capture | The current draw captures surfels for irradiance baking. |
| Is Pre-FX Render | The draw belongs to the separate Pre-FX companion render. |
| Is Playback | The host viewport is playing animation. |
| Is Navigating | The host viewport is being navigated. |

The conditions are independent. A shell shadow draw can return `1` for both
**Is Outline Shell** and **Is Shadow Pass**. A viewport image render can return
`1` for both **Is Viewport** and **Is Image Render**. Use the outputs of one Draw Info
node and Math nodes to combine conditions.

## Material evaluation and AOVs

Draw Info reports the context of expressions that the renderer actually executes.
It does not cause opaque depth or shadow passes to execute a full surface shader,
and does not make post-processing operations re-evaluate object materials.
Changing Alpha, depth offset or displacement between a depth prepass and its
shading pass can make their coverage disagree.

Material AOV Output nodes execute for both the body and the Outline Shell.
For example, connect **Is Outline Shell** to a color Mix factor, set A to gray and
B to red, then connect the mixed color to AOV Output. The AOV contains gray on
the body and red on visible shell pixels. Color and Value AOVs are supported in
Deferred and Forward shell rendering. Shell AOV side outputs execute after shell
coverage tests, so zero-alpha and clipped shell fragments do not write AOVs.
They follow the existing AOV replacement semantics rather than beauty alpha blending.
A result of `0` is data, not an instruction to skip an AOV write.
Shader to RGB inputs are supported, including the Hybrid deferred path. Object
holdout suppresses both body and shell AOV writes through the object-info flags.

Render Texture and probe contexts are restored after capture. Queries describing
the host task stay valid inside auxiliary captures. The node is Eevee-specific;
MaterialX export uses a constant zero. Existing Render Info dimensions, sampling
outputs and identifiers are unchanged.

## Regression

`tests/python/npr/test_draw_info.py` checks actual linear render pixels, shader
stages and capture isolation. Set `BLENDER_TEST_OUTPUT_DIR` or pass `--output-dir`.
The checked-in legacy fixture covers all old Query values and a shared node group;
the converted copy is saved only in the configured test output directory.
Run the offline mode with an isolated Blender user directory. The `offscreen`
mode requires an isolated foreground instance and also checks that viewport image
rendering is simultaneously a viewport and an image render.
The `window` mode requires `--enable-event-simulate` and checks the native viewport,
animation playback, navigation, and restoration after both interactions. It saves
a node-editor preview and a small demo scene, then exits its own test instance.
