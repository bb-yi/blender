# npr-test_goo_shader_info_render

## 测试内容

验证 `Shader Info` 节点各输出在实际 Eevee 渲染中的光照语义。

覆盖内容：

- `Diffuse Shading` 对直接灯光有响应，无灯和 World Sky 伪太阳下保持黑。
- `Diffuse Shading` 会按 `Shader Info.lightgroup_id` 只接收同组灯光贡献。
- `Ambient Lighting` 会读取烘焙的 Light Probe Volume 间接光，绿色世界探针产生绿色主导的非零输出，黑世界保持黑。
- `Half-Lambert Factor` 在背光法线下仍高于黑，并在球体/平面上形成平滑衰减。
- `Blinn-Phong Factor` 在球体和平面上集中到受光侧，且 `Exponent` 输入会收窄高光。
- `Shadow` 会在遮挡区域变暗。
- `Diffuse Shading`、`Half-Lambert Factor`、`Blinn-Phong Factor` 这些非 shadow 输出应忽略 blocker 阴影。

## 通过条件

- Diffuse 有灯样本比无灯样本至少亮 `0.2`，且有灯样本大于 `0.2`。
- Lightgroup 1 场景必须输出绿色主导的 Diffuse，Lightgroup 0 场景必须输出红色主导的 Diffuse。
- `Ambient Lighting` 在绿色烘焙探针下绿色通道必须大于 `0.005` 并高于红/蓝通道；黑世界探针下 RGB 必须小于 `0.05`。
- 非 shadow 输出在 blocked/unblocked 参考中的亮度差必须小于脚本容差。
- `Shadow` 的被遮挡采样必须比 lit/unblocked 参考至少暗 `0.35`。
- 无灯和 World Sky 场景下相关输出中心红通道小于 `0.05`。
- Diffuse、Half-Lambert、Blinn-Phong 在球体/平面上的采样序列必须按脚本定义的方向单调衰减。
- `Blinn-Phong Factor` 的低 exponent 样本必须比高 exponent 样本至少亮 `0.15`，证明高光宽度由节点输入控制。

## 测试入口

`run.py`

## 原始测试

`blender_5_1_port\tests\python\npr\test_goo_shader_info_render.py`
