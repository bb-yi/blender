import math
import bpy
import os
import tempfile
from mathutils import Vector


RESOLUTION = 256
ORTHO_SCALE = 8.0
POSITION_TOLERANCE = 0.03
DIRECTION_TOLERANCE = 0.03
LIGHT_LOCATION = (0.2, 0.4, 0.6)

LIGHT_ACCUM_SOURCE = """
vec4 accumulate_lights(vec3 normal)
{
  vec3 n = normalize(normal);
  vec3 accum = vec3(0.0);
  int light_count = glsl_light_count();
  for (int i = 0; i < light_count; i++) {
    GLSLLight light = glsl_light_get(i);
    float diffuse = max(dot(n, light.vector), 0.0);
    float shadow = glsl_light_shadow(i, n);
    accum += light.diffuse_color * light.attenuation * diffuse * shadow;
  }
  return vec4(accum, 1.0);
}
"""

SPECULAR_ACCUM_SOURCE = """
vec4 accumulate_specular(vec3 normal)
{
  vec3 n = normalize(normal);
  vec3 v = vec3(0.0, 0.0, 1.0);
  vec3 accum = vec3(0.0);
  int light_count = glsl_light_count();
  for (int i = 0; i < light_count; i++) {
    GLSLLight light = glsl_light_get(i);
    vec3 reflection = reflect(-light.vector, n);
    float specular = pow(max(dot(reflection, v), 0.0), 24.0);
    float shadow = glsl_light_shadow(i, n);
    accum += light.specular_color * light.attenuation * specular * shadow;
  }
  return vec4(accum, 1.0);
}
"""

SHADOW_MASK_SOURCE = """
vec4 shadow_mask(vec3 normal)
{
  vec3 n = normalize(normal);
  float visibility_sum = 0.0;
  float weight_sum = 0.0;
  int light_count = glsl_light_count();
  for (int i = 0; i < light_count; i++) {
    GLSLLight light = glsl_light_get(i);
    float weight = max(light.diffuse_color.r, max(light.diffuse_color.g, light.diffuse_color.b));
    visibility_sum += glsl_light_shadow(i, n) * weight;
    weight_sum += weight;
  }
  float visibility = (weight_sum > 1e-6) ? (visibility_sum / weight_sum) : 0.0;
  return vec4(vec3(clamp(visibility, 0.0, 1.0)), 1.0);
}
"""

POSITION_SOURCE = """
vec4 debug_light_position()
{
  GLSLLight light = glsl_light_get(0);
  return vec4(light.position, 1.0);
}
"""

DIRECTION_SOURCE = """
vec4 debug_light_direction()
{
  GLSLLight light = glsl_light_get(0);
  return vec4(light.direction, 1.0);
}
"""

TYPE_SOURCE = """
vec4 debug_light_type()
{
  GLSLLight light = glsl_light_get(0);
  if (light.type == GLSL_LIGHT_TYPE_SUN) {
    return vec4(1.0, 0.0, 0.0, 1.0);
  }
  if (light.type == GLSL_LIGHT_TYPE_POINT) {
    return vec4(0.0, 1.0, 0.0, 1.0);
  }
  if (light.type == GLSL_LIGHT_TYPE_SPOT) {
    return vec4(0.0, 0.0, 1.0, 1.0);
  }
  if (light.type == GLSL_LIGHT_TYPE_AREA_RECT) {
    return vec4(1.0, 1.0, 0.0, 1.0);
  }
  if (light.type == GLSL_LIGHT_TYPE_AREA_ELLIPSE) {
    return vec4(0.0, 1.0, 1.0, 1.0);
  }
  return vec4(1.0, 0.0, 1.0, 1.0);
}
"""

COUNT_SOURCE = """
vec4 debug_light_count()
{
  float v = clamp(float(glsl_light_count()) / 4.0, 0.0, 1.0);
  return vec4(vec3(v), 1.0);
}
"""

