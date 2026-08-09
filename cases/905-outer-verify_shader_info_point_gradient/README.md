# outer-verify_shader_info_point_gradient

## 测试内容

验证本地 `Shader Info` 的 `Diffuse Shading` 点光源梯度回归场景。

脚本创建一个 8x8 平面，材质把 `Shader Info` 的 `Diffuse Shading` 输出接入发光颜色；点光源位于负 X 方向上方。脚本沿 X 轴从 `-3` 到 `3` 采样红通道，检查光照从靠近灯光侧向远离灯光侧递减。

## 通过条件

- 采样序列中最大红通道大于 `0.2`，说明点光源产生可见 Diffuse Shading。
- 从 `X=-3` 到 `X=3` 的相邻红通道值必须严格递减，说明局部点光源梯度方向正确。

## 测试入口

`run.py`

## 原始测试

`test\verify_shader_info_point_gradient.py`
