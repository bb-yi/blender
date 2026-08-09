# glsl-function-meta-labels

## 测试内容

验证 `GLSL Function` 的 `@glsl_meta label="..."` 可以把合法 GLSL 参数名显示成本地化 socket 名称。

具体覆盖：

- `vec4 color` 输入可以显示成中文 label，但 socket identifier 仍是 `In_color`。
- `sampler2D tex` 输入可以显示成中文 label，并且仍然生成 `NodeSocketClosure`。
- 未写 label 的 `uv` 输入继续显示 GLSL 参数名。
- `out vec4 out_color` 输出支持 label。
- 修改 label 后刷新节点，已有同名 socket 的用户默认值不会被 meta default 重置。
- `out` 参数只允许 `label`；写 `description` 等其他 meta 必须报错。

## 通过条件

脚本中的所有断言通过，并打印 `GLSL_FUNCTION_META_LABELS_RELEASE_OK`。

## 测试入口

`run.py`
