# npr-test_goo_light_info_render

## 测试内容

验证材质域 `Light Info` 节点的所有输出和运行时更新。

覆盖内容：

- `Color`、`Power`、`Position`、`Direction`、`Radius`、`Spot Size`、`Sun Angle` 输出能映射到渲染结果。
- `Type` 对 POINT/SUN/SPOT/AREA 输出正确枚举值，未绑定灯光时输出 0。
- 未绑定灯光的 `Color` 输出接近黑色。
- 修改灯光颜色、能量、位置、方向、缩放半径、类型后，不重新连线也能在下一次渲染中更新。

## 通过条件

- 颜色/位置/方向类输出与输入灯光数据在 `0.05` 容差内一致。
- Power 输出随能量达到脚本设定阈值，例如能量 7 时 RGB 都大于 `6.8`。
- Radius/Spot Size/Sun Angle 输出大于对应阈值。
- Type 输出按 POINT=0、SUN=1、SPOT=2、AREA=3 映射；脚本用 bias 后检查具体数值。
- 所有 live update 前后采样必须反映修改后的灯光数据。

## 测试入口

`run.py`

## 原始测试

`blender_5_1_port\tests\python\npr\test_goo_light_info_render.py`
