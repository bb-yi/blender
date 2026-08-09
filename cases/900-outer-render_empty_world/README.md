# outer-render_empty_world

## 测试内容

验证发布安装树的 Eevee 后台渲染能在空世界/空场景条件下完成一次 PNG 写出。

脚本清理除相机外的对象，确保有一个相机，设置 `BLENDER_EEVEE`，渲染到 `temp\render_exports\empty_world.png`。

## 通过条件

- Eevee render 调用完成。
- `temp\render_exports\empty_world.png` 被写出。
- 输出文件大小大于 0。

## 测试入口

`run.py`

## 原始测试

`test\render_empty_world.py`
