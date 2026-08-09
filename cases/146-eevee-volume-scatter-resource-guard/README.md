# eevee-volume-scatter-resource-guard

## 测试内容

创建一个只有 World Volume Absorption 的最小场景，不创建体积散射节点和任何灯光，然后使用 Eevee 后台渲染。这条路径应使用不带体积灯光资源的 `VOLUME_SCATTER` shader 变体。

## 通过条件

- Blender 后台渲染正常退出，没有 Vulkan 资源绑定崩溃。
- Render Result 尺寸为 16x16。
- 输出 PNG 文件存在。
- 脚本打印 `EEVEE_VOLUME_SCATTER_RESOURCE_GUARD_OK`。

## 测试入口

`run.py`
