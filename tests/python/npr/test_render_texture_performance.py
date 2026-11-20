# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Capture resource reuse, output history demand, probe work and view-local fallbacks."""

import json
from pathlib import Path
import re
import sys
import time

import bpy
import gpu
import numpy as np

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_render_texture_updates as rt
from material_compat_test_utils import OUT, scene, render, center, near
from filter_graph_test_utils import attach_filter_material


def setup(source='NORMAL', size=(79, 53), mode='EVERY_FRAME'):
    s, material, emission = scene()
    s.eevee.use_shadows = s.eevee.use_raytracing = s.eevee.use_outline = False
    emission.inputs['Color'].default_value = (.2, .3, .4, 1)
    diffuse = material.node_tree.nodes.new('ShaderNodeBsdfDiffuse')
    diffuse.inputs['Color'].default_value = (0, 0, 0, 1)
    add = material.node_tree.nodes.new('ShaderNodeAddShader')
    output = next(n for n in material.node_tree.nodes if n.type == 'OUTPUT_MATERIAL')
    material.node_tree.links.new(emission.outputs[0], add.inputs[0])
    material.node_tree.links.new(diffuse.outputs[0], add.inputs[1])
    material.node_tree.links.new(add.outputs[0], output.inputs['Surface'])
    target = rt.capture(s, source, mode=mode, size=size)
    fm = bpy.data.materials.new('Performance capture reader')
    fm.eevee_domain = 'FILTER'
    fm.node_tree.nodes.clear()
    rt.filter_samples(fm.node_tree, [(target, 1)])
    attach_filter_material(fm)
    return s, material, emission, target, fm


def planar():
    s, _, _, target, _ = setup()
    probe = bpy.data.objects.new('Planar', bpy.data.lightprobes.new('Planar', 'PLANE'))
    s.collection.objects.link(probe)
    probe.location.z = -.2
    near(center(render('normal_planar')), [.5, .5, 1])
    return {'uid': target.uid}


def storage():
    s, _, _, target, fm = setup('COLOR', mode='EVERY_SAMPLE')
    s.eevee.taa_render_samples = 4
    bpy.context.view_layer.use_pass_normal = True
    b = rt.capture(s, mode='EVERY_SAMPLE', size=(127, 91))
    c = rt.capture(s, mode='EVERY_SAMPLE', size=(53, 83))
    fm.node_tree.nodes.clear()
    rt.filter_samples(fm.node_tree, [(target, .1), (b, .2), (c, .3)])
    near(center(render('storage')), [.12, .18, .24])
    return {'uids': [target.uid, b.uid, c.uid]}


def history():
    s, material, emission, target, fm = setup('COLOR', (512, 384), 'MANUAL')
    emission.inputs['Color'].default_value = (.1, 0, 0, 1)
    viewport = rt.Viewport()
    records = []
    try:
        near(viewport.draw(), [.1, 0, 0])
        sample = rt.sample(material.node_tree, target)
        add = material.node_tree.nodes.new('ShaderNodeVectorMath')
        add.operation = 'ADD'
        add.inputs[1].default_value = (.1, 0, 0)
        material.node_tree.links.new(sample.outputs['Color'], add.inputs[0])
        material.node_tree.links.new(add.outputs[0], emission.inputs['Color'])
        near(viewport.draw(), [.1, 0, 0])
        s.eevee.render_textures.request_update(0)
        actual = viewport.draw()
        near(actual, [.2, 0, 0])
        records.append(actual.tolist())
        material.node_tree.links.remove(emission.inputs['Color'].links[0])
        emission.inputs['Color'].default_value = (.3, 0, 0, 1)
        near(viewport.draw(), [.2, 0, 0])
        s.eevee.render_textures.request_update(0)
        actual = viewport.draw()
        near(actual, [.3, 0, 0])
        records.append(actual.tolist())
    finally:
        viewport.free()
    return {'uid': target.uid, 'records': records}


def shared_storage():
    s, _, _, a, fm = setup('COLOR', mode='EVERY_SAMPLE')
    s.eevee.use_raytracing = True
    s.eevee.taa_render_samples = 4
    b = rt.capture(s, mode='EVERY_SAMPLE', size=(79, 53))
    fm.node_tree.nodes.clear()
    rt.filter_samples(fm.node_tree, [(a, .25), (b, .75)])
    near(center(render('shared_storage')), [.2, .3, .4])
    return {'uids': [a.uid, b.uid]}


