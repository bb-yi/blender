# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Render Texture scheduling, demand, batching, format and viewport pixel regressions."""

import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys

import bpy
import gpu
import numpy as np

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import OUT, center, near, render, scene
from filter_graph_test_utils import attach_filter_material


def capture(scene, source='COLOR', mode='EVERY_FRAME', size=(79, 53), fmt='RGBA16F', camera=None):
    rt = scene.eevee.render_textures.add()
    rt.enabled = True
    rt.camera = camera or scene.camera
    rt.source, rt.format, rt.update_mode = source, fmt, mode
    rt.resolution_x, rt.resolution_y = size
    return rt


def sample(tree, rt):
    node = tree.nodes.new('ShaderNodeRenderTexture')
    node.render_texture = f'RENDER_TEXTURE_{rt.uid}'
    uv = tree.nodes.new('ShaderNodeCombineXYZ')
    uv.inputs['X'].default_value = uv.inputs['Y'].default_value = .5
    tree.links.new(uv.outputs['Vector'], node.inputs['Vector'])
    return node


def feedback(mode):
    s, material, emission = scene()
    s.eevee.use_shadows = False
    s.eevee.use_raytracing = False
    s.eevee.use_outline = False
    rt = capture(s, mode=mode)
    node = sample(material.node_tree, rt)
    add = material.node_tree.nodes.new('ShaderNodeVectorMath')
    add.operation = 'ADD'
    add.inputs[1].default_value = (.1, 0, 0)
    material.node_tree.links.new(node.outputs['Color'], add.inputs[0])
    material.node_tree.links.new(add.outputs[0], emission.inputs['Color'])
    return s, material, rt, add


def schedule(mode):
    s, _, _, _ = feedback(mode)
    s.eevee.taa_render_samples = 8
    actual = center(render(mode))
    # Captured feedback is .1, .2, ...; the main view adds .1 again.
    expected = .55 if mode == 'EVERY_SAMPLE' else .2
    # Film's spatial filter can slightly change the weights of the individual TAA samples.
    near(actual, [expected, 0, 0], .02 if mode == 'EVERY_SAMPLE' else .004)
    print('RT_CASE_RESULT', json.dumps({'mode': mode, 'pixel': actual.tolist()}), flush=True)


def filter_samples(tree, captures, *, spatial=False):
    result = None
    texcoord = tree.nodes.new('ShaderNodeTexCoord') if spatial else None
    for rt, scale in captures:
        node = sample(tree, rt)
        if texcoord is not None:
            tree.links.new(texcoord.outputs['Window'], node.inputs['Vector'])
        mul = tree.nodes.new('ShaderNodeVectorMath')
        mul.operation = 'SCALE'
        mul.inputs['Scale'].default_value = scale
        tree.links.new(node.outputs['Color'], mul.inputs[0])
        if result is None:
            result = mul.outputs[0]
        else:
            add = tree.nodes.new('ShaderNodeVectorMath')
            add.operation = 'ADD'
            tree.links.new(result, add.inputs[0])
            tree.links.new(mul.outputs[0], add.inputs[1])
            result = add.outputs[0]
    out = tree.nodes.new('ShaderNodeOutputFilter')
    out.inputs['Alpha'].default_value = 1
    tree.links.new(result, out.inputs['Color'])


