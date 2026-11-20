# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise same-frame shadow reconstruction, fallback inputs and light-budget overflow."""
import argparse
import json
import math
from pathlib import Path
import sys

import bpy
import gpu
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_shadow_precision_penumbra import fixture, render_mask, disk_visibility


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--reference', type=Path)
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])
    args.out.mkdir(parents=True, exist_ok=True)
    gpu.init()
    report = {'backend': gpu.platform.backend_type_get(), 'cases': [], 'failures': []}

    def render(scene, label):
        pixels, _ = render_mask(scene, args.out / (label + '.exr'))
        assert np.isfinite(pixels).all()
        return pixels

    def reference(label):
        image = bpy.data.images.load(str(args.reference / (label + '.exr')), check_existing=False)
        pixels = np.asarray(image.pixels[:], dtype=np.float32).reshape(image.size[1], image.size[0], 4)
        bpy.data.images.remove(image)
        return pixels[:, :, :3]

    size = 320
    for kind in ('SUN', 'POINT'):
        for near in (.001, .01, .1):
            scene = fixture(size, mode='SOFT_FILTERED', kind=kind,
                            radius=.25 if kind == 'SUN' else .6)
            scene.camera.data.clip_start = near
            bpy.data.objects['Receiver'].rotation_euler.y = .45
            bpy.context.view_layer.update()
            label = '%s_near_%g' % (kind, near)
            pixels = render(scene, label)
            rx = ((np.arange(size) + .5) / size - .5) * .72
            distance = 6 / (1 - math.tan(.45) * rx)
            x, z = distance * rx, 6 - distance
            radius = math.tan(.25 / 2) if kind == 'SUN' else .6 / 3
            expected = np.array([disk_visibility(value, radius * (1 - height))
                                 for value, height in zip(x, z)])
            measured = pixels[size // 2 - 4:size // 2 + 4, :, 0].mean(axis=0)
            region = np.abs(x) < radius * 1.5
            error = float(np.abs(measured[region] - expected[region]).mean())
            report['cases'].append({'case': label, 'reference_mae': error})
            if error > .04:
                report['failures'].append(label + ': receiver-plane reconstruction')

    # A default node and an offset query in one material must never share the same answer.
    scene = fixture(size, mode='SOFT_FILTERED', kind='POINT', radius=.6, classify=False)
    mat = bpy.data.objects['Receiver'].data.materials[0]
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    info = next(node for node in nodes if node.bl_idname == 'ShaderNodeShaderInfo')
    combine = next(node for node in nodes if node.bl_idname == 'ShaderNodeCombineXYZ')
    custom = nodes.new('ShaderNodeShaderInfo')
    custom.shadow_mode = 'SOFT_FILTERED'
    custom.stable_shadow_samples = 8
    geometry = nodes.new('ShaderNodeNewGeometry')
    offset = nodes.new('ShaderNodeVectorMath')
    offset.operation = 'ADD'
    offset.inputs[1].default_value = (.15, 0, 0)
    links.new(geometry.outputs['Position'], offset.inputs[0])
    links.new(offset.outputs['Vector'], custom.inputs['World Position'])
    links.new(custom.outputs['Shadow'], combine.inputs[1])
    higher = nodes.new('ShaderNodeShaderInfo')
    higher.shadow_mode = 'SOFT_FILTERED'
    higher.stable_shadow_samples = 32
    links.new(higher.outputs['Shadow'], combine.inputs[2])
    pixels = render(scene, 'mixed_queries')
    if args.reference:
        expected = reference('mixed_queries')
        error = float(np.abs(pixels[:, :, 1] - expected[:, :, 1]).max())
        quality_error = float(np.abs(pixels[:, :, 2] - expected[:, :, 2]).max())
        default_change = float(np.abs(pixels[:, :, 0] - expected[:, :, 0]).max())
        report['cases'].append({'case': 'custom_fallback', 'max_error': error,
                                'mixed_quality_32_max_error': quality_error,
                                'default_cache_change': default_change})
        if error > .001 or quality_error > .001 or default_change < .001:
            report['failures'].append('Cache not used or custom query changed')

    scene = fixture(size, mode='SOFT_FILTERED', kind='POINT', radius=.6, quality=32)
    pixels = render(scene, 'quality_32')
    if args.reference:
        error = float(np.abs(pixels - reference('quality_32')).max())
        measured = pixels[size // 2 - 8:size // 2 + 8, :, 0].mean(axis=0)
        x = ((np.arange(size) + .5) / size - .5) * 4.32
        expected = disk_visibility(x, .6 / 3)
        region = np.abs(x) < .3
        accuracy = float(np.abs(measured[region] - expected[region]).mean())
        report['cases'].append({'case': 'quality_32', 'change_vs_direct': error,
                                'reference_mae': accuracy})
        if accuracy > .02:
            report['failures'].append('Quality 32 penumbra inaccurate')

    for mode in ('custom_normal', 'terminator_offset', 'transparent', 'orthographic'):
        scene = fixture(192, mode='SOFT_FILTERED', kind='POINT', radius=.6)
        receiver = bpy.data.objects['Receiver']
        mat = receiver.data.materials[0]
        nodes, links = mat.node_tree.nodes, mat.node_tree.links
        info = next(node for node in nodes if node.bl_idname == 'ShaderNodeShaderInfo')
        if mode == 'custom_normal':
            normal = nodes.new('ShaderNodeCombineXYZ')
            normal.inputs[0].default_value = .2
            normal.inputs[2].default_value = 1
            links.new(normal.outputs[0], info.inputs['Normal'])
        elif mode == 'terminator_offset':
            receiver.shadow_terminator_normal_offset = .1
            receiver.shadow_terminator_geometry_offset = .2
        elif mode == 'transparent':
            output = next(node for node in nodes if node.bl_idname == 'ShaderNodeOutputMaterial')
            emission = next(node for node in nodes if node.bl_idname == 'ShaderNodeEmission')
            transparent = nodes.new('ShaderNodeBsdfTransparent')
            mix = nodes.new('ShaderNodeMixShader')
            mix.inputs[0].default_value = .5
            links.new(emission.outputs[0], mix.inputs[1])
            links.new(transparent.outputs[0], mix.inputs[2])
            links.new(mix.outputs[0], output.inputs['Surface'])
            mat.surface_render_method = 'BLENDED'
        else:
            scene.camera.data.type = 'ORTHO'
            scene.camera.data.ortho_scale = 4.32
        bpy.context.view_layer.update()
        pixels = render(scene, mode)
        if args.reference:
            error = float(np.abs(pixels - reference(mode)).max())
            report['cases'].append({'case': mode, 'max_change': error})
            if mode != 'orthographic' and error > .001:
                report['failures'].append(mode + ': fallback semantics changed')
            if mode == 'orthographic':
                x = ((np.arange(192) + .5) / 192 - .5) * 4.32
                expected = disk_visibility(x, .6 / 3)
                measured = pixels[92:100, :, 0].mean(axis=0)
                region = np.abs(x) < .3
                accuracy = float(np.abs(measured[region] - expected[region]).mean())
                report['cases'][-1]['reference_mae'] = accuracy
                if accuracy > .03:
                    report['failures'].append('Orthographic reconstruction inaccurate')

    for count in (1, 8, 16):
        scene = fixture(192, mode='SOFT_FILTERED', kind='POINT', radius=.6, light_count=count)
        pixels = render(scene, 'lights_%d' % count)
        values = pixels[8:-8, 8:-8]
        classification_error = float(np.abs(values[:, :, 0] - values[:, :, 2]).mean())
        report['cases'].append({'case': 'lights_%d' % count,
                                'classification_mae': classification_error,
                                'minimum': float(values[:, :, 0].min()),
                                'maximum': float(values[:, :, 0].max())})
        if values[:, :, 0].min() > .05 or values[:, :, 0].max() < .95 or classification_error > .005:
            report['failures'].append('Light-budget overflow lost shadowing or classification')

    scene = fixture(192, mode='SOFT_FILTERED', kind='POINT', radius=.6, light_count=8)
    for light in list(bpy.data.lights)[1:]:
        light.use_shadow = False
    pixels = render(scene, 'unshadowed_lights')
    profile = pixels[92:100, :, 0].mean(axis=0)
    dark = float(profile[-24:-8].mean())
    report['cases'].append({'case': 'unshadowed_lights', 'dark_visibility': dark})
    if abs(dark - 7 / 8) > .01:
        report['failures'].append('Unshadowed lights lost their weight')

    (args.out / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('RECONSTRUCTION_REPORT ' + json.dumps(report), flush=True)
    assert not report['failures'], report['failures']
    print('SHADOW_RECONSTRUCTION_COMPLETE', flush=True)


if __name__ == '__main__':
    main()
