# Interface & Settings Guide

This page covers interface settings, configuration options, and troubleshooting for Blender 5.1 NPR Port.

## Material Preview Settings

### Material Selector Preview Toggle

**Location:** Preferences > Rendering > Material Preview

**Purpose:** Control real-time preview of materials in viewport.

| Setting | Effect |
|---------|--------|
| **On** | Shows high-quality material preview in viewport |
| **Off** | Uses simplified material display |

**When to Use:**
- Enable for art-direction and material review
- Disable for viewport performance on heavy scenes

## World Environment Exclusion

### Exclude Objects from World Environment

**Location:** Scene Properties > World Environment Exclude

**Purpose:** Selectively exclude specific objects from environment lighting contributions.

**Setup:**
1. Go to `Scene Properties`
2. Find `World Environment Exclude`
3. Add objects to the exclusion list
4. These objects won't receive environment lighting

**Use Cases:**
- Prevent certain objects from receiving environment reflections
- Highlight key characters/objects with custom lighting
- Optimize lighting in complex scenes

## Lightgroup Configuration

### Lightgroup ID Settings

**Location:** Object Properties > Light Groups

**Purpose:** Assign objects to specific light groups for selective lighting.

| Property | Description |
|----------|-------------|
| **Lightgroup ID** | Numeric ID for grouping objects (0-15) |
| **Render in Group** | Which light groups affect this object |
| **Emission Group** | Light group for emissive contribution |

**Workflow:**
1. Select object
2. In Object Properties, set `Lightgroup ID`
3. Configure which lights affect this group
4. Render to see group-specific lighting

## Startup Splash Information

### Version & Feature Information

**Location:** Splash screen at Blender startup

**Shows:**
- Blender version (5.1)
- NPR Port features enabled
- Build date and commit info

**Keyboard Shortcut:** `F1` to show splash screen anytime

## Quick Reference

### Eevee vs Cycles Feature Support

| Feature | Eevee | Cycles |
|---------|-------|--------|
| Scene-level Render Textures | ✅ | ❌ |
| Filter Materials | ✅ | ❌ |
| NPR Tree | ✅ | ❌ |
| Shader Nodes (20+) | ✅ | ⚠️ Limited |
| Screen Derivative | ✅ | ❌ |
| Light Info | ✅ | ⚠️ Limited |
| Screenspace Info | ✅ | ❌ |
| World Environment | ✅ | ✅ |

**Note:** Cycles has limited support for NPR-specific features. Eevee is the primary render engine.

### Viewport Shading Modes

| Mode | Shortcut | Use |
|------|----------|-----|
| Wireframe | `Z` | View geometry |
| Material Preview | `Z` | Quick material check |
| Rendered | `Z` | Final render preview |
| Dithered | N/A | Improved quality filtering |

### Useful Shortcuts

| Action | Shortcut |
|--------|----------|
| Toggle NPR Tree Edit | `Ctrl + Tab` |
| Reload Shaders | `F8` (in Shader Editor) |
| Frame All | `Home` |
| Focus Selected | `NumPad .` |
| Show Splash | `F1` |

## Common Problems & Solutions

### "NPR Tree nodes not found"

**Problem:** NPR nodes don't appear in shader node menu.

**Solutions:**
1. Verify you're in Shader Editor (not Compositor)
2. Ensure active material has correct domain
3. Check Blender is in Object Mode

### "Scene Color not working in Filter"

**Problem:** Scene Color node returns black.

**Solutions:**
1. Verify Filter Material is in correct `Execution Stage`
2. Check scene has lights and render target
3. Ensure `Window` texture coordinate is used
4. Try adjusting `Source` option in Scene Color node

### "Render Textures not updating"

**Problem:** Render Texture looks stale or shows old frame.

**Solutions:**
1. Check `Update Mode` is set appropriately
2. Verify source camera exists and is visible
3. Try switching to `Every Frame` update
4. Check texture resolution isn't too high for VRAM

### "Filter Materials causing artifacts"

**Problem:** Filter effects show banding or other issues.

**Solutions:**
1. Use appropriate texture format (16F for standard, 32F for precision)
2. Check `Execution Stage` order
3. Verify shader network is correct
4. Try enabling dithering in render properties

### "Material colors look wrong"

**Problem:** Colors appear desaturated or incorrect.

**Solutions:**
1. Check color space (Linear vs sRGB)
2. Verify texture type (Image vs Color in properties)
3. Check Eevee color management settings
4. Ensure no conflicting filter materials

### "Editor shortcuts not working"

**Problem:** Keyboard shortcuts don't respond.

**Solutions:**
1. Verify window focus is on Editor (not Properties)
2. Check custom keymap hasn't reassigned shortcut
3. Try Edit > Preferences > Keymap to reset
4. Ensure NumPad is enabled (if NumPad shortcut)

## Performance Optimization

### Tips for Better Performance

- **Reduce texture resolution:** Lower Render Texture resolution if VRAM is limited
- **Batch filter effects:** Combine multiple filters in one material instead of separate
- **Use light groups:** Limit For Each Light nodes to relevant lights only
- **Disable previews:** Turn off Material Preview toggle for viewport performance
- **Simplify NPR Tree:** Use pre-built node groups instead of complex custom networks

### Profiling & Debugging

1. **Shader Compilation Time:** Monitor in Shader Editor (F8 to recompile)
2. **Render Time:** Check Frame Render Time in Render Properties
3. **Memory Usage:** Monitor texture and buffer allocations
4. **Viewport Performance:** Adjust matcap quality and sample counts

## Advanced Configuration

### Environment Variables

Some behavior can be controlled via environment variables ($BLENDER_NPR_* prefixes).

### Debug Output

Enable debug shaders for development:
1. Edit > Preferences > System
2. Enable logging options
3. Check system console for shader compilation info

## Support & Resources

- **Official Documentation:** https://github.com/bb-yi/blender
- **Issues & Bug Reports:** GitHub Issues page
- **Community:** Blender forums and Discord

---

**Next Steps:** Choose a feature above to dive deeper, or visit the Scene Extensions or Workflow pages for more details.
