import os
import tempfile

import bpy


RED = (0.8, 0.0, 0.0, 1.0)
BLUE = (0.0, 0.0, 0.8, 1.0)
NPR_RED = (0.2, 0.0, 0.0, 1.0)
NPR_BLUE = (0.0, 0.0, 0.2, 1.0)


def configure_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 192
    scene.render.resolution_y = 96
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = True
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    if hasattr(scene.eevee, "taa_samples"):
        scene.eevee.taa_samples = 1
    if hasattr(scene.eevee, "taa_render_samples"):
        scene.eevee.taa_render_samples = 1

    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 3.0
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 4.0)
    scene.collection.objects.link(camera)
    scene.camera = camera


def make_source_material():
    material = bpy.data.materials.new("SourceWithNPRTree")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    npr_tree = bpy.data.node_groups.new("SourcePassthroughNPRTree", "ShaderNodeTree")
    npr_attribute = npr_tree.nodes.new("ShaderNodeAttribute")
    npr_attribute.attribute_type = "INSTANCER"
    npr_attribute.attribute_name = "NPR_ID"
    npr_output = npr_tree.nodes.new("ShaderNodeNPR_Output")
    npr_tree.links.new(npr_attribute.outputs["Color"], npr_output.inputs["Color"])
    output.nprtree = npr_tree
    return material


def make_instance_material():
    material = bpy.data.materials.new("InstanceIDMaterial")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    attribute = nodes.new("ShaderNodeAttribute")
    attribute.attribute_type = "INSTANCER"
    attribute.attribute_name = "ID"
    emission = nodes.new("ShaderNodeEmission")
    output = nodes.new("ShaderNodeOutputMaterial")
    links.new(attribute.outputs["Color"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_source(name, location_x, material):
    bpy.ops.mesh.primitive_plane_add(size=1.0, location=(location_x, 0.0, 0.0))
    source = bpy.context.active_object
    source.name = name
    source.data.materials.append(material)
    return source


def make_instance_host(source_red, source_blue, instance_material):
    mesh = bpy.data.meshes.new("InstanceHostMesh")
    host = bpy.data.objects.new("InstanceHost", mesh)
    bpy.context.scene.collection.objects.link(host)

    tree = bpy.data.node_groups.new("InstanceAttributeGeometry", "GeometryNodeTree")
    tree.interface.new_socket(name="Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    nodes = tree.nodes
    links = tree.links

    group_output = nodes.new("NodeGroupOutput")
    join = nodes.new("GeometryNodeJoinGeometry")

    for source, color, npr_color, translation_x in (
        (source_red, RED, NPR_RED, -1.5),
        (source_blue, BLUE, NPR_BLUE, 1.5),
    ):
        object_info = nodes.new("GeometryNodeObjectInfo")
        object_info.inputs["Object"].default_value = source
        object_info.inputs["As Instance"].default_value = True

        store = nodes.new("GeometryNodeStoreNamedAttribute")
        store.data_type = "FLOAT_COLOR"
        store.domain = "INSTANCE"
        store.inputs["Name"].default_value = "ID"
        store.inputs["Value"].default_value = color

        store_npr = nodes.new("GeometryNodeStoreNamedAttribute")
        store_npr.data_type = "FLOAT_COLOR"
        store_npr.domain = "INSTANCE"
        store_npr.inputs["Name"].default_value = "NPR_ID"
        store_npr.inputs["Value"].default_value = npr_color

        transform = nodes.new("GeometryNodeTransform")
        transform.inputs["Translation"].default_value = (translation_x, 0.0, 0.0)

        links.new(object_info.outputs["Geometry"], transform.inputs["Geometry"])
        links.new(transform.outputs["Geometry"], store.inputs["Geometry"])
        links.new(store.outputs["Geometry"], store_npr.inputs["Geometry"])
        links.new(store_npr.outputs["Geometry"], join.inputs["Geometry"])

    set_material = nodes.new("GeometryNodeSetMaterial")
    set_material.inputs["Material"].default_value = instance_material
    links.new(join.outputs["Geometry"], set_material.inputs["Geometry"])
    links.new(set_material.outputs["Geometry"], group_output.inputs["Geometry"])

    modifier = host.modifiers.new("GeometryNodes", "NODES")
    modifier.node_group = tree
    return host


def render_color_counts():
    file_descriptor, filepath = tempfile.mkstemp(suffix=".exr")
    os.close(file_descriptor)

    scene = bpy.context.scene
    scene.render.image_settings.file_format = "OPEN_EXR"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "32"
    scene.render.filepath = filepath
    bpy.ops.render.render(write_still=True)

    image = bpy.data.images.load(filepath, check_existing=False)
    try:
        pixels = list(image.pixels[:])
        width = image.size[0]
    finally:
        bpy.data.images.remove(image)
        os.remove(filepath)

    counts = {
        "left_opaque": 0,
        "left_red": 0,
        "left_blue": 0,
        "right_opaque": 0,
        "right_red": 0,
        "right_blue": 0,
    }
    for index in range(0, len(pixels), 4):
        red, green, blue, alpha = pixels[index:index + 4]
        if alpha < 0.5:
            continue
        x = (index // 4) % width
        side = "left" if x < width // 2 else "right"
        counts[f"{side}_opaque"] += 1
        if red > 0.6 and green < 0.1 and blue < 0.1:
            counts[f"{side}_red"] += 1
        if blue > 0.6 and red < 0.1 and green < 0.1:
            counts[f"{side}_blue"] += 1
    return counts


def assert_instance_colors(label):
    counts = render_color_counts()
    print(f"NPR_INSTANCE_ATTRIBUTE_{label}={counts}")
    assert counts["left_opaque"] > 400, f"Expected left instance geometry after {label}: {counts}"
    assert counts["right_opaque"] > 400, f"Expected right instance geometry after {label}: {counts}"
    assert counts["left_red"] > 400, f"Expected stable red left instance after {label}: {counts}"
    assert counts["right_blue"] > 400, f"Expected stable blue right instance after {label}: {counts}"
    assert counts["left_blue"] < 20, f"Unexpected blue ID on left instance after {label}: {counts}"
    assert counts["right_red"] < 20, f"Unexpected red ID on right instance after {label}: {counts}"


configure_scene()
source_material = make_source_material()
instance_material = make_instance_material()
source_red = make_source("SourceRed", -4.0, source_material)
source_blue = make_source("SourceBlue", 4.0, source_material)
make_instance_host(source_red, source_blue, instance_material)

assert_instance_colors("INITIAL")

source_red.location = (-3.8, 0.2, 0.0)
source_blue.location = (3.8, -0.2, 0.0)
bpy.context.view_layer.update()
assert_instance_colors("MOVED")

print("EEVEE_NPR_INSTANCE_ATTRIBUTE_STABILITY_OK")
