"""Geometry Lines cache/performance probe using one persistent GPUOffScreen viewport.

Timings include synchronization and GPU readback; they are reproducible measurements, not FPS.
"""

import argparse
import json
import math
import os
from pathlib import Path
import re
import runpy
import subprocess
import sys
import time

import bpy
import numpy as np


def child(directory):
    import gpu
    helpers = runpy.run_path(str(Path(__file__).with_name('test_geometry_lines.py')))
    reset = helpers['reset']
    setup_lines = helpers['setup_lines']
    set_value = helpers['set_value']
    make_settings = helpers['make_settings']
    socket = helpers['socket']
    gpu.init()
    metrics = []
    for label, count in (('5k', 5), ('50k', 50)):
        scene = reset()
        scene.render.resolution_x, scene.render.resolution_y = 512, 384
        scene.eevee.taa_samples = 1
        scene.eevee.use_taa_reprojection = False
        scene.camera.location.z = 20
        scene.camera.data.ortho_scale = 12
        collection = bpy.data.collections.new('Shared Sphere Source')
        bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=17, radius=.45)
        sphere = bpy.context.object
        scene.collection.objects.unlink(sphere)
        collection.objects.link(sphere)
        sphere.data.calc_loop_triangles()
        triangles = len(sphere.data.loop_triangles) * count
        material = bpy.data.materials.new('Benchmark Emission')
        material.use_nodes = True
        material.node_tree.nodes.clear()
        output = material.node_tree.nodes.new('ShaderNodeOutputMaterial')
        emission = material.node_tree.nodes.new('ShaderNodeEmission')
        emission.inputs['Color'].default_value = (.5,.5,.5,1)
        material.node_tree.links.new(emission.outputs[0], output.inputs['Surface'])
        sphere.data.materials.append(material)
        instances = []
        columns = min(count, 10)
        rows = math.ceil(count / columns)
        for index in range(count):
            instance = bpy.data.objects.new('Sphere Instance ' + str(index), None)
            instance.instance_type = 'COLLECTION'
            instance.instance_collection = collection
            instance.location = ((index % columns - (columns - 1) / 2) * 1.05,
                                 (index // columns - (rows - 1) / 2) * 1.05, 0)
            scene.collection.objects.link(instance)
            instances.append(instance)
        tree, source, brush, settings, output = setup_lines(instances, width=2)
        output.mode = 'FULL'
        area = next(area for area in bpy.context.screen.areas if area.type == 'VIEW_3D')
        region = next(region for region in area.regions if region.type == 'WINDOW')
        space = area.spaces.active
        space.shading.type = 'RENDERED'
        space.shading.use_scene_world_render = space.shading.use_scene_lights_render = True
        space.overlay.show_overlays = False
        viewport = gpu.types.GPUOffScreen(512, 384, format='RGBA16F')

        def draw(stage, repetitions=1):
            print('GEOMETRY_LINES_PERF_BEGIN ' + json.dumps({'size': label, 'stage': stage}), flush=True)
            samples = []
            pixels = None
            for _ in range(repetitions):
                begin = time.perf_counter()
                bpy.context.view_layer.update()
                depsgraph = bpy.context.evaluated_depsgraph_get()
                projection = scene.camera.calc_matrix_camera(depsgraph, x=512, y=384)
                viewport.draw_view3d(scene, bpy.context.view_layer, space, region,
                                     scene.camera.matrix_world.inverted(), projection,
                                     do_color_management=False, draw_background=True)
                pixels = np.asarray(viewport.texture_color.read(), dtype=np.float32).reshape(384,512,4)
                assert np.isfinite(pixels).all(), (label, stage, 'non-finite pixels')
                samples.append((time.perf_counter() - begin) * 1000)
            result = {'size': label, 'triangles': triangles, 'instances': count, 'stage': stage,
                      'elapsed_ms': samples, 'median_ms': float(np.median(samples)),
                      'backend': gpu.platform.backend_type_get()}
            metrics.append(result)
            print('GEOMETRY_LINES_PERF_END ' + json.dumps(result), flush=True)
            return pixels.copy()

        try:
            base = draw('base')
            draw('repeat', 3)
            set_value(brush, 'color', (0,.1,1,1))
            colored = draw('color', 3)
            assert np.count_nonzero(np.abs(colored - base) > .1) > 100, 'Color probe must change real pixels'
            set_value(settings, 'split_angle', 100)
            draw('connection')
            scene.camera.location.x += .2
            draw('camera')
            duplicate = make_settings(tree, source, brush)
            combine = tree.nodes.new('NPRLineNodeCombine')
            tree.links.new(settings.outputs[0], socket(combine, 'plans'))
            tree.links.new(duplicate.outputs[0], socket(combine, 'plans'))
            tree.links.new(combine.outputs[0], socket(output, 'plan'))
            draw('shared_rules')
            set_value(duplicate, 'visible_wire', True)
            draw('wire')
        finally:
            viewport.free()
    (directory / 'timings.json').write_text(json.dumps(metrics, indent=2), encoding='utf-8')
    print('GEOMETRY_LINES_PERFORMANCE_CHILD_OK', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True)
    parser.add_argument('--backend', choices=('opengl', 'vulkan'), default='opengl')
    parser.add_argument('--child', action='store_true')
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])
    directory = Path(args.output).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    if args.child:
        child(directory)
        return
    environment = os.environ.copy()
    environment['PYTHONDONTWRITEBYTECODE'] = '1'
    environment['BLENDER_NPR_LINE_STATS'] = '1'
    sandbox = directory / 'user'
    environment['BLENDER_USER_RESOURCES'] = str(sandbox)
    for resource in ('CONFIG', 'SCRIPTS', 'DATAFILES', 'EXTENSIONS'):
        path = sandbox / resource.lower()
        path.mkdir(parents=True, exist_ok=True)
        environment['BLENDER_USER_' + resource] = str(path)
    command = [bpy.app.binary_path, '--background', '--factory-startup', '--gpu-backend', args.backend,
               '--python-exit-code', '1', '--python', str(Path(__file__).resolve()), '--',
               '--output', str(directory), '--backend', args.backend, '--child']
    completed = subprocess.run(command, env=environment, capture_output=True, text=True,
                               encoding='utf-8', errors='replace')
    text = completed.stdout + completed.stderr
    (directory / 'performance.log').write_text(text, encoding='utf-8')
    print(text, flush=True)
    assert completed.returncode == 0, completed.returncode
    assert 'GEOMETRY_LINES_PERFORMANCE_CHILD_OK' in text, text
    statistics = {}
    active = None
    for line in text.splitlines():
        if line.startswith('GEOMETRY_LINES_PERF_BEGIN '):
            entry = json.loads(line.split(' ', 1)[1])
            active = (entry['size'], entry['stage'])
            statistics[active] = []
        elif line.startswith('GEOMETRY_LINES_PERF_END '):
            active = None
        elif line.startswith('NPR_GEOMETRY_LINES ') and active is not None:
            statistics[active].append(dict(re.findall(r'(\w+)=([0-9.]+)', line)))
    for size in ('5k', '50k'):
        assert statistics[(size, 'base')], (size, 'missing CPU statistics')
        assert not statistics[(size, 'repeat')], (size, 'repeated drawing recomputed visibility')
        assert not statistics[(size, 'color')], (size, 'color changed geometry/visibility')
        connection = statistics[(size, 'connection')]
        assert connection and all(float(item['visibility_ms']) == 0 for item in connection), (
            size, 'connection settings reran visibility', connection)
        assert statistics[(size, 'camera')], (size, 'camera change must update visibility')
        base = statistics[(size, 'base')][-1]
        shared = statistics[(size, 'shared_rules')][-1]
        assert base['candidates'] == shared['candidates'], (size, 'same-source rules duplicated geometry')
        wire = statistics[(size, 'wire')][-1]
        assert int(wire['candidates']) > int(base['candidates']), (size, 'wire benchmark did not add edges')
    (directory / 'statistics.json').write_text(
        json.dumps({f'{size}/{stage}': data for (size, stage), data in statistics.items()}, indent=2),
        encoding='utf-8')
    print('GEOMETRY_LINES_PERFORMANCE_OK', flush=True)


if __name__ == '__main__':
    main()
