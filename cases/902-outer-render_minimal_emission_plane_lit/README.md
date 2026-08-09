# outer-render_minimal_emission_plane_lit

## 测试内容

验证带显式灯光和黑色 World 的最小红色发光平面能在 Eevee 中渲染写出。

脚本清空场景，创建相机、Sun 光和红色 `Emission` 平面，渲染到 `temp\render_exports\minimal_emission_lit.png`。

## 通过条件

- Eevee render 调用完成。
- `temp\render_exports\minimal_emission_lit.png` 被写出。
- 输出文件大小大于 0。

## 测试入口

`run.py`

## 原始测试

`test\render_minimal_emission_plane_lit.py`
