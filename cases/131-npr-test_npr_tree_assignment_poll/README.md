# npr-test_npr_tree_assignment_poll

## 测试内容

验证 Material Output 和 World Output 的 `nprtree` 属性只接受真正的 NPR Tree。

测试分别对材质输出和世界输出执行同一组赋值：

- 尝试赋值普通 ShaderNodeTree，里面只有 RGB 节点。
- 再赋值包含 `ShaderNodeNPR_Output` 的 ShaderNodeTree。

## 通过条件

- 普通 ShaderNodeTree 不能被保留下来，`output.nprtree` 必须仍为 `None`。
- 包含 `ShaderNodeNPR_Output` 的树必须能成功赋值并读回同一个对象。
- 材质输出和世界输出都必须满足上述规则。

## 测试入口

`run.py`

## 原始测试

`blender_5_1_port\tests\python\npr\test_npr_tree_assignment_poll.py`
