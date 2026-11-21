# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import math
import os
from pathlib import Path
import sys
from tempfile import TemporaryDirectory

import bpy
import gpu

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_eevee_deferred_refraction_light_passes import render


GLOSSY = (0.75, 0.35, 0.15)
TRANSMISSION = (0.10, 0.20, 0.40)


def assert_rgb(label, actual, expected, tolerance=0.003):
    assert all(math.isfinite(x) and abs(x - y) < tolerance
               for x, y in zip(actual[:3], expected[:3])), (label, actual, expected)


def setup_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = scene.render.resolution_y = 96
    scene.render.resolution_percentage = 100
    scene.render.use_compositing = True
    scene.eevee.taa_render_samples = 4
    scene.world.use_nodes = True
    scene.world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.0
    bpy.ops.object.camera_add(location=(0.0, 0.0, 4.0))
    scene.camera = bpy.context.object
    scene.camera.data.type = "ORTHO"
    scene.camera.data.ortho_scale = 3.0
    bpy.ops.object.light_add(type="POINT", location=(0.0, 0.0, 2.0))
    light = bpy.context.object
    light.data.energy = 120.0
    light.data.use_shadow = False
    bpy.ops.mesh.primitive_plane_add(size=2.0)
    material = bpy.data.materials.new("Light Accumulation Compatibility")
    material.use_nodes = True
    bpy.context.object.data.materials.append(material)
    return scene, light, material


def set_graph(material, lobes, *, explicit_color=False, use_factor=False):
    tree = material.node_tree
    tree.nodes.clear()
    output = tree.nodes.new("ShaderNodeOutputMaterial")
    accumulation = tree.nodes.new("ShaderNodeLightAccumulation")
    tree.links.new(accumulation.outputs[0], output.inputs["Surface"])
    for lobe, color in lobes.items():
        accumulation.inputs[lobe + " Color"].default_value = (*color, 1.0)
        if use_factor:
            node = tree.nodes.new("ShaderNodeLightEvaluation")
            signal = node.outputs["Factor"]
            if explicit_color:
                info = tree.nodes.new("ShaderNodeLightInfo")
                scale = tree.nodes.new("ShaderNodeVectorMath")
                scale.operation = "SCALE"
                tree.links.new(info.outputs["Color"], scale.inputs[0])
                tree.links.new(signal, scale.inputs["Scale"])
                signal = scale.outputs[0]
        else:
            node = tree.nodes.new("ShaderNodeValue")
            node.outputs[0].default_value = 0.5
            signal = node.outputs[0]
        tree.links.new(signal, accumulation.inputs[lobe + " Light"])


def test_passes(scene, material, directory):
    passes = ("Diffuse Direct", "Diffuse Color", "Glossy Direct", "Glossy Color")
    cases = ({"Glossy": GLOSSY}, {"Transmission": TRANSMISSION},
             {"Glossy": GLOSSY, "Transmission": TRANSMISSION})
    for i, lobes in enumerate(cases):
        set_graph(material, lobes)
        values = render(scene, directory / f"pass-{i}", passes)
        pixel = {key: value[0] for key, value in values.items()}
        expected = tuple(sum(c[channel] for c in lobes.values()) for channel in range(3))
        print("LIGHT_ACCUMULATION_PASSES", tuple(lobes), pixel, flush=True)
        assert_rgb("Glossy Color", pixel["Glossy Color"], expected)
        assert_rgb("Unused Diffuse Color", pixel["Diffuse Color"], (0, 0, 0))
        assert max(pixel["Image"][:3]) > 0.05, pixel
        assert_rgb("Reconstructed Combined",
                   tuple(c * d for c, d in zip(pixel["Glossy Color"], pixel["Glossy Direct"])),
                   pixel["Image"])
        combined = render(scene, directory / f"combined-{i}", ())["Image"][0]
        assert_rgb("Combined with/without passes", pixel["Image"], combined)


