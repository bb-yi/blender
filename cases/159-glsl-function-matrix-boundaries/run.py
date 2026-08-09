import os
import tempfile

import bpy


RESOLUTION = 64
EXPECTED_RGB = (0.2, 0.4, 0.2)
EPSILON = 0.08


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


def set_vec(socket, values):
    socket.default_value = values


def set_mat2(node, name, columns):
    for column, values in enumerate(columns):
        set_vec(find_socket(node.inputs, f"In_{name}_c{column}"), values)


def set_mat3(node, name, columns):
    for column, values in enumerate(columns):
        set_vec(find_socket(node.inputs, f"In_{name}_c{column}"), values)


def set_mat4(node, name, columns):
    for column, values in enumerate(columns):
        set_vec(find_socket(node.inputs, f"In_{name}_c{column}"), values[:3])
        find_socket(node.inputs, f"In_{name}_c{column}_w").default_value = values[3]


def assert_socket_shape(node):
    for column in range(2):
        socket = find_socket(node.inputs, f"In_m2_c{column}")
        if socket.bl_idname != "NodeSocketVector2D":
            raise AssertionError(f"Expected mat2 column {column} vec2 socket, got {socket.bl_idname}")

    for column in range(3):
        socket = find_socket(node.inputs, f"In_m3_c{column}")
        if socket.bl_idname != "NodeSocketVector":
            raise AssertionError(f"Expected mat3 column {column} vec3 socket, got {socket.bl_idname}")

    for column in range(4):
        xyz_socket = find_socket(node.inputs, f"In_m4_c{column}")
        w_socket = find_socket(node.inputs, f"In_m4_c{column}_w")
        result_socket = find_socket(node.outputs, f"Result_c{column}")
        result_w_socket = find_socket(node.outputs, f"Result_c{column}_w")
        out_socket = find_socket(node.outputs, f"Out_out_m4_c{column}")
        out_w_socket = find_socket(node.outputs, f"Out_out_m4_c{column}_w")
        if xyz_socket.bl_idname != "NodeSocketVector":
            raise AssertionError(f"Expected mat4 input column {column} vec3 socket, got {xyz_socket.bl_idname}")
        if w_socket.bl_idname != "NodeSocketFloat":
            raise AssertionError(f"Expected mat4 input column {column} W float socket, got {w_socket.bl_idname}")
        if result_socket.bl_idname != "NodeSocketVector" or result_w_socket.bl_idname != "NodeSocketFloat":
            raise AssertionError("Expected mat4 return columns to split into vec3 plus W")
        if out_socket.bl_idname != "NodeSocketVector" or out_w_socket.bl_idname != "NodeSocketFloat":
            raise AssertionError("Expected out mat4 columns to split into vec3 plus W")


def build_matrix_material():
    material = bpy.data.materials.new("GLSLMatrixBoundariesRelease")
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    glsl = nodes.new("ShaderNodeGLSLFunction")

    make_text_block(
        "glsl_matrix_boundaries_release.glsl",
        "mat4 matrix_boundary_probe(mat2 m2, mat3 m3, mat4 m4, out mat4 out_m4){\n"
        "  vec2 uv = m2 * vec2(0.25, 0.5);\n"
        "  vec3 rgb = m3 * vec3(uv.x, uv.y, 1.0);\n"
        "  out_m4 = m4;\n"
        "  return mat4(vec4(rgb, 0.0), m4[1], m4[2], m4[3]);\n"
        "}\n",
    )
    glsl.script = bpy.data.texts["glsl_matrix_boundaries_release.glsl"]
    glsl.function_name = "matrix_boundary_probe"
    refresh_glsl_node(glsl)

    if glsl.parse_status != "READY":
        raise AssertionError(f"Expected GLSL Function parse READY, got {glsl.parse_status}")

    assert_socket_shape(glsl)
    set_mat2(glsl, "m2", ((0.4, 0.0), (0.0, 0.8)))
    set_mat3(glsl, "m3", ((2.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 0.2)))
    set_mat4(
        glsl,
        "m4",
        (
            (0.0, 0.0, 0.0, 0.0),
            (0.0, 0.0, 0.0, 0.0),
            (0.0, 0.0, 0.0, 0.0),
            (0.0, 0.0, 0.0, 1.0),
        ),
    )

    links.new(find_socket(glsl.outputs, "Result_c0"), emission.inputs["Color"])
    links.new(find_socket(glsl.outputs, "Out_out_m4_c3_w"), emission.inputs["Strength"])
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
    plane.data.materials.append(build_matrix_material())
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
    for channel, expected in zip(color[:3], EXPECTED_RGB):
        if abs(channel - expected) > EPSILON:
            raise AssertionError(
                f"Expected matrix boundary render RGB near {EXPECTED_RGB}, got center pixel {color}"
            )
    print(f"GLSL_FUNCTION_MATRIX_BOUNDARIES_RELEASE_OK center={color}")


if __name__ == "__main__":
    main()
