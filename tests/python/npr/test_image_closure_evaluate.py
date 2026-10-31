# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Image closures retain texture identity across routing and support explicit evaluation.

Run with isolated user resources and BLENDER_TEST_OUTPUT_DIR inside the workspace.
The same test is run on OpenGL and Vulkan by the Release case.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bpy
import gpu
import numpy as np

from material_compat_test_utils import OUT, scene, glsl, render, center, near, background_child

SOURCE = """
vec3 sample_image(sampler2D image, vec3 uv) {
  vec4 c = texture(image, uv.xy);
  return vec3(c.r, c.g, c.a);
}
"""
COLORS = [(0.1, 0.2, 0.3, 0.25), (0.7, 0.4, 0.1, 0.5),
          (0.3, 0.8, 0.2, 0.75), (0.9, 0.6, 0.5, 1.0)]


def refresh(node):
    name = node.function_name
    node.function_name = ''
    node.function_name = name
    node.id_data.interface_update(bpy.context)
    node.id_data.update_tag()
    bpy.context.view_layer.update()


def new_image(tree, colors=COLORS, width=2, height=2, kind='IMAGE_2D'):
    image = bpy.data.images.new('ImageClosurePixels', width, height, alpha=True, float_buffer=True)
    image.colorspace_settings.name = 'Non-Color'
    image.alpha_mode = 'CHANNEL_PACKED'
    image.pixels.foreach_set(np.array(colors, dtype=np.float32).reshape(-1))
    image.update()
    node = tree.nodes.new('ShaderNodeImageToClosure')
    node.image = image
    # The enum name is checked against RNA below, allowing no silent mode fallback.
    node.texture_type = kind
    node.interpolation = 'Closest'
    return node


def reroutes(tree, source, count=4):
    for _ in range(count):
        node = tree.nodes.new('NodeReroute')
        tree.links.new(source, node.inputs[0])
        source = node.outputs[0]
    return source


def bundle(tree, source, nested=False, joined=False):
    combine = tree.nodes.new('NodeCombineBundle')
    combine.bundle_items.new('CLOSURE', 'Image')
    tree.links.new(source, combine.inputs['Image'])
    source = combine.outputs['Bundle']
    if nested:
        outer = tree.nodes.new('NodeCombineBundle')
        outer.bundle_items.new('BUNDLE', 'Nested')
        tree.links.new(source, outer.inputs['Nested'])
        separate = tree.nodes.new('NodeSeparateBundle')
        separate.bundle_items.new('BUNDLE', 'Nested')
        tree.links.new(outer.outputs['Bundle'], separate.inputs['Bundle'])
        source = separate.outputs['Nested']
    if joined:
        join = tree.nodes.new('NodeJoinBundle')
        tree.links.new(source, join.inputs['Bundle'])
        source = join.outputs['Bundle']
    separate = tree.nodes.new('NodeSeparateBundle')
    separate.bundle_items.new('CLOSURE', 'Image')
    tree.links.new(source, separate.inputs['Bundle'])
    return separate.outputs['Image']


def group_interface(name, sockets):
    group = bpy.data.node_groups.new(name, 'ShaderNodeTree')
    for label, direction, kind in sockets:
        group.interface.new_socket(name=label, in_out=direction, socket_type=kind)
    group.interface_update(bpy.context)
    return group, group.nodes.new('NodeGroupInput'), group.nodes.new('NodeGroupOutput')


def pass_group(depth, socket_type='NodeSocketClosure'):
    group, inp, out = group_interface('Image Closure Pass', [
        ('Source', 'INPUT', socket_type), ('Source', 'OUTPUT', socket_type)])
    source = reroutes(group, inp.outputs['Source'])
    if depth:
        child = group.nodes.new('ShaderNodeGroup')
        child.node_tree = pass_group(depth - 1, socket_type)
        group.links.new(source, child.inputs['Source'])
        source = child.outputs['Source']
    group.links.new(reroutes(group, source), out.inputs['Source'])
    return group


