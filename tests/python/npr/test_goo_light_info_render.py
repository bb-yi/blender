import bpy
import os
import tempfile

from mathutils import Vector


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


def make_light(name, light_type="POINT", color=(1.0, 1.0, 1.0), energy=1.0, location=(0.0, 0.0, 2.0)):
    light_data = bpy.data.lights.new(name, type=light_type)
    light_data.color = color
    light_data.energy = energy
    light_object = bpy.data.objects.new(name, light_data)
    light_object.location = location
    bpy.context.scene.collection.objects.link(light_object)
    return light_object


def orient_light(light_object, direction):
    light_object.rotation_euler = direction.normalized().to_track_quat("-Z", "Y").to_euler()


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


def make_light_info_material(light_object=None, output_name="Color", scalar_bias=0.0):
    material = bpy.data.materials.new(f"LightInfo_{output_name}")
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    output.location = (520.0, 0.0)

    emission = nodes.new("ShaderNodeEmission")
    emission.location = (320.0, 0.0)
    emission.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    emission.inputs["Strength"].default_value = 1.0

    light_info = nodes.new("ShaderNodeLightInfo")
    light_info.location = (0.0, 0.0)
    if light_object is not None:
        light_info.light_object = light_object

    if output_name in {"Color", "Position", "Direction"}:
        links.new(light_info.outputs[output_name], emission.inputs["Color"])
    else:
        strength_source = light_info.outputs[output_name]
        if abs(scalar_bias) > 1e-6:
            bias = nodes.new("ShaderNodeMath")
            bias.location = (180.0, -120.0)
            bias.operation = "ADD"
            bias.inputs[1].default_value = scalar_bias
            links.new(strength_source, bias.inputs[0])
            strength_source = bias.outputs[0]
        links.new(strength_source, emission.inputs["Strength"])

    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def assert_rgb_close(pixel, expected, label, tolerance=0.05):
    assert abs(pixel[0] - expected[0]) < tolerance, f"{label} R mismatch: {pixel} vs {expected}"
    assert abs(pixel[1] - expected[1]) < tolerance, f"{label} G mismatch: {pixel} vs {expected}"
    assert abs(pixel[2] - expected[2]) < tolerance, f"{label} B mismatch: {pixel} vs {expected}"


def assert_scalar_close(pixel, expected, label, tolerance=0.05):
    assert abs(pixel[0] - expected) < tolerance, f"{label} R mismatch: {pixel} vs {expected}"
    assert abs(pixel[1] - expected) < tolerance, f"{label} G mismatch: {pixel} vs {expected}"
    assert abs(pixel[2] - expected) < tolerance, f"{label} B mismatch: {pixel} vs {expected}"


def assert_color_output():
    clear_scene()
    configure_scene()
    make_camera()
    light_object = make_light("LightInfoColor", color=(0.2, 0.6, 0.9), energy=3.0)
    make_plane(make_light_info_material(light_object, "Color"))

    pixel = render_center_pixel()
    assert_rgb_close(pixel, (0.2, 0.6, 0.9), "Color output")


def assert_power_output():
    clear_scene()
    configure_scene()
    make_camera()
    light_object = make_light("LightInfoPower", color=(1.0, 1.0, 1.0), energy=7.0)
    make_plane(make_light_info_material(light_object, "Power"))

    pixel = render_center_pixel()

    assert pixel[0] > 6.8, f"Power output should drive emission strength, got {pixel}"
    assert pixel[1] > 6.8, f"Power output should drive emission strength, got {pixel}"
    assert pixel[2] > 6.8, f"Power output should drive emission strength, got {pixel}"


def assert_position_output():
    clear_scene()
    configure_scene()
    make_camera()
    light_object = make_light("LightInfoPosition", location=(0.2, 0.45, 0.7))
    make_plane(make_light_info_material(light_object, "Position"))

    pixel = render_center_pixel()
    assert_rgb_close(pixel, tuple(light_object.location), "Position output")


