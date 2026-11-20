"""Standalone Brush Output, legacy field conversion and deferred inline preview."""

import argparse
import hashlib
import json
from pathlib import Path
import sys

import bpy
import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from test_geometry_line_brush_regions import line_scene, ink, centroid, socket, set_value, require
from bl_operators import npr_lines


def field_operation_parity(out):
    _, tree, _, settings, _, output = line_scene()
    tree.links.new(output.outputs[0], socket(settings, 'visible_brush'))
    reference = ink(out, 'math-width-reference')
    require(reference[:, :, 3].sum() > 100, 'Math fixture must draw real ink')
    math = tree.nodes.new('ShaderNodeMath')
    add = tree.nodes.new('ShaderNodeMath')
    add.operation = 'ADD'
    tree.links.new(math.outputs[0], add.inputs[0])
    tree.links.new(add.outputs[0], socket(output, 'width'))
    for operation, values, adjustment in (('POWER', (-2, 3), 12), ('ARCSINE', (2, 0), 4)):
        math.operation = operation
        for item, value in zip(math.inputs, values):
            item.default_value = value
        add.inputs[1].default_value = adjustment
        actual = ink(out, 'math-' + operation)
        require(np.abs(actual - reference).max() < .003,
                'Brush fields must match Blender Math semantics for ' + operation)
    tree.links.remove(socket(output, 'width').links[0])
    set_value(output, 'position_offset', (5, -np.sqrt(.75) * 10, 0))
    reference = ink(out, 'refract-normal-reference')
    require(reference[:, :, 3].sum() > 100, 'Vector fixture must draw real ink')
    refract = tree.nodes.new('ShaderNodeVectorMath')
    refract.operation = 'REFRACT'
    refract.inputs[0].default_value = (1, 0, 0)
    refract.inputs[1].default_value = (0, 2, 0)
    refract.inputs[3].default_value = .5
    scale = tree.nodes.new('ShaderNodeVectorMath')
    scale.operation = 'SCALE'
    scale.inputs[3].default_value = 10
    tree.links.new(refract.outputs['Vector'], scale.inputs[0])
    tree.links.new(scale.outputs['Vector'], socket(output, 'position_offset'))
    actual = ink(out, 'refract-nonunit-normal')
    require(np.abs(actual - reference).max() < .003,
            'Brush Vector Math must normalize the refraction normal like Blender')
    return 'passed'