def route(tree, source, kind):
    if kind == 'direct':
        return source
    if kind == 'reroutes':
        return reroutes(tree, source)
    if kind in {'bundle', 'nested_bundle', 'join_bundle'}:
        return bundle(tree, source, nested=kind == 'nested_bundle', joined=kind == 'join_bundle')
    if kind == 'bundle_group':
        combine = tree.nodes.new('NodeCombineBundle')
        combine.bundle_items.new('CLOSURE', 'Image')
        tree.links.new(source, combine.inputs['Image'])
        node = tree.nodes.new('ShaderNodeGroup')
        node.node_tree = pass_group(2, 'NodeSocketBundle')
        tree.links.new(combine.outputs['Bundle'], node.inputs['Source'])
        sep = tree.nodes.new('NodeSeparateBundle')
        sep.bundle_items.new('CLOSURE', 'Image')
        tree.links.new(node.outputs['Source'], sep.inputs['Bundle'])
        return sep.outputs['Image']
    if kind in {'groups', 'muted_group', 'mixed'}:
        node = tree.nodes.new('ShaderNodeGroup')
        node.node_tree = pass_group(2)
        node.mute = kind == 'muted_group'
        tree.links.new(source, node.inputs['Source'])
        source = node.outputs['Source']
        return reroutes(tree, bundle(tree, source, nested=True, joined=True)) if kind == 'mixed' else source
    raise AssertionError(kind)


def sync(tree, node, group_path=(), input_keys=('UV',)):
    tree.interface_update(bpy.context)
    bpy.context.view_layer.update()
    area = bpy.context.screen.areas[0]
    area.type = 'NODE_EDITOR'
    space = area.spaces.active
    space.tree_type = 'ShaderNodeTree'
    space.pin = True
    space.node_tree = tree
    space.path.start(tree)
    for group_node in group_path:
        space.path.append(group_node.node_tree, node=group_node)
    edit_tree = node.id_data
    edit_tree.nodes.active = node
    region = next(r for r in area.regions if r.type == 'WINDOW')
    with bpy.context.temp_override(area=area, region=region, space_data=space,
                                   node=node, active_node=node, edit_tree=edit_tree,
                                   node_tree=edit_tree):
        assert bpy.ops.node.sockets_sync() == {'FINISHED'}
    assert sorted((i.name, i.socket_type) for i in node.input_items) == sorted(
        (key, 'VECTOR') for key in input_keys)
    assert [(i.name, i.socket_type) for i in node.output_items] == [('Color', 'RGBA'), ('Alpha', 'FLOAT')]
    space.path.start(tree)


def encoded_evaluation(tree, source, uv, do_sync=True):
    node = tree.nodes.new('NodeEvaluateClosure')
    tree.links.new(source, node.inputs['Closure'])
    tree.interface_update(bpy.context)
    bpy.context.view_layer.update()
    if do_sync:
        sync(tree, node)
    else:
        node.input_items.new('VECTOR', 'UV')
        node.output_items.new('RGBA', 'Color')
        node.output_items.new('FLOAT', 'Alpha')
    node.inputs['UV'].default_value = uv
    sep = tree.nodes.new('ShaderNodeSeparateXYZ')
    combine = tree.nodes.new('ShaderNodeCombineXYZ')
    tree.links.new(node.outputs['Color'], sep.inputs[0])
    tree.links.new(sep.outputs['X'], combine.inputs['X'])
    tree.links.new(sep.outputs['Y'], combine.inputs['Y'])
    tree.links.new(node.outputs['Alpha'], combine.inputs['Z'])
    return node, combine.outputs[0]


def average(tree, a, b):
    mix = tree.nodes.new('ShaderNodeMixRGB')
    mix.inputs[0].default_value = 0.5
    tree.links.new(a, mix.inputs[1])
    tree.links.new(b, mix.inputs[2])
    return mix.outputs[0]


def check_pixel(label, expected):
    actual = center(render(label))
    near(actual, expected, 0.01)
    print(f'IMAGE_CLOSURE_PIXEL {label}: {actual.tolist()}', flush=True)


