# eevee_viewport_transparent_jittered_shadow_accumulates

## Test content

This case builds a small Eevee viewport scene with a static semi-transparent
hashed material. The material has transparent shadows enabled and the scene has
viewport jittered shadows enabled. A child Blender process opens a rendered
viewport, captures an early frame, waits for temporal accumulation, and captures
a later frame.

## Pass condition

The later frame must keep the red transparent object visible and must have
substantially lower high-frequency noise than the early frame. This proves the
transparent jittered shadow path keeps updating shadow pages without forcing
the viewport film history to be discarded every sample.
