# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Expression error readback, compiler diagnostics and edit recovery."""
import sys
from pathlib import Path

import bpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import scene, render, center, near, glsl, socket


def expression(tree, name, text='0.2'):
    node = tree.nodes.new('ShaderNodeScriptExpression')
    node.name = name
    node.expression = text
    return node


def refresh(tree):
    tree.update_tag()
    tree.interface_update(bpy.context)
    bpy.context.view_layer.update()


def assert_clean(*nodes):
    for node in nodes:
        assert node.error_message == '', (node.name, node.error_message)

sc, material, emission = scene()
tree = material.node_tree
node = tree.nodes.new('ShaderNodeScriptExpression')
tree.links.new(node.outputs[0], emission.inputs['Color'])
node.expression = 'sin('
assert 'balanced' in node.error_message, node.error_message
near(center(render('syntax_fallback')), [0, 0, 0])
node.expression = '0.2'
assert node.error_message == '', node.error_message
near(center(render('syntax_recovery')), [.2] * 3)

node.expression = 'missing_expression_variable + 1.0'
near(center(render('compiler_error')), [1, 0, 1])
assert 'Expression line' in node.error_message, node.error_message
assert 'missing_expression_variable' in node.error_message, node.error_message
print('COMPILER_DIAGNOSTIC:', node.error_message, flush=True)
node.expression = '0.3'
assert node.error_message == '', node.error_message
near(center(render('compiler_recovery')), [.3] * 3)
assert node.error_message == '', node.error_message

# A compiler error belongs only to the expression which generated it.
neighbor = expression(tree, 'HealthyNeighbor')
unused = expression(tree, 'DisconnectedExpression')
muted = expression(tree, 'MutedExpression')
muted.mute = True
unused_math = tree.nodes.new('ShaderNodeMath')
add = tree.nodes.new('ShaderNodeMath')
add.operation = 'ADD'
tree.links.new(node.outputs[0], add.inputs[0])
tree.links.new(neighbor.outputs[0], add.inputs[1])
tree.links.new(add.outputs[0], emission.inputs['Color'])
node.expression = 'missing_owned_expression +\n 1.0'
near(center(render('owned_error')), [1, 0, 1])
assert 'missing_owned_expression' in node.error_message, node.error_message
assert_clean(neighbor, unused, muted)

# A non-output change must not clear a still valid diagnostic.
tree.links.new(unused.outputs[0], unused_math.inputs[0])
node.label = 'Manual title'
bpy.context.view_layer.update()
assert 'missing_owned_expression' in node.error_message, node.error_message

# Changes to material compilation settings also replace the diagnostic revision.
for mode in ('BLENDED', 'DITHERED'):
    material.surface_render_method = mode
    near(center(render('material_mode_' + mode)), [1, 0, 1])
    assert 'missing_owned_expression' in node.error_message, node.error_message
    assert_clean(neighbor, unused, muted)

# A second material can reuse the failed pass without reusing the first material's node pointers.
duplicate = material.copy()
duplicate_node = duplicate.node_tree.nodes[node.name]
original_material = material
bpy.context.object.data.materials[0] = duplicate
near(center(render('cached_owned_error')), [1, 0, 1])
assert 'missing_owned_expression' in duplicate_node.error_message, duplicate_node.error_message
assert_clean(duplicate.node_tree.nodes[neighbor.name])
duplicate_node.expression = '0.3'
near(center(render('cached_recovery')), [.5] * 3)
assert_clean(duplicate_node, duplicate.node_tree.nodes[neighbor.name])
assert 'missing_owned_expression' in node.error_message, node.error_message
bpy.context.object.data.materials[0] = original_material

node.expression = '0.3'
near(center(render('owned_recovery')), [.5] * 3)
assert_clean(node, neighbor, unused, muted)

