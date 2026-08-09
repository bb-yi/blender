# outer-render_minimal_emission_plane

## 测试内容

验证最小发光材质平面可以在发布安装树的 Eevee 后台渲染中正常写出。

脚本创建一个红色 `ShaderNodeEmission` 平面，材质直接连接到 Material Output Surface，渲染到 `temp\render_exports\minimal_emission.png`。

## 通过条件

- Eevee render 调用完成。
- `temp\render_exports\minimal_emission.png` 被写出。
- 输出文件大小大于 0。

## 测试入口

`run.py`

## 原始测试

`test\render_minimal_emission_plane.py`
