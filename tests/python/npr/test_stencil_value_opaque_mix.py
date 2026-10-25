"""GPU coverage/depth dependency regression for stencil-driven opaque shader mixes."""

import argparse
import json
from pathlib import Path
import sys
import traceback

import bpy
import gpu
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_stencil_value as core


def fixture(kind='principled', mode='STANDARD', cache=False):
    scene = core.configure()
    bpy.ops.object.light_add(type='SUN', location=(0, 0, 4))
    bpy.context.object.data.energy = 2
    mat = core.material('OpaqueStencilMix')
    mat.stencil_mode = mode
    tree = mat.node_tree
    tree.nodes.remove(tree.nodes['Emission'])
    output = tree.nodes.get('Material Output')
    first = tree.nodes.new({'principled': 'ShaderNodeBsdfPrincipled',
                            'npr': 'ShaderNodePrincipledNPR',
                            'specular': 'ShaderNodeEeveeSpecular',
                            'emission': 'ShaderNodeEmission'}[kind])
    color = first.inputs.get('Base Color') or first.inputs.get('Color')
    color.default_value = (0.8, 0.05, 0.02, 1)
    second = tree.nodes.new('ShaderNodeBsdfDiffuse')
    second.inputs['Color'].default_value = (0.02, 0.7, 0.08, 1)
    mix = tree.nodes.new('ShaderNodeMixShader')
    tree.links.new(first.outputs[0], mix.inputs[1])
    tree.links.new(second.outputs[0], mix.inputs[2])
    surface = mix.outputs[0]
    if cache:
        # This consumer disables uniform-only tracking and exercises preprocessed-tree reuse
        # between prepass/surface passes. Its Color must still carry full value dependencies.
        rgb = tree.nodes.new('ShaderNodeShaderToRGB')
        tree.links.new(surface, rgb.inputs[0])
        emission = tree.nodes.new('ShaderNodeEmission')
        tree.links.new(rgb.outputs['Color'], emission.inputs['Color'])
        surface = emission.outputs[0]
    tree.links.new(surface, output.inputs['Surface'])
    value = tree.nodes.new('ShaderNodeStencilValue')
    info = tree.nodes.new('ShaderNodeRenderInfo')
    offset = tree.nodes.new('ShaderNodeVectorMath')
    offset.operation = 'ADD'
    offset.inputs[1].default_value = (0, 0.08, 0)
    tree.links.new(info.outputs['Frag Coord'], offset.inputs[0])
    tree.links.new(offset.outputs[0], value.inputs['UV'])
    face = core.plane('Receiver', mat)
    marker = core.plane('Marker', core.marker_material(), size=1.8, x=-0.45, z=0.3)
    return scene, mat, first, second, mix, value, face, marker


def connect_factor(tree, mix, source=None, constant=0):
    for link in list(mix.inputs[0].links):
        tree.links.remove(link)
    mix.inputs[0].default_value = constant
    if source is not None:
        tree.links.new(source, mix.inputs[0])


def check_regions(actual, zero, one, mask):
    # Compare stable interiors, excluding rasterized geometry and mask edge samples.
    covered = mask[..., 0] > 0.99
    clear = mask[..., 0] < 0.01
    for area, reference in ((covered, one), (clear, zero)):
        stable = area.copy()
        for axis in (0, 1):
            for shift in (-2, -1, 1, 2):
                stable &= np.roll(area, shift, axis=axis)
        stable[:8] = stable[-8:] = False
        stable[:, :8] = stable[:, -8:] = False
        assert np.count_nonzero(stable) > 100
        core.near(float(np.max(np.abs(actual[stable, :3] - reference[stable, :3]))),
                  0, tolerance=0.006)


