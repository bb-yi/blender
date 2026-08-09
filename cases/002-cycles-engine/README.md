# cycles-engine

## 测试内容

验证当前安装树是带 Cycles 的发布构建，而不是日常的 Eevee-only 构建。

## 通过条件

- `bpy.context.scene.render.engine = 'CYCLES'` 能成功执行。
- 赋值后 `scene.render.engine` 读回值必须仍然是 `CYCLES`。
- 脚本打印 `CYCLES_OK=True`。

## 测试入口

`run.py`
