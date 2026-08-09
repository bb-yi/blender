# outer-verify_npr_displacement_render

## 测试内容

验证本地 NPR displacement 回归场景：带 Displacement 输出的红色 Principled 材质仍由 NPR Tree 的绿色输出接管最终颜色。

脚本创建一个带 Displacement 节点的平面材质，NPR Tree 输出纯绿色，然后分别在 `BUMP` 和 `DISPLACEMENT` 模式下渲染中心像素。

## 通过条件

- BUMP 模式中心像素绿通道大于 `0.8`，红/蓝通道小于 `0.1`。
- DISPLACEMENT 模式中心像素绿通道大于 `0.8`，红/蓝通道小于 `0.1`。
- 这两个模式都不能退回到红色 Principled 基础材质。

## 测试入口

`run.py`

## 原始测试

`test\verify_npr_displacement_render.py`
