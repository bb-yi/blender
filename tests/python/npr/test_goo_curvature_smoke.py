import bpy


assert hasattr(bpy.types, "ShaderNodeCurvature"), "ShaderNodeCurvature is not registered"

material = bpy.data.materials.new("GooCurvatureSmoke")
material.use_nodes = True

node = material.node_tree.nodes.new("ShaderNodeCurvature")

assert node.bl_label == "Curvature"
assert [socket.name for socket in node.inputs] == [
    "Samples",
    "Sample Radius",
    "Thickness",
    "Scale",
]
assert [socket.name for socket in node.outputs] == [
    "Scene Curvature",
    "Scene Rim",
    "Bevel Normal",
]
