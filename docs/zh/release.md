---
hide:
  - navigation
---

<div class="npr-release-hero" markdown>

<div class="npr-eyebrow">Release · 02993970c8cd</div>

# 下载 Blender NPR Port

<p class="lead">
正式版基于 Blender <strong>5.2.2 LTS</strong>，源代码 <code>02993970c8cd</code>，发布于 2026-09-28（UTC+8；GitHub UTC 日期为 09-27）。
<strong>Windows x64</strong> 为完整验证基线（Release 测试 129/129）；同版本另附 Linux / macOS 包。
</p>

<div class="npr-meta">
<span class="npr-chip">Tests <strong>129/129 · Win</strong></span>
<span class="npr-chip">Engine <strong>NPR = Eevee</strong></span>
<span class="npr-chip">Also <strong>Linux · macOS</strong></span>
</div>

</div>

<div class="npr-download-card" markdown>

<div markdown>
### Windows x64 · 完整验证基线

<p class="file">blender-5.2.2-npr-port-win64-02993970c8cd-20260928-053554.zip</p>

<div class="npr-stats">
  <div class="npr-stat"><span class="label">Size</span><span class="value">≈ 523 MiB</span></div>
  <div class="npr-stat"><span class="label">Bytes</span><span class="value">548,503,600</span></div>
  <div class="npr-stat"><span class="label">Platform</span><span class="value">Win x64</span></div>
</div>

<div class="npr-hash"><strong>SHA256</strong><br>b606a95cf4b5c35f150e05ba8ed8aee2cb12fb3153c2ad6f0114a42fa61bfcda</div>
</div>

<div class="npr-dl-side" markdown>

<a class="npr-btn npr-btn--primary" href="https://github.com/bb-yi/blender/releases/download/v5.2.2-npr-port-win64-02993970c8cd-20260928-053554/blender-5.2.2-npr-port-win64-02993970c8cd-20260928-053554.zip">直接下载 ZIP</a>

<a class="npr-btn npr-btn--outline" href="https://github.com/bb-yi/blender/releases/tag/v5.2.2-npr-port-win64-02993970c8cd-20260928-053554">GitHub Release</a>

<a class="npr-btn npr-btn--outline" href="https://github.com/bb-yi/blender/releases">全部历史</a>

</div>

</div>

<div class="npr-section-head" markdown>
<div class="npr-eyebrow">Also available</div>

## 其他平台（同版本附带包）
</div>

!!! note "验证范围"
    Linux / macOS 与 Windows 同属 `02993970` 构建线，已随 GitHub Release 发布；**未**跑与 Windows 同等的完整 `129/129` Release 测试矩阵。生产关键路径请优先用 Windows 包验证，或在对应平台自行回归。

