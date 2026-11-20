# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Persistent viewport pixels must follow Actions, edits and unsupported drivers."""
import sys
from pathlib import Path
import bpy
import gpu
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import scene, near

s, material, emission = scene()
s.eevee.taa_samples = 1
s.eevee.use_taa_reprojection = False
s.eevee.use_outline = False
s.eevee.use_shadows = False
group = bpy.data.node_groups.new('Animated group', 'ShaderNodeTree')
group.interface.new_socket(name='Value', in_out='OUTPUT', socket_type='NodeSocketFloat')
out = group.nodes.new('NodeGroupOutput')
value = group.nodes.new('ShaderNodeValue')
scale = group.nodes.new('ShaderNodeMath')
scale.operation = 'MULTIPLY'
scale.inputs[1].default_value = 1
group.links.new(value.outputs[0], scale.inputs[0])
group.links.new(scale.outputs[0], out.inputs[0])
inst = material.node_tree.nodes.new('ShaderNodeGroup')
inst.node_tree = group
material.node_tree.links.new(inst.outputs[0], emission.inputs['Color'])
for frame, v in [(1, .2), (2, .8), (3, .8), (4, .4)]:
    value.outputs[0].default_value = v
    value.outputs[0].keyframe_insert('default_value', frame=frame)

gpu.init()
area = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
region = next(r for r in area.regions if r.type == 'WINDOW')
space = area.spaces.active
space.shading.type = 'RENDERED'
space.overlay.show_overlays = False
buffer = gpu.types.GPUOffScreen(64, 64, format='RGBA32F')

def draw(expected):
    bpy.context.view_layer.update()
    deps = bpy.context.evaluated_depsgraph_get()
    assert deps.mode == 'VIEWPORT', deps.mode
    projection = s.camera.calc_matrix_camera(deps, x=64, y=64)
    buffer.draw_view3d(s, bpy.context.view_layer, space, region,
                      s.camera.matrix_world.inverted(), projection,
                      do_color_management=False, draw_background=True)
    pixels = np.array(buffer.texture_color.read(), dtype=np.float32).reshape(64, 64, 4)
    assert np.isfinite(pixels).all()
    near(pixels[32, 32, :3], (expected,)*3)

try:
    for frame, expected in [(1,.2),(2,.8),(3,.8),(4,.4),(8,.4),(9,.4),(2,.8),(3,.8)]:
        s.frame_set(frame)
        draw(expected)
    # Same animated value, but a user edited a different socket in the shared group.
    scale.inputs[1].default_value = .5
    draw(.4)
    s.frame_set(2)
    draw(.4)
    # A topology edit must invalidate even when every animated value is unchanged.
    other = group.nodes.new('ShaderNodeValue')
    other.outputs[0].default_value = .6
    group.links.new(other.outputs[0], out.inputs[0])
    draw(.6)
    group.links.new(scale.outputs[0], out.inputs[0])
    draw(.4)
    s.frame_set(1)
    draw(.1)
    # Drivers intentionally keep normal invalidation.
    group.animation_data.action = None
    scale.inputs[1].default_value = 1
    driver = value.outputs[0].driver_add('default_value').driver
    driver.expression = 'frame / 10.0'
    for frame in [1,2,3]:
        s.frame_set(frame)
        draw(frame/10)
finally:
    buffer.free()
print('Backend:', gpu.platform.backend_type_get())
print('MATERIAL_ANIMATION_NOOP_OK')
