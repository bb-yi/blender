# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import *
s, m, e = scene()
size = 8
colors = np.array([[2, .1, .2], [.2, 2, .3], [.3, .4, 2], [1, .5, .1], [.1, .7, .8], [.8, .1, .6]], dtype=np.float32)
pixels = np.ones((size, size * 6, 4), dtype=np.float32)
for i, c in enumerate(colors):
    pixels[:, i * size:(i + 1) * size, :3] = c
im = bpy.data.images.new('CubeFaces', width=size * 6, height=size, float_buffer=True)
im.colorspace_settings.name = 'Non-Color'
im.alpha_mode = 'CHANNEL_PACKED'
im.pixels.foreach_set(pixels.reshape(-1))
im.update()
tex = m.node_tree.nodes.new('ShaderNodeImageToClosure')
tex.image = im
tex.texture_type = 'CUBE_STRIP'
f = glsl(
    m.node_tree,
    'vec3 probe(samplerCube env, vec3 direction, float lod) { return textureLod(env,direction,lod).rgb; }',
    'probe')
m.node_tree.links.new(tex.outputs[0], socket(f.inputs, 'env'))
m.node_tree.links.new(f.outputs[0], e.inputs['Color'])
directions = [(1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)]
for i, d in enumerate(directions):
    socket(f.inputs, 'direction').default_value = d
    print('CUBE_DIRECTION', i, tuple(socket(f.inputs, 'direction').default_value), flush=True)
    for lod in (0, 2, 3):
        socket(f.inputs, 'lod').default_value = lod
        actual = center(render(f'cube_face{i}_lod{lod}'))
        near(actual, colors[i])
size_node = glsl(m.node_tree, 'vec3 dimensions(samplerCube env) { return vec3(textureSize(env,1),0); }', 'dimensions')
m.node_tree.links.new(tex.outputs[0], socket(size_node.inputs, 'env'))
m.node_tree.links.new(size_node.outputs[0], e.inputs['Color'])
near(center(render('cube_size')), [4, 4, 0])
group = bpy.data.node_groups.new('CubeGroup', 'ShaderNodeTree')
group.interface.new_socket(name='Cube', in_out='INPUT', socket_type='NodeSocketClosure')
group.interface.new_socket(name='Color', in_out='OUTPUT', socket_type='NodeSocketColor')
gi = group.nodes.new('NodeGroupInput')
go = group.nodes.new('NodeGroupOutput')
gf = glsl(group, 'vec3 grouped(samplerCube env) { return texture(env,vec3(0,0,-1)).rgb; }', 'grouped')
group.links.new(gi.outputs[0], socket(gf.inputs, 'env'))
group.links.new(gf.outputs[0], go.inputs[0])
gn = m.node_tree.nodes.new('ShaderNodeGroup')
gn.node_tree = group
m.node_tree.links.new(tex.outputs[0], gn.inputs[0])
m.node_tree.links.new(gn.outputs[0], e.inputs['Color'])
near(center(render('cube_group')), colors[5])
# A continuous direction field catches swapped/flipped faces and filtering across cube edges.
for face in range(6):
    for y in range(size):
        for x in range(size):
            u = 2 * (x + .5) / size - 1
            v = 2 * (y + .5) / size - 1
            direction = np.array([(1, -v, -u), (-1, -v, u), (u, 1, v), (u, -1, -v), (u, -v, 1), (-u, -v, -1)][face])
            pixels[y, face * size + x, :3] = direction / np.linalg.norm(direction) * .5 + .5
im.pixels.foreach_set(pixels.reshape(-1))
im.update()
m.node_tree.links.new(f.outputs[0], e.inputs['Color'])
socket(f.inputs, 'lod').default_value = 0
for i, d in enumerate([(1, .5, .25), (-1, .2, .4), (.99, 0, 1.01), (1.01, 0, .99), (0, 1.01, .99), (0, .99, 1.01)]):
    socket(f.inputs, 'direction').default_value = d
    near(center(render('cube_edge_' + str(i))), np.array(d) / np.linalg.norm(d) * .5 + .5, .03)
# Explicit LOD must select a mip even when interpolation is set to Closest.
for y in range(size):
    for x in range(size * 6):
        pixels[y, x, :3] = ((x + y) % 2, .2, .3)
im.pixels.foreach_set(pixels.reshape(-1))
im.update()
tex.interpolation = 'Closest'
socket(f.inputs, 'direction').default_value = (1, .4, .25)
socket(f.inputs, 'lod').default_value = 3
near(center(render('cube_closest_lod')), [.5, .2, .3])
# Pixel edits must replace the cached cube and regenerate mip levels.
pixels[:, :, :3] = (.35, .25, .15)
im.pixels.foreach_set(pixels.reshape(-1))
im.update()
m.node_tree.links.new(f.outputs[0], e.inputs['Color'])
near(center(render('cube_updated')), [.35, .25, .15])
assets = OUT / 'assets'
assets.mkdir(exist_ok=True)
im.pack()
bpy.ops.wm.save_as_mainfile(filepath=str(assets / 'cube.blend'))
bpy.ops.wm.open_mainfile(filepath=str(assets / 'cube.blend'))
near(center(render('cube_reopened')), [.35, .25, .15])
print('CUBE_PASS', flush=True)
