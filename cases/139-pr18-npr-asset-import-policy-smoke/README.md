# pr18-npr-asset-import-policy-smoke

## 测试内容

验证 PR #18 修复依赖的资产导入前提：

- release 安装树中的内置 NPR node group bundle 存在。
- 内置 NPR 组资产仍在固定 NPR catalog 中。
- 对内置 NPR 组走 reuse-local-id 导入时不会生成 `.001` 重复组。
- 普通外部节点组使用默认 append 时仍会生成独立副本，避免导入策略被全局改成复用。

## 通过条件

- `npr_node_groups.blend` 能从当前 `bpy.app.binary_path` 对应安装树找到。
- `Cavity`、`Curvature`、`Kuwahara` 等核心组有预期 NPR catalog id。
- `Cavity` 用 `do_reuse_local_id=True` append 两次后，本地只有一个 `Cavity`，没有 `Cavity.001`。
- 临时创建的普通外部节点组默认 append 两次后，本地出现 `ExternalPlainGroup` 和 `ExternalPlainGroup.001`。
