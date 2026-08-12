---
hide:
  - navigation
---

<div class="npr-release-hero" markdown>

<div class="npr-eyebrow">Release · fd9fabb4f531</div>

# Download Blender NPR Port

<p class="lead">
Stable build based on Blender <strong>5.2.0 LTS</strong>, source <code>fd9fabb4f531</code>, published 2026-08-11.
<strong>Windows x64</strong> is the fully validated baseline (Release tests 110/110). Linux and macOS packages ship in the same release.
</p>

<div class="npr-meta">
<span class="npr-chip">Tests <strong>110/110 · Win</strong></span>
<span class="npr-chip">Engine <strong>NPR = Eevee</strong></span>
<span class="npr-chip">Also <strong>Linux · macOS</strong></span>
</div>

</div>

<div class="npr-download-card" markdown>

<div markdown>
### Windows x64 · fully validated baseline

<p class="file">blender-5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604.zip</p>

<div class="npr-stats">
  <div class="npr-stat"><span class="label">Size</span><span class="value">≈ 385 MB</span></div>
  <div class="npr-stat"><span class="label">Bytes</span><span class="value">403,722,980</span></div>
  <div class="npr-stat"><span class="label">Platform</span><span class="value">Win x64</span></div>
</div>

<div class="npr-hash"><strong>SHA256</strong><br>63709e3aa4c43ed190a83c82b3814f8343376737694bd774e1d97ddf91c8fdd7</div>
</div>

<div class="npr-dl-side" markdown>

<a class="npr-btn npr-btn--primary" href="https://github.com/bb-yi/blender/releases/download/v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604/blender-5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604.zip">Download ZIP</a>

<a class="npr-btn npr-btn--outline" href="https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604">GitHub Release</a>

<a class="npr-btn npr-btn--outline" href="https://github.com/bb-yi/blender/releases">All releases</a>

</div>

</div>

<div class="npr-section-head" markdown>
<div class="npr-eyebrow">Also available</div>

## Other platforms (same-release companion builds)
</div>

!!! note "Validation scope"
    Linux and macOS ship on the same `fd9fabb4` line via GitHub Release. They did **not** run the full Windows `110/110` Release test matrix. Prefer the Windows package for production-critical paths, or re-validate on your platform.

