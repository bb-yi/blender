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


def attach_image_sample_world_npr(output, input_name, tree_name, scale=1.0):
    npr_tree = bpy.data.node_groups.new(tree_name, "ShaderNodeTree")
    nodes = npr_tree.nodes
    links = npr_tree.links
    nodes.clear()

    npr_input = nodes.new("ShaderNodeNPR_Input")
    image_sample = nodes.new("ShaderNodeNPR_ImageSample")
    npr_output = nodes.new("ShaderNodeNPR_Output")

    links.new(npr_input.outputs[input_name], image_sample.inputs["Image"])

    if scale == 1.0:
        links.new(image_sample.outputs["Color"], npr_output.inputs["Color"])
    else:
        scale_node = nodes.new("ShaderNodeVectorMath")
        scale_node.operation = "SCALE"
        scale_node.inputs["Scale"].default_value = scale
        links.new(image_sample.outputs["Color"], scale_node.inputs["Vector"])
        links.new(scale_node.outputs["Vector"], npr_output.inputs["Color"])

    output.nprtree = npr_tree


def use_directional_world_background():
    nodes = bpy.context.scene.world.node_tree.nodes
    links = bpy.context.scene.world.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputWorld")
    background = nodes.new("ShaderNodeBackground")
    tex_coord = nodes.new("ShaderNodeTexCoord")
    separate = nodes.new("ShaderNodeSeparateXYZ")
    multiply_add = nodes.new("ShaderNodeMath")
    combine = nodes.new("ShaderNodeCombineColor")

    background.inputs["Strength"].default_value = 1.0
    multiply_add.operation = "MULTIPLY_ADD"
    multiply_add.inputs[1].default_value = 0.5
    multiply_add.inputs[2].default_value = 0.5
    multiply_add.use_clamp = True

    links.new(tex_coord.outputs["Generated"], separate.inputs["Vector"])
    links.new(separate.outputs["X"], multiply_add.inputs[0])
    links.new(multiply_add.outputs["Value"], combine.inputs["Red"])
    links.new(combine.outputs["Color"], background.inputs["Color"])
    links.new(background.outputs["Background"], output.inputs["Surface"])

    return output


def attach_combined_color_image_sample_world_npr(output, offset_x):
    npr_tree = bpy.data.node_groups.new("WorldCombinedColorImageSampleNPRTree", "ShaderNodeTree")
    nodes = npr_tree.nodes
    links = npr_tree.links
    nodes.clear()

    npr_input = nodes.new("ShaderNodeNPR_Input")
    image_sample = nodes.new("ShaderNodeNPR_ImageSample")
    image_sample.offset_type = "PIXEL"
    offset = nodes.new("ShaderNodeCombineXYZ")
    offset.inputs["X"].default_value = offset_x
    npr_output = nodes.new("ShaderNodeNPR_Output")

    links.new(npr_input.outputs["Combined Color"], image_sample.inputs["Image"])
    links.new(offset.outputs["Vector"], image_sample.inputs["Offset"])
    links.new(image_sample.outputs["Color"], npr_output.inputs["Color"])

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

attach_image_sample_world_npr(world_output, "Normal", "WorldNormalImageSampleNPRTree")
pixel = render_center_pixel()
r, g, b, _a = pixel
print(f"WORLD_NPR_IMAGE_SAMPLE_NORMAL_CENTER={pixel}")

assert b > 0.8, f"Expected World NPR Image Sample to read Normal blue channel, got {pixel}"
assert abs(r) < 0.1, f"Expected low red from World NPR Normal Image Sample, got {pixel}"
assert abs(g) < 0.1, f"Expected low green from World NPR Normal Image Sample, got {pixel}"

attach_image_sample_world_npr(world_output, "Position", "WorldPositionImageSampleNPRTree", -1.0)
pixel = render_center_pixel()
r, g, b, _a = pixel
print(f"WORLD_NPR_IMAGE_SAMPLE_POSITION_CENTER={pixel}")

assert b > 0.8, f"Expected World NPR Image Sample to read Position vector, got {pixel}"
assert abs(r) < 0.1, f"Expected low red from World NPR Position Image Sample, got {pixel}"
assert abs(g) < 0.1, f"Expected low green from World NPR Position Image Sample, got {pixel}"

world_output = use_directional_world_background()
bpy.context.scene.camera.data.type = "PERSP"
bpy.context.scene.camera.data.lens = 18.0

attach_combined_color_image_sample_world_npr(world_output, 0.0)
center_pixel = render_center_pixel()
print(f"WORLD_NPR_IMAGE_SAMPLE_COMBINED_CENTER={center_pixel}")

attach_combined_color_image_sample_world_npr(world_output, 24.0)
offset_pixel = render_center_pixel()
print(f"WORLD_NPR_IMAGE_SAMPLE_COMBINED_OFFSET={offset_pixel}")

assert abs(offset_pixel[0] - center_pixel[0]) > 0.05, (
    "Expected World NPR Image Sample Combined Color pixel offset to sample a different world "
    f"direction, got center={center_pixel}, offset={offset_pixel}"
)