def assert_direction_output():
    clear_scene()
    configure_scene()
    make_camera()
    light_object = make_light("LightInfoDirection", light_type="AREA")
    orient_light(light_object, Vector((1.0, 0.0, 0.0)))
    make_plane(make_light_info_material(light_object, "Direction"))

    pixel = render_center_pixel()
    assert_rgb_close(pixel, (1.0, 0.0, 0.0), "Direction output")


def assert_radius_output():
    clear_scene()
    configure_scene()
    make_camera()
    light_object = make_light("LightInfoRadius")
    light_object.data.shadow_soft_size = 0.35
    make_plane(make_light_info_material(light_object, "Radius"))

    pixel = render_center_pixel()
    assert pixel[0] > 0.33, f"Radius output mismatch: {pixel}"


def assert_area_radius_output():
    clear_scene()
    configure_scene()
    make_camera()
    light_object = make_light("LightInfoAreaRadius", light_type="AREA")
    light_object.data.shape = "RECTANGLE"
    light_object.data.size = 0.4
    light_object.data.size_y = 0.8
    make_plane(make_light_info_material(light_object, "Radius"))

    pixel = render_center_pixel()
    assert pixel[0] > 0.38, f"Area radius output mismatch: {pixel}"


def assert_spot_size_output():
    clear_scene()
    configure_scene()
    make_camera()
    light_object = make_light("LightInfoSpotSize", light_type="SPOT")
    light_object.data.spot_size = 0.9
    make_plane(make_light_info_material(light_object, "Spot Size"))

    pixel = render_center_pixel()
    assert pixel[0] > 0.85, f"Spot Size output mismatch: {pixel}"


def assert_sun_angle_output():
    clear_scene()
    configure_scene()
    make_camera()
    light_object = make_light("LightInfoSunAngle", light_type="SUN")
    light_object.data.angle = 0.2
    make_plane(make_light_info_material(light_object, "Sun Angle"))

    pixel = render_center_pixel()
    assert pixel[0] > 0.18, f"Sun Angle output mismatch: {pixel}"


def assert_type_output_mapping():
    for light_type, expected_type in (
        ("POINT", 0.0),
        ("SUN", 1.0),
        ("SPOT", 2.0),
        ("AREA", 3.0),
    ):
        clear_scene()
        configure_scene()
        make_camera()
        light_object = make_light(f"LightInfoType{light_type}", light_type=light_type)
        make_plane(make_light_info_material(light_object, "Type", scalar_bias=1.0))

        pixel = render_center_pixel()
        assert_scalar_close(pixel, expected_type + 1.0, f"Type output for {light_type}")

    clear_scene()
    configure_scene()
    make_camera()
    make_plane(make_light_info_material(None, "Type", scalar_bias=1.0))

    pixel = render_center_pixel()
    assert_scalar_close(pixel, 0.0, "Type output for unassigned light")


def assert_visible_output():
    clear_scene()
    configure_scene()
    make_camera()
    light_object = make_light("LightInfoVisible")
    make_plane(make_light_info_material(light_object, "Visible"))

    pixel = render_center_pixel()
    assert_scalar_close(pixel, 1.0, "Visible output for visible light")

    light_object.hide_viewport = True
    pixel = render_center_pixel()
    assert_scalar_close(pixel, 0.0, "Visible output for hidden viewport light")

    light_object.hide_viewport = False
    light_object.hide_render = True
    pixel = render_center_pixel()
    assert_scalar_close(pixel, 0.0, "Visible output for hidden render light")

    clear_scene()
    configure_scene()
    make_camera()
    make_plane(make_light_info_material(None, "Visible"))

    pixel = render_center_pixel()
    assert_scalar_close(pixel, 0.0, "Visible output for unassigned light")


def assert_unassigned_light_is_black():
    clear_scene()
    configure_scene()
    make_camera()
    make_plane(make_light_info_material(None, "Color"))

    pixel = render_center_pixel()
    assert max(pixel[:3]) < 0.01, f"Unassigned Light Info color should be black, got {pixel}"