def positive(out, report):
    for kind, cache in (('principled', False), ('principled', True), ('npr', False),
                        ('specular', False), ('emission', True)):
        scene, mat, first, second, mix, value, face, marker = fixture(kind, cache=cache)
        tree = mat.node_tree
        prefix = kind + ('_cached' if cache else '')
        # Use the same UV path to capture the exact sampled region as a reference.
        preview = core.reader_material(raw=True)
        pt = preview.node_tree
        pinfo = pt.nodes.new('ShaderNodeRenderInfo')
        poffset = pt.nodes.new('ShaderNodeVectorMath')
        poffset.operation = 'ADD'
        poffset.inputs[1].default_value = (0, 0.08, 0)
        pt.links.new(pinfo.outputs['Frag Coord'], poffset.inputs[0])
        pt.links.new(poffset.outputs[0], pt.nodes['Stencil Value'].inputs['UV'])
        face.data.materials[0] = preview
        mask = core.render(prefix + '_mask', out, report) * 15
        face.data.materials[0] = mat
        connect_factor(tree, mix, constant=0)
        zero = core.render(prefix + '_factor0', out, report)
        connect_factor(tree, mix, constant=1)
        one = core.render(prefix + '_factor1', out, report)
        assert np.max(np.abs(zero[16:-16, 16:-16, :3] - one[16:-16, 16:-16, :3])) > 0.1
        connect_factor(tree, mix, value.outputs[0])
        actual = core.render(prefix + '_stencil_mix', out, report)
        check_regions(actual, zero, one, mask)

        if kind != 'principled' or cache:
            continue
        # The screenshot's setup, half mix, nested closures, expanded groups and bundles.
        connect_factor(tree, mix, constant=0.5)
        half = core.render('principled_factor_half', out, report)
        source = core.math_node(tree, 'MULTIPLY', value.outputs[0], 0.5)
        connect_factor(tree, mix, source)
        check_regions(core.render('principled_stencil_half', out, report), zero, half, mask)
        nested = tree.nodes.new('ShaderNodeMixShader')
        nested.inputs[0].default_value = 0.5
        tree.links.new(second.outputs[0], nested.inputs[1])
        tree.links.new(second.outputs[0], nested.inputs[2])
        tree.links.new(nested.outputs[0], mix.inputs[2])
        connect_factor(tree, mix, value.outputs[0])
        check_regions(core.render('nested_opaque_mix', out, report), zero, one, mask)
        add = tree.nodes.new('ShaderNodeAddShader')
        tree.links.new(second.outputs[0], add.inputs[0])
        tree.links.new(second.outputs[0], add.inputs[1])
        tree.links.new(add.outputs[0], mix.inputs[2])
        connect_factor(tree, mix, constant=1)
        added = core.render('nested_add_reference', out, report)
        connect_factor(tree, mix, value.outputs[0])
        check_regions(core.render('nested_add_mix', out, report), zero, added, mask)
        tree.links.new(second.outputs[0], mix.inputs[2])
        group = bpy.data.node_groups.new('OpaqueMixGroup', 'ShaderNodeTree')
        for name, socket in (('Factor', 'NodeSocketFloat'), ('First', 'NodeSocketShader'),
                             ('Second', 'NodeSocketShader')):
            group.interface.new_socket(name=name, in_out='INPUT', socket_type=socket)
        group.interface.new_socket(name='Shader', in_out='OUTPUT', socket_type='NodeSocketShader')
        gi = group.nodes.new('NodeGroupInput')
        go = group.nodes.new('NodeGroupOutput')
        gm = group.nodes.new('ShaderNodeMixShader')
        for i, name in enumerate(('Factor', 'First', 'Second')):
            group.links.new(gi.outputs[name], gm.inputs[i])
        group.links.new(gm.outputs[0], go.inputs[0])
        call = tree.nodes.new('ShaderNodeGroup')
        call.node_tree = group
        reroute = tree.nodes.new('NodeReroute')
        tree.links.new(value.outputs[0], reroute.inputs[0])
        tree.links.new(reroute.outputs[0], call.inputs['Factor'])
        tree.links.new(first.outputs[0], call.inputs['First'])
        tree.links.new(second.outputs[0], call.inputs['Second'])
        tree.links.new(call.outputs[0], tree.nodes['Material Output'].inputs['Surface'])
        check_regions(core.render('group_reroute_mix', out, report), zero, one, mask)
        combine = tree.nodes.new('NodeCombineBundle')
        combine.bundle_items.new('FLOAT', 'Mask')
        separate = tree.nodes.new('NodeSeparateBundle')
        separate.bundle_items.new('FLOAT', 'Mask')
        tree.links.new(value.outputs[0], combine.inputs['Mask'])
        tree.links.new(combine.outputs[0], separate.inputs[0])
        tree.links.new(separate.outputs['Mask'], call.inputs['Factor'])
        check_regions(core.render('bundle_group_mix', out, report), zero, one, mask)
        constant = tree.nodes.new('ShaderNodeValue')
        constant.outputs[0].default_value = 1
        tree.links.new(constant.outputs[0], first.inputs['Alpha'])
        check_regions(core.render('folded_constant_alpha', out, report), zero, one, mask)
        constant.outputs[0].default_value = 0.5
        core.near(core.sample(core.render('changed_constant_alpha_rejected', out, report))[0], [1, 0, 1])
        constant.outputs[0].default_value = 1
        check_regions(core.render('constant_alpha_recovers', out, report), zero, one, mask)