def test_editor_connection():
    _, mat, emission = scene()
    tree = mat.node_tree
    image = new_image(tree)
    ev = tree.nodes.new('NodeEvaluateClosure')
    image.location = (0, 0)
    ev.location = (300, 0)
    for node in tree.nodes:
        node.select = node in (image, ev)
    tree.nodes.active = ev
    area = bpy.context.screen.areas[0]
    area.type = 'NODE_EDITOR'
    space = area.spaces.active
    space.tree_type = 'ShaderNodeTree'
    space.pin = True
    space.node_tree = tree
    region = next(r for r in area.regions if r.type == 'WINDOW')
    with bpy.context.temp_override(area=area, region=region, space_data=space):
        assert bpy.ops.node.link_make(replace=False) == {'FINISHED'}
    assert ev.inputs['Closure'].is_linked
    assert 'UV' in ev.inputs and 'Color' in ev.outputs and 'Alpha' in ev.outputs
    ev.inputs['UV'].default_value = (0.25, 0.75, 0)
    tree.links.new(ev.outputs['Color'], emission.inputs['Color'])
    check_pixel('editor_connection_color', COLORS[2][:3])
    tree.links.new(ev.outputs['Alpha'], emission.inputs['Color'])
    check_pixel('editor_connection_alpha', (0.75,)*3)


def test_routes():
    for kind in ('direct', 'reroutes', 'bundle', 'nested_bundle', 'join_bundle', 'groups', 'muted_group', 'bundle_group', 'mixed'):
        _, mat, emission = scene()
        tree = mat.node_tree
        image = new_image(tree)
        source = route(tree, image.outputs['Closure'], kind)
        fn = glsl(tree, SOURCE, 'sample_image')
        tree.links.new(source, fn.inputs['image'])
        fn.inputs['uv'].default_value = (0.25, 0.25, 0)
        refresh(fn)
        assert fn.parse_status == 'READY', (kind, fn.parse_status)
        ev, encoded = encoded_evaluation(tree, source, (0.75, 0.25, 0))
        # Same provider, native GLSL sampling and independent closure evaluation concurrently.
        tree.links.new(average(tree, fn.outputs['Result'], encoded), emission.inputs['Color'])
        check_pixel(kind, (0.4, 0.3, 0.375))
        ev.inputs['UV'].default_value = (0.75, 0.75, 0)
        check_pixel(kind + '_uv_changed', (0.5, 0.4, 0.625))
        assert fn.parse_status == 'READY'
        # Synchronization must preserve the existing input value and output connections.
        sync(tree, ev)
        near(tuple(ev.inputs['UV'].default_value), (0.75, 0.75, 0))
        assert ev.outputs['Color'].is_linked and ev.outputs['Alpha'].is_linked


def consumer_group(depth, evaluate):
    group, inp, out = group_interface('Image Consumer', [
        ('Image', 'INPUT', 'NodeSocketClosure'), ('UV', 'INPUT', 'NodeSocketVector'),
        ('Color', 'OUTPUT', 'NodeSocketColor')])
    source = route(group, inp.outputs['Image'], 'mixed')
    if depth:
        child = group.nodes.new('ShaderNodeGroup')
        child.node_tree = consumer_group(depth-1, evaluate)
        group.links.new(source, child.inputs['Image'])
        group.links.new(inp.outputs['UV'], child.inputs['UV'])
        color = child.outputs['Color']
    elif evaluate:
        ev, color = encoded_evaluation(group, source, (0, 0, 0), do_sync=False)
        group.links.new(inp.outputs['UV'], ev.inputs['UV'])
    else:
        fn = glsl(group, SOURCE, 'sample_image')
        group.links.new(source, fn.inputs['image'])
        group.links.new(inp.outputs['UV'], fn.inputs['uv'])
        refresh(fn)
        assert fn.parse_status == 'READY'
        color = fn.outputs['Result']
    group.links.new(color, out.inputs['Color'])
    return group


