# npr-test_filter_object_mask_render

## 测试内容

验证同一 Filter material 中 `Filter Object Mask` 的 Cryptomatte UBO/index 路径与 `Filter Object Info` 的 referenced-object SSBO 路径可以共存，并检查 Cryptomatte Object pass 开关和目标移动。

场景中目标立方体初始位于画面中心，`Rotation.X=0.6`。Filter material 把目标 mask 乘以 `Filter Object Info.Rotation.X` 后写入红通道；随后移动目标并再次渲染。

## 通过条件

- Cryptomatte Object pass 关闭时中心和角落红通道均小于 `0.1`。
- 初始中心像素红通道位于 `0.5` 到 `0.7`。
- 初始角落像素红通道小于 `0.1`。
- 目标移走后中心像素红通道小于 `0.1`。

## 测试入口

`run.py`

## 原始测试

`blender_npr_post\tests\python\npr\test_filter_object_mask_render.py`
