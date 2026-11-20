# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Sparse slots and cached attribute requests must preserve evaluated per-object values."""
import sys
from pathlib import Path

import bpy
import gpu
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import scene, near

s, material, emission = scene()
s.eevee.taa_samples = 1
s.eevee.use_taa_reprojection = False
s.eevee.use_outline = False
s.eevee.use_shadows = False
s.camera.data.ortho_scale = 3.2
left = bpy.context.object
left.location.x = -.6
left.scale = (.25, .25, .25)
right = left.copy()
right.location.x = .6
s.collection.objects.link(right)
mesh = left.data
tree = material.node_tree
output = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeOutputMaterial')
attribute = tree.nodes.new('ShaderNodeAttribute')
attribute.attribute_type = 'OBJECT'
attribute.attribute_name = 'Tint'
tree.links.new(attribute.outputs['Color'], emission.inputs['Color'])

colors = [((.8, .1, .2, 1), (.1, .2, .8, 1)),
          ((.2, .7, .1, 1), (.7, .4, .1, 1))]
for frame, values in zip((1, 3), colors):
    for obj, value in zip((left, right), values):
        obj['Tint'] = value
        obj.keyframe_insert(data_path='["Tint"]', frame=frame)

unused = material.copy()
unused.name = 'UnusedSlot'
unused.node_tree.nodes.remove(unused.node_tree.nodes.get(attribute.name))
unused_emission = unused.node_tree.nodes.get(emission.name)
unused_emission.inputs['Color'].default_value = (.15, .25, .35, 1)
mesh.materials.clear()
for index in range(63):
    mesh.materials.append(unused)
mesh.materials.append(material)
for polygon in mesh.polygons:
    polygon.material_index = 63

gpu.init()
area = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
region = next(r for r in area.regions if r.type == 'WINDOW')
space = area.spaces.active
space.shading.type = 'RENDERED'
space.overlay.show_overlays = False
buffer = gpu.types.GPUOffScreen(96, 64, format='RGBA32F')


def draw():
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    projection = s.camera.calc_matrix_camera(depsgraph, x=96, y=64)
    buffer.draw_view3d(s, bpy.context.view_layer, space, region,
                      s.camera.matrix_world.inverted(), projection,
                      do_color_management=False, draw_background=True)
    pixels = np.array(buffer.texture_color.read(), dtype=np.float32).reshape(64, 96, 4)
    assert np.isfinite(pixels).all()
    return pixels


def check_colors(pixels, values):
    near(pixels[32, 30, :3], values[0][:3])
    near(pixels[32, 66, :3], values[1][:3])


def equal(actual, expected, label):
    error = float(np.max(np.abs(actual - expected)))
    assert error < .005, (label, error)


try:
    # Identical material/mesh keys must still read different values from each object each frame.
    sparse = {}
    for frame, expected in ((1, colors[0]), (3, colors[1]), (1, colors[0])):
        s.frame_set(frame)
        sparse[frame] = draw()
        check_colors(sparse[frame], expected)

    # A previously unused slot becomes live immediately, including after returning to slot 63.
    for polygon in mesh.polygons:
        polygon.material_index = 0
    mesh.update()
    check_colors(draw(), ((.15, .25, .35, 1),) * 2)
    for polygon in mesh.polygons:
        polygon.material_index = 63
    mesh.update()
    equal(draw(), sparse[1], 'restored sparse slot')

    # Edit mode retains its fallback path and produces the same image with overlays disabled.
    bpy.context.view_layer.objects.active = left
    left.select_set(True)
    right.select_set(False)
    bpy.ops.object.mode_set(mode='EDIT')
    equal(draw(), sparse[1], 'edit mode fallback')
    bpy.ops.object.mode_set(mode='OBJECT')

    mesh.materials.clear()
    mesh.materials.append(material)
    for polygon in mesh.polygons:
        polygon.material_index = 0
    mesh.update()
    for frame, expected in ((1, colors[0]), (3, colors[1])):
        s.frame_set(frame)
        pixels = draw()
        check_colors(pixels, expected)
        equal(pixels, sparse[frame], 'compact versus sparse')

    # The merged mesh request must include attributes used only by the attached NPR tree.
    npr_color = (.12, .32, .52, 1)
    layer = mesh.color_attributes.new(name='NPRColor', type='FLOAT_COLOR', domain='CORNER')
    for item in layer.data:
        item.color = npr_color
    npr = bpy.data.node_groups.new('NPR attribute only', 'ShaderNodeTree')
    npr_attribute = npr.nodes.new('ShaderNodeAttribute')
    npr_attribute.attribute_name = layer.name
    npr_output = npr.nodes.new('ShaderNodeNPR_Output')
    npr.links.new(npr_attribute.outputs['Color'], npr_output.inputs['Color'])
    output.nprtree = npr
    mesh.update()
    npr_pixels = draw()
    # NPR Output ignores unlinked defaults; its reference must remain a connected color.
    npr_reference = npr.nodes.new('ShaderNodeRGB')
    npr_reference.outputs['Color'].default_value = npr_color
    npr.links.new(npr_reference.outputs['Color'], npr_output.inputs['Color'])
    equal(draw(), npr_pixels, 'NPR-only mesh attribute versus constant')
    output.nprtree = None

    # Shell color requires a different attribute, and the body no longer draws any color.
    shell_color = (.65, .15, .35, 1)
    shell_layer = mesh.color_attributes.new(name='ShellColor', type='FLOAT_COLOR', domain='CORNER')
    for item in shell_layer.data:
        item.color = shell_color
    transparent = tree.nodes.new('ShaderNodeBsdfTransparent')
    tree.links.new(transparent.outputs[0], output.inputs['Surface'])
    shell = tree.nodes.new('ShaderNodeOutputOutlineShell')
    shell.inputs['Strength'].default_value = 0
    shell.inputs['Alpha'].default_value = 1
    material.outline_shell_cull_method = 'NONE'
    shell_attribute = tree.nodes.new('ShaderNodeAttribute')
    shell_attribute.attribute_name = shell_layer.name
    shell_link = tree.links.new(shell_attribute.outputs['Color'], shell.inputs['Color'])
    mesh.update()
    shell_pixels = draw()
    check_colors(shell_pixels, (shell_color, shell_color))
    tree.links.remove(shell_link)
    # Unlike NPR Output, Outline Shell explicitly turns an unlinked Color into a uniform.
    shell.inputs['Color'].default_value = shell_color
    equal(draw(), shell_pixels, 'Shell-only mesh attribute versus constant')
finally:
    if left.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')
    buffer.free()

print('Backend:', gpu.platform.backend_type_get())
print('SYNC_MATERIAL_REQUESTS_OK')