def assert_light_color_updates_without_relink():
    clear_scene()
    configure_scene()
    make_camera()
    light_object = make_light("LightInfoLiveColor", color=(0.1, 0.2, 0.3))
    make_plane(make_light_info_material(light_object, "Color"))

    pixel_before = render_center_pixel()

    light_object.data.color = (0.7, 0.15, 0.45)
    pixel_after = render_center_pixel()

    assert_rgb_close(pixel_before, (0.1, 0.2, 0.3), "Initial color output")
    assert_rgb_close(pixel_after, (0.7, 0.15, 0.45), "Updated color output")


def assert_light_power_updates_without_relink():
    clear_scene()
    configure_scene()
    make_camera()
    light_object = make_light("LightInfoLivePower", energy=2.0)
    make_plane(make_light_info_material(light_object, "Power"))

    pixel_before = render_center_pixel()

    light_object.data.energy = 6.0
    pixel_after = render_center_pixel()

    assert pixel_before[0] > 1.8, f"Initial power output mismatch: {pixel_before}"
    assert pixel_after[0] > 5.8, f"Updated power output mismatch: {pixel_after}"


def assert_light_position_updates_without_relink():
    clear_scene()
    configure_scene()
    make_camera()
    light_object = make_light("LightInfoLivePosition", location=(0.1, 0.2, 0.3))
    make_plane(make_light_info_material(light_object, "Position"))

    pixel_before = render_center_pixel()

    light_object.location = (0.65, 0.15, 0.45)
    pixel_after = render_center_pixel()

    assert_rgb_close(pixel_before, (0.1, 0.2, 0.3), "Initial position output")
    assert_rgb_close(pixel_after, (0.65, 0.15, 0.45), "Updated position output")


def assert_light_direction_updates_without_relink():
    clear_scene()
    configure_scene()
    make_camera()
    light_object = make_light("LightInfoLiveDirection", light_type="AREA")
    orient_light(light_object, Vector((1.0, 0.0, 0.0)))
    make_plane(make_light_info_material(light_object, "Direction"))

    pixel_before = render_center_pixel()

    orient_light(light_object, Vector((0.0, 1.0, 0.0)))
    pixel_after = render_center_pixel()

    assert_rgb_close(pixel_before, (1.0, 0.0, 0.0), "Initial direction output")
    assert_rgb_close(pixel_after, (0.0, 1.0, 0.0), "Updated direction output")


def assert_light_radius_updates_without_relink_from_scale():
    clear_scene()
    configure_scene()
    make_camera()
    light_object = make_light("LightInfoLiveRadius")
    light_object.data.shadow_soft_size = 0.25
    make_plane(make_light_info_material(light_object, "Radius"))

    pixel_before = render_center_pixel()

    light_object.scale = (3.0, 3.0, 3.0)
    pixel_after = render_center_pixel()

    assert pixel_before[0] > 0.23, f"Initial radius output mismatch: {pixel_before}"
    assert pixel_after[0] > 0.73, f"Updated radius output mismatch: {pixel_after}"


def assert_light_type_updates_without_relink():
    clear_scene()
    configure_scene()
    make_camera()
    light_object = make_light("LightInfoLiveType", light_type="SPOT")
    make_plane(make_light_info_material(light_object, "Type"))

    pixel_before = render_center_pixel()

    light_object.data.type = "SUN"
    pixel_after = render_center_pixel()

    assert pixel_before[0] > 1.8, f"Initial type output mismatch: {pixel_before}"
    assert 0.8 < pixel_after[0] < 1.2, f"Updated type output mismatch: {pixel_after}"


assert_color_output()
assert_power_output()
assert_position_output()
assert_direction_output()
assert_radius_output()
assert_area_radius_output()
assert_spot_size_output()
assert_sun_angle_output()
assert_type_output_mapping()
assert_visible_output()
assert_unassigned_light_is_black()
assert_light_color_updates_without_relink()
assert_light_power_updates_without_relink()
assert_light_position_updates_without_relink()
assert_light_direction_updates_without_relink()
assert_light_radius_updates_without_relink_from_scale()
assert_light_type_updates_without_relink()
