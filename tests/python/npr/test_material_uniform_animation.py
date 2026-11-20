# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Persistent-viewport pixel parity and material-uniform animation regression.

Run the baseline and candidate with the same CASES/FRAMES/ROUNDS settings and isolated user
resources. NPR_UNIFORM_BASELINE=1 records the unoptimized reference without requiring counters.
NPR_UNIFORM_REFERENCE_DIR points the candidate at that output. The default stream is 240 frames,
three rounds, followed in each round by reverse/jump probes. No final renders occur in that stream.

Optional environment: NPR_UNIFORM_CASES, NPR_UNIFORM_FRAMES, NPR_UNIFORM_ROUNDS,
NPR_UNIFORM_WIDTH/HEIGHT, NPR_UNIFORM_TRANSITIONS=0. BLENDER_TEST_OUTPUT_DIR selects the output.
"""

import hashlib
import json
import math
import os
from pathlib import Path
import re
import sys
import time
import traceback

import bpy
import gpu
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import OUT, background_child, glsl, scene

FRAMES = int(os.environ.get('NPR_UNIFORM_FRAMES', '240'))
ROUNDS = int(os.environ.get('NPR_UNIFORM_ROUNDS', '3'))
WIDTH = int(os.environ.get('NPR_UNIFORM_WIDTH', '64'))
HEIGHT = int(os.environ.get('NPR_UNIFORM_HEIGHT', '48'))
BASELINE = os.environ.get('NPR_UNIFORM_BASELINE', '0') == '1'
TRANSITIONS = os.environ.get('NPR_UNIFORM_TRANSITIONS', '1') == '1'
TRANSITIONS_ONLY = os.environ.get('NPR_UNIFORM_TRANSITIONS_ONLY', '0') == '1'
assert not TRANSITIONS_ONLY or TRANSITIONS, 'Transition-only recovery requires transitions enabled'
REFERENCE = os.environ.get('NPR_UNIFORM_REFERENCE_DIR')
REFERENCE = Path(REFERENCE) if REFERENCE else None
SELFDIFF = os.environ.get('NPR_UNIFORM_SELFDIFF_DIR')
SELFDIFF = Path(SELFDIFF) if SELFDIFF else None
assert SELFDIFF is None or REFERENCE is not None, 'Self-diff evidence requires a primary reference'
SUPPORTED = {'value', 'rgb', 'normal_strength', 'emission', 'nested', 'group_input', 'static_image'}
FALLBACK = {'glsl', 'driver', 'nla', 'image_sequence', 'external_id', 'folded',
            'shared_mix', 'shared_shader_to_rgb'}
CASES = os.environ.get('NPR_UNIFORM_CASES',
                      'value,rgb,normal_strength,emission,nested,group_input,static_image,'
                      'glsl,driver,nla,image_sequence,external_id,folded,'
                      'shared_mix,shared_shader_to_rgb').split(',')
assert FRAMES >= 4 and ROUNDS > 0 and WIDTH >= 16 and HEIGHT >= 16
assert set(CASES) <= SUPPORTED | FALLBACK, CASES


def scalar(frame):
    # Both genuinely changing spans and exact plateaus are present, also in shortened runs.
    phase = (frame - 1) / max(1, FRAMES - 1)
    if .32 <= phase <= .43 or .72 <= phase <= .80:
        return .35
    return .15 + .55 * (.5 + .5 * math.sin(frame * .173))


def color(frame):
    value = scalar(frame)
    return (value, .8 - value * .6, .12 + value * .4, 1)


def animate_socket(socket, values):
    for frame in range(1, FRAMES + 1):
        socket.default_value = values(frame)
        socket.keyframe_insert('default_value', frame=frame)


def value_node(tree, name='Animated Value'):
    node = tree.nodes.new('ShaderNodeValue')
    node.name = name
    animate_socket(node.outputs[0], scalar)
    return node


def rgb_node(tree):
    node = tree.nodes.new('ShaderNodeRGB')
    node.name = 'Animated RGB'
    animate_socket(node.outputs[0], color)
    return node


def shader_group(name, inputs=False):
    group = bpy.data.node_groups.new(name, 'ShaderNodeTree')
    if inputs:
        group.interface.new_socket(name='Gain', in_out='INPUT', socket_type='NodeSocketFloat')
        group.interface.new_socket(name='Tint', in_out='INPUT', socket_type='NodeSocketColor')
    group.interface.new_socket(name='Shader', in_out='OUTPUT', socket_type='NodeSocketShader')
    output = group.nodes.new('NodeGroupOutput')
    return group, output


def repeated_groups(tree, output, use_inputs):
    inner, inner_output = shader_group('Shared inner', inputs=use_inputs)
    emission = inner.nodes.new('ShaderNodeEmission')
    if use_inputs:
        source = inner.nodes.new('NodeGroupInput')
        gain, tint = source.outputs['Gain'], source.outputs['Tint']
    else:
        gain, tint = value_node(inner).outputs[0], rgb_node(inner).outputs[0]
    for source, target in ((gain, emission.inputs['Strength']), (tint, emission.inputs['Color'])):
        reroute = inner.nodes.new('NodeReroute')
        inner.links.new(source, reroute.inputs[0])
        inner.links.new(reroute.outputs[0], target)
    inner.links.new(emission.outputs[0], inner_output.inputs['Shader'])

    outer, outer_output = shader_group('Repeated outer', inputs=use_inputs)
    source = outer.nodes.new('NodeGroupInput') if use_inputs else None
    add = outer.nodes.new('ShaderNodeAddShader')
    for index in range(2):
        instance = outer.nodes.new('ShaderNodeGroup')
        instance.node_tree = inner
        if use_inputs:
            for name in ('Gain', 'Tint'):
                reroute = outer.nodes.new('NodeReroute')
                outer.links.new(source.outputs[name], reroute.inputs[0])
                outer.links.new(reroute.outputs[0], instance.inputs[name])
        outer.links.new(instance.outputs[0], add.inputs[index])
    outer.links.new(add.outputs[0], outer_output.inputs[0])
    gain = value_node(tree).outputs[0] if use_inputs else None
    tint = rgb_node(tree).outputs[0] if use_inputs else None
    add = tree.nodes.new('ShaderNodeAddShader')
    for index in range(2):
        instance = tree.nodes.new('ShaderNodeGroup')
        instance.node_tree = outer
        if use_inputs:
            tree.links.new(gain, instance.inputs['Gain'])
            tree.links.new(tint, instance.inputs['Tint'])
        tree.links.new(instance.outputs[0], add.inputs[index])
    tree.links.new(add.outputs[0], output.inputs['Surface'])


def make_image(case_dir, index):
    image = bpy.data.images.new('Uniform fallback image', 2, 2, float_buffer=True)
    image.colorspace_settings.name = 'Non-Color'
    image.pixels = list((.2 + .3 * index, .25, .35, 1)) * 4
    image.file_format = 'PNG'
    image.filepath_raw = str(case_dir / f'image_{index:03d}.png')
    image.save()
    path = image.filepath_raw
    bpy.data.images.remove(image)
    return path


def build_scene(case, case_dir):
    s, material, emission = scene()
    material.name = 'Uniform fixture material'
    s.name = 'Uniform fixture scene'
    s.eevee.taa_samples = 1
    s.eevee.use_taa_reprojection = False
    s.eevee.use_outline = False
    s.eevee.use_shadows = False
    s.eevee.use_performance_profiler = True
    s.eevee.performance_profiler_average_window = 1
    s.render.resolution_x, s.render.resolution_y = WIDTH, HEIGHT
    s.frame_start, s.frame_end = 1, FRAMES
    tree = material.node_tree
    output = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeOutputMaterial')
    emission.name = 'Fixture Emission'
    emission.inputs['Color'].default_value = (.2, .5, .8, 1)
    if case in {'value', 'glsl', 'driver', 'nla', 'image_sequence', 'static_image', 'external_id', 'folded',
                'shared_mix', 'shared_shader_to_rgb'}:
        value = value_node(tree)
        source = value.outputs[0]
        if case == 'glsl':
            function = glsl(tree, 'float animated_passthrough(float x) { return x; }',
                            'animated_passthrough')
            tree.links.new(source, function.inputs[0])
            source = function.outputs[0]
        elif case == 'driver':
            tree.animation_data_clear()
            value.outputs[0].driver_add('default_value').driver.expression = '.2 + frame / 1000.0'
        elif case == 'nla':
            animation = tree.animation_data
            action, slot = animation.action, animation.action_slot
            track = animation.nla_tracks.new()
            strip = track.strips.new('Unsupported NLA', 1, action)
            strip.action_slot = slot
            animation.action = None
        elif case == 'folded':
            multiply = tree.nodes.new('ShaderNodeMath')
            multiply.operation = 'MULTIPLY'
            multiply.inputs[1].default_value = 0
            tree.links.new(source, multiply.inputs[0])
            source = multiply.outputs[0]
        elif case in {'image_sequence', 'static_image'}:
            first = make_image(case_dir, 1)
            make_image(case_dir, 2)
            texture = tree.nodes.new('ShaderNodeTexImage')
            texture.image = bpy.data.images.load(first, check_existing=False)
            texture.image.colorspace_settings.name = 'Non-Color'
            if case == 'image_sequence':
                texture.image.source = 'SEQUENCE'
                texture.image_user.frame_duration = 2
                texture.image_user.use_cyclic = True
                texture.image_user.use_auto_refresh = True
            tree.links.new(texture.outputs['Color'], emission.inputs['Color'])
        elif case == 'external_id':
            # A static Image Texture is supported. This case needs an actual external Object ID.
            reference = bpy.data.objects.new('Animated coordinate reference', None)
            s.collection.objects.link(reference)
            for frame in range(1, FRAMES + 1):
                reference.location = (-.3 - scalar(frame) * .2, -.4, -.5)
                reference.keyframe_insert(data_path='location', frame=frame)
            coordinates = tree.nodes.new('ShaderNodeTexCoord')
            coordinates.object = reference
            tree.links.new(coordinates.outputs['Object'], emission.inputs['Color'])
        elif case == 'shared_mix':
            # The same animated source has both a safe Strength use and an unsafe Mix use.
            mix = tree.nodes.new('ShaderNodeMixRGB')
            mix.inputs[1].default_value = (.1, .2, .3, 1)
            mix.inputs[2].default_value = (.7, .4, .2, 1)
            tree.links.new(source, mix.inputs[0])
            tree.links.new(mix.outputs[0], emission.inputs['Color'])
        elif case == 'shared_shader_to_rgb':
            # Closure duplication cannot leave one tracked and one stale copy of this source.
            branch = tree.nodes.new('ShaderNodeEmission')
            branch.inputs['Color'].default_value = (.3, .6, .2, 1)
            convert = tree.nodes.new('ShaderNodeShaderToRGB')
            tree.links.new(source, branch.inputs['Strength'])
            tree.links.new(branch.outputs[0], convert.inputs[0])
            tree.links.new(convert.outputs['Color'], emission.inputs['Color'])
        # Deliberately Float -> Float. Value -> Emission.Color conversion is unsupported.
        tree.links.new(source, emission.inputs['Strength'])
    elif case == 'rgb':
        tree.links.new(rgb_node(tree).outputs[0], emission.inputs['Color'])
    elif case == 'normal_strength':
        normal = tree.nodes.new('ShaderNodeNormalMap')
        normal.space = 'OBJECT'
        normal.inputs['Color'].default_value = (.8, .6, 1, 1)
        animate_socket(normal.inputs['Strength'], scalar)
        tree.links.new(normal.outputs['Normal'], emission.inputs['Color'])
    elif case == 'emission':
        animate_socket(emission.inputs['Color'], color)
        animate_socket(emission.inputs['Strength'], scalar)
    elif case in {'nested', 'group_input'}:
        repeated_groups(tree, output, case == 'group_input')
    s.frame_set(1)
    return s


class Viewport:
    def __init__(self):
        area = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
        self.region = next(r for r in area.regions if r.type == 'WINDOW')
        self.space = area.spaces.active
        self.space.shading.type = 'RENDERED'
        self.space.shading.render_pass = 'COMBINED'
        self.space.overlay.show_overlays = False
        self.buffer = gpu.types.GPUOffScreen(WIDTH, HEIGHT, format='RGBA32F')

    def draw(self, frame=None):
        s = bpy.context.scene
        start = time.perf_counter()
        if frame is not None:
            s.frame_set(frame)
        bpy.context.view_layer.update()
        eval_ms = (time.perf_counter() - start) * 1000
        depsgraph = bpy.context.evaluated_depsgraph_get()
        assert depsgraph.mode == 'VIEWPORT'
        projection = s.camera.calc_matrix_camera(depsgraph, x=WIDTH, y=HEIGHT)
        start = time.perf_counter()
        self.buffer.draw_view3d(s, bpy.context.view_layer, self.space, self.region,
                               s.camera.matrix_world.inverted(), projection,
                               do_color_management=False, draw_background=True)
        draw_ms = (time.perf_counter() - start) * 1000
        pixels = np.array(self.buffer.texture_color.read(), dtype=np.float32).reshape(HEIGHT, WIDTH, 4)
        assert np.isfinite(pixels).all()
        report = s.eevee.performance_profiler_viewport_report
        # Sync Analysis has another material_hits field; only read Material Preparation's row.
        match = re.search(r'^\s*-\s+material_hits=[^\n]+', report, re.MULTILINE)
        counts = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)',
                  match.group(0))} if match else {}
        return pixels, {'frame':s.frame_current, 'eval_ms':eval_ms, 'draw_ms':draw_ms,
                        'counts':counts}, report

    def close(self):
        self.buffer.free()


def exact_pixels(actual, expected):
    return np.array_equal(actual.view(np.uint32), expected.view(np.uint32))


def save_pixels(path, images, frames=None, source_bits=None):
    pixels = np.stack(images)
    sequence = np.array(frames if frames is not None else [], dtype=np.int32)
    source_bits = np.array(source_bits if source_bits is not None else [], dtype=np.uint32)
    np.savez_compressed(path, pixels=pixels, frames=sequence, source_bits=source_bits)
    if REFERENCE is None:
        return None
    reference_path = REFERENCE / path.relative_to(OUT)
    assert reference_path.is_file(), reference_path
    with np.load(reference_path) as reference:
        assert np.array_equal(sequence, reference['frames']), (path, 'different frame sequence')
        assert np.array_equal(source_bits, reference['source_bits']), (path, 'source bits differ')
        expected = reference['pixels']
        assert expected.shape == pixels.shape, (path, expected.shape, pixels.shape)
        difference = np.abs(pixels - expected)
    result = {'max':float(difference.max()), 'mean':float(difference.mean()),
              'bit_mismatches':int(np.count_nonzero(pixels.view(np.uint32) != expected.view(np.uint32)))}
    if SELFDIFF is None:
        assert exact_pixels(pixels, expected), (path, 'exact pixel mismatch', result)
    else:
        # No hand-written tolerance: derive it only from a second run of the identical baseline.
        first_manifest = json.loads((REFERENCE / 'uniform_manifest.json').read_text(encoding='utf-8'))
        second_manifest = json.loads((SELFDIFF / 'uniform_manifest.json').read_text(encoding='utf-8'))
        assert first_manifest['baseline'] and second_manifest['baseline']
        for key in ('binary_sha256', 'backend', 'frames', 'rounds', 'size'):
            assert first_manifest[key] == second_manifest[key], (key, 'invalid self-diff evidence')
        evidence_path = SELFDIFF / path.relative_to(OUT)
        with np.load(evidence_path) as evidence:
            assert np.array_equal(sequence, evidence['frames'])
            assert np.array_equal(source_bits, evidence['source_bits'])
            assert evidence['pixels'].shape == expected.shape
            self_difference = np.abs(evidence['pixels'] - expected)
            evidence_bit_mismatches = int(np.count_nonzero(
                evidence['pixels'].view(np.uint32) != expected.view(np.uint32)))
        measured = {'max':float(self_difference.max()), 'mean':float(self_difference.mean()),
                    'bit_mismatches':evidence_bit_mismatches}
        result['self_difference'] = measured
        result['self_difference_file'] = str(evidence_path)
        assert (result['max'] <= measured['max'] and result['mean'] <= measured['mean'] and
                result['bit_mismatches'] <= measured['bit_mismatches']), (path, result)
    return result


def transitions(case_dir):
    """Changes outside the supported Action path must invalidate, without timing cold rebuilds."""
    build_scene('value', case_dir)
    view = Viewport()
    images, labels = [], []
    frame = max(2, FRAMES // 2)

    def capture(label):
        pixels, _, _ = view.draw()
        labels.append(label)
        images.append(pixels)
        return pixels

    try:
        view.draw(frame)
        before = capture('before edits')
        material = bpy.data.materials['Uniform fixture material']
        tree = material.node_tree
        emission = tree.nodes['Fixture Emission']
        emission.inputs['Color'].default_value = (.8, .2, .1, 1)
        edited = capture('same-frame socket edit')
        assert float(np.max(np.abs(edited - before))) > .05
        material.surface_render_method = 'BLENDED'
        mode = capture('pipeline mode edit')
        # Compare each mode to its own baseline image; their internal precision may differ.
        assert float(mode[HEIGHT//2, WIDTH//2, :3].max()) > .01
        alternate = tree.nodes.new('ShaderNodeValue')
        alternate.name = 'Replacement Value'
        alternate.outputs[0].default_value = .25
        tree.links.new(alternate.outputs[0], emission.inputs['Strength'])
        linked = capture('same-frame link edit')
        assert not np.array_equal(linked, mode)
        bpy.context.preferences.edit.use_global_undo = True
        bpy.ops.ed.undo_push(message='Uniform before deletion')
        tree.nodes.remove(alternate)
        capture('delete linked node')
        bpy.ops.ed.undo_push(message='Uniform after deletion')
        view.close()
        view = None
        assert bpy.ops.ed.undo() == {'FINISHED'}
        view = Viewport()
        restored = capture('undo deletion')
        assert exact_pixels(restored, linked), 'Undo did not restore exact pixels'
        asset = case_dir / 'uniform_cold_open.blend'
        bpy.ops.wm.save_as_mainfile(filepath=str(asset), copy=True)
        cold_expected = capture('before cold open')
        view.close()
        view = None
        child = background_child(Path(__file__).resolve(),
                                 ['--uniform-cold-open', str(asset), str(frame)])
        assert 'UNIFORM_COLD_OPEN_OK' in child, child[-4000:]
        with np.load(case_dir / 'cold_open.npz') as cold:
            assert exact_pixels(cold['pixels'][0], cold_expected), 'Cold-open pixel mismatch'
        save_pixels(case_dir / 'transitions.npz', images)
        (case_dir / 'transition_labels.json').write_text(json.dumps(labels), encoding='utf-8')
    finally:
        if view is not None:
            view.close()


def source_identity_transitions(case_dir):
    """Equal current numbers must not alias two source sockets, Actions, or Action slots."""
    s = build_scene('value', case_dir)
    tree = bpy.data.materials['Uniform fixture material'].node_tree
    emission = tree.nodes['Fixture Emission']
    frame = max(2, FRAMES // 2)
    second = tree.nodes.new('ShaderNodeValue')
    second.name = 'Different Source'
    animate_socket(second.outputs[0], lambda f: scalar(f) if f == frame else scalar(f) * .5)
    view = Viewport()
    images, frames, labels, rows = [], [], [], []

    def capture(label, at_frame):
        pixels, row, _ = view.draw(at_frame)
        images.append(pixels)
        frames.append(at_frame)
        labels.append(label)
        rows.append(row)
        return pixels

    try:
        for _ in range(4):
            view.draw(frame)
        original = capture('source A', frame)
        tree.links.new(second.outputs[0], emission.inputs['Strength'])
        replacement = capture('source B with identical current value', frame)
        assert exact_pixels(original, replacement)
        diverged = capture('source B diverges next frame', frame + 1)
        assert not exact_pixels(replacement, diverged)

        action, original_slot = tree.animation_data.action, tree.animation_data.action_slot
        alternate_slot = action.slots.new(tree.id_type, 'Replacement slot')
        bag = action.layers[0].strips[0].channelbag(alternate_slot, ensure=True)
        curve = bag.fcurves.new(second.outputs[0].path_from_id('default_value'), index=0)
        curve.keyframe_points.insert(1, .8)
        curve.keyframe_points.insert(FRAMES, .9)
        tree.animation_data.action_slot = alternate_slot
        switched = capture('different Action slot', frame + 1)
        assert not exact_pixels(switched, diverged)
        tree.animation_data.action_slot = original_slot
        restored = capture('restored Action slot', frame + 1)
        assert exact_pixels(restored, diverged)

        copied_action = action.copy()
        copied_slot = next(slot for slot in copied_action.slots if slot.identifier == original_slot.identifier)
        tree.animation_data.action = copied_action
        tree.animation_data.action_slot = copied_slot
        copied = capture('different Action with equal values', frame + 1)
        assert exact_pixels(copied, restored)
        copied_bag = copied_action.layers[0].strips[0].channelbag(copied_slot)
        copied_curve = next(curve for curve in copied_bag.fcurves
                            if curve.data_path == second.outputs[0].path_from_id('default_value'))
        for key in copied_curve.keyframe_points:
            key.co[1] += .1
        copied_curve.update()
        tree.update_tag()
        changed_action = capture('edited replacement Action at same frame', frame + 1)
        assert not exact_pixels(changed_action, copied)
        save_pixels(case_dir / 'source_identity.npz', images, frames)
        (case_dir / 'source_identity.json').write_text(
            json.dumps({'labels':labels, 'rows':rows}, indent=2), encoding='utf-8')
    finally:
        view.close()

    # Store evaluated IEEE-754 source bits as well as pixels. If baseline evaluation itself
    # normalizes a key's -0, report that fact instead of claiming the GPU saw a negative zero.
    s = build_scene('value', case_dir)
    material = bpy.data.materials['Uniform fixture material']
    tree = material.node_tree
    tree.animation_data_clear()
    value = tree.nodes['Animated Value']
    for at_frame, number in ((1, 0.0), (2, -0.0), (3, .4), (4, -0.0)):
        value.outputs[0].default_value = number
        value.outputs[0].keyframe_insert('default_value', frame=at_frame)
    view = Viewport()
    images, bits, rows = [], [], []
    frames = [1, 2, 3, 4, 2, 1]
    try:
        for _ in range(4):
            view.draw(1)
        for at_frame in frames:
            pixels, row, _ = view.draw(at_frame)
            evaluated = material.evaluated_get(bpy.context.evaluated_depsgraph_get())
            number = evaluated.node_tree.nodes['Animated Value'].outputs[0].default_value
            current_bits = int(np.array(number, dtype=np.float32).view(np.uint32))
            if (not BASELINE and bits and {bits[-1], current_bits} == {0, 0x80000000}):
                counts = row['counts']
                assert counts.get('uniform_updates', 0) + counts.get('builds', 0) > 0, row
            bits.append(current_bits)
            images.append(pixels)
            rows.append(row)
        save_pixels(case_dir / 'signed_zero.npz', images, frames, bits)
        (case_dir / 'signed_zero.json').write_text(json.dumps(
            {'source_bits':bits, 'negative_zero_observed':0x80000000 in bits, 'rows':rows}, indent=2),
            encoding='utf-8')
    finally:
        view.close()


def frame_sequence():
    return list(range(1, FRAMES + 1)) + [FRAMES, 1, max(1, FRAMES//2), 2,
                                        FRAMES-1, max(1, FRAMES//3), 1]


def resume_completed_stream(current):
    """Reuse only complete stream artifacts from this exact runtime/configuration."""
    manifest_path = OUT / 'uniform_manifest.json'
    assert manifest_path.is_file(), 'Transition-only mode requires an existing manifest'
    recorded = json.loads(manifest_path.read_text(encoding='utf-8'))
    for key in ('binary_sha256', 'backend', 'frames', 'rounds', 'size', 'baseline'):
        assert recorded.get(key) == current[key], (key, 'transition-only resume mismatch',
                                                  recorded.get(key), current[key])
    assert set(recorded.get('cases', {})) == set(CASES), 'Recorded case matrix is incomplete or different'
    expected_frames = np.array(frame_sequence(), dtype=np.int32)
    expected_shape = (len(expected_frames), HEIGHT, WIDTH, 4)
    for case in CASES:
        saved = recorded['cases'][case]
        assert len(saved.get('comparisons', [])) == ROUNDS, (case, 'incomplete round manifest')
        for round_index in range(ROUNDS):
            path = OUT / case / f'round_{round_index:02d}.npz'
            assert path.is_file(), (case, 'missing round', path)
            with np.load(path, allow_pickle=False) as artifact:
                assert np.array_equal(artifact['frames'], expected_frames), (path, 'frame sequence')
                pixels = artifact['pixels']
                assert pixels.shape == expected_shape, (path, 'pixel dimensions')
                assert pixels.dtype == np.float32, (path, 'pixel dtype')
                assert artifact['source_bits'].size == 0, (path, 'unexpected stream source bits')
        rows = json.loads((OUT / case / 'timings.json').read_text(encoding='utf-8'))
        assert len(rows) == ROUNDS * len(expected_frames), (case, 'incomplete timing rows')
        for index, row in enumerate(rows):
            assert row['round'] == index // len(expected_frames), (case, 'round order')
            assert row['index'] == index % len(expected_frames), (case, 'sample order')
            assert row['frame'] == int(expected_frames[index % len(expected_frames)]), (case, 'frame order')
        assert sum(row['counts'].get('builds', 0) for row in rows) == saved['builds'], case
        assert sum(row['counts'].get('uniform_updates', 0) for row in rows) == saved['uniform_updates'], case
    print('UNIFORM_TRANSITIONS_RESUME_VERIFIED', len(CASES), ROUNDS, flush=True)
    return recorded


gpu.init()
if '--uniform-cold-open' in sys.argv:
    argument = sys.argv.index('--uniform-cold-open')
    asset, frame = Path(sys.argv[argument + 1]), int(sys.argv[argument + 2])
    bpy.ops.wm.open_mainfile(filepath=str(asset))
    view = Viewport()
    try:
        pixels, _, _ = view.draw(frame)
        np.savez_compressed(asset.parent / 'cold_open.npz', pixels=pixels[None, ...])
    finally:
        view.close()
    print('UNIFORM_COLD_OPEN_OK')
else:
    manifest = {'baseline':BASELINE, 'backend':gpu.platform.backend_type_get(),
                'frames':FRAMES, 'rounds':ROUNDS, 'size':[WIDTH, HEIGHT], 'cases':{},
                'note':'Persistent viewport timing excludes texture readback and artifact writes.'}
    with open(bpy.app.binary_path, 'rb') as binary:
        digest = hashlib.sha256()
        for chunk in iter(lambda: binary.read(1024 * 1024), b''):
            digest.update(chunk)
    manifest['binary_sha256'] = digest.hexdigest()
    if TRANSITIONS_ONLY:
        manifest = resume_completed_stream(manifest)
    for case in ([] if TRANSITIONS_ONLY else CASES):
        print('UNIFORM_CASE_BEGIN', case, flush=True)
        case_dir = OUT / case
        case_dir.mkdir(parents=True, exist_ok=True)
        build_scene(case, case_dir)
        view = Viewport()
        rows, comparisons = [], []
        frames = frame_sequence()
        try:
            # Establish a real compiled persistent viewport before measuring the changing stream.
            for _ in range(4):
                view.draw(1)
            for round_index in range(ROUNDS):
                images = []
                for index, frame in enumerate(frames):
                    pixels, row, report = view.draw(frame)
                    row['round'], row['index'] = round_index, index
                    rows.append(row)
                    images.append(pixels)
                    if not BASELINE:
                        assert 'builds' in row['counts'] and 'uniform_updates' in row['counts'], report
                        if case in SUPPORTED:
                            assert row['counts']['builds'] == 0, (case, row, report)
                    if index % 60 == 0:
                        print('UNIFORM_PROGRESS', case, round_index, index, flush=True)
                comparisons.append(save_pixels(case_dir / f'round_{round_index:02d}.npz', images, frames))
                if case != 'folded':
                    assert float(np.max(np.ptp(np.stack(images)[:, :, :, :3], axis=0))) > .0001, case
                (case_dir / 'last_profiler.txt').write_text(report, encoding='utf-8')
        finally:
            view.close()
        (case_dir / 'timings.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
        builds = sum(row['counts'].get('builds', 0) for row in rows)
        updates = sum(row['counts'].get('uniform_updates', 0) for row in rows)
        if not BASELINE:
            if case in SUPPORTED:
                assert updates > 0 and builds == 0, (case, builds, updates)
            else:
                assert builds > 0 and updates == 0, (case, builds, updates)
        draw_times = [row['draw_ms'] for row in rows]
        manifest['cases'][case] = {'builds':builds, 'uniform_updates':updates,
                                  'draw_median_ms':float(np.median(draw_times)),
                                  'draw_p95_ms':float(np.percentile(draw_times, 95)),
                                  'comparisons':comparisons}
        (OUT / 'uniform_manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    manifest['stream_complete'] = True
    manifest['transitions'] = {'complete':False, 'status':'running' if TRANSITIONS else 'disabled',
                               'transitions_only_resume':TRANSITIONS_ONLY}
    (OUT / 'uniform_manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    try:
        if TRANSITIONS:
            directory = OUT / 'transitions'
            directory.mkdir(parents=True, exist_ok=True)
            transitions(directory)
            source_identity_transitions(directory)
            manifest['transitions'].update(complete=True, status='complete')
    except Exception:
        manifest['transitions'].update(status='failed', error=traceback.format_exc())
        raise
    finally:
        (OUT / 'uniform_manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print('MATERIAL_UNIFORM_ANIMATION_BACKEND', gpu.platform.backend_type_get(), flush=True)
    print('MATERIAL_UNIFORM_ANIMATION_OK', flush=True)
