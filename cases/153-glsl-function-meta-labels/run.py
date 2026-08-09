import bpy


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


def configure_glsl_node(node, text, function_name):
    node.source_mode = "INTERNAL"
    node.script = text
    node.function_name = function_name
    refresh_glsl_node(node)


def find_socket(sockets, identifier):
    for socket in sockets:
        if socket.identifier == identifier:
            return socket
    raise AssertionError(f"Socket identifier {identifier!r} not found")


def assert_equal(actual, expected, label):
    if actual != expected:
        raise AssertionError(f"{label}: expected {expected!r}, got {actual!r}")


def assert_close_tuple(actual, expected, label, epsilon=1e-6):
    actual_tuple = tuple(float(value) for value in actual)
    if len(actual_tuple) != len(expected):
        raise AssertionError(f"{label}: expected length {len(expected)}, got {len(actual_tuple)}")
    for index, (actual_value, expected_value) in enumerate(zip(actual_tuple, expected)):
        if abs(actual_value - expected_value) > epsilon:
            raise AssertionError(
                f"{label}[{index}]: expected {expected_value!r}, got {actual_value!r}"
            )


def make_material_tree():
    material = bpy.data.materials.new("GLSLMetaLabelsRelease")
    material.use_nodes = True
    tree = material.node_tree
    tree.nodes.clear()
    return tree


def main():
    bpy.ops.wm.read_homefile(use_factory_startup=True)

    base_label = "\u57fa\u7840\u8272"
    updated_label = "\u4e3b\u989c\u8272"
    texture_label = "\u8d34\u56fe"
    output_label = "\u8f93\u51fa\u8272"

    tree = make_material_tree()
    node = tree.nodes.new("ShaderNodeGLSLFunction")
    source = (
        "/* @glsl_meta v1\n"
        f"color: label=\"{base_label}\" default=vec4(0.1, 0.2, 0.3, 0.4)\n"
        f"tex: label=\"{texture_label}\" description=\"Texture input\"\n"
        f"out_color: label=\"{output_label}\"\n"
        "*/\n"
        "vec4 localized_labels(vec4 color, sampler2D tex, vec2 uv, out vec4 out_color){\n"
        "  out_color = color;\n"
        "  return color;\n"
        "}\n"
    )
    text = make_text_block("glsl_meta_labels_release.glsl", source)
    configure_glsl_node(node, text, "localized_labels")

    assert_equal(node.parse_status, "READY", "localized label parse status")
    color_socket = find_socket(node.inputs, "In_color")
    color_w_socket = find_socket(node.inputs, "In_color_w")
    tex_socket = find_socket(node.inputs, "In_tex")
    uv_socket = find_socket(node.inputs, "In_uv")
    out_socket = find_socket(node.outputs, "Out_out_color")
    out_w_socket = find_socket(node.outputs, "Out_out_color_w")

    assert_equal(color_socket.name, base_label, "vec4 input label")
    assert_equal(color_w_socket.name, base_label + " W", "vec4 input w label")
    assert_equal(tex_socket.name, texture_label, "sampler2D input label")
    assert_equal(uv_socket.name, "uv", "unlabeled input keeps GLSL parameter name")
    assert_equal(out_socket.name, output_label, "out parameter label")
    assert_equal(out_w_socket.name, output_label + " W", "out parameter w label")
    assert_equal(color_socket.bl_idname, "NodeSocketVector", "vec4 input xyz socket type")
    assert_equal(color_w_socket.bl_idname, "NodeSocketFloat", "vec4 input w socket type")
    assert_equal(tex_socket.bl_idname, "NodeSocketClosure", "sampler2D input socket type")
    assert_equal(out_socket.bl_idname, "NodeSocketVector", "out vec4 socket type")
    assert_equal(out_w_socket.bl_idname, "NodeSocketFloat", "out vec4 w socket type")

    preserved_xyz = (0.9, 0.8, 0.7)
    preserved_w = 0.6
    color_socket.default_value = preserved_xyz
    color_w_socket.default_value = preserved_w

    text.clear()
    text.write(
        "/* @glsl_meta v1\n"
        f"color: label=\"{updated_label}\" default=vec4(0.1, 0.2, 0.3, 0.4)\n"
        f"tex: label=\"{texture_label}\" description=\"Texture input\"\n"
        f"out_color: label=\"{output_label}\"\n"
        "*/\n"
        "vec4 localized_labels(vec4 color, sampler2D tex, vec2 uv, out vec4 out_color){\n"
        "  out_color = color;\n"
        "  return color;\n"
        "}\n"
    )
    refresh_glsl_node(node)

    assert_equal(node.parse_status, "READY", "updated label parse status")
    color_socket = find_socket(node.inputs, "In_color")
    color_w_socket = find_socket(node.inputs, "In_color_w")
    assert_equal(color_socket.name, updated_label, "refreshed vec4 input label")
    assert_close_tuple(color_socket.default_value, preserved_xyz, "label refresh preserved xyz value")
    assert_equal(color_w_socket.name, updated_label + " W", "refreshed vec4 input w label")
    if abs(color_w_socket.default_value - preserved_w) > 1e-6:
        raise AssertionError(
            f"label refresh preserved w value: expected {preserved_w!r}, got {color_w_socket.default_value!r}"
        )

    bad_node = tree.nodes.new("ShaderNodeGLSLFunction")
    bad_source = (
        "/* @glsl_meta v1\n"
        "out_color: description=\"Output tooltip is not supported\"\n"
        "*/\n"
        "void bad_out_meta(out vec4 out_color){\n"
        "  out_color = vec4(1.0);\n"
        "}\n"
    )
    bad_text = make_text_block("glsl_bad_out_meta_release.glsl", bad_source)
    configure_glsl_node(bad_node, bad_text, "bad_out_meta")
    assert_equal(bad_node.parse_status, "ERROR", "unsupported out description parse status")

    print("GLSL_FUNCTION_META_LABELS_RELEASE_OK")


if __name__ == "__main__":
    main()
