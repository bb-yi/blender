import bpy
import bl_ui.node_add_menu_shader as shader_menu


assert hasattr(
    bpy.types, "ShaderNodeLightProbeColor"
), "ShaderNodeLightProbeColor is not registered"

material = bpy.data.materials.new("LightProbeColorSmoke")
material.use_nodes = True

node = material.node_tree.nodes.new("ShaderNodeLightProbeColor")

assert node.bl_label == "Light Probe Color"
assert [socket.name for socket in node.inputs] == ["Direction"]
assert [socket.name for socket in node.outputs] == ["Reflection", "Irradiance", "Combined"]
assert node.inputs["Direction"].type == "VECTOR"
assert node.inputs["Direction"].hide_value is True
assert node.outputs["Reflection"].type == "RGBA"
assert node.outputs["Irradiance"].type == "RGBA"
assert node.outputs["Combined"].type == "RGBA"

seen_menu_nodes = []


class FakeShaderInputMenu:
    bl_label = "Input"
    layout = object()

    def node_operator(self, layout, node_type, **kwargs):
        seen_menu_nodes.append((node_type, kwargs.get("poll")))
        return object()

    def node_operator_with_outputs(self, context, layout, node_type, subnames, **kwargs):
        return self.node_operator(layout, node_type, **kwargs)

    def draw_assets_for_catalog(self, layout, catalog_path):
        pass


class FakeSpace:
    tree_type = "ShaderNodeTree"
    shader_type = "OBJECT"


class FakeContext:
    space_data = FakeSpace()
    engine = "BLENDER_EEVEE"


shader_menu.NODE_MT_shader_node_input_base.draw(FakeShaderInputMenu(), FakeContext())
menu_matches = [item for item in seen_menu_nodes if item[0] == "ShaderNodeLightProbeColor"]
assert menu_matches, "ShaderNodeLightProbeColor is not in the shader input menu"
assert menu_matches[0][1] is True, "ShaderNodeLightProbeColor menu poll did not pass"
