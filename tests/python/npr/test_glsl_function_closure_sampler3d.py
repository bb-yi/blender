# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import os
import time
from pathlib import Path

import bpy


RESOLUTION = 64
SAMPLE_POINTS = {
    "center": (32, 32),
    "left": (22, 32),
    "right": (42, 32),
    "bottom": (32, 22),
    "top": (32, 42),
    "corner": (4, 4),
}


def require(condition, message):
    if not condition:
        raise AssertionError(message)


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


def make_text_block(name, source):
    text = bpy.data.texts.get(name)
    if text is None:
        text = bpy.data.texts.new(name)
    else:
        text.clear()
    text.write(source)
    return text


def refresh_glsl_node(node):
    function_name = node.function_name
    node.function_name = ""
    node.function_name = function_name
    node.id_data.interface_update(bpy.context)
    node.id_data.update_tag()
    bpy.context.view_layer.update()


def find_socket(sockets, name):
    for socket in sockets:
        if socket.name == name or socket.identifier == name:
            return socket
    raise AssertionError(f"Socket {name!r} not found")


def add_centered_sdf_closure(nodes, links, *, smooth_union=False):
    closure_input = nodes.new("NodeClosureInput")
    closure_output = nodes.new("NodeClosureOutput")
    closure_input.pair_with_output(closure_output)
    closure_output.input_items.new("VECTOR", "UV")
    closure_output.output_items.new("FLOAT", "Color")

    center = nodes.new("ShaderNodeVectorMath")
    center.operation = "SUBTRACT"
    center.inputs[1].default_value = (0.5, 0.5, 0.5)
    links.new(closure_input.outputs["UV"], center.inputs[0])

    if not smooth_union:
        radius = nodes.new("ShaderNodeValue")
        radius.outputs["Value"].default_value = 0.30
        sphere = nodes.new("ShaderNodeSdfPrimitive")
        sphere.mode = "SPHERE_3D"
        sphere.inputs["Size"].default_value = 1.0
        links.new(center.outputs["Vector"], sphere.inputs["Vector"])
        links.new(radius.outputs["Value"], sphere.inputs["Radius"])
        links.new(sphere.outputs["Distance"], closure_output.inputs["Color"])
    else:
        radius = nodes.new("ShaderNodeValue")
        radius.outputs["Value"].default_value = 0.22
        sphere_outputs = []
        for index, offset in enumerate(((0.16, 0.0, 0.0), (-0.16, 0.0, 0.0))):
            translate = nodes.new("ShaderNodeVectorMath")
            translate.name = f"Union Coordinate {index}"
            translate.operation = "ADD"
            translate.inputs[1].default_value = offset
            sphere = nodes.new("ShaderNodeSdfPrimitive")
            sphere.mode = "SPHERE_3D"
            sphere.inputs["Size"].default_value = 1.0
            links.new(center.outputs["Vector"], translate.inputs[0])
            links.new(translate.outputs["Vector"], sphere.inputs["Vector"])
            links.new(radius.outputs["Value"], sphere.inputs["Radius"])
            sphere_outputs.append(sphere.outputs["Distance"])

        union = nodes.new("ShaderNodeSdfOp")
        union.operation = "UNION_SMOOTH"
        union.inputs["Value"].default_value = 0.10
        links.new(sphere_outputs[0], union.inputs[0])
        links.new(sphere_outputs[1], union.inputs[1])
        links.new(union.outputs["Distance"], closure_output.inputs["Color"])

    return closure_output


