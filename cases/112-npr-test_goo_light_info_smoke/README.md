# npr-test_goo_light_info_smoke

## 测试内容

验证 `Light Info` 节点的注册、socket 列表、`light_object` 属性和不同灯光类型下的动态 socket 启用规则。

## 通过条件

- `ShaderNodeLightInfo` 存在，label 为 `Light Info`。
- 输出 socket 为 `Color`、`Power`、`Type`、`Position`、`Direction`、`Radius`、`Spot Size`、`Sun Angle`。
- `Type` 输出类型为 `INT`，节点暴露 `light_object` 属性。
- POINT 启用 `Color/Power/Type/Position/Radius`。
- SUN 启用 `Color/Power/Type/Direction/Sun Angle`。
- SPOT 启用 `Color/Power/Type/Position/Direction/Radius/Spot Size`。
- AREA 启用 `Color/Power/Type/Position/Direction/Radius`。
- 未绑定灯光时只启用 `Color/Power/Type`。

## 测试入口

`run.py`

## 原始测试

`blender_5_1_port\tests\python\npr\test_goo_light_info_smoke.py`
