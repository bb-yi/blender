# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Render Texture passes: export-only demand, stable sockets, typed pixels and live compositing."""
import json
from pathlib import Path
import re
import sys
from types import SimpleNamespace

import bpy
import gpu
import numpy as np
import OpenImageIO as oiio

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import OUT, center, near, render, scene
from filter_graph_test_utils import attach_filter_material
import test_render_texture_updates as rt


def setup():
    s, _, emission = scene()
    s.eevee.use_shadows = s.eevee.use_raytracing = s.eevee.use_outline = False
    s.render.film_transparent = True
    emission.inputs['Color'].default_value = (.05, .1, .15, 1)
    camera = bpy.data.objects.new('Capture camera', s.camera.data.copy())
    camera.location = (10, 0, 3)
    s.collection.objects.link(camera)
    emissions, objects = [], []
    for index, x in enumerate((9.5, 10.5)):
        bpy.ops.mesh.primitive_plane_add(size=2, location=(x, 0, 0))
        obj = bpy.context.object
        obj.scale = (.5, 2, 1)
        material = bpy.data.materials.new('Capture half ' + str(index))
        material.use_nodes = True
        material.node_tree.nodes.clear()
        nodes, links = material.node_tree.nodes, material.node_tree.links
        emit = nodes.new('ShaderNodeEmission')
        emit.inputs['Color'].default_value = ((.2, 0, .1, 1) if index == 0 else (0, .4, .6, 1))
        diffuse = nodes.new('ShaderNodeBsdfDiffuse')
        diffuse.inputs['Color'].default_value = (0, 0, 0, 1)
        add = nodes.new('ShaderNodeAddShader')
        output = nodes.new('ShaderNodeOutputMaterial')
        links.new(emit.outputs[0], add.inputs[0])
        links.new(diffuse.outputs[0], add.inputs[1])
        links.new(add.outputs[0], output.inputs['Surface'])
        obj.data.materials.append(material)
        emissions.append(emit)
        objects.append(obj)
    return s, camera, emissions, objects


def capture(s, camera, name='Capture', source='COLOR', fmt='RGBA16F', mode='EVERY_FRAME'):
    item = rt.capture(s, source, camera=camera, fmt=fmt, mode=mode)
    item.name = name
    assert item.use_compositor_output is False, 'Old scenes must opt in to export'
    item.use_compositor_output = True
    return item


def socket(layers, item):
    return next(out for out in layers.outputs if out.identifier == 'RenderTexture_' + str(item.uid))


def compositor(s, items):
    s.use_nodes = True
    tree = bpy.data.node_groups.new('RT layer regression', 'CompositorNodeTree')
    s.compositing_node_group = tree
    layers = tree.nodes.new('CompositorNodeRLayers')
    layers.layer = bpy.context.view_layer.name
    file = tree.nodes.new('CompositorNodeOutputFile')
    file.directory = str(OUT)
    file.format.file_format = 'OPEN_EXR_MULTILAYER'
    file.file_output_items.clear()
    for item in items:
        out = socket(layers, item)
        assert out.name == 'RT: ' + item.name, (out.name, item.name)
        assert out.type == ('VALUE' if item.source == 'DEPTH' or item.format in {'R16F', 'R32F'} else 'RGBA')
        file.file_output_items.new('RGBA', item.name)
        tree.links.new(out, file.inputs[item.name])
    file.file_output_items.new('RGBA', 'Main')
    tree.links.new(layers.outputs['Image'], file.inputs['Main'])
    return tree, layers, file


def export(file, label):
    file.file_name = label
    bpy.context.view_layer.update()
    bpy.ops.render.render()
    paths = list(OUT.glob(label + '*.exr'))
    assert len(paths) == 1, paths
    reader = oiio.ImageInput.open(str(paths[0]))
    assert reader is not None
    passes = {}
    index = 0
    try:
        while reader.seek_subimage(index, 0):
            name = reader.spec().get_string_attribute('oiio:subimagename')
            passes[name] = reader.read_image(format='float')
            assert np.isfinite(passes[name]).all(), name
            index += 1
    finally:
        reader.close()
    return passes


