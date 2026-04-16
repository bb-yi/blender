import bpy
import os
import tempfile


RESOLUTION = 64


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
    scene.world.use_nodes = False
    scene.world.color = (0.0, 0.0, 0.0)

    while len(scene.eevee.filter_materials) > 0:
        scene.eevee.filter_materials.remove(0)


def make_camera():
    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 4.0
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 5.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def make_surface_material(color):
    material = bpy.data.materials.new("Surface")
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = color
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    return material


def make_plane(material):
    bpy.ops.mesh.primitive_plane_add(size=4.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.active_object
    plane.data.materials.append(material)
    return plane


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


def make_filter_material_invert_scene():
    material = bpy.data.materials.new("FilterGLSLInvert")
    material.use_nodes = True
    material.eevee_domain = "FILTER"

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputFilter")
    output.location = (520.0, 0.0)
    output.inputs["Alpha"].default_value = 1.0

    scene_color = nodes.new("ShaderNodeSceneColor")
    scene_color.location = (0.0, 0.0)
    scene_color.source = "COLOR"

    glsl = nodes.new("ShaderNodeGLSLFunction")
    glsl.location = (260.0, 0.0)
    make_text_block(
        "filter_glsl_invert_scene.glsl",
        "vec4 invert_scene(vec4 color){\n"
        "  return vec4(1.0 - color.rgb, color.a);\n"
        "}\n",
    )
    glsl.script = bpy.data.texts["filter_glsl_invert_scene.glsl"]
    glsl.function_name = "invert_scene"
    refresh_glsl_node(glsl)

    links.new(scene_color.outputs["Color"], glsl.inputs["color"])
    links.new(glsl.outputs["Result"], output.inputs["Color"])
    return material


def make_filter_material_sample2d():
    material = bpy.data.materials.new("FilterGLSLSample2D")
    material.use_nodes = True
    material.eevee_domain = "FILTER"

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputFilter")
    output.location = (520.0, 0.0)
    output.inputs["Alpha"].default_value = 1.0

    image_node = nodes.new("ShaderNodeImageToClosure")
    image_node.location = (0.0, 0.0)
    image = bpy.data.images.new("filter_glsl_sample2d_image", 1, 1, alpha=True, float_buffer=True)
    image.pixels = [1.0, 0.25, 0.0, 1.0]
    image_node.image = image

    glsl = nodes.new("ShaderNodeGLSLFunction")
    glsl.location = (260.0, 0.0)
    make_text_block(
        "filter_glsl_sample2d.glsl",
        "vec4 sample_color(sampler2D tex, vec2 uv){\n"
        "  return texture(tex, uv);\n"
        "}\n",
    )
    glsl.script = bpy.data.texts["filter_glsl_sample2d.glsl"]
    glsl.function_name = "sample_color"
    refresh_glsl_node(glsl)

    links.new(image_node.outputs["Closure"], glsl.inputs["tex"])
    links.new(glsl.outputs["Result"], output.inputs["Color"])
    return material


def attach_filter_material(material):
    entry = bpy.context.scene.eevee.filter_materials.add()
    entry.material = material
    entry.enabled = True
    entry.execution_stage = "BEFORE_COMPOSITE"
    return entry


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
    finally:
        bpy.data.images.remove(image)
        if os.path.exists(filepath):
            os.remove(filepath)

    return pixels


def sample_center_color(pixels):
    index = ((RESOLUTION // 2) * RESOLUTION + (RESOLUTION // 2)) * 4
    return list(pixels[index:index + 4])


def test_invert_scene_color():
    clear_scene()
    configure_scene()
    make_camera()
    make_plane(make_surface_material((0.0, 0.0, 1.0, 1.0)))
    attach_filter_material(make_filter_material_invert_scene())

    bpy.context.view_layer.update()
    color = sample_center_color(render_image())

    assert color[0] > 0.9 and color[1] > 0.9, (
        f"Expected GLSL Function filter to invert blue into yellow, got {color}"
    )
    assert color[2] < 0.1, (
        f"Expected inverted filter output to suppress blue, got {color}"
    )


def test_sample2d_image_to_closure():
    clear_scene()
    configure_scene()
    make_camera()
    make_plane(make_surface_material((0.0, 0.0, 0.0, 1.0)))
    attach_filter_material(make_filter_material_sample2d())

    bpy.context.view_layer.update()
    color = sample_center_color(render_image())

    assert color[0] > 0.9, f"Expected sampler2D filter red channel near 1.0, got {color}"
    assert 0.2 < color[1] < 0.3, f"Expected sampler2D filter green channel near 0.25, got {color}"
    assert color[2] < 0.1, f"Expected sampler2D filter blue channel near 0.0, got {color}"


def main():
    test_invert_scene_color()
    test_sample2d_image_to_closure()


if __name__ == "__main__":
    main()
