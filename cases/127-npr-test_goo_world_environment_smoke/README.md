# npr-test_goo_world_environment_smoke

## 测试内容

验证 `World Environment` 节点的注册和 socket 形状。

## 通过条件

- `ShaderNodeWorldEnvironment` 存在，label 为 `World Environment`。
- 输入 socket 为 `Direction`，类型为 `VECTOR`，并且隐藏默认值。
- 输出 socket 为 `Color`，类型为 `RGBA`。

## 测试入口

`run.py`

## 原始测试

`blender_5_1_port\tests\python\npr\test_goo_world_environment_smoke.py`
