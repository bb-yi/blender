"""Paired stencil writer surface reference/depth GPU regression. Isolated user dirs required."""
import json
import math
from pathlib import Path
import sys
sys.dont_write_bytecode = True

import bpy
import gpu
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_stencil_value import arguments, configure, material, marker_material, math_node, plane


args = arguments()
gpu.init()
backend = gpu.platform.backend_type_get().lower()
OUT = args.output_dir or Path(__file__).resolve().parents[4] / 'temp/stencil_writer_depth/out' / backend
OUT.mkdir(parents=True, exist_ok=True)
REPORT = {'build': bpy.app.build_hash.decode(), 'backend': backend, 'cases': []}


def reader(name='WriterSurfaceReader', source='WRITER_SURFACE'):
    mat = material(name, (0, 0, 0, 1))
    tree = mat.node_tree
    sample = tree.nodes.new('ShaderNodeStencilValue')
    assert [s.name for s in sample.outputs] == ['Value', 'Depth', 'Valid']
    assert sample.source == 'STENCIL_SNAPSHOT'
    sample.source = source
    color = tree.nodes.new('ShaderNodeCombineXYZ')
    tree.links.new(math_node(tree, 'DIVIDE', sample.outputs['Value'], 15), color.inputs['X'])
    tree.links.new(math_node(tree, 'DIVIDE', sample.outputs['Depth'], 10), color.inputs['Y'])
    tree.links.new(sample.outputs['Valid'], color.inputs['Z'])
    tree.links.new(color.outputs[0], tree.nodes.get('Emission').inputs['Color'])
    return mat, sample


def render(label):
    bpy.context.view_layer.update()
    scene = bpy.context.scene
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.render.image_settings.color_depth = '32'
    scene.render.filepath = str(OUT / (label + '.exr'))
    bpy.ops.render.render(write_still=True)
    image = bpy.data.images.load(scene.render.filepath, check_existing=False)
    width, height = image.size
    pixels = np.array(image.pixels[:], dtype=np.float32).reshape(height, width, 4).copy()
    bpy.data.images.remove(image)
    assert np.isfinite(pixels).all(), label
    return pixels


def near(label, actual, expected, tolerance=.002):
    error = float(np.max(np.abs(np.asarray(actual) - expected)))
    REPORT['cases'].append({'case': label, 'actual': np.asarray(actual).tolist(),
                            'expected': np.asarray(expected).tolist(), 'max_error': error})
    assert error < tolerance, REPORT['cases'][-1]


