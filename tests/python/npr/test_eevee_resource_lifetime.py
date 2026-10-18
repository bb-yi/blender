# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Pixel regressions for persistent viewport resources. Run with isolated user directories.

NPR_RESOURCE_MEMORY=1 records Windows dedicated GPU bytes. NPR_RESOURCE_DLSS=1 also
requires actual Vulkan/NGX execution and checks OFF/ON resource recreation.
"""

import json
import os
from pathlib import Path
import subprocess
import sys

import bpy
import gpu
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import OUT, near, scene
from filter_graph_test_utils import attach_filter_material, refresh_tree


class Viewport:
    def __init__(self, width=128, height=128):
        self.width, self.height = width, height
        self.scene = bpy.context.scene
        self.scene.eevee.taa_samples = 1
        self.scene.eevee.use_taa_reprojection = False
        self.scene.eevee.use_shadows = False
        self.scene.eevee.use_outline = False
        gpu.init()
        area = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
        self.region = next(r for r in area.regions if r.type == 'WINDOW')
        self.space = area.spaces.active
        self.space.shading.type = 'RENDERED'
        self.space.overlay.show_overlays = False
        self.space.shading.use_scene_world_render = True
        self.space.shading.use_scene_lights_render = True
        self.buffer = gpu.types.GPUOffScreen(width, height, format='RGBA16F')

    def draw(self):
        bpy.context.view_layer.update()
        deps = bpy.context.evaluated_depsgraph_get()
        projection = self.scene.camera.calc_matrix_camera(deps, x=self.width, y=self.height)
        self.buffer.draw_view3d(self.scene, bpy.context.view_layer, self.space, self.region,
                               self.scene.camera.matrix_world.inverted(), projection,
                               do_color_management=False, draw_background=True)
        pixels = np.array(self.buffer.texture_color.read(), dtype=np.float32)
        pixels = pixels.reshape(self.height, self.width, 4)
        assert np.isfinite(pixels).all()
        return pixels[self.height // 2, self.width // 2, :3]

    def free(self):
        self.buffer.free()


def memory():
    if not os.environ.get('NPR_RESOURCE_MEMORY') or os.name != 'nt':
        return None
    command = ("$r=@(Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory | "
               f"Where-Object Name -Like 'pid_{os.getpid()}_*'); "
               "[long](($r | Measure-Object DedicatedUsage -Sum).Sum)")
    return int(subprocess.check_output(['pwsh', '-NoProfile', '-Command', command], text=True).strip())


def test_render_texture():
    s, m, emission = scene()
    rt = s.eevee.render_textures.add()
    rt.name = 'Feedback'
    rt.enabled = True
    rt.camera = s.camera
    rt.resolution_x = rt.resolution_y = 128
    rt.format, rt.source, rt.update_mode = 'RGBA16F', 'COLOR', 'EVERY_FRAME'
    sample = m.node_tree.nodes.new('ShaderNodeRenderTexture')
    sample.render_texture = f'RENDER_TEXTURE_{rt.uid}'
    uv = m.node_tree.nodes.new('ShaderNodeCombineXYZ')
    uv.inputs['X'].default_value = uv.inputs['Y'].default_value = 0.5
    add = m.node_tree.nodes.new('ShaderNodeVectorMath')
    add.operation = 'ADD'
    links = m.node_tree.links
    links.new(uv.outputs['Vector'], sample.inputs['Vector'])
    links.new(sample.outputs['Color'], add.inputs[0])
    links.new(add.outputs['Vector'], emission.inputs['Color'])
    viewport = Viewport()
    records = []
    try:
        for i, expected in enumerate((.2, .5, .9)):
            add.inputs[1].default_value = (.1 * (i + 1), 0, 0)
            s.frame_set(i + 1)
            actual = viewport.draw()
            near(actual, [expected, 0, 0], .003)
            records.append(actual.tolist())
        add.inputs[1].default_value = (.1, 0, 0)
        rt.resolution_x = rt.resolution_y = 64
        near(viewport.draw(), [.2, 0, 0], .003)
        rt.format = 'RGBA32F'
        near(viewport.draw(), [.2, 0, 0], .003)
        camera = s.camera.copy()
        s.collection.objects.link(camera)
        rt.camera = camera
        near(viewport.draw(), [.2, 0, 0], .003)
        rt.enabled = False
        near(viewport.draw(), [.1, 0, 0], .003)
        rt.enabled = True
        near(viewport.draw(), [.2, 0, 0], .003)
    finally:
        viewport.free()
    return records


def test_filter():
    s, _, emission = scene()
    emission.inputs['Color'].default_value = (.1, .2, .3, 1)
    fm = bpy.data.materials.new('Filter resource lifetime')
    fm.eevee_domain = 'FILTER'
    nodes, links = fm.node_tree.nodes, fm.node_tree.links
    nodes.clear()
    source = nodes.new('ShaderNodeFilterGraphInput')
    sample = nodes.new('ShaderNodeNPR_ImageSample')
    add = nodes.new('ShaderNodeVectorMath')
    add.operation = 'ADD'
    add.inputs[1].default_value = (.025, .025, .025)
    out = nodes.new('ShaderNodeOutputFilter')
    out.inputs['Alpha'].default_value = 1
    links.new(source.outputs['Image'], sample.inputs['Image'])
    links.new(sample.outputs['Color'], add.inputs[0])
    links.new(add.outputs['Vector'], out.inputs['Color'])
    graph, previous, stage = attach_filter_material(fm, scene_socket='Color Image')
    for _ in range(9):
        current = graph.nodes.new('EeveeFilterGraphNodeFilterMaterial')
        current.material = fm
        refresh_tree(graph)
        graph.links.new(previous.outputs['Image'], current.inputs['Image'])
        previous = current
    graph.links.new(previous.outputs['Image'], stage.inputs['Image'])
    refresh_tree(graph)
    viewport = Viewport(1024, 768)
    records = {}
    try:
        for repeat in range(3):
            s.eevee.filter_graph = graph
            for _ in range(12):
                near(viewport.draw(), [.35, .45, .55], .008)
            records[f'enabled_{repeat}'] = memory()
            s.eevee.filter_graph = None
            for _ in range(12):
                near(viewport.draw(), [.1, .2, .3], .003)
            records[f'disabled_{repeat}'] = memory()
        if records['enabled_0'] is not None:
            assert records['enabled_2'] <= records['enabled_0'] + 128 * 1024**2, records
    finally:
        viewport.free()
    return records


def test_aov_snapshots():
    s, material, _ = scene()
    for name, kind, value in [('FeedbackColor', 'COLOR', (.2, .3, .4, 1)),
                              ('FeedbackValue', 'VALUE', .25)]:
        aov = bpy.context.view_layer.aovs.add()
        aov.name, aov.type = name, kind
        output = material.node_tree.nodes.new('ShaderNodeOutputAOV')
        output.aov_name = name
        output.inputs['Color' if kind == 'COLOR' else 'Value'].default_value = value
    fm = bpy.data.materials.new('Read and write AOVs')
    fm.eevee_domain = 'FILTER'
    nodes, links = fm.node_tree.nodes, fm.node_tree.links
    nodes.clear()
    samples = []
    for name, socket in [('FeedbackColor', 'Color'), ('FeedbackValue', 'Value')]:
        aov = nodes.new('ShaderNodeInputAOV')
        aov.aov_name = name
        sample = nodes.new('ShaderNodeNPR_ImageSample')
        links.new(aov.outputs[socket], sample.inputs['Image'])
        samples.append(sample)
    color_add = nodes.new('ShaderNodeVectorMath')
    color_add.operation = 'ADD'
    links.new(samples[0].outputs['Color'], color_add.inputs[0])
    links.new(samples[1].outputs['Color'], color_add.inputs[1])
    value_add = nodes.new('ShaderNodeMath')
    value_add.operation = 'ADD'
    value_add.inputs[1].default_value = .125
    links.new(samples[1].outputs['Color'], value_add.inputs[0])
    for name, socket, source in [('FeedbackColor', 'Color', color_add.outputs['Vector']),
                                 ('FeedbackValue', 'Value', value_add.outputs[0])]:
        out = nodes.new('ShaderNodeOutputAOV')
        out.aov_name = name
        links.new(source, out.inputs[socket])
    out = nodes.new('ShaderNodeOutputFilter')
    out.inputs['Alpha'].default_value = 1
    links.new(color_add.outputs['Vector'], out.inputs['Color'])
    # Legacy shader AOV Output nodes are inert in Filter materials, but still trigger the
    # compatibility snapshot path. Actual writes use the Filter Graph AOV Output node.
    graph, first, _ = attach_filter_material(fm, stage='BEFORE_POSTFX')
    write = graph.nodes.new('EeveeFilterGraphNodeAOVOutput')
    write.aov_name = 'FeedbackColor'
    write.execution_stage = 'BEFORE_POSTFX'
    graph.links.new(first.outputs['Image'], write.inputs['Color'])
    refresh_tree(graph)
    attach_filter_material(fm, stage='BEFORE_COMPOSITE', graph=graph)
    viewport = Viewport()
    try:
        for _ in range(12):
            near(viewport.draw(), [.7, .8, .9], .006)
    finally:
        viewport.free()
    return 'legacy color/value snapshots preserve graph AOV writes between stages'


def test_mesh_attributes():
    _, material, emission = scene()
    mesh = bpy.context.object.data
    # Only slot 16 is used: cache.surface_per_mat[0] need not have a batch.
    for _ in range(16):
        mesh.materials.append(material)
    for polygon in mesh.polygons:
        polygon.material_index = 16
    colors = {'ColorA': (.2, .4, .6, 1), 'ColorB': (.6, .3, .1, 1)}
    for name, color in colors.items():
        attr = mesh.attributes.new(name=name, type='FLOAT_COLOR', domain='POINT')
        attr.data.foreach_set('color', list(color) * len(mesh.vertices))
    reader = material.node_tree.nodes.new('ShaderNodeAttribute')
    material.node_tree.links.new(reader.outputs['Color'], emission.inputs['Color'])
    viewport = Viewport()
    try:
        for _ in range(3):
            for name, color in colors.items():
                reader.attribute_name = name
                near(viewport.draw(), color[:3], .003)
        mesh.attributes.remove(mesh.attributes['ColorA'])
        reader.attribute_name = 'ColorB'
        mesh.update()
        near(viewport.draw(), colors['ColorB'][:3], .003)
    finally:
        viewport.free()
    return 'attribute switches and removal preserve slot-16 pixels; timed GC is a separate UI path'


def test_dlss():
    s, _, emission = scene()
    emission.inputs['Color'].default_value = (.3, .4, .5, 1)
    viewport = Viewport(512, 384)
    records = {}
    try:
        assert gpu.platform.backend_type_get() == 'VULKAN'
        for mode in ('sr', 'nr'):
            for repeat in range(2):
                s.eevee.dlss5_intensity = 1
                s.eevee.dlss_sr_viewport_quality = 'QUALITY' if mode == 'sr' else 'OFF'
                s.eevee.dlss5_mode = 'DLSSNR' if mode == 'nr' else 'OFF'
                for _ in range(4):
                    viewport.draw()
                # GPUOffScreen is an image-render viewport; NR publishes to the final-image
                # status while SR publishes using the owning viewport instance.
                status = getattr(s.eevee, 'dlss_sr_viewport_status' if mode == 'sr' else 'dlss5_render_status')
                assert 'Active' in status, status
                records[f'{mode}_on_{repeat}'] = memory()
                print('RESOURCE_DLSS_ACTIVE', mode, repeat, status, flush=True)
                s.eevee.dlss_sr_viewport_quality = 'OFF'
                if mode == 'nr' and repeat == 1:
                    s.eevee.dlss5_intensity = 0
                else:
                    s.eevee.dlss5_mode = 'OFF'
                for _ in range(12):
                    near(viewport.draw(), [.3, .4, .5], .003)
                records[f'{mode}_off_{repeat}'] = memory()
                if records[f'{mode}_on_{repeat}'] is not None:
                    assert records[f'{mode}_off_{repeat}'] < records[f'{mode}_on_{repeat}'], records
                print('RESOURCE_DLSS_OFF', mode, repeat, flush=True)
    finally:
        viewport.free()
    return records


result = {'build_hash': bpy.app.build_hash.decode(), 'pid': os.getpid()}
if os.environ.get('NPR_RESOURCE_DLSS'):
    result['dlss'] = test_dlss()
else:
    result['render_texture'] = test_render_texture()
    result['filter'] = test_filter()
    result['aov_snapshots'] = test_aov_snapshots()
    result['mesh_attributes'] = test_mesh_attributes()
result['backend'] = gpu.platform.backend_type_get()
(OUT / ('resource_lifetime_' + ('dlss_' if 'dlss' in result else '') + result['backend'].lower() + '.json')).write_text(
    json.dumps(result, indent=2), encoding='utf-8')
print('RESOURCE_LIFETIME_RESULT', json.dumps(result), flush=True)
print('EEVEE_RESOURCE_LIFETIME_PASS', flush=True)
