# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import sys
import json
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import scene, glsl, socket, OUT
import bpy
import numpy as np
OUT.mkdir(parents=True, exist_ok=True)
s, m, e = scene()
s.render.resolution_x = 160
s.render.resolution_y = 128
s.render.film_transparent = True
obj = bpy.context.object
# A two-unit plane has Generated XY = local position * .5 + .5, Z = .5.
for v in obj.data.vertices:
    v.co *= .5
s.camera.data.ortho_scale = 2.5
nodes = m.node_tree.nodes
links = m.node_tree.links
output = next(n for n in nodes if n.type == 'OUTPUT_MATERIAL')
tess = nodes.new('ShaderNodeTessellationControl')
tess.mode = 'FIXED'
tess.inputs['Max Level'].default_value = 4
coords = nodes.new('ShaderNodeTexCoord')
ref = nodes.new('ShaderNodeVectorMath')
ref.operation = 'MULTIPLY_ADD'
ref.inputs[1].default_value = (.5, .5, .5)
ref.inputs[2].default_value = (.5, .5, .5)
links.new(coords.outputs['Object'], ref.inputs[0])
noise = nodes.new('ShaderNodeTexNoise')
noise.inputs['Scale'].default_value = 6
links.new(noise.outputs['Fac'], e.inputs['Color'])


def render(name):
    s.render.filepath = str(OUT / (name + '.exr'))
    bpy.ops.render.render(write_still=True)
    im = bpy.data.images.load(s.render.filepath, check_existing=False)
    a = np.array(im.pixels[:], dtype=np.float32).reshape(128, 160, 4)
    bpy.data.images.remove(im)
    assert np.isfinite(a).all()
    assert not ((a[:, :, 0] > .95) & (a[:, :, 1] < .01) & (a[:, :, 2] > .95)).any(), name
    return a


def compare(a, b, label, tol=.003):
    error = float(np.max(np.abs(a - b)))
    print('COMPARE', label, error, flush=True)
    assert error < tol, (label, error)
    results.append({'case': label, 'max_error': error})
    return error


results = []
normal = render('noise_implicit_no_tess')
links.new(tess.outputs[0], output.inputs['Tessellation'])
actual = render('noise_implicit_tess')
mask = actual[:, :, 3] > .99
variation = float(actual[:, :, 0][mask].std())
print('DEFAULT_VARIATION', variation, flush=True)
assert variation > .03, ('Default texture coordinates became constant', variation)
compare(normal, actual, 'noise_on_off')

# Both the implicit path and explicit Generated must agree with the independent object-space map.
for kind in ['ShaderNodeTexNoise', 'ShaderNodeTexVoronoi', 'ShaderNodeTexWave']:
    tex = nodes.new(kind)
    tex.inputs['Scale'].default_value = 6
    links.new(tex.outputs[0], e.inputs['Color'])
    implicit = render(kind + '_implicit')
    links.new(ref.outputs[0], tex.inputs['Vector'])
    reference = render(kind + '_reference')
    compare(implicit, reference, kind)
    links.new(coords.outputs['Generated'], tex.inputs['Vector'])
    explicit = render(kind + '_generated')
    compare(explicit, reference, kind + '_generated')
    nodes.remove(tex)

# Reproduce the user's Noise -> Displacement chain, including actual vertex displacement.
for link in list(e.inputs['Color'].links):
    links.remove(link)
e.inputs['Color'].default_value = (.2, .6, .3, 1)
s.camera.location = (0, -3, 3)
s.camera.rotation_euler = (-s.camera.location).to_track_quat('-Z', 'Y').to_euler()
disp = nodes.new('ShaderNodeDisplacement')
disp.space = 'OBJECT'
disp.inputs['Midlevel'].default_value = .55
disp.inputs['Scale'].default_value = .15
links.new(noise.outputs['Fac'], disp.inputs['Height'])
links.new(disp.outputs[0], output.inputs['Displacement'])
m.displacement_method = 'BOTH'
m.max_vertex_displacement = .15
implicit = render('displacement_implicit')
links.new(ref.outputs[0], noise.inputs['Vector'])
reference = render('displacement_reference')
compare(implicit, reference, 'displacement', .006)

# A deformed mesh may carry a real ORCO buffer; its behavior must remain consistent.
for link in list(output.inputs['Displacement'].links):
    links.remove(link)
links.new(coords.outputs['Generated'], e.inputs['Color'])
obj.shape_key_add(name='Basis')
key = obj.shape_key_add(name='Deformed')
for v in key.data:
    if v.co.x > 0:
        v.co.x += .35
        v.co.z += .2
key.value = 1
for link in list(output.inputs['Tessellation'].links):
    links.remove(link)
normal = render('deformed_generated_no_tess')
links.new(tess.outputs[0], output.inputs['Tessellation'])
actual = render('deformed_generated_tess')
compare(normal, actual, 'deformed_generated', .006)

# Image Texture defaults to the active UV map, unlike procedural Generated coordinates.
image = bpy.data.images.new('UV Gradient', width=8, height=8, float_buffer=True)
image.colorspace_settings.name = 'Non-Color'
pixels = np.ones((8, 8, 4), dtype=np.float32)
for y in range(8):
    for x in range(8):
        pixels[y, x, :3] = (x / 7, y / 7, .3)
image.pixels.foreach_set(pixels.reshape(-1))
image.update()
tex = nodes.new('ShaderNodeTexImage')
tex.image = image
links.new(tex.outputs['Color'], e.inputs['Color'])
implicit = render('image_uv_implicit')
links.new(coords.outputs['UV'], tex.inputs['Vector'])
explicit = render('image_uv_explicit')
compare(implicit, explicit, 'active_uv', .003)

# An identity shape key changes ORCO storage, not the texture mapping under Phong smoothing.
s, m, e = scene()
s.render.resolution_x = 160
s.render.resolution_y = 128
bpy.data.objects.remove(bpy.context.object, do_unlink=True)
bpy.ops.mesh.primitive_uv_sphere_add(segments=8, ring_count=4, radius=1)
obj = bpy.context.object
obj.data.materials.append(m)
for p in obj.data.polygons:
    p.use_smooth = True
nodes = m.node_tree.nodes
links = m.node_tree.links
coords = nodes.new('ShaderNodeTexCoord')
links.new(coords.outputs['Generated'], e.inputs['Color'])
tess = nodes.new('ShaderNodeTessellationControl')
tess.inputs['Max Level'].default_value = 4
tess.inputs['Phong Smoothing'].default_value = 1
output = next(n for n in nodes if n.type == 'OUTPUT_MATERIAL')
links.new(tess.outputs[0], output.inputs['Tessellation'])
fallback = render('phong_fallback_orco')
obj.shape_key_add(name='Basis')
key = obj.shape_key_add(name='Identity')
key.value = 1
stored = render('phong_stored_orco')
compare(fallback, stored, 'phong_orco_parity', .003)

import gpu
gpu.init()
backend = gpu.platform.backend_type_get().lower()
print('BACKEND', backend, flush=True)
(OUT / (backend + '.json')).write_text(json.dumps(results, indent=2), encoding='utf-8')
print('TESS_DEFAULT_COORDINATES_PASS', flush=True)
