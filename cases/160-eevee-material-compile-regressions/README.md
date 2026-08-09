# eevee-material-compile-regressions

## Test Coverage

This case protects the Blender 5.2 NPR material compiler fixes for:

- False sampler overflow in deferred Eevee materials when NPR/filter fixed sampler slots sit above the backend texture count.
- Missing `hiz_tx` binding for soft-filtered Shader Info shadow evaluation.
- Missing `hiz_tx` binding for Curvature nodes, including the non-local path that does not set the raycast material flag.
- Mesh volume material compilation after the BSL migration, including stale legacy `drw_resource_id_raw()` helper exposure.
- World To Tangent node compilation in the BSL material path, where legacy `OBINFO_LIB` gating used to hide `node_world_to_tangent()`.
- Follow-on generated GLSL failures such as missing `node_tree` or `samp*` resources.
- Duplicate or missing GPU resource bindings in Vulkan material create-info layouts.

The test creates a small scene procedurally and renders it once with the default backend and once
with the Vulkan backend plus GPU validation. It does not depend on the external MMD repro file.

## Pass Criteria

- The child Blender process exits successfully and reaches the render completion marker.
- The captured child Blender log contains no `uses too many samplers` message.
- The captured child Blender log contains no missing `hiz_tx`, `node_tree`, or `samp*` variable diagnostics.
- The captured child Blender log contains no missing BSL draw-resource helpers such as `drw_resource_id_raw`, `object_infos_get`, or `drw_object_infos`.
- The captured child Blender log contains no missing `node_world_to_tangent` diagnostics.
- The Vulkan child confirms that its active backend is Vulkan.
- The Vulkan log contains no overlapping or missing GPU resource binding diagnostics.
- The scene renders through Eevee with image-textured Shader Info, Curvature, mesh volume, and World To Tangent materials on both tested backends.

## Entry Point

`run.py`
