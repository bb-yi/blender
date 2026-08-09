# npr-test_world_npr_tree_render

## 测试内容

验证 World Output 的 NPR Tree 支持，包括直接颜色、NPR Input 和 Image Sample 采样路径。

可直接查看的测试工程位于 `assets/` 子目录，每个子场景一个 `scene.blend`；预览图输出到 `out/asset-previews/`。

覆盖内容：

- World NPR Tree 输出纯红，应覆盖绿色普通 World 背景。
- `NPR Input Combined Color` 应读取绿色普通 World 背景。
- World NPR 的 Diffuse 与 Specular 颜色类输入应和 Combined Color 一样可读取普通 World 背景，而不是保持默认黑。
- `Image Sample` 采样 `NPR Input Normal` 和 `Position` 时应得到蓝通道为主的向量结果。
- 对方向相关的 world background，`Image Sample Combined Color` 加 offset 后应采到不同方向的颜色。

## 通过条件

- Solid World NPR 中心像素红通道大于 `0.8`，绿/蓝小于 `0.1`。
- Combined Color 中心像素绿通道大于 `0.8`，红/蓝小于 `0.1`。
- Diffuse 与 Specular 颜色类输入中心像素绿通道大于 `0.8`，红/蓝小于 `0.1`。
- Normal 和 Position 的 Image Sample 中心像素蓝通道大于 `0.8`，红/绿绝对值小于 `0.1`。
- Combined Color 的 offset 采样与中心采样红通道差值必须大于 `0.05`。

## 测试入口

`run.py`

## 原始测试

`blender_5_1_port\tests\python\npr\test_world_npr_tree_render.py`