| 平台 | 文件 | 大小 | SHA256 |
|---|---|---:|---|
| Linux x64 | [`blender-5.2.2-npr-port-linux64-02993970-20260927.tar.xz`](https://github.com/bb-yi/blender/releases/download/v5.2.2-npr-port-win64-02993970c8cd-20260928-053554/blender-5.2.2-npr-port-linux64-02993970-20260927.tar.xz) | 334,977,208 | `afde4865d05fdf2d7531e57bd26963639e5b028b0c4d0a8cc7427251fcf6875b` |
| macOS | [`blender-5.2.2-npr-port-macos-02993970-20260927.dmg`](https://github.com/bb-yi/blender/releases/download/v5.2.2-npr-port-win64-02993970c8cd-20260928-053554/blender-5.2.2-npr-port-macos-02993970-20260927.dmg) | 380,036,136 | `d069b10b7d61c6c0ef7ad710cfdb6aff53e8267fbd1a7491fa6c3e35ba767158` |

<div class="npr-actions">
<a class="npr-btn npr-btn--outline" href="https://github.com/bb-yi/blender/releases/download/v5.2.2-npr-port-win64-02993970c8cd-20260928-053554/blender-5.2.2-npr-port-linux64-02993970-20260927.tar.xz">下载 Linux tar.xz</a>
<a class="npr-btn npr-btn--outline" href="https://github.com/bb-yi/blender/releases/download/v5.2.2-npr-port-win64-02993970c8cd-20260928-053554/blender-5.2.2-npr-port-macos-02993970-20260927.dmg">下载 macOS dmg</a>
</div>

<div class="npr-section-head" markdown>
<div class="npr-eyebrow">Integrity</div>

## 完整性校验
</div>

下载完成后请按平台核对 SHA256。

本页下载链接、文件名、大小与校验值固定指向同一版 `02993970c8cd`，不会单独把按钮替换成未来版本。SHA256 与字节数来自 GitHub Release 资产信息；其他版本请到「全部历史」查看。

=== "Windows PowerShell"

    ```powershell
    Get-FileHash .\blender-5.2.2-npr-port-win64-02993970c8cd-20260928-053554.zip -Algorithm SHA256
    ```

=== "Linux"

    ```bash
    sha256sum blender-5.2.2-npr-port-linux64-02993970-20260927.tar.xz
    # 若校验 Windows zip：
    # sha256sum blender-5.2.2-npr-port-win64-02993970c8cd-20260928-053554.zip
    ```

=== "macOS"

    ```bash
    shasum -a 256 blender-5.2.2-npr-port-macos-02993970-20260927.dmg
    # 若校验 Windows zip：
    # shasum -a 256 blender-5.2.2-npr-port-win64-02993970c8cd-20260928-053554.zip
    ```

<div class="npr-section-head" markdown>
<div class="npr-eyebrow">Notes</div>

## 本版本摘要
</div>

<div class="npr-split" markdown>

<div markdown>
### 新增
- 更新到 Blender 5.2.2 LTS，合入 5.2.1 / 5.2.2 官方修复。
- [Principled NPR V2](extended-nodes.md#principled-npr-v2)：逐灯阴影分类、Total / Max Lighting、可控高光、深度边缘光和各向异性间接反射，保留旧材质兼容路径。
- [NPR Surface Diffusion 与 NPR Rim](extended-nodes.md#npr-surface-diffusion)：独立 NPR 添加菜单、屏幕空间表面扩散和边缘光塑形。
- [Outline Shell Output](extended-nodes.md#outline-shell-output)：独立材质变体、前向/延迟路由及剔除、深度、模板和阴影控制。
- [灯光着色参数](interface-guide.md#light-shader-parameters)：动态 Light Info 输出、动画、驱动、数据重映射与 GLSL Function 读取。
- [多层 EXR 逐层色彩空间](interface-guide.md#exr-layer-color-spaces)：节点继承 / OCIO 目标、色彩元数据和 Non-Color 数据层。
- [DLSS NR 与 Super Resolution](scene-extensions.md#dlss)：视口/渲染 SR 品质、自定义输入比例，以及辅助通道、AOV、覆盖率与 Cryptomatte 重建。
</div>

<div markdown>
### 修复
- 修复 DLSS SR 重建对齐、运动矢量与历史重置；辅助输出不再直接触发原生回退。
- 修复 DLSS NR 默认值、小尺寸渲染回退、运行库部署与失败恢复；移除冗余 Value Info 界面。
- 修复相机隐藏但参与阴影的对象丢失 Outline Shell 阴影。
- Principled NPR 按实际功能裁剪着色器编译路径，降低默认材质编译开销。
- 修复 NPR For Each Light 在延迟 NPR 与 EEVEE Color Bake 中缺少 shadow caster atlas 绑定导致的 OpenGL 编译失败。
</div>

</div>

正式说明以 [本版 GitHub Release](https://github.com/bb-yi/blender/releases/tag/v5.2.2-npr-port-win64-02993970c8cd-20260928-053554) 为准。完整历史：[`blender-npr-release-changelog.md`](https://github.com/bb-yi/blender/blob/main/blender-npr-release-changelog.md)。

<div class="npr-section-head" markdown>
<div class="npr-eyebrow">Support</div>

## 支持范围
</div>

!!! important "渲染引擎"
    发布包包含 Cycles，但 NPR Port 扩展仅支持 **EEVEE**。

- **Windows x64**：完整验证基线，已通过 `129/129` Release 测试。
- 上述数字来自本版发布时的测试记录，不表示本次文档更新重新执行了 Blender 测试，也不保证所有显卡与驱动组合。
- **DLSS**：当前运行路径要求 Windows、Vulkan、兼容的 NVIDIA GPU / 驱动与对应运行库；Linux / macOS 包不代表已具备相同 DLSS 支持。
- **Linux / macOS**：同版本附带包，未跑同等完整测试矩阵；请自行验证关键路径。
- 报错时请提供：平台、完整文件名、启动画面版本、最小复现步骤。

## 下载信息（本版本全部资产）

| 平台 | 验证 | 文件 | 大小 | SHA256 |
|---|---|---|---:|---|
| Windows x64 | 完整 129/129 | [`...win64-02993970c8cd-20260928-053554.zip`](https://github.com/bb-yi/blender/releases/download/v5.2.2-npr-port-win64-02993970c8cd-20260928-053554/blender-5.2.2-npr-port-win64-02993970c8cd-20260928-053554.zip) | 548,503,600 | `b606a95cf4b5c35f150e05ba8ed8aee2cb12fb3153c2ad6f0114a42fa61bfcda` |
| Linux x64 | 附带 | [`...linux64-02993970-20260927.tar.xz`](https://github.com/bb-yi/blender/releases/download/v5.2.2-npr-port-win64-02993970c8cd-20260928-053554/blender-5.2.2-npr-port-linux64-02993970-20260927.tar.xz) | 334,977,208 | `afde4865d05fdf2d7531e57bd26963639e5b028b0c4d0a8cc7427251fcf6875b` |
| macOS | 附带 | [`...macos-02993970-20260927.dmg`](https://github.com/bb-yi/blender/releases/download/v5.2.2-npr-port-win64-02993970c8cd-20260928-053554/blender-5.2.2-npr-port-macos-02993970-20260927.dmg) | 380,036,136 | `d069b10b7d61c6c0ef7ad710cfdb6aff53e8267fbd1a7491fa6c3e35ba767158` |

## 历史正式包

| 日期 | 哈希 | Release |
|---|---|---|
| 2026-09-11 | `680997dc4ccb` | [`v5.2.0-npr-port-win64-680997dc4ccb-20260911-220732`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-680997dc4ccb-20260911-220732) |
| 2026-08-11 | `fd9fabb4f531` | [`v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604) |
| 2026-08-01 | `1257abb95445` | [`v5.2.0-npr-port-win64-1257abb95445-20260801-065135`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-1257abb95445-20260801-065135) |
| 2026-07-27 | `2c437ecb7c1b` | [`v5.2.0-npr-port-win64-2c437ecb7c1b-20260727-054725`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-2c437ecb7c1b-20260727-054725) |
| 2026-07-20 | `f0da4307f3ec` | [`v5.2.0-npr-port-win64-f0da4307f3ec-20260720-020117`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-f0da4307f3ec-20260720-020117) |
| 2026-07-16 | `bab5a63ca3b8` | [`v5.2.0-npr-port-win64-bab5a63ca3b8-20260716-223405`](https://github.com/bb-yi/blender/releases/tag/v5.2.0-npr-port-win64-bab5a63ca3b8-20260716-223405) |
