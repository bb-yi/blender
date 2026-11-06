# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Exercise small serialized legacy/corrupt fixtures without large allocations."""
from pathlib import Path
import json
import os
import struct
import sys

import bpy

SOURCE = Path(__file__).resolve().parents[3]
ROOT = SOURCE.parent
sys.path.insert(0, str(SOURCE / 'tests/python/npr'))
sys.path.insert(0, str(SOURCE / 'tools/modules'))
import blendfile
from test_glsl_function_bundle import combine, function
from material_compat_test_utils import OUT, scene

ASSETS = Path(os.environ.get('BLENDER_TEST_ASSET_DIR', str(OUT / 'assets')))
ASSETS.mkdir(parents=True, exist_ok=True)


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


def write_number(saved, block, name, value, format):
    offset, count = block.get_file_offset(name)
    assert count == 1
    saved.handle.seek(offset)
    saved.handle.write(struct.pack(saved.header.endian_str.decode('ascii') + format, value))


records = []
for scenario in ('next_identifier', 'duplicate_key', 'identifier_collision'):
    _, material, _ = scene()
    tree = material.node_tree
    packet = combine(tree, [('alpha','FLOAT',0.25), ('other','FLOAT',0.5)])
    packet.name = 'StorageGuardRoot'
    target = function(tree, 'struct Data { float alpha; float required; }; float f(Data d) { return d.alpha; }', 'f')
    tree.links.new(packet.outputs[0], target.inputs[0])
    path = ASSETS / ('bundle_storage_' + scenario + '.blend')
    bpy.ops.wm.save_as_mainfile(filepath=str(path), compress=False)
    with blendfile.open_blend(str(path), access='r+b') as saved:
        node = next(block for block in saved.blocks if block.dna_type_name == 'bNode'
                    and block.get(b'name') == 'StorageGuardRoot')
        storage = node.get_pointer(b'storage')
        if scenario == 'next_identifier':
            write_number(saved, storage, b'next_identifier', 2147483647, 'i')
        elif scenario == 'identifier_collision':
            write_number(saved, storage, b'next_identifier', 0, 'i')
        else:
            items = storage.get_pointer(b'items')
            name = items.get_pointer(b'name', base_index=1)
            assert name.size >= 6
            saved.handle.seek(name.file_offset)
            saved.handle.write(b'alpha\0')
    bpy.ops.wm.open_mainfile(filepath=str(path), load_ui=False, use_scripts=False)
    tree = bpy.data.materials['Probe'].node_tree
    packet = tree.nodes['StorageGuardRoot']
    before = ([item.name for item in packet.bundle_items],
              [socket.identifier for socket in packet.inputs],
              [socket.default_value for socket in packet.inputs if socket.type == 'VALUE'])
    okay, error = sync(tree, packet)
    after = ([item.name for item in packet.bundle_items],
             [socket.identifier for socket in packet.inputs],
             [socket.default_value for socket in packet.inputs if socket.type == 'VALUE'])
    assert not okay and before == after, (scenario, okay, error, before, after)
    records.append({'case':scenario, 'error':error, 'unchanged':before == after})

# The old one-byte padding may contain arbitrary bits. Version55 must always
# normalize it to float3; simulate that header on a tiny uncompressed fixture.
_, material, _ = scene()
tree = material.node_tree
packet = combine(tree, [('value','VEC4',(0.125,0.25,0.5,0.75))])
packet.name = 'LegacyVectorRoot'
path = ASSETS / 'bundle_legacy_subversion55.blend'
bpy.ops.wm.save_as_mainfile(filepath=str(path), compress=False)
with blendfile.open_blend(str(path), access='r+b') as saved:
    global_block = next(block for block in saved.blocks if block.dna_type_name == 'FileGlobal')
    assert global_block.get(b'subversion') >= 56
    write_number(saved, global_block, b'subversion', 55, 'h')
bpy.ops.wm.open_mainfile(filepath=str(path), load_ui=False, use_scripts=False)
packet = bpy.data.materials['Probe'].node_tree.nodes['LegacyVectorRoot']
assert packet.bundle_items[0].vector_socket_dimensions == 3
assert tuple(packet.inputs['value'].default_value) == (0.125,0.25,0.5)
records.append({'case':'synthetic_subversion55', 'dimensions':3,
                'xyz':list(packet.inputs['value'].default_value)})
(OUT / 'bundle_storage_boundaries.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
print('GLSL_BUNDLE_STORAGE_BOUNDARIES_OK', flush=True)
