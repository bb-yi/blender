# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

from itertools import permutations
from pathlib import Path
import json
import sys

import bpy

SOURCE = Path(__file__).resolve().parents[3]
ROOT = SOURCE.parent
sys.path.insert(0, str(SOURCE / 'tests/python/npr'))
from test_glsl_function_bundle import combine, function, refresh
from material_compat_test_utils import OUT, scene


def sync(tree, node):
    tree.nodes.active = node
    window = bpy.context.window_manager.windows[0]
    area = max(window.screen.areas, key=lambda a: a.width*a.height)
    area.type = 'NODE_EDITOR'
    area.spaces.active.tree_type = 'ShaderNodeTree'
    area.spaces.active.shader_type = 'OBJECT'
    area.spaces.active.path.start(tree)
    region = next(r for r in area.regions if r.type == 'WINDOW')
    try:
        with bpy.context.temp_override(window=window, area=area, region=region):
            result = bpy.ops.node.sockets_sync()
        return result == {'FINISHED'}, ''
    except RuntimeError as exc:
        return False, str(exc)


records = []
for nested in (False, True):
    for dimensions, strict_dimensions in ((3,3), (4,4), (4,3)):
        for order in permutations(('color', 'vector', 'strict')):
            _, material, _ = scene()
            tree = material.node_tree
            packet = combine(tree, [])
            root = combine(tree, [('inner','BUNDLE',packet.outputs[0])]) if nested else packet
            consumers = {}
            for name, kind in (('color','RGBA'), ('vector','VECTOR')):
                outer = tree.nodes.new('NodeSeparateBundle')
                inner = outer
                if nested:
                    outer.bundle_items.new('BUNDLE','inner')
                    inner = tree.nodes.new('NodeSeparateBundle')
                    tree.links.new(outer.outputs['inner'], inner.inputs[0])
                item = inner.bundle_items.new(kind, 'color')
                if kind == 'VECTOR':
                    item.vector_socket_dimensions = dimensions
                consumers[name] = outer.inputs[0]
            definitions = f'struct Inner {{ vec{strict_dimensions} color; }};'
            if nested:
                definitions += 'struct Data { Inner inner; };'
            else:
                definitions = f'struct Data {{ vec{strict_dimensions} color; }};'
            path = 'd.inner.color' if nested else 'd.color'
            source = definitions + '\n/* @glsl_meta v2\n' + path + ': subtype=color\n*/\n'
            source += 'float f(Data d) { return 0.0; }'
            strict = function(tree, source, 'f')
            strict.name = 'StrictConsumer'
            consumers['strict'] = strict.inputs[0]
            for name in order:
                tree.links.new(root.outputs[0], consumers[name])
            refresh(tree)
            okay, error = sync(tree, packet)
            expected = dimensions == strict_dimensions
            record = {'nested':nested, 'dimensions':dimensions, 'strict_dimensions':strict_dimensions,
                      'order':order, 'success':okay, 'expected':expected, 'error':error,
                      'fields':[item.name for item in packet.bundle_items]}
            records.append(record)

# A type conflict in a provided nested packet must prevent adding a root sibling.
_, material, _ = scene()
tree = material.node_tree
child = combine(tree, [('color','FLOAT',0.25)])
root = combine(tree, [('inner','BUNDLE',child.outputs[0])])
node = function(tree, 'struct Inner { vec3 color; }; struct Data { Inner inner; float gain; }; float f(Data d) { return d.gain; }', 'f')
tree.links.new(root.outputs[0], node.inputs[0])
before = [item.name for item in root.bundle_items]
okay, error = sync(tree, root)
atomic = not okay and [item.name for item in root.bundle_items] == before
report = {'permutations':records, 'nested_atomic':atomic, 'nested_error':error,
          'nested_fields':[item.name for item in root.bundle_items]}

extra = []
# Reusing one child in two fields is valid; each path must still check its own schema.
for different_schema in (False, True):
    _, material, _ = scene()
    tree = material.node_tree
    child = combine(tree, [('x','FLOAT',0.25)])
    packet = combine(tree, [('a','BUNDLE',child.outputs[0]), ('b','BUNDLE',child.outputs[0])])
    b_type = 'vec3' if different_schema else 'float'
    source = f'struct A {{ float x; }}; struct B {{ {b_type} x; }}; struct Data {{ A a; B b; float gain; }}; float f(Data d) {{ return d.gain; }}'
    target = function(tree, source, 'f')
    tree.links.new(packet.outputs[0], target.inputs[0])
    before = [item.name for item in packet.bundle_items]
    okay, error = sync(tree, packet)
    expected = not different_schema
    assert okay == expected, ('shared child', different_schema, error)
    assert okay or [item.name for item in packet.bundle_items] == before
    extra.append({'shared_child_different_schema':different_schema, 'success':okay, 'error':error})

