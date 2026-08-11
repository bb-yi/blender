# 下载与版本

## Blender 5.2.0 LTS NPR Port

当前正式版本发布于 2026-08-11，源代码版本为 `fd9fabb4f531`。

- **Release**：[`v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604)
- **Release 标题**：`Blender 5.2.0 NPR Port - fd9fabb4f531`

### 下载

| 平台 | 文件 | 大小 | SHA256 |
|---|---|---:|---|
| Windows x64 | [`blender-5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604.zip`](https://github.com/bb-yi/blender/releases/download/v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604/blender-5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604.zip) | 403,722,980 bytes | `63709e3aa4c43ed190a83c82b3814f8343376737694bd774e1d97ddf91c8fdd7` |

### 完整性校验

下载后请核对 SHA256。Windows PowerShell 示例：

```powershell
Get-FileHash .\blender-5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604.zip -Algorithm SHA256
```

Linux 示例：

```bash
sha256sum <downloaded-file>
```

macOS 示例：

```bash
shasum -a 256 <downloaded-file>
```

### 本版本更新摘要

#### 新增功能

- EEVEE NPR 折射体积近似（refraction volume），并完成采样 / 捕获隔离与背景漏光相关修复
- `GLSL Function` 暴露 draw-view 变换矩阵 helper（视图 / 投影 / 模型）
- 视口 Shadow LOD 可视化
- 改进 IME 输入支持（含 5.2 API 适配与文本编辑器光标度量）

#### 修复与改进

- 修复 NPR 折射体积 Image Sample offset 黑洞、背景 miss、深度映射等问题
- 修复太阳阴影 `Shadow Map Scale` 对 tilemap 的应用，并使 scale 提升细节
- 未连接 GLSL sampler 使用白色 fallback
- 恢复 NPR Tree AOV 输出
- 稳定 Render Texture 资源生命周期

完整历史见仓库根目录 [`blender-npr-release-changelog.md`](https://github.com/bb-yi/blender/blob/main/blender-npr-release-changelog.md)。

### 支持与验证范围

!!! important "渲染引擎支持"
    发布包包含 Cycles，但 NPR Port 扩展功能仅支持 EEVEE。使用 Cycles 时不会获得 NPR Port 的扩展节点、Filter Graph 或其他 EEVEE NPR 功能。

- Windows x64 包是本次正式发布的完整验证基线，已通过 `110/110` Release 测试。
- 如需报告问题，请同时提供平台、完整文件名、启动画面版本信息和最小复现步骤。

### 历史正式包

| 日期 | 哈希 | Release |
|---|---|---|
| 2026-08-01 | `1257abb95445` | [`v5.2.0-npr-port-win64-1257abb95445-20260801-065135`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-1257abb95445-20260801-065135) |
| 2026-07-27 | `2c437ecb7c1b` | [`v5.2.0-npr-port-win64-2c437ecb7c1b-20260727-054725`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-2c437ecb7c1b-20260727-054725) |
| 2026-07-20 | `f0da4307f3ec` | [`v5.2.0-npr-port-win64-f0da4307f3ec-20260720-020117`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-f0da4307f3ec-20260720-020117) |
| 2026-07-16 | `c663d58f4da9` | [`v5.2.0-npr-port-win64-c663d58f4da9-20260716-165938`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-c663d58f4da9-20260716-165938) |
