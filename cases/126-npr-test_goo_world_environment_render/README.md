# npr-test_goo_world_environment_render

## 测试内容

验证材质域 `World Environment` 节点能采样世界背景，而不会被前景红色平面遮挡污染。

测试两种模式：

- 默认 Direction 输入，读取当前方向的绿色世界。
- 显式 Direction 输入 `(1.0, 0.5, 0.25)`，仍应读取绿色世界。

## 通过条件

- 默认 Direction 和自定义 Direction 两次渲染中，中心像素绿通道都必须大于 `0.8`。
- 两次渲染中红通道都必须小于 `0.1`。
- 两次渲染中蓝通道都必须小于 `0.1`。

## 测试入口

`run.py`

## 原始测试

`blender_5_1_port\tests\python\npr\test_goo_world_environment_render.py`