def view_fallback():
    s, _, _, a, fm = setup()
    camera = bpy.data.objects.new('Offset capture', s.camera.data.copy())
    camera.location = (20, 0, 3)
    s.collection.objects.link(camera)
    b = rt.capture(s, 'NORMAL', camera=camera)
    bpy.ops.mesh.primitive_plane_add(size=4, location=(20, 0, .1))
    outside = bpy.context.object
    material = bpy.data.materials.new('Stencil shared across views')
    material.use_nodes = True
    material.use_stencil = True
    outside.data.materials.append(material)
    fm.node_tree.nodes.clear()
    rt.filter_samples(fm.node_tree, [(a, 1), (b, .01)])
    near(center(render('local_one')), [.505, .505, 1.01])
    array = outside.modifiers.new('Extend into the first camera', 'ARRAY')
    array.use_relative_offset = False
    array.use_constant_offset = True
    array.constant_offset_displace = (-20, 0, 0)
    array.count = 2
    near(center(render('local_modifier')), [.505, .505, 1.01])
    outside.modifiers.remove(array)
    # Reusing an already cached GPUMaterial on another object must still mark its visible view.
    inside = bpy.data.objects.new('Shared material inside', outside.data)
    inside.location.z = .1
    s.collection.objects.link(inside)
    near(center(render('local_shared')), [.505, .505, 1.01])
    return {'a': a.uid, 'b': b.uid}


def benchmark():
    s, _, _, target, fm = setup('NORMAL', (256, 256), 'EVERY_SAMPLE')
    s.eevee.taa_render_samples = 4
    s.render.resolution_x = s.render.resolution_y = 128
    probe = bpy.data.objects.new('Planar', bpy.data.lightprobes.new('Planar', 'PLANE'))
    probe.location.z = -.2
    s.collection.objects.link(probe)
    elapsed = []
    for index in range(7):
        begin = time.perf_counter()
        bpy.ops.render.render()
        elapsed.append(time.perf_counter() - begin)
        path = OUT / ('benchmark_' + str(index) + '.exr')
        bpy.data.images['Render Result'].save_render(str(path), scene=s)
        image = bpy.data.images.load(str(path), check_existing=False)
        pixels = np.array(image.pixels[:]).reshape(image.size[1], image.size[0], 4)
        bpy.data.images.remove(image)
        near(center(pixels), [.5, .5, 1])
    return {'seconds': elapsed[2:], 'median_seconds': float(np.median(elapsed[2:]))}


CASES = {'perf_planar': planar, 'perf_storage': storage,
         'perf_history': history, 'perf_view_fallback': view_fallback,
         'perf_shared_storage': shared_storage, 'perf_benchmark': benchmark}


def main():
    if '--rt-case' in sys.argv:
        case = sys.argv[sys.argv.index('--rt-case') + 1]
        result = CASES[case]()
        print('RT_CASE_RESULT', json.dumps(result), flush=True)
        return
    gpu.init()
    results = {}
    for case in ('perf_planar', 'perf_storage', 'perf_shared_storage',
                 'perf_history', 'perf_view_fallback'):
        log = rt.child_output(case, script=__file__)
        calls = re.findall(r'RenderTexture capture uid=(\d+) outputs=(\d+) path=(\w+)', log)
        result = json.loads(re.findall(r'RT_CASE_RESULT (\{[^\r\n]+\})', log)[-1])
        if '--baseline' in sys.argv:
            pass
        elif case == 'perf_planar':
            assert len(calls) == 1 and calls[0][2] == 'normal', calls
            assert log.count('PlanarProbe render ') == 1, 'Only the main view may render the probe'
        elif case == 'perf_storage':
            created = [int(v) for v in re.findall(r'storage_created=(\d+)', log)]
            assert len(calls) == len(created) == 12, (calls, created)
            assert all(v > 0 for v in created[:3]) and not any(created[3:]), created
        elif case == 'perf_shared_storage':
            created = [int(v) for v in re.findall(r'storage_created=(\d+)', log)]
            assert len(calls) == len(created) == 8 and all(c[1] == '1' for c in calls), calls
            assert created[0] > 0 and not any(created[1:]), created
        elif case == 'perf_history':
            histories = re.findall(r'RenderTexture output uid=\d+ current=512x384 history=(\d+x\d+)', log)
            assert histories[0] == histories[-1] == '1x1', histories
            assert '512x384' in histories, histories
        elif case == 'perf_view_fallback':
            assert calls == [(str(result['a']), '1', 'normal'), (str(result['b']), '1', 'color'),
                             (str(result['a']), '1', 'color'), (str(result['b']), '1', 'color'),
                             (str(result['a']), '1', 'color'), (str(result['b']), '1', 'color')], calls
        results[case] = {'pixels': result, 'captures': calls}
        print('RT_PERFORMANCE_CASE_PASS', case, flush=True)
    backend = gpu.platform.backend_type_get()
    if '--gpu-backend' in sys.argv:
        assert backend.lower() == sys.argv[sys.argv.index('--gpu-backend') + 1].lower(), backend
    (OUT / 'render_texture_performance.json').write_text(
        json.dumps({'backend': backend, 'reference': '--baseline' in sys.argv,
                    'cases': results}, indent=2), encoding='utf-8')
    print('RENDER_TEXTURE_PERFORMANCE_REFERENCE' if '--baseline' in sys.argv else
          'RENDER_TEXTURE_PERFORMANCE_PASS', flush=True)


if __name__ == '__main__':
    main()
