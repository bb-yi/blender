# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Deferred shell ownership and immutable NPR background across refraction layers.

Run with isolated user resources and BLENDER_TEST_OUTPUT_DIR. The Release case
runs the full pixel suite serially on OpenGL and Vulkan.
"""

import argparse
import json
import os
from pathlib import Path
import sys
import unittest

import bpy
import gpu
import numpy as np


OUT = Path(os.environ['BLENDER_TEST_OUTPUT_DIR'])
OUT.mkdir(parents=True, exist_ok=True)
RES = 128
BACKGROUND = (0.05, 0.65, 0.15)
RED = (1.0, 0.0, 0.0)
BLUE = (0.0, 0.0, 1.0)
RENDERS = []


def emission_material(name, color, *, refractive=False, outline=None):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    mat.surface_render_method = 'DITHERED'
    mat.use_raytrace_refraction = refractive
    tree = mat.node_tree
    tree.nodes.clear()
    output = tree.nodes.new('ShaderNodeOutputMaterial')
    emission = tree.nodes.new('ShaderNodeEmission')
    emission.inputs['Color'].default_value = (*color, 1)
    tree.links.new(emission.outputs[0], output.inputs['Surface'])
    if refractive:
        npr = bpy.data.node_groups.new(name + ' NPR', 'ShaderNodeTree')
        source = npr.nodes.new('ShaderNodeNPR_Refraction')
        target = npr.nodes.new('ShaderNodeNPR_Output')
        npr.links.new(source.outputs['Combined Color'], target.inputs['Color'])
        output.nprtree = npr
    if outline is not None:
        shell = tree.nodes.new('ShaderNodeOutputOutlineShell')
        shell.inputs['Color'].default_value = (*outline, 1)
        shell.inputs['Alpha'].default_value = 1
        shell.inputs['Strength'].default_value = .08
        mat.outline_shell_render_method = 'DEFERRED'
        mat.outline_shell_cull_method = 'FRONT'
        mat.use_outline_shell_depth_write = True
    return mat


def plane(name, mat, z=0, size=3, layer=0):
    bpy.ops.mesh.primitive_plane_add(size=size, location=(0, 0, z))
    obj = bpy.context.object
    obj.name = name
    obj.data.materials.append(mat)
    obj.refraction_layer_index = layer
    return obj


def sphere(name, mat, *, location=(0, 0, 0), radius=.7, layer=0):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=64, ring_count=32, radius=radius,
                                       location=location)
    obj = bpy.context.object
    obj.name = name
    obj.data.materials.append(mat)
    obj.refraction_layer_index = layer
    for polygon in obj.data.polygons:
        polygon.use_smooth = True
    return obj


def shell_node(mat):
    return next(n for n in mat.node_tree.nodes if n.bl_idname == 'ShaderNodeOutputOutlineShell')


def setup_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    scene.render.resolution_x = scene.render.resolution_y = RES
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.render.image_settings.color_depth = '32'
    scene.render.image_settings.color_mode = 'RGBA'
    scene.eevee.taa_render_samples = 1
    scene.eevee.use_taa_reprojection = False
    scene.eevee.use_raytracing = True
    scene.eevee.ray_tracing_method = 'SCREEN'
    scene.eevee.use_outline = False
    scene.view_settings.view_transform = 'Standard'
    scene.view_settings.look = 'None'
    bpy.ops.object.camera_add(location=(0, 0, 5))
    scene.camera = bpy.context.object
    scene.camera.data.type = 'ORTHO'
    scene.camera.data.ortho_scale = 3
    world = bpy.data.worlds.new('Refraction world')
    world.use_nodes = True
    world.node_tree.nodes['Background'].inputs['Color'].default_value = (*BACKGROUND, 1)
    world.node_tree.nodes['Background'].inputs['Strength'].default_value = 1
    scene.world = world
    back = plane('Opaque background', emission_material('Background', BACKGROUND), -2, 8)
    return scene, back


def render(label, preview=False):
    scene = bpy.context.scene
    path = OUT / (label + '.exr')
    scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)
    image = bpy.data.images.load(str(path), check_existing=False)
    pixels = np.array(image.pixels[:], dtype=np.float32).reshape(RES, RES, 4)
    bpy.data.images.remove(image)
    assert np.isfinite(pixels).all(), label
    RENDERS.append({'label': label, 'center': center(pixels).tolist()})
    print('OUTLINE_REFRACTION_PIXEL ' + label + ': ' + str(center(pixels).tolist()), flush=True)
    if preview:
        image = bpy.data.images.load(str(path), check_existing=False)
        scene.render.image_settings.file_format = 'PNG'
        scene.render.image_settings.color_depth = '8'
        image.save_render(filepath=str(OUT / (label + '.png')), scene=scene)
        scene.render.image_settings.file_format = 'OPEN_EXR'
        scene.render.image_settings.color_depth = '32'
        bpy.data.images.remove(image)
    return pixels


def center(pixels):
    return pixels[56:72, 56:72].mean(axis=(0, 1))


def pixel(pixels, x, y=0):
    return pixels[int((y / 3 + .5) * RES), int((x / 3 + .5) * RES)]


def red_mask(pixels):
    return (pixels[..., 0] > .8) & (pixels[..., 1] < .1) & (pixels[..., 2] < .1)


class OutlineRefractionLayersTest(unittest.TestCase):
    def setUp(self):
        self.scene, self.back = setup_scene()

    def assert_color(self, actual, expected, tolerance=.02):
        np.testing.assert_allclose(actual, expected, atol=tolerance, rtol=0)

    def test_self_shell(self):
        mat = emission_material('Own layer', (.1, .2, .8), refractive=True, outline=RED)
        sphere('Owner', mat, layer=5)
        shell = shell_node(mat)
        shell.mute = True
        baseline = render('self_without_shell')
        self.assert_color(center(baseline), (*BACKGROUND, 1))
        shell.mute = False
        pixels = render('self_with_shell', preview=True)
        self.assert_color(pixels[45:83, 45:83], baseline[45:83, 45:83])
        self.assertGreater(int(red_mask(pixels).sum()), 50)
        self.assert_color(pixel(pixels, .74)[:3], RED)

    def test_clamp_feedback_hdr(self):
        hdr = (.3, 1.4, .1)
        emission = self.back.data.materials[0].node_tree.nodes.get('Emission')
        emission.inputs['Color'].default_value = (*hdr, 1)
        for i, layer in enumerate((0, 5, 12)):
            mat = emission_material('Clamp layer ' + str(layer), (.8, .1, .2), refractive=True)
            plane('Clamp object ' + str(layer), mat, i * .6, layer=layer)
        for clamp in (0, 10, 0):
            self.scene.eevee.clamp_surface_direct = clamp
            pixels = render('clamp_hdr_' + str(len(RENDERS)))
            self.assert_color(center(pixels), (*hdr, 1))

    def test_world_only_layers(self):
        self.back.hide_render = True
        objects = []
        for i, layer in enumerate((0, 5, 12)):
            mat = emission_material('World layer ' + str(layer), (.8, .1, .2), refractive=True)
            objects.append(plane('World object ' + str(layer), mat, i * .6, layer=layer))
        for count in (1, 2, 3):
            for i, obj in enumerate(objects):
                obj.hide_render = i >= count
            for clamp in (0, 10):
                self.scene.eevee.clamp_surface_direct = clamp
                pixels = render(f'world_layers_{count}_clamp_{clamp}')
                self.assert_color(center(pixels), (*BACKGROUND, 1))
        self.scene.render.film_transparent = True
        pixels = render('world_layers_transparent_film')
        self.assert_color(center(pixels), (0, 0, 0, 0))

    def test_lower_layer_outlines(self):
        a = emission_material('Layer 0', (.1, .2, .8), refractive=True, outline=RED)
        b = emission_material('Layer 5', (.8, .1, .2), refractive=True, outline=BLUE)
        sphere('Layer 0 object', a, location=(-.35, 0, 0), radius=.45, layer=0)
        sphere('Layer 5 object', b, location=(.35, 0, .9), radius=.35, layer=5)
        lower = render('lower_layers_0_5', preview=True)
        self.assertGreater(int(red_mask(lower).sum()), 20)
        self.assertGreater(int(((lower[..., 2] > .8) & (lower[..., 1] < .1)).sum()), 20)
        front_mat = emission_material('Layer 12', (.7, .1, .8), refractive=True, outline=(1, 1, 0))
        # A smooth hull has a visible expanded silhouette and covers the whole comparison region.
        sphere('Layer 12 object', front_mat, location=(0, 0, 2), radius=1.35, layer=12)
        for clamp in (0, 10):
            self.scene.eevee.clamp_surface_direct = clamp
            pixels = render('higher_layer_12_clamp_' + str(clamp), preview=True)
            self.assert_color(pixels[24:104, 24:104], lower[24:104, 24:104])
            yellow = (pixels[..., 0] > .8) & (pixels[..., 1] > .8) & (pixels[..., 2] < .1)
            self.assertGreater(int(yellow.sum()), 20)

    def test_shared_material_and_layer_changes(self):
        mat = emission_material('Shared', (.8, .1, .2), refractive=True, outline=RED)
        back = sphere('Shared back', mat, layer=0)
        front = sphere('Shared front', mat, location=(0, 0, 1.8), radius=.95, layer=7)
        front.hide_render = True
        reference = render('shared_back_only')
        self.assert_color(pixel(reference, .74)[:3], RED)
        front.hide_render = False
        pixels = render('shared_layers_0_7')
        self.assert_color(pixel(pixels, .74)[:3], RED)
        self.assert_color(center(pixels), (*BACKGROUND, 1))
        for frame, back_layer, front_layer, expected in (
                (2, 0, 0, BACKGROUND), (3, 7, 0, BACKGROUND), (4, 0, 7, RED)):
            self.scene.frame_set(frame)
            back.refraction_layer_index = back_layer
            front.refraction_layer_index = front_layer
            pixels = render(f'shared_layer_change_{frame}')
            self.assert_color(pixel(pixels, .74)[:3], expected)
        mat.use_raytrace_refraction = False
        mat.node_tree.nodes.get('Material Output').nprtree = None
        pixels = render('shared_transmission_disabled')
        self.assert_color(center(pixels)[:3], (.8, .1, .2))

    def test_multiple_material_slots(self):
        left = emission_material('Slot left', (.1, .2, .8), refractive=True, outline=RED)
        right = emission_material('Slot right', (.8, .1, .2), refractive=True, outline=BLUE)
        obj = sphere('Multiple slots', left, layer=3)
        obj.data.materials.append(right)
        for polygon in obj.data.polygons:
            polygon.material_index = int(polygon.center.x > 0)
        pixels = render('multiple_slots')
        self.assert_color(center(pixels), (*BACKGROUND, 1))
        self.assert_color(pixel(pixels, -.74)[:3], RED)
        self.assert_color(pixel(pixels, .74)[:3], BLUE)

    def test_alpha_and_animation(self):
        mat = emission_material('Alpha owner', (.1, .2, .8), refractive=True, outline=RED)
        obj = sphere('Alpha object', mat, layer=5)
        shell = shell_node(mat)
        for mode, alpha, visible in (('CLIP', 0, False), ('CLIP', .4, False),
                                      ('CLIP', .6, True), ('DITHERED', 0, False),
                                      ('DITHERED', 1, True)):
            mat.outline_shell_transparency_method = mode
            shell.inputs['Alpha'].default_value = alpha
            pixels = render(f'alpha_{mode}_{alpha}')
            self.assert_color(center(pixels), (*BACKGROUND, 1))
            self.assertEqual(int(red_mask(pixels).sum()) > 20, visible)
        # A fractional shell must keep coverage while the NPR body reads the intact background.
        self.scene.eevee.taa_render_samples = 64
        solid = render('alpha_dither_solid')
        shell.inputs['Alpha'].default_value = .5
        half = render('alpha_dither_half')
        ring = red_mask(solid)
        coverage = float(((half[..., 0] - BACKGROUND[0]) / (1 - BACKGROUND[0]))[ring].mean())
        self.assertGreater(coverage, .3)
        self.assertLess(coverage, .7)
        self.assert_color(center(half), (*BACKGROUND, 1))
        self.scene.eevee.taa_render_samples = 8
        for frame, alpha, x in ((1, 0, -.1), (2, 1, .1)):
            shell.inputs['Alpha'].default_value = alpha
            shell.inputs['Alpha'].keyframe_insert('default_value', frame=frame)
            obj.location.x = x
            obj.keyframe_insert('location', frame=frame)
        for frame, visible in ((1, False), (2, True)):
            self.scene.frame_set(frame)
            pixels = render('alpha_animated_' + str(frame))
            self.assert_color(center(pixels), (*BACKGROUND, 1))
            self.assertEqual(int(red_mask(pixels).sum()) > 20, visible)

    def test_forward_controls(self):
        mat = emission_material('Forward control', (.1, .2, .8), refractive=True, outline=RED)
        sphere('Forward owner', mat, layer=5)
        for method, alpha_mode, depth in (('FORWARD', 'DITHERED', True),
                                           ('DEFERRED', 'BLENDED', True),
                                           ('DEFERRED', 'DITHERED', False)):
            mat.outline_shell_render_method = method
            mat.outline_shell_transparency_method = alpha_mode
            mat.use_outline_shell_depth_write = depth
            pixels = render(f'forward_control_{method}_{alpha_mode}_{depth}')
            self.assert_color(center(pixels), (*BACKGROUND, 1))
            self.assert_color(pixel(pixels, .74)[:3], RED)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--test', default='')
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])
    gpu.init()
    backend = gpu.platform.backend_type_get()
    print('OUTLINE_REFRACTION_BACKEND=' + backend, flush=True)
    print('OUTLINE_REFRACTION_BUILD=' + bpy.app.build_hash.decode(), flush=True)
    loader = unittest.defaultTestLoader
    if args.test:
        names = args.test.split(',')
        assert set(names) <= set(loader.getTestCaseNames(OutlineRefractionLayersTest))
        suite = unittest.TestSuite(OutlineRefractionLayersTest(name) for name in names)
    else:
        suite = loader.loadTestsFromTestCase(OutlineRefractionLayersTest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    report = {'backend': backend, 'build': bpy.app.build_hash.decode(), 'tests': result.testsRun,
              'failures': len(result.failures), 'errors': len(result.errors), 'renders': RENDERS}
    (OUT / 'summary.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    assert result.wasSuccessful(), 'Outline shell refraction layer regressions failed'
    print('OUTLINE_REFRACTION_OK', flush=True)


if __name__ == '__main__':
    main()
