# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import *
s, m, e = scene()
group = bpy.data.node_groups.new('SamplerCapture', 'ShaderNodeTree')
group.interface.new_socket(name='Gain', in_out='INPUT', socket_type='NodeSocketFloat').default_value = .1
group.interface.new_socket(name='Color', in_out='OUTPUT', socket_type='NodeSocketColor')
gi = group.nodes.new('NodeGroupInput')
go = group.nodes.new('NodeGroupOutput')
ci = group.nodes.new('NodeClosureInput')
co = group.nodes.new('NodeClosureOutput')
ci.pair_with_output(co)
co.input_items.new('VECTOR', 'UV')
co.output_items.new('FLOAT', 'Color')
sep = group.nodes.new('ShaderNodeSeparateXYZ')
group.links.new(ci.outputs['UV'], sep.inputs[0])
mul = group.nodes.new('ShaderNodeMath')
mul.operation = 'MULTIPLY'
group.links.new(sep.outputs['X'], mul.inputs[0])
group.links.new(gi.outputs['Gain'], mul.inputs[1])
group.links.new(mul.outputs[0], co.inputs['Color'])
f = glsl(group, 'vec3 probe(sampler3D volume) { return texture(volume,vec3(.5)).rgb; }', 'probe')
group.links.new(co.outputs[0], socket(f.inputs, 'volume'))
group.links.new(f.outputs[0], go.inputs[0])
instance = m.node_tree.nodes.new('ShaderNodeGroup')
instance.node_tree = group
m.node_tree.links.new(instance.outputs[0], e.inputs['Color'])
for gain in (.2, .8, .4):
    instance.inputs['Gain'].default_value = gain
    actual = center(render('procedural_' + str(gain)))
    print('PROCEDURAL_CAPTURE', gain, actual, flush=True)
    near(actual, [gain * .5] * 3)
print('PROCEDURAL_CAPTURE_PASS', flush=True)

# Share the same closure-producing group under another group and use two different gains.
wrapper = bpy.data.node_groups.new('NestedSamplerCapture', 'ShaderNodeTree')
wrapper.interface.new_socket(name='Gain', in_out='INPUT', socket_type='NodeSocketFloat')
wrapper.interface.new_socket(name='Color', in_out='OUTPUT', socket_type='NodeSocketColor')
wi = wrapper.nodes.new('NodeGroupInput')
wo = wrapper.nodes.new('NodeGroupOutput')
inner = wrapper.nodes.new('ShaderNodeGroup')
inner.node_tree = group
wrapper.links.new(wi.outputs['Gain'], inner.inputs['Gain'])
wrapper.links.new(inner.outputs[0], wo.inputs[0])
a = m.node_tree.nodes.new('ShaderNodeGroup')
a.node_tree = wrapper
a.inputs['Gain'].default_value = .2
b = m.node_tree.nodes.new('ShaderNodeGroup')
b.node_tree = wrapper
b.inputs['Gain'].default_value = .6
add = m.node_tree.nodes.new('ShaderNodeVectorMath')
add.operation = 'ADD'
m.node_tree.links.new(a.outputs[0], add.inputs[0])
m.node_tree.links.new(b.outputs[0], add.inputs[1])
m.node_tree.links.new(add.outputs[0], e.inputs['Color'])
near(center(render('procedural_shared_nested')), [.4] * 3)
b.inputs['Gain'].default_value = .8
near(center(render('procedural_shared_edit')), [.5] * 3)
print('PROCEDURAL_NESTED_PASS', flush=True)
