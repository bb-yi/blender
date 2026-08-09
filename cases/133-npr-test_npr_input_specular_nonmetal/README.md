# npr-test_npr_input_specular_nonmetal

## 测试内容

验证 NPR Input 的 `Combined Color`、Diffuse、Specular、`Position`、`Normal` 通道能正确读取非金属 Principled 材质的 GBuffer 与光照信息。

测试打开 `assets/010-specular-nonmetal/scene.blend`，使用文件中的 Suzanne 猴头和它的 `NPR Tree`。脚本会把猴头材质 `Metallic` 强制设为 `0`，先用白色 NPR 输出渲染一张 Suzanne 对象 mask，再分别把 `NPR Input` 的各个关键 socket 连接到 `NPR Output` 的颜色输入并进行 Eevee 后台渲染。

这个测试覆盖的具体问题是：普通非金属 Principled 会把 diffuse 和 specular 写入不同的 GBuffer layer，NPR pass 必须能直接读到所有 GBuffer layer 与 radiance buffer；不能只让 `Combined Color` 可用而让 Diffuse、Specular、Normal 等通道保持纯黑。

## 通过条件

- Suzanne 对象 mask 至少覆盖 512 个像素，确保统计区域来自猴头而不是背景。
- `Combined Color`、`Diffuse Color`、`Diffuse Direct`、`Position`、`Normal` 在 mask 内最大亮度必须大于 `0.05`，亮度大于 `0.02` 的像素至少达到脚本阈值。
- `Diffuse Indirect` 使用白色 World 提供间接光，在 mask 内最大亮度必须大于 `0.05`，亮度大于 `0.02` 的像素至少 32 个。
- `Specular Color` 在 mask 内最大亮度必须大于 `0.05`，亮度大于 `0.02` 的像素至少 32 个。
- `Specular Direct` 在 mask 内最大亮度必须大于 `0.05`，亮度大于 `0.02` 的像素至少 8 个。
- `Specular Indirect` 使用白色 World 提供间接光，在 mask 内最大亮度必须大于 `0.05`，亮度大于 `0.02` 的像素至少 32 个。

## 测试入口

`run.py`

## 原始测试

`blender_5_1_port\tests\python\npr\test_npr_input_specular_nonmetal.py`
