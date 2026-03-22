import bpy


assert hasattr(bpy.types, "ShaderNodeRenderInfo"), "ShaderNodeRenderInfo is not registered"

material = bpy.data.materials.new("RenderInfoSmoke")
material.use_nodes = True

node = material.node_tree.nodes.new("ShaderNodeRenderInfo")

assert node.bl_label == "Render Info"
assert [socket.name for socket in node.inputs] == []
assert [socket.name for socket in node.outputs] == ["Width", "Height", "Frag Coord"]
assert node.outputs["Frag Coord"].type == "VECTOR"

