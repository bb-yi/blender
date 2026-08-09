import os
import tempfile
from pathlib import Path

import bpy


CASE_DIR = Path(__file__).resolve().parent
SCENE_PATH = CASE_DIR / "assets" / "010-specular-nonmetal" / "scene.blend"


def configure_scene():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 128
    scene.render.resolution_y = 128
    scene.render.resolution_percentage = 100
    scene.eevee.taa_samples = 1
    scene.eevee.taa_render_samples = 1
    scene.eevee.clamp_surface_indirect = 10.0
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0

    suzanne = bpy.data.objects["Suzanne"]
    bpy.context.view_layer.objects.active = suzanne
    suzanne.select_set(True)

    material = suzanne.active_material
    principled = material.node_tree.nodes["Principled BSDF"]
    principled.inputs["Metallic"].default_value = 0.0

    output = material.node_tree.nodes["Material Output"]
    assert output.nprtree is not None, "Material Output must use the NPR test tree"
    return output.nprtree


def configure_world(strength):
    scene = bpy.context.scene
    scene.world.use_nodes = True
    background = scene.world.node_tree.nodes["Background"]
    background.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    background.inputs["Strength"].default_value = strength


def connect_npr_socket(npr_tree, socket_name):
    links = npr_tree.links
    npr_input = npr_tree.nodes["NPR Input"]
    npr_output = npr_tree.nodes["NPR Output"]
    for link in list(npr_output.inputs["Color"].links):
        links.remove(link)
    links.new(npr_input.outputs[socket_name], npr_output.inputs["Color"])


def connect_white_mask(npr_tree):
    links = npr_tree.links
    npr_output = npr_tree.nodes["NPR Output"]
    for link in list(npr_output.inputs["Color"].links):
        links.remove(link)
    rgb = npr_tree.nodes.new("ShaderNodeRGB")
    rgb.outputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    links.new(rgb.outputs["Color"], npr_output.inputs["Color"])


def render_pixels():
    file_descriptor, filepath = tempfile.mkstemp(suffix=".exr")
    os.close(file_descriptor)

    scene = bpy.context.scene
    scene.render.image_settings.file_format = "OPEN_EXR"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "32"
    scene.render.filepath = filepath

    bpy.ops.render.render(write_still=False)
    bpy.data.images["Render Result"].save_render(filepath)

    image = bpy.data.images.load(filepath, check_existing=False)
    try:
        pixels = list(image.pixels)
    finally:
        bpy.data.images.remove(image)
        if os.path.exists(filepath):
            os.remove(filepath)
    return pixels


def luminance(rgb):
    return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]


def luminance_at(pixels, index):
    return luminance(pixels[index:index + 3])


assert SCENE_PATH.exists(), f"Missing NPR specular test scene: {SCENE_PATH}"

bpy.ops.wm.open_mainfile(filepath=str(SCENE_PATH))
npr_tree = configure_scene()
configure_world(0.0)
connect_white_mask(npr_tree)
mask_pixels = render_pixels()
object_pixel_indices = [
    i for i in range(0, len(mask_pixels), 4) if luminance_at(mask_pixels, i) > 0.5
]

print(f"NPR_INPUT_NONMETAL_MASK_PIXELS={len(object_pixel_indices)}")
assert len(object_pixel_indices) > 512, "Expected the Suzanne object mask to cover visible pixels"

checks = [
    ("Combined Color", 0.0, 0.05, 32),
    ("Diffuse Color", 0.0, 0.05, 32),
    ("Diffuse Direct", 0.0, 0.05, 8),
    ("Diffuse Indirect", 1.0, 0.05, 32),
    ("Specular Color", 0.0, 0.05, 32),
    ("Specular Direct", 0.0, 0.05, 8),
    ("Specular Indirect", 1.0, 0.05, 32),
    ("Position", 0.0, 0.05, 32),
    ("Normal", 0.0, 0.05, 32),
]

for socket_name, world_strength, min_max_luma, min_bright_pixels in checks:
    bpy.ops.wm.open_mainfile(filepath=str(SCENE_PATH))
    npr_tree = configure_scene()
    configure_world(world_strength)
    connect_npr_socket(npr_tree, socket_name)

    pixels = render_pixels()
    luminances = [luminance_at(pixels, i) for i in object_pixel_indices]
    max_luma = max(luminances)
    bright_pixels = sum(1 for value in luminances if value > 0.02)

    safe_name = socket_name.upper().replace(" ", "_")
    print(f"NPR_INPUT_{safe_name}_NONMETAL_MAX={max_luma:.6f}")
    print(f"NPR_INPUT_{safe_name}_NONMETAL_BRIGHT_PIXELS={bright_pixels}")

    assert max_luma > min_max_luma, (
        f"NPR Input {socket_name} should be non-black on the non-metal Suzanne material, "
        f"got max luminance {max_luma:.6f}"
    )
    assert bright_pixels >= min_bright_pixels, (
        f"NPR Input {socket_name} should affect visible Suzanne pixels, "
        f"got {bright_pixels} bright pixels"
    )
