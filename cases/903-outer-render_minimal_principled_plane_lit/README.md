# outer-render_minimal_principled_plane_lit

## 测试内容

验证最小 `Principled BSDF` 红色平面在黑色 World 和 Sun 光下能通过 Eevee 后台渲染写出。

脚本清空场景，创建相机、Sun 光和红色 Principled 平面，渲染到 `temp\render_exports\minimal_principled_lit.png`。

## 通过条件

- Eevee render 调用完成。
- `temp\render_exports\minimal_principled_lit.png` 被写出。
- 输出文件大小大于 0。

## 测试入口

`run.py`

## 原始测试

`test\render_minimal_principled_plane_lit.py`
