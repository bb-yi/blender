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


def shell_plane(method='DITHERED', alpha=1.0, color=(1, 0, 0, 1), location=(0, 0, 0)):
    """Two triangles and an invisible body isolate fragment coverage from hull displacement."""
    bpy.ops.mesh.primitive_plane_add(size=2, location=location, rotation=(math.pi / 2, 0, 0))
    obj = bpy.context.object
    mat = bpy.data.materials.new('ShellTransparency')
    mat.use_nodes = True
    obj.data.materials.append(mat)
    tree = mat.node_tree
    tree.nodes.clear()
    out = tree.nodes.new('ShaderNodeOutputMaterial')
    transparent = tree.nodes.new('ShaderNodeBsdfTransparent')
    tree.links.new(transparent.outputs[0], out.inputs['Surface'])
    shell = tree.nodes.new('ShaderNodeOutputOutlineShell')
    shell.inputs['Strength'].default_value = 0
    shell.inputs['Color'].default_value = color
    shell.inputs['Alpha'].default_value = alpha
    mat.outline_shell_cull_method = 'NONE'
    mat.outline_shell_transparency_method = method
    return obj, mat, shell


def render_channels(label):
    """Read raw linear multipart EXR values, including data passes, without display transforms."""
    import OpenImageIO as oiio
    scene = bpy.context.scene
    layer = scene.view_layers[0]
    layer.use_pass_z = layer.use_pass_normal = layer.use_pass_emit = True
    layer.use_pass_vector = layer.use_pass_cryptomatte_object = True
    scene.use_nodes = True
    tree = bpy.data.node_groups.new('ShellPasses', 'CompositorNodeTree')
    scene.compositing_node_group = tree
    rl = tree.nodes.new('CompositorNodeRLayers')
    out = tree.nodes.new('CompositorNodeOutputFile')
    out.directory = str(Path(OUTPUT_DIR))
    out.file_name = label
    out.file_output_items.clear()
    for name, kind, socket in [('Combined', 'RGBA', 'Image'), ('Z', 'FLOAT', 'Depth'),
                               ('Normal', 'VECTOR', 'Normal'), ('Emit', 'RGBA', 'Emission'),
                               ('Vector', 'RGBA', 'Vector'), ('Crypto', 'RGBA', 'CryptoObject00')]:
        out.file_output_items.new(kind, name)
        tree.links.new(rl.outputs[socket], out.inputs[name])
    Path(OUTPUT_DIR).mkdir(parents=True, exist_ok=True)
    bpy.ops.render.render()
    inp = oiio.ImageInput.open(str(Path(OUTPUT_DIR) / (label + '.exr')))
    if inp is None:
        raise AssertionError('Missing multipart EXR: ' + label)
    result = {}
    index = 0
    while True:
        spec = inp.spec()
        name = spec.channelnames[0].split('.')[0]
        result[name] = np.flipud(inp.read_image(format='float')).copy()
        if not inp.seek_subimage(index + 1, 0):
            break
        index += 1
    inp.close()
    scene.use_nodes = False
    scene.compositing_node_group = None
    bpy.data.node_groups.remove(tree)
    return result


