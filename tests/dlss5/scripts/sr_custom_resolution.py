# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Custom SR input-size and persistence regression on a supported Vulkan/NGX device."""

import argparse
import json
import math
import os
from pathlib import Path
import sys

import bpy
import numpy as np
from bl_ui.properties_render import RENDER_PT_eevee_dlss5

parser = argparse.ArgumentParser()
parser.add_argument('--output-dir', required=True)
parser.add_argument('--legacy-blend')
args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
out = Path(args.output_dir).resolve()
out.mkdir(parents=True, exist_ok=True)
os.environ['DLSS5_CACHE_DIR'] = str(out / 'ngx-cache')

if args.legacy_blend:
    bpy.ops.wm.open_mainfile(filepath=str(Path(args.legacy_blend).resolve()))
    assert bpy.context.scene.eevee.dlss_sr_render_quality == 'BALANCED'
    assert bpy.context.scene.eevee.dlss_sr_render_percentage == 80.0
    assert bpy.context.scene.eevee.dlss_sr_viewport_percentage == 80.0

bpy.ops.wm.read_factory_settings(use_empty=True)
assert 'DEFAULT_CLOSED' in RENDER_PT_eevee_dlss5.bl_options
s = bpy.context.scene
assert s.eevee.dlss_sr_render_quality == s.eevee.dlss_sr_viewport_quality == 'OFF'
assert s.eevee.dlss_sr_render_percentage == s.eevee.dlss_sr_viewport_percentage == 80.0
s.render.engine = 'BLENDER_EEVEE'
s.render.resolution_x, s.render.resolution_y, s.render.resolution_percentage = 773, 517, 100
s.render.use_compositing = False
s.render.use_sequencer = False
s.render.use_motion_blur = False
s.eevee.taa_render_samples = 8
s.eevee.dlss5_mode = 'OFF'
s.eevee.dlss_sr_viewport_quality = 'CUSTOM'
s.eevee.dlss_sr_viewport_percentage = 76.5
s.render.image_settings.file_format = 'OPEN_EXR'
s.render.image_settings.color_depth = '32'
bpy.ops.object.camera_add(location=(0, 0, 10))
s.camera = bpy.context.object
s.camera.data.type = 'ORTHO'
s.camera.data.ortho_scale = 6
bpy.ops.mesh.primitive_plane_add(size=3)
mat = bpy.data.materials.new('Custom SR emission')
mat.use_nodes = True
tree = mat.node_tree
tree.nodes.clear()
emission = tree.nodes.new('ShaderNodeEmission')
emission.inputs['Color'].default_value = (1.0, 0.2, 0.05, 1)
surface = tree.nodes.new('ShaderNodeOutputMaterial')
tree.links.new(emission.outputs[0], surface.inputs['Surface'])
bpy.context.object.data.materials.append(mat)
report = {}


def render(label, mode, percentage=80.0):
    s.eevee.dlss_sr_render_quality = mode
    s.eevee.dlss_sr_render_percentage = percentage
    assert s.eevee.dlss_sr_viewport_percentage == 76.5
    s.render.filepath = str(out / (label + '.exr'))
    bpy.ops.render.render(write_still=True)
    status = s.eevee.dlss_sr_render_status
    w = s.render.resolution_x * s.render.resolution_percentage // 100
    h = s.render.resolution_y * s.render.resolution_percentage // 100
    if mode == 'CUSTOM':
        iw, ih = (math.floor(v * percentage / 100.0 + .5) for v in (w, h))
        assert status.startswith(f'Completed {iw}x{ih} -> {w}x{h} |'), status
    elif mode != 'OFF':
        assert status.startswith('Completed'), status
    else:
        assert status == 'Disabled', status
    image = bpy.data.images.load(s.render.filepath, check_existing=False)
    assert tuple(image.size) == (w, h)
    values = np.empty(len(image.pixels), dtype=np.float32)
    image.pixels.foreach_get(values)
    bpy.data.images.remove(image)
    rgb = values.reshape(h, w, 4)[:, :, :3]
    assert np.isfinite(rgb).all(), label
    assert rgb.max() > .5 and rgb.std() > .1, label
    report[label] = {'status': status, 'mean': float(rgb.mean())}


for percentage in (50.0, 67.0, 75.0, 80.0, 82.5, 90.0, 100.0, 50.0):
    render(f'custom_{len(report)}_{percentage}', 'CUSTOM', percentage)
for mode in ('QUALITY', 'BALANCED', 'PERFORMANCE', 'OFF', 'CUSTOM'):
    render('switch_' + mode, mode, 82.5)
# Scene output percentage and SR percentage compose, without modifying final output size.
s.render.resolution_percentage = 80
render('output_80_custom_75', 'CUSTOM', 75.0)
# Persist independent viewport/render values and Custom enums; do not touch user assets.
saved = out / 'custom.blend'
bpy.ops.wm.save_as_mainfile(filepath=str(saved))
bpy.ops.wm.open_mainfile(filepath=str(saved))
s = bpy.context.scene
assert s.eevee.dlss_sr_render_quality == s.eevee.dlss_sr_viewport_quality == 'CUSTOM'
assert s.eevee.dlss_sr_render_percentage == 75.0
assert s.eevee.dlss_sr_viewport_percentage == 76.5
render('reopened', 'CUSTOM', 75.0)
(out / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print('DLSS_SR_CUSTOM_RESOLUTION_PASS', flush=True)
