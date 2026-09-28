---
hide:
  - navigation
---

<div class="npr-release-hero" markdown>

<div class="npr-eyebrow">Release · 02993970c8cd</div>

# Download Blender NPR Port

<p class="lead">
Stable build based on Blender <strong>5.2.2 LTS</strong>, source <code>02993970c8cd</code>, published 2026-09-28 (UTC+8; September 27 in GitHub UTC).
<strong>Windows x64</strong> is the fully validated baseline (Release tests 129/129). Linux and macOS packages ship in the same release.
</p>

<div class="npr-meta">
<span class="npr-chip">Tests <strong>129/129 · Win</strong></span>
<span class="npr-chip">Engine <strong>NPR = Eevee</strong></span>
<span class="npr-chip">Also <strong>Linux · macOS</strong></span>
</div>

</div>

<div class="npr-download-card" markdown>

<div markdown>
### Windows x64 · fully validated baseline

<p class="file">blender-5.2.2-npr-port-win64-02993970c8cd-20260928-053554.zip</p>

<div class="npr-stats">
  <div class="npr-stat"><span class="label">Size</span><span class="value">≈ 523 MiB</span></div>
  <div class="npr-stat"><span class="label">Bytes</span><span class="value">548,503,600</span></div>
  <div class="npr-stat"><span class="label">Platform</span><span class="value">Win x64</span></div>
</div>

<div class="npr-hash"><strong>SHA256</strong><br>b606a95cf4b5c35f150e05ba8ed8aee2cb12fb3153c2ad6f0114a42fa61bfcda</div>
</div>

<div class="npr-dl-side" markdown>

<a class="npr-btn npr-btn--primary" href="https://github.com/bb-yi/blender/releases/download/v5.2.2-npr-port-win64-02993970c8cd-20260928-053554/blender-5.2.2-npr-port-win64-02993970c8cd-20260928-053554.zip">Download ZIP</a>

<a class="npr-btn npr-btn--outline" href="https://github.com/bb-yi/blender/releases/tag/v5.2.2-npr-port-win64-02993970c8cd-20260928-053554">GitHub Release</a>

<a class="npr-btn npr-btn--outline" href="https://github.com/bb-yi/blender/releases">All releases</a>

</div>

</div>

<div class="npr-section-head" markdown>
<div class="npr-eyebrow">Also available</div>

## Other platforms (same-release companion builds)
</div>

!!! note "Validation scope"
    Linux and macOS ship on the same `02993970` line via GitHub Release. They did **not** run the full Windows `129/129` Release test matrix. Prefer the Windows package for production-critical paths, or re-validate on your platform.

