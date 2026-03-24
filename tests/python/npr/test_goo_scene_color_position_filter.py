import bpy
import os
import tempfile


RESOLUTION = 256
ORTHO_SCALE = 8.0


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
    camera_data.ortho_scale = ORTHO_SCALE
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 6.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def make_surface_material():
    material = bpy.data.materials.new("PositionReceiver")
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)

    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_filter_material():
    material = bpy.data.materials.new("ScenePositionFilter")
    material.use_nodes = True
    material.eevee_domain = "FILTER"

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputFilter")
    output.location = (520.0, 0.0)

    scene_color = nodes.new("ShaderNodeSceneColor")
    scene_color.location = (0.0, 0.0)
    scene_color.source = "POSITION"
    assert scene_color.source == "POSITION", "Scene Color source should accept POSITION"

    separate_xyz = nodes.new("ShaderNodeSeparateXYZ")
    separate_xyz.location = (180.0, 0.0)

    threshold = nodes.new("ShaderNodeMath")
    threshold.location = (360.0, 0.0)
    threshold.operation = "GREATER_THAN"
    threshold.inputs[1].default_value = 0.5

    links.new(scene_color.outputs["Color"], separate_xyz.inputs["Vector"])
    links.new(separate_xyz.outputs["X"], threshold.inputs[0])
    links.new(threshold.outputs["Value"], output.inputs["Color"])
    links.new(threshold.outputs["Value"], output.inputs["Alpha"])
    return material


def attach_filter_material(material):
    filter_entry = bpy.context.scene.eevee.filter_materials.add()
    filter_entry.material = material
    filter_entry.enabled = True


def make_plane(material):
    bpy.ops.mesh.primitive_plane_add(size=8.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.active_object
    plane.data.materials.append(material)


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


def sample_world_point(pixels, width, height, world_x, world_y=0.0):
    x_ratio = (world_x / ORTHO_SCALE) + 0.5
    y_ratio = (world_y / ORTHO_SCALE) + 0.5
    x = min(width - 1, max(0, int(width * x_ratio)))
    y = min(height - 1, max(0, int(height * y_ratio)))
    index = (y * width + x) * 4
    return list(pixels[index:index + 4])


def main():
    clear_scene()
    configure_scene()
    make_camera()
    make_plane(make_surface_material())
    attach_filter_material(make_filter_material())

    pixels, width, height = render_image()
    left = sample_world_point(pixels, width, height, -1.5)
    right = sample_world_point(pixels, width, height, 1.5)

    assert left[0] < 0.1, f"Position-based filter should stay dark on the negative X side, got {left}"
    assert right[0] > 0.9, f"Position-based filter should stay bright on the positive X side, got {right}"


if __name__ == "__main__":
    main()
