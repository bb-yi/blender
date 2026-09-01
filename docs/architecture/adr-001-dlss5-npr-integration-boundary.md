# DLSS5 NPR Integration Architecture Decision Record

- **Status:** In progress / experimental mainfix worktree
- **Date:** 2026-08-29
- **Branch:** `feat/viewport-value-hud`
- **Baseline:** `v5.2.0-npr-port-win64-fd9fabb4f531` (`fd9fabb4f531`)
- **Active worktree:** `E:\blender_bulid_test\blender_npr_bulid\blender_npr_post_mainfix`

## Context

The local runtime contains `nvngx_dlssnr.dll` 310.8.0 and ReShade addons that run DLSS5 through a D3D12 NGX session. UnityDLSSNR uses the same signed snippet through a native Unity D3D12 plugin.

The NPR Blender branch uses EEVEE's existing render buffers:

- `RenderBuffers::combined_tx` — rendered color;
- `RenderBuffers::depth_tx` — hardware depth;
- `RenderBuffers::vector_tx` — motion-vector buffer;
- `Film::pixel_jitter_get()` and `VelocityModule` — temporal camera/object data.

However, Blender's GPU abstraction currently exposes OpenGL, Vulkan, Metal and dummy backends, not D3D12. The DLSSNR snippet exposes D3D12 NGX entry points only. `GPU_texture.hh` has no cross-backend native texture handle API; Vulkan's `VKTexture` has an internal `VkImage` and external-memory export, while OpenGL only has a backend-local texture name.

## Decision

Use a two-stage architecture:

1. **EEVEE adapter boundary first.** Add a small `Dlss5Module` at the existing `ShadingView::render` boundary, after EEVEE post effects and before `Film::accumulate`. It receives color/depth/vector textures, render/display extents, jitter and temporal-reset state. With no runtime/backend enabled it returns the input unchanged and emits diagnostics. This is a compile-time and data-contract validation layer, not a fake DLSS implementation.
2. **Offline CPU-staging executor first.** For the first real output, read the three EEVEE textures to CPU memory, invoke a separate D3D12 host that loads `nvngx_dlssnr.dll`, and upload the result back. This validates NGX initialization, parameter conventions and image quality without adding a D3D12 backend to Blender.
3. **Vulkan-D3D12 interop second.** Only after the offline executor produces valid output, investigate Windows external-memory and fence sharing between Blender's Vulkan image and a D3D12 resource. This requires explicit public GPU handle/synchronization APIs and vendor/backend-specific code.
4. **Do not add a Blender D3D12 backend in this experiment.** A full backend would duplicate a large part of Blender's GPU abstraction and is not justified by the current DLSS5 test objective.

## Data contract

The adapter will pass the following logical inputs:

| Input | Source | Conversion/requirement |
|---|---|---|
| Color | EEVEE `combined_tx` after post FX | Linear HDR preferred; format conversion may be required |
| Depth | EEVEE `depth_tx` | Blender depth uses Blender's depth convention; convert to the NGX convention |
| Motion vectors | EEVEE `vector_tx` | Resolve to screen-space pixels; account for viewport RG16F swizzle |
| Render extent | `Film::render_extent_get()` | Low-resolution input extent |
| Display extent | Film display extent | DLSS output extent |
| Jitter | `Film::pixel_jitter_get()` / view matrices | Must match the frame that produced Color |
| Exposure | Color-management/exposure state | Must match the linear/HDR convention |
| Reset | viewport history invalidation / camera changes | Reset DLSS history and recreate feature when required |

## Insertion point

The first adapter call belongs around:

```text
source/blender/draw/engines/eevee/eevee_view.cc
ShadingView::render()
  render_postfx(...)
  Dlss5Module::process(color, depth, vector, frame_data)
  film.accumulate(...)
```

This point has all three required render buffers and keeps outlines/post effects ordering explicit. It is not yet the final production placement: the existing `Film::accumulate` performs its own temporal accumulation and maps render extent to display extent. A production DLSS mode must either:

- make DLSS produce display extent and add a Film path that does not resample it as low-resolution input; or
- move DLSS after Film's temporal resolve, while preserving depth/vector/jitter metadata.

The first branch only records and validates this boundary.

## Alternatives considered

### A. ReShade injection into Blender
Rejected for this branch. Blender does not call `NVSDK_NGX_D3D11_EvaluateFeature`, has no D3D11 swap chain, and the supplied bridge's hook contract is specifically a D3D11-game contract.

### B. External D3D12 process with file exchange
Useful for quality validation but not an interactive feature. Kept as the first real executor because it has low Blender coupling and clear failure isolation.

