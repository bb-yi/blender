# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Isolated Vulkan image-quality regression; write artifacts outside the source tree."""

import argparse
import json
import math
import os
from pathlib import Path
import sys

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
s.render.resolution_x, s.render.resolution_y, s.render.resolution_percentage = 768, 512, 100
s.render.use_compositing = False
s.render.use_sequencer = False
s.eevee.dlss5_mode = 'OFF'
s.eevee.motion_blur_steps = 1
s.view_settings.view_transform = 'Standard'
s.view_settings.look = 'None'
s.render.image_settings.file_format = 'OPEN_EXR'
s.render.image_settings.color_depth = '32'
bpy.ops.object.camera_add(location=(0, 0, 10))
s.camera = bpy.context.object
s.camera.data.type = 'ORTHO'
s.camera.data.ortho_scale = 6
mat = bpy.data.materials.new('Emission')
mat.use_nodes = True
tree = mat.node_tree
tree.nodes.clear()
emission = tree.nodes.new('ShaderNodeEmission')
emission.inputs['Color'].default_value = (0.8, 0.8, 0.8, 1)
output = tree.nodes.new('ShaderNodeOutputMaterial')
tree.links.new(emission.outputs[0], output.inputs['Surface'])
report, failures = {}, []


def check(condition, message):
    if not condition:
        failures.append(message)


def render(label, quality='QUALITY', samples=64, blur=False):
    s.eevee.dlss_sr_render_quality = quality
    s.eevee.taa_render_samples = samples
    s.render.use_motion_blur = blur
    s.render.filepath = str(out / (label + '.exr'))
    bpy.ops.render.render(write_still=True)
    status = s.eevee.dlss_sr_render_status
    check(quality == 'OFF' or status.startswith('Completed'), label + ': ' + status)
    image = bpy.data.images.load(s.render.filepath, check_existing=False)
    values = np.empty(len(image.pixels), dtype=np.float32)
    image.pixels.foreach_get(values)
    check(tuple(image.size) == (768, 512), label + ': wrong output size')
    bpy.data.images.remove(image)
    rgb = values.reshape(512, 768, 4)[:, :, :3]
    check(np.isfinite(rgb).all(), label + ': nonfinite pixels')
    report[label] = dict(status=status, mean=float(rgb.mean()))
    s.render.image_settings.file_format = 'PNG'
    s.render.image_settings.color_depth = '8'
    bpy.data.images['Render Result'].save_render(str(out / (label + '.png')), scene=s)
    s.render.image_settings.file_format = 'OPEN_EXR'
    s.render.image_settings.color_depth = '32'
    return rgb


def compare(label, actual, reference):
    error = float(np.sqrt(np.mean((actual - reference) ** 2)))
    report[label]['rmse_native'] = error
    return error


# Thin geometry in both axes catches incorrect jitter signs without involving mipmaps.
lines = []
for i in range(26):
    bpy.ops.mesh.primitive_plane_add(size=1, location=(-2.6 + i * .2, 0, 0))
    ob = bpy.context.object
    ob.scale = (.012 + (i % 4) * .004, 3, 1)
    ob.rotation_euler.z = math.radians(12)
    ob.data.materials.append(mat)
    lines.append(ob)

for axis in ('x', 'y'):
    if axis == 'y':
        s.camera.data.type = 'PERSP'
        for ob in lines:
            ob.location.y = ob.location.x * .65
            ob.location.x = 0
            ob.scale *= .65
            ob.rotation_euler.z += math.pi / 2
    native = render(axis + '_native', 'OFF')
    native_blur = render(axis + '_native_blur', 'OFF', blur=True)
    check(np.max(np.abs(native - native_blur)) < 1e-5, axis + ': static native control moved')
    for quality in ('QUALITY', 'BALANCED', 'PERFORMANCE'):
        label = axis + '_' + quality
        single = render(label + '_single', quality, samples=1)
        single_error = compare(label + '_single', single, native)
        settled = render(label, quality)
        error = compare(label, settled, native)
        # Before the fix Quality RMSE was 0.070 (X) and 0.052 (Y), with striped edges.
        # Performance has only one quarter of the output pixels. These subpixel-width lines
        # lose more coverage with its runtime preset; still reject the old striped reconstruction
        # (RMSE 0.080 on X, 0.064 on Y) rather than demanding Quality-level reconstruction.
        performance = quality == 'PERFORMANCE'
        check(error < (.045 if performance else .025),
              label + ': jitter does not reconstruct thin lines')
        check(error < single_error * (.7 if performance else .5), label + ': samples do not converge')
        check((.75 if performance else .85) < settled.mean() / native.mean() < 1.15,
              label + ': lost thin-line coverage')
        blurred = render(label + '_blur', quality, blur=True)
        delta = float(np.sqrt(np.mean((blurred - settled) ** 2)))
        report[label + '_blur']['rmse_static_toggle'] = delta
        check(delta < .003, label + ': static motion-blur toggle resets every sample')

