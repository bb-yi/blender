import bpy
import os
import tempfile


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def configure_scene():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 128
    scene.render.resolution_y = 128
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
    camera_data.ortho_scale = 2.0
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 4.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def make_plane(material):
    bpy.ops.mesh.primitive_plane_add(size=2.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.active_object
    plane.data.materials.append(material)
    return plane


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


def make_screen_derivative_material(operation, bias=1.0):
    material = bpy.data.materials.new(f"ScreenDerivative_{operation}")
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    output.location = (700.0, 0.0)

    emission = nodes.new("ShaderNodeEmission")
    emission.location = (500.0, 0.0)
    emission.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    emission.inputs["Strength"].default_value = 1.0

    bias_add = nodes.new("ShaderNodeMath")
    bias_add.location = (300.0, 0.0)
    bias_add.operation = "ADD"
    bias_add.inputs[1].default_value = bias

    derivative = nodes.new("ShaderNodeScreenDerivative")
    derivative.location = (80.0, 0.0)
    derivative.operation = operation
    derivative.data_type = "FLOAT"

    add_xy = nodes.new("ShaderNodeMath")
    add_xy.location = (-120.0, 0.0)
    add_xy.operation = "ADD"

    separate_xyz = nodes.new("ShaderNodeSeparateXYZ")
    separate_xyz.location = (-320.0, 0.0)

    geometry = nodes.new("ShaderNodeNewGeometry")
    geometry.location = (-520.0, 0.0)

    links.new(geometry.outputs["Position"], separate_xyz.inputs["Vector"])
    links.new(separate_xyz.outputs["X"], add_xy.inputs[0])
    links.new(separate_xyz.outputs["Y"], add_xy.inputs[1])
    links.new(add_xy.outputs["Value"], derivative.inputs["Value"])
    links.new(derivative.outputs["Value"], bias_add.inputs[0])
    links.new(bias_add.outputs["Value"], emission.inputs["Strength"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    return material


def measure_derivative(operation, bias=1.0):
    clear_scene()
    configure_scene()
    make_camera()
    make_plane(make_screen_derivative_material(operation, bias))

    pixel = render_center_pixel()
    return pixel[0] - bias


assert hasattr(bpy.types, "ShaderNodeScreenDerivative"), "ShaderNodeScreenDerivative is not registered"

bias = 1.0
ddx = measure_derivative("DDX", bias)
ddy = measure_derivative("DDY", bias)
ddxy = measure_derivative("DDXY", bias)

assert abs(ddx) > 1e-4, f"DDX should be non-zero for X + Y input, got {ddx}"
assert abs(ddy) > 1e-4, f"DDY should be non-zero for X + Y input, got {ddy}"
assert abs(ddxy - (ddx + ddy)) < 1e-4, f"DDXY should equal DDX + DDY, got {ddxy}, {ddx}, {ddy}"
