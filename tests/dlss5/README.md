# DLSS5 / DLSS-NR validation

This directory contains the experimental EEVEE-side boundary and the
standalone Windows D3D12 host used to validate `nvngx_dlssnr.dll`.

## Blender UI test tool

The mainfix install includes the built-in `dlss5_npr_test` add-on. Start:

```text
install_windows_x64_vc17_Release_npr_post_mainfix\blender.exe
```

Enable **DLSS5 NPR Test** in Edit > Preferences > Add-ons. With an EEVEE
scene active, open Render Properties and use the **DLSS5 NPR** panel:

1. Set `Input Scale` to `0.50` for the currently validated same-resolution
   DLSS-NR test.
2. Leave the automatically detected Host, Runtime, Python and NGX Core paths
   unless the files are stored elsewhere. If the fields are empty, click
   **Detect Paths**.
3. Click **Run DLSSNR Test**.
4. Keep **Output Encoding** at **Preserve Input** for Blender BMP renders.
   **Encode sRGB** is only for a linear HDR input.
5. Use **DLSSNR**, **Input** and **Ground Truth** to load the three images into
   an Image Editor. **Open Output Folder** opens the complete run directory.

The operator temporarily installs a compositor that exports Depth and Vector,
renders the low-resolution input and full-resolution reference, invokes the
external D3D12 host, loads the PPM result into Blender, and restores the
original render settings and compositor by default. If the scene is configured
for video output, the operator temporarily switches the render media type to
image mode for the BMP capture, then restores the original video settings.
The host PPM is converted to `dlssnr.png` before loading because this Blender
build does not populate image pixels when a P6 PPM is loaded through
`bpy.data.images.load`. Outputs are written under
`temp\render_exports\dlss5\blender_ui\`.

This is an in-Blender offline test path. It does not yet replace the EEVEE
viewport or final-render color buffer with live DLSS output.

## Current scope

- The Blender source boundary is pass-through until a D3D12 executor or
  Vulkan-D3D12 interop path is available.
- The standalone host validates NGX initialization, feature 18 creation,
  resource upload, output UAV writes, readback and temporal history.
- The host does not link NVIDIA runtime binaries into Blender.

## Host build

Run from the workspace root:

```powershell
cmake -S blender_npr_post_mainfix/tests/dlss5/dlss5_host `
  -B temp/build/dlss5_host_mainfix `
  -DNGX_ROOT=temp/external/NVIDIA-DLSS
cmake --build temp/build/dlss5_host_mainfix --config Release --parallel 4
```

## The Junk Shop temporal case

The reproducible end-to-end runner is:

```powershell
.\temp\scripts\run_junk_shop_dlss5_temporal.ps1
```

It renders a static frame 1, injects a `+0.12` camera X shift for frame 2,
exports Blender Color/Depth/Vector passes, converts the pass files to the host
format, evaluates two frames through one feature handle, and compares frame 2
against a 960x480 Ground Truth.

The input contract is:

- Color: SDR RGB converted to `R16G16B16A16_FLOAT`;
- Depth: Blender `Depth.V` distance pass converted to approximate reverse-Z
  device depth;
- Motion: Blender Vector X/Y pass, already in pixel space;
- frame 1 uses `Reset=true`;
- frame 2 uses `Reset=false`.

## Current limitations

- The archived `.blend` is a static Blender 2.81 splash scene; animation in the
  temporal case is injected by the test script.
- The compositor Depth pass is not a direct readback of EEVEE `depth_tx`.
- The Blender EEVEE vector texture is internally packed and may use an RG16F
  RGRG swizzle; a production executor must resolve that encoding before NGX.
- The local RTX 4080 accepts same-resolution feature evaluation, but all tested
  non-matching output sizes returned `0xbad00005`
  (`NVSDK_NGX_Result_FAIL_InvalidParameter`).
- Same-resolution image quality remains below the input baseline, so this is
  not yet a production-ready DLSS-NR integration.

Detailed decisions and measured results are recorded in
`docs/architecture/adr-001-dlss5-npr-integration-boundary.md`.

## Installed EEVEE runtime regression

The standalone-host measurements above are historical. The current EEVEE integration
is tested by `test_runtime.py` against an installed Blender executable. Configure
`DLSS5_RUNTIME_DIR` in CMake (or supply the environment variable on initial configure)
to install `nvngx_dlssnr.dll` and `nvngx.dll` under the executable's `dlss5` directory.
Runtime lookup uses that directory without any dependency on a development workspace.

NGX data is stored in Blender's writable user cache. `DLSS5_CACHE_DIR` can override
the cache for an isolated test; DLL directories are never used for cache storage.

Run Blender with `--background --factory-startup --python-exit-code 1 --python
tests/dlss5/test_runtime.py`. Set `DLSS5_TEST_OUT` to an output directory outside the
source repository. The test clears development runtime overrides and launches seven
isolated Vulkan subprocess cases, asserting changed NR pixels, exact native bypass,
preserved Alpha and safe failure behavior.

`BLENDER_DLSS5_TEST_FAILURE` is an isolated regression hook with values `sync_init`,
`output_signal`, and `completion_signal`; it simulates API failure without removing
the GPU device. Never set it for normal operation. An untrackable GPU submission
disables the session and retains its resources until process exit/device removal,
rather than freeing memory still potentially used by the GPU; the status requests
a restart in this exceptional case.