def main(out, legacy=None, reference=None):
    results = {}
    obj, tree, quick, settings, _, output = line_scene()
    require(not any(n.bl_idname == 'NPRLineNodeBrushInput' for n in tree.nodes),
            'Standalone output must work without an input node')
    require(output.mode == 'SIMPLE' and output.use_field_inputs,
            'New output must own the brush settings and use field inputs')
    require(not any(s.identifier == 'brush' for s in output.inputs),
            'Output must have no incoming Brush')
    require(all(hasattr(output, name) for name in ('width_curve', 'opacity_curve', 'stroke_mode',
                                                  'preview_length', 'brush_preview_icon')),
            'Output must own all brush properties and its inline preview')
    tree.links.new(output.outputs[0], socket(settings, 'visible_brush'))
    base = ink(out, 'standalone')
    set_value(output, 'position_offset', (0, 13, 0))
    moved = ink(out, 'offset')
    require(abs(abs(centroid(moved)[1] - centroid(base)[1]) - 13) < .6,
            'Output Position Offset must move the source stroke')
    info = tree.nodes.new('NPRLineNodeStrokeInfo')
    require(len(info.inputs) == 0, 'Stroke Info must only provide data outputs')
    vector = tree.nodes.new('ShaderNodeSeparateXYZ')
    tree.links.new(info.outputs['Position'], vector.inputs[0])
    # Factor and Position are original data; changing output offset cannot feed back.
    scale = tree.nodes.new('ShaderNodeMath')
    scale.operation = 'MULTIPLY'
    scale.inputs[1].default_value = .1
    tree.links.new(vector.outputs['X'], scale.inputs[0])
    tree.links.new(scale.outputs[0], socket(output, 'width'))
    varied = ink(out, 'original-position-width')
    require(varied[:, :, 3].sum() > base[:, :, 3].sum(),
            'Implicit Position must expose source arc distance to fields')
    tree.links.remove(socket(output, 'width').links[0])
    set_value(output, 'position_offset', (0, 0, 0))
    scene = bpy.context.scene
    render_result = bpy.data.images.get('Render Result')
    original_slot = render_result.render_slots.active_index
    # PNG is deterministic and includes the current scene's saved render result.
    scene.render.image_settings.media_type = 'IMAGE'
    scene.render.image_settings.file_format = 'PNG'
    render_result.save_render(str(out / 'render-result-before.png'), scene=scene)
    counts = tuple(len(c) for c in (bpy.data.scenes, bpy.data.objects, bpy.data.meshes,
                                    bpy.data.cameras, bpy.data.node_groups, bpy.data.images))
    for color, label in (((1, 0, 0, 1), 'red'), ((0, 0, 1, 1), 'blue')):
        set_value(output, 'color', color)
        preview = npr_lines.output_brush_preview(output)
        npr_lines._update_output_brush_previews()
        pixels = np.asarray(preview.image_pixels_float[:]).reshape(64, 192, 4)
        # Checker values are achromatic; channel differences identify actual ink.
        dominant, other = (0, 2) if label == 'red' else (2, 0)
        require((pixels[:, :, dominant] - pixels[:, :, other]).max() > .8,
                'Inline preview must display updated ' + label + ' evaluated ink')
        require(counts == tuple(len(c) for c in (bpy.data.scenes, bpy.data.objects,
                            bpy.data.meshes, bpy.data.cameras, bpy.data.node_groups, bpy.data.images)),
                'Inline preview must not retain temporary scene data')
    render_result.save_render(str(out / 'render-result-after.png'), scene=scene)
    require((out / 'render-result-before.png').read_bytes() ==
            (out / 'render-result-after.png').read_bytes(),
            'Automatic preview must preserve the previous Render Result pixels')
    require(bpy.context.scene == scene and render_result.render_slots.active_index == original_slot,
            'Automatic preview must preserve the active scene and render slot')
    npr_lines.output_brush_preview(output, 384, 128)
    require(not npr_lines._region_preview_pending,
            'Sidebar and inline sizes must reuse one evaluated stroke')
    results['preview_preserves_render_result_and_shares_sizes'] = 'passed'

    image = bpy.data.images.new('Mutable preview source', 1, 1, alpha=True, float_buffer=True)
    image.colorspace_settings.name = 'Non-Color'
    texture = tree.nodes.new('ShaderNodeTexImage')
    texture.image = image
    tree.links.new(texture.outputs['Color'], socket(output, 'color'))
    for color, dominant in (((1, 0, 0, 1), 0), ((0, 1, 0, 1), 1)):
        image.pixels[:] = color
        image.update()
        npr_lines.output_brush_preview(output)
        require(npr_lines._region_preview_pending, 'Image pixel edits must queue a preview update')
        npr_lines._update_output_brush_previews()
        preview = npr_lines.output_brush_preview(output)
        pixels = np.asarray(preview.image_pixels_float[:]).reshape(64, 192, 4)
        require((pixels[:, :, dominant] - pixels[:, :, 2]).max() > .8,
                'Preview must display the current image pixel values')
        require(not npr_lines._region_preview_pending,
                'Unchanged images must reuse the preview without periodic rendering')
    results['image_content_revision_and_idle_cache'] = 'passed'

    original = output
    original.preview_length = 384
    original.use_width_curve = True
    original.width_curve.curves[0].points[-1].location.y = .7
    original.width_curve.update()
    bpy.ops.node.npr_line_make_brush_unique(node_name=settings.name)
    output = socket(settings, 'visible_brush').links[0].from_node
    require(output != original and output.bl_idname == original.bl_idname,
            'Single-user copies must support the standalone output')
    require(output.preview_length == 384 and output.use_width_curve and
            abs(output.width_curve.curves[0].points[-1].location.y - .7) < 1e-6 and
            socket(output, 'color').links[0].from_node == texture,
            'Output copy must preserve curves, preview length and connected fields')
    output.width_curve.curves[0].points[-1].location.y = .2
    output.width_curve.update()
    require(abs(original.width_curve.curves[0].points[-1].location.y - .7) < 1e-6,
            'Output copies must own independent curve data')
    results['shared_brush_copy_implementation'] = 'passed'
    require(not npr_lines._region_preview_errors, 'Inline preview must not hide evaluation errors')
    results['standalone_implicit_fields_inline_updates_cleanup'] = 'passed'

    # Drawing has an editor/group context; the deferred timer deliberately has none.
    # Keep the selected instance's complete upstream field graph for that timer.
    npr_lines.clear_brush_preview()
    obj, tree, quick, settings, _, output = line_scene()
    group = bpy.data.node_groups.new('Preview group', 'NPRLineNodeTree')
    group.interface.new_socket(name='Color', in_out='INPUT', socket_type='NodeSocketColor')
    group.interface.new_socket(name='Brush', in_out='OUTPUT', socket_type='NodeSocketNPRLineBrush')
    group_in = group.nodes.new('NodeGroupInput')
    group_out = group.nodes.new('NodeGroupOutput')
    grouped_output = group.nodes.new('NPRLineNodeBrushOutput')
    socket(grouped_output, 'width').default_value = 8
    group.links.new(group_in.outputs['Color'], socket(grouped_output, 'color'))
    group.links.new(grouped_output.outputs[0], group_out.inputs['Brush'])
    instance = tree.nodes.new('NPRLineNodeGroup')
    instance.node_tree = group
    color = tree.nodes.new('ShaderNodeMix')
    color.data_type = 'RGBA'
    color.inputs[0].default_value = .5
    color.inputs[6].default_value = (0, 1, 0, 1)
    color.inputs[7].default_value = (0, 1, 0, 1)
    tree.links.new(color.outputs[2], instance.inputs['Color'])
    window = bpy.context.window_manager.windows[0]
    area = next(a for a in window.screen.areas if a.type == 'VIEW_3D')
    area.type = 'NODE_EDITOR'
    area.ui_type = 'NPRLineNodeTree'
    region = next(r for r in area.regions if r.type == 'WINDOW')
    with bpy.context.temp_override(window=window, area=area, region=region):
        space = area.spaces.active
        space.path.start(tree)
        tree.nodes.active = instance
        space.path.append(group, node=instance)
        preview = npr_lines.output_brush_preview(grouped_output)
    npr_lines._update_output_brush_previews()
    pixels = np.asarray(preview.image_pixels_float[:]).reshape(64, 192, 4)
    require((pixels[:, :, 1] - pixels[:, :, 0]).max() > .8,
            'Deferred group preview must retain the edited instance and its upstream Mix')
    with bpy.context.temp_override(window=window, area=area, region=region):
        space.path.pop()
        other = tree.nodes.new('NPRLineNodeGroup')
        other.node_tree = group
        other.inputs['Color'].default_value = (0, 0, 1, 1)
        tree.nodes.active = other
        space.path.append(group, node=other)
        blue_preview = npr_lines.output_brush_preview(grouped_output)
    npr_lines._update_output_brush_previews()
    blue = np.asarray(blue_preview.image_pixels_float[:]).reshape(64, 192, 4)
    require((blue[:, :, 2] - blue[:, :, 1]).max() > .8,
            'Two instances of one group must keep independent preview caches')
    with bpy.context.temp_override(window=window, area=area, region=region):
        space.path.pop()
        tree.nodes.active = instance
        space.path.append(group, node=instance)
        same_preview = npr_lines.output_brush_preview(grouped_output, 384, 128)
        require(not npr_lines._region_preview_pending,
                'Returning to an unchanged group instance must reuse its evaluated stroke')
        color.inputs[6].default_value = color.inputs[7].default_value = (1, 0, 0, 1)
        npr_lines.output_brush_preview(grouped_output)
    npr_lines._update_output_brush_previews()
    pixels = np.asarray(preview.image_pixels_float[:]).reshape(64, 192, 4)
    require((pixels[:, :, 0] - pixels[:, :, 1]).max() > .8,
            'Upstream edits outside the group must invalidate its deferred preview')
    with bpy.context.temp_override(window=window, area=area, region=region):
        space.path.pop()
        # A second group boundary must preserve the same instance values.
        wrapper = bpy.data.node_groups.new('Nested preview', 'NPRLineNodeTree')
        wrapper.interface.new_socket(name='Color', in_out='INPUT', socket_type='NodeSocketColor')
        wrapper.interface.new_socket(name='Brush', in_out='OUTPUT', socket_type='NodeSocketNPRLineBrush')
        wrapper_in = wrapper.nodes.new('NodeGroupInput')
        wrapper_out = wrapper.nodes.new('NodeGroupOutput')
        inner = wrapper.nodes.new('NPRLineNodeGroup')
        inner.node_tree = group
        wrapper.links.new(wrapper_in.outputs[0], inner.inputs['Color'])
        wrapper.links.new(inner.outputs[0], wrapper_out.inputs[0])
        outer = tree.nodes.new('NPRLineNodeGroup')
        outer.node_tree = wrapper
        outer.inputs['Color'].default_value = (0, 1, 0, 1)
        tree.nodes.active = outer
        space.path.append(wrapper, node=outer)
        wrapper.nodes.active = inner
        space.path.append(group, node=inner)
        nested_preview = npr_lines.output_brush_preview(grouped_output)
    nested_counts = tuple(len(c) for c in (bpy.data.scenes, bpy.data.objects, bpy.data.meshes,
                                           bpy.data.cameras, bpy.data.node_groups, bpy.data.images))
    npr_lines._update_output_brush_previews()
    nested = np.asarray(nested_preview.image_pixels_float[:]).reshape(64, 192, 4)
    require((nested[:, :, 1] - nested[:, :, 0]).max() > .8,
            'Nested groups must retain field inputs in the deferred preview')
    require(nested_counts == tuple(len(c) for c in (bpy.data.scenes, bpy.data.objects, bpy.data.meshes,
                                           bpy.data.cameras, bpy.data.node_groups, bpy.data.images)),
            'Nested previews must clean every copied ancestor tree')
    with bpy.context.temp_override(window=window, area=area, region=region):
        space.path.pop()
        space.path.pop()
    area.type = 'VIEW_3D'
    require(not npr_lines._region_preview_errors, 'Group previews must complete without errors')
    results['deferred_group_instance_context'] = 'passed'
    results['native_math_edge_cases'] = field_operation_parity(out)

    if legacy:
        legacy_hash = hashlib.sha256(Path(legacy).read_bytes()).hexdigest()
        bpy.ops.wm.open_mainfile(filepath=str(legacy))
        tree = bpy.context.scene.eevee.geometry_line_tree
        require(not any(n.bl_idname in {'NPRLineNodeBrushInput', 'NPRLineNodeSetPosition',
                                       'NPRLineNodeResample'} for n in tree.nodes),
                'Opening the previous example must remove the obsolete Brush chain')
        require(all(not n.inputs for n in tree.nodes if n.bl_idname == 'NPRLineNodeStrokeInfo'),
                'Converted Stroke Info must expose only original data')
        actual = ink(out, 'migrated-example')
        if reference:
            error = float(np.abs(actual - np.load(reference)).max())
            require(error < .003, 'Migrated example pixels must match: ' + str(error))
            results['legacy_example_max_pixel_error'] = error
        results['legacy_load_conversion'] = 'passed'
        require(hashlib.sha256(Path(legacy).read_bytes()).hexdigest() == legacy_hash,
                'In-memory conversion must preserve the original example file')
    npr_lines.clear_brush_preview()
    (out / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf8')
    print('BRUSH_OUTPUT_TEST_OK ' + json.dumps(results), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True)
    parser.add_argument('--legacy')
    parser.add_argument('--reference')
    parser.add_argument('--fields-only', action='store_true')
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    if args.fields_only:
        print('BRUSH_FIELD_OPERATIONS_OK ' + field_operation_parity(out), flush=True)
    else:
        main(out, args.legacy, args.reference)