def test_group_instances():
    for evaluate in (False, True):
        _, mat, emission = scene()
        tree = mat.node_tree
        shared = consumer_group(2, evaluate)
        instances = []
        images = []
        for color, uv in ((COLORS, (0.25, 0.25, 0)), ([COLORS[1]] * 4, (0.75, 0.75, 0))):
            node = tree.nodes.new('ShaderNodeGroup')
            node.node_tree = shared
            image = new_image(tree, color)
            tree.links.new(route(tree, image.outputs[0], 'mixed'), node.inputs['Image'])
            node.inputs['UV'].default_value = uv
            images.append(image)
            instances.append(node)
        tree.links.new(average(tree, *(n.outputs['Color'] for n in instances)), emission.inputs['Color'])
        label = 'group_evaluate' if evaluate else 'group_glsl'
        check_pixel(label, (0.4, 0.3, 0.375))
        tree.links.new(images[0].outputs[0], instances[1].inputs['Image'])
        check_pixel(label + '_reconnect', (0.5, 0.4, 0.625))
        tree.links.remove(instances[1].inputs['Image'].links[0])
        check_pixel(label + '_disconnected', (0.05, 0.1, 0.125) if evaluate else (0.55, 0.6, 0.625))
        tree.links.new(images[0].outputs[0], instances[1].inputs['Image'])
        check_pixel(label + '_restored', (0.5, 0.4, 0.625))
        if evaluate:
            path = [instances[0]]
            while True:
                owner = path[-1].node_tree
                child = next((n for n in owner.nodes if n.type == 'GROUP' and n.node_tree.name.startswith('Image Consumer')), None)
                if child is None:
                    break
                path.append(child)
            ev = next(n for n in path[-1].node_tree.nodes if n.bl_idname == 'NodeEvaluateClosure')
            sync(tree, ev, path)
            check_pixel(label + '_sync_in_group', (0.5, 0.4, 0.625))
            for image in bpy.data.images:
                if image.source == 'GENERATED':
                    image.pack()
            assets = OUT / 'assets'
            assets.mkdir(exist_ok=True)
            filepath = assets / 'image_closure_nested_groups.blend'
            bpy.ops.wm.save_as_mainfile(filepath=str(filepath))
            output = background_child(Path(__file__), ('--reload', str(filepath)))
            (OUT / 'cold_reload.log').write_text(output, encoding='utf-8')
            assert 'IMAGE_CLOSURE_RELOAD_OK' in output, output


def test_source_group_and_signature_edit():
    _, mat, emission = scene()
    tree = mat.node_tree
    group, inp, out = group_interface('Image Provider', [('Image', 'OUTPUT', 'NodeSocketClosure')])
    image = new_image(group)
    group.links.new(route(group, image.outputs[0], 'mixed'), out.inputs['Image'])
    instance = tree.nodes.new('ShaderNodeGroup')
    instance.node_tree = group
    fn = glsl(tree, SOURCE, 'sample_image')
    tree.links.new(route(tree, instance.outputs['Image'], 'mixed'), fn.inputs['image'])
    fn.inputs['uv'].default_value = (0.25, 0.25, 0)
    refresh(fn)
    assert fn.parse_status == 'READY'
    fn.script.clear()
    fn.script.write(SOURCE.replace('vec3 uv)', 'vec3 uv, float amount)').replace('vec3(c.r, c.g, c.a);', 'vec3(c.r, c.g, c.a) * amount;'))
    refresh(fn)
    assert fn.parse_status == 'READY' and 'amount' in fn.inputs
    fn.inputs['amount'].default_value = 0.5
    tree.links.new(fn.outputs['Result'], emission.inputs['Color'])
    check_pixel('routed_signature_edit', (0.05, 0.1, 0.125))
    ev, color = encoded_evaluation(tree, instance.outputs['Image'], (0.75, 0.75, 0))
    tree.links.new(color, emission.inputs['Color'])
    check_pixel('group_provider_evaluate', (0.9, 0.6, 1.0))


def test_native_texture_operations():
    _, mat, emission = scene()
    tree = mat.node_tree
    pixels = [(float((x+y) % 2), 0.25, 0.75, 0.5) for y in range(8) for x in range(8)]
    image = new_image(tree, pixels, 8, 8)
    source = route(tree, image.outputs[0], 'mixed')
    queries = (
        ('size', 'vec3(textureSize(image,0),textureSize(image,3).x)', (8,8,1)),
        ('fetch', 'vec3(texelFetch(image,ivec2(0,0),0).r,texelFetch(image,ivec2(1,0),0).r,texelFetch(image,ivec2(0,0),0).a)', (0,1,0.5)),
        ('gather', 'vec3(dot(textureGather(image,vec2(0.2),0),vec4(0.25)),0.25,0.75)', (0.5,0.25,0.75)),
        ('lod', 'textureLod(image,vec2(0.0625),3.0).rgb', (0.5,0.25,0.75)),
        ('grad', 'textureGrad(image,vec2(0.0625),vec2(1,0),vec2(0,1)).rgb', (0.5,0.25,0.75)),
    )
    for name, expression, expected in queries:
        image.interpolation = 'Linear' if name in {'lod','grad'} else 'Closest'
        fn = glsl(tree, 'vec3 probe(sampler2D image){return '+expression+';}', 'probe')
        tree.links.new(source, fn.inputs['image'])
        tree.links.new(fn.outputs['Result'], emission.inputs['Color'])
        refresh(fn)
        assert fn.parse_status == 'READY'
        check_pixel('native_' + name, expected)


