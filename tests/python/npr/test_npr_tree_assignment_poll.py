import bpy


def make_plain_tree():
    tree = bpy.data.node_groups.new("PlainShaderTree", "ShaderNodeTree")
    tree.nodes.new("ShaderNodeRGB")
    return tree


def make_npr_tree():
    tree = bpy.data.node_groups.new("RealNPRTree", "ShaderNodeTree")
    tree.nodes.new("ShaderNodeNPR_Output")
    return tree


def assert_npr_assignment_poll(output):
    plain_tree = make_plain_tree()
    real_npr_tree = make_npr_tree()

    output.nprtree = None
    try:
        output.nprtree = plain_tree
    except TypeError:
        pass

    assert output.nprtree is None, "Plain shader node trees should not be assignable as NPR trees"

    output.nprtree = real_npr_tree
    assert output.nprtree == real_npr_tree, (
        "Trees containing ShaderNodeNPR_Output should remain assignable"
    )


material = bpy.data.materials.new("NPRTreeAssignmentPoll")
material.use_nodes = True
material_output = next(
    node for node in material.node_tree.nodes if node.bl_idname == "ShaderNodeOutputMaterial"
)
assert_npr_assignment_poll(material_output)

world = bpy.data.worlds.new("WorldNPRTreeAssignmentPoll")
world.use_nodes = True
world_output = next(
    node for node in world.node_tree.nodes if node.bl_idname == "ShaderNodeOutputWorld"
)
assert_npr_assignment_poll(world_output)

print("PASS")
