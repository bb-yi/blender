import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

import bpy
import gpu


CASE_DIR = Path(__file__).resolve().parent
OUT_DIR = CASE_DIR / "out"
BACKEND_MARKER = "GLSL_TYPED_CLOSURE_BACKEND="
SUCCESS_MARKER = "GLSL_TYPED_CLOSURE_CALLBACK_OK"
CHANNEL_TOLERANCE = 0.01

CONNECTED_EXPECTED = (0.300, 0.650, 0.225)
FALLBACK_EXPECTED = (0.020, 0.066, 0.500)

TYPED_CONNECTED_EXPECTED = (0.910, 0.518, 0.448)
TYPED_NESTED_FALLBACK_EXPECTED = (0.358, 0.1876, 0.1184)
TYPED_FALLBACK_EXPECTED = (0.350, 0.305375, 0.0635625)
INT_BOOL_CONNECTED_EXPECTED = (1.0, 1.0, 0.0)
INT_BOOL_FALLBACK_EXPECTED = (0.0, 0.0, 0.0)
TYPED_MATERIAL_NAME = "ZZ919TypedTransportMaterial"
INT_BOOL_MATERIAL_NAME = "ZZ919IntBoolTransportMaterial"
TYPED_INPUT_KEYS = ("uv", "tint", "payload", "payload.__w")
TYPED_OUTPUT_KEYS = (
    "Result",
    "Result.__w",
    "out_uv",
    "out_tint",
    "out_packed",
    "out_packed.__w",
)

CALLBACK_SOURCE = """/* @glsl_closure v1 label="Scalar Override" \
description="Override scalar remap"
value: label="Value"
gain: label="Gain"
Result: label="Remapped"
mask: label="Mask"
*/
float remap(float value, float gain, out float mask)
{
  mask = gain;
  return value * gain;
}

vec3 typed_closure_probe()
{
  float first_mask;
  float second_mask;
  float first = remap(0.1, 0.2, first_mask);
  float second = remap(first + first_mask, 0.3, second_mask);
  return vec3(first, second, first_mask + second_mask);
}
"""

TYPED_CALLBACK_SOURCE = """/* @glsl_closure v1 label="Typed Transport" \
description="Override typed vector, color, and vec4 transport"
uv: label="UV Pair"
tint: label="Tint" subtype=color
payload: label="Packed"
Result: label="Transport Result"
out_uv: label="Output UV"
out_tint: label="Output Tint" subtype=color
out_packed: label="Output Packed"
*/
vec4 typed_transport(
    vec2 uv,
    vec3 tint,
    vec4 payload,
    out vec2 out_uv,
    out vec3 out_tint,
    out vec4 out_packed)
{
  out_uv = uv.yx;
  out_tint = tint * 0.25;
  out_packed = vec4(payload.xyz * 0.25, payload.w * 0.5);
  return vec4(uv, tint.r, payload.w * 0.25);
}

vec3 typed_transport_probe()
{
  vec2 first_uv;
  vec3 first_tint;
  vec4 first_packed;
  vec4 first = typed_transport(
      vec2(0.1, 0.2),
      vec3(0.3, 0.4, 0.5),
      vec4(0.6, 0.7, 0.8, 0.9),
      first_uv,
      first_tint,
      first_packed);

  vec2 second_uv;
  vec3 second_tint;
  vec4 second_packed;
  vec4 second = typed_transport(
      first_uv.yx,
      first_tint * 0.5,
      first_packed * vec4(0.5, 0.4, 0.3, 0.2),
      second_uv,
      second_tint,
      second_packed);

  return vec3(
      first.x + first.w * 0.1 + first_uv.x + first_tint.x * 0.1 + first_packed.z * 0.1,
      second.y + second.w * 0.1 + second_uv.y + second_tint.y * 0.1 +
          second_packed.x * 0.1,
      second.z + second_uv.x * 0.1 + second_tint.z * 0.1 + second_packed.w * 0.1);
}
"""

NESTED_CALLBACK_SOURCE = """/* @glsl_closure v1 label="Nested Blend" \
description="Nested callback used by the typed transport zone"
value: label="Value"
bias: label="Bias" subtype=color
Result: label="Nested Result"
*/
vec3 inner_blend(vec3 value, vec3 bias)
{
  return value * 0.1 + bias * 0.2;
}

/* @glsl_meta v1
value: label="Value"
bias: label="Bias" subtype=color
*/
vec3 nested_transport(vec3 value, vec3 bias)
{
  return inner_blend(value, bias);
}
"""

INT_BOOL_CALLBACK_SOURCE = """/* @glsl_closure v1 label="Exact Int Bool Transport"
value: label="Value"
enabled: label="Enabled"
Result: label="Result"
out_enabled: label="Output Enabled"
*/
int exact_transport(int value, bool enabled, out bool out_enabled)
{
  out_enabled = !enabled;
  return value - 1;
}

vec3 exact_transport_probe()
{
  bool out_enabled;
  int result = exact_transport(16777216, true, out_enabled);
  return vec3(float(result - 16777215), out_enabled ? 1.0 : 0.0, 0.0);
}
"""

ERROR_PATTERNS = (
    (
        "GPU shader diagnostic",
        re.compile(r"gpu\.shader\s+\|\s+ERROR", re.IGNORECASE),
    ),
    (
        "shader compilation failure",
        re.compile(
            r"shader.*compil.*(?:error|fail)|compil.*shader.*(?:error|fail)",
            re.IGNORECASE,
        ),
    ),
    (
        "GPU validation failure",
        re.compile(r"validation (?:error|failed)|validation failed", re.IGNORECASE),
    ),
    ("Vulkan device loss", re.compile(r"VK_ERROR_DEVICE_LOST", re.IGNORECASE)),
    ("native access violation", re.compile(r"EXCEPTION_ACCESS_VIOLATION", re.IGNORECASE)),
    ("Blender crash", re.compile(r"Blender crashed", re.IGNORECASE)),
    (
        "GPU memory leak",
        re.compile(r"Not freed memory blocks:\s*[1-9]\d*|GPUNodeLink len", re.IGNORECASE),
    ),
)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def find_socket(sockets, name_or_identifier):
    for socket in sockets:
        if socket.name == name_or_identifier or socket.identifier == name_or_identifier:
            return socket
    raise AssertionError(f"Socket {name_or_identifier!r} not found")


def update_tree(tree):
    tree.interface_update(bpy.context)
    tree.update_tag()
    bpy.context.view_layer.update()


def configure_glsl_node(node, text, function_name):
    node.source_mode = "INTERNAL"
    node.script = text
    node.function_name = function_name
    node.function_name = ""
    node.function_name = function_name
    update_tree(node.id_data)