def negative(out, report):
    for case in ('transparent', 'holdout', 'missing', 'alpha_half', 'alpha_dynamic',
                 'alpha_stencil', 'nested_transparent', 'rgb_to_alpha', 'rgb_to_depth',
                 'rgb_to_displacement', 'rgb_to_tessellation', 'custom_alpha'):
        scene, mat, first, second, mix, value, face, marker = fixture()
        tree = mat.node_tree
        connect_factor(tree, mix, value.outputs[0])
        if case in ('transparent', 'holdout', 'nested_transparent'):
            other = tree.nodes.new('ShaderNodeHoldout' if case == 'holdout' else 'ShaderNodeBsdfTransparent')
            source = other.outputs[0]
            if case == 'nested_transparent':
                nested = tree.nodes.new('ShaderNodeMixShader')
                tree.links.new(source, nested.inputs[1])
                tree.links.new(second.outputs[0], nested.inputs[2])
                source = nested.outputs[0]
            tree.links.new(source, mix.inputs[2])
        elif case == 'missing':
            tree.links.remove(mix.inputs[2].links[0])
        elif case == 'alpha_half':
            first.inputs['Alpha'].default_value = 0.5
        elif case == 'alpha_dynamic':
            tree.links.new(tree.nodes.new('ShaderNodeTexNoise').outputs['Fac'], first.inputs['Alpha'])
        elif case == 'alpha_stencil':
            tree.links.new(value.outputs[0], first.inputs['Alpha'])
        elif case == 'custom_alpha':
            text = bpy.data.texts.new('opaque_mix_alpha.glsl')
            text.write('void coverage(float mask, out float alpha) { alpha = 1.0; }')
            function = tree.nodes.new('ShaderNodeGLSLFunction')
            function.source_mode = 'INTERNAL'
            function.script = text
            function.function_name = 'coverage'
            tree.update_tag()
            tree.interface_update(bpy.context)
            bpy.context.view_layer.update()
            tree.links.new(value.outputs[0], function.inputs['mask'])
            tree.links.new(function.outputs['alpha'], first.inputs['Alpha'])
        else:
            rgb = tree.nodes.new('ShaderNodeShaderToRGB')
            tree.links.new(mix.outputs[0], rgb.inputs[0])
            receiver = tree.nodes.new('ShaderNodeBsdfPrincipled')
            output = tree.nodes['Material Output']
            tree.links.new(receiver.outputs[0], output.inputs['Surface'])
            if case == 'rgb_to_alpha':
                tree.links.new(rgb.outputs['Color'], receiver.inputs['Alpha'])
            elif case == 'rgb_to_depth':
                tree.links.new(rgb.outputs['Color'], output.inputs['Depth Offset'])
            elif case == 'rgb_to_displacement':
                mat.displacement_method = 'BOTH'
                tree.links.new(rgb.outputs['Color'], output.inputs['Displacement'])
            else:
                tess = tree.nodes.new('ShaderNodeTessellationControl')
                tree.links.new(rgb.outputs['Color'], tess.inputs['Max Level'])
                tree.links.new(tess.outputs[0], output.inputs['Tessellation'])
        core.near(core.sample(core.render('reject_' + case, out, report))[0], [1, 0, 1])


