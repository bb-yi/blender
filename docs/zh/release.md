---
hide:
  - navigation
---

<div class="npr-release-hero" markdown>

<div class="npr-eyebrow">Release · fd9fabb4f531</div>

# 下载 Blender NPR Port

<p class="lead">
正式版基于 Blender <strong>5.2.0 LTS</strong>，源代码 <code>fd9fabb4f531</code>，发布于 2026-08-11。
Windows x64 是本次完整验证基线。
</p>

<div class="npr-meta">
<span class="npr-chip">Tests <strong>110/110</strong></span>
<span class="npr-chip">Engine <strong>NPR = Eevee</strong></span>
<span class="npr-chip">Platform <strong>Win x64</strong></span>
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

<a class="npr-btn npr-btn--primary" href="https://github.com/bb-yi/blender/releases/download/v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604/blender-5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604.zip">直接下载 ZIP</a>

<a class="npr-btn npr-btn--outline" href="https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604">GitHub Release</a>

<a class="npr-btn npr-btn--outline" href="https://github.com/bb-yi/blender/releases">全部历史</a>

</div>

</div>

<div class="npr-section-head" markdown>
<div class="npr-eyebrow">Integrity</div>

## 完整性校验
</div>

下载完成后请核对 SHA256。

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

<div class="npr-section-head" markdown>
<div class="npr-eyebrow">Notes</div>

## 本版本摘要
</div>

<div class="npr-split" markdown>

<div markdown>
### 新增
- EEVEE NPR 折射体积近似，并完成采样 / 捕获隔离与背景漏光修复
- `GLSL Function` draw-view 变换矩阵 helper（视图 / 投影 / 模型）
- 视口 Shadow LOD 可视化
- 改进 IME 输入支持（5.2 API 适配与文本编辑器光标度量）
</div>

<div markdown>
### 修复
- 折射体积 Image Sample offset 黑洞、背景 miss、深度映射
- 太阳阴影 `Shadow Map Scale` 对 tilemap 的应用
- 未连接 GLSL sampler 使用白色 fallback
- 恢复 NPR Tree AOV 输出
- 稳定 Render Texture 资源生命周期
</div>

</div>

完整历史：[`blender-npr-release-changelog.md`](https://github.com/bb-yi/blender/blob/main/blender-npr-release-changelog.md)。

<div class="npr-section-head" markdown>
<div class="npr-eyebrow">Support</div>

## 支持范围
</div>

!!! important "渲染引擎"
    发布包包含 Cycles，但 NPR Port 扩展仅支持 **EEVEE**。

- Windows x64 包是完整验证基线，已通过 `110/110` Release 测试。
- 报错时请提供：平台、完整文件名、启动画面版本、最小复现步骤。

## 下载信息

| 平台 | 文件 | 大小 | SHA256 |
|---|---|---:|---|
| Windows x64 | [`blender-5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604.zip`](https://github.com/bb-yi/blender/releases/download/v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604/blender-5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604.zip) | 403,722,980 | `63709e3aa4c43ed190a83c82b3814f8343376737694bd774e1d97ddf91c8fdd7` |

## 历史正式包

| 日期 | 哈希 | Release |
|---|---|---|
| 2026-08-01 | `1257abb95445` | [`v5.2.0-npr-port-win64-1257abb95445-20260801-065135`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-1257abb95445-20260801-065135) |
| 2026-07-27 | `2c437ecb7c1b` | [`v5.2.0-npr-port-win64-2c437ecb7c1b-20260727-054725`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-2c437ecb7c1b-20260727-054725) |
| 2026-07-20 | `f0da4307f3ec` | [`v5.2.0-npr-port-win64-f0da4307f3ec-20260720-020117`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-f0da4307f3ec-20260720-020117) |
| 2026-07-16 | `c663d58f4da9` | [`v5.2.0-npr-port-win64-c663d58f4da9-20260716-165938`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-c663d58f4da9-20260716-165938) |
