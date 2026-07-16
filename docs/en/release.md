# Download & Release

## Blender 5.2.0 LTS NPR Port

The current stable release was published on 2026-07-16 and is based on source revision `c663d58f4da9`.

- **Release**: [`v5.2.0-npr-port-win64-c663d58f4da9-20260716-165938`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-c663d58f4da9-20260716-165938)
- **Release title**: `Blender 5.2.0 NPR Port - c663d58f4da9`

### Downloads

| Platform | File | Size | SHA256 |
|---|---|---:|---|
| Windows x64 | [`blender-5.2.0-npr-port-win64-c663d58f4da9-20260716-165938.zip`](https://github.com/bb-yi/blender/releases/download/v5.2.0-npr-port-win64-c663d58f4da9-20260716-165938/blender-5.2.0-npr-port-win64-c663d58f4da9-20260716-165938.zip) | 403,583,383 bytes | `64f762d6749bca06145b60042febc068c7a8dd2184230ae516cacc0ec9d41fa6` |
| Linux x64 | [`blender-5.2.0-npr-port-linux64-c663d58f-20260716.tar.xz`](https://github.com/bb-yi/blender/releases/download/v5.2.0-npr-port-win64-c663d58f4da9-20260716-165938/blender-5.2.0-npr-port-linux64-c663d58f-20260716.tar.xz) | 334,859,976 bytes | `336cb0eba1cb38d7229a812538f00b0ba348773f746afde8f250613617c4a4b9` |
| macOS | [`blender-5.2.0-npr-port-macos-c663d58f-20260716.dmg`](https://github.com/bb-yi/blender/releases/download/v5.2.0-npr-port-win64-c663d58f4da9-20260716-165938/blender-5.2.0-npr-port-macos-c663d58f-20260716.dmg) | 380,602,074 bytes | `4f74f6d5bacf1426fdac7f36616710307bb70f3ff901596cbf291622fb2bdb27` |

### Integrity Check

Verify the SHA256 value after downloading. Windows PowerShell example:

```powershell
Get-FileHash .\blender-5.2.0-npr-port-win64-c663d58f4da9-20260716-165938.zip -Algorithm SHA256
```

Linux example:

```bash
sha256sum <downloaded-file>
```

macOS example:

```bash
shasum -a 256 <downloaded-file>
```

### Support and Validation Scope

!!! important "Render engine support"
    Cycles is included in these packages, but the NPR Port extensions support EEVEE only. NPR Port nodes, Filter Graph, and the other EEVEE NPR features are not available when rendering with Cycles.

- The Windows x64 package is the fully validated baseline for this stable release and passed all `92/92` Release tests.
- Linux x64 and macOS artifacts are provided in the same stable Release, but they are not claimed to have received the same complete runtime validation as Windows.
- When reporting an issue, include the platform, full asset filename, splash-screen version information, and minimal reproduction steps.
