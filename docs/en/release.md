# Download & Release

## Blender 5.2.0 LTS NPR Port

The current stable release was published on 2026-08-11 and is based on source revision `fd9fabb4f531`.

- **Release**: [`v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604)
- **Release title**: `Blender 5.2.0 NPR Port - fd9fabb4f531`

### Downloads

| Platform | File | Size | SHA256 |
|---|---|---:|---|
| Windows x64 | [`blender-5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604.zip`](https://github.com/bb-yi/blender/releases/download/v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604/blender-5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604.zip) | 403,722,980 bytes | `63709e3aa4c43ed190a83c82b3814f8343376737694bd774e1d97ddf91c8fdd7` |

### Integrity Check

Verify the SHA256 value after downloading. Windows PowerShell example:

```powershell
Get-FileHash .\blender-5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604.zip -Algorithm SHA256
```

Linux example:

```bash
sha256sum <downloaded-file>
```

macOS example:

```bash
shasum -a 256 <downloaded-file>
```

### Highlights in this release

#### New features

- EEVEE NPR refraction volume approximation, with sampling/capture isolation and background-leak fixes
- `GLSL Function` draw-view transform matrix helpers (view / projection / model)
- Viewport Shadow LOD visualization
- Improved IME support (Blender 5.2 API adaptation and text-editor cursor metrics)

#### Fixes and improvements

- Fixed NPR refraction volume black holes on Image Sample offsets, background misses, and depth mapping issues
- Fixed Sun `Shadow Map Scale` application to tilemaps and improved detail when scale increases
- White fallback for unconnected GLSL samplers
- Restored NPR Tree AOV output
- Stabilized Render Texture resource lifetime

Full history: [`blender-npr-release-changelog.md`](https://github.com/bb-yi/blender/blob/main/blender-npr-release-changelog.md).

### Support and Validation Scope

!!! important "Render engine support"
    Cycles is included in these packages, but the NPR Port extensions support EEVEE only. NPR Port nodes, Filter Graph, and the other EEVEE NPR features are not available when rendering with Cycles.

- The Windows x64 package is the fully validated baseline for this stable release and passed all `110/110` Release tests.
- When reporting an issue, include the platform, full asset filename, splash-screen version information, and minimal reproduction steps.

### Previous stable packages

| Date | Hash | Release |
|---|---|---|
| 2026-08-01 | `1257abb95445` | [`v5.2.0-npr-port-win64-1257abb95445-20260801-065135`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-1257abb95445-20260801-065135) |
| 2026-07-27 | `2c437ecb7c1b` | [`v5.2.0-npr-port-win64-2c437ecb7c1b-20260727-054725`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-2c437ecb7c1b-20260727-054725) |
| 2026-07-20 | `f0da4307f3ec` | [`v5.2.0-npr-port-win64-f0da4307f3ec-20260720-020117`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-f0da4307f3ec-20260720-020117) |
| 2026-07-16 | `c663d58f4da9` | [`v5.2.0-npr-port-win64-c663d58f4da9-20260716-165938`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-c663d58f4da9-20260716-165938) |
