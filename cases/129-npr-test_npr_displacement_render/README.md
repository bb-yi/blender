# npr-test_npr_displacement_render

## 测试内容

验证带 Displacement 输入的材质仍然能正确应用 NPR Tree 输出。

材质基础 Principled 为红色，并连接 Displacement 节点；NPR Tree 输出纯绿色。测试分别把材质 `displacement_method` 设为 `BUMP` 和 `DISPLACEMENT` 后渲染。

## 通过条件

- BUMP 模式中心像素绿通道大于 `0.8`，红/蓝通道小于 `0.1`。
- DISPLACEMENT 模式中心像素绿通道大于 `0.8`，红/蓝通道小于 `0.1`。
- 这说明 displacement 路径没有让红色基础 shading 覆盖 NPR 输出。

## 测试入口

`run.py`

## 原始测试

`blender_5_1_port\tests\python\npr\test_npr_displacement_render.py`
