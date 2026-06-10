import bpy
import os
import tempfile


SAMPLE_POINTS = {
    "left": (16, 32),
    "right": (48, 32),
    "bottom": (32, 16),
    "top": (32, 48),
}


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

    scene.world.use_nodes = True
    nodes = scene.world.node_tree.nodes
    links = scene.world.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputWorld")
    background = nodes.new("ShaderNodeBackground")
    background.inputs["Color"].default_value = (0.0, 1.0, 0.0, 1.0)
    background.inputs["Strength"].default_value = 1.0
    links.new(background.outputs["Background"], output.inputs["Surface"])

    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 2.0
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 4.0)
    scene.collection.objects.link(camera)
    scene.camera = camera

    return output


def render_pixels():
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
        pixels = list(image.pixels[:])
    finally:
        bpy.data.images.remove(image)
        if os.path.exists(filepath):
            os.remove(filepath)

    return pixels, width, height


def sample_pixel(pixels, width, height, x, y):
    index = (y * width + x) * 4
    return list(pixels[index:index + 4])


def render_center_pixel():
    pixels, width, height = render_pixels()
    return sample_pixel(pixels, width, height, width // 2, height // 2)


def render_sample_points():
    pixels, width, height = render_pixels()
    return {
        name: sample_pixel(pixels, width, height, x, y)
        for name, (x, y) in SAMPLE_POINTS.items()
    }


def rgb_distance(a, b):
    return sum(abs(a[i] - b[i]) for i in range(3))


def assert_samples_vary(samples, label):
    values = list(samples.values())
    max_delta = max(rgb_distance(a, b) for a in values for b in values)
    assert max_delta > 0.2, f"Expected {label} to vary across screen samples, got {samples}"


def socket_by_identifier(sockets, identifier):
    for socket in sockets:
        if socket.identifier == identifier:
            return socket
    raise KeyError(f"Socket identifier {identifier!r} not found")


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


def make_image(name, width, height, colors):
    image = bpy.data.images.new(name, width, height, alpha=True, float_buffer=True)
    pixels = []
    for color in colors:
        pixels.extend(color)
    image.pixels = pixels
    return image


def make_pattern_image(name):
    return make_image(
        name,
        2,
        2,
        [
            (1.0, 0.0, 0.0, 1.0),
            (0.0, 1.0, 0.0, 1.0),
            (0.0, 0.0, 1.0, 1.0),
            (1.0, 1.0, 0.0, 1.0),
        ],
    )


def make_second_pattern_image(name):
    return make_image(
        name,
        2,
        2,
        [
            (0.0, 0.0, 0.0, 1.0),
            (1.0, 0.0, 1.0, 1.0),
            (0.0, 1.0, 1.0, 1.0),
            (1.0, 1.0, 1.0, 1.0),
        ],
    )


def attach_solid_world_npr(output):
    npr_tree = bpy.data.node_groups.new("WorldSolidNPRTree", "ShaderNodeTree")
    nodes = npr_tree.nodes
    links = npr_tree.links
    nodes.clear()

    rgb = nodes.new("ShaderNodeRGB")
    rgb.outputs["Color"].default_value = (1.0, 0.0, 0.0, 1.0)
    npr_output = nodes.new("ShaderNodeNPR_Output")
    links.new(rgb.outputs["Color"], npr_output.inputs["Color"])

    output.nprtree = npr_tree


def attach_npr_input_passthrough_world_npr(output, input_name, tree_name):
    npr_tree = bpy.data.node_groups.new(tree_name, "ShaderNodeTree")
    nodes = npr_tree.nodes
    links = npr_tree.links
    nodes.clear()

    npr_input = nodes.new("ShaderNodeNPR_Input")
    npr_output = nodes.new("ShaderNodeNPR_Output")
    links.new(npr_input.outputs[input_name], npr_output.inputs["Color"])

    output.nprtree = npr_tree


def attach_combined_color_world_npr(output):
    attach_npr_input_passthrough_world_npr(
        output, "Combined Color", "WorldCombinedColorNPRTree"
    )


def attach_image_sample_world_npr(output, input_name, tree_name, scale=1.0):
    npr_tree = bpy.data.node_groups.new(tree_name, "ShaderNodeTree")
    nodes = npr_tree.nodes
    links = npr_tree.links
    nodes.clear()

    npr_input = nodes.new("ShaderNodeNPR_Input")
    image_sample = nodes.new("ShaderNodeNPR_ImageSample")
    npr_output = nodes.new("ShaderNodeNPR_Output")

    links.new(npr_input.outputs[input_name], image_sample.inputs["Image"])

    if scale == 1.0:
        links.new(image_sample.outputs["Color"], npr_output.inputs["Color"])
    else:
        scale_node = nodes.new("ShaderNodeVectorMath")
        scale_node.operation = "SCALE"
        scale_node.inputs["Scale"].default_value = scale
        links.new(image_sample.outputs["Color"], scale_node.inputs["Vector"])
        links.new(scale_node.outputs["Vector"], npr_output.inputs["Color"])

    output.nprtree = npr_tree


def use_directional_world_background():
    nodes = bpy.context.scene.world.node_tree.nodes
    links = bpy.context.scene.world.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputWorld")
    background = nodes.new("ShaderNodeBackground")
    tex_coord = nodes.new("ShaderNodeTexCoord")
    separate = nodes.new("ShaderNodeSeparateXYZ")
    multiply_add = nodes.new("ShaderNodeMath")
    combine = nodes.new("ShaderNodeCombineColor")

    background.inputs["Strength"].default_value = 1.0
    multiply_add.operation = "MULTIPLY_ADD"
    multiply_add.inputs[1].default_value = 0.5
    multiply_add.inputs[2].default_value = 0.5
    multiply_add.use_clamp = True

    links.new(tex_coord.outputs["Generated"], separate.inputs["Vector"])
    links.new(separate.outputs["X"], multiply_add.inputs[0])
    links.new(multiply_add.outputs["Value"], combine.inputs["Red"])
    links.new(combine.outputs["Color"], background.inputs["Color"])
    links.new(background.outputs["Background"], output.inputs["Surface"])

    return output


def attach_combined_color_image_sample_world_npr(output, offset_x):
    npr_tree = bpy.data.node_groups.new("WorldCombinedColorImageSampleNPRTree", "ShaderNodeTree")
    nodes = npr_tree.nodes
    links = npr_tree.links
    nodes.clear()

    npr_input = nodes.new("ShaderNodeNPR_Input")
    image_sample = nodes.new("ShaderNodeNPR_ImageSample")
    image_sample.offset_type = "PIXEL"
    offset = nodes.new("ShaderNodeCombineXYZ")
    offset.inputs["X"].default_value = offset_x
    npr_output = nodes.new("ShaderNodeNPR_Output")

    links.new(npr_input.outputs["Combined Color"], image_sample.inputs["Image"])
    links.new(offset.outputs["Vector"], image_sample.inputs["Offset"])
    links.new(image_sample.outputs["Color"], npr_output.inputs["Color"])

    output.nprtree = npr_tree


def attach_image_texture_world_npr(output, image):
    npr_tree = bpy.data.node_groups.new("WorldImageTextureNPRTree", "ShaderNodeTree")
    nodes = npr_tree.nodes
    links = npr_tree.links
    nodes.clear()

    image_texture = nodes.new("ShaderNodeTexImage")
    image_texture.image = image
    image_texture.interpolation = "Closest"
    image_texture.extension = "EXTEND"
    npr_output = nodes.new("ShaderNodeNPR_Output")

    links.new(image_texture.outputs["Color"], npr_output.inputs["Color"])
    output.nprtree = npr_tree


def attach_mixed_image_texture_world_npr(output, image_a, image_b):
    npr_tree = bpy.data.node_groups.new("WorldMixedImageTextureNPRTree", "ShaderNodeTree")
    nodes = npr_tree.nodes
    links = npr_tree.links
    nodes.clear()

    image_texture_a = nodes.new("ShaderNodeTexImage")
    image_texture_a.image = image_a
    image_texture_a.interpolation = "Closest"
    image_texture_a.extension = "EXTEND"
    image_texture_b = nodes.new("ShaderNodeTexImage")
    image_texture_b.image = image_b
    image_texture_b.interpolation = "Closest"
    image_texture_b.extension = "EXTEND"
    mix = nodes.new("ShaderNodeMix")
    mix.data_type = "RGBA"
    socket_by_identifier(mix.inputs, "Factor_Float").default_value = 0.5
    npr_output = nodes.new("ShaderNodeNPR_Output")

    links.new(image_texture_a.outputs["Color"], socket_by_identifier(mix.inputs, "A_Color"))
    links.new(image_texture_b.outputs["Color"], socket_by_identifier(mix.inputs, "B_Color"))
    links.new(socket_by_identifier(mix.outputs, "Result_Color"), npr_output.inputs["Color"])
    output.nprtree = npr_tree


def attach_glsl_image_to_closure_mix_world_npr(output):
    npr_tree = bpy.data.node_groups.new("WorldGLSLImageToClosureMixNPRTree", "ShaderNodeTree")
    nodes = npr_tree.nodes
    links = npr_tree.links
    nodes.clear()

    image_a = make_image("world_glsl_sampler_red", 1, 1, [(1.0, 0.0, 0.0, 1.0)])
    image_b = make_image("world_glsl_sampler_blue", 1, 1, [(0.0, 0.0, 1.0, 1.0)])
    image_node_a = nodes.new("ShaderNodeImageToClosure")
    image_node_a.image = image_a
    image_node_b = nodes.new("ShaderNodeImageToClosure")
    image_node_b.image = image_b

    glsl = nodes.new("ShaderNodeGLSLFunction")
    glsl.script = make_text_block(
        "world_glsl_sampler_mix.glsl",
        "vec4 average_tex(sampler2D tex_a, sampler2D tex_b, vec2 uv){\n"
        "  return 0.5 * (texture(tex_a, uv) + texture(tex_b, uv));\n"
        "}\n",
    )
    glsl.function_name = "average_tex"
    refresh_glsl_node(glsl)

    npr_output = nodes.new("ShaderNodeNPR_Output")
    links.new(image_node_a.outputs["Closure"], glsl.inputs["tex_a"])
    links.new(image_node_b.outputs["Closure"], glsl.inputs["tex_b"])
    links.new(glsl.outputs["Result"], npr_output.inputs["Color"])
    output.nprtree = npr_tree


def attach_glsl_closure_output_sampler_world_npr(output):
    npr_tree = bpy.data.node_groups.new("WorldGLSLClosureOutputSamplerNPRTree", "ShaderNodeTree")
    nodes = npr_tree.nodes
    links = npr_tree.links
    nodes.clear()

    closure_output = nodes.new("NodeClosureOutput")
    closure_output.input_items.new("VECTOR", "UV")
    closure_output.output_items.new("RGBA", "Color")
    npr_tree.interface_update(bpy.context)

    rgb = nodes.new("ShaderNodeRGB")
    rgb.outputs["Color"].default_value = (0.0, 0.75, 0.25, 1.0)
    glsl = nodes.new("ShaderNodeGLSLFunction")
    glsl.script = make_text_block(
        "world_glsl_closure_sampler.glsl",
        "vec4 sample_closure(sampler2D tex, vec2 uv){\n"
        "  return texture(tex, uv);\n"
        "}\n",
    )
    glsl.function_name = "sample_closure"
    refresh_glsl_node(glsl)

    npr_output = nodes.new("ShaderNodeNPR_Output")
    links.new(rgb.outputs["Color"], closure_output.inputs["Color"])
    links.new(closure_output.outputs["Closure"], glsl.inputs["tex"])
    links.new(glsl.outputs["Result"], npr_output.inputs["Color"])
    output.nprtree = npr_tree


def test_basic_world_npr_output():
    clear_scene()
    world_output = configure_scene()

    attach_solid_world_npr(world_output)
    pixel = render_center_pixel()
    r, g, b, _a = pixel
    print(f"WORLD_NPR_SOLID_CENTER={pixel}")

    assert r > 0.8, f"Expected World NPR solid output to replace background with red, got {pixel}"
    assert g < 0.1, f"Expected World NPR solid output to replace green background, got {pixel}"
    assert b < 0.1, f"Expected low blue from World NPR solid output, got {pixel}"

    attach_combined_color_world_npr(world_output)
    pixel = render_center_pixel()
    r, g, b, _a = pixel
    print(f"WORLD_NPR_COMBINED_CENTER={pixel}")

    assert g > 0.8, f"Expected World NPR Combined Color to read green background, got {pixel}"
    assert r < 0.1, f"Expected low red from World NPR Combined Color passthrough, got {pixel}"
    assert b < 0.1, f"Expected low blue from World NPR Combined Color passthrough, got {pixel}"


def test_world_npr_input_color_channels():
    clear_scene()
    world_output = configure_scene()

    for input_name in [
        "Combined Color",
        "Diffuse Color",
        "Diffuse Direct",
        "Diffuse Indirect",
        "Specular Color",
        "Specular Direct",
        "Specular Indirect",
    ]:
        tree_name = "World" + input_name.replace(" ", "") + "PassthroughNPRTree"
        attach_npr_input_passthrough_world_npr(world_output, input_name, tree_name)
        pixel = render_center_pixel()
        r, g, b, _a = pixel
        safe_name = input_name.upper().replace(" ", "_")
        print(f"WORLD_NPR_INPUT_{safe_name}_CENTER={pixel}")

        assert g > 0.8, f"Expected World NPR {input_name} to read green background, got {pixel}"
        assert r < 0.1, f"Expected low red from World NPR {input_name}, got {pixel}"
        assert b < 0.1, f"Expected low blue from World NPR {input_name}, got {pixel}"


def test_world_npr_image_sample_buffers():
    clear_scene()
    world_output = configure_scene()

    attach_image_sample_world_npr(world_output, "Normal", "WorldNormalImageSampleNPRTree")
    pixel = render_center_pixel()
    r, g, b, _a = pixel
    print(f"WORLD_NPR_IMAGE_SAMPLE_NORMAL_CENTER={pixel}")

    assert b > 0.8, f"Expected World NPR Image Sample to read Normal blue channel, got {pixel}"
    assert abs(r) < 0.1, f"Expected low red from World NPR Normal Image Sample, got {pixel}"
    assert abs(g) < 0.1, f"Expected low green from World NPR Normal Image Sample, got {pixel}"

    attach_image_sample_world_npr(world_output, "Position", "WorldPositionImageSampleNPRTree", -1.0)
    pixel = render_center_pixel()
    r, g, b, _a = pixel
    print(f"WORLD_NPR_IMAGE_SAMPLE_POSITION_CENTER={pixel}")

    assert b > 0.8, f"Expected World NPR Image Sample to read Position vector, got {pixel}"
    assert abs(r) < 0.1, f"Expected low red from World NPR Position Image Sample, got {pixel}"
    assert abs(g) < 0.1, f"Expected low green from World NPR Position Image Sample, got {pixel}"


def test_world_npr_combined_color_image_sample_offset():
    clear_scene()
    configure_scene()
    world_output = use_directional_world_background()
    bpy.context.scene.camera.data.type = "PERSP"
    bpy.context.scene.camera.data.lens = 18.0

    attach_combined_color_image_sample_world_npr(world_output, 0.0)
    center_pixel = render_center_pixel()
    print(f"WORLD_NPR_IMAGE_SAMPLE_COMBINED_CENTER={center_pixel}")

    attach_combined_color_image_sample_world_npr(world_output, 24.0)
    offset_pixel = render_center_pixel()
    print(f"WORLD_NPR_IMAGE_SAMPLE_COMBINED_OFFSET={offset_pixel}")

    assert abs(offset_pixel[0] - center_pixel[0]) > 0.05, (
        "Expected World NPR Image Sample Combined Color pixel offset to sample a different world "
        f"direction, got center={center_pixel}, offset={offset_pixel}"
    )


def test_world_npr_image_texture_defaults_to_window_coordinates():
    clear_scene()
    world_output = configure_scene()
    attach_image_texture_world_npr(world_output, make_pattern_image("world_image_texture_pattern"))

    samples = render_sample_points()
    print(f"WORLD_NPR_IMAGE_TEXTURE_WINDOW_SAMPLES={samples}")
    assert_samples_vary(samples, "World NPR Image Texture default window coordinates")


def test_world_npr_mixed_image_textures_default_to_window_coordinates():
    clear_scene()
    world_output = configure_scene()
    attach_mixed_image_texture_world_npr(
        world_output,
        make_pattern_image("world_image_texture_mix_pattern_a"),
        make_second_pattern_image("world_image_texture_mix_pattern_b"),
    )

    samples = render_sample_points()
    print(f"WORLD_NPR_MIXED_IMAGE_TEXTURE_WINDOW_SAMPLES={samples}")
    assert_samples_vary(samples, "World NPR mixed Image Texture default window coordinates")


def test_world_npr_glsl_image_to_closure_sampler_mix():
    clear_scene()
    world_output = configure_scene()
    attach_glsl_image_to_closure_mix_world_npr(world_output)

    pixel = render_center_pixel()
    print(f"WORLD_NPR_GLSL_IMAGE_TO_CLOSURE_MIX_CENTER={pixel}")
    assert 0.35 < pixel[0] < 0.65, f"Expected red half of sampler mix, got {pixel}"
    assert pixel[1] < 0.1, f"Expected low green from red/blue sampler mix, got {pixel}"
    assert 0.35 < pixel[2] < 0.65, f"Expected blue half of sampler mix, got {pixel}"


def test_world_npr_glsl_closure_output_sampler():
    clear_scene()
    world_output = configure_scene()
    attach_glsl_closure_output_sampler_world_npr(world_output)

    pixel = render_center_pixel()
    print(f"WORLD_NPR_GLSL_CLOSURE_OUTPUT_SAMPLER_CENTER={pixel}")
    assert pixel[0] < 0.1, f"Expected low red from Closure Output sampler, got {pixel}"
    assert 0.55 < pixel[1] < 0.9, f"Expected green from Closure Output sampler, got {pixel}"
    assert 0.15 < pixel[2] < 0.45, f"Expected blue-green from Closure Output sampler, got {pixel}"


def main():
    test_basic_world_npr_output()
    test_world_npr_input_color_channels()
    test_world_npr_image_sample_buffers()
    test_world_npr_combined_color_image_sample_offset()
    test_world_npr_image_texture_defaults_to_window_coordinates()
    test_world_npr_mixed_image_textures_default_to_window_coordinates()
    test_world_npr_glsl_image_to_closure_sampler_mix()
    test_world_npr_glsl_closure_output_sampler()


if __name__ == "__main__":
    main()