| Platform | File | Size | SHA256 |
|---|---|---:|---|
| Linux x64 | [`blender-5.2.2-npr-port-linux64-02993970-20260927.tar.xz`](https://github.com/bb-yi/blender/releases/download/v5.2.2-npr-port-win64-02993970c8cd-20260928-053554/blender-5.2.2-npr-port-linux64-02993970-20260927.tar.xz) | 334,977,208 | `afde4865d05fdf2d7531e57bd26963639e5b028b0c4d0a8cc7427251fcf6875b` |
| macOS | [`blender-5.2.2-npr-port-macos-02993970-20260927.dmg`](https://github.com/bb-yi/blender/releases/download/v5.2.2-npr-port-win64-02993970c8cd-20260928-053554/blender-5.2.2-npr-port-macos-02993970-20260927.dmg) | 380,036,136 | `d069b10b7d61c6c0ef7ad710cfdb6aff53e8267fbd1a7491fa6c3e35ba767158` |

<div class="npr-actions">
<a class="npr-btn npr-btn--outline" href="https://github.com/bb-yi/blender/releases/download/v5.2.2-npr-port-win64-02993970c8cd-20260928-053554/blender-5.2.2-npr-port-linux64-02993970-20260927.tar.xz">Download Linux tar.xz</a>
<a class="npr-btn npr-btn--outline" href="https://github.com/bb-yi/blender/releases/download/v5.2.2-npr-port-win64-02993970c8cd-20260928-053554/blender-5.2.2-npr-port-macos-02993970-20260927.dmg">Download macOS dmg</a>
</div>

<div class="npr-section-head" markdown>
<div class="npr-eyebrow">Integrity</div>

## Integrity check
</div>

Verify the SHA256 hash for your platform after downloading.

Download links, filenames, sizes and hashes on this page are pinned to the same `02993970c8cd` release. Buttons do not silently switch to a future build. SHA256 and byte counts come from GitHub Release asset metadata; use “All releases” for other versions.

=== "Windows PowerShell"

    ```powershell
    Get-FileHash .\blender-5.2.2-npr-port-win64-02993970c8cd-20260928-053554.zip -Algorithm SHA256
    ```

=== "Linux"

    ```bash
    sha256sum blender-5.2.2-npr-port-linux64-02993970-20260927.tar.xz
    # For the Windows zip:
    # sha256sum blender-5.2.2-npr-port-win64-02993970c8cd-20260928-053554.zip
    ```

=== "macOS"

    ```bash
    shasum -a 256 blender-5.2.2-npr-port-macos-02993970-20260927.dmg
    # For the Windows zip:
    # shasum -a 256 blender-5.2.2-npr-port-win64-02993970c8cd-20260928-053554.zip
    ```

<div class="npr-section-head" markdown>
<div class="npr-eyebrow">Notes</div>

## Release notes
</div>

<div class="npr-split" markdown>

<div markdown>
### New
- Blender 5.2.2 LTS baseline, including upstream 5.2.1 / 5.2.2 fixes.
- [Principled NPR V2](extended-nodes.md#principled-npr-v2): per-light shadow classification, Total / Max Lighting, controlled highlights, depth rim lighting and anisotropic indirect reflections; legacy materials remain compatible.
- [NPR Surface Diffusion and NPR Rim](extended-nodes.md#npr-surface-diffusion): dedicated NPR add menu, screen-space surface diffusion and standalone rim shaping.
- [Outline Shell Output](extended-nodes.md#outline-shell-output): independent material variants, forward/deferred routing, culling, depth, stencil and shadow control.
- [Light shader parameters](interface-guide.md#light-shader-parameters): dynamic Light Info outputs, animation, drivers, data remapping and GLSL Function access.
- [Per-layer EXR color spaces](interface-guide.md#exr-layer-color-spaces): inherited / explicit OCIO targets, color metadata and Non-Color data layers.
- [DLSS NR and Super Resolution](scene-extensions.md#dlss): separate viewport/render SR quality, custom input scale, auxiliary passes, AOVs, coverage and Cryptomatte reconstruction.
</div>

<div markdown>
### Fixes
- Corrected DLSS SR reconstruction alignment, motion vectors and history reset; auxiliary outputs no longer directly force native fallback.
- Corrected NR defaults, small-render fallback, runtime deployment and recovery; removed the redundant Value Info UI.
- Retained Outline Shell shadows from objects hidden from the camera.
- Specialized Principled NPR shader paths by enabled features to reduce default-material compilation cost.
- Fixed missing shadow caster atlas bindings for NPR For Each Light in deferred NPR and EEVEE Color Bake, which caused OpenGL shader compilation failures.
</div>

</div>

Authoritative notes: [this GitHub Release](https://github.com/bb-yi/blender/releases/tag/v5.2.2-npr-port-win64-02993970c8cd-20260928-053554). Full history: [`blender-npr-release-changelog.md`](https://github.com/bb-yi/blender/blob/main/blender-npr-release-changelog.md).

<div class="npr-section-head" markdown>
<div class="npr-eyebrow">Support</div>

## Support scope
</div>

!!! important "Render engine"
    Cycles is included, but NPR Port extensions support **EEVEE** only.

- **Windows x64**: fully validated baseline; passed `129/129` Release tests.
- This count comes from the release-time test record, not a new Blender test run for this documentation update or a guarantee for every GPU / driver combination.
- **DLSS**: the current runtime path requires Windows, Vulkan, a compatible NVIDIA GPU / driver and the relevant runtime binaries. Linux / macOS packages do not imply equivalent DLSS support.
- **Linux / macOS**: same-release companion builds; not covered by the full Windows test matrix — re-validate critical paths.
- When reporting issues, include platform, full filename, splash version, and minimal reproduction steps.

## Download table (all assets in this release)

| Platform | Validation | File | Size | SHA256 |
|---|---|---|---:|---|
| Windows x64 | Full 129/129 | [`...win64-02993970c8cd-20260928-053554.zip`](https://github.com/bb-yi/blender/releases/download/v5.2.2-npr-port-win64-02993970c8cd-20260928-053554/blender-5.2.2-npr-port-win64-02993970c8cd-20260928-053554.zip) | 548,503,600 | `b606a95cf4b5c35f150e05ba8ed8aee2cb12fb3153c2ad6f0114a42fa61bfcda` |
| Linux x64 | Companion | [`...linux64-02993970-20260927.tar.xz`](https://github.com/bb-yi/blender/releases/download/v5.2.2-npr-port-win64-02993970c8cd-20260928-053554/blender-5.2.2-npr-port-linux64-02993970-20260927.tar.xz) | 334,977,208 | `afde4865d05fdf2d7531e57bd26963639e5b028b0c4d0a8cc7427251fcf6875b` |
| macOS | Companion | [`...macos-02993970-20260927.dmg`](https://github.com/bb-yi/blender/releases/download/v5.2.2-npr-port-win64-02993970c8cd-20260928-053554/blender-5.2.2-npr-port-macos-02993970-20260927.dmg) | 380,036,136 | `d069b10b7d61c6c0ef7ad710cfdb6aff53e8267fbd1a7491fa6c3e35ba767158` |

## Previous stable packages

| Date | Hash | Release |
|---|---|---|
| 2026-09-11 | `680997dc4ccb` | [`v5.2.0-npr-port-win64-680997dc4ccb-20260911-220732`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-680997dc4ccb-20260911-220732) |
| 2026-08-11 | `fd9fabb4f531` | [`v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604) |
| 2026-08-01 | `1257abb95445` | [`v5.2.0-npr-port-win64-1257abb95445-20260801-065135`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-1257abb95445-20260801-065135) |
| 2026-07-27 | `2c437ecb7c1b` | [`v5.2.0-npr-port-win64-2c437ecb7c1b-20260727-054725`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-2c437ecb7c1b-20260727-054725) |
| 2026-07-20 | `f0da4307f3ec` | [`v5.2.0-npr-port-win64-f0da4307f3ec-20260720-020117`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-f0da4307f3ec-20260720-020117) |
| 2026-07-16 | `bab5a63ca3b8` | [`v5.2.0-npr-port-win64-bab5a63ca3b8-20260716-223405`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-bab5a63ca3b8-20260716-223405) |
