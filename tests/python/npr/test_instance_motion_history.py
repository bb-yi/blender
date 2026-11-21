# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Motion vectors across Geometry Nodes membership and order edits with unreliable IDs."""

import json
import os
from pathlib import Path

import bpy
import gpu
import numpy as np
import OpenImageIO as oiio


output = Path(os.environ['BLENDER_TEST_OUTPUT_DIR'])
output.mkdir(parents=True, exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.engine = 'BLENDER_EEVEE'
scene.render.resolution_x = scene.render.resolution_y = 96
scene.render.resolution_percentage = 100
scene.render.image_settings.media_type = 'MULTI_LAYER_IMAGE'
scene.render.image_settings.file_format = 'OPEN_EXR_MULTILAYER'
scene.render.image_settings.color_depth = '32'
scene.render.film_transparent = True
scene.render.use_compositing = False
scene.eevee.taa_render_samples = 4
scene.eevee.use_raytracing = False
scene.eevee.use_shadows = False
scene.eevee.use_performance_profiler = True
scene.view_layers[0].use_pass_vector = True

camera = bpy.data.objects.new('Camera', bpy.data.cameras.new('Camera'))
scene.collection.objects.link(camera)
camera.location.z = 6
camera.data.type = 'ORTHO'
camera.data.ortho_scale = 6
scene.camera = camera

material = bpy.data.materials.new('Motion fixture material')
material.use_nodes = True
shader_tree = material.node_tree
shader_tree.nodes.clear()
emission = shader_tree.nodes.new('ShaderNodeEmission')
surface = shader_tree.nodes.new('ShaderNodeOutputMaterial')
shader_tree.links.new(emission.outputs[0], surface.inputs['Surface'])

mesh = bpy.data.meshes.new('Motion prototype')
mesh.from_pydata([(-.4, -.4, 0), (.4, -.4, 0), (.4, .4, 0), (-.4, .4, 0)],
                 [], [(0, 1, 2, 3)])
mesh.materials.append(material)
prototype = bpy.data.objects.new('Off-screen source', mesh)
prototype.location.x = 100
scene.collection.objects.link(prototype)

positions = bpy.data.meshes.new('Scatter points')
positions.from_pydata([(-1, 0, 0), (1, 0, 0), (2, 0, 0)] + [(50 + i, 0, 0) for i in range(1024)],
                      [], [])
emitter = bpy.data.objects.new('Geometry Nodes emitter', positions)
emitter.show_instancer_for_render = False
scene.collection.objects.link(emitter)
tree = bpy.data.node_groups.new('Animated instance membership', 'GeometryNodeTree')
for direction in ('INPUT', 'OUTPUT'):
    tree.interface.new_socket(name='Geometry', in_out=direction, socket_type='NodeSocketGeometry')
source = tree.nodes.new('NodeGroupInput')
target = tree.nodes.new('NodeGroupOutput')
clock = tree.nodes.new('GeometryNodeInputSceneTime')
compare = tree.nodes.new('FunctionNodeCompare')
compare.data_type = 'INT'
compare.operation = 'GREATER_EQUAL'
compare.inputs['B'].default_value = 2
index = tree.nodes.new('GeometryNodeInputIndex')
first = tree.nodes.new('FunctionNodeCompare')
first.data_type = 'INT'
first.operation = 'EQUAL'
first.inputs['B'].default_value = 0
remove = tree.nodes.new('FunctionNodeBooleanMath')
remove.operation = 'AND'
delete = tree.nodes.new('GeometryNodeDeleteGeometry')
delete.domain = 'POINT'
info = tree.nodes.new('GeometryNodeObjectInfo')
info.inputs['Object'].default_value = prototype
info.inputs['As Instance'].default_value = True
instance = tree.nodes.new('GeometryNodeInstanceOnPoints')
instance.inputs['Pick Instance'].default_value = True
tree.links.new(clock.outputs['Frame'], compare.inputs['A'])
tree.links.new(index.outputs[0], first.inputs['A'])
tree.links.new(compare.outputs['Result'], remove.inputs[0])
tree.links.new(first.outputs['Result'], remove.inputs[1])
tree.links.new(remove.outputs[0], delete.inputs['Selection'])
tree.links.new(source.outputs[0], delete.inputs['Geometry'])
tree.links.new(delete.outputs[0], instance.inputs['Points'])
tree.links.new(info.outputs['Geometry'], instance.inputs['Instance'])
tree.links.new(instance.outputs[0], target.inputs[0])
emitter.modifiers.new('Instancing', 'NODES').node_group = tree

# A real, small emitter motion makes preservation of well-identified motion observable.
for frame, x in ((1, -.25), (2, 0), (3, .25)):
    emitter.location.x = x
    emitter.keyframe_insert(data_path='location', frame=frame)


def read_pass(path, name):
    reader = oiio.ImageInput.open(str(path))
    assert reader is not None, oiio.geterror()
    try:
        part = 0
        while reader.seek_subimage(part, 0):
            spec = reader.spec()
            indices = [i for i, channel in enumerate(spec.channelnames) if f'.{name}.' in channel]
            if indices:
                return np.asarray(reader.read_image(part, 0, 0, spec.nchannels, oiio.FLOAT))[..., indices]
            part += 1
    finally:
        reader.close()
    raise AssertionError(f'{name} pass missing from {path}')


def check_instance_ids(expected_count, expected_duplicate, expected_prefix=None, alternate=None,
                       capture_transforms=False):
    depsgraph = bpy.context.evaluated_depsgraph_get()
    geometry = emitter.evaluated_get(depsgraph).evaluated_geometry()
    cloud = geometry.instances_pointcloud()
    assert len(cloud.points) == expected_count
    refs = geometry.instance_references()
    expected_prototypes = {prototype.name, alternate.name} if alternate else {prototype.name}
    assert {ref.name for ref in refs} == expected_prototypes
    raw_ids = cloud.attributes.get('id')
    if expected_duplicate is None:
        assert raw_ids is None
    else:
        values = np.empty(expected_count, dtype=np.int32)
        raw_ids.data.foreach_get('value', values)
        assert len(np.unique(values)) == expected_count - expected_duplicate
        if expected_prefix is not None:
            assert tuple(values[:len(expected_prefix)]) == expected_prefix
    if capture_transforms:
        return instance_transforms(cloud), instance_handles(cloud)


def instance_transforms(cloud):
    transforms = np.empty((len(cloud.points), 16), dtype=np.float32)
    cloud.attributes['instance_transform'].data.foreach_get('value', transforms.ravel())
    return transforms


def instance_handles(cloud):
    handles = np.empty(len(cloud.points), dtype=np.int32)
    cloud.attributes['.reference_index'].data.foreach_get('value', handles)
    return handles


def render(label, expected_duplicate=None, reorder=False, swap_prefixes=None, alternate=None):
    scene.frame_set(1)
    count = len(positions.vertices)
    if swap_prefixes is not None:
        before, before_handles = check_instance_ids(count, expected_duplicate, swap_prefixes[0],
                                                    alternate, capture_transforms=True)
    else:
        check_instance_ids(count, expected_duplicate, (0, 0, 2) if reorder else None)
    scene.frame_set(2)
    if swap_prefixes is not None:
        after, after_handles = check_instance_ids(count, expected_duplicate, swap_prefixes[1],
                                                  alternate, capture_transforms=True)
        np.testing.assert_array_equal(before[[1, 0]], after[:2])
        np.testing.assert_array_equal(before[2:], after[2:])
        np.testing.assert_array_equal(before_handles, after_handles)
        if alternate:
            assert before_handles[0] != before_handles[1]
    elif reorder:
        check_instance_ids(count, expected_duplicate, (0, 2, 0))
    else:
        check_instance_ids(count - 1, None if expected_duplicate is None else 0)
    path = output / (label + '.exr')
    scene.render.filepath = str(path)
    assert 'FINISHED' in bpy.ops.render.render(write_still=True)
    color = read_pass(path, 'Combined')
    velocity = read_pass(path, 'Vector')
    vectors = {}
    samples_by_name = [('survivor', 64), ('neighbor', 80)]
    if swap_prefixes is not None:
        samples_by_name.append(('left', 32))
    for name, px in samples_by_name:
        region = (slice(45, 51), slice(px - 3, px + 3))
        assert np.min(color[region][..., :3]) > .5, (label, name)
        samples = velocity[region]
        assert samples.shape[-1] == 4 and np.isfinite(samples).all(), (label, name)
        vectors[name] = np.median(np.abs(samples), axis=(0, 1)).tolist()
    print('INSTANCE_MOTION_VECTOR', label, vectors, flush=True)
    return vectors


os.environ['BLENDER_INSTANCE_CACHE'] = '1'
base = render('missing_id')
ids = positions.attributes.new('id', 'INT', 'POINT')
ids.data.foreach_set('value', np.arange(len(positions.vertices), dtype=np.int32))
positions.update()
stable = render('unique_id', expected_duplicate=0)
ids.data[1].value = ids.data[0].value
positions.update()
duplicate = render('duplicate_id', expected_duplicate=1)

# Keep both duplicate-ID points, changing their encounter order across the independent neighbor.
first.operation = 'LESS_THAN'
first.inputs['B'].default_value = 3
index_offset = tree.nodes.new('ShaderNodeMath')
index_offset.operation = 'ADD'
index_offset.inputs[1].default_value = 2
weight = tree.nodes.new('ShaderNodeMath')
weight.operation = 'MODULO'
weight.inputs[1].default_value = 3
sort = tree.nodes.new('GeometryNodeSortElements')
sort.domain = 'POINT'
tree.links.new(source.outputs[0], sort.inputs['Geometry'])
tree.links.new(remove.outputs[0], sort.inputs['Selection'])
tree.links.new(index.outputs[0], index_offset.inputs[0])
tree.links.new(index_offset.outputs[0], weight.inputs[0])
tree.links.new(weight.outputs[0], sort.inputs['Sort Weight'])
tree.links.new(sort.outputs[0], instance.inputs['Points'])
reordered = render('reordered_duplicate_id', expected_duplicate=1, reorder=True)

# With unique IDs, reordering preserves the history of the two stationary members.
first.inputs['B'].default_value = 2
weight.operation = 'MULTIPLY'
weight.inputs[1].default_value = -1
tree.links.new(index.outputs[0], weight.inputs[0])
ids.data[1].value = 1
positions.update()
swapped_unique = render('swapped_unique_id', 0, swap_prefixes=((0, 1, 2), (1, 0, 2)))
ids.data[1].value = 0
positions.update()

# Index picks a prototype in the already sorted stream. Both slots keep their respective
# prototypes and move across the field even though their local transforms form a permutation.
alternate = bpy.data.objects.new('Off-screen second source', mesh)
alternate.location = prototype.location
scene.collection.objects.link(alternate)
alternate_info = tree.nodes.new('GeometryNodeObjectInfo')
alternate_info.inputs['Object'].default_value = alternate
alternate_info.inputs['As Instance'].default_value = True
join = tree.nodes.new('GeometryNodeJoinGeometry')
tree.links.new(info.outputs['Geometry'], join.inputs['Geometry'])
tree.links.new(alternate_info.outputs['Geometry'], join.inputs['Geometry'])
pick_second = tree.nodes.new('FunctionNodeCompare')
pick_second.data_type = 'INT'
pick_second.operation = 'EQUAL'
pick_second.inputs['B'].default_value = 1
tree.links.new(index.outputs[0], pick_second.inputs['A'])
tree.links.new(pick_second.outputs['Result'], instance.inputs['Instance Index'])
tree.links.new(join.outputs['Geometry'], instance.inputs['Instance'])
mixed = render('mixed_prototype_crossing', 1, swap_prefixes=((0, 0, 2), (0, 0, 2)),
               alternate=alternate)
tree.links.remove(instance.inputs['Instance Index'].links[0])
tree.links.new(info.outputs['Geometry'], instance.inputs['Instance'])
positions.attributes.remove(ids)
positions.update()

# Set Position moves the first two points to each other's original positions without
# changing their order. The matrices alone do not establish that membership changed.
point_position = tree.nodes.new('GeometryNodeInputPosition')
split_position = tree.nodes.new('ShaderNodeSeparateXYZ')
displacement = tree.nodes.new('ShaderNodeMath')
displacement.operation = 'MULTIPLY'
displacement.inputs[1].default_value = -2
animated_displacement = tree.nodes.new('ShaderNodeMath')
animated_displacement.operation = 'MULTIPLY'
move_vector = tree.nodes.new('ShaderNodeCombineXYZ')
move = tree.nodes.new('GeometryNodeSetPosition')
tree.links.new(point_position.outputs['Position'], split_position.inputs[0])
tree.links.new(split_position.outputs['X'], displacement.inputs[0])
tree.links.new(displacement.outputs[0], animated_displacement.inputs[0])
tree.links.new(compare.outputs['Result'], animated_displacement.inputs[1])
tree.links.new(animated_displacement.outputs[0], move_vector.inputs['X'])
tree.links.new(move_vector.outputs[0], move.inputs['Offset'])
tree.links.new(first.outputs['Result'], move.inputs['Selection'])
tree.links.new(source.outputs[0], move.inputs['Geometry'])
tree.links.new(move.outputs[0], instance.inputs['Points'])
moved_ordinal = render('moved_ordinal_id', swap_prefixes=(None, None))
ids = positions.attributes.new('id', 'INT', 'POINT')
values = np.arange(len(positions.vertices), dtype=np.int32)
values[1] = values[0]
ids.data.foreach_set('value', values)
positions.update()
moved_duplicate = render('moved_duplicate_id', 1, swap_prefixes=((0, 0, 2), (0, 0, 2)))

# The surviving surface keeps its 4-pixel motion on the side with unchanged membership.
# The neighbor's explicit unique ID keeps both directions even when the duplicate bucket changes.
result = {'backend': gpu.platform.backend_type_get(), 'missing_id': base,
          'unique_id': stable, 'duplicate_id': duplicate, 'reordered_duplicate_id': reordered,
          'swapped_unique_id': swapped_unique, 'mixed_prototype_crossing': mixed,
          'moved_ordinal_id': moved_ordinal, 'moved_duplicate_id': moved_duplicate}
(output / 'instance_motion_history.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
for values, motion_axes in ((base['survivor'], 1), (base['neighbor'], 1),
                            (stable['survivor'], 2), (stable['neighbor'], 2),
                            (duplicate['survivor'], 1), (duplicate['neighbor'], 2),
                            (reordered['survivor'], 1), (reordered['neighbor'], 2),
                            (swapped_unique['survivor'], 2),
                            (swapped_unique['left'], 2), (swapped_unique['neighbor'], 2),
                            (moved_ordinal['neighbor'], 2), (moved_duplicate['neighbor'], 2),
                            (mixed['neighbor'], 2)):
    vector = np.array(values)
    assert np.count_nonzero(vector < .05) == 4 - motion_axes, result
    assert np.count_nonzero((vector > 2) & (vector < 6)) == motion_axes, result
for movement in (mixed, moved_ordinal, moved_duplicate):
    for name in ('left', 'survivor'):
        vector = np.array(movement[name])
        assert np.count_nonzero((vector > 20) & (vector < 40)) == 1, result
        assert np.count_nonzero((vector > 2) & (vector < 6)) == 1, result
print('INSTANCE_MOTION_HISTORY_OK', result['backend'], flush=True)
