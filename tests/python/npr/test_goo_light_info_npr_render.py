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
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 2.0
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 4.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def make_light():
    light_data = bpy.data.lights.new("LightInfoNPR", type="POINT")
    light_data.color = (0.2, 0.6, 0.9)
    light_data.energy = 3.0
    light_object = bpy.data.objects.new("LightInfoNPR", light_data)
    light_object.location = (0.0, 0.0, 2.0)
    bpy.context.scene.collection.objects.link(light_object)
    return light_object


def make_material(light_object):
    material = bpy.data.materials.new("LightInfoNPRMaterial")
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    output.location = (420.0, 0.0)

    emission = nodes.new("ShaderNodeEmission")
    emission.location = (180.0, 0.0)
    emission.inputs["Color"].default_value = (1.0, 0.0, 0.0, 1.0)
    emission.inputs["Strength"].default_value = 1.0
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    npr_tree = bpy.data.node_groups.new("LightInfoNPRTree", "ShaderNodeTree")
    npr_nodes = npr_tree.nodes
    npr_links = npr_tree.links

    light_info = npr_nodes.new("ShaderNodeLightInfo")
    light_info.light_object = light_object

    npr_output = npr_nodes.new("ShaderNodeNPR_Output")
    npr_links.new(light_info.outputs["Color"], npr_output.inputs["Color"])

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

    try:
        scene.render.filepath = filepath
        scene.render.image_settings.file_format = "OPEN_EXR"
        scene.render.image_settings.color_mode = "RGBA"
        scene.render.image_settings.color_depth = "32"
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
    finally:
        if os.path.exists(filepath):
            os.remove(filepath)

    return pixel


clear_scene()
configure_scene()
make_camera()
light_object = make_light()
make_plane(make_material(light_object))

pixel = render_center_pixel()
r, g, b, _a = pixel

print(f"LIGHT_INFO_NPR_CENTER={pixel}")

assert abs(r - 0.2) < 0.08, f"Expected NPR Light Info red output near 0.2, got {pixel}"
assert abs(g - 0.6) < 0.08, f"Expected NPR Light Info green output near 0.6, got {pixel}"
assert abs(b - 0.9) < 0.08, f"Expected NPR Light Info blue output near 0.9, got {pixel}"
