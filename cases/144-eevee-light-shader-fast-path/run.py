from pathlib import Path
import math

import bpy
import gpu
from mathutils import Vector


CASE_DIR = Path(__file__).resolve().parent
OUT_DIR = CASE_DIR / "out"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()
    for datablock in (
        bpy.data.meshes,
        bpy.data.materials,
        bpy.data.images,
        bpy.data.lights,
        bpy.data.cameras,
        bpy.data.collections,
        bpy.data.worlds,
    ):
        for item in list(datablock):
            if item.users == 0:
                datablock.remove(item)


def look_at(obj, target):
    direction = Vector(target) - obj.location
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def setup_render(resolution=96, volume=False):
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = resolution
    scene.render.resolution_y = resolution
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = False
    scene.render.image_settings.file_format = "PNG"
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0

    eevee = scene.eevee
    for attr, value in (
        ("taa_render_samples", 1),
        ("taa_samples", 1),
        ("use_gtao", False),
        ("use_bloom", False),
        ("use_raytracing", False),
        ("use_volumetric_lights", volume),
        ("use_volumetric_shadows", False),
        ("volumetric_samples", 16),
        ("volumetric_start", 0.1),
        ("volumetric_end", 8.0),
        ("use_volume_custom_range", True),
        ("volumetric_tile_size", "2"),
        ("volumetric_sample_distribution", 0.8),
        ("volumetric_light_clamp", 0.0),
        ("clamp_direct", 0.0),
        ("clamp_volume_direct", 0.0),
    ):
        if hasattr(eevee, attr):
            try:
                setattr(eevee, attr, value)
            except TypeError:
                pass

    old_world = scene.world
    world = bpy.data.worlds.new(f"FastPathWorld_{len(bpy.data.worlds):03d}")
    scene.world = world
    if old_world is not None and old_world.users == 0:
        bpy.data.worlds.remove(old_world)

    world.color = (0.0, 0.0, 0.0)
    world.use_nodes = True
    nodes = world.node_tree.nodes
    links = world.node_tree.links
    nodes.clear()
    output = nodes.new("ShaderNodeOutputWorld")
    if volume:
        scatter = nodes.new("ShaderNodeVolumeScatter")
        scatter.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
        scatter.inputs["Density"].default_value = 0.10
        if "Anisotropy" in scatter.inputs:
            scatter.inputs["Anisotropy"].default_value = 0.0
        links.new(scatter.outputs["Volume"], output.inputs["Volume"])
    else:
        background = nodes.new("ShaderNodeBackground")
        background.inputs["Color"].default_value = (0.0, 0.0, 0.0, 1.0)
        background.inputs["Strength"].default_value = 0.0
        links.new(background.outputs["Background"], output.inputs["Surface"])


def add_camera(location=(0.0, -4.0, 2.6), target=(0.0, 0.0, 0.0), ortho_scale=2.7):
    bpy.ops.object.camera_add(location=location)
    camera = bpy.context.object
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = ortho_scale
    look_at(camera, target)
    bpy.context.scene.camera = camera
    return camera


def make_diffuse_material(name, forward=False, shader_to_rgb=False):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    if forward:
        if hasattr(material, "surface_render_method"):
            material.surface_render_method = "BLENDED"
        else:
            material.blend_method = "BLEND"
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    diffuse = nodes.new("ShaderNodeBsdfDiffuse")
    diffuse.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    diffuse.inputs["Roughness"].default_value = 0.8
    if shader_to_rgb:
        shader_to_rgb_node = nodes.new("ShaderNodeShaderToRGB")
        emission = nodes.new("ShaderNodeEmission")
        emission.inputs["Strength"].default_value = 1.0
        links.new(diffuse.outputs["BSDF"], shader_to_rgb_node.inputs["Shader"])
        links.new(shader_to_rgb_node.outputs["Color"], emission.inputs["Color"])
        links.new(emission.outputs["Emission"], output.inputs["Surface"])
    else:
        links.new(diffuse.outputs["BSDF"], output.inputs["Surface"])
    return material


def add_plane(material, size=2.0, location=(0.0, 0.0, 0.0)):
    bpy.ops.mesh.primitive_plane_add(size=size, location=location)
    obj = bpy.context.object
    obj.name = "LightShaderFastPathReceiver"
    obj.data.materials.append(material)
    return obj


def ensure_light_nodes(light):
    if light.node_tree is None:
        light.use_nodes = True
    require(light.node_tree is not None, "light did not get a node tree")
    return light.node_tree


