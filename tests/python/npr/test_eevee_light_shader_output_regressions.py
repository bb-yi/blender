from pathlib import Path

import bpy
from mathutils import Vector


ROOT = Path(__file__).resolve().parents[4]
OUT_DIR = ROOT / "temp" / "eevee_light_shader_output" / "regressions"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def look_at(obj, target):
    direction = Vector(target) - obj.location
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def setup_render(output_dir):
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 96
    scene.render.resolution_y = 96
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.filepath = str(output_dir / "render.png")
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.world.color = (0.0, 0.0, 0.0)

    eevee = scene.eevee
    for attr, value in (
        ("taa_render_samples", 1),
        ("taa_samples", 1),
        ("use_raytracing", False),
        ("clamp_direct", 0.0),
    ):
        if hasattr(eevee, attr):
            setattr(eevee, attr, value)


def add_camera():
    bpy.ops.object.camera_add(location=(0.0, -4.0, 2.5))
    camera = bpy.context.object
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = 2.5
    look_at(camera, (0.0, 0.0, 0.0))
    bpy.context.scene.camera = camera


def add_receiver():
    material = bpy.data.materials.new("LightShaderRegressionReceiver")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    diffuse = nodes.new("ShaderNodeBsdfDiffuse")
    diffuse.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    diffuse.inputs["Roughness"].default_value = 0.8
    links.new(diffuse.outputs["BSDF"], output.inputs["Surface"])

    bpy.ops.mesh.primitive_plane_add(size=2.0)
    plane = bpy.context.object
    plane.data.materials.append(material)


def find_light_shader_node(tree, bl_idname):
    active = None
    first = None
    for node in tree.nodes:
        if node.bl_idname != bl_idname:
            continue
        first = first or node
        if getattr(node, "is_active_output", False):
            active = node
    return active or first


def clear_socket_links(tree, socket):
    for link in list(socket.links):
        tree.links.remove(link)


def add_scene_time_light():
    bpy.ops.object.light_add(type="POINT", location=(0.0, -1.5, 2.5))
    light_obj = bpy.context.object
    light = light_obj.data
    light.energy = 80.0
    light.shadow_soft_size = 0.0
    light.use_shadow = False
    light.use_nodes = True
    tree = light.node_tree
    require(tree is not None, "light node tree was not created")

    output = find_light_shader_node(tree, "ShaderNodeEeveeLightShaderOutput")
    require(output is not None, "Light Shader Output was not created")
    for socket in (output.inputs["Color"], output.inputs["Intensity"], output.inputs["Attenuation"]):
        clear_socket_links(tree, socket)
    output.inputs["Intensity"].default_value = 2.0
    output.inputs["Attenuation"].default_value = 1.0
    if hasattr(output, "range_scale"):
        output.range_scale = 1.0

    nodes = tree.nodes
    links = tree.links
    scene_time = nodes.new("GeometryNodeInputSceneTime")
    map_range = nodes.new("ShaderNodeMapRange")
    map_range.inputs["From Min"].default_value = 1.0
    map_range.inputs["From Max"].default_value = 11.0
    map_range.inputs["To Min"].default_value = 0.0
    map_range.inputs["To Max"].default_value = 1.0
    ramp = nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].position = 0.0
    ramp.color_ramp.elements[0].color = (0.0, 0.0, 1.0, 1.0)
    ramp.color_ramp.elements[1].position = 1.0
    ramp.color_ramp.elements[1].color = (1.0, 0.0, 0.0, 1.0)
    links.new(scene_time.outputs["Frame"], map_range.inputs["Value"])
    links.new(map_range.outputs["Result"], ramp.inputs["Fac"])
    links.new(ramp.outputs["Color"], output.inputs["Color"])


def render_pixels(path):
    bpy.context.scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        pixels = list(image.pixels[:])
        size = tuple(image.size)
    finally:
        bpy.data.images.remove(image)
    return size, pixels


def sample_center_rgb(size, pixels):
    width, height = size
    sums = [0.0, 0.0, 0.0]
    count = 0
    for y in range(height // 3, height * 2 // 3):
        for x in range(width // 3, width * 2 // 3):
            i = (y * width + x) * 4
            for c in range(3):
                sums[c] += pixels[i + c]
            count += 1
    return tuple(value / count for value in sums)


def max_pixel_rgb_diff(size_a, pixels_a, size_b, pixels_b):
    require(size_a == size_b, f"render sizes differ: {size_a} vs {size_b}")
    max_diff = 0.0
    for i in range(0, len(pixels_a), 4):
        for c in range(3):
            max_diff = max(max_diff, abs(pixels_a[i + c] - pixels_b[i + c]))
    return max_diff


def main():
    output_dir = OUT_DIR
    output_dir.mkdir(parents=True, exist_ok=True)
    clear_scene()
    setup_render(output_dir)
    add_camera()
    add_receiver()
    add_scene_time_light()

    bpy.context.scene.frame_set(1)
    size_1, pixels_1 = render_pixels(output_dir / "scene_time_frame_001.png")
    color_1 = sample_center_rgb(size_1, pixels_1)

    bpy.context.scene.frame_set(11)
    size_11, pixels_11 = render_pixels(output_dir / "scene_time_frame_011.png")
    color_11 = sample_center_rgb(size_11, pixels_11)
    diff = max_pixel_rgb_diff(size_1, pixels_1, size_11, pixels_11)

    require(color_11[0] > color_1[0] + 0.04, f"Scene Time red channel stayed stale: {color_1}, {color_11}")
    require(color_1[2] > color_11[2] + 0.04, f"Scene Time blue channel stayed stale: {color_1}, {color_11}")
    require(diff > 0.04, f"Scene Time render did not change enough: diff={diff}")

    blend_path = output_dir / "light_shader_output_regression.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(blend_path))
    bpy.ops.wm.open_mainfile(filepath=str(blend_path))
    size_reload, pixels_reload = render_pixels(output_dir / "reloaded_frame_011.png")
    color_reload = sample_center_rgb(size_reload, pixels_reload)

    require(
        color_reload[0] > color_reload[2] + 0.03,
        f"Reloaded Light Shader Output did not keep the custom red result: {color_reload}",
    )
    print(
        "EEVEE_LIGHT_SHADER_OUTPUT_REGRESSION_OK "
        f"frame1={tuple(round(v, 6) for v in color_1)} "
        f"frame11={tuple(round(v, 6) for v in color_11)} "
        f"reload={tuple(round(v, 6) for v in color_reload)} "
        f"max_diff={diff:.6f} "
        f"output_dir={output_dir}",
        flush=True,
    )


if __name__ == "__main__":
    main()
