# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Each EEVEE sorted point cloud draw must shade only its instance."""

import json
import os
from pathlib import Path

import bpy
import gpu
import numpy as np


output = Path(os.environ['BLENDER_TEST_OUTPUT_DIR'])
output.mkdir(parents=True, exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
size = 96
scene.render.engine = 'BLENDER_EEVEE'
scene.render.resolution_x = scene.render.resolution_y = size
scene.render.resolution_percentage = 100
scene.render.image_settings.file_format = 'OPEN_EXR'
scene.render.image_settings.color_depth = '32'
scene.render.image_settings.color_mode = 'RGBA'
scene.render.film_transparent = True
scene.render.use_compositing = False
scene.eevee.taa_render_samples = 8
scene.eevee.use_shadows = False
scene.eevee.use_raytracing = False

camera = bpy.data.objects.new('Camera', bpy.data.cameras.new('Camera'))
scene.collection.objects.link(camera)
camera.location.z = 6
camera.data.type = 'ORTHO'
camera.data.ortho_scale = 4
scene.camera = camera


def transparent_emission(name, *, overlap):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    material.surface_render_method = 'BLENDED'
    material.use_transparency_overlap = overlap
    nodes, links = material.node_tree.nodes, material.node_tree.links
    nodes.clear()
    emission = nodes.new('ShaderNodeEmission')
    transparent = nodes.new('ShaderNodeBsdfTransparent')
    mix = nodes.new('ShaderNodeMixShader')
    mix.inputs[0].default_value = .35
    target = nodes.new('ShaderNodeOutputMaterial')
    links.new(transparent.outputs[0], mix.inputs[1])
    links.new(emission.outputs[0], mix.inputs[2])
    links.new(mix.outputs[0], target.inputs['Surface'])
    return material, emission


material, emission = transparent_emission('Blended by world depth', overlap=True)
nodes, links = material.node_tree.nodes, material.node_tree.links
geometry = nodes.new('ShaderNodeNewGeometry')
position = nodes.new('ShaderNodeSeparateXYZ')
front = nodes.new('ShaderNodeMath')
front.operation = 'GREATER_THAN'
color = nodes.new('ShaderNodeMixRGB')
color.inputs[1].default_value = (1, .04, .02, 1)
color.inputs[2].default_value = (.02, .04, 1, 1)
links.new(geometry.outputs['Position'], position.inputs[0])
links.new(position.outputs['Z'], front.inputs[0])
links.new(front.outputs[0], color.inputs[0])
links.new(color.outputs[0], emission.inputs['Color'])

override, emission = transparent_emission('View layer mask and geometry attribute', overlap=False)
attribute = override.node_tree.nodes.new('ShaderNodeAttribute')
attribute.attribute_name = 'cloud_tint'
override.node_tree.links.new(attribute.outputs['Color'], emission.inputs['Color'])

cloud = bpy.data.pointclouds.new('Shared native Point Cloud geometry')
cloud.resize(1)
cloud.points[0].co = (0, 0, 0)
cloud.points[0].radius = .8
cloud.attributes.new('cloud_tint', 'FLOAT_COLOR', 'POINT').data[0].color = (.03, .9, .08, 1)
cloud.materials.append(material)


def cloud_object(name, location):
    obj = bpy.data.objects.new(name, cloud)
    obj.location = location
    scene.collection.objects.link(obj)
    return obj


prototype = cloud_object('Off-screen source', (1000, 0, 0))
visible = [(-.55, 0, -.95), (.55, 0, .95)]
reference_objects = [cloud_object(f'Independent point cloud {i}', loc) for i, loc in enumerate(visible)]
# The off-screen members make the same case exercise both ordinary and cached instance ranges.
placements = visible + [(50 + i, 0, 0) for i in range(64)]
mesh = bpy.data.meshes.new('Scatter positions')
mesh.from_pydata(placements, [], [])
emitter = bpy.data.objects.new('Geometry Nodes point cloud scatter', mesh)
emitter.show_instancer_for_render = False
scene.collection.objects.link(emitter)
tree = bpy.data.node_groups.new('Native point cloud instances', 'GeometryNodeTree')
for direction in ('INPUT', 'OUTPUT'):
    tree.interface.new_socket(name='Geometry', in_out=direction, socket_type='NodeSocketGeometry')
source = tree.nodes.new('NodeGroupInput')
target = tree.nodes.new('NodeGroupOutput')
info = tree.nodes.new('GeometryNodeObjectInfo')
info.inputs['Object'].default_value = prototype
info.inputs['As Instance'].default_value = True
instance = tree.nodes.new('GeometryNodeInstanceOnPoints')
instance.inputs['Pick Instance'].default_value = True
tree.links.new(source.outputs[0], instance.inputs['Points'])
tree.links.new(info.outputs['Geometry'], instance.inputs['Instance'])
tree.links.new(instance.outputs['Instances'], target.inputs[0])
emitter.modifiers.new('Point clouds', 'NODES').node_group = tree

geometry = emitter.evaluated_get(bpy.context.evaluated_depsgraph_get()).evaluated_geometry()
assert len(geometry.instances_pointcloud().points) == len(placements)
assert [ref.name for ref in geometry.instance_references()] == [prototype.name]


def render(label, *, scatter):
    emitter.hide_render = not scatter
    for obj in reference_objects:
        obj.hide_render = scatter
    path = output / (label + '.exr')
    scene.render.filepath = str(path)
    assert 'FINISHED' in bpy.ops.render.render(write_still=True)
    image = bpy.data.images.load(str(path), check_existing=False)
    pixels = np.empty(size * size * 4, dtype=np.float32)
    image.pixels.foreach_get(pixels)
    bpy.data.images.remove(image)
    return pixels.reshape(size, size, 4)


def interior(pixels):
    # Left and right samples each cover one cloud, and the middle samples cover both.
    return np.stack([pixels[45:51, col - 2:col + 2] for col in (35, 48, 61)])


results = {}
for label, material_override in (('blended', None), ('layer_override', override)):
    scene.view_layers[0].material_override = material_override
    reference = interior(render(label + '_reference', scatter=False))
    result = interior(render(label + '_instanced', scatter=True))
    reference_medians = np.median(reference, axis=(1, 2))
    assert np.all((reference_medians[:, 3] > .2) & (reference_medians[:, 3] < .8)), reference_medians
    if material_override is None:
        assert reference_medians[0, 0] > reference_medians[0, 2] + .2, reference_medians
        assert reference_medians[2, 2] > reference_medians[2, 0] + .2, reference_medians
        assert reference_medians[1, 2] > reference_medians[1, 0] > .06, reference_medians
        assert reference_medians[1, 3] > reference_medians[0, 3] + .1, reference_medians
    else:
        assert np.all(reference_medians[:, 1] > reference_medians[:, 0] + .15), reference_medians
    results[label] = {
        'reference_rgba': reference_medians.tolist(),
        'instance_rgba': np.median(result, axis=(1, 2)).tolist(),
        'max_sample_delta': float(np.max(np.abs(result - reference))),
    }
    print('POINTCLOUD_ALPHA_CASE', label, json.dumps(results[label]), flush=True)
    np.testing.assert_allclose(result, reference, atol=.02, rtol=0, err_msg=label)

(output / 'instance_pointcloud_alpha.json').write_text(json.dumps({
    'backend': gpu.platform.backend_type_get(),
    'instances': len(placements),
    'cases': results,
}, indent=2))
print('INSTANCE_POINTCLOUD_ALPHA_OK', gpu.platform.backend_type_get(), flush=True)
