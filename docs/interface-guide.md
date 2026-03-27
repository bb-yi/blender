# Interface & Settings | 界面与设置

## Quick Settings Reference

=== "English"

    | Setting | Location | Purpose |
    |---------|----------|---------|
    | **Material Preview** | Preferences > Rendering | Real-time material display |
    | **Lightgroup ID** | Object Props > Light Groups | Group objects for selective lighting |
    | **World Exclude** | Scene Props > World Environment Exclude | Exclude objects from environment |
    | **Render Textures** | Scene Props > Render Textures | Scene-level texture rendering |
    | **Filter Materials** | Scene Props > Filter Materials | Global filter effects |

=== "中文"

    | 设置 | 位置 | 用途 |
    |------|------|------|
    | **材质预览** | Preferences > Rendering | 实时材质显示 |
    | **Lightgroup ID** | Object Props > Light Groups | 为灯光分组对象 |
    | **World Exclude** | Scene Props > World Environment Exclude | 排除环境对象 |
    | **Render Textures** | Scene Props > Render Textures | 场景级纹理渲染 |
    | **Filter Materials** | Scene Props > Filter Materials | 全局滤镜效果 |

---

## Viewport Shortcuts | 视口快捷键

=== "English"

    | Action | Shortcut | Purpose |
    |--------|----------|---------|
    | Material Preview | `Z` | Toggle preview mode |
    | NPR Tree Edit | `Ctrl + Tab` | Switch to NPR editor |
    | Rendered View | `Z` | Final render preview |
    | Frame All | `Home` | Center view on all |
    | Focus Selected | `NumPad .` | Center on selection |

=== "中文"

    | 操作 | 快捷键 | 用途 |
    |------|--------|------|
    | 材质预览 | `Z` | 切换预览模式 |
    | NPR Tree 编辑 | `Ctrl + Tab` | 切换到 NPR 编辑器 |
    | 渲染视图 | `Z` | 最终渲染预览 |
    | 显示全部 | `Home` | 居中显示所有 |
    | 聚焦选中 | `NumPad .` | 居中显示选中项 |

---

## Common Issues & Solutions

### Issue: Scene Color Returns Black

=== "English"

    **Problem:** Scene Color node in Filter shows no color
    
    **Solutions:**
    1. Check Filter Material is in correct Execution Stage
    2. Verify scene has lights and visible geometry
    3. Ensure Window texture coordinates are connected
    4. Try switching Scene Color Source option
    5. Check render happens before filter evaluation

=== "中文"

    **问题：** Filter 中的 Scene Color 节点返回黑色
    
    **解决方案：**
    1. 检查 Filter Material 是否在正确的执行阶段
    2. 验证场景有灯光和可见几何体
    3. 确保 Window 纹理坐标已连接
    4. 尝试切换 Scene Color Source 选项
    5. 检查渲染是否在滤镜评估之前进行

---

### Issue: Render Texture Not Updating

=== "English"

    **Problem:** Render Texture shows stale or old data
    
    **Solutions:**
    1. Change Update Mode to "Every Frame"
    2. Verify source camera exists and is enabled
    3. Check texture resolution doesn't exceed VRAM
    4. Ensure render goes to texture before material samples it
    5. Try reducing resolution if memory-constrained

=== "中文"

    **问题：** Render Texture 显示过时或旧数据
    
    **解决方案：**
    1. 将更新模式改为 "Every Frame"
    2. 验证源相机存在且已启用
    3. 检查纹理分辨率是否超过显存
    4. 确保渲染在材质采样前进行
    5. 如果内存受限，尝试降低分辨率

---

## Feature Support Matrix | 功能支持矩阵

=== "English"

    | Feature | Eevee | Cycles |
    |---------|-------|--------|
    | Scene Render Textures | ✅ | ❌ |
    | Filter Materials | ✅ | ❌ |
    | NPR Tree | ✅ | ❌ |
    | Screen Derivative | ✅ | ❌ |
    | Light Info | ✅ | ⚠️ Limited |
    | Shader Nodes (20+) | ✅ | ⚠️ Limited |
    | World Environment | ✅ | ✅ |
    | Lightgroups | ✅ | ✅ |

=== "中文"

    | 功能 | Eevee | Cycles |
    |------|-------|--------|
    | 场景渲染纹理 | ✅ | ❌ |
    | 滤镜材质 | ✅ | ❌ |
    | NPR Tree | ✅ | ❌ |
    | 屏幕导数 | ✅ | ❌ |
    | 灯光信息 | ✅ | ⚠️ 有限 |
    | 着色器节点 (20+) | ✅ | ⚠️ 有限 |
    | 世界环境 | ✅ | ✅ |
    | 灯光组 | ✅ | ✅ |

---

## Best Practices | 最佳实践

=== "English"

    1. **Always use Eevee** for NPR Port features
    2. **Test in viewport** with Material Preview before final render
    3. **Organize with Lightgroups** for complex scenes
    4. **Monitor VRAM** when using multiple Render Textures
    5. **Stack Filters Efficiently** - combine effects when possible
    6. **Profile Performance** - use Shader Editor timing info

=== "中文"

    1. **始终使用 Eevee** 用于 NPR Port 功能
    2. **在最终渲染前** 用材质预览在视口中测试
    3. **使用灯光组** 组织复杂场景
    4. **监控显存** 使用多个渲染纹理时
    5. **高效堆叠滤镜** - 尽可能合并效果
    6. **性能分析** - 使用着色器编辑器计时信息

!!! warning
    NPR Port is optimized for **Eevee only**. Cycles support is limited.
