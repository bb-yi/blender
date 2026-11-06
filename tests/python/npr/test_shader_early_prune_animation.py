# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Early pruning must preserve implicit emission and branches reactivated by animation."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import center, glsl, near, render, scene

s, material, emission = scene()
tree = material.node_tree
output = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeOutputMaterial')
tree.nodes.remove(emission)
mix = tree.nodes.new('ShaderNodeMix')
mix.data_type = 'RGBA'
mix.blend_type = 'MIX'
mix.inputs[6].default_value = (0.2, 0.4, 0.6, 1)
branch = glsl(tree, 'vec4 branch_color() { return vec4(0.8, 0.1, 0.3, 1.0); }', 'branch_color')
tree.links.new(branch.outputs[0], mix.inputs[7])
# Color-to-shader conversion must still generate the same implicit emission.
tree.links.new(mix.outputs[2], output.inputs['Surface'])
for frame, factor in [(1, 0.0), (2, 0.25), (3, 1.0), (4, 0.0)]:
    mix.inputs[0].default_value = factor
    mix.inputs[0].keyframe_insert('default_value', frame=frame)

for frame, expected in [(1, (0.2, 0.4, 0.6)), (2, (0.35, 0.325, 0.525)),
                        (3, (0.8, 0.1, 0.3)), (4, (0.2, 0.4, 0.6))]:
    s.frame_set(frame)
    near(center(render('early_prune_frame_%d' % frame)), expected)

# Editing a previously pruned branch must become visible when enabled again.
branch.script.clear()
branch.script.write('vec4 branch_color() { return vec4(0.1, 0.7, 0.2, 1.0); }')
tree.update_tag()
s.frame_set(3)
near(center(render('early_prune_source_edit')), (0.1, 0.7, 0.2))

# Scalar Mix may prune A at one, but must retain extrapolation when unclamped.
s, material, emission = scene()
tree = material.node_tree
mix = tree.nodes.new('ShaderNodeMix')
mix.data_type = 'FLOAT'
mix.inputs[2].default_value = 0.2
branch = glsl(tree, 'float scalar_branch() { return 0.8; }', 'scalar_branch')
tree.links.new(branch.outputs[0], mix.inputs[3])
tree.links.new(mix.outputs[0], emission.inputs['Color'])
for clamp, factor, expected in [(True, -1, 0.2), (True, 2, 0.8), (False, 0, 0.2),
                                (False, 1, 0.8), (False, 1.5, 1.1), (False, 0.5, 0.5)]:
    mix.clamp_factor = clamp
    mix.inputs[0].default_value = factor
    near(center(render('scalar_%s_%s' % (clamp, factor))), (expected,)*3)

# A linked per-pixel factor must not be treated as a compile-time endpoint.
factor_node = glsl(tree, 'float dynamic_factor() { return 0.25; }', 'dynamic_factor')
tree.links.new(factor_node.outputs[0], mix.inputs[0])
near(center(render('dynamic_factor')), (0.35,)*3)

# Ordinary surface preprocessing is shared by passes, never across changed evaluated values.
import bpy
s, material, emission = scene()
tree = material.node_tree
group = bpy.data.node_groups.new('Animated cached surface', 'ShaderNodeTree')
group.interface.new_socket(name='Value', in_out='INPUT', socket_type='NodeSocketFloat')
group.interface.new_socket(name='Value', in_out='OUTPUT', socket_type='NodeSocketFloat')
inp = group.nodes.new('NodeGroupInput')
out = group.nodes.new('NodeGroupOutput')
multiply = group.nodes.new('ShaderNodeMath')
multiply.operation = 'MULTIPLY'
multiply.inputs[1].default_value = 1
group.links.new(inp.outputs[0], multiply.inputs[0])
group.links.new(multiply.outputs[0], out.inputs[0])
instance = tree.nodes.new('ShaderNodeGroup')
instance.node_tree = group
tree.links.new(instance.outputs[0], emission.inputs['Color'])
for frame, value in [(1, 0.2), (2, 0.8)]:
    instance.inputs[0].default_value = value
    instance.inputs[0].keyframe_insert('default_value', frame=frame)
for frame, expected in [(1, 0.2), (2, 0.8), (1, 0.2)]:
    s.frame_set(frame)
    near(center(render('cached_group_%d' % frame)), (expected,)*3)
multiply.inputs[1].default_value = 2
near(center(render('cached_group_edit')), (0.4,)*3)
print('SHADER_EARLY_PRUNE_ANIMATION_OK')
