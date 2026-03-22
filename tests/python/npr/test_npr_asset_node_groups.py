import bpy
import math
import os
import tempfile
from pathlib import Path


EXPECTED_ASSET_GROUPS = {
    "Cavity",
    "Co-Planar Edge Detection",
    "Curvature",
    "Kuwahara",
    "Shading Models",
    "Surface Curvature",
}


def asset_bundle_path():
    return Path(__file__).resolve().parents[3] / "assets" / "nodes" / "npr_node_groups.blend"


def inspect_asset_bundle(path: Path):
    bpy.ops.wm.open_mainfile(filepath=str(path), load_ui=False)

    asset_groups = {group.name for group in bpy.data.node_groups if group.asset_data is not None}
    missing = EXPECTED_ASSET_GROUPS - asset_groups
    assert not missing, f"Missing NPR asset node groups: {sorted(missing)}"

    for group in bpy.data.node_groups:
        if group.bl_idname != "ShaderNodeTree":
            continue

        undefined_nodes = [node.name for node in group.nodes if node.bl_idname == "NodeUndefined"]
        assert not undefined_nodes, f"{group.name} still contains undefined nodes: {undefined_nodes}"

    cavity = bpy.data.node_groups["Cavity"]
    assert any(node.bl_idname == "GeometryNodeRepeatInput" for node in cavity.nodes)
    assert any(node.bl_idname == "GeometryNodeRepeatOutput" for node in cavity.nodes)


def configure_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 64
    scene.render.resolution_y = 64
    scene.render.resolution_percentage = 100
    scene.eevee.taa_samples = 1
    scene.eevee.taa_render_samples = 1
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"

    world = bpy.data.worlds.new("World")
    world.use_nodes = False
    world.color = (0.0, 0.0, 0.0)
    scene.world = world
    return scene


def make_camera():
    camera_data = bpy.data.cameras.new("Camera")
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, -4.0, 0.0)
    camera.rotation_euler = (math.radians(90.0), 0.0, 0.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def append_cavity_group(path: Path):
    with bpy.data.libraries.load(str(path), link=False) as (data_from, data_to):
        assert "Cavity" in data_from.node_groups
        data_to.node_groups = ["Cavity"]
    return bpy.data.node_groups["Cavity"]


def make_npr_tree(cavity_group):
    npr_tree = bpy.data.node_groups.new("NPRAssetTree", "ShaderNodeTree")
    nodes = npr_tree.nodes
    links = npr_tree.links

    group = nodes.new("ShaderNodeGroup")
    group.node_tree = cavity_group

    ramp = nodes.new("ShaderNodeValToRGB")
    ramp.location = (220.0, 0.0)
    links.new(group.outputs["Combined"], ramp.inputs["Fac"])

    npr_output = nodes.new("ShaderNodeNPR_Output")
    npr_output.location = (460.0, 0.0)
    links.new(ramp.outputs["Color"], npr_output.inputs["Color"])
    return npr_tree


def make_material(npr_tree):
    material = bpy.data.materials.new("NPRAssetMaterial")
    material.use_nodes = True
    output = next(node for node in material.node_tree.nodes if node.bl_idname == "ShaderNodeOutputMaterial")
    output.nprtree = npr_tree
    return material


def make_sphere(material):
    bpy.ops.mesh.primitive_uv_sphere_add(radius=1.0, location=(0.0, 0.0, 0.0))
    sphere = bpy.context.active_object
    sphere.data.materials.append(material)


def render_image():
    scene = bpy.context.scene
    file_descriptor, filepath = tempfile.mkstemp(suffix=".exr")
    os.close(file_descriptor)
    try:
        scene.render.filepath = filepath
        scene.render.image_settings.file_format = "OPEN_EXR"
        scene.render.image_settings.color_mode = "RGBA"
        scene.render.image_settings.color_depth = "32"
        bpy.ops.render.render(write_still=False)
        bpy.data.images["Render Result"].save_render(filepath)
        image = bpy.data.images.load(filepath, check_existing=False)
        try:
            pixels = list(image.pixels[:])
            width = image.size[0]
            height = image.size[1]
        finally:
            bpy.data.images.remove(image)
    finally:
        if os.path.exists(filepath):
            os.remove(filepath)
    return pixels, width, height


def sample_center(pixels, width, height):
    index = ((height // 2) * width + (width // 2)) * 4
    return pixels[index:index + 4]


bundle = asset_bundle_path()
assert bundle.exists(), f"Missing NPR asset bundle at {bundle}"

inspect_asset_bundle(bundle)
configure_scene()
make_camera()
cavity_group = append_cavity_group(bundle)
make_sphere(make_material(make_npr_tree(cavity_group)))
pixels, width, height = render_image()
center = sample_center(pixels, width, height)

assert max(pixels) > 0.05, "Migrated NPR asset group render should not be fully black"
assert center[0] > 0.05, f"Expected visible center sample from migrated asset group, got {center}"

print("PASS")