def sync_closure_output(tree, closure_output):
    screen = bpy.context.screen
    require(screen is not None and screen.areas, "No screen area available for Closure sync")
    area = screen.areas[0]
    previous_area_type = area.type
    try:
        area.type = "NODE_EDITOR"
        region = next((item for item in area.regions if item.type == "WINDOW"), None)
        require(region is not None, "No node editor window region available for Closure sync")
        space = area.spaces.active
        space.tree_type = "ShaderNodeTree"
        space.node_tree = tree
        for node in tree.nodes:
            node.select = False
        tree.nodes.active = closure_output
        closure_output.select = True
        with bpy.context.temp_override(
            area=area,
            region=region,
            space_data=space,
            node=closure_output,
            active_node=closure_output,
            edit_tree=tree,
            node_tree=tree,
        ):
            result = bpy.ops.node.sockets_sync(node_name=closure_output.name)
        require(result == {"FINISHED"}, f"Closure socket sync failed: {result}")
    finally:
        area.type = previous_area_type
    update_tree(tree)


def set_if_available(owner, name, value):
    if hasattr(owner, name):
        setattr(owner, name, value)


def select_eevee_engine(scene):
    engines = {item.identifier for item in scene.render.bl_rna.properties["engine"].enum_items}
    for engine in ("BLENDER_EEVEE", "BLENDER_EEVEE_NEXT"):
        if engine in engines:
            scene.render.engine = engine
            return engine
    raise AssertionError(f"No Eevee render engine found in {sorted(engines)}")


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()
    for collection in (
        bpy.data.meshes,
        bpy.data.materials,
        bpy.data.cameras,
        bpy.data.worlds,
        bpy.data.node_groups,
        bpy.data.texts,
    ):
        for datablock in list(collection):
            if datablock.users == 0:
                collection.remove(datablock)


