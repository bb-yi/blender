# Scene-Level Eevee Extensions | Scene 级 Eevee 扩展

## Render Textures

=== "English"

    ### Description
    
    `Render Textures` is a scene-level Eevee additional render texture system.
    
    It allows a scene to maintain up to `4` Render Texture slots, where each slot can specify a camera and output type. The scene is rendered from that camera perspective into a texture, which can then be sampled in object materials using the `Render Texture` node.
    
    ### Panel Location
    
    `Scene Properties > Render Textures`
    
    ### Configurable Parameters
    
    Each Render Texture entry supports:
    
    | Parameter | Description |
    |-----------|-------------|
    | `Name` | Identifier name for referencing in nodes |
    | `Enabled` | Enable/disable rendering for this texture |
    | `Source` | Capture type: Color / Depth / Normal |
    | `Camera` | Specify camera for rendering |
    | `Resolution X/Y` | Output texture dimensions |
    | `Update Mode` | Every Sample / Frame / Manual |
    | `Format` | RGBA16F / RGBA32F / R16F / R32F |
    
    ### Basic Usage
    
    1. Open `Scene Properties > Render Textures`
    2. Create a new entry
    3. Select Source, Camera, resolution, and format
    4. In object material, add `Render Texture` node
    5. Select the entry from the node panel
    6. Use outputs in material calculations
    
    !!! tip
        Perfect for real-time reflections, screen-space effects, and dynamic textures.

=== "中文"

    ### 功能说明
    
    `Render Textures` 是场景级的 Eevee 额外渲染纹理系统。
    
    它允许场景预先维护最多 `4` 个 Render Texture 槽位，每个槽位都可以指定一个相机和一个输出类型，把该相机视角下的场景结果先渲染成纹理，再在普通物体材质中通过 `Render Texture` 节点采样。
    
    ### 面板入口
    
    `Scene Properties > Render Textures`
    
    ### 可配置内容
    
    每个 Render Texture 条目当前支持：
    
    | 参数 | 说明 |
    |------|------|
    | `Name` | 纹理条目的标识名称，用于在节点中引用 |
    | `Enabled` | 启用/禁用该纹理的渲染 |
    | `Source` | 选择要捕获的内容：Color / Depth / Normal |
    | `Camera` | 指定用于渲染该纹理的摄像机 |
    | `Resolution X/Y` | 输出纹理的像素尺寸 |
    | `Update Mode` | 更新频率：Every Sample / Every Frame / Manual |
    | `Format` | 输出精度：RGBA16F / RGBA32F / R16F / R32F |
    
    ### 基本使用方法
    
    1. 打开 `Scene Properties > Render Textures`
    2. 新建一个条目
    3. 选择 Source、Camera、分辨率和格式
    4. 在物体材质中添加 `Render Texture` 节点
    5. 在节点面板中选择对应的条目
    6. 使用节点输出参与材质计算
    
    !!! tip "提示"
        非常适合用于制作实时反射、屏幕空间效果和动态纹理。

---

## Filter Materials

=== "English"

    ### Description
    
    A scene-level Eevee full-screen filter stack. Each entry is a `Filter` domain material, applied in order to process each frame.
    
    ### Panel Location
    
    `Scene Properties > Filter Materials`
    
    Node Editor: Shader Editor > Shader Type > Filter
    
    ### Basic Usage
    
    1. Open `Scene Properties > Filter Materials`
    2. Create a new entry
    3. Material must be `Filter` domain
    4. Open Shader Editor, switch to `Filter` shader type
    5. Use `Scene Color` to read scene data
    6. Output with `Filter Output` node
    7. Choose `Execution Stage` for order
    
    ### Execution Stages
    
    - `Before Volume Fog` - Before volumetric effects
    - `Before Depth of Field` - Before depth of field
    - `Before Composite` - Before final composition
    
    !!! warning
        Filter Materials can only use Filter domain materials.
    
    !!! example "Use Cases"
        - Color grading and tone mapping
        - Screen-space effects (edge detection, toon rendering)
        - Motion blur and motion vectors
        - Post-process denoising and sharpening

=== "中文"

    ### 功能说明
    
    它是一套场景级的 Eevee 全屏滤镜栈。每个条目都是一个 `Filter` 域材质，按列表顺序依次对当前帧进行处理。
    
    ### 面板入口
    
    `Scene Properties > Filter Materials`
    
    节点树入口: 着色器节点编辑器 > 着色器类型 > Filter
    
    ### 基本使用方法
    
    1. 打开 `Scene Properties > Filter Materials`
    2. 新建一个条目
    3. 选中的材质必须是 `Filter` 域材质
    4. 打开 Shader Editor，把顶部 `Shader Type` 切换到 `Filter`
    5. 使用 `Scene Color` 读取场景数据
    6. 用 `Filter Output` 输出结果
    7. 通过 `Execution Stage` 选择滤镜执行位置
    
    ### 执行阶段
    
    - `Before Volume Fog` - 在体积雾处理前执行
    - `Before Depth of Field` - 在景深前执行
    - `Before Composite` - 在合成器前执行
    
    !!! warning "注意"
        Filter Materials 只能使用 Filter 域材质。
    
    !!! example "使用场景"
        - 色彩分级和色调映射
        - 屏幕空间效果（如边界检测、卡通渲染）
        - 动态模糊和运动矢量效果
        - 后期降噪和锐化处理
