import bpy


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
]
