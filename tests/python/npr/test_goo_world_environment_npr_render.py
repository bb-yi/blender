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


def make_emission_material():
    material = bpy.data.materials.new("BaseEmission")
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (1.0, 0.0, 0.0, 1.0)
    emission.inputs["Strength"].default_value = 1.0
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    npr_tree = bpy.data.node_groups.new("WorldEnvironmentNPRTree", "ShaderNodeTree")
    npr_nodes = npr_tree.nodes
    npr_links = npr_tree.links

    world_environment = npr_nodes.new("ShaderNodeWorldEnvironment")
    npr_output = npr_nodes.new("ShaderNodeNPR_Output")
    npr_links.new(world_environment.outputs["Color"], npr_output.inputs["Color"])

    output.nprtree = npr_tree
    return material


def make_plane(material):
    bpy.ops.mesh.primitive_plane_add(size=2.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.active_object
    plane.data.materials.append(material)


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


clear_scene()
configure_scene()
make_camera()
make_plane(make_emission_material())

pixel = render_center_pixel()
r, g, b, _a = pixel

print(f"WORLD_ENVIRONMENT_NPR_CENTER={pixel}")

assert g > 0.8, f"Expected NPR Tree world environment output to sample green world color, got {pixel}"
assert r < 0.1, f"Expected NPR Tree world environment output to override red base emission, got {pixel}"
assert b < 0.1, f"Expected low blue in NPR Tree world environment sampling, got {pixel}"
