# npr-test_npr_group_render

## 测试内容

验证 NPR Tree 中嵌套 `ShaderNodeGroup` 的输出能传递到 `ShaderNodeNPR_Output` 并影响最终渲染。

测试创建一个内部节点组，输出颜色 `(0.25, 0.7, 0.35)`，再把该组放入 NPR Tree 并连接到 NPR Output。

## 通过条件

- 中心像素绿通道大于 `0.4`，说明节点组输出确实进入了 NPR Tree 的最终颜色。

## 测试入口

`run.py`

## 原始测试

`blender_5_1_port\tests\python\npr\test_npr_group_render.py`
