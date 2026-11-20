# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""NPR topology, ID ownership and file-versioning regressions.

Run each case in a separate factory-startup Blender process. Portal cases require
external process memory and time limits; see node_ownership_and_versioning.md.
"""
import argparse
import contextlib
import os
from pathlib import Path
import struct
import sys
import bpy

sys.dont_write_bytecode = True

BASE = Path(__file__).resolve().parent
REPO = BASE.parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('case', choices=('scene_copy', 'mask', 'portal', 'versioning', 'mask_render'))
parser.add_argument('--output-dir', type=Path, default=os.environ.get('NPR_REVIEW_OUTPUT_DIR'))
args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
if args.output_dir is None:
    parser.error('--output-dir is required')
OUT = args.output_dir / 'out'
ASSETS = args.output_dir / 'assets'
OUT.mkdir(parents=True, exist_ok=True)
ASSETS.mkdir(parents=True, exist_ok=True)
MODE = args.case


def check(value, message):
    if not value:
        raise AssertionError(message)


def scene_copy():
    scene = bpy.context.scene
    mat = bpy.data.materials.new("ReviewFilter")
    mat.eevee_domain = 'FILTER'
    for _ in range(2):
        scene.eevee.filter_materials.add().material = mat
    scene.eevee.filter_materials.add()
    graph = bpy.data.node_groups.new("ReviewGraph", "EeveeFilterGraphNodeTree")
    scene.eevee.filter_graph = graph
    before = mat.users, graph.users
    bpy.ops.scene.new(type='EMPTY')
    copied = bpy.context.scene
    check(copied != scene, "EMPTY must create a scene")
    check(len(copied.eevee.filter_materials) == 3, "all filter entries copied")
    check(copied.eevee.filter_graph == graph, "graph remains shared")
    check((mat.users, graph.users) == (before[0] + 2, before[1] + 1),
          f"EMPTY counts: {before} -> {(mat.users, graph.users)}")
    bpy.context.window.scene = scene
    bpy.data.scenes.remove(copied)
    check((mat.users, graph.users) == before, "EMPTY deletion restores counts")
    copied = scene.copy()
    check((mat.users, graph.users) == (before[0] + 2, before[1] + 1), "ordinary ID copy counts once")
    bpy.data.scenes.remove(copied)
    check((mat.users, graph.users) == before, "ordinary scene deletion restores counts")
    print("COUNTS", before, flush=True)


@contextlib.contextmanager
def editor(tree):
    window = bpy.context.window or next(iter(bpy.context.window_manager.windows))
    area = window.screen.areas[0]
    old_type = area.type
    area.type = 'NODE_EDITOR'
    area.spaces.active.tree_type = 'ShaderNodeTree'
    area.spaces.active.pin = True
    area.spaces.active.node_tree = tree
    region = next(r for r in area.regions if r.type == 'WINDOW')
    try:
        with bpy.context.temp_override(window=window, area=area, region=region):
            yield
    finally:
        area.type = old_type


def mask():
    bpy.ops.mesh.primitive_cube_add()
    obj = bpy.context.object
    obj.name = "ReviewMaskObject"
    bpy.ops.mesh.primitive_cube_add(location=(3, 0, 0))
    other = bpy.context.object
    other.name = "ReviewOtherObject"
    base_obj, base_other = obj.users, other.users
    mat = bpy.data.materials.new("ReviewMaskMaterial")
    mat.use_nodes = True
    obj.data.materials.append(mat)
    tree = mat.node_tree
    node = tree.nodes.new('ShaderNodeFilterObjectMask')
    node.name = "ReviewMask"
    node.mode = 'OBJECT_LIST'
    for n in tree.nodes:
        n.select = (n == node)
    tree.nodes.active = node
    with editor(tree):
        check(bpy.ops.node.filter_mask_item_add() == {'FINISHED'}, "add item")
        node.object_items[0].object = obj
        check(obj.users == base_obj + 1, "assignment adds one user")
        node.object_items[0].object = other
        check((obj.users, other.users) == (base_obj, base_other + 1), "replacement balances users")
        node.object_items[0].object = obj
        node.object_items[0].object = obj
        check(obj.users == base_obj + 1, "same object assignment is balanced")
        check(bpy.ops.node.duplicate() == {'FINISHED'}, "standalone node duplicate")
        check(obj.users == base_obj + 2, "standalone copy adds exactly one user")
        duplicate = tree.nodes.active
        check(duplicate != node, "duplicate is active")
        tree.nodes.remove(duplicate)
        check(obj.users == base_obj + 1, "node removal balances standalone copy")
        tree.nodes.active = node
        node.select = True
        before_nodes = set(n.name for n in tree.nodes)
        check(bpy.ops.node.clipboard_copy() == {'FINISHED'}, 'clipboard copy')
        check(obj.users == base_obj + 1, 'clipboard snapshot has no users')
        check(bpy.ops.node.clipboard_paste() == {'FINISHED'}, 'clipboard paste')
        pasted = [n for n in tree.nodes if n.name not in before_nodes]
        check(len(pasted) == 1 and obj.users == base_obj + 2, 'clipboard paste adds once')
        check(pasted[0].bl_idname == 'ShaderNodeFilterObjectMask', 'clipboard node type')
        check(len(pasted[0].object_items) == 1 and pasted[0].object_items[0].object == obj,
              'clipboard preserves referenced object')
        tree.nodes.remove(pasted[0])
        check(obj.users == base_obj + 1, 'clipboard node removal restores users')
        tree.nodes.active = node
        node.select = True
        for target in (obj, other):
            check(bpy.ops.node.filter_mask_item_add() == {'FINISHED'}, 'append list item')
            node.object_items[node.active_index].object = target
        check((obj.users, other.users) == (base_obj + 2, base_other + 1), 'multiple item users')
        check(bpy.ops.node.filter_mask_item_move(direction='UP') == {'FINISHED'}, 'move middle item')
        check(node.active_index == 1, 'move selects the middle item')
        check([item.object for item in node.object_items] == [obj, other, obj],
              'move preserves list order and objects')
        check((obj.users, other.users) == (base_obj + 2, base_other + 1), 'move transfers ownership')
        check(bpy.ops.node.filter_mask_item_remove() == {'FINISHED'}, 'remove middle item')
        check((obj.users, other.users) == (base_obj + 2, base_other), 'middle removal releases once')
        check([item.object for item in node.object_items] == [obj, obj], 'middle removal list order')
        node.active_index = 1
        check(bpy.ops.node.filter_mask_item_remove() == {'FINISHED'}, 'remove duplicate reference')
        check(obj.users == base_obj + 1, 'remaining list owns one user')
        copied_mat = mat.copy()
        check(obj.users == base_obj + 2, "embedded tree copy adds once")
        bpy.data.materials.remove(copied_mat)
        check(obj.users == base_obj + 1, "embedded tree free balances copy")
        for i in range(12):
            mat.diffuse_color = (0.1 + i * 0.01, 0.2, 0.3, 1)
            bpy.context.view_layer.update()
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            check(evaluated.data.materials[0] is not None, "evaluated material is present")
            check(obj.users == base_obj + 1, "depsgraph copies do not grow users")
        check(bpy.ops.node.filter_mask_item_remove() == {'FINISHED'}, "remove item")
        check(obj.users == base_obj, "item removal decrements user")
        bpy.ops.object.select_all(action='DESELECT')
        obj.select_set(True)
        check(bpy.ops.node.filter_mask_items_from_selection(replace=True) == {'FINISHED'}, "selection replace")
        check(obj.users == base_obj + 1, "selection owns user")
        check(bpy.ops.node.filter_mask_items_from_selection(replace=True) == {'FINISHED'}, "repeat replace")
        check(obj.users == base_obj + 1, "repeat replacement is balanced")
        bpy.ops.object.select_all(action='DESELECT')
        check(bpy.ops.node.filter_mask_items_from_selection(replace=True) == {'FINISHED'}, "empty selection clears")
        check(obj.users == base_obj and len(node.object_items) == 0, "clear restores baseline")
        bpy.ops.node.filter_mask_item_add()
        node.object_items[0].object = obj
    group = bpy.data.node_groups.new('ReviewMaskGroup', 'ShaderNodeTree')
    group_node = group.nodes.new('ShaderNodeFilterObjectMask')
    group_node.mode = 'OBJECT_LIST'
    group.nodes.active = group_node
    with editor(group):
        check(bpy.ops.node.filter_mask_item_add() == {'FINISHED'}, 'group list item')
        group_node.object_items[0].object = obj
    check(obj.users == base_obj + 2, 'standalone group owns one user')
    copied_group = group.copy()
    check(obj.users == base_obj + 3, 'standalone group copy adds once')
    bpy.data.node_groups.remove(copied_group)
    check(obj.users == base_obj + 2, 'copied group free releases one user')
    bpy.data.node_groups.remove(group)
    check(obj.users == base_obj + 1, 'standalone group frees balance users')
    path = ASSETS / 'mask_roundtrip.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(path), compress=False)
    expected_users = obj.users
    bpy.ops.wm.open_mainfile(filepath=str(path), load_ui=False)
    obj = bpy.data.objects['ReviewMaskObject']
    tree = bpy.data.materials['ReviewMaskMaterial'].node_tree
    node = tree.nodes['ReviewMask']
    check(obj.users == expected_users, "saved user counts are reconstructed consistently")
    bpy.data.objects.remove(obj, do_unlink=True)
    check(node.object_items[0].object is None, "deleting referenced object remaps the item")
    tree.nodes.remove(node)


def make_portal(tree, name):
    pin = tree.nodes.new('ShaderNodePortalIn')
    pout = tree.nodes.new('ShaderNodePortalOut')
    pin.portal_name = pout.portal_name = name
    pin.data_type = pout.data_type = 'FLOAT'
    return pin, pout


def first_enabled(sockets):
    return next(s for s in sockets if s.enabled)


def render_portal_center(label):
    bpy.ops.render.render(write_still=False)
    path = OUT / f'portal_{label}.exr'
    bpy.data.images['Render Result'].save_render(str(path))
    image = bpy.data.images.load(str(path), check_existing=False)
    rgb = tuple(image.pixels[(16 * 32 + 16) * 4:(16 * 32 + 16) * 4 + 3])
    bpy.data.images.remove(image)
    check(len(rgb) == 3, 'rendered center pixel exists')
    return rgb


def check_portal_value(label, expected):
    rgb = render_portal_center(label)
    check(all(abs(c - expected) < 0.03 for c in rgb), f'portal cache {label}: {rgb}')
    print('PORTAL_CACHE', label, 'rgb', rgb, flush=True)


def portal():
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    scene.render.resolution_x = scene.render.resolution_y = 32
    scene.render.resolution_percentage = 100
    scene.eevee.taa_render_samples = 1
    scene.camera.location = (0, 0, 6)
    scene.camera.rotation_euler = (0, 0, 0)
    scene.camera.data.type = 'ORTHO'
    scene.camera.data.ortho_scale = 4
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.render.image_settings.color_depth = '32'
    obj = bpy.data.objects['Cube']
    cases = (
        'acyclic', 'cycle', 'two_portals', 'duplicate_name', 'unmatched',
        'type_mismatch', 'reroute_cycle', 'muted_math_cycle', 'acyclic_fanout',
        'muted_in_cycle', 'muted_out_cycle', 'empty_name', 'cache_refresh',
    )
    for kind in cases:
        mat = bpy.data.materials.new('ReviewPortal_' + kind)
        mat.use_nodes = True
        obj.data.materials.clear()
        obj.data.materials.append(mat)
        tree = mat.node_tree
        tree.nodes.clear()
        pin, pout = make_portal(tree, 'A')
        math = tree.nodes.new('ShaderNodeMath')
        math.inputs[1].default_value = 0
        emission = tree.nodes.new('ShaderNodeEmission')
        output = tree.nodes.new('ShaderNodeOutputMaterial')
        tree.links.new(math.outputs[0], emission.inputs['Strength'])
        tree.links.new(emission.outputs[0], output.inputs['Surface'])
        tree.links.new(first_enabled(pout.outputs), math.inputs[0])
        if kind in {'acyclic', 'acyclic_fanout', 'cache_refresh'}:
            value = tree.nodes.new('ShaderNodeValue')
            value.outputs[0].default_value = 0.75
            tree.links.new(value.outputs[0], first_enabled(pin.inputs))
            if kind == 'acyclic_fanout':
                extra_out = tree.nodes.new('ShaderNodePortalOut')
                extra_out.portal_name = 'A'
                extra_out.data_type = 'FLOAT'
                tree.links.new(first_enabled(extra_out.outputs), math.inputs[1])
        elif kind == 'duplicate_name':
            extra = tree.nodes.new('ShaderNodePortalIn')
            extra.portal_name = 'A'
            extra.data_type = 'FLOAT'
            tree.links.new(math.outputs[0], first_enabled(pin.inputs))
        elif kind == 'empty_name':
            pin.portal_name = pout.portal_name = ''
            tree.links.new(math.outputs[0], first_enabled(pin.inputs))
        elif kind == 'unmatched':
            pout.portal_name = 'B'
            tree.links.new(math.outputs[0], first_enabled(pin.inputs))
        elif kind == 'type_mismatch':
            pout.data_type = 'VECTOR'
            tree.links.new(math.outputs[0], first_enabled(pin.inputs))
        elif kind == 'two_portals':
            pin2, pout2 = make_portal(tree, 'B')
            tree.links.new(math.outputs[0], first_enabled(pin2.inputs))
            tree.links.new(first_enabled(pout2.outputs), first_enabled(pin.inputs))
        elif kind == 'reroute_cycle':
            reroute = tree.nodes.new('NodeReroute')
            tree.links.new(math.outputs[0], reroute.inputs[0])
            tree.links.new(reroute.outputs[0], first_enabled(pin.inputs))
        else:
            tree.links.new(math.outputs[0], first_enabled(pin.inputs))
        if kind == 'muted_math_cycle':
            math.mute = True
        if kind == 'muted_in_cycle':
            pin.mute = True
        if kind == 'muted_out_cycle':
            pout.mute = True
        tree.update_tag()
        bpy.context.view_layer.update()
        invalid = sum(not link.is_valid for link in tree.links)
        if kind == 'acyclic':
            check(invalid == 0, "acyclic portal must remain valid")
        if kind == 'cache_refresh':
            check_portal_value('initial', 0.75)
            pin.portal_name = 'Renamed'
            check_portal_value('renamed_missing', 0.0)
            pin.portal_name = 'A'
            check_portal_value('renamed_restored', 0.75)
            pin.data_type = 'VECTOR'
            check_portal_value('type_mismatch', 0.0)
            pin.data_type = 'FLOAT'
            check_portal_value('type_restored', 0.75)
            extra = tree.nodes.new('ShaderNodePortalIn')
            extra.portal_name = 'A'
            extra.data_type = 'FLOAT'
            check_portal_value('duplicate_input', 0.0)
            tree.nodes.remove(extra)
        # A cycle can close on the implicit edge, leaving physical links valid.
        # Rendering exercises hash calculation, localization and Portal materialization.
        rgb = render_portal_center(kind)
        if kind in {'acyclic', 'acyclic_fanout', 'cache_refresh'}:
            expected = 1.5 if kind == 'acyclic_fanout' else 0.75
            check(all(abs(c - expected) < 0.03 for c in rgb), f'valid portal result: {kind} {rgb}')
        print('PORTAL', kind, 'invalid_links', invalid, 'rgb', rgb, flush=True)
        bpy.data.materials.remove(mat)


def path_points(obj):
    return [tuple(p.co) for p in obj.motion_path.points] if obj.motion_path else None


def bake_object(name, camera_space):
    bpy.ops.object.select_all(action='DESELECT')
    bpy.ops.mesh.primitive_cube_add(location=(0, 0, -3))
    obj = bpy.context.object
    obj.name = name
    for frame in (1, 3):
        obj.location.x = frame * 0.2
        obj.keyframe_insert(data_path='location', frame=frame)
    obj.animation_visualization.motion_path.use_camera_space_bake = camera_space
    bpy.ops.object.paths_calculate()
    check(obj.motion_path and len(obj.motion_path.points), 'baked object path exists')
    return obj


def patch_fixture(source, destination, subversion, official):
    # Deliberately synthetic layout fixtures: change only GLOB subversion and DNA member names.
    # These exercise the migration boundary without pretending to be full official exports.
    sys.path.insert(0, str(REPO / 'scripts/modules'))
    import _blendfile_header
    data = bytearray(source.read_bytes())
    import io
    stream = io.BytesIO(data)
    header = _blendfile_header.BlendFileHeader(stream)
    block_struct = header.create_block_header_struct()
    while True:
        raw = stream.read(block_struct.size)
        if len(raw) != block_struct.size:
            raise AssertionError('Missing ENDB')
        block = block_struct.parse(raw)
        offset = stream.tell()
        if block.code == b'ENDB':
            break
        if block.code == b'GLOB':
            struct.pack_into('<h', data, offset + 4, subversion)
            data[offset:offset + 4] = f'{subversion:4d}'.encode('ascii')
        if block.code == b'DNA1' and official:
            dna = bytes(data[offset:offset + block.len])
            for old, new in ((b'use_outline\0', b'old_outline\0'),
                             (b'shadow_map_scale\0', b'shadow_map_scalX\0')):
                check(len(old) == len(new) and dna.count(old) == 1, 'unique synthetic DNA rename')
                dna = dna.replace(old, new)
            data[offset:offset + block.len] = dna
        stream.seek(offset + block.len)
    destination.write_bytes(data)


def versioning():
    bpy.context.scene.frame_start = 1
    bpy.context.scene.frame_end = 4
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()
    bpy.ops.object.camera_add(location=(0, 0, 3))
    bpy.context.scene.camera = bpy.context.object
    light = bpy.data.lights.new('ReviewLight', 'SUN')
    light.shadow_map_scale = 2.5
    lamp = bpy.data.objects.new('ReviewLightObject', light)
    bpy.context.scene.collection.objects.link(lamp)
    camera_obj = bake_object('CameraPath', True)
    world_obj = bake_object('WorldPath', False)
    expected_camera, expected_world = path_points(camera_obj), path_points(world_obj)
    bpy.ops.object.select_all(action='DESELECT')
    bpy.ops.object.armature_add(location=(0, 0, -3))
    rig = bpy.context.object
    rig.name = 'ReviewRig'
    rig.pose.animation_visualization.motion_path.use_camera_space_bake = True
    bpy.ops.object.mode_set(mode='POSE')
    bpy.ops.pose.select_all(action='SELECT')
    for frame in (1, 3):
        rig.pose.bones[0].location.x = 0.1 * frame
        rig.pose.bones[0].keyframe_insert(data_path='location', frame=frame)
    bpy.ops.pose.paths_calculate()
    expected_bone = path_points(rig.pose.bones[0])
    check(expected_bone is not None, 'bone camera path exists')
    bpy.ops.object.mode_set(mode='OBJECT')
    source = ASSETS / 'migration_source.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(source), compress=False)
    fixtures = (
        (True, 43), (True, 44), (True, 45),
        (False, 44), (False, 45), (False, 46), (False, bpy.app.version_file[2]),
    )
    for official, subversion in fixtures:
        path = ASSETS / f'migration_{"official_layout" if official else "npr"}_{subversion}.blend'
        patch_fixture(source, path, subversion, official)
        bpy.ops.wm.open_mainfile(filepath=str(path), load_ui=False)
        check(abs(bpy.data.lights['ReviewLight'].shadow_map_scale - (1.0 if official else 2.5)) < 1e-6,
              f'shadow scale migration: {official}/{subversion}')
        removed = subversion < 44 or (not official and subversion < 46)
        camera_path = path_points(bpy.data.objects['CameraPath'])
        bone_path = path_points(bpy.data.objects['ReviewRig'].pose.bones[0])
        check(camera_path == (None if removed else expected_camera), f'object motion migration {official}/{subversion}')
        check(bone_path == (None if removed else expected_bone), f'bone motion migration {official}/{subversion}')
        check(path_points(bpy.data.objects['WorldPath']) == expected_world, 'world path must survive')
        saved = ASSETS / 'migration_roundtrip.blend'
        bpy.ops.wm.save_as_mainfile(filepath=str(saved), compress=False)
        bpy.ops.wm.open_mainfile(filepath=str(saved), load_ui=False)
        check(path_points(bpy.data.objects['CameraPath']) == camera_path, 'object migration is idempotent')
        check(path_points(bpy.data.objects['ReviewRig'].pose.bones[0]) == bone_path, 'bone migration is idempotent')
        check(path_points(bpy.data.objects['WorldPath']) == expected_world, 'world path survives roundtrip')
        check(abs(bpy.data.lights['ReviewLight'].shadow_map_scale - (1.0 if official else 2.5)) < 1e-6,
              'shadow scale migration is idempotent')
        print('MIGRATION', official, subversion, 'removed', removed, flush=True)


def mask_render():
    import importlib.util
    source = BASE / 'test_filter_object_mask_render.py'
    spec = importlib.util.spec_from_file_location('review_mask_render', source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    make_material = module.make_filter_material
    counts = {}

    def make_list_material(target):
        material = make_material(target)
        tree = material.node_tree
        node = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeFilterObjectMask')
        node.object = None
        node.mode = 'OBJECT_LIST'
        tree.nodes.active = node
        with editor(tree):
            check(bpy.ops.node.filter_mask_item_add() == {'FINISHED'}, 'render list item')
            node.object_items[0].object = target
        counts[target.name] = target.users
        return material

    module.make_filter_material = make_list_material
    module.main()
    for name, users in counts.items():
        check(bpy.data.objects[name].users == users, 'render localization and teardown preserve users')
    print('MASK_RENDER_USERS', counts, flush=True)

print('REVIEW_BUILD', bpy.app.version_string, bpy.app.build_hash, 'file_version', bpy.app.version_file,
      'reference_graph_registered', hasattr(bpy.types, 'DataBlockReferenceNodeTree'), flush=True)
tests = {
    'scene_copy': scene_copy,
    'mask': mask,
    'portal': portal,
    'versioning': versioning,
    'mask_render': mask_render,
}
tests[MODE]()
print('REVIEW_PASS', MODE, flush=True)
