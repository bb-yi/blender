import bpy
import math
import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from filter_graph_test_utils import (
    add_pass_input_image_sample,
    attach_filter_material as attach_filter_material_to_graph,
    clear_filter_graph,
)


WIDTH = 257
HEIGHT = 257
ORTHO_SCALE = 4.0
ENCODE_BIAS = 4.0
ENCODE_RANGE = 8.0
POSITION_TOLERANCE = 0.03


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def configure_scene():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = WIDTH
    scene.render.resolution_y = HEIGHT
    scene.render.resolution_percentage = 100
    scene.eevee.taa_samples = 1
    scene.eevee.taa_render_samples = 1
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.world.use_nodes = False
    scene.world.color = (0.0, 0.0, 0.0)
    clear_filter_graph(scene)


def make_camera():
    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = ORTHO_SCALE
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, -6.0, 0.0)
    camera.rotation_euler = (math.radians(90.0), 0.0, 0.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def make_surface_material():
    material = bpy.data.materials.new("PositionReceiver")
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)

    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_filter_material():
    material = bpy.data.materials.new("ScenePositionFilter")
    material.use_nodes = True
    material.eevee_domain = "FILTER"

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputFilter")
    output.location = (700.0, 0.0)
    output.inputs["Alpha"].default_value = 1.0

    _, image_sample = add_pass_input_image_sample(nodes, links, location=(-40.0, 0.0))

    separate_xyz = nodes.new("ShaderNodeSeparateXYZ")
    separate_xyz.location = (180.0, 0.0)

    encode_x = nodes.new("ShaderNodeMath")
    encode_x.location = (360.0, 80.0)
    encode_x.operation = "MULTIPLY_ADD"
    encode_x.inputs[1].default_value = 1.0 / ENCODE_RANGE
    encode_x.inputs[2].default_value = ENCODE_BIAS / ENCODE_RANGE
    encode_x.use_clamp = True

    encode_z = nodes.new("ShaderNodeMath")
    encode_z.location = (360.0, -80.0)
    encode_z.operation = "MULTIPLY_ADD"
    encode_z.inputs[1].default_value = 1.0 / ENCODE_RANGE
    encode_z.inputs[2].default_value = ENCODE_BIAS / ENCODE_RANGE
    encode_z.use_clamp = True

    combine = nodes.new("ShaderNodeCombineColor")
    combine.location = (540.0, 0.0)

    links.new(image_sample.outputs["Color"], separate_xyz.inputs["Vector"])
    links.new(separate_xyz.outputs["X"], encode_x.inputs[0])
    links.new(separate_xyz.outputs["Z"], encode_z.inputs[0])
    links.new(encode_x.outputs["Value"], combine.inputs["Red"])
    links.new(encode_z.outputs["Value"], combine.inputs["Green"])
    links.new(combine.outputs["Color"], output.inputs["Color"])
    return material


def attach_filter_material(material):
    attach_filter_material_to_graph(material, stage="BEFORE_COMPOSITE", scene_socket="Position Image")


def make_plane(material):
    mesh = bpy.data.meshes.new("PositionReceiverMesh")
    mesh.from_pydata(
        [
            (-6.0, 0.0, -6.0),
            (6.0, 0.0, -6.0),
            (6.0, 0.0, 6.0),
            (-6.0, 0.0, 6.0),
        ],
        [],
        [(0, 1, 2, 3)],
    )
    mesh.update()

    plane = bpy.data.objects.new("PositionReceiver", mesh)
    bpy.context.scene.collection.objects.link(plane)
    plane.data.materials.append(material)


def render_image():
    scene = bpy.context.scene
    file_descriptor, filepath = tempfile.mkstemp(suffix=".exr")
    os.close(file_descriptor)

    scene.render.image_settings.file_format = "OPEN_EXR"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "32"
    scene.render.filepath = filepath

    bpy.ops.render.render(write_still=False)
    bpy.data.images["Render Result"].save_render(filepath)

    image = bpy.data.images.load(filepath, check_existing=False)
    try:
        pixels = list(image.pixels[:])
        width = image.size[0]
        height = image.size[1]
    finally:
        bpy.data.images.remove(image)
        if os.path.exists(filepath):
            os.remove(filepath)

    return pixels, width, height


def sample_pixel(pixels, width, x, y):
    index = (y * width + x) * 4
    return list(pixels[index:index + 4])


def decode_position_sample(color):
    return (
        color[0] * ENCODE_RANGE - ENCODE_BIAS,
        color[1] * ENCODE_RANGE - ENCODE_BIAS,
    )


def expected_world_position(width, height, x, y):
    aspect = width / height
    return (
        ((x + 0.5) / width - 0.5) * ORTHO_SCALE * aspect,
        ((y + 0.5) / height - 0.5) * ORTHO_SCALE,
    )


def assert_position_sample(pixels, width, height, x, y):
    color = sample_pixel(pixels, width, x, y)
    actual_x, actual_z = decode_position_sample(color)
    expected_x, expected_z = expected_world_position(width, height, x, y)
    assert abs(actual_x - expected_x) < POSITION_TOLERANCE, (
        f"Scene Color(Position) X mismatch at pixel ({x}, {y}): "
        f"expected {expected_x:.5f}, got {actual_x:.5f}, raw color {color}"
    )
    assert abs(actual_z - expected_z) < POSITION_TOLERANCE, (
        f"Scene Color(Position) Z mismatch at pixel ({x}, {y}): "
        f"expected {expected_z:.5f}, got {actual_z:.5f}, raw color {color}"
    )


def main():
    clear_scene()
    configure_scene()
    make_camera()
    make_plane(make_surface_material())
    attach_filter_material(make_filter_material())

    pixels, width, height = render_image()

    assert width == WIDTH and height == HEIGHT, f"Expected {WIDTH}x{HEIGHT} render, got {width}x{height}"
    for x, y in (
        (width // 2, height // 2),
        (width // 4, height // 4),
        (width * 3 // 4, height * 3 // 4),
        (width // 3, height * 2 // 3),
    ):
        assert_position_sample(pixels, width, height, x, y)


if __name__ == "__main__":
    main()
