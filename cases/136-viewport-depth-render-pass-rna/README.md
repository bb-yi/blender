# viewport-depth-render-pass-rna

## 测试内容

验证 3D Viewport 的 Shading Render Pass 列表包含 `Depth`，并且后台启动时可用的 `VIEW_3D` space 能把 `shading.render_pass` 设置为 `DEPTH` 后保持该值，不会被 RNA setter 回退到 `COMBINED`。

脚本还会创建一个前后错开的 Eevee 场景并进行普通后台渲染，确保这个设置不会破坏 Eevee 初始化和基础渲染。

## 通过条件

- `View3DShading.render_pass` enum 中必须有 `DEPTH`。
- 至少一个 `VIEW_3D` space 设置 `shading.type='RENDERED'`、`shading.render_pass='DEPTH'` 后读回仍为 `DEPTH`。
- 后台 Eevee 渲染完成。
- 前景和背景采样点颜色明显不同，避免空场景或未渲染结果误判通过。
