# DLSSNR 310.9.16 Ada runtime profile

The local mainfix installation uses the externally supplied runtime identified by
`runtime_310_9_16_ada.json`. This is a locally adapted, unsigned binary validated on
an RTX 4080 with driver 616.92. It is not an NVIDIA-supported Ada release. The
original Blackwell images are preserved, but other GPU architectures were not tested.

The source DLL's SHA-256 is pinned in the manifest. `dev-camo/dlssnr-patcher` at the
pinned commit adds Ada cubins and updates the host architecture paths. Apply
`runtime_patches/acquire_fence.patch` to that checkout before building this version:
310.9.16 contains GPU-scope acquire fences which require sm_90. The patch strengthens
those to acquire-release fences while preserving scope and predicates. CUDA 13.3
`ptxas`, `fatbinary` and `cuobjdump` are required. The patcher compiles and verifies
all 15 fatbins before writing the output, and its 67 tests pass with the patch.

Example invocation from an external working directory:

```text
git apply --unidiff-zero BLENDER_SOURCE/tests/dlss5/runtime_patches/acquire_fence.patch
python dlssnr_patcher.py original/nvngx_dlssnr.dll --ada --cuda-bin CUDA_13_3_BIN --output runtime/nvngx_dlssnr.dll --work-dir build_ada
```

Keep the user-supplied input and generated NVIDIA binaries outside the source
repository. Verify the output hash against the manifest before installation.

## Build and installation

Configure `DLSS5_RUNTIME_DIR` to a persistent external directory containing
`nvngx_dlssnr.dll` and the companion `nvngx.dll`. CMake installs them under
`<install>/dlss5`. `DLSS_SR_RUNTIME_DIR` independently selects the SR binary.

In the Windows development workspace, the mainfix build/packaging launchers select
`tools/dlss5_runtime/mainfix` unless an explicit runtime override is supplied. The
launchers live in the outer workspace, outside this source Git repository. A clean
build on another machine must provide its own runtime path and binaries.

At runtime, use the driver's automatically selected NGX core. Do not force the
legacy companion `nvngx.dll` through `DLSS5_CORE_DLL`: it is a different loading
path, and forcing it fails the Blender initialization test on the validated setup.

## Validation and boundaries

The candidate passed a standalone D3D12 host test, Blender OFF/ON/OFF and zero-strength
checks with unchanged alpha, and all 15 scenarios in `test_animation_history.py`.
The latter covers repeated animation, reset/cache behavior, frame jumps, camera and
setting changes, view layers, motion blur, motion vectors, compositing, backward
playback, missing masks, small output, guide failure and Pre-FX isolation.

An additional 960x640 studio comparison used identical native input pixels for old
and new runtimes. Both produced finite output and reported Completed. This is not
a controlled performance benchmark or a Blackwell-reference equivalence test.
Production scenes, long-running interactive viewport sessions and other GPUs need
their own validation before deployment.
