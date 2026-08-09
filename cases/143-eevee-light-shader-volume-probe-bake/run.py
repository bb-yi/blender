from pathlib import Path
import math
import sys

import bpy
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
    ):
        for item in list(datablock):
            if item.users == 0:
                datablock.remove(item)


def look_at(obj, target):
    direction = Vector(target) - obj.location
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def setup_render():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 96
    scene.render.resolution_y = 96
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = False
    scene.render.image_settings.file_format = "PNG"
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    scene.world.color = (0.0, 0.0, 0.0)

    eevee = scene.eevee
    for attr, value in (
        ("taa_render_samples", 1),
        ("taa_samples", 1),
        ("use_gtao", False),
        ("use_bloom", False),
        ("use_raytracing", False),
        ("gi_cubemap_resolution", "128"),
    ):
        if hasattr(eevee, attr):
            setattr(eevee, attr, value)


def make_diffuse_material():
    material = bpy.data.materials.new("volume_probe_bake_diffuse")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()
    output = nodes.new("ShaderNodeOutputMaterial")
    diffuse = nodes.new("ShaderNodeBsdfDiffuse")
    diffuse.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    diffuse.inputs["Roughness"].default_value = 0.8
    links.new(diffuse.outputs["BSDF"], output.inputs["Surface"])
    return material


