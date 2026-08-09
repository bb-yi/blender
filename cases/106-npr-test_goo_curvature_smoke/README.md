# npr-test_goo_curvature_smoke

## 测试内容

验证 Curvature 节点已注册，并且节点 UI socket 结构符合发布预期。

## 通过条件

- `bpy.types.ShaderNodeCurvature` 存在。
- 节点 label 为 `Curvature`。
- 输入 socket 名称依次为 `Samples`、`Sample Radius`、`Thickness`、`Scale`。
- 输出 socket 名称依次为 `Scene Curvature`、`Scene Rim`。

## 测试入口

`run.py`

## 原始测试

`blender_5_1_port\tests\python\npr\test_goo_curvature_smoke.py`