def stages(out, report):
    for method in ('DEFERRED', 'FORWARD', 'REFRACTION', 'EARLY_MARKER', 'MARKER_ONLY'):
        scene, mat, first, second, mix, value, face, marker = fixture('emission', cache=True)
        tree = mat.node_tree
        # Use emission at both endpoints for exact color assertions in every supported stage.
        tree.nodes.remove(second)
        second = tree.nodes.new('ShaderNodeEmission')
        second.inputs['Color'].default_value = (0.02, 0.7, 0.08, 1)
        tree.links.new(second.outputs[0], mix.inputs[2])
        connect_factor(tree, mix, value.outputs[0])
        if method == 'FORWARD':
            mat.surface_render_method = 'BLENDED'
        elif method == 'REFRACTION':
            mat.use_raytrace_refraction = True
            mat.surface_render_method = 'DITHERED'
            scene.eevee.use_raytracing = True
        elif method in ('EARLY_MARKER', 'MARKER_ONLY'):
            mat.stencil_mode = method
            mat.use_stencil = True
            mat.stencil_reference = 2
            mat.stencil_write_mask = 15
            mat.stencil_test = 'ALWAYS'
            mat.stencil_pass_op = 'REPLACE'
            mat.stencil_order = -1
        if method == 'MARKER_ONLY':
            face.location.z = 0.1
            core.plane('SnapshotPreview', core.reader_material(raw=True), z=-0.1)
            image = core.render('marker_only_opaque_mix', out, report)
            core.near(core.sample(image, -0.45)[0], 1 / 15)
            core.near(core.sample(image, 1.4)[0], 2 / 15)
            mat.use_stencil = False
            image = core.render('marker_only_disabled_mix', out, report)
            core.near(core.sample(image, 1.4)[0], 0)
        else:
            image = core.render(method.lower() + '_opaque_mix', out, report)
            core.near(core.sample(image, -0.45)[0], (0.02, 0.7, 0.08), tolerance=0.003)
            # An Early Marker receiver writes 2 outside the other marker, clamped to Factor=1.
            expected = (0.02, 0.7, 0.08) if method == 'EARLY_MARKER' else (0.8, 0.05, 0.02)
            core.near(core.sample(image, 1.4)[0], expected, tolerance=0.003)

    scene, mat, first, second, mix, value, face, marker = fixture()
    tree = mat.node_tree
    connect_factor(tree, mix, value.outputs[0])
    first.inputs['Alpha'].default_value = 1
    first.inputs['Alpha'].keyframe_insert(data_path='default_value', frame=1)
    first.inputs['Alpha'].default_value = 0.5
    first.inputs['Alpha'].keyframe_insert(data_path='default_value', frame=2)
    previous = None
    for index, frame in enumerate((1, 2, 1)):
        scene.frame_set(frame)
        pixels = core.render(f'alpha_animation_{index}_{frame}', out, report)
        if frame == 2:
            core.near(core.sample(pixels)[0], [1, 0, 1])
        elif previous is not None:
            core.near(float(np.max(np.abs(pixels - previous))), 0)
        else:
            assert not np.allclose(core.sample(pixels)[0], [1, 0, 1])
            previous = pixels


def active_outputs(out, report):
    scene, mat, first, second, mix, value, face, marker = fixture(cache=True)
    tree = mat.node_tree
    connect_factor(tree, mix, value.outputs[0])
    active = tree.nodes['Material Output']
    reference = core.render('active_output_reference', out, report)
    inactive = tree.nodes.new('ShaderNodeOutputMaterial')
    tree.links.new(mix.outputs[0], inactive.inputs['Surface'])
    tess = tree.nodes.new('ShaderNodeTessellationControl')
    tree.links.new(value.outputs[0], tess.inputs['Max Level'])
    tree.links.new(tess.outputs[0], inactive.inputs['Tessellation'])
    active.is_active_output = True
    actual = core.render('inactive_tessellation_output_ignored', out, report)
    core.near(float(np.max(np.abs(actual - reference))), 0)
    inactive.is_active_output = True
    core.near(core.sample(core.render('activated_tessellation_output_rejected', out, report))[0], [1, 0, 1])
    active.is_active_output = True
    actual = core.render('active_output_mix_recovers', out, report)
    core.near(float(np.max(np.abs(actual - reference))), 0)
    inactive.target = 'EEVEE'
    active.is_active_output = True
    core.near(core.sample(core.render('eevee_output_overrides_all', out, report))[0], [1, 0, 1])
    inactive.target = 'CYCLES'
    actual = core.render('cycles_output_ignored', out, report)
    core.near(float(np.max(np.abs(actual - reference))), 0)


