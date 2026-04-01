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


def make_surface_material():
    material = bpy.data.materials.new("Surface")
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    return material


def make_target_object():
    bpy.ops.mesh.primitive_cube_add(size=1.2, location=(0.0, 0.0, 0.0))
    target = bpy.context.active_object
    target.name = "MaskTarget"
    target.data.materials.append(make_surface_material())
    return target


def make_filter_material(target):
    material = bpy.data.materials.new("FilterObjectMask")
    material.use_nodes = True
    material.eevee_domain = "FILTER"

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputFilter")
    output.location = (520.0, 0.0)
    output.inputs["Alpha"].default_value = 1.0

    object_mask = nodes.new("ShaderNodeFilterObjectMask")
    object_mask.location = (0.0, 0.0)
    object_mask.object = target

    combine = nodes.new("ShaderNodeCombineColor")
    combine.location = (260.0, 0.0)

    links.new(object_mask.outputs["Mask"], combine.inputs["Red"])
    links.new(combine.outputs["Color"], output.inputs["Color"])

    return material


def attach_filter_material(material):
    entry = bpy.context.scene.eevee.filter_materials.add()
    entry.material = material
    entry.enabled = True
    entry.execution_stage = "BEFORE_POSTFX"


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


def sample_red(pixels, x, y):
    index = (y * RESOLUTION + x) * 4
    return pixels[index]


def main():
    clear_scene()
    configure_scene()
    make_camera()
    target = make_target_object()
    attach_filter_material(make_filter_material(target))

    bpy.context.view_layer.update()
    pixels_initial = render_image()

    red_center_initial = sample_red(pixels_initial, RESOLUTION // 2, RESOLUTION // 2)
    red_corner_initial = sample_red(pixels_initial, 4, 4)

    target.location.x = 1.8
    bpy.context.view_layer.update()
    pixels_moved = render_image()
    red_center_moved = sample_red(pixels_moved, RESOLUTION // 2, RESOLUTION // 2)

    print(f"FILTER_OBJECT_MASK_RED_CENTER_INITIAL={red_center_initial:.6f}")
    print(f"FILTER_OBJECT_MASK_RED_CORNER_INITIAL={red_corner_initial:.6f}")
    print(f"FILTER_OBJECT_MASK_RED_CENTER_MOVED={red_center_moved:.6f}")

    assert red_center_initial > 0.8, (
        f"Expected the selected object to be masked at the center, got {red_center_initial}"
    )
    assert red_corner_initial < 0.1, (
        f"Expected pixels outside the selected object to stay dark, got {red_corner_initial}"
    )
    assert red_center_moved < 0.1, (
        f"Expected the mask to follow the moved object, got {red_center_moved}"
    )


if __name__ == "__main__":
    main()
