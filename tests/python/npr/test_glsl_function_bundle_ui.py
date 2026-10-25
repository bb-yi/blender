# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Exercise Bundle creation, explicit overrides, Sync and real editor undo/diagnostics.

Foreground mode uses a rendered viewport and timers, writes an editor screenshot,
and only exits automatically with --auto-exit on a dedicated temp runtime.
"""

import json
import os
from pathlib import Path
import subprocess
import sys
import time
import traceback

import bpy

if not bpy.app.background:
    bpy.context.preferences.view.show_splash = False
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_glsl_function_bundle import OUT, function, refresh, scene

SOURCE = '''struct Inner { float required; vec4 color; };
struct Data { vec2 uv; Inner inner; float gain; mat4 matrix; };
/* @glsl_meta v2
d: label="Surface Data"
d.inner.color: default=vec4(0.125,0.25,0.5,0.75)
d.gain: default=0.5
d.matrix: default=mat4(1.0)
*/
Data pass_data(Data d) { return d; }
vec3 display_data(Data d) { return vec3(d.uv.y, d.inner.required, d.gain); }
'''

STATE = {'steps': [], 'started': time.monotonic(), 'stage': 0}


def optional_input_creation():
    """The UI example has no required members: its source must still have a Bundle output."""
    tree, target = objects()
    source = '''struct BundleTestInput { vec4 color; float gain; };
struct BundleTestOutput { vec4 color; float alpha; };
/* @glsl_meta v2
data: label="输入捆包"
data.color: subtype=color default=vec4(0.25, 0.5, 1.0, 0.75)
data.gain: label="亮度" default=0.5 min=0.0 max=2.0
return: label="输出捆包"
return.color: subtype=color
*/
BundleTestOutput bundle_test(BundleTestInput data) {
  return BundleTestOutput(vec4(data.color.rgb * data.gain, data.color.a), data.color.a);
}
'''
    frame = tree.nodes.new('NodeFrame')
    frame.name = 'OptionalBundleFrame'
    node = tree.nodes.new('ShaderNodeGLSLFunction')
    node.name = 'OptionalBundleTarget'
    node.parent = frame
    node.display_mode = 'CODE'
    node.source_code = source
    node.function_name = 'bundle_test'
    node.display_mode = 'NODE'
    assert node.parse_status == 'READY'
    assert len(node.inputs) == len(node.outputs) == 1
    assert len(node.panel_states) == 0  # A struct adds sockets, not a second interface panel.
    tree.nodes.active = node
    with context():
        if not bpy.app.background:
            bpy.ops.ed.undo_push(message='Before optional bundle creation')
        assert bpy.ops.node.glsl_bundle_input(
            'EXEC_DEFAULT', True, socket_identifier=node.inputs[0].identifier) == {'FINISHED'}
    root = node.inputs[0].links[0].from_node
    root.name = 'OptionalBundleSource'
    assert root.bl_idname == 'NodeCombineBundle'
    assert len(root.bundle_items) == 0
    assert len(root.outputs) == 1 and root.outputs[0].type == 'BUNDLE'
    assert root.parent == frame
    if not bpy.app.background:
        with context():
            bpy.ops.ed.undo()
        tree, target = objects()
        node = tree.nodes['OptionalBundleTarget']
        assert not node.inputs[0].is_linked
        assert tree.nodes.get('OptionalBundleSource') is None
        with context():
            bpy.ops.ed.redo()
        tree, target = objects()
        node = tree.nodes['OptionalBundleTarget']
        root = node.inputs[0].links[0].from_node
        frame = tree.nodes['OptionalBundleFrame']
        assert root.parent == frame and len(root.bundle_items) == 0
    node.display_mode = 'CODE'
    assert len(node.panel_states) == 1
    assert node.inputs[0].links[0].from_node == root
    node.display_mode = 'NODE'
    assert len(node.panel_states) == 0
    tree.nodes.active = root
    with context():
        if not bpy.app.background:
            bpy.ops.ed.undo_push(message='Before native optional Sync')
        assert bpy.ops.node.sockets_sync('EXEC_DEFAULT', True) == {'FINISHED'}
    assert [item.name for item in root.bundle_items] == ['color', 'gain']
    assert tuple(root.inputs['color'].default_value) == (0.25, 0.5, 1.0, 0.75)
    assert root.inputs['gain'].default_value == 0.5
    if not bpy.app.background:
        with context():
            bpy.ops.ed.undo()
        tree, target = objects()
        node = tree.nodes['OptionalBundleTarget']
        root = node.inputs[0].links[0].from_node
        assert len(root.bundle_items) == 0
        with context():
            bpy.ops.ed.redo()
        tree, target = objects()
        node = tree.nodes['OptionalBundleTarget']
        root = node.inputs[0].links[0].from_node
        frame = tree.nodes['OptionalBundleFrame']
        assert root.inputs['gain'].default_value == 0.5
    gain_identifier = root.inputs['gain'].identifier
    root.inputs['gain'].default_value = 0.25
    tree.nodes.active = root
    with context():
        bpy.ops.node.sockets_sync()
    assert root.inputs['gain'].identifier == gain_identifier
    assert root.inputs['gain'].default_value == 0.25
    node.source_code = source.replace('default=0.5 min=', 'default=0.75 min=')
    refresh_source(node)
    tree.nodes.active = root
    with context():
        bpy.ops.node.sockets_sync()
    assert root.inputs['gain'].identifier == gain_identifier
    assert root.inputs['gain'].default_value == 0.25
    tree.nodes.remove(root)
    tree.nodes.remove(node)
    tree.nodes.remove(frame)
    tree.nodes.active = target
    STATE['steps'].append('all_optional_create_native_sync_undo_and_compact_interface')


def native_nested_and_shared_sync():
    tree, target = objects()
    node = function(tree, '''struct Inner { vec2 uv; float expression; };
struct Data { Inner inner; mat2 matrix; };
/* @glsl_meta v2
d.inner.uv: default=vec2(0.25, 0.5)
d.inner.expression: default=sin(0.0)
d.matrix: default=mat2(1.0, 2.0, 3.0, 4.0)
*/
Data nested(Data d) { return d; }
''', 'nested')
    packet = tree.nodes.new('NodeCombineBundle')
    tree.links.new(packet.outputs[0], node.inputs[0])
    tree.nodes.active = packet
    with context():
        bpy.ops.node.sockets_sync()
    assert [item.name for item in packet.bundle_items] == ['inner', 'matrix']
    inner = packet.inputs['inner'].links[0].from_node
    matrix = packet.inputs['matrix'].links[0].from_node
    assert [item.name for item in inner.bundle_items] == ['uv']
    assert tuple(inner.inputs['uv'].default_value) == (0.25, 0.5)
    assert tuple(matrix.inputs['c0'].default_value) == (1.0, 2.0)
    assert tuple(matrix.inputs['c1'].default_value) == (3.0, 4.0)
    for candidate in [inner, matrix, packet, node]:
        tree.nodes.remove(candidate)

    shared = tree.nodes.new('NodeCombineBundle')
    consumers = []
    for index, default in enumerate(['0.25', '0.75']):
        consumer = function(tree, '''struct Data { float common; float different; float expression;
  mat2 matrix; float signed_zero; int large_integer; };
/* @glsl_meta v2
d.common: default=0.5
d.different: default=DEFAULT
d.expression: default=sin(0.0)
d.matrix: default=mat2(DEFAULT, 0.0, 0.0, 1.0)
d.signed_zero: default=SIGNED_ZERO
d.large_integer: default=16777217
*/
float consume(Data d) { return d.common + d.different + d.expression; }
'''.replace('DEFAULT', default).replace('SIGNED_ZERO', '-0.0' if index == 0 else '0.0'), 'consume')
        consumer.name = 'OptionalShared' + str(index)
        tree.links.new(shared.outputs[0], consumer.inputs[0])
        consumers.append(consumer)
    tree.nodes.active = shared
    with context():
        bpy.ops.node.sockets_sync()
    assert [item.name for item in shared.bundle_items] == ['common']
    assert shared.inputs['common'].default_value == 0.5
    for candidate in consumers + [shared]:
        tree.nodes.remove(candidate)
    tree.nodes.active = target
    STATE['steps'].append('native_nested_matrix_sync_and_shared_defaults')


def editor_scene():
    """Build data directly: resetting the startup file inside a UI timer invalidates its context."""
    window = bpy.context.window_manager.windows[0]
    sc = window.scene
    for obj in list(sc.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    sc.render.engine = 'BLENDER_EEVEE'
    sc.render.resolution_x = sc.render.resolution_y = 64
    sc.render.resolution_percentage = 100
    sc.eevee.taa_render_samples = 1
    sc.view_settings.view_transform = 'Standard'
    camera_data = bpy.data.cameras.new('BundleUICamera')
    camera_data.type = 'ORTHO'
    camera_data.ortho_scale = 2
    camera = bpy.data.objects.new('BundleUICamera', camera_data)
    camera.location = (0, 0, 3)
    sc.collection.objects.link(camera)
    sc.camera = camera
    mesh = bpy.data.meshes.new('BundleUIPlane')
    mesh.from_pydata([(-2,-2,0),(2,-2,0),(2,2,0),(-2,2,0)], [], [(0,1,2,3)])
    mesh.update()
    plane = bpy.data.objects.new('BundleUIPlane', mesh)
    sc.collection.objects.link(plane)
    window.view_layer.objects.active = plane
    plane.select_set(True)
    material = bpy.data.materials.new('BundleUITest')
    material.use_nodes = True
    plane.data.materials.append(material)
    tree = material.node_tree
    tree.nodes.clear()
    output = tree.nodes.new('ShaderNodeOutputMaterial')
    emission = tree.nodes.new('ShaderNodeEmission')
    tree.links.new(emission.outputs[0], output.inputs['Surface'])
    return sc, material, emission


def setup():
    _, material, emission = editor_scene()
    material.name = 'BundleUITest'
    tree = material.node_tree
    node = function(tree, SOURCE, 'pass_data')
    node.name = 'BundleUITarget'
    node.display_mode = 'NODE'
    node.location = (0, 180)
    node.width = 320
    display = function(tree, SOURCE, 'display_data')
    display.name = 'BundleUIDisplay'
    display.location = (300, 180)
    tree.links.new(node.outputs[0], display.inputs[0])
    tree.links.new(display.outputs[0], emission.inputs['Color'])
    emission.location = (600, 180)
    tree.nodes.get('Material Output').location = (820, 180)
    window = bpy.context.window_manager.windows[0]
    area = max(window.screen.areas, key=lambda area: area.width * area.height)
    if not bpy.app.background:
        with bpy.context.temp_override(window=window, area=area):
            bpy.ops.screen.area_split(direction='VERTICAL', factor=0.6)
        areas = [area for area in window.screen.areas if area.type == 'VIEW_3D']
        area = min(areas, key=lambda area: area.x)
        viewport = next(candidate for candidate in areas if candidate != area)
        viewport.spaces.active.shading.type = 'RENDERED'
        viewport.spaces.active.region_3d.view_perspective = 'CAMERA'
    area.type = 'NODE_EDITOR'
    area.spaces.active.tree_type = 'ShaderNodeTree'
    area.spaces.active.shader_type = 'OBJECT'
    area.spaces.active.path.start(tree)
    area.spaces.active.show_region_ui = False
    area.spaces.active.show_region_toolbar = False
    STATE['area'] = area
    STATE['window'] = window
    tree.nodes.active = node
    if not bpy.app.background:
        bpy.context.preferences.edit.use_global_undo = True
        with context():
            bpy.ops.ed.undo_push(message='Bundle UI baseline')
    STATE['steps'].append('setup')


def objects():
    tree = bpy.data.materials['BundleUITest'].node_tree
    return tree, tree.nodes['BundleUITarget']


def refresh_source(node):
    selected = node.function_name
    node.function_name = ''
    node.function_name = selected
    refresh(node.id_data)


def context():
    area = STATE['area']
    region = next(region for region in area.regions if region.type == 'WINDOW')
    return bpy.context.temp_override(window=STATE['window'], area=area, region=region)


def create_and_override():
    tree, node = objects()
    tree.nodes.active = node
    with context():
        assert bpy.ops.node.glsl_bundle_input(socket_identifier=node.inputs[0].identifier) == {'FINISHED'}
    root = node.inputs[0].links[0].from_node
    root.name = 'BundleUIRoot'
    assert [item.name for item in root.bundle_items] == ['uv', 'inner']
    assert len(root.inputs['uv'].default_value) == 2
    root.inputs['uv'].default_value = (0.125, 0.25)
    inner = root.inputs['inner'].links[0].from_node
    inner.name = 'BundleUIInner'
    assert [item.name for item in inner.bundle_items] == ['required']
    inner.inputs['required'].default_value = 0.375
    with context():
        bpy.ops.node.glsl_bundle_input(socket_identifier=node.inputs[0].identifier, member_path='d.gain')
    assert root.inputs['gain'].default_value == 0.5
    with context():
        bpy.ops.node.glsl_bundle_input(socket_identifier=node.inputs[0].identifier, member_path='d.inner.color')
    assert tuple(inner.inputs['color'].default_value) == (0.125, 0.25, 0.5, 0.75)
    with context():
        bpy.ops.node.glsl_bundle_input(socket_identifier=node.inputs[0].identifier, member_path='d.matrix')
    matrix = root.inputs['matrix'].links[0].from_node
    for column in range(4):
        assert tuple(matrix.inputs['c' + str(column)].default_value) == tuple(float(row == column) for row in range(4))
    assert tuple(root.inputs['uv'].default_value) == (0.125, 0.25)
    STATE['steps'].append('create_and_overrides')


def synchronize():
    tree, node = objects()
    root = tree.nodes['BundleUIRoot']
    before = [(item.name, root.inputs[item.name].identifier) for item in root.bundle_items]
    values = tuple(root.inputs['uv'].default_value)
    tree.nodes.active = root
    with context():
        bpy.ops.node.sockets_sync()
    assert [(item.name, root.inputs[item.name].identifier) for item in root.bundle_items] == before
    assert tuple(root.inputs['uv'].default_value) == values
    sep = tree.nodes.new('NodeSeparateBundle')
    sep.name = 'BundleUISeparate'
    sep.location = (300, -220)
    tree.links.new(node.outputs[0], sep.inputs[0])
    tree.nodes.active = sep
    with context():
        bpy.ops.node.sockets_sync()
    assert [item.name for item in sep.bundle_items] == ['uv', 'inner', 'gain', 'matrix']
    assert len(sep.outputs['uv'].default_value) == 2
    nested = tree.nodes.new('NodeSeparateBundle')
    nested.location = (570, -220)
    tree.links.new(sep.outputs['inner'], nested.inputs[0])
    tree.nodes.active = nested
    with context():
        bpy.ops.node.sockets_sync()
    assert [item.name for item in nested.bundle_items] == ['required', 'color']
    assert len(nested.outputs['color'].default_value) == 4
    tree.nodes.active = node
    STATE['steps'].append('preserving_sync_and_nested_sync')


def shared_signature_checks():
    tree, target = objects()
    a = function(tree, '''struct ColorData { vec3 color; };
/* @glsl_meta v2
d.color: subtype=color
*/
vec3 a(ColorData d) { return d.color; }
''', 'a')
    a.name = 'ColorConsumer'
    b = function(tree, 'struct VectorData { vec3 color; }; vec3 b(VectorData d) { return d.color; }', 'b')
    b.name = 'VectorConsumer'
    packet = tree.nodes.new('NodeCombineBundle')
    packet.bundle_items.new('VECTOR', 'color')
    refresh(tree)
    packet.inputs['color'].default_value = (0.125,0.25,0.5)
    tree.links.new(packet.outputs[0], a.inputs[0])
    tree.links.new(packet.outputs[0], b.inputs[0])
    identifier = packet.inputs['color'].identifier
    tree.nodes.active = packet
    with context():
        bpy.ops.node.sockets_sync()
    assert packet.inputs['color'].identifier == identifier
    assert tuple(packet.inputs['color'].default_value) == (0.125,0.25,0.5)
    b.script.clear()
    b.script.write('struct VectorData { vec4 color; }; vec3 b(VectorData d) { return d.color.rgb; }')
    refresh_source(b)
    tree.nodes.active = packet
    try:
        with context():
            bpy.ops.node.sockets_sync()
    except RuntimeError as exc:
        message = str(exc)
        assert 'color' in message and 'ColorConsumer' in message and 'VectorConsumer' in message, message
    else:
        raise AssertionError('Conflicting consumers did not report the field and consumer names')
    assert packet.inputs['color'].identifier == identifier
    assert tuple(packet.inputs['color'].default_value) == (0.125,0.25,0.5)
    tree.nodes.remove(a)
    tree.nodes.remove(b)
    tree.nodes.remove(packet)
    tree.nodes.active = target
    STATE['steps'].append('shared_color_alias_and_conflict_atomicity')


def undo_roundtrip():
    tree, node = objects()
    with context():
        bpy.ops.ed.undo_push(message='Before bundle undo probe')
    # The existing packet is used to test that an operator adds one optional field
    # in one undo step and never rewrites existing values.
    source = SOURCE.replace('float gain; mat4 matrix;', 'float gain; mat4 matrix; float optional;')
    source = source.replace('d.gain: default=0.5', 'd.optional: default=0.625\nd.gain: default=0.5')
    node.script.clear()
    node.script.write(source)
    refresh_source(node)
    with context():
        bpy.ops.ed.undo_push(message='Bundle schema ready')
        bpy.ops.node.glsl_bundle_input('EXEC_DEFAULT', True,
                                      socket_identifier=node.inputs[0].identifier, member_path='d.optional')
    assert tree.nodes['BundleUIRoot'].inputs['optional'].default_value == 0.625
    with context():
        bpy.ops.ed.undo()
    tree, node = objects()
    assert 'optional' not in tree.nodes['BundleUIRoot'].inputs
    assert tuple(tree.nodes['BundleUIRoot'].inputs['uv'].default_value) == (0.125, 0.25)
    with context():
        bpy.ops.ed.redo()
    tree, node = objects()
    assert tree.nodes['BundleUIRoot'].inputs['optional'].default_value == 0.625
    STATE['steps'].append('undo_redo')


def finish(success, error=''):
    STATE['success'] = success
    STATE['error'] = error
    report = {key: value for key, value in STATE.items() if key not in {'area', 'window'}}
    (OUT / 'bundle_ui_results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    if success:
        print('GLSL_FUNCTION_BUNDLE_UI_OK', flush=True)
    if '--auto-exit' in sys.argv:
        binary = Path(bpy.app.binary_path).resolve()
        assert 'release' not in [part.lower() for part in binary.parts]
        assert binary.parent.name == 'runtime' and binary.parent.parent.name == 'glsl_function_bundle'
        query = (f'Get-CimInstance Win32_Process -Filter "ProcessId = {os.getpid()}" | '
                 'Select-Object ProcessId,ExecutablePath,CommandLine | ConvertTo-Json')
        identity = json.loads(subprocess.check_output(['pwsh', '-NoProfile', '-Command', query], text=True))
        assert Path(identity['ExecutablePath']).resolve() == binary
        assert Path(__file__).name in identity['CommandLine']
        print('OWNED_UI_TEST_PROCESS', json.dumps(identity), flush=True)
        bpy.ops.wm.quit_blender()


def tick():
    try:
        stage = STATE['stage']
        if stage == 0:
            setup()
        elif stage == 1:
            optional_input_creation()
            native_nested_and_shared_sync()
            create_and_override()
        elif stage == 2:
            synchronize()
            shared_signature_checks()
        elif stage == 3:
            undo_roundtrip()
        elif stage == 4:
            tree, node = objects()
            with context():
                bpy.ops.node.view_all()
            root = tree.nodes['BundleUIRoot']
            STATE['link_root'] = root.name
            tree.links.remove(node.inputs[0].links[0])
            STATE['deadline'] = time.monotonic() + 15
        elif stage == 5:
            tree, node = objects()
            if "Missing required bundle field" not in node.error_message:
                assert time.monotonic() < STATE['deadline'], ('missing diagnostic', node.error_message)
                return 0.15
            STATE['diagnostic'] = node.error_message
            tree.links.new(tree.nodes[STATE['link_root']].outputs[0], node.inputs[0])
            STATE['deadline'] = time.monotonic() + 15
        elif stage == 6:
            tree, node = objects()
            if node.error_message:
                assert time.monotonic() < STATE['deadline'], ('stale diagnostic', node.error_message)
                return 0.15
            STATE['steps'].append('viewport_error_recovery')
        elif stage == 7:
            tree, node = objects()
            STATE['good_source'] = node.script.as_string()
            root = tree.nodes['BundleUIRoot']
            root.bundle_items.remove(next(item for item in root.bundle_items if item.name == 'gain'))
            node.script.clear()
            node.script.write(STATE['good_source'].replace('d.gain: default=0.5',
                                                          'd.gain: default=(bundle_default_missing)'))
            refresh_source(node)
            STATE['deadline'] = time.monotonic() + 15
        elif stage == 8:
            tree, node = objects()
            if "Bundle default 'd.gain'" not in node.error_message or 'bundle_default_missing' not in node.error_message:
                assert time.monotonic() < STATE['deadline'], ('default diagnostic', node.error_message)
                return 0.15
            STATE['default_diagnostic'] = node.error_message
            node.script.clear()
            node.script.write(STATE['good_source'].replace('d.gain: default=0.5',
                                                          'd.gain: default=(bundle_default_in_flight)'))
            refresh_source(node)
        elif stage == 9:
            tree, node = objects()
            node.script.clear()
            node.script.write(STATE['good_source'])
            refresh_source(node)
            STATE['deadline'] = time.monotonic() + 15
        elif stage == 10:
            tree, node = objects()
            if node.error_message:
                assert time.monotonic() < STATE['deadline'], ('stale default diagnostic', node.error_message)
                return 0.15
            STATE['steps'].append('default_expression_error_and_rapid_recovery')
            STATE['settled'] = 0
        elif stage == 11:
            tree, node = objects()
            assert node.error_message == '', node.error_message
            STATE['settled'] += 1
            if STATE['settled'] < 8:
                return 0.15
            for candidate in tree.nodes:
                candidate.select = candidate == node
            for panel in node.panel_states:
                panel.is_collapsed = False
            with context():
                bpy.ops.node.view_selected()
        elif stage == 12:
            bpy.ops.screen.screenshot(filepath=str(OUT / 'bundle_editor.png'))
            finish(True)
            return None
        STATE['stage'] += 1
        return 0.5
    except Exception:
        error = traceback.format_exc()
        print(error, flush=True)
        finish(False, error)
        return None


if bpy.app.background:
    setup()
    optional_input_creation()
    native_nested_and_shared_sync()
    create_and_override()
    synchronize()
    shared_signature_checks()
    finish(True)
else:
    bpy.app.timers.register(tick, first_interval=0.5)
