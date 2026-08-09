import os
import tempfile

import bpy


RESOLUTION = 64
MAX_BLACK_CHANNEL = 0.05


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


def find_socket(sockets, identifier):
    for socket in sockets:
        if socket.identifier == identifier or socket.name == identifier:
            return socket
    raise AssertionError(f"Socket {identifier!r} not found")


def build_vec4_w_material():
    material = bpy.data.materials.new("GLSLVec4InputWRelease")
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    glsl = nodes.new("ShaderNodeGLSLFunction")

    make_text_block(
        "glsl_vec4_input_w_release.glsl",
        "vec4 show_input_w(vec4 color){\n"
        "  return vec4(vec3(color.w), 1.0);\n"
        "}\n",
    )
    glsl.script = bpy.data.texts["glsl_vec4_input_w_release.glsl"]
    glsl.function_name = "show_input_w"
    refresh_glsl_node(glsl)

    if glsl.parse_status != "READY":
        raise AssertionError(f"Expected GLSL Function parse READY, got {glsl.parse_status}")

    color_socket = find_socket(glsl.inputs, "In_color")
    color_w_socket = find_socket(glsl.inputs, "In_color_w")
    if color_socket.bl_idname != "NodeSocketVector":
        raise AssertionError(f"Expected NodeSocketVector, got {color_socket.bl_idname}")
    if color_w_socket.bl_idname != "NodeSocketFloat":
        raise AssertionError(f"Expected NodeSocketFloat, got {color_w_socket.bl_idname}")

    color_socket.default_value = (0.0, 0.0, 0.0)
    color_w_socket.default_value = 0.0
    links.new(glsl.outputs["Result"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_scene():
    clear_scene()
    configure_scene()

    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 4.0
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 5.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera

    bpy.ops.mesh.primitive_plane_add(size=4.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.active_object
    plane.data.materials.append(build_vec4_w_material())
    bpy.context.view_layer.update()


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
        return list(image.pixels[:])
    finally:
        bpy.data.images.remove(image)
        if os.path.exists(filepath):
            os.remove(filepath)


def sample_center_color(pixels):
    index = ((RESOLUTION // 2) * RESOLUTION + (RESOLUTION // 2)) * 4
    return list(pixels[index:index + 4])


def main():
    make_scene()
    color = sample_center_color(render_pixels())
    if max(color[:3]) > MAX_BLACK_CHANNEL:
        raise AssertionError(
            f"Expected vec4 input w=0.0 to render black, got center pixel {color}"
        )
    print(f"GLSL_FUNCTION_VEC4_INPUT_W_RELEASE_OK center={color}")


if __name__ == "__main__":
    main()
