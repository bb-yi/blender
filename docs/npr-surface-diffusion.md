# NPR 表面扩散

在 EEVEE 对象材质的 **添加 → NPR** 中添加 **NPR 表面扩散**。
**原理化 NPR** 也移入这个独立分类，节点标识与既有工程连线不变。

## 接法

`已有 Shader → NPR 表面扩散 → 材质输出 Surface`

也可以直接输入二分颜色、颜色渐变或 GLSL Function 的颜色输出，无需先加漫射节点。
颜色在这里表示已经完成着色的表面颜色，不会再乘一遍灯光，也不作为体积 GI 的发光源。
显式 Emission 节点仍保留其 GI 发光语义。

它扩散输入 Shader 的着色结果，不是开启一个自带漫反射的额外 BSDF。
输入分支内的高光也会扩散。如果要保留锐利高光，把高光分支留在节点外，最后用 Add/Mix 合成。
Alpha、Holdout 和物体覆盖率不随颜色模糊。原生次表面节点及原理化的 SSS 仍保留各自的作用。

## 参数

| 输入 | 作用 |
| --- | --- |
| Shader | 要扩散的着色器分支，也接受颜色 |
| Strength | 原结果与扩散结果的混合强度；0 是旁路 |
| Radius | 红、绿、蓝三个通道的相对扩散距离；默认 `(1,1,1)` 不引入色偏，零通道不扩散 |
| Scale | 世界单位下的整体距离；默认 `0.005`，0 是旁路 |

采用与 EEVEE SSS 同源的 Burley 屏幕空间核，按深度和物体身份拒绝不相关的邻域，
不把其他物体或透明背景的颜色直接混入。原始 Closure 保留材质、间接反射和折射数据；
扩散作用在完成光照的颜色上，而不是重新用白色漫射材质计算灯光。

## 当前边界

- EEVEE **Dithered** 表面生效；Blended、探针捕获不执行这项屏幕空间处理，保留未扩散输入。
- 可以使用上游 Shader to RGB 的颜色；扩散节点外并列的 Shader to RGB 分支互不影响。反向接法（扩散后再 Shader to RGB）无法读取后续屏幕扩散结果。
- 多个各自内部含 Shader to RGB 的扩散节点并列时，求值顺序可能互相干扰，不作保证。
- 嵌套节点不是连续多次模糊；不要依靠嵌套构建多级卷积。
- 这是屏幕空间表面处理，不是体积传输，不能从离屏/背面取得散射颜色。
- 多 Closure 沿用 EEVEE 的有限槽位与随机抽样；复杂同类 BSDF 混合需要足够渲染样本。
- 不承诺 Cycles、MaterialX 或其他渲染器支持。Metal、复杂折射、动态视口和性能尚需单独验收。

## 回归

源码测试：`tests/python/npr/test_surface_diffusion.py`。
工作区 Release case：`test/release/cases/931-npr-surface-diffusion/`，
对指定源码/安装树分别运行 OpenGL、Vulkan 像素断言。
覆盖颜色边界、HDR、零值、RGB 距离、外部 Mix/Add、原理化 NPR、普通 BSDF、金属反射、
Shader to RGB、透明覆盖率、Forward 旁路与保存重开。
这些针对性验证不等于完整 Release 套件或所有材质组合已验收。