### C. Vulkan image exported directly to D3D12
Potentially the long-term interactive route on Windows, but requires external image memory and fence handles, compatible heap/resource descriptions, row/format guarantees and lifetime coordination. It cannot be implemented safely through the current public GPU API alone.

### D. New Blender D3D12 GPU backend
Technically direct, but the largest scope: device/context, texture, framebuffer, shader, descriptor, synchronization, window and shader compiler paths. It is disproportionate to testing one D3D12-only vendor feature.

## Failure handling

- Missing `nvngx_dlssnr.dll`, NGX core or unsupported driver: adapter remains pass-through; log a diagnostic once per session.
- Missing depth/vector or unsupported format: pass-through or explicitly configured degraded mode; never feed an ambiguous texture silently.
- Runtime initialization/evaluation failure: release the feature, mark runtime unavailable for the session and preserve the normal EEVEE frame.
- Resource/synchronization failure in interop: disable DLSS5 and leave Blender's original rendering path intact.

## Verification gates

1. Branch starts exactly at the previous NPR release tag.
2. Default build has no DLSS runtime dependency and produces byte-for-byte equivalent rendering behavior outside diagnostics.
3. Adapter receives matching Color/Depth/Vector extents at the chosen insertion point.
4. Offline D3D12 host initializes the signed snippet and evaluates one frame.
5. Multi-frame test demonstrates correct motion-vector orientation and history reset.
6. Only after gates 1–5 pass, prototype Vulkan-D3D12 sharing and measure end-to-end frame time.

## Consequences

- The first branch is useful even without an RTX/DLSS-capable machine: it validates Blender-side plumbing and preserves a clean fallback.
- The offline path has a CPU readback/upload cost and is not suitable for viewport use.
- A truly interactive implementation needs either a D3D12 Blender backend or a Vulkan-D3D12 interop layer; the adapter alone does not solve ownership/synchronization.
- The DLSS5 runtime remains an external, optional NVIDIA binary and is not committed to the Blender source repository.

## Handoff validation update

The offline host was moved to the mainfix worktree and now records independent
R/G/B/A statistics, a float RGBA readback, and an Output sentinel test. The
host also has two controls that do not load NGX:

- `--control-upload` uploads the input directly into the output texture.
- `--control-copy` copies the input texture with `CopyResource`.

Both controls produced identical 256x144 results:

```text
R mean=0.482823  G mean=0.428213  B mean=0.451247  A mean=1.000000
```

This also fixed a real D3D12 lifetime bug: upload buffers are now retained
until the submission fence completes. Before that fix, the debug layer reported
command lists referencing deleted resources and the driver returned
`DXGI_ERROR_DEVICE_HUNG`.

With `nvngx_dlssnr.dll` 310.8.0 on the local RTX 4080, same-resolution
evaluation returned success and overwrote all sentinel pixels. The measured
means were:

| Color encoding | R | G | B | A |
|---|---:|---:|---:|---:|
| BMP values (`srgb`) | 0.458602 | 0.428631 | 0.471210 | 1.000000 |
| sRGB inverse (`linear`) | 0.278910 | 0.245660 | 0.314458 | 1.000000 |
| linear multiplied by 4 (`linear4`) | 0.777951 | 0.558308 | 0.578769 | 1.000000 |

These runs prove that the feature writes the output resource, but they do not
prove image quality. After fixing upload-buffer lifetime, the `srgb` output
compared against the 512x288 Blender reference produced:

```text
same_resolution_vs_lanczos: MAE=8.4616 PSNR=26.5336 dB
input_vs_lanczos:           MAE=1.7744 PSNR=34.5942 dB
```

The DLSSNR output is no longer the earlier near-black invalid result, but it
still loses to the unprocessed input control. The color-space/exposure and
feature parameter contract therefore remains unresolved.

The host now computes `DLSSNR.Scale` and `DLSSNR.ScalingRatio` from the actual
input/output width when upscaling is requested. Despite that correction, all
of the following returned `NVSDK_NGX_Result_FAIL_InvalidParameter`
(`0xbad00005`):

- 256x144 -> 384x216 with `Upscaling=true`;
- 256x144 -> 448x252 with `Upscaling=true`;
- 256x144 -> 512x288 with `Upscaling=true`;
- 256x144 -> 512x288 with `Upscaling=false`.

