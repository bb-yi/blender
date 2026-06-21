import bpy


assert hasattr(bpy.types, "ShaderNodeRenderInfo"), "ShaderNodeRenderInfo is not registered"

material = bpy.data.materials.new("RenderInfoSmoke")
material.use_nodes = True

node = material.node_tree.nodes.new("ShaderNodeRenderInfo")

assert node.bl_label == "Render Info"
assert [socket.name for socket in node.inputs] == []
assert [socket.name for socket in node.outputs] == [
    "Frag Coord",
    "Width",
    "Height",
    "Resolution",
    "Current Sample",
    "Total Samples",
    "Sample Offset",
]
assert node.outputs["Frag Coord"].type == "VECTOR"
assert node.outputs["Resolution"].type == "VECTOR"
assert node.outputs["Current Sample"].type == "VALUE"
assert node.outputs["Total Samples"].type == "VALUE"
assert node.outputs["Sample Offset"].type == "VECTOR"
