import os
import tempfile
from pathlib import Path

import bpy


def render_center(scene, path):
    bpy.ops.render.render()
    bpy.data.images["Render Result"].save_render(str(path), scene=scene)
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        width, height = image.size
        index = ((height // 2) * width + width // 2) * 4
        return tuple(image.pixels[index:index + 4])
    finally:
        bpy.data.images.remove(image)


def assert_pixel(label, actual, expected, tolerance=0.005):
    print(f"NPR Emission {label}: {actual} (expected {expected})", flush=True)
    assert all(abs(value - target) < tolerance for value, target in zip(actual, expected)), label


def run(path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.world = None
    scene.render.engine = 'BLENDER_EEVEE'
    scene.render.resolution_x = scene.render.resolution_y = 64
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.render.image_settings.color_mode = 'RGBA'
    scene.render.image_settings.color_depth = '32'
    scene.render.film_transparent = True
    scene.eevee.taa_render_samples = 1
    view_layer = bpy.context.view_layer
    view_layer.use_pass_emit = False

    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = 'ORTHO'
    camera_data.ortho_scale = 4.0
    camera = bpy.data.objects.new("Camera", camera_data)
    scene.collection.objects.link(camera)
    camera.location.z = 4.0
    scene.camera = camera

    light_data = bpy.data.lights.new("Sun", 'SUN')
    light_data.energy = 2.0
    scene.collection.objects.link(bpy.data.objects.new("Sun", light_data))

    material = bpy.data.materials.new("Surface with Emission")
    surface_tree = material.node_tree
    surface_tree.nodes.clear()
    diffuse = surface_tree.nodes.new("ShaderNodeBsdfDiffuse")
    diffuse.inputs["Color"].default_value = (0.0, 0.75, 0.0, 1.0)
    expected_emission = (1.25, 0.0, 0.25, 1.0)
    emission = surface_tree.nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = expected_emission
    add = surface_tree.nodes.new("ShaderNodeAddShader")
    output = surface_tree.nodes.new("ShaderNodeOutputMaterial")
    surface_tree.links.new(diffuse.outputs[0], add.inputs[0])
    surface_tree.links.new(emission.outputs[0], add.inputs[1])
    surface_tree.links.new(add.outputs[0], output.inputs["Surface"])
    bpy.ops.mesh.primitive_plane_add(size=2.0)
    bpy.context.object.data.materials.append(material)

    original = render_center(scene, path)
    assert original[1] > 0.1, f"Expected diffuse lighting, got {original}"

    npr_tree = bpy.data.node_groups.new("Emission Test", 'ShaderNodeTree')
    npr_input = npr_tree.nodes.new("ShaderNodeNPR_Input")
    npr_output = npr_tree.nodes.new("ShaderNodeNPR_Output")
    destination = npr_output.inputs["Color"]
    npr_tree.links.new(npr_input.outputs["Combined Color"], destination)
    output.nprtree = npr_tree
    assert_pixel("Combined Color", render_center(scene, path), original)

    emission_socket = npr_input.outputs.get("Emission")
    assert emission_socket is not None, "NPR Input must expose Emission"
    npr_tree.links.new(emission_socket, destination)
    independent = render_center(scene, path)
    assert_pixel("Emission with render pass disabled", independent, expected_emission)

    view_layer.use_pass_emit = True
    assert_pixel("Emission with render pass enabled", render_center(scene, path), independent)
    view_layer.use_pass_emit = False

    sample = npr_tree.nodes.new("ShaderNodeNPR_ImageSample")
    npr_tree.links.new(emission_socket, sample.inputs["Image"])
    npr_tree.links.new(sample.outputs["Color"], destination)
    offset = npr_tree.nodes.new("ShaderNodeCombineXYZ")
    npr_tree.links.new(offset.outputs[0], sample.inputs["Offset"])
    sample.offset_type = 'PIXEL'
    offset.inputs["X"].default_value = 4.0
    assert_pixel("Pixel offset", render_center(scene, path), independent)
    sample.offset_type = 'UV'
    offset.inputs["X"].default_value = 0.25
    offset.inputs["Y"].default_value = 0.5
    assert_pixel("UV sample", render_center(scene, path), independent)
    offset.inputs["X"].default_value = 0.875
    assert_pixel("UV sample outside the surface", render_center(scene, path), (0.0, 0.0, 0.0, 1.0))

    npr_tree.links.new(npr_input.outputs["Diffuse Direct"], destination)
    direct = render_center(scene, path)
    print(f"NPR Diffuse Direct: {direct}", flush=True)
    assert min(direct[:3]) > 0.1, f"Expected nonzero diffuse direct light, got {direct}"

    diffuse_lighting = npr_tree.nodes.new("ShaderNodeMixRGB")
    diffuse_lighting.blend_type = 'MULTIPLY'
    diffuse_lighting.inputs[0].default_value = 1.0
    for index, name in enumerate(("Diffuse Color", "Diffuse Direct"), start=1):
        channel = npr_tree.nodes.new("ShaderNodeNPR_ImageSample")
        npr_tree.links.new(npr_input.outputs[name], channel.inputs["Image"])
        npr_tree.links.new(channel.outputs["Color"], diffuse_lighting.inputs[index])
    recombine = npr_tree.nodes.new("ShaderNodeMixRGB")
    recombine.blend_type = 'ADD'
    recombine.inputs[0].default_value = 1.0
    npr_tree.links.new(diffuse_lighting.outputs[0], recombine.inputs[1])
    npr_tree.links.new(emission_socket, sample.inputs["Image"])
    npr_tree.links.new(sample.outputs["Color"], recombine.inputs[2])
    sample.offset_type = 'PIXEL'
    offset.inputs["X"].default_value = offset.inputs["Y"].default_value = 0.0
    npr_tree.links.new(recombine.outputs[0], destination)
    assert_pixel("Recombined lighting and Emission", render_center(scene, path), original)

    npr_tree.links.new(emission_socket, destination)
    emission.inputs["Strength"].default_value = 0.0
    assert_pixel("Zero emission", render_center(scene, path), (0.0, 0.0, 0.0, 1.0))
    assert [socket.identifier for socket in npr_input.outputs][-3:] == ["Emission", "Position", "Normal"]


with tempfile.TemporaryDirectory(dir=os.environ.get("BLENDER_TEST_OUTPUT_DIR")) as directory:
    run(Path(directory) / "npr-emission.exr")

print("NPR_INPUT_EMISSION_PASS")
