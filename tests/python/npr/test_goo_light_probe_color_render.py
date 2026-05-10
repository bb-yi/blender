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


def make_light_probe_color_material(
    output_name, use_custom_direction=False, direct_surface=False, roughness=0.0
):
    material = bpy.data.materials.new(f"LightProbeColor_{output_name}")
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    output.location = (320.0, 0.0)

    emission = nodes.new("ShaderNodeEmission")
    emission.location = (160.0, 0.0)
    emission.inputs["Strength"].default_value = 1.0

    probe_color = nodes.new("ShaderNodeLightProbeColor")
    probe_color.location = (0.0, 0.0)
    probe_color.inputs["Roughness"].default_value = roughness

    if use_custom_direction:
        direction = nodes.new("ShaderNodeCombineXYZ")
        direction.location = (-200.0, 0.0)
        direction.inputs["X"].default_value = 1.0
        direction.inputs["Y"].default_value = 0.5
        direction.inputs["Z"].default_value = 0.25

        links.new(direction.outputs["Vector"], probe_color.inputs["Direction"])

    if direct_surface:
        links.new(probe_color.outputs[output_name], output.inputs["Surface"])
    else:
        links.new(probe_color.outputs[output_name], emission.inputs["Color"])
        links.new(emission.outputs["Emission"], output.inputs["Surface"])

    return material


def make_light_probe_color_npr_material(output_name):
    material = bpy.data.materials.new(f"LightProbeColorNPR_{output_name}")
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (1.0, 0.0, 0.0, 1.0)
    emission.inputs["Strength"].default_value = 1.0
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    npr_tree = bpy.data.node_groups.new(f"LightProbeColorNPRTree_{output_name}", "ShaderNodeTree")
    npr_nodes = npr_tree.nodes
    npr_links = npr_tree.links

    probe_color = npr_nodes.new("ShaderNodeLightProbeColor")
    npr_output = npr_nodes.new("ShaderNodeNPR_Output")
    npr_links.new(probe_color.outputs[output_name], npr_output.inputs["Color"])

    output.nprtree = npr_tree
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


def assert_green_probe(pixel, label, minimum_green=0.2, max_other=0.15):
    r, g, b, _a = pixel
    assert g > minimum_green, f"Expected green {label} output from light probe, got {pixel}"
    assert r < max_other, f"Expected low red in {label} output from light probe, got {pixel}"
    assert b < max_other, f"Expected low blue in {label} output from light probe, got {pixel}"


assert hasattr(
    bpy.types, "ShaderNodeLightProbeColor"
), "ShaderNodeLightProbeColor is not registered"

clear_scene()
configure_scene()
make_camera()

back_plane = make_plane("BackPlane", -0.2)
back_plane.data.materials.append(make_emission_material("BackPlaneMaterial", (1.0, 0.0, 0.0, 1.0)))

front_plane = make_plane("FrontPlane", 0.0)

front_plane.data.materials.clear()
front_plane.data.materials.append(make_light_probe_color_material("Reflection"))
assert_green_probe(render_center_pixel(), "Reflection", minimum_green=0.8, max_other=0.1)

front_plane.data.materials.clear()
front_plane.data.materials.append(make_light_probe_color_material("Reflection", direct_surface=True))
assert_green_probe(render_center_pixel(), "Reflection direct to Surface", minimum_green=0.8, max_other=0.1)

front_plane.data.materials.clear()
front_plane.data.materials.append(make_light_probe_color_material("Irradiance"))
assert_green_probe(render_center_pixel(), "Irradiance")

front_plane.data.materials.clear()
front_plane.data.materials.append(make_light_probe_color_material("Combined"))
assert_green_probe(render_center_pixel(), "Combined", minimum_green=0.8, max_other=0.2)

front_plane.data.materials.clear()
front_plane.data.materials.append(
    make_light_probe_color_material("Reflection", use_custom_direction=True, roughness=1.0)
)
assert_green_probe(
    render_center_pixel(), "Reflection with custom direction and roughness", minimum_green=0.8, max_other=0.1
)

front_plane.data.materials.clear()
front_plane.data.materials.append(make_light_probe_color_npr_material("Reflection"))
assert_green_probe(render_center_pixel(), "Reflection in NPR Tree", minimum_green=0.8, max_other=0.1)
