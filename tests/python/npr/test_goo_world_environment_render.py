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
    scene.world.use_nodes = True
    nodes = scene.world.node_tree.nodes
    links = scene.world.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputWorld")
    background = nodes.new("ShaderNodeBackground")
    background.inputs["Color"].default_value = (0.0, 1.0, 0.0, 1.0)
    background.inputs["Strength"].default_value = 1.0

    links.new(background.outputs["Background"], output.inputs["Surface"])


def make_camera():
    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 2.0
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 4.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def make_plane(name, z_location):
    bpy.ops.mesh.primitive_plane_add(size=2.0, location=(0.0, 0.0, z_location))
    obj = bpy.context.active_object
    obj.name = name
    return obj


def make_emission_material(name, color):
    material = bpy.data.materials.new(name)
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = color
    emission.inputs["Strength"].default_value = 1.0

    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_world_environment_material(use_custom_direction=False):
    material = bpy.data.materials.new("WorldEnvironmentRender")
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    output.location = (320.0, 0.0)

    emission = nodes.new("ShaderNodeEmission")
    emission.location = (160.0, 0.0)
    emission.inputs["Strength"].default_value = 1.0

    world_environment = nodes.new("ShaderNodeWorldEnvironment")
    world_environment.location = (0.0, 0.0)

    if use_custom_direction:
        direction = nodes.new("ShaderNodeCombineXYZ")
        direction.location = (-200.0, 0.0)
        direction.inputs["X"].default_value = 1.0
        direction.inputs["Y"].default_value = 0.5
        direction.inputs["Z"].default_value = 0.25

        links.new(direction.outputs["Vector"], world_environment.inputs["Direction"])

    links.new(world_environment.outputs["Color"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    return material


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


assert hasattr(
    bpy.types, "ShaderNodeWorldEnvironment"
), "ShaderNodeWorldEnvironment is not registered"

clear_scene()
configure_scene()
make_camera()

back_plane = make_plane("BackPlane", -0.2)
back_plane.data.materials.append(make_emission_material("BackPlaneMaterial", (1.0, 0.0, 0.0, 1.0)))

front_plane = make_plane("FrontPlane", 0.0)
front_plane.data.materials.append(make_world_environment_material())

pixel = render_center_pixel()
r, g, b, _a = pixel

assert g > 0.8, f"Expected strong green world color from world environment, got {pixel}"
assert r < 0.1, f"Expected world environment to ignore occluding red plane, got {pixel}"
assert b < 0.1, f"Expected low blue in sampled world environment, got {pixel}"

front_plane.data.materials.clear()
front_plane.data.materials.append(make_world_environment_material(use_custom_direction=True))

pixel = render_center_pixel()
r, g, b, _a = pixel

assert g > 0.8, f"Expected strong green world color from custom world environment sampling, got {pixel}"
assert r < 0.1, f"Expected custom world environment sampling to ignore occluding red plane, got {pixel}"
assert b < 0.1, f"Expected low blue in custom sampled world environment, got {pixel}"
