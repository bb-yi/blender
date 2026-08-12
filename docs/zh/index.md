---
hide:
  - navigation
  - toc
---

<div class="npr-hero" markdown>

<div class="npr-eyebrow">Blender 5.2.0 LTS · NPR Port · fd9fabb4f531</div>

# Eevee 上的风格化渲染扩展

<p class="lead">
把 NPR 能力收成四条可直接开工的路径：Scene 管线、扩展节点、NPR Tree、界面控制。
安装包带 Cycles；本站文档只覆盖 <strong>Eevee</strong> 扩展。
</p>

<div class="npr-actions">
<a class="npr-btn npr-btn--primary" href="release.html">下载正式版</a>
<a class="npr-btn npr-btn--outline" href="#modules">功能模块</a>
<a class="npr-btn npr-btn--outline" href="https://github.com/bb-yi/blender">GitHub</a>
</div>

<div class="npr-meta">
<span class="npr-chip">测试 <strong>110/110</strong></span>
<span class="npr-chip">日期 <strong>2026-08-11</strong></span>
<span class="npr-chip">平台 <strong>Win x64</strong></span>
<span class="npr-chip">引擎 <strong>Eevee</strong></span>
</div>

</div>

<hr class="npr-rule">

<div class="npr-section-head" markdown>
<div class="npr-eyebrow">Map</div>

## 先选工作路径
<p>按你在 Blender 里会打开的入口分组，而不是平铺功能名。</p>
</div>

<div class="npr-map">
<a href="scene-extensions.html">
<span class="code">01 · Scene</span>
<span class="title">Scene 管线</span>
<span class="desc">渲染纹理、Filter Graph、相机输出、描边</span>
</a>
<a href="extended-nodes.html">
<span class="code">02 · Nodes</span>
<span class="title">扩展节点</span>
<span class="desc">Filter 域、辅助信息、GLSL、SDF、材质节点</span>
</a>
<a href="npr-workflow.html">
<span class="code">03 · NPR Tree</span>
<span class="title">NPR 工作流</span>
<span class="desc">光照分解、折射、图像采样、逐灯循环</span>
</a>
<a href="interface-guide.html">
<span class="code">04 · UI</span>
<span class="title">界面控制</span>
<span class="desc">性能归因、材质状态、灯光组、阴影缩放</span>
</a>
</div>

<div id="modules" class="npr-section-head" markdown>
<div class="npr-eyebrow">Modules</div>

## 模块说明
</div>

<div class="npr-modules" markdown>

<div class="npr-module" markdown>
<div class="npr-module__body" markdown>
<span class="code">01 · Scene</span>
### Scene 管线
<p class="blurb">在场景层搭渲染纹理与后处理图，而不是把整条链路塞进单个物体材质。</p>

- **Render Textures**：场景级自定义渲染纹理
- **Filter Graph**：四阶段执行图，Filter Pass 可复用
- **Camera FX Outputs**：原生相机特效输出源
- **Eevee Outline**：场景级描边

<div class="npr-tags">
<span>Render Textures</span><span>Filter Graph</span><span>Camera FX</span><span>Outline</span>
</div>

<div class="npr-actions">
<a class="npr-btn npr-btn--outline" href="scene-extensions.html">阅读 Scene 文档</a>
</div>
</div>
<div class="npr-module__media">
<img src="images/filter_graph_effect_example.png" alt="Filter Graph 效果示例：视口与 Filter Material 节点">
<p class="cap">Filter Graph · 视口 + Filter Material</p>
</div>
</div>

<div class="npr-module" markdown>
<div class="npr-module__body" markdown>
<span class="code">02 · Nodes</span>
### 扩展着色器节点
<p class="blurb">补齐 Filter、屏幕空间、物体材质和自定义 GLSL 所需节点，从采样做到程序化造型。</p>

- **Filter 域**：Object Info、Mask、Scene Color
- **通用辅助**：Render Info、Scene Time、Screen Derivative、Portal
- **物体材质**：Outline Control、Screenspace Info、World / Probe
- **GLSL / 程序化**：Function、Script Expression、SDF、OKLab

<div class="npr-tags">
<span>Scene Color</span><span>GLSL Function</span><span>SDF</span><span>Portal</span>
</div>

