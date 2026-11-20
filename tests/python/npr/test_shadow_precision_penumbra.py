# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Real Eevee mask renders and paired render-time measurements; run in isolated Blender.

--suite precision|soft|benchmark|all --out DIR [--label NAME] [--size 256] [--repeat 15]
The fixture uses a camera-invisible geometric blocker, not a postprocess blur or TAA.
"""
import argparse
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time

import bpy
import gpu
import numpy as np

COMPARISONS = []
REFERENCE_DIRECTORY = None

def arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument('--suite', choices=['precision', 'soft', 'regression', 'viewport', 'benchmark', 'all'], default='all')
    parser.add_argument('--out', required=True)
    parser.add_argument('--label', default='candidate')
    parser.add_argument('--size', type=int, default=256)
    parser.add_argument('--repeat', type=int, default=15)
    parser.add_argument('--profile', action='store_true')
    parser.add_argument('--stress', action='store_true', help='Benchmark only maximum-quality Soft Filtered')
    parser.add_argument('--reference-dir', type=Path)
    parser.add_argument('--benchmark-light', choices=['SUN','POINT'])
    parser.add_argument('--benchmark-mode', choices=['NATIVE_BSDF','TEMPORAL','SOFT_FILTERED'])
    parser.add_argument('--benchmark-count', type=int, choices=[1,8])
    parser.add_argument('--warmup', type=int, default=3)
    parser.add_argument('--native-only', action='store_true')
    parser.add_argument('--assert-quality', action='store_true')
    return parser.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])


def configure(size):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    scene.render.resolution_x = size
    scene.render.resolution_y = size
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.render.image_settings.color_mode = 'RGBA'
    scene.render.image_settings.color_depth = '32'
    scene.render.film_transparent = False
    scene.eevee.taa_samples = 1
    scene.eevee.taa_render_samples = 1
    scene.eevee.use_taa_reprojection = False
    scene.eevee.use_shadow_jitter_viewport = False
    scene.eevee.shadow_ray_count = 1
    scene.eevee.shadow_step_count = 1
    scene.eevee.shadow_pool_size = '512'
    scene.eevee.shadow_resolution_scale = 1
    scene.view_settings.view_transform = 'Standard'
    scene.view_settings.look = 'None'
    scene.world = bpy.data.worlds.new('BlackWorld')
    scene.world.color = (0, 0, 0)
    return scene


def receiver_material(mode, quality, classify=True):
    material = bpy.data.materials.new('Mask')
    material.use_nodes = True
    nodes = material.node_tree.nodes
    nodes.clear()
    out = nodes.new('ShaderNodeOutputMaterial')
    if mode == 'NATIVE_BSDF':
        surface = nodes.new('ShaderNodeBsdfPrincipled')
        surface.inputs['Base Color'].default_value = (1, 1, 1, 1)
        surface.inputs['Roughness'].default_value = .8
        material.node_tree.links.new(surface.outputs[0], out.inputs['Surface'])
        return material
    emission = nodes.new('ShaderNodeEmission')
    combine = nodes.new('ShaderNodeCombineXYZ')
    info = nodes.new('ShaderNodeShaderInfo')
    info.shadow_mode = mode
    info.stable_shadow_samples = quality
    for index, name in enumerate(['Shadow', 'Self Shadow', 'Cast Shadow']):
        if index > 0 and not classify:
            continue
        material.node_tree.links.new(info.outputs[name], combine.inputs[index])
    material.node_tree.links.new(combine.outputs[0], emission.inputs['Color'])
    material.node_tree.links.new(emission.outputs[0], out.inputs['Surface'])
    return material


def blocker_material():
    material = bpy.data.materials.new('ShadowOnlyBlocker')
    material.use_nodes = True
    nodes = material.node_tree.nodes
    nodes.clear()
    out = nodes.new('ShaderNodeOutputMaterial')
    mix = nodes.new('ShaderNodeMixShader')
    opaque = nodes.new('ShaderNodeEmission')
    transparent = nodes.new('ShaderNodeBsdfTransparent')
    path = nodes.new('ShaderNodeLightPath')
    links = material.node_tree.links
    links.new(path.outputs['Is Camera Ray'], mix.inputs[0])
    links.new(opaque.outputs[0], mix.inputs[1])
    links.new(transparent.outputs[0], mix.inputs[2])
    links.new(mix.outputs[0], out.inputs['Surface'])
    return material


def fixture(size, mode='TEMPORAL', quality=8, scale=1, kind='SUN', radius=0, checker=False,
            self_shadow=False, light_count=1, resolution_limit=1e-7, classify=True):
    scene = configure(size)
    root = bpy.data.objects.new('UniformScale', None)
    scene.collection.objects.link(root)
    bpy.ops.mesh.primitive_plane_add(size=8)
    receiver = bpy.context.object
    receiver.name = 'Receiver'
    receiver.parent = root
    receiver.data.materials.append(receiver_material(mode, quality, classify))
    vertices, faces = [], []
    rects = []
    if checker:
        cell = .3
        for y in range(-5, 5):
            for x in range(-5, 5):
                if (x+y) % 2 == 0:
                    rects.append((x*cell, y*cell, (x+1)*cell, (y+1)*cell))
    else:
        rects.append((0, -20, 20, 20))
    for x0, y0, x1, y1 in rects:
        first = len(vertices)
        vertices.extend([(x0,y0,1), (x1,y0,1), (x1,y1,1), (x0,y1,1)])
        faces.append(tuple(first+i for i in range(4)))
    mesh = bpy.data.meshes.new('BlockerGeometry')
    mesh.from_pydata(vertices, [], faces)
    blocker = bpy.data.objects.new('Blocker', mesh)
    scene.collection.objects.link(blocker)
    blocker.parent = root
    blocker.data.materials.append(blocker_material())
    if self_shadow:
        bpy.ops.object.select_all(action='DESELECT')
        receiver.select_set(True)
        blocker.select_set(True)
        bpy.context.view_layer.objects.active = receiver
        bpy.ops.object.join()
    cam_data = bpy.data.cameras.new('MaskCamera')
    cam_data.lens = 50
    cam_data.sensor_width = 36
    cam_data.clip_start = max(scale*.001, 1e-6)
    cam_data.clip_end = max(scale*30, .01)
    cam = bpy.data.objects.new('MaskCamera', cam_data)
    scene.collection.objects.link(cam)
    cam.parent = root
    cam.location = (0,0,6)
    scene.camera = cam
    for index in range(light_count):
        data = bpy.data.lights.new('TestLight%d' % index, kind)
        data.energy = 1 if kind == 'SUN' else 1000
        data.use_shadow = True
        data.use_shadow_jitter = False
        data.shadow_filter_radius = 0
        data.shadow_maximum_resolution = resolution_limit
        if kind == 'SUN':
            data.angle = radius
        else:
            data.shadow_soft_size = radius
        light = bpy.data.objects.new(data.name, data)
        scene.collection.objects.link(light)
        light.parent = root
        light.location = (0,0,4)
    root.scale = (scale,)*3
    bpy.context.view_layer.update()
    return scene


def render_mask(scene, path):
    scene.render.filepath = str(path)
    start = time.perf_counter()
    bpy.ops.render.render(write_still=True)
    elapsed = time.perf_counter()-start
    image = bpy.data.images.load(str(path), check_existing=False)
    pixels = np.asarray(image.pixels[:], dtype=np.float32).reshape(image.size[1],image.size[0],4)
    if REFERENCE_DIRECTORY is not None:
        reference_path = REFERENCE_DIRECTORY / path.name
        assert reference_path.is_file(), reference_path
        reference_image = bpy.data.images.load(str(reference_path), check_existing=False)
        reference = np.asarray(reference_image.pixels[:], dtype=np.float32).reshape(pixels.shape)
        bpy.data.images.remove(reference_image)
        COMPARISONS.append({'image':path.name,'max_error':float(np.max(np.abs(pixels-reference))),
                            'mae':float(np.mean(np.abs(pixels-reference)))})
    settings = scene.render.image_settings
    settings.file_format = 'PNG'
    settings.color_depth = '8'
    image.save_render(str(path.with_suffix('.png')), scene=scene)
    settings.file_format = 'OPEN_EXR'
    settings.color_depth = '32'
    bpy.data.images.remove(image)
    assert np.isfinite(pixels).all(), 'Non-finite mask'
    return pixels[:,:,:3], elapsed


def disk_visibility(x, half_width):
    t = np.clip(x/max(half_width,1e-9), -1, 1)
    return (np.arccos(t) - t*np.sqrt(np.maximum(0,1-t*t))) / math.pi


def assert_mask_content(pixels):
    values = pixels[8:-8, 8:-8, 0]
    assert values.min() < .1 and values.max() > .9, (
        'Fixture must contain both lit and occluded receivers', values.min(), values.max())


def replace_blocker_shape(kind):
    blocker = bpy.data.objects['Blocker']
    mesh = bpy.data.meshes.new(kind)
    if kind == 'annulus':
        vertices = [(radius*math.cos(i*2*math.pi/128), radius*math.sin(i*2*math.pi/128), 1)
                    for radius in [.05, .1] for i in range(128)]
        faces = [(i, (i+1)%128, 128+(i+1)%128, 128+i) for i in range(128)]
    else:
        vertices = [(-.03,-20,1),(.03,-20,1),(.03,20,1),(-.03,20,1)]
        faces = [(0,1,2,3)]
    mesh.from_pydata(vertices, [], faces)
    mesh.materials.append(blocker.data.materials[0])
    blocker.data = mesh
    bpy.context.view_layer.update()


def extended_soft_tests(args, out, report, failures):
    for kind in ['SUN', 'POINT']:
        radius = math.tan(.25/2) if kind == 'SUN' else .6/3
        magnification = 1 if kind == 'SUN' else 4/3
        for shape in ['annulus', 'thin_strip']:
            scene = fixture(args.size, mode='SOFT_FILTERED', kind=kind,
                            radius=.25 if kind == 'SUN' else .6)
            replace_blocker_shape(shape)
            pixels, _ = render_mask(scene, out / ('%s_%s.exr' % (kind,shape)))
            if shape == 'annulus':
                # Ring lies inside the filter disk, while its center and eight rim probes are lit.
                expected = 1 - (.1**2-.05**2)*magnification**2/radius**2
                actual = float(pixels[args.size//2-1:args.size//2+1,
                                      args.size//2-1:args.size//2+1,0].mean())
                error = abs(actual-expected)
            else:
                x = ((np.arange(args.size)+.5)/args.size-.5) * 4.32
                expected = 1-disk_visibility(-.03*magnification-x,radius)+disk_visibility(.03*magnification-x,radius)
                actual = pixels[args.size//2-4:args.size//2+4,:,0].mean(axis=0)
                region = np.abs(x)<radius+.05
                error = float(np.mean(np.abs(actual[region]-expected[region])))
            report['cases'].append({'case':shape,'light':kind,'reference_error':float(error)})
            if error > .04: failures.append(kind+' '+shape+' misses interior coverage')

        scene = fixture(args.size, mode='SOFT_FILTERED', kind=kind,
                        radius=.25 if kind == 'SUN' else .6)
        tilt = .45
        bpy.data.objects['Receiver'].rotation_euler.y = tilt
        bpy.context.view_layer.update()
        pixels, _ = render_mask(scene, out / (kind+'_slope.exr'))
        ray_x = ((np.arange(args.size)+.5)/args.size-.5) * .72
        distance = 6/(1-math.tan(tilt)*ray_x)
        x = distance*ray_x
        z = 6-distance
        half_width = radius*(1-z)
        expected = np.array([disk_visibility(value, width) for value,width in zip(x,half_width)])
        measured = pixels[args.size//2-4:args.size//2+4,:,0].mean(axis=0)
        region = np.abs(x)<radius*1.5
        error = float(np.mean(np.abs(measured[region]-expected[region])))
        report['cases'].append({'case':'slope','light':kind,'reference_error':error})
        if error > .04: failures.append(kind+' sloped receiver')

    scene = fixture(args.size, mode='SOFT_FILTERED', kind='POINT', radius=.6)
    bpy.data.objects['Receiver'].location.x = 4
    scene.camera.location.x = 4
    blocker = bpy.data.objects['Blocker']
    for vertex in blocker.data.vertices: vertex.co.x += 3
    bpy.context.view_layer.update()
    pixels, _ = render_mask(scene, out / 'POINT_cube_seam.exr')
    assert_mask_content(pixels)
    measured = pixels[args.size//2-4:args.size//2+4,:,0].mean(axis=0)
    center = args.size//2
    jump = float(np.max(np.abs(np.diff(measured[center-8:center+8]))))
    report['cases'].append({'case':'cube_seam','light':'POINT','max_adjacent_jump':jump})
    if jump > .12: failures.append('Point cube seam discontinuity')


def memory_snapshot():
    helper = os.environ.get('SHADOW_MEMORY_HELPER')
    if not helper:
        return {'available':False}
    try:
        output = subprocess.run(['pwsh', '-NoProfile', '-File', helper, '-TargetPid', str(os.getpid())],
                                capture_output=True, text=True, timeout=30, check=True)
        return json.loads(output.stdout)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        return {'available':False, 'reason':str(error)}


def viewport_tests(args, out, report, failures):
    import gpu
    scene = fixture(args.size, checker=True)
    area = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    space.shading.type = 'RENDERED'
    space.shading.use_scene_world_render = True
    space.shading.use_scene_lights_render = True
    space.overlay.show_overlays = False
    buffers = [gpu.types.GPUOffScreen(args.size,args.size,format='RGBA32F'),
               gpu.types.GPUOffScreen(args.size,args.size,format='RGBA32F')]
    def draw(index=0):
        buffer = buffers[index]
        bpy.context.view_layer.update()
        deps = bpy.context.evaluated_depsgraph_get()
        projection = scene.camera.calc_matrix_camera(deps,x=args.size,y=args.size)
        start = time.perf_counter()
        buffer.draw_view3d(scene,bpy.context.view_layer,space,region,
                           scene.camera.matrix_world.normalized().inverted(),projection,
                           do_color_management=False,draw_background=True)
        pixels = np.array(buffer.texture_color.read(),dtype=np.float32).reshape(args.size,args.size,4)
        assert np.isfinite(pixels).all()
        return pixels[:,:,:3], time.perf_counter()-start
    try:
        reference, _ = draw()
        assert_mask_content(reference)
        for scale in [.01,.001,1.,.001,1.]:
            bpy.data.objects['UniformScale'].scale = (scale,)*3
            scene.camera.data.clip_start = max(scale*.001,1e-6)
            scene.camera.data.clip_end = max(scale*30,.01)
            pixels, elapsed = draw()
            error = float(np.mean(np.abs(pixels[8:-8,8:-8,0]-reference[8:-8,8:-8,0])))
            report['cases'].append({'case':'viewport_scale','scale':scale,
                                    'mae_vs_scale_1':error,'seconds':elapsed})
            if error > .06: failures.append('Viewport scale %g' % scale)
        # Two independent offscreen viewports alternate over the same changing caster bounds.
        for index, offset in enumerate([0.,2.,.1,0.]):
            bpy.data.objects['Blocker'].location.z = offset
            pixels, _ = draw(index%2)
            assert_mask_content(pixels)
            repeated, _ = draw(index%2)
            error = float(np.max(np.abs(repeated-pixels)))
            report['cases'].append({'case':'viewport_bounds_repeat','offset':offset,'max_error':error})
            if error > .001: failures.append('Viewport depth range changed on repeated draw')
    finally:
        for buffer in buffers: buffer.free()


def main():
    global REFERENCE_DIRECTORY
    args = arguments()
    REFERENCE_DIRECTORY = args.reference_dir
    gpu.init()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    report = {'label':args.label, 'hash':bpy.app.build_hash.decode(), 'backend':gpu.platform.backend_type_get(),
              'device':gpu.platform.renderer_get(),
              'resolution':args.size, 'single_sample':True, 'cases':[]}
    failures = []
    if args.suite in ['viewport', 'all']:
        viewport_tests(args,out,report,failures)
    if args.suite in ['precision','all']:
        reference = None
        for scale in [1., .01, .001]:
            scene = fixture(args.size, scale=scale, checker=True)
            pixels, elapsed = render_mask(scene, out / ('precision_%g.exr' % scale))
            if reference is None:
                assert_mask_content(pixels)
                reference = pixels
            error = float(np.mean(np.abs(pixels[8:-8,8:-8,0]-reference[8:-8,8:-8,0])))
            report['cases'].append({'case':'precision','scale':scale,'mae_vs_scale_1':error,'seconds':elapsed})
            if scale != 1 and error > .06: failures.append('Scale %g error %.5f' % (scale,error))
    if args.suite in ['soft','all']:
        for kind in ['SUN','POINT']:
            for self_shadow in [False,True]:
                scene = fixture(args.size, mode='SOFT_FILTERED', kind=kind,
                                radius=.25 if kind=='SUN' else .6, self_shadow=self_shadow)
                label = '%s_%s' % (kind,'self' if self_shadow else 'cast')
                pixels, elapsed = render_mask(scene, out / (label+'.exr'))
                assert_mask_content(pixels)
                x = ((np.arange(args.size)+.5)/args.size-.5) * (6*.036/.050)
                half_width = math.tan(.25/2) if kind=='SUN' else .6/3
                expected = disk_visibility(x,half_width)
                measured = pixels[args.size//2-8:args.size//2+8].mean(axis=0)
                mask = np.abs(x) <= half_width*1.5
                error = float(np.mean(np.abs(measured[mask,0]-expected[mask])))
                relevant = 1 if self_shadow else 2
                classification_error = float(np.mean(np.abs(measured[:,0]-measured[:,relevant])))
                flat_error = float(np.mean(np.abs(measured[:,3-relevant]-1)))
                scene.frame_set(2)
                repeated, _ = render_mask(scene, out / (label+'_repeat.exr'))
                repeat_error = float(np.max(np.abs(repeated-pixels)))
                row = {'case':'soft','light':kind,'self':self_shadow,'mae_reference':error,
                       'classification_error':classification_error,'other_channel_error':flat_error,
                       'repeat_max_error':repeat_error,'seconds':elapsed}
                report['cases'].append(row)
                if error > .02: failures.append(label+' inaccurate penumbra')
                if classification_error > .005 or flat_error > .005: failures.append(label+' classification')
                if repeat_error > .001: failures.append(label+' unstable')
        extended_soft_tests(args, out, report, failures)
    if args.suite in ['regression', 'all']:
        for kind in ['SUN', 'POINT']:
            reference = None
            for classify in [True,False]:
                scene = fixture(args.size, mode='SOFT_FILTERED', kind=kind,
                                radius=.25 if kind=='SUN' else .6, classify=classify)
                pixels, _ = render_mask(scene, out / ('visibility_%s_%s.exr' % (kind,classify)))
                if reference is None:
                    reference = pixels[:,:,0]
                else:
                    error = float(np.max(np.abs(pixels[:,:,0]-reference)))
                    report['cases'].append({'case':'visibility_without_classification','light':kind,'max_error':error})
                    if error > .001: failures.append(kind+' visibility-only path differs')
        for kind in ['SUN', 'POINT', 'SPOT', 'AREA']:
            for mode in ['TEMPORAL', 'SOFT_FILTERED']:
                scene = fixture(args.size, mode=mode, kind=kind, radius=0)
                label = 'regression_%s_%s' % (kind, mode)
                pixels, _ = render_mask(scene, out / (label+'.exr'))
                assert_mask_content(pixels)
                # Disabling shadows must return the unoccluded value on all three outputs.
                for light in bpy.data.lights:
                    light.use_shadow = False
                plain, _ = render_mask(scene, out / (label+'_disabled.exr'))
                quarter = args.size // 4
                error = float(np.max(np.abs(plain[quarter:-quarter,quarter:-quarter]-1)))
                report['cases'].append({'case':'regression', 'light':kind, 'mode':mode,
                                        'disabled_max_error':error})
                # Render target/film arithmetic is half precision. The old SPOT path already
                # differs by 3 half-float ULPs at 1; keep the bound below one 8-bit mask step.
                if error > .002: failures.append(label+' shadows disabled')
    if args.suite in ['benchmark','all']:
        for kind in ['SUN', 'POINT']:
            if args.benchmark_light and kind != args.benchmark_light: continue
            for mode in (['SOFT_FILTERED'] if args.stress else ['NATIVE_BSDF', 'TEMPORAL', 'SOFT_FILTERED']):
                if args.native_only and mode == 'SOFT_FILTERED': continue
                if args.benchmark_mode and mode != args.benchmark_mode: continue
                for lights in [1,8]:
                    if args.benchmark_count and lights != args.benchmark_count: continue
                    quality = 32 if args.stress else 8
                    scene = fixture(args.size, mode=mode, kind=kind, quality=quality,
                                    radius=.25 if kind == 'SUN' else .6, light_count=lights,
                                    resolution_limit=.002 if kind == 'SUN' else 1e-7)
                    scene.eevee.use_performance_profiler = args.profile
                    label = 'benchmark_%s_%s_%s' % (kind, mode, lights)
                    preview, first_render_seconds = render_mask(scene, out / (label+'.exr'))
                    if mode != 'NATIVE_BSDF':
                        assert_mask_content(preview)
                    samples = []
                    warmup_seconds = []
                    for index in range(args.repeat+args.warmup):
                        start = time.perf_counter()
                        bpy.ops.render.render(write_still=False)
                        elapsed = time.perf_counter()-start
                        if index >= args.warmup: samples.append(elapsed)
                        else: warmup_seconds.append(elapsed)
                    report['cases'].append({'case':'benchmark','mode':mode,'light':kind,'lights':lights,'quality':quality,
                        'median_seconds':statistics.median(samples),'p95_seconds':float(np.percentile(samples,95)),
                        'samples_seconds':samples,
                        'first_render_seconds':first_render_seconds,'warmup_seconds':warmup_seconds,
                        'resolution_limit':.002 if kind == 'SUN' else 1e-7,
                        'iterations':len(samples), 'memory':memory_snapshot(),
                        'note':'Blocking frame wall time, not isolated GPU timer.'})
                    if args.profile:
                        (out / (label+'_cpu_profile.txt')).write_text(
                            scene.eevee.performance_profiler_render_report, encoding='utf-8')
                    (out/'report_partial.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    report['failures'] = failures
    if COMPARISONS:
        report['reference_comparisons'] = COMPARISONS
        if max(case['max_error'] for case in COMPARISONS) > .001:
            failures.append('Optimized kernel differs from reference path by more than .001')
    (out/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print('SHADOW_REPORT '+json.dumps({'label':args.label,'backend':report['backend'],
          'cases':len(report['cases']),'failures':failures,'path':str(out/'report.json')}),flush=True)
    if args.assert_quality: assert not failures, failures
    print('SHADOW_PRECISION_PENUMBRA_COMPLETE',flush=True)


if __name__ == '__main__':
    main()
