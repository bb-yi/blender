# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Actual capture context, planar readiness, shell and compositor LUT interaction pixels."""

import json
import os
from pathlib import Path
import sys

import bpy
import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import test_planar_probe as planar
import test_compositor_lut as lut_helpers


def capture_fixture(query='PLANAR_PROBE_CAPTURE', shell_route='DEFERRED'):
    scene, mat, reader, target, receiver, sources = planar.fixture(
        method='BLENDED' if shell_route == 'FORWARD' else 'DITHERED')
    for source in sources:
        tree = source.data.materials[0].node_tree
        context = tree.nodes.new('ShaderNodeDrawInfo')
        tree.links.new(context.outputs['Is ' + query.replace('_', ' ').title()], tree.nodes['Emission'].inputs['Color'])
    tree = mat.node_tree
    context = tree.nodes.new('ShaderNodeDrawInfo')
    channels = tree.nodes.new('ShaderNodeCombineXYZ')
    tree.links.new(reader.outputs['Color'], channels.inputs['X'])
    tree.links.new(context.outputs['Is ' + query.replace('_', ' ').title()], channels.inputs['Y'])
    tree.links.new(reader.outputs['Is Valid'], channels.inputs['Z'])
    planar.wire_output(mat, channels.outputs[0])
    shell = tree.nodes.new('ShaderNodeOutputOutlineShell')
    shell.inputs['Strength'].default_value = .1
    shell.inputs['Alpha'].default_value = 1
    # Exercise planar samplers, the context uniform, and the per-object shell depth together.
    tree.links.new(channels.outputs[0], shell.inputs['Color'])
    mat.use_outline_shell_self_occlusion = True
    mat.outline_shell_render_method = shell_route
    return scene, mat, reader, target, context


def main():
    for route in ('DEFERRED', 'FORWARD'):
        scene, mat, reader, target, context = capture_fixture(shell_route=route)
        planar.near(planar.sample(planar.render(route + '_capture_context')), (1, 0, 1))
        target.hide_render = True
        planar.near(planar.sample(planar.render(route + '_capture_removed')), (0, 0, 0))
        target.hide_render = False
        planar.near(planar.sample(planar.render(route + '_capture_restored')), (1, 0, 1))
    capture_fixture('IMAGE_RENDER')
    planar.near(planar.sample(planar.render('host_task_context')), (1, 1, 1))

    scene, mat, reader, target, context = capture_fixture()
    scene.use_nodes = True
    scene.render.use_compositing = True
    scene.render.compositor_precision = 'FULL'
    tree = bpy.data.node_groups.new('Four Feature Integration', 'CompositorNodeTree')
    scene.compositing_node_group = tree
    layers = tree.nodes.new('CompositorNodeRLayers')
    grade = tree.nodes.new('CompositorNodeLUT')
    grade.preset = 'RAW'
    path = planar.OUT / 'invert.cube'
    with path.open('w', encoding='utf-8') as stream:
        stream.write('LUT_3D_SIZE 2\n')
        for blue in (0, 1):
            for green in (0, 1):
                for red in (0, 1):
                    stream.write(f'{1-red} {1-green} {1-blue}\n')
    grade.filepath = str(path)
    output = tree.nodes.new('CompositorNodeOutputFile')
    output.directory = str(planar.OUT)
    output.format.media_type = 'MULTI_LAYER_IMAGE'
    output.format.color_depth = '32'
    output.format.color_management = 'OVERRIDE'
    output.format.linear_colorspace_settings.name = 'Non-Color'
    output.file_output_items.clear()
    output.file_output_items.new('RGBA', 'Image')
    tree.links.new(layers.outputs['Image'], grade.inputs['Image'])
    tree.links.new(grade.outputs['Image'], output.inputs['Image'])
    for device in ('CPU', 'GPU'):
        scene.render.compositor_device = device
        output.file_name = 'combined_' + device
        bpy.ops.render.render()
        parts = lut_helpers.read_parts(planar.OUT / (output.file_name + '.exr'))
        assert len(parts) == 1, parts.keys()
        pixels = next(iter(parts.values()))
        assert np.isfinite(pixels).all()
        planar.near(planar.sample(pixels), (0, 1, 0))
    report = dict(planar.REPORT, passed=True, compositor_devices=['CPU', 'GPU'])
    (planar.OUT / 'summary_interop.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('DRAW_INFO_PLANAR_INTEROP_OK', flush=True)


if __name__ == '__main__':
    assert all(os.environ.get('BLENDER_USER_' + name)
               for name in ('RESOURCES', 'CONFIG', 'SCRIPTS', 'DATAFILES', 'EXTENSIONS'))
    main()
