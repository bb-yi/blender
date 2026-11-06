# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Read a relative 3D image in a fresh process, without any image prewarming."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import OUT, background_child, center, glsl, near, render, scene, socket
import bpy
import numpy as np

assets = OUT / 'cold'
assets.mkdir(parents=True, exist_ok=True)
path = assets / 'relative_lut.blend'

if '--cold-child' in sys.argv:
    bpy.ops.wm.open_mainfile(filepath=str(path))
    # No image.reload(), pixels access, or separate image loading before rendering.
    near(center(render('cold_relative')), [.2, .4, .6])
    print('COLD_IMAGE_PASS', flush=True)
else:
    _, material, emission = scene()
    image = bpy.data.images.new('RelativeLUT', width=16, height=4, float_buffer=True)
    pixels = np.ones((4, 16, 4), dtype=np.float32)
    pixels[:, :, :3] = (.2, .4, .6)
    image.pixels.foreach_set(pixels.reshape(-1))
    image.filepath_raw = str(assets / 'lut.exr')
    image.file_format = 'OPEN_EXR'
    image.save()
    image.source = 'FILE'
    image.filepath = '//lut.exr'
    texture = material.node_tree.nodes.new('ShaderNodeImageToClosure')
    texture.image = image
    texture.texture_type = 'LUT_STRIP_3D'
    function = glsl(material.node_tree,
                    'vec3 probe(sampler3D volume) { return texture(volume,vec3(.5)).rgb; }', 'probe')
    material.node_tree.links.new(texture.outputs[0], socket(function.inputs, 'volume'))
    material.node_tree.links.new(function.outputs[0], emission.inputs['Color'])
    bpy.ops.wm.save_as_mainfile(filepath=str(path), relative_remap=False)
    output = background_child(Path(__file__).resolve(), ['--cold-child'])
    assert 'COLD_IMAGE_PASS' in output, output
    print(output, flush=True)
