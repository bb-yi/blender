from pathlib import Path

import bpy


OKLAB_NODE = "ShaderNodeOKLabColorRamp"
COLOR_RAMP_NODE = "ShaderNodeValToRGB"


def assert_true(condition, message):
    if not condition:
        raise AssertionError(message)


def color_mode_identifiers(color_ramp):
    prop = color_ramp.bl_rna.properties["color_mode"]
    return {item.identifier for item in prop.enum_items}


def assert_oklab_storage(color_ramp, label):
    assert_true(color_ramp.color_mode == "OKLAB", f"{label}: OKLab storage is {color_ramp.color_mode}")
    try:
        color_ramp.color_mode = "RGB"
    except TypeError:
        pass
    else:
        raise AssertionError(f"{label}: OKLab storage accepted regular RGB mode")
    assert_true(color_ramp.color_mode == "OKLAB", f"{label}: OKLab storage changed mode")


def add_oklab_node(node_tree, label):
    node = node_tree.nodes.new(OKLAB_NODE)
    node.label = label
    assert_true(node.bl_idname == OKLAB_NODE, f"{label}: wrong node idname {node.bl_idname}")
    assert_true("Color" in node.outputs, f"{label}: missing Color output")
    assert_true("Alpha" in node.outputs, f"{label}: missing Alpha output")
    assert_true(hasattr(node, "color_ramp"), f"{label}: missing color_ramp storage")

    ramp = node.color_ramp
    assert_oklab_storage(ramp, label)
    assert_true(len(ramp.elements) >= 2, f"{label}: expected default color stops")
    ramp.elements[0].position = 0.0
    ramp.elements[0].color = (1.0, 0.1, 0.0, 1.0)
    ramp.elements[1].position = 1.0
    ramp.elements[1].color = (0.0, 0.35, 1.0, 1.0)
    ramp.interpolation = "EASE"
    return node


def assert_regular_color_ramp_has_no_oklab(node_tree):
    node = node_tree.nodes.new(COLOR_RAMP_NODE)
    identifiers = color_mode_identifiers(node.color_ramp)
    assert_true("OKLAB" not in identifiers, "regular Color Ramp exposes OKLAB color_mode")
    assert_true({"RGB", "HSV", "HSL"}.issubset(identifiers), "regular Color Ramp lost RGB/HSV/HSL")
    return node


def build_shader_tree():
    material = bpy.data.materials.new("oklab_release_material")
    material.use_fake_user = True
    material.use_nodes = True
    tree = material.node_tree
    assert_regular_color_ramp_has_no_oklab(tree)
    oklab = add_oklab_node(tree, "Shader OKLab")

    bsdf = tree.nodes.get("Principled BSDF")
    assert_true(bsdf is not None, "material is missing Principled BSDF")
    tree.links.new(oklab.outputs["Color"], bsdf.inputs["Base Color"])
    return material


def build_geometry_tree():
    tree = bpy.data.node_groups.new("oklab_release_geometry", "GeometryNodeTree")
    tree.use_fake_user = True
    add_oklab_node(tree, "Geometry OKLab")
    return tree


def build_compositor_tree():
    scene = bpy.context.scene
    scene.use_nodes = True
    tree = bpy.data.node_groups.new("oklab_release_compositor", "CompositorNodeTree")
    tree.use_fake_user = True
    scene.compositing_node_group = tree
    add_oklab_node(tree, "Compositor OKLab")
    return tree


def build_light_tree():
    bpy.context.scene.render.engine = "BLENDER_EEVEE"
    light = bpy.data.lights.new("oklab_release_light", "POINT")
    light.use_fake_user = True
    light.use_nodes = True
    tree = light.node_tree
    assert_true(tree is not None, "light node tree was not created")
    oklab = add_oklab_node(tree, "Light OKLab")
    output = next((node for node in tree.nodes if node.bl_idname == "ShaderNodeEeveeLightShaderOutput"), None)
    assert_true(output is not None, "light node tree is missing Eevee Light Shader Output")
    tree.links.new(oklab.outputs["Color"], output.inputs["Color"])

    from bl_ui.node_add_menu_shader import light_eevee_shader_node_type_supported

    assert_true(
        light_eevee_shader_node_type_supported(OKLAB_NODE),
        "Eevee Light Shader Python allow-list does not include OKLab Color Ramp",
    )
    return light


def render_shader_material(material, light):
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 8
    scene.render.resolution_y = 8
    scene.eevee.taa_render_samples = 1

    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()
    bpy.ops.mesh.primitive_cube_add(size=2.0, location=(0.0, 0.0, 0.0))
    cube = bpy.context.object
    cube.data.materials.append(material)

    light_object = bpy.data.objects.new("oklab_release_light_object", light)
    light_object.location = (2.0, -3.0, 4.0)
    scene.collection.objects.link(light_object)

    bpy.ops.object.camera_add(location=(0.0, -5.0, 2.5), rotation=(1.1, 0.0, 0.0))
    scene.camera = bpy.context.object
    bpy.ops.render.render(write_still=False)


def save_and_reopen_roundtrip(case_dir):
    out_dir = case_dir / "out"
    out_dir.mkdir(exist_ok=True)
    blend_path = out_dir / "oklab_color_ramp_roundtrip.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(blend_path))
    bpy.ops.wm.open_mainfile(filepath=str(blend_path))

    found = []
    for tree in bpy.data.node_groups:
        found.extend(node.bl_idname for node in tree.nodes if node.bl_idname == OKLAB_NODE)
    for material in bpy.data.materials:
        if material.node_tree:
            found.extend(node.bl_idname for node in material.node_tree.nodes if node.bl_idname == OKLAB_NODE)
    for scene in bpy.data.scenes:
        tree = getattr(scene, "compositing_node_group", None)
        if tree:
            found.extend(node.bl_idname for node in tree.nodes if node.bl_idname == OKLAB_NODE)
    for light in bpy.data.lights:
        if light.node_tree:
            found.extend(node.bl_idname for node in light.node_tree.nodes if node.bl_idname == OKLAB_NODE)

    assert_true(len(found) >= 4, f"expected at least 4 OKLab nodes after reload, found {len(found)}")


def verify_legacy_valtorgb_oklab_migration(case_dir):
    legacy_path = case_dir / "assets" / "legacy_valtorgb_oklab_501_49.blend"
    assert_true(legacy_path.exists(), f"missing legacy migration asset: {legacy_path}")
    bpy.ops.wm.open_mainfile(filepath=str(legacy_path))

    oklab_nodes = []
    regular_ramp_nodes = []
    for material in bpy.data.materials:
        if not material.node_tree:
            continue
        for node in material.node_tree.nodes:
            if node.bl_idname == OKLAB_NODE:
                oklab_nodes.append(node)
            elif node.bl_idname == COLOR_RAMP_NODE:
                regular_ramp_nodes.append(node)

    assert_true(len(oklab_nodes) == 1, f"expected one migrated OKLab node, found {len(oklab_nodes)}")
    assert_true(not regular_ramp_nodes, "legacy OKLAB Color Ramp was left as ShaderNodeValToRGB")
    migrated = oklab_nodes[0]
    assert_oklab_storage(migrated.color_ramp, "migrated OKLab")


def main():
    case_dir = Path(__file__).resolve().parent
    material = build_shader_tree()
    build_geometry_tree()
    build_compositor_tree()
    light = build_light_tree()
    render_shader_material(material, light)
    save_and_reopen_roundtrip(case_dir)
    verify_legacy_valtorgb_oklab_migration(case_dir)
    print("OKLAB_COLOR_RAMP_NODE_OK=1")


if __name__ == "__main__":
    main()
