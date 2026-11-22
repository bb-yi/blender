"""Run with Blender factory startup and an isolated NPR_REVIEW_OUTPUT_DIR."""

import contextlib
import os
from pathlib import Path
import sys

import bpy


arguments = sys.argv[sys.argv.index("--") + 1:]
mode = arguments[0]
output_dir = Path(os.environ["NPR_REVIEW_OUTPUT_DIR"])
output_dir.mkdir(parents=True, exist_ok=True)
fixture = Path(arguments[1]) if len(arguments) > 1 else output_dir / "portal_legacy.blend"


@contextlib.contextmanager
def editor(tree):
    window = bpy.context.window
    area = next(area for area in window.screen.areas if area.type == 'VIEW_3D')
    old_type = area.type
    area.type = 'NODE_EDITOR'
    area.spaces.active.tree_type = 'ShaderNodeTree'
    area.spaces.active.pin = True
    area.spaces.active.node_tree = tree
    region = next(region for region in area.regions if region.type == 'WINDOW')
    try:
        with bpy.context.temp_override(window=window, area=area, region=region):
            yield
    finally:
        area.type = old_type


def select(tree, *nodes):
    for node in tree.nodes:
        node.select = node in nodes
    tree.nodes.active = nodes[0]


def socket(sockets):
    return next(item for item in sockets if item.enabled)


def make_portal(tree, name, data_type):
    portal_in = tree.nodes.new('ShaderNodePortalIn')
    portal_out = tree.nodes.new('ShaderNodePortalOut')
    portal_in.portal_name = portal_out.portal_name = name
    portal_in.data_type = portal_out.data_type = data_type
    return portal_in, portal_out


def rgba(tree, value):
    node = tree.nodes.new('ShaderNodeRGB')
    node.outputs[0].default_value = (*value, 1.0)
    return node


def setup():
    scene = bpy.context.scene
    scene.render.resolution_x = scene.render.resolution_y = 32
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.render.image_settings.color_depth = '32'
    scene.camera.location = (0, 0, 6)
    scene.camera.rotation_euler = (0, 0, 0)
    scene.camera.data.type = 'ORTHO'
    scene.camera.data.ortho_scale = 4
    scene.eevee.taa_render_samples = 1
    material = bpy.data.materials.new("Portal Editing")
    bpy.data.objects["Cube"].data.materials.clear()
    bpy.data.objects["Cube"].data.materials.append(material)
    tree = material.node_tree
    tree.nodes.clear()
    emission = tree.nodes.new('ShaderNodeEmission')
    output = tree.nodes.new('ShaderNodeOutputMaterial')
    tree.links.new(emission.outputs[0], output.inputs['Surface'])
    return tree, emission


def pixel(label):
    bpy.ops.render.render()
    path = output_dir / f"{label}.exr"
    bpy.data.images['Render Result'].save_render(str(path))
    image = bpy.data.images.load(str(path), check_existing=False)
    offset = (16 * 32 + 16) * 4
    result = tuple(image.pixels[offset:offset + 3])
    bpy.data.images.remove(image)
    print("PIXEL", label, result, flush=True)
    return result


def near(actual, expected):
    assert all(abs(value - target) < 0.015 for value, target in zip(actual, expected)), (actual, expected)


def make_legacy_fixture(mismatched=False):
    tree, emission = setup()
    source = rgba(tree, (0.7, 0.2, 0.1))
    portal_in, portal_out = make_portal(tree, "旧工程", 'RGBA')
    tree.links.new(source.outputs[0], socket(portal_in.inputs))
    tree.links.new(socket(portal_out.outputs), emission.inputs['Color'])
    if mismatched:
        stale_out = tree.nodes.new('ShaderNodePortalOut')
        stale_out.name = "Legacy mismatched output"
        stale_out.portal_name = portal_in.portal_name
        stale_out.data_type = 'SHADER'
        assert stale_out.data_type == 'SHADER'
    bpy.ops.wm.save_as_mainfile(filepath=str(fixture))


