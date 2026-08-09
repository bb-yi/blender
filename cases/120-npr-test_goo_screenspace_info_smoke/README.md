# npr-test_goo_screenspace_info_smoke

## 测试内容

验证 `Screenspace Info` 节点的注册和 socket 形状。

## 通过条件

- `ShaderNodeScreenspaceInfo` 存在，label 为 `Screenspace Info`。
- 输入 socket 为 `View Position`。
- 输出 socket 为 `Scene Color`、`Scene Depth`。

## 测试入口

`run.py`

## 原始测试

`blender_5_1_port\tests\python\npr\test_goo_screenspace_info_smoke.py`
