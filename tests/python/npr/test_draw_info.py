# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Draw Info: actual material pixels, capture isolation, stages, and file round-trip."""

import argparse
import json
import os
from pathlib import Path
import sys
import time
import traceback

import bpy
import numpy as np
import OpenImageIO as oiio

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))

parser = argparse.ArgumentParser()
parser.add_argument('--output-dir', type=Path, default=os.environ.get('BLENDER_TEST_OUTPUT_DIR'))
parser.add_argument('--mode', choices=('offline', 'offscreen', 'window'),
                    default=os.environ.get('DRAW_INFO_TEST_MODE', 'offline'))
parser.add_argument('--case', action='append',
                    default=os.environ['DRAW_INFO_TEST_CASES'].split(',')
                    if os.environ.get('DRAW_INFO_TEST_CASES') else None)
args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])
if args.output_dir is None:
    parser.error('--output-dir or BLENDER_TEST_OUTPUT_DIR is required')
OUT = args.output_dir.resolve()
OUT.mkdir(parents=True, exist_ok=True)
temporary_directory = OUT / 'tmp'
temporary_directory.mkdir(exist_ok=True)
bpy.context.preferences.filepaths.temporary_directory = str(temporary_directory)
bpy.context.preferences.use_preferences_save = False
os.environ['BLENDER_TEST_OUTPUT_DIR'] = str(OUT)
from material_compat_test_utils import scene, render, center, near
from filter_graph_test_utils import attach_filter_material

QUERIES = (
    'OUTLINE_SHELL', 'SHADOW_PASS', 'RENDER_TEXTURE_CAPTURE', 'PROBE_CAPTURE',
    'IMAGE_RENDER', 'COLOR_BAKE', 'VIEWPORT', 'DEPTH_PREPASS', 'DEFERRED', 'FORWARD',
    'NPR_EVALUATION', 'FILTER_PASS', 'WORLD', 'VERTEX_EVALUATION', 'FRAGMENT_EVALUATION',
    'VOLUME_EVALUATION', 'SPHERE_PROBE_CAPTURE', 'PLANAR_PROBE_CAPTURE', 'IRRADIANCE_CAPTURE',
    'PRE_FX_RENDER', 'PLAYBACK', 'NAVIGATING',
)
OUTPUTS = {query: 'Is ' + query.replace('_', ' ').title() for query in QUERIES}
OUTPUTS.update(NPR_EVALUATION='Is NPR Evaluation', PRE_FX_RENDER='Is Pre-FX Render')

REPORT = {'version': bpy.app.version_string, 'build_hash': bpy.app.build_hash.decode(),
          'mode': args.mode, 'cases': {}, 'passed': False}
(OUT / ('summary_' + args.mode + '.json')).write_text(json.dumps(REPORT, indent=2), encoding='utf-8')


def probe(tree, query='OUTLINE_SHELL'):
    node = tree.nodes.new('ShaderNodeDrawInfo')
    select_query(node, query)
    return node


def query_output(node):
    return node.outputs[OUTPUTS[node.get('_test_query', 'OUTLINE_SHELL')]]


def select_query(node, query):
    targets = [link.to_socket for socket in node.outputs for link in socket.links]
    node['_test_query'] = query
    for target in targets:
        connect(node.id_data, query_output(node), target)


def connect(tree, output, target):
    for link in list(target.links):
        tree.links.remove(link)
    tree.links.new(output, target)