def add_constant_sample_closure(
    nodes,
    links,
    name,
    color,
    *,
    color_type="RGBA",
    link_color=True,
    alpha=None,
    alpha_default=None,
):
    closure_input = nodes.new("NodeClosureInput")
    closure_input.name = f"{name} Input"
    closure_output = nodes.new("NodeClosureOutput")
    closure_output.name = f"{name} Output"
    closure_input.pair_with_output(closure_output)
    closure_output.input_items.new("VECTOR", "UV")
    closure_output.output_items.new(color_type, "Color")

    if not link_color:
        closure_output.inputs["Color"].default_value = color
    elif color_type == "RGBA":
        color_node = nodes.new("ShaderNodeRGB")
        color_node.name = f"{name} Color"
        color_node.outputs["Color"].default_value = color
        links.new(color_node.outputs["Color"], closure_output.inputs["Color"])
    else:
        color_node = nodes.new("ShaderNodeCombineXYZ")
        color_node.name = f"{name} Color"
        for socket, value in zip(color_node.inputs, color[:3]):
            socket.default_value = value
        links.new(color_node.outputs["Vector"], closure_output.inputs["Color"])

    if alpha is not None or alpha_default is not None:
        closure_output.output_items.new("FLOAT", "Alpha")
    if alpha_default is not None:
        closure_output.inputs["Alpha"].default_value = alpha_default
    if alpha is not None:
        alpha_node = nodes.new("ShaderNodeValue")
        alpha_node.name = f"{name} Alpha"
        alpha_node.outputs["Value"].default_value = alpha
        links.new(alpha_node.outputs["Value"], closure_output.inputs["Alpha"])
    return closure_output