def find_light_shader_node(tree, bl_idname):
    active = None
    first = None
    for node in tree.nodes:
        if node.bl_idname != bl_idname:
            continue
        if first is None:
            first = node
        if getattr(node, "is_active_output", False):
            active = node
    return active or first


def clear_socket_links(tree, socket):
    for link in list(socket.links):
        tree.links.remove(link)


def add_uniform_scene_time_point_light(name="UniformSceneTimePoint", location=(0.0, -1.6, 2.6)):
    bpy.ops.object.light_add(type="POINT", location=location)
    light_obj = bpy.context.object
    light_obj.name = name
    light = light_obj.data
    light.name = f"{name}Data"
    light.energy = 80.0
    light.shadow_soft_size = 0.0
    light.use_shadow = False

    tree = ensure_light_nodes(light)
    nodes = tree.nodes
    links = tree.links
    output = find_light_shader_node(tree, "ShaderNodeEeveeLightShaderOutput")
    require(output is not None, "uniform test is missing Light Shader Output")
    clear_socket_links(tree, output.inputs["Color"])
    clear_socket_links(tree, output.inputs["Intensity"])
    clear_socket_links(tree, output.inputs["Attenuation"])
    output.inputs["Intensity"].default_value = 2.0
    output.inputs["Attenuation"].default_value = 1.0
    if hasattr(output, "range_scale"):
        output.range_scale = 1.0

    scene_time = nodes.new("GeometryNodeInputSceneTime")
    map_range = nodes.new("ShaderNodeMapRange")
    map_range.inputs["From Min"].default_value = 1.0
    map_range.inputs["From Max"].default_value = 11.0
    map_range.inputs["To Min"].default_value = 0.0
    map_range.inputs["To Max"].default_value = 1.0
    ramp = nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].position = 0.0
    ramp.color_ramp.elements[0].color = (0.0, 0.0, 1.0, 1.0)
    ramp.color_ramp.elements[1].position = 1.0
    ramp.color_ramp.elements[1].color = (1.0, 0.0, 0.0, 1.0)
    links.new(scene_time.outputs["Frame"], map_range.inputs["Value"])
    links.new(map_range.outputs["Result"], ramp.inputs["Fac"])
    links.new(ramp.outputs["Color"], output.inputs["Color"])
    return light_obj


def add_light_space_checker_sun(name="FrontCheckerSun"):
    bpy.ops.object.light_add(type="SUN", location=(0.0, 0.0, 0.0))
    light_obj = bpy.context.object
    light_obj.name = name
    light = light_obj.data
    light.name = f"{name}Data"
    light.energy = 1.0
    light.use_shadow = False

    tree = ensure_light_nodes(light)
    nodes = tree.nodes
    links = tree.links
    info = find_light_shader_node(tree, "ShaderNodeEeveeLightShaderInfo")
    output = find_light_shader_node(tree, "ShaderNodeEeveeLightShaderOutput")
    require(info is not None and output is not None, "checker light shader nodes are missing")
    clear_socket_links(tree, output.inputs["Color"])
    clear_socket_links(tree, output.inputs["Intensity"])
    clear_socket_links(tree, output.inputs["Attenuation"])
    output.inputs["Intensity"].default_value = 2.0
    output.inputs["Attenuation"].default_value = 1.0
    if hasattr(output, "range_scale"):
        output.range_scale = 1.0

    checker = nodes.new("ShaderNodeTexChecker")
    checker.inputs["Scale"].default_value = 4.0
    checker.inputs["Color1"].default_value = (1.0, 0.0, 0.0, 1.0)
    checker.inputs["Color2"].default_value = (0.0, 0.0, 1.0, 1.0)
    links.new(info.outputs["Light Space"], checker.inputs["Vector"])
    links.new(checker.outputs["Color"], output.inputs["Color"])
    return light_obj


def set_point_dependent_checker(light, red=1.0):
    tree = ensure_light_nodes(light)
    nodes = tree.nodes
    links = tree.links
    info = find_light_shader_node(tree, "ShaderNodeEeveeLightShaderInfo")
    output = find_light_shader_node(tree, "ShaderNodeEeveeLightShaderOutput")
    require(info is not None and output is not None, "overflow light shader nodes are missing")
    clear_socket_links(tree, output.inputs["Color"])
    clear_socket_links(tree, output.inputs["Intensity"])
    clear_socket_links(tree, output.inputs["Attenuation"])
    output.inputs["Intensity"].default_value = 1.0
    output.inputs["Attenuation"].default_value = 1.0
    if hasattr(output, "range_scale"):
        output.range_scale = 1.0

    checker = nodes.new("ShaderNodeTexChecker")
    checker.inputs["Scale"].default_value = 2.0
    checker.inputs["Color1"].default_value = (red, 0.0, 0.0, 1.0)
    checker.inputs["Color2"].default_value = (0.0, 0.0, 1.0, 1.0)
    links.new(info.outputs["Light Space"], checker.inputs["Vector"])
    links.new(checker.outputs["Color"], output.inputs["Color"])


