# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Exercise real viewport snapshot buffers through growth, stable topology churn and shrink.

Run in an isolated Blender process with BLENDER_TEST_OUTPUT_DIR outside the source tree.
The 120-use shrink threshold is per physical history buffer, so this deliberately renders
more than 240 small frames. No user scene or preferences are loaded or saved.
"""

import json
import os
from pathlib import Path
import re

import bpy
import gpu
import numpy as np


out = Path(os.environ['BLENDER_TEST_OUTPUT_DIR'])
out.mkdir(parents=True, exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.engine = 'BLENDER_EEVEE'
scene.eevee.taa_samples = 1
scene.eevee.use_taa_reprojection = False
scene.eevee.use_shadows = False
scene.eevee.use_outline = False
scene.eevee.use_performance_profiler = True
scene.eevee.performance_profiler_average_window = 1
scene.world = bpy.data.worlds.new('Velocity capacity world')
scene.world.color = (0, 0, 0)
cam = bpy.data.objects.new('Camera', bpy.data.cameras.new('Camera'))
scene.collection.objects.link(cam)
cam.location = (0, 0, 5)
cam.data.type = 'ORTHO'
cam.data.ortho_scale = 3
scene.camera = cam

material = bpy.data.materials.new('Visible snapshot surface')
material.use_nodes = True
material.node_tree.nodes.clear()
surface = material.node_tree.nodes.new('ShaderNodeEmission')
surface.inputs['Color'].default_value = (0.2, 0.5, 0.8, 1)
output = material.node_tree.nodes.new('ShaderNodeOutputMaterial')
material.node_tree.links.new(surface.outputs[0], output.inputs['Surface'])

mesh = bpy.data.meshes.new('Grid input')
mesh.from_pydata([(-1,-1,0), (1,-1,0), (1,1,0), (-1,1,0)], [], [(0,1,2,3)])
mesh.materials.append(material)
obj = bpy.data.objects.new('Topology animation', mesh)
scene.collection.objects.link(obj)
tree = bpy.data.node_groups.new('Snapshot topology', 'GeometryNodeTree')
tree.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
grid = tree.nodes.new('GeometryNodeMeshGrid')
grid.inputs['Size X'].default_value = grid.inputs['Size Y'].default_value = 2
grid.inputs['Vertices Y'].default_value = 95
assign = tree.nodes.new('GeometryNodeSetMaterial')
assign.inputs['Material'].default_value = material
output = tree.nodes.new('NodeGroupOutput')
tree.links.new(grid.outputs['Mesh'], assign.inputs['Geometry'])
tree.links.new(assign.outputs['Geometry'], output.inputs['Geometry'])
modifier = obj.modifiers.new('Animated topology', 'NODES')
modifier.node_group = tree
for frame in range(1, 13):
    grid.inputs['Vertices X'].default_value = 95 + (frame & 1)
    grid.inputs['Vertices X'].keyframe_insert('default_value', frame=frame)
for frame, count in [(13, 4), (285, 4), (286, 96), (287, 95)]:
    grid.inputs['Vertices X'].default_value = count
    grid.inputs['Vertices X'].keyframe_insert('default_value', frame=frame)
    grid.inputs['Vertices Y'].default_value = 4 if frame < 286 else 95
    grid.inputs['Vertices Y'].keyframe_insert('default_value', frame=frame)
# The Y channel needs an initial large state as well.
grid.inputs['Vertices Y'].default_value = 95
grid.inputs['Vertices Y'].keyframe_insert('default_value', frame=1)
for layer in tree.animation_data.action.layers:
    for strip in layer.strips:
        for bag in strip.channelbags:
            for curve in bag.fcurves:
                for point in curve.keyframe_points:
                    point.interpolation = 'CONSTANT'

# Off-camera objects still need resource IDs, and therefore grow the indirection table.
extras = []
for index in range(300):
    extra = bpy.data.objects.new(f'Static {index:03d}', mesh)
    extra.location = (10 + index, 0, 0)
    scene.collection.objects.link(extra)
    extras.append(extra)

gpu.init()
area = next(area for area in bpy.context.screen.areas if area.type == 'VIEW_3D')
region = next(region for region in area.regions if region.type == 'WINDOW')
space = area.spaces.active
space.shading.type = 'RENDERED'
space.overlay.show_overlays = False
buffer = gpu.types.GPUOffScreen(64, 64, format='RGBA32F')
records = []
pixels = {}


def draw(frame):
    scene.frame_set(frame)
    bpy.context.view_layer.update()
    deps = bpy.context.evaluated_depsgraph_get()
    projection = cam.calc_matrix_camera(deps, x=64, y=64)
    buffer.draw_view3d(scene, bpy.context.view_layer, space, region,
                      cam.matrix_world.inverted(), projection,
                      do_color_management=False, draw_background=True)
    report = scene.eevee.performance_profiler_viewport_report
    match = re.search(r'Velocity Buffers:\s*([^\n]+)', report)
    assert match, 'Velocity runtime counters are missing from the profiler report'
    stats = {key: float(value) for key, value in
             re.findall(r'(\w+)=([0-9.]+)', match.group(1))}
    stats['frame'] = frame
    records.append(stats)
    if frame in (4, 12, 15, 284, 286, 287):
        image = np.asarray(buffer.texture_color.read(), dtype=np.float32).reshape(64, 64, 4)
        assert np.isfinite(image).all(), frame
        assert float(image[32,32,:3].max()) > 0.3, (frame, image[32,32])
        pixels[str(frame)] = image


try:
    for frame in range(1, 288):
        if frame == 13:
            for extra in extras:
                extra.hide_set(True)
        draw(frame)
finally:
    buffer.free()

steady = [row for row in records if 5 <= row['frame'] <= 12]
assert all(row['geometry_reallocations'] == 0 for row in steady), steady
assert sum(row['geometry_shrinks'] for row in records) >= 2, 'Both physical snapshots must shrink'
assert sum(row['indirection_shrinks'] for row in records) >= 1, 'Indirection never shrank'
assert all(row['geometry_shrinks'] == 0 for row in records if 13 <= row['frame'] <= 248), \
    'A physical buffer shrank before 120 low-use snapshots'
assert all(row['indirection_shrinks'] == 0 for row in records if 13 <= row['frame'] < 132), \
    'Indirection shrank before 120 low-use updates'
assert sum(row['geometry_reallocations'] for row in records if row['frame'] >= 286) >= 2, \
    'Both physical buffers must regrow after the small topology span'
assert np.array_equal(pixels['15'], pixels['284']), 'Shrinking changed the static image'
assert np.array_equal(pixels['4'], pixels['286']), 'Regrowing changed the image'
np.savez_compressed(out / 'velocity_capacity_pixels.npz', **pixels)
(out / 'velocity_capacity.json').write_text(json.dumps({
    'backend': gpu.platform.backend_type_get(), 'records': records,
}, indent=2), encoding='utf-8')
print('VELOCITY_CAPACITY_OK', gpu.platform.backend_type_get(), flush=True)