<div class="npr-actions">
<a class="npr-btn npr-btn--outline" href="extended-nodes.html">阅读节点文档</a>
</div>
</div>
<div class="npr-module__media">
<img src="images/glsl_function_node_mode.png" alt="GLSL Function 节点模式面板">
<p class="cap">GLSL Function · Node / Code</p>
</div>
</div>

<div class="npr-module" markdown>
<div class="npr-module__body" markdown>
<span class="code">03 · NPR Tree</span>
### NPR Tree 工作流
<p class="blurb">在专用 NPR Tree 里做光照分解、折射、采样和逐灯处理，并附带可拖用的内置节点组。</p>

- **NPR Input**：合成 / 漫射 / 高光等光照分解
- **NPR Refraction**：折射路径（含体积近似相关修复）
- **Image Sample**：支持偏移模式的图像采样
- **For Each Light**：逐灯循环与灯光信息
- **内置资产**：常用 NPR 节点组可直接拖入

<div class="npr-tags">
<span>NPR Input</span><span>Refraction</span><span>Image Sample</span><span>Assets</span>
</div>

<div class="npr-actions">
<a class="npr-btn npr-btn--outline" href="npr-workflow.html">阅读 NPR 工作流</a>
</div>
</div>
<div class="npr-module__media">
<img src="images/SnowShot_2026-03-28_04-23-33.png" alt="NPR Tree 节点总览">
<p class="cap">NPR Tree · Input / Refraction / Assets</p>
</div>
</div>

<div class="npr-module" markdown>
<div class="npr-module__body" markdown>
<span class="code">04 · UI</span>
### 界面与生产控制
<p class="blurb">把性能归因、材质渲染状态和灯光控制放进日常面板，方便风格化项目排错与调参。</p>

- **Eevee Performance**：阴影 / 探针成本归因
- **材质状态**：预览、剔除、ZTest / Stencil / Write
- **灯光**：Lightgroup ID、太阳光 Shadow Map Scale
- **生产辅助**：启动图版本、IME、骨骼 Outliner 显示

<div class="npr-tags">
<span>Performance</span><span>Stencil</span><span>Lightgroup</span><span>Shadow Scale</span>
</div>

<div class="npr-actions">
<a class="npr-btn npr-btn--outline" href="interface-guide.html">阅读界面文档</a>
</div>
</div>
<div class="npr-module__media">
<img src="images/eevee_performance_shadow_probe.png" alt="Eevee Performance 阴影与探针归因面板">
<p class="cap">Eevee Performance · Shadow / Probe</p>
</div>
</div>

</div>

<div class="npr-section-head" markdown>
<div class="npr-eyebrow">This build</div>

## 本版变更 · fd9fabb4f531
<p>正式包重点：折射体积、GLSL 矩阵 helper、阴影细节、输入体验。</p>
</div>

<div class="npr-delta" markdown>

<div markdown>
### 新增
- EEVEE NPR 折射体积近似，并完成采样 / 捕获隔离与背景漏光修复
- `GLSL Function` 提供 draw-view 变换矩阵 helper（视图 / 投影 / 模型）
- 视口 Shadow LOD 可视化
- 改进 IME（适配 5.2 API，修正文本编辑器光标度量）
</div>

<div markdown>
### 修复
- 折射体积下 Image Sample 偏移黑洞、背景 miss、深度映射
- 太阳光 `Shadow Map Scale` 正确作用于 tilemap
- 未连接 GLSL sampler 回退为白色
- 恢复 NPR Tree AOV 输出
- 稳定 Render Texture 资源生命周期
</div>

</div>

<div class="npr-actions">
<a class="npr-btn npr-btn--outline" href="release.html">下载与校验信息</a>
<a class="npr-btn npr-btn--outline" href="https://github.com/bb-yi/blender/blob/main/blender-npr-release-changelog.md">完整 changelog</a>
</div>

!!! warning "仅支持 Eevee"
    安装包可以跑 Cycles，但本站记录的 NPR Port 扩展只在 **Eevee** 下可用。

<div class="npr-cta-band" markdown>
<div class="npr-eyebrow">Release</div>

## Windows x64 正式包

<p>完整验证基线，Release 测试 110/110，含 SHA256。</p>

<div class="npr-actions">
<a class="npr-btn npr-btn--primary" href="release.html">打开下载页</a>
<a class="npr-btn npr-btn--outline" href="https://github.com/bb-yi/blender/releases">全部 Release</a>
</div>
</div>
