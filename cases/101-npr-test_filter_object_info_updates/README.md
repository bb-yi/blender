# npr-test_filter_object_info_updates

## 测试内容

验证 Filter 域的 `Filter Object Info` 节点会随目标物体变换和显示属性实时更新，并且目标更新不会重编译材质。

场景中白色平面叠加一个 Filter material。测试先在目标 `X=0` 渲染，再把目标移动到 `X=1` 后重新渲染；随后分别读取目标的 Rotation、Scale 和 Color。

## 通过条件

- 初始中心红通道小于 `0.1`。
- 目标移动后中心红通道大于 `0.4`。
- 移动前后红通道差值大于 `0.3`。
- Rotation、Scale、Color 三次输出各通道与目标值误差小于 `0.05`。
- 目标移动前后 shader compile status 不失败且 compile timestamp 不变。

## 测试入口

`run.py`

## 原始测试

`blender_npr_post\tests\python\npr\test_filter_object_info_updates.py`
