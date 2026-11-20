# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Single-frame area-light penumbra and local-shadow crop invariance regressions."""
import argparse
import json
import math
from pathlib import Path
import sys

import bpy
import gpu
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_shadow_precision_penumbra import fixture, render_mask


def area_reference(size, slope, light, aspect, rectangle):
    """Exact visibility of the x>=0,z=1 half-plane under a planar emitter at z=4."""
    ray_x = ((np.arange(size) + .5) / size - .5) * .72
    distance = 6 / (1 - math.tan(slope) * ray_x)
    x, z = distance * ray_x, 6 - distance
    rotation = np.asarray(light.matrix_world.to_3x3(), dtype=float)
    du, dv = rotation[:, 0] * .8, rotation[:, 1] * (.8 * aspect)
    a, b = x * du[2] + (1 - z) * du[0], x * dv[2] + (1 - z) * dv[0]
    c = x * 3
    if not rectangle:
        t = np.clip(c / np.sqrt(a * a + b * b), -1, 1)
        return (np.arccos(t) - t * np.sqrt(np.maximum(0, 1 - t * t))) / math.pi
    a, b = np.abs(a), np.abs(b)
    t = -c + a + b
    positive2 = lambda value: np.maximum(value, 0) ** 2
    return np.clip((positive2(t) - positive2(t - 2 * a) - positive2(t - 2 * b) +
                    positive2(t - 2 * (a + b))) / (8 * a * b), 0, 1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])
    args.out.mkdir(parents=True, exist_ok=True)
    gpu.init()
    report = {'backend': gpu.platform.backend_type_get(), 'cases': [], 'failures': []}
    size = 320
    for shape, aspect, light_tilt, slope, empty in [
            ('DISK', 1, 0, 0, False), ('DISK', 1, 0, .45, False),
            ('DISK', 1, .55, .45, False), ('ELLIPSE', .3, .55, .45, False),
            ('RECTANGLE', .3, 0, 0, False), ('RECTANGLE', .3, .55, .45, False),
            ('DISK', 1, .55, .45, True)]:
        scene = fixture(size, mode='SOFT_FILTERED', kind='POINT', radius=.8)
        light = bpy.data.objects['TestLight0']
        light.data.type = 'AREA'
        light.data.shape = shape
        light.data.size, light.data.size_y = 1.6, 1.6 * aspect
        light.rotation_euler = (0, light_tilt, .3)
        bpy.data.objects['Receiver'].rotation_euler.y = slope
        if empty:
            bpy.data.objects.remove(bpy.data.objects['Blocker'], do_unlink=True)
        bpy.context.view_layer.update()
        label = '%s_%g_%g_%g_%s' % (shape, aspect, light_tilt, slope, empty)
        pixels, _ = render_mask(scene, args.out / (label + '.exr'))
        expected = (np.ones(size) if empty else
                    area_reference(size, slope, light, aspect, shape == 'RECTANGLE'))
        measured = pixels[size // 2 - 2:size // 2 + 2].mean(axis=0)
        region = (expected > .01) & (expected < .99) if not empty else np.ones(size, dtype=bool)
        error = float(np.abs(measured[region, 0] - expected[region]).mean())
        classification_error = float(np.abs(measured[:, 0] - measured[:, 2]).mean())
        self_error = float(np.abs(measured[:, 1] - 1).max())
        report['cases'].append({'case': label, 'reference_mae': error,
                                'classification_mae': classification_error,
                                'self_max_error': self_error})
        if error > (.005 if empty else .035):
            report['failures'].append(label + ': incorrect penumbra')
        if classification_error > .005 or self_error > .005:
            report['failures'].append(label + ': incorrect caster classification')

    # Both camera views sample the same world-space pixels. Fine-page coverage differs.
    for kind in ('POINT', 'AREA'):
        scene = fixture(512, mode='SOFT_FILTERED', kind='POINT', radius=1.2)
        if kind == 'AREA':
            light = bpy.data.objects['TestLight0']
            light.data.type = 'AREA'
            light.data.shape, light.data.size = 'DISK', 2.4
        full, _ = render_mask(scene, args.out / (kind + '_full.exr'))
        scene.render.resolution_x = scene.render.resolution_y = 256
        scene.camera.data.lens, scene.camera.data.shift_x = 100, .5
        crop, _ = render_mask(scene, args.out / (kind + '_crop.exr'))
        error = float(np.abs(full[128:384, 256:512] - crop).max())
        report['cases'].append({'case': kind + '_crop', 'max_error': error})
        if error > .002:
            report['failures'].append(kind + ': page coverage changes the penumbra')
        scene.frame_set(2)
        repeat, _ = render_mask(scene, args.out / (kind + '_repeat.exr'))
        error = float(np.abs(repeat - crop).max())
        report['cases'].append({'case': kind + '_repeat', 'max_error': error})
        if error > .001:
            report['failures'].append(kind + ': unstable without temporal accumulation')
    (args.out / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('SOFT_FILTER_REPORT ' + json.dumps(report), flush=True)
    assert not report['failures'], report['failures']
    print('SOFT_FILTER_TEST_COMPLETE', flush=True)


if __name__ == '__main__':
    main()
