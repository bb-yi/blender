# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import sys
import importlib.util
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


def reset_glsl_node_defaults_with_operator(node, tree):
    return run_glsl_node_operator(node, tree, bpy.ops.node.glsl_function_reset_defaults)


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
