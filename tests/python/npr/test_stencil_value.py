"""Same-sample stencil snapshot GPU regression. Run with isolated Blender user directories."""

import argparse
import json
import sys
import time
import traceback
from pathlib import Path

import bpy
import gpu
import numpy as np

ACTIVE_CASE = None


def arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--phase', default='smoke')
    parser.add_argument('--geometry', choices=('MESH', 'CURVES', 'POINTCLOUD'), default='MESH')
    parser.add_argument('--benchmark-width', type=int, default=0)
    parser.add_argument('--benchmark-height', type=int, default=0)
    parser.add_argument('--benchmark-variant', default='all')
    return parser.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])


def configure(reset=True):
    if reset:
        bpy.ops.wm.read_factory_settings(use_empty=True)
    else:
        for obj in list(bpy.context.scene.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    scene.render.resolution_x = scene.render.resolution_y = 128
    scene.render.resolution_percentage = 100
    scene.render.use_compositing = False
    scene.eevee.taa_render_samples = 1
    scene.eevee.use_raytracing = False
    scene.eevee.use_outline = False
    scene.view_settings.view_transform = 'Standard'
    scene.view_settings.look = 'None'
    scene.world = bpy.data.worlds.new('StencilTestWorld')
    scene.world.use_nodes = True
    scene.world.node_tree.nodes['Background'].inputs['Strength'].default_value = 0
    bpy.ops.object.camera_add(location=(0, 0, 5))
    scene.camera = bpy.context.view_layer.objects.active
    scene.camera.data.type = 'ORTHO'
    scene.camera.data.ortho_scale = 4
    return scene


def material(name, color=(1, 0, 0, 1)):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    mat.surface_render_method = 'DITHERED'
    nodes = mat.node_tree.nodes
    nodes.clear()
    emit = nodes.new('ShaderNodeEmission')
    emit.inputs['Color'].default_value = color
    output = nodes.new('ShaderNodeOutputMaterial')
    mat.node_tree.links.new(emit.outputs[0], output.inputs['Surface'])
    return mat


def plane(name, mat, size=3.8, x=0, z=0):
    bpy.ops.mesh.primitive_plane_add(size=size, location=(x, 0, z))
    obj = bpy.context.view_layer.objects.active
    obj.name = name
    obj.data.materials.append(mat)
    return obj


def marker_material(name='Marker', reference=1, order=0, mode='MARKER_ONLY'):
    mat = material(name)
    mat.stencil_mode = mode
    mat.use_stencil = True
    mat.stencil_reference = reference
    mat.stencil_read_mask = 15
    mat.stencil_write_mask = 15
    mat.stencil_test = 'ALWAYS'
    mat.stencil_pass_op = 'REPLACE'
    mat.stencil_fail_op = mat.stencil_zfail_op = 'KEEP'
    mat.stencil_order = order
    return mat


def math_node(tree, operation, first, second):
    node = tree.nodes.new('ShaderNodeMath')
    node.operation = operation
    for index, source in enumerate((first, second)):
        if isinstance(source, (int, float)):
            node.inputs[index].default_value = source
        else:
            tree.links.new(source, node.inputs[index])
    return node.outputs[0]


def reader_material(name='Reader', raw=False):
    mat = material(name, (1, 1, 1, 1))
    tree = mat.node_tree
    value = tree.nodes.new('ShaderNodeStencilValue')
    if raw:
        result = math_node(tree, 'DIVIDE', value.outputs['Value'], 15)
    else:
        geometry = tree.nodes.new('ShaderNodeNewGeometry')
        xyz = tree.nodes.new('ShaderNodeSeparateXYZ')
        tree.links.new(geometry.outputs['Position'], xyz.inputs[0])
        b = math_node(tree, 'ADD', math_node(tree, 'MULTIPLY', xyz.outputs['X'], 0.25), 0.6)
        mask = math_node(tree, 'GREATER_THAN', value.outputs['Value'], 0.5)
        cap = math_node(tree, 'MINIMUM', b, 0.5)
        delta = math_node(tree, 'SUBTRACT', cap, b)
        result = math_node(tree, 'ADD', b, math_node(tree, 'MULTIPLY', mask, delta))
    tree.links.new(result, tree.nodes.get('Emission').inputs['Color'])
    return mat


def render(label, out, report):
    global ACTIVE_CASE
    scene = bpy.context.scene
    bpy.context.view_layer.update()
    start = time.perf_counter()
    bpy.ops.render.render()
    elapsed = time.perf_counter() - start
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.render.image_settings.color_depth = '32'
    path = out / (label + '.exr')
    bpy.data.images['Render Result'].save_render(str(path))
    image = bpy.data.images.load(str(path), check_existing=False)
    width, height = image.size
    pixels = np.empty(width * height * 4, dtype=np.float32)
    image.pixels.foreach_get(pixels)
    pixels = pixels.reshape((height, width, 4))
    bpy.data.images.remove(image)
    ACTIVE_CASE = {'extent': [width, height], 'render_seconds': elapsed, 'checks': []}
    report['cases'][label] = ACTIVE_CASE
    print('STENCIL_CASE', label, 'seconds', elapsed, flush=True)
    return pixels


def sample(pixels, x=0, y=0):
    height, width = pixels.shape[:2]
    ix = int((x / 4 + 0.5) * width)
    iy = int((y / 4 + 0.5) * height)
    return pixels[iy, ix, :3], ((ix + 0.5) / width - 0.5) * 4


def near(actual, expected, tolerance=0.002):
    error = float(np.max(np.abs(np.asarray(actual) - expected)))
    if ACTIVE_CASE is not None:
        check = {'max_error': error, 'shape': list(np.asarray(actual).shape)}
        if np.asarray(actual).size <= 16:
            check.update(actual=np.asarray(actual).tolist(), expected=np.asarray(expected).tolist())
        ACTIVE_CASE['checks'].append(check)
    assert error <= tolerance, {'actual': list(np.asarray(actual)), 'expected': expected, 'error': error}


def smoke(out, report):
    scene = configure()
    writer_mat = marker_material()
    writer = plane('Marker', writer_mat, size=2.4, z=0.3)
    face_mat = reader_material()
    face = plane('Face', face_mat)
    pixels = render('01_single_face_shader', out, report)
    for x in (-1.5, -0.9, 0, 0.9, 1.5):
        rgb, wx = sample(pixels, x)
        b = 0.6 + wx * 0.25
        near(rgb, min(b, 0.5) if abs(wx) < 1.2 else b)
    writer.hide_render = True
    pixels = render('02_hidden_marker', out, report)
    rgb, wx = sample(pixels)
    near(rgb, 0.6 + wx * 0.25)
    writer.hide_render = False
    pixels = render('03_restored_marker', out, report)
    near(sample(pixels)[0], 0.5)

    face.data.materials[0] = reader_material('RawStencilReader', raw=True)
    for reference in range(16):
        writer_mat.stencil_reference = reference
        pixels = render(f'04_value_{reference:02}', out, report)
        near(sample(pixels)[0], reference / 15)
        near(sample(pixels, 1.5)[0], 0)

    writer_mat.stencil_reference = 1
    writer_mat.use_stencil = False
    pixels = render('05_marker_only_disabled_stays_hidden', out, report)
    near(sample(pixels)[0], 0)
    writer_mat.use_stencil = True
    writer.keyframe_insert(data_path='location', frame=1)
    writer.location.x = 5
    writer.keyframe_insert(data_path='location', frame=2)
    for frame in (1, 2, 1):
        scene.frame_set(frame)
        pixels = render(f'06_frame_{frame}_{len(report["cases"])}', out, report)
        near(sample(pixels)[0], 1 / 15 if frame == 1 else 0)


def routing(out, report):
    scene = configure()
    first = marker_material('First', 1, -20)
    second = marker_material('Second', 2, 20)
    a = plane('First', first, size=2.4, z=0.5)
    b = plane('SecondFarther', second, size=2.4, z=0.2)
    read_mat = reader_material(raw=True)
    face = plane('Reader', read_mat)
    near(sample(render('order_beats_marker_depth', out, report))[0], 2 / 15)
    first.stencil_order, second.stencil_order = 20, -20
    near(sample(render('order_changed', out, report))[0], 1 / 15)
    clone = b.copy()
    scene.collection.objects.link(clone)
    bpy.data.objects.remove(b, do_unlink=True)
    near(sample(render('creation_order_independent', out, report))[0], 1 / 15)

    # Decode the mask even behind the occluder, isolating marker depth testing from final visibility.
    read_mat.use_depth_write = False
    read_mat.ztest_mode = 'ALWAYS'
    occlusion = marker_material('StandardOccluder', 0, 100, 'STANDARD')
    occlusion.use_color_write = False
    occlusion.stencil_write_mask = 0
    occlusion.stencil_pass_op = 'KEEP'
    plane('LateStandardDepth', occlusion, size=1.2, x=0.7, z=1)
    pixels = render('standard_depth_before_early_marker', out, report)
    near(sample(pixels, -0.5)[0], 1 / 15)
    near(sample(pixels, 0.7)[0], 0)

    configure()
    writer = marker_material('VisibleEarly', 0, mode='EARLY_MARKER')
    writer.stencil_test = 'EQUAL'
    writer.stencil_pass_op = 'INCREMENT_WRAP'
    plane('VisibleEarly', writer, size=2.4)
    near(sample(render('early_surface_does_not_retest_stencil', out, report))[0], [1, 0, 0])

    scene = configure()
    writer = marker_material()
    marker = plane('Marker', writer, size=2.4, z=0.3)
    read_mat = reader_material(raw=True)
    read_mat.surface_render_method = 'BLENDED'
    face = plane('ForwardReader', read_mat)
    near(sample(render('forward_only_scene', out, report))[0], 1 / 15)
    marker.hide_render = True
    near(sample(render('forward_no_markers', out, report))[0], 0)
    marker.hide_render = False
    read_mat.surface_render_method = 'DITHERED'
    read_mat.use_raytrace_refraction = True
    scene.eevee.use_raytracing = True
    near(sample(render('refractive_only_scene', out, report))[0], 1 / 15)
    marker.hide_render = True
    near(sample(render('refractive_no_markers', out, report))[0], 0)

    scene = configure()
    scene.eevee.use_raytracing = True
    face_mat = reader_material('SingleFace')
    plane('Face', face_mat)
    hair_mat = marker_material('RefractiveHair', mode='EARLY_MARKER')
    hair_mat.use_raytrace_refraction = True
    hair = plane('Hair', hair_mat, size=2.4, z=0.3)
    npr = bpy.data.node_groups.new('HairReadsShadedFace', 'ShaderNodeTree')
    refraction = npr.nodes.new('ShaderNodeNPR_Refraction')
    output = npr.nodes.new('ShaderNodeNPR_Output')
    npr.links.new(refraction.outputs['Combined Color'], output.inputs['Color'])
    hair_mat.node_tree.nodes.get('Material Output').nprtree = npr
    pixels = render('hair_marks_face_then_reads_face_color', out, report)
    for x in (-0.9, 0, 0.9):
        rgb, wx = sample(pixels, x)
        near(rgb, min(0.6 + wx * 0.25, 0.5))

    # The NPR reader must retain the snapshot after the refractive layer clears native stencil.
    value = npr.nodes.new('ShaderNodeStencilValue')
    multiply = npr.nodes.new('ShaderNodeMixRGB')
    multiply.blend_type = 'MULTIPLY'
    multiply.inputs[0].default_value = 1
    npr.links.new(refraction.outputs['Combined Color'], multiply.inputs[1])
    npr.links.new(value.outputs['Value'], multiply.inputs[2])
    npr.links.new(multiply.outputs[0], output.inputs['Color'])
    near(sample(render('npr_reader_survives_later_stencil_clear', out, report))[0], 0.5)
    hair.hide_render = True
    pixels = render('hair_hidden_same_frame', out, report)
    rgb, wx = sample(pixels)
    near(rgb, 0.6 + wx * 0.25)
    hair.hide_render = False
    near(sample(render('hair_restored_same_frame', out, report))[0], 0.5)


def views(out, report):
    scene = configure()
    marker = plane('Marker', marker_material(reference=11), size=1.8, x=0.3, z=0.3)
    mat = reader_material(raw=True)
    plane('Reader', mat)
    tree = mat.node_tree
    value = tree.nodes.get('Stencil Value')
    info = tree.nodes.new('ShaderNodeRenderInfo')

    for label, width, height, border, samples in (
            ('full', 128, 128, False, 1), ('resized', 173, 91, False, 1),
            ('crop', 173, 91, True, 1), ('overscan', 128, 128, False, 1),
            ('temporal_samples', 128, 128, False, 8)):
        scene.render.resolution_x, scene.render.resolution_y = width, height
        scene.render.use_border = border
        scene.render.use_crop_to_border = border
        scene.render.border_min_x, scene.render.border_max_x = 0.17, 0.91
        scene.render.border_min_y, scene.render.border_max_y = 0.23, 0.87
        scene.eevee.taa_render_samples = samples
        scene.eevee.use_overscan = label == 'overscan'
        scene.eevee.overscan_size = 12
        for link in list(value.inputs['UV'].links):
            tree.links.remove(link)
        reference = render('uv_default_' + label, out, report)
        tree.links.new(info.outputs['Frag Coord'], value.inputs['UV'])
        actual = render('uv_connected_' + label, out, report)
        near(actual, reference, tolerance=0.00001)

    scene.render.use_border = scene.render.use_crop_to_border = False
    scene.eevee.use_overscan = False
    scene.render.resolution_x = scene.render.resolution_y = 128
    scene.eevee.taa_render_samples = 1
    offset = tree.nodes.new('ShaderNodeVectorMath')
    offset.operation = 'ADD'
    offset.inputs[1].default_value = (100, 0, 0)
    tree.links.new(info.outputs['Frag Coord'], offset.inputs[0])
    tree.links.new(offset.outputs[0], value.inputs['UV'])
    near(sample(render('uv_out_of_bounds', out, report))[0], 0)
    offset.inputs[1].default_value = (0.25, 0, 0)
    shifted = render('uv_offset_samples_other_pixel', out, report)
    near(sample(shifted, -0.7)[0], 11 / 15)
    near(sample(shifted, 0.7)[0], 0)
    tree.links.new(info.outputs['Frag Coord'], value.inputs['UV'])

    # A differently sized capture reuses render buffers before the main view.
    rt_camera = bpy.data.objects.new('CaptureCamera', scene.camera.data.copy())
    rt_camera.location = (8, 0, 5)
    scene.collection.objects.link(rt_camera)
    capture_reader = plane('CaptureReader', reader_material('CaptureReaderMaterial', raw=True), x=8)
    ct = capture_reader.data.materials[0].node_tree
    capture_info = ct.nodes.new('ShaderNodeRenderInfo')
    ct.links.new(capture_info.outputs['Frag Coord'], ct.nodes.get('Stencil Value').inputs['UV'])
    plane('CaptureMarker', marker_material('CaptureMarkerMaterial', reference=4),
          size=2.4, x=8, z=0.3)
    capture = scene.eevee.render_textures.add()
    capture.name = 'StencilSnapshotCapture'
    capture.enabled = True
    capture.camera = rt_camera
    capture.source = 'COLOR'
    capture.resolution_x = 79
    capture.resolution_y = 53
    capture.format = 'RGBA16F'
    capture.update_mode = 'EVERY_FRAME'
    display_mat = material('CaptureDisplay', (0, 0, 0, 1))
    display = plane('CaptureDisplay', display_mat, size=0.35, x=-1.5, z=0.4)
    dt = display_mat.node_tree
    capture_sample = dt.nodes.new('ShaderNodeRenderTexture')
    capture_sample.render_texture = f'RENDER_TEXTURE_{capture.uid}'
    coords = dt.nodes.new('ShaderNodeCombineXYZ')
    coords.inputs['X'].default_value = coords.inputs['Y'].default_value = 0.5
    dt.links.new(coords.outputs[0], capture_sample.inputs['Vector'])
    dt.links.new(capture_sample.outputs['Color'], dt.nodes.get('Emission').inputs['Color'])
    pixels = render('capture_then_main_view', out, report)
    near(sample(pixels)[0], 11 / 15)
    near(sample(pixels, -1.5)[0], 4 / 15)
    capture.resolution_x, capture.resolution_y = 97, 65
    marker.hide_render = True
    pixels = render('capture_resize_main_marker_hidden', out, report)
    near(sample(pixels)[0], 0)
    near(sample(pixels, -1.5)[0], 4 / 15)
    marker.hide_render = False
    pixels = render('capture_resize_main_marker_restored', out, report)
    near(sample(pixels)[0], 11 / 15)
    near(sample(pixels, -1.5)[0], 4 / 15)


def operations(out, report):
    configure()
    base = marker_material('BaseBits', 5, -10)
    op = marker_material('Operation', 9, 10)
    plane('Base', base, size=2.4, z=0.2)
    plane('Operation', op, size=2.4, z=0.3)
    plane('Reader', reader_material(raw=True))
    for operation, initial, expected in (
            ('KEEP', 5, 5), ('ZERO', 5, 0), ('REPLACE', 5, 9),
            ('INVERT', 5, 10), ('INCREMENT_WRAP', 15, 0), ('DECREMENT_WRAP', 0, 15)):
        base.stencil_reference = initial
        op.stencil_pass_op = operation
        near(sample(render('operation_' + operation.lower(), out, report))[0], expected / 15)
    base.stencil_reference = 10
    op.stencil_pass_op = 'REPLACE'
    op.stencil_reference = 5
    op.stencil_write_mask = 3
    near(sample(render('write_mask_preserves_other_user_bits', out, report))[0], 9 / 15)
    op.stencil_test = 'EQUAL'
    op.stencil_read_mask = 3
    op.stencil_reference = 2
    op.stencil_write_mask = 15
    near(sample(render('read_mask_ignores_other_user_bits', out, report))[0], 2 / 15)
    op.stencil_test = 'NEVER'
    op.stencil_fail_op = 'REPLACE'
    op.stencil_reference = 7
    near(sample(render('stencil_fail_operation', out, report))[0], 7 / 15)
    op.stencil_test = 'ALWAYS'
    op.stencil_fail_op = 'KEEP'
    op.ztest_mode = 'NEVER'
    op.stencil_zfail_op = 'REPLACE'
    op.stencil_reference = 6
    near(sample(render('depth_fail_operation', out, report))[0], 6 / 15)
    op.ztest_mode = 'LESS_EQUAL'
    op.stencil_zfail_op = 'KEEP'
    for operation in ('INCREMENT_CLAMP', 'DECREMENT_CLAMP'):
        op.stencil_pass_op = operation
        near(sample(render('unsupported_' + operation.lower(), out, report))[0], 10 / 15)


def coverage(out, report):
    scene = configure()
    mat = marker_material()
    tree = mat.node_tree
    geo = tree.nodes.new('ShaderNodeNewGeometry')
    split = tree.nodes.new('ShaderNodeSeparateXYZ')
    tree.links.new(geo.outputs['Position'], split.inputs[0])
    alpha = math_node(tree, 'GREATER_THAN', split.outputs['X'], 0)
    transparent = tree.nodes.new('ShaderNodeBsdfTransparent')
    mix = tree.nodes.new('ShaderNodeMixShader')
    tree.links.new(alpha, mix.inputs[0])
    tree.links.new(transparent.outputs[0], mix.inputs[1])
    tree.links.new(tree.nodes.get('Emission').outputs[0], mix.inputs[2])
    tree.links.new(mix.outputs[0], tree.nodes.get('Material Output').inputs['Surface'])
    writer = plane('AlphaMarker', mat, size=2.4, z=0.3)
    plane('Reader', reader_material(raw=True))
    pixels = render('alpha_coverage', out, report)
    near(sample(pixels, -0.5)[0], 0)
    near(sample(pixels, 0.5)[0], 1 / 15)
    mat.surface_render_method = 'BLENDED'
    pixels = render('forward_alpha_marker_coverage', out, report)
    near(sample(pixels, -0.5)[0], 0)
    near(sample(pixels, 0.5)[0], 1 / 15)

    # Shell and volume outputs must not leak from a Marker Only slot.
    shell = tree.nodes.new('ShaderNodeOutputOutlineShell')
    shell.inputs['Color'].default_value = (1, 0, 0, 1)
    shell.inputs['Alpha'].default_value = 1
    shell.inputs['Strength'].default_value = 0.2
    mat.outline_shell_render_method = 'FORWARD'
    mat.outline_shell_cull_method = 'NONE'
    volume = tree.nodes.new('ShaderNodeVolumeScatter')
    volume.inputs['Density'].default_value = 10
    tree.links.new(volume.outputs[0], tree.nodes.get('Material Output').inputs['Volume'])
    pixels = render('marker_only_suppresses_shell_volume', out, report)
    near(sample(pixels, -0.5)[0], 0)
    near(sample(pixels, 0.5)[0], 1 / 15)

    scene = configure()
    invisible = marker_material()
    visible = material('VisibleSlot', (0, 1, 0, 1))
    mesh = bpy.data.meshes.new('TwoSlots')
    mesh.from_pydata([(-1.6,-1,0),(-0.2,-1,0),(-0.2,1,0),(-1.6,1,0),
                     (0.2,-1,0),(1.6,-1,0),(1.6,1,0),(0.2,1,0)], [], [(0,1,2,3),(4,5,6,7)])
    mesh.materials.append(invisible)
    mesh.materials.append(visible)
    mesh.polygons[1].material_index = 1
    obj = bpy.data.objects.new('MixedMaterialSlots', mesh)
    obj.location.z = 0.3
    scene.collection.objects.link(obj)
    plane('Reader', reader_material(raw=True))
    pixels = render('marker_only_is_per_slot', out, report)
    near(sample(pixels, -0.7)[0], 1 / 15)
    near(sample(pixels, 0.7)[0], [0, 1, 0])

    scene = configure()
    receiver = material('ShadowReceiver')
    rt = receiver.node_tree
    diffuse = rt.nodes.new('ShaderNodeBsdfDiffuse')
    diffuse.inputs['Color'].default_value = (0.7, 0.7, 0.7, 1)
    rt.links.new(diffuse.outputs[0], rt.nodes.get('Material Output').inputs['Surface'])
    plane('Receiver', receiver, size=4)
    sun = bpy.data.objects.new('Sun', bpy.data.lights.new('Sun', 'SUN'))
    sun.data.energy = 2
    sun.data.angle = 0
    scene.collection.objects.link(sun)
    bpy.ops.mesh.primitive_cube_add(size=1.2, location=(0, 0, 1))
    cube = bpy.context.object
    cube.data.materials.append(marker_material('ShadowlessMarker'))
    cube.hide_render = True
    reference = render('marker_shadow_reference', out, report)
    cube.hide_render = False
    actual = render('marker_only_casts_no_shadow', out, report)
    near(actual, reference, tolerance=0.00001)


def geometry(out, report):
    scene = configure()
    mat = marker_material()
    source = plane('InstancedMarker', mat, size=0.8, z=0.3)
    collection = bpy.data.collections.new('MarkerSource')
    scene.collection.objects.unlink(source)
    collection.objects.link(source)
    for x in (-0.8, 0.8):
        instance = bpy.data.objects.new('MarkerInstance', None)
        instance.instance_type = 'COLLECTION'
        instance.instance_collection = collection
        instance.location.x = x
        scene.collection.objects.link(instance)
    plane('Reader', reader_material(raw=True))
    pixels = render('collection_instances', out, report)
    near(sample(pixels, -0.8)[0], 1 / 15)
    near(sample(pixels, 0.8)[0], 1 / 15)
    near(sample(pixels)[0], 0)

    for kind in ('CURVES', 'POINTCLOUD'):
        scene = configure()
        mat = marker_material('Marker' + kind)
        data = bpy.data.hair_curves.new('Curves') if kind == 'CURVES' else bpy.data.meshes.new('PointsHost')
        data.materials.append(mat)
        obj = bpy.data.objects.new(kind, data)
        obj.location.z = 0.5
        scene.collection.objects.link(obj)
        scene.render.hair_type = 'STRIP'
        group = bpy.data.node_groups.new(kind + 'Geometry', 'GeometryNodeTree')
        group.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
        output = group.nodes.new('NodeGroupOutput')
        assign = group.nodes.new('GeometryNodeSetMaterial')
        assign.inputs['Material'].default_value = mat
        group.links.new(assign.outputs['Geometry'], output.inputs['Geometry'])
        if kind == 'CURVES':
            line = group.nodes.new('GeometryNodeCurvePrimitiveLine')
            line.inputs['Start'].default_value = (-1, 0, 0)
            line.inputs['End'].default_value = (1, 0, 0)
            radius = group.nodes.new('GeometryNodeSetCurveRadius')
            radius.inputs['Radius'].default_value = 0.35
            group.links.new(line.outputs['Curve'], radius.inputs['Curve'])
            group.links.new(radius.outputs['Curve'], assign.inputs['Geometry'])
        else:
            points = group.nodes.new('GeometryNodePoints')
            points.inputs['Count'].default_value = 1
            points.inputs['Position'].default_value = (0, 0, 0)
            points.inputs['Radius'].default_value = 0.65
            group.links.new(points.outputs[0], assign.inputs['Geometry'])
        modifier = obj.modifiers.new('Geometry', 'NODES')
        modifier.node_group = group
        plane('Reader', reader_material(raw=True), z=-0.5)
        pixels = render('native_' + kind.lower(), out, report)
        near(sample(pixels)[0], 1 / 15)
        near(sample(pixels, 1.5)[0], 0)

    configure()
    mat = marker_material('TessellatedMarker')
    mat.displacement_method = 'BOTH'
    mat.max_vertex_displacement = 0.5
    tree = mat.node_tree
    output = tree.nodes.get('Material Output')
    tess = tree.nodes.new('ShaderNodeTessellationControl')
    tess.normal_mode = 'GEOMETRY'
    for socket in tess.inputs:
        if socket.name in ('Max Level', 'Min Level'):
            socket.default_value = 4
        if socket.name == 'Phong Smoothing':
            socket.default_value = 0
    tree.links.new(tess.outputs[0], output.inputs['Tessellation'])
    displacement = tree.nodes.new('ShaderNodeCombineXYZ')
    displacement.inputs['X'].default_value = 0.4
    tree.links.new(displacement.outputs[0], output.inputs['Displacement'])
    plane('TessellatedMarker', mat, size=1.2, z=0.3)
    plane('Reader', reader_material(raw=True))
    pixels = render('tessellated_displaced_marker', out, report)
    near(sample(pixels, 0.7)[0], 1 / 15)
    near(sample(pixels, -0.5)[0], 0)


def dependencies(out, report):
    for sink in ('alpha', 'mix_weight', 'displacement', 'depth_offset', 'tessellation', 'group', 'bundle'):
        configure()
        mat = material('Invalid_' + sink)
        tree = mat.node_tree
        output = tree.nodes.get('Material Output')
        value = tree.nodes.new('ShaderNodeStencilValue')
        source = value.outputs['Value']
        if sink == 'group':
            group = bpy.data.node_groups.new('NestedMask', 'ShaderNodeTree')
            group.interface.new_socket(name='Mask', in_out='OUTPUT', socket_type='NodeSocketFloat')
            inner = group.nodes.new('ShaderNodeStencilValue')
            reroute = group.nodes.new('NodeReroute')
            group_output = group.nodes.new('NodeGroupOutput')
            group.links.new(inner.outputs['Value'], reroute.inputs[0])
            group.links.new(reroute.outputs[0], group_output.inputs['Mask'])
            call = tree.nodes.new('ShaderNodeGroup')
            call.node_tree = group
            source = call.outputs['Mask']
        if sink == 'bundle':
            combine = tree.nodes.new('NodeCombineBundle')
            combine.bundle_items.new('FLOAT', 'Mask')
            separate = tree.nodes.new('NodeSeparateBundle')
            separate.bundle_items.new('FLOAT', 'Mask')
            tree.links.new(source, combine.inputs['Mask'])
            tree.links.new(combine.outputs['Bundle'], separate.inputs['Bundle'])
            source = separate.outputs['Mask']
        if sink in ('alpha', 'group', 'bundle'):
            principled = tree.nodes.new('ShaderNodeBsdfPrincipled')
            tree.links.new(source, principled.inputs['Alpha'])
            tree.links.new(principled.outputs[0], output.inputs['Surface'])
        elif sink == 'mix_weight':
            mix = tree.nodes.new('ShaderNodeMixShader')
            transparent = tree.nodes.new('ShaderNodeBsdfTransparent')
            tree.links.new(source, mix.inputs[0])
            tree.links.new(tree.nodes.get('Emission').outputs[0], mix.inputs[1])
            tree.links.new(transparent.outputs[0], mix.inputs[2])
            tree.links.new(mix.outputs[0], output.inputs['Surface'])
        elif sink == 'displacement':
            mat.displacement_method = 'BOTH'
            tree.links.new(source, output.inputs['Displacement'])
        elif sink == 'depth_offset':
            tree.links.new(source, output.inputs['Depth Offset'])
        else:
            tess = tree.nodes.new('ShaderNodeTessellationControl')
            tree.links.new(source, tess.inputs['Max Level'])
            tree.links.new(tess.outputs[0], output.inputs['Tessellation'])
        plane('InvalidReader', mat)
        near(sample(render('reject_' + sink, out, report))[0], [1, 0, 1])

    configure()
    mat = marker_material('BadMarkerShader')
    tree = mat.node_tree
    expr = tree.nodes.new('ShaderNodeScriptExpression')
    expr.expression = '1.0'
    transparent = tree.nodes.new('ShaderNodeBsdfTransparent')
    mix = tree.nodes.new('ShaderNodeMixShader')
    tree.links.new(expr.outputs[0], mix.inputs[0])
    tree.links.new(transparent.outputs[0], mix.inputs[1])
    tree.links.new(tree.nodes.get('Emission').outputs[0], mix.inputs[2])
    tree.links.new(mix.outputs[0], tree.nodes.get('Material Output').inputs['Surface'])
    plane('Marker', mat, size=2.4, z=0.3)
    plane('Reader', reader_material(raw=True))
    near(sample(render('marker_shader_before_failure', out, report))[0], 1 / 15)
    expr.expression = 'missing_stencil_marker_symbol'
    near(sample(render('failed_marker_writes_nothing', out, report))[0], 0)
    expr.expression = '1.0'
    near(sample(render('marker_shader_recovers', out, report))[0], 1 / 15)

    scene = configure()
    wt = scene.world.node_tree
    value = wt.nodes.new('ShaderNodeStencilValue')
    wt.links.new(value.outputs['Value'], wt.nodes.get('Background').inputs['Color'])
    wt.nodes.get('Background').inputs['Strength'].default_value = 1
    near(sample(render('world_reads_zero', out, report))[0], 0)


def state_isolation(out, report):
    for method in ('DITHERED', 'BLENDED'):
        configure()
        old = material('StandardNever', (0, 1, 0, 1))
        old.surface_render_method = method
        old.use_stencil = True
        old.stencil_test = 'NEVER'
        old.stencil_write_mask = 0
        new = old.copy()
        new.name = 'EarlyNeverSameShader'
        new.stencil_mode = 'EARLY_MARKER'
        plane('StandardLeft', old, size=1.2, x=-0.8)
        plane('EarlyRight', new, size=1.2, x=0.8)
        pixels = render('mixed_state_' + method, out, report)
        near(sample(pixels, -0.8)[0], 0)
        near(sample(pixels, 0.8)[0], [0, 1, 0])
        new.stencil_mode = 'STANDARD'
        near(sample(render('mode_to_standard_' + method, out, report), 0.8)[0], 0)
        new.stencil_mode = 'EARLY_MARKER'
        near(sample(render('mode_back_to_early_' + method, out, report), 0.8)[0], [0, 1, 0])
        new.stencil_mode = 'MARKER_ONLY'
        near(sample(render('mode_to_marker_only_' + method, out, report), 0.8)[0], 0)
        new.use_stencil = False
        near(sample(render('marker_only_disabled_' + method, out, report), 0.8)[0], 0)
        new.stencil_mode = 'EARLY_MARKER'
        near(sample(render('early_disabled_visible_' + method, out, report), 0.8)[0], [0, 1, 0])
        new.stencil_mode = 'STANDARD'
        near(sample(render('standard_disabled_restores_state_' + method, out, report), 0.8)[0], [0, 1, 0])
        shell = new.node_tree.nodes.new('ShaderNodeOutputOutlineShell')
        shell.inputs['Color'].default_value = (0, 0, 1, 1)
        shell.inputs['Alpha'].default_value = 1
        shell.inputs['Strength'].default_value = 0
        new.outline_shell_cull_method = 'NONE'
        new.outline_shell_render_method = 'DEFERRED' if method == 'DITHERED' else 'FORWARD'
        near(sample(render('shell_restores_state_' + method, out, report), 0.8)[0], [0, 0, 1])


def viewport(out, report):
    global ACTIVE_CASE
    scene = configure()
    scene.eevee.taa_samples = 1
    scene.eevee.use_taa_reprojection = False
    marker_mat = marker_material(reference=3)
    marker = plane('Marker', marker_mat, size=2.4, z=0.3)
    mat = reader_material(raw=True)
    plane('Reader', mat)
    area = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    space.shading.type = 'RENDERED'
    space.shading.use_scene_world_render = True
    space.shading.use_scene_lights_render = True
    space.overlay.show_overlays = False
    buffers = [gpu.types.GPUOffScreen(128, 128, format='RGBA32F'),
               gpu.types.GPUOffScreen(96, 64, format='RGBA32F')]

    def draw(label, which=0):
        global ACTIVE_CASE
        buffer = buffers[which]
        bpy.context.view_layer.update()
        deps = bpy.context.evaluated_depsgraph_get()
        projection = scene.camera.calc_matrix_camera(deps, x=buffer.width, y=buffer.height)
        buffer.draw_view3d(scene, bpy.context.view_layer, space, region,
                           scene.camera.matrix_world.inverted(), projection,
                           do_color_management=False, draw_background=True)
        pixels = np.array(buffer.texture_color.read(), dtype=np.float32).reshape(buffer.height, buffer.width, 4)
        ACTIVE_CASE = {'extent': [buffer.width, buffer.height], 'checks': []}
        report['cases'][label] = ACTIVE_CASE
        print('STENCIL_VIEWPORT_CASE', label, flush=True)
        return pixels[buffer.height // 2, buffer.width // 2, :3]

    try:
        near(draw('viewport_initial'), 3 / 15)
        marker.hide_viewport = True
        near(draw('viewport_hidden'), 0)
        marker.hide_viewport = False
        near(draw('viewport_restored'), 3 / 15)
        info = mat.node_tree.nodes.new('ShaderNodeRenderInfo')
        mat.node_tree.links.new(info.outputs['Frag Coord'], mat.node_tree.nodes.get('Stencil Value').inputs['UV'])
        near(draw('viewport_second_size', 1), 3 / 15)
        marker_mat.stencil_reference = 7
        near(draw('viewport_first_updated'), 7 / 15)
        near(draw('viewport_second_updated', 1), 7 / 15)
        marker.location.x = 3
        near(draw('viewport_moved'), 0)
        marker.location.x = 0
        near(draw('viewport_moved_back'), 7 / 15)
        marker.keyframe_insert(data_path='location', frame=1)
        marker.location.x = 3
        marker.keyframe_insert(data_path='location', frame=2)
        for index, frame in enumerate((1, 2, 1)):
            scene.frame_set(frame)
            near(draw(f'viewport_frame_{index}_{frame}'), 7 / 15 if frame == 1 else 0)
    finally:
        for buffer in buffers:
            buffer.free()


def dependency_edges(out, report):
    # Inactive outputs must not reject a valid active material.
    configure()
    mat = reader_material(raw=True)
    tree = mat.node_tree
    active = tree.nodes.get('Material Output')
    inactive = tree.nodes.new('ShaderNodeOutputMaterial')
    principled = tree.nodes.new('ShaderNodeBsdfPrincipled')
    tree.links.new(tree.nodes.get('Stencil Value').outputs['Value'], principled.inputs['Alpha'])
    tree.links.new(principled.outputs[0], inactive.inputs['Surface'])
    active.is_active_output = True
    plane('Reader', mat)
    plane('Marker', marker_material(reference=3), size=2.4, z=0.3)
    near(sample(render('inactive_output_is_ignored', out, report))[0], 3 / 15)
    inactive.is_active_output = True
    near(sample(render('activated_invalid_output_is_rejected', out, report))[0], [1, 0, 1])
    active.is_active_output = True
    near(sample(render('active_output_recovers', out, report))[0], 3 / 15)

    # GLSL functions conservatively depend on every argument, even unused ones.
    # An inactive source is harmless, but a connected late input must reject marker coverage.
    for source_kind in ('scene_color', 'ambient_occlusion', 'portal', 'glsl'):
        configure()
        mat = marker_material('InvalidCoverage_' + source_kind)
        tree = mat.node_tree
        output = tree.nodes.get('Material Output')
        if source_kind == 'scene_color':
            source = tree.nodes.new('ShaderNodeSceneColor').outputs[0]
        elif source_kind == 'ambient_occlusion':
            source = tree.nodes.new('ShaderNodeAmbientOcclusion').outputs['AO']
        else:
            source = tree.nodes.new('ShaderNodeStencilValue').outputs['Value']
        if source_kind == 'portal':
            portal_in = tree.nodes.new('ShaderNodePortalIn')
            portal_out = tree.nodes.new('ShaderNodePortalOut')
            portal_out.portal_name = portal_in.portal_name
            tree.links.new(source, portal_in.inputs[0])
            source = portal_out.outputs[0]
        elif source_kind == 'glsl':
            script = bpy.data.texts.new('stencil_conservative.glsl')
            script.write('void coverage(float unused_value, out float alpha) { alpha = 1.0; }')
            function = tree.nodes.new('ShaderNodeGLSLFunction')
            function.source_mode = 'INTERNAL'
            function.script = script
            function.function_name = 'coverage'
            tree.update_tag()
            tree.interface_update(bpy.context)
            bpy.context.view_layer.update()
            tree.links.new(source, function.inputs['unused_value'])
            source = function.outputs['alpha']
        principled = tree.nodes.new('ShaderNodeBsdfPrincipled')
        tree.links.new(source, principled.inputs['Alpha'])
        tree.links.new(principled.outputs[0], output.inputs['Surface'])
        plane('Marker', mat, size=2.4, z=0.3)
        plane('Reader', reader_material(raw=True))
        near(sample(render('reject_marker_' + source_kind, out, report))[0], 0)
        tree.links.new(tree.nodes.get('Emission').outputs[0], output.inputs['Surface'])
        near(sample(render('recovered_marker_' + source_kind, out, report))[0], 1 / 15)


def probes(out, report):
    from mathutils import Vector
    scene = configure()
    scene.eevee.use_raytracing = True
    scene.eevee.taa_render_samples = 8
    scene.camera.data.type = 'PERSP'
    scene.camera.location = (3, -4, 3)
    scene.camera.rotation_euler = (Vector((0, 0, 0)) - scene.camera.location).to_track_quat('-Z', 'Y').to_euler()
    scene.world.node_tree.nodes['Background'].inputs['Strength'].default_value = 0.2
    mirror = material('Mirror')
    mt = mirror.node_tree
    glossy = mt.nodes.new('ShaderNodeBsdfGlossy')
    glossy.inputs['Color'].default_value = (1, 1, 1, 1)
    glossy.inputs['Roughness'].default_value = 0
    mt.links.new(glossy.outputs[0], mt.nodes.get('Material Output').inputs['Surface'])
    plane('Mirror', mirror, size=6)
    probe = bpy.data.objects.new('PlanarProbe', bpy.data.lightprobes.new('PlanarProbe', 'PLANE'))
    probe.location.z = 0.01
    probe.data.influence_distance = 10
    scene.collection.objects.link(probe)
    bpy.ops.mesh.primitive_cube_add(size=1, location=(0, 0, 1))
    cube = bpy.context.view_layer.objects.active
    cube.visible_camera = False
    mat = marker_material('ProbeOnlyMarker')
    mat.node_tree.nodes['Emission'].inputs['Color'].default_value = (3, 0, 0, 1)
    cube.data.materials.append(mat)
    cube.hide_render = True
    reference = render('probe_hidden_reference', out, report)
    cube.hide_render = False
    actual = render('marker_only_suppresses_probe', out, report)
    near(actual, reference, tolerance=0.003)
    mat.stencil_mode = 'STANDARD'
    mat.use_stencil = False
    control = render('probe_visible_positive_control', out, report)
    delta = float(np.max(np.abs(control - reference)))
    report['cases']['probe_visible_positive_control']['reflection_delta'] = delta
    assert delta > 0.05, ('Probe fixture must see the camera-hidden object', delta)

    # Allocate a live main-view snapshot, while probe shaders must still read zero.
    plane('OffscreenSnapshotReader', reader_material(raw=True), x=20)
    plane('ProbeFallbackMarker', marker_material(reference=15), size=20, z=2)
    mat.node_tree.nodes['Emission'].inputs['Color'].default_value = (0, 0, 0, 1)
    reference = render('probe_zero_reference', out, report)
    value = mat.node_tree.nodes.new('ShaderNodeStencilValue')
    mat.node_tree.links.new(value.outputs['Value'], mat.node_tree.nodes['Emission'].inputs['Color'])
    near(render('probe_reads_zero', out, report), reference, tolerance=0.003)


def unavailable_contexts(out, report):
    scene = configure()
    plane('Marker', marker_material(reference=15), size=3.8, z=0.3)
    plane('Reader', reader_material(raw=True))
    # A live Surface snapshot must not be exposed to volume shading.
    bpy.ops.mesh.primitive_cube_add(size=1.5, location=(0, 0, 1.2))
    volume_object = bpy.context.object
    mat = material('VolumeReader')
    tree = mat.node_tree
    output = tree.nodes.get('Material Output')
    for link in list(output.inputs['Surface'].links):
        tree.links.remove(link)
    volume = tree.nodes.new('ShaderNodeVolumePrincipled')
    volume.inputs['Density'].default_value = 0
    volume.inputs['Emission Color'].default_value = (1, 1, 1, 1)
    tree.links.new(volume.outputs[0], output.inputs['Volume'])
    volume_object.data.materials.append(mat)
    reference = render('volume_zero_reference', out, report)
    value = tree.nodes.new('ShaderNodeStencilValue')
    tree.links.new(value.outputs['Value'], volume.inputs['Emission Strength'])
    near(render('volume_reads_zero', out, report), reference, tolerance=0.003)
    for link in list(volume.inputs['Emission Strength'].links):
        tree.links.remove(link)
    volume.inputs['Emission Strength'].default_value = 1
    control = render('volume_positive_control', out, report)
    assert float(np.max(np.abs(control - reference))) > 0.05

    # Filter runs after Surface but deliberately receives no Surface snapshot binding.
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from filter_graph_test_utils import attach_filter_material
    mat = bpy.data.materials.new('FilterReader')
    mat.eevee_domain = 'FILTER'
    tree = mat.node_tree
    tree.nodes.clear()
    output = tree.nodes.new('ShaderNodeOutputFilter')
    output.inputs['Alpha'].default_value = 1
    tree.nodes.new('ShaderNodeFilterGraphInput')
    value = tree.nodes.new('ShaderNodeStencilValue')
    tree.links.new(value.outputs['Value'], output.inputs['Color'])
    attach_filter_material(mat, scene_socket='Color Image')
    near(sample(render('filter_reads_zero', out, report))[0], 0)
    for link in list(output.inputs['Color'].links):
        tree.links.remove(link)
    output.inputs['Color'].default_value = (0.25, 0.25, 0.25, 1)
    near(sample(render('filter_positive_control', out, report))[0], 0.25)


def dlss(out, report):
    scene = configure()
    assert gpu.platform.backend_type_get() == 'VULKAN'
    marker = plane('Marker', marker_material(reference=3), size=2.4, z=0.3)
    mat = reader_material(raw=True)
    plane('Reader', mat)
    area = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    space.shading.type = 'RENDERED'
    space.shading.use_scene_world_render = True
    space.shading.use_scene_lights_render = True
    space.overlay.show_overlays = False
    scene.eevee.taa_samples = 1
    scene.eevee.use_taa_reprojection = False
    buffer = gpu.types.GPUOffScreen(512, 384, format='RGBA32F')
    info = mat.node_tree.nodes.new('ShaderNodeRenderInfo')
    value = mat.node_tree.nodes.get('Stencil Value')

    def draw():
        bpy.context.view_layer.update()
        deps = bpy.context.evaluated_depsgraph_get()
        projection = scene.camera.calc_matrix_camera(deps, x=512, y=384)
        buffer.draw_view3d(scene, bpy.context.view_layer, space, region,
                           scene.camera.matrix_world.inverted(), projection,
                           do_color_management=False, draw_background=True)
        pixels = np.array(buffer.texture_color.read(), dtype=np.float32).reshape(384, 512, 4)
        assert np.isfinite(pixels).all()
        return pixels[192, 256, :3]

    try:
        near(draw(), 3 / 15)
        for mode in ('sr', 'nr'):
            scene.eevee.dlss5_intensity = 1
            scene.eevee.dlss_sr_viewport_quality = 'QUALITY' if mode == 'sr' else 'OFF'
            scene.eevee.dlss5_mode = 'DLSSNR' if mode == 'nr' else 'OFF'
            for _ in range(4):
                reference = draw()
            # GPUOffScreen uses the viewport image-render path for NR.
            status = getattr(scene.eevee, 'dlss_sr_viewport_status' if mode == 'sr' else 'dlss5_render_status')
            assert 'Active' in status, status
            mat.node_tree.links.new(info.outputs['Frag Coord'], value.inputs['UV'])
            for _ in range(4):
                actual = draw()
            near(actual, reference, tolerance=0.02)
            report['cases']['dlss_' + mode] = {'status': status, 'default_uv': reference.tolist(),
                                               'explicit_uv': actual.tolist()}
            scene.eevee.dlss_sr_viewport_quality = 'OFF'
            scene.eevee.dlss5_mode = 'OFF'
            for _ in range(4):
                actual = draw()
            near(actual, 3 / 15)
            for link in list(value.inputs['UV'].links):
                mat.node_tree.links.remove(link)
        marker.hide_viewport = True
        near(draw(), 0)
        marker.hide_viewport = False
        near(draw(), 3 / 15)
    finally:
        buffer.free()


def benchmark(out, report, args, capture_hooks=None):
    scene = configure()
    writer_mat = marker_material()
    writer = plane('Marker', writer_mat, size=2.4, z=0.3)
    mat = reader_material(raw=True)
    plane('Reader', mat)
    tree = mat.node_tree
    emission = tree.nodes.get('Emission')
    reading = emission.inputs['Color'].links[0].from_socket
    wt = writer_mat.node_tree
    image = bpy.data.images.new('BenchmarkAlpha', width=256, height=256, alpha=True)
    texels = np.ones((256, 256, 4), dtype=np.float32)
    yy, xx = np.indices((256, 256))
    texels[:, :, 3] = ((xx // 16 + yy // 16) % 2).astype(np.float32)
    image.pixels.foreach_set(texels.reshape(-1))
    image.colorspace_settings.name = 'Non-Color'
    texture = wt.nodes.new('ShaderNodeTexImage')
    texture.image = image
    transparent = wt.nodes.new('ShaderNodeBsdfTransparent')
    alpha_mix = wt.nodes.new('ShaderNodeMixShader')
    wt.links.new(texture.outputs['Alpha'], alpha_mix.inputs[0])
    wt.links.new(transparent.outputs[0], alpha_mix.inputs[1])
    wt.links.new(wt.nodes.get('Emission').outputs[0], alpha_mix.inputs[2])
    sizes = ((args.benchmark_width, args.benchmark_height),) if args.benchmark_width else (
        (1920, 1080), (3840, 2160))
    for width, height in sizes:
        scene.render.resolution_x, scene.render.resolution_y = width, height
        for variant in ('baseline', 'snapshot_read', 'marker_and_read', 'marker_alpha_and_read'):
            if args.benchmark_variant not in ('all', variant):
                continue
            writer_mat.use_stencil = variant in ('marker_and_read', 'marker_alpha_and_read')
            source = alpha_mix.outputs[0] if variant == 'marker_alpha_and_read' else wt.nodes.get('Emission').outputs[0]
            wt.links.new(source, wt.nodes.get('Material Output').inputs['Surface'])
            for link in list(emission.inputs['Color'].links):
                tree.links.remove(link)
            emission.inputs['Color'].default_value = (0, 0, 0, 1)
            if variant != 'baseline':
                tree.links.new(reading, emission.inputs['Color'])
            label = f'benchmark_{width}_{height}_{variant}'
            samples = []
            print('STENCIL_BENCH_BEGIN', label, flush=True)
            for index in range(10):
                start = time.perf_counter()
                if capture_hooks is not None and index == 3:
                    capture_hooks[0]()
                bpy.ops.render.render()
                if capture_hooks is not None and index == 3:
                    capture_hooks[1]()
                samples.append(time.perf_counter() - start)
            report['cases'][label] = {'render_seconds': samples, 'warmup_count': 3,
                                     'r8ui_bytes': width * height if variant != 'baseline' else 0}
            print('STENCIL_BENCH_END', label, flush=True)


def main():
    args = arguments()
    gpu.init()
    backend = gpu.platform.backend_type_get().lower()
    out = args.output_dir or Path(__file__).resolve().parents[4] / 'temp/npr_stencil_read' / ('gpu_' + backend) / args.phase
    out.mkdir(parents=True, exist_ok=True)
    report = {'version': bpy.app.version_string, 'build_hash': bpy.app.build_hash.decode(),
              'build_date_utc': bpy.app.build_date.decode() + ' ' + bpy.app.build_time.decode(),
              'executable': bpy.app.binary_path, 'renderer': gpu.platform.renderer_get(),
              'backend': backend, 'phase': args.phase, 'cases': {}, 'passed': False}
    try:
        phases = {'smoke': smoke, 'routing': routing, 'views': views, 'operations': operations,
                  'coverage': coverage, 'geometry': geometry, 'dependencies': dependencies}
        phases['state_isolation'] = state_isolation
        phases['viewport'] = viewport
        phases['probes'] = probes
        phases['dependency_edges'] = dependency_edges
        phases['unavailable_contexts'] = unavailable_contexts
        if args.phase == 'dlss':
            dlss(out, report)
            report['passed'] = True
            print('STENCIL_VALUE_GPU_TEST_PASSED', flush=True)
            return
        if args.phase == 'benchmark':
            benchmark(out, report, args)
            report['passed'] = True
            return
        selected = phases.values() if args.phase == 'all' else [phases[args.phase]]
        for run in selected:
            run(out, report)
        report['passed'] = True
        print('STENCIL_VALUE_GPU_TEST_PASSED', flush=True)
    except Exception:
        report['error'] = traceback.format_exc()
        raise
    finally:
        (out / 'verification.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