def gui_scene():
    """Keep the live test window intact; factory reload defers rebuilding its context."""
    s = bpy.context.scene
    for obj in list(s.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    s.render.engine = 'BLENDER_EEVEE'
    s.render.resolution_x, s.render.resolution_y = 64, 48
    s.render.resolution_percentage = 100
    s.eevee.taa_render_samples = 1
    s.view_settings.view_transform = 'Standard'
    camera = bpy.data.objects.new('DrawInfoCamera', bpy.data.cameras.new('DrawInfoCamera'))
    s.collection.objects.link(camera)
    camera.location = (0, 0, 3)
    camera.data.type = 'ORTHO'
    camera.data.ortho_scale = 2
    s.camera = camera
    mesh = bpy.data.meshes.new('DrawInfoPlane')
    mesh.from_pydata([(-2, -2, 0), (2, -2, 0), (2, 2, 0), (-2, 2, 0)], [], [(0, 1, 2, 3)])
    obj = bpy.data.objects.new('DrawInfoPlane', mesh)
    s.collection.objects.link(obj)
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    m = bpy.data.materials.new('DrawInfoGUI')
    m.use_nodes = True
    obj.data.materials.append(m)
    m.node_tree.nodes.clear()
    emit = m.node_tree.nodes.new('ShaderNodeEmission')
    output = m.node_tree.nodes.new('ShaderNodeOutputMaterial')
    connect(m.node_tree, emit.outputs[0], output.inputs['Surface'])
    bpy.context.view_layer.update()
    return s, m, emit


def check_center(label, expected):
    pixels = render(label)
    near(center(pixels), (expected,) * 3, tolerance=.015)
    return pixels


def compositor(scene, sockets):
    scene.use_nodes = True
    tree = bpy.data.node_groups.new('DrawInfoPasses', 'CompositorNodeTree')
    scene.compositing_node_group = tree
    layers = tree.nodes.new('CompositorNodeRLayers')
    output = tree.nodes.new('CompositorNodeOutputFile')
    output.directory = str(OUT)
    output.format.file_format = 'OPEN_EXR_MULTILAYER'
    output.file_output_items.clear()
    for name, kind, socket in sockets:
        output.file_output_items.new(kind, name)
        tree.links.new(layers.outputs[socket], output.inputs[name])
    return tree, layers, output


def export(output, label):
    output.file_name = label
    bpy.context.view_layer.update()
    bpy.ops.render.render()
    reader = oiio.ImageInput.open(str(OUT / (label + '.exr')))
    assert reader is not None, label
    images = {}
    try:
        index = 0
        while reader.seek_subimage(index, 0):
            spec = reader.spec()
            name = spec.channelnames[0].split('.')[0]
            images[name] = reader.read_image(format='float').copy()
            assert np.isfinite(images[name]).all(), (label, name)
            index += 1
    finally:
        reader.close()
    return images


def test_legacy():
    path = Path(__file__).parent / 'assets' / 'draw_info_legacy_queries.blend'

    def check():
        tree = bpy.data.materials['LegacyDrawInfo'].node_tree
        nodes = [node for node in tree.nodes if node.bl_idname == 'ShaderNodeDrawInfo']
        assert {node.name for node in nodes} == set(QUERIES)
        for node in nodes:
            assert len(node.outputs) == len(QUERIES)
            links = [link for socket in node.outputs for link in socket.links]
            assert len(links) == 1 and links[0].from_socket.name == OUTPUTS[node.name]
            assert links[0].to_node.name == node.name + '_sink'
        group = bpy.data.node_groups['LegacyDrawInfoGroup']
        link = next(link for link in group.links if link.from_node.bl_idname == 'ShaderNodeDrawInfo')
        assert link.from_socket.name == 'Is Image Render'

    bpy.ops.wm.open_mainfile(filepath=str(path))
    check()
    assets = OUT / 'assets'
    assets.mkdir(exist_ok=True)
    converted = assets / 'draw_info_legacy_migrated.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(converted), check_existing=False)
    bpy.ops.wm.open_mainfile(filepath=str(converted))
    check()
    return {'queries': len(QUERIES), 'group': True, 'roundtrip': True}


def test_registration():
    s, m, emit = scene()
    node = probe(m.node_tree)
    assert 'query' not in node.bl_rna.properties
    assert len(node.inputs) == 0 and len(node.outputs) == len(QUERIES)
    assert tuple(socket.name for socket in node.outputs) == tuple(OUTPUTS.values())
    assert all(socket.type == 'VALUE' for socket in node.outputs)
    query_count = len(node.outputs)
    ri = m.node_tree.nodes.new('ShaderNodeRenderInfo')
    assert [socket.name for socket in ri.outputs] == [
        'Frag Coord', 'Width', 'Height', 'Resolution', 'Current Sample', 'Total Samples']
    combine = m.node_tree.nodes.new('ShaderNodeCombineXYZ')
    for query, target in [('IMAGE_RENDER', 'X'), ('DEFERRED', 'Y'), ('OUTLINE_SHELL', 'Z')]:
        connect(m.node_tree, node.outputs[OUTPUTS[query]], combine.inputs[target])
    connect(m.node_tree, combine.outputs[0], emit.inputs['Color'])
    near(center(render('registration_simultaneous_outputs')), (1, 1, 0), tolerance=.015)
    m.node_tree.nodes.remove(combine)
    select_query(node, 'IMAGE_RENDER')
    connect(m.node_tree, query_output(node), emit.inputs['Color'])
    check_center('registration_image_render', 1)
    path = OUT / 'assets' / 'draw_info_roundtrip.blend'
    path.parent.mkdir(exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=str(path), check_existing=False)
    bpy.ops.wm.open_mainfile(filepath=str(path))
    m = bpy.data.materials['Probe']
    node = next(node for node in m.node_tree.nodes if node.bl_idname == 'ShaderNodeDrawInfo')
    assert node.outputs['Is Image Render'].is_linked
    check_center('roundtrip_image_render', 1)
    return {'queries': query_count, 'roundtrip': True}


def test_surface():
    results = {}
    for method in ('DITHERED', 'BLENDED'):
        s, m, emit = scene()
        m.surface_render_method = method
        node = probe(m.node_tree)
        connect(m.node_tree, query_output(node), emit.inputs['Color'])
        positive = {'IMAGE_RENDER', 'FRAGMENT_EVALUATION',
                    'DEFERRED' if method == 'DITHERED' else 'FORWARD'}
        for query in QUERIES:
            select_query(node, query)
            check_center(f'surface_{method}_{query}', int(query in positive))
        results[method] = sorted(positive)
    return results


def test_shell():
    results = {}
    for route in ('DEFERRED', 'FORWARD'):
        s, m, emit = scene()
        s.render.resolution_x = s.render.resolution_y = 96
        s.camera.data.ortho_scale = 3
        obj = bpy.context.object
        bpy.data.objects.remove(obj, do_unlink=True)
        bpy.ops.mesh.primitive_uv_sphere_add(segments=64, ring_count=32)
        bpy.context.object.data.materials.append(m)
        for polygon in bpy.context.object.data.polygons:
            polygon.use_smooth = True
        emit.inputs['Color'].default_value = (.2, .2, .2, 1)
        node = probe(m.node_tree)
        shell = m.node_tree.nodes.new('ShaderNodeOutputOutlineShell')
        shell.inputs['Strength'].default_value = .2
        m.outline_shell_render_method = route
        connect(m.node_tree, query_output(node), shell.inputs['Color'])
        positive = {'OUTLINE_SHELL', 'IMAGE_RENDER', 'FRAGMENT_EVALUATION', route}
        for query in QUERIES:
            select_query(node, query)
            pixels = render(f'shell_{route}_{query}')
            near(center(pixels), (.2,) * 3, tolerance=.015)
            near(pixels[48, int((1.1 / 3 + .5) * 96), :3],
                 (int(query in positive),) * 3, tolerance=.02)
        results[route] = sorted(positive)
    return results


def test_world():
    s, m, emit = scene()
    bpy.data.objects.remove(bpy.context.object, do_unlink=True)
    world = bpy.data.worlds.new('DrawInfoWorld')
    world.use_nodes = True
    s.world = world
    node = probe(world.node_tree)
    connect(world.node_tree, query_output(node), world.node_tree.nodes['Background'].inputs['Color'])
    positive = {'WORLD', 'IMAGE_RENDER', 'FRAGMENT_EVALUATION'}
    for query in QUERIES:
        select_query(node, query)
        check_center('world_' + query, int(query in positive))
    return sorted(positive)


def test_filter():
    s, m, emit = scene()
    fm = bpy.data.materials.new('DrawInfoFilter')
    fm.eevee_domain = 'FILTER'
    fm.node_tree.nodes.clear()
    node = probe(fm.node_tree)
    output = fm.node_tree.nodes.new('ShaderNodeOutputFilter')
    output.inputs['Alpha'].default_value = 1
    connect(fm.node_tree, query_output(node), output.inputs['Color'])
    attach_filter_material(fm)
    positive = {'FILTER_PASS', 'IMAGE_RENDER', 'FRAGMENT_EVALUATION'}
    for query in QUERIES:
        select_query(node, query)
        check_center('filter_' + query, int(query in positive))
    return sorted(positive)


def test_npr():
    s, m, emit = scene()
    tree = bpy.data.node_groups.new('DrawInfoNPR', 'ShaderNodeTree')
    output = tree.nodes.new('ShaderNodeNPR_Output')
    node = probe(tree)
    connect(tree, query_output(node), output.inputs['Color'])
    m.node_tree.nodes.get('Material Output').nprtree = tree
    positive = {'NPR_EVALUATION', 'IMAGE_RENDER', 'FRAGMENT_EVALUATION', 'DEFERRED'}
    for query in QUERIES:
        select_query(node, query)
        check_center('npr_' + query, int(query in positive))
    return sorted(positive)


def test_bake():
    s, m, emit = scene()
    image = bpy.data.images.new('DrawInfoBake', width=16, height=16, alpha=True, float_buffer=True)
    image_node = m.node_tree.nodes.new('ShaderNodeTexImage')
    image_node.image = image
    m.node_tree.nodes.active = image_node
    node = probe(m.node_tree, 'COLOR_BAKE')
    connect(m.node_tree, query_output(node), emit.inputs['Color'])
    s.render.bake.target = 'IMAGE_TEXTURES'
    results = {}
    for query, expected in [('COLOR_BAKE', 1), ('FRAGMENT_EVALUATION', 1),
                            ('VERTEX_EVALUATION', 0), ('OUTLINE_SHELL', 0)]:
        select_query(node, query)
        assert bpy.ops.object.bake(type='EMIT') == {'FINISHED'}
        pixels = np.array(image.pixels[:]).reshape(16, 16, 4)
        near(pixels[8, 8, :3], (expected,) * 3, tolerance=.015)
        results[query] = pixels[8, 8, :3].tolist()
    return results


def test_render_texture():
    s, m, emit = scene()
    node = probe(m.node_tree, 'RENDER_TEXTURE_CAPTURE')
    connect(m.node_tree, query_output(node), emit.inputs['Color'])
    capture = s.eevee.render_textures.add()
    capture.name = 'DrawInfoCapture'
    capture.enabled = True
    capture.camera = s.camera
    capture.resolution_x, capture.resolution_y = 64, 48
    capture.source = 'COLOR'
    capture.use_compositor_output = True
    capture.update_mode = 'EVERY_FRAME'
    s.use_nodes = True
    tree = bpy.data.node_groups.new('DrawInfoCapturePasses', 'CompositorNodeTree')
    s.compositing_node_group = tree
    layers = tree.nodes.new('CompositorNodeRLayers')
    output = tree.nodes.new('CompositorNodeOutputFile')
    output.directory = str(OUT)
    output.format.file_format = 'OPEN_EXR_MULTILAYER'
    output.file_output_items.clear()
    socket = next(socket for socket in layers.outputs
                  if socket.identifier == 'RenderTexture_' + str(capture.uid))
    for name, source in [('Capture', socket), ('Main', layers.outputs['Image'])]:
        output.file_output_items.new('RGBA', name)
        tree.links.new(source, output.inputs[name])
    for iteration in range(2):
        images = export(output, f'render_texture_{iteration}')
        near(center(images['Capture']), (1,) * 3, tolerance=.015)
        near(center(images['Main']), (0,) * 3, tolerance=.015)
    return {'capture': 1, 'main_after_capture': 0, 'frames': 2}


def test_vertex_depth():
    s, m, emit = scene()
    s.view_layers[0].use_pass_z = True
    m.displacement_method = 'DISPLACEMENT'
    node = probe(m.node_tree)
    combine = m.node_tree.nodes.new('ShaderNodeCombineXYZ')
    scale = m.node_tree.nodes.new('ShaderNodeVectorMath')
    scale.operation = 'SCALE'
    scale.inputs['Scale'].default_value = .2
    connect(m.node_tree, query_output(node), combine.inputs['Z'])
    connect(m.node_tree, combine.outputs[0], scale.inputs[0])
    connect(m.node_tree, scale.outputs[0], m.node_tree.nodes['Material Output'].inputs['Displacement'])
    _, _, output = compositor(s, [('Depth', 'FLOAT', 'Depth')])
    results = {}
    for query, expected in [('VERTEX_EVALUATION', 2.8), ('FRAGMENT_EVALUATION', 3),
                            ('DEPTH_PREPASS', 2.8), ('SHADOW_PASS', 3)]:
        select_query(node, query)
        pixels = export(output, 'depth_' + query)['Depth']
        actual = float(pixels[24, 32, 0])
        assert abs(actual - expected) < .015, (query, actual, expected)
        results[query] = actual
    return results


def test_aov():
    s, m, emit = scene()
    emit.inputs['Color'].default_value = (.2, .2, .2, 1)
    item = s.view_layers[0].aovs.add()
    item.name, item.type = 'DrawInfoValue', 'VALUE'
    node = probe(m.node_tree, 'IMAGE_RENDER')
    aov = m.node_tree.nodes.new('ShaderNodeOutputAOV')
    aov.aov_name = item.name
    connect(m.node_tree, query_output(node), aov.inputs['Value'])
    _, _, output = compositor(s, [('Value', 'FLOAT', item.name), ('Main', 'RGBA', 'Image')])
    for query, expected in [('IMAGE_RENDER', 1), ('OUTLINE_SHELL', 0)]:
        select_query(node, query)
        images = export(output, 'aov_' + query)
        assert abs(float(images['Value'][24, 32, 0]) - expected) < .015
        near(center(images['Main']), (.2,) * 3, tolerance=.015)
    return {'surface_aov': True, 'beauty_unchanged': True}


def test_shell_aov():
    results = {}
    for body_method in ('DITHERED', 'BLENDED'):
        for route, transparency in [('DEFERRED', 'DITHERED'), ('DEFERRED', 'CLIP'),
                                    ('FORWARD', 'DITHERED'), ('FORWARD', 'CLIP'),
                                    ('FORWARD', 'BLENDED')]:
            s, m, emit = scene()
            s.render.resolution_x = s.render.resolution_y = 96
            s.camera.data.ortho_scale = 3
            bpy.data.objects.remove(bpy.context.object, do_unlink=True)
            bpy.ops.mesh.primitive_uv_sphere_add(segments=64, ring_count=32)
            sphere = bpy.context.object
            for polygon in sphere.data.polygons:
                polygon.use_smooth = True
            sphere.data.materials.append(m)
            m.surface_render_method = body_method
            m.outline_shell_render_method = route
            m.outline_shell_transparency_method = transparency
            emit.inputs['Color'].default_value = (.2, .2, .2, 1)
            tree = m.node_tree
            shell = tree.nodes.new('ShaderNodeOutputOutlineShell')
            shell.inputs['Strength'].default_value = .2
            shell.inputs['Color'].default_value = (.05, .05, .05, 1)
            node = tree.nodes.new('ShaderNodeDrawInfo')
            flag = node.outputs['Is Outline Shell']
            color = tree.nodes.new('ShaderNodeMixRGB')
            color.inputs[1].default_value = (.4, .4, .4, 1)
            color.inputs[2].default_value = (1, 0, 0, 1)
            tree.links.new(flag, color.inputs[0])
            value = tree.nodes.new('ShaderNodeMapRange')
            value.inputs['To Min'].default_value = .25
            value.inputs['To Max'].default_value = .75
            tree.links.new(flag, value.inputs['Value'])
            for name, kind, socket, source in [
                    ('ShellColor', 'COLOR', 'Color', color.outputs[0]),
                    ('ShellValue', 'VALUE', 'Value', value.outputs[0])]:
                item = s.view_layers[0].aovs.add()
                item.name, item.type = name, kind
                aov = tree.nodes.new('ShaderNodeOutputAOV')
                aov.aov_name = name
                tree.links.new(source, aov.inputs[socket])
            _, _, output = compositor(s, [('Color', 'RGBA', 'ShellColor'),
                                         ('Value', 'FLOAT', 'ShellValue'),
                                         ('Main', 'RGBA', 'Image')])
            label = f'aov_{body_method}_{route}_{transparency}'
            if body_method == 'DITHERED' and route == 'DEFERRED' and transparency == 'DITHERED':
                node.location, color.location, value.location = (-640, 400), (-320, 400), (-320, 100)
                emit.location = (-320, 650)
                tree.nodes.get('Material Output').location = (0, 650)
                shell.location = (0, -180)
                for aov in (item for item in tree.nodes if item.bl_idname == 'ShaderNodeOutputAOV'):
                    aov.location = (0, 400 if aov.aov_name == 'ShellColor' else 100)
                assets = OUT / 'assets'
                assets.mkdir(exist_ok=True)
                bpy.ops.wm.save_as_mainfile(filepath=str(assets / 'draw_info_shell_aov.blend'),
                                           check_existing=False)
            images = export(output, label)
            body, rim = (48, 48), (48, 83)
            near(images['Color'][body][:3], (.4,) * 3, tolerance=.015)
            near(images['Color'][rim][:3], (1, 0, 0), tolerance=.015)
            assert abs(float(images['Value'][body][0]) - .25) < .015
            assert abs(float(images['Value'][rim][0]) - .75) < .015
            near(images['Main'][body][:3], (.2,) * 3, tolerance=.015)
            near(images['Main'][rim][:3], (.05,) * 3, tolerance=.015)
            # Alter only the shell AOV; body and beauty must remain unchanged.
            color.inputs[2].default_value = (0, 0, 1, 1)
            changed = export(output, label + '_blue')
            near(changed['Color'][rim][:3], (0, 0, 1), tolerance=.015)
            near(changed['Color'][body][:3], (.4,) * 3, tolerance=.015)
            assert np.max(np.abs(changed['Main'] - images['Main'])) < .015
            # Shader to RGB switches deferred materials to the Hybrid entry point.
            aov_emit = tree.nodes.new('ShaderNodeEmission')
            convert = tree.nodes.new('ShaderNodeShaderToRGB')
            tree.links.new(color.outputs[0], aov_emit.inputs['Color'])
            tree.links.new(aov_emit.outputs[0], convert.inputs[0])
            color_aov = next(item for item in tree.nodes
                             if item.bl_idname == 'ShaderNodeOutputAOV' and item.aov_name == 'ShellColor')
            connect(tree, convert.outputs['Color'], color_aov.inputs['Color'])
            converted = export(output, label + '_shader_to_rgb')
            near(converted['Color'][rim][:3], (0, 0, 1), tolerance=.015)
            near(converted['Color'][body][:3], (.4,) * 3, tolerance=.015)
            assert np.max(np.abs(converted['Main'] - images['Main'])) < .015
            sphere.is_holdout = True
            holdout = export(output, label + '_holdout')
            for pixel in (body, rim):
                near(holdout['Color'][pixel][:3], (0, 0, 0), tolerance=.015)
                assert abs(float(holdout['Value'][pixel][0])) < .015
                near(holdout['Main'][pixel], (0, 0, 0, 0), tolerance=.015)
            sphere.is_holdout = False
            shell.inputs['Alpha'].default_value = 0
            hidden = export(output, label + '_alpha_zero')
            near(hidden['Color'][rim][:3], (0, 0, 0), tolerance=.015)
            assert abs(float(hidden['Value'][rim][0])) < .015
            near(hidden['Color'][body][:3], (.4,) * 3, tolerance=.015)
            shell.inputs['Alpha'].default_value = 1
            # A nearer surface owns both passes; the shell must not leak through it.
            bpy.ops.mesh.primitive_plane_add(size=.5, location=(1.1, 0, 1.5))
            foreground = bpy.data.materials.new('ForegroundAOV')
            foreground.use_nodes = True
            bpy.context.object.data.materials.append(foreground)
            for name, kind, socket, source in [('ShellColor', 'COLOR', 'Color', (0, 1, 0, 1)),
                                               ('ShellValue', 'VALUE', 'Value', .6)]:
                aov = foreground.node_tree.nodes.new('ShaderNodeOutputAOV')
                aov.aov_name = name
                aov.inputs[socket].default_value = source
            occluded = export(output, label + '_occluded')
            near(occluded['Color'][rim][:3], (0, 1, 0), tolerance=.015)
            assert abs(float(occluded['Value'][rim][0]) - .6) < .015
            results[label] = {'body_gray': True, 'shell_red': True, 'value': True,
                              'independent_control': True, 'zero_alpha': True, 'occlusion': True,
                              'shader_to_rgb': True, 'holdout': True}
    return results


def test_shell_aov_hybrid_self_occlusion():
    import test_outline_shell_self_occlusion as helper

    results = {}
    for route in ('DEFERRED', 'FORWARD'):
        s = helper.setup()
        s.eevee.taa_render_samples = 16
        mat = helper.body_material(alpha=.5)
        helper.sphere(mat)
        mat.outline_shell_render_method = route
        tree = mat.node_tree
        shell = next(item for item in tree.nodes if item.bl_idname == 'ShaderNodeOutputOutlineShell')
        item = s.view_layers[0].aovs.add()
        item.name, item.type = 'HybridSelfOcclusion', 'COLOR'
        flag = tree.nodes.new('ShaderNodeDrawInfo')
        emission = tree.nodes.new('ShaderNodeEmission')
        convert = tree.nodes.new('ShaderNodeShaderToRGB')
        aov = tree.nodes.new('ShaderNodeOutputAOV')
        aov.aov_name = item.name
        tree.links.new(flag.outputs['Is Outline Shell'], emission.inputs['Color'])
        tree.links.new(emission.outputs[0], convert.inputs[0])
        tree.links.new(convert.outputs['Color'], aov.inputs['Color'])
        shell.inputs['Alpha'].default_value = 0
        reference = helper.render('aov_hybrid_self_' + route + '_reference')
        shell.inputs['Alpha'].default_value = 1
        original = helper.render('aov_hybrid_self_' + route + '_disabled')
        mat.use_outline_shell_self_occlusion = True
        fixed = helper.render('aov_hybrid_self_' + route + '_enabled')
        near(helper.center(fixed), helper.center(reference), tolerance=.05)
        assert float((helper.center(fixed) - helper.center(original)).mean()) > .06
        assert float(helper.at(fixed, .86).max()) < .07
        results[route] = {'self_occlusion': True, 'visible_rim': True}
    return results


def test_shared_group():
    s, m, emit = scene()
    bpy.context.object.scale = (.2, .2, 1)
    group = bpy.data.node_groups.new('SharedDrawInfo', 'ShaderNodeTree')
    group.interface.new_socket(name='Value', in_out='OUTPUT', socket_type='NodeSocketFloat')
    node = probe(group, 'WORLD')
    connect(group, query_output(node), group.nodes.new('NodeGroupOutput').inputs['Value'])
    body = m.node_tree.nodes.new('ShaderNodeGroup')
    body.node_tree = group
    connect(m.node_tree, body.outputs['Value'], emit.inputs['Color'])
    world = bpy.data.worlds.new('SharedDrawInfoWorld')
    world.use_nodes = True
    s.world = world
    background = world.node_tree.nodes.new('ShaderNodeGroup')
    background.node_tree = group
    connect(world.node_tree, background.outputs['Value'],
            world.node_tree.nodes['Background'].inputs['Color'])
    pixels = render('shared_world_and_surface')
    near(center(pixels), (0,) * 3, tolerance=.015)
    near(pixels[24, 2, :3], (1,) * 3, tolerance=.015)
    return {'shared_group_body': 0, 'shared_group_world': 1}


def test_shadow():
    s, m, emit = scene()
    s.eevee.taa_render_samples = 16
    receiver = m.node_tree.nodes.new('ShaderNodeBsdfDiffuse')
    receiver.inputs['Color'].default_value = (1, 1, 1, 1)
    connect(m.node_tree, receiver.outputs[0], m.node_tree.nodes['Material Output'].inputs['Surface'])
    world = bpy.data.worlds.new('ShadowBlackWorld')
    world.use_nodes = True
    world.node_tree.nodes['Background'].inputs['Strength'].default_value = 0
    s.world = world
    light = bpy.data.lights.new('DrawInfoSun', 'SUN')
    light.energy = 3
    light.angle = .01
    sun = bpy.data.objects.new('DrawInfoSun', light)
    s.collection.objects.link(sun)
    sun.rotation_euler = (.6, 0, .5)
    bpy.ops.mesh.primitive_uv_sphere_add(radius=.25, location=(-.2, 0, .8))
    caster = bpy.context.object
    mat = bpy.data.materials.new('DrawInfoShadowCaster')
    mat.use_nodes = True
    mat.use_transparent_shadow = True
    caster.data.materials.append(mat)
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    nodes.clear()
    output = nodes.new('ShaderNodeOutputMaterial')
    red = nodes.new('ShaderNodeEmission')
    red.inputs['Color'].default_value = (1, 0, 0, 1)
    transparent = nodes.new('ShaderNodeBsdfTransparent')
    mix = nodes.new('ShaderNodeMixShader')
    mix.inputs[0].default_value = 0
    links.new(red.outputs[0], mix.inputs[1])
    links.new(transparent.outputs[0], mix.inputs[2])
    links.new(mix.outputs[0], output.inputs['Surface'])
    reference = render('shadow_constant_opaque')
    node = probe(mat.node_tree, 'SHADOW_PASS')
    connect(mat.node_tree, query_output(node), mix.inputs[0])
    actual = render('shadow_context_transparent')
    receiver_pixels = np.max(reference[:, :, :3], axis=2) - np.min(reference[:, :, :3], axis=2) < .01
    gain = (actual[:, :, 0] - reference[:, :, 0])[receiver_pixels]
    assert float(gain.max()) > .1 and int(np.count_nonzero(gain > .05)) > 5
    caster_pixels = (reference[:, :, 0] > .8) & (reference[:, :, 1] < .02)
    assert caster_pixels.any()
    near(actual[caster_pixels, :3], reference[caster_pixels, :3], tolerance=.02)
    return {'shadow_removed_pixels': int(np.count_nonzero(gain > .05))}


def test_pre_fx():
    s, m, emit = scene()
    s.eevee.taa_render_samples = 4
    s.camera.data.type = 'PERSP'
    s.camera.data.dof.use_dof = True
    s.camera.data.dof.focus_distance = 2
    s.camera.data.dof.aperture_fstop = 1
    s.view_layers[0].use_pass_pre_fx = True
    node = probe(m.node_tree, 'PRE_FX_RENDER')
    connect(m.node_tree, query_output(node), emit.inputs['Color'])
    _, _, output = compositor(s, [('Main', 'RGBA', 'Image'), ('PreFX', 'RGBA', 'Pre-FX')])
    images = export(output, 'pre_fx_context')
    near(center(images['Main']), (0,) * 3, tolerance=.015)
    near(center(images['PreFX']), (0,) * 3, tolerance=.015)
    return {'main': 0, 'pre_fx': 0, 'shared_scene_draw': True}


def test_volume():
    s, m, emit = scene()
    s.render.film_transparent = True
    bpy.data.objects.remove(bpy.context.object, do_unlink=True)
    bpy.ops.mesh.primitive_cube_add(size=1)
    bpy.context.object.data.materials.append(m)
    tree = m.node_tree
    tree.nodes.remove(emit)
    volume = tree.nodes.new('ShaderNodeVolumePrincipled')
    volume.inputs['Density'].default_value = 0
    volume.inputs['Emission Color'].default_value = (1, 1, 1, 1)
    volume.inputs['Emission Strength'].default_value = 1
    connect(tree, volume.outputs[0], tree.nodes['Material Output'].inputs['Volume'])
    reference = center(render('volume_constant_emission')).copy()
    assert float(reference.max()) > .05
    node = probe(tree)
    connect(tree, query_output(node), volume.inputs['Emission Strength'])
    for query, expected in [('VOLUME_EVALUATION', reference), ('FRAGMENT_EVALUATION', reference),
                            ('WORLD', np.zeros(3)), ('VERTEX_EVALUATION', np.zeros(3))]:
        select_query(node, query)
        near(center(render('volume_' + query)), expected, tolerance=.015)
    return {'volume_reference': reference.tolist()}


def test_probe_capture():
    results = {}
    for kind, query in [('SPHERE', 'SPHERE_PROBE_CAPTURE'), ('SPHERE', 'PROBE_CAPTURE')]:
        s, m, emit = scene()
        s.eevee.taa_render_samples = 8
        s.eevee.use_raytracing = False
        obj = bpy.context.object
        bpy.data.objects.remove(obj, do_unlink=True)
        bpy.ops.mesh.primitive_uv_sphere_add(radius=.5, segments=48, ring_count=24)
        obj = bpy.context.object
        obj.data.materials.append(m)
        for polygon in obj.data.polygons:
            polygon.use_smooth = True
        glossy = m.node_tree.nodes.new('ShaderNodeBsdfAnisotropic')
        glossy.inputs['Color'].default_value = (1, 1, 1, 1)
        glossy.inputs['Roughness'].default_value = 0
        connect(m.node_tree, glossy.outputs[0], m.node_tree.nodes['Material Output'].inputs['Surface'])
        world = bpy.data.worlds.new('DrawInfoProbeWorld')
        world.use_nodes = True
        s.world = world
        target_tree = world.node_tree
        target_socket = target_tree.nodes['Background'].inputs['Color']
        target_socket.default_value = (1, 1, 1, 1)
        reference = render('probe_reference_' + kind + '_' + query)
        assert float(center(reference).min()) > .3, ('probe reference', kind, center(reference))
        node = probe(target_tree, query)
        connect(target_tree, query_output(node), target_socket)
        pixels = render('probe_' + kind + '_' + query)
        reflected = center(pixels)
        assert float(reflected.min()) > .3, (kind, query, reflected)
        near(pixels[24, 2, :3], (0,) * 3, tolerance=.02)
        results[kind + '_' + query] = reflected.tolist()
    return results


def test_irradiance_capture():
    s, receiver, emit = scene()
    s.eevee.taa_render_samples = 8
    world = bpy.data.worlds.new('DrawInfoIrradianceWorld')
    world.use_nodes = True
    world.node_tree.nodes['Background'].inputs['Strength'].default_value = 0
    s.world = world
    diffuse = receiver.node_tree.nodes.new('ShaderNodeBsdfDiffuse')
    diffuse.inputs['Color'].default_value = (1, 1, 1, 1)
    connect(receiver.node_tree, diffuse.outputs[0], receiver.node_tree.nodes['Material Output'].inputs['Surface'])
    bpy.ops.mesh.primitive_uv_sphere_add(radius=.25, location=(.65, .25, .8))
    emitter = bpy.data.materials.new('IrradianceOnlyEmitter')
    emitter.use_nodes = True
    bpy.context.object.data.materials.append(emitter)
    emitter.node_tree.nodes.clear()
    output = emitter.node_tree.nodes.new('ShaderNodeOutputMaterial')
    emission = emitter.node_tree.nodes.new('ShaderNodeEmission')
    emission.inputs['Color'].default_value = (0, 0, 0, 1)
    emission.inputs['Strength'].default_value = 10
    connect(emitter.node_tree, emission.outputs[0], output.inputs['Surface'])
    data = bpy.data.lightprobes.new('DrawInfoIrradiance', 'VOLUME')
    data.resolution_x = data.resolution_y = data.resolution_z = 2
    data.bake_samples = 32
    capture = bpy.data.objects.new('DrawInfoIrradiance', data)
    capture.location.z = .9
    capture.scale = (1.5, 1.5, 1.5)
    s.collection.objects.link(capture)
    bpy.context.view_layer.update()
    assert bpy.ops.object.lightprobe_cache_bake(subset='ALL') == {'FINISHED'}
    reference = render('irradiance_without_capture_emission')
    node = probe(emitter.node_tree, 'IRRADIANCE_CAPTURE')
    connect(emitter.node_tree, query_output(node), emission.inputs['Color'])
    bpy.context.view_layer.update()
    assert bpy.ops.object.lightprobe_cache_bake(subset='ALL') == {'FINISHED'}
    actual = render('irradiance_with_capture_emission')
    gain = actual[:, :, :3] - reference[:, :, :3]
    assert float(gain.max()) > .005, float(gain.max())
    return {'irradiance_gain': float(gain.max())}


def test_offscreen():
    import gpu
    assert not bpy.app.background, 'Offscreen viewport checks need an isolated foreground instance'
    s, m, emit = gui_scene()
    s.eevee.taa_samples = 1
    s.eevee.use_taa_reprojection = False
    node = probe(m.node_tree)
    connect(m.node_tree, query_output(node), emit.inputs['Color'])
    area = next(area for area in bpy.context.screen.areas if area.type == 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    space = area.spaces.active
    space.shading.type = 'RENDERED'
    space.overlay.show_overlays = False
    space.shading.use_scene_world_render = space.shading.use_scene_lights_render = True
    gpu.init()
    buffer = gpu.types.GPUOffScreen(64, 48, format='RGBA32F')
    positive = {'VIEWPORT', 'IMAGE_RENDER', 'DEFERRED', 'FRAGMENT_EVALUATION'}
    results = {}

    def draw_current():
        for _ in range(3):
            bpy.context.view_layer.update()
            deps = bpy.context.evaluated_depsgraph_get()
            projection = s.camera.calc_matrix_camera(deps, x=64, y=48)
            buffer.draw_view3d(s, bpy.context.view_layer, space, region,
                               s.camera.matrix_world.inverted(), projection,
                               do_color_management=False, draw_background=True)
        return np.array(buffer.texture_color.read()).reshape(48, 64, 4)

    try:
        for query in QUERIES:
            select_query(node, query)
            pixels = draw_current()
            near(center(pixels), (int(query in positive),) * 3, tolerance=.02)
            results[query] = center(pixels).tolist()
        print('DRAW_INFO_OFFSCREEN_MATRIX_PASS', sorted(positive), flush=True)

        # The native probe preview reads the captured texture without ray-tracing miss behavior.
        bpy.data.objects.remove(bpy.context.object, do_unlink=True)
        bpy.ops.mesh.primitive_uv_sphere_add(radius=10, segments=48, ring_count=24)
        bpy.context.object.data.materials.append(m)
        data = bpy.data.lightprobes.new('DrawInfoPlanarPreview', 'PLANE')
        data.influence_distance = 5
        data.clip_start = .01
        data.show_data = True
        capture = bpy.data.objects.new('DrawInfoPlanarPreview', data)
        s.collection.objects.link(capture)
        space.overlay.show_overlays = True
        space.overlay.show_extras = False
        space.overlay.show_floor = False
        space.overlay.show_axis_x = space.overlay.show_axis_y = False
        space.overlay.show_outline_selected = False
        space.overlay.show_cursor = False
        space.overlay.show_object_origins = False
        for obj in s.objects:
            obj.select_set(False)
        bpy.context.view_layer.objects.active = None
        for query, expected in [('PLANAR_PROBE_CAPTURE', 1), ('PROBE_CAPTURE', 1),
                                ('SPHERE_PROBE_CAPTURE', 0)]:
            select_query(node, query)
            data.show_data = True
            shown = center(draw_current())
            near(shown, (expected,) * 3, tolerance=.03)
            data.show_data = False
            hidden = center(draw_current())
            near(hidden, (0,) * 3, tolerance=.03)
            results['planar_' + query] = {'preview': shown.tolist(), 'main': hidden.tolist()}
    finally:
        buffer.free()
    return results


def start_window_test():
    """Use Blender's own event simulation in our isolated test window."""
    assert not bpy.app.background
    assert 'release' not in [part.lower() for part in Path(bpy.app.binary_path).parts]
    assert os.environ.get('BLENDER_USER_RESOURCES')
    s, m, emit = gui_scene()
    bpy.context.preferences.use_preferences_save = False
    bpy.context.preferences.view.show_splash = False
    s.eevee.taa_samples = 1
    s.eevee.use_taa_reprojection = False
    s.eevee.use_shadows = False
    node = probe(m.node_tree, 'VIEWPORT')
    connect(m.node_tree, query_output(node), emit.inputs['Color'])
    window = bpy.context.window
    area = next(area for area in window.screen.areas if area.type == 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    space = area.spaces.active
    space.shading.type = 'RENDERED'
    space.shading.use_scene_world_render = space.shading.use_scene_lights_render = True
    space.overlay.show_overlays = False
    space.region_3d.view_perspective = 'ORTHO'
    space.region_3d.view_matrix = s.camera.matrix_world.inverted()
    space.region_3d.view_distance = 3
    phases = [('viewport_idle', 'VIEWPORT', 1, None),
              ('image_idle', 'IMAGE_RENDER', 0, None),
              ('playback_idle', 'PLAYBACK', 0, None),
              ('playback_active', 'PLAYBACK', 1, 'play'),
              ('playback_restored', 'PLAYBACK', 0, 'stop'),
              ('navigation_idle', 'NAVIGATING', 0, None),
              ('navigation_active', 'NAVIGATING', 1, 'navigate'),
              ('navigation_restored', 'NAVIGATING', 0, 'end_navigate'),
              ('viewport_restored', 'VIEWPORT', 1, None)]
    state = {'index': -1, 'since': time.monotonic(), 'moves': 0, 'matches': 0}
    REPORT['cases']['window'] = {}

    def event(kind, value, x=None, y=None):
        window.event_simulate(type=kind, value=value,
                              x=region.x + region.width // 2 if x is None else x,
                              y=region.y + region.height // 2 if y is None else y)

    def finish_demo():
        try:
            select_query(node, 'OUTLINE_SHELL')
            bpy.data.objects.remove(bpy.context.object, do_unlink=True)
            bpy.ops.mesh.primitive_uv_sphere_add(radius=.7, segments=64, ring_count=32)
            bpy.context.object.data.materials.append(m)
            for polygon in bpy.context.object.data.polygons:
                polygon.use_smooth = True
            mix = m.node_tree.nodes.new('ShaderNodeMixRGB')
            mix.inputs[1].default_value = (.12, .4, .85, 1)
            mix.inputs[2].default_value = (1, .12, .02, 1)
            connect(m.node_tree, query_output(node), mix.inputs[0])
            connect(m.node_tree, mix.outputs[0], emit.inputs['Color'])
            shell = m.node_tree.nodes.new('ShaderNodeOutputOutlineShell')
            shell.inputs['Strength'].default_value = .04
            connect(m.node_tree, mix.outputs[0], shell.inputs['Color'])
            node.location, mix.location, emit.location = (-650, 120), (-370, 120), (-100, 180)
            m.node_tree.nodes['Material Output'].location = (140, 180)
            shell.location = (140, -100)
            for item in m.node_tree.nodes:
                item.select = False
            node.select = True
            m.node_tree.nodes.active = node
            area.type = 'NODE_EDITOR'
            area.spaces.active.tree_type = 'ShaderNodeTree'
            area.spaces.active.shader_type = 'OBJECT'
            area.tag_redraw()

            def save_preview():
                editor_region = next(region for region in area.regions if region.type == 'WINDOW')
                with bpy.context.temp_override(window=window, area=area, region=editor_region):
                    bpy.ops.node.view_all()
                    bpy.ops.wm.redraw_timer(type='DRAW_WIN_SWAP', iterations=2)
                    bpy.ops.screen.screenshot(filepath=str(OUT / 'draw_info_node.png'))
                assets = OUT / 'assets'
                assets.mkdir(exist_ok=True)
                bpy.ops.wm.save_as_mainfile(filepath=str(assets / 'draw_info_demo.blend'), check_existing=False)
                REPORT['passed'] = True
                import gpu
                REPORT['backend'] = gpu.platform.backend_type_get()
                (OUT / 'summary_window.json').write_text(json.dumps(REPORT, indent=2), encoding='utf-8')
                print('DRAW_INFO_WINDOW_PASS', json.dumps(REPORT), flush=True)
                bpy.ops.wm.quit_blender()
                return None

            def save_preview_safely():
                try:
                    return save_preview()
                except Exception:
                    fail()
                    return None

            bpy.app.timers.register(save_preview_safely, first_interval=1)
        except Exception:
            fail()

    def fail():
        REPORT['error'] = traceback.format_exc()
        (OUT / 'summary_window.json').write_text(json.dumps(REPORT, indent=2), encoding='utf-8')
        print('DRAW_INFO_WINDOW_FAILED', REPORT['error'], flush=True)
        bpy.ops.wm.quit_blender()

    def tick():
        try:
            if state['index'] < 0:
                state['index'] = 0
                state['since'] = time.monotonic()
                event('ESC', 'PRESS')
            name, query, expected, action = phases[state['index']]
            if name == 'navigation_active':
                state['moves'] += 1
                event('MOUSEMOVE', 'NOTHING', region.x + region.width // 2 + state['moves'] * 2,
                      region.y + region.height // 2 + state['moves'])
            area.tag_redraw()
            with bpy.context.temp_override(window=window, area=area, region=region):
                bpy.ops.wm.redraw_timer(type='DRAW_WIN_SWAP', iterations=2)
                path = OUT / ('window_' + name + '.png')
                bpy.ops.screen.screenshot(filepath=str(path))
            reader = oiio.ImageInput.open(str(path))
            pixels = reader.read_image(format='float')
            reader.close()
            x = region.x + region.width // 2
            y = pixels.shape[0] - region.y - region.height // 2
            value = pixels[y - 2:y + 3, x - 2:x + 3, :3].mean(axis=(0, 1))
            elapsed = time.monotonic() - state['since']
            if elapsed > 1.5 and float(np.max(np.abs(value - expected))) < .04:
                state['matches'] += 1
            else:
                state['matches'] = 0
            assert elapsed < 35, (name, value.tolist(), expected)
            if state['matches'] >= 2:
                REPORT['cases']['window'][name] = value.tolist()
                print('DRAW_INFO_WINDOW_PHASE', name, value.tolist(), flush=True)
                state['index'] += 1
                if state['index'] == len(phases):
                    finish_demo()
                    return None
                name, query, expected, action = phases[state['index']]
                if node.get('_test_query') != query:
                    select_query(node, query)
                with bpy.context.temp_override(window=window, area=area, region=region):
                    if action == 'play':
                        bpy.ops.screen.animation_play()
                    elif action == 'stop':
                        bpy.ops.screen.animation_cancel(restore_frame=False)
                    elif action == 'navigate':
                        event('MOUSEMOVE', 'NOTHING')
                        event('MIDDLEMOUSE', 'PRESS')
                    elif action == 'end_navigate':
                        event('MIDDLEMOUSE', 'RELEASE',
                              region.x + region.width // 2 + state['moves'] * 2,
                              region.y + region.height // 2 + state['moves'])
                state['since'], state['matches'] = time.monotonic(), 0
            return .3
        except Exception:
            fail()
            return None

    bpy.app.timers.register(tick, first_interval=2)


def main():
    tests = {'legacy': test_legacy, 'registration': test_registration,
             'surface': test_surface, 'shell': test_shell,
             'world': test_world, 'filter': test_filter, 'npr': test_npr, 'bake': test_bake,
             'render_texture': test_render_texture, 'vertex_depth': test_vertex_depth,
             'aov': test_aov, 'shell_aov': test_shell_aov,
             'shell_aov_hybrid_self_occlusion': test_shell_aov_hybrid_self_occlusion,
             'shared_group': test_shared_group, 'shadow': test_shadow,
             'pre_fx': test_pre_fx, 'volume': test_volume, 'probe_capture': test_probe_capture,
             'irradiance_capture': test_irradiance_capture}
    if args.mode == 'offscreen':
        tests = {'offscreen': test_offscreen}
    for name, function in tests.items():
        if args.case and name not in args.case:
            continue
        REPORT['cases'][name] = function()
        print('DRAW_INFO_CASE_PASS', name, flush=True)
    REPORT['passed'] = True
    import gpu
    REPORT['backend'] = gpu.platform.backend_type_get()
    (OUT / ('summary_' + args.mode + '.json')).write_text(json.dumps(REPORT, indent=2), encoding='utf-8')
    print('DRAW_INFO_PASS', json.dumps(REPORT), flush=True)


def run_and_exit():
    try:
        main()
    except Exception:
        REPORT['error'] = traceback.format_exc()
        (OUT / ('summary_' + args.mode + '.json')).write_text(json.dumps(REPORT, indent=2), encoding='utf-8')
        if bpy.app.background:
            raise
        print(REPORT['error'], flush=True)
    if not bpy.app.background:
        bpy.ops.wm.quit_blender()


if __name__ == '__main__':
    if args.mode == 'window':
        bpy.app.timers.register(start_window_test, first_interval=1)
    elif bpy.app.background:
        run_and_exit()
    else:
        bpy.app.timers.register(run_and_exit, first_interval=1)
