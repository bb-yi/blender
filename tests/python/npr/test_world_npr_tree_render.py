import bpy
import os
import tempfile


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def configure_scene():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 64
    scene.render.resolution_y = 64
    scene.render.resolution_percentage = 100
    scene.eevee.taa_samples = 1
    scene.eevee.taa_render_samples = 1
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"

    scene.world.use_nodes = True
    nodes = scene.world.node_tree.nodes
    links = scene.world.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputWorld")
    background = nodes.new("ShaderNodeBackground")
    background.inputs["Color"].default_value = (0.0, 1.0, 0.0, 1.0)
    background.inputs["Strength"].default_value = 1.0
    links.new(background.outputs["Background"], output.inputs["Surface"])

    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 2.0
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 4.0)
    scene.collection.objects.link(camera)
    scene.camera = camera

    return output


def render_center_pixel():
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
        width = image.size[0]
        height = image.size[1]
        index = ((height // 2) * width + (width // 2)) * 4
        pixel = list(image.pixels[index:index + 4])
    finally:
        bpy.data.images.remove(image)
        if os.path.exists(filepath):
            os.remove(filepath)

    return pixel


def attach_solid_world_npr(output):
    npr_tree = bpy.data.node_groups.new("WorldSolidNPRTree", "ShaderNodeTree")
    nodes = npr_tree.nodes
    links = npr_tree.links
    nodes.clear()

    rgb = nodes.new("ShaderNodeRGB")
    rgb.outputs["Color"].default_value = (1.0, 0.0, 0.0, 1.0)
    npr_output = nodes.new("ShaderNodeNPR_Output")
    links.new(rgb.outputs["Color"], npr_output.inputs["Color"])

    output.nprtree = npr_tree


def attach_combined_color_world_npr(output):
    npr_tree = bpy.data.node_groups.new("WorldCombinedColorNPRTree", "ShaderNodeTree")
    nodes = npr_tree.nodes
    links = npr_tree.links
    nodes.clear()

    npr_input = nodes.new("ShaderNodeNPR_Input")
    npr_output = nodes.new("ShaderNodeNPR_Output")
    links.new(npr_input.outputs["Combined Color"], npr_output.inputs["Color"])

    output.nprtree = npr_tree


clear_scene()
world_output = configure_scene()

attach_solid_world_npr(world_output)
pixel = render_center_pixel()
r, g, b, _a = pixel
print(f"WORLD_NPR_SOLID_CENTER={pixel}")

assert r > 0.8, f"Expected World NPR solid output to replace background with red, got {pixel}"
assert g < 0.1, f"Expected World NPR solid output to replace green background, got {pixel}"
assert b < 0.1, f"Expected low blue from World NPR solid output, got {pixel}"

attach_combined_color_world_npr(world_output)
pixel = render_center_pixel()
r, g, b, _a = pixel
print(f"WORLD_NPR_COMBINED_CENTER={pixel}")

assert g > 0.8, f"Expected World NPR Combined Color to read green background, got {pixel}"
assert r < 0.1, f"Expected low red from World NPR Combined Color passthrough, got {pixel}"
assert b < 0.1, f"Expected low blue from World NPR Combined Color passthrough, got {pixel}"
