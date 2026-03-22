import bpy
import os
import tempfile


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def configure_scene():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 128
    scene.render.resolution_y = 128
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
    camera_data.ortho_scale = 2.0
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 4.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def make_plane(material):
    bpy.ops.mesh.primitive_plane_add(size=2.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.active_object
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


def make_render_info_material():
    material = bpy.data.materials.new("RenderInfoRender")
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    output.location = (520.0, 0.0)

    emission = nodes.new("ShaderNodeEmission")
    emission.location = (300.0, 0.0)
    emission.inputs["Strength"].default_value = 1.0

    separate_xyz = nodes.new("ShaderNodeSeparateXYZ")
    separate_xyz.location = (80.0, 0.0)

    render_info = nodes.new("ShaderNodeRenderInfo")
    render_info.location = (-160.0, 0.0)

    links.new(render_info.outputs["Frag Coord"], separate_xyz.inputs["Vector"])
    links.new(render_info.outputs["Frag Coord"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    return material


assert hasattr(bpy.types, "ShaderNodeRenderInfo"), "ShaderNodeRenderInfo is not registered"

clear_scene()
configure_scene()
make_camera()
make_plane(make_render_info_material())

pixel = render_center_pixel()
r, g, b, _a = pixel

assert 0.45 < r < 0.55, f"Expected normalized Frag Coord X near 0.5, got {r}"
assert 0.45 < g < 0.55, f"Expected normalized Frag Coord Y near 0.5, got {g}"
assert 0.0 <= b <= 1.0, f"Expected Frag Coord Z in window-depth range, got {b}"
