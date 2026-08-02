# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_glsl_function_closure_sampler3d import (  # noqa: E402
    build_plane,
    clear_scene,
    configure_scene,
    find_socket,
    make_text_block,
    refresh_glsl_node,
    render_pixels,
    require,
    sample_pixel,
)

import bpy
import gpu


WHITE_TOLERANCE = 0.02
CONNECTED_COLOR = (0.25, 0.5, 0.75)
DUMP_MATERIAL_NAME = "ZZ925UnconnectedSamplerQueries"


def assert_rgb(actual, expected, label, tolerance=WHITE_TOLERANCE):
    for channel, value, target in zip("RGB", actual, expected):
        require(
            abs(value - target) <= tolerance,
            f"{label} {channel}: expected {target}, got {value}; RGBA={actual}",
        )


def render_center(name):
    pixels, width, height, elapsed = render_pixels(name)
    center = sample_pixel(pixels, width, width // 2, height // 2)
    return center, elapsed


def make_glsl_material(name, source, function_name):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    glsl = nodes.new("ShaderNodeGLSLFunction")
    glsl.name = name + " GLSL"
    glsl.source_mode = "INTERNAL"
    glsl.script = make_text_block(name + ".glsl", source)
    glsl.function_name = function_name
    material.node_tree.interface_update(bpy.context)
    links.new(find_socket(glsl.outputs, "Result"), emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    refresh_glsl_node(glsl)
    require(glsl.parse_status == "READY", f"{name}: parse status is {glsl.parse_status}")
    return material, glsl


def add_image_to_closure(material, name, color=CONNECTED_COLOR):
    image = bpy.data.images.new(name + " Image", 1, 1, alpha=True, float_buffer=True)
    image.colorspace_settings.name = "Non-Color"
    image.pixels = [color[0], color[1], color[2], 1.0]
    image_node = material.node_tree.nodes.new("ShaderNodeImageToClosure")
    image_node.name = name + " Image to Closure"
    image_node.image = image
    return image_node


def update_material(material, glsl):
    material.node_tree.interface_update(bpy.context)
    refresh_glsl_node(glsl)
    material.node_tree.update_tag()
    bpy.context.view_layer.update()


def test_direct_nested_alias_and_query_fallbacks():
    clear_scene()
    source = r"""
vec4 fallback_sample_2d(sampler2D image, vec2 uv)
{
  sampler2D alias_image = image;
  vec4 sampled = texture(alias_image, uv);
  sampled *= texture(alias_image, uv, 0.0);
  sampled *= textureLod(alias_image, uv, 0.0);
  sampled *= textureGrad(alias_image, uv, vec2(0.0), vec2(0.0));
  sampled *= texelFetch(alias_image, ivec2(123, -456), 7);
  sampled *= textureGather(alias_image, uv, 2);
  return sampled;
}

vec4 fallback_sample_3d(sampler3D volume, vec3 coordinate)
{
  sampler3D alias_volume = volume;
  vec4 sampled = texture(alias_volume, coordinate);
  sampled *= texture(alias_volume, coordinate, 0.0);
  sampled *= textureLod(alias_volume, coordinate, 0.0);
  sampled *= textureGrad(alias_volume, coordinate, vec3(0.0), vec3(0.0));
  sampled *= texelFetch(alias_volume, ivec3(-10, 20, 30), 4);
  return sampled;
}

vec4 fallback_queries(sampler2D image, sampler3D volume)
{
  ivec2 image_size = textureSize(image, 5);
  ivec3 volume_size = textureSize(volume, 6);
  bool sizes_are_one = all(equal(image_size, ivec2(1))) &&
                       all(equal(volume_size, ivec3(1)));
  vec4 sampled = fallback_sample_2d(image, vec2(1000.0, -1000.0));
  sampled *= fallback_sample_3d(volume, vec3(-50.0, 25.0, 75.0));
  return sizes_are_one ? sampled : vec4(0.0);
}
"""
    material, _glsl = make_glsl_material(
        DUMP_MATERIAL_NAME, source, "fallback_queries"
    )
    build_plane(material)
    center, elapsed = render_center("glsl_unconnected_sampler_queries")
    assert_rgb(center, (1.0, 1.0, 1.0), "fallback query result")
    require(abs(center[3] - 1.0) <= WHITE_TOLERANCE, f"Fallback alpha mismatch: {center}")
    print(f"GLSL_UNCONNECTED_SAMPLER_QUERIES={center} render_seconds={elapsed:.3f}")


def test_mixed_connected_and_fallback_samplers():
    clear_scene()
    source = r"""
vec4 mixed_sampler_sources(sampler2D image, sampler3D volume)
{
  return texture(image, vec2(0.5)) * texture(volume, vec3(0.5));
}
"""
    material, glsl = make_glsl_material(
        "Mixed Connected and Fallback Samplers", source, "mixed_sampler_sources"
    )
    image_node = add_image_to_closure(material, "Mixed Connected")
    material.node_tree.links.new(
        image_node.outputs["Closure"], find_socket(glsl.inputs, "In_image")
    )
    update_material(material, glsl)
    build_plane(material)
    center, elapsed = render_center("glsl_mixed_connected_fallback_sampler")
    assert_rgb(center, CONNECTED_COLOR, "mixed connected/fallback result")
    print(f"GLSL_MIXED_CONNECTED_FALLBACK_SAMPLER={center} render_seconds={elapsed:.3f}")


def test_connection_state_transitions():
    clear_scene()
    source = r"""
vec4 reconnect_sampler(sampler2D image)
{
  return texture(image, vec2(0.5));
}
"""
    material, glsl = make_glsl_material(
        "Sampler Connection Transitions", source, "reconnect_sampler"
    )
    image_node = add_image_to_closure(material, "Transition")
    build_plane(material)

    initial, _elapsed = render_center("glsl_sampler_transition_initial_fallback")
    assert_rgb(initial, (1.0, 1.0, 1.0), "initial fallback")

    link = material.node_tree.links.new(
        image_node.outputs["Closure"], find_socket(glsl.inputs, "In_image")
    )
    update_material(material, glsl)
    connected, _elapsed = render_center("glsl_sampler_transition_connected")
    assert_rgb(connected, CONNECTED_COLOR, "connected sampler")

    material.node_tree.links.remove(link)
    update_material(material, glsl)
    disconnected, _elapsed = render_center("glsl_sampler_transition_disconnected")
    assert_rgb(disconnected, (1.0, 1.0, 1.0), "disconnected fallback")

    material.node_tree.links.new(
        image_node.outputs["Closure"], find_socket(glsl.inputs, "In_image")
    )
    update_material(material, glsl)
    reconnected, elapsed = render_center("glsl_sampler_transition_reconnected")
    assert_rgb(reconnected, CONNECTED_COLOR, "reconnected sampler")
    print(
        "GLSL_SAMPLER_CONNECTION_TRANSITIONS="
        f"{initial}->{connected}->{disconnected}->{reconnected} "
        f"render_seconds={elapsed:.3f}"
    )


def make_group_fallback_material():
    group = bpy.data.node_groups.new("Unconnected Sampler Group", "ShaderNodeTree")
    group.interface.new_socket(name="Image", in_out="INPUT", socket_type="NodeSocketClosure")
    group.interface.new_socket(name="Color", in_out="OUTPUT", socket_type="NodeSocketColor")
    group.interface_update(bpy.context)

    group_input = group.nodes.new("NodeGroupInput")
    group_output = group.nodes.new("NodeGroupOutput")
    glsl = group.nodes.new("ShaderNodeGLSLFunction")
    glsl.source_mode = "INTERNAL"
    glsl.script = make_text_block(
        "UnconnectedSamplerGroup.glsl",
        "vec4 group_sampler(sampler2D image){ return texture(image, vec2(0.5)); }\n",
    )
    glsl.function_name = "group_sampler"
    group.interface_update(bpy.context)
    group.links.new(group_input.outputs["Image"], find_socket(glsl.inputs, "In_image"))
    group.links.new(find_socket(glsl.outputs, "Result"), group_output.inputs["Color"])
    refresh_glsl_node(glsl)
    require(glsl.parse_status == "READY", f"Group fallback parse status is {glsl.parse_status}")

    material = bpy.data.materials.new("Unconnected Sampler Group Material")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()
    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    group_node = nodes.new("ShaderNodeGroup")
    group_node.node_tree = group
    links.new(group_node.outputs["Color"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material, group_node


def test_unconnected_group_input_fallback():
    clear_scene()
    material, group_node = make_group_fallback_material()
    build_plane(material)
    unconnected, _elapsed = render_center("glsl_unconnected_group_sampler")
    assert_rgb(unconnected, (1.0, 1.0, 1.0), "unconnected group fallback")

    image_node = add_image_to_closure(material, "Connected Group Input")
    material.node_tree.links.new(
        image_node.outputs["Closure"], find_socket(group_node.inputs, "Image")
    )
    material.node_tree.update_tag()
    bpy.context.view_layer.update()
    connected, elapsed = render_center("glsl_connected_group_sampler")
    assert_rgb(connected, CONNECTED_COLOR, "connected group sampler")
    print(
        f"GLSL_GROUP_SAMPLER={unconnected}->{connected} render_seconds={elapsed:.3f}"
    )


def test_invalid_fallback_call_arity_is_not_accepted_by_specialization():
    clear_scene()
    source = r"""
vec4 invalid_fallback_arity(sampler2D image)
{
  return texture(image, vec2(0.5), 0.0, 1.0);
}
"""
    material, _glsl = make_glsl_material(
        "Invalid Fallback Sampler Arity", source, "invalid_fallback_arity"
    )
    build_plane(material)
    center, elapsed = render_center("glsl_invalid_fallback_sampler_arity")
    require(
        sum(abs(channel) for channel in center[:3]) <= WHITE_TOLERANCE,
        f"Invalid fallback sampler arity unexpectedly compiled: {center}",
    )
    print(f"GLSL_INVALID_FALLBACK_SAMPLER_ARITY={center} render_seconds={elapsed:.3f}")


def test_invalid_sampler3d_gather_is_not_accepted_by_specialization():
    clear_scene()
    source = r"""
vec4 invalid_sampler3d_gather(sampler3D volume)
{
  return textureGather(volume, vec3(0.5));
}
"""
    material, _glsl = make_glsl_material(
        "Invalid Fallback Sampler3D Gather", source, "invalid_sampler3d_gather"
    )
    build_plane(material)
    center, elapsed = render_center("glsl_invalid_fallback_sampler3d_gather")
    require(
        sum(abs(channel) for channel in center[:3]) <= WHITE_TOLERANCE,
        f"Invalid sampler3D textureGather unexpectedly compiled: {center}",
    )
    print(f"GLSL_INVALID_FALLBACK_SAMPLER3D_GATHER={center} render_seconds={elapsed:.3f}")


def main():
    bpy.ops.wm.read_homefile(use_factory_startup=True)
    gpu.init()
    print(f"GLSL_UNCONNECTED_SAMPLER_BACKEND={gpu.platform.backend_type_get()}", flush=True)
    configure_scene()
    test_direct_nested_alias_and_query_fallbacks()
    test_mixed_connected_and_fallback_samplers()
    test_connection_state_transitions()
    test_unconnected_group_input_fallback()
    test_invalid_fallback_call_arity_is_not_accepted_by_specialization()
    test_invalid_sampler3d_gather_is_not_accepted_by_specialization()
    print("GLSL_UNCONNECTED_SAMPLER_DEFAULT_OK", flush=True)


if __name__ == "__main__":
    main()
