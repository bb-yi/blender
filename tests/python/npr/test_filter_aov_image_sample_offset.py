import bpy
import os
import tempfile


RESOLUTION = 64
AOV_NAME = "OffsetAOV"


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

    view_layer = bpy.context.view_layer
    while len(view_layer.aovs) > 0:
        view_layer.aovs.remove(view_layer.aovs[0])
    aov = view_layer.aovs.add()
    aov.name = AOV_NAME
    aov.type = "COLOR"


def make_camera():
    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 4.0
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 5.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def make_aov_source_material():
    material = bpy.data.materials.new("AOVSource")
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    tex_coord = nodes.new("ShaderNodeTexCoord")
    separate = nodes.new("ShaderNodeSeparateXYZ")
    threshold = nodes.new("ShaderNodeMath")
    threshold.operation = "GREATER_THAN"
    threshold.inputs[1].default_value = 0.6
    combine = nodes.new("ShaderNodeCombineColor")
    aov_output = nodes.new("ShaderNodeOutputAOV")
    aov_output.aov_name = AOV_NAME

    links.new(tex_coord.outputs["Generated"], separate.inputs["Vector"])
    links.new(separate.outputs["X"], threshold.inputs[0])
    links.new(threshold.outputs["Value"], combine.inputs["Red"])
    links.new(combine.outputs["Color"], aov_output.inputs["Color"])

    return material


def make_plane(material):
    bpy.ops.mesh.primitive_plane_add(size=4.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.active_object
    plane.data.materials.append(material)
    return plane


def make_filter_material(offset_x):
    material = bpy.data.materials.new("FilterAOVImageSampleOffset")
    material.use_nodes = True
    material.eevee_domain = "FILTER"

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputFilter")
    output.inputs["Alpha"].default_value = 1.0

    aov_input = nodes.new("ShaderNodeInputAOV")
    aov_input.aov_name = AOV_NAME

    image_sample = nodes.new("ShaderNodeNPR_ImageSample")
    image_sample.offset_type = "PIXEL"

    offset = nodes.new("ShaderNodeCombineXYZ")
    offset.inputs["X"].default_value = offset_x

    links.new(aov_input.outputs["Color"], image_sample.inputs["Image"])
    links.new(offset.outputs["Vector"], image_sample.inputs["Offset"])
    links.new(image_sample.outputs["Color"], output.inputs["Color"])

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


def sample_color(pixels, x, y):
    index = (y * RESOLUTION + x) * 4
    return list(pixels[index:index + 4])


def main():
    clear_scene()
    configure_scene()
    make_camera()
    make_plane(make_aov_source_material())
    attach_filter_material(make_filter_material(10.0))

    bpy.context.view_layer.update()
    pixels = render_image()

    center = sample_color(pixels, RESOLUTION // 2, RESOLUTION // 2)
    left = sample_color(pixels, RESOLUTION // 2 - 12, RESOLUTION // 2)

    print(f"FILTER_AOV_IMAGE_SAMPLE_OFFSET_CENTER={center}")
    print(f"FILTER_AOV_IMAGE_SAMPLE_OFFSET_LEFT={left}")

    assert center[0] > 0.8, (
        f"Expected center pixel to sample the red AOV region after positive pixel offset, got {center}"
    )
    assert left[0] < 0.2, (
        f"Expected left-side pixel to remain outside the red AOV region, got {left}"
    )
    assert center[1] < 0.2 and center[2] < 0.2, (
        f"Expected offset AOV sample to stay red-only, got {center}"
    )


if __name__ == "__main__":
    main()
