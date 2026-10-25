# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Render shadows and exact geometric normals on tiny translated meshes without TAA."""
import argparse
import json
import os
from pathlib import Path
import sys

import bpy
import gpu
import numpy as np
from mathutils import Vector

sys.path.insert(0, str(Path(os.environ.get('NPR_RELEASE_SOURCE_DIR', Path(__file__).resolve().parents[3])) /
                       'tests' / 'python' / 'npr'))
from test_shadow_precision_penumbra import fixture, render_mask


def arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--size', type=int, default=256)
    return parser.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])


def normal_material():
    material = bpy.data.materials.new('ExactGeometricNormal')
    material.use_nodes = True
    nodes = material.node_tree.nodes
    nodes.clear()
    geometry = nodes.new('ShaderNodeNewGeometry')
    encode = nodes.new('ShaderNodeVectorMath')
    encode.operation = 'MULTIPLY_ADD'
    encode.inputs[1].default_value = (.5, .5, .5)
    encode.inputs[2].default_value = (.5, .5, .5)
    emission = nodes.new('ShaderNodeEmission')
    output = nodes.new('ShaderNodeOutputMaterial')
    links = material.node_tree.links
    links.new(geometry.outputs['True Normal'], encode.inputs[0])
    links.new(encode.outputs['Vector'], emission.inputs['Color'])
    links.new(emission.outputs[0], output.inputs['Surface'])
    return material


def main():
    args = arguments()
    args.out.mkdir(parents=True, exist_ok=True)
    gpu.init()
    report = {'backend': gpu.platform.backend_type_get(), 'binary': bpy.app.binary_path,
              'single_sample': True, 'cases': [], 'failures': []}
    for rotated in [False, True]:
        scene = fixture(args.size, scale=1e-4)
        bpy.data.objects['Blocker'].hide_render = True
        receiver = bpy.data.objects['Receiver']
        receiver.data.materials.clear()
        receiver.data.materials.append(normal_material())
        root = bpy.data.objects['UniformScale']
        root.location = (-.438, -4.262, -10.04)
        if rotated:
            root.rotation_euler = (.25, .45, .2)
            root.scale = (1e-4, 2e-4, .5e-4)
        bpy.context.view_layer.update()
        expected = root.rotation_euler.to_matrix() @ Vector((0, 0, 1))
        pixels, _ = render_mask(scene, args.out / ('normal_%s.exr' % rotated))
        normals = pixels[16:-16, 16:-16] * 2 - 1
        error = np.linalg.norm(normals - np.asarray(expected), axis=2)
        bad = float(np.mean(error > .01))
        report['cases'].append({'case': 'geometric_normal', 'rotated_nonuniform': rotated,
                                'mean_error': float(error.mean()), 'max_error': float(error.max()),
                                'fraction_error_gt_0_01': bad})
        if bad > .001:
            report['failures'].append('Translated tiny mesh geometric normal: %s' % rotated)

    for mode in ['TEMPORAL', 'SOFT_FILTERED']:
        for light_kind in ['SUN', 'POINT']:
            # Local-light shadow projections retain a minimum physical emitter/clip size.
            # Keep this compatibility case outside that limit; the SUN case exercises the
            # submillimetre precision regression independently of local-light clipping.
            for scale in ([1., 1e-4] if light_kind == 'SUN' else [1., .01]):
                scene = fixture(args.size, scale=scale, mode=mode, kind=light_kind, checker=True)
                if light_kind == 'POINT':
                    bpy.data.lights['TestLight0'].shadow_buffer_clip_start = max(scale * .001, 1e-6)
                reference, _ = render_mask(scene, args.out / ('%s_%s_%g_origin.exr' % (mode, light_kind, scale)))
                if reference[16:-16, 16:-16, 0].min() > .1 or reference[16:-16, 16:-16, 0].max() < .9:
                    report['failures'].append('Origin lost lit/occluded coverage %s %s %g' % (mode, light_kind, scale))
                root = bpy.data.objects['UniformScale']
                root.location = (-.438, -4.262, -10.04)
                bpy.context.view_layer.update()
                translated, _ = render_mask(scene, args.out / ('%s_%s_%g_translated.exr' % (mode, light_kind, scale)))
                region = np.s_[16:-16, 16:-16, 0]
                mae = float(np.mean(np.abs(translated[region] - reference[region])))
                dark_reference = float(np.mean(reference[region] < .1))
                dark_translated = float(np.mean(translated[region] < .1))
                report['cases'].append({'case': 'translation_invariance', 'mode': mode,
                                        'light': light_kind, 'scale': scale, 'mae': mae,
                                        'dark_fraction_reference': dark_reference,
                                        'dark_fraction_translated': dark_translated})
                if mae > .025 or abs(dark_reference-dark_translated) > .025:
                    report['failures'].append('Translated shadow %s %s %g' % (mode, light_kind, scale))
                if scale < 1:
                    if translated[region].min() > .1 or translated[region].max() < .9:
                        report['failures'].append('Lost lit/occluded coverage %s %s' % (mode, light_kind))

    (args.out / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    if report['failures']:
        raise AssertionError(report['failures'])
    print('SHADOW_SMALL_SCALE_COMPLETE', flush=True)


if __name__ == '__main__':
    main()
