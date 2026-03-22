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
    scene.world.use_nodes = False
    scene.world.color = (0.0, 0.0, 0.0)


def make_camera():
    camera_data = bpy.data.cameras.new("Camera")
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 5.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def make_inner_group():
    group = bpy.data.node_groups.new("NPRInnerGroup", "ShaderNodeTree")
    nodes = group.nodes
    links = group.links

    nodes.new("NodeGroupInput")
    group_output = nodes.new("NodeGroupOutput")
    group.interface.new_socket(name="Color", in_out="OUTPUT", socket_type="NodeSocketColor")

    rgb = nodes.new("ShaderNodeRGB")
    rgb.outputs["Color"].default_value = (0.25, 0.7, 0.35, 1.0)
    links.new(rgb.outputs["Color"], group_output.inputs["Color"])
    return group


def make_npr_tree():
    npr_tree = bpy.data.node_groups.new("NPRTree", "ShaderNodeTree")
    nodes = npr_tree.nodes
    links = npr_tree.links

    group = nodes.new("ShaderNodeGroup")
    group.node_tree = make_inner_group()

    npr_output = nodes.new("ShaderNodeNPR_Output")
    links.new(group.outputs["Color"], npr_output.inputs["Color"])
    return npr_tree


def make_material():
    material = bpy.data.materials.new("NPRGroupMaterial")
    material.use_nodes = True
    nodes = material.node_tree.nodes

    output = next(node for node in nodes if node.bl_idname == "ShaderNodeOutputMaterial")
    output.nprtree = make_npr_tree()
    return material


def make_plane(material):
    bpy.ops.mesh.primitive_plane_add(size=2.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.active_object
    plane.data.materials.append(material)


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


clear_scene()
configure_scene()
make_camera()
make_plane(make_material())
pixels, width, height = render_image()
center = sample_center(pixels, width, height)
assert center[1] > 0.4, f"NPR group output should affect render color, got center={center}"

print("PASS")
