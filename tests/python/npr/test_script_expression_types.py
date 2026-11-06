# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Expression dimensions, alpha routing, stable sockets and saved-file regression."""

import sys
from pathlib import Path

import bpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import OUT, center, near, render, scene


sc, material, emission = scene()
tree = material.node_tree
node = tree.nodes.new('ShaderNodeScriptExpression')


def check(label, expected, output='Result'):
    tree.links.new(node.outputs[output], emission.inputs['Color'])
    near(center(render(label)), expected)


node.expression = '0.25'
check('scalar', [.25] * 3)
node.output_type = 'VECTOR'
node.expression = '0.0'
unused_color = node.variables.new('RGBA', 'Float')
output_material = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeOutputMaterial')
tree.links.new(node.outputs['Result'], output_material.inputs['Surface'])
near(center(render('scalar_zero_vector_direct_surface')), [0, 0, 0])
node.expression = '0.25'
near(center(render('scalar_vector_direct_surface')), [.25] * 3)
tree.links.new(emission.outputs[0], output_material.inputs['Surface'])
node.variables.remove(unused_color)
for dimensions, expected in [('VEC2', [.25, .25, 0]), ('VEC3', [.25] * 3), ('VEC4', [.25] * 3)]:
    node.output_vector_dimensions = dimensions
    check('scalar_to_' + dimensions, expected)
    if dimensions == 'VEC4':
        check('scalar_to_vec4_w', [.25] * 3, 'W')
node.output_type = 'RGBA'
check('scalar_to_color', [.25] * 3)
check('scalar_to_color_alpha', [.25] * 3, 'Alpha')
node.output_type = 'VECTOR'
node.output_vector_dimensions = 'VEC3'
v = node.variables.new('VECTOR', 'v')
assert v.vector_dimensions == 'VEC3'
node.output_type = 'VECTOR'
node.expression = 'v'
node.inputs['v'].default_value = (.2, .3, .4)
check('legacy_vec3', [.2, .3, .4])

v.vector_dimensions = 'VEC2'
node.output_vector_dimensions = 'VEC2'
assert [s.name for s in node.inputs] == ['v']
assert node.inputs['v'].type == 'VECTOR'
node.inputs['v'].default_value = (.2, .35, 9.0)
node.expression = 'v * 2.0'
check('vec2', [.4, .7, 0])

vector_source = tree.nodes.new('ShaderNodeCombineXYZ')
vector_source.inputs['X'].default_value = .15
vector_source.inputs['Y'].default_value = .25
vector_source.inputs['Z'].default_value = 42.0
tree.links.new(vector_source.outputs[0], node.inputs['v'])
check('vec2_linked_ignore_z', [.3, .5, 0])
socket_id = node.inputs['v'].identifier
v.vector_dimensions = 'VEC3'
assert node.inputs['v'].identifier == socket_id and node.inputs['v'].is_linked
v.vector_dimensions = 'VEC2'
assert node.inputs['v'].identifier == socket_id and node.inputs['v'].is_linked
check('vec2_dimension_toggle_preserves_link', [.3, .5, 0])
tree.links.remove(node.inputs['v'].links[0])

v.vector_dimensions = 'VEC4'
node.output_vector_dimensions = 'VEC4'
node.inputs['v'].default_value = (.1, .2, .3)
node.inputs['v W'].default_value = .4
node.expression = 'v * 2.0'
check('vec4_xyz', [.2, .4, .6])
check('vec4_w', [.8] * 3, 'W')

node.variables.remove(v)
color = node.variables.new('RGBA', 'tint')
node.output_type = 'RGBA'
node.expression = 'tint'
assert not color.separate_alpha
node.inputs['tint'].default_value = (.2, .3, .4, .65)
check('legacy_color', [.2, .3, .4])
check('legacy_alpha', [.65] * 3, 'Alpha')

