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
