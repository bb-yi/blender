import bpy
import os
import tempfile


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def make_camera():
    camera_data = bpy.data.cameras.new("Camera")
    camera = bpy.data.objects.new("Camera", camera_data)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera
    camera.location = (0.0, -4.0, 1.5)
    camera.rotation_euler = (1.309, 0.0, 0.0)
    return camera


def make_light():
    light_data = bpy.data.lights.new("Sun", "SUN")
    light = bpy.data.objects.new("Sun", light_data)
    bpy.context.scene.collection.objects.link(light)
    light.rotation_euler = (0.8, 0.2, -0.6)
    return light


def make_material():
    material = bpy.data.materials.new("CurvatureObjectInfo")
    material.use_nodes = True
    material.surface_render_method = "DITHERED"

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    output.location = (700.0, 0.0)

    emission = nodes.new("ShaderNodeEmission")
    emission.location = (480.0, 0.0)
    emission.inputs["Strength"].default_value = 1.0

    object_info = nodes.new("ShaderNodeObjectInfo")
    object_info.location = (0.0, 60.0)

    curvature = nodes.new("ShaderNodeCurvature")
    curvature.location = (0.0, -140.0)
    curvature.local = True
    curvature.inputs["Samples"].default_value = 8.0
    curvature.inputs["Sample Radius"].default_value = 1.0
    curvature.inputs["Thickness"].default_value = 1.0

    add_strength = nodes.new("ShaderNodeMath")
    add_strength.location = (260.0, -120.0)
    add_strength.operation = "ADD"
    add_strength.inputs[0].default_value = 1.0

    links.new(object_info.outputs["Color"], emission.inputs["Color"])
    links.new(curvature.outputs["Scene Curvature"], add_strength.inputs[1])
    links.new(add_strength.outputs["Value"], emission.inputs["Strength"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    return material


def render_center_pixel():
    scene = bpy.context.scene
    fd, filepath = tempfile.mkstemp(suffix=".exr")
    os.close(fd)

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
        x = width // 2
        y = height // 2
        pixel_index = (y * width + x) * 4
        pixel = list(image.pixels[pixel_index:pixel_index + 4])
    finally:
        bpy.data.images.remove(image)
        if os.path.exists(filepath):
            os.remove(filepath)

    return pixel


clear_scene()
make_camera()
make_light()

scene = bpy.context.scene
scene.render.engine = "BLENDER_EEVEE"
scene.render.resolution_x = 128
scene.render.resolution_y = 128
scene.render.resolution_percentage = 100
scene.eevee.taa_render_samples = 1
scene.eevee.taa_samples = 1
scene.eevee.use_raytracing = False
scene.view_settings.view_transform = "Standard"
scene.view_settings.look = "None"

bpy.ops.mesh.primitive_monkey_add(location=(0.0, 0.0, 0.8))
obj = bpy.context.active_object
obj.color = (1.0, 0.2, 0.1, 1.0)
obj.data.materials.append(make_material())

bpy.ops.mesh.primitive_plane_add(size=6.0, location=(0.0, 0.0, 0.0))

pixel = render_center_pixel()
assert max(pixel[:3]) > 0.05, f"expected rendered output, got {pixel}"
assert pixel[0] > pixel[1] and pixel[0] > pixel[2], f"expected object info red tint, got {pixel}"
