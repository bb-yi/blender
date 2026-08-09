import bpy


view_layer_eevee = bpy.context.view_layer.eevee
property_name = "use_pass_outline"

assert property_name in view_layer_eevee.bl_rna.properties, (
    f"ViewLayerEEVEE is missing {property_name}: "
    f"{sorted(view_layer_eevee.bl_rna.properties.keys())}"
)

initial_value = view_layer_eevee.use_pass_outline
try:
    for expected in (True, False):
        view_layer_eevee.use_pass_outline = expected
        assert view_layer_eevee.use_pass_outline is expected, (
            f"Expected {property_name}={expected}, got {view_layer_eevee.use_pass_outline}"
        )
finally:
    view_layer_eevee.use_pass_outline = initial_value

print("EEVEE_OUTLINE_RENDER_PASS_RNA_OK=1")