def extract_case(kind):
    s, material, emission = scene()
    s.eevee.use_shadows = False
    s.eevee.use_raytracing = False
    s.eevee.use_outline = False
    emission.inputs['Color'].default_value = (.25, .5, .75, 1)
    # Supply one deferred closure so that NORMAL has a surface to extract.
    diffuse = material.node_tree.nodes.new('ShaderNodeBsdfDiffuse')
    diffuse.inputs['Color'].default_value = (0, 0, 0, 1)
    add = material.node_tree.nodes.new('ShaderNodeAddShader')
    output = next(n for n in material.node_tree.nodes if n.type == 'OUTPUT_MATERIAL')
    material.node_tree.links.new(emission.outputs[0], add.inputs[0])
    material.node_tree.links.new(diffuse.outputs[0], add.inputs[1])
    material.node_tree.links.new(add.outputs[0], output.inputs['Surface'])

    if kind == 'unused':
        rt = capture(s, size=(2048, 2048))
        pixel = center(render(kind))
        near(pixel, [.25, .5, .75])
        print('RT_CASE_RESULT', json.dumps({'unused_uid': rt.uid}), flush=True)
        return

    captures = []
    if kind == 'fallback':
        material.use_stencil = True
        captures.append((capture(s, 'NORMAL'), 1))
        expected = [.5, .5, 1]
    elif kind == 'batch':
        for source, fmt, scale in [('DEPTH', 'R32F', .01), ('COLOR', 'RGBA32F', .1),
                                   ('NORMAL', 'RGBA16F', .1)]:
            captures.append((capture(s, source, fmt=fmt), scale))
        unused = capture(s, size=(2048, 2048))
        expected = [.105, .13, .205]
    elif kind == 'batch_lightweight_rt':
        s.eevee.use_raytracing = True
        captures = [(capture(s, 'DEPTH', fmt='R32F'), .01),
                    (capture(s, 'NORMAL'), .1)]
        expected = [.08, .08, .13]
    else:
        source, fmt = kind.split(':')
        captures.append((capture(s, source, fmt=fmt), 1))
        expected = [3, 3, 3] if source == 'DEPTH' else [.5, .5, 1]

    fm = bpy.data.materials.new('Capture readers')
    fm.eevee_domain = 'FILTER'
    fm.node_tree.nodes.clear()
    filter_samples(fm.node_tree, captures)
    attach_filter_material(fm)
    pixel = center(render(kind.replace(':', '_')))
    near(pixel, expected, .005)
    print('RT_CASE_RESULT', json.dumps({'kind': kind, 'pixel': pixel.tolist(),
                                      'uids': [rt.uid for rt, _ in captures]}), flush=True)


class Viewport:
    def __init__(self, width=96, height=64):
        self.width, self.height = width, height
        self.scene = bpy.context.scene
        self.scene.eevee.taa_samples = 1
        self.scene.eevee.use_taa_reprojection = False
        gpu.init()
        area = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
        self.region = next(r for r in area.regions if r.type == 'WINDOW')
        self.space = area.spaces.active
        self.space.shading.type = 'RENDERED'
        self.space.overlay.show_overlays = False
        self.space.shading.use_scene_world_render = True
        self.space.shading.use_scene_lights_render = True
        self.buffer = gpu.types.GPUOffScreen(width, height, format='RGBA16F')

    def draw(self):
        bpy.context.view_layer.update()
        deps = bpy.context.evaluated_depsgraph_get()
        camera = self.scene.camera
        projection = camera.calc_matrix_camera(deps, x=self.width, y=self.height)
        self.buffer.draw_view3d(self.scene, bpy.context.view_layer, self.space, self.region,
                               camera.matrix_world.inverted(), projection,
                               do_color_management=False, draw_background=True)
        pixels = np.array(self.buffer.texture_color.read(), dtype=np.float32)
        pixels = pixels.reshape(self.height, self.width, 4)
        assert np.isfinite(pixels).all()
        return center(pixels)

    def free(self):
        self.buffer.free()


def material_display(mode='EVERY_FRAME'):
    """A separate display must sample UVs independently of its world position."""
    s, _, emission = scene()
    s.eevee.use_shadows = s.eevee.use_raytracing = s.eevee.use_outline = False
    source = bpy.context.object
    emission.inputs['Color'].default_value = (.2, .4, .6, 1)
    camera = s.camera
    main = bpy.data.objects.new('Display camera', camera.data.copy())
    s.collection.objects.link(main)
    main.location = (6, 0, 3)
    s.camera = main
    rt = capture(s, mode=mode, camera=camera, size=(128, 128))
    bpy.ops.mesh.primitive_plane_add(size=2, location=(6, 0, 0))
    display = bpy.context.object
    material = bpy.data.materials.new('Render Texture directly to Surface')
    material.use_nodes = True
    display.data.materials.append(material)
    tree = material.node_tree
    tree.nodes.clear()
    output = tree.nodes.new('ShaderNodeOutputMaterial')
    node = tree.nodes.new('ShaderNodeRenderTexture')
    node.render_texture = f'RENDER_TEXTURE_{rt.uid}'
    tree.links.new(node.outputs['Color'], output.inputs['Surface'])
    coordinates = tree.nodes.new('ShaderNodeTexCoord')
    return s, emission, source, display, rt, tree, node, coordinates