# Rerouted links back into the same packet must not mutate it or hang.
_, material, _ = scene()
tree = material.node_tree
packet = combine(tree, [('inner','BUNDLE',None)])
route = tree.nodes.new('NodeReroute')
tree.links.new(packet.outputs[0], route.inputs[0])
tree.links.new(route.outputs[0], packet.inputs['inner'])
target = function(tree, 'struct Inner { float x; }; struct Data { Inner inner; float gain; }; float f(Data d) { return d.gain; }', 'f')
tree.links.new(packet.outputs[0], target.inputs[0])
before = [item.name for item in packet.bundle_items]
okay, error = sync(tree, packet)
assert [item.name for item in packet.bundle_items] == before, ('cycle', okay, error)
extra.append({'rerouted_cycle':True, 'error':error, 'unchanged':True})

# Each group instance resolves the group's Bundle input in its own compute context.
for wrong_instance in (False, True):
    _, material, _ = scene()
    tree = material.node_tree
    group = bpy.data.node_groups.new('BundleSyncGroup', 'ShaderNodeTree')
    group.interface.new_socket(name='Packet', in_out='INPUT', socket_type='NodeSocketBundle')
    group.interface.new_socket(name='Packet', in_out='OUTPUT', socket_type='NodeSocketBundle')
    inp = group.nodes.new('NodeGroupInput')
    out = group.nodes.new('NodeGroupOutput')
    internal = combine(group, [('inner','BUNDLE',inp.outputs[0])])
    group.links.new(internal.outputs[0], out.inputs[0])
    instances = [tree.nodes.new('ShaderNodeGroup') for _ in range(2)]
    for index, instance in enumerate(instances):
        instance.node_tree = group
        kind = 'VEC3' if wrong_instance and index == 1 else 'FLOAT'
        value = (0.25,0.5,0.75) if kind == 'VEC3' else 0.25
        source = combine(tree, [('x',kind,value)])
        tree.links.new(source.outputs[0], instance.inputs[0])
    packet = combine(tree, [('a','BUNDLE',instances[0].outputs[0]), ('b','BUNDLE',instances[1].outputs[0])])
    target = function(tree, 'struct Inner { float x; }; struct Outer { Inner inner; }; struct Data { Outer a; Outer b; float gain; }; float f(Data d) { return d.gain; }', 'f')
    tree.links.new(packet.outputs[0], target.inputs[0])
    before = [item.name for item in packet.bundle_items]
    okay, error = sync(tree, packet)
    assert okay == (not wrong_instance), ('group context', wrong_instance, error)
    assert okay or [item.name for item in packet.bundle_items] == before
    extra.append({'group_wrong_instance':wrong_instance, 'success':okay, 'error':error})

# The 512 limit belongs to each exported GLSL call, not a shared source packet.
_, material, _ = scene()
tree = material.node_tree
packet = combine(tree, [('extra','FLOAT',0.75)])
for prefix in ('a', 'b'):
    fields = ''.join(f'float {prefix}{i};' for i in range(300))
    target = function(tree, f'struct Data {{ {fields} }}; float f(Data d) {{ return 0.0; }}', 'f')
    tree.links.new(packet.outputs[0], target.inputs[0])
okay, error = sync(tree, packet)
assert okay and len(packet.bundle_items) == 601, (okay, len(packet.bundle_items), error)
assert packet.inputs['extra'].default_value == 0.75
extra.append({'merged_source_items':len(packet.bundle_items)})
report['extra'] = extra
(OUT / 'sync_order.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
bad = [r for r in records if r['success'] != r['expected']]
print('SYNC_ORDER_SUMMARY', json.dumps({'failures':len(bad), 'cases':len(records),
      'nested_atomic':atomic, 'first_failures':bad[:3]}), flush=True)
assert not bad and atomic, (len(bad), atomic)
print('GLSL_BUNDLE_SYNC_ORDER_OK', flush=True)