def verify_legacy():
    bpy.ops.wm.open_mainfile(filepath=str(fixture), load_ui=False, use_scripts=False)
    tree = bpy.data.materials["Portal Editing"].node_tree
    in_node = next(node for node in tree.nodes if node.bl_idname == 'ShaderNodePortalIn')
    outputs = [node for node in tree.nodes if node.bl_idname == 'ShaderNodePortalOut']
    assert {node.portal_name for node in (in_node, *outputs)} == {"旧工程"}
    assert {node.data_type for node in (in_node, *outputs)} == {'RGBA'}
    near(in_node.portal_color, (0.15, 0.36, 0.40))
    with editor(tree):
        select(tree, in_node)
        assert bpy.ops.node.shader_portal_rename(name="旧工程新名称") == {'FINISHED'}
    assert {node.portal_name for node in (in_node, *outputs)} == {"旧工程新名称"}
    in_node.portal_color = (0.52, 0.21, 0.32)
    saved = output_dir / "portal_color_roundtrip.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(saved))
    bpy.ops.wm.open_mainfile(filepath=str(saved), load_ui=False, use_scripts=False)
    tree = bpy.data.materials["Portal Editing"].node_tree
    in_node = next(node for node in tree.nodes if node.bl_idname == 'ShaderNodePortalIn')
    out_node = next(node for node in tree.nodes if node.bl_idname == 'ShaderNodePortalOut')
    assert in_node.paired_name == out_node.portal_name == "旧工程新名称"
    assert in_node.use_custom_color
    near(in_node.portal_color, (0.52, 0.21, 0.32))
    near(pixel("legacy"), (0.7, 0.2, 0.1))


def navigation_and_binding():
    tree, emission = setup()
    portal_in, portal_out = make_portal(tree, "Brightness", 'FLOAT')
    warmer, spare = make_portal(tree, "Warm", 'RGBA')
    closure, unused_out = make_portal(tree, "Closure", 'SHADER')
    select(tree, spare)
    with editor(tree):
        assert bpy.ops.node.shader_portal_bind(source=portal_in.name) == {'FINISHED'}
        assert spare.portal_name == "Brightness" and spare.data_type == 'FLOAT'
        tree.links.new(socket(portal_out.outputs), emission.inputs['Color'])
        select(tree, portal_out)
        assert bpy.ops.node.shader_portal_bind(source=warmer.name) == {'FINISHED'}
        assert portal_out.portal_name == "Warm" and portal_out.data_type == 'RGBA'
        assert socket(portal_out.outputs).links[0].to_node == emission
        assert bpy.ops.node.shader_portal_bind(source=closure.name) == {'CANCELLED'}
        assert portal_out.portal_name == "Warm" and portal_out.data_type == 'RGBA'
        assert bpy.ops.node.shader_portal_bind(source=closure.name, allow_type_change=True) == {'FINISHED'}
        assert portal_out.data_type == 'SHADER'
        assert not socket(portal_out.outputs).links
        assert len(emission.inputs['Color'].links) == 1
        assert bpy.ops.node.shader_portal_bind(source=warmer.name, allow_type_change=True) == {'FINISHED'}
        assert socket(portal_out.outputs).links[0].to_node == emission
        portal_out.portal_name = portal_in.portal_name
        assert portal_out.data_type == 'FLOAT'
        portal_in.data_type = 'VECTOR'
        assert portal_out.data_type == spare.data_type == 'VECTOR'
        assert socket(portal_out.outputs).links[0].to_node == emission
        portal_in.data_type = 'SHADER'
        assert portal_out.data_type == spare.data_type == 'SHADER'
        assert not socket(portal_out.outputs).links
        assert len(emission.inputs['Color'].links) == 1
        portal_in.data_type = 'VECTOR'
        assert socket(portal_out.outputs).links[0].to_node == emission
        portal_in.data_type = 'FLOAT'
        assert portal_out.data_type == spare.data_type == 'FLOAT'
        assert socket(portal_out.outputs).links[0].to_node == emission
        select(tree, portal_in)
        assert bpy.ops.node.shader_portal_rename(name="Warm") == {'CANCELLED'}
        portal_in.paired_name = "Warm"
        assert portal_in.paired_name == "Brightness"
        portal_in.paired_name = "前景亮度"
        assert {portal_in.portal_name, portal_out.portal_name, spare.portal_name} == {"前景亮度"}
        assert warmer.portal_name == "Warm"
        near(portal_in.portal_color, (0.15, 0.36, 0.40))
        portal_in.portal_color = (0.52, 0.21, 0.32)
        assert portal_in.use_custom_color
        near(portal_in.color, (0.52, 0.21, 0.32))
        assert bpy.ops.node.shader_portal_jump_to_output(output=spare.name) == {'FINISHED'}
        assert tree.nodes.active == spare


