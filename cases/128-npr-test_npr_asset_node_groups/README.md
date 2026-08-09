# npr-test_npr_asset_node_groups

## 测试内容

验证发布包内置 NPR 节点组资产文件可用，并且迁移后的节点组能参与实际 NPR 渲染。

检查 `assets\nodes\npr_node_groups.blend`：

- 必须包含 `Cavity`、`Co-Planar Edge Detection`、`Curvature`、`Kuwahara`、`Shading Models`、`Surface Curvature` 这些 asset node groups。
- 任意 ShaderNodeTree 中不能有 `NodeUndefined`。
- `Cavity` 组必须保留 repeat input/output 节点。
- 追加 `Cavity` 组后，用它构造 NPR Tree 并渲染球体。

## 通过条件

- 资产 bundle 文件存在。
- 所有预期资产节点组存在，且无 undefined node。
- `Cavity` 组包含 `GeometryNodeRepeatInput` 和 `GeometryNodeRepeatOutput`。
- 渲染结果不是全黑，且中心像素红通道大于 `0.05`。

## 测试入口

`run.py`

## 原始测试

`blender_5_1_port\tests\python\npr\test_npr_asset_node_groups.py`