def configure_scene():
    clear_scene()
    scene = bpy.context.scene
    engine = select_eevee_engine(scene)
    scene.render.resolution_x = 32
    scene.render.resolution_y = 32
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "OPEN_EXR"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "32"
    scene.render.film_transparent = False
    scene.render.use_compositing = False
    scene.render.use_sequencer = False
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0

    set_if_available(scene.eevee, "taa_samples", 1)
    set_if_available(scene.eevee, "taa_render_samples", 1)
    set_if_available(scene.eevee, "use_taa_reprojection", False)
    set_if_available(scene.eevee, "use_raytracing", False)
    set_if_available(scene.eevee, "use_shadows", False)
    set_if_available(scene.eevee, "use_outline", False)

    world = bpy.data.worlds.new("Typed Closure World")
    world.use_nodes = True
    world_nodes = world.node_tree.nodes
    world_nodes.clear()
    background = world_nodes.new("ShaderNodeBackground")
    background.inputs["Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    background.inputs["Strength"].default_value = 0.0
    world_output = world_nodes.new("ShaderNodeOutputWorld")
    world.node_tree.links.new(background.outputs["Background"], world_output.inputs["Surface"])
    scene.world = world

    bpy.ops.object.camera_add(location=(0.0, 0.0, 3.0))
    camera = bpy.context.object
    camera.name = "Typed Closure Camera"
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = 2.0
    scene.camera = camera
    return scene, engine


def make_callback_group():
    group = bpy.data.node_groups.new("Typed Closure Arithmetic", "ShaderNodeTree")
    group.interface.new_socket(name="Value", in_out="INPUT", socket_type="NodeSocketFloat")
    group.interface.new_socket(name="Gain", in_out="INPUT", socket_type="NodeSocketFloat")
    group.interface.new_socket(name="Result", in_out="OUTPUT", socket_type="NodeSocketFloat")
    group.interface.new_socket(name="Mask", in_out="OUTPUT", socket_type="NodeSocketFloat")
    group.interface_update(bpy.context)

    nodes = group.nodes
    links = group.links
    group_input = nodes.new("NodeGroupInput")
    group_output = nodes.new("NodeGroupOutput")
    add = nodes.new("ShaderNodeMath")
    add.operation = "ADD"
    multiply = nodes.new("ShaderNodeMath")
    multiply.operation = "MULTIPLY"

    links.new(group_input.outputs["Value"], add.inputs[0])
    links.new(group_input.outputs["Gain"], add.inputs[1])
    links.new(group_input.outputs["Value"], multiply.inputs[0])
    links.new(group_input.outputs["Gain"], multiply.inputs[1])
    links.new(add.outputs[0], group_output.inputs["Result"])
    links.new(multiply.outputs[0], group_output.inputs["Mask"])
    group.interface_update(bpy.context)
    return group


def make_scalar_material():
    material = bpy.data.materials.new("Typed Closure Callback Material")
    material.use_nodes = True
    tree = material.node_tree
    nodes = tree.nodes
    links = tree.links
    nodes.clear()

    text = bpy.data.texts.new("typed_closure_callback.glsl")
    text.write(CALLBACK_SOURCE)
    glsl_node = nodes.new("ShaderNodeGLSLFunction")
    glsl_node.name = "Typed Closure GLSL"
    configure_glsl_node(glsl_node, text, "typed_closure_probe")
    require(glsl_node.parse_status == "READY", f"GLSL parse status is {glsl_node.parse_status}")

    callback_socket = find_socket(glsl_node.inputs, "closure.remap")
    require(
        callback_socket.name == "Scalar Override",
        f"Unexpected callback label {callback_socket.name!r}",
    )
    require(
        callback_socket.description == "Override scalar remap",
        f"Unexpected callback description {callback_socket.description!r}",
    )

    closure_input = nodes.new("NodeClosureInput")
    closure_input.name = "Typed Closure Input"
    closure_output = nodes.new("NodeClosureOutput")
    closure_output.name = "Typed Closure Output"
    closure_input.pair_with_output(closure_output)
    for key in ("value", "gain"):
        item = closure_output.input_items.new("FLOAT", key)
        item.structure_type = "SINGLE"
    for key in ("Result", "mask"):
        item = closure_output.output_items.new("FLOAT", key)
        item.structure_type = "SINGLE"
    update_tree(tree)

    callback_group_tree = make_callback_group()
    result_group = nodes.new("ShaderNodeGroup")
    result_group.name = "Typed Closure Result Group"
    result_group.node_tree = callback_group_tree
    mask_group = nodes.new("ShaderNodeGroup")
    mask_group.name = "Typed Closure Mask Group"
    mask_group.node_tree = callback_group_tree
    mask_group.inputs["Gain"].default_value = 0.5
    value_reroute = nodes.new("NodeReroute")
    value_reroute.name = "Typed Closure Value Reroute"
    mask_reroute = nodes.new("NodeReroute")
    mask_reroute.name = "Typed Closure Mask Reroute"

    links.new(find_socket(closure_input.outputs, "value"), value_reroute.inputs[0])
    links.new(value_reroute.outputs[0], result_group.inputs["Value"])
    links.new(value_reroute.outputs[0], mask_group.inputs["Value"])
    links.new(find_socket(closure_input.outputs, "gain"), result_group.inputs["Gain"])
    links.new(result_group.outputs["Result"], find_socket(closure_output.inputs, "Result"))
    links.new(mask_group.outputs["Mask"], mask_reroute.inputs[0])
    links.new(mask_reroute.outputs[0], find_socket(closure_output.inputs, "mask"))
    links.new(closure_output.outputs["Closure"], callback_socket)

    emission = nodes.new("ShaderNodeEmission")
    emission.name = "Typed Closure Emission"
    emission.inputs["Strength"].default_value = 1.0
    output = nodes.new("ShaderNodeOutputMaterial")
    output.name = "Typed Closure Material Output"
    links.new(glsl_node.outputs["Result"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    update_tree(tree)

    require(
        len(glsl_node.panel_states) == 1,
        f"Expected one Closures panel, got {len(glsl_node.panel_states)}",
    )
    glsl_node.panel_states[0].is_collapsed = False

    bpy.ops.mesh.primitive_plane_add(size=4.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.object
    plane.name = "Typed Closure Plane"
    plane.data.materials.append(material)
    update_tree(tree)
    return material, plane


def make_typed_transport_material(plane):
    material = bpy.data.materials.new(TYPED_MATERIAL_NAME)
    material.use_nodes = True
    tree = material.node_tree
    nodes = tree.nodes
    links = tree.links
    nodes.clear()

    typed_text = bpy.data.texts.new("typed_transport_callback.glsl")
    typed_text.write(TYPED_CALLBACK_SOURCE)
    typed_glsl = nodes.new("ShaderNodeGLSLFunction")
    typed_glsl.name = "Typed Transport GLSL"
    configure_glsl_node(typed_glsl, typed_text, "typed_transport_probe")
    require(
        typed_glsl.parse_status == "READY",
        f"Typed transport GLSL parse status is {typed_glsl.parse_status}",
    )
    typed_callback = find_socket(typed_glsl.inputs, "closure.typed_transport")
    require(typed_callback.name == "Typed Transport", "Typed callback label was not applied")
    require(
        typed_callback.description == "Override typed vector, color, and vec4 transport",
        "Typed callback description was not applied",
    )

    outer_input = nodes.new("NodeClosureInput")
    outer_input.name = "Typed Transport Input"
    outer_output = nodes.new("NodeClosureOutput")
    outer_output.name = "Typed Transport Output"
    outer_input.pair_with_output(outer_output)
    links.new(outer_output.outputs["Closure"], typed_callback)
    update_tree(tree)
    sync_closure_output(tree, outer_output)

    nested_text = bpy.data.texts.new("nested_transport_callback.glsl")
    nested_text.write(NESTED_CALLBACK_SOURCE)
    nested_glsl = nodes.new("ShaderNodeGLSLFunction")
    nested_glsl.name = "Nested Transport GLSL"
    configure_glsl_node(nested_glsl, nested_text, "nested_transport")
    require(
        nested_glsl.parse_status == "READY",
        f"Nested transport GLSL parse status is {nested_glsl.parse_status}",
    )
    nested_callback = find_socket(nested_glsl.inputs, "closure.inner_blend")
    require(nested_callback.name == "Nested Blend", "Nested callback label was not applied")
    require(
        nested_callback.description == "Nested callback used by the typed transport zone",
        "Nested callback description was not applied",
    )

    inner_input = nodes.new("NodeClosureInput")
    inner_input.name = "Nested Blend Input"
    inner_output = nodes.new("NodeClosureOutput")
    inner_output.name = "Nested Blend Output"
    inner_input.pair_with_output(inner_output)
    links.new(inner_output.outputs["Closure"], nested_callback)
    update_tree(tree)
    sync_closure_output(tree, inner_output)

    shared = nodes.new("ShaderNodeVectorMath")
    shared.name = "Nested Shared Multiply Add"
    shared.operation = "MULTIPLY_ADD"
    shared.inputs[1].default_value = (0.5, 0.5, 0.5)
    image = bpy.data.images.new("Nested Shared Callback Texture", width=1, height=1, alpha=True)
    image.colorspace_settings.name = "Non-Color"
    image.pixels[:] = (0.0, 0.0, 0.0, 1.0)
    texture = nodes.new("ShaderNodeTexImage")
    texture.name = "Nested Shared Texture"
    texture.image = image
    texture.interpolation = "Closest"
    bias_with_texture = nodes.new("ShaderNodeVectorMath")
    bias_with_texture.name = "Nested Bias Plus Texture"
    bias_with_texture.operation = "ADD"
    links.new(find_socket(inner_input.outputs, "value"), shared.inputs[0])
    links.new(find_socket(inner_input.outputs, "bias"), bias_with_texture.inputs[0])
    links.new(texture.outputs["Color"], bias_with_texture.inputs[1])
    links.new(bias_with_texture.outputs["Vector"], shared.inputs[2])
    links.new(shared.outputs["Vector"], find_socket(inner_output.inputs, "Result"))

    links.new(
        find_socket(outer_input.outputs, "payload"), find_socket(nested_glsl.inputs, "In_value")
    )
    links.new(
        find_socket(outer_input.outputs, "tint"), find_socket(nested_glsl.inputs, "In_bias")
    )
    nested_result = find_socket(nested_glsl.outputs, "Result")
    links.new(nested_result, find_socket(outer_output.inputs, "Result"))
    links.new(nested_result, find_socket(outer_output.inputs, "out_packed"))
    links.new(
        find_socket(outer_input.outputs, "payload.__w"),
        find_socket(outer_output.inputs, "Result.__w"),
    )
    links.new(find_socket(outer_input.outputs, "uv"), find_socket(outer_output.inputs, "out_uv"))
    links.new(
        find_socket(outer_input.outputs, "tint"), find_socket(outer_output.inputs, "out_tint")
    )
    links.new(
        find_socket(outer_input.outputs, "payload.__w"),
        find_socket(outer_output.inputs, "out_packed.__w"),
    )

    emission = nodes.new("ShaderNodeEmission")
    emission.name = "Typed Transport Emission"
    emission.inputs["Strength"].default_value = 1.0
    output = nodes.new("ShaderNodeOutputMaterial")
    output.name = "Typed Transport Material Output"
    links.new(typed_glsl.outputs["Result"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    update_tree(tree)

    require(len(typed_glsl.panel_states) == 1, "Typed GLSL node has no Closures panel")
    require(len(nested_glsl.panel_states) == 1, "Nested GLSL node has no Closures panel")
    typed_glsl.panel_states[0].is_collapsed = False
    nested_glsl.panel_states[0].is_collapsed = False

    plane.data.materials.append(material)
    update_tree(tree)
    return material


def make_int_bool_material(plane):
    material = bpy.data.materials.new(INT_BOOL_MATERIAL_NAME)
    material.use_nodes = True
    tree = material.node_tree
    nodes = tree.nodes
    links = tree.links
    nodes.clear()

    text = bpy.data.texts.new("int_bool_transport_callback.glsl")
    text.write(INT_BOOL_CALLBACK_SOURCE)
    glsl = nodes.new("ShaderNodeGLSLFunction")
    glsl.name = "Int Bool Transport GLSL"
    configure_glsl_node(glsl, text, "exact_transport_probe")
    require(glsl.parse_status == "READY", f"Int/Bool GLSL parse status is {glsl.parse_status}")

    closure_input = nodes.new("NodeClosureInput")
    closure_input.name = "Int Bool Transport Input"
    closure_output = nodes.new("NodeClosureOutput")
    closure_output.name = "Int Bool Transport Output"
    closure_input.pair_with_output(closure_output)
    links.new(closure_output.outputs["Closure"], find_socket(glsl.inputs, "closure.exact_transport"))
    update_tree(tree)
    sync_closure_output(tree, closure_output)

    links.new(find_socket(closure_input.outputs, "value"), find_socket(closure_output.inputs, "Result"))
    links.new(
        find_socket(closure_input.outputs, "enabled"),
        find_socket(closure_output.inputs, "out_enabled"),
    )

    emission = nodes.new("ShaderNodeEmission")
    output = nodes.new("ShaderNodeOutputMaterial")
    links.new(glsl.outputs["Result"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    update_tree(tree)
    plane.data.materials.append(material)
    return material


def capture_identity(material):
    tree = material.node_tree
    glsl_node = tree.nodes["Typed Closure GLSL"]
    closure_input = tree.nodes["Typed Closure Input"]
    closure_output = tree.nodes["Typed Closure Output"]
    return {
        "callback": find_socket(glsl_node.inputs, "closure.remap").identifier,
        "closure_inputs": {
            key: find_socket(closure_input.outputs, key).identifier for key in ("value", "gain")
        },
        "closure_outputs": {
            key: find_socket(closure_output.inputs, key).identifier for key in ("Result", "mask")
        },
        "panel_identifier": int(glsl_node.panel_states[0].identifier),
    }


def capture_typed_identity(material):
    tree = material.node_tree
    typed_glsl = tree.nodes["Typed Transport GLSL"]
    nested_glsl = tree.nodes["Nested Transport GLSL"]
    outer_input = tree.nodes["Typed Transport Input"]
    outer_output = tree.nodes["Typed Transport Output"]
    inner_input = tree.nodes["Nested Blend Input"]
    inner_output = tree.nodes["Nested Blend Output"]
    return {
        "typed_callback": find_socket(
            typed_glsl.inputs, "closure.typed_transport"
        ).identifier,
        "nested_callback": find_socket(nested_glsl.inputs, "closure.inner_blend").identifier,
        "outer_inputs": {
            key: find_socket(outer_input.outputs, key).identifier for key in TYPED_INPUT_KEYS
        },
        "outer_outputs": {
            key: find_socket(outer_output.inputs, key).identifier for key in TYPED_OUTPUT_KEYS
        },
        "inner_inputs": {
            key: find_socket(inner_input.outputs, key).identifier for key in ("value", "bias")
        },
        "inner_outputs": {
            "Result": find_socket(inner_output.inputs, "Result").identifier,
        },
        "typed_panel_identifier": int(typed_glsl.panel_states[0].identifier),
        "nested_panel_identifier": int(nested_glsl.panel_states[0].identifier),
    }


def capture_int_bool_identity(material):
    tree = material.node_tree
    glsl = tree.nodes["Int Bool Transport GLSL"]
    closure_input = tree.nodes["Int Bool Transport Input"]
    closure_output = tree.nodes["Int Bool Transport Output"]
    return {
        "callback": find_socket(glsl.inputs, "closure.exact_transport").identifier,
        "inputs": {
            key: find_socket(closure_input.outputs, key).identifier
            for key in ("value", "enabled")
        },
        "outputs": {
            key: find_socket(closure_output.inputs, key).identifier
            for key in ("Result", "out_enabled")
        },
    }


def assert_roundtrip_graph(material, expected_identity):
    tree = material.node_tree
    glsl_node = tree.nodes["Typed Closure GLSL"]
    closure_input = tree.nodes["Typed Closure Input"]
    closure_output = tree.nodes["Typed Closure Output"]
    callback_socket = find_socket(glsl_node.inputs, "closure.remap")

    require(
        glsl_node.parse_status == "READY",
        f"Round-trip GLSL parse status is {glsl_node.parse_status}",
    )
    require(
        callback_socket.identifier == expected_identity["callback"],
        "Callback identifier changed after reopen",
    )
    require(callback_socket.name == "Scalar Override", "Callback label changed after reopen")
    require(
        callback_socket.description == "Override scalar remap",
        "Callback description changed after reopen",
    )
    require(callback_socket.is_linked, "Callback connection was lost after reopen")
    require(
        callback_socket.links[0].from_node == closure_output,
        "Callback link no longer originates at the paired Closure Output",
    )
    require(
        [item.name for item in closure_output.input_items] == ["value", "gain"],
        "Closure input item keys changed after reopen",
    )
    require(
        [item.name for item in closure_output.output_items] == ["Result", "mask"],
        "Closure output item keys changed after reopen",
    )
    require(
        {key: find_socket(closure_input.outputs, key).identifier for key in ("value", "gain")}
        == expected_identity["closure_inputs"],
        "Closure Input socket identifiers changed after reopen",
    )
    require(
        {key: find_socket(closure_output.inputs, key).identifier for key in ("Result", "mask")}
        == expected_identity["closure_outputs"],
        "Closure Output socket identifiers changed after reopen",
    )
    require(len(glsl_node.panel_states) == 1, "Closures panel state was not restored")
    require(
        int(glsl_node.panel_states[0].identifier) == expected_identity["panel_identifier"],
        "Closures panel identifier changed after reopen",
    )
    require(
        not glsl_node.panel_states[0].is_collapsed,
        "User-opened Closures panel was not preserved",
    )
    require(
        sum(node.bl_idname == "ShaderNodeGroup" for node in tree.nodes) == 2,
        "Two callback instances of the shared Shader Node Group were not preserved",
    )
    result_group = tree.nodes["Typed Closure Result Group"]
    mask_group = tree.nodes["Typed Closure Mask Group"]
    require(
        result_group.node_tree == mask_group.node_tree,
        "Callback group instances no longer reference the same node tree",
    )
    require(result_group.inputs["Gain"].is_linked, "Result group Gain input was disconnected")
    require(not mask_group.inputs["Gain"].is_linked, "Mask group Gain unexpectedly became linked")
    require(
        abs(mask_group.inputs["Gain"].default_value - 0.5) <= 1e-6,
        "Mask group instance lost its distinct Gain value",
    )
    require(
        sum(node.bl_idname == "NodeReroute" for node in tree.nodes) >= 2,
        "Callback reroutes were not preserved",
    )
    require(
        find_socket(closure_input.outputs, "value").is_linked,
        "Callback value input is unlinked",
    )
    require(
        find_socket(closure_input.outputs, "gain").is_linked,
        "Callback gain input is unlinked",
    )
    require(
        find_socket(closure_output.inputs, "Result").is_linked,
        "Callback Result is unlinked",
    )
    require(
        find_socket(closure_output.inputs, "mask").is_linked,
        "Callback mask is unlinked",
    )


def assert_typed_roundtrip_graph(material, expected_identity):
    tree = material.node_tree
    typed_glsl = tree.nodes["Typed Transport GLSL"]
    nested_glsl = tree.nodes["Nested Transport GLSL"]
    outer_input = tree.nodes["Typed Transport Input"]
    outer_output = tree.nodes["Typed Transport Output"]
    inner_input = tree.nodes["Nested Blend Input"]
    inner_output = tree.nodes["Nested Blend Output"]
    typed_callback = find_socket(typed_glsl.inputs, "closure.typed_transport")
    nested_callback = find_socket(nested_glsl.inputs, "closure.inner_blend")

    require(typed_glsl.parse_status == "READY", "Typed GLSL node was not READY after reopen")
    require(nested_glsl.parse_status == "READY", "Nested GLSL node was not READY after reopen")
    require(
        typed_callback.identifier
        == expected_identity["typed_callback"]
        == "closure.typed_transport",
        "Typed callback identifier changed after reopen",
    )
    require(
        nested_callback.identifier == expected_identity["nested_callback"] == "closure.inner_blend",
        "Nested callback identifier changed after reopen",
    )
    require(typed_callback.name == "Typed Transport", "Typed callback label changed after reopen")
    require(nested_callback.name == "Nested Blend", "Nested callback label changed after reopen")
    require(
        typed_callback.description == "Override typed vector, color, and vec4 transport",
        "Typed callback description changed after reopen",
    )
    require(
        nested_callback.description == "Nested callback used by the typed transport zone",
        "Nested callback description changed after reopen",
    )
    require(
        typed_callback.is_linked and typed_callback.links[0].from_node == outer_output,
        "Typed callback connection was not preserved",
    )
    require(
        nested_callback.is_linked and nested_callback.links[0].from_node == inner_output,
        "Nested callback connection was not preserved",
    )

    require(
        [item.name for item in outer_output.input_items] == list(TYPED_INPUT_KEYS),
        "Typed callback input keys changed after reopen",
    )
    require(
        [item.name for item in outer_output.output_items] == list(TYPED_OUTPUT_KEYS),
        "Typed callback output keys changed after reopen",
    )
    require(
        [item.name for item in inner_output.input_items] == ["value", "bias"],
        "Nested callback input keys changed after reopen",
    )
    require(
        [item.name for item in inner_output.output_items] == ["Result"],
        "Nested callback output keys changed after reopen",
    )

    current_identity = capture_typed_identity(material)
    require(
        current_identity == expected_identity,
        f"Typed or nested socket identity changed after reopen: {current_identity}",
    )
    require(not typed_glsl.panel_states[0].is_collapsed, "Typed Closures panel state was lost")
    require(not nested_glsl.panel_states[0].is_collapsed, "Nested Closures panel state was lost")

    outer_input_types = {
        key: find_socket(outer_input.outputs, key).bl_idname for key in TYPED_INPUT_KEYS
    }
    require(
        outer_input_types
        == {
            "uv": "NodeSocketVector2D",
            "tint": "NodeSocketColor",
            "payload": "NodeSocketVector",
            "payload.__w": "NodeSocketFloat",
        },
        f"Unexpected typed callback input sockets: {outer_input_types}",
    )
    outer_output_types = {
        key: find_socket(outer_output.inputs, key).bl_idname for key in TYPED_OUTPUT_KEYS
    }
    require(
        outer_output_types
        == {
            "Result": "NodeSocketVector",
            "Result.__w": "NodeSocketFloat",
            "out_uv": "NodeSocketVector2D",
            "out_tint": "NodeSocketColor",
            "out_packed": "NodeSocketVector",
            "out_packed.__w": "NodeSocketFloat",
        },
        f"Unexpected typed callback output sockets: {outer_output_types}",
    )
    require(
        find_socket(inner_input.outputs, "value").bl_idname == "NodeSocketVector",
        "Nested value is not a Vector",
    )
    require(
        find_socket(inner_input.outputs, "bias").bl_idname == "NodeSocketColor",
        "Nested bias is not a Color",
    )
    require(
        find_socket(inner_output.inputs, "Result").bl_idname == "NodeSocketVector",
        "Nested Result is not a Vector",
    )

    uv = find_socket(outer_input.outputs, "uv")
    tint = find_socket(outer_input.outputs, "tint")
    payload = find_socket(outer_input.outputs, "payload")
    require(uv.label == "UV Pair", "vec2 Meta label was not preserved")
    require(tint.label == "Tint", "Color Meta label was not preserved")
    require(payload.label == "Packed", "vec4 XYZ Meta label was not preserved")
    require(
        find_socket(outer_output.inputs, "out_tint").label == "Output Tint",
        "Color output Meta label was not preserved",
    )

    for key in TYPED_INPUT_KEYS:
        require(find_socket(outer_input.outputs, key).is_linked, f"Outer input {key!r} is unlinked")
    for key in TYPED_OUTPUT_KEYS:
        require(
            find_socket(outer_output.inputs, key).is_linked,
            f"Outer output {key!r} is unlinked",
        )
    require(find_socket(inner_input.outputs, "value").is_linked, "Nested value input is unlinked")
    require(find_socket(inner_input.outputs, "bias").is_linked, "Nested bias input is unlinked")
    require(find_socket(inner_output.inputs, "Result").is_linked, "Nested Result is unlinked")
    shared_nodes = [node for node in tree.nodes if node.name == "Nested Shared Multiply Add"]
    require(
        len(shared_nodes) == 1 and shared_nodes[0].operation == "MULTIPLY_ADD",
        "Nested shared dependency was not preserved",
    )
    texture_nodes = [node for node in tree.nodes if node.name == "Nested Shared Texture"]
    require(
        len(texture_nodes) == 1 and texture_nodes[0].image is not None,
        "Nested shared texture dependency was not preserved",
    )


def assert_int_bool_roundtrip_graph(material, expected_identity):
    tree = material.node_tree
    glsl = tree.nodes["Int Bool Transport GLSL"]
    closure_input = tree.nodes["Int Bool Transport Input"]
    closure_output = tree.nodes["Int Bool Transport Output"]
    callback = find_socket(glsl.inputs, "closure.exact_transport")
    require(glsl.parse_status == "READY", "Int/Bool GLSL node was not READY after reopen")
    require(callback.is_linked and callback.links[0].from_node == closure_output,
            "Int/Bool callback connection was not preserved")
    require(capture_int_bool_identity(material) == expected_identity,
            "Int/Bool socket identity changed after reopen")
    require(find_socket(closure_input.outputs, "value").bl_idname == "NodeSocketInt",
            "Int callback input changed type")
    require(find_socket(closure_input.outputs, "enabled").bl_idname == "NodeSocketBool",
            "Bool callback input changed type")
    require(find_socket(closure_output.inputs, "Result").bl_idname == "NodeSocketInt",
            "Int callback output changed type")
    require(find_socket(closure_output.inputs, "out_enabled").bl_idname == "NodeSocketBool",
            "Bool callback output changed type")


def region_mean_rgb(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        image.colorspace_settings.name = "Non-Color"
        width, height = int(image.size[0]), int(image.size[1])
        pixels = list(image.pixels[:])
    finally:
        bpy.data.images.remove(image)

    require((width, height) == (32, 32), f"Unexpected render size {width}x{height}")
    totals = [0.0, 0.0, 0.0]
    count = 0
    for y in range(12, 20):
        for x in range(12, 20):
            index = (y * width + x) * 4
            for channel in range(3):
                totals[channel] += pixels[index + channel]
            count += 1
    return tuple(value / count for value in totals)


def assert_rgb(actual, expected, label):
    errors = tuple(abs(a - e) for a, e in zip(actual, expected))
    require(
        max(errors) <= CHANNEL_TOLERANCE,
        f"{label}: expected {expected}, got {actual}, abs_error={errors}",
    )


def render_probe(scene, output_dir, label):
    path = output_dir / f"{label}.exr"
    scene.render.filepath = str(path)
    result = bpy.ops.render.render(write_still=True)
    require(result == {"FINISHED"}, f"Render {label} failed: {result}")
    require(path.exists(), f"Render {label} did not write {path}")
    return region_mean_rgb(path)


def set_plane_material(plane, material):
    matching_slots = [
        index
        for index, slot_material in enumerate(plane.data.materials)
        if slot_material == material
    ]
    require(
        len(matching_slots) == 1,
        f"Expected one material slot for {material.name!r}, got {matching_slots}",
    )
    material_index = matching_slots[0]
    plane.active_material_index = material_index
    for polygon in plane.data.polygons:
        polygon.material_index = material_index
    bpy.context.view_layer.update()
    require(
        all(polygon.material_index == material_index for polygon in plane.data.polygons),
        f"Material {material.name!r} was not applied to every probe polygon",
    )


def set_callback_connected(
    material, glsl_node_name, closure_output_name, helper_name, connected
):
    tree = material.node_tree
    glsl_node = tree.nodes[glsl_node_name]
    closure_output = tree.nodes[closure_output_name]
    callback_socket = find_socket(glsl_node.inputs, f"closure.{helper_name}")
    for link in list(callback_socket.links):
        tree.links.remove(link)
    if connected:
        tree.links.new(closure_output.outputs["Closure"], callback_socket)
    update_tree(tree)
    require(
        callback_socket.is_linked == connected,
        f"Callback {helper_name!r} connected={connected} was not applied",
    )
    if connected:
        require(
            callback_socket.links[0].from_node == closure_output,
            f"Callback {helper_name!r} was connected from the wrong Closure Output",
        )


def set_callback_muted(material, glsl_node_name, helper_name, muted):
    tree = material.node_tree
    glsl_node = tree.nodes[glsl_node_name]
    callback_socket = find_socket(glsl_node.inputs, f"closure.{helper_name}")
    require(callback_socket.is_linked, f"Callback {helper_name!r} is not linked")
    for link in callback_socket.links:
        link.is_muted = muted
    update_tree(tree)


FUNCTION_DEFINITION_PATTERN = re.compile(
    r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{}]*\)\s*\{", re.DOTALL
)


def find_matching_brace(source, opening_index):
    require(
        0 <= opening_index < len(source) and source[opening_index] == "{",
        f"Invalid opening brace index {opening_index}",
    )
    depth = 0
    index = opening_index
    state = "code"
    quote = ""
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment":
            if char == "*" and next_char == "/":
                state = "code"
                index += 1
        elif state == "string":
            if char == "\\":
                index += 1
            elif char == quote:
                state = "code"
        elif char == "/" and next_char == "/":
            state = "line_comment"
            index += 1
        elif char == "/" and next_char == "*":
            state = "block_comment"
            index += 1
        elif char in ('"', "'"):
            state = "string"
            quote = char
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return index
        index += 1
    raise AssertionError(f"No matching closing brace for source index {opening_index}")


def function_definitions(source, logical_name):
    definitions = []
    for match in FUNCTION_DEFINITION_PATTERN.finditer(source):
        resolved_name = match.group(1)
        if resolved_name != logical_name and not resolved_name.endswith("_" + logical_name):
            continue
        opening_index = source.find("{", match.start(), match.end())
        closing_index = find_matching_brace(source, opening_index)
        definitions.append(
            {
                "name": resolved_name,
                "body": source[opening_index + 1 : closing_index],
            }
        )
    return definitions


def unique_function_definition(source, logical_name):
    definitions = function_definitions(source, logical_name)
    require(
        len(definitions) == 1,
        f"Expected one definition for {logical_name!r}, got "
        f"{[definition['name'] for definition in definitions]}",
    )
    return definitions[0]


def function_call_count(body, resolved_name):
    return len(
        re.findall(rf"(?<![A-Za-z0-9_]){re.escape(resolved_name)}\s*\(", body)
    )


def inspect_codegen_source(source):
    typed_helper = unique_function_definition(source, "typed_transport")
    typed_probe = unique_function_definition(source, "typed_transport_probe")
    inner_helper = unique_function_definition(source, "inner_blend")
    nested_export = unique_function_definition(source, "nested_transport")

    typed_helper_call_count = function_call_count(typed_probe["body"], typed_helper["name"])
    require(
        typed_helper_call_count == 2,
        f"typed_transport_probe has {typed_helper_call_count} typed helper calls instead of two",
    )

    outer_sub_function_calls = re.findall(r"\b(ntree_fn[0-9]+)\s*\(", typed_helper["body"])
    inner_sub_function_calls = re.findall(r"\b(ntree_fn[0-9]+)\s*\(", inner_helper["body"])
    require(
        len(outer_sub_function_calls) == 1,
        f"Rewritten typed_transport must call one ntree function, got {outer_sub_function_calls}",
    )
    require(
        len(inner_sub_function_calls) == 1,
        f"Rewritten inner_blend must call one ntree function, got {inner_sub_function_calls}",
    )
    outer_sub_function = unique_function_definition(source, outer_sub_function_calls[0])
    inner_sub_function = unique_function_definition(source, inner_sub_function_calls[0])
    require(
        outer_sub_function["name"] != inner_sub_function["name"],
        "Outer and nested callbacks unexpectedly share one generated ntree function",
    )

    nested_wrapper_calls = re.findall(
        r"\b(glsl_fn_[A-Za-z0-9_]+)\s*\(", outer_sub_function["body"]
    )
    require(
        len(nested_wrapper_calls) == 1,
        f"Outer ntree function must evaluate one nested GLSL wrapper, got {nested_wrapper_calls}",
    )
    nested_wrapper = unique_function_definition(source, nested_wrapper_calls[0])
    nested_export_call_count = function_call_count(
        nested_wrapper["body"], nested_export["name"]
    )
    require(
        nested_export_call_count == 1,
        f"Nested GLSL wrapper calls nested_transport {nested_export_call_count} times",
    )

    assignments = {
        match.group(1): "".join(match.group(2).split())
        for match in re.finditer(
            r"(?m)^\s*out([0-9]+)\s*=\s*([^;]+);", outer_sub_function["body"]
        )
    }
    require("0" in assignments and "4" in assignments, "Outer ntree outputs 0 and 4 are missing")
    require(
        assignments["0"] == assignments["4"],
        f"Shared nested result was serialized twice: out0={assignments['0']}, "
        f"out4={assignments['4']}",
    )

    vector_math_call_count = len(
        re.findall(r"\bvector_math_multiply_add\s*\(", inner_sub_function["body"])
    )
    require(
        vector_math_call_count == 1,
        f"Nested ntree function has {vector_math_call_count} shared multiply-add calls",
    )
    image_texture_call_count = len(
        re.findall(r"\bnode_tex_image_(?:linear|linear_grad|cubic)\s*\(", inner_sub_function["body"])
    )
    require(
        image_texture_call_count == 1,
        f"Nested ntree function has {image_texture_call_count} shared image texture calls",
    )
    return {
        "typed_helper_call_count": typed_helper_call_count,
        "typed_helper_name": typed_helper["name"],
        "outer_sub_function": outer_sub_function["name"],
        "inner_sub_function": inner_sub_function["name"],
        "nested_wrapper": nested_wrapper["name"],
        "nested_export": nested_export["name"],
        "outer_shared_result": assignments["0"],
        "nested_vector_math_call_count": vector_math_call_count,
        "nested_image_texture_call_count": image_texture_call_count,
    }


def inspect_codegen_dump(output_dir):
    shader_dir = output_dir / "Shaders"
    shader_paths = sorted(
        shader_dir.glob("*.glsl"),
        key=lambda path: (path.name.endswith(".expanded.glsl"), path.name),
    )
    require(shader_paths, f"No shader sources were written under {shader_dir}")

    candidates = []
    for path in shader_paths:
        source = path.read_text(encoding="utf-8", errors="replace")
        if all(marker in source for marker in ("typed_transport", "inner_blend", "ntree_fn")):
            candidates.append((path, source))
    require(candidates, "No shader dump contains typed, nested, and ntree callback code")

    failures = []
    for path, source in candidates:
        try:
            metadata = inspect_codegen_source(source)
        except AssertionError as error:
            failures.append(f"{path.name}: {error}")
            continue
        preserved_path = output_dir / "codegen_connected.glsl"
        shutil.copyfile(path, preserved_path)
        metadata.update(
            {
                "source_path": str(path),
                "preserved_path": str(preserved_path),
                "candidate_count": len(candidates),
            }
        )
        return metadata
    raise AssertionError("No connected shader dump passed codegen checks:\n" + "\n".join(failures))


def child_main(requested_backend):
    gpu.init()
    active_backend = gpu.platform.backend_type_get()
    print(BACKEND_MARKER + active_backend, flush=True)
    require(
        active_backend == requested_backend,
        f"Requested {requested_backend}, active backend is {active_backend}",
    )

    output_dir = OUT_DIR / requested_backend.lower()
    output_dir.mkdir(parents=True, exist_ok=True)
    scene, engine = configure_scene()
    scalar_material, plane = make_scalar_material()
    typed_material = make_typed_transport_material(plane)
    int_bool_material = make_int_bool_material(plane)
    scalar_identity = capture_identity(scalar_material)
    typed_identity = capture_typed_identity(typed_material)
    int_bool_identity = capture_int_bool_identity(int_bool_material)

    blend_path = output_dir / "typed_closure_roundtrip.blend"
    save_result = bpy.ops.wm.save_as_mainfile(filepath=str(blend_path), check_existing=False)
    require(save_result == {"FINISHED"}, f"Saving round-trip blend failed: {save_result}")
    require(blend_path.exists(), f"Round-trip blend was not written: {blend_path}")
    open_result = bpy.ops.wm.open_mainfile(filepath=str(blend_path), load_ui=False)
    require(open_result == {"FINISHED"}, f"Reopening round-trip blend failed: {open_result}")

    scalar_material = bpy.data.materials["Typed Closure Callback Material"]
    typed_material = bpy.data.materials[TYPED_MATERIAL_NAME]
    int_bool_material = bpy.data.materials[INT_BOOL_MATERIAL_NAME]
    plane = bpy.data.objects["Typed Closure Plane"]
    scene = bpy.context.scene
    assert_roundtrip_graph(scalar_material, scalar_identity)
    assert_typed_roundtrip_graph(typed_material, typed_identity)
    assert_int_bool_roundtrip_graph(int_bool_material, int_bool_identity)

    set_plane_material(plane, scalar_material)
    scalar_connected_rgb = render_probe(scene, output_dir, "scalar_connected")
    assert_rgb(scalar_connected_rgb, CONNECTED_EXPECTED, "scalar connected callback RGB")
    set_callback_muted(scalar_material, "Typed Closure GLSL", "remap", True)
    scalar_muted_rgb = render_probe(scene, output_dir, "scalar_muted_fallback")
    assert_rgb(scalar_muted_rgb, FALLBACK_EXPECTED, "muted scalar callback fallback RGB")
    set_callback_muted(scalar_material, "Typed Closure GLSL", "remap", False)
    set_callback_connected(
        scalar_material, "Typed Closure GLSL", "Typed Closure Output", "remap", False
    )
    scalar_fallback_rgb = render_probe(scene, output_dir, "scalar_fallback")
    assert_rgb(scalar_fallback_rgb, FALLBACK_EXPECTED, "scalar disconnected fallback RGB")
    set_callback_connected(
        scalar_material, "Typed Closure GLSL", "Typed Closure Output", "remap", True
    )
    scalar_reconnected_rgb = render_probe(scene, output_dir, "scalar_reconnected")
    assert_rgb(
        scalar_reconnected_rgb, CONNECTED_EXPECTED, "scalar reconnected callback RGB"
    )

    scalar_connected_delta = max(
        abs(a - b) for a, b in zip(scalar_connected_rgb, scalar_fallback_rgb)
    )
    require(
        scalar_connected_delta > 0.2,
        f"Scalar connected and fallback results are not distinct: {scalar_connected_delta}",
    )

    set_plane_material(plane, typed_material)
    typed_connected_rgb = render_probe(scene, output_dir, "typed_connected")
    assert_rgb(typed_connected_rgb, TYPED_CONNECTED_EXPECTED, "typed connected callback RGB")
    codegen = inspect_codegen_dump(output_dir)

    set_callback_connected(
        typed_material,
        "Nested Transport GLSL",
        "Nested Blend Output",
        "inner_blend",
        False,
    )
    typed_nested_fallback_rgb = render_probe(scene, output_dir, "typed_nested_fallback")
    assert_rgb(
        typed_nested_fallback_rgb,
        TYPED_NESTED_FALLBACK_EXPECTED,
        "typed nested callback fallback RGB",
    )
    set_callback_connected(
        typed_material,
        "Nested Transport GLSL",
        "Nested Blend Output",
        "inner_blend",
        True,
    )

    set_callback_connected(
        typed_material,
        "Typed Transport GLSL",
        "Typed Transport Output",
        "typed_transport",
        False,
    )
    typed_fallback_rgb = render_probe(scene, output_dir, "typed_outer_fallback")
    assert_rgb(typed_fallback_rgb, TYPED_FALLBACK_EXPECTED, "typed outer callback fallback RGB")
    set_callback_connected(
        typed_material,
        "Typed Transport GLSL",
        "Typed Transport Output",
        "typed_transport",
        True,
    )
    typed_reconnected_rgb = render_probe(scene, output_dir, "typed_reconnected")
    assert_rgb(
        typed_reconnected_rgb, TYPED_CONNECTED_EXPECTED, "typed reconnected callback RGB"
    )

    typed_nested_delta = max(
        abs(a - b) for a, b in zip(typed_connected_rgb, typed_nested_fallback_rgb)
    )
    typed_outer_delta = max(
        abs(a - b) for a, b in zip(typed_connected_rgb, typed_fallback_rgb)
    )
    require(
        typed_nested_delta > 0.2,
        f"Typed nested connected and fallback results are not distinct: {typed_nested_delta}",
    )
    require(
        typed_outer_delta > 0.2,
        f"Typed outer connected and fallback results are not distinct: {typed_outer_delta}",
    )

    set_plane_material(plane, int_bool_material)
    int_bool_connected_rgb = render_probe(scene, output_dir, "int_bool_connected")
    assert_rgb(
        int_bool_connected_rgb,
        INT_BOOL_CONNECTED_EXPECTED,
        "Int/Bool connected callback RGB",
    )
    set_callback_connected(
        int_bool_material,
        "Int Bool Transport GLSL",
        "Int Bool Transport Output",
        "exact_transport",
        False,
    )
    int_bool_fallback_rgb = render_probe(scene, output_dir, "int_bool_fallback")
    assert_rgb(
        int_bool_fallback_rgb,
        INT_BOOL_FALLBACK_EXPECTED,
        "Int/Bool fallback RGB",
    )

    summary = {
        "backend": active_backend,
        "engine": engine,
        "scalar": {
            "identity": scalar_identity,
            "connected_rgb": scalar_connected_rgb,
            "muted_fallback_rgb": scalar_muted_rgb,
            "fallback_rgb": scalar_fallback_rgb,
            "reconnected_rgb": scalar_reconnected_rgb,
            "connected_expected": CONNECTED_EXPECTED,
            "fallback_expected": FALLBACK_EXPECTED,
        },
        "typed": {
            "identity": typed_identity,
            "connected_rgb": typed_connected_rgb,
            "nested_fallback_rgb": typed_nested_fallback_rgb,
            "outer_fallback_rgb": typed_fallback_rgb,
            "reconnected_rgb": typed_reconnected_rgb,
            "connected_expected": TYPED_CONNECTED_EXPECTED,
            "nested_fallback_expected": TYPED_NESTED_FALLBACK_EXPECTED,
            "outer_fallback_expected": TYPED_FALLBACK_EXPECTED,
        },
        "int_bool": {
            "identity": int_bool_identity,
            "connected_rgb": int_bool_connected_rgb,
            "fallback_rgb": int_bool_fallback_rgb,
            "connected_expected": INT_BOOL_CONNECTED_EXPECTED,
            "fallback_expected": INT_BOOL_FALLBACK_EXPECTED,
        },
        "codegen": codegen,
        "channel_tolerance": CHANNEL_TOLERANCE,
        "blend_path": str(blend_path),
    }
    (output_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(SUCCESS_MARKER + "=" + requested_backend, flush=True)


def tail_text(text, line_count=120):
    return "\n".join(text.splitlines()[-line_count:])


def find_gpu_errors(output):
    errors = []
    for line in output.splitlines():
        for label, pattern in ERROR_PATTERNS:
            if pattern.search(line):
                errors.append(f"{label}: {line.strip()}")
                break
    return errors


def run_child(backend):
    require(backend in {"OPENGL", "VULKAN"}, f"Unsupported child backend {backend!r}")
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    output_dir = OUT_DIR / backend.lower()
    require(output_dir.parent.resolve() == OUT_DIR.resolve(), "Invalid backend output directory")
    if output_dir.exists():
        shutil.rmtree(output_dir)
    (output_dir / "Shaders").mkdir(parents=True)
    log_path = OUT_DIR / f"{backend.lower()}.log"
    command = [
        bpy.app.binary_path,
        "--background",
        "--factory-startup",
        "--gpu-backend",
        backend.lower(),
        "--debug-gpu",
        "--debug-memory",
        "--debug-gpu-shader-source",
        f"*{TYPED_MATERIAL_NAME}*",
        "--python-exit-code",
        "1",
        "--python",
        str(Path(__file__).resolve()),
        "--",
        "--child",
        backend,
    ]
    command_text = subprocess.list2cmdline([str(value) for value in command])
    process = subprocess.Popen(
        command,
        cwd=str(output_dir),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    launch_record = (
        f"child_backend={backend}\n"
        f"child_pid={process.pid}\n"
        f"child_executable={Path(command[0]).resolve()}\n"
        f"child_command={command_text}\n"
    )
    print(launch_record.rstrip(), flush=True)
    try:
        output, _unused = process.communicate(timeout=240)
    except subprocess.TimeoutExpired:
        process.kill()
        output, _unused = process.communicate()
        log_path.write_text(
            launch_record + f"TIMEOUT after 240s; killed exact child PID {process.pid}\n" + output,
            encoding="utf-8",
        )
        raise AssertionError(
            f"{backend} child timed out and exact PID {process.pid} was terminated\n"
            f"--- child tail ---\n{tail_text(output)}"
        )

    log_path.write_text(launch_record + output, encoding="utf-8")
    require(
        process.returncode == 0,
        f"{backend} child exited with {process.returncode}\n"
        f"--- child tail ---\n{tail_text(output)}",
    )
    require(
        BACKEND_MARKER + backend in output,
        f"{backend} child did not confirm its backend\n--- child tail ---\n{tail_text(output)}",
    )
    require(
        SUCCESS_MARKER + "=" + backend in output,
        f"{backend} child did not finish\n--- child tail ---\n{tail_text(output)}",
    )
    errors = find_gpu_errors(output)
    require(not errors, f"{backend} child reported GPU errors:\n" + "\n".join(errors))


def script_args():
    if "--" not in sys.argv:
        return []
    return sys.argv[sys.argv.index("--") + 1 :]


def main():
    args = script_args()
    if len(args) == 2 and args[0] == "--child":
        child_main(args[1])
        return
    require(not args, f"Unexpected arguments: {args}")
    for backend in ("OPENGL", "VULKAN"):
        run_child(backend)
    print("GLSL_TYPED_CLOSURE_CALLBACK_RELEASE_OK", flush=True)


if __name__ == "__main__":
    main()