def add_plain_point_light(name="PlainFallbackPoint", location=(0.0, -1.5, 2.2), energy=90.0):
    bpy.ops.object.light_add(type="POINT", location=location)
    obj = bpy.context.object
    obj.name = name
    light = obj.data
    light.energy = energy
    light.color = (1.0, 1.0, 1.0)
    light.shadow_soft_size = 0.0
    light.use_shadow = False
    if light.node_tree is not None:
        for node in list(light.node_tree.nodes):
            if node.bl_idname.startswith("ShaderNodeEeveeLightShader"):
                light.node_tree.nodes.remove(node)
    return obj


def render_pixels(label):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    bpy.context.view_layer.update()
    path = OUT_DIR / f"{label}.png"
    bpy.context.scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        pixels = list(image.pixels[:])
        size = tuple(image.size)
    finally:
        bpy.data.images.remove(image)
    return size, pixels, path


def sample_region(size, pixels, xmin=0.25, xmax=0.75, ymin=0.25, ymax=0.75, min_luma=0.0):
    width, height = size
    sums = [0.0, 0.0, 0.0]
    count = 0
    for y in range(int(height * ymin), int(height * ymax)):
        for x in range(int(width * xmin), int(width * xmax)):
            i = (y * width + x) * 4
            rgb = pixels[i : i + 3]
            luma = rgb[0] * 0.2126 + rgb[1] * 0.7152 + rgb[2] * 0.0722
            if luma < min_luma:
                continue
            for c in range(3):
                sums[c] += rgb[c]
            count += 1
    require(count > 0, "sample region has no pixels")
    return tuple(v / count for v in sums)


def max_pixel_rgb_diff(size_a, pixels_a, size_b, pixels_b):
    require(size_a == size_b, f"render sizes differ: {size_a} vs {size_b}")
    max_diff = 0.0
    for i in range(0, len(pixels_a), 4):
        max_diff = max(max_diff, *(abs(pixels_a[i + c] - pixels_b[i + c]) for c in range(3)))
    return max_diff


def validate_uniform_scene_time_surface(label, forward=False):
    clear_scene()
    setup_render()
    add_camera()
    add_plane(make_diffuse_material(f"{label}_mat", forward=forward))
    add_uniform_scene_time_point_light(f"{label}_Light")

    bpy.context.scene.frame_set(1)
    size_a, pixels_a, path_a = render_pixels(f"{label}_frame_001")
    rgb_a = sample_region(size_a, pixels_a, min_luma=0.001)

    bpy.context.scene.frame_set(11)
    size_b, pixels_b, path_b = render_pixels(f"{label}_frame_011")
    rgb_b = sample_region(size_b, pixels_b, min_luma=0.001)
    diff = max_pixel_rgb_diff(size_a, pixels_a, size_b, pixels_b)

    print(
        f"{label}: frame1={tuple(round(v, 6) for v in rgb_a)} "
        f"frame11={tuple(round(v, 6) for v in rgb_b)} max_pixel_diff={diff:.8f} "
        f"frame1_path={path_a} frame11_path={path_b}",
        flush=True,
    )
    require(rgb_b[0] > rgb_a[0] + 0.04, f"{label} did not pick up uniform red Scene Time result")
    require(rgb_a[2] > rgb_b[2] + 0.04, f"{label} did not reduce blue Scene Time result")
    require(diff > 0.04, f"{label} render did not change enough across uniform Scene Time frames")


def validate_uniform_scene_time_volume():
    clear_scene()
    setup_render(volume=True)
    add_camera(location=(0.0, -5.0, 1.5), target=(0.0, 0.0, 1.0), ortho_scale=4.0)
    add_uniform_scene_time_point_light("UniformVolumeSceneTime", location=(0.0, -1.2, 1.2))

    bpy.context.scene.frame_set(1)
    size_a, pixels_a, path_a = render_pixels("uniform_volume_frame_001")
    rgb_a = sample_region(size_a, pixels_a, xmin=0.30, xmax=0.70, ymin=0.25, ymax=0.75)

    bpy.context.scene.frame_set(11)
    size_b, pixels_b, path_b = render_pixels("uniform_volume_frame_011")
    rgb_b = sample_region(size_b, pixels_b, xmin=0.30, xmax=0.70, ymin=0.25, ymax=0.75)
    diff = max_pixel_rgb_diff(size_a, pixels_a, size_b, pixels_b)

    print(
        "uniform_volume_scene_time: "
        f"frame1={tuple(round(v, 6) for v in rgb_a)} "
        f"frame11={tuple(round(v, 6) for v in rgb_b)} max_pixel_diff={diff:.8f} "
        f"frame1_path={path_a} frame11_path={path_b}",
        flush=True,
    )
    require(rgb_b[0] > rgb_a[0] + 0.005, "volume did not pick up uniform red Scene Time result")
    require(rgb_a[2] > rgb_b[2] + 0.005, "volume did not reduce blue Scene Time result")
    require(diff > 0.005, "volume render did not change enough across uniform Scene Time frames")


