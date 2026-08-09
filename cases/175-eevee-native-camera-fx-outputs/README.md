# eevee-native-camera-fx-outputs

## Test Coverage

This case runs the Native Camera FX source test in explicit OpenGL and Vulkan child processes. It
covers:

- Value-pass Film accumulation with a native Depth output.
- Color-pass Film accumulation with a native Normal output.
- Native depth of field applied to a Depth pass with focused and defocused objects.
- Native motion blur applied to an animated Emission object while a static object remains stable.
- Backend markers, Vulkan GPU debug output, native crashes, and bounded child-process timeouts.

## Pass Criteria

- Native Depth and Normal contain non-zero surface data and zero-valued backgrounds.
- Native Normal remains close to the ordinary Normal render pass.
- The DOF output remains non-black and changes the deliberately defocused object more than the
  focused object.
- The motion-blur output changes more than 10 percent of the moving-object region while static and
  background regions stay below the configured limits.
- OpenGL and Vulkan children confirm their requested backend and exit without GPU errors.

## Entry Point

`run.py`

## Source Test

`blender_npr_post/tests/python/npr/test_eevee_native_camera_fx_outputs.py`
