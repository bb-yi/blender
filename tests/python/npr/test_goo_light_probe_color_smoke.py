import bpy


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
