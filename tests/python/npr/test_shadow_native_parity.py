# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Rendered native/ownership parity and temporal convergence of Shader Info soft shadows.

The comparison uses identical ray counts and light settings. Classification must be
metadata on the same hit, including history-extrapolated hits. --record-only records
an older implementation without suppressing failures in the report.
"""
import argparse
import json
import math
import os
from pathlib import Path
import sys

import bpy
import gpu
import numpy as np
from mathutils import Vector

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_shadow_precision_penumbra import configure, receiver_material, blocker_material, render_mask


def fixture(kind='SUN', mode='TEMPORAL', classify=False, samples=64, quality=4,
            jitter=False, custom=False, curved=False, lights=1, grazing=False):
    scene = configure(192)
    scene.render.resolution_x = 320
    scene.eevee.taa_render_samples = samples
    scene.eevee.shadow_ray_count = 4
    scene.eevee.shadow_step_count = 8
    bpy.ops.mesh.primitive_plane_add(size=60)
    receiver = bpy.context.object
    receiver.name = 'Receiver'
    material = receiver_material(mode, quality, classify)
    receiver.data.materials.append(material)
    if custom:
        geometry = material.node_tree.nodes.new('ShaderNodeNewGeometry')
        info = next(n for n in material.node_tree.nodes if n.bl_idname == 'ShaderNodeShaderInfo')
        material.node_tree.links.new(geometry.outputs['Position'], info.inputs['World Position'])
    bpy.ops.mesh.primitive_monkey_add(location=(0, 0, 1))
    caster = bpy.context.object
    caster.name = 'Caster'
    for face in caster.data.polygons:
        face.use_smooth = True
    caster.data.materials.append(blocker_material())
    if curved:
        bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16, location=(2.5, 0, 1))
        bpy.context.object.data.materials.append(material)
        for face in bpy.context.object.data.polygons:
            face.use_smooth = True
    source = Vector((-math.cos(math.radians(5 if grazing else 20)), 0,
                     math.sin(math.radians(5 if grazing else 20))))
    for i in range(lights):
        data = bpy.data.lights.new('Source%d' % i, 'AREA' if kind in ('RECTANGLE', 'DISK') else kind)
        data.energy = 1 if kind == 'SUN' else 1000
        data.use_shadow_jitter = jitter
        data.shadow_jitter_overblur = 0
        data.shadow_filter_radius = 1
        data.shadow_maximum_resolution = 1e-7
        if kind == 'SUN':
            data.angle = math.radians(2 if grazing else 30)
        elif kind in ('RECTANGLE', 'DISK'):
            data.shape = kind
            data.size = 2
            if kind == 'RECTANGLE':
                data.size_y = 1
        else:
            data.shadow_soft_size = 1
        light = bpy.data.objects.new(data.name, data)
        light.location = source * 8
        light.rotation_euler = (-source).to_track_quat('-Z', 'Y').to_euler()
        scene.collection.objects.link(light)
    camera_data = bpy.data.cameras.new('Camera')
    camera_data.type = 'ORTHO'
    camera_data.ortho_scale = 16
    camera_data.clip_start, camera_data.clip_end = .01, 100
    camera = bpy.data.objects.new('Camera', camera_data)
    camera.location = (4, 0, 16)
    scene.collection.objects.link(camera)
    scene.camera = camera
    bpy.context.view_layer.update()
    return scene


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--record-only', action='store_true')
    parser.add_argument('--quick', action='store_true')
    parser.add_argument('--only', default='')
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    args.out.mkdir(parents=True, exist_ok=True)
    gpu.init()
    report = {'binary': bpy.app.binary_path, 'backend': gpu.platform.backend_type_get(),
              'cases': [], 'failures': []}

    def render(label, **kwargs):
        pixels, elapsed = render_mask(fixture(**kwargs), args.out / (label + '.exr'))
        np.save(args.out / (label + '.npy'), pixels)
        return pixels

    cases = [(k, {}) for k in ('SUN', 'POINT', 'RECTANGLE', 'DISK', 'SPOT')]
    if not args.quick:
        cases += [('SUN', {'jitter': True}), ('POINT', {'jitter': True}),
                  ('RECTANGLE', {'jitter': True}), ('SUN', {'grazing': True}),
                  ('POINT', {'custom': True}), ('SUN', {'curved': True}),
                  ('POINT', {'lights': 10})]
    for index, (kind, options) in enumerate(cases):
        label = '%02d_%s' % (index, kind)
        if args.only and label not in args.only.split(','):
            continue
        options = dict(options, kind=kind, samples=16 if args.quick else 64)
        native = render(label + '_native', **options)
        classified = render(label + '_classified', classify=True, **options)
        soft = render(label + '_soft', mode='SOFT_FILTERED', classify=True, **options)
        os.environ['BLENDER_NPR_SHADOW_CACHE_DISABLE'] = '1'
        try:
            direct = render(label + '_direct', mode='SOFT_FILTERED', classify=True, **options)
            direct_plain = render(label + '_direct_plain', mode='SOFT_FILTERED', **options)
        finally:
            os.environ.pop('BLENDER_NPR_SHADOW_CACHE_DISABLE', None)
        region = np.s_[4:-4, 4:-4, 0]
        row = {'case': label, 'options': options}
        for name, image in [('classified', classified), ('soft', soft),
                            ('direct', direct), ('direct_plain', direct_plain)]:
            error = np.abs(image[region] - native[region])
            row[name + '_mae'] = float(error.mean())
            row[name + '_max'] = float(error.max())
            row[name + '_p99'] = float(np.percentile(error, 99))
            # Cache reconstruction may shift an individual shadow texel. The direct and
            # classified evaluations must agree exactly (apart from film rounding).
            if name != 'soft' and error.max() > .002:
                report['failures'].append(label + ': ' + name + ' changed native hit')
            # Jittered maps require pre-material visibility history for nonlinear
            # ramps. Its bounded mean differs from a single native film estimate.
            # The unjittered path must still agree without this extra estimator.
            mean_limit, tail_limit = (.015, .09) if options.get('jitter') else (.002, .02)
            if name == 'soft' and (error.mean() > mean_limit or
                                   np.percentile(error, 99) > tail_limit):
                report['failures'].append(label + ': cached tracing differs from native')
        report['cases'].append(row)
        (args.out / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        print('NATIVE_PARITY', json.dumps(row), flush=True)
    if not args.quick and not args.only:
        # Check actual convergence, with the same light projection as the native reference.
        reference = render('convergence_native256', samples=256, jitter=True)[:, :, 0]
        errors = []
        region = (reference > .02) & (reference < .98)
        for count in (1, 16, 64):
            image = render('convergence_soft%d' % count, samples=count, jitter=True,
                           mode='SOFT_FILTERED', classify=True)[:, :, 0]
            errors.append(float(np.abs(image[region] - reference[region]).mean()))
        report['convergence'] = {'samples': [1, 16, 64], 'penumbra_mae': errors}
        if not errors[2] < errors[0] * .4 or not errors[2] < errors[1] or errors[2] > .025:
            report['failures'].append('Soft samples do not converge toward native reference')
        for jitter in (False, True):
            scene = fixture(mode='SOFT_FILTERED', quality=8, samples=64, jitter=jitter)
            mat = bpy.data.objects['Receiver'].data.materials[0]
            nodes, links = mat.node_tree.nodes, mat.node_tree.links
            info = next(n for n in nodes if n.bl_idname == 'ShaderNodeShaderInfo')
            emission = next(n for n in nodes if n.bl_idname == 'ShaderNodeEmission')
            ramp = nodes.new('ShaderNodeValToRGB')
            ramp.color_ramp.elements[0].position = .15
            ramp.color_ramp.elements[0].color = (.02, .04, .15, 1)
            ramp.color_ramp.elements.new(.65).color = (1, .26, .06, 1)
            ramp.color_ramp.elements[-1].color = (.95, .95, .95, 1)
            links.new(info.outputs['Shadow'], ramp.inputs['Fac'])
            links.new(ramp.outputs['Color'], emission.inputs['Color'])
            pixels, _ = render_mask(scene, args.out / ('ramp_%s.exr' % jitter))
            orange = (pixels[:, :, 0] - pixels[:, :, 2] > .15) & (pixels[:, :, 0] > .2)
            # The midpoint orange is outside the convex mixture of endpoint colors.
            # Binary material inputs followed by film averaging cannot pass this.
            report.setdefault('ramp', []).append({'jitter': jitter,
                                                   'midpoint_pixels': int(orange.sum())})
            if orange.sum() < 100:
                report['failures'].append('ColorRamp received no intermediate shadow values')
    (args.out / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    if not args.record_only:
        assert not report['failures'], report['failures']
    print('SHADOW_NATIVE_PARITY_COMPLETE', flush=True)


if __name__ == '__main__':
    main()
