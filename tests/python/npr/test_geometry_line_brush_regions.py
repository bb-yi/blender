"""Pixel, routing and persistence acceptance for geometry-line Brush Regions."""

import argparse
import json
import sys
from pathlib import Path

import bpy
import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from test_geometry_lines import (active_output, make_brush, make_settings, make_source,
                                 mesh_object, new_tree, render, require, reset, set_value, socket)

USE_CLOSURE = False
_set_value = set_value


def set_value(node, identifier, value):
    if node.bl_idname == 'NPRLineNodeBrushClosureOutput' and identifier == 'position_offset':
        value = value[:2]
    _set_value(node, identifier, value)


def stroke_info(tree):
    if USE_CLOSURE:
        return next(n for n in tree.nodes if n.bl_idname == 'NPRLineNodeBrushClosureInput')
    return tree.nodes.new('NPRLineNodeStrokeInfo')


def region(tree):
    if USE_CLOSURE:
        from bl_operators.npr_lines import add_brush_closure
        return add_brush_closure(tree)
    end = tree.nodes.new('NPRLineNodeBrushOutput')
    end.raster_size_response = False
    end.location = (500, 100)
    require(end.outputs[0].bl_idname == 'NodeSocketNPRLineBrush',
            'Standalone output must use the existing Brush socket')
    require(not any(s.identifier == 'brush' for s in end.inputs),
            'Brush Output must not require an incoming Brush or a paired node')
    info = tree.nodes.new('NPRLineNodeStrokeInfo')
    require(not info.inputs, 'Stroke Info must provide implicit source data with no Brush input')
    tree.nodes.remove(info)
    # Keep the shared helper's two-item return shape for companion source tests.
    return end, end


def line_scene():
    reset()
    obj = mesh_object('Source Line', [(-1.5, 0, 0), (1.5, 0, 0)], edges=[(0, 1)])
    tree = new_tree()
    source = make_source(tree, [obj])
    quick = make_brush(tree, width=4)
    quick.raster_size_response = USE_CLOSURE
    settings = make_settings(tree, source, quick, ('wire',))
    tree.links.new(settings.outputs[0], socket(active_output(tree), 'plan'))
    start, end = region(tree)
    set_value(end, 'width', 4)
    return obj, tree, quick, settings, start, end


def ink(directory, name):
    return render(directory, name)[1]


def centroid(pixels):
    yy, xx = np.mgrid[:pixels.shape[0], :pixels.shape[1]]
    alpha = pixels[:, :, 3]
    weight = max(float(alpha.sum()), 1e-20)
    return np.array([(xx * alpha).sum() / weight, (yy * alpha).sum() / weight])


