# SPDX-License-Identifier: GPL-2.0-or-later
"""Pixel regressions for optional, per-instance Outline Shell self-occlusion.

Run with isolated Blender user directories on OpenGL and Vulkan. Outputs go to
BLENDER_TEST_OUTPUT_DIR, outside the source repository.
"""

import argparse
import json
import math
import os
from pathlib import Path
import re
import sys
import time
import unittest

import bpy
import gpu
import numpy as np


OUT = Path(os.environ['BLENDER_TEST_OUTPUT_DIR'])
OUT.mkdir(parents=True, exist_ok=True)
RES = 128
BACKGROUND = (.8, .45, .3)
BODY = (.1, .35, .8)
RENDERS = []


def setup():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    scene.render.resolution_x = scene.render.resolution_y = RES
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.render.image_settings.color_mode = 'RGBA'
    scene.render.image_settings.color_depth = '32'
    scene.eevee.taa_render_samples = 64
    scene.eevee.use_outline = False
    world = bpy.data.worlds.new('World')
    world.use_nodes = True
    world.node_tree.nodes['Background'].inputs[0].default_value = (*BACKGROUND, 1)
    world.node_tree.nodes['Background'].inputs[1].default_value = 1
    scene.world = world
    camera = bpy.data.objects.new('Camera', bpy.data.cameras.new('Camera'))
    scene.collection.objects.link(camera)
    camera.location = (0, -5, 0)
    camera.rotation_euler = (math.pi / 2, 0, 0)
    camera.data.type = 'ORTHO'
    camera.data.ortho_scale = 3
    scene.camera = camera
    return scene


def body_material(name='Body', alpha=.5, shell=True):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    tree = mat.node_tree
    tree.nodes.clear()
    out = tree.nodes.new('ShaderNodeOutputMaterial')
    transparent = tree.nodes.new('ShaderNodeBsdfTransparent')
    emission = tree.nodes.new('ShaderNodeEmission')
    emission.inputs['Color'].default_value = (*BODY, 1)
    mix = tree.nodes.new('ShaderNodeMixShader')
    mix.inputs[0].default_value = alpha
    tree.links.new(transparent.outputs[0], mix.inputs[1])
    tree.links.new(emission.outputs[0], mix.inputs[2])
    tree.links.new(mix.outputs[0], out.inputs['Surface'])
    if shell:
        node = tree.nodes.new('ShaderNodeOutputOutlineShell')
        node.inputs['Color'].default_value = (0, 0, 0, 1)
        node.inputs['Alpha'].default_value = 1
        node.inputs['Strength'].default_value = .12
    return mat


def sphere(mat, name='Body', location=(0, 0, 0)):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=48, ring_count=24, radius=.8, location=location)
    obj = bpy.context.object
    obj.name = name
    obj.data.materials.append(mat)
    for face in obj.data.polygons:
        face.use_smooth = True
    return obj


def render(label):
    scene = bpy.context.scene
    path = OUT / (label + '.exr')
    scene.render.filepath = str(path)
    started = time.perf_counter()
    bpy.ops.render.render(write_still=True)
    elapsed = time.perf_counter() - started
    image = bpy.data.images.load(str(path), check_existing=False)
    pixels = np.array(image.pixels[:], dtype=np.float32).reshape(RES, RES, 4)
    bpy.data.images.remove(image)
    assert np.isfinite(pixels).all(), label
    RENDERS.append({'label': label, 'seconds': elapsed, 'center': center(pixels).tolist()})
    return pixels


def center(pixels):
    return pixels[54:74, 54:74, :3].mean(axis=(0, 1))


def at(pixels, x, z=0):
    return pixels[int((z / 3 + .5) * RES), int((x / 3 + .5) * RES), :3]


def alpha_set(mat, value):
    mat.node_tree.nodes.get('Mix Shader').inputs[0].default_value = value


def glsl_function(tree, return_type, body):
    shader = tree.nodes.new('ShaderNodeGLSLFunction')
    source = bpy.data.texts.new('outline_self_depth.glsl')
    source.write(f'{return_type} evaluate() {{ {body} }}')
    shader.script = source
    shader.function_name = 'evaluate'
    assert shader.parse_status == 'READY', shader.error_message
    return shader


