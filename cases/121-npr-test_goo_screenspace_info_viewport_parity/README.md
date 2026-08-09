# npr-test_goo_screenspace_info_viewport_parity

## Test Coverage

This case verifies that Screen Space Info reads the previous Eevee layer without corrupting GPU
resource bindings. It covers:

- `BLENDED` and `DITHERED` materials.
- Default and explicit View Position inputs.
- Scene Color through Emission and directly connected to Material Output Surface.
- Dithered Scene Color and Scene Depth with scene raytracing and material raytraced transmission.
- Explicit OpenGL and Vulkan background renders with backend markers.
- Normal-window Vulkan Rendered viewport sessions with raytracing disabled and enabled.
- Strict material assignment: one material slot, the intended active material, and every polygon on
  slot zero.

## Pass Criteria

- Scene Color samples the red object behind the foreground material.
- Scene Depth is non-black for the object behind the foreground material.
- OpenGL and Vulkan child processes confirm their requested backend and exit successfully.
- Both Vulkan Rendered viewport sessions survive 40 redraw ticks and exit through the completion
  marker.
- No child log contains a missing GPU bind, validation failure, `VK_ERROR_DEVICE_LOST`, native
  access violation, crash, or timeout.

## Entry Point

`run.py`

## Source Test

`blender_npr_post/tests/python/npr/test_goo_screenspace_info_viewport_parity.py`
