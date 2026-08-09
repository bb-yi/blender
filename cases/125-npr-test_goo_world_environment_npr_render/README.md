# npr-test_goo_world_environment_npr_render

## 测试内容

验证 NPR Tree 中的 `World Environment` 节点能读取世界背景颜色，并覆盖基础材质输出。

场景世界背景为绿色，基础材质为红色发光。NPR Tree 中把 `World Environment Color` 连接到 `NPR Output Color`。

## 通过条件

- 中心像素绿通道大于 `0.8`。
- 中心像素红通道小于 `0.1`，说明红色基础材质被 NPR Tree 覆盖。
- 中心像素蓝通道小于 `0.1`。

## 测试入口

`run.py`

## 原始测试

`blender_5_1_port\tests\python\npr\test_goo_world_environment_npr_render.py`