def make_probe_display_material():
    material = bpy.data.materials.new("volume_probe_bake_display")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()
    output = nodes.new("ShaderNodeOutputMaterial")
    probe = nodes.new("ShaderNodeLightProbeColor")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Strength"].default_value = 1.0
    links.new(probe.outputs["Irradiance"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def add_camera():
    bpy.ops.object.camera_add(location=(0.0, -4.0, 1.0))
    camera = bpy.context.object
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = 2.4
    look_at(camera, (0.0, 0.0, 1.0))
    bpy.context.scene.camera = camera


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


def link_default_intensity(tree, output):
    info = find_light_shader_node(tree, "ShaderNodeEeveeLightShaderInfo")
    require(info is not None, "missing Light Shader Info")
    clear_socket_links(tree, output.inputs["Intensity"])
    tree.links.new(info.outputs["Default Intensity"], output.inputs["Intensity"])


def remove_light_shader_nodes(light):
    tree = ensure_light_nodes(light)
    for node in list(tree.nodes):
        if node.bl_idname.startswith("ShaderNodeEeveeLightShader"):
            tree.nodes.remove(node)


def set_light_shader_constant(
    light, color=(1.0, 1.0, 1.0), intensity=1.0, attenuation=1.0, use_default_intensity=False
):
    tree = ensure_light_nodes(light)
    output = find_light_shader_node(tree, "ShaderNodeEeveeLightShaderOutput")
    require(output is not None, "missing Light Shader Output")
    output.is_active_output = True
    clear_socket_links(tree, output.inputs["Color"])
    clear_socket_links(tree, output.inputs["Intensity"])
    clear_socket_links(tree, output.inputs["Attenuation"])
    output.inputs["Color"].default_value = (*color, 1.0)
    if use_default_intensity:
        link_default_intensity(tree, output)
    else:
        output.inputs["Intensity"].default_value = intensity
    output.inputs["Attenuation"].default_value = attenuation
    if hasattr(output, "range_scale"):
        output.range_scale = 6.0


def set_light_shader_default_attenuation(light):
    tree = ensure_light_nodes(light)
    info = find_light_shader_node(tree, "ShaderNodeEeveeLightShaderInfo")
    output = find_light_shader_node(tree, "ShaderNodeEeveeLightShaderOutput")
    require(info is not None and output is not None, "missing default light shader nodes")
    output.is_active_output = True
    clear_socket_links(tree, output.inputs["Color"])
    clear_socket_links(tree, output.inputs["Intensity"])
    clear_socket_links(tree, output.inputs["Attenuation"])
    output.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    tree.links.new(info.outputs["Default Intensity"], output.inputs["Intensity"])
    tree.links.new(info.outputs["Default Attenuation"], output.inputs["Attenuation"])
    if hasattr(output, "range_scale"):
        output.range_scale = 6.0


def set_light_shader_light_space_color(light):
    tree = ensure_light_nodes(light)
    nodes = tree.nodes
    links = tree.links
    info = find_light_shader_node(tree, "ShaderNodeEeveeLightShaderInfo")
    output = find_light_shader_node(tree, "ShaderNodeEeveeLightShaderOutput")
    require(info is not None and output is not None, "missing default light shader nodes")
    output.is_active_output = True
    clear_socket_links(tree, output.inputs["Color"])
    clear_socket_links(tree, output.inputs["Intensity"])
    clear_socket_links(tree, output.inputs["Attenuation"])
    tree.links.new(info.outputs["Default Intensity"], output.inputs["Intensity"])
    output.inputs["Attenuation"].default_value = 1.0
    if hasattr(output, "range_scale"):
        output.range_scale = 6.0

    separate = nodes.new("ShaderNodeSeparateXYZ")
    x_scale = nodes.new("ShaderNodeMath")
    x_bias = nodes.new("ShaderNodeMath")
    combine = nodes.new("ShaderNodeCombineColor")
    x_scale.operation = "MULTIPLY"
    x_scale.inputs[1].default_value = 0.45
    x_bias.operation = "ADD"
    x_bias.inputs[1].default_value = 0.5
    combine.inputs["Green"].default_value = 0.15
    combine.inputs["Blue"].default_value = 0.85
    links.new(info.outputs["Light Space"], separate.inputs["Vector"])
    links.new(separate.outputs["X"], x_scale.inputs[0])
    links.new(x_scale.outputs["Value"], x_bias.inputs[0])
    links.new(x_bias.outputs["Value"], combine.inputs["Red"])
    links.new(combine.outputs["Color"], output.inputs["Color"])


def add_bake_scene(light_setup=None, remove_nodes=False):
    clear_scene()
    setup_render()
    add_camera()

    bpy.ops.mesh.primitive_plane_add(
        size=2.0, location=(0.0, 0.0, 1.0), rotation=(math.radians(90.0), 0.0, 0.0)
    )
    wall = bpy.context.object
    wall.name = "VolumeProbeBakeWall"
    wall.data.materials.append(make_diffuse_material())

    bpy.ops.object.light_add(type="POINT", location=(0.0, -1.35, 1.0))
    light_obj = bpy.context.object
    light_obj.name = "VolumeProbeBakeLight"
    light = light_obj.data
    light.energy = 650.0
    light.shadow_soft_size = 0.05
    light.use_shadow = False
    ensure_light_nodes(light)
    if remove_nodes:
        remove_light_shader_nodes(light)
    elif light_setup is not None:
        light_setup(light)

    bpy.ops.object.lightprobe_add(type="VOLUME", location=(0.0, -0.08, 1.0))
    probe = bpy.context.object
    probe.name = "VolumeProbeBakeGrid"
    probe.scale = (1.25, 1.0, 1.25)
    data = probe.data
    data.resolution_x = 4
    data.resolution_y = 3
    data.resolution_z = 4
    data.capture_distance = 3.0
    data.surfel_density = 12
    data.bake_samples = 8
    data.capture_world = False
    data.capture_indirect = True
    data.capture_emission = False
    data.surface_bias = 0.02
    data.escape_bias = 0.02
    data.normal_bias = 0.0
    data.view_bias = 0.0
    data.facing_bias = 0.0
    data.validity_threshold = 0.0
    data.dilation_threshold = 0.0
    data.clamp_direct = 0.0
    data.clamp_indirect = 0.0

    bpy.ops.object.select_all(action="DESELECT")
    probe.select_set(True)
    bpy.context.view_layer.objects.active = probe
    return wall, light_obj, probe


def bake_probe():
    bpy.context.view_layer.update()
    result = bpy.ops.object.lightprobe_cache_bake(subset="ACTIVE")
    require("FINISHED" in result, f"Volume Probe bake failed: {result}")


def render_baked_irradiance(label, wall):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    wall.data.materials.clear()
    wall.data.materials.append(make_probe_display_material())
    for obj in bpy.context.scene.objects:
        if obj.type == "LIGHT":
            obj.hide_render = True
            obj.hide_viewport = True
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


def average_region_rgb(size, pixels, region):
    width, height = size
    xmin, xmax, ymin, ymax = region
    sums = [0.0, 0.0, 0.0]
    count = 0
    for y in range(int(height * ymin), int(height * ymax)):
        for x in range(int(width * xmin), int(width * xmax)):
            i = (y * width + x) * 4
            sums[0] += pixels[i]
            sums[1] += pixels[i + 1]
            sums[2] += pixels[i + 2]
            count += 1
    require(count > 0, "empty sample region")
    return tuple(v / count for v in sums)


def max_pixel_rgb_diff(size_a, pixels_a, size_b, pixels_b):
    require(size_a == size_b, "image size mismatch")
    max_diff = 0.0
    for i in range(0, len(pixels_a), 4):
        max_diff = max(max_diff, *(abs(pixels_a[i + c] - pixels_b[i + c]) for c in range(3)))
    return max_diff


def luma(rgb):
    return rgb[0] * 0.2126 + rgb[1] * 0.7152 + rgb[2] * 0.0722


def render_case(label, light_setup=None, remove_nodes=False):
    wall, _light, _probe = add_bake_scene(light_setup=light_setup, remove_nodes=remove_nodes)
    bake_probe()
    size, pixels, path = render_baked_irradiance(label, wall)
    avg = average_region_rgb(size, pixels, (0.25, 0.75, 0.25, 0.75))
    print(f"{label}: avg_rgb={tuple(round(v, 6) for v in avg)} path={path}", flush=True)
    return size, pixels, path, avg


def validate_default_passthrough():
    size_default, pixels_default, path_default, _avg_default = render_case(
        "volume_probe_default_passthrough"
    )
    size_removed, pixels_removed, path_removed, _avg_removed = render_case(
        "volume_probe_light_shader_removed", remove_nodes=True
    )
    diff = max_pixel_rgb_diff(size_default, pixels_default, size_removed, pixels_removed)
    print(
        f"volume_probe_default_passthrough: max_pixel_diff={diff:.8f} "
        f"default={path_default} removed={path_removed}",
        flush=True,
    )
    require(diff < 1e-5, f"default Light Shader nodes changed Volume Probe bake: {diff}")


def validate_custom_color():
    _size, _pixels, _path, avg = render_case(
        "volume_probe_custom_red",
        lambda light: set_light_shader_constant(
            light, color=(1.0, 0.0, 0.0), use_default_intensity=True
        ),
    )
    require(avg[0] > avg[1] * 2.0 and avg[0] > avg[2] * 2.0, f"custom red did not dominate baked irradiance: {avg}")


def validate_attenuation_one():
    size_default, pixels_default, path_default, _avg_default = render_case(
        "volume_probe_default_attenuation", set_light_shader_default_attenuation
    )
    size_constant, pixels_constant, path_constant, _avg_constant = render_case(
        "volume_probe_constant_attenuation",
        lambda light: set_light_shader_constant(
            light, attenuation=1.0, use_default_intensity=True
        ),
    )

    default_left = luma(average_region_rgb(size_default, pixels_default, (0.18, 0.34, 0.35, 0.65)))
    default_right = luma(average_region_rgb(size_default, pixels_default, (0.66, 0.82, 0.35, 0.65)))
    constant_left = luma(average_region_rgb(size_constant, pixels_constant, (0.18, 0.34, 0.35, 0.65)))
    constant_right = luma(average_region_rgb(size_constant, pixels_constant, (0.66, 0.82, 0.35, 0.65)))
    default_delta = abs(default_left - default_right)
    constant_delta = abs(constant_left - constant_right)
    print(
        "volume_probe_attenuation: "
        f"default_left={default_left:.6f} default_right={default_right:.6f} "
        f"constant_left={constant_left:.6f} constant_right={constant_right:.6f} "
        f"default={path_default} constant={path_constant}",
        flush=True,
    )
    require(constant_left > 1e-5 and constant_right > 1e-5, "constant attenuation baked result is too dark")
    require(
        constant_delta < max(default_delta * 0.5, 0.02),
        f"Attenuation=1 did not reduce baked distance falloff enough: default_delta={default_delta}, constant_delta={constant_delta}",
    )


def validate_light_space_pattern():
    size, pixels, path, _avg = render_case("volume_probe_light_space_pattern", set_light_shader_light_space_color)
    left = average_region_rgb(size, pixels, (0.18, 0.34, 0.35, 0.65))
    right = average_region_rgb(size, pixels, (0.66, 0.82, 0.35, 0.65))
    red_delta = abs(right[0] - left[0])
    blue_delta = abs(right[2] - left[2])
    print(
        f"volume_probe_light_space_pattern: left={tuple(round(v, 6) for v in left)} "
        f"right={tuple(round(v, 6) for v in right)} path={path}",
        flush=True,
    )
    require(red_delta > 0.015, f"Light Space pattern did not produce baked red variation: left={left}, right={right}")
    require(red_delta > blue_delta * 1.3, f"Light Space pattern variation is not on expected channel: left={left}, right={right}")


def main():
    validate_default_passthrough()
    validate_custom_color()
    validate_attenuation_one()
    validate_light_space_pattern()
    print("EEVEE_LIGHT_SHADER_VOLUME_PROBE_BAKE_RELEASE_OK", flush=True)


if __name__ == "__main__":
    try:
        main()
    except BaseException as ex:
        print(
            f"EEVEE_LIGHT_SHADER_VOLUME_PROBE_BAKE_RELEASE_FAILED: {type(ex).__name__}: {ex}",
            file=sys.stderr,
            flush=True,
        )
        raise SystemExit(1) from ex
