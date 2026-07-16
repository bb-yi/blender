# 下载与版本

## Blender 5.2.0 LTS NPR Port

当前正式版本发布于 2026-07-16，源代码版本为 `c663d58f4da9`。

- **Release**：[`v5.2.0-npr-port-win64-c663d58f4da9-20260716-165938`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-c663d58f4da9-20260716-165938)
- **Release 标题**：`Blender 5.2.0 NPR Port - c663d58f4da9`

### 下载

| 平台 | 文件 | 大小 | SHA256 |
|---|---|---:|---|
| Windows x64 | [`blender-5.2.0-npr-port-win64-c663d58f4da9-20260716-165938.zip`](https://github.com/bb-yi/blender/releases/download/v5.2.0-npr-port-win64-c663d58f4da9-20260716-165938/blender-5.2.0-npr-port-win64-c663d58f4da9-20260716-165938.zip) | 403,583,383 bytes | `64f762d6749bca06145b60042febc068c7a8dd2184230ae516cacc0ec9d41fa6` |
| Linux x64 | [`blender-5.2.0-npr-port-linux64-c663d58f-20260716.tar.xz`](https://github.com/bb-yi/blender/releases/download/v5.2.0-npr-port-win64-c663d58f4da9-20260716-165938/blender-5.2.0-npr-port-linux64-c663d58f-20260716.tar.xz) | 334,859,976 bytes | `336cb0eba1cb38d7229a812538f00b0ba348773f746afde8f250613617c4a4b9` |
| macOS | [`blender-5.2.0-npr-port-macos-c663d58f-20260716.dmg`](https://github.com/bb-yi/blender/releases/download/v5.2.0-npr-port-win64-c663d58f4da9-20260716-165938/blender-5.2.0-npr-port-macos-c663d58f-20260716.dmg) | 380,602,074 bytes | `4f74f6d5bacf1426fdac7f36616710307bb70f3ff901596cbf291622fb2bdb27` |

### 完整性校验

下载后请核对 SHA256。Windows PowerShell 示例：

```powershell
Get-FileHash .\blender-5.2.0-npr-port-win64-c663d58f4da9-20260716-165938.zip -Algorithm SHA256
```

Linux 示例：

```bash
sha256sum <downloaded-file>
```

macOS 示例：

```bash
shasum -a 256 <downloaded-file>
```

### 支持与验证范围

!!! important "渲染引擎支持"
    发布包包含 Cycles，但 NPR Port 扩展功能仅支持 EEVEE。使用 Cycles 时不会获得 NPR Port 的扩展节点、Filter Graph 或其他 EEVEE NPR 功能。

- Windows x64 包是本次正式发布的完整验证基线，已通过 `92/92` Release 测试。
- Linux x64 与 macOS 产物已随同一正式 Release 提供，但不声明经过与 Windows 相同的完整运行时测试。
- 如需报告问题，请同时提供平台、完整文件名、启动画面版本信息和最小复现步骤。
