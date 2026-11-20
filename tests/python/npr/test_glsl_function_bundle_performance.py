# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Paired struct/manual-Bundle compile and animated-value comparison; bounded edit churn."""

import ctypes
import gc
import json
import os
from pathlib import Path
import statistics
import sys
import time

import bpy
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_glsl_function_bundle import OUT, combine, function, refresh, scene
from material_compat_test_utils import center, render


def memory():
    if os.name != 'nt':
        return 0
    from ctypes import wintypes
    class Counters(ctypes.Structure):
        _fields_ = [('cb', wintypes.DWORD), ('PageFaultCount', wintypes.DWORD)] + [
            (name, ctypes.c_size_t) for name in ('PeakWorkingSetSize', 'WorkingSetSize',
            'QuotaPeakPagedPoolUsage', 'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage',
            'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage', 'PrivateUsage')]
    value = Counters()
    value.cb = ctypes.sizeof(value)
    kernel = ctypes.WinDLL('kernel32')
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    psapi = ctypes.WinDLL('psapi')
    psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD]
    assert psapi.GetProcessMemoryInfo(kernel.GetCurrentProcess(), ctypes.byref(value), value.cb)
    return value.PrivateUsage


FIELDS = [f'a{i}' for i in range(8)]
STRUCT_SOURCE = 'struct Data {' + ''.join('float ' + name + ';' for name in FIELDS) + '''
vec3 normal; vec3 color; float alpha; };
vec3 benchmark(Data data) {
float sum = (''' + '+'.join('data.' + name for name in FIELDS) + ''') * 0.015625;
return vec3(data.color.r+sum, data.normal.y+sum, data.alpha+sum);
}
'''
MANUAL_SOURCE = 'vec3 benchmark(' + ','.join('float ' + name for name in FIELDS) + ''',
vec3 normal, vec3 color, float alpha) {
float sum = (''' + '+'.join(FIELDS) + ''') * 0.015625;
return vec3(color.r+sum, normal.y+sum, alpha+sum);
}
'''


def make_material(kind):
    mat = bpy.data.materials.new('BundleBenchmark_' + kind)
    mat.use_nodes = True
    tree = mat.node_tree
    tree.nodes.clear()
    output = tree.nodes.new('ShaderNodeOutputMaterial')
    emission = tree.nodes.new('ShaderNodeEmission')
    tree.links.new(emission.outputs[0], output.inputs['Surface'])
    packet = combine(tree, [(name, 'FLOAT', (i+1)/128) for i,name in enumerate(FIELDS)] + [
        ('normal', 'VEC3', (0.125,0.25,0.375)), ('color','VEC3',(0.125,0.25,0.5)),
        ('alpha','FLOAT',0.5)])
    text_source = STRUCT_SOURCE if kind == 'struct' else MANUAL_SOURCE
    node = function(tree, text_source, 'benchmark')
    tree.links.new(node.outputs[0], emission.inputs['Color'])
    if kind == 'struct':
        tree.links.new(packet.outputs[0], node.inputs[0])
    else:
        separate = tree.nodes.new('NodeSeparateBundle')
        for name in FIELDS:
            separate.bundle_items.new('FLOAT', name)
        separate.bundle_items.new('VECTOR', 'normal')
        separate.bundle_items.new('VECTOR', 'color')
        separate.bundle_items.new('FLOAT', 'alpha')
        tree.links.new(packet.outputs[0], separate.inputs[0])
        for name in FIELDS + ['normal', 'color', 'alpha']:
            tree.links.new(separate.outputs[name], node.inputs[name])
    packet.inputs['a0'].keyframe_insert('default_value', frame=1)
    packet.inputs['a0'].default_value = 0.25
    packet.inputs['a0'].keyframe_insert('default_value', frame=64)
    refresh(tree)
    return mat, node, text_source


sc, _, _ = scene()
plane = bpy.context.object
materials = {kind: make_material(kind) for kind in ('manual', 'struct')}
def select(kind):
    mat, node, source = materials[kind]
    plane.data.materials[0] = mat
    return mat, node, source

def frame(kind, number):
    select(kind)
    sc.frame_set(number)
    started = time.perf_counter()
    bpy.ops.render.render(write_still=False)
    return time.perf_counter() - started

report = {'animation': [], 'compile': [], 'memory': {}, 'memory_warmup_edits': 64}
sc.frame_set(1)
select('manual')
reference = center(render('benchmark_manual_reference'))
select('struct')
actual = center(render('benchmark_struct_reference'))
assert np.max(np.abs(reference-actual)) <= 1e-5, (reference, actual)
report['pixel_max_error'] = float(np.max(np.abs(reference-actual)))
for repeat in range(5):
    row = {}
    order = ('manual','struct') if repeat % 2 == 0 else ('struct','manual')
    for kind in order:
        row[kind] = statistics.median(frame(kind, n) for n in range(2 + repeat*8, 10 + repeat*8))
    report['animation'].append(row)
    print('BUNDLE_ANIMATION_SAMPLE', row, flush=True)

for repeat in range(5):
    row = {}
    order = ('struct','manual') if repeat % 2 == 0 else ('manual','struct')
    for kind in order:
        mat, node, source = select(kind)
        node.script.clear()
        node.script.write(source + '\n// compile variant ' + str(repeat))
        node.function_name = ''
        node.function_name = 'benchmark'
        refresh(mat.node_tree)
        row[kind] = frame(kind, 48)
    report['compile'].append(row)
    print('BUNDLE_COMPILE_SAMPLE', row, flush=True)

for kind in ('manual', 'struct'):
    mat, node, source = select(kind)
    report['memory'][kind] = []
    for repeat in range(128):
        node.script.clear()
        node.script.write(source + '\n// bounded variant ' + str(repeat % 8))
        node.function_name = ''
        node.function_name = 'benchmark'
        refresh(mat.node_tree)
        frame(kind, 49)
        if repeat in (31,63,95,127):
            gc.collect()
            report['memory'][kind].append({'iteration':repeat, 'private_bytes':memory()})
for metric in ('animation','compile'):
    baseline = statistics.median(row['manual'] for row in report[metric])
    candidate = statistics.median(row['struct'] for row in report[metric])
    report[metric + '_ratio'] = candidate / baseline
(OUT / 'bundle_performance.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print('BUNDLE_PERFORMANCE', json.dumps(report), flush=True)
assert report['animation_ratio'] <= 1.10, report['animation_ratio']
assert report['compile_ratio'] <= 1.10, report['compile_ratio']
for kind, samples in report['memory'].items():
    if samples[1]['private_bytes']:
        growth = samples[-1]['private_bytes'] - samples[1]['private_bytes']
        assert growth <= max(32*1024*1024, samples[1]['private_bytes']*0.1), (kind, growth)
print('GLSL_FUNCTION_BUNDLE_PERFORMANCE_OK', flush=True)
