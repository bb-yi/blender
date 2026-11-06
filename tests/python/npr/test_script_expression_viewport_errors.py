# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Synchronous offscreen viewport diagnostics, pixels and successful-pass reuse.

The real deferred path is covered separately by test_script_expression_async_errors.py.
"""
import sys
import time
from pathlib import Path

import bpy
import gpu
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import scene

sc, material, emission = scene()
sc.eevee.taa_samples = 1
sc.eevee.use_taa_reprojection = False
sc.eevee.use_outline = False
sc.eevee.use_shadows = False
tree = material.node_tree
bad = tree.nodes.new('ShaderNodeScriptExpression')
bad.name = 'Edited expression'
bad.expression = 'missing_viewport_old'
healthy = tree.nodes.new('ShaderNodeScriptExpression')
healthy.expression = '0.2'
add = tree.nodes.new('ShaderNodeMath')
add.operation = 'ADD'
tree.links.new(bad.outputs[0], add.inputs[0])
tree.links.new(healthy.outputs[0], add.inputs[1])
tree.links.new(add.outputs[0], emission.inputs['Color'])

gpu.init()
area = next(area for area in bpy.context.screen.areas if area.type == 'VIEW_3D')
region = next(region for region in area.regions if region.type == 'WINDOW')
space = area.spaces.active
space.shading.type = 'RENDERED'
space.overlay.show_overlays = False
buffer = gpu.types.GPUOffScreen(64, 64, format='RGBA32F')


def draw():
    bpy.context.view_layer.update()
    deps = bpy.context.evaluated_depsgraph_get()
    projection = sc.camera.calc_matrix_camera(deps, x=64, y=64)
    buffer.draw_view3d(sc, bpy.context.view_layer, space, region,
                      sc.camera.matrix_world.inverted(), projection,
                      do_color_management=False, draw_background=True)
    pixels = np.array(buffer.texture_color.read(), dtype=np.float32).reshape(64, 64, 4)
    return pixels[32, 32, :3]


def settle(expected, symbol=None):
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        pixel = draw()
        assert healthy.error_message == '', healthy.error_message
        message = bad.error_message
        if np.max(np.abs(pixel - expected)) < .005 and (
                symbol in message if symbol else message == ''):
            return
        time.sleep(.01)
    raise AssertionError((expected, pixel, symbol, bad.error_message))


try:
    settle(np.array([1, 0, 1]), 'missing_viewport_old')
    # Offscreen drawing compiles synchronously; exercise edit recovery here.
    bad.expression = 'missing_viewport_in_flight'
    draw()
    bad.expression = '0.3'
    settle(np.array([.5] * 3))
    for _ in range(12):
        draw()
        assert bad.error_message == '', bad.error_message
    bad.expression = 'missing_viewport_new'
    settle(np.array([1, 0, 1]), 'missing_viewport_new')
    assert 'missing_viewport_old' not in bad.error_message, bad.error_message
    assert 'missing_viewport_in_flight' not in bad.error_message, bad.error_message
    bad.expression = '0.4'
    settle(np.array([.6] * 3))
    stamp = material.evaluated_get(bpy.context.evaluated_depsgraph_get()).shader_compile_timestamp
    for _ in range(12):
        draw()
        assert bad.error_message == '', bad.error_message
    after = material.evaluated_get(bpy.context.evaluated_depsgraph_get()).shader_compile_timestamp
    assert stamp == after and stamp > 0, (stamp, after)
finally:
    buffer.free()
print('SCRIPT_EXPRESSION_VIEWPORT_ERRORS_OK', flush=True)