def test_light_shader(scene, light, material, directory):
    light.data.use_nodes = True
    tree = light.data.node_tree
    output = next(n for n in tree.nodes if n.bl_idname == "ShaderNodeEeveeLightShaderOutput")
    for socket in output.inputs:
        for link in list(socket.links):
            tree.links.remove(link)
    color = output.inputs["Color"]
    color.default_value = (0.25, 0.8, 0.45, 1.0)
    output.inputs["Intensity"].default_value = 0.8
    output.inputs["Attenuation"].default_value = 0.5
    ramp = tree.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].color = (0.9, 0.1, 0.05, 1.0)
    ramp.color_ramp.elements[1].color = (0.1, 0.7, 0.2, 1.0)
    scale = tree.nodes.new("ShaderNodeMath")
    scale.operation = "MULTIPLY"
    scale.inputs[1].default_value = 0.35
    info = tree.nodes.new("ShaderNodeEeveeLightShaderInfo")
    tree.links.new(info.outputs["Distance"], scale.inputs[0])
    tree.links.new(scale.outputs[0], ramp.inputs[0])

    for mode in ("DITHERED", "BLENDED"):
        material.surface_render_method = mode
        for point_dependent in (False, True):
            if point_dependent:
                tree.links.new(ramp.outputs[0], color)
            label = f"{mode}-{point_dependent}"
            diffuse = {"Diffuse": (1.0, 1.0, 1.0)}
            set_graph(material, diffuse, use_factor=True)
            implicit = render(scene, directory / f"implicit-{label}", ())["Image"][0]
            set_graph(material, diffuse, explicit_color=True, use_factor=True)
            explicit = render(scene, directory / f"explicit-{label}", ())["Image"][0]
            print("LIGHT_ACCUMULATION_LIGHT_SHADER", label, implicit, explicit, flush=True)
            assert implicit[1] > implicit[0] * 1.2 > 0.02, implicit
            assert_rgb("Explicit evaluated color", explicit, implicit)
            output.inputs["Attenuation"].default_value = 1.0
            full = render(scene, directory / f"full-{label}", ())["Image"][0]
            assert_rgb("Light Shader Output attenuation", full, tuple(2 * v for v in implicit))
            output.inputs["Attenuation"].default_value = 0.5
            if point_dependent:
                tree.links.remove(color.links[0])


def test_bake(scene, light, material):
    # A light outside the camera frame still has to reach the UV bake target.
    light.location.x = 3.0
    light.data.use_custom_distance = True
    light.data.cutoff_distance = 10.0
    material.surface_render_method = "DITHERED"
    tree = material.node_tree
    set_graph(material, {"Diffuse": GLOSSY, "Glossy": TRANSMISSION, "Transmission": GLOSSY})
    expected = tuple(0.5 * (2 * g + t) for g, t in zip(GLOSSY, TRANSMISSION))
    image = bpy.data.images.new("Native Accumulation Bake", width=16, height=16,
                                alpha=True, float_buffer=True)
    target = tree.nodes.new("ShaderNodeTexImage")
    target.image = image
    tree.nodes.active = target
    scene.render.bake.target = "IMAGE_TEXTURES"
    index = 4 * (16 * 8 + 8)
    light_tree = light.data.node_tree
    output = next(n for n in light_tree.nodes if n.bl_idname == "ShaderNodeEeveeLightShaderOutput")
    info = next(n for n in light_tree.nodes if n.bl_idname == "ShaderNodeEeveeLightShaderInfo")
    for output_name, input_name in (("Default Color", "Color"),
                                    ("Default Intensity", "Intensity"),
                                    ("Default Attenuation", "Attenuation")):
        light_tree.links.new(info.outputs[output_name], output.inputs[input_name])
    assert bpy.ops.object.bake(type="EMIT") == {"FINISHED"}
    actual = tuple(image.pixels[index:index + 4])
    print("LIGHT_ACCUMULATION_BAKE", actual, expected, flush=True)
    assert_rgb("UV-space color bake", actual, expected)
    assert abs(actual[3] - 1.0) < 0.002, actual

    # Feed actual bake surface distance through a threshold. The camera sees none of this light.
    threshold = light_tree.nodes.new("ShaderNodeMath")
    threshold.operation = "GREATER_THAN"
    threshold.inputs[1].default_value = 3.0
    light_tree.links.new(info.outputs["Distance"], threshold.inputs[0])
    light_tree.links.new(threshold.outputs[0], output.inputs["Attenuation"])
    light_tree.links.remove(output.inputs["Color"].links[0])
    output.inputs["Color"].default_value = (0.2, 0.5, 0.9, 1.0)
    assert bpy.ops.object.bake(type="EMIT") == {"FINISHED"}
    actual = tuple(image.pixels[index:index + 4])
    expected = tuple(color * light_color for color, light_color in zip(expected, (0.2, 0.5, 0.9)))
    print("LIGHT_ACCUMULATION_BAKE_LIGHT_SHADER", actual, expected, flush=True)
    assert_rgb("UV-space evaluated light shader", actual, expected)


if __name__ == "__main__":
    gpu.init()
    backend = sys.argv[sys.argv.index("--gpu-backend") + 1].upper()
    assert gpu.platform.backend_type_get() == backend
    print("LIGHT_ACCUMULATION_BACKEND", backend, gpu.platform.renderer_get(), flush=True)
    output_dir = Path(os.environ["BLENDER_TEST_OUTPUT_DIR"])
    output_dir.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(dir=output_dir) as result:
        directory = Path(result)
        scene, light, material = setup_scene()
        test_passes(scene, material, directory)
        test_light_shader(scene, light, material, directory)
        test_bake(scene, light, material)
