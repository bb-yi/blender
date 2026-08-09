import bpy
import os
import subprocess


CASE_DIR = os.path.dirname(__file__)
ROOT_DIR = os.path.abspath(os.path.join(CASE_DIR, "..", "..", ".."))
ASSET_DIR = os.path.join(CASE_DIR, "assets")
OUT_DIR = os.path.join(CASE_DIR, "out")
IMAGE_SIZE = 32
EPSILON = 0.035


def set_world_color(color, strength=1.0):
    scene = bpy.context.scene
    if scene.world is None:
        scene.world = bpy.data.worlds.new("World")
    world = scene.world
    world.color = color
    world.use_nodes = True
    nodes = world.node_tree.nodes
    links = world.node_tree.links
    nodes.clear()
    background = nodes.new("ShaderNodeBackground")
    output = nodes.new("ShaderNodeOutputWorld")
    background.inputs["Color"].default_value = (*color, 1.0)
    background.inputs["Strength"].default_value = strength
    links.new(background.outputs["Background"], output.inputs["Surface"])
    bpy.context.view_layer.update()


def reset_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()
    for datablock in (
        bpy.data.meshes,
        bpy.data.materials,
        bpy.data.images,
        bpy.data.node_groups,
        bpy.data.lights,
        bpy.data.cameras,
        bpy.data.texts,
    ):
        for item in list(datablock):
            datablock.remove(item)

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.bake.target = "IMAGE_TEXTURES"
    scene.render.bake.use_clear = True
    scene.render.bake.margin = 0
    scene.render.bake.margin_type = "EXTEND"
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    set_world_color((0.0, 0.0, 0.0))
    if hasattr(scene, "eevee"):
        scene.eevee.taa_samples = 1
        scene.eevee.taa_render_samples = 1


def create_image(name, color=(0.0, 0.0, 0.0, 0.0)):
    image = bpy.data.images.new(name, IMAGE_SIZE, IMAGE_SIZE, alpha=True, float_buffer=True)
    image.colorspace_settings.name = "Non-Color"
    image.pixels.foreach_set(list(color) * (IMAGE_SIZE * IMAGE_SIZE))
    return image


def selected_image_texture(material, image):
    nodes = material.node_tree.nodes
    node = nodes.new("ShaderNodeTexImage")
    node.image = image
    node.select = True
    nodes.active = node
    return node


def build_surface_material(name):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()
    output = nodes.new("ShaderNodeOutputMaterial")
    return material, nodes, links, output


def make_text_block(name, source):
    text = bpy.data.texts.get(name)
    if text is None:
        text = bpy.data.texts.new(name)
    else:
        text.clear()
    text.write(source)
    return text


def refresh_glsl_node(node):
    current_name = node.function_name
    node.function_name = ""
    node.function_name = current_name
    node.id_data.interface_update(bpy.context)
    node.id_data.update_tag()
    bpy.context.view_layer.update()


def material_with_emission(name, image, color, strength=1.0):
    material, nodes, links, output = build_surface_material(name)
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = color
    emission.inputs["Strength"].default_value = strength
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    if image is not None:
        selected_image_texture(material, image)
    return material


