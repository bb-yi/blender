# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import sys
import importlib.util
import tempfile
import bpy
import unittest
from types import SimpleNamespace
from pathlib import Path


def load_source_node_add_menu_shader():
    script_path = (
        Path(__file__).resolve().parents[2]
        / "scripts"
        / "startup"
        / "bl_ui"
        / "node_add_menu_shader.py"
    )
    spec = importlib.util.spec_from_file_location("codex_node_add_menu_shader", script_path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


node_add_menu_shader = load_source_node_add_menu_shader()


def make_text_block(name, source):
    text = bpy.data.texts.get(name)
    if text is None:
        text = bpy.data.texts.new(name)
    else:
        text.clear()
    text.write(source)
    return text


def find_socket(sockets, name):
    for socket in sockets:
        if socket.name == name or socket.identifier == name:
            return socket
    raise AssertionError(f"Socket {name!r} not found")


def find_define_value(node, name):
    for value in node.define_values:
        if value.name == name:
            return value
    raise AssertionError(f"Define {name!r} not found")


def refresh_glsl_node(node):
    current_name = node.function_name
    node.function_name = ""
    node.function_name = current_name
    node.id_data.interface_update(bpy.context)
    node.id_data.update_tag()
    bpy.context.view_layer.update()


def refresh_glsl_node_with_operator(node, tree):
    return run_glsl_node_operator(node, tree, bpy.ops.node.glsl_function_refresh)


def sync_closure_output_with_operator(node, tree):
    return run_glsl_node_operator(node, tree, bpy.ops.node.sockets_sync)


def reset_glsl_node_defaults_with_operator(node, tree):
    return run_glsl_node_operator(node, tree, bpy.ops.node.glsl_function_reset_defaults)


def toggle_glsl_node_code_mode_with_operator(node, tree):
    return run_glsl_node_operator(node, tree, bpy.ops.node.glsl_function_toggle_code_mode)


def new_glsl_node_text_with_operator(node, tree):
    return run_glsl_node_operator(node, tree, bpy.ops.node.glsl_function_new_text)


def make_glsl_node_internal_with_operator(node, tree):
    return run_glsl_node_operator(node, tree, bpy.ops.node.glsl_function_make_internal)


def duplicate_glsl_node_with_operator(node, tree):
    for item in tree.nodes:
        item.select = False
    result = run_glsl_node_operator(node, tree, bpy.ops.node.duplicate)
    duplicate = next(item for item in tree.nodes if item.select and item != node)
    return result, duplicate


def run_glsl_node_operator(node, tree, operator):
    screen = bpy.context.screen
    if screen is None or not screen.areas:
        raise RuntimeError("No screen area available for node refresh operator context")

    area = screen.areas[0]
    area.type = 'NODE_EDITOR'
    region = next((region for region in area.regions if region.type == 'WINDOW'), None)
    if region is None:
        raise RuntimeError("No window region available for node refresh operator context")

    space = area.spaces.active
    space.tree_type = 'ShaderNodeTree'
    space.node_tree = tree
    for item in tree.nodes:
        item.select = False
    tree.nodes.active = node
    node.select = True

    with bpy.context.temp_override(
        area=area,
        region=region,
        space_data=space,
        node=node,
        active_node=node,
        edit_tree=tree,
        node_tree=tree,
    ):
        return operator()


def relink_and_update(tree, from_socket, to_socket):
    tree.links.new(from_socket, to_socket)
    tree.interface_update(bpy.context)
    tree.update_tag()
    bpy.context.view_layer.update()


def make_closure_sampler(tree, *, include_uv=True, color_type="FLOAT", alpha_type=None):
    closure_input = tree.nodes.new("NodeClosureInput")
    closure_output = tree.nodes.new("NodeClosureOutput")
    closure_input.pair_with_output(closure_output)
    if include_uv:
        closure_output.input_items.new("VECTOR", "UV")
    if color_type is not None:
        closure_output.output_items.new(color_type, "Color")
    if alpha_type is not None:
        closure_output.output_items.new(alpha_type, "Alpha")
    tree.interface_update(bpy.context)
    tree.update_tag()
    bpy.context.view_layer.update()
    return closure_input, closure_output


def make_synced_glsl_callback(tree, glsl_node, helper_name):
    closure_input = tree.nodes.new("NodeClosureInput")
    closure_output = tree.nodes.new("NodeClosureOutput")
    closure_input.pair_with_output(closure_output)
    relink_and_update(
        tree,
        find_socket(closure_output.outputs, "Closure"),
        find_socket(glsl_node.inputs, f"closure.{helper_name}"),
    )
    result = sync_closure_output_with_operator(closure_output, tree)
    return closure_input, closure_output, result


def glsl_callback_sockets(node):
    return [
        socket
        for socket in node.inputs
        if socket.type == 'CLOSURE' and socket.identifier.startswith("closure.")
    ]


def make_menu_context(shader_type, engine="BLENDER_EEVEE", tree_type="ShaderNodeTree"):
    return SimpleNamespace(
        engine=engine,
        space_data=SimpleNamespace(tree_type=tree_type, shader_type=shader_type),
    )


class GLSLFunctionNodeTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_homefile(use_factory_startup=True)

    def make_material_tree(self, domain="SURFACE"):
        material = bpy.data.materials.new(name="GLSLFunctionTest")
        material.use_nodes = True
        material.eevee_domain = domain
        tree = material.node_tree
        tree.nodes.clear()
        return material, tree

    def configure_glsl_node(self, node, text_name, function_name):
        node.source_mode = 'INTERNAL'
        node.script = bpy.data.texts[text_name]
        node.function_name = function_name
        refresh_glsl_node(node)

    def test_top_level_uniform_is_rejected_at_eof(self):
        _, tree = self.make_material_tree()
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = "uniform sampler2D image_tex\nvec4 shade(vec2 uv){return texture(image_tex, uv);}"
        make_text_block("glsl_uniform_rejected.glsl", source)

        self.configure_glsl_node(node, "glsl_uniform_rejected.glsl", "shade")

        self.assertEqual(node.parse_status, 'ERROR')

    def test_removed_light_alignment_helpers_are_rejected(self):
        _, tree = self.make_material_tree()
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = (
            "vec4 legacy_light(vec3 normal, vec3 incoming){\n"
            "  float value = glsl_light_diffuse_attenuation(0, normal, incoming);\n"
            "  return vec4(vec3(value), 1.0);\n"
            "}\n"
        )
        make_text_block("glsl_removed_light_helper.glsl", source)

        self.configure_glsl_node(node, "glsl_removed_light_helper.glsl", "legacy_light")

        self.assertEqual(node.parse_status, 'ERROR')

    def test_vec4_input_splits_xyz_and_w_sockets(self):
        _, tree = self.make_material_tree()
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = (
            "/* @glsl_meta v1\n"
            "color: default=vec4(0.1, 0.2, 0.3, 0.4)\n"
            "*/\n"
            "vec4 passthrough_vec4(vec4 color){\n"
            "  return color;\n"
            "}\n"
        )
        make_text_block("glsl_vec4_input_socket.glsl", source)

        self.configure_glsl_node(node, "glsl_vec4_input_socket.glsl", "passthrough_vec4")

        self.assertEqual(node.parse_status, 'READY')
        color_socket = find_socket(node.inputs, "color")
        color_w_socket = find_socket(node.inputs, "In_color_w")
        self.assertEqual(color_socket.bl_idname, "NodeSocketVector")
        self.assertEqual(len(color_socket.default_value), 3)
        self.assertEqual(color_w_socket.bl_idname, "NodeSocketFloat")
        self.assertAlmostEqual(color_socket.default_value[0], 0.1)
        self.assertAlmostEqual(color_socket.default_value[1], 0.2)
        self.assertAlmostEqual(color_socket.default_value[2], 0.3)
        self.assertAlmostEqual(color_w_socket.default_value, 0.4)

        color_socket.default_value = (0.0, 0.0, 0.0)
        color_w_socket.default_value = 0.0
        refresh_glsl_node(node)

        self.assertEqual(node.parse_status, 'READY')
        self.assertAlmostEqual(find_socket(node.inputs, "color").default_value[0], 0.0)
        self.assertAlmostEqual(find_socket(node.inputs, "In_color_w").default_value, 0.0)

    def test_matrix_boundaries_split_into_column_sockets(self):
        _, tree = self.make_material_tree()
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = (
            "mat4 matrix_boundaries(mat2 m2, mat3 m3, mat4 m4, out mat3 out_m3){\n"
            "  out_m3 = m3;\n"
            "  return m4;\n"
            "}\n"
        )
        make_text_block("glsl_matrix_boundaries.glsl", source)

        self.configure_glsl_node(node, "glsl_matrix_boundaries.glsl", "matrix_boundaries")

        self.assertEqual(node.parse_status, 'READY')
        for column in range(2):
            socket = find_socket(node.inputs, f"In_m2_c{column}")
            self.assertEqual(socket.name, f"m2 C{column + 1}")
            self.assertEqual(socket.bl_idname, "NodeSocketVector2D")
            self.assertEqual(len(socket.default_value), 2)
        for column in range(3):
            socket = find_socket(node.inputs, f"In_m3_c{column}")
            self.assertEqual(socket.name, f"m3 C{column + 1}")
            self.assertEqual(socket.bl_idname, "NodeSocketVector")
            self.assertEqual(len(socket.default_value), 3)
            out_socket = find_socket(node.outputs, f"Out_out_m3_c{column}")
            self.assertEqual(out_socket.name, f"out_m3 C{column + 1}")
            self.assertEqual(out_socket.bl_idname, "NodeSocketVector")
        for column in range(4):
            socket = find_socket(node.inputs, f"In_m4_c{column}")
            w_socket = find_socket(node.inputs, f"In_m4_c{column}_w")
            result_socket = find_socket(node.outputs, f"Result_c{column}")
            result_w_socket = find_socket(node.outputs, f"Result_c{column}_w")
            self.assertEqual(socket.name, f"m4 C{column + 1}")
            self.assertEqual(w_socket.name, f"m4 C{column + 1} W")
            self.assertEqual(result_socket.name, f"Result C{column + 1}")
            self.assertEqual(result_w_socket.name, f"Result C{column + 1} W")
            self.assertEqual(socket.bl_idname, "NodeSocketVector")
            self.assertEqual(w_socket.bl_idname, "NodeSocketFloat")
            self.assertEqual(result_socket.bl_idname, "NodeSocketVector")
            self.assertEqual(result_w_socket.bl_idname, "NodeSocketFloat")

    def test_split_boundary_generated_identifier_collisions_error(self):
        cases = [
            (
                "glsl_vec4_split_identifier_collision.glsl",
                "vec4_collision",
                "vec4 vec4_collision(vec4 color2, float color2_w){\n"
                "  return color2 + vec4(vec3(color2_w), 0.0);\n"
                "}\n",
            ),
            (
                "glsl_mat4_split_identifier_collision.glsl",
                "mat4_collision",
                "mat4 mat4_collision(mat4 m4, float m4_c0_w){\n"
                "  return m4 + mat4(vec4(vec3(m4_c0_w), 0.0), vec4(0.0), vec4(0.0), vec4(0.0));\n"
                "}\n",
            ),
        ]
        for text_name, function_name, source in cases:
            with self.subTest(function_name=function_name):
                _, tree = self.make_material_tree()
                node = tree.nodes.new("ShaderNodeGLSLFunction")
                make_text_block(text_name, source)

                self.configure_glsl_node(node, text_name, function_name)

                self.assertEqual(node.parse_status, 'ERROR')

    def test_image_sample2d_builds_color_output(self):
        _, tree = self.make_material_tree()
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        image_node = tree.nodes.new("ShaderNodeImageToClosure")

        image = bpy.data.images.new("glsl_test_image", 2, 2, alpha=True, float_buffer=True)
        image.generated_color = (0.25, 0.5, 0.75, 1.0)
        image_node.image = image

        source = "vec4 sample_color(sampler2D src, vec2 uv){\n" "  return texture(src, uv);\n" "}\n"
        make_text_block("glsl_sample2d_image.glsl", source)
        self.configure_glsl_node(glsl_node, "glsl_sample2d_image.glsl", "sample_color")

        self.assertIsNotNone(find_socket(glsl_node.inputs, "src"))
        self.assertIsNotNone(find_socket(glsl_node.inputs, "uv"))
        self.assertIsNotNone(find_socket(glsl_node.outputs, "Result"))

        relink_and_update(
            tree,
            find_socket(image_node.outputs, "Closure"),
            find_socket(glsl_node.inputs, "src"),
        )
        refresh_glsl_node(glsl_node)

        self.assertEqual(glsl_node.parse_status, 'READY')

    def test_group_input_sample2d_defers_source_validation(self):
        group = bpy.data.node_groups.new("GLSLGroupInputSamplerTest", "ShaderNodeTree")
        group_input = group.nodes.new("NodeGroupInput")
        glsl_node = group.nodes.new("ShaderNodeGLSLFunction")
        group.interface.new_socket(name="tex", in_out='INPUT', socket_type="NodeSocketClosure")
        group.interface_update(bpy.context)

        source = "vec4 sample_group_input(sampler2D tex, vec2 uv){\n" "  return texture(tex, uv);\n" "}\n"
        make_text_block("glsl_group_input_sampler.glsl", source)
        self.configure_glsl_node(glsl_node, "glsl_group_input_sampler.glsl", "sample_group_input")

        relink_and_update(
            group,
            find_socket(group_input.outputs, "tex"),
            find_socket(glsl_node.inputs, "tex"),
        )
        refresh_glsl_node(glsl_node)

        self.assertEqual(glsl_node.parse_status, 'READY')

    def test_meta_label_sets_socket_display_name(self):
        _, tree = self.make_material_tree()
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        color_label = "\u57fa\u7840\u8272"
        texture_label = "\u8d34\u56fe"
        output_label = "\u8f93\u51fa\u8272"
        source = (
            "/* @glsl_meta v1\n"
            f"color: label=\"{color_label}\" default=vec4(0.1, 0.2, 0.3, 0.4)\n"
            f"tex: label=\"{texture_label}\" description=\"Texture input\"\n"
            f"out_color: label=\"{output_label}\"\n"
            "*/\n"
            "vec4 localized_labels(vec4 color, sampler2D tex, vec2 uv, out vec4 out_color){\n"
            "  out_color = color;\n"
            "  return color;\n"
            "}\n"
        )
        make_text_block("glsl_meta_label.glsl", source)
        self.configure_glsl_node(glsl_node, "glsl_meta_label.glsl", "localized_labels")

        self.assertEqual(glsl_node.parse_status, 'READY')
        color_socket = find_socket(glsl_node.inputs, "In_color")
        color_w_socket = find_socket(glsl_node.inputs, "In_color_w")
        tex_socket = find_socket(glsl_node.inputs, "In_tex")
        out_socket = find_socket(glsl_node.outputs, "Out_out_color")
        out_w_socket = find_socket(glsl_node.outputs, "Out_out_color_w")
        self.assertEqual(color_socket.name, color_label)
        self.assertEqual(color_w_socket.name, color_label + " W")
        self.assertEqual(tex_socket.name, texture_label)
        self.assertEqual(out_socket.name, output_label)
        self.assertEqual(out_w_socket.name, output_label + " W")
        self.assertEqual(color_socket.identifier, "In_color")
        self.assertEqual(color_w_socket.identifier, "In_color_w")
        self.assertEqual(tex_socket.identifier, "In_tex")
        self.assertEqual(out_socket.identifier, "Out_out_color")
        self.assertEqual(out_w_socket.identifier, "Out_out_color_w")
        self.assertEqual(color_socket.bl_idname, "NodeSocketVector")
        self.assertEqual(color_w_socket.bl_idname, "NodeSocketFloat")
        self.assertEqual(tex_socket.bl_idname, "NodeSocketClosure")
        self.assertEqual(out_socket.bl_idname, "NodeSocketVector")
        self.assertEqual(out_w_socket.bl_idname, "NodeSocketFloat")

    def test_defines_are_parsed_and_not_sockets(self):
        _, tree = self.make_material_tree()
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = (
            "/* @glsl_defines v1\n"
            "@define USE_RIM bool default=true label=\"Rim\" description=\"Compile rim branch\"\n"
            "@define EFFECT_MODE int default=1 min=0 max=3 label=\"Mode\"\n"
            "*/\n"
            "vec4 define_probe(vec4 color){\n"
            "#ifdef USE_RIM\n"
            "  color.rgb += vec3(0.1);\n"
            "#endif\n"
            "#if EFFECT_MODE == 2\n"
            "  color.rgb *= 0.5;\n"
            "#endif\n"
            "  return color;\n"
            "}\n"
        )
        make_text_block("glsl_defines_probe.glsl", source)

        self.configure_glsl_node(glsl_node, "glsl_defines_probe.glsl", "define_probe")

        self.assertEqual(glsl_node.parse_status, 'READY')
        self.assertEqual([value.name for value in glsl_node.define_values], ["USE_RIM", "EFFECT_MODE"])

        use_rim = find_define_value(glsl_node, "USE_RIM")
        effect_mode = find_define_value(glsl_node, "EFFECT_MODE")
        self.assertEqual(use_rim.type, 'BOOL')
        self.assertTrue(use_rim.bool_value)
        self.assertEqual(effect_mode.type, 'INT')
        self.assertEqual(effect_mode.int_value, 1)

        input_keys = {socket.name for socket in glsl_node.inputs}
        input_keys.update(socket.identifier for socket in glsl_node.inputs)
        self.assertNotIn("USE_RIM", input_keys)
        self.assertNotIn("EFFECT_MODE", input_keys)

    def test_defines_panel_default_closed_option_parses(self):
        _, tree = self.make_material_tree()
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = (
            "/* @glsl_defines v1 closed=true\n"
            "@define USE_RIM bool default=true label=\"Rim\"\n"
            "*/\n"
            "vec4 define_closed(vec4 color){return color;}\n"
        )
        make_text_block("glsl_defines_closed.glsl", source)

        self.configure_glsl_node(glsl_node, "glsl_defines_closed.glsl", "define_closed")

        self.assertEqual(glsl_node.parse_status, 'READY')
        self.assertEqual([value.name for value in glsl_node.define_values], ["USE_RIM"])
        self.assertTrue(find_define_value(glsl_node, "USE_RIM").bool_value)

    def test_define_name_too_long_errors(self):
        _, tree = self.make_material_tree()
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        long_define_name = "A" * 64
        source = (
            "/* @glsl_defines v1\n"
            f"@define {long_define_name} bool default=true\n"
            "*/\n"
            "vec4 define_long_name(vec4 color){return color;}\n"
        )
        make_text_block("glsl_define_long_name.glsl", source)

        self.configure_glsl_node(glsl_node, "glsl_define_long_name.glsl", "define_long_name")

        self.assertEqual(glsl_node.parse_status, 'ERROR')

    def test_top_level_conditional_function_errors(self):
        _, tree = self.make_material_tree()
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = (
            "/* @glsl_defines v1\n"
            "@define USE_ALT bool default=true\n"
            "*/\n"
            "#ifdef USE_ALT\n"
            "vec4 conditional_helper(vec4 color){return color + vec4(0.1);}\n"
            "#endif\n"
            "vec4 conditional_export(vec4 color){return color;}\n"
        )
        make_text_block("glsl_conditional_top_level_function.glsl", source)

        self.configure_glsl_node(
            glsl_node, "glsl_conditional_top_level_function.glsl", "conditional_export")

        self.assertEqual(glsl_node.parse_status, 'ERROR')

    def test_top_level_preprocessor_only_conditional_parses(self):
        _, tree = self.make_material_tree()
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = (
            "/* @glsl_defines v1\n"
            "@define USE_CONST bool default=true\n"
            "*/\n"
            "#ifdef USE_CONST\n"
            "#define GLSL_CONST_VALUE 0.25\n"
            "#endif\n"
            "vec4 conditional_define_only(vec4 color){return color + vec4(GLSL_CONST_VALUE);}\n"
        )
        make_text_block("glsl_conditional_define_only.glsl", source)

        self.configure_glsl_node(
            glsl_node, "glsl_conditional_define_only.glsl", "conditional_define_only")

        self.assertEqual(glsl_node.parse_status, 'READY')

    def test_define_header_unknown_attribute_errors(self):
        _, tree = self.make_material_tree()
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = (
            "/* @glsl_defines v1 open=true\n"
            "@define USE_RIM bool default=true\n"
            "*/\n"
            "vec4 define_bad_header(vec4 color){return color;}\n"
        )
        make_text_block("glsl_defines_bad_header.glsl", source)

        self.configure_glsl_node(glsl_node, "glsl_defines_bad_header.glsl", "define_bad_header")

        self.assertEqual(glsl_node.parse_status, 'ERROR')

    def test_conflicting_define_panel_default_closed_values_error(self):
        _, tree = self.make_material_tree()
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = (
            "/* @glsl_defines v1 closed=true\n"
            "@define USE_RIM bool default=true\n"
            "*/\n"
            "/* @glsl_defines v1 closed=false\n"
            "@define EFFECT_MODE int default=1\n"
            "*/\n"
            "vec4 define_conflicting_panels(vec4 color){return color;}\n"
        )
        make_text_block("glsl_defines_conflicting_panels.glsl", source)

        self.configure_glsl_node(
            glsl_node, "glsl_defines_conflicting_panels.glsl", "define_conflicting_panels")

        self.assertEqual(glsl_node.parse_status, 'ERROR')

    def test_define_values_preserve_and_drop_stale_entries(self):
        _, tree = self.make_material_tree()
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        text = make_text_block(
            "glsl_defines_preserve.glsl",
            "/* @glsl_defines v1\n"
            "@define USE_RIM bool default=true\n"
            "@define EFFECT_MODE int default=1 min=0 max=3\n"
            "*/\n"
            "vec4 define_preserve(vec4 color){return color;}\n",
        )
        self.configure_glsl_node(glsl_node, "glsl_defines_preserve.glsl", "define_preserve")

        find_define_value(glsl_node, "USE_RIM").bool_value = False
        find_define_value(glsl_node, "EFFECT_MODE").int_value = 2
        refresh_glsl_node(glsl_node)

        self.assertEqual(glsl_node.parse_status, 'READY')
        self.assertFalse(find_define_value(glsl_node, "USE_RIM").bool_value)
        self.assertEqual(find_define_value(glsl_node, "EFFECT_MODE").int_value, 2)

        text.clear()
        text.write(
            "/* @glsl_defines v1\n"
            "@define USE_RIM bool default=true\n"
            "*/\n"
            "vec4 define_preserve(vec4 color){return color;}\n"
        )
        refresh_glsl_node(glsl_node)

        self.assertEqual(glsl_node.parse_status, 'READY')
        self.assertEqual([value.name for value in glsl_node.define_values], ["USE_RIM"])
        self.assertFalse(find_define_value(glsl_node, "USE_RIM").bool_value)

    def test_int_define_choice_items_preserve_and_fallback(self):
        _, tree = self.make_material_tree()
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        text = make_text_block(
            "glsl_define_int_choices.glsl",
            "/* @glsl_defines v1\n"
            "@define METHOD int default=1 label=\"Method\" "
            "items=\"0:Burley;1:Random Walk;2:Skin\" show_label=true\n"
            "*/\n"
            "vec4 define_int_choices(vec4 color){\n"
            "#if METHOD == 2\n"
            "  return color * 0.5;\n"
            "#else\n"
            "  return color;\n"
            "#endif\n"
            "}\n",
        )
        self.configure_glsl_node(glsl_node, "glsl_define_int_choices.glsl", "define_int_choices")

        self.assertEqual(glsl_node.parse_status, 'READY')
        method = find_define_value(glsl_node, "METHOD")
        self.assertEqual(method.type, 'INT')
        self.assertEqual(method.int_value, 1)
        self.assertEqual(method.choice_value, 'VALUE_1')

        method.choice_value = 'VALUE_2'
        refresh_glsl_node(glsl_node)

        self.assertEqual(glsl_node.parse_status, 'READY')
        self.assertEqual(find_define_value(glsl_node, "METHOD").int_value, 2)
        self.assertEqual(find_define_value(glsl_node, "METHOD").choice_value, 'VALUE_2')

        text.clear()
        text.write(
            "/* @glsl_defines v1\n"
            "@define METHOD int default=0 label=\"Method\" "
            "items=\"0:Burley;1:Random Walk\"\n"
            "*/\n"
            "vec4 define_int_choices(vec4 color){return color;}\n"
        )
        refresh_glsl_node(glsl_node)

        self.assertEqual(glsl_node.parse_status, 'READY')
        self.assertEqual(find_define_value(glsl_node, "METHOD").int_value, 0)

    def test_define_values_do_not_change_signature_hash(self):
        _, tree = self.make_material_tree()
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = (
            "/* @glsl_defines v1\n"
            "@define USE_RIM bool default=true\n"
            "*/\n"
            "vec4 define_signature(vec4 color){return color;}\n"
        )
        make_text_block("glsl_defines_signature.glsl", source)
        self.configure_glsl_node(glsl_node, "glsl_defines_signature.glsl", "define_signature")

        self.assertEqual(glsl_node.parse_status, 'READY')
        signature_hash = glsl_node.signature_hash

        find_define_value(glsl_node, "USE_RIM").bool_value = False
        refresh_glsl_node(glsl_node)

        self.assertEqual(glsl_node.parse_status, 'READY')
        self.assertEqual(glsl_node.signature_hash, signature_hash)

    def test_invalid_int_choice_items_error(self):
        cases = [
            (
                "glsl_bad_items_float_param.glsl",
                "bad_items_float_param",
                "/* @glsl_meta v1\n"
                "mode: default=0.0 items=\"0:Zero;1:One\"\n"
                "*/\n"
                "float bad_items_float_param(float mode){return mode;}\n",
            ),
            (
                "glsl_bad_items_bool_define.glsl",
                "bad_items_bool_define",
                "/* @glsl_defines v1\n"
                "@define USE_RIM bool default=true items=\"0:Off;1:On\"\n"
                "*/\n"
                "vec4 bad_items_bool_define(vec4 color){return color;}\n",
            ),
            (
                "glsl_bad_items_duplicate.glsl",
                "bad_items_duplicate",
                "/* @glsl_defines v1\n"
                "@define METHOD int default=1 items=\"1:One;1:Duplicate\"\n"
                "*/\n"
                "vec4 bad_items_duplicate(vec4 color){return color;}\n",
            ),
            (
                "glsl_bad_items_empty_label.glsl",
                "bad_items_empty_label",
                "/* @glsl_defines v1\n"
                "@define METHOD int default=1 items=\"0:;1:One\"\n"
                "*/\n"
                "vec4 bad_items_empty_label(vec4 color){return color;}\n",
            ),
            (
                "glsl_bad_items_default.glsl",
                "bad_items_default",
                "/* @glsl_defines v1\n"
                "@define METHOD int default=2 items=\"0:Zero;1:One\"\n"
                "*/\n"
                "vec4 bad_items_default(vec4 color){return color;}\n",
            ),
            (
                "glsl_bad_items_minmax.glsl",
                "bad_items_minmax",
                "/* @glsl_meta v1\n"
                "mode: default=1 min=0 items=\"0:Zero;1:One\"\n"
                "*/\n"
                "float bad_items_minmax(int mode){return float(mode);}\n",
            ),
            (
                "glsl_bad_show_label_without_items_param.glsl",
                "bad_show_label_without_items_param",
                "/* @glsl_meta v1\n"
                "mode: default=1 show_label=true\n"
                "*/\n"
                "float bad_show_label_without_items_param(int mode){return float(mode);}\n",
            ),
            (
                "glsl_bad_show_label_float_param.glsl",
                "bad_show_label_float_param",
                "/* @glsl_meta v1\n"
                "mode: default=0.0 items=\"0:Zero;1:One\" show_label=true\n"
                "*/\n"
                "float bad_show_label_float_param(float mode){return mode;}\n",
            ),
            (
                "glsl_bad_show_label_bool_define.glsl",
                "bad_show_label_bool_define",
                "/* @glsl_defines v1\n"
                "@define USE_RIM bool default=true show_label=true\n"
                "*/\n"
                "vec4 bad_show_label_bool_define(vec4 color){return color;}\n",
            ),
            (
                "glsl_bad_show_label_without_items_define.glsl",
                "bad_show_label_without_items_define",
                "/* @glsl_defines v1\n"
                "@define METHOD int default=1 show_label=true\n"
                "*/\n"
                "vec4 bad_show_label_without_items_define(vec4 color){return color;}\n",
            ),
        ]

        for text_name, function_name, source in cases:
            with self.subTest(text_name=text_name):
                _, tree = self.make_material_tree()
                glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
                make_text_block(text_name, source)
                self.configure_glsl_node(glsl_node, text_name, function_name)
                self.assertEqual(glsl_node.parse_status, 'ERROR')

    def test_duplicate_define_name_errors(self):
        _, tree = self.make_material_tree()
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = (
            "/* @glsl_defines v1\n"
            "@define USE_RIM bool default=true\n"
            "@define USE_RIM bool default=false\n"
            "*/\n"
            "vec4 duplicate_define(vec4 color){return color;}\n"
        )
        make_text_block("glsl_duplicate_define.glsl", source)

        self.configure_glsl_node(glsl_node, "glsl_duplicate_define.glsl", "duplicate_define")

        self.assertEqual(glsl_node.parse_status, 'ERROR')

    def test_node_group_refresh_operator_updates_glsl_sockets(self):
        group = bpy.data.node_groups.new("GLSLGroupRefreshOperatorTest", "ShaderNodeTree")
        glsl_node = group.nodes.new("ShaderNodeGLSLFunction")
        source = "vec4 refresh_probe(vec4 color){\n" "  return color;\n" "}\n"
        text = make_text_block("glsl_group_refresh_probe.glsl", source)
        self.configure_glsl_node(glsl_node, "glsl_group_refresh_probe.glsl", "refresh_probe")

        self.assertEqual(glsl_node.parse_status, 'READY')
        self.assertIsNotNone(find_socket(glsl_node.inputs, "color"))

        text.clear()
        text.write(
            "vec4 refresh_probe(vec4 color, float strength){\n"
            "  return color * strength;\n"
            "}\n"
        )
        result = refresh_glsl_node_with_operator(glsl_node, group)

        self.assertEqual(result, {'FINISHED'})
        self.assertEqual(glsl_node.parse_status, 'READY')
        self.assertIsNotNone(find_socket(glsl_node.inputs, "strength"))

    def test_unlinked_sample3d_is_ready(self):
        _, tree = self.make_material_tree()
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = (
            "vec4 sample_volume(sampler3D volume, vec3 coord){\n"
            "  return texture(volume, coord);\n"
            "}\n"
        )
        make_text_block("glsl_unlinked_sample3d.glsl", source)

        self.configure_glsl_node(glsl_node, "glsl_unlinked_sample3d.glsl", "sample_volume")

        self.assertEqual(glsl_node.parse_status, 'READY')
        self.assertEqual(find_socket(glsl_node.inputs, "volume").bl_idname, "NodeSocketClosure")

    def test_typed_closure_callback_meta_syncs_to_zone_sockets(self):
        _, tree = self.make_material_tree()
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = (
            "/* @glsl_closure v1 label=\"Remap Callback\" "
            "description=\"Override the remap helper\" closed=true\n"
            "color: label=\"Color\" description=\"Color passed to the helper\" "
            "default=vec3(0.1, 0.2, 0.3) subtype=color hide_value=true\n"
            "strength: label=\"Strength\" description=\"Remap amount\" "
            "default=0.25 min=0.0 max=1.0 subtype=factor\n"
            "steps: label=\"Steps\" default=3 min=1 max=8\n"
            "enabled: label=\"Enabled\" default=true hide_value=true\n"
            "Result: label=\"Mapped Color\" description=\"Remapped helper result\" subtype=color\n"
            "mask: label=\"Mask\" description=\"Mask produced by the helper\"\n"
            "*/\n"
            "vec3 remap(vec3 color, float strength, int steps, bool enabled, out float mask){\n"
            "  mask = enabled ? strength : 0.0;\n"
            "  return color * mask * float(steps);\n"
            "}\n"
            "vec4 closure_meta_export(vec3 base_color){\n"
            "  float mask;\n"
            "  vec3 result = remap(base_color, 0.5, 2, true, mask);\n"
            "  return vec4(result * mask, 1.0);\n"
            "}\n"
        )
        make_text_block("glsl_typed_closure_meta.glsl", source)
        self.configure_glsl_node(
            glsl_node, "glsl_typed_closure_meta.glsl", "closure_meta_export"
        )

        self.assertEqual(glsl_node.parse_status, 'READY')
        callback_socket = find_socket(glsl_node.inputs, "closure.remap")
        self.assertEqual(callback_socket.name, "Remap Callback")
        self.assertEqual(callback_socket.description, "Override the remap helper")
        self.assertTrue(any(state.is_collapsed for state in glsl_node.panel_states))

        closure_input, closure_output, sync_result = make_synced_glsl_callback(
            tree, glsl_node, "remap"
        )
        self.assertEqual(sync_result, {'FINISHED'})
        self.assertEqual(
            [item.name for item in closure_output.input_items],
            ["color", "strength", "steps", "enabled"],
        )
        self.assertEqual(
            [item.name for item in closure_output.output_items], ["Result", "mask"]
        )

        color = find_socket(closure_input.outputs, "color")
        strength = find_socket(closure_input.outputs, "strength")
        steps = find_socket(closure_input.outputs, "steps")
        enabled = find_socket(closure_input.outputs, "enabled")
        result = find_socket(closure_output.inputs, "Result")
        mask = find_socket(closure_output.inputs, "mask")

        self.assertEqual(color.bl_idname, "NodeSocketColor")
        self.assertEqual(color.label, "Color")
        self.assertEqual(color.description, "Color passed to the helper")
        self.assertTrue(color.hide_value)
        for actual, expected in zip(color.default_value, (0.1, 0.2, 0.3, 1.0)):
            self.assertAlmostEqual(actual, expected)
        self.assertEqual(strength.type, 'VALUE')
        self.assertEqual(strength.label, "Strength")
        self.assertEqual(strength.description, "Remap amount")
        self.assertAlmostEqual(strength.default_value, 0.25)
        self.assertEqual(steps.type, 'INT')
        self.assertEqual(steps.label, "Steps")
        self.assertEqual(steps.default_value, 3)
        self.assertEqual(enabled.type, 'BOOLEAN')
        self.assertEqual(enabled.label, "Enabled")
        self.assertTrue(enabled.default_value)
        self.assertTrue(enabled.hide_value)
        self.assertEqual(result.bl_idname, "NodeSocketColor")
        self.assertEqual(result.label, "Mapped Color")
        self.assertEqual(result.description, "Remapped helper result")
        self.assertEqual(mask.type, 'VALUE')
        self.assertEqual(mask.label, "Mask")
        self.assertEqual(mask.description, "Mask produced by the helper")

    def test_typed_closure_callback_meta_syncs_to_evaluate_closure_and_persists(self):
        material, tree = self.make_material_tree()
        material.use_fake_user = True
        material_name = material.name
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = (
            "/* @glsl_closure v1 label=\"Evaluate Meta\"\n"
            "value: label=\"Value\" description=\"Exact integer input\" "
            "default=16777216 min=-16777216 max=16777216\n"
            "enabled: label=\"Enabled\" default=true hide_value=true\n"
            "Result: label=\"Result\"\n"
            "*/\n"
            "int evaluate_meta(int value, bool enabled){return enabled ? value : 0;}\n"
            "vec4 evaluate_meta_export(float value){"
            "return vec4(float(evaluate_meta(int(value), true)));}\n"
        )
        make_text_block("glsl_typed_closure_evaluate_meta.glsl", source)
        self.configure_glsl_node(
            glsl_node, "glsl_typed_closure_evaluate_meta.glsl", "evaluate_meta_export"
        )
        self.assertEqual(glsl_node.parse_status, 'READY')

        closure_input, closure_output, sync_result = make_synced_glsl_callback(
            tree, glsl_node, "evaluate_meta"
        )
        self.assertEqual(sync_result, {'FINISHED'})
        evaluate = tree.nodes.new("NodeEvaluateClosure")
        evaluate.name = "Evaluate Callback Meta"
        relink_and_update(
            tree,
            find_socket(closure_output.outputs, "Closure"),
            find_socket(evaluate.inputs, "Closure"),
        )
        self.assertEqual(sync_closure_output_with_operator(evaluate, tree), {'FINISHED'})
        tree.interface_update(bpy.context)
        tree.update_tag()
        bpy.context.view_layer.update()

        def assert_evaluate_meta(node):
            value = find_socket(node.inputs, "value")
            enabled = find_socket(node.inputs, "enabled")
            result = find_socket(node.outputs, "Result")
            self.assertEqual(value.bl_idname, "NodeSocketInt")
            self.assertEqual(value.label, "value")
            self.assertEqual(value.description, "Exact integer input")
            self.assertEqual(value.default_value, 16777216)
            self.assertEqual(enabled.bl_idname, "NodeSocketBool")
            self.assertEqual(enabled.label, "enabled")
            self.assertTrue(enabled.default_value)
            self.assertFalse(enabled.hide_value)
            self.assertEqual(result.bl_idname, "NodeSocketInt")
            self.assertEqual(result.label, "Result")

        assert_evaluate_meta(evaluate)
        evaluate_name = evaluate.name
        closure_input_name = closure_input.name
        with tempfile.TemporaryDirectory() as directory:
            filepath = Path(directory) / "glsl_typed_closure_evaluate_meta.blend"
            self.assertEqual(
                bpy.ops.wm.save_as_mainfile(filepath=str(filepath), check_existing=False),
                {'FINISHED'},
            )
            self.assertEqual(
                bpy.ops.wm.open_mainfile(filepath=str(filepath), load_ui=False), {'FINISHED'}
            )
            loaded_tree = bpy.data.materials[material_name].node_tree
            loaded_evaluate = loaded_tree.nodes[evaluate_name]
            assert_evaluate_meta(loaded_evaluate)
            self.assertEqual(
                find_socket(loaded_tree.nodes[closure_input_name].outputs, "value").default_value,
                16777216,
            )

    def test_typed_closure_callback_maps_all_supported_types_and_vec4_keys(self):
        material, tree = self.make_material_tree()
        material.use_fake_user = True
        material_name = material.name
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = (
            "/* @glsl_closure v1\n"
            "scalar: default=0.4\n"
            "count: default=2\n"
            "enabled: default=true\n"
            "uv: default=vec2(0.1, 0.2)\n"
            "vector: default=vec3(0.3, 0.4, 0.5)\n"
            "tint: default=vec3(0.6, 0.7, 0.8) subtype=color\n"
            "packed: default=vec4(0.6, 0.7, 0.8, 0.9)\n"
            "out_tint: subtype=color\n"
            "*/\n"
            "vec4 typed_helper(float scalar, int count, bool enabled, vec2 uv, vec3 vector, "
            "vec3 tint, vec4 packed, out float out_scalar, out int out_count, "
            "out bool out_enabled, out vec2 out_uv, out vec3 out_vector, "
            "out vec3 out_tint, out vec4 out_packed){\n"
            "  out_scalar = scalar;\n"
            "  out_count = count;\n"
            "  out_enabled = enabled;\n"
            "  out_uv = uv;\n"
            "  out_vector = vector;\n"
            "  out_tint = tint;\n"
            "  out_packed = packed;\n"
            "  return packed;\n"
            "}\n"
            "vec4 typed_callback_export(float value){\n"
            "  float out_scalar; int out_count; bool out_enabled; vec2 out_uv;\n"
            "  vec3 out_vector; vec3 out_tint; vec4 out_packed;\n"
            "  return typed_helper(value, 2, true, vec2(0.1), vec3(0.2), vec3(0.3), "
            "vec4(0.4), out_scalar, out_count, out_enabled, out_uv, out_vector, "
            "out_tint, out_packed);\n"
            "}\n"
        )
        make_text_block("glsl_typed_closure_types.glsl", source)
        self.configure_glsl_node(
            glsl_node, "glsl_typed_closure_types.glsl", "typed_callback_export"
        )

        self.assertEqual(glsl_node.parse_status, 'READY')
        self.assertEqual(
            [socket.identifier for socket in glsl_callback_sockets(glsl_node)],
            ["closure.typed_helper"],
        )
        closure_input, closure_output, sync_result = make_synced_glsl_callback(
            tree, glsl_node, "typed_helper"
        )
        self.assertEqual(sync_result, {'FINISHED'})

        expected_inputs = [
            "scalar",
            "count",
            "enabled",
            "uv",
            "vector",
            "tint",
            "packed",
            "packed.__w",
        ]
        expected_outputs = [
            "Result",
            "Result.__w",
            "out_scalar",
            "out_count",
            "out_enabled",
            "out_uv",
            "out_vector",
            "out_tint",
            "out_packed",
            "out_packed.__w",
        ]
        self.assertEqual(
            [item.name for item in closure_output.input_items], expected_inputs
        )
        self.assertEqual(
            [item.name for item in closure_output.output_items], expected_outputs
        )

        input_types = {
            key: find_socket(closure_input.outputs, key).bl_idname for key in expected_inputs
        }
        self.assertEqual(input_types["scalar"], "NodeSocketFloat")
        self.assertEqual(input_types["count"], "NodeSocketInt")
        self.assertEqual(input_types["enabled"], "NodeSocketBool")
        self.assertEqual(input_types["uv"], "NodeSocketVector2D")
        self.assertEqual(input_types["vector"], "NodeSocketVector")
        self.assertEqual(input_types["tint"], "NodeSocketColor")
        self.assertEqual(input_types["packed"], "NodeSocketVector")
        self.assertEqual(input_types["packed.__w"], "NodeSocketFloat")
        self.assertAlmostEqual(find_socket(closure_input.outputs, "scalar").default_value, 0.4)
        self.assertEqual(find_socket(closure_input.outputs, "count").default_value, 2)
        self.assertTrue(find_socket(closure_input.outputs, "enabled").default_value)
        for actual, expected in zip(
            find_socket(closure_input.outputs, "uv").default_value, (0.1, 0.2)
        ):
            self.assertAlmostEqual(actual, expected)
        for actual, expected in zip(
            find_socket(closure_input.outputs, "vector").default_value, (0.3, 0.4, 0.5)
        ):
            self.assertAlmostEqual(actual, expected)
        for actual, expected in zip(
            find_socket(closure_input.outputs, "tint").default_value, (0.6, 0.7, 0.8, 1.0)
        ):
            self.assertAlmostEqual(actual, expected)
        for actual, expected in zip(
            find_socket(closure_input.outputs, "packed").default_value, (0.6, 0.7, 0.8)
        ):
            self.assertAlmostEqual(actual, expected)
        self.assertAlmostEqual(
            find_socket(closure_input.outputs, "packed.__w").default_value, 0.9
        )

        output_types = {
            key: find_socket(closure_output.inputs, key).bl_idname for key in expected_outputs
        }
        self.assertEqual(output_types["Result"], "NodeSocketVector")
        self.assertEqual(output_types["Result.__w"], "NodeSocketFloat")
        self.assertEqual(output_types["out_scalar"], "NodeSocketFloat")
        self.assertEqual(output_types["out_count"], "NodeSocketInt")
        self.assertEqual(output_types["out_enabled"], "NodeSocketBool")
        self.assertEqual(output_types["out_uv"], "NodeSocketVector2D")
        self.assertEqual(output_types["out_vector"], "NodeSocketVector")
        self.assertEqual(output_types["out_tint"], "NodeSocketColor")
        self.assertEqual(output_types["out_packed"], "NodeSocketVector")
        self.assertEqual(output_types["out_packed.__w"], "NodeSocketFloat")

        closure_input_name = closure_input.name
        closure_output_name = closure_output.name
        with tempfile.TemporaryDirectory() as directory:
            filepath = Path(directory) / "glsl_typed_closure_dimensions.blend"
            self.assertEqual(
                bpy.ops.wm.save_as_mainfile(filepath=str(filepath), check_existing=False),
                {'FINISHED'},
            )
            self.assertEqual(
                bpy.ops.wm.open_mainfile(filepath=str(filepath), load_ui=False), {'FINISHED'}
            )
            loaded_tree = bpy.data.materials[material_name].node_tree
            loaded_input = loaded_tree.nodes[closure_input_name]
            loaded_output = loaded_tree.nodes[closure_output_name]
            self.assertEqual(
                find_socket(loaded_input.outputs, "uv").bl_idname, "NodeSocketVector2D"
            )
            self.assertEqual(
                find_socket(loaded_output.inputs, "out_uv").bl_idname, "NodeSocketVector2D"
            )

    def test_typed_closure_callbacks_only_expose_reachable_helpers_in_definition_order(self):
        _, tree = self.make_material_tree()
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = (
            "/* @glsl_closure v1 label=\"Unused\" closed=true\n"
            "*/\n"
            "float unused_helper(float value){return value + 10.0;}\n"
            "/* @glsl_closure v1 label=\"Downstream\"\n"
            "*/\n"
            "float downstream(float value){return value * 2.0;}\n"
            "/* @glsl_closure v1 label=\"Upstream\"\n"
            "*/\n"
            "float upstream(float value){return downstream(value) + 1.0;}\n"
            "vec4 reachable_callback_export(float value){\n"
            "  return vec4(upstream(value));\n"
            "}\n"
        )
        make_text_block("glsl_typed_closure_reachable.glsl", source)
        self.configure_glsl_node(
            glsl_node, "glsl_typed_closure_reachable.glsl", "reachable_callback_export"
        )

        self.assertEqual(glsl_node.parse_status, 'READY')
        callbacks = glsl_callback_sockets(glsl_node)
        self.assertEqual(
            [socket.identifier for socket in callbacks],
            ["closure.downstream", "closure.upstream"],
        )
        self.assertEqual([socket.name for socket in callbacks], ["Downstream", "Upstream"])
        self.assertNotIn(
            "closure.unused_helper", {socket.identifier for socket in glsl_node.inputs}
        )

    def test_typed_closure_callback_line_comment_disables_annotation(self):
        _, tree = self.make_material_tree()
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = (
            "// /* @glsl_closure v1 */\n"
            "float helper(float value){return value * 2.0;}\n"
            "vec4 line_comment_export(float value){return vec4(helper(value));}\n"
        )
        make_text_block("glsl_typed_closure_line_comment.glsl", source)
        self.configure_glsl_node(
            glsl_node, "glsl_typed_closure_line_comment.glsl", "line_comment_export"
        )

        self.assertEqual(glsl_node.parse_status, 'READY')
        self.assertEqual(glsl_callback_sockets(glsl_node), [])

    def test_typed_closure_callback_sync_preserves_identity_links_and_values(self):
        material, tree = self.make_material_tree()
        material.use_fake_user = True
        material_name = material.name
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        text = make_text_block(
            "glsl_typed_closure_sync.glsl",
            "/* @glsl_closure v1 label=\"Callback A\"\n"
            "value: label=\"Value A\" description=\"Input A\" default=0.25 min=0.0 max=1.0\n"
            "Result: label=\"Result A\" description=\"Output A\"\n"
            "*/\n"
            "float remap(float value){return value;}\n"
            "vec4 callback_sync_export(float value){return vec4(remap(value));}\n",
        )
        self.configure_glsl_node(
            glsl_node, "glsl_typed_closure_sync.glsl", "callback_sync_export"
        )
        self.assertEqual(glsl_node.parse_status, 'READY')
        closure_input, closure_output, sync_result = make_synced_glsl_callback(
            tree, glsl_node, "remap"
        )
        self.assertEqual(sync_result, {'FINISHED'})

        callback = find_socket(glsl_node.inputs, "closure.remap")
        value_socket = find_socket(closure_input.outputs, "value")
        result_socket = find_socket(closure_output.inputs, "Result")
        self.assertEqual(value_socket.label, "Value A")
        self.assertEqual(result_socket.label, "Result A")
        callback_identifier = callback.identifier
        value_identifier = value_socket.identifier
        result_identifier = result_socket.identifier
        value_socket.default_value = 0.75

        value_node = tree.nodes.new("ShaderNodeValue")
        value_node.outputs[0].default_value = 0.6
        relink_and_update(tree, value_node.outputs[0], result_socket)

        text.clear()
        text.write(
            "/* @glsl_closure v1 label=\"Callback B\"\n"
            "value: label=\"Value B\" description=\"Input B\" default=0.1 min=-1.0 max=2.0\n"
            "Result: label=\"Result B\" description=\"Output B\"\n"
            "*/\n"
            "float remap(float value){return value;}\n"
            "vec4 callback_sync_export(float value){return vec4(remap(value));}\n"
        )
        refresh_glsl_node(glsl_node)

        self.assertEqual(glsl_node.parse_status, 'READY')
        callback = find_socket(glsl_node.inputs, "closure.remap")
        self.assertEqual(callback.identifier, callback_identifier)
        self.assertEqual(callback.name, "Callback B")
        self.assertTrue(callback.is_linked)
        self.assertEqual(sync_closure_output_with_operator(closure_output, tree), {'FINISHED'})

        value_socket = find_socket(closure_input.outputs, "value")
        result_socket = find_socket(closure_output.inputs, "Result")
        self.assertEqual(value_socket.identifier, value_identifier)
        self.assertEqual(result_socket.identifier, result_identifier)
        self.assertEqual(value_socket.label, "Value B")
        self.assertEqual(result_socket.label, "Result B")
        self.assertAlmostEqual(value_socket.default_value, 0.75)
        self.assertEqual(value_socket.description, "Input B")
        self.assertEqual(result_socket.description, "Output B")
        self.assertTrue(result_socket.is_linked)
        self.assertEqual(result_socket.links[0].from_node, value_node)

        self.assertEqual(len(glsl_node.panel_states), 1)
        glsl_node.panel_states[0].is_collapsed = True
        panel_identifier = glsl_node.panel_states[0].identifier
        glsl_node_name = glsl_node.name
        closure_input_name = closure_input.name
        closure_output_name = closure_output.name
        value_node_name = value_node.name

        with tempfile.TemporaryDirectory() as directory:
            filepath = Path(directory) / "glsl_typed_closure_sync.blend"
            self.assertEqual(
                bpy.ops.wm.save_as_mainfile(filepath=str(filepath), check_existing=False),
                {'FINISHED'},
            )
            self.assertEqual(
                bpy.ops.wm.open_mainfile(filepath=str(filepath), load_ui=False), {'FINISHED'}
            )

            loaded_tree = bpy.data.materials[material_name].node_tree
            loaded_glsl = loaded_tree.nodes[glsl_node_name]
            loaded_input = loaded_tree.nodes[closure_input_name]
            loaded_output = loaded_tree.nodes[closure_output_name]
            loaded_callback = find_socket(loaded_glsl.inputs, "closure.remap")
            loaded_value = find_socket(loaded_input.outputs, "value")
            loaded_result = find_socket(loaded_output.inputs, "Result")

            self.assertEqual(loaded_glsl.parse_status, 'READY')
            self.assertEqual(loaded_callback.identifier, callback_identifier)
            self.assertEqual(loaded_callback.name, "Callback B")
            self.assertTrue(loaded_callback.is_linked)
            self.assertEqual(loaded_callback.links[0].from_node, loaded_output)
            self.assertEqual(loaded_value.identifier, value_identifier)
            self.assertEqual(loaded_result.identifier, result_identifier)
            self.assertEqual(loaded_value.label, "Value B")
            self.assertEqual(loaded_result.label, "Result B")
            self.assertEqual(loaded_value.description, "Input B")
            self.assertEqual(loaded_result.description, "Output B")
            self.assertAlmostEqual(loaded_value.default_value, 0.75)
            self.assertTrue(loaded_result.is_linked)
            self.assertEqual(loaded_result.links[0].from_node.name, value_node_name)
            self.assertEqual(len(loaded_glsl.panel_states), 1)
            self.assertEqual(loaded_glsl.panel_states[0].identifier, panel_identifier)
            self.assertTrue(loaded_glsl.panel_states[0].is_collapsed)

    def test_typed_closure_callback_closed_change_preserves_panel_identity_and_state(self):
        _, tree = self.make_material_tree()
        text = make_text_block(
            "glsl_typed_closure_panel_identity.glsl",
            "/* @glsl_closure v1 closed=false\n*/\n"
            "float remap(float value){return value;}\n"
            "vec4 panel_identity_export(float value){return vec4(remap(value));}\n",
        )
        first = tree.nodes.new("ShaderNodeGLSLFunction")
        self.configure_glsl_node(
            first, "glsl_typed_closure_panel_identity.glsl", "panel_identity_export"
        )

        self.assertEqual(first.parse_status, 'READY')
        self.assertEqual(len(first.panel_states), 1)
        self.assertFalse(first.panel_states[0].is_collapsed)
        panel_identifier = first.panel_states[0].identifier

        text.clear()
        text.write(
            "/* @glsl_closure v1 closed=true\n*/\n"
            "float remap(float value){return value;}\n"
            "vec4 panel_identity_export(float value){return vec4(remap(value));}\n"
        )
        refresh_glsl_node(first)

        self.assertEqual(first.parse_status, 'READY')
        self.assertEqual(first.panel_states[0].identifier, panel_identifier)
        self.assertFalse(first.panel_states[0].is_collapsed)

        second = tree.nodes.new("ShaderNodeGLSLFunction")
        self.configure_glsl_node(
            second, "glsl_typed_closure_panel_identity.glsl", "panel_identity_export"
        )
        self.assertEqual(second.parse_status, 'READY')
        self.assertEqual(len(second.panel_states), 1)
        self.assertEqual(second.panel_states[0].identifier, panel_identifier)
        self.assertTrue(second.panel_states[0].is_collapsed)

    def test_typed_closure_callback_muted_link_state_survives_refresh(self):
        _, tree = self.make_material_tree()
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = (
            "/* @glsl_closure v1\n*/\n"
            "float remap(float value){return value * 0.5;}\n"
            "vec4 muted_callback_export(float value){return vec4(remap(value));}\n"
        )
        make_text_block("glsl_typed_closure_muted_link.glsl", source)
        self.configure_glsl_node(
            glsl_node, "glsl_typed_closure_muted_link.glsl", "muted_callback_export"
        )
        _, closure_output, sync_result = make_synced_glsl_callback(
            tree, glsl_node, "remap"
        )
        self.assertEqual(sync_result, {'FINISHED'})

        callback = find_socket(glsl_node.inputs, "closure.remap")
        self.assertTrue(callback.is_linked)
        callback.links[0].is_muted = True
        tree.interface_update(bpy.context)
        tree.update_tag()
        bpy.context.view_layer.update()
        refresh_glsl_node(glsl_node)

        callback = find_socket(glsl_node.inputs, "closure.remap")
        self.assertEqual(glsl_node.parse_status, 'READY')
        self.assertTrue(callback.is_linked)
        self.assertTrue(callback.links[0].is_muted)
        self.assertEqual(callback.links[0].from_node, closure_output)

    def test_typed_closure_callback_int_meta_preserves_exact_transport_boundaries(self):
        material, tree = self.make_material_tree()
        material.use_fake_user = True
        material_name = material.name
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = (
            "/* @glsl_closure v1\n"
            "value: default=16777216 min=-16777216 max=16777216\n"
            "*/\n"
            "int exact_int(int value){return value;}\n"
            "vec4 exact_int_export(float value){return vec4(float(exact_int(int(value))));}\n"
        )
        make_text_block("glsl_typed_closure_exact_int.glsl", source)
        self.configure_glsl_node(
            glsl_node, "glsl_typed_closure_exact_int.glsl", "exact_int_export"
        )

        self.assertEqual(glsl_node.parse_status, 'READY')
        closure_input, _, sync_result = make_synced_glsl_callback(
            tree, glsl_node, "exact_int"
        )
        self.assertEqual(sync_result, {'FINISHED'})
        value_socket = find_socket(closure_input.outputs, "value")
        self.assertEqual(value_socket.default_value, 16777216)

        glsl_node_name = glsl_node.name
        closure_input_name = closure_input.name
        with tempfile.TemporaryDirectory() as directory:
            filepath = Path(directory) / "glsl_typed_closure_exact_int.blend"
            self.assertEqual(
                bpy.ops.wm.save_as_mainfile(filepath=str(filepath), check_existing=False),
                {'FINISHED'},
            )
            self.assertEqual(
                bpy.ops.wm.open_mainfile(filepath=str(filepath), load_ui=False), {'FINISHED'}
            )
            loaded_tree = bpy.data.materials[material_name].node_tree
            loaded_glsl = loaded_tree.nodes[glsl_node_name]
            loaded_value = find_socket(loaded_tree.nodes[closure_input_name].outputs, "value")
            self.assertEqual(loaded_glsl.parse_status, 'READY')
            self.assertEqual(loaded_value.default_value, 16777216)

    def test_typed_closure_callback_meta_conflicts_do_not_change_abi(self):
        _, tree = self.make_material_tree()
        first = tree.nodes.new("ShaderNodeGLSLFunction")
        second = tree.nodes.new("ShaderNodeGLSLFunction")
        first_source = (
            "/* @glsl_closure v1\n"
            "value: label=\"First Value\" default=0.25\n"
            "Result: label=\"First Result\"\n"
            "*/\n"
            "float remap(float value){return value;}\n"
            "vec4 first_export(float value){return vec4(remap(value));}\n"
        )
        second_source = (
            "/* @glsl_closure v1\n"
            "value: label=\"Second Value\" default=0.75\n"
            "Result: label=\"Second Result\"\n"
            "*/\n"
            "float remap(float value){return value;}\n"
            "vec4 second_export(float value){return vec4(remap(value));}\n"
        )
        make_text_block("glsl_typed_closure_meta_first.glsl", first_source)
        make_text_block("glsl_typed_closure_meta_second.glsl", second_source)
        self.configure_glsl_node(first, "glsl_typed_closure_meta_first.glsl", "first_export")
        self.configure_glsl_node(second, "glsl_typed_closure_meta_second.glsl", "second_export")

        closure_input = tree.nodes.new("NodeClosureInput")
        closure_output = tree.nodes.new("NodeClosureOutput")
        closure_input.pair_with_output(closure_output)
        closure_socket = find_socket(closure_output.outputs, "Closure")
        relink_and_update(tree, closure_socket, find_socket(first.inputs, "closure.remap"))
        relink_and_update(tree, closure_socket, find_socket(second.inputs, "closure.remap"))

        self.assertEqual(sync_closure_output_with_operator(closure_output, tree), {'FINISHED'})
        self.assertEqual(first.parse_status, 'READY')
        self.assertEqual(second.parse_status, 'READY')
        self.assertEqual(find_socket(closure_input.outputs, "value").type, 'VALUE')
        self.assertEqual(find_socket(closure_output.inputs, "Result").type, 'VALUE')

    def test_typed_closure_callback_color_vector_consumers_use_stable_vector_abi(self):
        color_source = (
            "/* @glsl_closure v1\n"
            "tint: subtype=color\n"
            "Result: subtype=color\n"
            "*/\n"
            "vec3 remap(vec3 tint){return tint;}\n"
            "vec4 color_consumer(vec3 tint){return vec4(remap(tint), 1.0);}\n"
        )
        vector_source = (
            "/* @glsl_closure v1\n*/\n"
            "vec3 remap(vec3 tint){return tint;}\n"
            "vec4 vector_consumer(vec3 tint){return vec4(remap(tint), 1.0);}\n"
        )

        for order in (("color", "vector"), ("vector", "color")):
            with self.subTest(order=order):
                _, tree = self.make_material_tree()
                color = tree.nodes.new("ShaderNodeGLSLFunction")
                vector = tree.nodes.new("ShaderNodeGLSLFunction")
                suffix = "_".join(order)
                color_text = f"glsl_typed_closure_color_consumer_{suffix}.glsl"
                vector_text = f"glsl_typed_closure_vector_consumer_{suffix}.glsl"
                make_text_block(color_text, color_source)
                make_text_block(vector_text, vector_source)
                self.configure_glsl_node(color, color_text, "color_consumer")
                self.configure_glsl_node(vector, vector_text, "vector_consumer")

                closure_input = tree.nodes.new("NodeClosureInput")
                closure_output = tree.nodes.new("NodeClosureOutput")
                closure_input.pair_with_output(closure_output)
                consumers = {"color": color, "vector": vector}
                for consumer_name in order:
                    relink_and_update(
                        tree,
                        find_socket(closure_output.outputs, "Closure"),
                        find_socket(consumers[consumer_name].inputs, "closure.remap"),
                    )

                self.assertEqual(
                    sync_closure_output_with_operator(closure_output, tree), {'FINISHED'}
                )
                self.assertEqual(color.parse_status, 'READY')
                self.assertEqual(vector.parse_status, 'READY')
                self.assertEqual(
                    find_socket(closure_input.outputs, "tint").bl_idname,
                    "NodeSocketVector",
                )
                self.assertEqual(
                    find_socket(closure_output.inputs, "Result").bl_idname,
                    "NodeSocketVector",
                )

    def test_typed_closure_callback_color_vector_switch_preserves_state(self):
        _, tree = self.make_material_tree()
        color = tree.nodes.new("ShaderNodeGLSLFunction")
        vector = tree.nodes.new("ShaderNodeGLSLFunction")
        color_source = (
            "/* @glsl_closure v1\n"
            "tint: subtype=color default=vec3(0.1, 0.2, 0.3)\n"
            "Result: subtype=color\n"
            "*/\n"
            "vec3 remap(vec3 tint){return tint;}\n"
            "vec4 color_switch_export(vec3 tint){return vec4(remap(tint), 1.0);}\n"
        )
        vector_source = (
            "/* @glsl_closure v1\n*/\n"
            "vec3 remap(vec3 tint){return tint;}\n"
            "vec4 vector_switch_export(vec3 tint){return vec4(remap(tint), 1.0);}\n"
        )
        make_text_block("glsl_typed_closure_color_switch.glsl", color_source)
        make_text_block("glsl_typed_closure_vector_switch.glsl", vector_source)
        self.configure_glsl_node(
            color, "glsl_typed_closure_color_switch.glsl", "color_switch_export"
        )
        self.configure_glsl_node(
            vector, "glsl_typed_closure_vector_switch.glsl", "vector_switch_export"
        )

        closure_input, closure_output, sync_result = make_synced_glsl_callback(
            tree, color, "remap"
        )
        self.assertEqual(sync_result, {'FINISHED'})
        tint = find_socket(closure_input.outputs, "tint")
        result = find_socket(closure_output.inputs, "Result")
        self.assertEqual(tint.bl_idname, "NodeSocketColor")
        self.assertEqual(result.bl_idname, "NodeSocketColor")
        tint_identifier = tint.identifier
        result_identifier = result.identifier
        tint.default_value = (0.2, 0.4, 0.6, 1.0)

        source = tree.nodes.new("ShaderNodeCombineXYZ")
        relink_and_update(tree, source.outputs[0], result)
        vector_callback = find_socket(vector.inputs, "closure.remap")
        relink_and_update(tree, closure_output.outputs["Closure"], vector_callback)
        self.assertEqual(sync_closure_output_with_operator(closure_output, tree), {'FINISHED'})

        tint = find_socket(closure_input.outputs, "tint")
        result = find_socket(closure_output.inputs, "Result")
        self.assertEqual(tint.bl_idname, "NodeSocketVector")
        self.assertEqual(result.bl_idname, "NodeSocketVector")
        self.assertEqual(tint.identifier, tint_identifier)
        self.assertEqual(result.identifier, result_identifier)
        for actual, expected in zip(tint.default_value, (0.2, 0.4, 0.6)):
            self.assertAlmostEqual(actual, expected)
        self.assertTrue(find_socket(color.inputs, "closure.remap").is_linked)
        self.assertTrue(vector_callback.is_linked)
        self.assertTrue(result.is_linked)
        self.assertEqual(result.links[0].from_node, source)

        tree.links.remove(vector_callback.links[0])
        tree.interface_update(bpy.context)
        tree.update_tag()
        bpy.context.view_layer.update()
        self.assertEqual(sync_closure_output_with_operator(closure_output, tree), {'FINISHED'})

        tint = find_socket(closure_input.outputs, "tint")
        result = find_socket(closure_output.inputs, "Result")
        self.assertEqual(tint.bl_idname, "NodeSocketColor")
        self.assertEqual(result.bl_idname, "NodeSocketColor")
        self.assertEqual(tint.identifier, tint_identifier)
        self.assertEqual(result.identifier, result_identifier)
        for actual, expected in zip(tint.default_value, (0.2, 0.4, 0.6, 1.0)):
            self.assertAlmostEqual(actual, expected)
        self.assertTrue(find_socket(color.inputs, "closure.remap").is_linked)
        self.assertFalse(vector_callback.is_linked)
        self.assertTrue(result.is_linked)
        self.assertEqual(result.links[0].from_node, source)

    def test_typed_closure_callback_incompatible_switch_replaces_sockets(self):
        _, tree = self.make_material_tree()
        scalar = tree.nodes.new("ShaderNodeGLSLFunction")
        vector = tree.nodes.new("ShaderNodeGLSLFunction")
        make_text_block(
            "glsl_typed_closure_scalar_incompatible.glsl",
            "/* @glsl_closure v1\n*/\n"
            "float remap(float value){return value;}\n"
            "vec4 scalar_incompatible_export(float value){return vec4(remap(value));}\n",
        )
        make_text_block(
            "glsl_typed_closure_vector_incompatible.glsl",
            "/* @glsl_closure v1\n*/\n"
            "vec3 remap(vec3 value){return value;}\n"
            "vec4 vector_incompatible_export(vec3 value){return vec4(remap(value), 1.0);}\n",
        )
        self.configure_glsl_node(
            scalar,
            "glsl_typed_closure_scalar_incompatible.glsl",
            "scalar_incompatible_export",
        )
        self.configure_glsl_node(
            vector,
            "glsl_typed_closure_vector_incompatible.glsl",
            "vector_incompatible_export",
        )

        closure_input, closure_output, sync_result = make_synced_glsl_callback(
            tree, scalar, "remap"
        )
        self.assertEqual(sync_result, {'FINISHED'})
        value = find_socket(closure_input.outputs, "value")
        result = find_socket(closure_output.inputs, "Result")
        value_identifier = value.identifier
        result_identifier = result.identifier
        source = tree.nodes.new("ShaderNodeValue")
        relink_and_update(tree, source.outputs[0], result)

        scalar_callback = find_socket(scalar.inputs, "closure.remap")
        tree.links.remove(scalar_callback.links[0])
        tree.interface_update(bpy.context)
        tree.update_tag()
        bpy.context.view_layer.update()
        vector_callback = find_socket(vector.inputs, "closure.remap")
        relink_and_update(tree, closure_output.outputs["Closure"], vector_callback)
        self.assertEqual(sync_closure_output_with_operator(closure_output, tree), {'FINISHED'})

        value = find_socket(closure_input.outputs, "value")
        result = find_socket(closure_output.inputs, "Result")
        self.assertEqual(value.bl_idname, "NodeSocketVector")
        self.assertEqual(result.bl_idname, "NodeSocketVector")
        self.assertNotEqual(value.identifier, value_identifier)
        self.assertNotEqual(result.identifier, result_identifier)
        self.assertFalse(result.is_linked)

    def test_typed_closure_callback_rejects_invalid_meta(self):
        cases = [
            (
                "bad_header_version",
                "/* @glsl_closure v2\n*/\nfloat helper(float value){return value;}\n"
                "vec4 bad_header_version(float value){return vec4(helper(value));}\n",
            ),
            (
                "bad_header_attribute",
                "/* @glsl_closure v1 open=true\n*/\nfloat helper(float value){return value;}\n"
                "vec4 bad_header_attribute(float value){return vec4(helper(value));}\n",
            ),
            (
                "bad_nested_panel",
                "/* @glsl_closure v1\n@panel Nested\n*/\n"
                "float helper(float value){return value;}\n"
                "vec4 bad_nested_panel(float value){return vec4(helper(value));}\n",
            ),
            (
                "bad_items",
                "/* @glsl_closure v1\nmode: default=0 items=\"0:Off;1:On\"\n*/\n"
                "float helper(int mode){return float(mode);}\n"
                "vec4 bad_items(int mode){return vec4(helper(mode));}\n",
            ),
            (
                "bad_output_default",
                "/* @glsl_closure v1\nResult: default=0.5\n*/\n"
                "float helper(float value){return value;}\n"
                "vec4 bad_output_default(float value){return vec4(helper(value));}\n",
            ),
            (
                "bad_expression_default",
                "/* @glsl_closure v1\nvalue: default=scene_time\n*/\n"
                "float helper(float value){return value;}\n"
                "vec4 bad_expression_default(float value){return vec4(helper(value));}\n",
            ),
            (
                "bad_vec4_subtype",
                "/* @glsl_closure v1\nvalue: subtype=color\n*/\n"
                "vec4 helper(vec4 value){return value;}\n"
                "vec4 bad_vec4_subtype(vec4 value){return helper(value);}\n",
            ),
            (
                "bad_conflicting_closed",
                "/* @glsl_closure v1 closed=true\n*/\n"
                "float first_helper(float value){return value;}\n"
                "/* @glsl_closure v1 closed=false\n*/\n"
                "float second_helper(float value){return value;}\n"
                "vec4 bad_conflicting_closed(float value){\n"
                "  return vec4(first_helper(value) + second_helper(value));\n"
                "}\n",
            ),
            (
                "bad_int_default_exact_transport_overflow",
                "/* @glsl_closure v1\nvalue: default=16777217\n*/\n"
                "int helper(int value){return value;}\n"
                "vec4 bad_int_default_exact_transport_overflow(float value){"
                "return vec4(float(helper(int(value))));}\n",
            ),
            (
                "bad_int_default_exact_transport_underflow",
                "/* @glsl_closure v1\nvalue: default=-16777217\n*/\n"
                "int helper(int value){return value;}\n"
                "vec4 bad_int_default_exact_transport_underflow(float value){"
                "return vec4(float(helper(int(value))));}\n",
            ),
            (
                "bad_int_min_exact_transport_overflow",
                "/* @glsl_closure v1\nvalue: min=16777217\n*/\n"
                "int helper(int value){return value;}\n"
                "vec4 bad_int_min_exact_transport_overflow(float value){"
                "return vec4(float(helper(int(value))));}\n",
            ),
            (
                "bad_int_min_exact_transport_underflow",
                "/* @glsl_closure v1\nvalue: min=-16777217\n*/\n"
                "int helper(int value){return value;}\n"
                "vec4 bad_int_min_exact_transport_underflow(float value){"
                "return vec4(float(helper(int(value))));}\n",
            ),
            (
                "bad_int_max_exact_transport_overflow",
                "/* @glsl_closure v1\nvalue: max=16777217\n*/\n"
                "int helper(int value){return value;}\n"
                "vec4 bad_int_max_exact_transport_overflow(float value){"
                "return vec4(float(helper(int(value))));}\n",
            ),
            (
                "bad_int_max_exact_transport_underflow",
                "/* @glsl_closure v1\nvalue: max=-16777217\n*/\n"
                "int helper(int value){return value;}\n"
                "vec4 bad_int_max_exact_transport_underflow(float value){"
                "return vec4(float(helper(int(value))));}\n",
            ),
            (
                "bad_int_default_overflow",
                "/* @glsl_closure v1\nvalue: default=2147483648\n*/\n"
                "int helper(int value){return value;}\n"
                "vec4 bad_int_default_overflow(float value){return vec4(float(helper(int(value))));}\n",
            ),
            (
                "bad_int_default_underflow",
                "/* @glsl_closure v1\nvalue: default=-2147483649\n*/\n"
                "int helper(int value){return value;}\n"
                "vec4 bad_int_default_underflow(float value){return vec4(float(helper(int(value))));}\n",
            ),
            (
                "bad_int_min_overflow",
                "/* @glsl_closure v1\nvalue: min=2147483648\n*/\n"
                "int helper(int value){return value;}\n"
                "vec4 bad_int_min_overflow(float value){return vec4(float(helper(int(value))));}\n",
            ),
            (
                "bad_int_min_underflow",
                "/* @glsl_closure v1\nvalue: min=-2147483649\n*/\n"
                "int helper(int value){return value;}\n"
                "vec4 bad_int_min_underflow(float value){return vec4(float(helper(int(value))));}\n",
            ),
            (
                "bad_int_max_overflow",
                "/* @glsl_closure v1\nvalue: max=2147483648\n*/\n"
                "int helper(int value){return value;}\n"
                "vec4 bad_int_max_overflow(float value){return vec4(float(helper(int(value))));}\n",
            ),
            (
                "bad_int_max_underflow",
                "/* @glsl_closure v1\nvalue: max=-2147483649\n*/\n"
                "int helper(int value){return value;}\n"
                "vec4 bad_int_max_underflow(float value){return vec4(float(helper(int(value))));}\n",
            ),
            (
                "bad_float_default_nan",
                "/* @glsl_closure v1\nvalue: default=nan\n*/\n"
                "float helper(float value){return value;}\n"
                "vec4 bad_float_default_nan(float value){return vec4(helper(value));}\n",
            ),
            (
                "bad_float_min_negative_inf",
                "/* @glsl_closure v1\nvalue: min=-inf\n*/\n"
                "float helper(float value){return value;}\n"
                "vec4 bad_float_min_negative_inf(float value){return vec4(helper(value));}\n",
            ),
            (
                "bad_float_max_inf",
                "/* @glsl_closure v1\nvalue: max=inf\n*/\n"
                "float helper(float value){return value;}\n"
                "vec4 bad_float_max_inf(float value){return vec4(helper(value));}\n",
            ),
            (
                "bad_float_default_overflow",
                "/* @glsl_closure v1\nvalue: default=1e999\n*/\n"
                "float helper(float value){return value;}\n"
                "vec4 bad_float_default_overflow(float value){return vec4(helper(value));}\n",
            ),
            (
                "bad_float_default_negative_overflow",
                "/* @glsl_closure v1\nvalue: default=-1e999\n*/\n"
                "float helper(float value){return value;}\n"
                "vec4 bad_float_default_negative_overflow(float value){return vec4(helper(value));}\n",
            ),
        ]

        for function_name, source in cases:
            with self.subTest(function_name=function_name):
                _, tree = self.make_material_tree()
                node = tree.nodes.new("ShaderNodeGLSLFunction")
                text_name = f"glsl_typed_closure_{function_name}.glsl"
                make_text_block(text_name, source)
                self.configure_glsl_node(node, text_name, function_name)
                self.assertEqual(node.parse_status, 'ERROR')

    def test_typed_closure_callback_rejects_function_like_macro_references(self):
        cases = [
            (
                "macro_direct_export",
                "#define CALL_HELPER(value) helper(value)\n"
                "/* @glsl_closure v1\n*/\n"
                "float helper(float value){return value;}\n"
                "vec4 macro_direct_export(float value){return vec4(CALL_HELPER(value));}\n",
            ),
            (
                "macro_recursive_export",
                "#define CALL_HELPER(value) FORWARD_HELPER(value)\n"
                "#define FORWARD_HELPER(value) \\\n"
                "  helper(value)\n"
                "/* @glsl_closure v1\n*/\n"
                "float helper(float value){\n"
                "  return value > 0.0 ? CALL_HELPER(value - 1.0) : value;\n"
                "}\n"
                "vec4 macro_recursive_export(float value){return vec4(helper(value));}\n",
            ),
            (
                "macro_spliced_identifier_export",
                "#define CALL_HELPER(value) hel\\\n"
                "per(value)\n"
                "/* @glsl_closure v1\n*/\n"
                "float helper(float value){return value;}\n"
                "vec4 macro_spliced_identifier_export(float value){\n"
                "  return vec4(CALL_HELPER(value));\n"
                "}\n",
            ),
            (
                "macro_object_alias_export",
                "#define HELPER_ALIAS helper\n"
                "#define CALL_HELPER(value) HELPER_ALIAS(value)\n"
                "/* @glsl_closure v1\n*/\n"
                "float helper(float value){return value;}\n"
                "vec4 macro_object_alias_export(float value){\n"
                "  return vec4(CALL_HELPER(value));\n"
                "}\n",
            ),
            (
                "macro_token_paste_export",
                "#define CALL_HELPER(value) hel##per(value)\n"
                "/* @glsl_closure v1\n*/\n"
                "float helper(float value){return value;}\n"
                "vec4 macro_token_paste_export(float value){\n"
                "  return vec4(CALL_HELPER(value));\n"
                "}\n",
            ),
        ]

        for function_name, invalid_source in cases:
            with self.subTest(function_name=function_name):
                _, tree = self.make_material_tree()
                node = tree.nodes.new("ShaderNodeGLSLFunction")
                text_name = f"glsl_typed_closure_{function_name}.glsl"
                text = make_text_block(
                    text_name,
                    f"vec4 {function_name}(float value){{return vec4(value);}}\n",
                )
                self.configure_glsl_node(node, text_name, function_name)
                self.assertEqual(node.parse_status, 'READY')

                text.clear()
                text.write(invalid_source)
                with self.assertRaisesRegex(RuntimeError, "GLSL macro"):
                    refresh_glsl_node_with_operator(node, tree)
                self.assertEqual(node.parse_status, 'ERROR')

    def test_typed_closure_callback_macro_scanner_ignores_comments_and_strings(self):
        _, tree = self.make_material_tree()
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = (
            "#define TEXT() \"helper\"\n"
            "#define JOINED_NAME() hel/**/per\n"
            "/* @glsl_closure v1\n*/\n"
            "float helper(float value){return value;}\n"
            "vec4 macro_comment_string_export(float value){return vec4(helper(value));}\n"
        )
        make_text_block("glsl_typed_closure_macro_comment_string.glsl", source)
        self.configure_glsl_node(
            node,
            "glsl_typed_closure_macro_comment_string.glsl",
            "macro_comment_string_export",
        )
        self.assertEqual(node.parse_status, 'READY')

    def test_typed_closure_callback_rejects_invalid_signatures(self):
        cases = [
            (
                "bad_inout",
                "/* @glsl_closure v1\n*/\nfloat helper(inout float value){return value;}\n"
                "vec4 bad_inout(float value){return vec4(helper(value));}\n",
            ),
            (
                "bad_matrix",
                "/* @glsl_closure v1\n*/\nmat3 helper(mat3 value){return value;}\n"
                "vec4 bad_matrix(float value){return vec4(helper(mat3(value))[0], 1.0);}\n",
            ),
            (
                "bad_sampler",
                "/* @glsl_closure v1\n*/\nvec4 helper(sampler2D image, vec2 uv){\n"
                "  return texture(image, uv);\n"
                "}\n"
                "vec4 bad_sampler(sampler2D image, vec2 uv){return helper(image, uv);}\n",
            ),
            (
                "bad_array",
                "/* @glsl_closure v1\n*/\nfloat helper(float values[2]){return values[0];}\n"
                "vec4 bad_array(float value){\n"
                "  float values[2]; values[0] = value; values[1] = 0.0;\n"
                "  return vec4(helper(values));\n"
                "}\n",
            ),
            (
                "bad_unsized_array",
                "/* @glsl_closure v1\n*/\nfloat helper(in float values[]){return values[0];}\n"
                "vec4 bad_unsized_array(float value){\n"
                "  float values[2]; values[0] = value; values[1] = 0.0;\n"
                "  return vec4(helper(values));\n"
                "}\n",
            ),
            (
                "bad_multidimensional_array",
                "#define ROWS 2\n"
                "#define COLS 3\n"
                "/* @glsl_closure v1\n*/\n"
                "float helper(float value, out float values[ROWS][COLS]){\n"
                "  values[0][0] = value; return value;\n"
                "}\n"
                "vec4 bad_multidimensional_array(float value){\n"
                "  float values[ROWS][COLS]; return vec4(helper(value, values));\n"
                "}\n",
            ),
            (
                "bad_struct",
                "struct Payload { float value; };\n"
                "/* @glsl_closure v1\n*/\nfloat helper(Payload value){return value.value;}\n"
                "vec4 bad_struct(float value){\n"
                "  Payload payload; payload.value = value; return vec4(helper(payload));\n"
                "}\n",
            ),
            (
                "bad_reserved_result",
                "/* @glsl_closure v1\n*/\nfloat helper(float Result){return Result;}\n"
                "vec4 bad_reserved_result(float value){return vec4(helper(value));}\n",
            ),
            (
                "bad_reserved_wrapper_key",
                "/* @glsl_closure v1\n*/\n"
                "float helper(float npr_closure_callback_out_value){\n"
                "  return npr_closure_callback_out_value;\n"
                "}\n"
                "vec4 bad_reserved_wrapper_key(float value){return vec4(helper(value));}\n",
            ),
            (
                "bad_no_outputs",
                "/* @glsl_closure v1\n*/\nvoid helper(float value){}\n"
                "vec4 bad_no_outputs(float value){helper(value); return vec4(value);}\n",
            ),
        ]

        for function_name, source in cases:
            with self.subTest(function_name=function_name):
                _, tree = self.make_material_tree()
                node = tree.nodes.new("ShaderNodeGLSLFunction")
                text_name = f"glsl_typed_closure_{function_name}.glsl"
                make_text_block(text_name, source)
                self.configure_glsl_node(node, text_name, function_name)
                self.assertEqual(node.parse_status, 'ERROR')

    def test_typed_closure_callback_rejects_exports_overloads_and_recursion(self):
        cases = [
            (
                "annotated_export",
                "/* @glsl_closure v1\n*/\n"
                "vec4 annotated_export(float value){return vec4(value);}\n",
            ),
            (
                "overloaded_export",
                "/* @glsl_closure v1\n*/\nfloat helper(float value){return value;}\n"
                "float helper(vec2 value){return value.x;}\n"
                "vec4 overloaded_export(float value){return vec4(helper(value));}\n",
            ),
            (
                "export_name_overloaded",
                "/* @glsl_closure v1\n*/\nfloat helper(float value){return value;}\n"
                "vec4 export_name_overloaded(float value){return vec4(helper(value));}\n"
                "vec4 export_name_overloaded(vec2 value){return vec4(helper(value.x));}\n",
            ),
            (
                "reachable_bridge_overloaded",
                "/* @glsl_closure v1\n*/\nfloat helper(float value){return value;}\n"
                "float bridge(float value){return helper(value);}\n"
                "float bridge(vec2 value){return helper(value.x);}\n"
                "vec4 reachable_bridge_overloaded(float value){return vec4(bridge(value));}\n",
            ),
            (
                "duplicate_same_signature",
                "/* @glsl_closure v1\n*/\nfloat helper(float value){return value;}\n"
                "float bridge(float value){return helper(value);}\n"
                "float bridge(float value){return helper(value + 1.0);}\n"
                "vec4 duplicate_same_signature(float value){return vec4(bridge(value));}\n",
            ),
            (
                "unreachable_name_overloaded",
                "/* @glsl_closure v1\n*/\nfloat helper(float value){return value;}\n"
                "float unused(float value){return value;}\n"
                "float unused(vec2 value){return value.x;}\n"
                "vec4 unreachable_name_overloaded(float value){return vec4(helper(value));}\n",
            ),
            (
                "direct_recursive_export",
                "/* @glsl_closure v1\n*/\n"
                "float helper(float value){return value > 0.0 ? helper(value - 1.0) : value;}\n"
                "vec4 direct_recursive_export(float value){return vec4(helper(value));}\n",
            ),
            (
                "indirect_recursive_export",
                "float helper_b(float value);\n"
                "/* @glsl_closure v1\n*/\n"
                "float helper_a(float value){return helper_b(value);}\n"
                "float helper_b(float value){return helper_a(value);}\n"
                "vec4 indirect_recursive_export(float value){return vec4(helper_a(value));}\n",
            ),
        ]

        for function_name, source in cases:
            with self.subTest(function_name=function_name):
                _, tree = self.make_material_tree()
                node = tree.nodes.new("ShaderNodeGLSLFunction")
                text_name = f"glsl_typed_closure_{function_name}.glsl"
                make_text_block(text_name, source)
                self.configure_glsl_node(node, text_name, function_name)
                self.assertEqual(node.parse_status, 'ERROR')

    def test_typed_closure_callback_rejects_socket_text_over_63_bytes(self):
        long_helper = "h" * 56
        long_key = "k" * 64
        split_key = "v" * 60
        long_text = "x" * 64
        split_label = "L" * 62
        cases = [
            (
                "long_helper_identifier",
                f"/* @glsl_closure v1\n*/\nfloat {long_helper}(float value){{return value;}}\n"
                f"vec4 long_helper_identifier(float value){{\n"
                f"  return vec4({long_helper}(value));\n"
                "}\n",
            ),
            (
                "long_item_key",
                f"/* @glsl_closure v1\n*/\nfloat helper(float {long_key}){{return {long_key};}}\n"
                f"vec4 long_item_key(float value){{return vec4(helper(value));}}\n",
            ),
            (
                "long_vec4_split_key",
                f"/* @glsl_closure v1\n*/\nvec4 helper(vec4 {split_key}){{return {split_key};}}\n"
                "vec4 long_vec4_split_key(vec4 value){return helper(value);}\n",
            ),
            (
                "long_callback_label",
                f"/* @glsl_closure v1 label=\"{long_text}\"\n*/\n"
                "float helper(float value){return value;}\n"
                "vec4 long_callback_label(float value){return vec4(helper(value));}\n",
            ),
            (
                "long_item_description",
                f"/* @glsl_closure v1\nvalue: description=\"{long_text}\"\n*/\n"
                "float helper(float value){return value;}\n"
                "vec4 long_item_description(float value){return vec4(helper(value));}\n",
            ),
            (
                "long_generated_vec4_label",
                f"/* @glsl_closure v1\nvalue: label=\"{split_label}\"\n*/\n"
                "vec4 helper(vec4 value){return value;}\n"
                "vec4 long_generated_vec4_label(vec4 value){return helper(value);}\n",
            ),
            (
                "long_generated_result_label",
                f"/* @glsl_closure v1\nResult: label=\"{split_label}\"\n*/\n"
                "vec4 helper(vec4 value){return value;}\n"
                "vec4 long_generated_result_label(vec4 value){return helper(value);}\n",
            ),
        ]

        for function_name, source in cases:
            with self.subTest(function_name=function_name):
                _, tree = self.make_material_tree()
                node = tree.nodes.new("ShaderNodeGLSLFunction")
                text_name = f"glsl_typed_closure_{function_name}.glsl"
                make_text_block(text_name, source)
                self.configure_glsl_node(node, text_name, function_name)
                self.assertEqual(node.parse_status, 'ERROR')

    def test_closure_sampler_sync_adds_opaque_alpha(self):
        _, tree = self.make_material_tree()
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        closure_input, closure_output = make_closure_sampler(tree, color_type="RGBA")
        color_socket = find_socket(closure_output.inputs, "Color")
        color_socket.default_value = (0.2, 0.4, 0.6, 0.8)
        source = (
            "vec4 sample_image(sampler2D image, vec2 uv){\n"
            "  return texture(image, uv);\n"
            "}\n"
        )
        make_text_block("glsl_closure_sampler_alpha_sync.glsl", source)
        self.configure_glsl_node(
            glsl_node, "glsl_closure_sampler_alpha_sync.glsl", "sample_image"
        )
        relink_and_update(
            tree,
            find_socket(closure_output.outputs, "Closure"),
            find_socket(glsl_node.inputs, "image"),
        )

        self.assertEqual(sync_closure_output_with_operator(closure_output, tree), {'FINISHED'})

        self.assertEqual(find_socket(closure_input.outputs, "UV").type, 'VECTOR')
        synced_color = find_socket(closure_output.inputs, "Color")
        self.assertEqual(synced_color.type, 'RGBA')
        for actual, expected in zip(synced_color.default_value, (0.2, 0.4, 0.6, 0.8)):
            self.assertAlmostEqual(actual, expected)
        alpha_socket = find_socket(closure_output.inputs, "Alpha")
        self.assertEqual(alpha_socket.type, 'VALUE')
        self.assertEqual(alpha_socket.default_value, 1.0)

        alpha_socket.default_value = 0.37
        alpha_source = tree.nodes.new("ShaderNodeValue")
        tree.links.new(alpha_source.outputs["Value"], alpha_socket)

        self.assertEqual(sync_closure_output_with_operator(closure_output, tree), {'FINISHED'})

        alpha_socket = find_socket(closure_output.inputs, "Alpha")
        self.assertAlmostEqual(alpha_socket.default_value, 0.37)
        self.assertTrue(alpha_socket.is_linked)
        self.assertEqual(alpha_socket.links[0].from_socket, alpha_source.outputs["Value"])

    def test_closure_output_sample3d_signature_validation(self):
        cases = [
            (True, "FLOAT", None, 'READY'),
            (True, "VECTOR", "FLOAT", 'READY'),
            (True, "RGBA", "FLOAT", 'READY'),
            (True, "FLOAT", "VECTOR", 'ERROR'),
            (False, "FLOAT", None, 'ERROR'),
            (True, None, None, 'ERROR'),
        ]
        source = (
            "vec4 sample_volume(sampler3D volume, vec3 coord){\n"
            "  return texture(volume, coord);\n"
            "}\n"
        )
        for index, (include_uv, color_type, alpha_type, expected_status) in enumerate(cases):
            with self.subTest(
                include_uv=include_uv, color_type=color_type, alpha_type=alpha_type
            ):
                _, tree = self.make_material_tree()
                glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
                _, closure_output = make_closure_sampler(
                    tree,
                    include_uv=include_uv,
                    color_type=color_type,
                    alpha_type=alpha_type,
                )
                text_name = f"glsl_sample3d_closure_signature_{index}.glsl"
                make_text_block(text_name, source)
                self.configure_glsl_node(glsl_node, text_name, "sample_volume")

                relink_and_update(
                    tree,
                    find_socket(closure_output.outputs, "Closure"),
                    find_socket(glsl_node.inputs, "volume"),
                )
                refresh_glsl_node(glsl_node)

                self.assertEqual(glsl_node.parse_status, expected_status)

    def test_image_to_closure_sample3d_requires_lut_strip(self):
        source = (
            "vec4 sample_volume(sampler3D volume, vec3 coord){\n"
            "  return texture(volume, coord);\n"
            "}\n"
        )
        for texture_type, expected_status in (("IMAGE_2D", 'ERROR'), ("LUT_STRIP_3D", 'READY')):
            with self.subTest(texture_type=texture_type):
                _, tree = self.make_material_tree()
                glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
                image_node = tree.nodes.new("ShaderNodeImageToClosure")
                image_node.texture_type = texture_type
                image_node.image = bpy.data.images.new(
                    f"glsl_sample3d_{texture_type}", 16, 4, alpha=True, float_buffer=True
                )
                text_name = f"glsl_sample3d_{texture_type}.glsl"
                make_text_block(text_name, source)
                self.configure_glsl_node(glsl_node, text_name, "sample_volume")

                relink_and_update(
                    tree,
                    find_socket(image_node.outputs, "Closure"),
                    find_socket(glsl_node.inputs, "volume"),
                )
                refresh_glsl_node(glsl_node)

                self.assertEqual(glsl_node.parse_status, expected_status)

    def test_mixed_closure_sampler_dimensions_are_isolated(self):
        _, tree = self.make_material_tree()
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        _, closure_2d = make_closure_sampler(tree)
        _, closure_3d = make_closure_sampler(tree)
        source = (
            "vec4 sample_mixed(sampler2D image, sampler3D volume, vec2 uv, vec3 coord){\n"
            "  return texture(image, uv) + texture(volume, coord);\n"
            "}\n"
        )
        make_text_block("glsl_mixed_closure_samplers.glsl", source)
        self.configure_glsl_node(glsl_node, "glsl_mixed_closure_samplers.glsl", "sample_mixed")

        relink_and_update(
            tree,
            find_socket(closure_2d.outputs, "Closure"),
            find_socket(glsl_node.inputs, "image"),
        )
        relink_and_update(
            tree,
            find_socket(closure_3d.outputs, "Closure"),
            find_socket(glsl_node.inputs, "volume"),
        )
        refresh_glsl_node(glsl_node)

        self.assertEqual(glsl_node.parse_status, 'READY')

    def test_nested_closure_sample3d_dimension_validation(self):
        cases = [
            (
                "vec4 inner(sampler3D src, vec3 coord){ return texture(src, coord); }\n",
                'READY',
            ),
            (
                "vec4 inner(sampler2D src, vec3 coord){ return texture(src, coord.xy); }\n",
                'ERROR',
            ),
        ]
        for index, (inner_source, expected_status) in enumerate(cases):
            with self.subTest(expected_status=expected_status):
                _, tree = self.make_material_tree()
                glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
                _, closure_output = make_closure_sampler(tree)
                source = (
                    "vec4 outer(sampler3D src, vec3 coord){ return inner(src, coord); }\n"
                    + inner_source
                )
                text_name = f"glsl_nested_sample3d_{index}.glsl"
                make_text_block(text_name, source)
                self.configure_glsl_node(glsl_node, text_name, "outer")

                relink_and_update(
                    tree,
                    find_socket(closure_output.outputs, "Closure"),
                    find_socket(glsl_node.inputs, "src"),
                )
                refresh_glsl_node(glsl_node)

                self.assertEqual(glsl_node.parse_status, expected_status)

    def test_closure_sample3d_rejects_non_texture_sampling(self):
        _, tree = self.make_material_tree()
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        _, closure_output = make_closure_sampler(tree)
        source = (
            "vec4 sample_volume_lod(sampler3D volume, vec3 coord){\n"
            "  return textureLod(volume, coord, 0.0);\n"
            "}\n"
        )
        make_text_block("glsl_sample3d_closure_lod.glsl", source)
        self.configure_glsl_node(glsl_node, "glsl_sample3d_closure_lod.glsl", "sample_volume_lod")

        relink_and_update(
            tree,
            find_socket(closure_output.outputs, "Closure"),
            find_socket(glsl_node.inputs, "volume"),
        )
        refresh_glsl_node(glsl_node)

        self.assertEqual(glsl_node.parse_status, 'ERROR')

    def test_code_mode_creates_template_without_resizing_node(self):
        _, tree = self.make_material_tree()
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        node.width = 180.0

        result = toggle_glsl_node_code_mode_with_operator(node, tree)

        self.assertEqual(result, {'FINISHED'})
        self.assertEqual(node.display_mode, 'CODE')
        self.assertAlmostEqual(node.width, 180.0)
        self.assertIsNotNone(node.script)
        self.assertEqual(node.function_name, "glsl_function")
        self.assertIn("vec4 glsl_function(vec4 color)", node.script.as_string())
        self.assertEqual(node.parse_status, 'READY')
        self.assertEqual(find_socket(node.inputs, "In_color").type, 'VECTOR')
        self.assertEqual(find_socket(node.inputs, "In_color_w").type, 'VALUE')

    def test_code_mode_preserves_socket_links(self):
        _, tree = self.make_material_tree()
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        value_node = tree.nodes.new("ShaderNodeValue")
        source = "float linked_probe(float value){return value;}\n"
        make_text_block("glsl_code_mode_link.glsl", source)
        self.configure_glsl_node(node, "glsl_code_mode_link.glsl", "linked_probe")
        tree.links.new(value_node.outputs["Value"], find_socket(node.inputs, "In_value"))

        node.display_mode = 'CODE'

        self.assertTrue(find_socket(node.inputs, "In_value").is_linked)
        self.assertEqual(len(tree.links), 1)

        node.display_mode = 'NODE'

        self.assertTrue(find_socket(node.inputs, "In_value").is_linked)
        self.assertEqual(len(tree.links), 1)

    def test_direct_code_mode_creates_template(self):
        _, tree = self.make_material_tree()
        node = tree.nodes.new("ShaderNodeGLSLFunction")

        node.display_mode = 'CODE'

        self.assertIsNotNone(node.script)
        self.assertEqual(node.function_name, "glsl_function")
        self.assertIn("vec4 glsl_function(vec4 color)", node.script.as_string())

    def test_new_text_operator_creates_template(self):
        _, tree = self.make_material_tree()
        node = tree.nodes.new("ShaderNodeGLSLFunction")

        result = new_glsl_node_text_with_operator(node, tree)

        self.assertEqual(result, {'FINISHED'})
        self.assertIsNotNone(node.script)
        self.assertEqual(node.function_name, "glsl_function")
        self.assertIn("return color;", node.script.as_string())

    def test_code_mode_populates_assigned_empty_text(self):
        _, tree = self.make_material_tree()
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        text = make_text_block("glsl_empty_code_source.glsl", "")
        node.script = text

        result = toggle_glsl_node_code_mode_with_operator(node, tree)

        self.assertEqual(result, {'FINISHED'})
        self.assertEqual(node.display_mode, 'CODE')
        self.assertEqual(node.function_name, "glsl_function")
        self.assertIn("vec4 glsl_function(vec4 color)", text.as_string())

    def test_code_mode_edits_the_assigned_text_directly(self):
        _, tree = self.make_material_tree()
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        original_source = "vec4 draft_probe(vec4 color){return color;}\n"
        text = make_text_block("glsl_code_draft.glsl", original_source)
        self.configure_glsl_node(node, text.name, "draft_probe")

        draft_source = (
            "vec4 draft_probe(vec4 color, float strength){\n"
            "  return color * strength;\n"
            "}\n"
        )
        node.source_code = draft_source

        self.assertEqual(node.source_code, draft_source)
        self.assertEqual(text.as_string(), draft_source)
        with self.assertRaises(AssertionError):
            find_socket(node.inputs, "strength")

        result = refresh_glsl_node_with_operator(node, tree)

        self.assertEqual(result, {'FINISHED'})
        self.assertEqual(text.as_string(), draft_source)
        self.assertIsNotNone(find_socket(node.inputs, "strength"))

    def test_invalid_code_preserves_committed_sockets(self):
        _, tree = self.make_material_tree()
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        original_source = "float stable_probe(float value){return value;}\n"
        text = make_text_block("glsl_invalid_code_draft.glsl", original_source)
        self.configure_glsl_node(node, text.name, "stable_probe")
        original_socket_names = [socket.name for socket in node.inputs]

        invalid_source = "float stable_probe(float value){\n"
        node.source_code = invalid_source

        with self.assertRaises(RuntimeError):
            refresh_glsl_node_with_operator(node, tree)

        self.assertEqual(node.source_code, invalid_source)
        self.assertEqual(text.as_string(), invalid_source)
        self.assertEqual([socket.name for socket in node.inputs], original_socket_names)

        node.source_code = original_source
        self.assertEqual(refresh_glsl_node_with_operator(node, tree), {'FINISHED'})

    def test_gpu_invalid_code_preserves_committed_sockets(self):
        if bpy.app.background:
            self.skipTest("GPU draft validation requires an active graphics context")

        _, tree = self.make_material_tree()
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        original_source = "float stable_gpu_probe(float value){return value;}\n"
        text = make_text_block("glsl_gpu_invalid_code_draft.glsl", original_source)
        self.configure_glsl_node(node, text.name, "stable_gpu_probe")
        original_socket_names = [socket.name for socket in node.inputs]

        invalid_source = (
            "float stable_gpu_probe(float value){\n"
            "  return undefined_draft_identifier;\n"
            "}\n"
        )
        node.source_code = invalid_source

        with self.assertRaisesRegex(RuntimeError, "GPU shader compiler rejected"):
            refresh_glsl_node_with_operator(node, tree)

        self.assertEqual(node.source_code, invalid_source)
        self.assertEqual(text.as_string(), invalid_source)
        self.assertEqual([socket.name for socket in node.inputs], original_socket_names)

    def test_code_function_rename_applies_on_refresh(self):
        _, tree = self.make_material_tree()
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        text = make_text_block(
            "glsl_function_rename_draft.glsl",
            "float old_function(float value){return value;}\n",
        )
        self.configure_glsl_node(node, text.name, "old_function")

        node.source_code = "float new_function(float value){return value * 2.0;}\n"
        node.function_name = "new_function"

        self.assertEqual(node.function_name, "new_function")

        result = refresh_glsl_node_with_operator(node, tree)

        self.assertEqual(result, {'FINISHED'})
        self.assertEqual(node.function_name, "new_function")
        self.assertIn("new_function", text.as_string())
        self.assertEqual(node.parse_status, 'READY')

    def test_function_selection_does_not_rewrite_text(self):
        _, tree = self.make_material_tree()
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = (
            "float first_function(float value){return value;}\n"
            "float second_function(float value){return value * 2.0;}\n"
        )
        text = make_text_block("glsl_function_only_draft.glsl", source)
        self.configure_glsl_node(node, text.name, "first_function")

        node.function_name = "second_function"
        result = refresh_glsl_node_with_operator(node, tree)

        self.assertEqual(result, {'FINISHED'})
        self.assertEqual(node.function_name, "second_function")
        self.assertEqual(text.as_string(), source)

    def test_changing_internal_text_changes_code_editor_source(self):
        _, tree = self.make_material_tree()
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        first = make_text_block(
            "glsl_first_draft_source.glsl",
            "float first_source(float value){return value;}\n",
        )
        second = make_text_block(
            "glsl_second_draft_source.glsl",
            "float second_source(float value){return value;}\n",
        )
        self.configure_glsl_node(node, first.name, "first_source")

        node.script = second

        self.assertEqual(node.source_code, second.as_string())

    def test_shared_text_refreshes_each_independent_function(self):
        _, tree = self.make_material_tree()
        first = tree.nodes.new("ShaderNodeGLSLFunction")
        second = tree.nodes.new("ShaderNodeGLSLFunction")
        value_node = tree.nodes.new("ShaderNodeValue")
        original_source = (
            "float shared_first(float value){return value;}\n"
            "float shared_second(float value){return value;}\n"
        )
        text = make_text_block("glsl_shared_code_draft.glsl", original_source)
        self.configure_glsl_node(first, text.name, "shared_first")
        self.configure_glsl_node(second, text.name, "shared_second")
        tree.links.new(value_node.outputs["Value"], find_socket(second.inputs, "In_value"))

        valid_source = (
            "float shared_first(float value, float gain){return value * gain;}\n"
            "float shared_second(float value, float bias){return value + bias;}\n"
        )
        first.source_code = valid_source
        self.assertEqual(second.source_code, valid_source)
        result = refresh_glsl_node_with_operator(first, tree)

        self.assertEqual(result, {'FINISHED'})
        self.assertEqual(text.as_string(), valid_source)
        self.assertIsNotNone(find_socket(first.inputs, "gain"))
        self.assertIsNotNone(find_socket(second.inputs, "bias"))
        self.assertEqual(first.function_name, "shared_first")
        self.assertEqual(second.function_name, "shared_second")

        invalid_for_second = (
            "float shared_first(float value, float gain, float offset){\n"
            "  return value * gain + offset;\n"
            "}\n"
        )
        text.from_string(invalid_for_second)
        result = refresh_glsl_node_with_operator(first, tree)

        self.assertEqual(result, {'FINISHED'})
        self.assertEqual(text.as_string(), invalid_for_second)
        self.assertIsNotNone(find_socket(first.inputs, "offset"))
        self.assertEqual(first.function_name, "shared_first")
        self.assertEqual(second.function_name, "shared_second")
        self.assertEqual(second.parse_status, 'ERROR')
        self.assertIsNotNone(find_socket(second.inputs, "bias"))
        self.assertTrue(find_socket(second.inputs, "In_value").is_linked)
        self.assertEqual(len(tree.links), 1)

        with self.assertRaisesRegex(
            RuntimeError,
            'Cannot refresh GLSL Function "shared_second".*selected function was not found',
        ):
            refresh_glsl_node_with_operator(second, tree)

    def test_shared_text_nested_users_keep_independent_function_names(self):
        material, tree = self.make_material_tree()
        material.name = "Shared Owner Material"
        first = tree.nodes.new("ShaderNodeGLSLFunction")
        first.name = "Current GLSL Node"
        group = bpy.data.node_groups.new("Nested Shared GLSL Tree", "ShaderNodeTree")
        second = group.nodes.new("ShaderNodeGLSLFunction")
        second.name = "Nested GLSL Node"
        source = (
            "float material_function(float value){return value;}\n"
            "float group_function(float value){return value;}\n"
        )
        text = make_text_block("shared_nested_functions.glsl", source)
        self.configure_glsl_node(first, text.name, "material_function")
        self.configure_glsl_node(second, text.name, "group_function")

        updated_source = (
            "float material_function(float value, float gain){return value * gain;}\n"
            "float group_function(float value, float bias){return value + bias;}\n"
        )
        first.source_code = updated_source
        result = refresh_glsl_node_with_operator(first, tree)

        self.assertEqual(result, {'FINISHED'})
        self.assertEqual(first.function_name, "material_function")
        self.assertEqual(second.function_name, "group_function")
        self.assertIsNotNone(find_socket(first.inputs, "gain"))
        self.assertIsNotNone(find_socket(second.inputs, "bias"))

    def test_external_source_is_read_only_and_can_convert_to_internal(self):
        _, tree = self.make_material_tree()
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = "float external_probe(float value){return value;}\n"

        with tempfile.TemporaryDirectory() as directory:
            filepath = Path(directory) / "external_probe.glsl"
            filepath.write_text(source, encoding="utf-8")
            node.source_mode = 'EXTERNAL'
            node.filepath = str(filepath)
            node.function_name = "external_probe"
            refresh_glsl_node(node)

            self.assertEqual(node.source_code, source)
            node.source_code = "float overwritten(){return 0.0;}\n"
            self.assertEqual(filepath.read_text(encoding="utf-8"), source)

            result = make_glsl_node_internal_with_operator(node, tree)

            self.assertEqual(result, {'FINISHED'})
            self.assertEqual(node.source_mode, 'INTERNAL')
            self.assertIsNotNone(node.script)
            self.assertEqual(node.script.as_string(), source)
            self.assertEqual(filepath.read_text(encoding="utf-8"), source)
            self.assertEqual(node.parse_status, 'READY')

    def test_code_text_edits_survive_blend_save_and_reload(self):
        material, tree = self.make_material_tree()
        material.use_fake_user = True
        material_name = material.name
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        original_source = "float saved_probe(float value){return value;}\n"
        draft_source = "float saved_probe(float value){return value + 1.0;}\n"
        text = make_text_block("glsl_saved_code_draft.glsl", original_source)
        self.configure_glsl_node(node, text.name, "saved_probe")
        node.source_code = draft_source
        node.display_mode = 'CODE'

        with tempfile.TemporaryDirectory() as directory:
            filepath = Path(directory) / "glsl_code_draft.blend"
            bpy.ops.wm.save_as_mainfile(filepath=str(filepath), check_existing=False)
            bpy.ops.wm.open_mainfile(filepath=str(filepath), load_ui=False)

            loaded_material = bpy.data.materials[material_name]
            loaded_node = next(
                item
                for item in loaded_material.node_tree.nodes
                if item.bl_idname == "ShaderNodeGLSLFunction"
            )
            self.assertEqual(loaded_node.display_mode, 'CODE')
            self.assertEqual(loaded_node.source_code, draft_source)
            self.assertEqual(loaded_node.script.as_string(), draft_source)

    def test_code_text_is_shared_by_node_copy(self):
        _, tree = self.make_material_tree()
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        original_source = "float copy_probe(float value){return value;}\n"
        draft_source = "float copy_probe(float value){return value + 1.0;}\n"
        text = make_text_block("glsl_copied_code_draft.glsl", original_source)
        self.configure_glsl_node(node, text.name, "copy_probe")
        node.source_code = draft_source
        node.display_mode = 'CODE'

        result, duplicate = duplicate_glsl_node_with_operator(node, tree)

        self.assertEqual(result, {'FINISHED'})
        self.assertEqual(duplicate.script, text)
        self.assertEqual(duplicate.display_mode, 'CODE')
        self.assertEqual(duplicate.source_code, draft_source)
        self.assertEqual(text.as_string(), draft_source)

        tree.nodes.remove(node)
        result = refresh_glsl_node_with_operator(duplicate, tree)

        self.assertEqual(result, {'FINISHED'})
        self.assertEqual(text.as_string(), draft_source)

    def test_code_text_edits_survive_undo_and_redo(self):
        material, tree = self.make_material_tree()
        material.use_fake_user = True
        material_name = material.name
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        node_name = node.name
        original_source = "float undo_probe(float value){return value;}\n"
        first_draft = "float undo_probe(float value){return value + 1.0;}\n"
        second_draft = "float undo_probe(float value){return value + 2.0;}\n"
        text = make_text_block("glsl_undo_code_draft.glsl", original_source)
        self.configure_glsl_node(node, text.name, "undo_probe")

        bpy.ops.ed.undo_push(message="GLSL draft initial state")
        node.source_code = first_draft
        bpy.ops.ed.undo_push(message="GLSL draft first edit")
        node.source_code = second_draft
        bpy.ops.ed.undo_push(message="GLSL draft second edit")

        self.assertEqual(bpy.ops.ed.undo(), {'FINISHED'})
        undo_node = bpy.data.materials[material_name].node_tree.nodes[node_name]
        self.assertEqual(undo_node.source_code, first_draft)
        self.assertEqual(undo_node.script.as_string(), first_draft)

        self.assertEqual(bpy.ops.ed.redo(), {'FINISHED'})
        redo_node = bpy.data.materials[material_name].node_tree.nodes[node_name]
        self.assertEqual(redo_node.source_code, second_draft)
        self.assertEqual(redo_node.script.as_string(), second_draft)

    def test_external_packed_source_remains_available(self):
        _, tree = self.make_material_tree()
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = "float packed_probe(float value){return value;}\n"

        with tempfile.TemporaryDirectory() as directory:
            filepath = Path(directory) / "packed_probe.glsl"
            filepath.write_text(source, encoding="utf-8")
            node.source_mode = 'EXTERNAL'
            node.filepath = str(filepath)
            node.function_name = "packed_probe"
            refresh_glsl_node(node)

            bpy.ops.file.pack_all()
            filepath.unlink()

            self.assertEqual(node.source_code, source)
            self.assertEqual(node.parse_status, 'READY')

    def test_meta_description_preserves_existing_socket_values(self):
        _, tree = self.make_material_tree()
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        source = (
            "/* @glsl_meta v1\n"
            "strength: default=0.25 min=0.0 max=1.0 subtype=factor "
            "description=\"Blend amount for the effect\"\n"
            "tint: default=vec3(1.0, 0.8, 0.2) subtype=color "
            "description=\"Main tint color\"\n"
            "*/\n"
            "vec3 stylize(vec3 base_color, float strength, vec3 tint){\n"
            "  return mix(base_color, tint, strength);\n"
            "}\n"
        )
        text = make_text_block("glsl_meta_description.glsl", source)

        self.configure_glsl_node(node, "glsl_meta_description.glsl", "stylize")
        self.assertEqual(node.parse_status, 'READY')
        strength_socket = find_socket(node.inputs, "strength")
        self.assertAlmostEqual(strength_socket.default_value, 0.25)
        strength_socket.default_value = 0.75

        text.clear()
        text.write(
            "/* @glsl_meta v1\n"
            "strength: default=0.25 min=0.0 max=1.0 subtype=factor "
            "description=\"Updated blend amount\"\n"
            "tint: default=vec3(1.0, 0.8, 0.2) subtype=color "
            "description=\"Updated tint color\"\n"
            "*/\n"
            "vec3 stylize(vec3 base_color, float strength, vec3 tint){\n"
            "  return mix(base_color, tint, strength);\n"
            "}\n"
        )
        refresh_glsl_node(node)

        self.assertEqual(node.parse_status, 'READY')
        self.assertAlmostEqual(find_socket(node.inputs, "strength").default_value, 0.75)

    def test_reset_defaults_restores_meta_and_define_defaults(self):
        _, tree = self.make_material_tree()
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        value_node = tree.nodes.new("ShaderNodeValue")
        source = (
            "/* @glsl_defines v1\n"
            "@define USE_RIM bool default=true\n"
            "@define METHOD int default=1 items=\"0:Off;1:Soft;2:Hard\"\n"
            "*/\n"
            "/* @glsl_meta v1\n"
            "strength: default=0.25\n"
            "steps: default=3\n"
            "enabled: default=true\n"
            "uv: default=vec2(0.2, 0.4)\n"
            "tint: default=vec3(1.0, 0.5, 0.25) subtype=color\n"
            "color: default=vec4(0.1, 0.2, 0.3, 0.4)\n"
            "*/\n"
            "vec4 reset_probe(float strength, int steps, bool enabled, vec2 uv, vec3 tint, "
            "vec4 color, float no_default){\n"
            "  return color + vec4(tint * strength + vec3(float(steps)) + vec3(uv, 0.0), "
            "                  enabled ? no_default : 0.0);\n"
            "}\n"
        )
        make_text_block("glsl_reset_defaults.glsl", source)

        self.configure_glsl_node(node, "glsl_reset_defaults.glsl", "reset_probe")
        self.assertEqual(node.parse_status, 'READY')

        strength = find_socket(node.inputs, "In_strength")
        steps = find_socket(node.inputs, "In_steps")
        enabled = find_socket(node.inputs, "In_enabled")
        uv = find_socket(node.inputs, "In_uv")
        tint = find_socket(node.inputs, "In_tint")
        color = find_socket(node.inputs, "In_color")
        color_w = find_socket(node.inputs, "In_color_w")
        no_default = find_socket(node.inputs, "In_no_default")

        relink_and_update(tree, find_socket(value_node.outputs, "Value"), strength)
        self.assertTrue(strength.is_linked)

        strength.default_value = 0.9
        steps.default_value = 2
        enabled.default_value = False
        uv.default_value = (0.8, 0.7)
        tint.default_value = (0.0, 0.1, 0.2, 0.75)
        color.default_value = (0.9, 0.8, 0.7)
        color_w.default_value = 0.6
        no_default.default_value = 0.33
        find_define_value(node, "USE_RIM").bool_value = False
        find_define_value(node, "METHOD").choice_value = 'VALUE_2'

        result = reset_glsl_node_defaults_with_operator(node, tree)

        self.assertEqual(result, {'FINISHED'})
        self.assertTrue(strength.is_linked)
        self.assertAlmostEqual(strength.default_value, 0.25)
        self.assertEqual(steps.default_value, 3)
        self.assertTrue(enabled.default_value)
        self.assertAlmostEqual(uv.default_value[0], 0.2)
        self.assertAlmostEqual(uv.default_value[1], 0.4)
        self.assertAlmostEqual(tint.default_value[0], 1.0)
        self.assertAlmostEqual(tint.default_value[1], 0.5)
        self.assertAlmostEqual(tint.default_value[2], 0.25)
        self.assertAlmostEqual(tint.default_value[3], 1.0)
        self.assertAlmostEqual(color.default_value[0], 0.1)
        self.assertAlmostEqual(color.default_value[1], 0.2)
        self.assertAlmostEqual(color.default_value[2], 0.3)
        self.assertAlmostEqual(color_w.default_value, 0.4)
        self.assertAlmostEqual(no_default.default_value, 0.33)
        self.assertTrue(find_define_value(node, "USE_RIM").bool_value)
        self.assertEqual(find_define_value(node, "METHOD").int_value, 1)

    def test_reset_defaults_cancelled_on_parse_error_preserves_values(self):
        _, tree = self.make_material_tree()
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        text = make_text_block(
            "glsl_reset_parse_error.glsl",
            "/* @glsl_meta v1\n"
            "strength: default=0.25\n"
            "*/\n"
            "float reset_parse_error(float strength){return strength;}\n",
        )

        self.configure_glsl_node(node, "glsl_reset_parse_error.glsl", "reset_parse_error")
        self.assertEqual(node.parse_status, 'READY')
        strength = find_socket(node.inputs, "In_strength")
        strength.default_value = 0.75

        text.clear()
        text.write("float renamed_function(float strength){return strength;}\n")

        with self.assertRaises(RuntimeError):
            reset_glsl_node_defaults_with_operator(node, tree)
        self.assertAlmostEqual(strength.default_value, 0.75)

    def test_int_param_choice_items_preserve_and_fallback(self):
        _, tree = self.make_material_tree()
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        text = make_text_block(
            "glsl_param_int_choices.glsl",
            "/* @glsl_meta v1\n"
            "method: label=\"Method\" default=1 "
            "items=\"0:Burley;1:Random Walk;2:Skin\" show_label=true\n"
            "*/\n"
            "vec4 param_int_choices(vec4 color, int method){\n"
            "  return color * float(method + 1);\n"
            "}\n",
        )

        self.configure_glsl_node(node, "glsl_param_int_choices.glsl", "param_int_choices")
        self.assertEqual(node.parse_status, 'READY')
        method_socket = find_socket(node.inputs, "In_method")
        self.assertEqual(method_socket.default_value, 1)
        self.assertEqual(method_socket.glsl_int_choice_value, 'VALUE_1')

        method_socket.glsl_int_choice_value = 'VALUE_2'
        refresh_glsl_node(node)

        self.assertEqual(node.parse_status, 'READY')
        self.assertEqual(find_socket(node.inputs, "In_method").default_value, 2)
        self.assertEqual(find_socket(node.inputs, "In_method").glsl_int_choice_value, 'VALUE_2')

        text.clear()
        text.write(
            "/* @glsl_meta v1\n"
            "method: label=\"Method\" default=0 items=\"0:Burley;1:Random Walk\"\n"
            "*/\n"
            "vec4 param_int_choices(vec4 color, int method){\n"
            "  return color * float(method + 1);\n"
            "}\n"
        )
        refresh_glsl_node(node)

        self.assertEqual(node.parse_status, 'READY')
        self.assertEqual(find_socket(node.inputs, "In_method").default_value, 0)

        text.clear()
        text.write(
            "/* @glsl_meta v1\n"
            "method: label=\"Method\" default=1\n"
            "*/\n"
            "vec4 param_int_choices(vec4 color, int method){\n"
            "  return color * float(method + 1);\n"
            "}\n"
        )
        refresh_glsl_node(node)

        self.assertEqual(node.parse_status, 'READY')
        plain_method_socket = find_socket(node.inputs, "In_method")
        self.assertEqual(plain_method_socket.default_value, 0)
        self.assertEqual(plain_method_socket.glsl_int_choice_value, '')

    def test_code_mode_sampler3d_updates_isolate_int_choice_cache(self):
        _, tree = self.make_material_tree()
        tree.nodes.new("ShaderNodeImageToClosure")
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        node.display_mode = 'CODE'

        int_source = (
            "/* @glsl_meta v1\n"
            "sdf_volume: default=1 items=\"0:Off;1:On\"\n"
            "*/\n"
            "float probe_int_choice(int sdf_volume){return float(sdf_volume);}\n"
        )
        sampler3d_source = (
            "/* @glsl_meta v1\n"
            "sdf_volume: label=\"Procedural 3D Field\" "
            "description=\"Connect a Closure Output with UV Vector, Color, and optional Alpha Float outputs\"\n"
            "coordinate: label=\"Coordinate\" default=vec3(0.5)\n"
            "*/\n"
            "vec4 sample_sampler3d(sampler3D sdf_volume, vec3 coordinate)\n"
            "{\n"
            "  return texture(sdf_volume, coordinate);\n"
            "}\n"
        )

        for _ in range(4):
            node.source_code = int_source
            node.function_name = "probe_int_choice"
            tree.interface_update(bpy.context)
            bpy.context.view_layer.update()
            refresh_glsl_node(node)
            int_socket = find_socket(node.inputs, "In_sdf_volume")
            self.assertEqual(int_socket.type, 'INT')
            self.assertEqual(int_socket.glsl_int_choice_value, 'VALUE_1')

            node.source_code = sampler3d_source
            node.source_code = sampler3d_source
            node.function_name = "sample_sampler3d"
            tree.interface_update(bpy.context)
            bpy.context.view_layer.update()
            refresh_glsl_node(node)
            closure_socket = find_socket(node.inputs, "In_sdf_volume")
            self.assertEqual(closure_socket.type, 'CLOSURE')
            coordinate_socket = find_socket(node.inputs, "In_coordinate")
            self.assertEqual(coordinate_socket.type, 'VECTOR')
            self.assertEqual(node.parse_status, 'READY')

    def test_sampler2d_meta_allows_label_description_and_panel_only(self):
        _, tree = self.make_material_tree()
        node = tree.nodes.new("ShaderNodeGLSLFunction")
        texture_label = "\u8d34\u56fe"
        source = (
            "/* @glsl_meta v1\n"
            "@panel Texture closed=true\n"
            f"tex: label=\"{texture_label}\" description=\"Texture closure used by texture(tex, uv)\"\n"
            "uv: default=vec2(0.0) description=\"Texture coordinates\"\n"
            "@end_panel\n"
            "*/\n"
            "vec4 sample_it(sampler2D tex, vec2 uv){\n"
            "  return texture(tex, uv);\n"
            "}\n"
        )
        make_text_block("glsl_sampler_description.glsl", source)

        self.configure_glsl_node(node, "glsl_sampler_description.glsl", "sample_it")

        self.assertEqual(node.parse_status, 'READY')
        tex_socket = find_socket(node.inputs, "In_tex")
        self.assertEqual(tex_socket.name, texture_label)

        bad_node = tree.nodes.new("ShaderNodeGLSLFunction")
        bad_source = (
            "/* @glsl_meta v1\n"
            "tex: default=0.5\n"
            "*/\n"
            "vec4 bad_sampler(sampler2D tex, vec2 uv){\n"
            "  return texture(tex, uv);\n"
            "}\n"
        )
        make_text_block("glsl_sampler_bad_meta.glsl", bad_source)

        self.configure_glsl_node(bad_node, "glsl_sampler_bad_meta.glsl", "bad_sampler")

        self.assertEqual(bad_node.parse_status, 'ERROR')

    def test_nested_sample2d_closure_links_parse(self):
        _, tree = self.make_material_tree()
        outer_node = tree.nodes.new("ShaderNodeGLSLFunction")
        inner_node = tree.nodes.new("ShaderNodeGLSLFunction")
        image_node = tree.nodes.new("ShaderNodeImageToClosure")

        image = bpy.data.images.new("glsl_nested_test_image", 2, 2, alpha=True, float_buffer=True)
        image.generated_color = (0.8, 0.2, 0.1, 1.0)
        image_node.image = image

        inner_source = "vec4 inner_sample(sampler2D src, vec2 uv){\n" "  return texture(src, uv);\n" "}\n"
        outer_source = "vec4 outer_sample(sampler2D src, vec2 uv){\n" "  return inner_sample(src, uv);\n" "}\n" "vec4 inner_sample(sampler2D src, vec2 uv){\n" "  return texture(src, uv);\n" "}\n"
        make_text_block("glsl_nested_inner.glsl", inner_source)
        make_text_block("glsl_nested_outer.glsl", outer_source)

        self.configure_glsl_node(inner_node, "glsl_nested_inner.glsl", "inner_sample")
        self.configure_glsl_node(outer_node, "glsl_nested_outer.glsl", "outer_sample")

        relink_and_update(
            tree,
            find_socket(image_node.outputs, "Closure"),
            find_socket(inner_node.inputs, "src"),
        )
        relink_and_update(
            tree,
            find_socket(inner_node.outputs, "Result"),
            find_socket(outer_node.inputs, "src"),
        )
        refresh_glsl_node(inner_node)
        refresh_glsl_node(outer_node)

        self.assertEqual(inner_node.parse_status, 'READY')
        self.assertEqual(outer_node.parse_status, 'READY')

    def test_filter_domain_menu_poll_allows_glsl_nodes(self):
        filter_context = make_menu_context("FILTER")
        world_context = make_menu_context("WORLD")

        self.assertTrue(
            node_add_menu_shader.object_filter_or_npr_eevee_shader_nodes_poll(filter_context)
        )
        self.assertFalse(
            node_add_menu_shader.object_filter_or_npr_eevee_shader_nodes_poll(world_context)
        )

    def test_filter_material_basic_function_builds_alpha_output(self):
        material, tree = self.make_material_tree(domain="FILTER")
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        output_node = tree.nodes.new("ShaderNodeOutputFilter")

        source = "float alpha_passthrough(float x){\n  return x;\n}\n"
        make_text_block("glsl_filter_alpha.glsl", source)
        self.configure_glsl_node(glsl_node, "glsl_filter_alpha.glsl", "alpha_passthrough")

        self.assertEqual(material.eevee_domain, "FILTER")
        self.assertEqual(glsl_node.parse_status, 'READY')
        self.assertIsNotNone(find_socket(glsl_node.inputs, "x"))
        self.assertIsNotNone(find_socket(glsl_node.outputs, "Result"))

        relink_and_update(
            tree,
            find_socket(glsl_node.outputs, "Result"),
            find_socket(output_node.inputs, "Alpha"),
        )

        self.assertEqual(glsl_node.parse_status, 'READY')

    def test_filter_material_image_sample2d_builds_color_output(self):
        _, tree = self.make_material_tree(domain="FILTER")
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        image_node = tree.nodes.new("ShaderNodeImageToClosure")
        output_node = tree.nodes.new("ShaderNodeOutputFilter")

        image = bpy.data.images.new("glsl_filter_test_image", 2, 2, alpha=True, float_buffer=True)
        image.generated_color = (0.9, 0.4, 0.1, 1.0)
        image_node.image = image

        source = "vec4 sample_color(sampler2D src, vec2 uv){\n  return texture(src, uv);\n}\n"
        make_text_block("glsl_filter_sample2d_image.glsl", source)
        self.configure_glsl_node(glsl_node, "glsl_filter_sample2d_image.glsl", "sample_color")

        relink_and_update(
            tree,
            find_socket(image_node.outputs, "Closure"),
            find_socket(glsl_node.inputs, "src"),
        )
        relink_and_update(
            tree,
            find_socket(glsl_node.outputs, "Result"),
            find_socket(output_node.inputs, "Color"),
        )
        refresh_glsl_node(glsl_node)

        self.assertEqual(glsl_node.parse_status, 'READY')


if __name__ == "__main__":
    argv = []
    if "--" in sys.argv:
        argv = sys.argv[sys.argv.index("--") + 1 :]
    unittest.main(argv=[sys.argv[0], *argv])