| Platform | File | Size | SHA256 |
|---|---|---:|---|
| Linux x64 | [`blender-5.2.0-npr-port-linux64-fd9fabb4-20260811.tar.xz`](https://github.com/bb-yi/blender/releases/download/v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604/blender-5.2.0-npr-port-linux64-fd9fabb4-20260811.tar.xz) | 334,871,792 | `5cd57a808d90acbce26b82db652dad6cb03358666c8e6ad584214fae5b78909a` |
| macOS | [`blender-5.2.0-npr-port-macos-fd9fabb4-20260811.dmg`](https://github.com/bb-yi/blender/releases/download/v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604/blender-5.2.0-npr-port-macos-fd9fabb4-20260811.dmg) | 391,756,886 | `69f56e3eb70334b6a46093ad5a59b4be1472c11e0ee21c8270623af49023b0b8` |

<div class="npr-actions">
<a class="npr-btn npr-btn--outline" href="https://github.com/bb-yi/blender/releases/download/v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604/blender-5.2.0-npr-port-linux64-fd9fabb4-20260811.tar.xz">Download Linux tar.xz</a>
<a class="npr-btn npr-btn--outline" href="https://github.com/bb-yi/blender/releases/download/v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604/blender-5.2.0-npr-port-macos-fd9fabb4-20260811.dmg">Download macOS dmg</a>
</div>

<div class="npr-section-head" markdown>
<div class="npr-eyebrow">Integrity</div>

## Integrity check
</div>

Verify the SHA256 hash for your platform after downloading.

=== "Windows PowerShell"

    ```powershell
    Get-FileHash .\blender-5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604.zip -Algorithm SHA256
    ```

=== "Linux"

    ```bash
    sha256sum blender-5.2.0-npr-port-linux64-fd9fabb4-20260811.tar.xz
    # For the Windows zip:
    # sha256sum blender-5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604.zip
    ```

=== "macOS"

    ```bash
    shasum -a 256 blender-5.2.0-npr-port-macos-fd9fabb4-20260811.dmg
    # For the Windows zip:
    # shasum -a 256 blender-5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604.zip
    ```

<div class="npr-section-head" markdown>
<div class="npr-eyebrow">Notes</div>

## Release notes
</div>

<div class="npr-split" markdown>

<div markdown>
### New
- EEVEE NPR refraction volume approximation, with sampling/capture isolation and background-leak fixes
- `GLSL Function` draw-view matrix helpers: `glsl_view_matrix` / `glsl_projection_matrix` / `glsl_view_projection_matrix` and inverses, plus object-path `glsl_model_*` / `glsl_normal_matrix` (overscan / Film crop / TAA jitter)
- Viewport Shadow LOD visualization
- Improved IME support (5.2 API adaptation and text-editor cursor metrics)
</div>

<div markdown>
### Fixes
- Refraction-volume Image Sample offset black holes, background misses, depth mapping
- Sun `Shadow Map Scale` applied to tilemaps; higher scale increases effective detail
- White fallback for unconnected GLSL samplers
- Restored NPR Tree AOV output
- Stabilized Render Texture resource lifetime
</div>

</div>

Full history: [`blender-npr-release-changelog.md`](https://github.com/bb-yi/blender/blob/main/blender-npr-release-changelog.md). Matrix helper details: [Extended nodes · GLSL Function](extended-nodes.md#glsl-function).

<div class="npr-section-head" markdown>
<div class="npr-eyebrow">Support</div>

## Support scope
</div>

!!! important "Render engine"
    Cycles is included, but NPR Port extensions support **EEVEE** only.

- **Windows x64**: fully validated baseline; passed `110/110` Release tests.
- **Linux / macOS**: same-release companion builds; not covered by the full Windows test matrix — re-validate critical paths.
- When reporting issues, include platform, full filename, splash version, and minimal reproduction steps.

## Download table (all assets in this release)

| Platform | Validation | File | Size | SHA256 |
|---|---|---|---:|---|
| Windows x64 | Full 110/110 | [`...win64-fd9fabb4f531-20260811-235604.zip`](https://github.com/bb-yi/blender/releases/download/v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604/blender-5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604.zip) | 403,722,980 | `63709e3aa4c43ed190a83c82b3814f8343376737694bd774e1d97ddf91c8fdd7` |
| Linux x64 | Companion | [`...linux64-fd9fabb4-20260811.tar.xz`](https://github.com/bb-yi/blender/releases/download/v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604/blender-5.2.0-npr-port-linux64-fd9fabb4-20260811.tar.xz) | 334,871,792 | `5cd57a808d90acbce26b82db652dad6cb03358666c8e6ad584214fae5b78909a` |
| macOS | Companion | [`...macos-fd9fabb4-20260811.dmg`](https://github.com/bb-yi/blender/releases/download/v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604/blender-5.2.0-npr-port-macos-fd9fabb4-20260811.dmg) | 391,756,886 | `69f56e3eb70334b6a46093ad5a59b4be1472c11e0ee21c8270623af49023b0b8` |

## Previous stable packages

| Date | Hash | Release |
|---|---|---|
| 2026-08-01 | `1257abb95445` | [`v5.2.0-npr-port-win64-1257abb95445-20260801-065135`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-1257abb95445-20260801-065135) |
| 2026-07-27 | `2c437ecb7c1b` | [`v5.2.0-npr-port-win64-2c437ecb7c1b-20260727-054725`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-2c437ecb7c1b-20260727-054725) |
| 2026-07-20 | `f0da4307f3ec` | [`v5.2.0-npr-port-win64-f0da4307f3ec-20260720-020117`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-f0da4307f3ec-20260720-020117) |
| 2026-07-16 | `bab5a63ca3b8` | [`v5.2.0-npr-port-win64-bab5a63ca3b8-20260716-223405`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-bab5a63ca3b8-20260716-223405) |
