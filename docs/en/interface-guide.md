# Interface & Settings

## Quick Settings Reference

| Setting | Location | Purpose |
|---------|----------|----------|
| **Material Preview** | Preferences > Rendering | Real-time material display |
| **Lightgroup ID** | Object Props > Light Groups | Group objects by light |
| **World Exclude** | Scene Props > World Environment Exclude | Exclude environment objects |
| **Render Textures** | Scene Props > Render Textures | Scene-level texture rendering |
| **Filter Materials** | Scene Props > Filter Materials | Global filter effects |

---

## Viewport Shortcuts

| Action | Shortcut | Purpose |
|--------|----------|----------|
| Material Preview | `Z` | Switch preview mode |
| NPR Tree Edit | `Ctrl + Tab` | Switch to NPR editor |
| Render View | `Z` | Final render preview |
| Frame All | `Home` | Center display all |
| Focus Selected | `NumPad .` | Center display selected |

---

## Common Issues & Solutions

### Issue: Scene Color Returns Black

**Problem:** Scene Color node in Filter returns black

**Solution:**
1. Check if Filter Material is in the correct execution stage
2. Verify the scene has lights and visible geometry
3. Ensure Window texture coordinates are connected
4. Try switching Scene Color Source option
5. Check if rendering occurs before filter evaluation

---

### Issue: Render Texture Not Updating

**Problem:** Render Texture shows outdated or old data

**Solution:**
1. Change update mode to "Every Frame"
2. Verify source camera exists and is enabled
3. Check if texture resolution exceeds VRAM
4. Ensure rendering occurs before material sampling
5. If memory-constrained, try lowering resolution

---

## Feature Support Matrix

| Feature | Eevee | Cycles |
|---------|-------|--------|
| Scene Render Textures | ✅ | ❌ |
| Filter Materials | ✅ | ❌ |
| NPR Tree | ✅ | ❌ |
| Screen Derivative | ✅ | ❌ |
| Light Info | ✅ | ⚠️ Limited |
| Shader Nodes (20+) | ✅ | ⚠️ Limited |
| World Environment | ✅ | ✅ |
| Light Groups | ✅ | ✅ |

---

## Best Practices

1. **Always use Eevee** for NPR Port features
2. **Test in viewport** with material preview before final render
3. **Use light groups** to organize complex scenes
4. **Monitor VRAM** when using multiple render textures
5. **Stack filters efficiently** - combine effects where possible
6. **Profile performance** - use shader editor timing information

!!! warning
NPR Port is optimized for **Eevee only**. Cycles support is limited.
