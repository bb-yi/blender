# Node Projects

本 case 的可查看测试工程按子场景拆分在 `assets/` 下：

- `010-world-solid-npr/scene.blend`
  - `NPR Output` 直接输出纯红，覆盖普通 World 背景。
- `020-world-combined-color/scene.blend`
  - `NPR Input Combined Color` 直接读取普通 World 背景颜色。
- `030-world-image-sample-normal/scene.blend`
  - `Image Sample` 采样 `NPR Input Normal`。
- `040-world-image-sample-position/scene.blend`
  - `Image Sample` 采样 `NPR Input Position`。
- `050-world-image-sample-combined-offset/scene.blend`
  - `Image Sample` 对 `Combined Color` 做 offset 采样，展示方向变化。
