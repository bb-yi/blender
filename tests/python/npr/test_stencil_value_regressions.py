# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Marker Only state/volume and offline NR capture regressions. Use isolated user resources."""

import json
from pathlib import Path
import sys
import traceback

import bpy
import gpu
import numpy as np
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_stencil_value import (arguments, configure, marker_material, material, math_node,
                               near, plane, reader_material, render, sample)


def marker_geometry(kind, mat):
    if kind == 'MESH':
        return plane('Marker', mat, size=2.4, z=0.5)
    scene = bpy.context.scene
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
        points.inputs['Radius'].default_value = 0.65
        group.links.new(points.outputs[0], assign.inputs['Geometry'])
    obj.modifiers.new('Geometry', 'NODES').node_group = group
    return obj


def expression_coverage(mat, expression):
    tree = mat.node_tree
    expr = tree.nodes.new('ShaderNodeScriptExpression')
    expr.expression = expression
    transparent = tree.nodes.new('ShaderNodeBsdfTransparent')
    mix = tree.nodes.new('ShaderNodeMixShader')
    tree.links.new(expr.outputs[0], mix.inputs[0])
    tree.links.new(transparent.outputs[0], mix.inputs[1])
    tree.links.new(tree.nodes.get('Emission').outputs[0], mix.inputs[2])
    tree.links.new(mix.outputs[0], tree.nodes.get('Material Output').inputs['Surface'])
    return expr


def geometry_states(out, report):
    for kind in ('CURVES', 'POINTCLOUD'):
        configure()
        mat = marker_material('StateMarker_' + kind)
        expr = expression_coverage(mat, '0.53125')
        obj = marker_geometry(kind, mat)
        plane('Reader', reader_material(raw=True), z=-0.5)
        prefix = kind.lower() + '_'
        pixels = render(prefix + 'first_valid_compile', out, report)
        coverage = int(np.count_nonzero(pixels[:, :, 0] > 0.02))
        assert coverage > 10, (kind, coverage)
        near(pixels[:, :, :3].max(), 1 / 15)
        expr.expression = 'missing_marker_state_' + kind.lower()
        pixels = render(prefix + 'failed', out, report)
        near(pixels[:, :, :3], 0)
        status = mat.evaluated_get(bpy.context.evaluated_depsgraph_get()).shader_compile_status
        report['cases'][prefix + 'failed']['compile_status'] = status
        # Offline rendering owns a separate depsgraph; its materials can be gone at this point.
        # The compiler diagnostic persists on the source node. Actual live states are asserted
        # by test_stencil_value_async.py in the viewport's own evaluated material.
        report['cases'][prefix + 'failed']['diagnostic'] = expr.error_message
        assert 'missing_marker_state_' in expr.error_message, expr.error_message
        mat.use_stencil = False
        near(render(prefix + 'failed_disabled', out, report)[:, :, :3], 0)
        expr.expression = '1.0'
        near(render(prefix + 'compiled_disabled', out, report)[:, :, :3], 0)
        mat.use_stencil = True
        near(sample(render(prefix + 'recovered', out, report))[0], 1 / 15)
        obj.hide_render = True
        near(render(prefix + 'hidden', out, report)[:, :, :3], 0)
        obj.hide_render = False
        near(sample(render(prefix + 'restored', out, report))[0], 1 / 15)


