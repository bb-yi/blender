# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import math
import os
from pathlib import Path
from tempfile import TemporaryDirectory

import bpy


PASSES = {
    "Diffuse Direct": "use_pass_diffuse_direct",
    "Diffuse Color": "use_pass_diffuse_color",
    "Glossy Direct": "use_pass_glossy_direct",
    "Glossy Color": "use_pass_glossy_color",
}
SIZE = 96


def add_plane(name, size, z):
    bpy.ops.mesh.primitive_plane_add(size=size, location=(0.0, 0.0, z))
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    bpy.context.object.data.materials.append(material)
    tree = material.node_tree
    tree.nodes.clear()
    return material, tree, tree.nodes.new("ShaderNodeOutputMaterial")


def setup_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = SIZE
    scene.render.resolution_y = SIZE
    scene.render.resolution_percentage = 100
    scene.render.use_compositing = True
    scene.eevee.taa_render_samples = 8
    scene.eevee.use_raytracing = True
    scene.world.use_nodes = True
    scene.world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.0

    bpy.ops.object.camera_add(location=(0.0, 0.0, 5.0))
    scene.camera = bpy.context.object
    scene.camera.data.type = "ORTHO"
    scene.camera.data.ortho_scale = 5.0

    bpy.ops.object.light_add(type="SUN", rotation=(0.0, math.radians(60.0), 0.0))
    bpy.context.object.data.energy = 1.0

    _, tree, output = add_plane("Red background", 5.0, 0.0)
    shader = tree.nodes.new("ShaderNodeBsdfPrincipled")
    shader.inputs["Base Color"].default_value = (0.8, 0.05, 0.02, 1.0)
    shader.inputs["Roughness"].default_value = 0.8
    tree.links.new(shader.outputs[0], output.inputs["Surface"])

    front, tree, output = add_plane("Opaque blue toon surface", 2.0, 1.0)
    diffuse = tree.nodes.new("ShaderNodeBsdfDiffuse")
    diffuse.inputs["Color"].default_value = (0.08, 0.12, 0.2, 1.0)
    to_rgb = tree.nodes.new("ShaderNodeShaderToRGB")
    tree.links.new(diffuse.outputs[0], to_rgb.inputs[0])
    tree.links.new(to_rgb.outputs["Color"], output.inputs["Surface"])
    front.use_raytrace_refraction = True
    return scene


def read_pixels(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        center = 4 * ((SIZE // 2) * SIZE + SIZE // 2)
        background = 4 * ((SIZE // 2) * SIZE + 9 * SIZE // 10)
        return tuple(image.pixels[center:center + 4]), tuple(image.pixels[background:background + 4])
    finally:
        bpy.data.images.remove(image)


def render(scene, directory, requested_passes):
    view_layer = scene.view_layers[0]
    for name, property_name in PASSES.items():
        setattr(view_layer, property_name, name in requested_passes)

    tree = bpy.data.node_groups.new("Layered lighting passes", "CompositorNodeTree")
    old_tree = scene.compositing_node_group
    scene.compositing_node_group = tree
    if old_tree:
        bpy.data.node_groups.remove(old_tree)
    layers = tree.nodes.new("CompositorNodeRLayers")
    directory.mkdir()
    for name in ("Image", *requested_passes):
        output = tree.nodes.new("CompositorNodeOutputFile")
        output.directory = str(directory)
        output.file_name = name
        output.format.media_type = "IMAGE"
        output.format.file_format = "OPEN_EXR"
        output.format.color_mode = "RGBA"
        output.format.color_depth = "32"
        output.file_output_items.clear()
        output.file_output_items.new("RGBA", name)
        tree.links.new(layers.outputs[name], output.inputs[name])

    assert bpy.ops.render.render() == {"FINISHED"}
    return {
        name: read_pixels(next(directory.glob(f"{name}*.exr")))
        for name in ("Image", *requested_passes)
    }


def test_refraction_light_passes(output_dir):
    scene = setup_scene()
    reference = render(scene, output_dir / "combined", ())["Image"]
    assert reference[0][2] > reference[0][0] > 0.005, reference[0]
    assert abs(reference[0][3] - 1.0) < 0.001, reference[0]
    assert reference[1][0] > 0.05, reference[1]

    for label, requested in (("diffuse_direct", ("Diffuse Direct",)),
                             ("diffuse_color", ("Diffuse Color",)),
                             ("all_passes", tuple(PASSES))):
        images = render(scene, output_dir / label, requested)
        for actual, expected in zip(images["Image"], reference):
            error = max(abs(a - b) for a, b in zip(actual, expected))
            assert error < 0.002, f"{label}: Combined changed by {error}: {actual} != {expected}"
        for name in requested:
            front, background = images[name]
            assert max(front[:3]) < 0.002, f"{label}: {name} leaked onto the surface: {front}"
            assert max(background[:3]) > 0.002, f"{label}: lost background {name}: {background}"

    tree = bpy.data.materials["Opaque blue toon surface"].node_tree
    surface = next(node for node in tree.nodes if node.type == "OUTPUT_MATERIAL")
    diffuse = next(node for node in tree.nodes if node.type == "BSDF_DIFFUSE")
    lighting = tree.nodes.new("ShaderNodeLightEvaluation")
    accumulate = tree.nodes.new("ShaderNodeLightAccumulation")
    add = tree.nodes.new("ShaderNodeAddShader")
    tree.links.new(lighting.outputs["Factor"], accumulate.inputs["Diffuse Light"])
    accumulate.inputs["Diffuse Color"].default_value = (0.1, 0.8, 0.3, 1.0)
    tree.links.new(accumulate.outputs["Shader"], add.inputs[0])
    tree.links.new(diffuse.outputs[0], add.inputs[1])
    tree.links.new(add.outputs[0], surface.inputs["Surface"])

    native_reference = render(scene, output_dir / "native_combined", ())["Image"][0]
    native_passes = render(scene, output_dir / "native_diffuse", ("Diffuse Direct", "Diffuse Color"))
    native_combined = native_passes["Image"][0]
    error = max(abs(a - b) for a, b in zip(native_combined, native_reference))
    assert error < 0.002, f"Native foreground: Combined changed by {error}"
    assert native_passes["Diffuse Direct"][0][1] > 0.05, native_passes["Diffuse Direct"][0]
    print("DEFERRED_REFRACTION_LIGHT_PASSES_PASS", flush=True)


if __name__ == "__main__":
    output_dir = Path(os.environ["BLENDER_TEST_OUTPUT_DIR"])
    output_dir.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(dir=output_dir) as run_dir:
        test_refraction_light_passes(Path(run_dir))
