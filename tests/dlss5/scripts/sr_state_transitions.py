# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Isolated Vulkan regression; pass -- --output-dir outside the source tree."""

import argparse
import json
import os
from pathlib import Path
import sys
import tempfile

import bpy
import numpy as np

parser = argparse.ArgumentParser()
parser.add_argument('--output-dir', required=True)
args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
out = Path(args.output_dir).resolve()
out.mkdir(parents=True, exist_ok=True)
os.environ['DLSS5_CACHE_DIR'] = str(out / 'ngx-cache')
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.context.preferences.filepaths.temporary_directory = str(out)
s = bpy.context.scene
s.render.engine = 'BLENDER_EEVEE'
s.render.resolution_x, s.render.resolution_y, s.render.resolution_percentage = 384, 256, 100
s.eevee.taa_render_samples = 32
s.eevee.dlss5_mode = 'OFF'
s.render.motion_blur_shutter = 1.0
s.eevee.motion_blur_steps = 4
s.render.image_settings.media_type = 'IMAGE'
s.render.image_settings.file_format = 'OPEN_EXR'
s.render.image_settings.color_depth = '32'
bpy.ops.object.camera_add(location=(0, 0, 8))
s.camera = bpy.context.object
s.camera.data.type = 'ORTHO'
s.camera.data.ortho_scale = 6
bpy.ops.mesh.primitive_plane_add(size=1)
ob = bpy.context.object
for frame, x in ((1, -2), (3, 2)):
    ob.location.x = x
    ob.keyframe_insert('location', frame=frame)
mat = bpy.data.materials.new('White')
mat.use_nodes = True
mat.node_tree.nodes['Principled BSDF'].inputs['Emission Color'].default_value = (1, 1, 1, 1)
mat.node_tree.nodes['Principled BSDF'].inputs['Emission Strength'].default_value = 1
ob.data.materials.append(mat)
report, failures = {}, []


def check(condition, message):
    if not condition:
        failures.append(message)


def read_image(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    values = np.empty(len(image.pixels), dtype=np.float32)
    image.pixels.foreach_get(values)
    bpy.data.images.remove(image)
    assert np.isfinite(values).all(), path
    return values


def render(label, quality, fail=False, frame=2, subframe=0.25):
    s.frame_set(frame, subframe=subframe)
    s.eevee.dlss_sr_render_quality = quality
    if fail:
        os.environ['BLENDER_DLSS_SR_TEST_FAILURE'] = 'after_first_sample'
    else:
        os.environ.pop('BLENDER_DLSS_SR_TEST_FAILURE', None)
    s.render.filepath = str(out / (label + '.exr'))
    try:
        bpy.ops.render.render(write_still=True)
    finally:
        os.environ.pop('BLENDER_DLSS_SR_TEST_FAILURE', None)
    report[label] = dict(status=s.eevee.dlss_sr_render_status,
                         frame=s.frame_current, subframe=s.frame_subframe)
    check((s.frame_current, s.frame_subframe) == (frame, subframe), label + ': frame changed')
    if fail:
        check('Native retry:' in report[label]['status'], label + ': retry not exercised')
    return read_image(s.render.filepath)


# Clearing the selection hides the precision toggle without clearing its stored value.
aov = bpy.context.view_layer.aovs.add()
aov.name, aov.type = 'Mask', 'VALUE'
s.eevee.dlss5_mask_aov = aov.name
s.eevee.dlss5_mask_aov_output = True
render('precision_selected', 'QUALITY')
check(report['precision_selected']['status'] == 'Native: exact mask AOV output',
      'selected precision opt-out must remain effective')
s.eevee.dlss5_mask_aov = ''
render('precision_cleared', 'QUALITY')
check(report['precision_cleared']['status'].startswith('Completed'),
      'cleared mask must not force native rendering')
s.eevee.dlss5_mask_aov = aov.name
render('precision_reselected', 'QUALITY')
check(report['precision_reselected']['status'] == 'Native: exact mask AOV output',
      'reselected precision opt-out must remain effective')
s.eevee.dlss5_mask_aov = ''
s.eevee.dlss5_mask_aov_output = False

# All shutter positions, nonzero subframes, and disabled layer/module paths.
for position in ('CENTER', 'START', 'END'):
    s.render.use_motion_blur = True
    s.render.motion_blur_position = position
    native = render(position + '_native', 'OFF')
    retry = render(position + '_retry', 'QUALITY', fail=True)
    error = float(np.max(np.abs(native - retry)))
    report[position + '_error'] = error
    check(error == 0, position + ': native retry pixels differ')
for label, scene_enabled, layer_enabled in (('no_blur', False, True), ('layer_no_blur', True, False)):
    s.render.use_motion_blur = scene_enabled
    bpy.context.view_layer.use_motion_blur = layer_enabled
    native = render(label + '_native', 'OFF')
    retry = render(label + '_retry', 'QUALITY', fail=True)
    check(np.array_equal(native, retry), label + ': native retry pixels differ')

# Fail separately on every animation frame and compare against independent native frames.
s.render.use_motion_blur = True
bpy.context.view_layer.use_motion_blur = True
s.render.motion_blur_position = 'CENTER'
reference = {frame: render('animation_native_' + str(frame), 'OFF', frame=frame, subframe=0)
             for frame in (1, 2, 3)}
s.frame_set(1)
s.frame_start, s.frame_end = 1, 3
s.eevee.dlss_sr_render_quality = 'QUALITY'
animation_out = Path(tempfile.mkdtemp(prefix='animation_', dir=out))
report['animation_output'] = str(animation_out)
s.render.filepath = str(animation_out / 'retry_')
os.environ['BLENDER_DLSS_SR_TEST_FAILURE'] = 'after_first_sample'
try:
    bpy.ops.render.render(animation=True)
finally:
    os.environ.pop('BLENDER_DLSS_SR_TEST_FAILURE', None)
for frame in (1, 2, 3):
    path = animation_out / ('retry_' + str(frame).zfill(4) + '.exr')
    if not path.exists():
        check(False, 'missing animation output: ' + path.name)
        continue
    actual = read_image(path)
    error = float(np.max(np.abs(actual - reference[frame])))
    report['animation_' + str(frame) + '_error'] = error
    check(error == 0, 'animation frame ' + str(frame) + ': native retry pixels differ')
report['failures'] = failures
(out / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
assert not failures, failures
print('DLSS_SR_STATE_TRANSITIONS_PASS', flush=True)