def test_bundle_group_input_and_addressing():
    _, mat, emission = scene()
    tree = mat.node_tree
    group, inp, out = group_interface('Bundle Consumer', [
        ('Bundle', 'INPUT', 'NodeSocketBundle'), ('Color', 'OUTPUT', 'NodeSocketColor')])
    sep = group.nodes.new('NodeSeparateBundle')
    sep.bundle_items.new('CLOSURE', 'Image')
    group.links.new(reroutes(group, inp.outputs['Bundle']), sep.inputs['Bundle'])
    fn = glsl(group, SOURCE, 'sample_image')
    group.links.new(reroutes(group, sep.outputs['Image']), fn.inputs['image'])
    fn.inputs['uv'].default_value = (1.25, 0.25, 0)
    refresh(fn)
    assert fn.parse_status == 'READY'
    ev, color = encoded_evaluation(group, sep.outputs['Image'], (1.25, 0.25, 0), do_sync=False)
    group.links.new(average(group, fn.outputs['Result'], color), out.inputs['Color'])
    instance = tree.nodes.new('ShaderNodeGroup')
    instance.node_tree = group
    image = new_image(tree)
    combine = tree.nodes.new('NodeCombineBundle')
    combine.bundle_items.new('CLOSURE', 'Image')
    tree.links.new(image.outputs[0], combine.inputs['Image'])
    tree.links.new(combine.outputs[0], instance.inputs['Bundle'])
    tree.links.new(instance.outputs['Color'], emission.inputs['Color'])
    sync(tree, ev, [instance])
    for extension, expected in (('REPEAT',(0.1,0.2,0.25)), ('EXTEND',(0.7,0.4,0.5)),
                                ('CLIP',(0,0,0)), ('MIRROR',(0.7,0.4,0.5))):
        image.extension = extension
        check_pixel('bundle_group_' + extension, expected)
    tree.links.remove(instance.inputs['Bundle'].links[0])
    check_pixel('bundle_group_disconnected', (0.5,0.5,0.5))
    tree.links.new(combine.outputs[0], instance.inputs['Bundle'])
    image.extension = 'REPEAT'
    check_pixel('bundle_group_restored', (0.1,0.2,0.25))


def test_three_dimensional_images():
    for kind in ('LUT_STRIP_3D', 'CUBE_STRIP'):
        _, mat, emission = scene()
        tree = mat.node_tree
        if kind == 'LUT_STRIP_3D':
            pixels = [(float(x),float(y),float(z),0.25+0.5*z) for y in range(2) for z in range(2) for x in range(2)]
            image = new_image(tree,pixels,4,2,kind)
            coord, expected, sampler = (0.25,0.25,0.75),(0,0,0.75),'sampler3D'
        else:
            pixels = [(face/10,0.2,0.3,0.25+face/10) for y in range(2) for face in range(6) for x in range(2)]
            image = new_image(tree,pixels,12,2,kind)
            coord, expected, sampler = (0,0,-1),(0.5,0.2,0.75),'samplerCube'
        source = route(tree,image.outputs[0],'mixed')
        fn = glsl(tree,'vec3 probe('+sampler+' image,vec3 uv){vec4 c=texture(image,uv);return vec3(c.r,c.g,c.a);}','probe')
        tree.links.new(source,fn.inputs['image'])
        fn.inputs['uv'].default_value = coord
        refresh(fn)
        assert fn.parse_status == 'READY'
        ev, color = encoded_evaluation(tree,source,coord)
        tree.links.new(average(tree,fn.outputs['Result'],color),emission.inputs['Color'])
        check_pixel(kind,expected)
        ev.input_items[0].name = 'Vector'
        check_pixel(kind + '_legacy_coordinate', expected)
        # Changing resource dimension must not silently reinterpret a 3D/cube texture as 2D.
        wrong = glsl(tree,SOURCE,'sample_image')
        tree.links.new(source,wrong.inputs['image'])
        refresh(wrong)
        assert wrong.parse_status == 'ERROR', kind


