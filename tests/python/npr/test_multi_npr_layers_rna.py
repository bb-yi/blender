import bpy


material = bpy.data.materials.new("MultiNPRLayerRNA")
material.use_nodes = True

assert hasattr(material, "npr_layers"), "Material.npr_layers RNA collection is missing"

layer = material.npr_layers.add()
layer.name = "Layer A"
layer.enabled = True

tree = bpy.data.node_groups.new("Layer A Tree", "ShaderNodeTree")
tree.nodes.new("ShaderNodeNPR_Output")
layer.node_tree = tree

assert len(material.npr_layers) == 1
assert material.npr_layers[0].name == "Layer A"
assert material.npr_layers[0].enabled is True
assert material.npr_layers[0].node_tree == tree

print("PASS")
