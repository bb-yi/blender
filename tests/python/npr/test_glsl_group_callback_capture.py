# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import *

s, m, e = scene()
group = bpy.data.node_groups.new('CaptureGroup', 'ShaderNodeTree')
group.interface.new_socket(name='Gain', in_out='INPUT', socket_type='NodeSocketFloat').default_value = .1
group.interface.new_socket(name='Color', in_out='OUTPUT', socket_type='NodeSocketColor')
gi = group.nodes.new('NodeGroupInput')
go = group.nodes.new('NodeGroupOutput')
ci = group.nodes.new('NodeClosureInput')
co = group.nodes.new('NodeClosureOutput')
ci.pair_with_output(co)
co.input_items.new('FLOAT', 'value').structure_type = 'SINGLE'
co.output_items.new('FLOAT', 'Result').structure_type = 'SINGLE'
mul = group.nodes.new('ShaderNodeMath')
mul.operation = 'MULTIPLY'
group.links.new(socket(ci.outputs, 'value'), mul.inputs[0])
group.links.new(gi.outputs['Gain'], mul.inputs[1])
group.links.new(mul.outputs[0], socket(co.inputs, 'Result'))
f = glsl(group, '''/* @glsl_closure v1 */
float density(float value) { return value; }
vec3 probe() { return vec3(density(.5)); }
''', 'probe')
group.links.new(co.outputs[0], socket(f.inputs, 'closure.density'))
group.links.new(f.outputs[0], go.inputs[0])
instance = m.node_tree.nodes.new('ShaderNodeGroup')
instance.node_tree = group
m.node_tree.links.new(instance.outputs[0], e.inputs['Color'])
for gain in (.2, .8, .4):
    instance.inputs['Gain'].default_value = gain
    actual = center(render('closure_' + str(gain)))
    print('CAPTURE', gain, actual, flush=True)
    near(actual, [gain * .5] * 3)
print('CLOSURE_CAPTURE_PASS', flush=True)
