# npr-test_goo_environment_exclusion_render

## 测试内容

验证 World 的 environment exclusion collection 会在 Eevee 中排除指定物体的环境光照。

场景中有两个白色漫反射球，绿色 World 背景提供环境光。右侧球被移动到 `world.environment_exclusion_collection` 中，左侧球保持正常受环境光影响。

## 通过条件

- 左侧未排除球的绿色通道大于 `0.12`。
- 右侧被排除球的绿色通道小于 `0.03`。
- 左右绿色通道差值大于 `0.08`。

## 测试入口

`run.py`

## 原始测试

`blender_5_1_port\tests\python\npr\test_goo_environment_exclusion_render.py`