def make_sampler3d_material(name, source, function_name, *, smooth_union=False, use_uv=False):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    glsl = nodes.new("ShaderNodeGLSLFunction")
    glsl.source_mode = "INTERNAL"
    glsl.script = make_text_block(f"{name}.glsl", source)
    glsl.function_name = function_name
    closure_output = add_centered_sdf_closure(nodes, links, smooth_union=smooth_union)

    material.node_tree.interface_update(bpy.context)
    links.new(closure_output.outputs["Closure"], find_socket(glsl.inputs, "In_sdf_volume"))
    if use_uv:
        uv_map = nodes.new("ShaderNodeUVMap")
        links.new(uv_map.outputs["UV"], find_socket(glsl.inputs, "uv"))
    links.new(find_socket(glsl.outputs, "Result"), emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    refresh_glsl_node(glsl)
    require(glsl.parse_status == "READY", f"{name}: {glsl.parse_status}")
    return material, glsl


def make_lut_strip_material():
    material = bpy.data.materials.new("Sampler3DLutStripRegression")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    glsl = nodes.new("ShaderNodeGLSLFunction")
    image_node = nodes.new("ShaderNodeImageToClosure")
    image_node.texture_type = "LUT_STRIP_3D"
    image = bpy.data.images.new(
        "Sampler3DLutStripImage", 16, 4, alpha=True, float_buffer=True
    )
    image.pixels = [0.2, 0.6, 0.9, 1.0] * (16 * 4)
    image_node.image = image

    source = (
        "vec4 sample_lut(sampler3D sdf_volume){\n"
        "  return texture(sdf_volume, vec3(0.5, 0.5, 0.5));\n"
        "}\n"
    )
    glsl.source_mode = "INTERNAL"
    glsl.script = make_text_block("Sampler3DLutStripRegression.glsl", source)
    glsl.function_name = "sample_lut"
    material.node_tree.interface_update(bpy.context)
    links.new(image_node.outputs["Closure"], find_socket(glsl.inputs, "In_sdf_volume"))
    links.new(find_socket(glsl.outputs, "Result"), emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    refresh_glsl_node(glsl)
    require(glsl.parse_status == "READY", f"LUT Strip parse failed: {glsl.parse_status}")
    return material


def make_unsupported_helper_material():
    material = bpy.data.materials.new("Sampler3DUnsupportedHelper")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    glsl = nodes.new("ShaderNodeGLSLFunction")
    closure_input = nodes.new("NodeClosureInput")
    closure_output = nodes.new("NodeClosureOutput")
    closure_input.pair_with_output(closure_output)
    closure_output.input_items.new("VECTOR", "UV")
    closure_output.output_items.new("FLOAT", "Color")
    unsupported = nodes.new("ShaderNodeTexIES")
    links.new(closure_input.outputs["UV"], unsupported.inputs["Vector"])
    links.new(unsupported.outputs["Factor"], closure_output.inputs["Color"])

    source = (
        "vec4 sample_unsupported(sampler3D sdf_volume){\n"
        "  return texture(sdf_volume, vec3(0.5));\n"
        "}\n"
    )
    glsl.source_mode = "INTERNAL"
    glsl.script = make_text_block("Sampler3DUnsupportedHelper.glsl", source)
    glsl.function_name = "sample_unsupported"
    material.node_tree.interface_update(bpy.context)
    links.new(closure_output.outputs["Closure"], find_socket(glsl.inputs, "In_sdf_volume"))
    links.new(find_socket(glsl.outputs, "Result"), emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    refresh_glsl_node(glsl)
    require(glsl.parse_status == "READY", f"Unsupported helper parser setup failed: {glsl.parse_status}")
    return material


def make_mixed_closure_sampler_material():
    material = bpy.data.materials.new("MixedClosureSamplerDimensions")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    glsl = nodes.new("ShaderNodeGLSLFunction")

    closure_2d_input = nodes.new("NodeClosureInput")
    closure_2d_output = nodes.new("NodeClosureOutput")
    closure_2d_input.pair_with_output(closure_2d_output)
    closure_2d_output.input_items.new("VECTOR", "UV")
    closure_2d_output.output_items.new("FLOAT", "Color")
    separate = nodes.new("ShaderNodeSeparateXYZ")
    links.new(closure_2d_input.outputs["UV"], separate.inputs["Vector"])
    links.new(separate.outputs["X"], closure_2d_output.inputs["Color"])

    closure_3d_output = add_centered_sdf_closure(nodes, links)
    source = (
        "vec4 sample_mixed(sampler2D image, sampler3D sdf_volume){\n"
        "  float image_value = texture(image, vec2(0.25, 0.75)).r;\n"
        "  float volume_value = texture(sdf_volume, vec3(0.5)).r;\n"
        "  return vec4(0.5 + image_value, 0.5 + volume_value, 0.2, 1.0);\n"
        "}\n"
    )
    glsl.source_mode = "INTERNAL"
    glsl.script = make_text_block("MixedClosureSamplerDimensions.glsl", source)
    glsl.function_name = "sample_mixed"
    material.node_tree.interface_update(bpy.context)
    links.new(closure_2d_output.outputs["Closure"], find_socket(glsl.inputs, "image"))
    links.new(closure_3d_output.outputs["Closure"], find_socket(glsl.inputs, "In_sdf_volume"))
    links.new(find_socket(glsl.outputs, "Result"), emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    refresh_glsl_node(glsl)
    require(glsl.parse_status == "READY", f"Mixed helper parse failed: {glsl.parse_status}")
    return material


def make_closure_sampler_alpha_material(alpha_mode):
    suffix = alpha_mode.title()
    material = bpy.data.materials.new(f"ClosureSamplerAlpha{suffix}")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    glsl = nodes.new("ShaderNodeGLSLFunction")
    closure_2d = add_constant_sample_closure(
        nodes,
        links,
        "Sampler 2D",
        (0.2, 0.4, 0.5, 0.8),
        link_color=alpha_mode != "numeric",
        alpha=0.35 if alpha_mode == "linked" else None,
        alpha_default=(
            0.35 if alpha_mode == "numeric" else (1.0 if alpha_mode == "default" else None)
        ),
    )
    closure_3d = add_constant_sample_closure(
        nodes,
        links,
        "Sampler 3D",
        (0.1, 0.3, 0.6, 0.9),
        color_type="RGBA" if alpha_mode != "absent" else "VECTOR",
        link_color=alpha_mode != "numeric",
        alpha=0.65 if alpha_mode == "linked" else None,
        alpha_default=(
            0.65 if alpha_mode == "numeric" else (1.0 if alpha_mode == "default" else None)
        ),
    )
    source = (
        "vec3 sample_closure_alpha(sampler2D image, sampler3D volume){\n"
        "  vec4 image_sample = texture(image, vec2(0.25, 0.75));\n"
        "  vec4 volume_sample = texture(volume, vec3(0.5));\n"
        "  return vec3(image_sample.a, volume_sample.a, image_sample.r + volume_sample.b);\n"
        "}\n"
    )
    glsl.source_mode = "INTERNAL"
    glsl.script = make_text_block(f"ClosureSamplerAlpha{suffix}.glsl", source)
    glsl.function_name = "sample_closure_alpha"
    material.node_tree.interface_update(bpy.context)
    links.new(closure_2d.outputs["Closure"], find_socket(glsl.inputs, "image"))
    links.new(closure_3d.outputs["Closure"], find_socket(glsl.inputs, "volume"))
    links.new(find_socket(glsl.outputs, "Result"), emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    refresh_glsl_node(glsl)
    require(glsl.parse_status == "READY", f"Alpha helper parse failed: {glsl.parse_status}")
    return material


def build_plane(material):
    bpy.ops.mesh.primitive_plane_add(size=2.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.object
    plane.data.materials.append(material)

    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 2.0
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 2.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera
    return plane


def output_path(name):
    workspace_root = Path(
        os.environ.get("BLENDER_NPR_WORKSPACE_ROOT", Path(__file__).resolve().parents[4])
    )
    directory = workspace_root / "temp" / "render_exports"
    directory.mkdir(parents=True, exist_ok=True)
    return directory / f"{name}.exr"


def render_pixels(name):
    scene = bpy.context.scene
    filepath = output_path(name)
    scene.render.image_settings.file_format = "OPEN_EXR"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "32"
    scene.render.filepath = str(filepath)

    start = time.perf_counter()
    bpy.ops.render.render(write_still=False)
    elapsed = time.perf_counter() - start
    bpy.data.images["Render Result"].save_render(str(filepath))

    image = bpy.data.images.load(str(filepath), check_existing=False)
    try:
        return list(image.pixels), image.size[0], image.size[1], elapsed
    finally:
        bpy.data.images.remove(image)


def sample_pixel(pixels, width, x, y):
    index = (y * width + x) * 4
    return list(pixels[index : index + 4])


def test_three_dimensional_coordinate_probe():
    clear_scene()
    source = (
        "/* @glsl_meta v1\n"
        "sdf_volume: label=\"3D Sample\" description=\"Procedural Closure source\"\n"
        "coordinate: label=\"Coordinate\" default=vec3(0.5)\n"
        "*/\n"
        "vec4 sample_sampler3d(sampler3D sdf_volume, vec3 coordinate){\n"
        "  return texture(sdf_volume, coordinate);\n"
        "}\n"
    )
    material, glsl = make_sampler3d_material(
        "ClosureSampler3DProbe", source, "sample_sampler3d"
    )
    build_plane(material)
    coordinate = find_socket(glsl.inputs, "In_coordinate")
    samples = {}
    elapsed_total = 0.0
    for name, z in (("low", 0.0), ("middle", 0.5), ("high", 1.0)):
        coordinate.default_value = (0.5, 0.5, z)
        pixels, width, height, elapsed = render_pixels(f"glsl_closure_sampler3d_probe_{name}")
        samples[name] = sample_pixel(pixels, width, width // 2, height // 2)
        elapsed_total += elapsed

    low = samples["low"][0]
    middle = samples["middle"][0]
    high = samples["high"][0]
    require(abs(low - high) < 0.04, f"Symmetric Z probes diverged: {samples}")
    require(low - middle > 0.15, f"Coordinate input Z did not change 3D sample: {samples}")
    print(f"GLSL_CLOSURE_SAMPLER3D_PROBE={samples} render_seconds={elapsed_total:.3f}")


def test_sphere_raymarch_and_normals():
    clear_scene()
    source = r"""
float sample_sdf(sampler3D sdf_volume, vec3 position)
{
  return texture(sdf_volume, position).r;
}

vec3 sample_normal(sampler3D sdf_volume, vec3 position)
{
  float epsilon = 0.002;
  vec2 e = vec2(epsilon, 0.0);
  return normalize(vec3(
    sample_sdf(sdf_volume, position + e.xyy) - sample_sdf(sdf_volume, position - e.xyy),
    sample_sdf(sdf_volume, position + e.yxy) - sample_sdf(sdf_volume, position - e.yxy),
    sample_sdf(sdf_volume, position + e.yyx) - sample_sdf(sdf_volume, position - e.yyx)));
}

vec4 raymarch_sdf(sampler3D sdf_volume, vec3 uv)
{
  vec3 ray_origin = vec3(uv.xy, 0.0);
  vec3 ray_direction = vec3(0.0, 0.0, 1.0);
  float travel = 0.0;
  bool hit = false;
  vec3 position = ray_origin;
  for (int step_index = 0; step_index < 128; step_index++) {
    position = ray_origin + ray_direction * travel;
    float distance_to_surface = sample_sdf(sdf_volume, position);
    if (abs(distance_to_surface) < 0.0015) {
      hit = true;
      break;
    }
    travel += max(distance_to_surface * 0.8, 0.00075);
    if (travel > 1.25) {
      break;
    }
  }
  if (!hit) {
    return vec4(0.0, 0.0, 0.0, 1.0);
  }
  vec3 normal = sample_normal(sdf_volume, position);
  return vec4(normal * 0.5 + 0.5, 1.0);
}
"""
    material, _ = make_sampler3d_material(
        "ClosureSampler3DRaymarch", source, "raymarch_sdf", use_uv=True
    )
    build_plane(material)
    pixels, width, _, elapsed = render_pixels("glsl_closure_sampler3d_raymarch")
    samples = {
        name: sample_pixel(pixels, width, x, y) for name, (x, y) in SAMPLE_POINTS.items()
    }

    require(sum(samples["center"][:3]) > 0.7, f"Sphere center was not hit: {samples}")
    require(sum(samples["corner"][:3]) < 0.08, f"Sphere miss area was not black: {samples}")
    require(
        samples["right"][0] - samples["left"][0] > 0.18,
        f"X normal did not vary across the sphere: {samples}",
    )
    require(
        samples["top"][1] - samples["bottom"][1] > 0.18,
        f"Y normal did not vary across the sphere: {samples}",
    )
    print(f"GLSL_CLOSURE_SAMPLER3D_RAYMARCH={samples} render_seconds={elapsed:.3f}")


def test_smooth_union_programmatic_graph():
    clear_scene()
    source = (
        "vec4 sample_union(sampler3D sdf_volume){\n"
        "  float distance_value = texture(sdf_volume, vec3(0.5, 0.5, 0.5)).r;\n"
        "  return vec4(vec3(0.5 + distance_value), 1.0);\n"
        "}\n"
    )
    material, _ = make_sampler3d_material(
        "ClosureSampler3DSmoothUnion", source, "sample_union", smooth_union=True
    )
    build_plane(material)
    pixels, width, height, elapsed = render_pixels("glsl_closure_sampler3d_smooth_union")
    center = sample_pixel(pixels, width, width // 2, height // 2)
    require(0.05 < center[0] < 0.8, f"Smooth Union helper produced invalid output: {center}")
    require(max(center[:3]) - min(center[:3]) < 0.02, f"Expected grayscale union probe: {center}")
    print(f"GLSL_CLOSURE_SAMPLER3D_UNION={center} render_seconds={elapsed:.3f}")


def test_sampler3d_lut_strip_regression():
    clear_scene()
    material = make_lut_strip_material()
    build_plane(material)
    pixels, width, height, elapsed = render_pixels("glsl_sampler3d_lut_strip_regression")
    center = sample_pixel(pixels, width, width // 2, height // 2)
    require(abs(center[0] - 0.2) < 0.05, f"sampler3D LUT red mismatch: {center}")
    require(abs(center[1] - 0.6) < 0.05, f"sampler3D LUT green mismatch: {center}")
    require(abs(center[2] - 0.9) < 0.05, f"sampler3D LUT blue mismatch: {center}")
    print(f"GLSL_SAMPLER3D_LUT_STRIP={center} render_seconds={elapsed:.3f}")


def test_unsupported_helper_node_fails_readably():
    clear_scene()
    material = make_unsupported_helper_material()
    build_plane(material)
    pixels, width, height, elapsed = render_pixels("glsl_sampler3d_unsupported_helper")
    center = sample_pixel(pixels, width, width // 2, height // 2)
    require(sum(abs(channel) for channel in center[:3]) < 0.02, f"Unsupported helper was not rejected: {center}")
    print(f"GLSL_SAMPLER3D_UNSUPPORTED_FALLBACK={center} render_seconds={elapsed:.3f}")


def test_mixed_closure_sampler_dimensions():
    clear_scene()
    material = make_mixed_closure_sampler_material()
    build_plane(material)
    pixels, width, height, elapsed = render_pixels("glsl_mixed_closure_sampler_dimensions")
    center = sample_pixel(pixels, width, width // 2, height // 2)
    require(abs(center[0] - 0.75) < 0.05, f"sampler2D helper coordinate mismatch: {center}")
    require(abs(center[1] - 0.20) < 0.05, f"sampler3D helper coordinate mismatch: {center}")
    require(abs(center[2] - 0.20) < 0.05, f"Mixed helper blue channel mismatch: {center}")
    print(f"GLSL_MIXED_CLOSURE_SAMPLERS={center} render_seconds={elapsed:.3f}")


def test_closure_sampler_alpha_channels():
    cases = (
        ("linked", (0.28, 0.585, 0.80)),
        ("numeric", (0.28, 0.585, 0.80)),
        ("default", (0.80, 0.90, 0.80)),
        ("absent", (0.80, 1.00, 0.80)),
    )
    samples = {}
    elapsed_total = 0.0
    for name, expected in cases:
        clear_scene()
        material = make_closure_sampler_alpha_material(name)
        build_plane(material)
        pixels, width, height, elapsed = render_pixels(f"glsl_closure_sampler_alpha_{name}")
        center = sample_pixel(pixels, width, width // 2, height // 2)
        for channel, actual, target in zip("RGB", center, expected):
            require(
                abs(actual - target) < 0.05,
                f"Closure sampler alpha {name} {channel} mismatch: {center}",
            )
        samples[name] = center
        elapsed_total += elapsed
    print(f"GLSL_CLOSURE_SAMPLER_ALPHA={samples} render_seconds={elapsed_total:.3f}")


def test_closure_sampler_numeric_updates():
    clear_scene()
    material = make_closure_sampler_alpha_material("numeric")
    build_plane(material)

    pixels, width, height, _elapsed = render_pixels("glsl_closure_sampler_uniform_initial")
    initial = sample_pixel(pixels, width, width // 2, height // 2)

    output_2d = material.node_tree.nodes["Sampler 2D Output"]
    output_3d = material.node_tree.nodes["Sampler 3D Output"]
    output_2d.inputs["Color"].default_value = (0.25, 0.4, 0.5, 0.6)
    output_2d.inputs["Alpha"].default_value = 0.5
    output_3d.inputs["Color"].default_value = (0.1, 0.3, 0.6, 0.4)
    output_3d.inputs["Alpha"].default_value = 0.25
    material.node_tree.update_tag()
    bpy.context.view_layer.update()

    pixels, width, height, _elapsed = render_pixels("glsl_closure_sampler_uniform_updated")
    updated = sample_pixel(pixels, width, width // 2, height // 2)

    for channel, actual, target in zip("RGB", initial, (0.28, 0.585, 0.80)):
        require(
            abs(actual - target) < 0.05,
            f"Initial numeric Closure sampler {channel} mismatch: {initial}",
        )
    for channel, actual, target in zip("RGB", updated, (0.30, 0.10, 0.85)):
        require(
            abs(actual - target) < 0.05,
            f"Updated numeric Closure sampler {channel} mismatch: {updated}",
        )
    print(f"GLSL_CLOSURE_SAMPLER_UNIFORM_UPDATE={initial}->{updated}")


def main():
    bpy.ops.wm.read_homefile(use_factory_startup=True)
    configure_scene()
    test_three_dimensional_coordinate_probe()
    test_sphere_raymarch_and_normals()
    test_smooth_union_programmatic_graph()
    test_sampler3d_lut_strip_regression()
    test_unsupported_helper_node_fails_readably()
    test_mixed_closure_sampler_dimensions()
    test_closure_sampler_alpha_channels()
    test_closure_sampler_numeric_updates()


if __name__ == "__main__":
    main()
