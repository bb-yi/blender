# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""GLSL struct boundaries preserve typed Bundle values and missing-field semantics.

Run with isolated user resources and BLENDER_TEST_OUTPUT_DIR. --smoke runs the
initial parser/numeric/default checks; the default runs the full regression.
"""

import json
import os
import sys
import time
from pathlib import Path

import bpy
import gpu
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import OUT, background_child, center, glsl, render, scene

TOLERANCE = 1e-5
RESULTS = []


def refresh(tree):
    tree.update_tag()
    tree.interface_update(bpy.context)
    bpy.context.view_layer.update()


def function(tree, source, name):
    node = glsl(tree, source, name)
    assert node.parse_status == 'READY', (name, node.parse_status)
    return node


def combine(tree, entries):
    node = tree.nodes.new('NodeCombineBundle')
    for key, kind, value in entries:
        dimensions = int(kind[-1]) if kind.startswith('VEC') else None
        item = node.bundle_items.new('VECTOR' if dimensions else kind, key)
        if dimensions:
            item.vector_socket_dimensions = dimensions
        refresh(tree)
        target = node.inputs[key]
        if isinstance(value, bpy.types.NodeSocket):
            tree.links.new(value, target)
        elif value is not None:
            target.default_value = value
    refresh(tree)
    return node


def pixel(label, expected):
    started = time.perf_counter()
    value = center(render(label))
    delta = float(np.max(np.abs(np.asarray(value) - expected)))
    assert delta <= TOLERANCE, (label, value.tolist(), expected, delta)
    RESULTS.append({'case': label, 'pixel': value.tolist(), 'max_error': delta,
                    'seconds': time.perf_counter() - started})
    print('GLSL_BUNDLE_CASE', label, value.tolist(), flush=True)
    return value


def diagnosed_failure(label, node, expected_text):
    """Inspect the revision-owned diagnostic, independently of engine fallback color."""
    error_path = OUT / (label + '_diagnostic.txt')
    value = center(render(label))
    diagnostic = node.error_message
    error_path.write_text(diagnostic, encoding='utf-8')
    assert expected_text in diagnostic, (label, diagnostic)
    RESULTS.append({'case': label, 'pixel': value.tolist(), 'diagnostic': diagnostic})
    return value


def test_numeric_members():
    _, material, emission = scene()
    tree = material.node_tree
    source = '''
struct Data { float gain; vec2 uv; vec3 normal; vec4 payload; int mode; bool enabled; };
vec3 read_data(Data data) {
  return vec3(data.uv.y + data.gain, data.normal.z + float(data.mode) * 0.125,
              data.enabled ? data.payload.w : data.payload.x);
}
'''
    node = function(tree, source, 'read_data')
    assert len(node.inputs) == 1 and node.inputs[0].type == 'BUNDLE'
    bundle = combine(tree, [
        ('payload', 'VEC4', (0.125, 0.25, 0.5, 0.75)),
        ('extra', 'FLOAT', 99.0), ('mode', 'INT', 2), ('enabled', 'BOOLEAN', True),
        ('normal', 'VEC3', (0.0, 0.0, 0.25)), ('gain', 'FLOAT', 0.125),
        ('uv', 'VEC2', (0.0, 0.125)),
    ])
    assert len(bundle.inputs['payload'].default_value) == 4
    assert len(bundle.inputs['uv'].default_value) == 2
    tree.links.new(bundle.outputs[0], node.inputs['data'])
    tree.links.new(node.outputs[0], emission.inputs['Color'])
    pixel('numeric_members', (0.25, 0.5, 0.75))
    bundle.inputs['payload'].default_value[3] = 0.625
    bundle.inputs['gain'].default_value = 0.0
    pixel('numeric_edit_w_and_zero', (0.125, 0.5, 0.625))


def test_defaults_and_presence():
    _, material, emission = scene()
    tree = material.node_tree
    source = '''
struct Inner { float gain; vec4 tint; };
struct Data { Inner inner; vec3 normal; };
float default_gain() { return 0.25; }
/* @glsl_meta v2
data: label="Data"
data.inner.gain: default=default_gain()
data.inner.tint: default=vec4(0.125, 0.25, 0.5, 0.75) subtype=color
data.normal: default=glsl_normal()
*/
vec3 defaults(Data data) {
  return vec3(data.inner.gain, data.inner.tint.a, data.normal.z * 0.5);
}
'''
    node = function(tree, source, 'defaults')
    tree.links.new(node.outputs[0], emission.inputs['Color'])
    pixel('all_missing_defaults', (0.25, 0.75, 0.5))
    inner = combine(tree, [('gain', 'FLOAT', 0.0)])
    outer = combine(tree, [('inner', 'BUNDLE', inner.outputs[0])])
    tree.links.new(outer.outputs[0], node.inputs[0])
    pixel('explicit_zero_is_present', (0.0, 0.75, 0.5))
    value = tree.nodes.new('ShaderNodeValue')
    value.outputs[0].default_value = 0.125
    tree.links.new(value.outputs[0], inner.inputs['gain'])
    pixel('folded_link_is_present', (0.125, 0.75, 0.5))
    tree.links.remove(node.inputs[0].links[0])
    pixel('disconnect_restores_defaults', (0.25, 0.75, 0.5))


def test_return_out_and_matrices():
    _, material, emission = scene()
    tree = material.node_tree
    source = '''
struct Inner { vec4 tint; };
struct Data { Inner inner; mat2 a; mat3 b; mat4 c; };
Data modify_data(Data data, out Data copied) {
  copied = data;
  copied.inner.tint.w = 0.125;
  data.inner.tint.w += 0.125;
  return data;
}
vec3 inspect_data(Data data) {
  return vec3(data.a[1][0] + data.inner.tint.w, data.b[2][1], data.c[3][3]);
}
'''
    nested = combine(tree, [('tint', 'VEC4', (0.0, 0.25, 0.5, 0.25))])
    a = combine(tree, [('c0', 'VEC2', (1.0, 0.75)), ('c1', 'VEC2', (0.125, 1.0))])
    b = combine(tree, [('c0', 'VEC3', (1.0, 0.0, 0.0)), ('c1', 'VEC3', (0.125, 1.0, 0.0)),
                       ('c2', 'VEC3', (0.0, 0.25, 1.0))])
    c = combine(tree, [('c0', 'VEC4', (1.0, 0.0, 0.0, 0.125)),
                       ('c1', 'VEC4', (0.0, 1.0, 0.0, 0.25)),
                       ('c2', 'VEC4', (0.0, 0.0, 1.0, 0.5)),
                       ('c3', 'VEC4', (0.125, 0.25, 0.5, 0.75))])
    bundle = combine(tree, [('inner', 'BUNDLE', nested.outputs[0]), ('a', 'BUNDLE', a.outputs[0]),
                            ('b', 'BUNDLE', b.outputs[0]), ('c', 'BUNDLE', c.outputs[0])])
    producer = function(tree, source, 'modify_data')
    consumer = function(tree, source, 'inspect_data')
    assert [socket.type for socket in producer.outputs] == ['BUNDLE', 'BUNDLE']
    tree.links.new(bundle.outputs[0], producer.inputs[0])
    tree.links.new(producer.outputs['Result'], consumer.inputs[0])
    tree.links.new(consumer.outputs[0], emission.inputs['Color'])
    pixel('nested_matrix_return', (0.5, 0.25, 0.75))
    tree.links.new(producer.outputs['copied'], consumer.inputs[0])
    pixel('nested_matrix_out', (0.25, 0.25, 0.75))
    # The originating packet is immutable; an additional consumer still sees the old W.
    tree.links.new(bundle.outputs[0], consumer.inputs[0])
    pixel('input_bundle_unchanged', (0.375, 0.25, 0.75))


def test_parse_rejections_preserve_interface():
    _, material, _ = scene()
    tree = material.node_tree
    good = 'struct Data { float x; }; float f(Data d) { return d.x; }'
    node = function(tree, good, 'f')
    original = [(s.identifier, s.bl_idname) for s in node.inputs]
    invalid = [
        'struct Data { sampler2D image; }; float f(Data d) { return 0.0; }',
        'struct Data { float x[2]; }; float f(Data d) { return 0.0; }',
        'struct Data { Data child; }; float f(Data d) { return 0.0; }',
        'struct Data { float x; float x; }; float f(Data d) { return 0.0; }',
        'struct Data { float x; }; float f(inout Data d) { return d.x; }',
        'struct Data { float x; }; /* @glsl_meta v2\nd.missing: default=1.0\n*/\nfloat f(Data d) { return d.x; }',
        'struct Data { float x; }; /* @glsl_meta v2\nd.x: default=d.x\n*/\nfloat f(Data d) { return d.x; }',
    ]
    for index, source in enumerate(invalid):
        node.script.clear()
        node.script.write(source)
        node.function_name = ''
        node.function_name = 'f'
        refresh(tree)
        assert node.parse_status == 'ERROR', (index, node.parse_status)
        assert [(s.identifier, s.bl_idname) for s in node.inputs] == original, index
    node.script.clear()
    node.script.write(good)
    node.function_name = ''
    node.function_name = 'f'
    refresh(tree)
    assert node.parse_status == 'READY'


def test_group_instances_and_reload():
    _, material, emission = scene()
    tree = material.node_tree
    group = bpy.data.node_groups.new('BundleConsumer', 'ShaderNodeTree')
    group.interface.new_socket(name='Packet', in_out='INPUT', socket_type='NodeSocketBundle')
    group.interface.new_socket(name='Color', in_out='OUTPUT', socket_type='NodeSocketColor')
    inp = group.nodes.new('NodeGroupInput')
    out = group.nodes.new('NodeGroupOutput')
    source = '''struct Data { vec4 value; };
/* @glsl_meta v2
d.value: default=vec4(0.125,0.25,0.5,0.75)
*/
vec3 f(Data d) { return vec3(d.value.x,d.value.y,d.value.w); }
'''
    node = function(group, source, 'f')
    group.links.new(inp.outputs['Packet'], node.inputs[0])
    group.links.new(node.outputs[0], out.inputs['Color'])
    a, b = [tree.nodes.new('ShaderNodeGroup') for _ in range(2)]
    a.node_tree = b.node_tree = group
    bundle = combine(tree, [('value', 'VEC4', (0.25, 0.5, 0.0, 0.625))])
    tree.links.new(bundle.outputs[0], a.inputs[0])
    tree.links.new(a.outputs[0], emission.inputs['Color'])
    pixel('group_connected_instance', (0.25, 0.5, 0.625))
    tree.links.new(b.outputs[0], emission.inputs['Color'])
    pixel('group_empty_instance', (0.125, 0.25, 0.75))
    tree.links.new(a.outputs[0], emission.inputs['Color'])
    pixel('group_restore_instance', (0.25, 0.5, 0.625))
    assets = Path(os.environ.get('BLENDER_TEST_ASSET_DIR', str(OUT / 'assets')))
    assets.mkdir(parents=True, exist_ok=True)
    path = assets / 'bundle_roundtrip.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(path))
    result = background_child(__file__, ('--reopen', str(path)))
    assert 'GLSL_BUNDLE_REOPEN_OK' in result.splitlines(), result[-3000:]


def test_missing_invalid_and_recovery():
    _, material, emission = scene()
    tree = material.node_tree
    source = '''struct Data { float x; vec4 y; };
/* @glsl_meta v2
d.y: default=vec4(0.0, 0.0, 0.0, 0.75)
*/
vec3 f(Data d) { return vec3(d.x, 0.25, d.y.w); }
'''
    node = function(tree, source, 'f')
    tree.links.new(node.outputs[0], emission.inputs['Color'])
    pixel('missing_required_error', (1.0, 0.0, 1.0))
    bundle = combine(tree, [('x', 'FLOAT', 0.5)])
    tree.links.new(bundle.outputs[0], node.inputs[0])
    pixel('missing_required_recovered', (0.5, 0.25, 0.75))
    bad = combine(tree, [('x', 'FLOAT', 0.5), ('y', 'FLOAT', 0.1)])
    tree.links.new(bad.outputs[0], node.inputs[0])
    pixel('wrong_type_does_not_use_default', (1.0, 0.0, 1.0))
    tree.links.new(bundle.outputs[0], node.inputs[0])
    pixel('wrong_type_recovered', (0.5, 0.25, 0.75))
    node.script.clear()
    node.script.write('''struct Data { mat4 matrix; };
/* @glsl_meta v2
d.matrix: default=mat4(0.5)
*/
vec3 f(Data d) { return vec3(d.matrix[0][0], d.matrix[1][1], d.matrix[3][3]); }
''')
    node.function_name = ''
    node.function_name = 'f'
    refresh(tree)
    tree.links.remove(node.inputs[0].links[0])
    pixel('matrix_whole_default', (0.5, 0.5, 0.5))
    matrix = combine(tree, [('c0', 'VEC4', (0.25, 0.0, 0.0, 0.0))])
    partial = combine(tree, [('matrix', 'BUNDLE', matrix.outputs[0])])
    tree.links.new(partial.outputs[0], node.inputs[0])
    pixel('partial_matrix_is_error', (1.0, 0.0, 1.0))


def test_world_filter_npr():
    source = '''struct Data { vec3 color; };
/* @glsl_meta v2
d.color: default=vec3(0.125,0.25,0.5) subtype=color
*/
vec3 f(Data d) { return d.color; }
'''
    invalid = 'struct Data { vec3 color; }; vec3 f(Data d) { return d.color; }'
    for domain in ('WORLD', 'FILTER', 'NPR'):
        sc, material, emission = scene()
        if domain == 'WORLD':
            bpy.context.object.hide_render = True
            world = bpy.data.worlds.new('BundleWorld')
            sc.world = world
            world.use_nodes = True
            tree = world.node_tree
            tree.nodes.clear()
            output = tree.nodes.new('ShaderNodeOutputWorld')
            background = tree.nodes.new('ShaderNodeBackground')
            tree.links.new(background.outputs[0], output.inputs['Surface'])
            target = background.inputs['Color']
        elif domain == 'FILTER':
            from filter_graph_test_utils import attach_filter_material
            material = bpy.data.materials.new('BundleFilter')
            material.use_nodes = True
            material.eevee_domain = 'FILTER'
            tree = material.node_tree
            tree.nodes.clear()
            output = tree.nodes.new('ShaderNodeOutputFilter')
            output.inputs['Alpha'].default_value = 1.0
            target = output.inputs['Color']
            attach_filter_material(material)
        else:
            tree = bpy.data.node_groups.new('BundleNPR', 'ShaderNodeTree')
            output = tree.nodes.new('ShaderNodeNPR_Output')
            target = output.inputs['Color']
            material.node_tree.nodes.get('Material Output').nprtree = tree
        node = function(tree, source, 'f')
        tree.links.new(node.outputs[0], target)
        pixel(domain.lower() + '_bundle_valid', (0.125, 0.25, 0.5))
        node.script.clear()
        node.script.write(invalid)
        node.function_name = ''
        node.function_name = 'f'
        refresh(tree)
        failed_pixel = diagnosed_failure(domain.lower() + '_bundle_error', node, "Missing required bundle field 'd.color'")
        # World deliberately falls back to its default world. Other domains also
        # retain their existing engine failure policy; compare to a plain GLSL error.
        node.script.clear()
        node.script.write('vec3 f() { return vec3(bundle_test_missing_symbol); }')
        node.function_name = ''
        node.function_name = 'f'
        refresh(tree)
        reference = center(render(domain.lower() + '_ordinary_glsl_error'))
        assert np.max(np.abs(failed_pixel - reference)) <= TOLERANCE, (domain, failed_pixel, reference)
        node.script.clear()
        node.script.write(source)
        node.function_name = ''
        node.function_name = 'f'
        refresh(tree)
        pixel(domain.lower() + '_bundle_recovered', (0.125, 0.25, 0.5))


def test_same_type_name_different_schema():
    _, material, emission = scene()
    tree = material.node_tree
    a = function(tree, '''struct Data { vec4 color; };
/* @glsl_meta v2
d.color: default=vec4(0.0,0.0,0.0,0.125)
*/
float a(Data d) { return d.color.w; }
''', 'a')
    b = function(tree, '''struct Data { float x; float y; };
/* @glsl_meta v2
d.x: default=0.25
d.y: default=0.125
*/
float b(Data d) { return d.x+d.y; }
''', 'b')
    add = tree.nodes.new('ShaderNodeMath')
    add.operation = 'ADD'
    tree.links.new(a.outputs[0], add.inputs[0])
    tree.links.new(b.outputs[0], add.inputs[1])
    tree.links.new(add.outputs[0], emission.inputs['Color'])
    pixel('same_name_independent_schemas', (0.5, 0.5, 0.5))


def test_routing_join_and_static_repeat():
    _, material, emission = scene()
    tree = material.node_tree
    source = '''struct Data { vec3 color; };
Data advance(Data d) { d.color += vec3(0.125); return d; }
vec3 read_data(Data d) { return d.color; }
'''
    first = combine(tree, [('color', 'VEC3', (0.125, 0.25, 0.375))])
    second = combine(tree, [('color', 'VEC3', (0.5, 0.625, 0.75))])
    join = tree.nodes.new('NodeJoinBundle')
    tree.links.new(first.outputs[0], join.inputs[0])
    tree.links.new(second.outputs[0], join.inputs[0])
    routed = join.outputs[0]
    for _ in range(4):
        reroute = tree.nodes.new('NodeReroute')
        tree.links.new(routed, reroute.inputs[0])
        routed = reroute.outputs[0]
    reader = function(tree, source, 'read_data')
    tree.links.new(routed, reader.inputs[0])
    tree.links.new(reader.outputs[0], emission.inputs['Color'])
    bundled = center(render('join_bundle_reference'))
    separate = tree.nodes.new('NodeSeparateBundle')
    separate.bundle_items.new('VECTOR', 'color')
    tree.links.new(routed, separate.inputs[0])
    tree.links.new(separate.outputs['color'], emission.inputs['Color'])
    pixel('join_priority_matches_manual_separation', bundled)
    # A static repeat carries an immutable Bundle between iterations; zero iterations
    # must forward the initial packet, and three iterations must call the function thrice.
    loop_out = tree.nodes.new('GeometryNodeRepeatOutput')
    loop_out.repeat_items.clear()
    loop_out.repeat_items.new('BUNDLE', 'Packet')
    loop_in = tree.nodes.new('GeometryNodeRepeatInput')
    loop_in.pair_with_output(loop_out)
    refresh(tree)
    step = function(tree, source, 'advance')
    tree.links.new(first.outputs[0], loop_in.inputs['Packet'])
    tree.links.new(loop_in.outputs['Packet'], step.inputs[0])
    tree.links.new(step.outputs[0], loop_out.inputs['Packet'])
    tree.links.new(loop_out.outputs['Packet'], reader.inputs[0])
    tree.links.new(reader.outputs[0], emission.inputs['Color'])
    loop_in.inputs['Iterations'].default_value = 0
    pixel('repeat_zero_bundle', (0.125, 0.25, 0.375))
    loop_in.inputs['Iterations'].default_value = 3
    pixel('repeat_three_bundles', (0.5, 0.625, 0.75))


def test_existing_sampler_and_callback_mix():
    _, material, emission = scene()
    tree = material.node_tree
    source = '''struct Data { vec2 uv; float gain; };
/* @glsl_closure v1
value: label="Value"
Result: label="Result"
*/
float remap(float value) { return value * 0.5; }
/* @glsl_meta v2
d.uv: default=vec2(0.5)
d.gain: default=0.5
amount: default=0.25
*/
vec3 sample_data(Data d, sampler2D image, float amount) {
  vec4 color = texture(image, d.uv);
  return vec3(color.r * d.gain, remap(color.g), color.a + amount);
}
'''
    node = function(tree, source, 'sample_data')
    node.inputs['amount'].default_value = 0.125
    image = bpy.data.images.new('BundleSampler', 1, 1, alpha=True, float_buffer=True)
    image.colorspace_settings.name = 'Non-Color'
    image.alpha_mode = 'CHANNEL_PACKED'
    image.pixels[:] = (0.5, 0.75, 0.0, 0.5)
    image.update()
    texture = tree.nodes.new('ShaderNodeImageToClosure')
    texture.image = image
    tree.links.new(texture.outputs[0], node.inputs['image'])
    tree.links.new(node.outputs[0], emission.inputs['Color'])
    pixel('bundle_with_sampler_and_callback_fallback', (0.25, 0.375, 0.625))
    output = tree.nodes.new('NodeClosureOutput')
    output.input_items.new('FLOAT', 'value').structure_type = 'SINGLE'
    output.output_items.new('FLOAT', 'Result').structure_type = 'SINGLE'
    inp = tree.nodes.new('NodeClosureInput')
    inp.pair_with_output(output)
    refresh(tree)
    multiply = tree.nodes.new('ShaderNodeMath')
    multiply.operation = 'MULTIPLY'
    multiply.inputs[1].default_value = 0.25
    tree.links.new(inp.outputs['value'], multiply.inputs[0])
    tree.links.new(multiply.outputs[0], output.inputs['Result'])
    callback = next(socket for socket in node.inputs if socket.identifier == 'closure.remap')
    tree.links.new(output.outputs['Closure'], callback)
    pixel('bundle_with_sampler_and_connected_callback', (0.25, 0.1875, 0.625))


def test_schema_edits_and_limits():
    _, material, emission = scene()
    tree = material.node_tree
    source = 'struct Data { float x; float y; }; vec3 f(Data d) { return vec3(d.x,d.y,0.75); }'
    node = function(tree, source, 'f')
    bundle = combine(tree, [('x', 'FLOAT', 0.25), ('y', 'FLOAT', 0.5)])
    tree.links.new(bundle.outputs[0], node.inputs[0])
    tree.links.new(node.outputs[0], emission.inputs['Color'])
    identifier = node.inputs[0].identifier
    before = [(item.name, bundle.inputs[item.name].identifier) for item in bundle.bundle_items]
    pixel('schema_before_reorder', (0.25, 0.5, 0.75))
    node.script.clear()
    node.script.write(source.replace('float x; float y;', 'float y; float x;'))
    node.function_name = ''
    node.function_name = 'f'
    refresh(tree)
    assert node.inputs[0].identifier == identifier and node.inputs[0].is_linked
    assert [(item.name, bundle.inputs[item.name].identifier) for item in bundle.bundle_items] == before
    pixel('schema_reorder_preserves_fields', (0.25, 0.5, 0.75))
    # New dimensions must not leak into Geometry Nodes' float3 execution path.
    geometry = bpy.data.node_groups.new('LegacyGeometryBundle', 'GeometryNodeTree')
    geo_bundle = geometry.nodes.new('NodeCombineBundle')
    item = geo_bundle.bundle_items.new('VECTOR', 'Value')
    item.vector_socket_dimensions = 4
    assert item.vector_socket_dimensions == 3
    assert len(geo_bundle.inputs['Value'].default_value) == 3
    large = 'struct Data {' + ''.join(f'float field{i};' for i in range(513)) + '}; float f(Data d) { return d.field0; }'
    node.script.clear()
    node.script.write(large)
    node.function_name = ''
    node.function_name = 'f'
    refresh(tree)
    assert node.parse_status == 'ERROR'
    assert node.inputs[0].identifier == identifier and node.inputs[0].is_linked
    definitions = ['struct Level0 { float value; };']
    for level in range(1, 24):
        definitions.append(f'struct Level{level} {{ Level{level-1} a; Level{level-1} b; }};')
    node.script.clear()
    node.script.write('\n'.join(definitions) + '\nfloat f(Level23 d) { return 0.0; }')
    started = time.monotonic()
    node.function_name = ''
    node.function_name = 'f'
    refresh(tree)
    assert node.parse_status == 'ERROR'
    assert time.monotonic() - started < 5.0, 'Exponential schema was not rejected promptly'


def test_bundle_inside_closure():
    _, material, emission = scene()
    tree = material.node_tree
    output = tree.nodes.new('NodeClosureOutput')
    output.input_items.new('BUNDLE', 'Packet').structure_type = 'SINGLE'
    output.output_items.new('FLOAT', 'Result').structure_type = 'SINGLE'
    inp = tree.nodes.new('NodeClosureInput')
    inp.pair_with_output(output)
    refresh(tree)
    node = function(tree, 'struct Data { float value; }; float f(Data d) { return d.value*2.0; }', 'f')
    tree.links.new(inp.outputs['Packet'], node.inputs[0])
    tree.links.new(node.outputs[0], output.inputs['Result'])
    results = []
    for value in (0.125, 0.25):
        packet = combine(tree, [('value','FLOAT',value)])
        evaluate = tree.nodes.new('NodeEvaluateClosure')
        evaluate.input_items.new('BUNDLE','Packet').structure_type = 'SINGLE'
        evaluate.output_items.new('FLOAT','Result').structure_type = 'SINGLE'
        refresh(tree)
        tree.links.new(output.outputs['Closure'], evaluate.inputs['Closure'])
        tree.links.new(packet.outputs[0], evaluate.inputs['Packet'])
        results.append(evaluate.outputs['Result'])
    add = tree.nodes.new('ShaderNodeMath')
    add.operation = 'ADD'
    tree.links.new(results[0], add.inputs[0])
    tree.links.new(results[1], add.inputs[1])
    tree.links.new(add.outputs[0], emission.inputs['Color'])
    pixel('bundle_closure_two_evaluations', (0.75,0.75,0.75))


def test_return_only_struct_namespace():
    _, material, emission = scene()
    tree = material.node_tree
    producer = function(tree, 'struct Data { float f; }; Data f() { return Data(0.25); }', 'f')
    consumer = function(tree, 'struct Data { float f; }; float read_data(Data d) { return d.f; }', 'read_data')
    tree.links.new(producer.outputs[0], consumer.inputs[0])
    tree.links.new(consumer.outputs[0], emission.inputs['Color'])
    pixel('return_only_member_matches_function_name', (0.25,0.25,0.25))


def test_return_and_multiple_out():
    _, material, emission = scene()
    tree = material.node_tree
    source = '''struct Data { float x; };
Data produce(out Data a, out Data b) { a=Data(0.25); b=Data(0.75); return Data(0.5); }
vec3 inspect(Data a, Data b, Data result) { return vec3(a.x,result.x,b.x); }
'''
    producer = function(tree, source, 'produce')
    consumer = function(tree, source, 'inspect')
    assert len(producer.outputs) == 3 and all(s.type == 'BUNDLE' for s in producer.outputs)
    tree.links.new(producer.outputs['a'], consumer.inputs['a'])
    tree.links.new(producer.outputs['b'], consumer.inputs['b'])
    tree.links.new(producer.outputs['Result'], consumer.inputs['result'])
    tree.links.new(consumer.outputs[0], emission.inputs['Color'])
    pixel('return_and_two_out_bundles', (0.25,0.5,0.75))


def test_output_schema_rename_and_sync():
    _, material, emission = scene()
    tree = material.node_tree
    source = 'struct Data { float old; float keep; }; Data f() { return Data(0.25,0.5); }'
    node = function(tree, source, 'f')
    separate = tree.nodes.new('NodeSeparateBundle')
    separate.bundle_items.new('FLOAT','old')
    separate.bundle_items.new('FLOAT','keep')
    tree.links.new(node.outputs[0], separate.inputs[0])
    tree.links.new(separate.outputs['keep'], emission.inputs['Color'])
    identifier = separate.outputs['old'].identifier
    node.script.clear()
    node.script.write(source.replace('float old;', 'float renamed;'))
    node.function_name = ''
    node.function_name = 'f'
    refresh(tree)
    pixel('unconnected_obsolete_output_is_harmless', (0.5,0.5,0.5))
    tree.links.new(separate.outputs['old'], emission.inputs['Color'])
    pixel('connected_obsolete_output_is_error', (1.0,0.0,1.0))
    window = bpy.context.window_manager.windows[0]
    area = max(window.screen.areas, key=lambda a: a.width*a.height)
    area.type = 'NODE_EDITOR'
    area.spaces.active.tree_type = 'ShaderNodeTree'
    area.spaces.active.shader_type = 'OBJECT'
    area.spaces.active.path.start(tree)
    region = next(r for r in area.regions if r.type == 'WINDOW')
    tree.nodes.active = separate
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.node.sockets_sync()
    assert separate.outputs['old'].identifier == identifier and separate.outputs['old'].is_linked
    assert 'renamed' in separate.outputs
    tree.links.new(separate.outputs['renamed'], emission.inputs['Color'])
    pixel('renamed_output_explicit_reconnection', (0.25,0.25,0.25))


def test_provided_fallback_and_wrong_aggregate_type():
    _, material, emission = scene()
    tree = material.node_tree
    node = function(tree, '''struct Data { float x; };
/* @glsl_meta v2
d.x: default=0.5
*/
float f(Data d) { return d.x; }
''', 'f')
    tree.links.new(node.outputs[0], emission.inputs['Color'])
    for kind in ('BUNDLE', 'CLOSURE'):
        packet = combine(tree, [('x',kind,None)])
        tree.links.new(packet.outputs[0], node.inputs[0])
        diagnosed_failure('provided_wrong_' + kind.lower(), node, "d.x")
        pixel('provided_wrong_' + kind.lower() + '_fails', (1.0,0.0,1.0))
    source = function(tree, 'float source_value() { return 0.875; }', 'source_value')
    source.mute = True
    packet = combine(tree, [('x','FLOAT',source.outputs[0])])
    tree.links.new(packet.outputs[0], node.inputs[0])
    pixel('provided_muted_fallback_is_zero', (0.0,0.0,0.0))
    tree.links.remove(node.inputs[0].links[0])
    pixel('actually_missing_still_defaults', (0.5,0.5,0.5))


def test_cycles_switch():
    if not bpy.app.build_options.cycles:
        assert '--require-cycles' not in sys.argv, 'This validation requires a Cycles build'
        print('GLSL_BUNDLE_CYCLES=UNAVAILABLE', flush=True)
        return
    sc, material, emission = scene()
    tree = material.node_tree
    node = function(tree, '''struct Data { vec4 color; };
/* @glsl_meta v2
d.color: default=vec4(0.25,0.5,0.0,0.75)
*/
vec3 f(Data d) { return vec3(d.color.xy,d.color.w); }
''', 'f')
    tree.links.new(node.outputs[0], emission.inputs['Color'])
    pixel('before_cycles_switch', (0.25,0.5,0.75))
    sc.render.engine = 'CYCLES'
    sc.cycles.device = 'CPU'
    sc.cycles.samples = 1
    sc.cycles.use_denoising = False
    # Cycles has no GLSL execution path. Exercise its fallback and tree lifetime,
    # then require the original Eevee values after returning to that engine.
    render('cycles_bundle_fallback')
    sc.render.engine = 'BLENDER_EEVEE'
    refresh(tree)
    assert node.parse_status == 'READY' and node.inputs[0].type == 'BUNDLE'
    pixel('after_cycles_switch', (0.25,0.5,0.75))
    print('GLSL_BUNDLE_CYCLES=PASS', flush=True)


def main():
    if '--reopen' in sys.argv:
        bpy.ops.wm.open_mainfile(filepath=sys.argv[sys.argv.index('--reopen') + 1])
        pixel('reopened_bundle', (0.25, 0.5, 0.625))
        print('GLSL_BUNDLE_REOPEN_OK', flush=True)
        return
    tests = [test_numeric_members, test_defaults_and_presence, test_return_out_and_matrices]
    if '--smoke' not in sys.argv:
        tests += [test_parse_rejections_preserve_interface, test_missing_invalid_and_recovery,
                  test_same_type_name_different_schema, test_schema_edits_and_limits,
                  test_routing_join_and_static_repeat, test_existing_sampler_and_callback_mix,
                  test_bundle_inside_closure,
                  test_return_only_struct_namespace,
                  test_return_and_multiple_out,
                  test_output_schema_rename_and_sync,
                  test_provided_fallback_and_wrong_aggregate_type,
                  test_world_filter_npr,
                  test_cycles_switch,
                  test_group_instances_and_reload]
    for test in tests:
        print('GLSL_BUNDLE_TEST', test.__name__, flush=True)
        test()
    (OUT / 'bundle_results.json').write_text(json.dumps(RESULTS, indent=2), encoding='utf-8')
    print('GLSL_BUNDLE_BACKEND=' + gpu.platform.backend_type_get(), flush=True)
    print('GLSL_FUNCTION_BUNDLE_OK', flush=True)


if __name__ == '__main__':
    main()
