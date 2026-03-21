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
    camera.location = (0.0, 0.0, 4.0)
    camera.rotation_euler = (0.0, 0.0, 0.0)
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 2.0
    return camera


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
    output.location = (300.0, 0.0)

    emission = nodes.new("ShaderNodeEmission")
    emission.location = (0.0, 0.0)
    emission.inputs["Color"].default_value = color
    emission.inputs["Strength"].default_value = 1.0

    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_screenspace_material(name, surface_render_method, explicit_view_position=None):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    material.surface_render_method = surface_render_method

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    output.location = (500.0, 0.0)

    emission = nodes.new("ShaderNodeEmission")
    emission.location = (260.0, 0.0)
    emission.inputs["Strength"].default_value = 1.0

    screenspace = nodes.new("ShaderNodeScreenspaceInfo")
    screenspace.location = (0.0, 0.0)

    if explicit_view_position is not None:
        combine_xyz = nodes.new("ShaderNodeCombineXYZ")
        combine_xyz.location = (-220.0, 0.0)
        combine_xyz.inputs["X"].default_value = explicit_view_position[0]
        combine_xyz.inputs["Y"].default_value = explicit_view_position[1]
        combine_xyz.inputs["Z"].default_value = explicit_view_position[2]
        links.new(combine_xyz.outputs["Vector"], screenspace.inputs["View Position"])

    links.new(screenspace.outputs["Scene Color"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    return material, screenspace, emission, output


def render_center_rgb():
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
        x = width // 2
        y = height // 2
        pixel_index = (y * width + x) * 4
        pixels = list(image.pixels[pixel_index:pixel_index + 4])
    finally:
        bpy.data.images.remove(image)
        if os.path.exists(filepath):
            os.remove(filepath)

    return pixels


def assert_is_red(pixel, label):
    r, g, b, _a = pixel
    assert r > 0.5, f"{label}: expected strong red, got {pixel}"
    assert g < 0.2, f"{label}: expected low green, got {pixel}"
    assert b < 0.2, f"{label}: expected low blue, got {pixel}"


def assert_is_non_black(pixel, label):
    value = max(pixel[:3])
    assert value > 0.05, f"{label}: expected non-black depth visualization, got {pixel}"


def run_case(label, explicit_view_position=None):
    clear_scene()
    make_camera()

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 128
    scene.render.resolution_y = 128
    scene.render.resolution_percentage = 100
    scene.eevee.taa_render_samples = 1
    scene.eevee.taa_samples = 1
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"

    back_plane = make_plane(f"Back_{label}", 0.0)
    back_plane.data.materials.append(
        make_emission_material(f"BackMaterial_{label}", (1.0, 0.0, 0.0, 1.0))
    )

    front_plane = make_plane(f"Front_{label}", 0.1)
    front_material, screenspace, emission, output = make_screenspace_material(
        f"FrontMaterial_{label}", "BLENDED", explicit_view_position
    )
    front_plane.data.materials.append(front_material)

    color_pixel = render_center_rgb()
    assert_is_red(color_pixel, f"{label} scene color")

    front_material.node_tree.links.clear()
    front_material.node_tree.links.new(screenspace.outputs["Scene Depth"], emission.inputs["Color"])
    front_material.node_tree.links.new(emission.outputs["Emission"], output.inputs["Surface"])

    depth_pixel = render_center_rgb()
    assert_is_non_black(depth_pixel, f"{label} scene depth")


assert hasattr(bpy.types, "ShaderNodeScreenspaceInfo"), "ShaderNodeScreenspaceInfo is not registered"

run_case("current_pixel")
run_case("explicit_view_position", (0.0, 0.0, -4.0))
