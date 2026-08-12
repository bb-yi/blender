---
hide:
  - navigation
---

<div class="npr-release-hero" markdown>

# Download Blender NPR Port

<p class="lead">
Current stable build is based on Blender <strong>5.2.0 LTS</strong>, source revision <code>fd9fabb4f531</code>, published on 2026-08-11.
Windows x64 is the fully validated baseline for this release.
</p>

<div class="npr-meta">
<span class="npr-chip">Release <strong>fd9fabb4f531</strong></span>
<span class="npr-chip">Tests <strong>110/110 passed</strong></span>
<span class="npr-chip">Engine note <strong>NPR = Eevee</strong></span>
</div>

</div>

<div class="npr-download-card" markdown>

<div markdown>
### Windows x64 package

<p class="file">blender-5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604.zip</p>

<div class="npr-stats">
  <div class="npr-stat"><span class="label">Size</span><span class="value">≈ 385 MB</span></div>
  <div class="npr-stat"><span class="label">Bytes</span><span class="value">403,722,980</span></div>
  <div class="npr-stat"><span class="label">Platform</span><span class="value">Win x64</span></div>
</div>

<div class="npr-hash"><strong>SHA256</strong><br>63709e3aa4c43ed190a83c82b3814f8343376737694bd774e1d97ddf91c8fdd7</div>
</div>

<div class="npr-dl-side" markdown>
<a class="npr-btn npr-btn--primary" href="https://github.com/bb-yi/blender/releases/download/v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604/blender-5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604.zip">⬇ Download ZIP</a>
<a class="npr-btn npr-btn--ghost" href="https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604">View GitHub Release</a>
<a class="npr-btn npr-btn--ghost" href="https://github.com/bb-yi/blender/releases">All releases</a>
</div>

</div>

## Integrity check

Verify the SHA256 hash after downloading.

=== "Windows PowerShell"

    ```powershell
    Get-FileHash .\blender-5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604.zip -Algorithm SHA256
    ```

=== "Linux"

    ```bash
    sha256sum blender-5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604.zip
    ```

=== "macOS"

    ```bash
    shasum -a 256 blender-5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604.zip
    ```

## Highlights in this release

<div class="npr-split" markdown>

<div markdown>
### New features
- EEVEE NPR refraction volume approximation, with sampling/capture isolation and background-leak fixes
- `GLSL Function` draw-view transform matrix helpers (view / projection / model)
- Viewport Shadow LOD visualization
- Improved IME support (Blender 5.2 API adaptation and text-editor cursor metrics)
</div>

<div markdown>
### Fixes and improvements
- Fixed NPR refraction volume black holes on Image Sample offsets, background misses, and depth mapping issues
- Fixed Sun `Shadow Map Scale` application to tilemaps and improved detail when scale increases
- White fallback for unconnected GLSL samplers
- Restored NPR Tree AOV output
- Stabilized Render Texture resource lifetime
</div>

</div>

Full history: [`blender-npr-release-changelog.md`](https://github.com/bb-yi/blender/blob/main/blender-npr-release-changelog.md).

## Support and validation scope

!!! important "Render engine support"
    Cycles is included in these packages, but the NPR Port extensions support **EEVEE** only. NPR Port nodes, Filter Graph, and the other EEVEE NPR features are not available when rendering with Cycles.

- The Windows x64 package is the fully validated baseline for this stable release and passed all `110/110` Release tests.
- When reporting an issue, include the platform, full asset filename, splash-screen version information, and minimal reproduction steps.

## Download table

| Platform | File | Size | SHA256 |
|---|---|---:|---|
| Windows x64 | [`blender-5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604.zip`](https://github.com/bb-yi/blender/releases/download/v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604/blender-5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604.zip) | 403,722,980 | `63709e3aa4c43ed190a83c82b3814f8343376737694bd774e1d97ddf91c8fdd7` |

## Previous stable packages

| Date | Hash | Release |
|---|---|---|
| 2026-08-01 | `1257abb95445` | [`v5.2.0-npr-port-win64-1257abb95445-20260801-065135`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-1257abb95445-20260801-065135) |
| 2026-07-27 | `2c437ecb7c1b` | [`v5.2.0-npr-port-win64-2c437ecb7c1b-20260727-054725`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-2c437ecb7c1b-20260727-054725) |
| 2026-07-20 | `f0da4307f3ec` | [`v5.2.0-npr-port-win64-f0da4307f3ec-20260720-020117`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-f0da4307f3ec-20260720-020117) |
| 2026-07-16 | `c663d58f4da9` | [`v5.2.0-npr-port-win64-c663d58f4da9-20260716-165938`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-c663d58f4da9-20260716-165938) |