def extended(directory):
    results = {}
    obj, tree, quick, settings, start, end = line_scene()
    tree.links.new(end.outputs[0], socket(settings, 'visible_brush'))
    base = ink(directory, 'extended-base')
    set_value(end, 'position_offset', (12, -9, 0))
    moved = ink(directory, 'open-x-extension-negative-y')
    require(np.max(np.abs((centroid(moved) - centroid(base)) - (12, 9))) < .7,
            'Open strokes must extrapolate X along the tangent and allow negative Y')
    set_value(end, 'position_offset', (0, 0, 0))
    deselected = ink(directory, 'selection-disabled')
    require(np.max(np.abs(deselected - base)) < .003, 'Zero Offset must preserve the source position')
    info = tree.nodes.new('NPRLineNodeStrokeInfo')
    add = tree.nodes.new('ShaderNodeVectorMath')
    add.operation = 'ADD'
    add.inputs[0].default_value = (0, 7, 0)
    add.inputs[1].default_value = (0, 5, 0)
    tree.links.new(add.outputs['Vector'], socket(end, 'position_offset'))
    current = ink(directory, 'current-position-stage')
    require(abs(abs(centroid(current)[1] - centroid(base)[1]) - 12) < .7,
            'Ordinary vector fields must compose position offsets without Brush processing nodes')
    results['direct_x_y_offsets_and_vector_composition'] = 'passed'

    # A whole closed path maps around all source parts, instead of extending each edge.
    reset()
    obj = mesh_object('Closed Source', [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)],
                      edges=[(0,1),(1,2),(2,3),(3,0)])
    tree = new_tree()
    start, end = region(tree)
    source = make_source(tree, [obj])
    settings = make_settings(tree, source, end, ('wire',))
    set_value(settings, 'split_angle', 180)
    tree.links.new(settings.outputs[0], socket(active_output(tree), 'plan'))
    set_value(end, 'width', 3)
    closed = ink(directory, 'closed-base')
    set_value(end, 'position_offset', (96, 0, 0))
    wrapped = ink(directory, 'closed-wrapped')
    require(np.abs(closed - wrapped).mean() < .004 and
            abs(closed[:,:,3].sum() - wrapped[:,:,3].sum()) < 8,
            'Closed X remapping must wrap by the original complete path length')
    results['closed_wrap'] = 'passed'

    # Resampling must never fill non-drawable connection gaps.
    reset()
    obj = mesh_object('Gapped Source', [(-1.5,0,0),(-.12,0,0),(.12,0,0),(1.5,0,0)],
                      edges=[(0,1),(2,3)])
    tree = new_tree()
    start, end = region(tree)
    settings = make_settings(tree, make_source(tree, [obj]), end, ('wire',))
    set_value(settings, 'gap_join', 32)
    set_value(settings, 'split_angle', 180)
    tree.links.new(settings.outputs[0], socket(active_output(tree), 'plan'))
    set_value(end, 'sample_length', 8)
    gaps = ink(directory, 'resample-gaps')
    require(gaps[92:100,94:98,3].max() < .01 and gaps[:,:,3].sum() > 100,
            'Resampling must keep source endpoints and the undrawable gap')
    results['resample_connection_gaps'] = 'passed'

    reset()
    obj = mesh_object('Corner Source', [(-1.5,-.8,0),(1.5,-.8,0),(1.5,.8,0),(-1.5,.8,0)],
                      faces=[(0,1,2,3)])
    tree = new_tree()
    start, end = region(tree)
    settings = make_settings(tree, make_source(tree, [obj]), end, ('outline',))
    tree.links.new(settings.outputs[0], socket(active_output(tree), 'plan'))
    set_value(end, 'width', 4)
    attr_node = tree.nodes.new('NPRLineNodeColorAttribute')
    set_value(attr_node, 'name', 'CornerInk')
    tree.links.new(attr_node.outputs['Color'], socket(end, 'color'))
    for data_type in ('FLOAT_COLOR', 'BYTE_COLOR'):
        attr = obj.data.color_attributes.new(name='CornerInk', type=data_type, domain='CORNER')
        for value in attr.data:
            value.color = (.05, .8, .1, .4)
        corner = ink(directory, 'corner-' + data_type)
        require(corner[:,:,1].sum() > corner[:,:,0].sum() * 8,
                'Evaluated Corner color attributes must shade surface contours')
        obj.data.color_attributes.remove(attr)
    tree.links.new(attr_node.outputs['Valid'], socket(end, 'opacity'))
    missing = ink(directory, 'missing-attribute-validity')
    require(missing[:,:,3].max() == 0, 'A missing color attribute must expose false validity')
    tree.links.remove(socket(end, 'opacity').links[0])
    tree.links.remove(socket(end, 'color').links[0])
    results['corner_attributes_and_validity'] = 'passed'

    aov = bpy.context.view_layer.aovs.add()
    aov.name, aov.type = 'LineColor', 'COLOR'
    material = obj.data.materials[0]
    writer = material.node_tree.nodes.new('ShaderNodeOutputAOV')
    writer.aov_name = 'LineColor'
    writer.inputs['Color'].default_value = (0,.8,.05,1)
    reader = tree.nodes.new('NPRLineNodeMaterialAOV')
    set_value(reader, 'name', 'LineColor')
    set_value(reader, 'color', (1,0,1,1))
    tree.links.new(reader.outputs['Color'], socket(end, 'color'))
    visible = ink(directory, 'aov-visible-reference')
    require(visible[:,:,1].sum() > visible[:,:,0].sum() * 4, 'Fixture must read visible source AOV')
    set_value(end, 'position_offset', (0, 12, 0))
    displaced = ink(directory, 'aov-y-retains-source')
    require(displaced[:,:,1].sum() > displaced[:,:,0].sum() * 4,
            'Y displacement must retain source AOV, even away from the surface')
    set_value(reader, 'name', 'Unregistered')
    unregistered = ink(directory, 'aov-unregistered')
    require(unregistered[:,:,0].sum() > 200 and unregistered[:,:,1].sum() < 1,
            'Unregistered AOV must use fallback')
    set_value(reader, 'name', 'LineColor')
    reader.mode = 'VALUE'
    set_value(reader, 'value', 0)
    tree.links.new(reader.outputs['Value'], socket(end, 'opacity'))
    mismatch = ink(directory, 'aov-type-mismatch')
    require(mismatch[:,:,3].max() == 0, 'AOV type mismatch must use the Value fallback')
    tree.links.remove(socket(end, 'opacity').links[0])
    reader.mode = 'COLOR'
    material.surface_render_method = 'BLENDED'
    forward = ink(directory, 'aov-forward-fallback')
    require(forward[:,:,0].sum() > 200 and forward[:,:,1].sum() < 1,
            'Unsupported Forward AOV must use fallback')
    material.surface_render_method = 'DITHERED'
    # A foreground surface writes a different AOV. Hidden source must not borrow it.
    blocker = mesh_object('Foreground', [(-2,-2,.5),(2,-2,.5),(2,2,.5),(-2,2,.5)],
                          faces=[(0,1,2,3)])
    blocker_writer = blocker.data.materials[0].node_tree.nodes.new('ShaderNodeOutputAOV')
    blocker_writer.aov_name = 'LineColor'
    blocker_writer.inputs['Color'].default_value = (1,1,0,1)
    set_value(settings, 'hidden_enabled', True)
    set_value(settings, 'object_contours', True)
    hidden = ink(directory, 'aov-hidden-foreground-fallback')
    require(hidden[:,:,0].sum() > 200 and hidden[:,:,2].sum() > 200 and hidden[:,:,1].sum() < 1,
            'Hidden lines must use fallback instead of a foreground AOV')
    results['aov_source_offset_missing_type_forward_hidden'] = 'passed'

    # Preview shares actual scene evaluation and must clean its disposable data.
    obj, tree, quick, settings, start, end = line_scene()
    from bl_operators.npr_lines import region_preview
    counts = tuple(len(c) for c in (bpy.data.scenes,bpy.data.objects,bpy.data.meshes,
                                    bpy.data.cameras,bpy.data.node_groups))
    preview = region_preview(end, length=256, width=512, height=160)
    pixels = np.asarray(preview.image_pixels_float[:]).reshape(160,512,4)
    require(pixels[75:85,80:430,:3].min() < .05, 'Preview must draw actual black region ink')
    require(counts == tuple(len(c) for c in (bpy.data.scenes,bpy.data.objects,bpy.data.meshes,
                                            bpy.data.cameras,bpy.data.node_groups)),
            'Preview must remove all temporary scene data')
    require(bpy.context.scene.eevee.geometry_line_tree == tree, 'Preview must preserve active scene and graph')
    results['preview_shared_render_and_cleanup'] = 'passed'
    return results