def validate_shader_to_rgb_front_cache():
    clear_scene()
    setup_render(resolution=128)
    add_camera()
    add_plane(make_diffuse_material("front_checker_shader_to_rgb", shader_to_rgb=True), size=2.4)
    add_light_space_checker_sun()

    size, pixels, path = render_pixels("front_shader_to_rgb_light_space_checker")
    red_samples = []
    blue_samples = []
    for y in range(size[1] // 4, size[1] * 3 // 4):
        for x in range(size[0] // 4, size[0] * 3 // 4):
            i = (y * size[0] + x) * 4
            rgb = tuple(pixels[i : i + 3])
            if max(rgb) < 0.002:
                continue
            if rgb[0] > rgb[2] + 0.04:
                red_samples.append(rgb)
            elif rgb[2] > rgb[0] + 0.04:
                blue_samples.append(rgb)

    require(len(red_samples) > 50, f"front cache checker has too few red pixels: {len(red_samples)} path={path}")
    require(len(blue_samples) > 50, f"front cache checker has too few blue pixels: {len(blue_samples)} path={path}")
    red_avg = tuple(sum(rgb[i] for rgb in red_samples) / len(red_samples) for i in range(3))
    blue_avg = tuple(sum(rgb[i] for rgb in blue_samples) / len(blue_samples) for i in range(3))
    print(
        "front_shader_to_rgb_light_space_checker: "
        f"red_count={len(red_samples)} blue_count={len(blue_samples)} "
        f"red_avg={tuple(round(v, 6) for v in red_avg)} "
        f"blue_avg={tuple(round(v, 6) for v in blue_avg)} path={path}",
        flush=True,
    )
    require(
        red_avg[0] > red_avg[2] * 1.5 and blue_avg[2] > blue_avg[0] * 1.5,
        f"Shader to RGB did not read front-layer checker cache: red={red_avg} blue={blue_avg}",
    )


def validate_surface_layer_overflow_fallback():
    clear_scene()
    setup_render(resolution=64)
    add_camera()
    add_plane(make_diffuse_material("overflow_receiver"), size=2.0)
    add_plain_point_light()

    max_layers = gpu.capabilities.max_texture_layers_get()
    count = min(max_layers + 1, 4097)
    print(f"overflow_fallback_setup: max_texture_layers={max_layers} custom_light_count={count}", flush=True)
    require(count > max_layers, "overflow test did not exceed GPU texture layer count")

    for index in range(count):
        light = bpy.data.lights.new(f"OverflowPointData_{index:04d}", "POINT")
        light.energy = 0.0
        light.shadow_soft_size = 0.0
        light.use_shadow = False
        set_point_dependent_checker(light, red=1.0 if (index & 1) == 0 else 0.25)
        obj = bpy.data.objects.new(f"OverflowPoint_{index:04d}", light)
        obj.location = (3.0 + index * 0.001, -2.0, 3.0)
        bpy.context.collection.objects.link(obj)

    size, pixels, path = render_pixels("surface_texture_layer_overflow_fallback")
    rgb = sample_region(size, pixels, min_luma=0.001)
    print(
        "surface_texture_layer_overflow_fallback: "
        f"avg={tuple(round(v, 6) for v in rgb)} path={path}",
        flush=True,
    )
    require(sum(rgb) > 0.01, f"overflow fallback render is unexpectedly dark: {rgb}")


def main():
    validate_uniform_scene_time_surface("uniform_deferred_surface", forward=False)
    validate_uniform_scene_time_surface("uniform_forward_surface", forward=True)
    validate_uniform_scene_time_volume()
    validate_shader_to_rgb_front_cache()
    validate_surface_layer_overflow_fallback()
    print("EEVEE_LIGHT_SHADER_FAST_PATH_RELEASE_OK", flush=True)


if __name__ == "__main__":
    main()