def material_with_principled_emission(name, image, color, strength=1.0):
    material, nodes, links, output = build_surface_material(name)
    bsdf = nodes.new("ShaderNodeBsdfPrincipled")
    bsdf.inputs["Base Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    bsdf.inputs["Emission Color"].default_value = color
    bsdf.inputs["Emission Strength"].default_value = strength
    links.new(bsdf.outputs["BSDF"], output.inputs["Surface"])

    selected_image_texture(material, image)
    return material


def material_with_npr_color(name, image, color):
    material = material_with_emission(name, image, (0.01, 0.01, 0.01, 1.0), 1.0)
    nodes = material.node_tree.nodes
    output = next(node for node in nodes if node.bl_idname == "ShaderNodeOutputMaterial")

    npr_tree = bpy.data.node_groups.new(name + "Tree", "ShaderNodeTree")
    npr_nodes = npr_tree.nodes
    npr_links = npr_tree.links
    rgb = npr_nodes.new("ShaderNodeRGB")
    rgb.outputs["Color"].default_value = color
    npr_output = npr_nodes.new("ShaderNodeNPR_Output")
    npr_links.new(rgb.outputs["Color"], npr_output.inputs["Color"])
    output.nprtree = npr_tree
    return material


def material_with_shader_info(name, image):
    material, nodes, links, output = build_surface_material(name)
    emission = nodes.new("ShaderNodeEmission")
    shader_info = nodes.new("ShaderNodeShaderInfo")
    links.new(shader_info.outputs["Diffuse Shading"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    selected_image_texture(material, image)
    return material


def material_with_shader_info_shadow(name, image):
    material, nodes, links, output = build_surface_material(name)
    emission = nodes.new("ShaderNodeEmission")
    shader_info = nodes.new("ShaderNodeShaderInfo")
    shader_info.shadow_mode = "TEMPORAL"
    links.new(shader_info.outputs["Shadow"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    selected_image_texture(material, image)
    return material


def material_with_tangent_normal_shader_info(name, image, normal_color):
    material, nodes, links, output = build_surface_material(name)
    emission = nodes.new("ShaderNodeEmission")
    shader_info = nodes.new("ShaderNodeShaderInfo")
    normal_map = nodes.new("ShaderNodeNormalMap")

    normal_map.space = "TANGENT"
    normal_map.uv_map = "UVMap"
    normal_map.inputs["Strength"].default_value = 1.0
    normal_map.inputs["Color"].default_value = normal_color

    links.new(normal_map.outputs["Normal"], shader_info.inputs["Normal"])
    links.new(shader_info.outputs["Diffuse Shading"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    selected_image_texture(material, image)
    return material


def material_with_image_texture(name, bake_image, source_color):
    material, nodes, links, output = build_surface_material(name)
    bsdf = nodes.new("ShaderNodeBsdfDiffuse")
    source_image = create_image(name + "Source", source_color)
    tex_image = nodes.new("ShaderNodeTexImage")
    tex_image.image = source_image
    tex_image.interpolation = "Closest"
    links.new(tex_image.outputs["Color"], bsdf.inputs["Color"])
    links.new(bsdf.outputs["BSDF"], output.inputs["Surface"])
    selected_image_texture(material, bake_image)
    return material


def material_with_checker_ramp(name, bake_image, color_a, color_b):
    material, nodes, links, output = build_surface_material(name)
    emission = nodes.new("ShaderNodeEmission")
    uv_map = nodes.new("ShaderNodeUVMap")
    mapping = nodes.new("ShaderNodeMapping")
    checker = nodes.new("ShaderNodeTexChecker")
    ramp = nodes.new("ShaderNodeValToRGB")

    uv_map.uv_map = "UVMap"
    mapping.inputs["Location"].default_value = (0.0, 0.0, 0.0)
    mapping.inputs["Rotation"].default_value = (0.0, 0.0, 0.0)
    mapping.inputs["Scale"].default_value = (1.0, 1.0, 1.0)
    checker.inputs["Scale"].default_value = 2.0
    checker.inputs["Color1"].default_value = (1.0, 0.0, 0.0, 1.0)
    checker.inputs["Color2"].default_value = (0.0, 0.0, 1.0, 1.0)

    ramp.color_ramp.elements[0].position = 0.0
    ramp.color_ramp.elements[0].color = color_a
    ramp.color_ramp.elements[1].position = 1.0
    ramp.color_ramp.elements[1].color = color_b

    links.new(uv_map.outputs["UV"], mapping.inputs["Vector"])
    links.new(mapping.outputs["Vector"], checker.inputs["Vector"])
    links.new(checker.outputs["Fac"], ramp.inputs["Fac"])
    links.new(ramp.outputs["Color"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    selected_image_texture(material, bake_image)
    return material


def material_with_group_color(name, bake_image, color):
    material, nodes, links, output = build_surface_material(name)
    emission = nodes.new("ShaderNodeEmission")
    group = bpy.data.node_groups.new(name + "Group", "ShaderNodeTree")
    group.interface.new_socket(name="Color", in_out="OUTPUT", socket_type="NodeSocketColor")
    group_rgb = group.nodes.new("ShaderNodeRGB")
    group_rgb.outputs["Color"].default_value = color
    group_output = group.nodes.new("NodeGroupOutput")
    group.links.new(group_rgb.outputs["Color"], group_output.inputs["Color"])
    group_node = nodes.new("ShaderNodeGroup")
    group_node.node_tree = group
    links.new(group_node.outputs["Color"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    selected_image_texture(material, bake_image)
    return material


def material_with_group_unconnected_unsupported(name, bake_image, color):
    material, nodes, links, output = build_surface_material(name)
    emission = nodes.new("ShaderNodeEmission")
    group = bpy.data.node_groups.new(name + "Group", "ShaderNodeTree")
    group.interface.new_socket(name="Color", in_out="OUTPUT", socket_type="NodeSocketColor")
    group.interface.new_socket(name="Unused", in_out="OUTPUT", socket_type="NodeSocketColor")
    group_rgb = group.nodes.new("ShaderNodeRGB")
    group_rgb.outputs["Color"].default_value = color
    group_unsupported = group.nodes.new("ShaderNodeSceneColor")
    group_sample = group.nodes.new("ShaderNodeNPR_ImageSample")
    group_output = group.nodes.new("NodeGroupOutput")
    group.links.new(group_rgb.outputs["Color"], group_output.inputs["Color"])
    group.links.new(group_unsupported.outputs["Color Image"], group_sample.inputs["Image"])
    group.links.new(group_sample.outputs["Color"], group_output.inputs["Unused"])
    group_node = nodes.new("ShaderNodeGroup")
    group_node.node_tree = group
    links.new(group_node.outputs["Color"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    selected_image_texture(material, bake_image)
    return material


def material_with_diffuse_surface(name, image, color):
    material, nodes, links, output = build_surface_material(name)
    diffuse = nodes.new("ShaderNodeBsdfDiffuse")
    diffuse.inputs["Color"].default_value = color
    links.new(diffuse.outputs["BSDF"], output.inputs["Surface"])

    selected_image_texture(material, image)
    return material


def material_with_attribute_color(name, image, attr_name):
    material, nodes, links, output = build_surface_material(name)
    emission = nodes.new("ShaderNodeEmission")
    attribute = nodes.new("ShaderNodeAttribute")
    attribute.attribute_name = attr_name
    attribute.attribute_type = "GEOMETRY"
    links.new(attribute.outputs["Color"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    if image is not None:
        selected_image_texture(material, image)
    return material


def material_with_generated_coordinate(name, image):
    material, nodes, links, output = build_surface_material(name)
    emission = nodes.new("ShaderNodeEmission")
    tex_coord = nodes.new("ShaderNodeTexCoord")
    separate = nodes.new("ShaderNodeSeparateXYZ")
    combine = nodes.new("ShaderNodeCombineColor")
    combine.inputs["Green"].default_value = 0.0
    combine.inputs["Blue"].default_value = 0.0
    links.new(tex_coord.outputs["Generated"], separate.inputs["Vector"])
    links.new(separate.outputs["X"], combine.inputs["Red"])
    links.new(combine.outputs["Color"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    selected_image_texture(material, image)
    return material


def material_without_bake_target(name, color):
    material, nodes, links, output = build_surface_material(name)
    diffuse = nodes.new("ShaderNodeBsdfDiffuse")
    diffuse.inputs["Color"].default_value = color
    links.new(diffuse.outputs["BSDF"], output.inputs["Surface"])
    return material


def material_with_shader_to_rgb(name, image, color):
    material, nodes, links, output = build_surface_material(name)
    diffuse = nodes.new("ShaderNodeBsdfDiffuse")
    shader_to_rgb = nodes.new("ShaderNodeShaderToRGB")
    ramp = nodes.new("ShaderNodeValToRGB")
    emission = nodes.new("ShaderNodeEmission")

    diffuse.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    ramp.color_ramp.elements[0].position = 0.0
    ramp.color_ramp.elements[0].color = (0.0, 0.0, 0.0, 1.0)
    ramp.color_ramp.elements[1].position = 1.0
    ramp.color_ramp.elements[1].color = color

    links.new(diffuse.outputs["BSDF"], shader_to_rgb.inputs["Shader"])
    links.new(shader_to_rgb.outputs["Color"], ramp.inputs["Fac"])
    links.new(ramp.outputs["Color"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    selected_image_texture(material, image)
    return material


def material_with_glsl_function(name, image, input_color, expected_color):
    material, nodes, links, output = build_surface_material(name)
    emission = nodes.new("ShaderNodeEmission")
    glsl = nodes.new("ShaderNodeGLSLFunction")
    make_text_block(
        name + "_glsl",
        "vec4 invert_color(vec4 color) {\n"
        "  return vec4(1.0 - color.rgb, color.a);\n"
        "}\n",
    )
    glsl.script = bpy.data.texts[name + "_glsl"]
    glsl.function_name = "invert_color"
    refresh_glsl_node(glsl)
    if glsl.parse_status != "READY":
        raise AssertionError(f"Expected GLSL Function parse READY, got {glsl.parse_status}")
    glsl.inputs["color"].default_value = input_color[:3]
    glsl.inputs["color W"].default_value = input_color[3]
    links.new(glsl.outputs["Result"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    selected_image_texture(material, image)
    return material, expected_color


def material_with_npr_input(name, image):
    material = material_with_emission(name, image, (0.2, 0.2, 0.2, 1.0), 1.0)
    output = next(
        node for node in material.node_tree.nodes if node.bl_idname == "ShaderNodeOutputMaterial"
    )

    npr_tree = bpy.data.node_groups.new(name + "Tree", "ShaderNodeTree")
    npr_nodes = npr_tree.nodes
    npr_links = npr_tree.links
    npr_input = npr_nodes.new("ShaderNodeNPR_Input")
    npr_output = npr_nodes.new("ShaderNodeNPR_Output")
    npr_links.new(npr_input.outputs[0], npr_output.inputs["Color"])
    output.nprtree = npr_tree
    return material


def material_with_npr_refraction(name, image):
    material = material_with_emission(name, image, (0.2, 0.2, 0.2, 1.0), 1.0)
    output = next(
        node for node in material.node_tree.nodes if node.bl_idname == "ShaderNodeOutputMaterial"
    )

    npr_tree = bpy.data.node_groups.new(name + "Tree", "ShaderNodeTree")
    npr_nodes = npr_tree.nodes
    npr_links = npr_tree.links
    npr_refraction = npr_nodes.new("ShaderNodeNPR_Refraction")
    npr_output = npr_nodes.new("ShaderNodeNPR_Output")
    npr_links.new(npr_refraction.outputs["Combined Color"], npr_output.inputs["Color"])
    output.nprtree = npr_tree
    return material


def material_with_npr_input_aov(name, image):
    material = material_with_emission(name, image, (0.2, 0.2, 0.2, 1.0), 1.0)
    output = next(
        node for node in material.node_tree.nodes if node.bl_idname == "ShaderNodeOutputMaterial"
    )

    npr_tree = bpy.data.node_groups.new(name + "Tree", "ShaderNodeTree")
    npr_nodes = npr_tree.nodes
    npr_links = npr_tree.links
    input_aov = npr_nodes.new("ShaderNodeInputAOV")
    npr_output = npr_nodes.new("ShaderNodeNPR_Output")
    npr_links.new(input_aov.outputs["Color"], npr_output.inputs["Color"])
    output.nprtree = npr_tree
    return material


def material_with_unsupported_node(name, image, bl_idname):
    material = material_with_emission(name, image, (0.2, 0.2, 0.2, 1.0), 1.0)
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    output = next(node for node in nodes if node.bl_idname == "ShaderNodeOutputMaterial")
    emission = next(node for node in nodes if node.bl_idname == "ShaderNodeEmission")
    unsupported = nodes.new(bl_idname)
    if bl_idname == "ShaderNodeOutputAOV":
        links.new(emission.outputs["Emission"], unsupported.inputs["Color"])
    elif "BSDF" in unsupported.outputs:
        clear_socket_links(material.node_tree, output.inputs["Surface"])
        links.new(unsupported.outputs["BSDF"], output.inputs["Surface"])
    elif "Normal" in unsupported.outputs:
        diffuse = nodes.new("ShaderNodeBsdfDiffuse")
        clear_socket_links(material.node_tree, output.inputs["Surface"])
        links.new(unsupported.outputs["Normal"], diffuse.inputs["Normal"])
        links.new(diffuse.outputs["BSDF"], output.inputs["Surface"])
    elif "Hit Normal" in unsupported.outputs:
        diffuse = nodes.new("ShaderNodeBsdfDiffuse")
        clear_socket_links(material.node_tree, output.inputs["Surface"])
        links.new(unsupported.outputs["Hit Normal"], diffuse.inputs["Normal"])
        links.new(diffuse.outputs["BSDF"], output.inputs["Surface"])
    else:
        color_outputs = [socket for socket in unsupported.outputs if socket.type == "RGBA"]
        if color_outputs:
            clear_socket_links(material.node_tree, emission.inputs["Color"])
            links.new(color_outputs[0], emission.inputs["Color"])
        else:
            value_outputs = [socket for socket in unsupported.outputs if socket.type == "VALUE"]
            if value_outputs:
                ramp = nodes.new("ShaderNodeValToRGB")
                clear_socket_links(material.node_tree, emission.inputs["Color"])
                links.new(value_outputs[0], ramp.inputs["Fac"])
                links.new(ramp.outputs["Color"], emission.inputs["Color"])
    return material


def material_with_disconnected_unsupported_node(name, image, bl_idname, color):
    material = material_with_emission(name, image, color, 1.0)
    material.node_tree.nodes.new(bl_idname)
    return material


def create_plane(name, materials):
    mesh = bpy.data.meshes.new(name + "Mesh")
    mesh.from_pydata(
        [(-1.0, -1.0, 0.0), (1.0, -1.0, 0.0), (1.0, 1.0, 0.0), (-1.0, 1.0, 0.0)],
        [],
        [(0, 1, 2, 3)],
    )
    mesh.update()
    uv_layer = mesh.uv_layers.new(name="UVMap")
    uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
    for loop, uv in zip(uv_layer.data, uvs):
        loop.uv = uv

    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    for material in materials:
        obj.data.materials.append(material)
    obj.data.polygons[0].material_index = 0
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    return obj


def create_custom_uv_plane(name, material):
    obj = create_plane(name, [material])
    default_uv = obj.data.uv_layers["UVMap"]
    default_uv.name = "DefaultUV"
    default_uv.active = True
    default_uv.active_render = True
    bake_uv = obj.data.uv_layers.new(name="BakeUV")
    for loop, uv in zip(
        bake_uv.data,
        [(0.125, 0.125), (0.375, 0.125), (0.375, 0.375), (0.125, 0.375)],
    ):
        loop.uv = uv
    return obj


def create_two_material_plane(name, mat_left, mat_right):
    mesh = bpy.data.meshes.new(name + "Mesh")
    mesh.from_pydata(
        [
            (-2.0, -1.0, 0.0),
            (0.0, -1.0, 0.0),
            (0.0, 1.0, 0.0),
            (-2.0, 1.0, 0.0),
            (0.0, -1.0, 0.0),
            (2.0, -1.0, 0.0),
            (2.0, 1.0, 0.0),
            (0.0, 1.0, 0.0),
        ],
        [],
        [(0, 1, 2, 3), (4, 5, 6, 7)],
    )
    mesh.update()
    uv_layer = mesh.uv_layers.new(name="UVMap")
    uvs = [
        (0.0, 0.0),
        (1.0, 0.0),
        (1.0, 1.0),
        (0.0, 1.0),
        (0.0, 0.0),
        (1.0, 0.0),
        (1.0, 1.0),
        (0.0, 1.0),
    ]
    for loop, uv in zip(uv_layer.data, uvs):
        loop.uv = uv

    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    obj.data.materials.append(mat_left)
    obj.data.materials.append(mat_right)
    obj.data.polygons[0].material_index = 0
    obj.data.polygons[1].material_index = 1
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    return obj


def select_only(obj):
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj


def add_point_light(name, energy, color=(1.0, 1.0, 1.0), location=(0.0, 0.0, 3.0)):
    light_data = bpy.data.lights.new(name, "POINT")
    light_data.energy = energy
    light_data.color = color
    light_data.use_shadow = True
    light_data.shadow_soft_size = 0.1
    light = bpy.data.objects.new(name, light_data)
    bpy.context.collection.objects.link(light)
    light.location = location
    bpy.context.view_layer.update()
    return light_data


def ensure_light_nodes(light_data):
    if light_data.node_tree is None:
        light_data.use_nodes = True
    if light_data.node_tree is None:
        raise AssertionError("light data-block did not create a node tree")
    return light_data.node_tree


def find_light_shader_node(tree, bl_idname):
    active = None
    first = None
    for node in tree.nodes:
        if node.bl_idname != bl_idname:
            continue
        if first is None:
            first = node
        if getattr(node, "is_active_output", False):
            active = node
    return active or first


def clear_socket_links(tree, socket):
    for link in list(socket.links):
        tree.links.remove(link)


def add_light_shader_space_gradient_point(name):
    light_data = add_point_light(name, 1600.0, location=(0.0, 0.0, 3.0))
    light_data.use_shadow = False
    tree = ensure_light_nodes(light_data)
    nodes = tree.nodes
    links = tree.links

    info = find_light_shader_node(tree, "ShaderNodeEeveeLightShaderInfo")
    if info is None:
        info = nodes.new("ShaderNodeEeveeLightShaderInfo")
    output = find_light_shader_node(tree, "ShaderNodeEeveeLightShaderOutput")
    if output is None:
        output = nodes.new("ShaderNodeEeveeLightShaderOutput")
    output.is_active_output = True
    if hasattr(output, "range_scale"):
        output.range_scale = 1.0

    clear_socket_links(tree, output.inputs["Color"])
    clear_socket_links(tree, output.inputs["Intensity"])
    clear_socket_links(tree, output.inputs["Attenuation"])
    output.inputs["Intensity"].default_value = 2.0
    output.inputs["Attenuation"].default_value = 1.0

    separate = nodes.new("ShaderNodeSeparateXYZ")
    map_x = nodes.new("ShaderNodeMapRange")
    invert_x = nodes.new("ShaderNodeMath")
    combine = nodes.new("ShaderNodeCombineColor")

    map_x.inputs["From Min"].default_value = -1.0
    map_x.inputs["From Max"].default_value = 1.0
    map_x.inputs["To Min"].default_value = 0.0
    map_x.inputs["To Max"].default_value = 1.0
    if hasattr(map_x, "clamp"):
        map_x.clamp = True
    invert_x.operation = "SUBTRACT"
    invert_x.inputs[0].default_value = 1.0
    combine.inputs["Green"].default_value = 0.05

    links.new(info.outputs["Light Space"], separate.inputs["Vector"])
    links.new(separate.outputs["X"], map_x.inputs["Value"])
    links.new(map_x.outputs["Result"], combine.inputs["Red"])
    links.new(map_x.outputs["Result"], invert_x.inputs[1])
    links.new(invert_x.outputs["Value"], combine.inputs["Blue"])
    links.new(combine.outputs["Color"], output.inputs["Color"])
    bpy.context.view_layer.update()
    return light_data


def add_sun_light(name, energy, color=(1.0, 1.0, 1.0), rotation=(0.0, 0.0, 0.0)):
    light_data = bpy.data.lights.new(name, "SUN")
    light_data.energy = energy
    light_data.color = color
    light_data.use_shadow = True
    light = bpy.data.objects.new(name, light_data)
    bpy.context.collection.objects.link(light)
    light.rotation_euler = rotation
    bpy.context.view_layer.update()
    return light_data


def create_shadow_blocker(name):
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, 0.55))
    blocker = bpy.context.object
    blocker.name = name
    blocker.dimensions = (0.65, 0.65, 0.75)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    bpy.context.view_layer.update()
    return blocker


def create_shadow_blocker_at(name, location):
    blocker = create_shadow_blocker(name)
    blocker.location = location
    bpy.context.view_layer.update()
    return blocker


def create_uv_sphere(name, materials, location=(0.0, 0.0, 0.0)):
    bpy.ops.mesh.primitive_uv_sphere_add(
        segments=32,
        ring_count=16,
        radius=1.0,
        location=location,
    )
    obj = bpy.context.object
    obj.name = name
    for material in materials:
        obj.data.materials.append(material)
    if not obj.data.uv_layers:
        raise AssertionError(f"{name} expected a generated UV layer")
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    return obj


def bake_emit(obj, expect_success=True, bake_type="EMIT", uv_layer=""):
    select_only(obj)
    try:
        result = bpy.ops.object.bake(type=bake_type, uv_layer=uv_layer)
    except RuntimeError as ex:
        if expect_success:
            raise
        message = str(ex)
        if "Eevee Color Bake" not in message and "color attribute" not in message:
            raise AssertionError(f"Expected an Eevee Color Bake or color attribute error, got: {message}") from ex
        return {"CANCELLED"}

    success = "FINISHED" in result
    if expect_success and not success:
        raise AssertionError(f"Expected bake to finish, got {result}")
    if not expect_success and success:
        raise AssertionError(f"Expected bake to fail, got {result}")
    return result


def bake_emit_to_color_attribute(obj, expect_success=True, bake_type="EMIT"):
    scene = bpy.context.scene
    previous_target = scene.render.bake.target
    scene.render.bake.target = "VERTEX_COLORS"
    try:
        return bake_emit(obj, expect_success=expect_success, bake_type=bake_type)
    finally:
        scene.render.bake.target = previous_target


def bake_selected_emit_to_color_attribute(objects, expect_success=True, bake_type="EMIT"):
    scene = bpy.context.scene
    previous_target = scene.render.bake.target
    scene.render.bake.target = "VERTEX_COLORS"
    try:
        bpy.ops.object.select_all(action="DESELECT")
        for obj in objects:
            obj.select_set(True)
        bpy.context.view_layer.objects.active = objects[0]
        try:
            result = bpy.ops.object.bake(type=bake_type)
        except RuntimeError as ex:
            if expect_success:
                raise
            message = str(ex)
            if "Eevee Color Bake" not in message and "color attribute" not in message:
                raise AssertionError(f"Expected a color attribute bake error, got: {message}") from ex
            return {"CANCELLED"}

        success = "FINISHED" in result
        if expect_success and not success:
            raise AssertionError(f"Expected selected color attribute bake to finish, got {result}")
        if not expect_success and success:
            raise AssertionError(f"Expected selected color attribute bake to fail, got {result}")
        return result
    finally:
        scene.render.bake.target = previous_target


def pixel(image, x, y):
    index = (y * image.size[0] + x) * 4
    return tuple(image.pixels[index:index + 4])


def load_image_pixels(path):
    image = bpy.data.images.load(path, check_existing=False)
    pixels = list(image.pixels[:])
    size = tuple(image.size)
    bpy.data.images.remove(image)
    return size, pixels


def image_diff_stats(path_a, path_b):
    size_a, pixels_a = load_image_pixels(path_a)
    size_b, pixels_b = load_image_pixels(path_b)
    if size_a != size_b:
        raise AssertionError(f"Image size mismatch: {path_a}={size_a}, {path_b}={size_b}")

    changed = 0
    max_delta = 0.0
    delta_sum = 0.0
    count = len(pixels_a) // 4
    for pixel_i in range(count):
        offset = pixel_i * 4
        delta = max(
            abs(pixels_a[offset] - pixels_b[offset]),
            abs(pixels_a[offset + 1] - pixels_b[offset + 1]),
            abs(pixels_a[offset + 2] - pixels_b[offset + 2]),
        )
        max_delta = max(max_delta, delta)
        delta_sum += delta
        if delta > 0.05:
            changed += 1
    return {
        "mean_delta": delta_sum / max(count, 1),
        "max_delta": max_delta,
        "changed_px": changed,
    }


def save_image(image, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    image.filepath_raw = path
    image.file_format = "PNG"
    image.save()


def assert_rgb_close(actual, expected, label, epsilon=EPSILON):
    for channel, (a, e) in enumerate(zip(actual[:3], expected[:3])):
        if abs(a - e) > epsilon:
            raise AssertionError(
                f"{label} channel {channel} expected {expected[:3]}, got {actual[:3]}"
            )


def assert_rgb_greater(a, b, label, delta=0.05):
    if not all(a[i] > b[i] + delta for i in range(3)):
        raise AssertionError(f"{label} expected {a[:3]} to be brighter than {b[:3]}")


def assert_color_attribute_values(obj, attr_name, expected_values, label, epsilon=EPSILON):
    attr = obj.data.color_attributes[attr_name]
    actual_values = [tuple(color.color) for color in attr.data]
    if len(actual_values) != len(expected_values):
        raise AssertionError(
            f"{label} expected {len(expected_values)} colors, got {len(actual_values)}"
        )
    for index, (actual, expected) in enumerate(zip(actual_values, expected_values)):
        assert_rgb_close(actual, expected, f"{label} color {index}", epsilon)


def assert_rgb_near_black(actual, label, epsilon=0.02):
    if any(channel > epsilon for channel in actual[:3]):
        raise AssertionError(f"{label} expected near black, got {actual[:3]}")


def assert_channel_order(actual, order, label, delta=0.01):
    for first, second in zip(order, order[1:]):
        if actual[first] <= actual[second] + delta:
            raise AssertionError(f"{label} expected channel order {order}, got {actual[:3]}")


def assert_not_black(actual, label, epsilon=0.04):
    if sum(actual[:3]) <= epsilon:
        raise AssertionError(f"{label} expected a lit color, got {actual[:3]}")


def assert_luminance_greater(a, b, label, delta=0.05):
    if sum(a[:3]) <= sum(b[:3]) + delta:
        raise AssertionError(f"{label} expected {a[:3]} to be brighter than {b[:3]}")


def assert_no_shadow_darkening(center, edge, label, delta=0.05):
    if sum(center[:3]) + delta < sum(edge[:3]):
        raise AssertionError(
            f"{label} expected center to stay lit without shadow, got center={center[:3]} edge={edge[:3]}"
        )


def find_required_object(name, object_type=None):
    obj = bpy.data.objects.get(name)
    if obj is None:
        raise AssertionError(f"Required object {name!r} was not found")
    if object_type is not None and obj.type != object_type:
        raise AssertionError(f"Required object {name!r} expected type {object_type}, got {obj.type}")
    return obj


def active_output_node(node_tree):
    for node in node_tree.nodes:
        if node.bl_idname == "ShaderNodeOutputMaterial" and getattr(node, "is_active_output", True):
            return node
    for node in node_tree.nodes:
        if node.bl_idname == "ShaderNodeOutputMaterial":
            return node
    raise AssertionError("Material has no output node")


def selected_bake_image_node(obj):
    material = obj.active_material
    if material is None or material.node_tree is None:
        raise AssertionError(f"{obj.name} has no node material")
    image_node = None
    for node in material.node_tree.nodes:
        if node.bl_idname == "ShaderNodeTexImage" and node.image is not None:
            image_node = node
            break
    if image_node is None:
        raise AssertionError(f"{obj.name} material has no image texture bake target")
    for node in material.node_tree.nodes:
        node.select = False
    image_node.select = True
    material.node_tree.nodes.active = image_node
    return image_node


def set_single_image_emission_material(obj, image):
    material = bpy.data.materials.new(obj.name + "BakedImageMaterial")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()
    output = nodes.new("ShaderNodeOutputMaterial")
    tex = nodes.new("ShaderNodeTexImage")
    tex.image = image
    tex.interpolation = "Closest"
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Strength"].default_value = 1.0
    links.new(tex.outputs["Color"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    obj.active_material = material


def render_to(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    bpy.context.scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    return path


def test_emission():
    reset_scene()
    image = create_image("EmissionBake")
    color = (0.8, 0.2, 0.1, 1.0)
    obj = create_plane("EmissionPlane", [material_with_emission("EmissionMat", image, color)])
    bake_emit(obj)
    assert_rgb_close(pixel(image, 16, 16), color, "emission bake")


def test_principled_emission():
    reset_scene()
    image = create_image("PrincipledBake")
    color = (0.1, 0.65, 0.25, 1.0)
    obj = create_plane(
        "PrincipledPlane",
        [material_with_principled_emission("PrincipledMat", image, color, 1.0)],
    )
    bake_emit(obj)
    assert_rgb_close(pixel(image, 16, 16), color, "principled emission bake")


def test_multi_material_multi_image():
    reset_scene()
    image_a = create_image("MultiBakeA")
    image_b = create_image("MultiBakeB")
    color_a = (0.9, 0.05, 0.1, 1.0)
    color_b = (0.05, 0.15, 0.85, 1.0)
    mat_a = material_with_emission("MultiMatA", image_a, color_a)
    mat_b = material_with_emission("MultiMatB", image_b, color_b)
    obj = create_two_material_plane("MultiPlane", mat_a, mat_b)
    bake_emit(obj)
    assert_rgb_close(pixel(image_a, 16, 16), color_a, "multi material image A")
    assert_rgb_close(pixel(image_b, 16, 16), color_b, "multi material image B")


def test_npr_local_color():
    reset_scene()
    image = create_image("NPRBake")
    color = (0.15, 0.72, 0.33, 1.0)
    obj = create_plane("NPRPlane", [material_with_npr_color("NPRMat", image, color)])
    bake_emit(obj)
    assert_rgb_close(pixel(image, 16, 16), color, "local NPR color bake")


def test_shader_info_light_response():
    reset_scene()
    image = create_image("ShaderInfoBake")
    obj = create_plane("ShaderInfoPlane", [material_with_shader_info("ShaderInfoMat", image)])

    light_data = bpy.data.lights.new("ShaderInfoSun", "SUN")
    light_data.energy = 1.0
    light_data.color = (0.3, 0.5, 0.7)
    light = bpy.data.objects.new("ShaderInfoSun", light_data)
    bpy.context.collection.objects.link(light)
    light.rotation_euler = (0.0, 0.0, 0.0)
    bpy.context.view_layer.update()

    bake_emit(obj)
    low = pixel(image, 16, 16)

    light_data.energy = 3.0
    bpy.context.view_layer.update()
    bake_emit(obj)
    high = pixel(image, 16, 16)

    assert_luminance_greater(high, low, "Shader Info light response", delta=0.25)


def test_light_shader_node_light_space_response():
    reset_scene()
    image = create_image("LightShaderNodeBake")
    obj = create_plane(
        "LightShaderNodePlane",
        [material_with_diffuse_surface("LightShaderNodeMat", image, (1.0, 1.0, 1.0, 1.0))],
    )
    add_light_shader_space_gradient_point("LightShaderNodePoint")

    bake_emit(obj)
    left = pixel(image, 8, 16)
    right = pixel(image, 24, 16)
    assert_not_black(left, "Light Shader node left sample")
    assert_not_black(right, "Light Shader node right sample")
    if right[0] <= left[0] * 1.5 or right[0] <= left[0] + 0.01:
        raise AssertionError(
            f"Light Shader node bake expected right red > left red, left={left[:3]} right={right[:3]}"
        )
    if left[2] <= right[2] * 1.5 or left[2] <= right[2] + 0.01:
        raise AssertionError(
            f"Light Shader node bake expected left blue > right blue, left={left[:3]} right={right[:3]}"
        )


def tangent_normal_bake_sample(normal_color):
    reset_scene()
    image = create_image("TangentNormalBake")
    material = material_with_tangent_normal_shader_info(
        "TangentNormalMat", image, normal_color
    )
    obj = create_plane("TangentNormalPlane", [material])

    light_data = bpy.data.lights.new("TangentNormalPoint", "POINT")
    light_data.energy = 600.0
    light_data.color = (1.0, 1.0, 1.0)
    light_data.shadow_soft_size = 0.1
    light = bpy.data.objects.new("TangentNormalPoint", light_data)
    bpy.context.collection.objects.link(light)
    light.location = (3.0, 0.0, 2.0)
    bpy.context.view_layer.update()

    bake_emit(obj)
    return pixel(image, 16, 16)


def test_tangent_space_normal_map_light_response():
    tangent_facing_light = tangent_normal_bake_sample((1.0, 0.5, 0.5, 1.0))
    tangent_away_from_light = tangent_normal_bake_sample((0.0, 0.5, 0.5, 1.0))
    assert_luminance_greater(
        tangent_facing_light,
        tangent_away_from_light,
        "tangent-space Normal Map light response",
        0.1,
    )


def test_bsdf_no_light_is_black():
    reset_scene()
    image = create_image("BSDFNoLightBake")
    color = (0.75, 0.35, 0.12, 1.0)
    obj = create_plane(
        "BSDFNoLightPlane",
        [material_with_diffuse_surface("BSDFNoLightMat", image, color)],
    )
    bake_emit(obj)
    assert_rgb_near_black(pixel(image, 16, 16), "BSDF surface without lights")


def test_bsdf_world_ambient_response():
    reset_scene()
    image = create_image("BSDFAmbientBake")
    obj = create_plane(
        "BSDFAmbientPlane",
        [material_with_diffuse_surface("BSDFAmbientMat", image, (0.65, 0.65, 0.65, 1.0))],
    )

    set_world_color((0.08, 0.08, 0.08))
    bake_emit(obj)
    low = pixel(image, 16, 16)

    set_world_color((0.8, 0.8, 0.8))
    bake_emit(obj)
    high = pixel(image, 16, 16)

    assert_not_black(high, "BSDF surface world ambient")
    assert_luminance_greater(high, low, "BSDF surface world ambient response", 0.08)


def test_bsdf_surface_light_response():
    reset_scene()
    image = create_image("BSDFLightBake")
    obj = create_plane(
        "BSDFLightPlane",
        [material_with_diffuse_surface("BSDFLightMat", image, (0.65, 0.65, 0.65, 1.0))],
    )
    light_data = add_point_light("BSDFPoint", 60.0)

    bake_emit(obj)
    low = pixel(image, 16, 16)

    light_data.energy = 600.0
    bpy.context.view_layer.update()
    bake_emit(obj)
    high = pixel(image, 16, 16)

    assert_luminance_greater(high, low, "BSDF surface local lighting", 0.1)


def test_point_light_shadow():
    reset_scene()
    image = create_image("PointShadowBake")
    obj = create_plane(
        "PointShadowPlane",
        [material_with_diffuse_surface("PointShadowMat", image, (0.8, 0.8, 0.8, 1.0))],
    )
    create_shadow_blocker("PointShadowBlocker")
    add_point_light("PointShadowLight", 900.0, location=(0.0, 0.0, 4.0))

    for iteration in range(4):
        bake_emit(obj)
        shadowed = pixel(image, 16, 16)
        lit = pixel(image, 4, 4)
        assert_luminance_greater(
            lit, shadowed, f"point light shadow bake iteration {iteration}", 0.08
        )


def test_sun_light_shadow():
    reset_scene()
    image = create_image("SunShadowBake")
    obj = create_plane(
        "SunShadowPlane",
        [material_with_diffuse_surface("SunShadowMat", image, (0.8, 0.8, 0.8, 1.0))],
    )
    blocker = create_shadow_blocker("SunShadowBlocker")
    add_sun_light("SunShadowLight", 3.0)

    shadowed_values = []
    for iteration in range(4):
        bake_emit(obj)
        shadowed_values.append(pixel(image, 16, 16))
    blocker.visible_shadow = False
    bpy.context.view_layer.update()
    bake_emit(obj)
    lit = pixel(image, 16, 16)
    for iteration, shadowed in enumerate(shadowed_values):
        assert_luminance_greater(
            lit, shadowed, f"sun light shadow bake iteration {iteration}", 0.08
        )


def test_sun_light_shadow_without_scene_camera():
    reset_scene()
    image = create_image("SunNoCameraShadowBake")
    obj = create_plane(
        "SunNoCameraShadowPlane",
        [material_with_diffuse_surface("SunNoCameraShadowMat", image, (0.8, 0.8, 0.8, 1.0))],
    )
    blocker = create_shadow_blocker("SunNoCameraShadowBlocker")
    add_sun_light("SunNoCameraShadowLight", 3.0)
    bpy.context.scene.camera = None
    bake_emit(obj)
    shadowed = pixel(image, 16, 16)
    blocker.visible_shadow = False
    bpy.context.view_layer.update()
    bake_emit(obj)
    lit = pixel(image, 16, 16)
    assert_luminance_greater(lit, shadowed, "sun shadow bake without scene camera", 0.08)


def test_translated_shader_info_sun_shadow():
    reset_scene()
    image = create_image("TranslatedShaderInfoSunShadowBake")
    obj = create_uv_sphere(
        "TranslatedShaderInfoSunShadowSphere",
        [material_with_shader_info_shadow("TranslatedShaderInfoSunShadowMat", image)],
        location=(0.35, -0.45, 0.25),
    )
    blocker = create_shadow_blocker_at("TranslatedShaderInfoSunShadowBlocker", (0.35, -0.45, 1.9))
    blocker.dimensions = (0.65, 0.65, 0.75)
    bpy.context.view_layer.objects.active = blocker
    blocker.select_set(True)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    add_sun_light("TranslatedShaderInfoSunShadowLight", 3.0)
    bpy.context.view_layer.update()

    bake_emit(obj)
    covered = []
    for y in range(image.size[1]):
        for x in range(image.size[0]):
            sample = pixel(image, x, y)
            if sample[3] > 0.01:
                covered.append(sum(sample[:3]) / 3.0)

    if not covered:
        raise AssertionError("translated Shader Info sun shadow bake produced no covered pixels")
    dark_count = sum(1 for value in covered if value < 0.25)
    light_count = sum(1 for value in covered if value > 0.75)
    if dark_count < len(covered) * 0.10 or light_count < len(covered) * 0.05:
        raise AssertionError(
            "translated Shader Info sun shadow expected both shadowed and lit texels, "
            f"got dark={dark_count} light={light_count} covered={len(covered)}"
        )


def test_shader_info_projected_shadow_changes_bake():
    blend_path = os.path.join(ASSET_DIR, "shader_info_shadow_reference.blend")
    reference_path = os.path.join(ASSET_DIR, "shader_info_shadow_reference.png")
    if not os.path.exists(blend_path):
        raise AssertionError(f"Missing projected shadow test blend: {blend_path}")
    if not os.path.exists(reference_path):
        raise AssertionError(f"Missing projected shadow reference image: {reference_path}")

    bpy.ops.wm.open_mainfile(filepath=blend_path)
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    ref_size, _ = load_image_pixels(reference_path)
    scene.render.resolution_x = ref_size[0]
    scene.render.resolution_y = ref_size[1]
    scene.render.resolution_percentage = 100
    scene.render.bake.margin = 2

    obj = find_required_object("Sphere", "MESH")
    cube = find_required_object("Cube", "MESH")
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj

    image_node = selected_bake_image_node(obj)
    image = image_node.image

    case_out_dir = os.path.join(OUT_DIR, "projected_shadow_reference")
    direct_path = render_to(os.path.join(case_out_dir, "direct.png"))

    image.pixels.foreach_set([1.0, 1.0, 1.0, 1.0] * (image.size[0] * image.size[1]))
    image.update()
    bake_emit(obj)
    with_cube_path = os.path.join(case_out_dir, "with_cube_raw.png")
    save_image(image, with_cube_path)

    cube.visible_shadow = False
    bpy.context.view_layer.update()
    bake_emit(obj)
    without_cube_path = os.path.join(case_out_dir, "without_cube_raw.png")
    save_image(image, without_cube_path)
    cube.visible_shadow = True
    bpy.context.view_layer.update()

    with_without = image_diff_stats(with_cube_path, without_cube_path)
    if with_without["mean_delta"] < 0.05 or with_without["changed_px"] < 50000:
        raise AssertionError(
            "Shader Info projected shadow bake did not react to the external caster: "
            f"{with_without}"
        )

    image.pixels.foreach_set([1.0, 1.0, 1.0, 1.0] * (image.size[0] * image.size[1]))
    image.update()
    bake_emit(obj)
    set_single_image_emission_material(obj, image)
    baked_render_path = render_to(os.path.join(case_out_dir, "baked_render.png"))

    direct_vs_reference = image_diff_stats(direct_path, reference_path)
    baked_vs_reference = image_diff_stats(baked_render_path, reference_path)
    if direct_vs_reference["mean_delta"] > 0.005:
        raise AssertionError(
            f"Projected shadow reference scene no longer matches direct Eevee render: "
            f"{direct_vs_reference}"
        )
    if baked_vs_reference["mean_delta"] > 0.03:
        raise AssertionError(
            "Projected shadow bake connected back to material differs from the reference too much: "
            f"{baked_vs_reference}"
        )


def test_shadow_disabled():
    reset_scene()
    image = create_image("ShadowDisabledBake")
    obj = create_plane(
        "ShadowDisabledPlane",
        [material_with_diffuse_surface("ShadowDisabledMat", image, (0.8, 0.8, 0.8, 1.0))],
    )
    create_shadow_blocker("ShadowDisabledBlocker")
    light_data = add_point_light("ShadowDisabledLight", 900.0, location=(0.0, 0.0, 4.0))
    light_data.use_shadow = False
    bpy.context.view_layer.update()

    bake_emit(obj)
    center = pixel(image, 16, 16)
    edge = pixel(image, 4, 4)
    assert_no_shadow_darkening(center, edge, "disabled shadow color bake")


def test_shader_to_rgb_light_response():
    reset_scene()
    image = create_image("ShaderToRGBBake")
    obj = create_plane(
        "ShaderToRGBPlane",
        [material_with_shader_to_rgb("ShaderToRGBMat", image, (0.18, 0.72, 0.38, 1.0))],
    )
    light_data = add_point_light("ShaderToRGBPoint", 60.0)

    bake_emit(obj)
    low = pixel(image, 16, 16)

    light_data.energy = 600.0
    bpy.context.view_layer.update()
    bake_emit(obj)
    high = pixel(image, 16, 16)

    assert_luminance_greater(high, low, "Shader to RGB light response", 0.08)
    assert_channel_order(high, (1, 2, 0), "Shader to RGB ramp color")


def test_image_texture_base_color():
    reset_scene()
    image = create_image("ImageTextureBake")
    color = (0.23, 0.61, 0.88, 1.0)
    obj = create_plane(
        "ImageTexturePlane",
        [material_with_image_texture("ImageTextureMat", image, color)],
    )
    add_point_light("ImageTexturePoint", 600.0)
    bake_emit(obj)
    baked = pixel(image, 16, 16)
    assert_not_black(baked, "image texture shaded color")
    assert_channel_order(baked, (2, 1, 0), "image texture shaded channel order")


def test_checker_uv_mapping_color_ramp():
    reset_scene()
    image = create_image("CheckerBake")
    color_a = (0.07, 0.72, 0.18, 1.0)
    color_b = (0.86, 0.19, 0.55, 1.0)
    obj = create_plane(
        "CheckerPlane",
        [material_with_checker_ramp("CheckerMat", image, color_a, color_b)],
    )
    bake_emit(obj)
    assert_rgb_close(pixel(image, 8, 8), color_a, "checker low-low tile")
    assert_rgb_close(pixel(image, 24, 8), color_b, "checker high-low tile")


def test_node_group_base_color():
    reset_scene()
    image = create_image("GroupBake")
    color = (0.31, 0.26, 0.91, 1.0)
    obj = create_plane("GroupPlane", [material_with_group_color("GroupMat", image, color)])
    bake_emit(obj)
    assert_rgb_close(pixel(image, 16, 16), color, "node group base color bake")


def test_custom_uv_layer_target():
    reset_scene()
    image = create_image("CustomUVBake")
    color = (0.81, 0.17, 0.42, 1.0)
    material = material_with_emission("CustomUVMat", image, color)
    obj = create_custom_uv_plane("CustomUVPlane", material)
    bake_emit(obj, uv_layer="BakeUV")
    assert_rgb_close(pixel(image, 8, 8), color, "custom uv layer target bake")
    assert_rgb_close(pixel(image, 16, 16), (0.0, 0.0, 0.0, 0.0), "custom uv layer leaves default center empty")


def test_attribute_face_domain_color():
    reset_scene()
    image = create_image("AttributeFaceBake")
    attr_name = "face_color"
    color = (0.18, 0.74, 0.41, 1.0)
    obj = create_plane(
        "AttributeFacePlane",
        [material_with_attribute_color("AttributeFaceMat", image, attr_name)],
    )
    attr = obj.data.color_attributes.new(name=attr_name, type="FLOAT_COLOR", domain="FACE")
    attr.data[0].color = color
    bake_emit(obj)
    assert_rgb_close(pixel(image, 16, 16), color, "face-domain attribute color bake")


def test_active_color_attribute_corner_target():
    reset_scene()
    attr_name = "CornerBakeColor"
    color = (0.68, 0.24, 0.09, 1.0)
    obj = create_plane(
        "ActiveCornerColorPlane",
        [material_with_emission("ActiveCornerColorMat", None, color)],
    )
    attr = obj.data.color_attributes.new(name=attr_name, type="FLOAT_COLOR", domain="CORNER")
    obj.data.color_attributes.active_color_name = attr_name
    bake_emit_to_color_attribute(obj)
    assert_color_attribute_values(
        obj, attr_name, [color] * len(attr.data), "active corner color attribute bake"
    )


def test_active_color_attribute_point_target_average():
    reset_scene()
    attr_name = "point_source_color"
    target_name = "PointBakeColor"
    color_a = (0.82, 0.12, 0.05, 1.0)
    color_b = (0.06, 0.21, 0.77, 1.0)
    obj = create_two_material_plane(
        "ActivePointColorPlane",
        material_with_attribute_color("ActivePointColorMatA", None, attr_name),
        material_with_attribute_color("ActivePointColorMatB", None, attr_name),
    )
    source_attr = obj.data.color_attributes.new(name=attr_name, type="FLOAT_COLOR", domain="CORNER")
    for loop_index, color in enumerate([color_a] * 4 + [color_b] * 4):
        source_attr.data[loop_index].color = color
    obj.data.color_attributes.new(name=target_name, type="FLOAT_COLOR", domain="POINT")
    obj.data.color_attributes.active_color_name = target_name

    bake_emit_to_color_attribute(obj)

    expected = [
        color_a,
        color_a,
        color_a,
        color_a,
        color_b,
        color_b,
        color_b,
        color_b,
    ]
    assert_color_attribute_values(obj, target_name, expected, "active point color attribute bake")


def test_active_color_attribute_byte_corner_target():
    reset_scene()
    attr_name = "ByteCornerBakeColor"
    color = (0.91, 0.18, 0.39, 1.0)
    obj = create_plane(
        "ActiveByteCornerColorPlane",
        [material_with_emission("ActiveByteCornerColorMat", None, color)],
    )
    attr = obj.data.color_attributes.new(name=attr_name, type="BYTE_COLOR", domain="CORNER")
    obj.data.color_attributes.active_color_name = attr_name
    bake_emit_to_color_attribute(obj)
    assert_color_attribute_values(
        obj, attr_name, [color] * len(attr.data), "active byte corner color attribute bake", 0.02
    )


def test_active_color_attribute_multi_object_target():
    reset_scene()
    attr_name = "MultiObjectBakeColor"
    color_a = (0.17, 0.62, 0.83, 1.0)
    color_b = (0.86, 0.31, 0.13, 1.0)
    obj_a = create_plane(
        "ActiveColorMultiA",
        [material_with_emission("ActiveColorMultiMatA", None, color_a)],
    )
    attr_a = obj_a.data.color_attributes.new(name=attr_name, type="FLOAT_COLOR", domain="CORNER")
    obj_a.data.color_attributes.active_color_name = attr_name

    obj_b = create_plane(
        "ActiveColorMultiB",
        [material_with_emission("ActiveColorMultiMatB", None, color_b)],
    )
    obj_b.location.x = 3.0
    attr_b = obj_b.data.color_attributes.new(name=attr_name, type="FLOAT_COLOR", domain="CORNER")
    obj_b.data.color_attributes.active_color_name = attr_name

    bake_selected_emit_to_color_attribute([obj_a, obj_b])
    assert_color_attribute_values(
        obj_a, attr_name, [color_a] * len(attr_a.data), "multi-object active color attribute A"
    )
    assert_color_attribute_values(
        obj_b, attr_name, [color_b] * len(attr_b.data), "multi-object active color attribute B"
    )


def test_active_color_attribute_face_target_rejected():
    reset_scene()
    attr_name = "FaceTargetColor"
    obj = create_plane(
        "ActiveFaceColorTargetPlane",
        [material_with_emission("ActiveFaceColorTargetMat", None, (0.2, 0.6, 0.9, 1.0))],
    )
    obj.data.color_attributes.new(name=attr_name, type="FLOAT_COLOR", domain="FACE")
    obj.data.color_attributes.active_color_name = attr_name
    bake_emit_to_color_attribute(obj, expect_success=False)


def test_generated_coordinate_color():
    reset_scene()
    image = create_image("GeneratedCoordBake")
    obj = create_plane(
        "GeneratedCoordPlane",
        [material_with_generated_coordinate("GeneratedCoordMat", image)],
    )
    bake_emit(obj)
    left = pixel(image, 2, 16)
    right = pixel(image, 30, 16)
    if right[0] <= left[0] + 0.35:
        raise AssertionError(f"Generated coordinate bake expected X gradient, left={left[:3]} right={right[:3]}")


def test_disconnected_unsupported_nodes_do_not_block_bake():
    reset_scene()
    image = create_image("DisconnectedUnsupportedBake")
    color = (0.42, 0.66, 0.19, 1.0)
    obj = create_plane(
        "DisconnectedUnsupportedPlane",
        [material_with_disconnected_unsupported_node(
            "DisconnectedUnsupportedMat", image, "ShaderNodeSceneColor", color
        )],
    )
    bake_emit(obj)
    assert_rgb_close(pixel(image, 16, 16), color, "disconnected unsupported node bake")


def test_group_unused_unsupported_output_does_not_block_bake():
    reset_scene()
    image = create_image("GroupUnusedUnsupportedBake")
    color = (0.27, 0.34, 0.87, 1.0)
    obj = create_plane(
        "GroupUnusedUnsupportedPlane",
        [material_with_group_unconnected_unsupported("GroupUnusedUnsupportedMat", image, color)],
    )
    bake_emit(obj)
    assert_rgb_close(pixel(image, 16, 16), color, "group unused unsupported output bake")


def test_glsl_function_color():
    reset_scene()
    image = create_image("GLSLBake")
    input_color = (0.2, 0.7, 0.4, 1.0)
    expected = (0.8, 0.3, 0.6, 1.0)
    material, expected = material_with_glsl_function(
        "GLSLMat", image, input_color, expected
    )
    obj = create_plane("GLSLPlane", [material])
    bake_emit(obj)
    assert_rgb_close(pixel(image, 16, 16), expected, "GLSL Function color bake")


def test_unsupported_pass_type():
    reset_scene()
    image = create_image("UnsupportedTypeBake", color=(0.0, 0.0, 0.0, 1.0))
    obj = create_plane(
        "UnsupportedTypePlane",
        [material_with_emission("UnsupportedTypeMat", image, (0.9, 0.9, 0.1, 1.0))],
    )
    bake_emit(obj, expect_success=False, bake_type="DIFFUSE")
    assert_rgb_close(pixel(image, 16, 16), (0.0, 0.0, 0.0, 1.0), "unsupported type image")


def test_unsupported_npr_input():
    reset_scene()
    image = create_image("UnsupportedNPRBake", color=(0.0, 0.0, 0.0, 1.0))
    obj = create_plane("UnsupportedNPRPlane", [material_with_npr_input("UnsupportedNPRMat", image)])
    bake_emit(obj, expect_success=False)
    assert_rgb_close(pixel(image, 16, 16), (0.0, 0.0, 0.0, 1.0), "unsupported NPR input image")


def test_unsupported_node(kind, bl_idname):
    reset_scene()
    image = create_image("UnsupportedNodeBake", color=(0.0, 0.0, 0.0, 1.0))
    if bl_idname == "ShaderNodeNPR_Refraction":
        material = material_with_npr_refraction("UnsupportedNodeMat", image)
    elif bl_idname == "ShaderNodeInputAOV":
        material = material_with_npr_input_aov("UnsupportedNodeMat", image)
    else:
        material = material_with_unsupported_node("UnsupportedNodeMat", image, bl_idname)
    obj = create_plane(
        "UnsupportedNodePlane",
        [material],
    )
    bake_emit(obj, expect_success=False)
    assert_rgb_close(pixel(image, 16, 16), (0.0, 0.0, 0.0, 1.0), f"unsupported {kind} image")


def test_no_active_image_target_warning():
    reset_scene()
    obj = create_plane(
        "NoBakeTargetPlane",
        [material_without_bake_target("NoBakeTargetMat", (0.8, 0.8, 0.8, 1.0))],
    )
    select_only(obj)
    result = bpy.ops.object.bake(type="EMIT")
    if "CANCELLED" not in result:
        raise AssertionError(f"Expected bake without an image target to cancel, got {result}")


def run_negative_case(kind):
    if kind == "pass_type":
        test_unsupported_pass_type()
        print("EEVEE_COLOR_BAKE_NEGATIVE_PASS_TYPE_OK=1")
    elif kind == "npr_input":
        test_unsupported_npr_input()
        print("EEVEE_COLOR_BAKE_NEGATIVE_NPR_INPUT_OK=1")
    elif kind == "screen_space_info":
        test_unsupported_node(kind, "ShaderNodeScreenspaceInfo")
        print("EEVEE_COLOR_BAKE_NEGATIVE_SCREEN_SPACE_INFO_OK=1")
    elif kind == "render_texture":
        test_unsupported_node(kind, "ShaderNodeRenderTexture")
        print("EEVEE_COLOR_BAKE_NEGATIVE_RENDER_TEXTURE_OK=1")
    elif kind == "npr_refraction":
        test_unsupported_node(kind, "ShaderNodeNPR_Refraction")
        print("EEVEE_COLOR_BAKE_NEGATIVE_NPR_REFRACTION_OK=1")
    elif kind == "input_aov":
        test_unsupported_node(kind, "ShaderNodeInputAOV")
        print("EEVEE_COLOR_BAKE_NEGATIVE_INPUT_AOV_OK=1")
    elif kind == "output_aov":
        test_unsupported_node(kind, "ShaderNodeOutputAOV")
        print("EEVEE_COLOR_BAKE_NEGATIVE_OUTPUT_AOV_OK=1")
    elif kind == "output_filter":
        test_unsupported_node(kind, "ShaderNodeOutputFilter")
        print("EEVEE_COLOR_BAKE_NEGATIVE_OUTPUT_FILTER_OK=1")
    elif kind == "ambient_occlusion":
        test_unsupported_node(kind, "ShaderNodeAmbientOcclusion")
        print("EEVEE_COLOR_BAKE_NEGATIVE_AMBIENT_OCCLUSION_OK=1")
    elif kind == "bevel":
        test_unsupported_node(kind, "ShaderNodeBevel")
        print("EEVEE_COLOR_BAKE_NEGATIVE_BEVEL_OK=1")
    elif kind == "curvature":
        test_unsupported_node(kind, "ShaderNodeCurvature")
        print("EEVEE_COLOR_BAKE_NEGATIVE_CURVATURE_OK=1")
    elif kind == "raycast":
        test_unsupported_node(kind, "ShaderNodeRaycast")
        print("EEVEE_COLOR_BAKE_NEGATIVE_RAYCAST_OK=1")
    elif kind == "transparent_bsdf":
        test_unsupported_node(kind, "ShaderNodeBsdfTransparent")
        print("EEVEE_COLOR_BAKE_NEGATIVE_TRANSPARENT_BSDF_OK=1")
    elif kind == "ray_portal_bsdf":
        test_unsupported_node(kind, "ShaderNodeBsdfRayPortal")
        print("EEVEE_COLOR_BAKE_NEGATIVE_RAY_PORTAL_BSDF_OK=1")
    elif kind == "no_active_image":
        test_no_active_image_target_warning()
        print("EEVEE_COLOR_BAKE_NEGATIVE_NO_ACTIVE_IMAGE_OK=1")
    elif kind == "active_color_face_target":
        test_active_color_attribute_face_target_rejected()
        print("EEVEE_COLOR_BAKE_NEGATIVE_ACTIVE_COLOR_FACE_TARGET_OK=1")
    else:
        raise AssertionError(f"Unknown negative Eevee color bake case: {kind}")


def run_negative_blender(kind, expected_error, expected_marker):
    out_dir = os.path.join(os.path.dirname(__file__), "out")
    os.makedirs(out_dir, exist_ok=True)
    script_path = os.path.join(out_dir, f"negative_{kind}.py")
    with open(script_path, "w", encoding="utf-8") as handle:
        handle.write(
            "import importlib.util\n"
            "import sys\n"
            "sys.dont_write_bytecode = True\n"
            f"spec = importlib.util.spec_from_file_location('eevee_color_bake_case', {__file__!r})\n"
            "module = importlib.util.module_from_spec(spec)\n"
            "spec.loader.exec_module(module)\n"
            f"module.run_negative_case({kind!r})\n"
        )

    try:
        result = subprocess.run(
            [bpy.app.binary_path, "--background", "--factory-startup", "--python", script_path],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    finally:
        try:
            os.remove(script_path)
        except OSError:
            pass

    output = result.stdout
    if expected_marker not in output or expected_error not in output:
        raise AssertionError(
            f"Negative bake case {kind} did not report the expected failure.\n"
            f"returncode={result.returncode}\n"
            f"expected_marker={expected_marker!r}\n"
            f"expected_error={expected_error!r}\n"
            f"output:\n{output}"
        )


def test_unsupported_pass_type_subprocess():
    run_negative_blender(
        "pass_type",
        "Eevee Color Bake only supports the Emit bake type",
        "EEVEE_COLOR_BAKE_NEGATIVE_PASS_TYPE_OK=1",
    )


def test_unsupported_npr_input_subprocess():
    run_negative_blender(
        "npr_input",
        "Eevee Color Bake does not support NPR Input screen/GBuffer reads",
        "EEVEE_COLOR_BAKE_NEGATIVE_NPR_INPUT_OK=1",
    )


def test_unsupported_screen_nodes_subprocess():
    cases = [
        ("screen_space_info", "Screen Space Info", "EEVEE_COLOR_BAKE_NEGATIVE_SCREEN_SPACE_INFO_OK=1"),
        ("render_texture", "Render Texture feedback", "EEVEE_COLOR_BAKE_NEGATIVE_RENDER_TEXTURE_OK=1"),
        ("npr_refraction", "NPR Refraction", "EEVEE_COLOR_BAKE_NEGATIVE_NPR_REFRACTION_OK=1"),
        ("input_aov", "Input AOV", "EEVEE_COLOR_BAKE_NEGATIVE_INPUT_AOV_OK=1"),
        ("output_aov", "Output AOV", "EEVEE_COLOR_BAKE_NEGATIVE_OUTPUT_AOV_OK=1"),
        ("output_filter", "Filter-domain output", "EEVEE_COLOR_BAKE_NEGATIVE_OUTPUT_FILTER_OK=1"),
        ("ambient_occlusion", "Ambient Occlusion screen-space sampling", "EEVEE_COLOR_BAKE_NEGATIVE_AMBIENT_OCCLUSION_OK=1"),
        ("bevel", "Bevel screen-space raycast", "EEVEE_COLOR_BAKE_NEGATIVE_BEVEL_OK=1"),
        ("curvature", "Curvature screen-space depth sampling", "EEVEE_COLOR_BAKE_NEGATIVE_CURVATURE_OK=1"),
        ("raycast", "Raycast screen-space tracing", "EEVEE_COLOR_BAKE_NEGATIVE_RAYCAST_OK=1"),
        ("transparent_bsdf", "Transparent BSDF layering", "EEVEE_COLOR_BAKE_NEGATIVE_TRANSPARENT_BSDF_OK=1"),
        ("ray_portal_bsdf", "Ray Portal BSDF", "EEVEE_COLOR_BAKE_NEGATIVE_RAY_PORTAL_BSDF_OK=1"),
    ]
    for kind, expected_error, marker in cases:
        run_negative_blender(kind, expected_error, marker)


def test_no_active_image_target_warning_subprocess():
    run_negative_blender(
        "no_active_image",
        "Warning: No active and selected image texture node found",
        "EEVEE_COLOR_BAKE_NEGATIVE_NO_ACTIVE_IMAGE_OK=1",
    )


def test_active_color_attribute_face_target_rejected_subprocess():
    run_negative_blender(
        "active_color_face_target",
        "Active color attribute bake target must be on the Point or Corner domain",
        "EEVEE_COLOR_BAKE_NEGATIVE_ACTIVE_COLOR_FACE_TARGET_OK=1",
    )


def main():
    tests = [
        test_emission,
        test_principled_emission,
        test_multi_material_multi_image,
        test_bsdf_no_light_is_black,
        test_bsdf_world_ambient_response,
        test_bsdf_surface_light_response,
        test_point_light_shadow,
        test_sun_light_shadow,
        test_sun_light_shadow_without_scene_camera,
        test_translated_shader_info_sun_shadow,
        test_shader_info_projected_shadow_changes_bake,
        test_shadow_disabled,
        test_shader_to_rgb_light_response,
        test_image_texture_base_color,
        test_checker_uv_mapping_color_ramp,
        test_node_group_base_color,
        test_custom_uv_layer_target,
        test_attribute_face_domain_color,
        test_active_color_attribute_corner_target,
        test_active_color_attribute_point_target_average,
        test_active_color_attribute_byte_corner_target,
        test_active_color_attribute_multi_object_target,
        test_generated_coordinate_color,
        test_disconnected_unsupported_nodes_do_not_block_bake,
        test_group_unused_unsupported_output_does_not_block_bake,
        test_glsl_function_color,
        test_npr_local_color,
        test_shader_info_light_response,
        test_light_shader_node_light_space_response,
        test_tangent_space_normal_map_light_response,
        test_unsupported_pass_type_subprocess,
        test_unsupported_npr_input_subprocess,
        test_unsupported_screen_nodes_subprocess,
        test_no_active_image_target_warning_subprocess,
        test_active_color_attribute_face_target_rejected_subprocess,
    ]
    for test in tests:
        test()
        print(f"EEVEE_COLOR_BAKE_{test.__name__.upper()}=OK")
    print("EEVEE_COLOR_BAKE_OK=1")


if __name__ == "__main__":
    main()