# External GLSL failures must never be copied to expressions, including disconnected groups.
sc, material, emission = scene()
tree = material.node_tree
valid = expression(tree, 'ConnectedValidExpression')
unused = expression(tree, 'DisconnectedValidExpression')
muted = expression(tree, 'MutedValidExpression')
muted.mute = True
group_tree = bpy.data.node_groups.new('DisconnectedGroup', 'ShaderNodeTree')
group_expression = expression(group_tree, 'ValidExpressionInGroup')
group = tree.nodes.new('ShaderNodeGroup')
group.node_tree = group_tree
bad = glsl(tree, '''
void unrelated_error(float value, out vec4 Result)
{
    Result = vec4(value + missing_glsl_only_symbol);
}
''', 'unrelated_error')
assert bad.parse_status == 'READY', bad.parse_status
tree.links.new(valid.outputs[0], socket(bad.inputs, 'value'))
tree.links.new(socket(bad.outputs, 'Result'), emission.inputs['Color'])
near(center(render('unrelated_glsl_error')), [1, 0, 1])
assert_clean(valid, unused, muted, group_expression)

bad.script.clear()
bad.script.write('''
void unrelated_error(float value, out vec4 Result)
{
    Result = vec4(value + );
}
''')
refresh(tree)
near(center(render('unrelated_glsl_syntax_error')), [1, 0, 1])
assert_clean(valid, unused, muted, group_expression)
bad.script.clear()
bad.script.write('''
void unrelated_error(float value, out vec4 Result)
{
    Result = vec4(value + 0.1);
}
''')
refresh(tree)
near(center(render('unrelated_glsl_recovery')), [.3] * 3)
assert_clean(valid, unused, muted, group_expression)

# Nested groups use the original node identity, and disconnecting a parent retires old errors.
def group_with_expression(name, text):
    group_tree = bpy.data.node_groups.new(name, 'ShaderNodeTree')
    group_tree.interface.new_socket(name='Result', in_out='OUTPUT', socket_type='NodeSocketFloat')
    output = group_tree.nodes.new('NodeGroupOutput')
    expr = expression(group_tree, 'Expression', text)
    group_tree.links.new(expr.outputs[0], output.inputs['Result'])
    return group_tree, expr


sc, material, emission = scene()
tree = material.node_tree
inner, inner_bad = group_with_expression('InnerBad', 'missing_nested_expression')
healthy, inner_good = group_with_expression('InnerGood', '0.2')
outer = bpy.data.node_groups.new('Outer', 'ShaderNodeTree')
outer.interface.new_socket(name='Result', in_out='OUTPUT', socket_type='NodeSocketFloat')
outer_output = outer.nodes.new('NodeGroupOutput')
inner_instance = outer.nodes.new('ShaderNodeGroup')
inner_instance.node_tree = inner
outer.links.new(inner_instance.outputs[0], outer_output.inputs['Result'])
bad_instance = tree.nodes.new('ShaderNodeGroup')
bad_instance.node_tree = outer
good_instance = tree.nodes.new('ShaderNodeGroup')
good_instance.node_tree = healthy
add = tree.nodes.new('ShaderNodeMath')
add.operation = 'ADD'
tree.links.new(bad_instance.outputs[0], add.inputs[0])
tree.links.new(good_instance.outputs[0], add.inputs[1])
tree.links.new(add.outputs[0], emission.inputs['Color'])
near(center(render('nested_error')), [1, 0, 1])
assert 'missing_nested_expression' in inner_bad.error_message, inner_bad.error_message
assert_clean(inner_good)

# Shared group diagnostics have separate owners: a successful material cannot clear another's error.
shared_material = material.copy()
original_material = material
bpy.context.object.data.materials[0] = shared_material
near(center(render('shared_group_error')), [1, 0, 1])
assert 'missing_nested_expression' in inner_bad.error_message, inner_bad.error_message
shared_emission = shared_material.node_tree.nodes[emission.name]
shared_good = shared_material.node_tree.nodes[good_instance.name]
shared_material.node_tree.links.new(shared_good.outputs[0], shared_emission.inputs['Color'])
near(center(render('shared_group_other_owner_recovery')), [.2] * 3)
assert 'missing_nested_expression' in inner_bad.error_message, inner_bad.error_message
bpy.context.object.data.materials[0] = original_material
near(center(render('shared_group_original_still_fails')), [1, 0, 1])

inner_bad.expression = '0.3'
near(center(render('nested_recovery')), [.5] * 3)
assert_clean(inner_bad, inner_good)
inner_bad.expression = 'missing_nested_after_edit'
near(center(render('nested_error_again')), [1, 0, 1])
assert 'missing_nested_after_edit' in inner_bad.error_message, inner_bad.error_message
tree.links.new(good_instance.outputs[0], emission.inputs['Color'])
near(center(render('nested_disconnect')), [.2] * 3)
assert_clean(inner_bad, inner_good)

