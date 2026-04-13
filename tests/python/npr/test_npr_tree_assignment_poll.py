import bpy


material = bpy.data.materials.new("NPRTreeAssignmentPoll")
material.use_nodes = True

output = next(node for node in material.node_tree.nodes if node.bl_idname == "ShaderNodeOutputMaterial")

plain_tree = bpy.data.node_groups.new("PlainShaderTree", "ShaderNodeTree")
plain_tree.nodes.new("ShaderNodeRGB")

real_npr_tree = bpy.data.node_groups.new("RealNPRTree", "ShaderNodeTree")
real_npr_tree.nodes.new("ShaderNodeNPR_Output")

output.nprtree = None
try:
    output.nprtree = plain_tree
except TypeError:
    pass

assert output.nprtree is None, "Plain shader node trees should not be assignable as NPR trees"

output.nprtree = real_npr_tree
assert output.nprtree == real_npr_tree, "Trees containing ShaderNodeNPR_Output should remain assignable"

print("PASS")
