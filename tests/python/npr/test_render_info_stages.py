# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import json
from pathlib import Path
import bpy
import numpy as np
import OpenImageIO as oiio
import sys

backend = sys.argv[sys.argv.index('--gpu-backend') + 1] if '--gpu-backend' in sys.argv else 'default'
sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import OUT as OUTPUT_ROOT
OUT = OUTPUT_ROOT / backend
OUT.mkdir(parents=True, exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
s = bpy.context.scene
s.render.engine = 'BLENDER_EEVEE'
s.render.resolution_x = 64
s.render.resolution_y = 48
s.render.resolution_percentage = 100
s.eevee.taa_render_samples = 1
s.render.film_transparent = True
s.view_settings.view_transform = 'Standard'
s.view_layers[0].use_pass_z = True
s.view_layers[0].use_pass_vector = True
c = bpy.data.cameras.new('Camera')
c.type = 'ORTHO'
c.ortho_scale = 3
cam = bpy.data.objects.new('Camera', c)
s.collection.objects.link(cam)
cam.location = (0, 0, 4)
s.camera = cam
bpy.ops.mesh.primitive_plane_add(size=1)
obj = bpy.context.object
obj.location.x = -.05
obj.keyframe_insert(data_path='location', frame=1)
obj.location.x = .05
obj.keyframe_insert(data_path='location', frame=3)
s.frame_set(2)
m = bpy.data.materials.new('RenderInfoStages')
m.use_nodes = True
m.displacement_method = 'DISPLACEMENT'
obj.data.materials.append(m)
n = m.node_tree.nodes
l = m.node_tree.links
n.clear()
o = n.new('ShaderNodeOutputMaterial')
e = n.new('ShaderNodeEmission')
e.inputs['Color'].default_value = (.2, .6, .1, 1)
l.new(e.outputs[0], o.inputs['Surface'])
ri = n.new('ShaderNodeRenderInfo')
scale = n.new('ShaderNodeVectorMath')
scale.operation = 'SCALE'
scale.inputs['Scale'].default_value = .002
l.new(scale.outputs['Vector'], o.inputs['Displacement'])
s.use_nodes = True
tree = bpy.data.node_groups.new('PassValidation', 'CompositorNodeTree')
s.compositing_node_group = tree
rl = tree.nodes.new('CompositorNodeRLayers')
fo = tree.nodes.new('CompositorNodeOutputFile')
fo.directory = str(OUT)
fo.file_output_items.clear()
for name, kind in [('Depth', 'FLOAT'), ('Vector', 'VECTOR')]:
    fo.file_output_items.new(kind, name)
    tree.links.new(rl.outputs[name], fo.inputs[name])


def render(label):
    p = OUT / (label + '.exr')
    s.render.image_settings.file_format = 'OPEN_EXR'
    s.render.image_settings.color_mode = 'RGBA'
    s.render.image_settings.color_depth = '32'
    s.render.filepath = str(p)
    fo.file_name = label + '_passes'
    bpy.ops.render.render(write_still=True)
    im = bpy.data.images.load(str(p), check_existing=False)
    a = np.array(im.pixels[:], dtype=np.float32).reshape(-1, 4)
    bpy.data.images.remove(im)
    assert np.isfinite(a).all(), label
    assert not ((a[:, 0] > .9) & (a[:, 1] < .01) & (a[:, 2] > .9)).any(), 'Error material: ' + label
    assert a[:, 3].max() > .9, 'Missing geometry: ' + label
    inp = oiio.ImageInput.open(str(OUT / (fo.file_name + '.exr')))
    assert inp is not None
    arrays = [a.reshape(-1)]
    index = 0
    while True:
        data = inp.read_image(format='float')
        assert np.isfinite(data).all(), label
        arrays.append(data.reshape(-1))
        index += 1
        if not inp.seek_subimage(index, 0):
            break
    inp.close()
    return np.concatenate(arrays)


results = []
for samples in (1, 4):
    s.eevee.taa_render_samples = samples
    values = {'Frag Coord': (0, 0, 0), 'Width': (64,) * 3, 'Height': (48,) * 3,
              'Resolution': (64, 48, 0), 'Total Samples': (samples,) * 3}
    if samples == 1:
        values['Current Sample'] = (0,) * 3
    for name, value in values.items():
        for link in list(scale.inputs[0].links):
            l.remove(link)
        scale.inputs[0].default_value = value
        ref = render(f'{samples}_{name}_reference')
        l.new(ri.outputs[name], scale.inputs[0])
        actual = render(f'{samples}_{name}_node')
        error = float(np.abs(actual - ref).max())
        assert error < 1e-5, (samples, name, error)
        results.append({'samples': samples, 'output': name, 'max_error': error})
shell = n.new('ShaderNodeOutputOutlineShell')
shell.inputs['Color'].default_value = (.1, .3, .8, 1)
shell.inputs['Strength'].default_value = .05
m.outline_shell_cull_method = 'NONE'
for link in list(o.inputs['Displacement'].links):
    l.remove(link)
l.new(scale.outputs['Vector'], shell.inputs['Displacement'])
for mode in ('DEFERRED', 'FORWARD'):
    m.outline_shell_render_method = mode
    for name, value in [('Height', (48,) * 3), ('Total Samples', (4,) * 3)]:
        for link in list(scale.inputs[0].links):
            l.remove(link)
        scale.inputs[0].default_value = value
        ref = render(f'shell_{mode}_{name}_reference')
        l.new(ri.outputs[name], scale.inputs[0])
        actual = render(f'shell_{mode}_{name}_node')
        error = float(np.abs(actual - ref).max())
        assert error < 1e-5, (mode, name, error)
        results.append({'shell': mode, 'output': name, 'max_error': error})
print('RENDER_INFO_STAGES_PASS', json.dumps(results))
(OUT / 'report.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
