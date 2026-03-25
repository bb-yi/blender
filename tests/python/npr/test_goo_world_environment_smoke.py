import bpy


assert hasattr(
    bpy.types, "ShaderNodeWorldEnvironment"
), "ShaderNodeWorldEnvironment is not registered"

material = bpy.data.materials.new("WorldEnvironmentSmoke")
material.use_nodes = True

node = material.node_tree.nodes.new("ShaderNodeWorldEnvironment")

assert node.bl_label == "World Environment"
assert [socket.name for socket in node.inputs] == ["Direction"]
assert [socket.name for socket in node.outputs] == ["Color"]
assert node.inputs["Direction"].type == "VECTOR"
assert node.inputs["Direction"].hide_value is True
assert node.outputs["Color"].type == "RGBA"