def coordinate_evaluator(tree, source, keys=('Vector',), socket_type='VECTOR'):
    """Author the old interface explicitly, without editor auto synchronization."""
    node = tree.nodes.new('NodeEvaluateClosure')
    for key in keys:
        node.input_items.new(socket_type, key)
    node.output_items.new('RGBA', 'Color')
    node.output_items.new('FLOAT', 'Alpha')
    tree.links.new(source, node.inputs['Closure'])
    return node


def test_legacy_coordinates():
    for kind in ('direct', 'reroutes', 'bundle', 'nested_bundle', 'join_bundle',
                 'groups', 'muted_group', 'bundle_group', 'mixed'):
        _, mat, emission = scene()
        tree = mat.node_tree
        image = new_image(tree)
        ev = coordinate_evaluator(tree, route(tree, image.outputs[0], kind))
        ev.inputs['Vector'].default_value = (0.75, 0.75, 0)
        tree.links.new(ev.outputs['Color'], emission.inputs['Color'])
        check_pixel('legacy_' + kind, COLORS[3][:3])
        ev.inputs['Vector'].default_value = (0.75, 0.25, 0)
        check_pixel('legacy_' + kind + '_changed', COLORS[1][:3])
        tree.links.new(ev.outputs['Alpha'], emission.inputs['Color'])
        check_pixel('legacy_' + kind + '_alpha', (0.5,) * 3)


def test_coordinate_sync():
    for linked in (False, True):
        _, mat, emission = scene()
        tree = mat.node_tree
        ev = coordinate_evaluator(tree, new_image(tree).outputs[0])
        uv = (0.75, 0.25, 0)
        ev.inputs['Vector'].default_value = uv
        identifier = ev.inputs['Vector'].identifier
        color_identifier = ev.outputs['Color'].identifier
        alpha_identifier = ev.outputs['Alpha'].identifier
        if linked:
            coords = tree.nodes.new('ShaderNodeCombineXYZ')
            for socket, value in zip(coords.inputs, (0.25, 0.75, 0)):
                socket.default_value = value
            tree.links.new(coords.outputs[0], ev.inputs['Vector'])
        tree.links.new(ev.outputs['Color'], emission.inputs['Color'])
        sync(tree, ev)
        assert 'Vector' not in ev.inputs
        assert ev.inputs['UV'].identifier == identifier
        assert ev.outputs['Color'].identifier == color_identifier
        assert ev.outputs['Alpha'].identifier == alpha_identifier
        assert ev.outputs['Color'].is_linked
        near(tuple(ev.inputs['UV'].default_value), uv)
        if linked:
            assert ev.inputs['UV'].links[0].from_node == coords
        check_pixel('coordinate_sync_' + str(linked), COLORS[2 if linked else 1][:3])
        # A second sync must leave both identity and the authored default unchanged.
        sync(tree, ev)
        assert ev.inputs['UV'].identifier == identifier
        near(tuple(ev.inputs['UV'].default_value), uv)

    # Shared consumer group: sync follows the edited instance through nested routing.
    _, mat, emission = scene()
    tree = mat.node_tree
    group, inp, out = group_interface('Legacy Image Consumer', [
        ('Image', 'INPUT', 'NodeSocketClosure'), ('Color', 'OUTPUT', 'NodeSocketColor')])
    ev = coordinate_evaluator(group, route(group, inp.outputs['Image'], 'mixed'))
    ev.inputs['Vector'].default_value = (0.75, 0.75, 0)
    identifier = ev.inputs['Vector'].identifier
    group.links.new(ev.outputs['Color'], out.inputs['Color'])
    instance = tree.nodes.new('ShaderNodeGroup')
    instance.node_tree = group
    tree.links.new(new_image(tree).outputs[0], instance.inputs['Image'])
    tree.links.new(instance.outputs['Color'], emission.inputs['Color'])
    sync(tree, ev, [instance])
    assert ev.inputs['UV'].identifier == identifier
    near(tuple(ev.inputs['UV'].default_value), (0.75, 0.75, 0))
    check_pixel('coordinate_sync_group', COLORS[3][:3])


