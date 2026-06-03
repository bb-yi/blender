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

    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 2.0
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 4.0)
    scene.collection.objects.link(camera)
    scene.camera = camera


def make_image(name, color):
    image = bpy.data.images.new(name, 1, 1, alpha=True, float_buffer=True)
    image.pixels = color
    return image


def socket_by_identifier(sockets, identifier):
    for socket in sockets:
        if socket.identifier == identifier:
            return socket
    raise KeyError(f"Socket identifier {identifier!r} not found")


def link_mix(nodes, links, color_a, color_b, factor):
    mix = nodes.new("ShaderNodeMix")
    mix.data_type = "RGBA"
    socket_by_identifier(mix.inputs, "Factor_Float").default_value = factor
    links.new(color_a, socket_by_identifier(mix.inputs, "A_Color"))
    links.new(color_b, socket_by_identifier(mix.inputs, "B_Color"))
    return socket_by_identifier(mix.outputs, "Result_Color")


def make_material_with_npr_image_mix(name, colors):
    material = bpy.data.materials.new(name)
    material.use_nodes = True

    nodes = material.node_tree.nodes
    output = next(node for node in nodes if node.bl_idname == "ShaderNodeOutputMaterial")
    principled = next(node for node in nodes if node.bl_idname == "ShaderNodeBsdfPrincipled")
    principled.inputs["Base Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    principled.inputs["Roughness"].default_value = 1.0

    npr_tree = bpy.data.node_groups.new(f"{name}NPRTree", "ShaderNodeTree")
    npr_nodes = npr_tree.nodes
    npr_links = npr_tree.links
    npr_nodes.clear()

    image_outputs = []
    for index, color in enumerate(colors):
        image_texture = npr_nodes.new("ShaderNodeTexImage")
        image_texture.image = make_image(f"{name}Image{index}", color)
        image_texture.interpolation = "Closest"
        image_outputs.append(image_texture.outputs["Color"])

    current = link_mix(npr_nodes, npr_links, image_outputs[0], image_outputs[1], 0.5)
    for next_output in image_outputs[2:]:
        current = link_mix(npr_nodes, npr_links, current, next_output, 1.0)

    npr_output = npr_nodes.new("ShaderNodeNPR_Output")
    npr_links.new(current, npr_output.inputs["Color"])
    output.nprtree = npr_tree

    return material


def make_plane(material):
    bpy.ops.mesh.primitive_plane_add(size=2.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.active_object
    plane.name = "NPRImageMixPlane"
    plane.data.materials.append(material)
    return plane


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


def assert_rgb_close(pixel, expected, label):
    assert sum(pixel[:3]) > 0.2, f"Expected non-black {label}, got {pixel}"
    max_delta = max(abs(pixel[i] - expected[i]) for i in range(3))
    assert max_delta < 0.18, f"Expected {label} near {expected}, got {pixel}"


def test_three_image_mix_uses_third_texture():
    clear_scene()
    configure_scene()

    expected = (0.1, 0.55, 1.0, 1.0)
    material = make_material_with_npr_image_mix(
        "NPRThreeImageMix",
        [
            (1.0, 0.0, 0.0, 1.0),
            (0.0, 1.0, 0.0, 1.0),
            expected,
        ],
    )
    make_plane(material)

    pixel = render_center_pixel()
    print(f"MATERIAL_NPR_THREE_IMAGE_MIX_CENTER={pixel}")
    assert_rgb_close(pixel, expected, "three-image NPR texture mix")


def test_four_image_mix_uses_fourth_texture():
    clear_scene()
    configure_scene()

    expected = (1.0, 0.8, 0.05, 1.0)
    material = make_material_with_npr_image_mix(
        "NPRFourImageMix",
        [
            (1.0, 0.0, 0.0, 1.0),
            (0.0, 1.0, 0.0, 1.0),
            (0.0, 0.2, 1.0, 1.0),
            expected,
        ],
    )
    make_plane(material)

    pixel = render_center_pixel()
    print(f"MATERIAL_NPR_FOUR_IMAGE_MIX_CENTER={pixel}")
    assert_rgb_close(pixel, expected, "four-image NPR texture mix")


def main():
    test_three_image_mix_uses_third_texture()
    test_four_image_mix_uses_fourth_texture()


if __name__ == "__main__":
    main()
