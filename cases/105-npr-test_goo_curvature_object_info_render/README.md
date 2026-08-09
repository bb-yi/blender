# npr-test_goo_curvature_object_info_render

## 测试内容

验证 Curvature 节点和 Object Info 节点可以在同一个 Eevee 材质中共同参与渲染。

场景创建一个 Suzanne，物体颜色设为红色，材质用 `Object Info Color` 驱动发光颜色，用 `Curvature Scene Curvature` 增强发光强度。

## 通过条件

- 中心采样不能全黑，RGB 最大值大于 `0.05`。
- 中心采样红通道必须同时大于绿、蓝通道，说明 Object Info 的红色物体 tint 生效。

## 测试入口

`run.py`

## 原始测试

`blender_5_1_port\tests\python\npr\test_goo_curvature_object_info_render.py`
