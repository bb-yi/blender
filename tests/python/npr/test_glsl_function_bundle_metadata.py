# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path
import json
import os
import subprocess
import sys

import bpy

SOURCE = Path(__file__).resolve().parents[3]
ROOT = SOURCE.parent
sys.path.insert(0, str(SOURCE / 'tests/python/npr'))
sys.path.insert(0, str(SOURCE / 'tools/modules'))
import blendfile
from test_glsl_function_bundle import function, refresh
from material_compat_test_utils import OUT, scene

ASSETS = Path(os.environ.get('BLENDER_TEST_ASSET_DIR', str(OUT / 'assets')))
ASSETS.mkdir(parents=True, exist_ok=True)

BEFORE = '''struct Data { float required; float knob; };
/* @glsl_meta v2
d.knob: label="MetaBefore" description="Original description" default=0.5 min=0.0 max=1.0
*/
float meta_probe(Data d) { return d.required + d.knob; }
'''
AFTER = BEFORE.replace('MetaBefore', 'MetaAfter').replace('Original description', 'Refreshed description')
AFTER = AFTER.replace('min=0.0 max=1.0', 'min=0.125 max=0.875')

if bpy.app.background:
    _, material, _ = scene()
else:
    # A startup window must retain its current context; resetting the factory
    # file while executing its launch script invalidates context.object.
    window = bpy.context.window_manager.windows[0]
    sc = window.scene
    for obj in list(sc.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    mesh = bpy.data.meshes.new('MetadataPlane')
    mesh.from_pydata([(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)], [], [(0,1,2,3)])
    plane = bpy.data.objects.new('MetadataPlane', mesh)
    sc.collection.objects.link(plane)
    window.view_layer.objects.active = plane
    plane.select_set(True)
    material = bpy.data.materials.new('MetadataRefreshProbe')
    material.use_nodes = True
    plane.data.materials.append(material)
tree = material.node_tree
window = bpy.context.window_manager.windows[0]
area = max(window.screen.areas, key=lambda a: a.width*a.height)
area.type = 'NODE_EDITOR'
area.spaces.active.tree_type = 'ShaderNodeTree'
area.spaces.active.shader_type = 'OBJECT'
area.spaces.active.path.start(tree)
area.spaces.active.show_region_ui = False
area.spaces.active.show_region_toolbar = False
region = next(r for r in area.regions if r.type == 'WINDOW')


def operation(node, name, **kwargs):
    tree.nodes.active = node
    with bpy.context.temp_override(window=window, area=area, region=region):
        return getattr(bpy.ops.node, name)(**kwargs)


def add_packet(node):
    assert operation(node, 'glsl_bundle_input', socket_identifier=node.inputs[0].identifier,
                     member_path='d.knob') == {'FINISHED'}
    return node.inputs[0].links[0].from_node


def check_serialized_range(path):
    with blendfile.open_blend(str(path)) as saved:
        sockets = [block for block in saved.blocks if block.dna_type_name == 'bNodeSocket'
                   and block.get(b'label') == 'MetaAfter']
        assert sockets, 'Updated member label was not serialized'
        for socket in sockets:
            value = socket.get_pointer(b'default_value')
            assert value.dna_type_name == 'bNodeSocketValueFloat'
            assert value.get(b'min') == 0.125 and value.get(b'max') == 0.875


fresh_nodes = []
interface_nodes = []
records = []
for index, mode in enumerate(('INTERNAL', 'EXTERNAL')):
    path = ASSETS / ('metadata_' + mode.lower() + '.glsl')
    if mode == 'INTERNAL':
        node = function(tree, BEFORE, 'meta_probe')
    else:
        path.write_text(BEFORE, encoding='utf-8')
        node = tree.nodes.new('ShaderNodeGLSLFunction')
        node.source_mode = 'EXTERNAL'
        node.filepath = str(path)
        node.function_name = 'meta_probe'
        refresh(tree)
    assert node.parse_status == 'READY'
    packet = add_packet(node)
    assert packet.inputs['knob'].label == 'MetaBefore'
    assert packet.inputs['knob'].description == 'Original description'
    packet.inputs['knob'].default_value = 0.625
    shared = tree.nodes.new('ShaderNodeGLSLFunction')
    shared.source_mode = mode
    if mode == 'INTERNAL':
        shared.script = node.script
    else:
        shared.filepath = str(path)
    shared.function_name = 'meta_probe'
    refresh(tree)
    tree.links.new(packet.outputs[0], shared.inputs[0])
    value = tree.nodes.new('ShaderNodeValue')
    value.outputs[0].default_value = 0.25
    tree.links.new(value.outputs[0], packet.inputs['required'])
    identifiers = [socket.identifier for socket in packet.inputs]
    link = packet.inputs['required'].links[0].from_socket.as_pointer()
    if mode == 'INTERNAL':
        node.script.clear()
        node.script.write(AFTER)
    else:
        path.write_text(AFTER, encoding='utf-8')
    assert operation(node, 'glsl_function_refresh') == {'FINISHED'}
    node.display_mode = 'NODE'
    node.width = 450
    node.label = mode + ' refreshed interface'
    node.location = (index*600, 300)
    interface_nodes.append(node)
    assert node.parse_status == 'READY'
    assert [socket.identifier for socket in packet.inputs] == identifiers
    assert packet.inputs['knob'].default_value == 0.625
    assert packet.inputs['knob'].label == 'MetaBefore'
    assert packet.inputs['required'].links[0].from_socket.as_pointer() == link
    assert shared.inputs[0].is_linked and shared.inputs[0].links[0].from_node == packet
    assert len(packet.outputs[0].links) == 2
    # A fresh source packet must use the new UI metadata, without changing the old packet.
    fresh = tree.nodes.new('ShaderNodeGLSLFunction')
    fresh.source_mode = mode
    if mode == 'INTERNAL':
        fresh.script = node.script
    else:
        fresh.filepath = str(path)
    fresh.function_name = 'meta_probe'
    refresh(tree)
    fresh.display_mode = 'NODE'
    fresh.width = 450
    fresh.label = mode + ' metadata refresh'
    fresh.location = (index*600, 300)
    created = add_packet(fresh)
    assert created.inputs['knob'].label == 'MetaAfter'
    assert created.inputs['knob'].description == 'Refreshed description'
    assert created.inputs['knob'].default_value == 0.5
    saved_path = ASSETS / ('metadata_' + mode.lower() + '.blend')
    bpy.ops.wm.save_as_mainfile(filepath=str(saved_path), compress=False)
    check_serialized_range(saved_path)
    fresh_nodes.append(fresh)
    records.append({'mode': mode, 'new_label': created.inputs['knob'].label,
                    'range': [0.125,0.875], 'old_value': packet.inputs['knob'].default_value,
                    'source_link_preserved': True})

material_name = material.name
fresh_names = [node.name for node in fresh_nodes]
interface_names = [node.name for node in interface_nodes]
if not bpy.app.background:
    bpy.context.preferences.edit.use_global_undo = True
    target = tree.nodes.new('ShaderNodeGLSLFunction')
    target.source_mode = 'EXTERNAL'
    target.filepath = str(path)
    target.function_name = 'meta_probe'
    target.name = 'MetadataUndoTarget'
    refresh(tree)
    tree.nodes.active = target
    identifier = target.inputs[0].identifier
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.ed.undo_push(message='Before metadata override')
        bpy.ops.node.glsl_bundle_input('EXEC_DEFAULT', True,
                                      socket_identifier=identifier, member_path='d.knob')
    assert target.inputs[0].links[0].from_node.inputs['knob'].label == 'MetaAfter'
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.ed.undo()
    tree = bpy.data.materials[material_name].node_tree
    assert not tree.nodes['MetadataUndoTarget'].inputs[0].is_linked
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.ed.redo()
    tree = bpy.data.materials[material_name].node_tree
    assert tree.nodes['MetadataUndoTarget'].inputs[0].links[0].from_node.inputs['knob'].label == 'MetaAfter'
    records.append({'undo_redo':True})

old_packets = [(node.name, [socket.identifier for socket in node.inputs]) for node in tree.nodes
               if node.bl_idname == 'NodeCombineBundle' and 'knob' in node.inputs
               and node.inputs['knob'].default_value == 0.625]
saved_path = ASSETS / 'metadata_persistence.blend'
bpy.ops.wm.save_as_mainfile(filepath=str(saved_path), compress=False)
bpy.ops.wm.open_mainfile(filepath=str(saved_path), load_ui=False, use_scripts=False)
tree = bpy.data.materials[material_name].node_tree
fresh_nodes = [tree.nodes[name] for name in fresh_names]
interface_nodes = [tree.nodes[name] for name in interface_names]
for node in fresh_nodes:
    packet = node.inputs[0].links[0].from_node
    assert packet.inputs['knob'].label == 'MetaAfter'
    assert packet.inputs['knob'].description == 'Refreshed description'
    assert packet.inputs['knob'].default_value == 0.5
for name, identifiers in old_packets:
    packet = tree.nodes[name]
    assert [socket.identifier for socket in packet.inputs] == identifiers
    assert packet.inputs['knob'].default_value == 0.625
    assert packet.inputs['required'].is_linked
    assert len(packet.outputs[0].links) == 2
check_serialized_range(saved_path)
# Geometry Bundle declarations retain their original float3 behavior.
geometry = bpy.data.node_groups.new('MetadataGeometryCompatibility', 'GeometryNodeTree')
geo = geometry.nodes.new('NodeCombineBundle')
item = geo.bundle_items.new('VECTOR', 'value')
item.vector_socket_dimensions = 4
assert item.vector_socket_dimensions == 3 and len(geo.inputs['value'].default_value) == 3

(OUT / 'metadata_refresh.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
if bpy.app.background:
    print('GLSL_BUNDLE_METADATA_REFRESH_OK', flush=True)
else:
    bpy.context.preferences.view.show_splash = False
    window = bpy.context.window_manager.windows[0]
    area = max(window.screen.areas, key=lambda a: a.width*a.height)
    area.type = 'NODE_EDITOR'
    area.spaces.active.tree_type = 'ShaderNodeTree'
    area.spaces.active.shader_type = 'OBJECT'
    area.spaces.active.path.start(tree)
    region = next(r for r in area.regions if r.type == 'WINDOW')
    for node in tree.nodes:
        node.select = node in interface_nodes
    for node in interface_nodes:
        for panel in node.panel_states:
            panel.is_collapsed = False
    def frame_nodes():
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.node.view_selected()
        bpy.app.timers.register(capture, first_interval=0.5)
    def capture():
        bpy.ops.screen.screenshot(filepath=str(OUT / 'metadata_refresh.png'))
        print('GLSL_BUNDLE_METADATA_REFRESH_OK', flush=True)
        binary = Path(bpy.app.binary_path).resolve()
        assert 'release' not in [part.lower() for part in binary.parts]
        if '--auto-exit' in sys.argv:
            query = (f'Get-CimInstance Win32_Process -Filter "ProcessId = {os.getpid()}" | '
                     'Select-Object ProcessId,ExecutablePath,CommandLine | ConvertTo-Json')
            identity = json.loads(subprocess.check_output(['pwsh','-NoProfile','-Command',query], text=True))
            assert identity['ProcessId'] == os.getpid()
            assert Path(identity['ExecutablePath']).resolve() == binary
            assert Path(__file__).name in identity['CommandLine']
            bpy.ops.wm.quit_blender()
    bpy.app.timers.register(frame_nodes, first_interval=0.5)