# A result conversion/type error is part of expression evaluation, not a material-wide warning.
bad_type = expression(tree, 'BadResultType', 'vec3(0.2, 0.3, 0.4) + vec2(0.2, 0.3)')
tree.links.new(bad_type.outputs[0], emission.inputs['Color'])
near(center(render('result_type_error')), [1, 0, 1])
assert 'Expression line' in bad_type.error_message, bad_type.error_message
assert_clean(inner_bad, inner_good)
bad_type.expression = '0.4'
near(center(render('result_type_recovery')), [.4] * 3)
assert_clean(bad_type, inner_bad, inner_good)

# Undo/redo rebuild evaluated trees and must not revive a diagnostic for a different source.
material_name = material.name
bad_name = bad_type.name
bpy.context.preferences.edit.use_global_undo = True
bpy.ops.ed.undo_push(message='Valid expression')
bad_type.expression = 'missing_undo_expression'
near(center(render('before_undo_error')), [1, 0, 1])
assert 'missing_undo_expression' in bad_type.error_message, bad_type.error_message
bpy.ops.ed.undo_push(message='Invalid expression')
assert bpy.ops.ed.undo() == {'FINISHED'}
material = bpy.data.materials[material_name]
bad_type = material.node_tree.nodes[bad_name]
near(center(render('undo_recovery')), [.4] * 3)
assert_clean(bad_type)
assert bpy.ops.ed.redo() == {'FINISHED'}
material = bpy.data.materials[material_name]
bad_type = material.node_tree.nodes[bad_name]
near(center(render('redo_error')), [1, 0, 1])
assert 'missing_undo_expression' in bad_type.error_message, bad_type.error_message
material.node_tree.nodes.remove(bad_type)
near(center(render('delete_error_node')), [1, 1, 1])

# Parse full compiler logs but retain only bounded expression diagnostics.
sc, material, emission = scene()
tree = material.node_tree
healthy = expression(tree, 'HealthyBesideLongError')
long_errors = []
previous = None
for i in range(40):
    symbol = f'missing_long_{i}_' + 'a' * 192
    long_error = expression(tree, f'LongError{i}', symbol)
    assert long_error.expression == symbol, 'The fixture must not hit the 511-character RNA limit'
    long_errors.append(long_error)
    if previous is None:
        previous = long_error.outputs[0]
    else:
        add = tree.nodes.new('ShaderNodeMath')
        add.operation = 'ADD'
        tree.links.new(previous, add.inputs[0])
        tree.links.new(long_error.outputs[0], add.inputs[1])
        previous = add.outputs[0]
tree.links.new(previous, emission.inputs['Color'])
near(center(render('long_error_log')), [1, 0, 1])
diagnostic_characters = 0
for i, long_error in enumerate(long_errors):
    message = long_error.error_message
    if message:
        assert f'missing_long_{i}_' in message, (i, message)
        assert all(f'missing_long_{j}_' not in message for j in range(40) if j != i), (i, message)
        assert len(message) < 65536, len(message)
        diagnostic_characters += len(message)
assert diagnostic_characters > 0
print('LONG_DIAGNOSTIC_CHARACTERS', diagnostic_characters, flush=True)
assert_clean(healthy)
for long_error in long_errors:
    long_error.expression = str(.25 / 40)
near(center(render('long_error_recovery')), [.25] * 3)
assert_clean(*long_errors, healthy)

# An animated endpoint can prune every expression without editing the original node tree.
sc, material, emission = scene()
bpy.context.view_layer.name = 'Animated/Expression/Errors'
tree = material.node_tree
animated_bad = expression(tree, 'AnimatedPrunedExpression', 'missing_animated_branch')
mix = tree.nodes.new('ShaderNodeMix')
mix.data_type = 'FLOAT'
mix.inputs[3].default_value = .2
tree.links.new(animated_bad.outputs[0], mix.inputs[2])
tree.links.new(mix.outputs[0], emission.inputs['Color'])
for frame, factor in ((1, 0.0), (2, 1.0), (3, 0.0)):
    mix.inputs[0].default_value = factor
    mix.inputs[0].keyframe_insert('default_value', frame=frame)
