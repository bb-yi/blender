# EEVEE Super Resolution runtime

SR is NGX SuperSampling (feature 1), not the feature-18 Neural Rendering model.
The optional Windows build setting `DLSS_SR_RUNTIME_DIR` installs `nvngx_dlss.dll`
to `dlss5/`. `DLSS5_RUNTIME_DIR` continues to install the independent NR binaries.
Missing SR must not prevent NR, and missing NR must not prevent SR.

## Development runtime provenance (2026-09-27)

- Official source: https://github.com/NVIDIA/DLSS
- Source commit: `374959484e79a640feaba44c93ac8cfb0a03f5b5`
- File: `lib/Windows_x86_64/rel/nvngx_dlss.dll`
- File version: `310.9.1.0`; size: 58,956,912 bytes.
- SHA-256: `3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983`
- Windows Authenticode: Valid, NVIDIA Corporation.
- SDK integration reference: https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md
- License: upstream `LICENSE.txt`; obtaining the development binary does not imply
  that this source repository redistributes the binary or grants additional rights.

The existing NR binary remains unchanged:
`nvngx_dlssnr.dll`, SHA-256
`4b8d19bc3eff58a084f5eca7489c921501c203450169fb82ff4f649a4482ba05`.
Its pre-existing Authenticode **HashMismatch** is retained as a known provenance
warning. No DLL signature or executable bytes were repaired by this change.

## Output contract

- SR accepts cropped low-resolution scene-linear HDR, reverse-Z depth,
  current-to-previous pixel motion and matching sample jitter. NR has its own
  motion conversion sign and executes after Film accumulation.
- Viewport Combined bypasses native TAA when SR is active. Offline SR evaluates
  each sample and Film averages full-resolution results; this can cost more than
  native rendering at high sample counts.
- A selected Value AOV mixes the HDR-recovered NR result against the base image.
  Black keeps the base exactly; non-finite values protect the base, including
  when inverted. Alpha belongs to the base image.
- Auxiliary passes and Color/Value AOVs are reconstructed by Film, not by DLSS.
  Continuous passes use four bilinear neighbors with matching jitter and sample
  accumulation. Z/Normal/Position use a nearest surface sample; Vector is converted
  to output pixel units. Cryptomatte accumulates coverage per unchanged ID.
  AOV `use_sr_nearest` preserves discrete samples without interpolation or sample
  averaging (storage precision is unchanged). Default AOVs remain continuous.
  AOVs, compositor inputs and multilayer EXR do not force native rendering.
  The explicit Output Precise Mask AOV opt-out still requests native rendering.
  Clearing the mask selection makes that mask-specific opt-out inactive.
  Resampled pass edges are not guaranteed to match neural color reconstruction.
- `BLENDER_DLSS_SR_TEST_FAILURE=after_first_sample` exercises the whole-frame
  native retry, not a spatial-upscale substitution.
  Retries restore the original render request time before rebuilding motion blur.

Run Release cases `009-dlss-sr-mask` and `011-dlss-sr-auxiliary` against an isolated
Vulkan installation. The auxiliary case checks pass pixels, coverage, output
motion units, discrete AOV persistence, and motion blur with internal vectors.
Runtime status and successful compilation alone are not pixel,
motion-stability or performance acceptance evidence.

`tests/dlss5/scripts/sr_state_transitions.py` is a self-contained regression for
mask selection transitions and native retry with multi-step motion blur. Run in
an isolated user directory with Vulkan and `-- --output-dir <outside-source-path>`.
It checks all shutter positions, nonzero subframes, disabled scene/layer motion
blur, and per-frame animation output against independent native renders.

## Offline lifetime and performance

Offline modules exclusively lease a cached session from their active GPU context.
The cache holds at most one idle SR session and one idle NR session per context;
it never stores scene, view-layer or Film pointers. Every lease forces a history
reset, so resources can be reused across animation frames without inheriting the
previous output frame's temporal history. Viewport sessions remain private.
Size/quality changes use the existing resource validation/recreation path.
Failed or untracked submissions are never returned to the idle cache.

Context destruction frees its idle entries with that context active. Engine
shutdown additionally drains idle caches **before** GPU/Python teardown and NGX
shutdown; waiting until the final context destructor regresses SR-only exit.
An untrackable submission retains its resources, as before.

SR prepares color/depth/motion in one MRT pass. Imported semaphores retire as a bundle
after both API users are finished; safety waits are not simply removed.

Diagnostic environment variables (not scene settings):

- `BLENDER_DLSS5_PROFILE=1`: CPU lifecycle scopes and existing NGX GPU timestamps.
  CPU scopes include nested waits/work and must not be summed with their children.
- `BLENDER_DLSS_DISABLE_SESSION_CACHE=1`: same-binary cache-off control.

Release case `010-dlss-session-cache` compares cached/uncached outputs,
animation/order independence, resize, and 100 stable frames.
Warm-frame improvements do not remove cold creation cost, per-sample SR cost, or
NR inference. Cached resources stay resident until their context is freed.
Separate interactive F12 jobs destroy their render context and therefore cannot
reuse these entries across jobs; frames within one animation job can reuse them.
Both diagnostic switches test variable presence, so remove the variable entirely
to disable it (an empty value is not disabled).