class OutlineShellSelfOcclusionTest(unittest.TestCase):
    def setUp(self):
        self.scene = setup()

    def assert_color(self, actual, expected, tolerance=.035):
        self.assertLess(float(np.max(np.abs(np.asarray(actual) - expected))), tolerance)

    def assert_no_compile_failures(self, shader):
        self.assertFalse(shader.error_message, shader.error_message)
        report = self.scene.eevee.performance_profiler_render_report
        totals = re.search(r'Total requests=.* failed=(\d+)', report)
        self.assertIsNotNone(totals, 'Missing material compilation telemetry')
        self.assertEqual(totals.group(1), '0', totals.group(0))

    def test_glsl_light_in_diffusion_callback(self):
        self.scene.eevee.use_performance_profiler = True
        mat = body_material('Light in diffusion', alpha=.5)
        mat.use_outline_shell_self_occlusion = True
        sphere(mat)
        tree = mat.node_tree
        shader = glsl_function(
            tree, 'vec3',
            'return glsl_light_count() > 0 ? vec3(0.1, 0.35, 0.8) : vec3(0.6, 0.15, 0.1);')
        emission = next(n for n in tree.nodes if n.type == 'EMISSION')
        mix = next(n for n in tree.nodes if n.type == 'MIX_SHADER')
        diffusion = tree.nodes.new('ShaderNodeNPRSurfaceDiffusion')
        tree.links.new(shader.outputs['Result'], emission.inputs['Color'])
        tree.links.new(emission.outputs[0], diffusion.inputs['Shader'])
        tree.links.new(diffusion.outputs[0], mix.inputs[2])
        unlit = center(render('glsl_diffusion_no_light'))
        self.assert_no_compile_failures(shader)
        bpy.ops.object.light_add(type='SUN')
        lit = center(render('glsl_diffusion_self'))
        self.assert_no_compile_failures(shader)
        self.assertGreater(float(lit[2] - unlit[2]), .2)
        shell = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeOutputOutlineShell')
        shell.inputs['Alpha'].default_value = 0
        reference = center(render('glsl_diffusion_no_shell'))
        self.assert_no_compile_failures(shader)
        self.assert_color(lit, reference)

    def test_glsl_light_in_depth_offset(self):
        self.scene.eevee.use_performance_profiler = True
        mat = body_material('Light in depth offset', alpha=0)
        mat.use_outline_shell_self_occlusion = True
        sphere(mat)
        tree = mat.node_tree
        output = next(n for n in tree.nodes if n.type == 'OUTPUT_MATERIAL')
        shader = glsl_function(tree, 'float', 'return glsl_light_count() > 0 ? -3.0 : 0.0;')
        tree.links.new(shader.outputs['Result'], output.inputs['Depth Offset'])
        self.assert_color(center(render('glsl_depth_offset_no_light')), BACKGROUND)
        self.assert_no_compile_failures(shader)
        bpy.ops.object.light_add(type='SUN')
        self.assertLess(float(center(render('glsl_depth_offset_light')).max()), .03)
        self.assert_no_compile_failures(shader)

    def test_default_and_file_roundtrip(self):
        mat = body_material(alpha=0)
        sphere(mat)
        self.assertFalse(mat.use_outline_shell_self_occlusion)
        old = render('toggle_original')
        mat.use_outline_shell_self_occlusion = True
        fixed = render('toggle_fixed')
        self.assert_color(center(fixed), BACKGROUND)
        self.assertLess(float(center(old).max()), .03)
        self.assertLess(float(at(fixed, .86).max()), .05)
        mat.use_outline_shell_self_occlusion = False
        restored = render('toggle_restored')
        self.assertLess(float(np.abs(old - restored).max()), .015)
        path = OUT / 'roundtrip.blend'
        bpy.ops.wm.save_as_mainfile(filepath=str(path))
        mat.use_outline_shell_self_occlusion = True
        bpy.ops.wm.save_as_mainfile(filepath=str(OUT / 'roundtrip_on.blend'))
        bpy.ops.wm.open_mainfile(filepath=str(path))
        self.assertFalse(bpy.data.materials['Body'].use_outline_shell_self_occlusion)
        bpy.ops.wm.open_mainfile(filepath=str(OUT / 'roundtrip_on.blend'))
        self.assertTrue(bpy.data.materials['Body'].use_outline_shell_self_occlusion)

    def test_alpha_endpoints_and_forward_deferred(self):
        mat = body_material()
        sphere(mat)
        shell = mat.node_tree.nodes.get('Outline Shell Output')
        if shell is None:
            shell = next(n for n in mat.node_tree.nodes if n.bl_idname == 'ShaderNodeOutputOutlineShell')
        for route in ('DEFERRED', 'FORWARD'):
            mat.outline_shell_render_method = route
            for alpha in (0, .5, 1):
                with self.subTest(route=route, alpha=alpha):
                    alpha_set(mat, alpha)
                    mat.use_outline_shell_self_occlusion = False
                    shell.inputs['Alpha'].default_value = 0
                    reference = render(f'{route}_{alpha}_no_shell')
                    shell.inputs['Alpha'].default_value = 1
                    original = render(f'{route}_{alpha}_old')
                    mat.use_outline_shell_self_occlusion = True
                    fixed = render(f'{route}_{alpha}_fixed')
                    self.assert_color(center(fixed), center(reference), .035)
                    if alpha < 1:
                        self.assertGreater(float((center(fixed) - center(original)).mean()), .06)
                    self.assertLess(float(at(fixed, .86).max()), .07)
                    if alpha == .5:
                        self.assertGreater(float(center(fixed)[0]), BODY[0] + .07)

    def test_shell_independent_alpha_and_modes(self):
        mat = body_material(alpha=0)
        mat.use_outline_shell_self_occlusion = True
        sphere(mat)
        shell = next(n for n in mat.node_tree.nodes if n.bl_idname == 'ShaderNodeOutputOutlineShell')
        for method in ('DITHERED', 'CLIP', 'BLENDED'):
            mat.outline_shell_transparency_method = method
            for alpha in (0, 1):
                with self.subTest(method=method, alpha=alpha):
                    shell.inputs['Alpha'].default_value = alpha
                    pixels = render(f'shell_{method}_{alpha}')
                    self.assert_color(center(pixels), BACKGROUND)
                    self.assert_color(at(pixels, .86), BACKGROUND if alpha == 0 else (0, 0, 0), .06)
        mat.outline_shell_transparency_method = 'BLENDED'
        shell.inputs['Alpha'].default_value = .5
        pixels = render('shell_blended_half')
        self.assert_color(center(pixels), BACKGROUND)
        self.assert_color(at(pixels, .86), np.array(BACKGROUND) * .5, .05)

    def test_blended_body(self):
        mat = body_material(alpha=.5)
        mat.surface_render_method = 'BLENDED'
        mat.use_outline_shell_self_occlusion = True
        sphere(mat)
        shell = next(n for n in mat.node_tree.nodes if n.bl_idname == 'ShaderNodeOutputOutlineShell')
        shell.inputs['Alpha'].default_value = 0
        reference = render('blended_body_no_shell')
        shell.inputs['Alpha'].default_value = 1
        fixed = render('blended_body_fixed')
        self.assert_color(center(fixed), center(reference))
        self.assertLess(float(at(fixed, .86).max()), .07)

    def test_overlapping_shared_material_instances(self):
        mat = body_material(alpha=0)
        mat.use_outline_shell_self_occlusion = True
        first = sphere(mat, location=(-.12, -.3, 0))
        second = first.copy()
        second.data = first.data
        second.location = (.12, .9, 0)
        self.scene.collection.objects.link(second)
        fixed = render('shared_instances_fixed')
        self.assert_color(center(fixed), BACKGROUND)
        mat.use_outline_shell_self_occlusion = False
        old = render('shared_instances_old')
        self.assertLess(float(center(old).max()), .03)
        mat.use_outline_shell_self_occlusion = True
        second.location.x = .35
        second.scale.x = -1
        changed = render('shared_instances_moved')
        self.assert_color(center(changed), BACKGROUND)

    def test_multi_material_body_and_geometry_hole(self):
        mat = body_material(alpha=0)
        mat.use_outline_shell_self_occlusion = True
        obj = sphere(mat)
        other = body_material('Other body', alpha=0, shell=False)
        obj.data.materials.append(other)
        for face in obj.data.polygons:
            if face.center.y < 0:
                face.material_index = 1
        pixels = render('multiple_body_materials')
        self.assert_color(center(pixels), BACKGROUND)
        bpy.data.objects.remove(obj, do_unlink=True)
        bpy.ops.mesh.primitive_torus_add(major_radius=.65, minor_radius=.18,
                                        major_segments=64, minor_segments=24,
                                        rotation=(math.pi / 2, 0, 0))
        torus = bpy.context.object
        torus.data.materials.append(mat)
        for face in torus.data.polygons:
            face.use_smooth = True
        pixels = render('geometric_hole')
        self.assert_color(center(pixels), BACKGROUND)
        self.assertLess(float(at(pixels, .41).max()), .1)
        self.assertLess(float(at(pixels, .88).max()), .1)

    def test_geometry_nodes_instances(self):
        mat = body_material(alpha=1)
        shell = next(n for n in mat.node_tree.nodes if n.bl_idname == 'ShaderNodeOutputOutlineShell')
        shell.inputs['Alpha'].default_value = 0
        mesh = bpy.data.meshes.new('Instance points')
        mesh.from_pydata([(-.16, -.3, 0), (.16, .3, 0)], [], [])
        obj = bpy.data.objects.new('Instance emitter', mesh)
        self.scene.collection.objects.link(obj)
        group = bpy.data.node_groups.new('Instanced shells', 'GeometryNodeTree')
        for direction in ('INPUT', 'OUTPUT'):
            group.interface.new_socket(name='Geometry', in_out=direction,
                                        socket_type='NodeSocketGeometry')
        nodes, links = group.nodes, group.links
        inp, out = nodes.new('NodeGroupInput'), nodes.new('NodeGroupOutput')
        primitive = nodes.new('GeometryNodeMeshUVSphere')
        primitive.inputs['Radius'].default_value = .8
        primitive.inputs['Segments'].default_value = 32
        primitive.inputs['Rings'].default_value = 16
        smooth = nodes.new('GeometryNodeSetShadeSmooth')
        links.new(primitive.outputs['Mesh'], smooth.inputs['Geometry'])
        material = nodes.new('GeometryNodeSetMaterial')
        material.inputs['Material'].default_value = mat
        links.new(smooth.outputs['Geometry'], material.inputs['Geometry'])
        instances = nodes.new('GeometryNodeInstanceOnPoints')
        links.new(inp.outputs['Geometry'], instances.inputs['Points'])
        links.new(material.outputs['Geometry'], instances.inputs['Instance'])
        links.new(instances.outputs['Instances'], out.inputs['Geometry'])
        modifier = obj.modifiers.new('Instances', 'NODES')
        modifier.node_group = group
        self.assert_color(center(render('gn_body_control')), BODY)
        alpha_set(mat, 0)
        shell.inputs['Alpha'].default_value = 1
        self.assertLess(float(center(render('gn_original')).max()), .03)
        mat.use_outline_shell_self_occlusion = True
        self.assert_color(center(render('gn_self')), BACKGROUND)

    def test_perspective_camera_and_animation(self):
        mat = body_material(alpha=0)
        mat.use_outline_shell_self_occlusion = True
        obj = sphere(mat)
        self.scene.camera.data.type = 'PERSP'
        for frame, y in ((1, 0), (2, .6), (3, -.5)):
            obj.location.y = y
            obj.keyframe_insert('location', frame=frame)
        for frame in (1, 2, 3):
            self.scene.frame_set(frame)
            pixels = render('perspective_animation_' + str(frame))
            self.assert_color(center(pixels), BACKGROUND)

    def test_body_displacement_and_depth_offset(self):
        mat = body_material(alpha=0)
        mat.use_outline_shell_self_occlusion = True
        sphere(mat)
        tree = mat.node_tree
        out = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeOutputMaterial')
        self.assert_color(center(render('depth_offset_zero')), BACKGROUND)
        offset = tree.nodes.new('ShaderNodeValue')
        offset.outputs[0].default_value = -3
        link = tree.links.new(offset.outputs[0], out.inputs['Depth Offset'])
        # Moving the body depth behind its shell must let that shell remain visible.
        self.assertLess(float(center(render('depth_offset_away')).max()), .03)
        tree.links.remove(link)
        displacement = tree.nodes.new('ShaderNodeCombineXYZ')
        displacement.inputs['Y'].default_value = 2.2
        link = tree.links.new(displacement.outputs[0], out.inputs['Displacement'])
        mat.displacement_method = 'BOTH'
        mat.max_vertex_displacement = 2.3
        self.assertLess(float(center(render('vertex_displacement_away')).max()), .03)
        tree.links.remove(link)
        self.assert_color(center(render('displacement_restored')), BACKGROUND)

    def test_no_depth_write_and_custom_ztest(self):
        mat = body_material(alpha=0)
        mat.use_outline_shell_self_occlusion = True
        sphere(mat)
        for depth_write, ztest in ((False, 'LESS_EQUAL'), (False, 'ALWAYS'), (True, 'ALWAYS')):
            with self.subTest(depth_write=depth_write, ztest=ztest):
                mat.use_outline_shell_depth_write = depth_write
                mat.outline_shell_ztest_mode = ztest
                pixels = render(f'depth_write_{depth_write}_{ztest}')
                self.assert_color(center(pixels), BACKGROUND)
                self.assertLess(float(at(pixels, .86).max()), .07)

    def test_tessellated_body(self):
        mat = body_material(alpha=0)
        mat.use_outline_shell_self_occlusion = True
        sphere(mat)
        tree = mat.node_tree
        output = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeOutputMaterial')
        tessellation = tree.nodes.new('ShaderNodeTessellationControl')
        tessellation.mode = 'FIXED'
        tessellation.inputs['Max Level'].default_value = 4
        tree.links.new(tessellation.outputs[0], output.inputs['Tessellation'])
        self.assert_color(center(render('tessellated_body')), BACKGROUND)
        displacement = tree.nodes.new('ShaderNodeCombineXYZ')
        displacement.inputs['Y'].default_value = 2.2
        tree.links.new(displacement.outputs[0], output.inputs['Displacement'])
        mat.displacement_method = 'BOTH'
        mat.max_vertex_displacement = 2.3
        self.assertLess(float(center(render('tessellated_body_displaced')).max()), .03)

    def test_live_viewport_toggle(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from test_render_texture_updates import Viewport
        mat = body_material(alpha=0)
        sphere(mat)
        viewport = Viewport(96, 96)
        try:
            colors = []
            for enabled in (False, True, False):
                mat.use_outline_shell_self_occlusion = enabled
                for _ in range(3):
                    color = viewport.draw()
                colors.append(color)
            self.assertLess(float(np.max(colors[0])), .03)
            self.assert_color(colors[1], BACKGROUND)
            self.assert_color(colors[2], colors[0], .015)
        finally:
            viewport.free()

    def test_render_texture_color_camera(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from filter_graph_test_utils import attach_filter_material
        mat = body_material(alpha=0)
        mat.use_outline_shell_self_occlusion = True
        sphere(mat)
        capture_camera = self.scene.camera.copy()
        capture_camera.data = self.scene.camera.data.copy()
        capture_camera.location.x = .1
        self.scene.collection.objects.link(capture_camera)
        rt = self.scene.eevee.render_textures.add()
        rt.enabled = True
        rt.camera = capture_camera
        rt.source, rt.format, rt.update_mode = 'COLOR', 'RGBA32F', 'EVERY_SAMPLE'
        rt.resolution_x, rt.resolution_y = 79, 53
        fm = bpy.data.materials.new('Capture reader')
        fm.eevee_domain = 'FILTER'
        tree = fm.node_tree
        tree.nodes.clear()
        sample = tree.nodes.new('ShaderNodeRenderTexture')
        sample.render_texture = f'RENDER_TEXTURE_{rt.uid}'
        uv = tree.nodes.new('ShaderNodeCombineXYZ')
        uv.inputs['X'].default_value = uv.inputs['Y'].default_value = .5
        tree.links.new(uv.outputs[0], sample.inputs['Vector'])
        output = tree.nodes.new('ShaderNodeOutputFilter')
        output.inputs['Alpha'].default_value = 1
        tree.links.new(sample.outputs['Color'], output.inputs['Color'])
        attach_filter_material(fm)
        self.assert_color(center(render('render_texture_fixed')), BACKGROUND)
        mat.use_outline_shell_self_occlusion = False
        self.assertLess(float(center(render('render_texture_original')).max()), .03)

    def test_render_texture_depth_and_normal(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from filter_graph_test_utils import attach_filter_material
        from test_render_texture_updates import capture, sample
        mat = body_material(alpha=0)
        sphere(mat)
        shell = next(n for n in mat.node_tree.nodes if n.bl_idname == 'ShaderNodeOutputOutlineShell')
        # A real opaque normal behind the shell makes both data channels observable.
        bpy.ops.mesh.primitive_plane_add(size=6, location=(0, 2, 0),
                                        rotation=(math.pi / 2 + .7, 0, 0))
        background = body_material('Background surface', alpha=1, shell=False)
        bpy.context.object.data.materials.append(background)
        tree = background.node_tree
        emission = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeEmission')
        emission.inputs['Color'].default_value = (*BACKGROUND, 1)
        diffuse = tree.nodes.new('ShaderNodeBsdfDiffuse')
        diffuse.inputs['Color'].default_value = (0, 0, 0, 1)
        add = tree.nodes.new('ShaderNodeAddShader')
        tree.links.new(emission.outputs[0], add.inputs[0])
        tree.links.new(diffuse.outputs[0], add.inputs[1])
        output = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeOutputMaterial')
        tree.links.new(add.outputs[0], output.inputs['Surface'])
        rt = capture(self.scene, size=(79, 53), source='DEPTH', fmt='R32F')
        fm = bpy.data.materials.new('Data capture reader')
        fm.eevee_domain = 'FILTER'
        fm.node_tree.nodes.clear()
        reader = sample(fm.node_tree, rt)
        out = fm.node_tree.nodes.new('ShaderNodeOutputFilter')
        out.inputs['Alpha'].default_value = 1
        fm.node_tree.links.new(reader.outputs['Color'], out.inputs['Color'])
        attach_filter_material(fm)
        for source, fmt in (('DEPTH', 'R32F'), ('NORMAL', 'RGBA32F')):
            with self.subTest(source=source):
                rt.source, rt.format = source, fmt
                mat.use_outline_shell_self_occlusion = False
                shell.inputs['Alpha'].default_value = 0
                reference = center(render('data_' + source + '_reference'))
                shell.inputs['Alpha'].default_value = 1
                original = center(render('data_' + source + '_original'))
                mat.use_outline_shell_self_occlusion = True
                fixed = center(render('data_' + source + '_fixed'))
                self.assert_color(fixed, reference)
                self.assertGreater(float(np.max(np.abs(original-reference))), .2)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--test', default=os.environ.get('BLENDER_SELF_TEST', ''))
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])
    gpu.init()
    backend = gpu.platform.backend_type_get()
    print('OUTLINE_SHELL_SELF_BACKEND=' + backend, flush=True)
    print('OUTLINE_SHELL_SELF_BUILD=' + bpy.app.build_hash.decode(), flush=True)
    loader = unittest.defaultTestLoader
    names = args.test.split(',') if args.test else loader.getTestCaseNames(OutlineShellSelfOcclusionTest)
    assert set(names) <= set(loader.getTestCaseNames(OutlineShellSelfOcclusionTest))
    suite = unittest.TestSuite(OutlineShellSelfOcclusionTest(name) for name in names)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    report = {'backend': backend, 'build': bpy.app.build_hash.decode(), 'tests': result.testsRun,
              'failures': len(result.failures), 'errors': len(result.errors), 'renders': RENDERS}
    (OUT / 'summary.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    assert result.wasSuccessful(), 'Outline Shell self-occlusion regressions failed'
    print('OUTLINE_SHELL_SELF_OK', flush=True)


if __name__ == '__main__':
    main()
