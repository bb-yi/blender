# NPR For Each Light

## Test Coverage

This case builds its NPR trees and scenes programmatically and runs them in isolated OpenGL and
Vulkan child processes. It covers:

- A black result with no lights, followed by red and green point-light contributions and a blue
  Sun contribution.
- Two sequential For Each Light zones in the same NPR tree.
- The Blender 5.1-compatible local-light-before-directional-light order using an overwrite zone.
- A 32-red-point-light plus 33rd-blue-point-light probe that crosses the first 32-bit visibility
  word and proves the second word contributes to the result.
- A shadowed point light whose Shadow Mask must distinguish an unobstructed receiver from the same
  receiver behind an occluder, exercising the Blender 5.2 shadow resource-table bridge.
- A rectangular Area Light and long blocker that produce a straight soft-shadow edge. The same
  Shadow Mask is rendered with one and 64 samples to verify stochastic sampling, temporal
  convergence, continuous penumbra levels, and the absence of fixed 1/8 grayscale clustering.
- A signed per-light direction result that is remapped by a Filter pass, proving its negative RGB
  components remain available after NPR output.
- A stronger signed direction source reflected by a rough metallic plane with Eevee screen tracing,
  proving reflection sampling sanitizes negative values before logarithmic roughness filtering.
- Eevee Color Bake, where `LIGHT_ITER_FORCE_NO_CULLING` must not access Z-bin or tile buffers.
- Shader compilation, GPU validation, device loss, native crashes, and bounded child timeouts.

The test does not depend on a checked-in `.blend` file or on private generated shader symbol names.

## Pass Criteria

- OpenGL and Vulkan children confirm their active backend after GPU initialization.
- Regular renders are exactly 32 by 32; the Area Light probe is exactly 96 by 64. Every result is
  non-empty and contains only finite pixels.
- The progressive-light probe produces the expected isolated red, red/green, and red/green/blue
  channels.
- The order probe finishes with the blue Sun color, proving local lights run before directionals.
- The local-light word-boundary probe contains both red and blue, proving the 33rd blue light is
  visited beyond the first word.
- Shadow Mask is bright without an occluder and dark with an occluder.
- The one-sample Area Light penumbra has spatial ray variation; at 64 samples its along-edge
  variance falls below 45% of the one-sample value, it contains at least 12 rounded grayscale
  levels, and fewer than 75% of its profile values lie near fixed 1/8 steps.
- The Filter remap receives signed NPR data and produces approximately `(0.305, 0.713, 0.092)`;
  clamping at NPR output would instead leave red/blue near `0.5`.
- The rough screen-space reflection is finite and contains more than 20 green-dominant pixels with
  a maximum green value above `0.08`.
- The Color Bake image is finite, evaluates a shadow-enabled point light, and finishes with the
  blue Sun color.
- Neither child emits shader compilation, GPU validation, Vulkan device-loss, access-violation,
  Blender-crash, or timeout diagnostics.

## Entry Point

`run.py`

## Source Test

`blender_npr_post/tests/python/npr/test_npr_foreach_light.py`
