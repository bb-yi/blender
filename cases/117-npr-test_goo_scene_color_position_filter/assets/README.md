# Node Projects

本 case 的可查看测试工程位于子目录 `010-scene-color-position-filter/scene.blend`。

该工程展示：

- 普通发光平面作为输入画面。
- `Scene Color` 读取 `Position` 源。
- Filter Material 根据世界空间 `X` 坐标做阈值分割，负半边保持黑色，正半边输出白色。
