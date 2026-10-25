# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Automatic entry selection preserves explicit choices and the last valid interface.

Run with isolated Blender user resources and BLENDER_TEST_OUTPUT_DIR. Pass
--interfaces-only for CPU checks. The default also renders color/Shader links.
"""

import json
from pathlib import Path
import sys

import bpy
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import OUT, background_child, center, render, scene

RESULTS = []


def update(tree):
    tree.update_tag()
    tree.interface_update(bpy.context)
    bpy.context.view_layer.update()


def auto_node(tree, source):
    node = tree.nodes.new('ShaderNodeGLSLFunction')
    text = bpy.data.texts.new('AutoEntry.glsl')
    text.write(source)
    node.script = text
    update(tree)
    assert node.function_name == ''
    return node


def editor_context(tree):
    window = bpy.context.window_manager.windows[0]
    area = max(window.screen.areas, key=lambda item: item.width * item.height)
    area.type = 'NODE_EDITOR'
    area.spaces.active.tree_type = 'ShaderNodeTree'
    area.spaces.active.shader_type = 'OBJECT'
    area.spaces.active.path.start(tree)
    region = next(item for item in area.regions if item.type == 'WINDOW')
    return bpy.context.temp_override(window=window, area=area, region=region)


def test_interfaces():
    _, material, _ = scene()
    tree = material.node_tree
    node = auto_node(tree, 'vec3 only_entry(float x) { return vec3(x); }')
    assert node.parse_status == 'READY', node.error_message
    assert node.inputs[0].identifier == 'In_x'
    old_identifier = node.inputs[0].identifier
    node.inputs[0].default_value = 0.375
    assert len(node.panel_states) == 1 and not node.panel_states[0].is_collapsed
    panel_identifier = node.panel_states[0].identifier
    node.panel_states[0].is_collapsed = False
    node.display_mode = 'CODE'
    assert len(node.panel_states) == 1
    assert node.panel_states[0].identifier == panel_identifier
    assert not node.panel_states[0].is_collapsed
    node.display_mode = 'NODE'
    assert not node.panel_states[0].is_collapsed
    assert node.inputs[0].identifier == old_identifier and node.inputs[0].default_value == 0.375
    node.panel_states[0].is_collapsed = True
    node.display_mode = 'CODE'
    assert len(node.panel_states) == 1 and node.panel_states[0].is_collapsed
    assert node.panel_states[0].identifier == panel_identifier
    node.display_mode = 'NODE'
    assert node.panel_states[0].is_collapsed

    node.source_code = 'vec3 renamed(float x) { return vec3(x); }'
    assert node.parse_status == 'READY', node.error_message
    assert node.function_name == '' and node.inputs[0].identifier == old_identifier
    assert node.inputs[0].default_value == 0.375

    node.source_code = 'vec3 alpha(float x) { return vec3(x); } vec3 beta(float y) { return vec3(y); }'
    assert node.parse_status == 'ERROR'
    assert 'alpha' in node.error_message and 'beta' in node.error_message
    assert node.inputs[0].identifier == old_identifier and node.inputs[0].default_value == 0.375
    node.function_name = 'beta'
    assert node.parse_status == 'READY' and node.inputs[0].identifier == 'In_y'
    node.source_code = 'vec3 another(float y) { return vec3(y); }'
    assert node.parse_status == 'ERROR' and node.function_name == 'beta'
    assert 'selected function' in node.error_message
    node.function_name = ''
    assert node.parse_status == 'READY' and node.inputs[0].identifier == 'In_y'

    fixtures = [
        ('helper_inout', '''void adjust(inout float value) { value *= 0.5; }
vec3 entry(float x) { adjust(x); return vec3(x); }''', 'In_x'),
        ('helper_array', '''float first_value(float a[2]) { return a[0]; }
vec3 entry(float x) { float a[2]; a[0] = x; a[1] = x; return vec3(first_value(a)); }''', 'In_x'),
        ('unique_meta', '''float unused_helper(float x) { return x; }
/* @glsl_meta v2
gain: default=0.5
*/
vec3 entry(float gain) { return vec3(gain); }''', 'In_gain'),
        ('empty_meta', '''float unused_helper(float x) { return x; }
/* @glsl_meta v1
*/
vec3 entry(float x) { return vec3(x); }''', 'In_x'),
        ('prototype', '''float helper(float x);
float helper(float x) { return x; }
vec3 entry(float x) { return vec3(helper(x)); }''', 'In_x'),
        ('comment_calls', '''// fake(); void fake() { }
float helper(float x) { return x; }
vec3 entry(float x) { return vec3(helper(x)); }''', 'In_x'),
    ]
    for label, source, identifier in fixtures:
        fixture = auto_node(tree, source)
        assert fixture.parse_status == 'READY', (label, fixture.error_message)
        assert fixture.inputs[0].identifier == identifier
        RESULTS.append({'case': label, 'status': fixture.parse_status})

    both = auto_node(tree, '''/* @glsl_meta v1
x: default=0.25
*/
float a(float x) { return x; }
/* @glsl_meta v2
y: default=0.5
*/
float b(float y) { return y; }''')
    assert both.parse_status == 'ERROR' and 'candidates: a, b' in both.error_message
    both.function_name = 'a'
    assert both.parse_status == 'READY'

    shared = auto_node(tree, '''float helper(float x) { return x * 0.5; }
/* @glsl_meta v2
x: default=0.25
*/
vec3 entry(float x) { return vec3(helper(x)); }''')
    explicit = tree.nodes.new('ShaderNodeGLSLFunction')
    explicit.function_name = 'helper'
    explicit.script = shared.script
    assert explicit.parse_status == 'READY'
    shared.source_code = shared.source_code.replace('vec3 entry(', 'vec3 new_entry(')
    with editor_context(tree):
        tree.nodes.active = shared
        assert bpy.ops.node.glsl_function_refresh() == {'FINISHED'}
    assert shared.parse_status == explicit.parse_status == 'READY'
    assert shared.function_name == '' and explicit.function_name == 'helper'

    fresh = tree.nodes.new('ShaderNodeGLSLFunction')
    fresh.display_mode = 'CODE'
    assert fresh.function_name == '' and fresh.parse_status == 'READY'
    assert len(fresh.panel_states) == 1 and not fresh.panel_states[0].is_collapsed
    fresh.source_code = 'vec3 pasted_entry(float pasted) { return vec3(pasted); }'
    fresh.display_mode = 'NODE'
    assert fresh.function_name == '' and fresh.parse_status == 'READY'
    assert fresh.inputs[0].identifier == 'In_pasted'

    external_path = OUT / 'auto_entry_external.glsl'
    external_path.write_text('vec3 file_entry(float external) { return vec3(external); }', encoding='utf-8')
    external = tree.nodes.new('ShaderNodeGLSLFunction')
    external.source_mode = 'EXTERNAL'
    external.filepath = str(external_path)
    assert external.function_name == '' and external.parse_status == 'READY'
    external_path.write_text('vec3 file_renamed(float external) { return vec3(external); }', encoding='utf-8')
    with editor_context(tree):
        tree.nodes.active = external
        assert bpy.ops.node.glsl_function_refresh() == {'FINISHED'}
    assert external.function_name == '' and external.parse_status == 'READY'
    RESULTS.append({'case': 'ambiguity_manual_shared_paste_external', 'status': 'PASS'})


def test_direct_bundle_color():
    _, material, emission = scene()
    tree = material.node_tree
    output = next(item for item in tree.nodes if item.bl_idname == 'ShaderNodeOutputMaterial')
    node = auto_node(tree, '''struct Surface { vec4 diffuse_color; };
/* @glsl_meta v2
return.diffuse_color: subtype=color
*/
Surface make_surface() { return Surface(vec4(0.125, 0.25, 0.5, 0.75)); }''')
    assert node.parse_status == 'READY', node.error_message
    separate = tree.nodes.new('NodeSeparateBundle')
    separate.bundle_items.new('RGBA', 'color')
    tree.links.new(node.outputs[0], separate.inputs[0])
    tree.nodes.active = separate
    with editor_context(tree):
        bpy.ops.node.sockets_sync()
    # Sync preserves authored fields. A stale requested key remains an error.
    tree.links.new(separate.outputs['color'], output.inputs['Surface'])
    broken = center(render('auto_stale_color')).copy()
    assert np.max(np.abs(broken - (1.0, 0.0, 1.0))) <= 1e-5, broken
    tree.links.new(separate.outputs['diffuse_color'], output.inputs['Surface'])
    direct = center(render('auto_direct_color')).copy()
    assert np.max(np.abs(direct - (0.125, 0.25, 0.5))) <= 1e-5, direct
    tree.links.new(separate.outputs['diffuse_color'], emission.inputs['Color'])
    tree.links.new(emission.outputs[0], output.inputs['Surface'])
    emitted = center(render('auto_emitted_color')).copy()
    assert np.max(np.abs(emitted - direct)) <= 1e-5, (emitted, direct)
    RESULTS.append({'case': 'stale_and_direct_color', 'direct': direct.tolist(), 'max_error': 0.0})

    probe = auto_node(tree, '''struct Surface { vec4 diffuse_color; };
vec3 inspect_surface(Surface d) { return vec3(d.diffuse_color.a); }''')
    tree.links.new(node.outputs[0], probe.inputs[0])
    tree.links.new(probe.outputs[0], emission.inputs['Color'])
    node.panel_states[0].is_collapsed = True
    assert np.max(np.abs(center(render('auto_color_alpha')) - (0.75, 0.75, 0.75))) <= 1e-5
    material.name = 'AutoEntryRoundtrip'
    node.name = 'AutomaticEntry'
    path = OUT / 'auto_entry_roundtrip.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(path))
    child = background_child(__file__, ['--reopen', str(path)])
    assert 'GLSL_AUTO_ENTRY_REOPEN_OK' in child.splitlines(), child


def test_helper_execution():
    _, material, emission = scene()
    tree = material.node_tree
    fixtures = [
        ('helper_chain', '''float scale(float x) { return x * 0.5; }
float wrapped(float x) { return scale(x); }
vec3 entry(float x) { return vec3(wrapped(x)); }''', 0.25),
        ('helper_vector', '''vec3 expand(float x) { return vec3(x); }
vec3 entry(float x) { return expand(x); }''', 0.5),
        ('typed_callback', '''/* @glsl_closure v1
value: label="Value"
Result: label="Result"
*/
float remap(float value) { return value * 0.25; }
vec3 entry(float x) { return vec3(remap(x)); }''', 0.125),
    ]
    for label, source, expected in fixtures:
        node = auto_node(tree, source)
        assert node.parse_status == 'READY', (label, node.error_message)
        node.inputs['x'].default_value = 0.5
        tree.links.new(node.outputs[0], emission.inputs['Color'])
        actual = center(render('auto_' + label)).copy()
        error = float(np.max(np.abs(actual - (expected, expected, expected))))
        assert error <= 1e-5, (label, actual, expected)
        assert not node.error_message, (label, node.error_message)
        RESULTS.append({'case': label + '_gpu', 'max_error': error})


if '--reopen' in sys.argv:
    bpy.ops.wm.open_mainfile(filepath=sys.argv[sys.argv.index('--reopen') + 1], use_scripts=False)
    node = bpy.data.materials['AutoEntryRoundtrip'].node_tree.nodes['AutomaticEntry']
    assert node.function_name == '' and node.parse_status == 'READY'
    assert len(node.panel_states) == 1 and node.panel_states[0].is_collapsed
    assert np.max(np.abs(center(render('auto_reopened')) - (0.75, 0.75, 0.75))) <= 1e-5
    print('GLSL_AUTO_ENTRY_REOPEN_OK', flush=True)
else:
    test_interfaces()
    if '--interfaces-only' not in sys.argv:
        test_helper_execution()
        test_direct_bundle_color()
    (OUT / 'glsl_auto_entry_results.json').write_text(json.dumps(RESULTS, indent=2), encoding='utf-8')
    print('GLSL_AUTO_ENTRY_OK', flush=True)
