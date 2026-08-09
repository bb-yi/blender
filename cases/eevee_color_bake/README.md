# eevee-color-bake

## Test Coverage

Validates that Eevee `object.bake(type='EMIT')` runs through the GPU/DRW Color Bake path. The generic Bake API owns image texture targets, active color attribute targets, UV coverage, margin and mesh color writeback; Eevee owns GPU rasterization and local GPUMaterial shading.

The case covers Emission, Principled Emission, BSDF local lighting, world ambient, repeated shadow-map behavior, `Shader to RGB`, multi-material/multi-image targets, active color attribute targets, Image Texture, Checker/UV/Mapping/ColorRamp, Node Group, GLSL Function, local NPR Tree color, Shader Info light response, Light Shader node point-dependent color, tangent-space Normal Map lighting, a fixed copied `.blend` reference scene under `assets/`, and unsupported screen-space/non-Emit/missing-target cases.

## Pass Criteria

- Emission-style materials bake the expected scene-linear color.
- BSDF materials are near black without lights, brighten with point lights, and respond to world ambient.
- Repeated Point and Sun shadow caster scenarios produce darker shadowed pixels deterministically, while disabled shadows do not darken the receiver.
- The copied Shader Info shadow reference scene renders and bakes back close to its stored reference image, including self-shadow and another object's projected shadow.
- `Shader to RGB` and ordinary shader outputs use the local closure-to-color path.
- Texture, procedural, UV/Mapping, Node Group and GLSL Function nodes evaluate through GPU material compilation, not a CPU node whitelist.
- Active corner and point color attributes, including byte-color corner attributes and multiple selected objects, can be used as bake targets and receive the baked material result.
- Face-domain active color attributes are rejected clearly as unsupported bake targets instead of silently succeeding without a writeback.
- Shader Info responds to light energy changes.
- Light Shader Info/Output nodes on a light datablock can use point-dependent `Light Space` and affect baked receiver pixels.
- Tangent-space Normal Map uses UV tangent attributes during bake lighting.
- Local NPR Tree color can override the surface color and bake correctly.
- Non-`EMIT` bake type, `NPR Input`, Scene Color, Screen Space Info, Render Texture, NPR Refraction, Input/Output AOV, and Filter-domain output fail clearly instead of silently writing an invalid image.
- Missing image texture bake targets cancel with a warning report instead of an informational-only message.

## Entry Point

`run.py`