def native_volume(out, report):
    scene = configure()
    scene.eevee.taa_render_samples = 8
    fixture = Path(__file__).resolve().parents[2] / 'files/usd/volume-data/fluid_noise_0019.vdb'
    assert fixture.is_file(), f'Required VDB test fixture is missing: {fixture}'
    data = bpy.data.volumes.new('MarkerOnlyVolume')
    data.filepath = str(fixture)
    assert data.grids.load(), fixture
    obj = bpy.data.objects.new('MarkerOnlyVolume', data)
    scene.collection.objects.link(obj)
    bpy.context.view_layer.update()
    corners = [Vector(corner) for corner in obj.bound_box]
    minimum = Vector(tuple(min(c[i] for c in corners) for i in range(3)))
    maximum = Vector(tuple(max(c[i] for c in corners) for i in range(3)))
    scale = 3 / max(maximum - minimum)
    obj.scale = (scale,) * 3
    obj.location = -((minimum + maximum) * 0.5) * scale
    mat = marker_material('NativeVolumeMarker', mode='STANDARD')
    mat.use_stencil = False
    tree = mat.node_tree
    tree.nodes.clear()
    output = tree.nodes.new('ShaderNodeOutputMaterial')
    volume = tree.nodes.new('ShaderNodeVolumePrincipled')
    attribute = tree.nodes.new('ShaderNodeAttribute')
    attribute.attribute_name = next(grid.name for grid in data.grids if grid.data_type == 'FLOAT')
    tree.links.new(attribute.outputs['Fac'], volume.inputs['Density'])
    tree.links.new(attribute.outputs['Fac'], volume.inputs['Emission Strength'])
    volume.inputs['Emission Color'].default_value = (1, 0, 0, 1)
    tree.links.new(volume.outputs[0], output.inputs['Volume'])
    data.materials.append(mat)
    obj.hide_render = True
    hidden = render('vdb_hidden_reference', out, report)
    obj.hide_render = False
    standard = render('vdb_standard_positive_control', out, report)
    assert float(np.max(np.abs(standard - hidden))) > 0.05
    mat.stencil_mode = 'MARKER_ONLY'
    mat.use_stencil = True
    near(render('vdb_marker_only', out, report), hidden, tolerance=0.00001)
    mat.use_stencil = False
    near(render('vdb_marker_disabled', out, report), hidden, tolerance=0.00001)
    mat.stencil_mode = 'EARLY_MARKER'
    near(render('vdb_early_keeps_volume', out, report), standard, tolerance=0.00001)
    mat.stencil_mode = 'STANDARD'
    near(render('vdb_standard_restored', out, report), standard, tolerance=0.00001)


def probe_bake(out, report):
    scene = configure()
    scene.world.node_tree.nodes['Background'].inputs['Color'].default_value = (0.5, 0.5, 0.5, 1)
    scene.world.node_tree.nodes['Background'].inputs['Strength'].default_value = 0.5
    scene.eevee.taa_render_samples = 4
    markers = [marker_geometry(kind, marker_material('BakedMarker_' + kind))
               for kind in ('CURVES', 'POINTCLOUD')]
    floor_mat = material('BakedReceiver')
    tree = floor_mat.node_tree
    diffuse = tree.nodes.new('ShaderNodeBsdfDiffuse')
    diffuse.inputs['Color'].default_value = (0.5, 0.5, 0.5, 1)
    tree.links.new(diffuse.outputs[0], tree.nodes.get('Material Output').inputs['Surface'])
    plane('BakedReceiver', floor_mat, z=-0.5)
    probe_data = bpy.data.lightprobes.new('MarkerOnlyBake', 'VOLUME')
    probe_data.resolution_x = probe_data.resolution_y = probe_data.resolution_z = 2
    probe_data.bake_samples = 8
    probe = bpy.data.objects.new('MarkerOnlyBake', probe_data)
    scene.collection.objects.link(probe)
    for obj in markers:
        obj.hide_render = True
    bpy.context.view_layer.update()
    assert bpy.ops.object.lightprobe_cache_bake(subset='ALL') == {'FINISHED'}
    reference = render('probe_bake_hidden_reference', out, report)
    assert float(reference[:, :, :3].max()) > 0.05
    for obj in markers:
        obj.hide_render = False
    bpy.context.view_layer.update()
    assert bpy.ops.object.lightprobe_cache_bake(subset='ALL') == {'FINISHED'}
    near(render('probe_bake_marker_only_geometries', out, report), reference, tolerance=0.003)