def wire_roundtrip(engine):
    tree, emission = setup()
    scene = bpy.context.scene
    scene.render.engine = engine
    if engine == 'CYCLES':
        scene.cycles.device = 'CPU'
        scene.cycles.samples = 1
    source = rgba(tree, (0.8, 0.2, 0.1))
    tree.links.new(source.outputs[0], emission.inputs['Color'])
    direct = pixel("direct")
    near(direct, (0.8, 0.2, 0.1))
    orphan = tree.nodes.new('ShaderNodePortalOut')
    orphan.portal_name = "Portal"
    orphan.data_type = 'RGBA'
    with editor(tree):
        select(tree, source, emission)
        assert bpy.ops.node.shader_portal_from_link() == {'FINISHED'}
    portal_in = next(node for node in tree.nodes if node.bl_idname == 'ShaderNodePortalIn')
    portal_out = emission.inputs['Color'].links[0].from_node
    assert portal_in.portal_name == portal_out.portal_name
    assert portal_out.portal_name != orphan.portal_name
    near(pixel("portalized"), direct)
    with editor(tree):
        select(tree, portal_out)
        assert bpy.ops.node.shader_portal_to_links() == {'FINISHED'}
    assert emission.inputs['Color'].links[0].from_node == source
    near(pixel("restored"), direct)
    portal_name = portal_in.portal_name
    other = tree.nodes.new('ShaderNodePortalOut')
    other.portal_name = portal_name
    scalar = tree.nodes.new('ShaderNodeValue')
    scalar.outputs[0].default_value = 0.37
    vector = tree.nodes.new('ShaderNodeCombineXYZ')
    for item, value in zip(vector.inputs, (0.2, 0.4, 0.6)):
        item.default_value = value
    for from_type, origin, to_type, converter in (
        ('RGBA', source, 'FLOAT', 'ShaderNodeRGBToBW'),
        ('VECTOR', vector, 'FLOAT', 'ShaderNodeVectorMath'),
        ('FLOAT', scalar, 'VECTOR', 'ShaderNodeCombineXYZ'),
        ('FLOAT', scalar, 'RGBA', 'ShaderNodeCombineColor'),
        ('RGBA', source, 'VECTOR', 'ShaderNodeCombineXYZ'),
        ('VECTOR', vector, 'RGBA', 'ShaderNodeCombineColor'),
    ):
        label = f"{from_type}_to_{to_type}"
        for item in list(tree.links):
            if item.to_node == portal_in:
                tree.links.remove(item)
        portal_in.data_type = other.data_type = to_type
        tree.links.new(origin.outputs[0], socket(portal_in.inputs))
        chosen = tree.nodes.new('ShaderNodePortalOut')
        chosen.portal_name = portal_name
        chosen.data_type = to_type
        tree.links.new(socket(chosen.outputs), emission.inputs['Color'])
        mixed = pixel(f"{label}_portal")
        assert min(mixed) > 0.05
        with editor(tree):
            select(tree, chosen)
            assert bpy.ops.node.shader_portal_to_links() == {'FINISHED'}
        assert other in tree.nodes[:]
        assert emission.inputs['Color'].links[0].from_node.bl_idname == converter
        near(pixel(f"{label}_restored"), mixed)


def repeat_roundtrip():
    tree, emission = setup()
    repeat_out = tree.nodes.new('GeometryNodeRepeatOutput')
    repeat_out.repeat_items.clear()
    repeat_out.repeat_items.new('FLOAT', 'Carry')
    repeat_in = tree.nodes.new('GeometryNodeRepeatInput')
    assert repeat_in.pair_with_output(repeat_out)
    repeat_in.inputs['Iterations'].default_value = 3
    repeat_in.inputs['Carry'].default_value = 0.25
    step = tree.nodes.new('ShaderNodeMath')
    step.operation = 'ADD'
    step.inputs[1].default_value = 0.25
    tree.links.new(repeat_in.outputs['Carry'], step.inputs[0])
    tree.links.new(step.outputs[0], repeat_out.inputs['Carry'])
    tree.links.new(repeat_out.outputs['Carry'], emission.inputs['Color'])
    reference = pixel("repeat_direct")
    near(reference, (1.0, 1.0, 1.0))
    with editor(tree):
        select(tree, repeat_in, step)
        assert bpy.ops.node.shader_portal_from_link() == {'FINISHED'}
    near(pixel("repeat_portal"), reference)
    with editor(tree):
        select(tree, step.inputs[0].links[0].from_node)
        assert bpy.ops.node.shader_portal_to_links() == {'FINISHED'}
    near(pixel("repeat_restored"), reference)


if mode == "legacy_write":
    make_legacy_fixture()
elif mode == "legacy_mismatch_write":
    make_legacy_fixture(mismatched=True)
elif mode == "legacy_read":
    verify_legacy()
elif mode == "editor":
    navigation_and_binding()
    wire_roundtrip('BLENDER_EEVEE')
    repeat_roundtrip()
elif mode == "editor_cycles":
    wire_roundtrip('CYCLES')
    repeat_roundtrip()
else:
    raise ValueError(mode)

print("PORTAL_EDITOR_PASS", mode, bpy.app.build_hash.decode(), flush=True)
