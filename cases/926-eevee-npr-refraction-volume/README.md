# Eevee NPR Refraction Volume

## 测试内容

创建一个 Eevee 场景：红色发光背板前放置一个 Mesh Volume Absorption 立方体，并在前方依次放置 0、1 两个折射层。场景同时保留 World Volume 节点，折射层的 NPR Tree 读取 `NPR Refraction` 的 `Combined Color`。

测试分别渲染：

- 只有背板和体积；
- 一层 NPR Refraction 和体积；
- 两层 NPR Refraction 和体积；
- 一层 NPR Refraction 但关闭体积。
- 开启透明 Film、隐藏背板，以 World Volume Scatter 验证没有背后几何的 background miss；
- 相同 background miss 场景关闭体积。
- 相同无体积 background miss 场景增加正面点光源，验证折射层不会泄漏自身的 Deferred 灯光。
- 无体积时分别渲染无遮挡背景和一层 NPR Refraction，验证结果不变；
- 使用 8 个 TAA render samples，对比关闭和开启 Active Render Texture 的主视图结果；Capture
  Camera 观察独立的 Deferred 几何和 World Volume，不得覆盖主视图的 Mesh Volume 结果或历史；
- 使用强彩色 World Volume Absorption，使部分透射通道接近零，检查整帧无 NaN、无无穷大且亮度不溢出。

## 通过条件

- 有体积时，单层和双层折射的中心像素应与没有折射层的体积背景接近，证明 front→back 体积分段没有漏算或重复合成；
- 关闭体积后中心像素必须发生可测变化；
- 没有背后几何时，折射层结果应与无遮挡的 World Volume Scatter 接近，且 alpha 必须非零；
- 无体积、透明 World 时，无论折射层自身是否被灯光照亮，直连 `NPR Refraction` 的输出都必须保持黑色全透明；
- 无体积且背后有几何时，一层 NPR Refraction 必须与无遮挡背景一致；
- 8-sample TAA 下，启用 Active Render Texture 前后的主视图中心像素必须在容差内一致；
- 强彩色吸收必须保持整帧有限，最大 RGB 不得超过稳定性阈值；
- 脚本必须打印 `EEVEE_NPR_REFRACTION_VOLUME_OK`；
- 渲染结果必须是有限值，不能出现 NaN 或无穷大。
