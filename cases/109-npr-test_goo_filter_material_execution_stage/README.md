# Filter Graph Execution Stage

## 测试内容

验证 Eevee Filter Graph 的 execution stage 能正确保存和执行。测试先把一个反相 Filter 依次放到 `BEFORE_VOLUME_FOG`、`BEFORE_DEPTH_OF_FIELD` 和 `BEFORE_COMPOSITE`，再让 `BEFORE_VOLUME_FOG`、`BEFORE_POSTFX`、`BEFORE_DEPTH_OF_FIELD` 和 `BEFORE_COMPOSITE` 四个阶段分别执行不同系数的乘法 Filter。

Release case 分别使用 OpenGL 和 Vulkan 子进程运行，并启用 `--debug-gpu` 检查 GPU 资源反馈回路。

## 通过条件

- 三个单独阶段的中心像素都是蓝色反相后的黄色。
- 四个阶段同时执行后，蓝通道必须接近 `0.9 * 0.8 * 0.7 * 0.6 = 0.3024`；漏执行任意阶段都会超出容差。
- 子进程明确报告 OpenGL 或 Vulkan 后端并完成渲染。
- 两个后端的调试输出都不包含 framebuffer/texture feedback-loop 错误。

## 测试入口

`run.py`

## 原始测试

`blender_npr_post/tests/python/npr/test_goo_filter_material_execution_stage.py`
