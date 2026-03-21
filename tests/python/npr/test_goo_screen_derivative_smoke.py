import bpy


assert hasattr(bpy.types, "ShaderNodeScreenDerivative"), "ShaderNodeScreenDerivative is not registered"

material = bpy.data.materials.new("GooScreenDerivativeSmoke")
material.use_nodes = True

node = material.node_tree.nodes.new("ShaderNodeScreenDerivative")

assert node.bl_label == "Screen Derivative"
assert [socket.name for socket in node.inputs] == ["Value", "Value", "Value"]
assert [socket.name for socket in node.outputs] == ["Value", "Value", "Value"]
assert node.operation == "DDX"

operation_items = {item.identifier for item in node.bl_rna.properties["operation"].enum_items}
assert operation_items == {"DDX", "DDY", "DDXY"}

node.operation = "DDXY"
assert node.operation == "DDXY"

node.data_type = "FLOAT"
assert [socket.name for socket in node.inputs if socket.enabled] == ["Value"]
assert [socket.name for socket in node.outputs if socket.enabled] == ["Value"]