DIFFUSE_COLOR_SOURCE = """
vec4 debug_light_diffuse_color()
{
  GLSLLight light = glsl_light_get(0);
  return vec4(clamp(light.diffuse_color, vec3(0.0), vec3(1.0)), 1.0);
}
"""

ATTENUATED_DIFFUSE_SOURCE = """
vec4 debug_attenuated_diffuse()
{
  GLSLLight light = glsl_light_get(0);
  return vec4(clamp(light.diffuse_color * light.attenuation, vec3(0.0), vec3(1.0)), 1.0);
}
"""

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


def make_plane(material):
    bpy.ops.mesh.primitive_plane_add(size=8.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.active_object
    plane.name = "GLSLLightPlane"
    plane.data.materials.append(material)
    return plane


def make_blocker():
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, 1.0))
    blocker = bpy.context.active_object
    blocker.name = "GLSLLightBlocker"
    blocker.scale = (0.5, 0.5, 1.0)
    return blocker


def make_point_light(name, location, color, energy=5000.0):
    light_data = bpy.data.lights.new(name, type="POINT")
    light_data.energy = energy
    light_data.shadow_soft_size = 0.05
    light_data.use_shadow = True
    light = bpy.data.objects.new(name, light_data)
    light.location = location
    light.data.color = color
    bpy.context.scene.collection.objects.link(light)
    return light


def find_node(tree, bl_idname):
    active = None
    first = None
    for node in tree.nodes:
        if node.bl_idname != bl_idname:
            continue
        first = first or node
        if getattr(node, "is_active_output", False):
            active = node
    return active or first


def clear_socket_links(tree, socket):
    for link in list(socket.links):
        tree.links.remove(link)


def configure_light_shader_output(
    light_object, color, intensity=1.0, attenuation=1.0, range_scale=1.0
):
    light = light_object.data
    light.use_nodes = True
    tree = light.node_tree
    assert tree is not None, "Expected light node tree to exist"
    output = find_node(tree, "ShaderNodeEeveeLightShaderOutput")
    assert output is not None, "Expected Eevee Light Shader Output node"

    for socket_name in ("Color", "Intensity", "Attenuation"):
        clear_socket_links(tree, output.inputs[socket_name])
    output.inputs["Color"].default_value = color
    output.inputs["Intensity"].default_value = intensity
    output.inputs["Attenuation"].default_value = attenuation
    if hasattr(output, "range_scale"):
        output.range_scale = range_scale
    bpy.context.view_layer.update()


def configure_light_shader_output_default_attenuation(light_object, color, range_scale=1.0):
    light = light_object.data
    light.use_nodes = True
    tree = light.node_tree
    assert tree is not None, "Expected light node tree to exist"
    output = find_node(tree, "ShaderNodeEeveeLightShaderOutput")
    assert output is not None, "Expected Eevee Light Shader Output node"
    info = find_node(tree, "ShaderNodeEeveeLightShaderInfo")
    assert info is not None, "Expected Eevee Light Shader Info node"

    for socket_name in ("Color", "Intensity", "Attenuation"):
        clear_socket_links(tree, output.inputs[socket_name])
    output.inputs["Color"].default_value = color
    output.inputs["Intensity"].default_value = 1.0
    if hasattr(output, "range_scale"):
        output.range_scale = range_scale
    tree.links.new(info.outputs["Default Attenuation"], output.inputs["Attenuation"])
    bpy.context.view_layer.update()


def make_light(light_type):
    light_data = bpy.data.lights.new(light_type, type=light_type)
    light_data.energy = 1000.0
    light_object = bpy.data.objects.new(light_type, light_data)

    if light_type == "SUN":
        light_object.location = (0.0, 0.0, 0.0)
    else:
        light_object.location = LIGHT_LOCATION

    if light_type in {"SPOT", "AREA", "SUN"}:
        light_object.rotation_euler = (0.0, 0.0, 0.0)

    if light_type == "AREA":
        light_data.shape = "RECTANGLE"
        light_data.size = 0.25
        light_data.size_y = 0.5
    if light_type == "SPOT":
        light_data.spot_size = math.radians(45.0)

    bpy.context.scene.collection.objects.link(light_object)
    return light_object