def offline_nr_capture(out, report):
    scene = configure()
    scene.render.resolution_x, scene.render.resolution_y = 256, 192
    plane('Reader', reader_material(raw=True))
    main_marker = marker_material(reference=3)
    plane('Marker', main_marker, size=2.4, z=0.3)
    camera = bpy.data.objects.new('CaptureCamera', scene.camera.data.copy())
    camera.location = (8, 0, 5)
    scene.collection.objects.link(camera)
    plane('CaptureReader', reader_material('CaptureReader', raw=True), x=8)
    capture_marker = marker_material('CaptureMarker', reference=4)
    plane('CaptureMarker', capture_marker, size=2.4, x=8, z=0.3)
    capture = scene.eevee.render_textures.add()
    capture.name = 'NRGuideCapture'
    capture.enabled = True
    capture.camera = camera
    capture.source = 'COLOR'
    capture.resolution_x, capture.resolution_y = 79, 53
    capture.format = 'RGBA16F'
    capture.update_mode = 'EVERY_FRAME'
    display_mat = material('CaptureDisplay')
    plane('CaptureDisplay', display_mat, size=0.35, x=-1.5, z=0.4)
    tree = display_mat.node_tree
    texture = tree.nodes.new('ShaderNodeRenderTexture')
    texture.render_texture = f'RENDER_TEXTURE_{capture.uid}'
    coords = tree.nodes.new('ShaderNodeCombineXYZ')
    coords.inputs['X'].default_value = coords.inputs['Y'].default_value = 0.5
    tree.links.new(coords.outputs[0], texture.inputs['Vector'])
    tree.links.new(texture.outputs['Color'], tree.nodes.get('Emission').inputs['Color'])
    reference = render('offline_nr_off', out, report)
    near(sample(reference)[0], 3 / 15)
    near(sample(reference, -1.5)[0], 4 / 15)
    scene.eevee.dlss5_intensity = 1
    scene.eevee.dlss5_mode = 'DLSSNR'
    for index in range(2):
        if index:
            capture.resolution_x, capture.resolution_y = 97, 65
            main_marker.stencil_reference = 5
            capture_marker.stencil_reference = 7
            scene.frame_set(2)
        pixels = render('offline_nr_capture_' + str(index), out, report)
        assert np.isfinite(pixels).all()
        assert float(pixels[:, :, :3].max()) > 0.05
        status = scene.eevee.dlss5_render_status
        report['cases']['offline_nr_capture_' + str(index)]['nr_status'] = status
        if gpu.platform.backend_type_get() == 'VULKAN':
            assert status.startswith('Completed'), status
        else:
            assert 'Vulkan backend required' in status, status
            near(sample(pixels)[0], (5 if index else 3) / 15)
            near(sample(pixels, -1.5)[0], (7 if index else 4) / 15)
    scene.eevee.dlss5_mode = 'OFF'
    pixels = render('offline_nr_disabled_again', out, report)
    near(sample(pixels)[0], 5 / 15)
    near(sample(pixels, -1.5)[0], 7 / 15)
    scene.frame_set(1)
    main_marker.stencil_reference = 3
    capture_marker.stencil_reference = 4
    capture.resolution_x, capture.resolution_y = 79, 53
    near(render('offline_nr_restore_frame_one', out, report), reference, tolerance=0.00001)


def main():
    args = arguments()
    gpu.init()
    backend = gpu.platform.backend_type_get().lower()
    out = args.output_dir or Path(__file__).resolve().parents[4] / 'temp/npr_stencil_fixes' / backend / args.phase
    out.mkdir(parents=True, exist_ok=True)
    report = {'passed': False, 'cases': {}, 'backend': backend, 'phase': args.phase,
              'version': bpy.app.version_string, 'build_hash': bpy.app.build_hash.decode(),
              'build_date_utc': bpy.app.build_date.decode() + ' ' + bpy.app.build_time.decode(),
              'renderer': gpu.platform.renderer_get()}
    phases = {'geometry_states': geometry_states, 'native_volume': native_volume,
              'probe_bake': probe_bake,
              'offline_nr_capture': offline_nr_capture}
    try:
        for run in phases.values() if args.phase == 'all' else (phases[args.phase],):
            run(out, report)
        report['passed'] = True
        print('STENCIL_VALUE_REGRESSIONS_PASSED', flush=True)
    except Exception:
        report['error'] = traceback.format_exc()
        raise
    finally:
        (out / 'verification.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
