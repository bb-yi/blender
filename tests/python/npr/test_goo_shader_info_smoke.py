import bpy
import bl_ui.node_add_menu_shader as shader_menu


assert hasattr(bpy.types, "ShaderNodeShaderInfo"), "ShaderNodeShaderInfo is not registered"

material = bpy.data.materials.new("ShaderInfoSmoke")
material.use_nodes = True

node = material.node_tree.nodes.new("ShaderNodeShaderInfo")

assert node.bl_label == "Shader Info"
assert [socket.name for socket in node.inputs] == ["World Position", "Normal", "Exponent", "PCF Radius", "Max Penumbra"]
assert node.shadow_mode == "TEMPORAL"
assert node.stable_shadow_samples == 8
mode_items = node.bl_rna.properties["shadow_mode"].enum_items_static
assert [(item.identifier, item.value) for item in mode_items if item.identifier] == [
    ("TEMPORAL", 1),
    ("SOFT_FILTERED", 2),
    ("PCSS", 5),
    ("HARD", 3),
    ("PCF", 4),
    ("VSM", 6),
    ("VSSM", 7),
    ("MSM", 8),
    ("CHS", 9),
]
assert mode_items["PCF"].name == "PCF (Fixed Radius)"
assert node.inputs["PCF Radius"].default_value == -1.0
assert node.inputs["PCF Radius"].enabled

tree = material.node_tree
inputs_by_identifier = {socket.identifier: socket for socket in node.inputs}
radius = inputs_by_identifier["PCF Radius"]
penumbra = inputs_by_identifier["Max Penumbra"]
radius.default_value = 3.5
penumbra.default_value = 0.375
value_output = tree.nodes.new("ShaderNodeValue").outputs[0]
tree.links.new(value_output, radius)
node.shadow_mode = "CHS"
tree.links.new(value_output, penumbra)
for mode, radius_enabled, penumbra_enabled in (
    ("SOFT_FILTERED", False, False),
    ("PCSS", True, False),
    ("CHS", False, True),
    ("TEMPORAL", True, False),
):
    node.shadow_mode = mode
    assert (radius.enabled, penumbra.enabled) == (radius_enabled, penumbra_enabled), mode
    assert (radius.default_value, penumbra.default_value) == (3.5, 0.375), mode
    assert {link.to_socket.identifier for link in tree.links if link.from_socket == value_output} == {
        "PCF Radius", "Max Penumbra",
    }, mode

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
