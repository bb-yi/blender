import bpy
import os
import tempfile


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def configure_scene():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 64
    scene.render.resolution_y = 64
    scene.render.resolution_percentage = 100
    scene.eevee.taa_samples = 1
    scene.eevee.taa_render_samples = 1
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.world.use_nodes = False
    scene.world.color = (0.0, 0.0, 0.0)


def make_camera():
    camera_data = bpy.data.cameras.new("Camera")
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 5.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def make_material():
    material = bpy.data.materials.new("NPRDisplacementMaterial")
    material.use_nodes = True
    material.displacement_method = "DISPLACEMENT"
    material.max_vertex_displacement = 2.0

    nodes = material.node_tree.nodes
    links = material.node_tree.links

    output = next(node for node in nodes if node.bl_idname == "ShaderNodeOutputMaterial")
    principled = next(node for node in nodes if node.bl_idname == "ShaderNodeBsdfPrincipled")
    principled.inputs["Base Color"].default_value = (1.0, 0.0, 0.0, 1.0)
    principled.inputs["Roughness"].default_value = 1.0

    displacement = nodes.new("ShaderNodeDisplacement")
    displacement.inputs["Height"].default_value = 1.0
    displacement.inputs["Midlevel"].default_value = 0.0
    displacement.inputs["Scale"].default_value = 1.0
    links.new(displacement.outputs["Displacement"], output.inputs["Displacement"])

    npr_tree = bpy.data.node_groups.new("NPRDisplacementTree", "ShaderNodeTree")
    npr_nodes = npr_tree.nodes
    npr_links = npr_tree.links

    rgb = npr_nodes.new("ShaderNodeRGB")
    rgb.outputs["Color"].default_value = (0.0, 1.0, 0.0, 1.0)
    npr_output = npr_nodes.new("ShaderNodeNPR_Output")
    npr_links.new(rgb.outputs["Color"], npr_output.inputs["Color"])

    output.nprtree = npr_tree
    return material


