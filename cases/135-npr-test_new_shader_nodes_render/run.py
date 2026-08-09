import bpy
import os
import tempfile


RESOLUTION = 128


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def configure_scene():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = RESOLUTION
    scene.render.resolution_y = RESOLUTION
    scene.render.resolution_percentage = 100
    scene.eevee.taa_samples = 1
    scene.eevee.taa_render_samples = 1
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    scene.world.use_nodes = False
    scene.world.color = (0.0, 0.0, 0.0)


def make_camera():
    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 2.0
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 4.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera
    return camera


def make_plane(material):
    bpy.ops.mesh.primitive_plane_add(size=2.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.active_object
    plane.data.materials.append(material)
    return plane


def render_image():
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
        pixels = list(image.pixels[:])
        width = image.size[0]
        height = image.size[1]
    finally:
        bpy.data.images.remove(image)
        if os.path.exists(filepath):
            os.remove(filepath)

    return pixels, width, height


def sample_pixel(pixels, width, height, x_ratio, y_ratio):
    x = min(width - 1, max(0, int(width * x_ratio)))
    y = min(height - 1, max(0, int(height * y_ratio)))
    pixel_index = (y * width + x) * 4
    return list(pixels[pixel_index:pixel_index + 4])


def image_min_max(pixels):
    rgb = pixels[0::4] + pixels[1::4] + pixels[2::4]
    return min(rgb), max(rgb)


def mean_abs_diff(pixels_a, pixels_b):
    assert len(pixels_a) == len(pixels_b)
    total = 0.0
    for a, b in zip(pixels_a, pixels_b):
        total += abs(a - b)
    return total / len(pixels_a)


def active_enum_identifier(node, property_name, *contains_values):
    prop = node.bl_rna.properties[property_name]
    for item in prop.enum_items:
        for contains in contains_values:
            if contains.lower() in item.identifier.lower():
                return item.identifier
    raise AssertionError(
        f"{node.bl_idname}.{property_name} is missing enum containing any of {contains_values!r}"
    )


def make_scene_time_material():
    material = bpy.data.materials.new("SceneTime")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    scene_time = nodes.new("GeometryNodeInputSceneTime")
    scene_time.inputs["Scale"].default_value = 10.0

    links.new(scene_time.outputs["Scaled Frame"], emission.inputs["Strength"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_render_texture_none_material():
    material = bpy.data.materials.new("RenderTextureNone")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    render_texture = nodes.new("ShaderNodeRenderTexture")

    links.new(render_texture.outputs["Color"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_render_texture_capture_material():
    material = bpy.data.materials.new("RenderTextureCapture")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    diffuse = nodes.new("ShaderNodeBsdfDiffuse")
    emission = nodes.new("ShaderNodeEmission")
    add_shader = nodes.new("ShaderNodeAddShader")
    diffuse.inputs["Color"].default_value = (1.0, 0.0, 0.0, 1.0)
    emission.inputs["Color"].default_value = (1.0, 0.0, 0.0, 1.0)

    links.new(diffuse.outputs["BSDF"], add_shader.inputs[0])
    links.new(emission.outputs["Emission"], add_shader.inputs[1])
    links.new(add_shader.outputs["Shader"], output.inputs["Surface"])
    return material


def make_render_texture_active_material(render_texture_uid):
    material = bpy.data.materials.new("RenderTextureActive")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    render_texture = nodes.new("ShaderNodeRenderTexture")
    sample_vector = nodes.new("ShaderNodeCombineXYZ")
    sample_vector.inputs["X"].default_value = 0.5
    sample_vector.inputs["Y"].default_value = 0.5
    render_texture.render_texture = f"RENDER_TEXTURE_{render_texture_uid}"

    links.new(sample_vector.outputs["Vector"], render_texture.inputs["Vector"])
    links.new(render_texture.outputs["Color"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_portal_material():
    material = bpy.data.materials.new("PortalRoundTrip")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    rgb = nodes.new("ShaderNodeRGB")
    rgb.outputs["Color"].default_value = (0.8, 0.1, 0.1, 1.0)
    portal_in = nodes.new("ShaderNodePortalIn")
    portal_out = nodes.new("ShaderNodePortalOut")

    color_identifier = active_enum_identifier(portal_in, "data_type", "RGBA", "COLOR")
    portal_in.data_type = color_identifier
    portal_out.data_type = color_identifier
    portal_in.portal_name = "PortalSmoke"
    portal_out.portal_name = "PortalSmoke"

    portal_in_input = next(socket for socket in portal_in.inputs if socket.enabled)
    portal_out_output = next(socket for socket in portal_out.outputs if socket.enabled)

    links.new(rgb.outputs["Color"], portal_in_input)
    links.new(portal_out_output, emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_twirl_material(amount):
    material = bpy.data.materials.new(f"Twirl_{amount}")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    texcoord = nodes.new("ShaderNodeTexCoord")
    twirl = nodes.new("ShaderNodeTwirl")
    separate = nodes.new("ShaderNodeSeparateXYZ")
    combine = nodes.new("ShaderNodeCombineColor")

    twirl.inputs["Amount"].default_value = amount

    links.new(texcoord.outputs["Generated"], twirl.inputs["Vector"])
    links.new(twirl.outputs["Vector"], separate.inputs["Vector"])
    links.new(separate.outputs["X"], combine.inputs["Red"])
    links.new(separate.outputs["Y"], combine.inputs["Green"])
    links.new(combine.outputs["Color"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_hex_material():
    material = bpy.data.materials.new("HexGrid")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    texcoord = nodes.new("ShaderNodeTexCoord")
    hexagon = nodes.new("ShaderNodeTexHexagon")

    links.new(texcoord.outputs["Generated"], hexagon.inputs["Vector"])
    links.new(hexagon.outputs["Color"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def assert_scene_time():
    clear_scene()
    configure_scene()
    make_camera()
    make_plane(make_scene_time_material())
    bpy.context.scene.frame_set(20)
    pixels, width, height = render_image()
    center = sample_pixel(pixels, width, height, 0.5, 0.5)
    assert 1.5 < center[0] < 2.5, f"Scaled Frame should be near 2.0 at frame 20 with scale 10, got {center}"


def assert_render_texture_none():
    clear_scene()
    configure_scene()
    make_camera()
    make_plane(make_render_texture_none_material())
    pixels, width, height = render_image()
    center = sample_pixel(pixels, width, height, 0.5, 0.5)
    assert max(center[:3]) < 0.01, f"Unbound Render Texture should stay black, got {center}"


def assert_render_texture_active_sources():
    clear_scene()
    configure_scene()
    make_camera()

    capture_camera_data = bpy.data.cameras.new("RenderTextureCaptureCamera")
    capture_camera_data.type = "ORTHO"
    capture_camera_data.ortho_scale = 2.0
    capture_camera = bpy.data.objects.new("RenderTextureCaptureCamera", capture_camera_data)
    capture_camera.location = (3.0, 0.0, 4.0)
    bpy.context.scene.collection.objects.link(capture_camera)

    render_texture = bpy.context.scene.eevee.render_textures.add()
    render_texture.name = "RenderTextureActiveCase"
    render_texture.enabled = True
    render_texture.camera = capture_camera
    render_texture.source = "COLOR"
    render_texture.resolution_x = 64
    render_texture.resolution_y = 64
    render_texture.update_mode = "EVERY_FRAME"
    render_texture.format = "RGBA16F"

    bpy.ops.mesh.primitive_plane_add(size=1.5, location=(3.0, 0.0, 0.0))
    capture_plane = bpy.context.active_object
    capture_plane.name = "RenderTextureCapturePlane"
    capture_plane.data.materials.append(make_render_texture_capture_material())

    make_plane(make_render_texture_active_material(render_texture.uid))

    pixels, width, height = render_image()
    color_center = sample_pixel(pixels, width, height, 0.5, 0.5)
    assert color_center[0] > 0.8 and color_center[1] < 0.1 and color_center[2] < 0.1, (
        f"Active Render Texture color should capture the red plane, got {color_center}"
    )

    render_texture.source = "NORMAL"
    pixels, width, height = render_image()
    normal_center = sample_pixel(pixels, width, height, 0.5, 0.5)
    assert abs(normal_center[0] - 0.5) < 0.1 and abs(normal_center[1] - 0.5) < 0.1, (
        f"Active Render Texture normal XY should encode zero as 0.5, got {normal_center}"
    )
    assert normal_center[2] > 0.9, (
        f"Active Render Texture normal Z should encode the front-facing normal, got {normal_center}"
    )
    print("ACTIVE_RENDER_TEXTURE_COLOR_NORMAL_OK=1")


def assert_portal_round_trip():
    clear_scene()
    configure_scene()
    make_camera()
    make_plane(make_portal_material())
    pixels, width, height = render_image()
    center = sample_pixel(pixels, width, height, 0.5, 0.5)
    assert center[0] > 0.7 and center[1] < 0.2 and center[2] < 0.2, (
        f"Portal Out should forward the Portal In color, got {center}"
    )


def assert_twirl_changes_image():
    clear_scene()
    configure_scene()
    make_camera()
    make_plane(make_twirl_material(0.0))
    pixels_a, _, _ = render_image()

    clear_scene()
    configure_scene()
    make_camera()
    make_plane(make_twirl_material(8.0))
    pixels_b, _, _ = render_image()

    difference = mean_abs_diff(pixels_a, pixels_b)
    assert difference > 0.01, f"Twirl amount should materially change the image, got mean abs diff {difference}"


def assert_hex_grid_variation():
    clear_scene()
    configure_scene()
    make_camera()
    make_plane(make_hex_material())
    pixels, _, _ = render_image()
    min_value, max_value = image_min_max(pixels)
    assert max_value - min_value > 0.1, (
        f"Hex Grid Texture should create visible color variation, got min={min_value} max={max_value}"
    )


assert_scene_time()
assert_render_texture_none()
assert_portal_round_trip()
assert_twirl_changes_image()
assert_hex_grid_variation()
assert_render_texture_active_sources()
