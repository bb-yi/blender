# cycles-bake-image-smoke

## 测试内容

验证 release 安装树中的 Cycles 能完成一次真实贴图烘焙，而不仅仅是 `scene.render.engine = 'CYCLES'` 可设置。

脚本会创建一个带 UV 的平面、红色 `Principled BSDF` 材质和活动 `Image Texture` 节点，然后执行 `DIFFUSE` 的 color-only bake。

## 通过条件

- `CYCLES` 渲染引擎可用。
- `bpy.ops.object.bake(type='DIFFUSE', pass_filter={'COLOR'})` 完成。
- bake 目标图像中有效 alpha 像素数量大于图片的一半。
- 有效像素的红通道均值明显高于绿/蓝通道，证明烘焙写入了红色材质颜色。
