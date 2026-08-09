# Node Projects

本 case 当前可直接查看的 `Image to Closure` / `sampler2D` 测试工程位于：

- `020-image-to-closure-sampler2d/scene.blend`

该工程用于展示：

- `Image to Closure` 把 1x1 纹理作为 closure 暴露给 `sampler2D`。
- `GLSL Function` 通过 `texture()` 读取该纹理并在 Filter 材质中输出颜色。