def test_coordinate_precedence():
    # The declared UV always wins, even when Vector appears first or has a link.
    for keys in (('Vector', 'UV'), ('UV', 'Vector')):
        _, mat, emission = scene()
        tree = mat.node_tree
        ev = coordinate_evaluator(tree, new_image(tree).outputs[0], keys)
        ev.inputs['UV'].default_value = (0.75, 0.25, 0)
        coords = tree.nodes.new('ShaderNodeCombineXYZ')
        coords.inputs['X'].default_value = 0.25
        coords.inputs['Y'].default_value = 0.75
        tree.links.new(coords.outputs[0], ev.inputs['Vector'])
        identifier = ev.inputs['UV'].identifier
        tree.links.new(ev.outputs['Color'], emission.inputs['Color'])
        check_pixel('coordinate_precedence_' + keys[0], COLORS[1][:3])
        sync(tree, ev)
        assert ev.inputs['UV'].identifier == identifier and 'Vector' not in ev.inputs
        near(tuple(ev.inputs['UV'].default_value), (0.75, 0.25, 0))
        assert not ev.inputs['UV'].is_linked
        check_pixel('coordinate_precedence_synced_' + keys[0], COLORS[1][:3])


def test_custom_coordinate_key():
    _, mat, emission = scene()
    tree = mat.node_tree
    inp = tree.nodes.new('NodeClosureInput')
    out = tree.nodes.new('NodeClosureOutput')
    inp.pair_with_output(out)
    out.input_items.new('VECTOR', 'Vector').structure_type = 'SINGLE'
    out.output_items.new('RGBA', 'Color').structure_type = 'SINGLE'
    out.output_items.new('FLOAT', 'Alpha').structure_type = 'SINGLE'
    tree.links.new(inp.outputs['Vector'], out.inputs['Color'])
    out.inputs['Alpha'].default_value = 1
    ev = coordinate_evaluator(tree, route(tree, out.outputs[0], 'mixed'))
    value = (0.2, 0.4, 0.6)
    ev.inputs['Vector'].default_value = value
    tree.links.new(ev.outputs['Color'], emission.inputs['Color'])
    check_pixel('custom_vector_key', value)
    identifier = ev.inputs['Vector'].identifier
    sync(tree, ev, input_keys=('Vector',))
    assert ev.inputs['Vector'].identifier == identifier
    near(tuple(ev.inputs['Vector'].default_value), value)
    # This source's key is Vector. UV must not become a global alias for it.
    ev.input_items[0].name = 'UV'
    check_pixel('custom_uv_not_alias', (0, 0, 0))
    # Synchronization restores the actual custom signature, not the image signature.
    sync(tree, ev, input_keys=('Vector',))
    ev.inputs['Vector'].default_value = value
    check_pixel('custom_vector_synced', value)

    # Join Bundle exposes multiple candidate signatures: retain their separate keys.
    packets = []
    for source in (new_image(tree).outputs[0], out.outputs[0]):
        packet = tree.nodes.new('NodeCombineBundle')
        packet.bundle_items.new('CLOSURE', 'Image')
        tree.links.new(source, packet.inputs['Image'])
        packets.append(packet)
    join = tree.nodes.new('NodeJoinBundle')
    tree.links.new(packets[0].outputs[0], join.inputs[0])
    custom_link = tree.links.new(packets[1].outputs[0], join.inputs[0])
    separate = tree.nodes.new('NodeSeparateBundle')
    separate.bundle_items.new('CLOSURE', 'Image')
    tree.links.new(join.outputs[0], separate.inputs[0])
    tree.links.new(separate.outputs['Image'], ev.inputs['Closure'])
    identifier = ev.inputs['Vector'].identifier
    sync(tree, ev, input_keys=('UV', 'Vector'))
    assert ev.inputs['Vector'].identifier == identifier
    assert ev.inputs['UV'].identifier != identifier
    near(tuple(ev.inputs['Vector'].default_value), value)
    ev.inputs['UV'].default_value = (0.75, 0.25, 0)
    tree.links.remove(custom_link)
    check_pixel('mixed_image_coordinate', COLORS[1][:3])
    tree.links.new(out.outputs[0], ev.inputs['Closure'])
    check_pixel('mixed_custom_coordinate', value)


