# SPDX-License-Identifier: GPL-2.0-or-later
"""Outline Shell visibility and Principled NPR integration regressions.

Run with an isolated Blender user directory on each GPU backend. The caller sets
OUTPUT_DIR when importing the test case, or passes --output-dir when running it.
"""
import argparse
import math
from pathlib import Path
import sys
import unittest

import bpy
import numpy as np

OUTPUT_DIR = None
RES = 128


def scene_setup():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    scene.render.resolution_x = scene.render.resolution_y = RES
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.render.image_settings.color_mode = 'RGBA'
    scene.render.image_settings.color_depth = '32'
    scene.eevee.taa_render_samples = 16
    world = bpy.data.worlds.new("World")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs[1].default_value = 0
    scene.world = world
    camera = bpy.data.objects.new("Camera", bpy.data.cameras.new("Camera"))
    scene.collection.objects.link(camera)
    camera.location = (0, -3, 0)
    camera.rotation_euler = (math.pi / 2, 0, 0)
    camera.data.type = 'ORTHO'
    camera.data.ortho_scale = 3
    scene.camera = camera
    sun = bpy.data.objects.new("Sun", bpy.data.lights.new("Sun", 'SUN'))
    scene.collection.objects.link(sun)
    sun.rotation_euler = (math.pi / 2, 0, 0)
    sun.data.energy = 3
    sun.data.angle = 0
    return scene


def sphere():
    bpy.ops.mesh.primitive_uv_sphere_add(segments=64, ring_count=32)
    obj = bpy.context.object
    for polygon in obj.data.polygons:
        polygon.use_smooth = True
    return obj


def material(obj):
    mat = bpy.data.materials.new("Body")
    mat.use_nodes = True
    obj.data.materials.append(mat)
    mat.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (1, 1, 1, 1)
    mat.node_tree.nodes["Principled BSDF"].inputs["Roughness"].default_value = 1
    return mat


def render(label):
    path = Path(OUTPUT_DIR) / (label + '.exr')
    path.parent.mkdir(parents=True, exist_ok=True)
    bpy.context.scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)
    image = bpy.data.images.load(str(path), check_existing=False)
    pixels = np.array(image.pixels[:], dtype=np.float32).reshape(RES, RES, 4)
    bpy.data.images.remove(image)
    assert np.isfinite(pixels).all(), label
    return pixels


def pixel(pixels, x, z=0):
    return pixels[int((z / 3 + 0.5) * RES), int((x / 3 + 0.5) * RES), :3]


class OutlineShellIntegrationTest(unittest.TestCase):
    def test_camera_hidden_shadow(self):
        scene_setup()
        caster = sphere()
        caster.location = (0, -0.7, 0)
        caster.scale = (0.4, 0.4, 0.4)
        caster.visible_camera = False
        mat = material(caster)
        shell = mat.node_tree.nodes.new("ShaderNodeOutputOutlineShell")
        shell.inputs["Strength"].default_value = 0.3
        # Exercise mesh attributes required only by the shadow variant as well.
        directions = caster.data.attributes.new("shell_direction", 'FLOAT_VECTOR', 'POINT')
        for value, vertex in zip(directions.data, caster.data.vertices):
            value.vector = vertex.co.normalized()
        normal = mat.node_tree.nodes.new("ShaderNodeAttribute")
        normal.attribute_name = "shell_direction"
        mat.node_tree.links.new(normal.outputs["Vector"], shell.inputs["Displacement"])
        bpy.ops.mesh.primitive_plane_add(size=6, location=(0, 0.5, 0),
                                        rotation=(math.pi / 2, 0, 0))
        material(bpy.context.object)
        off = render("hidden_shadow_off")
        mat.use_outline_shell_shadow = True
        on = render("hidden_shadow_on")
        self.assertGreater(float(pixel(off, .55).min()), .3)
        self.assertLess(float(pixel(on, .55).max()), float(pixel(off, .55).min()) * .5)
        caster.visible_shadow = False
        disabled = render("hidden_object_shadow_off")
        self.assertGreater(float(pixel(disabled, 0).min()), .3)
        self.assertLess(float(np.abs(pixel(disabled, .55) - pixel(off, .55)).max()), .02)

    def test_principled_and_diffusion_coexistence(self):
        for diffusion in (False, True):
            with self.subTest(diffusion=diffusion):
                scene_setup()
                mat = material(sphere())
                tree = mat.node_tree
                output = tree.nodes["Material Output"]
                tree.nodes.remove(tree.nodes["Principled BSDF"])
                npr = tree.nodes.new("ShaderNodePrincipledNPR")
                npr.shadow_mode = 'NONE'
                for identifier, value in (("highlight_strength", 0), ("reflection_strength", 0),
                                          ("ambient_strength", 0), ("rim_strength", 0)):
                    next(s for s in npr.inputs if s.identifier == identifier).default_value = value
                surface = npr.outputs["Shader"]
                if diffusion:
                    diffuse = tree.nodes.new("ShaderNodeNPRSurfaceDiffusion")
                    tree.links.new(surface, diffuse.inputs[0])
                    surface = diffuse.outputs[0]
                tree.links.new(surface, output.inputs["Surface"])
                baseline = render(f"npr_{diffusion}_baseline")
                self.assertGreater(float(pixel(baseline, 0).max()), .05)
                shell = tree.nodes.new("ShaderNodeOutputOutlineShell")
                shell.inputs["Strength"].default_value = .15
                shell.inputs["Color"].default_value = (1, 0, 0, 1)
                for route in ('DEFERRED', 'FORWARD'):
                    mat.outline_shell_render_method = route
                    pixels = render(f"npr_{diffusion}_{route}")
                    center_delta = np.abs(pixels[48:80, 48:80] - baseline[48:80, 48:80])
                    self.assertLess(float(center_delta.max()), .02)
                    self.assertLess(float(np.abs(pixel(pixels, 1.075) - (1, 0, 0)).max()), .02)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    OUTPUT_DIR = args.output_dir
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(OutlineShellIntegrationTest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        raise AssertionError('Outline Shell integration regressions failed')
