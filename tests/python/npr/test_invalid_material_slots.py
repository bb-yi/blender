# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Sparse-slot eligibility and Blender's invalid material-index normalization.

The high invalid face index must retain Blender's last-slot clamping without taking the sparse
fast path. An evaluated Geometry Nodes mesh may legitimately add slots beyond the source object.
"""

import json
import os
from pathlib import Path
import re
import sys

import bpy
import gpu
import numpy as np

OUT = Path(os.environ['BLENDER_TEST_OUTPUT_DIR'])
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import scene

scene, red, emission = scene()
obj = bpy.context.object
mesh = obj.data
scene.eevee.taa_samples = 1
scene.eevee.use_taa_reprojection = False
scene.eevee.use_outline = False
scene.eevee.use_shadows = False
scene.eevee.use_performance_profiler = True
scene.eevee.performance_profiler_average_window = 1
emission.inputs['Color'].default_value = (.8, .1, .2, 1)
blue = red.copy()
blue.name = 'Last actual slot'
blue.node_tree.nodes.get(emission.name).inputs['Color'].default_value = (.15, .35, .75, 1)
green = red.copy()
green.name = 'Object override'
green.node_tree.nodes.get(emission.name).inputs['Color'].default_value = (.2, .8, .1, 1)
mesh.materials.append(blue)

gpu.init()
area = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
region = next(r for r in area.regions if r.type == 'WINDOW')
space = area.spaces.active
space.shading.type = 'RENDERED'
space.overlay.show_overlays = False
buffer = gpu.types.GPUOffScreen(64, 48, format='RGBA32F')
report = {'backend':gpu.platform.backend_type_get(), 'binary':bpy.app.binary_path,
          'rows':[], 'errors':[], 'complete':False}
images = {}


def index(value):
    for face in mesh.polygons:
        face.material_index = value
    mesh.update()


def draw(label):
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    projection = scene.camera.calc_matrix_camera(depsgraph, x=64, y=48)
    for _ in range(2):
        buffer.draw_view3d(scene, bpy.context.view_layer, space, region,
                           scene.camera.matrix_world.inverted(), projection,
                           do_color_management=False, draw_background=True)
    pixels = np.array(buffer.texture_color.read(), dtype=np.float32).reshape(48, 64, 4)
    assert np.isfinite(pixels).all(), label
    profiler = scene.eevee.performance_profiler_viewport_report
    line = re.search(r'^\s*Sync Analysis:([^\n]+)', profiler, re.MULTILINE)
    assert line, profiler
    counts = {name:int(value) for name, value in re.findall(r'(\w+)=(\d+)', line.group(1))}
    evaluated = obj.evaluated_get(depsgraph)
    row = {'label':label, 'counts':counts, 'source_slots':len(obj.material_slots),
           'evaluated_mesh_slots':len(evaluated.data.materials),
           'evaluated_indices':sorted({face.material_index for face in evaluated.data.polygons}),
           'center':pixels[24, 32].tolist()}
    report['rows'].append(row)
    images[label] = pixels
    return pixels, counts


def require(condition, message):
    if not condition:
        report['errors'].append(message)


def equal(a, b):
    return np.array_equal(a.view(np.uint32), b.view(np.uint32))


try:
    index(1)
    valid, _ = draw('valid_last_slot')
    index(199)
    invalid, counts = draw('invalid_high_index')
    require(counts['skipped_slots'] == 0, 'Invalid index 199 with two slots took sparse fast path')
    require(equal(invalid, valid), 'Invalid high index did not clamp to the last actual slot')

    # Write the raw face attribute: MeshPolygon.material_index's RNA setter clamps negatives.
    # Existing normalization maps negative values to slot zero; no extra face scan is required.
    index(0)
    zero, _ = draw('valid_zero_slot')
    raw_indices = mesh.attributes.get('material_index')
    if raw_indices is None:
        raw_indices = mesh.attributes.new(name='material_index', type='INT', domain='FACE')
    for item in raw_indices.data:
        item.value = -7
    mesh.update()
    require(all(item.value == -7 for item in raw_indices.data), 'Raw negative fixture was clamped early')
    negative, _ = draw('raw_negative_index')
    require(equal(negative, zero), 'Raw negative index did not normalize to slot zero')

    obj.material_slots[1].link = 'OBJECT'
    obj.material_slots[1].material = green
    index(1)
    valid_override, _ = draw('valid_last_slot_override')
    index(199)
    invalid_override, counts = draw('invalid_high_index_override')
    require(counts['skipped_slots'] == 0, 'Invalid index with object override took sparse fast path')
    require(equal(invalid_override, valid_override), 'Last-slot object override was not preserved')

    # A real sparse index remains eligible once the actual evaluated slots cover it.
    obj.material_slots[1].link = 'DATA'
    for _ in range(198):
        mesh.materials.append(blue)
    real_sparse, counts = draw('valid_sparse_200_slots')
    require(counts['skipped_slots'] == 199, 'Valid sparse slots unexpectedly fell back')
    require(equal(real_sparse, valid), 'Valid sparse slot changed pixels')

    mesh.materials.clear()
    mesh.materials.append(red)
    index(0)
    tree = bpy.data.node_groups.new('Evaluated material slot growth', 'GeometryNodeTree')
    tree.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
    tree.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
    source = tree.nodes.new('NodeGroupInput')
    assign = tree.nodes.new('GeometryNodeSetMaterial')
    assign.inputs['Material'].default_value = blue
    output = tree.nodes.new('NodeGroupOutput')
    tree.links.new(source.outputs['Geometry'], assign.inputs['Geometry'])
    tree.links.new(assign.outputs['Geometry'], output.inputs['Geometry'])
    modifier = obj.modifiers.new('GN adds material', 'NODES')
    modifier.node_group = tree
    gn, counts = draw('gn_evaluated_slots_grow')
    require(report['rows'][-1]['source_slots'] == 1, 'GN fixture source must have one slot')
    require(report['rows'][-1]['evaluated_mesh_slots'] == 2, 'GN did not append the second material')
    require(counts['skipped_slots'] == 1, 'GN-added evaluated slot was rejected as invalid')
    require(equal(gn, valid), 'GN evaluated material slot produced different pixels')
    report['complete'] = True
finally:
    buffer.free()
    np.savez_compressed(OUT / 'invalid_material_slots.npz', **images)
    (OUT / 'invalid_material_slots.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
assert not report['errors'], report['errors']
print('INVALID_MATERIAL_SLOTS_OK')
