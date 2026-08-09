# viewport-crypto-render-pass-gating

## 测试内容

验证 3D Viewport 的 `CryptoObject`、`CryptoAsset`、`CryptoMaterial` render pass 预览项只是视口选择，不会隐式打开 ViewLayer 的 Cryptomatte pass。

这对应最近的性能修复：用户没有在 ViewLayer Passes 中开启 Crypto 通道时，后台不应该因为 viewport 菜单里可选这些项就继续计算 CryptoObject / Asset / Material。

## 通过条件

- `View3DShading.render_pass` enum 中必须包含 `CryptoObject`、`CryptoAsset`、`CryptoMaterial`。
- 三个 `use_pass_cryptomatte_*` 开关都为 `False` 时，选择任意 Crypto 预览项后三个开关仍保持 `False`。
- 逐个开启 Object / Asset / Material 后，选择对应预览项不会隐式打开另外两个 Crypto pass。
