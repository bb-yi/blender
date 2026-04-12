import bpy
import os
import tempfile
import traceback


RESOLUTION = 128
ORTHO_SCALE = 8.0
SAMPLE_WORLD_X = 1.4
SAMPLE_WORLD_Y = 0.0
COLOR_EPSILON = 0.06


def write_result(status, message=""):
    print(status)
    if message:
        print(message)


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


def make_camera():
    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = ORTHO_SCALE
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 6.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def make_shader_info_material(name, output_name, exponent=16.0):
    material = bpy.data.materials.new(name)
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Strength"].default_value = 1.0
    shader_info = nodes.new("ShaderNodeShaderInfo")
    exponent_value = nodes.new("ShaderNodeValue")
    exponent_value.outputs[0].default_value = exponent

    links.new(exponent_value.outputs[0], shader_info.inputs["Exponent"])
    links.new(shader_info.outputs[output_name], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_plane(material):
    bpy.ops.mesh.primitive_plane_add(size=8.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.active_object
    plane.data.materials.append(material)


def make_blocker():
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, 1.0))
    blocker = bpy.context.active_object
    blocker.scale = (0.5, 0.5, 1.0)
    return blocker


def make_light():
    light_data = bpy.data.lights.new("ShaderInfoPoint", type="POINT")
    light_data.energy = 5000.0
    light_data.shadow_soft_size = 0.05
    light_data.use_shadow = True
    light = bpy.data.objects.new("ShaderInfoPoint", light_data)
    light.location = (-4.0, 0.0, 4.0)
    bpy.context.scene.collection.objects.link(light)


def build_scene(output_name, with_blocker):
    clear_scene()
    configure_scene()
    make_camera()
    make_plane(make_shader_info_material(f"{output_name}Material", output_name))
    if with_blocker:
        make_blocker()
    make_light()
    bpy.ops.object.select_all(action="DESELECT")
    bpy.context.view_layer.objects.active = None


def find_view3d_context():
    window = bpy.context.window
    screen = window.screen
    for area in screen.areas:
        if area.type != "VIEW_3D":
            continue
        for region in area.regions:
            if region.type == "WINDOW":
                return window, screen, area, region, area.spaces.active
    raise RuntimeError("No VIEW_3D window region found")


def sample_world_point(pixels, width, height, world_x, world_y):
    x_ratio = (world_x / ORTHO_SCALE) + 0.5
    y_ratio = (world_y / ORTHO_SCALE) + 0.5
    x = min(width - 1, max(0, int(width * x_ratio)))
    y = min(height - 1, max(0, int(height * y_ratio)))
    index = (y * width + x) * 4
    return list(pixels[index:index + 4])


def viewport_render_sample(output_name, with_blocker):
    build_scene(output_name, with_blocker)
    window, screen, area, region, space = find_view3d_context()
    space.shading.type = "RENDERED"
    space.shading.use_scene_world_render = True
    space.shading.use_scene_lights_render = True
    space.overlay.show_overlays = False
    space.shading.show_object_outline = False

    with bpy.context.temp_override(
        window=window, screen=screen, area=area, region=region, space_data=space
    ):
        bpy.ops.view3d.view_camera()
        bpy.ops.wm.redraw_timer(type="DRAW_WIN_SWAP", iterations=12)
        bpy.ops.render.opengl(write_still=False, view_context=True)

    file_descriptor, filepath = tempfile.mkstemp(suffix=".exr")
    os.close(file_descriptor)
    try:
        bpy.data.images["Render Result"].save_render(filepath)
        image = bpy.data.images.load(filepath, check_existing=False)
        try:
            return sample_world_point(
                list(image.pixels[:]), image.size[0], image.size[1], SAMPLE_WORLD_X, SAMPLE_WORLD_Y
            )
        finally:
            bpy.data.images.remove(image)
    finally:
        if os.path.exists(filepath):
            os.remove(filepath)


def assert_unshadowed_viewport(output_name, min_value=0.2, tolerance=COLOR_EPSILON):
    blocked = viewport_render_sample(output_name, with_blocker=True)
    unblocked = viewport_render_sample(output_name, with_blocker=False)

    assert blocked[0] > min_value, f"{output_name} viewport blocked sample too dark: {blocked}"
    assert unblocked[0] > min_value, f"{output_name} viewport unblocked sample too dark: {unblocked}"
    assert abs(blocked[0] - unblocked[0]) < tolerance, (
        f"{output_name} viewport should ignore shadowing, got blocked={blocked} unblocked={unblocked}"
    )


def run_tests():
    try:
        assert_unshadowed_viewport("Diffuse Shading")
        assert_unshadowed_viewport("Half-Lambert Factor")
        assert_unshadowed_viewport("Blinn-Phong Factor", min_value=0.05, tolerance=COLOR_EPSILON)
        write_result("PASS")
    except Exception:
        write_result("FAIL", traceback.format_exc())
    finally:
        bpy.ops.wm.quit_blender()
    return None


def fail_safe_quit():
    write_result("FAIL", "Viewport Shader Info test timed out.")
    bpy.ops.wm.quit_blender()
    return None


assert hasattr(bpy.types, "ShaderNodeShaderInfo"), "ShaderNodeShaderInfo is not registered"

bpy.app.timers.register(run_tests, first_interval=1.0)
bpy.app.timers.register(fail_safe_quit, first_interval=45.0)
