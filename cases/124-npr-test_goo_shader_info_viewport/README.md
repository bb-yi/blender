# npr-test_goo_shader_info_viewport

## 测试内容

验证 `Shader Info` 的视口 OpenGL 渲染路径与普通渲染路径一致，重点检查非 shadow 光照输出不会被 blocker 阴影错误压暗。

脚本在 View3D 上下文中分别对 `Diffuse Shading`、`Half-Lambert Factor`、`Blinn-Phong Factor` 做 viewport render，比较有 blocker 和无 blocker 的同一世界坐标采样。

## 通过条件

- 三个输出在无 blocker 场景下都必须有可见亮度。
- 三个输出在 blocker 场景下也必须保持可见亮度。
- blocked 和 unblocked 采样差值必须小于 viewport 容差 `0.06`。
- 脚本会写入 viewport 测试状态；任何异常都会让测试失败并退出 Blender。

## 测试入口

`run.py`

## 原始测试

`blender_5_1_port\tests\python\npr\test_goo_shader_info_viewport.py`
