import argparse
import math
from pathlib import Path
import statistics
import sys

import bpy
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))

from filter_graph_test_utils import (
    add_pass_input_image_sample,
    attach_filter_material,
    clear_filter_graph,
)


RENDER_SIZE = 32
SOFT_SHADOW_WIDTH = 96
SOFT_SHADOW_HEIGHT = 64
POINT_ENERGY = 100.0
BACKEND_MARKER = "NPR_FOREACH_LIGHT_BACKEND="
SUCCESS_MARKER = "NPR_FOREACH_LIGHT_OK"
OUTPUT_DIR = None


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def parse_args():
    argv = sys.argv
    argv = argv[argv.index("--") + 1 :] if "--" in argv else []
    parser = argparse.ArgumentParser()
    parser.add_argument("--expected-backend", required=True, choices=("OPENGL", "VULKAN"))
    parser.add_argument("--output-dir", required=True, type=Path)
    return parser.parse_args(argv)


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)


def configure_scene():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = RENDER_SIZE
    scene.render.resolution_y = RENDER_SIZE
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = False
    scene.render.image_settings.file_format = "OPEN_EXR"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "32"
    scene.eevee.taa_samples = 1
    scene.eevee.taa_render_samples = 1
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    scene.world.use_nodes = False
    scene.world.color = (0.0, 0.0, 0.0)
    clear_filter_graph(scene)
    return scene


def make_camera():
    camera_data = bpy.data.cameras.new("ForeachLightCamera")
    camera = bpy.data.objects.new("ForeachLightCamera", camera_data)
    camera.location = (0.0, 0.0, 5.0)
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 3.0
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera
    return camera


def socket_with_identifier(sockets, identifier):
    for socket in sockets:
        if socket.identifier == identifier:
            return socket
    raise AssertionError(f"Socket {identifier!r} not found in {[socket.identifier for socket in sockets]}")


def make_foreach_zone(tree, initial_socket, mode, name):
    nodes = tree.nodes
    links = tree.links
    input_node = nodes.new("ShaderNodeForeachLightInput")
    input_node.name = name + " Input"
    output_node = nodes.new("ShaderNodeForeachLightOutput")
    output_node.name = name + " Output"
    input_node.pair_with_output(output_node)
    tree.interface_update(bpy.context)

    zone_input = socket_with_identifier(input_node.inputs, "Item_0")
    zone_value = socket_with_identifier(input_node.outputs, "Item_0")
    zone_output = socket_with_identifier(output_node.inputs, "Item_0")
    zone_result = socket_with_identifier(output_node.outputs, "Item_0")
    links.new(initial_socket, zone_input)

    if mode == "overwrite":
        links.new(input_node.outputs["Color"], zone_output)
    elif mode == "shadow":
        links.new(input_node.outputs["Shadow Mask"], zone_output)
    elif mode in {"signed_direction", "signed_direction_strong"}:
        scale = nodes.new("ShaderNodeVectorMath")
        scale.name = name + " Negate Direction"
        scale.operation = "SCALE"
        scale.inputs[3].default_value = -4.0 if mode == "signed_direction_strong" else -1.0
        links.new(input_node.outputs["Direction"], scale.inputs[0])
        links.new(scale.outputs["Vector"], zone_output)
    else:
        add = nodes.new("ShaderNodeVectorMath")
        add.name = name + " Add"
        add.operation = "ADD"
        links.new(zone_value, add.inputs[0])
        if mode == "sum":
            links.new(input_node.outputs["Color"], add.inputs[1])
        else:
            raise AssertionError(f"Unsupported foreach mode {mode!r}")
        links.new(add.outputs["Vector"], zone_output)

    return zone_result


