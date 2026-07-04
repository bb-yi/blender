import math
import os
import tempfile

import bpy


RESOLUTION = 96


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def configure_scene():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = RESOLUTION
    scene.render.resolution_y = RESOLUTION
    scene.render.resolution_percentage = 100
    scene.eevee.taa_samples = 4
    scene.eevee.taa_render_samples = 4
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    scene.world.use_nodes = False
    scene.world.color = (0.0, 0.0, 0.0)


def make_camera():
    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 2.4
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 4.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def make_plane(name, x_location, material):
    bpy.ops.mesh.primitive_plane_add(size=0.65, location=(x_location, 0.0, 0.0))
    plane = bpy.context.active_object
    plane.name = name
    plane.data.materials.append(material)


def make_output_material(name, render_info_output):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    render_info = nodes.new("ShaderNodeRenderInfo")

    links.new(render_info.outputs[render_info_output], emission.inputs["Strength"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_white_noise_material():
    material = bpy.data.materials.new("CurrentSampleWhiteNoise")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    render_info = nodes.new("ShaderNodeRenderInfo")
    white_noise = nodes.new("ShaderNodeTexWhiteNoise")
    white_noise.noise_dimensions = "4D"

    links.new(render_info.outputs["Current Sample"], white_noise.inputs["W"])
    links.new(white_noise.outputs["Value"], emission.inputs["Strength"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


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


def sample_pixel(pixels, width, height, x_ratio, y_ratio):
    x = min(width - 1, max(0, int(width * x_ratio)))
    y = min(height - 1, max(0, int(height * y_ratio)))
    pixel_index = (y * width + x) * 4
    return list(pixels[pixel_index:pixel_index + 4])


def assert_close(value, expected, tolerance, label):
    assert abs(value - expected) <= tolerance, (
        f"{label} expected {expected} +/- {tolerance}, got {value}"
    )


clear_scene()
configure_scene()
make_camera()
make_plane("CurrentSamplePlane", -0.72, make_output_material("CurrentSample", "Current Sample"))
make_plane("TotalSamplesPlane", 0.0, make_output_material("TotalSamples", "Total Samples"))
make_plane("WhiteNoisePlane", 0.72, make_white_noise_material())

pixels, width, height = render_image()

current_sample = sample_pixel(pixels, width, height, 0.2, 0.5)[0]
total_samples = sample_pixel(pixels, width, height, 0.5, 0.5)[0]
white_noise = sample_pixel(pixels, width, height, 0.8, 0.5)[0]

print(
    "Render Info sampling values: "
    f"current_sample={current_sample:.6f}, "
    f"total_samples={total_samples:.6f}, "
    f"white_noise={white_noise:.6f}"
)

assert_close(current_sample, 1.5, 0.2, "Current Sample accumulation")
assert_close(total_samples, 4.0, 0.2, "Total Samples output")
assert math.isfinite(white_noise), f"White Noise output must be finite, got {white_noise}"
assert 0.0 <= white_noise <= 1.0, f"White Noise output should stay in [0, 1], got {white_noise}"
