# startup-basic

## 测试内容

验证发布包里的 Blender 能在 `--background --factory-startup` 下完成最小启动流程，并且 Python 运行环境可以访问当前场景。

## 通过条件

- `bpy.context.scene` 存在，说明 factory startup 创建了可用的活动场景。
- 脚本打印 `STARTUP_OK=1`。

## 测试入口

`run.py`