def check_halves(pixels, left, right, extent=(64, 48)):
    assert pixels.shape[:2] == (extent[1], extent[0]), pixels.shape
    for y in (extent[1] // 8, extent[1] // 2, 7 * extent[1] // 8):
        near(pixels[y, extent[0] // 8, :len(left)], left)
        near(pixels[y, 7 * extent[0] // 8, :len(right)], right)


def offline():
    s, camera, emissions, objects = setup()
    items = [capture(s, camera, 'Color'), capture(s, camera, 'Depth', 'DEPTH', 'R32F'),
             capture(s, camera, 'Normal', 'NORMAL', 'RGBA32F'),
             capture(s, camera, 'Single', fmt='R16F')]
    tree, layers, file = compositor(s, items)
    conflicting_aov = bpy.context.view_layer.aovs.add()
    conflicting_aov.name = socket(layers, items[0]).identifier
    assert not conflicting_aov.is_valid, 'AOV must not shadow a Render Texture pass'
    bpy.context.view_layer.aovs.remove(conflicting_aov)
    pixels = export(file, 'offline_first')
    check_halves(pixels['Color'], [.2, 0, .1, 1], [0, .4, .6, 1])
    check_halves(pixels['Depth'], [3, 3, 3], [3, 3, 3])
    check_halves(pixels['Normal'], [.5, .5, 1, 1], [.5, .5, 1, 1])
    check_halves(pixels['Single'], [.2, .2, .2], [0, 0, 0])
    near(center(pixels['Main']), [.05, .1, .15])
    emissions[0].inputs['Color'].default_value = (.6, .3, .1, 1)
    s.frame_set(2)
    pixels = export(file, 'offline_second')
    check_halves(pixels['Color'], [.6, .3, .1, 1], [0, .4, .6, 1])
    items[0].format = 'R16F'
    pixels = export(file, 'single_channel_transition')
    check_halves(pixels['Color'], [.6, .6, .6, 1], [0, 0, 0, 1])
    items[0].format = 'RGBA16F'
    identifier = socket(layers, items[0]).identifier
    s.eevee.render_textures.move(0, 3)
    items[0].name = 'Renamed capture'
    assert socket(layers, items[0]).identifier == identifier
    assert socket(layers, items[0]).name == 'RT: Renamed capture'
    assert any(link.from_socket.identifier == identifier and link.to_socket.name == 'Color' for link in tree.links)
    items[0].use_compositor_output = False
    assert socket(layers, items[0]).is_unavailable
    items[0].use_compositor_output = True
    assert not socket(layers, items[0]).is_unavailable
    objects[0].hide_render = True
    pixels = export(file, 'offline_transparency')
    check_halves(pixels['Color'], [0, 0, 0, 0], [0, .4, .6, 1])
    # The native Render Result also retains typed passes without a compositor/File Output wrapper.
    s.render.image_settings.media_type = 'MULTI_LAYER_IMAGE'
    s.render.image_settings.file_format = 'OPEN_EXR_MULTILAYER'
    bpy.data.images['Render Result'].save_render(str(OUT / 'native_passes.exr'), scene=s)
    image = bpy.data.images.load(str(OUT / 'native_passes.exr'), check_existing=False)
    image_node = tree.nodes.new('CompositorNodeImage')
    image_node.image = image
    assert identifier in [out.identifier for out in image_node.outputs], [out.identifier for out in image_node.outputs]
    path = OUT / 'roundtrip.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(path))
    bpy.ops.wm.open_mainfile(filepath=str(path))
    s = bpy.context.scene
    restored = next(item for item in s.eevee.render_textures if item.uid == int(identifier.split('_')[-1]))
    assert restored.use_compositor_output and restored.name == 'Renamed capture'
    layers = next(n for n in s.compositing_node_group.nodes if n.bl_idname == 'CompositorNodeRLayers')
    assert socket(layers, restored).identifier == identifier
    file = next(n for n in s.compositing_node_group.nodes if n.bl_idname == 'CompositorNodeOutputFile')
    pixels = export(file, 'after_reopen')
    check_halves(pixels['Color'], [0, 0, 0, 0], [0, .4, .6, 1])
    return {'identifier': identifier, 'export_only': True, 'roundtrip': True}


def filter_material():
    s, camera, _, _ = setup()
    item = capture(s, camera)
    item.use_compositor_output = False
    material = bpy.data.materials.new('Filter RT entry')
    material.eevee_domain = 'FILTER'
    material.node_tree.nodes.clear()
    rt.filter_samples(material.node_tree, [(item, 1)], spatial=True)
    attach_filter_material(material)
    pixels = render('filter_reader')
    check_halves(pixels, [.2, 0, .1], [0, .4, .6])
    # Exercise the actual Python Add menu with a Filter editor context.
    from bl_ui import node_add_menu_shader
    calls = []
    menu = SimpleNamespace(layout=None, bl_label='Texture', draw_assets_for_catalog=lambda *args: None,
                           node_operator=lambda layout, kind, **kw: calls.append((kind, kw.get('poll', True))))
    context = SimpleNamespace(engine='BLENDER_EEVEE', space_data=SimpleNamespace(
        tree_type='ShaderNodeTree', shader_type='FILTER', id_from=None))
    node_add_menu_shader.NODE_MT_shader_node_texture_base.draw(menu, context)
    assert ('ShaderNodeRenderTexture', True) in calls, calls
    return {'filter_pixels': 'spatial UVs cover capture', 'menu_entry': True}


def viewport():
    s, camera, emissions, _ = setup()
    for emission in emissions:
        emission.inputs['Color'].default_value = (.2, .3, .4, 1)
    item = capture(s, camera, mode='MANUAL')
    s.use_nodes = True
    tree = bpy.data.node_groups.new('Viewport RT compositor', 'CompositorNodeTree')
    s.compositing_node_group = tree
    tree.interface.new_socket(name='Image', in_out='OUTPUT', socket_type='NodeSocketColor')
    output = tree.nodes.new('NodeGroupOutput')
    layers = tree.nodes.new('CompositorNodeRLayers')
    tree.links.new(socket(layers, item), output.inputs['Image'])
    viewport = rt.Viewport()
    viewport.space.shading.use_compositor = 'ALWAYS'
    records = []
    try:
        actual = viewport.draw()
        near(actual, [.2, .3, .4])
        records.append(actual.tolist())
        for emission in emissions:
            emission.inputs['Color'].default_value = (.6, .3, .1, 1)
        s.frame_set(2)
        near(viewport.draw(), [.2, .3, .4])
        s.eevee.render_textures.request_update(0)
        actual = viewport.draw()
        near(actual, [.6, .3, .1])
        records.append(actual.tolist())
        item.name = 'Renamed live'
        near(viewport.draw(), [.6, .3, .1])
    finally:
        viewport.free()
    return {'manual_viewport': records}


def border():
    s, camera, _, _ = setup()
    item = capture(s, camera)
    _, _, file = compositor(s, [item])
    s.render.resolution_percentage = 50
    pixels = export(file, 'half_resolution')
    check_halves(pixels[item.name], [.2, 0, .1, 1], [0, .4, .6, 1], (32, 24))
    s.render.resolution_percentage = 100
    s.render.use_border = s.render.use_crop_to_border = True
    s.render.border_min_x, s.render.border_max_x = .5, 1
    s.render.border_min_y, s.render.border_max_y = 0, 1
    pixels = export(file, 'right_border')
    check_halves(pixels[item.name], [0, .4, .6, 1], [0, .4, .6, 1], (32, 48))
    return {'half_resolution': [32, 24], 'cropped_border': [32, 48]}


def view_layers():
    s, camera, _, objects = setup()
    collection = bpy.data.collections.new('Right capture visibility')
    s.collection.children.link(collection)
    s.collection.objects.unlink(objects[1])
    collection.objects.link(objects[1])
    other = s.view_layers.new('Other capture visibility')
    other.layer_collection.children[collection.name].exclude = True
    item = capture(s, camera)
    tree, _, file = compositor(s, [item])
    layers = tree.nodes.new('CompositorNodeRLayers')
    layers.layer = other.name
    file.file_output_items.new('RGBA', 'Other')
    tree.links.new(socket(layers, item), file.inputs['Other'])
    pixels = export(file, 'view_layer_visibility')
    check_halves(pixels[item.name], [.2, 0, .1, 1], [0, .4, .6, 1])
    check_halves(pixels['Other'], [.2, 0, .1, 1], [0, 0, 0, 0])
    return {'view_layer_visibility': True}


def snapshot():
    s, _, item, _ = rt.feedback('EVERY_SAMPLE')
    s.eevee.taa_render_samples = 8
    item.use_compositor_output = True
    _, _, file = compositor(s, [item])
    pixels = export(file, 'sample_snapshot')
    near(center(pixels[item.name]), [.8, 0, 0])
    near(center(pixels['Main']), [.55, 0, 0], .02)
    item.update_mode = 'EVERY_FRAME'
    pixels = export(file, 'frame_snapshot')
    near(center(pixels[item.name]), [.1, 0, 0])
    return {'every_sample_snapshot': .8, 'every_frame_snapshot': .1}


def spatial():
    s, camera, _, objects = setup()
    for obj in objects:
        bpy.data.objects.remove(obj, do_unlink=True)
    bpy.ops.mesh.primitive_plane_add(size=4, location=(10, 0, 0))
    material = bpy.data.materials.new('Capture world-position gradient')
    material.use_nodes = True
    material.node_tree.nodes.clear()
    nodes, links = material.node_tree.nodes, material.node_tree.links
    geometry = nodes.new('ShaderNodeNewGeometry')
    add = nodes.new('ShaderNodeVectorMath')
    add.operation = 'ADD'
    add.inputs[1].default_value = (-9, 1, .5)
    scale = nodes.new('ShaderNodeVectorMath')
    scale.operation = 'SCALE'
    scale.inputs['Scale'].default_value = .5
    emission = nodes.new('ShaderNodeEmission')
    output = nodes.new('ShaderNodeOutputMaterial')
    links.new(geometry.outputs['Position'], add.inputs[0])
    links.new(add.outputs[0], scale.inputs[0])
    links.new(scale.outputs[0], emission.inputs['Color'])
    links.new(emission.outputs[0], output.inputs['Surface'])
    bpy.context.object.data.materials.append(material)
    item = capture(s, camera, fmt='RGBA32F')
    item.resolution_x, item.resolution_y = 91, 37
    _, _, file = compositor(s, [item])
    pixels = export(file, 'spatial_gradient')[item.name]
    for y in (6, 24, 41):
        for x in (8, 32, 55):
            u, v = (x + .5) / 64, 1 - (y + .5) / 48
            near(pixels[y, x, :4], [u, .5 + (v - .5) * 37 / 91, .25, 1])
    return {'capture_extent': [91, 37], 'output_extent': [64, 48], 'gradient_points': 9}


CASES = {'layers_offline': offline, 'layers_filter': filter_material,
         'layers_viewport': viewport, 'layers_border': border, 'layers_visibility': view_layers,
         'layers_snapshot': snapshot, 'layers_spatial': spatial}


def main():
    gpu.init()
    backend = gpu.platform.backend_type_get()
    if '--gpu-backend' in sys.argv:
        assert backend.lower() == sys.argv[sys.argv.index('--gpu-backend') + 1].lower(), backend
    if '--rt-case' in sys.argv:
        case = sys.argv[sys.argv.index('--rt-case') + 1]
        print('RT_CASE_RESULT', json.dumps(CASES[case]()), flush=True)
        return
    results = {}
    for case in CASES:
        log = rt.child_output(case, script=__file__)
        results[case] = json.loads(re.findall(r'RT_CASE_RESULT (\{[^\r\n]+\})', log)[-1])
        print('RT_LAYERS_CASE_PASS', case, flush=True)
    (OUT / 'render_texture_layers.json').write_text(json.dumps({
        'backend': backend, 'cases': results}, indent=2), encoding='utf-8')
    print('RENDER_TEXTURE_LAYERS_PASS', flush=True)


if __name__ == '__main__':
    main()