for frame, failed in ((1, True), (2, False), (3, True), (2, False)):
    if frame == 2:
        bpy.context.view_layer.name += '/Renamed'
        assert 'missing_animated_branch' in animated_bad.error_message, animated_bad.error_message
    sc.frame_set(frame)
    near(center(render('animated_prune_%d' % frame)), [1, 0, 1] if failed else [.2] * 3)
    if failed:
        assert 'missing_animated_branch' in animated_bad.error_message, animated_bad.error_message
    else:
        assert_clean(animated_bad)

# Removing a diagnostic consumer must retire its snapshot.
sc.frame_set(1)
near(center(render('before_layer_delete')), [1, 0, 1])
assert 'missing_animated_branch' in animated_bad.error_message
old_layer = bpy.context.view_layer
new_layer = sc.view_layers.new('Replacement layer')
bpy.context.window.view_layer = new_layer
sc.view_layers.remove(old_layer)
assert_clean(animated_bad)
sc.frame_set(2)
near(center(render('after_layer_delete')), [.2] * 3)
assert_clean(animated_bad)

# World and fullscreen Filter shaders bypass the surface material pass.
sc, material, emission = scene()
world = bpy.data.worlds.new('Expression world')
world.use_nodes = True
sc.world = world
background = next(n for n in world.node_tree.nodes if n.bl_idname == 'ShaderNodeBackground')
world_bad = expression(world.node_tree, 'World expression', 'missing_world_expression')
world_good = expression(world.node_tree, 'Unrelated world expression')
world.node_tree.links.new(world_bad.outputs[0], background.inputs['Color'])
for obj in sc.objects:
    if obj.type == 'MESH':
        obj.hide_render = True
render('world_error')
assert 'missing_world_expression' in world_bad.error_message, world_bad.error_message
assert_clean(world_good)
world_bad.expression = '0.2'
near(center(render('world_recovery')), [.2] * 3)
assert_clean(world_bad, world_good)

# A successful surface variant cannot erase a failed volume variant in the same world.
volume = world.node_tree.nodes.new('ShaderNodeVolumeScatter')
volume_bad = expression(world.node_tree, 'World volume expression', '0.0')
world.node_tree.links.new(volume_bad.outputs[0], volume.inputs['Density'])
world_output = next(n for n in world.node_tree.nodes if n.bl_idname == 'ShaderNodeOutputWorld')
volume_link = world.node_tree.links.new(volume.outputs[0], world_output.inputs['Volume'])
volume_reference = center(render('world_volume_reference'))
assert_clean(volume_bad, world_bad, world_good)
volume_bad.expression = 'missing_world_volume_expression'
render('world_volume_error')
assert 'missing_world_volume_expression' in volume_bad.error_message, volume_bad.error_message
assert_clean(world_bad, world_good)
volume_bad.expression = '0.0'
near(center(render('world_volume_recovery')), volume_reference)
assert_clean(volume_bad, world_bad, world_good)
world.node_tree.links.remove(volume_link)
near(center(render('world_volume_disconnect')), [.2] * 3)
assert_clean(volume_bad, world_bad, world_good)

from filter_graph_test_utils import attach_filter_material, clear_filter_graph
clear_filter_graph(sc)
filter_material = bpy.data.materials.new('Expression filter')
filter_material.use_nodes = True
filter_material.eevee_domain = 'FILTER'
filter_tree = filter_material.node_tree
filter_tree.nodes.clear()
filter_output = filter_tree.nodes.new('ShaderNodeOutputFilter')
filter_output.inputs['Alpha'].default_value = 1
filter_bad = expression(filter_tree, 'Filter expression', 'missing_filter_expression')
filter_good = expression(filter_tree, 'Unrelated filter expression')
filter_tree.links.new(filter_bad.outputs[0], filter_output.inputs['Color'])
attach_filter_material(filter_material, stage='BEFORE_COMPOSITE')
render('filter_error')
assert 'missing_filter_expression' in filter_bad.error_message, filter_bad.error_message
assert_clean(filter_good)
filter_bad.expression = '0.4'
near(center(render('filter_recovery')), [.4] * 3)
assert_clean(filter_bad, filter_good)
print('SCRIPT_EXPRESSION_ERRORS_OK', flush=True)
