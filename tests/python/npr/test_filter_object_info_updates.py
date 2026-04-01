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


def make_plane():
    material = bpy.data.materials.new("Surface")
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    bpy.ops.mesh.primitive_plane_add(size=4.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.active_object
    plane.data.materials.append(material)


def make_target_object():
    bpy.ops.mesh.primitive_cube_add(size=0.5, location=(0.0, 0.0, 0.0))
    target = bpy.context.active_object
    target.name = "FilterTarget"
    return target


def make_filter_material(target):
    material = bpy.data.materials.new("FilterObjectInfo")
    material.use_nodes = True
    material.eevee_domain = "FILTER"

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputFilter")
    output.location = (700.0, 0.0)
    output.inputs["Alpha"].default_value = 1.0

    object_info = nodes.new("ShaderNodeFilterObjectInfo")
    object_info.location = (0.0, 0.0)
    object_info.object = target

    separate = nodes.new("ShaderNodeSeparateXYZ")
    separate.location = (220.0, 0.0)

    multiply = nodes.new("ShaderNodeMath")
    multiply.location = (440.0, 0.0)
    multiply.operation = "MULTIPLY"
    multiply.inputs[1].default_value = 0.5

    combine = nodes.new("ShaderNodeCombineColor")
    combine.location = (560.0, 0.0)

    links.new(object_info.outputs["Location"], separate.inputs["Vector"])
    links.new(separate.outputs["X"], multiply.inputs[0])
    links.new(multiply.outputs["Value"], combine.inputs["Red"])
    links.new(combine.outputs["Color"], output.inputs["Color"])

    return material


def attach_filter_material(material):
    entry = bpy.context.scene.eevee.filter_materials.add()
    entry.material = material
    entry.enabled = True


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


def sample_center_red(pixels):
    index = ((RESOLUTION // 2) * RESOLUTION + (RESOLUTION // 2)) * 4
    return pixels[index]


def main():
    clear_scene()
    configure_scene()
    make_camera()
    make_plane()
    target = make_target_object()
    attach_filter_material(make_filter_material(target))

    bpy.context.view_layer.update()
    red_initial = sample_center_red(render_image())

    target.location.x = 1.0
    bpy.context.view_layer.update()
    red_moved = sample_center_red(render_image())

    print(f"FILTER_OBJECT_INFO_RED_INITIAL={red_initial:.6f}")
    print(f"FILTER_OBJECT_INFO_RED_MOVED={red_moved:.6f}")

    assert red_initial < 0.1, (
        f"Expected near-black initial filter color from X=0.0, got {red_initial}"
    )
    assert red_moved > 0.4, (
        f"Expected red channel to update after moving object to X=1.0, got {red_moved}"
    )
    assert (red_moved - red_initial) > 0.3, (
        f"Expected object move to change filter output, got initial={red_initial}, moved={red_moved}"
    )


if __name__ == "__main__":
    main()