def material_coordinates():
    s, _, _, display, _, tree, node, coordinates = material_display()
    viewport = Viewport()
    records = {}
    try:
        # With no Vector link, the display is outside the capture frustum: black is expected.
        near(viewport.draw(), [0, 0, 0])
        tree.links.new(coordinates.outputs['UV'], node.inputs['Vector'])
        near(viewport.draw(), [.2, .4, .6])
        # A spatially distinct right half detects missing UV attributes or constant UVs.
        bpy.ops.mesh.primitive_plane_add(size=2, location=(.5, 0, .01))
        half = bpy.context.object
        half.scale = (.5, 2, 1)
        material = bpy.data.materials.new('Right half')
        material.use_nodes = True
        half.data.materials.append(material)
        material.node_tree.nodes.clear()
        emit = material.node_tree.nodes.new('ShaderNodeEmission')
        emit.inputs['Color'].default_value = (.6, .1, .2, 1)
        out = material.node_tree.nodes.new('ShaderNodeOutputMaterial')
        material.node_tree.links.new(emit.outputs[0], out.inputs['Surface'])

        def halves(label, left, right):
            viewport.draw()
            pixels = np.array(viewport.buffer.texture_color.read(), dtype=np.float32)
            pixels = pixels.reshape(viewport.height, viewport.width, 4)
            actual = [pixels[viewport.height // 2, x, :3] for x in
                      (viewport.width // 4, 3 * viewport.width // 4)]
            near(actual[0], left)
            near(actual[1], right)
            records[label] = [value.tolist() for value in actual]

        halves('uv_outside', [.2, .4, .6], [.6, .1, .2])
        # Projection is still supported. Put the display behind the source so it cannot
        # obscure the capture, and observe its back face with an independent main camera.
        tree.links.remove(node.inputs['Vector'].links[0])
        display.location = (0, 0, -.5)
        s.camera.location = (0, 0, -3)
        s.camera.rotation_euler = (math.pi, 0, 0)
        halves('projection_inside', [.2, .4, .6], [.6, .1, .2])
        display.location.x = s.camera.location.x = .9
        halves('projection_edge', [.6, .1, .2], [0, 0, 0])
        display.location.x = s.camera.location.x = 6
        near(viewport.draw(), [0, 0, 0])
        tree.links.new(coordinates.outputs['UV'], node.inputs['Vector'])
        halves('uv_restored', [.2, .4, .6], [.6, .1, .2])
    finally:
        viewport.free()
    print('RT_CASE_RESULT', json.dumps({'material_coordinates': records}), flush=True)


def material_refresh(mode='EVERY_FRAME', preview=False, offline=False):
    s, emission, source, display, rt, tree, node, coordinates = material_display(mode)
    tree.links.new(coordinates.outputs['UV'], node.inputs['Vector'])
    records = []
    if offline:
        actual = center(render('material_direct_first'))
        near(actual, [.2, .4, .6])
        records.append(actual.tolist())
        emission.inputs['Color'].default_value = (.6, .1, .2, 1)
        actual = center(render('material_direct_edited'))
        near(actual, [.6, .1, .2])
        records.append(actual.tolist())
    else:
        viewport = Viewport()
        s.eevee.taa_samples = 8
        if preview:
            viewport.space.shading.type = 'MATERIAL'
            viewport.space.shading.use_scene_world = False
            viewport.space.shading.use_scene_lights = False

        def check(expected):
            actual = viewport.draw()
            near(actual, expected)
            records.append(actual.tolist())

        try:
            for _ in range(10):
                check([.2, .4, .6])
            for frame, color in ((1, [.6, .1, .2]), (2, [.1, .7, .3])):
                previous = [.2, .4, .6] if frame == 1 else [.6, .1, .2]
                emission.inputs['Color'].default_value = (*color, 1)
                s.frame_set(frame)
                check(previous if mode == 'MANUAL' else color)
                if mode == 'MANUAL':
                    s.eevee.render_textures.request_update(0)
                    check(color)
            # Scene geometry edits must invalidate a converged Film too.
            source.location.x = 20
            check([.1, .7, .3] if mode == 'MANUAL' else [0, 0, 0])
            if mode == 'MANUAL':
                s.eevee.render_textures.request_update(0)
                check([0, 0, 0])
            source.location.x = 0
            if mode == 'MANUAL':
                s.eevee.render_textures.request_update(0)
            check([.1, .7, .3])
            rt.camera.location.x = 20
            check([.1, .7, .3] if mode == 'MANUAL' else [0, 0, 0])
            if mode == 'MANUAL':
                s.eevee.render_textures.request_update(0)
                check([0, 0, 0])
            rt.camera.location.x = 0
            if mode == 'MANUAL':
                s.eevee.render_textures.request_update(0)
            check([.1, .7, .3])
            for enabled in (True, False):
                rt.use_compositor_output = enabled
                check([.1, .7, .3])
            display.location.x = s.camera.location.x = 8
            check([.1, .7, .3])
        finally:
            viewport.free()
    print('RT_CASE_RESULT', json.dumps({'material_refresh': mode, 'preview': preview,
                                      'offline': offline, 'pixels': records}), flush=True)


def manual_viewport():
    s, _, rt, add = feedback('MANUAL')
    viewport = Viewport()
    records = []
    try:
        near(viewport.draw(), [.2, 0, 0])
        add.inputs[1].default_value = (.2, 0, 0)
        s.frame_set(2)
        actual = viewport.draw()
        near(actual, [.3, 0, 0])
        records.append(actual.tolist())
        # Explicit requests must refresh at the same timeline frame as well as on new frames.
        s.eevee.render_textures.request_update(0)
        near(viewport.draw(), [.5, 0, 0])
        s.eevee.active_render_texture_index = 0
        assert bpy.ops.scene.eevee_render_texture_update() == {'FINISHED'}
        near(viewport.draw(), [.7, 0, 0])
        rt.resolution_x, rt.resolution_y = 63, 47
        near(viewport.draw(), [.4, 0, 0])
        rt.format = 'RGBA32F'
        near(viewport.draw(), [.4, 0, 0])
        rt.enabled = False
        near(viewport.draw(), [.2, 0, 0])
        rt.enabled = True
        near(viewport.draw(), [.4, 0, 0])
    finally:
        viewport.free()
    print('RT_CASE_RESULT', json.dumps({'manual_viewport': records}), flush=True)


def frame_viewport():
    s, _, _, _ = feedback('EVERY_FRAME')
    viewport = Viewport()
    try:
        near(viewport.draw(), [.2, 0, 0])
        s.frame_set(2)
        near(viewport.draw(), [.3, 0, 0])
        s.frame_set(3)
        near(viewport.draw(), [.4, 0, 0])
    finally:
        viewport.free()
    print('RT_CASE_RESULT', json.dumps({'frame_viewport': 'frame changes update feedback'}), flush=True)


def manual_slot_identity():
    s, _, target, add = feedback('MANUAL')
    prefix = capture(s)
    s.eevee.render_textures.move(1, 0)
    viewport = Viewport()
    records = {}

    def unchanged(label):
        actual = viewport.draw()
        near(actual, [.3, 0, 0])
        records[label] = actual.tolist()

    try:
        near(viewport.draw(), [.2, 0, 0])
        add.inputs[1].default_value = (.2, 0, 0)
        unchanged('scene_edit')
        prefix.enabled = False
        unchanged('prefix_disabled')
        prefix.enabled = True
        unchanged('prefix_enabled')
        s.eevee.render_textures.move(0, 1)
        unchanged('reordered')
        s.eevee.render_textures.remove(1)
        unchanged('prefix_removed')
        capture(s)
        s.eevee.render_textures.move(1, 0)
        unchanged('new_prefix_inserted')
        assert s.eevee.render_textures[1].uid == target.uid
        s.eevee.render_textures.request_update(1)
        near(viewport.draw(), [.5, 0, 0])
    finally:
        viewport.free()
    print('RT_CASE_RESULT', json.dumps({'manual_slot_identity': records}), flush=True)


def manual_slot_reorder():
    s, _, emission = scene()
    s.eevee.use_shadows = s.eevee.use_raytracing = s.eevee.use_outline = False
    emission.inputs['Color'].default_value = (.1, .02, .03, 1)
    a = capture(s, mode='MANUAL')
    b = capture(s, mode='MANUAL')
    fm = bpy.data.materials.new('Distinct manual snapshots')
    fm.eevee_domain = 'FILTER'
    fm.node_tree.nodes.clear()
    filter_samples(fm.node_tree, [(a, 1), (b, 2)])
    attach_filter_material(fm)
    viewport = Viewport()
    records = []
    try:
        near(viewport.draw(), [.3, .06, .09])
        emission.inputs['Color'].default_value = (.2, .04, .06, 1)
        s.eevee.render_textures.request_update(0)
        near(viewport.draw(), [.4, .08, .12])
        emission.inputs['Color'].default_value = (.3, .06, .09, 1)
        s.eevee.render_textures.move(0, 1)
        actual = viewport.draw()
        near(actual, [.4, .08, .12])
        records.append(actual.tolist())
        # Insert before both live snapshots. Their former slots must not be overwritten first.
        capture(s)
        s.eevee.render_textures.move(2, 0)
        actual = viewport.draw()
        near(actual, [.4, .08, .12])
        records.append(actual.tolist())
        assert s.eevee.render_textures[1].uid == b.uid
        s.eevee.render_textures.request_update(1)
        near(viewport.draw(), [.8, .16, .24])
    finally:
        viewport.free()
    print('RT_CASE_RESULT', json.dumps({'manual_slot_reorder': records}), flush=True)


def mixed_update_history():
    from mathutils import Vector

    s, _, emission = scene()
    s.render.resolution_x = s.render.resolution_y = 128
    s.eevee.taa_render_samples = 2
    s.eevee.use_shadows = s.eevee.use_outline = False
    s.eevee.use_raytracing = True
    s.eevee.ray_tracing_method = 'SCREEN'
    options = s.eevee.ray_tracing_options
    options.resolution_scale = '1'
    options.trace_max_roughness = 1
    options.screen_trace_quality = options.screen_trace_thickness = 1
    options.use_denoise = False
    camera = s.camera
    camera.data.type = 'PERSP'
    camera.data.lens = 50
    camera.location = (0, -6, 3)
    camera.rotation_euler = (Vector((0, .7, .8)) - camera.location).to_track_quat('-Z', 'Y').to_euler()
    source = bpy.context.object
    source.location = (0, 1, 1.25)
    source.rotation_euler = (math.pi * .5, 0, 0)
    source.scale = (.7, .5, 1)
    emission.inputs['Color'].default_value = (.05, 1, .05, 1)
    emission.inputs['Strength'].default_value = 2
    bpy.ops.mesh.primitive_plane_add(size=12)
    material = bpy.data.materials.new('Reflective floor')
    material.use_nodes = True
    material.node_tree.nodes.clear()
    glossy = material.node_tree.nodes.new('ShaderNodeBsdfGlossy')
    glossy.inputs['Roughness'].default_value = .01
    output = material.node_tree.nodes.new('ShaderNodeOutputMaterial')
    material.node_tree.links.new(glossy.outputs[0], output.inputs['Surface'])
    bpy.context.object.data.materials.append(material)
    a = capture(s, mode='EVERY_FRAME', size=(128, 128))
    b = capture(s, mode='EVERY_SAMPLE', size=(128, 128))
    fm = bpy.data.materials.new('Mixed update history reader')
    fm.eevee_domain = 'FILTER'
    fm.node_tree.nodes.clear()
    filter_samples(fm.node_tree, [(b, 1), (a, 1e-20)], spatial=True)
    attach_filter_material(fm)
    errors = {}
    for denoise in (False, True):
        options.use_denoise = denoise
        a.enabled = False
        reference = render('history_alone_' + str(denoise))
        # Reflections must be visible; a black fallback cannot validate history isolation.
        assert np.count_nonzero(reference[:32, :, 1] > .1) > 100
        a.enabled = True
        for mode in ('EVERY_FRAME', 'MANUAL', 'EVERY_SAMPLE'):
            a.update_mode = mode
            label = mode + '_denoise_' + str(denoise)
            actual = render('history_mixed_' + label)
            error = float(np.max(np.abs(actual - reference)))
            assert error < .003, (label, error)
            errors[label] = error
    print('RT_CASE_RESULT', json.dumps({'mixed_update_history': errors}), flush=True)


def history_views():
    s, material, emission = scene()
    s.eevee.use_shadows = False
    s.eevee.use_outline = False
    s.eevee.use_raytracing = True
    s.eevee.taa_render_samples = 16
    s.eevee.ray_tracing_method = 'SCREEN'
    s.eevee.ray_tracing_options.trace_max_roughness = 1
    s.eevee.ray_tracing_options.use_denoise = True
    emission.inputs['Color'].default_value = (.4, .1, .05, 1)
    glossy = material.node_tree.nodes.new('ShaderNodeBsdfGlossy')
    glossy.inputs['Roughness'].default_value = .25
    add = material.node_tree.nodes.new('ShaderNodeAddShader')
    output = next(n for n in material.node_tree.nodes if n.type == 'OUTPUT_MATERIAL')
    material.node_tree.links.new(emission.outputs[0], add.inputs[0])
    material.node_tree.links.new(glossy.outputs[0], add.inputs[1])
    material.node_tree.links.new(add.outputs[0], output.inputs['Surface'])
    s.world = bpy.data.worlds.new('Reflection environment')
    s.world.use_nodes = True
    s.world.node_tree.nodes.get('Background').inputs['Color'].default_value = (.2, .35, .6, 1)
    camera_b = bpy.data.objects.new('Second capture', s.camera.data.copy())
    s.collection.objects.link(camera_b)
    camera_b.location = (.2, 0, 3)
    camera_b.rotation_euler = (0, .12, 0)
    a = capture(s, mode='EVERY_SAMPLE')
    b = capture(s, mode='EVERY_SAMPLE', size=(63, 91), camera=camera_b)
    fm = bpy.data.materials.new('Independent capture history readers')
    fm.eevee_domain = 'FILTER'
    fm.node_tree.nodes.clear()
    filter_samples(fm.node_tree, [(a, 1), (b, 1e-6)])
    attach_filter_material(fm)
    b.enabled = False
    reference = render('history_one_view')
    b.enabled = True
    actual = render('history_two_views')
    error = float(np.max(np.abs(actual - reference)))
    assert error < .02, error
    near(center(actual), center(reference), .005)
    print('RT_CASE_RESULT', json.dumps({'two_view_max_error': error}), flush=True)


def child(case):
    if case.startswith('schedule:'):
        schedule(case.split(':')[1])
    elif case == 'manual_viewport':
        manual_viewport()
    elif case == 'frame_viewport':
        frame_viewport()
    elif case == 'history_views':
        history_views()
    elif case == 'manual_slot_identity':
        manual_slot_identity()
    elif case == 'manual_slot_reorder':
        manual_slot_reorder()
    elif case == 'mixed_update_history':
        mixed_update_history()
    elif case == 'material_coordinates':
        material_coordinates()
    elif case.startswith('material_refresh:'):
        material_refresh(case.split(':')[1])
    elif case == 'material_preview':
        material_refresh(preview=True)
    elif case == 'material_offline':
        material_refresh(offline=True)
    else:
        extract_case(case)


def child_output(case, script=None):
    assert os.environ.get('BLENDER_USER_CONFIG'), 'Run with isolated Blender user resources'
    gpu.init()
    command = [bpy.app.binary_path, '--background', '--factory-startup', '--gpu-backend',
               gpu.platform.backend_type_get().lower(), '--debug-gpu', '--log', 'eevee',
               '--log-level', '2', '--python-exit-code', '1', '--python', script or __file__,
               '--', '--rt-case', case]
    path = OUT / (case.replace(':', '_') + '.log')
    environment = os.environ.copy()
    environment['PYTHONDONTWRITEBYTECODE'] = '1'
    with path.open('w', encoding='utf-8') as log:
        process = subprocess.Popen(command, cwd=OUT, stdout=log, stderr=subprocess.STDOUT,
                                   env=environment)
        try:
            process.wait(timeout=180)
        except subprocess.TimeoutExpired:
            if os.name == 'nt':
                query = (f'Get-CimInstance Win32_Process -Filter "ProcessId = {process.pid}" | '
                         'Select-Object ProcessId,ExecutablePath,CommandLine | ConvertTo-Json')
                info = json.loads(subprocess.check_output(['pwsh', '-NoProfile', '-Command', query],
                                                         text=True))
                print('TIMED_OUT_TEST_PROCESS', info, flush=True)
                assert Path(info['ExecutablePath']).resolve() == Path(bpy.app.binary_path).resolve()
                assert '--rt-case' in info['CommandLine'] and case in info['CommandLine']
            if 'release' not in [part.lower() for part in Path(bpy.app.binary_path).resolve().parts]:
                process.kill()
                process.wait(timeout=15)
            raise AssertionError(f'Test timed out; inspect PID {process.pid} and {path}') from None
    log = path.read_text(encoding='utf-8', errors='replace')
    assert process.returncode == 0, (case, process.returncode, log[-7000:])
    assert not re.search(r'Missing .*bind|VK_ERROR_|Validation Error', log), log[-7000:]
    return log


def main():
    if '--rt-case' in sys.argv:
        child(sys.argv[sys.argv.index('--rt-case') + 1])
        return
    cases = ['schedule:EVERY_FRAME', 'schedule:EVERY_SAMPLE', 'schedule:MANUAL',
             'unused', 'batch', 'fallback', 'DEPTH:R16F', 'DEPTH:R32F',
             'NORMAL:RGBA16F', 'NORMAL:RGBA32F', 'manual_viewport', 'frame_viewport',
             'history_views', 'manual_slot_identity', 'manual_slot_reorder',
             'mixed_update_history', 'batch_lightweight_rt', 'material_coordinates',
             'material_refresh:EVERY_FRAME', 'material_refresh:EVERY_SAMPLE',
             'material_refresh:MANUAL', 'material_preview', 'material_offline']
    results = {}
    for case in cases:
        log = child_output(case)
        calls = re.findall(r'RenderTexture capture uid=(\d+) outputs=(\d+) path=(\w+)', log)
        if case.startswith('schedule:'):
            expected = 8 if case.endswith('EVERY_SAMPLE') else 1
            assert len(calls) == expected, (case, calls)
        elif case == 'unused':
            assert not calls, calls
        elif case == 'batch':
            assert len(calls) == 1 and calls[0][1:] == ('3', 'color'), calls
        elif case == 'batch_lightweight_rt':
            assert len(calls) == 1 and calls[0][1:] == ('2', 'normal'), calls
        elif case == 'fallback':
            assert len(calls) == 1 and calls[0][2] == 'color', calls
        elif case == 'manual_slot_identity':
            assert len(calls) == 2, calls
        elif case == 'mixed_update_history':
            assert len(calls) == 24 and all(call[1] == '1' for call in calls), calls
        elif case.startswith(('DEPTH:', 'NORMAL:')):
            assert len(calls) == 1 and calls[0][2] == case.split(':')[0].lower(), calls
        payloads = re.findall(r'RT_CASE_RESULT (\{[^\r\n]+\})', log)
        assert payloads, log[-4000:]
        results[case] = {'captures': calls, 'pixels': json.loads(payloads[-1])}
        print('RT_CASE_PASSED', case, calls, flush=True)
    backend = gpu.platform.backend_type_get()
    if '--gpu-backend' in sys.argv:
        assert backend.lower() == sys.argv[sys.argv.index('--gpu-backend') + 1].lower(), backend
    report = {'backend': backend, 'build_hash': bpy.app.build_hash.decode(), 'cases': results}
    (OUT / 'render_texture_updates.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('RENDER_TEXTURE_UPDATES_PASS', flush=True)


if __name__ == '__main__':
    main()
