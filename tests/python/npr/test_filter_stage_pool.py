# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Stable multi-stage Filter Graphs must not keep reallocating their GPU intermediates."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import OUT, background_child, near, scene
from filter_graph_test_utils import attach_filter_material
import bpy
import gpu
import numpy as np

if '--stage-child' not in sys.argv:
    output = background_child(Path(__file__).resolve(), ['--stage-child'],
                              ['--log', 'eevee.filter_graph', '--log-level', 'debug'])
    (OUT / 'stage_pool.log').write_text(output, encoding='utf-8')
    assert 'Texture allocation:' in output, 'Allocation logging was not enabled'
    steady = output.split('FILTER_STAGE_WARM', 1)[1].split('FILTER_STAGE_DISABLED', 1)[0]
    assert 'Texture allocation:' not in steady, steady
    assert output.count('Release inactive stage:') == 2, output
    print('FILTER_STAGE_POOL_PASS', flush=True)
else:
    scene, material, emission = scene()
    emission.inputs['Color'].default_value = (.2, .4, .6, 1)
    filter_material = bpy.data.materials.new('StagePool')
    filter_material.eevee_domain = 'FILTER'
    filter_material.node_tree.nodes.clear()
    filter_material.node_tree.nodes.new('ShaderNodeFilterGraphInput')
    output = filter_material.node_tree.nodes.new('ShaderNodeOutputFilter')
    output.inputs['Color'].default_value = (.2, .4, .6, 1)
    output.inputs['Alpha'].default_value = 1
    graph, first, _ = attach_filter_material(filter_material, stage='BEFORE_POSTFX',
                                             scene_socket='Color Image')
    _, second, _ = attach_filter_material(filter_material, graph=graph, stage='BEFORE_COMPOSITE',
                                          scene_socket='Color Image')
    first.resolution_basis = second.resolution_basis = 'STAGE'
    first.resolution_scale = .5
    second.resolution_scale = .25
    scene.eevee.taa_samples = 1
    scene.eevee.use_taa_reprojection = False
    gpu.init()
    area = next(area for area in bpy.context.screen.areas if area.type == 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    space = area.spaces.active
    space.shading.type = 'RENDERED'
    space.overlay.show_overlays = False
    space.shading.use_scene_world_render = space.shading.use_scene_lights_render = True
    viewport = gpu.types.GPUOffScreen(128, 96, format='RGBA16F')
    try:
        for frame in range(44):
            if frame == 40:
                print('FILTER_STAGE_DISABLED', flush=True)
                for node in list(graph.nodes):
                    if node.bl_idname == 'EeveeFilterGraphNodeStageOutput':
                        graph.nodes.remove(node)
            bpy.context.view_layer.update()
            depsgraph = bpy.context.evaluated_depsgraph_get()
            projection = scene.camera.calc_matrix_camera(depsgraph, x=128, y=96)
            viewport.draw_view3d(scene, bpy.context.view_layer, space, region,
                                 scene.camera.matrix_world.inverted(), projection,
                                 do_color_management=False, draw_background=True)
            pixels = np.array(viewport.texture_color.read()).reshape(96, 128, 4)
            near(pixels[48, 64, :3], [.2, .4, .6], .015)
            if frame == 7:
                print('FILTER_STAGE_WARM', flush=True)
        print('FILTER_STAGE_DONE', flush=True)
    finally:
        viewport.free()
