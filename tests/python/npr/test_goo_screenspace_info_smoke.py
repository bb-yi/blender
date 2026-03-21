import bpy


assert hasattr(bpy.types, "ShaderNodeScreenspaceInfo"), "ShaderNodeScreenspaceInfo is not registered"

material = bpy.data.materials.new("GooScreenspaceInfoSmoke")
material.use_nodes = True

node = material.node_tree.nodes.new("ShaderNodeScreenspaceInfo")

assert node.bl_label == "Screenspace Info"
assert [socket.name for socket in node.inputs] == ["View Position"]
assert [socket.name for socket in node.outputs] == ["Scene Color", "Scene Depth"]