def legacy_file():
    _, mat, emission = scene()
    tree = mat.node_tree
    image = new_image(tree)
    image.image.pack()
    ev = coordinate_evaluator(tree, route(tree, image.outputs[0], 'mixed'))
    ev.inputs['Vector'].default_value = (0.75, 0.75, 0)
    tree.links.new(ev.outputs['Color'], emission.inputs['Color'])
    assets = OUT / 'assets'
    assets.mkdir(exist_ok=True)
    filepath = assets / 'image_closure_legacy_vector.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(filepath))
    return filepath


def test_coordinate_reload():
    filepath = legacy_file()
    output = background_child(Path(__file__), ('--reload-vector', str(filepath)))
    (OUT / 'legacy_vector_reload.log').write_text(output, encoding='utf-8')
    assert 'IMAGE_CLOSURE_LEGACY_RELOAD_OK' in output, output


def invalid_coordinates(kind):
    _, mat, emission = scene()
    tree = mat.node_tree
    keys = () if kind == 'missing' else ('Coordinates',) if kind == 'unknown' else ('UV',)
    ev = coordinate_evaluator(tree, new_image(tree).outputs[0], keys,
                              'FLOAT' if kind == 'wrong_type' else 'VECTOR')
    if kind == 'wrong_type':
        # An invalid canonical UV must be diagnosed instead of silently choosing Vector.
        ev.input_items.new('VECTOR', 'Vector')
        ev.inputs['Vector'].default_value = (0.75, 0.75, 0)
    tree.links.new(ev.outputs['Color'], emission.inputs['Color'])
    check_pixel('invalid_coordinates_' + kind, (1, 0, 1))
    # Correcting the interface in the same material must recover from its prior error.
    ev.input_items.clear()
    ev.input_items.new('VECTOR', 'UV')
    ev.inputs['UV'].default_value = (0.75, 0.25, 0)
    check_pixel('invalid_coordinates_recovered_' + kind, COLORS[1][:3])
    print('IMAGE_CLOSURE_COORDINATE_ERROR_OK=' + kind, flush=True)


def test_coordinate_diagnostics():
    for kind in ('missing', 'unknown', 'wrong_type'):
        output = background_child(Path(__file__), ('--invalid-coordinates', kind))
        (OUT / ('invalid_coordinates_' + kind + '.log')).write_text(output, encoding='utf-8')
        assert 'Image closure coordinates' in output, output
        assert 'IMAGE_CLOSURE_COORDINATE_ERROR_OK=' + kind in output, output


def main():
    if '--write-legacy' in sys.argv:
        print('IMAGE_CLOSURE_LEGACY_FILE=' + str(legacy_file()), flush=True)
        return
    if '--reload-vector' in sys.argv:
        bpy.ops.wm.open_mainfile(filepath=sys.argv[sys.argv.index('--reload-vector') + 1])
        gpu.init()
        check_pixel('legacy_vector_cold_reload', COLORS[3][:3])
        print('IMAGE_CLOSURE_LEGACY_RELOAD_OK', flush=True)
        return
    if '--invalid-coordinates' in sys.argv:
        gpu.init()
        invalid_coordinates(sys.argv[sys.argv.index('--invalid-coordinates') + 1])
        return
    if '--reload' in sys.argv:
        bpy.ops.wm.open_mainfile(filepath=sys.argv[sys.argv.index('--reload')+1])
        gpu.init()
        check_pixel('cold_reload', (0.5,0.4,0.625))
        print('IMAGE_CLOSURE_RELOAD_OK', flush=True)
        return
    gpu.init()
    print('IMAGE_CLOSURE_BACKEND='+gpu.platform.backend_type_get(),flush=True)
    print('IMAGE_CLOSURE_BUILD='+bpy.app.build_hash.decode(),flush=True)
    tests = (test_editor_connection, test_routes, test_group_instances,
             test_source_group_and_signature_edit, test_native_texture_operations,
             test_bundle_group_input_and_addressing, test_three_dimensional_images,
             test_legacy_coordinates, test_coordinate_sync, test_coordinate_precedence,
             test_custom_coordinate_key, test_coordinate_reload, test_coordinate_diagnostics)
    selected = sys.argv[sys.argv.index('--test')+1].split(',') if '--test' in sys.argv else None
    if selected:
        assert set(selected) <= {test.__name__ for test in tests}, selected
    for test in tests:
        if selected is None or test.__name__ in selected:
            test()
    print('IMAGE_CLOSURE_EVALUATE_OK',flush=True)


if __name__ == '__main__':
    main()
