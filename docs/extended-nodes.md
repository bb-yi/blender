# Extended Shader Nodes | 扩展着色器节点

This page documents 20+ new shader nodes added to Blender 5.1 NPR Port.

## Render Info Node

=== "English"

    **Purpose:** Access rendered image properties
    
    **Outputs:**
    - `Frag Coord` - Fragment/pixel coordinates (X, Y, Z, W)
    - `Width` - Render target width in pixels
    - `Height` - Render target height in pixels
    
    **Usage:** Use in calculations requiring screen resolution or pixel position.

=== "中文"

    **用途：** 访问渲染后的图像属性
    
    **输出：**
    - `Frag Coord` - 片段/像素坐标 (X, Y, Z, W)
    - `Width` - 渲染目标宽度（像素）
    - `Height` - 渲染目标高度（像素）
    
    **用法：** 在需要屏幕分辨率或像素位置的计算中使用。

---

## Scene Time Node

=== "English"

    **Purpose:** Access time-based values for animations
    
    **Outputs:**
    - `Frame` - Current frame number
    - `Seconds` - Elapsed time in seconds
    - `Timeline` - Timeline position
    - `Scaled Frame` - Frame with custom scale
    
    **Usage:** Create time-based animations and effects.

=== "中文"

    **用途：** 访问基于时间的值用于动画
    
    **输出：**
    - `Frame` - 当前帧数
    - `Seconds` - 经过的时间（秒）
    - `Timeline` - 时间线位置
    - `Scaled Frame` - 自定义缩放的帧数
    
    **用法：** 创建基于时间的动画和效果。

---

## Screen Derivative Node

=== "English"

    **Purpose:** Detect edges and gradients in screen space
    
    **Inputs:**
    - `Value` - Input to differentiate
    
    **Outputs:**
    - `DDX` - Derivative in X direction
    - `DDY` - Derivative in Y direction
    - `DDXY` - Both derivatives combined
    
    **Usage:** Edge detection, gradient mapping, post-processing effects.

=== "中文"

    **用途：** 检测屏幕空间中的边缘和梯度
    
    **输入：**
    - `Value` - 要求导的输入
    
    **输出：**
    - `DDX` - X 方向导数
    - `DDY` - Y 方向导数
    - `DDXY` - 两个导数结合
    
    **用法：** 边缘检测、梯度映射、后期处理效果。

---

## Light Info Node

=== "English"

    **Purpose:** Access per-light information
    
    **Inputs:**
    - `Light Type` - Filter: All / Sun / Point / Spot
    
    **Outputs:**
    - `Direction` - Light direction
    - `Distance` - Distance to light
    - `Color` - Light color
    - `Energy` - Light intensity
    
    **Usage:** Per-light calculations, conditional lighting effects.

=== "中文"

    **用途：** 访问逐光源信息
    
    **输入：**
    - `Light Type` - 筛选：All / Sun / Point / Spot
    
    **输出：**
    - `Direction` - 光方向
    - `Distance` - 到光源的距离
    - `Color` - 光颜色
    - `Energy` - 光强度
    
    **用法：** 逐光源计算、条件照明效果。

---

## Scene Color Node

=== "English"

    **Purpose:** Read scene data in filter materials
    
    **Inputs:**
    - `UV`/`Coordinate` - Sampling coordinates (default: screen space)
    
    **Outputs:**
    - `Color` - Scene color at coordinates
    - `Depth` - Scene depth value
    - `Normal` - Scene normal data
    
    **Parameters:**
    - `Source` - Color / Depth / Normal / Emission / Environment
    
    **Usage:** Post-processing effects, reflections, depth-based effects.

=== "中文"

    **用途：** 在滤镜材质中读取场景数据
    
    **输入：**
    - `UV`/`Coordinate` - 采样坐标（默认：屏幕空间）
    
    **输出：**
    - `Color` - 该坐标处的场景颜色
    - `Depth` - 场景深度值
    - `Normal` - 场景法线数据
    
    **参数：**
    - `Source` - Color / Depth / Normal / Emission / Environment
    
    **用法：** 后期处理效果、反射、基于深度的效果。

---

## Additional Nodes

=== "English"

    **Other Important Nodes:**
    
    - **Portal In/Out** - Portal utility nodes
    - **Screenspace Info** - View-space information
    - **World Environment** - Access environment
    - **World To Tangent** - Coordinate transformation
    - **Bevel** - Edge beveling with parameters
    - **Curvature** - Output surface curvature
    - **Shader Info** - Shader parameters access
    - **Render Texture** - Sample pre-rendered textures

=== "中文"

    **其他重要节点：**
    
    - **Portal In/Out** - 门户实用节点
    - **Screenspace Info** - 视图空间信息
    - **World Environment** - 访问环境
    - **World To Tangent** - 坐标变换
    - **Bevel** - 边缘斜面处理
    - **Curvature** - 输出表面曲率
    - **Shader Info** - 着色器参数访问
    - **Render Texture** - 采样预渲染纹理

!!! tip
    For detailed parameters and usage, refer to Blender's official shader node documentation combined with NPR Port extensions.
