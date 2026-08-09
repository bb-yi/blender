# Node Projects

本目录存放 `135-npr-test_new_shader_nodes_render` 的可打开测试工程。

每个子目录都包含一个 `scene.blend`，对应一个可直接渲染的节点效果场景。预览图统一输出到 `../out/asset-previews/`。

## Asset Index

| Folder | Nodes | Rendered Effect |
|---|---|---|
| `010-twirl` | `Twirl` | 左右对比基础坐标场与扭曲坐标场 |
| `020-water-ripples` | `Water Ripples` | 四宫格展示 `DROPS / RIPPLES / FLOW / CAUSTIC` 模式 |
| `030-hex-grid-texture` | `Hex Grid Texture` | 左侧显示 value 格子，右侧显示 color 单元 ID 变化 |
| `040-sdf-primitive-operator` | `SDF Primitive` + `SDF Operator` | 左右对比原始圆形 mask 与膨胀后的圆形 mask |
| `050-sdf-vector-operator` | `SDF Vector Operator` | 显示 `UV_GRID` 输出的单元坐标场 |
| `060-basis-transform` | `Basis Transform` | 左右对比 `TO_BASIS` 与 `FROM_BASIS` 的通道映射 |
| `070-scene-time` | `Scene Time` | 在固定帧输出时间驱动的颜色值 |
| `080-world-to-tangent` | `World To Tangent` | 绿色平面展示世界空间向量转切线空间后的结果 |
| `090-portal-in-out` | `Portal In` + `Portal Out` | 显示同名 Portal 的颜色转发结果 |
| `100-aov-input-output` | `AOV Output` + `AOV Input` + `Filter Output` | 通过 Filter Material 读回 AOV 颜色 |
| `110-render-texture` | `Render Texture` | 主相机显示由第二相机捕获的红色平面 |
| `120-outline-control` | `Outline Control` | 左右对比零线宽与可见描边 |
| `130-foreach-light` | `For Each Light Input` + `For Each Light Output` | 红灯与绿灯累加后的黄光结果 |