class OutlineShellTransparencyTest(unittest.TestCase):
    def setUp(self):
        scene_setup()
        Path(OUTPUT_DIR).mkdir(parents=True, exist_ok=True)

    def assert_rgb(self, pixels, color, tolerance=.025):
        values = pixels[40:88, 40:88, :3].mean(axis=(0, 1))
        self.assertLess(float(np.abs(values - color).max()), tolerance, str(values))

    def test_defaults_and_roundtrip(self):
        obj, mat, shell = shell_plane()
        self.assertEqual([s.name for s in shell.inputs], ['Color', 'Alpha', 'Displacement', 'Strength'])
        self.assertEqual(shell.inputs['Alpha'].default_value, 1)
        self.assertEqual(mat.outline_shell_transparency_method, 'DITHERED')
        self.assertAlmostEqual(mat.outline_shell_alpha_threshold, .5)
        value = mat.node_tree.nodes.new('ShaderNodeValue')
        value.outputs[0].default_value = .3
        mat.node_tree.links.new(value.outputs[0], shell.inputs['Alpha'])
        mat.outline_shell_transparency_method = 'CLIP'
        mat.outline_shell_alpha_threshold = .2
        path = Path(__file__).resolve().parents[4] / 'test/outline_shell/assets/alpha_roundtrip.blend'
        path.parent.mkdir(parents=True, exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=str(path))
        bpy.ops.wm.open_mainfile(filepath=str(path))
        mat = bpy.data.materials['ShellTransparency']
        shell = next(n for n in mat.node_tree.nodes if n.type == 'OUTPUT_OUTLINE_SHELL')
        self.assertTrue(shell.inputs['Alpha'].is_linked)
        self.assertEqual(mat.outline_shell_transparency_method, 'CLIP')
        self.assertAlmostEqual(mat.outline_shell_alpha_threshold, .2)
        self.assert_rgb(render('alpha_roundtrip'), (1, 0, 0))
        # Color's alpha is deliberately independent; linked Alpha is clamped in the shader.
        shell.inputs['Color'].default_value = (1, 0, 0, 0)
        for mode in ('CLIP', 'DITHERED', 'BLENDED'):
            mat.outline_shell_transparency_method = mode
            for alpha, expected in ((-1, (0, 0, 0)), (2, (1, 0, 0))):
                mat.node_tree.nodes['Value'].outputs[0].default_value = alpha
                self.assert_rgb(render(f'clamp_{mode}_{alpha}'), expected)

    def test_alpha_zero_has_no_pass_residue(self):
        obj, mat, shell = shell_plane()
        bpy.context.scene.render.film_transparent = True
        shell.mute = True
        baseline = render_channels('alpha_zero_baseline')
        shell.mute = False
        shell.inputs['Alpha'].default_value = 0
        for mode in ('CLIP', 'DITHERED', 'BLENDED'):
            for route in ('DEFERRED', 'FORWARD'):
                for depth in (False, True):
                    with self.subTest(mode=mode, route=route, depth=depth):
                        mat.outline_shell_transparency_method = mode
                        mat.outline_shell_render_method = route
                        mat.use_outline_shell_depth_write = depth
                        result = render_channels(f'zero_{mode}_{route}_{depth}')
                        for name in baseline:
                            np.testing.assert_allclose(result[name], baseline[name], atol=.002, rtol=0,
                                                       err_msg=name)

    def test_clip_threshold_endpoints(self):
        obj, mat, shell = shell_plane('CLIP')
        for threshold in (0, .5, 1):
            mat.outline_shell_alpha_threshold = threshold
            for alpha in (0, .49, .5, 1):
                with self.subTest(threshold=threshold, alpha=alpha):
                    shell.inputs['Alpha'].default_value = alpha
                    visible = alpha > 0 and alpha >= threshold
                    self.assert_rgb(render(f'clip_{threshold}_{alpha}'),
                                    (1, 0, 0) if visible else (0, 0, 0))

    def test_coverage_and_blend_energy(self):
        obj, mat, shell = shell_plane()
        scene = bpy.context.scene
        scene.eevee.taa_render_samples = 128
        world = scene.world.node_tree.nodes['Background']
        world.inputs[0].default_value = (0, 0, .4, 1)
        world.inputs[1].default_value = 1
        for mode in ('DITHERED', 'BLENDED'):
            mat.outline_shell_transparency_method = mode
            for route, depth in (('DEFERRED', True), ('FORWARD', True), ('FORWARD', False)):
                mat.outline_shell_render_method = route
                mat.use_outline_shell_depth_write = depth
                for alpha in (.25, .5, .99, 1):
                    with self.subTest(mode=mode, route=route, depth=depth, alpha=alpha):
                        shell.inputs['Alpha'].default_value = alpha
                        self.assert_rgb(render(f'energy_{mode}_{route}_{depth}_{alpha}'),
                                        (alpha, 0, .4 * (1 - alpha)), .04)
        # Alpha=1 must not turn Blended back into an opaque/depth-writing pass.
        mat.outline_shell_transparency_method = 'BLENDED'
        mat.use_outline_shell_depth_write = True
        shell.mute = True
        empty_passes = render_channels('blended_empty_passes')
        shell.mute = False
        for alpha in (.99, 1):
            shell.inputs['Alpha'].default_value = alpha
            passes = render_channels(f'blended_depth_{alpha}')
            self.assertGreater(float(passes['Z'][64, 64, 0]), 1e9)
            self.assertLess(float(np.abs(passes['Vector'][64, 64]).max()), .001)
            # Cryptomatte encodes the background as ID=0, coverage=1 (not all zeros).
            np.testing.assert_allclose(passes['Crypto'], empty_passes['Crypto'], atol=.001)
            self.assertLess(float(np.abs(passes['Emit'][64, 64, :3]).max()), .001)
        scene.render.film_transparent = True
        shell.inputs['Alpha'].default_value = .5
        passes = render_channels('blended_film_alpha')
        self.assertAlmostEqual(float(passes['Combined'][64, 64, 3]), .5, delta=.015)

    def test_uv_mask_and_hybrid(self):
        obj, mat, shell = shell_plane('CLIP')
        tree = mat.node_tree
        image = bpy.data.images.new('FineAlpha', width=32, height=4, alpha=True)
        values = np.ones((4, 32, 4), np.float32)
        values[:, :, 3] = np.arange(32) % 2
        image.pixels.foreach_set(values.ravel())
        image.update()
        tex = tree.nodes.new('ShaderNodeTexImage')
        tex.image = image
        tex.interpolation = 'Closest'
        uv = tree.nodes.new('ShaderNodeTexCoord')
        tree.links.new(uv.outputs['UV'], tex.inputs['Vector'])
        tree.links.new(tex.outputs['Alpha'], shell.inputs['Alpha'])
        baseline = render('fine_mask_deferred')
        row = baseline[64, 25:103, 0]
        self.assertGreaterEqual(int(np.count_nonzero(row < .05)), 20)
        self.assertGreaterEqual(int(np.count_nonzero(row > .95)), 20)
        emission = tree.nodes.new('ShaderNodeEmission')
        emission.inputs['Color'].default_value = (1, 0, 0, 1)
        rgb = tree.nodes.new('ShaderNodeShaderToRGB')
        tree.links.new(emission.outputs[0], rgb.inputs[0])
        tree.links.new(rgb.outputs['Color'], shell.inputs['Color'])
        for route, depth in (('DEFERRED', True), ('FORWARD', True), ('FORWARD', False)):
            for mode in ('CLIP', 'DITHERED', 'BLENDED'):
                with self.subTest(route=route, depth=depth, mode=mode):
                    mat.outline_shell_render_method = route
                    mat.use_outline_shell_depth_write = depth
                    mat.outline_shell_transparency_method = mode
                    result = render(f'fine_hybrid_{route}_{depth}_{mode}')
                    np.testing.assert_allclose(result[30:98, 30:98], baseline[30:98, 30:98], atol=.02)
        # A transparent closure in Color must not switch coverage shells to the blended queue.
        tree.links.new(tree.nodes.get('Transparent BSDF').outputs[0], rgb.inputs[0])
        mat.outline_shell_transparency_method = 'DITHERED'
        mat.outline_shell_render_method = 'FORWARD'
        mat.use_outline_shell_depth_write = True
        result = render_channels('color_branch_transparency')
        visible_columns = np.flatnonzero(baseline[64, :, 0] > .95)
        column = int(visible_columns[len(visible_columns) // 2])
        self.assertLess(float(result['Z'][64, column, 0]), 1e9)
        # These attributes are used only by the shell, never by the body's batch.
        tree.links.new(emission.outputs[0], rgb.inputs[0])
        colors = obj.data.color_attributes.new('ShellAlpha', 'FLOAT_COLOR', 'CORNER')
        for color in colors.data:
            color.color = (1, 1, 1, .25)
        attribute = tree.nodes.new('ShaderNodeVertexColor')
        attribute.layer_name = colors.name
        tree.links.new(attribute.outputs['Alpha'], shell.inputs['Alpha'])
        bpy.context.scene.eevee.taa_render_samples = 128
        for mode, expected in (('CLIP', 0), ('DITHERED', .25), ('BLENDED', .25)):
            mat.outline_shell_transparency_method = mode
            self.assert_rgb(render('color_attribute_' + mode), (expected, 0, 0), .04)
        checker = tree.nodes.new('ShaderNodeTexChecker')
        checker.inputs['Scale'].default_value = 8
        tree.links.new(uv.outputs['UV'], checker.inputs['Vector'])
        tree.links.new(checker.outputs['Fac'], shell.inputs['Alpha'])
        for mode in ('CLIP', 'DITHERED', 'BLENDED'):
            mat.outline_shell_transparency_method = mode
            self.assert_rgb(render('procedural_alpha_' + mode), (.5, 0, 0), .04)
        # Mixed zero/fractional alpha puts hash derivatives across divergent coverage edges.
        multiply = tree.nodes.new('ShaderNodeMath')
        multiply.operation = 'MULTIPLY'
        multiply.inputs[1].default_value = .5
        tree.links.new(checker.outputs['Fac'], multiply.inputs[0])
        tree.links.new(multiply.outputs[0], shell.inputs['Alpha'])
        mat.outline_shell_transparency_method = 'DITHERED'
        mat.outline_shell_render_method = 'DEFERRED'
        fractional = render('fractional_edges_deferred')
        self.assert_rgb(fractional, (.25, 0, 0), .04)
        mat.outline_shell_render_method = 'FORWARD'
        for depth in (True, False):
            mat.use_outline_shell_depth_write = depth
            result = render(f'fractional_edges_forward_{depth}')
            np.testing.assert_allclose(result[30:98, 30:98], fractional[30:98, 30:98], atol=.02)

    def test_shared_material_instances_and_sorted_blending(self):
        obj, mat, shell = shell_plane('BLENDED', .5, (1, 0, 0, 1), (0, .2, 0))
        near, near_mat, near_shell = shell_plane('BLENDED', .5, (0, 0, 1, 1), (0, -.2, 0))
        self.assert_rgb(render('blend_sorted_front'), (.25, 0, .5))
        scene = bpy.context.scene
        scene.camera.location.y = 3
        scene.camera.rotation_euler = (-math.pi / 2, 0, 0)
        self.assert_rgb(render('blend_sorted_back'), (.5, 0, .25))
        near.hide_render = True
        copy = obj.copy()
        copy.data = obj.data
        scene.collection.objects.link(copy)
        copy.location.y = -.2
        self.assert_rgb(render('blend_shared_material'), (.75, 0, 0))

    def test_blended_body_and_multiple_shell_slots(self):
        obj, mat, shell = shell_plane('BLENDED', .5)
        # Two coincident material slots deliberately exercise stable equal-depth ordering.
        mesh = bpy.data.meshes.new('CoincidentSlots')
        vertices = [(-1, 0, -1), (1, 0, -1), (1, 0, 1), (-1, 0, 1)]
        mesh.from_pydata(vertices + vertices, [], [(0, 1, 2, 3), (4, 5, 6, 7)])
        obj.data = mesh
        obj.rotation_euler = (0, 0, 0)
        tree = mat.node_tree
        em = tree.nodes.new('ShaderNodeEmission')
        em.inputs['Color'].default_value = (0, 1, 0, 1)
        mix = tree.nodes.new('ShaderNodeMixShader')
        mix.inputs[0].default_value = .5
        tree.links.new(tree.nodes['Transparent BSDF'].outputs[0], mix.inputs[1])
        tree.links.new(em.outputs[0], mix.inputs[2])
        tree.links.new(mix.outputs[0], tree.nodes['Material Output'].inputs['Surface'])
        mat.surface_render_method = 'BLENDED'
        other = mat.copy()
        next(n for n in other.node_tree.nodes if n.type == 'OUTPUT_OUTLINE_SHELL').inputs[
            'Color'].default_value = (0, 0, 1, 1)
        mesh.materials.append(mat)
        mesh.materials.append(other)
        mesh.polygons[1].material_index = 1
        # Red shell, blue shell, green body, green body (all alpha=.5).
        self.assert_rgb(render('blend_slots_shells_before_bodies'), (.0625, .75, .125))

    def test_shadow_alpha_and_animation(self):
        scene = bpy.context.scene
        scene.eevee.taa_render_samples = 128
        caster = sphere()
        caster.location = (0, -.7, 0)
        caster.scale = (.4, .4, .4)
        caster.visible_camera = False
        mat = material(caster)
        shell = mat.node_tree.nodes.new('ShaderNodeOutputOutlineShell')
        shell.inputs['Strength'].default_value = .3
        mat.use_outline_shell_shadow = True
        mat.use_transparent_shadow = False  # Must not disable shell alpha shadows.
        bpy.ops.mesh.primitive_plane_add(size=6, location=(0, .5, 0), rotation=(math.pi / 2, 0, 0))
        material(bpy.context.object)
        shell.inputs['Alpha'].default_value = 0
        clear = render('shadow_alpha_clear')
        shell.inputs['Alpha'].default_value = 1
        solid = render('shadow_alpha_solid')
        self.assertLess(float(pixel(solid, .55).mean()), float(pixel(clear, .55).mean()) * .5)
        for mode in ('CLIP', 'DITHERED', 'BLENDED'):
            mat.outline_shell_transparency_method = mode
            shell.inputs['Alpha'].default_value = 0
            result = render('shadow_zero_' + mode)
            np.testing.assert_allclose(pixel(result, .55), pixel(clear, .55), atol=.025)
            shell.inputs['Alpha'].default_value = .5
            result = render('shadow_half_' + mode)
            if mode != 'CLIP':
                value = float(result[61:67, 85:90, :3].mean())
                self.assertGreater(value, float(solid[61:67, 85:90, :3].mean()) + .02)
                self.assertLess(value, float(clear[61:67, 85:90, :3].mean()) - .02)
                # A static alpha caster must refresh shadow maps each sample, not freeze noise.
                scene.eevee.taa_render_samples = 1
                noisy = render('shadow_one_sample_' + mode)
                scene.eevee.taa_render_samples = 512
                reference = render('shadow_reference_' + mode)
                scene.eevee.taa_render_samples = 128
                ring = (clear[:, :, :3] - solid[:, :, :3]).mean(axis=2) > .2
                self.assertGreater(int(ring.sum()), 100)
                noisy_error = float(np.square(noisy[ring, :3] - reference[ring, :3]).mean())
                converged_error = float(np.square(result[ring, :3] - reference[ring, :3]).mean())
                self.assertLess(converged_error, noisy_error * .5)
        for frame, alpha in ((1, 0), (2, 1), (3, 0)):
            shell.inputs['Alpha'].default_value = alpha
            shell.inputs['Alpha'].keyframe_insert('default_value', frame=frame)
        for frame, expected in ((1, clear), (2, solid), (3, clear)):
            scene.frame_set(frame)
            result = render(f'shadow_animated_{frame}')
            np.testing.assert_allclose(pixel(result, .55), pixel(expected, .55), atol=.025)

    def test_coverage_velocity_and_holes(self):
        obj, mat, shell = shell_plane('CLIP')
        tree = mat.node_tree
        uv = tree.nodes.new('ShaderNodeTexCoord')
        xyz = tree.nodes.new('ShaderNodeSeparateXYZ')
        mask = tree.nodes.new('ShaderNodeMath')
        mask.operation = 'LESS_THAN'
        mask.inputs[1].default_value = .5
        tree.links.new(uv.outputs['UV'], xyz.inputs[0])
        tree.links.new(xyz.outputs['X'], mask.inputs[0])
        tree.links.new(mask.outputs[0], shell.inputs['Alpha'])
        for frame, x in ((1, -.2), (2, 0), (3, .2)):
            obj.location.x = x
            obj.keyframe_insert('location', frame=frame)
        scene = bpy.context.scene
        scene.frame_set(2)
        for mode in ('CLIP', 'DITHERED'):
            mat.outline_shell_transparency_method = mode
            for route in ('DEFERRED', 'FORWARD'):
                with self.subTest(mode=mode, route=route):
                    mat.outline_shell_render_method = route
                    passes = render_channels(f'velocity_{mode}_{route}')
                    visible = np.abs(passes['Vector'][64, 48]).max()
                    hole = np.abs(passes['Vector'][64, 80]).max()
                    self.assertGreater(float(visible), .01)
                    self.assertLess(float(hole), .001)
                    self.assertGreater(float(passes['Z'][64, 80, 0]), 1e9)
                    for name in ('Normal', 'Emit'):
                        self.assertLess(float(np.abs(passes[name][64, 80, :3]).max()), .001,
                                        name)
                    np.testing.assert_allclose(passes['Crypto'][64, 80], (0, 1, 0, 0), atol=.001)

    def test_culling_ztest_and_depth_switches(self):
        obj, mat, shell = shell_plane('CLIP', .5, location=(0, .2, 0))
        bpy.ops.mesh.primitive_plane_add(size=2.5, rotation=(math.pi / 2, 0, 0))
        wall = material(bpy.context.object)
        tree = wall.node_tree
        em = tree.nodes.new('ShaderNodeEmission')
        em.inputs['Color'].default_value = (0, 0, 1, 1)
        tree.links.new(em.outputs[0], tree.nodes['Material Output'].inputs['Surface'])
        for mode in ('CLIP', 'DITHERED', 'BLENDED'):
            mat.outline_shell_transparency_method = mode
            shell.inputs['Alpha'].default_value = 1
            for depth in (True, False):
                mat.use_outline_shell_depth_write = depth
                for ztest, color in (('LESS_EQUAL', (0, 0, 1)), ('ALWAYS', (1, 0, 0)),
                                     ('NEVER', (0, 0, 1))):
                    with self.subTest(mode=mode, depth=depth, ztest=ztest):
                        mat.outline_shell_ztest_mode = ztest
                        self.assert_rgb(render(f'ztest_{mode}_{depth}_{ztest}'), color)
            mat.outline_shell_ztest_mode = 'ALWAYS'
            for cull, color in (('FRONT', (0, 0, 1)), ('BACK', (1, 0, 0)), ('NONE', (1, 0, 0))):
                mat.outline_shell_cull_method = cull
                self.assert_rgb(render(f'cull_{mode}_{cull}'), color)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    OUTPUT_DIR = args.output_dir
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(OutlineShellIntegrationTest)
    suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(OutlineShellTransparencyTest))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        raise AssertionError('Outline Shell integration regressions failed')