color.separate_alpha = True
assert node.inputs['tint Alpha'].default_value == 1.0
node.inputs['tint Alpha'].default_value = .25
node.expression = 'vec4(tint.rgb * tint.a, tint.a * 2.0)'
check('alpha_default_rgb', [.05, .075, .1])
check('alpha_default_result', [.5] * 3, 'Alpha')

value = tree.nodes.new('ShaderNodeValue')
value.outputs[0].default_value = .4
tree.links.new(value.outputs[0], node.inputs['tint Alpha'])
check('alpha_linked_rgb', [.08, .12, .16])
check('alpha_linked_result', [.8] * 3, 'Alpha')

rgb = tree.nodes.new('ShaderNodeRGB')
rgb.outputs[0].default_value = (.3, .5, .7, 1)
tree.links.new(rgb.outputs[0], node.inputs['tint'])
check('color_and_alpha_linked', [.12, .2, .28])
color_link_id = node.inputs['tint'].identifier
color.name = 'paint'
node.expression = 'vec4(paint.rgb * paint.a, paint.a * 2.0)'
assert node.inputs['paint'].identifier == color_link_id
assert node.inputs['paint'].is_linked and node.inputs['paint Alpha'].is_linked
check('renamed', [.12, .2, .28])

image = bpy.data.images.new('RGBA probe', width=2, height=2, alpha=True, float_buffer=True)
image.alpha_mode = 'CHANNEL_PACKED'
image.pixels[:] = [.25, .5, .75, .3] * 4
image.pack()
texture = tree.nodes.new('ShaderNodeTexImage')
texture.image = image
tree.links.new(texture.outputs['Color'], node.inputs['paint'])
tree.links.new(texture.outputs['Alpha'], node.inputs['paint Alpha'])
check('texture_alpha_rgb', [.075, .15, .225])
check('texture_alpha_result', [.6] * 3, 'Alpha')
tree.links.new(rgb.outputs[0], node.inputs['paint'])
tree.links.new(value.outputs[0], node.inputs['paint Alpha'])

node.variables.new('FLOAT', 'gain')
node.inputs['gain'].default_value = .5
node.expression = 'vec4(paint.rgb * paint.a * gain, paint.a * gain)'
check('multiple_variables_rgb', [.06, .1, .14])
check('multiple_variables_alpha', [.2] * 3, 'Alpha')
node.variables.remove(node.variables[-1])
node.expression = 'vec4(paint.rgb * paint.a, paint.a * 2.0)'

copy = tree.nodes.new('ShaderNodeScriptExpression')
copy.expression = 'vec4(0.1, 0.2, 0.3, 0.4)'
copy.output_type = 'VECTOR'
copy.output_vector_dimensions = 'VEC4'
copy.variables.new('VECTOR', 'uv').vector_dimensions = 'VEC2'
copy.label = 'Manual title'
copy.name = 'Saved vector'
node.name = 'Saved color'
bpy.ops.wm.save_as_mainfile(filepath=str(OUT / 'expression_types.blend'))
bpy.ops.wm.open_mainfile(filepath=str(OUT / 'expression_types.blend'))
material = bpy.data.materials['Probe']
tree = material.node_tree
node = tree.nodes['Saved color']
emission = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeEmission')
assert node.variables[0].separate_alpha
assert node.inputs['paint'].is_linked and node.inputs['paint Alpha'].is_linked
assert tree.nodes['Saved vector'].output_vector_dimensions == 'VEC4'
assert tree.nodes['Saved vector'].variables[0].vector_dimensions == 'VEC2'
assert tree.nodes['Saved vector'].inputs['uv'].type == 'VECTOR'
assert len(tree.nodes['Saved vector'].inputs) == 1
assert tree.nodes['Saved vector'].label == 'Manual title'
check('reopened', [.12, .2, .28])

node.expression = ''
check('invalid_color', [0] * 3)
check('invalid_alpha', [0] * 3, 'Alpha')
print('SCRIPT_EXPRESSION_TYPES_OK', flush=True)