def make_text_block(name, source):
    text = bpy.data.texts.get(name)
    if text is None:
        text = bpy.data.texts.new(name)
    else:
        text.clear()
    text.write(source)
    return text


def refresh_glsl_node(node):
    current_name = node.function_name
    node.function_name = ""
    node.function_name = current_name
    node.id_data.interface_update(bpy.context)
    node.id_data.update_tag()
    bpy.context.view_layer.update()


def make_glsl_light_material(name, render_method, source_name, function_name, source):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    material.surface_render_method = render_method

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    output.location = (520.0, 0.0)

    emission = nodes.new("ShaderNodeEmission")
    emission.location = (320.0, 0.0)
    emission.inputs["Strength"].default_value = 1.0

    glsl = nodes.new("ShaderNodeGLSLFunction")
    glsl.location = (120.0, 0.0)

    geometry = nodes.new("ShaderNodeNewGeometry")
    geometry.location = (-120.0, -80.0)

    make_text_block(source_name, source)
    glsl.script = bpy.data.texts[source_name]
    glsl.function_name = function_name
    refresh_glsl_node(glsl)

    if "normal" in glsl.inputs:
        links.new(geometry.outputs["Normal"], glsl.inputs["normal"])
    links.new(glsl.outputs["Result"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_glsl_probe_material(name, source_name, function_name, source):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    material.surface_render_method = "DITHERED"

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    glsl = nodes.new("ShaderNodeGLSLFunction")

    make_text_block(source_name, source)
    glsl.script = bpy.data.texts[source_name]
    glsl.function_name = function_name
    refresh_glsl_node(glsl)

    links.new(glsl.outputs["Result"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_glsl_npr_material(name, source_name, function_name, source):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    material.surface_render_method = "DITHERED"

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    emission.inputs["Strength"].default_value = 1.0
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    npr_tree = bpy.data.node_groups.new(f"{name}NPRTree", "ShaderNodeTree")
    npr_nodes = npr_tree.nodes
    npr_links = npr_tree.links
    glsl = npr_nodes.new("ShaderNodeGLSLFunction")
    npr_output = npr_nodes.new("ShaderNodeNPR_Output")

    make_text_block(source_name, source)
    glsl.script = bpy.data.texts[source_name]
    glsl.function_name = function_name
    refresh_glsl_node(glsl)

    npr_links.new(glsl.outputs["Result"], npr_output.inputs["Color"])
    output.nprtree = npr_tree
    return material


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
    return list(pixels[index : index + 4])


def sample_center_rgb(pixels, width, height):
    x = width // 2
    y = height // 2
    index = (y * width + x) * 4
    return Vector(pixels[index : index + 3])


def assert_close(label, actual, expected, tolerance):
    delta = (actual - expected).length
    assert delta <= tolerance, (
        f"{label}: expected {tuple(round(v, 4) for v in expected)}, "
        f"got {tuple(round(v, 4) for v in actual)}, delta={delta:.5f}"
    )


def build_scene(material, with_blocker=False):
    clear_scene()
    configure_scene()
    make_camera()
    make_plane(material)
    if with_blocker:
        make_blocker()


def assert_near_black(color, label):
    assert max(color[:3]) < 0.03, f"{label}: expected near-black output, got {color}"


def assert_single_light_response(render_method):
    material = make_glsl_light_material(
        f"GLSLLight{render_method}",
        render_method,
        f"glsl_light_{render_method.lower()}.glsl",
        "accumulate_lights",
        LIGHT_ACCUM_SOURCE,
    )

    build_scene(material)
    make_point_light("KeyLight", (-4.0, 0.0, 4.0), (1.0, 1.0, 1.0))
    lit_pixels, lit_width, lit_height = render_image()
    lit = sample_world_point(lit_pixels, lit_width, lit_height, 0.0, 0.0)

    build_scene(material)
    dark_pixels, dark_width, dark_height = render_image()
    unlit = sample_world_point(dark_pixels, dark_width, dark_height, 0.0, 0.0)

    assert max(lit[:3]) > 0.08, f"{render_method}: expected visible lighting, got {lit}"
    assert max(lit[:3]) < 100.0, f"{render_method}: expected non-explosive lighting, got {lit}"
    assert max(lit[:3]) > max(unlit[:3]) + 0.05, (
        f"{render_method}: expected light response above no-light baseline, "
        f"got lit={lit} unlit={unlit}"
    )
    assert_near_black(unlit, f"{render_method} unlit")


def assert_multi_light_response():
    material = make_glsl_light_material(
        "GLSLLightMulti",
        "DITHERED",
        "glsl_light_multi.glsl",
        "accumulate_lights",
        LIGHT_ACCUM_SOURCE,
    )

    build_scene(material)
    make_point_light("RedLight", (-4.0, -1.5, 4.0), (1.0, 0.0, 0.0))
    make_point_light("GreenLight", (-4.0, 1.5, 4.0), (0.0, 1.0, 0.0))
    pixels, width, height = render_image()
    center = sample_world_point(pixels, width, height, 0.0, 0.0)

    assert center[0] > 0.05, f"Multi-light test expected red contribution, got {center}"
    assert center[1] > 0.05, f"Multi-light test expected green contribution, got {center}"
    assert center[2] < 0.03, f"Multi-light test expected low blue contribution, got {center}"
    assert max(center[:3]) < 100.0, f"Multi-light test expected non-explosive lighting, got {center}"


def assert_specular_light_response():
    material = make_glsl_light_material(
        "GLSLLightSpecular",
        "DITHERED",
        "glsl_light_specular.glsl",
        "accumulate_specular",
        SPECULAR_ACCUM_SOURCE,
    )

    build_scene(material)
    make_point_light("SpecularLight", (0.0, 0.0, 4.0), (1.0, 1.0, 1.0), energy=1000.0)
    lit_pixels, lit_width, lit_height = render_image()
    lit = sample_center_rgb(lit_pixels, lit_width, lit_height)

    build_scene(material)
    dark_pixels, dark_width, dark_height = render_image()
    unlit = sample_center_rgb(dark_pixels, dark_width, dark_height)

    assert lit.length > 0.08, f"Expected visible custom specular response, got {tuple(lit)}"
    assert max(lit) < 100.0, f"Expected non-explosive custom specular response, got {tuple(lit)}"
    assert lit.length > unlit.length + 0.05, (
        f"Expected specular response above no-light baseline, got lit={tuple(lit)} "
        f"unlit={tuple(unlit)}"
    )


def assert_shadow_response():
    material = make_glsl_light_material(
        "GLSLLightShadow",
        "DITHERED",
        "glsl_light_shadow.glsl",
        "shadow_mask",
        SHADOW_MASK_SOURCE,
    )

    build_scene(material, with_blocker=True)
    make_point_light("ShadowLight", (-4.0, 0.0, 4.0), (1.0, 1.0, 1.0))
    blocked_pixels, blocked_width, blocked_height = render_image()
    shadowed = sample_world_point(blocked_pixels, blocked_width, blocked_height, 1.4, 0.0)
    lit = sample_world_point(blocked_pixels, blocked_width, blocked_height, -1.4, 0.0)

    build_scene(material, with_blocker=False)
    make_point_light("ShadowLightClear", (-4.0, 0.0, 4.0), (1.0, 1.0, 1.0))
    clear_pixels, clear_width, clear_height = render_image()
    unblocked = sample_world_point(clear_pixels, clear_width, clear_height, 1.4, 0.0)

    assert lit[0] > 0.55, f"Shadow test expected lit side to stay bright, got {lit}"
    assert unblocked[0] > 0.55, f"Shadow test expected clear scene to stay bright, got {unblocked}"
    assert shadowed[0] < lit[0] - 0.15, (
        f"Shadow test expected blocker to reduce visibility, got shadowed={shadowed} lit={lit}"
    )
    assert shadowed[0] < unblocked[0] - 0.15, (
        "Shadow test expected blocked sample to be darker than the unblocked reference, "
        f"got shadowed={shadowed} unblocked={unblocked}"
    )


def assert_position_direction_fields():
    build_scene(
        make_glsl_probe_material(
            "CountProbe",
            "count_probe.glsl",
            "debug_light_count",
            COUNT_SOURCE,
        )
    )
    make_light("POINT")
    pixels, width, height = render_image()
    count_color = sample_center_rgb(pixels, width, height)
    assert_close("light_count", count_color, Vector((0.25, 0.25, 0.25)), 0.02)

    for light_type in ("POINT", "SPOT", "AREA", "SUN"):
        build_scene(
            make_glsl_probe_material(
                f"{light_type}_PositionProbe",
                f"{light_type.lower()}_position_probe.glsl",
                "debug_light_position",
                POSITION_SOURCE,
            )
        )
        light_object = make_light(light_type)
        pixels, width, height = render_image()
        position_color = sample_center_rgb(pixels, width, height)
        expected_position = Vector((0.0, 0.0, 0.0)) if light_type == "SUN" else Vector(light_object.location)
        assert_close(f"{light_type} position", position_color, expected_position, POSITION_TOLERANCE)

        build_scene(
            make_glsl_probe_material(
                f"{light_type}_DirectionProbe",
                f"{light_type.lower()}_direction_probe.glsl",
                "debug_light_direction",
                DIRECTION_SOURCE,
            )
        )
        light_object = make_light(light_type)
        pixels, width, height = render_image()
        direction_color = sample_center_rgb(pixels, width, height)
        expected_direction = Vector((0.0, 0.0, 0.0)) if light_type == "POINT" else (
            light_object.matrix_world.to_3x3() @ Vector((0.0, 0.0, 1.0))
        ).normalized()
        assert_close(f"{light_type} direction", direction_color, expected_direction, DIRECTION_TOLERANCE)


def assert_type_field():
    expected_colors = {
        "POINT": Vector((0.0, 1.0, 0.0)),
        "SPOT": Vector((0.0, 0.0, 1.0)),
        "AREA": Vector((1.0, 1.0, 0.0)),
        "SUN": Vector((1.0, 0.0, 0.0)),
    }

    for light_type in ("POINT", "SPOT", "AREA", "SUN"):
        build_scene(
            make_glsl_probe_material(
                f"{light_type}_TypeProbe",
                f"{light_type.lower()}_type_probe.glsl",
                "debug_light_type",
                TYPE_SOURCE,
            )
        )
        make_light(light_type)
        pixels, width, height = render_image()
        type_color = sample_center_rgb(pixels, width, height)
        assert_close(f"{light_type} type", type_color, expected_colors[light_type], 0.02)


def assert_light_shader_output_affects_glsl_light():
    material = make_glsl_probe_material(
        "LightShaderDiffuseProbe",
        "light_shader_diffuse_probe.glsl",
        "debug_light_diffuse_color",
        DIFFUSE_COLOR_SOURCE,
    )
    build_scene(material)
    red_light = make_point_light(
        "RedOutputLight", (-4.0, 0.0, 4.0), (1.0, 1.0, 1.0), energy=20.0
    )
    configure_light_shader_output(
        red_light, (1.0, 0.0, 0.0, 1.0), intensity=1.0, attenuation=1.0, range_scale=10.0
    )
    pixels, width, height = render_image()
    color = sample_center_rgb(pixels, width, height)

    assert color[0] > 0.05, f"Expected Light Shader Output red diffuse color, got {tuple(color)}"
    assert color[1] < 0.08 and color[2] < 0.08, (
        f"Expected Light Shader Output to suppress green/blue diffuse channels, got {tuple(color)}"
    )


def assert_light_shader_output_attenuation_affects_glsl_light():
    material = make_glsl_probe_material(
        "LightShaderAttenuationProbe",
        "light_shader_attenuation_probe.glsl",
        "debug_attenuated_diffuse",
        ATTENUATED_DIFFUSE_SOURCE,
    )

    build_scene(material)
    default_light = make_point_light(
        "DefaultOutputLight", (-4.0, 0.0, 4.0), (1.0, 1.0, 1.0), energy=2.0
    )
    configure_light_shader_output(
        default_light,
        (1.0, 1.0, 1.0, 1.0),
        intensity=1.0,
        attenuation=1.0,
        range_scale=10.0,
    )
    full_pixels, full_width, full_height = render_image()
    full = sample_center_rgb(full_pixels, full_width, full_height)

    build_scene(material)
    dim_light = make_point_light("DimOutputLight", (-4.0, 0.0, 4.0), (1.0, 1.0, 1.0), energy=2.0)
    configure_light_shader_output(
        dim_light,
        (1.0, 1.0, 1.0, 1.0),
        intensity=1.0,
        attenuation=0.2,
        range_scale=10.0,
    )
    dim_pixels, dim_width, dim_height = render_image()
    dim = sample_center_rgb(dim_pixels, dim_width, dim_height)

    assert full[0] > 0.03, f"Expected visible full attenuation reference, got {tuple(full)}"
    assert dim[0] < full[0] * 0.45, (
        f"Expected Light Shader Output attenuation to reduce GLSLLight attenuation, "
        f"got full={tuple(full)} dim={tuple(dim)}"
    )


def assert_light_shader_output_texture_affects_glsl_light():
    material = make_glsl_probe_material(
        "LightShaderTextureProbe",
        "light_shader_texture_probe.glsl",
        "debug_light_diffuse_color",
        DIFFUSE_COLOR_SOURCE,
    )
    build_scene(material)
    point_light = make_point_light(
        "PointDependentOutputLight", (-4.0, 0.0, 4.0), (1.0, 1.0, 1.0), energy=20.0
    )
    configure_light_shader_output_default_attenuation(
        point_light, (1.0, 0.0, 0.0, 1.0), range_scale=10.0
    )
    pixels, width, height = render_image()
    color = sample_center_rgb(pixels, width, height)

    assert color[0] > 0.05, (
        f"Expected GLSLLight diffuse color to read the front Light Shader Output texture, "
        f"got {tuple(color)}"
    )
    assert color[1] < 0.08 and color[2] < 0.08, (
        f"Expected front Light Shader Output texture to suppress green/blue channels, got {tuple(color)}"
    )


def assert_light_shader_output_affects_npr_glsl_light():
    material = make_glsl_npr_material(
        "NPRLightShaderDiffuseProbe",
        "npr_light_shader_diffuse_probe.glsl",
        "debug_light_diffuse_color",
        DIFFUSE_COLOR_SOURCE,
    )
    build_scene(material)
    red_light = make_point_light(
        "NPRRedOutputLight", (-4.0, 0.0, 4.0), (1.0, 1.0, 1.0), energy=20.0
    )
    configure_light_shader_output(
        red_light, (1.0, 0.0, 0.0, 1.0), intensity=1.0, attenuation=1.0, range_scale=10.0
    )
    pixels, width, height = render_image()
    color = sample_center_rgb(pixels, width, height)

    assert color[0] > 0.05, f"Expected NPR GLSLLight to read red Light Shader Output, got {tuple(color)}"
    assert color[1] < 0.08 and color[2] < 0.08, (
        f"Expected NPR GLSLLight to suppress green/blue Light Shader Output, got {tuple(color)}"
    )


def main():
    assert_single_light_response("DITHERED")
    assert_single_light_response("BLENDED")
    assert_multi_light_response()
    assert_specular_light_response()
    assert_shadow_response()
    assert_position_direction_fields()
    assert_type_field()
    assert_light_shader_output_affects_glsl_light()
    assert_light_shader_output_attenuation_affects_glsl_light()
    assert_light_shader_output_texture_affects_glsl_light()
    assert_light_shader_output_affects_npr_glsl_light()


if __name__ == "__main__":
    main()