def make_plane(material):
    bpy.ops.mesh.primitive_plane_add(size=2.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.active_object
    plane.data.materials.append(material)
    return plane


def make_nonmesh_geometry(material, geometry_type):
    """Keep curves/points as native geometry so their own vertex shaders are exercised."""
    if geometry_type == "CURVES":
        data = bpy.data.hair_curves.new("NPRCurvesHost")
        # Native curves must enter the curves draw path. STRAND is a one-pixel line and can
        # miss the center pixel at even resolutions; STRIP uses the declared nonzero radius.
        bpy.context.scene.render.hair_type = "STRIP"
    else:
        data = bpy.data.meshes.new(f"{geometry_type}Host")
    data.materials.append(material)
    obj = bpy.data.objects.new(f"NPR{geometry_type}", data)
    bpy.context.scene.collection.objects.link(obj)
    tree = bpy.data.node_groups.new(f"NPR{geometry_type}Geometry", "GeometryNodeTree")
    tree.interface.new_socket(name="Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    output = tree.nodes.new("NodeGroupOutput")
    assign_material = tree.nodes.new("GeometryNodeSetMaterial")
    assign_material.inputs["Material"].default_value = material
    tree.links.new(assign_material.outputs["Geometry"], output.inputs["Geometry"])
    if geometry_type == "CURVES":
        assert obj.type == "CURVES"
        line = tree.nodes.new("GeometryNodeCurvePrimitiveLine")
        line.inputs["Start"].default_value = (-1.0, 0.0, 0.0)
        line.inputs["End"].default_value = (1.0, 0.0, 0.0)
        radius = tree.nodes.new("GeometryNodeSetCurveRadius")
        radius.inputs["Radius"].default_value = 0.35
        tree.links.new(line.outputs["Curve"], radius.inputs["Curve"])
        tree.links.new(radius.outputs["Curve"], assign_material.inputs["Geometry"])
    else:
        assert geometry_type == "POINTCLOUD"
        points = tree.nodes.new("GeometryNodePoints")
        points.inputs["Count"].default_value = 1
        points.inputs["Position"].default_value = (0.0, 0.0, 0.0)
        points.inputs["Radius"].default_value = 0.65
        tree.links.new(points.outputs[0], assign_material.inputs["Geometry"])
    modifier = obj.modifiers.new("Native geometry", "NODES")
    modifier.node_group = tree
    return obj


def render_center_pixel():
    scene = bpy.context.scene
    file_descriptor, filepath = tempfile.mkstemp(suffix=".exr")
    os.close(file_descriptor)

    scene.render.image_settings.file_format = "OPEN_EXR"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "32"
    scene.render.filepath = filepath

    bpy.ops.render.render(write_still=False)
    bpy.data.images["Render Result"].save_render(filepath)

    image = bpy.data.images.load(filepath, check_existing=False)
    try:
        width = image.size[0]
        height = image.size[1]
        index = ((height // 2) * width + (width // 2)) * 4
        pixel = list(image.pixels[index:index + 4])
    finally:
        bpy.data.images.remove(image)
        if os.path.exists(filepath):
            os.remove(filepath)

    return pixel


def assert_green(pixel, label):
    r, g, b, _a = pixel
    assert g > 0.8, f"Expected {label} render to keep NPR green output, got {pixel}"
    assert r < 0.1, f"Expected {label} render to suppress red base shading, got {pixel}"
    assert b < 0.1, f"Expected {label} render to keep blue low, got {pixel}"


clear_scene()
configure_scene()
make_camera()
material = make_material()
plane = make_plane(material)

material.displacement_method = "BUMP"
bump_pixel = render_center_pixel()
assert_green(bump_pixel, "bump")

material.displacement_method = "DISPLACEMENT"
displacement_pixel = render_center_pixel()
assert_green(displacement_pixel, "displacement")

print(f"NPR_BUMP_CENTER={bump_pixel}")
print(f"NPR_DISPLACEMENT_CENTER={displacement_pixel}")

# Preserve the original mesh assertions above and also cover mixed displacement and the other
# geometry entry points that include the same BSL nodetree library in their vertex stage.
material.displacement_method = "BOTH"
assert_green(render_center_pixel(), "mesh BOTH")
nodes = material.node_tree.nodes
output = next(node for node in nodes if node.bl_idname == "ShaderNodeOutputMaterial")
tess = nodes.new("ShaderNodeTessellationControl")
tess.inputs["Max Level"].default_value = 4
material.node_tree.links.new(tess.outputs[0], output.inputs["Tessellation"])
for mode in ("BUMP", "DISPLACEMENT", "BOTH"):
    material.displacement_method = mode
    pixel = render_center_pixel()
    assert_green(pixel, f"mesh tessellation {mode}")
    print(f"NPR_TESS_{mode}_CENTER={pixel}")
nodes.remove(tess)
bpy.data.objects.remove(plane, do_unlink=True)

displacement = next(node for node in nodes if node.bl_idname == "ShaderNodeDisplacement")
# Keep a nonzero displacement graph without moving the narrow curve outside the sampled pixel.
displacement.inputs["Scale"].default_value = 0.05
for geometry_type in ("CURVES", "POINTCLOUD"):
    obj = make_nonmesh_geometry(material, geometry_type)
    for mode in ("BUMP", "DISPLACEMENT", "BOTH"):
        material.displacement_method = mode
        pixel = render_center_pixel()
        assert_green(pixel, f"{geometry_type} {mode}")
        print(f"NPR_{geometry_type}_{mode}_CENTER={pixel}")
    bpy.data.objects.remove(obj, do_unlink=True)

print("NPR_DISPLACEMENT_GEOMETRIES_PASS mesh,tessellation,curves,pointcloud x BUMP,DISPLACEMENT,BOTH")
