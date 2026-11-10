"""Minimal native Brush Closure: real fields, ordinary groups, preview and persistence."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

import bpy
import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import test_geometry_line_brush_regions as helpers
from test_geometry_line_brush_regions import (active_output, centroid, ink, line_scene, make_settings,
                                              mesh_object, new_tree, require, reset, set_value, socket)
from bl_operators import npr_lines


def scene():
    helpers.USE_CLOSURE = True
    obj, tree, quick, settings, start, end = line_scene()
    tree.links.new(end.outputs[0], socket(settings, 'visible_brush'))
    return obj, tree, quick, settings, start, end


def instance(tree, effect):
    node = tree.nodes.new('NPRLineNodeGroup')
    node.node_tree = npr_lines.brush_effect_group(effect)
    return node


def core(out):
    results = {}
    obj, tree, quick, settings, start, end = scene()
    require(start.paired_output == end, 'Native zone must pair its boundaries')
    require(not start.inputs and [s.identifier for s in start.outputs] == ['Distance', 'Factor', 'Length', 'ID', 'Closed'],
            'Parameter input must expose all five original stroke data outputs')
    require(not hasattr(start, 'parameters') and not start.panel_states,
            'Parameter input must have no data visibility options')
    require([s.identifier for s in end.inputs] == ['position_offset', 'width', 'color', 'opacity'],
            'Brush output must have exactly four result inputs')
    require(len(socket(end, 'position_offset').default_value) == 2 and len(end.panel_states) == 0,
            'Offset must be 2D; output must have no effect panels')
    require(not any(hasattr(end, name) for name in ('mode', 'raster_size_response', 'use_width_curve',
                'width_curve', 'use_opacity_curve', 'wave_units', 'override_stroke_connection')),
            'Closure RNA must not inherit legacy brush effects')
    base = ink(out, 'closure-base')
    require(base[:, :, 3].sum() > 400, 'Closure must draw real ink')
    tree.links.new(quick.outputs[0], socket(settings, 'visible_brush'))
    legacy = ink(out, 'quick-reference')
    require(np.abs(base - legacy).max() < .003,
            'Neutral closure must match the Pencil-compatible quick continuous brush width response')
    tree.links.new(end.outputs[0], socket(settings, 'visible_brush'))
    set_value(end, 'position_offset', (11, -9))
    moved = ink(out, 'closure-offset')
    require(np.abs(centroid(moved) - centroid(base) - (11, 9)).max() < .6,
            'Offset must use original pixel arc distance and its normal')
    set_value(end, 'position_offset', (0, 0))
    set_value(end, 'width', 0)
    require(ink(out, 'closure-zero-width')[:, :, 3].max() == 0, 'Legitimate zero width must draw nothing')
    set_value(end, 'width', 4)

    wave = instance(tree, 'WAVE')
    tree.links.new(start.outputs['Distance'], wave.inputs['Distance'])
    tree.links.new(wave.outputs['Offset'], socket(end, 'position_offset'))
    shaped = ink(out, 'closure-wave-group')
    require(np.ptp(np.nonzero(shaped[:, :, 3] > .5)[0]) >= 17, 'Wave group must draw an 8px wave')
    set_value(active_output(tree), 'field_sample_length', 32)
    coarse = ink(out, 'closure-coarse-shared-quality')
    require(np.abs(coarse - shaped).sum() > 30, 'Shared sampling quality must affect evaluated shape resolution')
    set_value(active_output(tree), 'field_sample_length', .9)
    tree.links.remove(socket(end, 'position_offset').links[0])
    dash = instance(tree, 'DASH')
    tree.links.new(start.outputs['Distance'], dash.inputs['Distance'])
    tree.links.new(dash.outputs['Opacity'], socket(end, 'opacity'))
    dashed = ink(out, 'closure-dash-group')
    require(50 < dashed[:, :, 3].sum() < base[:, :, 3].sum() * .8,
            'Dashes must be an explicit opacity field')
    require(np.count_nonzero(dashed[95, 24:168, 3] < .01) > 35,
            'Overlapping Simple capsules must not fill the requested five-pixel opacity gaps')
    tree.links.remove(socket(end, 'opacity').links[0])
    taper = instance(tree, 'TAPER')
    tree.links.new(start.outputs['Factor'], taper.inputs['Factor'])
    tree.links.new(taper.outputs['Width'], socket(end, 'width'))
    tapered = ink(out, 'closure-taper-group')
    require(0 < tapered[:, :45, 3].sum() < base[:, :45, 3].sum() * .85,
            'Float Curve must taper the beginning of the line')
    mapping = next(n.mapping for n in taper.node_tree.nodes if n.bl_idname == 'ShaderNodeFloatCurve')
    for point in mapping.curves[0].points:
        point.location.y = 1
    mapping.update()
    flat = ink(out, 'closure-curve-edited-flat')
    require(np.abs(flat - base).max() < .003, 'A flat width curve must not inherit a second hidden taper')
    tree.links.remove(socket(end, 'width').links[0])
    random = instance(tree, 'RANDOM')
    random.inputs['Min'].default_value = 2
    random.inputs['Max'].default_value = 10
    tree.links.new(start.outputs['ID'], random.inputs['ID'])
    tree.links.new(random.outputs['Value'], socket(end, 'width'))
    one = ink(out, 'closure-random-seed0')
    random.inputs['Seed'].default_value = 18
    two = ink(out, 'closure-random-seed18')
    require(np.abs(one - two).sum() > 10, 'ID/seed must feed Blender White Noise and change actual width')
    tree.links.remove(socket(end, 'width').links[0])
    ramp = tree.nodes.new('ShaderNodeValToRGB')
    ramp.color_ramp.elements[0].color = (1, 0, 0, .1)
    ramp.color_ramp.elements[1].color = (0, 0, 1, .9)
    tree.links.new(start.outputs['Factor'], ramp.inputs[0])
    tree.links.new(ramp.outputs['Color'], socket(end, 'color'))
    gradient = ink(out, 'closure-gradient')
    require(gradient[:, :65, 0].sum() > gradient[:, :65, 2].sum() * 2 and
            gradient[:, 128:, 2].sum() > gradient[:, 128:, 0].sum() * 2,
            'Factor must shade the source stroke with a red-blue gradient')
    require(np.abs(gradient[:, :, 3] - base[:, :, 3]).max() < .003,
            'Color Alpha must not silently alter stroke opacity')
    set_value(end, 'opacity', .25)
    transparent = ink(out, 'closure-opacity')
    require(abs(transparent[:, :, 3].sum() / gradient[:, :, 3].sum() - .25) < .02,
            'Opacity must be the single explicit alpha field')
    set_value(end, 'opacity', 1)
    results['minimal_interface_neutral_fields_and_four_ordinary_effect_groups'] = 'passed'
    np.save(out / 'saved.npy', gradient)
    bpy.ops.wm.save_as_mainfile(filepath=str(out / 'brush-closure.blend'))
    other_parameters, other_output = npr_lines.add_brush_closure(tree)
    tree.links.new(start.outputs['Factor'], socket(other_output, 'width'))
    tree.links.new(other_output.outputs[0], socket(settings, 'visible_brush'))
    invalid = ink(out, 'closure-invalid-cross-scope')
    require(invalid[:, :, 3].max() == 0, 'A closure must reject parameter data from a different pair')
    tree.links.remove(socket(other_output, 'width').links[0])
    tree.nodes.remove(other_parameters)
    unpaired = ink(out, 'closure-invalid-unpaired')
    require(unpaired[:, :, 3].max() == 0, 'An unpaired closure must not silently use a default brush')
    results['cross_scope_and_unpaired_closures_are_not_executed'] = 'passed'
    return results


def curve_brushes(out):
    obj, tree, quick, first_settings, first_parameters, first_output = scene()
    second_parameters, second_output = npr_lines.add_brush_closure(tree)
    source = next(node for node in tree.nodes if node.bl_idname == 'NPRLineNodeObjects')
    second_settings = make_settings(tree, source, second_output, ('wire',))
    for parameters, output, factor, offset, color in (
            (first_parameters, first_output, .25, -12, (1, 0, 0, 1)),
            (second_parameters, second_output, .75, 12, (0, 0, 1, 1))):
        curve = tree.nodes.new('ShaderNodeFloatCurve')
        for point in curve.mapping.curves[0].points:
            point.location.y = factor
        curve.mapping.update()
        multiply = tree.nodes.new('ShaderNodeMath')
        multiply.operation = 'MULTIPLY'
        multiply.inputs[1].default_value = 12
        tree.links.new(parameters.outputs['Factor'], curve.inputs['Value'])
        tree.links.new(curve.outputs[0], multiply.inputs[0])
        tree.links.new(multiply.outputs[0], socket(output, 'width'))
        set_value(output, 'position_offset', (0, offset))
        set_value(output, 'color', color)
    root = active_output(tree)
    tree.links.new(first_settings.outputs[0], socket(root, 'plan'))
    first = ink(out, 'closure-first-curve-only')
    tree.links.new(second_settings.outputs[0], socket(root, 'plan'))
    second = ink(out, 'closure-second-curve-only')
    combine = tree.nodes.new('NPRLineNodeCombine')
    combine.mode = 'OVERLAY'
    tree.links.new(first_settings.outputs[0], socket(combine, 'plans'))
    tree.links.new(second_settings.outputs[0], socket(combine, 'plans'))
    tree.links.new(combine.outputs[0], socket(root, 'plan'))
    together = ink(out, 'closure-independent-curve-brushes')
    require(np.abs(together - first - second).max() < .003,
            'Different brushes must address their own Float Curve table when drawn together')
    return {'multiple_brush_curve_table_offsets': 'passed'}


def preview(out):
    obj, tree, quick, settings, start, end = scene()
    ink(out, 'preview-render-result')
    result = bpy.data.images['Render Result']
    bpy.context.scene.render.image_settings.media_type = 'IMAGE'
    bpy.context.scene.render.image_settings.file_format = 'PNG'
    result.save_render(str(out / 'render-result-before.png'), scene=bpy.context.scene)
    counts = lambda: tuple(len(c) for c in (bpy.data.scenes, bpy.data.objects, bpy.data.meshes,
                                           bpy.data.cameras, bpy.data.node_groups, bpy.data.images))
    before = counts()
    image = bpy.data.images.new('Closure mutable preview', 1, 1, float_buffer=True)
    image.colorspace_settings.name = 'Non-Color'
    texture = tree.nodes.new('ShaderNodeTexImage')
    texture.image = image
    tree.links.new(texture.outputs['Color'], socket(end, 'color'))
    before = counts()
    for color, channel in (((1, 0, 0, 1), 0), ((0, 1, 0, 1), 1)):
        image.pixels[:] = color
        image.update()
        swatch = npr_lines.output_brush_preview(end)
        npr_lines._update_output_brush_previews()
        pixels = np.asarray(swatch.image_pixels_float[:]).reshape(64, 192, 4)
        require((pixels[:, :, channel] - pixels[:, :, 2]).max() > .8, 'Preview must draw current evaluated ink')
        require(counts() == before, 'Preview must remove its disposable scenes and node trees')
    npr_lines.output_brush_preview(end, 384, 128)
    require(not npr_lines._region_preview_pending, 'Sidebar and inline swatches must share evaluated pixels')
    result.save_render(str(out / 'render-result-after.png'), scene=bpy.context.scene)
    require((out / 'render-result-before.png').read_bytes() == (out / 'render-result-after.png').read_bytes(),
            'Preview must preserve the user Render Result')
    discarded = bpy.data.node_groups.new('Deleted queued preview', 'NPRLineNodeTree')
    unused_parameters, unused_output = npr_lines.add_brush_closure(discarded)
    npr_lines.output_brush_preview(unused_output)
    bpy.data.node_groups.remove(discarded)
    npr_lines._update_output_brush_previews()
    require(not npr_lines._region_preview_errors and not npr_lines._region_preview_pending,
            'Deleting a queued node tree must safely discard its preview request')
    npr_lines.clear_brush_preview()
    return {'inline_preview_image_edits_shared_sizes_cleanup_render_result': 'passed'}


def groups(out):
    obj, tree, quick, settings, start, end = scene()
    group = bpy.data.node_groups.new('Reusable Brush Closure', 'NPRLineNodeTree')
    for name, kind in (('Color', 'NodeSocketColor'), ('Width', 'NodeSocketFloat')):
        group.interface.new_socket(name=name, in_out='INPUT', socket_type=kind)
    group.interface.new_socket(name='Brush', in_out='OUTPUT', socket_type='NodeSocketNPRLineBrush')
    gi, go = group.nodes.new('NodeGroupInput'), group.nodes.new('NodeGroupOutput')
    parameters, output = npr_lines.add_brush_closure(group)
    for name, identifier in (('Color', 'color'), ('Width', 'width')):
        group.links.new(gi.outputs[name], socket(output, identifier))
    group.links.new(output.outputs[0], go.inputs['Brush'])
    wrapper = bpy.data.node_groups.new('Nested Brush Closure', 'NPRLineNodeTree')
    for name, kind in (('Color', 'NodeSocketColor'), ('Width', 'NodeSocketFloat')):
        wrapper.interface.new_socket(name=name, in_out='INPUT', socket_type=kind)
    wrapper.interface.new_socket(name='Brush', in_out='OUTPUT', socket_type='NodeSocketNPRLineBrush')
    wi, wo = wrapper.nodes.new('NodeGroupInput'), wrapper.nodes.new('NodeGroupOutput')
    inner = wrapper.nodes.new('NPRLineNodeGroup')
    inner.node_tree = group
    for name in ('Color', 'Width'):
        wrapper.links.new(wi.outputs[name], inner.inputs[name])
    reroute = wrapper.nodes.new('NodeReroute')
    wrapper.links.new(inner.outputs[0], reroute.inputs[0])
    wrapper.links.new(reroute.outputs[0], wo.inputs[0])
    instances = []
    for color, width in (((1, 0, 0, 1), 4), ((0, 0, 1, 1), 8)):
        node = tree.nodes.new('NPRLineNodeGroup')
        node.node_tree = wrapper
        node.inputs['Color'].default_value = color
        node.inputs['Width'].default_value = width
        instances.append(node)
    tree.links.new(instances[0].outputs[0], socket(settings, 'visible_brush'))
    red = ink(out, 'nested-closure-red')
    tree.links.new(instances[1].outputs[0], socket(settings, 'visible_brush'))
    blue = ink(out, 'nested-closure-blue')
    require(red[:, :, 0].sum() > 300 and red[:, :, 2].sum() < 1 and
            blue[:, :, 2].sum() > red[:, :, 0].sum() * 1.7,
            'Nested closure groups and reroutes must preserve independent instance values')
    window = bpy.context.window_manager.windows[0]
    area = next(a for a in window.screen.areas if a.type == 'VIEW_3D')
    area.type = 'NODE_EDITOR'
    area.ui_type = 'NPRLineNodeTree'
    region = next(r for r in area.regions if r.type == 'WINDOW')
    counts = lambda: tuple(len(c) for c in (bpy.data.scenes, bpy.data.objects, bpy.data.meshes,
                                           bpy.data.cameras, bpy.data.node_groups, bpy.data.images))
    before = counts()
    with bpy.context.temp_override(window=window, area=area, region=region):
        space = area.spaces.active
        for node, channel in zip(instances, (0, 2)):
            space.path.start(tree)
            tree.nodes.active = node
            space.path.append(wrapper, node=node)
            wrapper.nodes.active = inner
            space.path.append(group, node=inner)
            swatch = npr_lines.output_brush_preview(output)
            # Leave the editor context before running the deferred callback.
            with bpy.context.temp_override(area=None, region=None):
                npr_lines._update_output_brush_previews()
            pixels = np.asarray(swatch.image_pixels_float[:]).reshape(64, 192, 4)
            other = 2 if channel == 0 else 0
            require((pixels[:, :, channel] - pixels[:, :, other]).max() > .8,
                    'Deferred nested closure preview must use the selected group instance')
        space.path.start(tree)
    area.type = 'VIEW_3D'
    require(counts() == before and not npr_lines._region_preview_errors,
            'Nested previews must clean every ancestor copy without errors')
    npr_lines.clear_brush_preview()
    return {'nested_closure_instances_reroutes_deferred_preview': 'passed'}


def sources(out):
    import test_geometry_line_brush_region_sources as s
    helpers.USE_CLOSURE = True
    results = {}
    for function in (s.composition, s.source_tracking, s.material_boundaries, s.style_interfaces, s.image_borders):
        results.update(function(out))
    import test_geometry_line_brush_region_viewport as v
    v.main(out)
    return results | {'persistent_viewport_sources_and_group_instances': 'passed'}


def reopen(out):
    bpy.ops.wm.open_mainfile(filepath=str(out / 'brush-closure.blend'))
    tree = bpy.context.scene.eevee.geometry_line_tree
    start = next(n for n in tree.nodes if n.bl_idname == 'NPRLineNodeBrushClosureInput')
    end = next(n for n in tree.nodes if n.bl_idname == 'NPRLineNodeBrushClosureOutput')
    require(start.paired_output == end and len(end.inputs) == 4 and not end.panel_states,
            'Cold reload must preserve the minimal paired closure')
    actual = ink(out, 'closure-cold-reopened')
    require(np.abs(actual - np.load(out / 'saved.npy')).max() < .003, 'Cold process reload must preserve pixels')
    return {'cold_reload_pairing_and_pixels': 'passed'}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True)
    parser.add_argument('--phase', choices=('core', 'groups', 'preview', 'sources', 'reopen', 'all'), default='all')
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    results = {}
    for phase, function in (('core', core), ('groups', groups), ('preview', preview), ('sources', sources), ('reopen', reopen)):
        if args.phase == phase or (args.phase == 'all' and phase != 'reopen'):
            results.update(function(out))
    if args.phase in ('core', 'all'):
        results.update(curve_brushes(out))
    results['build_hash'] = bpy.app.build_hash.decode()
    (out / ('results-' + args.phase + '.json')).write_text(json.dumps(results, indent=2), encoding='utf8')
    print('BRUSH_CLOSURE_TEST_OK ' + json.dumps(results), flush=True)


if __name__ == '__main__':
    main()
