# npr-test_goo_screen_derivative_smoke

## 测试内容

验证 `Screen Derivative` 节点的注册、枚举项和按 data type 启用 socket 的行为。

## 通过条件

- `ShaderNodeScreenDerivative` 存在，label 为 `Screen Derivative`。
- 默认输入和输出 socket 均为三个 `Value`。
- 默认 operation 为 `DDX`。
- operation 枚举恰好包含 `DDX`、`DDY`、`DDXY`。
- 设置 operation 为 `DDXY` 后读回仍为 `DDXY`。
- 设置 `data_type = "FLOAT"` 后，仅有一个输入 `Value` 和一个输出 `Value` 处于 enabled 状态。

## 测试入口

`run.py`

## 原始测试

`blender_5_1_port\tests\python\npr\test_goo_screen_derivative_smoke.py`
