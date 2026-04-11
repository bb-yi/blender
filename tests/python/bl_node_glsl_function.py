# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import sys
import bpy
import unittest


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


def refresh_glsl_node(node):
    current_name = node.function_name
    node.function_name = ""
    node.function_name = current_name
    node.id_data.interface_update(bpy.context)
    node.id_data.update_tag()
    bpy.context.view_layer.update()


def relink_and_update(tree, from_socket, to_socket):
    tree.links.new(from_socket, to_socket)
    tree.interface_update(bpy.context)
    tree.update_tag()
    bpy.context.view_layer.update()


class GLSLFunctionNodeTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_homefile(use_factory_startup=True)

    def make_material_tree(self):
        material = bpy.data.materials.new(name="GLSLFunctionTest")
        material.use_nodes = True
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

    def test_image_sample2d_builds_color_output(self):
        _, tree = self.make_material_tree()
        glsl_node = tree.nodes.new("ShaderNodeGLSLFunction")
        image_node = tree.nodes.new("ShaderNodeImageToClosure")

        image = bpy.data.images.new("glsl_test_image", 2, 2, alpha=True, float_buffer=True)
        image.generated_color = (0.25, 0.5, 0.75, 1.0)
        image_node.image = image

        source = "vec4 sample_color(sample2D src, vec2 uv){\n" "  return texture(src, uv);\n" "}\n"
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

    def test_nested_sample2d_closure_links_parse(self):
        _, tree = self.make_material_tree()
        outer_node = tree.nodes.new("ShaderNodeGLSLFunction")
        inner_node = tree.nodes.new("ShaderNodeGLSLFunction")
        image_node = tree.nodes.new("ShaderNodeImageToClosure")

        image = bpy.data.images.new("glsl_nested_test_image", 2, 2, alpha=True, float_buffer=True)
        image.generated_color = (0.8, 0.2, 0.1, 1.0)
        image_node.image = image

        inner_source = "vec4 inner_sample(sample2D src, vec2 uv){\n" "  return texture(src, uv);\n" "}\n"
        outer_source = "vec4 outer_sample(sample2D src, vec2 uv){\n" "  return inner_sample(src, uv);\n" "}\n" "vec4 inner_sample(sample2D src, vec2 uv){\n" "  return texture(src, uv);\n" "}\n"
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


if __name__ == "__main__":
    argv = []
    if "--" in sys.argv:
        argv = sys.argv[sys.argv.index("--") + 1 :]
    unittest.main(argv=[sys.argv[0], *argv])
