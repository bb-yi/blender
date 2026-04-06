# 界面与工作流补充

## 1. 材质选择器预览开关

### 作用

控制材质下拉列表 / 搜索列表中是否渲染材质预览图。

这个开关主要用于在材质很多时，减少展开材质选择器时生成预览图的卡顿。

### 入口

`Edit > Preferences > Editing > Objects > Materials > Material Selector Previews`

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-28-34.png" alt="alt text" style="border-radius: 10px;">
	<br>
</div>

### 行为说明

- 开启时：材质选择器会按当前逻辑显示材质预览图。

- 关闭时：材质选择器会退回普通材质图标，不再在下拉列表里触发材质预览渲染。

- 默认值为开启。

### 当前范围

- 目前只影响 `template_ID(...)` 这类材质选择器下拉列表中的材质预览显示。

- 不影响 `Material Properties` 面板中的大预览球。

- 不影响材质本身的正常渲染结果。

## 2. 世界环境排除

### 作用

允许选择一个集合，使该集合中的对象不受世界环境影响。

### 入口

`World Properties > Environment Lighting > Exclude Collection`

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-29-43.png" alt="alt text" style="border-radius: 10px;">
	<br>
</div>

### 行为说明

- 被选中的集合会从世界环境照明中排除。

- 适合在同一场景里分离角色、前景道具和背景环境的环境光影响范围。

## 3. 骨骼在 Outliner 隐藏

### 作用

为每个 `Pose Bone` 增加单独的 Outliner 可见性标记，用来在不影响骨骼本身功能的前提下整理复杂绑定的层级显示。

它适合把机制骨、辅助骨或不需要频繁查看的控制层从 Outliner 中隐藏，只保留更重要的骨架结构。

### 入口

- `Bone Properties > Viewport Display > Hide in Outliner`
- `Outliner > Filter > Hidden PoseBones`

### 行为说明

- 每个 `Pose Bone` 都有自己的 `Hide in Outliner` 开关。

- 这个开关默认是开启的。

- `Outliner` 里的 `Hidden PoseBones` 过滤项默认也是开启的，所以默认不会立刻改变现有骨架的显示结果。

- 当关闭 `Outliner > Filter > Hidden PoseBones` 后，勾选了 `Hide in Outliner` 的姿态骨骼会从 Outliner 树中隐藏。

- 如果某个被隐藏的父骨骼仍然有可见子骨骼，可见子骨骼会继续保留在树里，不会整支层级一起消失。

### 当前范围

- 当前只作用于 `Pose Bone`

- 不作用于 `Edit Bone`

- 只改变 `Outliner` 的层级显示，不影响骨骼的变换、动画、驱动器和渲染结果

## 4. Eevee Performance

### 作用

在 `Outliner` 中查看 Eevee 当前视口 / 最终渲染的性能统计、阶段拆分和功能提示，用来快速定位性能热点。

它适合检查 `NPR Tree`、`GLSL Function`、滤镜材质、体积、景深等路径大概把时间花在了哪里。

### 入口

- `Outliner > Display Mode > Eevee Performance`
- `Outliner` 头部中的 `Profiler` / `Pause` / `Sort by Time`
- `Outliner` 头部中的设置弹出面板 `Eevee Performance`

### 行为说明

- 开启 `Profiler` 后，Eevee 会开始收集当前性能统计并在 `Outliner` 树中显示。

- `Pause` 会暂停视口性能数据的继续刷新，方便查看当前结果。

- `Sort by Time` 会按当前 CPU 开销排序阶段列表，而不是固定的管线顺序。

- `Average Window` 用于设置平滑统计时使用的帧窗口大小。

- 当前树结构会显示 `Viewport`、`Final Render`、`Metadata`、`Features`、`Stages`、`Hints` 等分组。

### 当前范围

- 当前主要是 Eevee 的 CPU 侧阶段统计与功能提示，不是完整 GPU profiler

- 只对 `Eevee` 有意义，不支持 `Cycles`

- 更适合作为“快速定位哪一段更重”的检查工具，不是逐微秒精确分析器
