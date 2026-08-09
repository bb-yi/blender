# GLSL Function Typed Closure Callback

## Test Content

This case launches isolated factory-startup Blender children for OpenGL and
Vulkan with GPU debugging and material shader-source capture enabled. Each child
builds three Eevee emission materials on one probe plane, saves the scene, reopens
it, validates the restored graphs, and performs connected and fallback renders.

The scalar material calls an annotated `float remap(...)` helper twice with
different arguments. Its `closure.remap` zone uses two instances of the same
Shader Node Group with different `Gain` inputs plus reroutes to calculate
`Result` and `mask`. This verifies that each localized group instance keeps its
own creation context. Disconnecting the callback must restore the original
helper body:

```glsl
mask = gain;
return value * gain;
```

The typed material calls annotated `vec4 typed_transport(...)` twice. Its
callback transports a two-dimensional Vector, a Color, and a vec4 split into XYZ
Vector plus `.__w` Float sockets. The outer zone contains a second GLSL Function
whose annotated `vec3 inner_blend(...)` callback is implemented by one shared
Vector Math Multiply Add node with a generated Image Texture dependency. The
same nested result feeds outer `Result` and
`out_packed`, which exercises localized nested callbacks and shared multi-output
serialization.

The Int/Bool material calls an annotated helper with the exact float-transport
integer boundary `16777216` and a Boolean input/output. Its connected Closure
passes both values through, while the original helper subtracts one and negates
the Boolean. This makes the connected and fallback renders unambiguous and
verifies both input and output conversion directions.

After the first connected typed render, the case inspects the captured GLSL with
brace-balanced function extraction. It verifies that the exported probe retains
two typed helper call sites, while each rewritten callback helper invokes its
generated multi-output `ntree_fnN` exactly once. It also verifies that the outer
sub-function invokes the nested GLSL wrapper once, outputs 0 and 4 use the same
serialized value, and the nested Multiply Add and Image Texture sample are each
emitted once.

## Expected Linear RGB

| State | Expected RGB |
|---|---|
| Scalar connected | `(0.300, 0.650, 0.225)` |
| Scalar muted fallback | `(0.020, 0.066, 0.500)` |
| Scalar fallback | `(0.020, 0.066, 0.500)` |
| Scalar reconnected | `(0.300, 0.650, 0.225)` |
| Typed connected | `(0.910, 0.518, 0.448)` |
| Typed nested callback fallback | `(0.358, 0.1876, 0.1184)` |
| Typed outer callback fallback | `(0.350, 0.305375, 0.0635625)` |
| Typed reconnected | `(0.910, 0.518, 0.448)` |
| Int/Bool connected | `(1.0, 1.0, 0.0)` |
| Int/Bool fallback | `(0.0, 0.0, 0.0)` |

Every channel must be within `0.01` of its expected value.

## Pass Criteria

- OpenGL and Vulkan children confirm their requested active GPU backend.
- All three GLSL Function nodes parse as `READY` and expose the stable callback
  identifiers `closure.remap`, `closure.typed_transport`, and
  `closure.inner_blend` with their annotated labels and descriptions.
- Saving and reopening preserves every callback link, Closure socket identifier,
  Int/Bool and vec2/Color/vec4 split socket type and key, minimal Meta labels/subtypes, internal graph
  link, paired Closure zone, and user-opened `Closures` panel state.
- The scalar graph retains two instances of one shared Shader Node Group with
  distinct `Gain` inputs and at least two reroutes. The typed graph retains one
  shared nested Multiply Add, its Image Texture dependency, and the shared links
  from its result to outer `Result` and `out_packed`.
- Every render matches the explicit RGB table. Connected and fallback results
  differ materially, and reconnecting restores the connected result.
- A muted callback link renders the same fallback value as a disconnected link;
  it must not make material compilation fail.
- Both backend logs must report zero GPU memory leaks; any `GPUNodeLink` leak is
  a failure.
- The connected shader source contains two typed helper call sites; exactly one
  `ntree_fnN` call in each rewritten callback helper; exactly one definition for
  both referenced sub-functions; one nested GLSL wrapper call; identical right
  sides for outer `out0` and `out4`; and one
  `vector_math_multiply_add(...)` call in the nested sub-function.
- Neither child log contains shader compilation failures, GPU validation errors,
  Vulkan device loss, access violations, or Blender crash diagnostics.

## Outputs

Each backend writes its round-trip blend, OpenEXR renders, summary JSON, isolated
`Shaders/` dump, and preserved `codegen_connected.glsl` under `out/<backend>/`.
Complete child logs are written as `out/opengl.log` and `out/vulkan.log`.

## Test Entry

`run.py`
