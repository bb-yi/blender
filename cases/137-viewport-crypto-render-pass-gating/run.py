import bpy


CRYPTO_PASSES = [
    ("CryptoObject", "use_pass_cryptomatte_object"),
    ("CryptoAsset", "use_pass_cryptomatte_asset"),
    ("CryptoMaterial", "use_pass_cryptomatte_material"),
]


def view3d_spaces():
    return [
        area.spaces.active
        for screen in bpy.data.screens
        for area in screen.areas
        if area.type == "VIEW_3D" and area.spaces.active is not None
    ]


def crypto_flags(view_layer):
    return {prop: bool(getattr(view_layer, prop)) for _, prop in CRYPTO_PASSES}


spaces = view3d_spaces()
assert spaces, "Factory startup should provide at least one VIEW_3D space"

enum_items = spaces[0].shading.bl_rna.properties["render_pass"].enum_items
identifiers = {item.identifier for item in enum_items if item.identifier}
for pass_name, _prop in CRYPTO_PASSES:
    assert pass_name in identifiers, f"View3DShading.render_pass is missing {pass_name}"

view_layer = bpy.context.view_layer

for _pass_name, prop in CRYPTO_PASSES:
    setattr(view_layer, prop, False)

for pass_name, _prop in CRYPTO_PASSES:
    for space in spaces:
        space.shading.type = "RENDERED"
        space.shading.render_pass = pass_name
        assert space.shading.render_pass == pass_name, (
            f"Expected viewport render pass {pass_name}, got {space.shading.render_pass}"
        )
    flags = crypto_flags(view_layer)
    assert not any(flags.values()), (
        f"Selecting viewport pass {pass_name} must not enable ViewLayer Cryptomatte flags: {flags}"
    )

for enabled_pass_name, enabled_prop in CRYPTO_PASSES:
    for _pass_name, prop in CRYPTO_PASSES:
        setattr(view_layer, prop, prop == enabled_prop)
    for space in spaces:
        space.shading.render_pass = enabled_pass_name
        assert space.shading.render_pass == enabled_pass_name

    flags = crypto_flags(view_layer)
    assert flags[enabled_prop] is True, f"Expected {enabled_prop} to stay enabled"
    for _pass_name, prop in CRYPTO_PASSES:
        if prop != enabled_prop:
            assert flags[prop] is False, (
                f"Selecting {enabled_pass_name} should not implicitly enable {prop}: {flags}"
            )

print(f"VIEWPORT_CRYPTO_RENDER_PASS_GATING_SPACES={len(spaces)}")