The current decision is therefore unchanged: keep same-resolution DLSSNR as
the active investigation target, and do not start Vulkan-D3D12 interop or a
Blender D3D12 backend until the same-resolution color contract produces a
valid reference-quality result. Matrix logs and raw outputs are under
`temp\logs\dlss5\` and `temp\render_exports\dlss5\s01\`.

## Blender sample-scene validation

The Blender Institute Archive sample from the Blender 2.81 splash-screen
gallery was downloaded into the external test area, without modifying the
source checkout:

```text
temp\external\blender-samples\blender-2-81\
  thejunkshopsplashscreen-35a35553b3dd4f8c8fb5a6ccc5065ff1.blend
size: 412530532 bytes
sha256: 33191670EF370DCAA5FA2483C7D75ABC9B8CE106ECEA33CD155DBFC95FF649D2
```

The file opens in the mainfix Blender 5.2 build with two scenes, 64 objects,
38 materials and 146 packed images. The `Extended` scene was selected and
forced to EEVEE. The old file reports that its `CYCLES` engine is unavailable
in the current startup configuration, but the EEVEE render completes and the
packed textures are usable.

The fixed comparison used a 480x240 input and a 960x480 Ground Truth. The
input included the rendered SDR Color plus compositor-exported Depth and
Vector passes. Depth was converted from Blender's distance-style `Depth.V`
pass to an approximate reverse-Z device-depth value using the scene camera
clip range `0.1..100.0`; it is not yet a direct readback of EEVEE's
`depth_tx`. The static Vector pass was all zero.

Results for the `Extended` scene:

| Output | MAE vs 960x480 Lanczos reference | PSNR |
|---|---:|---:|
| CopyResource control | 1.9572 | 35.9851 dB |
| DLSSNR same-resolution | 5.8005 | 29.2709 dB |

The control path establishes that the sample render, CPU staging, texture
copy and readback remain coherent at 480x240. DLSSNR writes a valid image and
preserves the scene structure, but its error is higher than the input control.
This sample therefore validates the test harness on a real Blender project,
not the final image-quality contract. The next implementation target remains
direct EEVEE depth/motion extraction and color-management alignment before
any live interop work.

## Temporal validation update

The sample has no built-in camera or object animation. A deterministic test
animation was injected only in the render script: frame 1 uses the original
camera location and frame 2 shifts the camera by `+0.12` on X. The frame 2
Blender Vector pass is non-zero (`X max=5.367188`, `Y max=1.022461`) while the
static frame remains zero.

The host now accepts a second Color/Depth/Vector set and evaluates both frames
through the same feature handle:

```text
frame 1: Reset=true
frame 2: Reset=false
```

The second-frame output changes when history is enabled. The reset regression
also passes: `--second-reset` produces the same result as a single-frame
evaluation of frame 2 with reset enabled. The default host motion scale is now
automatically `1.0` when a Blender Vector render pass is supplied, because
`eevee_film.bsl.hh` has already converted the resolved velocity to pixel space.
The old `-width/-height` default is retained only for synthetic UV-space input
without a Blender Vector pass.

For the temporal The Junk Shop run with the source-correct pixel-space motion
scale:

```text
input:  480x240
truth:  960x480
DLSSNR two-frame vs frame-2 Lanczos:
MAE  = 5.6986
PSNR = 29.2571 dB
input baseline:
MAE  = 1.9637
PSNR = 35.8851 dB
```

The motion/depth contract is now observable and the history path is proven,
but the image result still trails the input baseline. A depth-contract matrix
using scene depth, reverse-Z depth, and both inversion flags produced identical
outputs on this runtime/configuration, which suggests that this feature/runtime
combination is not materially responding to that parameter in the current
test. The next adapter task is therefore direct EEVEE GPU readback/format
inspection, plus runtime capability reporting, rather than further scale
tuning.

## In-Blender test tool

The validated offline host is exposed through the built-in
`scripts/addons_core/dlss5_npr_test` add-on. It adds a `DLSS5 NPR` panel to
Render Properties for EEVEE scenes. The operator snapshots the active scene,
renders the scaled input and full-resolution reference, exports Depth and
Vector, invokes the D3D12 host, loads the DLSSNR/Input/Ground Truth images into
Blender, shows MAE/PSNR, and restores the original render settings and
compositor by default.

The installed add-on was exercised from the mainfix Blender installation
without importing the source directory. A 128x72 self-test rendered a 64x36
input, returned `{'FINISHED'}`, loaded `DLSS5 NR Result`, and wrote a comparison
log. This is now the supported user-facing offline test path; live EEVEE
viewport replacement still requires a D3D12 backend or Vulkan-D3D12 interop.

## Blender UI output-encoding correction (2026-08-30)

The first installed UI run exposed a preview-only encoding regression. The
operator rendered a Blender BMP, which already contains the display-referred
view transform, then passed `--output-encoding srgb` to the host. The host
therefore applied a second sRGB transfer to the DLSSNR result. The resulting
preview was visibly washed out and measured:

```text
MAE=62.8344  PSNR=11.9259 dB
```

The add-on now defaults to `RAW`, labelled **Preserve Input**, for Blender BMP
renders. The host still receives the same `--color-encoding srgb` input mode;
only the PPM preview writer stops applying a second transfer function. The
same saved input/depth/vector files rerun with `--output-encoding raw`
measured:

```text
MAE=5.7975  PSNR=29.8901 dB
```

The installed mainfix Blender build was rebuilt and the add-on was rechecked:

```text
ADDON=True
PANEL=True
OPERATOR=True
DEFAULT_OUTPUT_ENCODING=RAW
```

The real `Extended` scene from the Blender 2.81 splash archive also completed
through the Blender operator with `{'FINISHED'}` and restored its original
render-pass settings. This fixes the UI preview path; it does not change the
remaining DLSSNR quality gap against the low-resolution input baseline.

## Video-output scene compatibility (2026-08-30)

The active MCP scene exposed a second Blender RNA constraint: its render
settings were `media_type=VIDEO` and `file_format=FFMPEG`. In that state the
file-format setter only accepts `FFMPEG`, so assigning `BMP` directly fails
even though BMP appears in the static RNA enum list.

The operator now:

1. snapshots `media_type` and `file_format`;
2. switches the scene render settings to `media_type=IMAGE` and `file_format=BMP`;
3. switches the temporary Depth/Vector compositor outputs to
   `media_type=MULTI_LAYER_IMAGE` and `file_format=OPEN_EXR_MULTILAYER`;
4. restores the original media type before restoring its original file format.

The fix was exercised through the Blender MCP session on the active 1600x1200
scene. It rendered an 800x600 input and returned:

```text
DLSS5_RESULT={'FINISHED'}
status=PASS: 800x600 -> same resolution
restored_media=VIDEO
restored_format=FFMPEG
```

## Blender image preview loading (2026-08-30)

The host writes a valid P6 PPM, but this Blender build loads that file into an
image datablock with `size=(0, 0)` and `has_data=False`. The Image Editor then
shows only the checkerboard transparency background even though the PPM on
disk contains the expected pixel payload.

The comparison helper already converts the host PPM to `dlssnr.png`, so the
add-on now loads that PNG and verifies `image.size` and `image.has_data` before
assigning it to `result_image`. The active MCP scene was rerun after the fix:

```text
image=DLSS5 NR Result.002
size=(800, 600)
has_data=True
DLSS5_RESULT={'FINISHED'}
```

## Real-time interop gate (2026-08-30)

The official Blender `dlss` reference branch was inspected at
`025b1d4c5b4d8d5bf7f86a3f1680bca7d76efc13`. Its implementation is a Cycles
CUDA DLSS denoiser; it provides reusable session lifetime, capability-cache,
lazy recreation and jitter/reset handling, but it is not an EEVEE DLSSNR
backend.

The mainfix worktree now contains an independent Vulkan/D3D12 interop probe
under `tests/dlss5/vulkan_d3d12_interop`. On the local RTX 4080 it reported:

```text
VK_KHR_external_memory=yes
VK_KHR_external_memory_win32=yes
VK_KHR_external_semaphore=yes
VK_KHR_external_semaphore_win32=yes
R16G16B16A16_SFLOAT D3D12_RESOURCE import=1 dedicated=1
D3D12_FENCE semaphore import=1
matching D3D12 device=yes
DLSS5_INTEROP_BASELINE_READY: YES
```

The follow-up shared-resource smoke passed:

```text
Vulkan clear -> shared D3D12 resource -> D3D12 readback
readback=0.125,0.25,0.5,1
DLSS5_VULKAN_D3D12_SHARED_RESOURCE: PASS
```

This changes the implementation direction from Vulkan exporting its existing
EEVEE allocation to D3D12. The current driver advertises Vulkan import of
D3D12-owned resources, so the real-time path should allocate D3D12-owned
Color/Depth/Vector/Output resources, import them into Vulkan, and copy EEVEE
buffers into them. EEVEE's depth buffer is `D32_SFLOAT_S8_UINT`, while the
DLSSNR host consumes `R32_FLOAT`; a GPU depth conversion pass is required.
The viewport vector buffer is `RG16_FLOAT` with the existing `rgrg` swizzle
on the storage texture and must be copied without losing the logical XY
interpretation.

The next implementation gate is therefore an in-process D3D12 NGX session
with CPU-side queue waits first. Shared fence submission will replace those
waits only after the session produces a correct EEVEE frame.
