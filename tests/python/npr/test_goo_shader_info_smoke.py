import bpy
import bl_ui.node_add_menu_shader as shader_menu


assert hasattr(bpy.types, "ShaderNodeShaderInfo"), "ShaderNodeShaderInfo is not registered"

material = bpy.data.materials.new("ShaderInfoSmoke")
material.use_nodes = True

node = material.node_tree.nodes.new("ShaderNodeShaderInfo")

assert node.bl_label == "Shader Info"
assert [socket.name for socket in node.inputs] == ["World Position", "Normal", "Exponent"]
assert [socket.name for socket in node.outputs] == [
    "Diffuse Shading",
    "Shadow",
    "Ambient Lighting",
    "Half-Lambert Factor",
    "Blinn-Phong Factor",
    "Self Shadow",
    "Cast Shadow",
]


seen_menu_nodes = []


class FakeLayout:
    def separator(self):
        pass


class FakeShaderInputMenu:
    bl_label = "Input"
    layout = FakeLayout()

    def node_operator(self, layout, node_type, **kwargs):
        seen_menu_nodes.append((node_type, kwargs.get("poll")))
        return object()

    def node_operator_with_outputs(self, context, layout, node_type, subnames, **kwargs):
        return self.node_operator(layout, node_type, **kwargs)

    def draw_assets_for_catalog(self, layout, catalog_path):
        pass

    def draw_menu(self, layout, path):
        pass


class FakeSpace:
    tree_type = "ShaderNodeTree"
    shader_type = "NPR"
    id_from = None


class FakeContext:
    space_data = FakeSpace()
    engine = "BLENDER_EEVEE"


shader_menu.NODE_MT_shader_node_input_base.draw(FakeShaderInputMenu(), FakeContext())
menu_matches = [item for item in seen_menu_nodes if item[0] == "ShaderNodeShaderInfo"]
assert menu_matches, "ShaderNodeShaderInfo is not in the shader input menu"
assert menu_matches[0][1] is True, "ShaderNodeShaderInfo menu poll did not pass for NPR"