def center(image):
    h, w = image.shape[:2]
    return np.median(image[h // 2 - 2:h // 2 + 2, w // 2 - 2:w // 2 + 2, :3], axis=(0, 1))


scene = configure()
assert scene.eevee.stencil_surface_resolution == 'FULL'
read_mat, sample = reader()
face = plane('Receiver', read_mat, z=0)
writer_mat = marker_material('NearestWriter', reference=5)
writer = plane('Writer', writer_mat, size=2.4, z=.3)
near('nearest_front', center(render('nearest_front')), [5 / 15, 4.7 / 10, 1])
writer.location.z = -.3
near('writer_behind_visible_face_still_captured', center(render('writer_behind')), [5 / 15, 5.3 / 10, 1])
writer.location.z = .3

# Reference zero is not confused with a missing hit.
writer_mat.stencil_reference = 0
near('reference_zero_valid', center(render('reference_zero')), [0, .47, 1])
writer_mat.stencil_reference = 5
writer.hide_render = True
near('hidden_clears', center(render('hidden')), [0, 0, 0])
writer.hide_render = False
near('restored', center(render('restored')), [5 / 15, .47, 1])

# Independent nearest-depth ownership, including a non-writer occluder.
far_mat = marker_material('FarWriter', reference=9)
far_writer = plane('FarWriter', far_mat, size=2.8, z=-.4)
near('overlap_keeps_paired_nearest', center(render('overlap')), [5 / 15, .47, 1])
writer.hide_render = True
near('far_writer_after_hide', center(render('far_after_hide')), [9 / 15, .54, 1])
far_writer.hide_render = True
writer.hide_render = False
test_only = marker_material('StencilTestOnly', reference=12)
test_only.stencil_pass_op = 'KEEP'
test_only.stencil_fail_op = test_only.stencil_zfail_op = 'KEEP'
test_plane = plane('TestOnly', test_only, z=.8)
near('test_only_excluded', center(render('test_only')), [5 / 15, .47, 1])
test_plane.hide_render = True

for mode in ['STANDARD', 'EARLY_MARKER', 'MARKER_ONLY']:
    writer_mat.stencil_mode = mode
    # Color/depth write changes are independent of auxiliary writer capture.
    writer_mat.use_color_write = False
    writer_mat.use_depth_write = False
    near('mode_' + mode, center(render('mode_' + mode)), [5 / 15, .47, 1])
writer_mat.stencil_mode = 'MARKER_ONLY'
for enabled, mask, label in [(False, 15, 'disabled'), (True, 0, 'zero_write_mask')]:
    writer_mat.use_stencil = enabled
    writer_mat.stencil_write_mask = mask
    near(label, center(render(label)), [0, 0, 0])
writer_mat.use_stencil = True
writer_mat.stencil_write_mask = 15

# Capture records the raw reference, independently of native operation and write-mask semantics.
writer_mat.stencil_pass_op = 'ZERO'
writer_mat.stencil_write_mask = 3
near('raw_reference_with_zero_op', center(render('raw_reference_with_zero_op')), [5 / 15, .47, 1])
writer_mat.stencil_pass_op = 'REPLACE'
writer_mat.stencil_write_mask = 15
for cull, expected in [('BACK', [5 / 15, .47, 1]), ('FRONT', [0, 0, 0])]:
    writer_mat.surface_cull_method = cull
    near('cull_' + cull, center(render('cull_' + cull)), expected)
writer_mat.surface_cull_method = 'NONE'

for resolution in ['FULL', 'HALF', 'QUARTER']:
    scene.eevee.stencil_surface_resolution = resolution
    scene.render.resolution_x, scene.render.resolution_y = 129, 97
    near('resolution_' + resolution, center(render('resolution_' + resolution)), [5 / 15, .47, 1])
scene.render.resolution_x = scene.render.resolution_y = 128
scene.eevee.stencil_surface_resolution = 'FULL'

# Explicit coordinates use the same UV convention as Render Info. Third component is not a mask.
uv = read_mat.node_tree.nodes.new('ShaderNodeCombineXYZ')
uv.inputs['X'].default_value = uv.inputs['Y'].default_value = .5
read_mat.node_tree.links.new(uv.outputs[0], sample.inputs['UV'])
near('explicit_uv', center(render('explicit_uv')), [5 / 15, .47, 1])
for x, y, label in [(-.01, .5, 'outside_left'), (1.01, .5, 'outside_right'),
                    (.5, 1.01, 'outside_top'), (.5, -.01, 'outside_bottom')]:
    uv.inputs['X'].default_value, uv.inputs['Y'].default_value = x, y
    near(label, center(render(label)), [0, 0, 0])
read_mat.node_tree.links.remove(sample.inputs['UV'].links[0])

# Group boundaries and reroutes preserve all three outputs and explicit UV coordinates.
tree = read_mat.node_tree
wrapped = bpy.data.node_groups.new('WriterSurfaceWrapped', 'ShaderNodeTree')
wrapped.interface.new_socket(name='UV', in_out='INPUT', socket_type='NodeSocketVector')
for name in ['Value', 'Depth', 'Valid']:
    wrapped.interface.new_socket(name=name, in_out='OUTPUT', socket_type='NodeSocketFloat')
group_in = wrapped.nodes.new('NodeGroupInput')
group_out = wrapped.nodes.new('NodeGroupOutput')
inside = wrapped.nodes.new('ShaderNodeStencilValue')
inside.source = 'WRITER_SURFACE'
route = wrapped.nodes.new('NodeReroute')
wrapped.links.new(group_in.outputs['UV'], route.inputs[0])
wrapped.links.new(route.outputs[0], inside.inputs['UV'])
for name in ['Value', 'Depth', 'Valid']:
    wrapped.links.new(inside.outputs[name], group_out.inputs[name])
group = tree.nodes.new('ShaderNodeGroup')
group.node_tree = wrapped
info = tree.nodes.new('ShaderNodeRenderInfo')
tree.links.new(info.outputs['Frag Coord'], group.inputs['UV'])
original_links = [(l.from_socket, l.to_socket) for l in list(tree.links) if l.from_node == sample]
for old_source, target in original_links:
    route = tree.nodes.new('NodeReroute')
    tree.links.new(group.outputs[old_source.name], route.inputs[0])
    tree.links.new(route.outputs[0], target)
near('group_and_reroutes', center(render('group_and_reroutes')), [5 / 15, .47, 1])
for old_source, target in original_links:
    tree.links.new(old_source, target)

# NPR shaders must receive the same paired resources as ordinary Surface shaders.
npr = bpy.data.node_groups.new('WriterSurfaceNPR', 'ShaderNodeTree')
npr_sample = npr.nodes.new('ShaderNodeStencilValue')
npr_sample.source = 'WRITER_SURFACE'
npr_color = npr.nodes.new('ShaderNodeCombineXYZ')
npr.links.new(math_node(npr, 'DIVIDE', npr_sample.outputs['Value'], 15), npr_color.inputs['X'])
npr.links.new(math_node(npr, 'DIVIDE', npr_sample.outputs['Depth'], 10), npr_color.inputs['Y'])
npr.links.new(npr_sample.outputs['Valid'], npr_color.inputs['Z'])
npr_output = npr.nodes.new('ShaderNodeNPR_Output')
npr.links.new(npr_color.outputs[0], npr_output.inputs['Color'])
tree.nodes.get('Material Output').nprtree = npr
near('npr_surface_reader', center(render('npr_surface_reader')), [5 / 15, .47, 1])
tree.nodes.get('Material Output').nprtree = None

# Different material batches on one object retain different reference values.
writer.hide_render = True
left_mat = marker_material('LeftSlot', reference=3)
right_mat = marker_material('RightSlot', reference=7)
mesh = bpy.data.meshes.new('MultiMaterialWriter')
mesh.from_pydata([(-1.2, -1, .3), (0, -1, .3), (0, 1, .3), (-1.2, 1, .3),
                 (0, -1, .3), (1.2, -1, .3), (1.2, 1, .3), (0, 1, .3)],
                [], [(0, 1, 2, 3), (4, 5, 6, 7)])
mesh.materials.append(left_mat)
mesh.materials.append(right_mat)
mesh.polygons[1].material_index = 1
multi = bpy.data.objects.new(mesh.name, mesh)
scene.collection.objects.link(multi)
pixels = render('multi_material')
near('multi_material_left', pixels[64, 45, :3], [3 / 15, .47, 1])
near('multi_material_right', pixels[64, 83, :3], [7 / 15, .47, 1])
multi.hide_render = True
writer.hide_render = False

# Binary transparency coverage is evaluated by the same depth shader on the caster geometry.
wt = writer_mat.node_tree
geometry = wt.nodes.new('ShaderNodeNewGeometry')
split = wt.nodes.new('ShaderNodeSeparateXYZ')
wt.links.new(geometry.outputs['Position'], split.inputs[0])
coverage = math_node(wt, 'GREATER_THAN', split.outputs['X'], 0)
transparent = wt.nodes.new('ShaderNodeBsdfTransparent')
mix = wt.nodes.new('ShaderNodeMixShader')
wt.links.new(coverage, mix.inputs[0])
wt.links.new(transparent.outputs[0], mix.inputs[1])
wt.links.new(wt.nodes.get('Emission').outputs[0], mix.inputs[2])
wt.links.new(mix.outputs[0], wt.nodes.get('Material Output').inputs['Surface'])
pixels = render('alpha_cutout')
near('alpha_cutout_left', pixels[64, 45, :3], [0, 0, 0])
near('alpha_cutout_right', pixels[64, 83, :3], [5 / 15, .47, 1])
wt.links.new(wt.nodes.get('Emission').outputs[0], wt.nodes.get('Material Output').inputs['Surface'])

# Evaluated vertex deformation updates capture on frame changes, not only object transforms.
writer.shape_key_add(name='Basis')
shape = writer.shape_key_add(name='CaptureDeform')
for vertex in shape.data:
    vertex.co.z += .2
shape.value = 0
shape.keyframe_insert(data_path='value', frame=1)
shape.value = 1
shape.keyframe_insert(data_path='value', frame=2)
for frame, expected_depth in [(1, .47), (2, .45), (1, .47)]:
    scene.frame_set(frame)
    near('deformation_frame_' + str(frame), center(render('deformation_' + str(frame))),
         [5 / 15, expected_depth, 1])

# Camera changes at the same frame must change the sampled depth immediately.
for camera_type, distance in [('ORTHO', 3), ('PERSP', 5), ('PERSP', 7)]:
    scene.camera.data.type = camera_type
    scene.camera.location.z = distance
    near('camera_' + camera_type + str(distance), center(render('camera_' + camera_type + str(distance))),
         [5 / 15, (distance - .3) / 10, 1])
scene.camera.location.z = 5
scene.camera.data.type = 'ORTHO'

# With a Forward-only receiver there is no ordinary opaque deferred surface to trigger capture.
read_mat.surface_render_method = 'BLENDED'
near('forward_only_receiver', center(render('forward_only_receiver')), [5 / 15, .47, 1])
writer.hide_render = True
near('forward_only_no_writer', center(render('forward_only_no_writer')), [0, 0, 0])
writer.hide_render = False
read_mat.surface_render_method = 'DITHERED'

# All new outputs retain the same early-stage dependency restrictions as the original mode.
tree = read_mat.node_tree
principled = tree.nodes.new('ShaderNodeBsdfPrincipled')
output = tree.nodes.get('Material Output')
tree.links.new(principled.outputs[0], output.inputs['Surface'])
for name in ['Value', 'Depth', 'Valid']:
    tree.links.new(sample.outputs[name], principled.inputs['Alpha'])
    near('reject_' + name + '_opacity', center(render('reject_' + name + '_opacity')), [1, 0, 1])
tree.links.remove(principled.inputs['Alpha'].links[0])
tree.links.new(tree.nodes.get('Emission').outputs[0], output.inputs['Surface'])
tree.links.new(sample.outputs['Depth'], output.inputs['Depth Offset'])
near('reject_depth_offset', center(render('reject_depth_offset')), [1, 0, 1])
tree.links.remove(output.inputs['Depth Offset'].links[0])
tree.nodes.remove(principled)
near('recover_after_forbidden_dependency', center(render('recover_after_forbidden_dependency')), [5 / 15, .47, 1])

# Switching back preserves the original snapshot and explicitly zeros the new outputs.
sample.source = 'STENCIL_SNAPSHOT'
writer_mat.stencil_mode = 'EARLY_MARKER'
# Keep the diagnostic receiver visible: an invisible depth-writing body would occlude it.
writer_mat.use_depth_write = False
near('snapshot_mode_compatibility', center(render('snapshot_compat')), [5 / 15, 0, 0])
sample.source = 'WRITER_SURFACE'

# Gate the rear writer in the GPU graph, rather than only inspecting its reported depth.
tree = read_mat.node_tree
camera_node = tree.nodes.new('ShaderNodeCameraData')
gap = math_node(tree, 'SUBTRACT', camera_node.outputs['View Z Depth'], sample.outputs['Depth'])
front = math_node(tree, 'GREATER_THAN', gap, 0)
gate = math_node(tree, 'MULTIPLY', front, sample.outputs['Valid'])
tree.links.new(gate, tree.nodes.get('Emission').inputs['Color'])
writer_mat.stencil_mode = 'MARKER_ONLY'
writer.location.z = .3
near('front_gate', center(render('front_gate')), [1, 1, 1])
writer.location.z = -.3
near('rear_gate', center(render('rear_gate')), [0, 0, 0])
writer.hide_render = True
near('missing_gate', center(render('missing_gate')), [0, 0, 0])
bpy.data.objects.remove(writer, do_unlink=True)
near('deleted_writer_gate', center(render('deleted_writer_gate')), [0, 0, 0])

# Reopen our own fixture, never the user's file, then verify persisted source and resolution.
assets = OUT / 'assets'
assets.mkdir(exist_ok=True)
fixture = assets / 'writer_surface_roundtrip.blend'
scene.eevee.stencil_surface_resolution = 'HALF'
bpy.ops.wm.save_as_mainfile(filepath=str(fixture), check_existing=False)
bpy.ops.wm.open_mainfile(filepath=str(fixture))
scene = bpy.context.scene
assert scene.eevee.stencil_surface_resolution == 'HALF'
reloaded = bpy.data.materials['WriterSurfaceReader'].node_tree.nodes
assert next(n for n in reloaded if n.bl_idname == 'ShaderNodeStencilValue').source == 'WRITER_SURFACE'
near('roundtrip_missing_gate', center(render('roundtrip_missing_gate')), [0, 0, 0])

# New sampling has explicit zero fallbacks outside Eevee object/NPR surface shading.
assert bpy.app.build_options.cycles
scene.render.engine = 'CYCLES'
scene.cycles.device = 'CPU'
scene.cycles.samples = 1
near('cycles_zero_fallback', center(render('cycles_zero_fallback')), [0, 0, 0])
scene.render.engine = 'BLENDER_EEVEE'
for ob in scene.objects:
    if ob.type == 'MESH': ob.hide_render = True
scene.render.film_transparent = False
world = scene.world
world.use_nodes = True
world.node_tree.nodes.clear()
wn = world.node_tree.nodes
ws = wn.new('ShaderNodeStencilValue')
ws.source = 'WRITER_SURFACE'
wc = wn.new('ShaderNodeCombineXYZ')
for source_name, target_name in [('Value', 'X'), ('Depth', 'Y'), ('Valid', 'Z')]:
    world.node_tree.links.new(ws.outputs[source_name], wc.inputs[target_name])
background = wn.new('ShaderNodeBackground')
wo = wn.new('ShaderNodeOutputWorld')
world.node_tree.links.new(wc.outputs[0], background.inputs['Color'])
world.node_tree.links.new(background.outputs[0], wo.inputs['Surface'])
near('world_zero_fallback', center(render('world_zero_fallback')), [0, 0, 0])

REPORT['passed'] = True
(OUT / 'verification.json').write_text(json.dumps(REPORT, indent=2), encoding='utf-8')
print('STENCIL_WRITER_SURFACE_RESULT', json.dumps({'backend': backend, 'checks': len(REPORT['cases'])}), flush=True)
print('STENCIL_WRITER_SURFACE_GPU_TEST_PASSED', flush=True)
