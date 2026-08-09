# npr-subsurface-compile-smoke

## 测试内容

验证带 NPR Tree 的 Subsurface/SSS 材质能走 Eevee deferred NPR shader 路径完成编译和渲染。

这个测试针对之前 release 控制台报错：`npr_sss_input_impl` 未定义。脚本会创建一个启用 `Subsurface Weight` 的 `Principled BSDF` 球体，并在 `Material Output` 上挂接一个读取 `NPR Input Combined Color` 的 NPR Tree。

## 通过条件

- Eevee 后台渲染完成，没有 shader compile exception。
- 球体中心像素有明显亮度，不是纯黑。
- 背景角落保持接近黑色，证明亮度来自 SSS/NPR 材质对象而不是全屏污染。