def preview(out, report, asset_dir):
    scene = core.configure()
    receiver = core.plane('Snapshot Preview', core.reader_material('Stencil Preview - Value 0 to 15', raw=True))
    grayscale = receiver.active_material
    # Four bits in overlapping stripes demonstrate Reference versus the final combined value.
    for bit in range(4):
        mat = core.marker_material('Bit ' + str(bit), reference=1 << bit, order=bit)
        mat.stencil_write_mask = 1 << bit
        for label in range(16):
            if label & (1 << bit):
                obj = core.plane(f'Bit {bit} Label {label:02}', mat, size=1,
                                 x=(label + 0.5) * 0.225 - 1.8, z=0.2)
                obj.scale = (0.225, 3.2, 1)
    pixels = core.render('preview_all_values', out, report)
    for label in range(16):
        core.near(core.sample(pixels, (label + 0.5) * 0.225 - 1.8)[0], label / 15)
    for bit in range(4):
        bit_mat = core.material(f'Stencil Preview - Bit {bit}')
        bit_mat.use_fake_user = True
        tree = bit_mat.node_tree
        value = tree.nodes.new('ShaderNodeStencilValue')
        quotient = core.math_node(tree, 'DIVIDE', value.outputs[0], 1 << bit)
        integer = core.math_node(tree, 'FLOOR', quotient, 0)
        result = core.math_node(tree, 'MODULO', integer, 2)
        tree.links.new(result, tree.nodes['Emission'].inputs['Color'])
        receiver.data.materials[0] = bit_mat
        pixels = core.render('preview_bit_' + str(bit), out, report)
        for label in range(16):
            core.near(core.sample(pixels, (label + 0.5) * 0.225 - 1.8)[0], (label >> bit) & 1)
    receiver.data.materials[0] = grayscale
    if asset_dir:
        asset_dir.mkdir(parents=True, exist_ok=True)
        core.render('preview_grayscale_restored', out, report)
        scene.render.image_settings.file_format = 'PNG'
        bpy.data.images['Render Result'].save_render(str(out / 'preview_all_values.png'))
        tree = receiver.active_material.node_tree
        tree.nodes['Stencil Value'].location = (-480, 0)
        next(n for n in tree.nodes if n.type == 'MATH').location = (-260, 0)
        tree.nodes['Emission'].location = (-40, 0)
        tree.nodes['Material Output'].location = (180, 0)
        bpy.context.view_layer.objects.active = receiver
        bpy.ops.object.select_all(action='DESELECT')
        receiver.select_set(True)
        scene['preview_help'] = 'Value / 15 -> Emission previews labels. floor(Value / 2^bit) modulo 2 previews a bit.'
        bpy.ops.wm.save_as_mainfile(filepath=str(asset_dir / 'stencil_snapshot_preview.blend'))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--asset-dir', type=Path)
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    args.output_dir.mkdir(parents=True, exist_ok=True)
    gpu.init()
    report = {'version': bpy.app.version_string, 'build_hash': bpy.app.build_hash.decode(),
              'build_time': bpy.app.build_date.decode() + ' ' + bpy.app.build_time.decode(),
              'backend': gpu.platform.backend_type_get(), 'renderer': gpu.platform.renderer_get(),
              'executable': bpy.app.binary_path, 'cases': {}, 'passed': False}
    try:
        positive(args.output_dir, report)
        negative(args.output_dir, report)
        stages(args.output_dir, report)
        active_outputs(args.output_dir, report)
        preview(args.output_dir, report, args.asset_dir)
        report['passed'] = True
        print('STENCIL_VALUE_OPAQUE_MIX_PASSED', flush=True)
    except Exception:
        report['error'] = traceback.format_exc()
        raise
    finally:
        (args.output_dir / 'verification.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
