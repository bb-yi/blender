# npr-test_goo_light_probe_color_smoke

## 测试内容

验证 `Light Probe Color` 节点的注册和 socket 形状。

## 通过条件

- `ShaderNodeLightProbeColor` 存在，label 为 `Light Probe Color`。
- 输入 socket 为 `Direction` 和 `Roughness`；`Direction` 类型为 `VECTOR` 并隐藏默认值，`Roughness` 类型为 `VALUE` / `NodeSocketFloatFactor`，默认值 `0.0`。
- 输出 socket 为 `Reflection`、`Irradiance`、`Combined`，三者类型均为 `RGBA`。

## 测试入口

`run.py`

## 原始测试

`blender_5_1_port\tests\python\npr\test_goo_light_probe_color_smoke.py`
