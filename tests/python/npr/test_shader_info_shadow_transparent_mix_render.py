# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import os
from pathlib import Path

import bpy
from mathutils import Vector


RESOLUTION = 96


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def configure_scene():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = RESOLUTION
    scene.render.resolution_y = RESOLUTION
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = True
    scene.eevee.taa_samples = 16
    scene.eevee.taa_render_samples = 16
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.world.color = (0.0, 0.0, 0.0)


def refresh_scene():
    for material in bpy.data.materials:
        material.node_tree.update_tag()
    bpy.context.view_layer.update()


def make_shadow_transparency_material():
    material = bpy.data.materials.new("ShaderInfoShadowTransparentMix")
    material.use_nodes = True
    material.surface_render_method = "DITHERED"
    material.blend_method = "HASHED"

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    shader_info = nodes.new("ShaderNodeShaderInfo")
    ramp = nodes.new("ShaderNodeValToRGB")
    transparent = nodes.new("ShaderNodeBsdfTransparent")
    emission = nodes.new("ShaderNodeEmission")
    mix = nodes.new("ShaderNodeMixShader")

    color_ramp = ramp.color_ramp
    color_ramp.elements[0].position = 0.0
    color_ramp.elements[0].color = (1.0, 1.0, 1.0, 1.0)
    color_ramp.elements[1].position = 0.1
    color_ramp.elements[1].color = (0.0, 0.0, 0.0, 1.0)
    emission.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    emission.inputs["Strength"].default_value = 1.0

    links.new(shader_info.outputs["Shadow"], ramp.inputs["Factor"])
    links.new(ramp.outputs["Color"], mix.inputs["Factor"])
    links.new(transparent.outputs["BSDF"], mix.inputs[1])
    links.new(emission.outputs["Emission"], mix.inputs[2])
    links.new(mix.outputs["Shader"], output.inputs["Surface"])
    return material


def look_at(obj, target):
    direction = Vector(target) - obj.location
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def build_scene():
    bpy.ops.mesh.primitive_cube_add(size=3.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.active_object
    plane.name = "TransparentReceiver"
    plane.scale.z = 0.02
    plane.data.materials.append(make_shadow_transparency_material())

    light_data = bpy.data.lights.new("KeyLight", "SUN")
    light_data.energy = 5.0
    light = bpy.data.objects.new("KeyLight", light_data)
    light.rotation_euler = (0.7, 0.0, 0.6)
    bpy.context.scene.collection.objects.link(light)

    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 4.5
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, -5.0, 2.5)
    look_at(camera, (0.0, 0.0, 0.0))
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera
    refresh_scene()


def output_path():
    workspace_root = Path(os.environ.get("BLENDER_NPR_WORKSPACE_ROOT", Path(__file__).resolve().parents[4]))
    directory = workspace_root / "temp" / "render_exports"
    directory.mkdir(parents=True, exist_ok=True)
    return directory / "shader_info_shadow_transparent_mix_render.exr"


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
        return list(image.pixels)
    finally:
        bpy.data.images.remove(image)


def pixel_stats(pixels):
    total = len(pixels) // 4
    alpha_sum = 0.0
    black_opaque = 0
    for index in range(0, len(pixels), 4):
        r, g, b, a = pixels[index : index + 4]
        alpha_sum += a
        if a > 0.95 and max(r, g, b) < 0.02:
            black_opaque += 1
    return alpha_sum / total, black_opaque, total


bpy.ops.wm.read_homefile(use_factory_startup=True)
clear_scene()
configure_scene()
build_scene()

pixels = render_pixels()
mean_alpha, black_opaque, total = pixel_stats(pixels)

assert mean_alpha < 0.1, (
    "Lit Shader Info Shadow output should keep the material transparent in the dithered depth "
    f"prepass; got mean alpha {mean_alpha}, black opaque pixels {black_opaque}/{total}"
)
assert black_opaque < 16, (
    "Transparent Shader Info Shadow mix should not leave opaque black prepass coverage; got "
    f"{black_opaque}/{total}"
)