def make_npr_tree(name, mode, sequential=False):
    tree = bpy.data.node_groups.new(name, "ShaderNodeTree")
    nodes = tree.nodes
    links = tree.links

    initial = nodes.new("ShaderNodeRGB")
    initial.name = "Initial Black"
    initial.outputs["Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    result = make_foreach_zone(tree, initial.outputs["Color"], mode, "Foreach A")
    if sequential:
        result = make_foreach_zone(tree, result, mode, "Foreach B")

    output = nodes.new("ShaderNodeNPR_Output")
    output.name = "NPR Output"
    links.new(result, output.inputs["Color"])
    tree.interface_update(bpy.context)
    tree.update_tag()
    return tree


def make_material(name, npr_tree, bake_image=None):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    emission.inputs["Strength"].default_value = 1.0
    output = nodes.new("ShaderNodeOutputMaterial")
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    output.nprtree = npr_tree

    if bake_image is not None:
        image_node = nodes.new("ShaderNodeTexImage")
        image_node.image = bake_image
        image_node.select = True
        nodes.active = image_node

    material.node_tree.update_tag()
    return material


def attach_signed_npr_remap_filter():
    material = bpy.data.materials.new("Signed NPR Remap Filter")
    material.use_nodes = True
    material.eevee_domain = "FILTER"
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputFilter")
    output.inputs["Alpha"].default_value = 1.0
    _, image_sample = add_pass_input_image_sample(nodes, links)
    glsl = nodes.new("ShaderNodeGLSLFunction")
    text = bpy.data.texts.new("signed_npr_remap_filter.glsl")
    text.write(
        "vec4 remap_signed_npr(vec4 color){\n"
        "  return vec4(color.rgb * 0.5 + 0.5, color.a);\n"
        "}\n"
    )
    glsl.source_mode = "INTERNAL"
    glsl.script = text
    glsl.function_name = "remap_signed_npr"
    material.node_tree.interface_update(bpy.context)
    material.node_tree.update_tag()
    bpy.context.view_layer.update()
    require(glsl.parse_status == "READY", f"Signed NPR remap filter failed: {glsl.parse_status}")

    links.new(image_sample.outputs["Color"], glsl.inputs["color"])
    links.new(glsl.outputs["Result"], output.inputs["Color"])
    attach_filter_material(
        material,
        stage="BEFORE_COMPOSITE",
        scene_socket="Color Image",
    )


def make_plane(material, size=4.0):
    bpy.ops.mesh.primitive_plane_add(size=size, location=(0.0, 0.0, 0.0))
    plane = bpy.context.object
    plane.name = "ForeachLightPlane"
    plane.data.materials.append(material)
    return plane


def point_camera_at(camera, target):
    camera.rotation_euler = (Vector(target) - camera.location).to_track_quat("-Z", "Y").to_euler()


def make_reflective_floor_material():
    material = bpy.data.materials.new("Signed NPR Reflection Receiver")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()
    output = nodes.new("ShaderNodeOutputMaterial")
    principled = nodes.new("ShaderNodeBsdfPrincipled")
    principled.inputs["Base Color"].default_value = (0.8, 0.8, 0.8, 1.0)
    principled.inputs["Metallic"].default_value = 1.0
    principled.inputs["Roughness"].default_value = 0.35
    links.new(principled.outputs["BSDF"], output.inputs["Surface"])
    return material


def add_point_light(
    name,
    color,
    location=(0.0, 0.0, 2.0),
    energy=POINT_ENERGY,
    use_shadow=False,
):
    light_data = bpy.data.lights.new(name, type="POINT")
    light_data.color = color
    light_data.energy = energy
    light_data.use_shadow = use_shadow
    light_data.shadow_soft_size = 0.05
    light = bpy.data.objects.new(name, light_data)
    light.location = location
    bpy.context.scene.collection.objects.link(light)
    return light


def add_sun_light(name, color, energy=1.0):
    light_data = bpy.data.lights.new(name, type="SUN")
    light_data.color = color
    light_data.energy = energy
    light_data.use_shadow = False
    light = bpy.data.objects.new(name, light_data)
    light.rotation_euler = (0.0, 0.0, 0.0)
    bpy.context.scene.collection.objects.link(light)
    return light


def add_area_light(name, location=(-2.5, 0.0, 3.5), energy=500.0):
    light_data = bpy.data.lights.new(name, type="AREA")
    light_data.color = (1.0, 1.0, 1.0)
    light_data.energy = energy
    light_data.use_shadow = True
    light_data.shape = "RECTANGLE"
    light_data.size = 2.0
    light_data.size_y = 6.0
    light = bpy.data.objects.new(name, light_data)
    light.location = location
    bpy.context.scene.collection.objects.link(light)
    return light


def render_image(label):
    require(OUTPUT_DIR is not None, "Render output directory was not configured")
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    filepath = OUTPUT_DIR / f"{label}.exr"
    bpy.context.scene.render.filepath = str(filepath)
    bpy.context.view_layer.update()
    result = bpy.ops.render.render(write_still=False)
    require("FINISHED" in result, f"{label}: render returned {result}")
    render_result = bpy.data.images.get("Render Result")
    require(render_result is not None, f"{label}: Render Result was not created")
    render_result.save_render(str(filepath))

    image = bpy.data.images.load(str(filepath), check_existing=False)
    try:
        width, height = image.size[:]
        pixels = list(image.pixels[:])
        require(len(pixels) == width * height * 4, f"{label}: unexpected pixel count {len(pixels)}")
        require(all(math.isfinite(value) for value in pixels), f"{label}: non-finite render pixel")
    finally:
        bpy.data.images.remove(image)
    return width, height, pixels


def render_pixels(label):
    width, height, pixels = render_image(label)
    require(
        (width, height) == (RENDER_SIZE, RENDER_SIZE),
        f"{label}: expected {RENDER_SIZE}x{RENDER_SIZE}, got {width}x{height}",
    )
    index = ((height // 2) * width + (width // 2)) * 4
    center = tuple(pixels[index : index + 4])
    print(f"NPR_FOREACH_LIGHT_{label.upper()}={center}", flush=True)
    return center


def assert_channels(label, color, minimum, maximum):
    for channel, threshold in minimum.items():
        require(color[channel] > threshold, f"{label}: channel {channel} should exceed {threshold}, got {color}")
    for channel, threshold in maximum.items():
        require(color[channel] < threshold, f"{label}: channel {channel} should be below {threshold}, got {color}")


def setup_render_scene(name, mode, sequential=False):
    clear_scene()
    configure_scene()
    make_camera()
    tree = make_npr_tree(name + "Tree", mode, sequential=sequential)
    material = make_material(name + "Material", tree)
    plane = make_plane(material)
    bpy.context.view_layer.update()
    return plane


def test_progressive_lights(expected_backend):
    setup_render_scene("ProgressiveForeach", "sum", sequential=True)

    no_lights = render_pixels("no_lights")
    import gpu

    active_backend = gpu.platform.backend_type_get()
    print(BACKEND_MARKER + active_backend, flush=True)
    require(active_backend == expected_backend, f"Expected {expected_backend}, got {active_backend}")
    assert_channels("no lights", no_lights, {}, {0: 0.01, 1: 0.01, 2: 0.01})

    add_point_light("Red Point", (1.0, 0.0, 0.0))
    red = render_pixels("red_point")
    assert_channels("red point", red, {0: 0.05}, {1: 0.02, 2: 0.02})

    add_point_light("Green Point", (0.0, 1.0, 0.0), location=(0.35, 0.0, 2.0))
    red_green = render_pixels("red_green_points")
    assert_channels("red and green points", red_green, {0: 0.05, 1: 0.05}, {2: 0.02})

    add_sun_light("Blue Sun", (0.0, 0.0, 1.0))
    all_types = render_pixels("red_green_blue_sun")
    assert_channels("red, green, and blue lights", all_types, {0: 0.05, 1: 0.05, 2: 0.05}, {})


def test_local_before_directional_order():
    setup_render_scene("ForeachOrder", "overwrite")
    add_point_light("Order Red Point", (1.0, 0.0, 0.0))
    add_sun_light("Order Blue Sun", (0.0, 0.0, 1.0))
    color = render_pixels("local_before_directional")
    assert_channels("local-before-directional order", color, {2: 0.05}, {0: 0.02, 1: 0.02})


def test_second_local_light_word():
    setup_render_scene("ForeachSecondWord", "sum")
    for index in range(32):
        x = ((index % 6) - 2.5) * 0.08
        y = ((index // 6) - 2.5) * 0.08
        add_point_light(
            f"Red Word Point {index:02d}",
            (1.0, 0.0, 0.0),
            location=(x, y, 2.0),
        )
    add_point_light("Blue Point 33", (0.0, 0.0, 1.0), location=(0.0, 0.0, 2.0))
    color = render_pixels("thirty_three_local_lights")
    assert_channels("second local-light word", color, {0: 0.05, 2: 0.05}, {1: 0.02})


def test_shadow_mask_visibility():
    setup_render_scene("ForeachShadow", "shadow")
    add_point_light(
        "Shadow Point",
        (1.0, 1.0, 1.0),
        location=(1.0, 0.0, 2.0),
        use_shadow=True,
    )

    visible = render_pixels("shadow_mask_visible")
    assert_channels("visible shadow mask", visible, {0: 0.5, 1: 0.5, 2: 0.5}, {})

    bpy.ops.mesh.primitive_cube_add(size=0.5, location=(0.5, 0.0, 1.0))
    occluder = bpy.context.object
    occluder.name = "Shadow Occluder"
    bpy.context.view_layer.update()
    occluded = render_pixels("shadow_mask_occluded")
    assert_channels("occluded shadow mask", occluded, {}, {0: 0.2, 1: 0.2, 2: 0.2})


def red_channel(pixels, width, x, y):
    return pixels[(y * width + x) * 4]


def test_area_shadow_temporal_convergence():
    plane = setup_render_scene("ForeachAreaShadow", "shadow")
    scene = bpy.context.scene
    scene.render.resolution_x = SOFT_SHADOW_WIDTH
    scene.render.resolution_y = SOFT_SHADOW_HEIGHT
    scene.camera.data.ortho_scale = 4.0
    plane.scale = (1.5, 1.5, 1.0)
    add_area_light("Temporal Area")

    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(-1.2, 0.0, 1.0))
    blocker = bpy.context.object
    blocker.name = "Temporal Area Blocker"
    blocker.scale = (0.2, 3.0, 1.0)

    scene.eevee.taa_render_samples = 1
    one_width, one_height, one_sample = render_image("area_shadow_one_sample")
    scene.eevee.taa_render_samples = 64
    many_width, many_height, many_samples = render_image("area_shadow_64_samples")
    require(
        (one_width, one_height) == (SOFT_SHADOW_WIDTH, SOFT_SHADOW_HEIGHT),
        f"Unexpected one-sample size {(one_width, one_height)}",
    )
    require(
        (many_width, many_height) == (SOFT_SHADOW_WIDTH, SOFT_SHADOW_HEIGHT),
        f"Unexpected multi-sample size {(many_width, many_height)}",
    )

    rows = range(SOFT_SHADOW_HEIGHT // 4, SOFT_SHADOW_HEIGHT * 3 // 4)
    columns = range(SOFT_SHADOW_WIDTH * 7 // 20, SOFT_SHADOW_WIDTH * 9 // 10)
    many_profile = {
        x: statistics.fmean(red_channel(many_samples, many_width, x, y) for y in rows)
        for x in columns
    }
    penumbra_columns = [x for x, value in many_profile.items() if 0.1 < value < 0.9]
    require(
        len(penumbra_columns) >= 8,
        f"Area shadow should contain a measurable penumbra, got profile {many_profile}",
    )

    one_variance = statistics.fmean(
        statistics.pvariance(red_channel(one_sample, one_width, x, y) for y in rows)
        for x in penumbra_columns
    )
    many_variance = statistics.fmean(
        statistics.pvariance(red_channel(many_samples, many_width, x, y) for y in rows)
        for x in penumbra_columns
    )
    grayscale_levels = len({round(many_profile[x], 2) for x in penumbra_columns})
    quantized_eighth_ratio = statistics.fmean(
        abs(value * 8.0 - round(value * 8.0)) < 0.08
        for value in (many_profile[x] for x in penumbra_columns)
    )

    print(
        "NPR_FOREACH_LIGHT_AREA_SHADOW="
        f"columns={len(penumbra_columns)} "
        f"one_variance={one_variance:.6f} "
        f"many_variance={many_variance:.6f} "
        f"levels={grayscale_levels} "
        f"eighth_ratio={quantized_eighth_ratio:.6f}",
        flush=True,
    )
    require(
        one_variance > 0.01,
        f"One-sample area shadow should use spatially randomized rays, variance={one_variance}",
    )
    require(
        many_variance < one_variance * 0.45,
        f"64 samples should converge spatial noise: one={one_variance}, many={many_variance}",
    )
    require(
        grayscale_levels >= 12,
        f"Area penumbra should exceed the fixed 8-ray grayscale levels, got {grayscale_levels}",
    )
    require(
        quantized_eighth_ratio < 0.75,
        f"Area penumbra should not cluster on 1/8 levels, ratio={quantized_eighth_ratio}",
    )


def test_signed_npr_output_is_preserved_for_filter_consumers():
    setup_render_scene("ForeachSignedRadiance", "signed_direction")
    add_point_light(
        "Signed Direction Point",
        (1.0, 1.0, 1.0),
        location=(1.0, -1.0, 2.0),
    )
    attach_signed_npr_remap_filter()
    color = render_pixels("signed_npr_filter_remap")
    require(
        color[0] < 0.4 and color[1] > 0.6 and color[2] < 0.25,
        f"filter should receive signed NPR red/blue before remapping to positive values, got {color}",
    )


def test_signed_npr_screen_reflection_is_sanitized():
    clear_scene()
    scene = configure_scene()
    scene.render.resolution_x = 128
    scene.render.resolution_y = 128
    scene.eevee.taa_render_samples = 16
    scene.eevee.use_raytracing = True
    scene.eevee.ray_tracing_method = "SCREEN"
    ray_options = scene.eevee.ray_tracing_options
    ray_options.resolution_scale = "1"
    ray_options.trace_max_roughness = 1.0
    ray_options.screen_trace_quality = 1.0
    ray_options.screen_trace_thickness = 1.0
    ray_options.use_denoise = False

    camera_data = bpy.data.cameras.new("Signed NPR Reflection Camera")
    camera = bpy.data.objects.new("Signed NPR Reflection Camera", camera_data)
    camera.location = (0.0, -6.0, 3.0)
    camera_data.lens = 50.0
    point_camera_at(camera, (0.0, 0.7, 0.8))
    scene.collection.objects.link(camera)
    scene.camera = camera

    signed_tree = make_npr_tree("Signed NPR Reflection Tree", "signed_direction_strong")
    signed_material = make_material("Signed NPR Reflection Source", signed_tree)
    bpy.ops.mesh.primitive_plane_add(
        size=2.0,
        location=(0.0, 1.0, 1.25),
        rotation=(math.pi * 0.5, 0.0, 0.0),
    )
    source = bpy.context.object
    source.name = "Signed NPR Reflection Source"
    source.scale = (1.4, 1.0, 1.0)
    source.data.materials.append(signed_material)

    floor_material = make_reflective_floor_material()
    bpy.ops.mesh.primitive_plane_add(size=12.0, location=(0.0, 0.0, 0.0))
    floor = bpy.context.object
    floor.name = "Signed NPR Reflection Floor"
    floor.data.materials.append(floor_material)

    add_point_light(
        "Signed Reflection Direction Point",
        (1.0, 1.0, 1.0),
        location=(1.0, -1.0, 2.0),
        energy=POINT_ENERGY,
    )

    width, height, pixels = render_image("signed_npr_screen_reflection")
    green_pixels = []
    for y in range(height // 2):
        for x in range(width):
            index = (y * width + x) * 4
            red, green, blue = pixels[index : index + 3]
            if green > 0.03 and green > red + 0.02 and green > blue + 0.02:
                green_pixels.append((red, green, blue))

    max_green = max((color[1] for color in green_pixels), default=0.0)
    print(
        "NPR_FOREACH_LIGHT_SIGNED_SCREEN_REFLECTION="
        f"green_pixels={len(green_pixels)} max_green={max_green:.6f}",
        flush=True,
    )
    require(
        len(green_pixels) > 20 and max_green > 0.08,
        "screen-space reflection did not preserve the sanitized positive green radiance: "
        f"green_pixels={len(green_pixels)} max_green={max_green}",
    )


def test_color_bake_no_culling():
    clear_scene()
    scene = configure_scene()
    image = bpy.data.images.new(
        "ForeachLightBakeTarget", width=16, height=16, alpha=True, float_buffer=True
    )
    tree = make_npr_tree("ForeachBakeTree", "overwrite")
    material = make_material("ForeachBakeMaterial", tree, bake_image=image)
    plane = make_plane(material, size=2.0)
    add_point_light("Bake Red Point", (1.0, 0.0, 0.0), use_shadow=True)
    add_sun_light("Bake Blue Sun", (0.0, 0.0, 1.0))

    bpy.ops.object.select_all(action="DESELECT")
    plane.select_set(True)
    bpy.context.view_layer.objects.active = plane
    scene.render.bake.target = "IMAGE_TEXTURES"
    scene.render.bake.use_clear = True
    bpy.context.view_layer.update()
    result = bpy.ops.object.bake(type="EMIT")
    require("FINISHED" in result, f"Eevee Color Bake returned {result}")

    pixels = list(image.pixels[:])
    require(len(pixels) == 16 * 16 * 4, f"Unexpected bake pixel count {len(pixels)}")
    require(all(math.isfinite(value) for value in pixels), "Color Bake produced non-finite pixels")
    index = ((16 // 2) * 16 + (16 // 2)) * 4
    center = tuple(pixels[index : index + 4])
    print(f"NPR_FOREACH_LIGHT_COLOR_BAKE={center}", flush=True)
    assert_channels("Color Bake no-culling", center, {2: 0.05}, {0: 0.02, 1: 0.02})


def main():
    global OUTPUT_DIR
    args = parse_args()
    OUTPUT_DIR = args.output_dir.resolve()
    test_progressive_lights(args.expected_backend)
    test_local_before_directional_order()
    test_second_local_light_word()
    test_shadow_mask_visibility()
    test_area_shadow_temporal_convergence()
    test_signed_npr_output_is_preserved_for_filter_consumers()
    test_signed_npr_screen_reflection_is_sanitized()
    test_color_bake_no_culling()
    print(SUCCESS_MARKER, flush=True)


if __name__ == "__main__":
    main()
