# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import sys
import os
import json
import time
import subprocess
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import scene, near, OUT
from filter_graph_test_utils import attach_filter_material
import bpy
import gpu
import numpy as np
s, m, e = scene()
fm = bpy.data.materials.new('ScaleDrag')
fm.eevee_domain = 'FILTER'
fm.node_tree.nodes.clear()
inp = fm.node_tree.nodes.new('ShaderNodeFilterGraphInput')
out = fm.node_tree.nodes.new('ShaderNodeOutputFilter')
out.inputs['Color'].default_value = (.2, .4, .6, 1)
out.inputs['Alpha'].default_value = 1
graph, p, stage = attach_filter_material(fm, scene_socket='Color Image')
s.eevee.taa_samples = 1
s.eevee.use_taa_reprojection = False
gpu.init()
area = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
region = next(r for r in area.regions if r.type == 'WINDOW')
space = area.spaces.active
space.shading.type = 'RENDERED'
space.overlay.show_overlays = False
space.shading.use_scene_world_render = True
space.shading.use_scene_lights_render = True
width, height = 512, 384
viewport = gpu.types.GPUOffScreen(width, height, format='RGBA16F')


def draw(scale):
    p.resolution_scale = scale
    bpy.context.view_layer.update()
    deps = bpy.context.evaluated_depsgraph_get()
    projection = s.camera.calc_matrix_camera(deps, x=width, y=height)
    begin = time.perf_counter()
    viewport.draw_view3d(
        s,
        bpy.context.view_layer,
        space,
        region,
        s.camera.matrix_world.inverted(),
        projection,
        do_color_management=False,
        draw_background=True)
    pixels = np.array(viewport.texture_color.read(), dtype=np.float32).reshape(height, width, 4)
    elapsed = (time.perf_counter() - begin) * 1000
    near(pixels[height // 2, width // 2, :3], [.2, .4, .6], .015)
    return elapsed


def memory():
    command = f"$r=@(Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory | Where-Object Name -Like 'pid_{
        os.getpid()}_*'); [long](($r | Measure-Object DedicatedUsage -Sum).Sum)"
    return int(subprocess.check_output(['pwsh', '-NoProfile', '-Command', command], text=True).strip())


try:
    for _ in range(8):
        draw(1)
    samples = [memory()]
    timings = []
    for batch in range(3):
        for i in range(100):
            # Unique integer-width render targets, not repeatedly revisiting a fixed preset set.
            scale = (180 + batch * 100 + i) / 512
            timings.append(draw(scale))
        samples.append(memory())
        print('DRAG_MEMORY', batch, samples[-1], flush=True)
    for _ in range(8):
        draw(1)
    samples.append(memory())
    result = {'backend': gpu.platform.backend_type_get(), 'pid': os.getpid(), 'dedicated_bytes': samples,
              'growth_bytes': max(samples[1:]) - samples[0],
              'first_100_ms_median': float(np.median(timings[:100])),
              'last_100_ms_median': float(np.median(timings[-100:])), 'draws': 316}
    dest = OUT
    dest.mkdir(parents=True, exist_ok=True)
    name = 'baseline' if '--baseline' in sys.argv else result['backend'].lower()
    (dest / (name + '.json')).write_text(json.dumps(result, indent=2), encoding='utf-8')
    print('DRAG_RESULT', json.dumps(result), flush=True)
    assert result['growth_bytes'] < 128 * 1024**2, result
    print('FILTER_SCALE_MEMORY_PASS', flush=True)
finally:
    viewport.free()
