import bpy
import math
import os
import tempfile


RESOLUTION = 192


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


def make_camera():
    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 4.0

    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 5.0)
    camera.rotation_euler = (0.0, 0.0, 0.0)

    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def make_cube():
    bpy.ops.mesh.primitive_cube_add(size=2.0, location=(0.0, 0.0, 0.0))
    cube = bpy.context.active_object
    cube.rotation_euler = (
        math.radians(35.0),
        math.radians(10.0),
        math.radians(45.0),
    )
    return cube


def make_bevel_material():
    material = bpy.data.materials.new("EeveeBevelPreview")
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    output.location = (720.0, 0.0)

    emission = nodes.new("ShaderNodeEmission")
    emission.location = (520.0, 0.0)
    emission.inputs["Strength"].default_value = 1.0

    multiply_add = nodes.new("ShaderNodeMath")
    multiply_add.location = (300.0, 0.0)
    multiply_add.operation = "MULTIPLY_ADD"
    multiply_add.inputs[1].default_value = 0.5
    multiply_add.inputs[2].default_value = 0.5
    multiply_add.use_clamp = True

    dot = nodes.new("ShaderNodeVectorMath")
    dot.location = (80.0, 0.0)
    dot.operation = "DOT_PRODUCT"
    dot.inputs[1].default_value = (0.57735, 0.57735, 0.57735)

    bevel = nodes.new("ShaderNodeBevel")
    bevel.location = (-180.0, 0.0)
    bevel.samples = 8

    links.new(bevel.outputs["Normal"], dot.inputs[0])
    links.new(dot.outputs["Value"], multiply_add.inputs[0])
    links.new(multiply_add.outputs["Value"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    return material, bevel


def render_pixels():
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
    finally:
        bpy.data.images.remove(image)
        if os.path.exists(filepath):
            os.remove(filepath)

    return pixels


def count_changed_pixels(reference_pixels, bevel_pixels, threshold):
    changed = 0
    max_diff = 0.0

    for index in range(0, len(reference_pixels), 4):
        diff = max(
            abs(reference_pixels[index + channel] - bevel_pixels[index + channel])
            for channel in range(3)
        )
        max_diff = max(max_diff, diff)
        if diff > threshold:
            changed += 1

    return changed, max_diff


def main():
    assert hasattr(bpy.types, "ShaderNodeBevel"), "ShaderNodeBevel is not registered"

    clear_scene()
    configure_scene()
    make_camera()

    cube = make_cube()
    material, bevel_node = make_bevel_material()
    cube.data.materials.append(material)

    bevel_node.inputs["Radius"].default_value = 0.0
    reference_pixels = render_pixels()

    bevel_node.inputs["Radius"].default_value = 0.25
    bevel_node.samples = 12
    bevel_pixels = render_pixels()

    changed_pixels, max_diff = count_changed_pixels(reference_pixels, bevel_pixels, threshold=0.02)

    assert changed_pixels > 120, (
        f"Expected Eevee bevel to alter a visible edge region, got only {changed_pixels} changed pixels"
    )
    assert max_diff > 0.08, (
        f"Expected Eevee bevel to noticeably change the normal preview, got max diff {max_diff}"
    )


if __name__ == "__main__":
    main()