for ob in lines:
    bpy.data.objects.remove(ob, do_unlink=True)

# A linear-filtered texture with six-output-pixel sinusoidal detail exercises real mip selection.
s.camera.data.type = 'ORTHO'
bpy.ops.mesh.primitive_plane_add(size=2)
plane = bpy.context.object
plane.scale = (3, 2, 1)
plane.data.materials.append(mat)
width, height = 1536, 1024
u = (np.arange(width, dtype=np.float32) + .5) / 12
v = (np.arange(height, dtype=np.float32) + .5) / 12
pattern = .45 + .35 * np.sin(2 * np.pi * u)[None, :] * np.sin(2 * np.pi * v)[:, None]
rgba = np.ones((height, width, 4), dtype=np.float32)
rgba[:, :, :3] = pattern[:, :, None]
texture = bpy.data.images.new('SR mip detail', width=width, height=height, float_buffer=True)
texture.colorspace_settings.name = 'Non-Color'
texture.pixels.foreach_set(rgba.ravel())
texture.update()
tex = tree.nodes.new('ShaderNodeTexImage')
tex.image = texture
tex.interpolation = 'Linear'
uv = tree.nodes.new('ShaderNodeTexCoord')
tree.links.new(uv.outputs['UV'], tex.inputs['Vector'])
tree.links.new(tex.outputs['Color'], emission.inputs['Color'])
native = render('texture_native', 'OFF')
native_contrast = float(native[32:-32, 32:-32].std())
check(native_contrast > .08, 'texture control has no detail')
for quality in ('QUALITY', 'BALANCED', 'PERFORMANCE'):
    label = 'texture_' + quality
    result = render(label, quality)
    contrast_ratio = float(result[32:-32, 32:-32].std()) / native_contrast
    report[label]['contrast_ratio'] = contrast_ratio
    check(contrast_ratio > .75, label + ': mip selection erased texture detail')
    check(compare(label, result, native) < .06, label + ': texture reconstruction differs')

# Multiple shutter steps must not mix temporally unrelated zero-motion histories.
tree.links.remove(emission.inputs['Color'].links[0])
plane.scale = (.5, .5, 1)
for frame, x in ((1, -2), (3, 2)):
    plane.location.x = x
    plane.keyframe_insert('location', frame=frame)
s.eevee.motion_blur_steps = 4
s.render.motion_blur_shutter = 1
s.frame_set(2, subframe=.25)
native = render('motion_native', 'OFF', blur=True)
for quality in ('QUALITY', 'PERFORMANCE'):
    label = 'motion_' + quality
    result = render(label, quality, blur=True)
    check(compare(label, result, native) < .035, label + ': shutter steps contaminate history')
    check(s.frame_current == 2 and abs(s.frame_subframe - .25) < 1e-6,
          label + ': render request time changed')

# A scene-level motion-blur toggle must not invalidate a layer that opted out.
bpy.context.view_layer.use_motion_blur = False
disabled = render('layer_blur_disabled', 'QUALITY', blur=True)
off = render('scene_blur_disabled', 'QUALITY')
check(float(np.sqrt(np.mean((disabled - off) ** 2))) < .003,
      'disabled view-layer motion blur changes SR history')

# Real lens jitter changes viewpoint, unlike repeated AA samples at one shutter time.
# Retaining zero-motion SR history across those views produces ghosts, so keep its reset.
plane.animation_data_clear()
plane.location.x = 0
s.camera.data.type = 'PERSP'
s.camera.data.dof.use_dof = True
s.camera.data.dof.focus_distance = 7
s.camera.data.dof.aperture_fstop = 1.4
s.eevee.use_bokeh_jittered = True
native = render('dof_native', 'OFF')
result = render('dof_quality', 'QUALITY')
check(compare('dof_quality', result, native) < .035, 'lens-jitter history leaks between views')

(out / 'report.json').write_text(json.dumps(dict(results=report, failures=failures), indent=2),
                                 encoding='utf-8')
assert not failures, '\n'.join(failures)
print('DLSS_SR_IMAGE_QUALITY_PASS', flush=True)
