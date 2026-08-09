# EEVEE Material Stencil Reader Mask

## 测试内容

这个用例验证 EEVEE 材质模板测试的 writer/reader 语义。场景中有一个较小的平面材质写入 stencil reference 1，一个较大的红色物体材质使用 stencil EQUAL 读取 reference 1。正确结果是红色物体只在平面投影范围内显示。

## 通过条件

- baseline 中红色 reader 必须可见，但红色像素数必须明显少于关闭 reader stencil 后的全量红色像素数。
- 将 reader 的 stencil test 改成 NEVER 时，红色 reader 必须不可见。
- 关闭 writer 的 stencil 写入时，红色 reader 必须不可见。
- 关闭 reader 的 stencil 测试时，红色 reader 必须完整显示。
