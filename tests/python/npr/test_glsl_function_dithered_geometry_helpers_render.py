# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import os
from pathlib import Path

import bpy
from mathutils import Vector


RESOLUTION = 64


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def configure_scene():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = RESOLUTION
    scene.render.resolution_y = RESOLUTION
    scene.render.resolution_percentage = 100
    scene.eevee.taa_samples = 1
    scene.eevee.taa_render_samples = 1
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.world.use_nodes = False
    scene.world.color = (0.0, 0.0, 0.0)


def make_text_block(name, source):
    text = bpy.data.texts.get(name)
    if text is None:
        text = bpy.data.texts.new(name)
    else:
        text.clear()
    text.write(source)
    return text


def refresh_glsl_node(node):
    current_name = node.function_name
    node.function_name = ""
    node.function_name = current_name
    node.id_data.interface_update(bpy.context)
    node.id_data.update_tag()
    bpy.context.view_layer.update()


def make_dithered_glsl_material():
    material = bpy.data.materials.new("DitheredGLSLGeometryHelper")
    material.use_nodes = True
    material.surface_render_method = "DITHERED"
    material.blend_method = "HASHED"

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    transparent = nodes.new("ShaderNodeBsdfTransparent")
    emission = nodes.new("ShaderNodeEmission")
    mix = nodes.new("ShaderNodeMixShader")
    glsl = nodes.new("ShaderNodeGLSLFunction")

    emission.inputs["Color"].default_value = (0.0, 1.0, 0.0, 1.0)
    emission.inputs["Strength"].default_value = 1.0

    make_text_block(
        "glsl_dithered_geometry_alpha.glsl",
        "float dithered_geometry_alpha(){\n"
        "  return (glsl_position().x > 1.5) ? 1.0 : 0.0;\n"
        "}\n",
    )
    glsl.script = bpy.data.texts["glsl_dithered_geometry_alpha.glsl"]
    glsl.function_name = "dithered_geometry_alpha"
    refresh_glsl_node(glsl)
    assert glsl.parse_status == "READY", glsl.parse_message

    links.new(glsl.outputs["Result"], mix.inputs["Factor"])
    links.new(transparent.outputs["BSDF"], mix.inputs[1])
    links.new(emission.outputs["Emission"], mix.inputs[2])
    links.new(mix.outputs["Shader"], output.inputs["Surface"])
    return material


def look_at(obj, target):
    direction = Vector(target) - obj.location
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def build_scene():
    bpy.ops.mesh.primitive_cube_add(size=2.0, location=(2.0, 0.0, 0.0))
    cube = bpy.context.active_object
    cube.data.materials.append(make_dithered_glsl_material())

    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 3.0
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (2.0, -5.0, 0.0)
    look_at(camera, (2.0, 0.0, 0.0))
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def output_path():
    workspace_root = Path(os.environ.get("BLENDER_NPR_WORKSPACE_ROOT", Path(__file__).resolve().parents[4]))
    directory = workspace_root / "temp" / "render_exports"
    directory.mkdir(parents=True, exist_ok=True)
    return directory / "glsl_dithered_geometry_helpers_render.exr"


def render_pixels():
    scene = bpy.context.scene
    filepath = output_path()
    scene.render.image_settings.file_format = "OPEN_EXR"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "32"
    scene.render.filepath = str(filepath)

    bpy.ops.render.render(write_still=False)
    bpy.data.images["Render Result"].save_render(str(filepath))

    image = bpy.data.images.load(str(filepath), check_existing=False)
    try:
        return list(image.pixels), image.size[0], image.size[1]
    finally:
        bpy.data.images.remove(image)


def center_pixel(pixels, width, height):
    index = ((height // 2) * width + (width // 2)) * 4
    return pixels[index : index + 4]


bpy.ops.wm.read_homefile(use_factory_startup=True)
clear_scene()
configure_scene()
build_scene()

pixels, width, height = render_pixels()
pixel = center_pixel(pixels, width, height)

assert pixel[1] > 0.45, (
    "Dithered GLSL Function material should keep geometry-helper alpha visible in the "
    f"depth prepass; got center pixel {pixel}"
)
assert pixel[0] < 0.15 and pixel[2] < 0.15, f"Expected green emission, got {pixel}"
