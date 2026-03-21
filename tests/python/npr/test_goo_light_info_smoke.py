import bpy


assert hasattr(bpy.types, "ShaderNodeLightInfo"), "ShaderNodeLightInfo is not registered"

material = bpy.data.materials.new("LightInfoSmoke")
material.use_nodes = True

node = material.node_tree.nodes.new("ShaderNodeLightInfo")

assert node.bl_label == "Light Info"
assert [socket.name for socket in node.inputs] == []
assert [socket.name for socket in node.outputs] == [
    "Color",
    "Power",
    "Type",
    "Position",
    "Direction",
    "Radius",
    "Spot Size",
    "Sun Angle",
]
assert node.outputs["Type"].type == "INT"
assert hasattr(node, "light_object"), "Light Info node should expose a light_object property"

light_data = bpy.data.lights.new("LightInfoSmokeLight", type="POINT")
light_object = bpy.data.objects.new("LightInfoSmokeLight", light_data)
node.light_object = light_object

assert node.light_object == light_object


def enabled_outputs(shader_node):
    return {socket.name for socket in shader_node.outputs if socket.enabled}


assert enabled_outputs(node) == {"Color", "Power", "Type", "Position", "Radius"}

light_data.type = "SUN"
assert enabled_outputs(node) == {"Color", "Power", "Type", "Direction", "Sun Angle"}

spot_data = bpy.data.lights.new("LightInfoSmokeSpot", type="SPOT")
spot_object = bpy.data.objects.new("LightInfoSmokeSpot", spot_data)
node.light_object = spot_object
assert enabled_outputs(node) == {"Color", "Power", "Type", "Position", "Direction", "Radius", "Spot Size"}

area_data = bpy.data.lights.new("LightInfoSmokeArea", type="AREA")
area_object = bpy.data.objects.new("LightInfoSmokeArea", area_data)
node.light_object = area_object
assert enabled_outputs(node) == {"Color", "Power", "Type", "Position", "Direction", "Radius"}

sun_data = bpy.data.lights.new("LightInfoSmokeSun", type="SUN")
sun_object = bpy.data.objects.new("LightInfoSmokeSun", sun_data)
node.light_object = sun_object
assert enabled_outputs(node) == {"Color", "Power", "Type", "Direction", "Sun Angle"}

node.light_object = None
assert enabled_outputs(node) == {"Color", "Power", "Type"}
