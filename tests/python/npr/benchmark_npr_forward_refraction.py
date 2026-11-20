# SPDX-License-Identifier: GPL-2.0-or-later
"""Three warmed rounds; wall time waits for render readback, not a GPU timer query."""
import json
import os
from pathlib import Path
import statistics
import sys
import time

import bpy
import gpu
import numpy as np

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).parent))
from test_npr_forward_refraction import setup_scene, forward_plane, glass

OUT = Path(os.environ['BLENDER_TEST_OUTPUT_DIR'])
OUT.mkdir(parents=True, exist_ok=True)
reports = []
gpu.init()
(OUT / 'hardware.json').write_text(json.dumps({'backend': gpu.platform.backend_type_get(),
    'renderer': gpu.platform.renderer_get(), 'vendor': gpu.platform.vendor_get(),
    'version': gpu.platform.version_get(), 'build': bpy.app.build_hash.decode()}, indent=2), encoding='utf-8')

def state(variant, **values):
    values.update(pid=os.getpid(), variant_id=variant)
    (OUT / 'benchmark_state.json').write_text(json.dumps(values), encoding='utf-8')

# Investigate threshold outliers with alternating pairs to remove run-order drift.
if os.environ.get('NPR_FORWARD_RECHECK'):
    for width, height, layers in json.loads(os.environ['NPR_FORWARD_RECHECK']):
        scene, _ = setup_scene()
        scene.render.resolution_x, scene.render.resolution_y = width, height
        scene.eevee.use_performance_profiler = False
        for index in range(layers):
            forward_plane('Idle background ' + str(index), (.8, .2, .1),
                          1 if index == 0 else .3, layer=index * 2,
                          z=-1.8 + index * .4, participate=False)
            glass('Idle glass ' + str(index), index * 2 + 1, z=-1.6 + index * .4)
        for mode in ('LEGACY', 'LAYERED'):
            scene.eevee.npr_forward_refraction_mode = mode
            for _ in range(6):
                bpy.ops.render.render()
        samples = []
        for round_id in range(3):
            for pair in range(7):
                modes = ('LEGACY', 'LAYERED') if (round_id + pair) % 2 else ('LAYERED', 'LEGACY')
                for mode in modes:
                    scene.eevee.npr_forward_refraction_mode = mode
                    start = time.perf_counter()
                    bpy.ops.render.render()
                    sample = dict(resolution=[width, height], layers=layers,
                                  round=round_id, pair=pair, mode=mode,
                                  render_readback_wall_ms=(time.perf_counter() - start) * 1000)
                    samples.append(sample)
                    print('NPR_IDLE_SAMPLE ' + json.dumps(sample), flush=True)
        reports.append({'resolution': [width, height], 'layers': layers, 'samples': samples})
        (OUT / 'inactive_recheck.json').write_text(json.dumps(reports, indent=2), encoding='utf-8')
    print('NPR_FORWARD_INACTIVE_RECHECK_OK', flush=True)
    raise SystemExit(0)

for width, height in ((1920, 1080), (3840, 2160)):
    for layers in (1, 2, 4):
        scene, _ = setup_scene()
        scene.render.resolution_x, scene.render.resolution_y = width, height
        scene.eevee.use_performance_profiler = False
        producers = []
        for index in range(layers):
            producers.append(forward_plane('Background ' + str(index), (.8, .2, .1),
                1 if index == 0 else .3, layer=index * 2, z=-1.8 + index * .4))
            glass('Glass ' + str(index), index * 2 + 1, z=-1.6 + index * .4)
        for mode, participate in (('LEGACY', True), ('LAYERED', False), ('LAYERED', True)):
            scene.eevee.npr_forward_refraction_mode = mode
            for obj in producers:
                obj.use_npr_refraction_background = participate
            state(len(reports), resolution=[width, height], layers=layers,
                  mode=mode, participate=participate, phase='warmup')
            for _ in range(3):
                bpy.ops.render.render()
            # Keep console report formatting outside measured render rounds.
            scene.eevee.use_performance_profiler = True
            bpy.ops.render.render()
            cpu_stages = scene.eevee.performance_profiler_render_report
            scene.eevee.use_performance_profiler = False
            for _ in range(2):
                bpy.ops.render.render()
            state(len(reports), resolution=[width, height], layers=layers,
                  mode=mode, participate=participate, phase='measure')
            rounds = []
            for round_id in range(3):
                samples = []
                for _ in range(7):
                    start = time.perf_counter()
                    bpy.ops.render.render()
                    samples.append((time.perf_counter() - start) * 1000)
                rounds.append(samples)
            all_samples = [value for values in rounds for value in values]
            report = {'backend': gpu.platform.backend_type_get(), 'resolution': [width, height],
                      'layers': layers, 'mode': mode, 'participate': participate,
                      'render_readback_wall_median_ms': statistics.median(all_samples),
                      'render_readback_wall_p95_ms': float(np.percentile(all_samples, 95)),
                      'rounds_ms': rounds,
                      'cpu_stages': cpu_stages, 'variant_id': len(reports)}
            reports.append(report)
            print('NPR_FORWARD_BENCHMARK ' + json.dumps({k: v for k, v in report.items()
                if k not in ('rounds_ms', 'cpu_stages')}), flush=True)
            (OUT / 'benchmark.json').write_text(json.dumps(reports, indent=2), encoding='utf-8')

# Large transparent-overdraw workloads, separately identified from scheduling-only costs.
for width, height in ((1920, 1080), (3840, 2160)):
    scene, _ = setup_scene()
    scene.render.resolution_x, scene.render.resolution_y = width, height
    for index in range(24):
        forward_plane('Particle layer ' + str(index), (.1, .2, .4), .05,
                      z=-1.8 + index * .02)
    glass()
    for mode in ('LEGACY', 'LAYERED'):
        scene.eevee.npr_forward_refraction_mode = mode
        state(len(reports), resolution=[width, height], layers=1, mode=mode,
              workload='24 transparent planes', phase='warmup')
        for _ in range(3): bpy.ops.render.render()
        state(len(reports), resolution=[width, height], layers=1, mode=mode,
              workload='24 transparent planes', phase='measure')
        rounds=[]
        for _ in range(3):
            samples=[]
            for _ in range(7):
                start=time.perf_counter()
                bpy.ops.render.render()
                samples.append((time.perf_counter()-start)*1000)
            rounds.append(samples)
        samples=[v for values in rounds for v in values]
        reports.append({'backend': gpu.platform.backend_type_get(), 'resolution': [width, height],
            'workload': '24 transparent planes', 'mode': mode, 'variant_id':len(reports),
            'render_readback_wall_median_ms': statistics.median(samples),
            'render_readback_wall_p95_ms': float(np.percentile(samples,95)), 'rounds_ms':rounds})
        print('NPR_FORWARD_BENCHMARK '+json.dumps({k:v for k,v in reports[-1].items() if k!='rounds_ms'}),flush=True)
(OUT / 'benchmark.json').write_text(json.dumps(reports, indent=2), encoding='utf-8')
print('NPR_FORWARD_BENCHMARK_OK', flush=True)
