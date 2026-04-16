import bpy
import os
import tempfile


RESOLUTION = 256
ORTHO_SCALE = 8.0


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def configure_scene():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = RESOLUTION
    scene.render.resolution_y = RESOLUTION
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
    camera_data.ortho_scale = ORTHO_SCALE
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 6.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera
    return camera


def make_shader_info_material(name, output_name, exponent=16.0):
    material = bpy.data.materials.new(name)
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    output.location = (420.0, 0.0)

    emission = nodes.new("ShaderNodeEmission")
    emission.location = (220.0, 0.0)
    emission.inputs["Strength"].default_value = 1.0

    shader_info = nodes.new("ShaderNodeShaderInfo")
    shader_info.location = (0.0, 0.0)

    exponent_value = nodes.new("ShaderNodeValue")
    exponent_value.location = (0.0, -140.0)
    exponent_value.outputs[0].default_value = exponent

    links.new(exponent_value.outputs[0], shader_info.inputs["Exponent"])
    links.new(shader_info.outputs[output_name], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_shader_info_material_with_normal(name, output_name, normal_xyz, exponent=16.0):
    material = bpy.data.materials.new(name)
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    output.location = (620.0, 0.0)

    emission = nodes.new("ShaderNodeEmission")
    emission.location = (420.0, 0.0)
    emission.inputs["Strength"].default_value = 1.0

    shader_info = nodes.new("ShaderNodeShaderInfo")
    shader_info.location = (200.0, 0.0)

    normal_value = nodes.new("ShaderNodeCombineXYZ")
    normal_value.location = (0.0, -120.0)
    normal_value.inputs["X"].default_value = normal_xyz[0]
    normal_value.inputs["Y"].default_value = normal_xyz[1]
    normal_value.inputs["Z"].default_value = normal_xyz[2]

    exponent_value = nodes.new("ShaderNodeValue")
    exponent_value.location = (0.0, -260.0)
    exponent_value.outputs[0].default_value = exponent

    links.new(normal_value.outputs["Vector"], shader_info.inputs["Normal"])
    links.new(exponent_value.outputs[0], shader_info.inputs["Exponent"])
    links.new(shader_info.outputs[output_name], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_plane(material):
    bpy.ops.mesh.primitive_plane_add(size=8.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.active_object
    plane.name = "ShaderInfoPlane"
    plane.data.materials.append(material)
    return plane


def make_sphere(material):
    bpy.ops.mesh.primitive_uv_sphere_add(
        radius=1.5,
        location=(0.0, 0.0, 0.0),
        segments=128,
        ring_count=64,
    )
    sphere = bpy.context.active_object
    sphere.name = "ShaderInfoSphere"
    sphere.data.materials.append(material)
    return sphere


def make_blocker():
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, 1.0))
    blocker = bpy.context.active_object
    blocker.name = "ShaderInfoBlocker"
    blocker.scale = (0.5, 0.5, 1.0)
    return blocker


def make_light():
    light_data = bpy.data.lights.new("ShaderInfoPoint", type="POINT")
    light_data.energy = 5000.0
    light_data.shadow_soft_size = 0.05
    light_data.use_shadow = True
    light = bpy.data.objects.new("ShaderInfoPoint", light_data)
    light.location = (-4.0, 0.0, 4.0)
    bpy.context.scene.collection.objects.link(light)
    return light


def make_centered_point_light():
    light_data = bpy.data.lights.new("ShaderInfoPointCentered", type="POINT")
    light_data.energy = 5000.0
    light_data.shadow_soft_size = 0.05
    light_data.use_shadow = True
    light = bpy.data.objects.new("ShaderInfoPointCentered", light_data)
    light.location = (-4.0, 0.0, 4.0)
    bpy.context.scene.collection.objects.link(light)
    return light


def make_sun_light():
    light_data = bpy.data.lights.new("ShaderInfoSun", type="SUN")
    light_data.energy = 5.0
    light_data.use_shadow = True
    light = bpy.data.objects.new("ShaderInfoSun", light_data)
    light.rotation_euler = (0.0, 0.78539816339, 0.0)
    bpy.context.scene.collection.objects.link(light)
    return light


def configure_world_sky():
    world = bpy.context.scene.world
    world.use_nodes = True
    world.sun_threshold = 0.01

    nodes = world.node_tree.nodes
    links = world.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputWorld")
    output.location = (420.0, 0.0)

    background = nodes.new("ShaderNodeBackground")
    background.location = (220.0, 0.0)
    background.inputs["Strength"].default_value = 10.0

    sky = nodes.new("ShaderNodeTexSky")
    sky.location = (0.0, 0.0)
    if hasattr(sky, "sky_type"):
        sky.sky_type = "PREETHAM"
    if hasattr(sky, "sun_elevation"):
        sky.sun_elevation = 0.9
    if hasattr(sky, "sun_rotation"):
        sky.sun_rotation = 0.0

    links.new(sky.outputs["Color"], background.inputs["Color"])
    links.new(background.outputs["Background"], output.inputs["Surface"])


def render_image():
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
        pixels = list(image.pixels[:])
        width = image.size[0]
        height = image.size[1]
    finally:
        bpy.data.images.remove(image)
        if os.path.exists(filepath):
            os.remove(filepath)

    return pixels, width, height


def sample_world_point(pixels, width, height, world_x, world_y):
    x_ratio = (world_x / ORTHO_SCALE) + 0.5
    y_ratio = (world_y / ORTHO_SCALE) + 0.5
    x = min(width - 1, max(0, int(width * x_ratio)))
    y = min(height - 1, max(0, int(height * y_ratio)))
    index = (y * width + x) * 4
    return list(pixels[index:index + 4])


def build_scene(output_name, with_light=True, with_blocker=False, world_mode="black"):
    clear_scene()
    configure_scene()
    if world_mode == "sky":
        configure_world_sky()
    make_camera()
    make_plane(make_shader_info_material(f"{output_name}Material", output_name))
    if with_blocker:
        make_blocker()
    if with_light:
        make_light()


def assert_diffuse_response():
    build_scene("Diffuse Shading", with_light=True, with_blocker=False)
    lit_pixels, lit_width, lit_height = render_image()
    lit = sample_world_point(lit_pixels, lit_width, lit_height, 0.0, 0.0)

    build_scene("Diffuse Shading", with_light=False, with_blocker=False)
    dark_pixels, dark_width, dark_height = render_image()
    unlit = sample_world_point(dark_pixels, dark_width, dark_height, 0.0, 0.0)

    assert lit[0] > unlit[0] + 0.2, f"Diffuse shading should react to light, got lit={lit} unlit={unlit}"
    assert lit[0] > 0.2, f"Diffuse shading should not stay black under direct light, got {lit}"


def assert_unshadowed_response(output_name, min_value=0.2, tolerance=0.05):
    build_scene(output_name, with_light=True, with_blocker=True)
    blocked_pixels, blocked_width, blocked_height = render_image()
    blocked = sample_world_point(blocked_pixels, blocked_width, blocked_height, 1.4, 0.0)

    build_scene(output_name, with_light=True, with_blocker=False)
    clear_pixels, clear_width, clear_height = render_image()
    unblocked = sample_world_point(clear_pixels, clear_width, clear_height, 1.4, 0.0)

    assert unblocked[0] > min_value, (
        f"{output_name} should produce visible lighting without the blocker, got {unblocked}"
    )
    assert blocked[0] > min_value, (
        f"{output_name} should remain lit even in the blocker region, got {blocked}"
    )
    assert abs(blocked[0] - unblocked[0]) < tolerance, (
        f"{output_name} should ignore shadowing, got blocked={blocked} unblocked={unblocked}"
    )


def assert_shadow_response(output_name):
    build_scene(output_name, with_light=True, with_blocker=True)
    pixels, width, height = render_image()
    shadowed = sample_world_point(pixels, width, height, 1.4, 0.0)
    lit = sample_world_point(pixels, width, height, -1.4, 0.0)

    build_scene(output_name, with_light=True, with_blocker=False)
    clear_pixels, clear_width, clear_height = render_image()
    unblocked = sample_world_point(clear_pixels, clear_width, clear_height, 1.4, 0.0)

    assert lit[0] > 0.9, f"{output_name} should stay bright on lit region, got {lit}"
    assert unblocked[0] > 0.9, f"{output_name} should stay bright without blocker, got {unblocked}"
    assert shadowed[0] < lit[0] - 0.35, (
        f"{output_name} should darken in shadow, got shadowed={shadowed} lit={lit}"
    )
    assert shadowed[0] < unblocked[0] - 0.35, (
        f"{output_name} should darken with blocker, got shadowed={shadowed} unblocked={unblocked}"
    )


def assert_no_light_black(output_name):
    build_scene(output_name, with_light=False, with_blocker=False)
    pixels, width, height = render_image()
    center = sample_world_point(pixels, width, height, 0.0, 0.0)
    assert center[0] < 0.05, f"{output_name} should be black without scene lights, got {center}"


def assert_world_sun_black(output_name):
    build_scene(output_name, with_light=False, with_blocker=False, world_mode="sky")
    pixels, width, height = render_image()
    center = sample_world_point(pixels, width, height, 0.0, 0.0)
    assert center[0] < 0.05, (
        f"{output_name} should ignore Eevee world sun extraction, got {center}"
    )


def assert_half_lambert_lifts_negative_ndotl():
    clear_scene()
    configure_scene()
    make_camera()
    make_plane(
        make_shader_info_material_with_normal(
            "HalfLambertNegativeNdotLMaterial",
            "Half-Lambert Factor",
            (1.0, 0.0, 0.0),
        )
    )
    make_centered_point_light()
    half_pixels, half_width, half_height = render_image()
    half_value = sample_world_point(half_pixels, half_width, half_height, 0.0, 0.0)

    clear_scene()
    configure_scene()
    make_camera()
    make_plane(
        make_shader_info_material_with_normal(
            "DiffuseNegativeNdotLMaterial",
            "Diffuse Shading",
            (1.0, 0.0, 0.0),
        )
    )
    make_centered_point_light()
    diffuse_pixels, diffuse_width, diffuse_height = render_image()
    diffuse_value = sample_world_point(diffuse_pixels, diffuse_width, diffuse_height, 0.0, 0.0)

    assert half_value[0] > 0.1, (
        f"Half-Lambert should stay above black for negative NdotL, got {half_value}"
    )
    assert diffuse_value[0] < 0.05, (
        f"Diffuse Shading should stay near black for negative NdotL, got {diffuse_value}"
    )


def assert_half_lambert_gradient_on_sphere():
    clear_scene()
    configure_scene()
    make_camera()
    make_sphere(make_shader_info_material("HalfLambertSphereMaterial", "Half-Lambert Factor"))
    make_light()

    pixels, width, height = render_image()
    bright_side = sample_world_point(pixels, width, height, -1.0, 0.0)
    center = sample_world_point(pixels, width, height, 0.0, 0.0)
    dark_side = sample_world_point(pixels, width, height, 1.0, 0.0)

    assert bright_side[0] > center[0] > dark_side[0], (
        "Half-Lambert should form a smooth light-to-dark gradient across the sphere, "
        f"got bright={bright_side} center={center} dark={dark_side}"
    )
    assert dark_side[0] > 0.1, (
        f"Half-Lambert dark side should stay above black unless fully opposite, got {dark_side}"
    )


def assert_half_lambert_point_gradient_on_plane():
    clear_scene()
    configure_scene()
    make_camera()
    make_plane(make_shader_info_material("HalfLambertPlaneMaterial", "Half-Lambert Factor"))
    make_light()

    pixels, width, height = render_image()
    bright_side = sample_world_point(pixels, width, height, -2.0, 0.0)
    upper_mid = sample_world_point(pixels, width, height, -1.0, 0.0)
    center = sample_world_point(pixels, width, height, 0.0, 0.0)
    lower_mid = sample_world_point(pixels, width, height, 1.0, 0.0)
    dark_side = sample_world_point(pixels, width, height, 2.0, 0.0)

    assert dark_side[0] < lower_mid[0] < center[0] < upper_mid[0] < bright_side[0], (
        "Half-Lambert Factor under a single point light should stay as a smooth local-light "
        f"falloff, got dark={dark_side} lower_mid={lower_mid} center={center} "
        f"upper_mid={upper_mid} bright={bright_side}"
    )


def assert_diffuse_sun_gradient_on_sphere():
    clear_scene()
    configure_scene()
    make_camera()
    make_sphere(make_shader_info_material("DiffuseSunSphereMaterial", "Diffuse Shading"))
    make_sun_light()

    pixels, width, height = render_image()
    dark_side = sample_world_point(pixels, width, height, -1.0, 0.0)
    lower_mid = sample_world_point(pixels, width, height, -0.5, 0.0)
    center = sample_world_point(pixels, width, height, 0.0, 0.0)
    upper_mid = sample_world_point(pixels, width, height, 0.5, 0.0)
    bright_side = sample_world_point(pixels, width, height, 1.0, 0.0)

    assert 0.0 <= dark_side[0] < lower_mid[0] < center[0] < upper_mid[0] < bright_side[0], (
        "Diffuse Shading under a single Sun light should stay as a smooth Goo-style gradient, "
        f"got dark={dark_side} lower_mid={lower_mid} center={center} "
        f"upper_mid={upper_mid} bright={bright_side}"
    )


def assert_diffuse_point_gradient_on_plane():
    clear_scene()
    configure_scene()
    make_camera()
    make_plane(make_shader_info_material("DiffusePointPlaneMaterial", "Diffuse Shading"))
    make_light()

    pixels, width, height = render_image()
    bright_side = sample_world_point(pixels, width, height, -2.0, 0.0)
    upper_mid = sample_world_point(pixels, width, height, -1.0, 0.0)
    center = sample_world_point(pixels, width, height, 0.0, 0.0)
    lower_mid = sample_world_point(pixels, width, height, 1.0, 0.0)
    dark_side = sample_world_point(pixels, width, height, 2.0, 0.0)

    assert dark_side[0] < lower_mid[0] < center[0] < upper_mid[0] < bright_side[0], (
        "Diffuse Shading under a single point light should stay as a smooth local-light falloff, "
        f"got dark={dark_side} lower_mid={lower_mid} center={center} "
        f"upper_mid={upper_mid} bright={bright_side}"
    )


def assert_blinn_phong_gradient_on_sphere():
    clear_scene()
    configure_scene()
    make_camera()
    make_sphere(make_shader_info_material("BlinnPhongSphereMaterial", "Blinn-Phong Factor"))
    make_light()

    pixels, width, height = render_image()
    bright_side = sample_world_point(pixels, width, height, -1.0, 0.0)
    dark_side = sample_world_point(pixels, width, height, 1.0, 0.0)

    assert bright_side[0] > dark_side[0] + 0.1, (
        "Blinn-Phong Factor should concentrate toward the lit side of the sphere, "
        f"got bright={bright_side} dark={dark_side}"
    )


def assert_blinn_phong_point_gradient_on_plane():
    clear_scene()
    configure_scene()
    make_camera()
    make_plane(make_shader_info_material("BlinnPhongPlaneMaterial", "Blinn-Phong Factor"))
    make_light()

    pixels, width, height = render_image()
    bright_side = sample_world_point(pixels, width, height, -2.0, 0.0)
    upper_mid = sample_world_point(pixels, width, height, -1.0, 0.0)
    center = sample_world_point(pixels, width, height, 0.0, 0.0)
    lower_mid = sample_world_point(pixels, width, height, 1.0, 0.0)
    dark_side = sample_world_point(pixels, width, height, 2.0, 0.0)

    assert dark_side[0] < lower_mid[0] < center[0] < upper_mid[0] < bright_side[0], (
        "Blinn-Phong Factor under a single point light should stay as a smooth local-light "
        f"falloff, got dark={dark_side} lower_mid={lower_mid} center={center} "
        f"upper_mid={upper_mid} bright={bright_side}"
    )


assert hasattr(bpy.types, "ShaderNodeShaderInfo"), "ShaderNodeShaderInfo is not registered"

assert_diffuse_response()
assert_diffuse_sun_gradient_on_sphere()
assert_diffuse_point_gradient_on_plane()
assert_unshadowed_response("Diffuse Shading", min_value=0.2, tolerance=0.05)
assert_no_light_black("Diffuse Shading")
assert_world_sun_black("Diffuse Shading")
assert_unshadowed_response("Half-Lambert Factor", min_value=0.2, tolerance=0.05)
assert_half_lambert_lifts_negative_ndotl()
assert_half_lambert_gradient_on_sphere()
assert_half_lambert_point_gradient_on_plane()
assert_unshadowed_response("Blinn-Phong Factor", min_value=0.05, tolerance=0.08)
assert_blinn_phong_gradient_on_sphere()
assert_blinn_phong_point_gradient_on_plane()
assert_shadow_response("Shadow")
assert_no_light_black("Blinn-Phong Factor")
assert_no_light_black("Shadow")
assert_no_light_black("Half-Lambert Factor")
assert_world_sun_black("Blinn-Phong Factor")
assert_world_sun_black("Shadow")
assert_world_sun_black("Half-Lambert Factor")
