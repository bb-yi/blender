# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Unchanged reroute sockets must not invalidate shared material outputs."""
import sys
from pathlib import Path
import bpy
import gpu
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import scene, glsl, near

s, material, emission = scene()
s.eevee.taa_samples = 1
s.eevee.use_taa_reprojection = False
s.eevee.use_outline = False
s.eevee.use_shadows = False
group = bpy.data.node_groups.new('Shared reroutes', 'ShaderNodeTree')
group.interface.new_socket(name='Color', in_out='OUTPUT', socket_type='NodeSocketColor')
output = group.nodes.new('NodeGroupOutput')
color = group.nodes.new('ShaderNodeRGB')
color.outputs[0].default_value = (.2, .4, .6, 1)
reroute = group.nodes.new('NodeReroute')
group.links.new(color.outputs[0], reroute.inputs[0])
group.links.new(reroute.outputs[0], output.inputs[0])
instance = material.node_tree.nodes.new('ShaderNodeGroup')
instance.node_tree = group
material.node_tree.links.new(instance.outputs[0], emission.inputs['Color'])
# A second visible material exercises shared-group propagation.
other_material = material.copy()
other_material.name = 'Other shared user'
obj = bpy.context.object.copy()
obj.data = obj.data.copy()
obj.data.materials.clear()
obj.data.materials.append(other_material)
obj.location.x = 8
s.collection.objects.link(obj)
unused = glsl(group, 'vec3 unused_color(vec3 color) { return color; }', 'unused_color')
group.links.new(reroute.outputs[0], unused.inputs[0])
assert not any(socket.is_linked for socket in unused.outputs)

gpu.init()
area = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
region = next(r for r in area.regions if r.type == 'WINDOW')
space = area.spaces.active
space.shading.type = 'RENDERED'
space.overlay.show_overlays = False
buffer = gpu.types.GPUOffScreen(64, 64, format='RGBA32F')

def draw(expected):
    bpy.context.view_layer.update()
    deps = bpy.context.evaluated_depsgraph_get()
    projection = s.camera.calc_matrix_camera(deps, x=64, y=64)
    buffer.draw_view3d(s, bpy.context.view_layer, space, region,
                      s.camera.matrix_world.inverted(), projection,
                      do_color_management=False, draw_background=True)
    pixels = np.array(buffer.texture_color.read(), dtype=np.float32).reshape(64, 64, 4)
    assert np.isfinite(pixels).all()
    near(pixels[32, 32, :3], expected)
    return pixels

def edit(operation, expected, *, unchanged=False):
    changed_materials = set()
    def post(scene, deps):
        changed_materials.update(u.id.name for u in deps.updates if isinstance(u.id, bpy.types.Material))
    bpy.app.handlers.depsgraph_update_post.append(post)
    try:
        operation()
        pixels = draw(expected)
    finally:
        bpy.app.handlers.depsgraph_update_post.remove(post)
    if unchanged:
        assert not changed_materials, sorted(changed_materials)
    else:
        assert {material.name, other_material.name} <= changed_materials, changed_materials
    return pixels

try:
    expected = (.2, .4, .6)
    before = draw(expected)
    after = edit(lambda: group.nodes.remove(unused), expected, unchanged=True)
    assert np.array_equal(before, after)
    # A disconnected branch still consumes the live reroute, but cannot affect its output.
    leaf = group.nodes.new('ShaderNodeMath')
    group.links.new(reroute.outputs[0], leaf.inputs[0])
    draw(expected)
    edit(lambda: group.links.remove(leaf.inputs[0].links[0]), expected, unchanged=True)
    # Real value and topology edits must still update both material users immediately.
    edit(lambda: setattr(color.outputs[0], 'default_value', (.7, .1, .3, 1)), (.7, .1, .3))
    scalar = group.nodes.new('ShaderNodeValue')
    scalar.outputs[0].default_value = .35
    draw((.7, .1, .3))
    edit(lambda: group.links.new(scalar.outputs[0], reroute.inputs[0]), (.35,) * 3)
    assert reroute.inputs[0].type == 'VALUE'
    leaf2 = group.nodes.new('ShaderNodeMath')
    group.links.new(reroute.outputs[0], leaf2.inputs[0])
    draw((.35,) * 3)
    edit(lambda: group.nodes.remove(leaf2), (.35,) * 3, unchanged=True)
    vector = group.nodes.new('ShaderNodeCombineXYZ')
    for socket, value in zip(vector.inputs, (.1, .3, .5)):
        socket.default_value = value
    draw((.35,) * 3)
    edit(lambda: group.links.new(vector.outputs[0], reroute.inputs[0]), (.1, .3, .5))
    assert reroute.inputs[0].type == 'VECTOR'
    leaf3 = group.nodes.new('ShaderNodeVectorMath')
    group.links.new(reroute.outputs[0], leaf3.inputs[0])
    draw((.1, .3, .5))
    edit(lambda: group.nodes.remove(leaf3), (.1, .3, .5), unchanged=True)
    # Use the editor delete operator as well as the RNA collection API.
    editor = next(a for a in bpy.context.screen.areas if a != area)
    editor.type = 'NODE_EDITOR'
    editor.spaces.active.tree_type = 'ShaderNodeTree'
    editor.spaces.active.path.start(material.node_tree)
    editor.spaces.active.path.append(group, node=instance)
    leaf4 = group.nodes.new('ShaderNodeMath')
    leaf4_name = leaf4.name
    group.links.new(reroute.outputs[0], leaf4.inputs[0])
    for node in group.nodes:
        node.select = node == leaf4
    draw((.1, .3, .5))
    def delete_in_editor():
        with bpy.context.temp_override(area=editor):
            assert bpy.ops.node.delete() == {'FINISHED'}
    edit(delete_in_editor, (.1, .3, .5), unchanged=True)
    assert leaf4_name not in group.nodes
    # Replacing a live reroute input must not keep a cached old result.
    edit(lambda: group.links.new(color.outputs[0], reroute.inputs[0]), (.7, .1, .3))
    assert reroute.inputs[0].type == 'RGBA'
finally:
    buffer.free()
print('Backend:', gpu.platform.backend_type_get())
print('SHADER_REROUTE_NOOP_OK')