def parity(directory):
    obj, tree, quick, settings, start, end = line_scene()
    for raster in (False, True):
        quick.raster_size_response = start.raster_size_response = raster
        for mode in ('SIMPLE','NORMAL','MULTIPLE'):
            quick.mode = start.mode = mode
            for width in (1,4,12):
                set_value(quick,'width',width)
                set_value(end,'width',width)
                tree.links.new(quick.outputs[0],socket(settings,'visible_brush'))
                legacy=ink(directory,f'parity-quick-{raster}-{mode}-{width}')
                tree.links.new(end.outputs[0],socket(settings,'visible_brush'))
                region_pixels=ink(directory,f'parity-region-{raster}-{mode}-{width}')
                error=float(np.max(np.abs(legacy-region_pixels)))
                require(error<.003,f'Constant region must match quick Brush: raster={raster} mode={mode} width={width} error={error}')
    return {'all_modes_constant_passthrough':'passed'}


def main(directory, reopen=False):
    results = {}
    if reopen:
        bpy.ops.wm.open_mainfile(filepath=str(directory / 'brush-regions.blend'))
        tree = bpy.context.scene.eevee.geometry_line_tree
        require(any(n.bl_idname == 'NPRLineNodeBrushOutput' and n.use_field_inputs for n in tree.nodes),
                'Standalone Brush Output must survive a cold process reload')
        require(not any(n.bl_idname in {'NPRLineNodeBrushInput', 'NPRLineNodeSetPosition'} for n in tree.nodes),
                'Cold reload must not recreate removed Brush control nodes')
        expected = np.load(directory / 'saved.npy')
        actual = ink(directory, 'cold-reopened')
        require(np.max(np.abs(actual - expected)) < .003, 'Cold reload must preserve region pixels')
        results['cold_reload'] = 'passed'
    else:
        obj, tree, quick, settings, start, end = line_scene()
        legacy = ink(directory, 'legacy')
        tree.links.new(end.outputs[0], socket(settings, 'visible_brush'))
        straight = ink(directory, 'region-straight')
        require(legacy[:, :, 3].sum() > 400, 'Fixture must draw real ink')
        require(np.max(np.abs(legacy - straight)) < .003, 'Pass-through region must match the quick brush')
        results['interface_and_passthrough'] = 'passed'

        set_value(end, 'position_offset', (0, 12, 0))
        shifted = ink(directory, 'normal-offset')
        require(abs(abs(centroid(shifted)[1] - centroid(straight)[1]) - 12) < .6,
                'Y offset must move ink 12 output pixels along the source normal')
        add = tree.nodes.new('ShaderNodeVectorMath')
        add.operation = 'ADD'
        add.inputs[0].default_value = (0, 12, 0)
        add.inputs[1].default_value = (0, 7, 0)
        tree.links.new(add.outputs['Vector'], socket(end, 'position_offset'))
        serial = ink(directory, 'serial-offset')
        require(abs(abs(centroid(serial)[1] - centroid(straight)[1]) - 19) < .6,
                'Vector fields must compose the offset before the output applies it')
        results['composed_offset_fields'] = 'passed'

        info = tree.nodes.new('NPRLineNodeStrokeInfo')
        frequency = tree.nodes.new('ShaderNodeMath')
        frequency.operation = 'MULTIPLY'
        frequency.inputs[1].default_value = .15
        tree.links.new(info.outputs['Distance'], frequency.inputs[0])
        sine = tree.nodes.new('ShaderNodeMath')
        sine.operation = 'SINE'
        tree.links.new(frequency.outputs[0], sine.inputs[0])
        amplitude = tree.nodes.new('ShaderNodeMath')
        amplitude.operation = 'MULTIPLY'
        amplitude.inputs[1].default_value = 8
        tree.links.new(sine.outputs[0], amplitude.inputs[0])
        vector = tree.nodes.new('ShaderNodeCombineXYZ')
        tree.links.new(amplitude.outputs[0], vector.inputs['Y'])
        tree.links.new(vector.outputs[0], socket(end, 'position_offset'))
        wave = ink(directory, 'sine')
        rows = np.nonzero(wave[:, :, 3] > .5)[0]
        require(np.ptp(rows) >= 17, 'Distance -> Sin -> Offset.Y must draw a real wave')
        results['sine_field'] = 'passed'

        # A group instance carries its own scalar defaults through a reroute.
        group = bpy.data.node_groups.new('Region Offset Function', 'NPRLineNodeTree')
        group.interface.new_socket(name='Amount', in_out='INPUT', socket_type='NodeSocketFloat')
        group.interface.new_socket(name='Offset', in_out='OUTPUT', socket_type='NodeSocketVector')
        group_in = group.nodes.new('NodeGroupInput')
        group_out = group.nodes.new('NodeGroupOutput')
        grouped_offset = group.nodes.new('ShaderNodeCombineXYZ')
        group.links.new(group_in.outputs['Amount'], grouped_offset.inputs['Y'])
        group.links.new(grouped_offset.outputs[0], group_out.inputs['Offset'])
        group_instance = tree.nodes.new('NPRLineNodeGroup')
        group_instance.node_tree = group
        group_instance.inputs['Amount'].default_value = 5
        reroute = tree.nodes.new('NodeReroute')
        tree.links.new(group_instance.outputs['Offset'], reroute.inputs[0])
        tree.links.new(reroute.outputs[0], socket(end, 'position_offset'))
        grouped = ink(directory, 'group-and-reroute')
        require(abs(abs(centroid(grouped)[1] - centroid(straight)[1]) - 5) < .6,
                'Brush and field defaults must survive groups and reroutes')
        group_instance.inputs['Amount'].default_value = 11
        grouped2 = ink(directory, 'group-parameter-updated')
        require(abs(abs(centroid(grouped2)[1] - centroid(straight)[1]) - 11) < .6,
                'Per-instance group parameters must invalidate field compilation')
        tree.links.new(vector.outputs[0], socket(end, 'position_offset'))
        results['groups_and_reroutes'] = 'passed'

        ramp = tree.nodes.new('ShaderNodeValToRGB')
        ramp.color_ramp.elements[0].color = (1, 0, 0, .1)
        ramp.color_ramp.elements[1].color = (0, 0, 1, .9)
        tree.links.new(info.outputs['Factor'], ramp.inputs[0])
        tree.links.new(ramp.outputs['Color'], socket(end, 'color'))
        gradient = ink(directory, 'gradient')
        require(float(gradient[:, :65, 0].sum()) > float(gradient[:, :65, 2].sum()) * 2,
                'Factor must shade the beginning of the stroke red')
        require(float(gradient[:, 128:, 2].sum()) > float(gradient[:, 128:, 0].sum()) * 2,
                'Factor must shade the end of the stroke blue')
        require(abs(gradient[:, :, 3].sum() - wave[:, :, 3].sum()) < 1,
                'Color alpha must not automatically reduce region opacity')
        set_value(end, 'opacity', .25)
        transparent = ink(directory, 'opacity')
        require(abs(float(transparent[:, :, 3].sum() / gradient[:, :, 3].sum()) - .25) < .02,
                'Opacity must independently scale the stroke alpha')
        set_value(end, 'opacity', 1)
        tree.links.new(ramp.outputs['Alpha'], socket(end, 'width'))
        thin = ink(directory, 'varying-width')
        require(0 < thin[:, :, 3].sum() < gradient[:, :, 3].sum() * .35,
                'A float field must control width before tip expansion')
        tree.links.remove(socket(end, 'width').links[0])
        set_value(end, 'width', 4)
        results['color_width_opacity'] = 'passed'

        # Float and byte color attributes preserve alpha independently from validity.
        for data_type, domain in [('FLOAT_COLOR', 'POINT'), ('BYTE_COLOR', 'POINT')]:
            attribute = obj.data.color_attributes.new(name='Ink', type=data_type, domain=domain)
            for value in attribute.data:
                value.color = (.1, .7, .2, .3)
            attr_node = tree.nodes.new('NPRLineNodeColorAttribute')
            set_value(attr_node, 'name', 'Ink')
            tree.links.new(attr_node.outputs['Color'], socket(end, 'color'))
            colored = ink(directory, 'attribute-' + data_type)
            require(float(colored[:, :, 1].sum()) > float(colored[:, :, 0].sum()) * 4,
                    'Point color attribute must shade loose source edges')
            tree.links.new(attr_node.outputs['Alpha'], socket(end, 'opacity'))
            alpha_attr = ink(directory, 'attribute-alpha-' + data_type)
            require(abs(float(alpha_attr[:, :, 3].sum() / colored[:, :, 3].sum()) - .3) < .03,
                    'Color Attribute Alpha must remain available separately')
            tree.links.remove(socket(end, 'opacity').links[0])
            tree.nodes.remove(attr_node)
            obj.data.color_attributes.remove(attribute)
        results['point_attributes'] = 'passed'

        tree.links.new(ramp.outputs[0], socket(end, 'color'))
        for mode in ('SIMPLE', 'NORMAL', 'MULTIPLE'):
            start.mode = mode
            set_value(active_output(tree), 'samples', 4)
            pixels = ink(directory, 'mode-' + mode)
            require(pixels[:, :, 3].sum() > 5 and np.isfinite(pixels).all(),
                    'Every brush mode must draw finite dynamically colored samples')
        start.mode = 'SIMPLE'
        set_value(active_output(tree), 'samples', 1)
        results['modes_and_multisampling'] = 'passed'
        saved = ink(directory, 'saved')
        np.save(directory / 'saved.npy', saved)
        bpy.ops.wm.save_as_mainfile(filepath=str(directory / 'brush-regions.blend'))

        reset()
        obj = mesh_object('Surface', [(-1.5, -.8, 0), (1.5, -.8, 0), (1.5, .8, 0), (-1.5, .8, 0)], faces=[(0, 1, 2, 3)])
        uv = obj.data.uv_layers.new(name='InkUV')
        for item, coordinate in zip(uv.data, ((0, 0), (1, 0), (1, 1), (0, 1))):
            item.uv = coordinate
        tree = new_tree()
        start, end = region(tree)
        source = make_source(tree, [obj])
        settings = make_settings(tree, source, end, ('outline',))
        tree.links.new(settings.outputs[0], socket(active_output(tree), 'plan'))
        set_value(end, 'width', 4)
        uv_node = tree.nodes.new('NPRLineNodeUVMap')
        set_value(uv_node, 'name', 'InkUV')
        texture = tree.nodes.new('ShaderNodeTexImage')
        image = bpy.data.images.new('UV test image', 16, 16, alpha=True, float_buffer=True)
        image.colorspace_settings.name = 'Non-Color'
        data = np.zeros((16, 16, 4), np.float32)
        data[:, :8, 0] = 1
        data[:, 8:, 2] = 1
        data[:, :, 3] = .2
        image.pixels.foreach_set(data.reshape(-1))
        image.update()
        texture.image = image
        texture.extension = 'EXTEND'
        tree.links.new(uv_node.outputs['UV'], texture.inputs[0])
        tree.links.new(texture.outputs['Color'], socket(end, 'color'))
        mapped = ink(directory, 'source-uv-image')
        require(mapped[:, :65, 0].sum() > mapped[:, :65, 2].sum() * 3,
                'Source UV must sample the red half of the image')
        require(mapped[:, 128:, 2].sum() > mapped[:, 128:, 0].sum() * 3,
                'Source UV must sample the blue half of the image')
        tree.links.new(texture.outputs['Alpha'], socket(end, 'opacity'))
        mapped_alpha = ink(directory, 'image-alpha')
        require(abs(float(mapped_alpha[:, :, 3].sum() / mapped[:, :, 3].sum()) - .2) < .02,
                'Image alpha must only affect ink when connected to Opacity')
        tree.links.remove(socket(end, 'opacity').links[0])
        results['uv_image_and_alpha'] = 'passed'

        aov = bpy.context.view_layer.aovs.add()
        aov.name = 'LineColor'
        aov.type = 'COLOR'
        material = obj.data.materials[0]
        writer = material.node_tree.nodes.new('ShaderNodeOutputAOV')
        writer.aov_name = 'LineColor'
        writer.inputs['Color'].default_value = (0, .8, .05, 1)
        reader = tree.nodes.new('NPRLineNodeMaterialAOV')
        set_value(reader, 'name', 'LineColor')
        set_value(reader, 'color', (1, 0, 1, 1))
        tree.links.new(reader.outputs['Color'], socket(end, 'color'))
        aov_color = ink(directory, 'aov-color')
        require(aov_color[:, :, 1].sum() > aov_color[:, :, 0].sum() * 5,
                'Deferred material color AOV must shade the source contour')
        writer.inputs['Color'].default_value = (0, 0, 0, 1)
        black = ink(directory, 'aov-valid-black')
        require(float(black[:, :, :3].max()) < .003 and black[:, :, 3].sum() > 200,
                'Written black must remain black rather than use the fallback')
        material.node_tree.nodes.remove(writer)
        missing = ink(directory, 'aov-unwritten')
        require(missing[:, :, 0].sum() > 200 and missing[:, :, 2].sum() > 200,
                'A registered but unwritten AOV must use the configured fallback')
        writer = material.node_tree.nodes.new('ShaderNodeOutputAOV')
        writer.aov_name = 'LineWidth'
        writer.inputs['Value'].default_value = 0
        width_aov = bpy.context.view_layer.aovs.add()
        width_aov.name = 'LineWidth'
        width_aov.type = 'VALUE'
        reader.mode = 'VALUE'
        set_value(reader, 'name', 'LineWidth')
        set_value(reader, 'value', 5)
        tree.links.remove(socket(end, 'color').links[0])
        tree.links.new(reader.outputs['Value'], socket(end, 'width'))
        zero = ink(directory, 'aov-valid-zero-width')
        require(float(zero[:, :, 3].max()) == 0,
                'A written zero width must suppress ink rather than use the fallback')
        writer.inputs['Value'].default_value = 3
        wide = ink(directory, 'aov-value-updated')
        require(wide[:, :, 3].sum() > 100, 'Material AOV edits must update the current sample')
        results['aov_color_value_validity_and_update'] = 'passed'
        results.update(extended(directory))
        results.update(parity(directory))

    import gpu
    results['backend'] = gpu.platform.backend_type_get()
    (directory / ('results-cold.json' if reopen else 'results.json')).write_text(
        json.dumps(results, indent=2), encoding='utf-8')
    print('BRUSH_REGIONS_TEST_OK ' + json.dumps(results), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True)
    parser.add_argument('--reopen', action='store_true')
    parser.add_argument('--extended', action='store_true')
    parser.add_argument('--parity', action='store_true')
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    target = Path(args.output).resolve()
    target.mkdir(parents=True, exist_ok=True)
    if args.parity:
        print('BRUSH_REGIONS_PARITY_OK ' + json.dumps(parity(target)), flush=True)
    elif args.extended:
        print('BRUSH_REGIONS_EXTENDED_OK ' + json.dumps(extended(target)), flush=True)
    else:
        main(target, args.reopen)
