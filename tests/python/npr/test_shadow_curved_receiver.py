# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Single-frame smooth-receiver convergence and real self/cast-shadow regressions."""
import argparse
import json
import math
import os
from pathlib import Path
import sys

import bpy
import gpu
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_shadow_precision_penumbra import (
    blocker_material, configure, receiver_material, render_mask,
)


def fixture(size, kind, dense=False, blocker=False):
    scene = configure(size)
    bpy.ops.mesh.primitive_uv_sphere_add(
        segments=128 if dense else 32, ring_count=64 if dense else 16, radius=1)
    receiver = bpy.context.object
    receiver.name = 'CurvedReceiver'
    for face in receiver.data.polygons:
        face.use_smooth = True
    receiver.shadow_terminator_normal_offset = 0
    receiver.data.materials.append(receiver_material('SOFT_FILTERED', 8))

    camera_data = bpy.data.cameras.new('Camera')
    camera_data.type = 'ORTHO'
    camera_data.ortho_scale = 2.4
    camera = bpy.data.objects.new('Camera', camera_data)
    scene.collection.objects.link(camera)
    camera.location = (0, -5, 0)
    camera.rotation_euler = (-camera.location).to_track_quat('-Z', 'Y').to_euler()
    scene.camera = camera

    light_data = bpy.data.lights.new('Light', kind)
    light = bpy.data.objects.new('Light', light_data)
    scene.collection.objects.link(light)
    light.location = (4, -2, 1)
    light.rotation_euler = (-light.location).to_track_quat('-Z', 'Y').to_euler()
    light_data.energy = 10 if kind == 'SUN' else 1000
    light_data.use_shadow_jitter = False
    light_data.shadow_filter_radius = 0
    light_data.shadow_maximum_resolution = 1e-7
    if kind == 'SUN':
        light_data.angle = .12
    elif kind == 'POINT':
        light_data.shadow_soft_size = math.tan(.06) * light.location.length
    else:
        light_data.shape = 'DISK'
        light_data.size = 2 * math.tan(.06) * light.location.length

    if blocker:
        bpy.ops.mesh.primitive_plane_add(size=2, location=light.location * .4)
        plane = bpy.context.object
        plane.name = 'SeparateCaster'
        plane.rotation_euler = light.rotation_euler
        plane.scale = (.18, 1.8, 1)
        plane.data.materials.append(blocker_material())
    return scene


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])
    args.out.mkdir(parents=True, exist_ok=True)
    gpu.init()
    size = 384
    axis = ((np.arange(size) + .5) / size - .5) * 2.4
    x, z = np.meshgrid(axis, axis)
    # Exclude the silhouette where coarse and dense meshes represent different geometry.
    inside = x * x + z * z < .85
    y = -np.sqrt(np.maximum(1 - x * x - z * z, 1e-8))
    position = np.stack((x, y, z), axis=-1)
    report = {'backend': gpu.platform.backend_type_get(), 'cases': [], 'failures': []}
    cache_disabled = os.environ.get('BLENDER_NPR_SHADOW_CACHE_DISABLE')
    try:
        for kind in ('POINT', 'SUN', 'AREA'):
            os.environ.pop('BLENDER_NPR_SHADOW_CACHE_DISABLE', None)
            low, _ = render_mask(fixture(size, kind), args.out / (kind + '_coarse.exr'))
            dense, _ = render_mask(fixture(size, kind, dense=True),
                                   args.out / (kind + '_dense.exr'))
            light_vector = np.broadcast_to(np.array((4., -2., 1.)), position.shape)
            if kind != 'SUN':
                light_vector = light_vector - position
            light_vector = light_vector / np.linalg.norm(light_vector, axis=-1)[..., None]
            facing = (position * light_vector).sum(axis=-1)
            terminator = inside & (np.abs(facing) < .2)
            error = float(np.abs(low[:, :, 0][terminator] -
                                 dense[:, :, 0][terminator]).mean())
            self_error = float(np.abs(low[:, :, 0][inside] - low[:, :, 1][inside]).mean())
            cast_error = float(np.abs(low[:, :, 2][inside] - 1).mean())
            row = {'case': kind + '_smooth_receiver', 'coarse_dense_mae': error,
                   'self_classification_mae': self_error, 'cast_visibility_error': cast_error,
                   'dark': float(low[:, :, 0][inside].min()),
                   'lit': float(low[:, :, 0][inside].max())}
            report['cases'].append(row)
            if error > .025 or self_error > .005 or cast_error > .005:
                report['failures'].append(kind + ': faceted terminator or incorrect classification')
            if row['dark'] > .1 or row['lit'] < .9:
                report['failures'].append(kind + ': actual self shadow missing')

            os.environ['BLENDER_NPR_SHADOW_CACHE_DISABLE'] = '1'
            direct, _ = render_mask(fixture(size, kind), args.out / (kind + '_direct.exr'))
            direct_error = float(np.abs(direct[:, :, 0][terminator] -
                                        dense[:, :, 0][terminator]).mean())
            report['cases'].append({'case': kind + '_direct',
                                    'coarse_dense_mae': direct_error})
            if direct_error > .035:
                report['failures'].append(kind + ': direct-query terminator remains faceted')

            os.environ.pop('BLENDER_NPR_SHADOW_CACHE_DISABLE', None)
            external, _ = render_mask(fixture(size, kind, blocker=True),
                                      args.out / (kind + '_external.exr'))
            lit_region = inside & (facing > .25)
            darkening = float((low[:, :, 0][lit_region] -
                               external[:, :, 0][lit_region]).mean())
            cast_fraction = float((external[:, :, 2][lit_region] < .9).mean())
            identity_error = float(np.abs(external[:, :, 0][inside] -
                                           (external[:, :, 1][inside] +
                                            external[:, :, 2][inside] - 1)).mean())
            report['cases'].append({'case': kind + '_separate_caster',
                'mean_darkening': darkening, 'cast_fraction': cast_fraction,
                'classification_mae': identity_error})
            if darkening < .02 or cast_fraction < .01 or identity_error > .005:
                report['failures'].append(kind + ': separate caster lost or misclassified')
    finally:
        if cache_disabled is None:
            os.environ.pop('BLENDER_NPR_SHADOW_CACHE_DISABLE', None)
        else:
            os.environ['BLENDER_NPR_SHADOW_CACHE_DISABLE'] = cache_disabled
    (args.out / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('CURVED_RECEIVER_REPORT ' + json.dumps(report), flush=True)
    assert not report['failures'], report['failures']
    print('SHADOW_CURVED_RECEIVER_COMPLETE', flush=True)


if __name__ == '__main__':
    main()
