# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Native WINDOW timed mesh GC probe; no GPUOffScreen or manual cache invalidation.

The process uses isolated user resources. A visible test window only exits after
the external launcher/operator writes allow_exit, so it cannot close a user window.
"""
import ctypes as C
import json
import os
import re
from pathlib import Path
import time
import traceback

import bpy
import gpu
import numpy as np
import OpenImageIO as oiio

OUT = Path(os.environ['NPR_GC_OUTPUT'])
OUT.mkdir(parents=True, exist_ok=True)
assert not bpy.app.background
assert all(os.environ.get('BLENDER_USER_' + name) for name in ('RESOURCES', 'CONFIG', 'SCRIPTS', 'DATAFILES', 'EXTENSIONS'))
bpy.context.preferences.use_preferences_save = False
bpy.context.preferences.view.show_splash = False
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.context.preferences.use_preferences_save = False
bpy.context.preferences.view.show_splash = False
prefs = bpy.context.preferences.system
prefs.vbo_time_out = 2
prefs.vbo_collection_rate = 1


def visible_windows():
    user = C.WinDLL('user32')
    found = []
    callback_type = C.WINFUNCTYPE(C.c_int, C.c_void_p, C.c_ssize_t)
    user.IsWindowVisible.argtypes = [C.c_void_p]
    user.GetWindowThreadProcessId.argtypes = [C.c_void_p, C.POINTER(C.c_uint32)]
    def each(hwnd, _):
        pid = C.c_uint32()
        user.GetWindowThreadProcessId(hwnd, C.byref(pid))
        if pid.value == os.getpid() and user.IsWindowVisible(hwnd):
            found.append(int(hwnd))
        return 1
    user.EnumWindows(callback_type(each), 0)
    return found


class DedicatedMemory:
    class Value(C.Structure):
        _fields_ = [('status', C.c_uint32), ('value', C.c_int64)]
    class Item(C.Structure):
        pass
    Item._fields_ = [('name', C.c_wchar_p), ('value', Value)]
    def __init__(self):
        self.dll = C.WinDLL('pdh')
        self.query, self.counter = C.c_void_p(), C.c_void_p()
        assert self.dll.PdhOpenQueryW(None, 0, C.byref(self.query)) == 0
        assert self.dll.PdhAddEnglishCounterW(self.query, C.c_wchar_p(
            r'\GPU Process Memory(*)\Dedicated Usage'), 0, C.byref(self.counter)) == 0
    def read(self):
        self.dll.PdhCollectQueryData(self.query)
        size, count = C.c_uint32(), C.c_uint32()
        self.dll.PdhGetFormattedCounterArrayW(self.counter, 0x400, C.byref(size), C.byref(count), None)
        if not size.value:
            return 0
        data = C.create_string_buffer(size.value)
        assert self.dll.PdhGetFormattedCounterArrayW(
            self.counter, 0x400, C.byref(size), C.byref(count), data) == 0
        items = C.cast(data, C.POINTER(self.Item))
        return sum(items[i].value.value for i in range(count.value)
                   if items[i].name.startswith(f'pid_{os.getpid()}_')
                   and items[i].value.status in (0, 1))
    def close(self):
        self.dll.PdhCloseQuery(self.query)


s = bpy.context.scene
s.render.engine = 'BLENDER_EEVEE'
s.eevee.taa_samples = 1
s.eevee.use_taa_reprojection = False
s.eevee.use_shadows = False
s.eevee.use_outline = False
s.view_settings.view_transform = 'Standard'
s.view_settings.look = 'None'
camera = bpy.data.objects.new('GC_camera', bpy.data.cameras.new('GC_camera'))
camera.location = (0, 0, 3)
s.collection.objects.link(camera)
s.camera = camera
s.camera.data.type = 'ORTHO'
s.camera.data.ortho_scale = 2.4
n = 768
yy, xx = np.mgrid[:n+1, :n+1]
vertices = np.zeros(((n+1)**2, 3), dtype=np.float32)
vertices[:, 0] = xx.ravel() * (2/n) - 1
vertices[:, 1] = yy.ravel() * (2/n) - 1
base = (yy[:-1, :-1] * (n+1) + xx[:-1, :-1]).ravel().astype(np.int32)
corners = np.stack([base, base+1, base+n+2, base, base+n+2, base+n+1], axis=1).ravel()
mesh = bpy.data.meshes.new('GC_large_grid')
mesh.vertices.add(len(vertices))
mesh.vertices.foreach_set('co', vertices.ravel())
mesh.loops.add(len(corners))
mesh.loops.foreach_set('vertex_index', corners)
mesh.polygons.add(len(corners)//3)
mesh.polygons.foreach_set('loop_start', np.arange(0, len(corners), 3, dtype=np.int32))
mesh.polygons.foreach_set('loop_total', np.full(len(corners)//3, 3, dtype=np.int32))
mesh.polygons.foreach_set('material_index', np.full(len(corners)//3, 16, dtype=np.int32))
mesh.update(calc_edges=True)
use_uv = bool(os.environ.get('NPR_GC_UV'))
if use_uv:
    uv_layer = mesh.uv_layers.new(name='GC_UV')
    uv_layer.data.foreach_set('uv', (vertices[corners, :2] * .5 + .5).ravel())
ob = bpy.data.objects.new('GC_grid_slot_16', mesh)
s.collection.objects.link(ob)
mat = bpy.data.materials.new('GC_unchanged_image')
mat.use_nodes = True
for _ in range(17):
    mesh.materials.append(mat)
mesh.polygons.foreach_set('material_index', np.full(len(mesh.polygons), 16, dtype=np.int32))
nodes, links = mat.node_tree.nodes, mat.node_tree.links
nodes.clear()
out = nodes.new('ShaderNodeOutputMaterial')
emission = nodes.new('ShaderNodeEmission')
links.new(emission.outputs[0], out.inputs['Surface'])
readers = []
for i, color in enumerate(((.2, .1, .05, 1), (.1, .2, .05, 1),
                           (.05, .1, .2, 1), (.05, .1, .2, 1))):
    name = f'GC_Color_{i}'
    attr = mesh.attributes.new(name=name, type='FLOAT_COLOR', domain='POINT')
    attr.data.foreach_set('color', np.tile(np.array(color, np.float32), len(vertices)))
    reader = nodes.new('ShaderNodeAttribute')
    reader.attribute_name = name
    readers.append(reader.outputs['Color'])
def add(a, b):
    node = nodes.new('ShaderNodeVectorMath')
    node.operation = 'ADD'
    links.new(a, node.inputs[0])
    links.new(b, node.inputs[1])
    return node.outputs['Vector']
pair = add(readers[0], readers[1])
all_attributes = add(pair, add(readers[2], readers[3]))
tail = nodes.new('ShaderNodeRGB')
tail.outputs[0].default_value = (.1, .2, .4, 1)
partial_attributes = add(pair, tail.outputs[0])
constant = nodes.new('ShaderNodeRGB')
constant.outputs[0].default_value = (.4, .5, .5, 1)
links.new(all_attributes, emission.inputs['Color'])
color_input = emission.inputs['Color']
if use_uv:
    uv = nodes.new('ShaderNodeUVMap')
    uv.uv_map = 'GC_UV'
    checker = nodes.new('ShaderNodeTexChecker')
    checker.inputs['Color1'].default_value = (.8, .8, .8, 1)
    checker.inputs['Color2'].default_value = (1, 1, 1, 1)
    checker.inputs['Scale'].default_value = 8
    links.new(uv.outputs['UV'], checker.inputs['Vector'])
    multiply = nodes.new('ShaderNodeVectorMath')
    multiply.operation = 'MULTIPLY'
    links.new(all_attributes, multiply.inputs[0])
    links.new(checker.outputs['Color'], multiply.inputs[1])
    normal = nodes.new('ShaderNodeNormalMap')
    normal.uv_map = 'GC_UV'
    normal.inputs['Color'].default_value = (.6, .7, .9, 1)
    separate = nodes.new('ShaderNodeSeparateXYZ')
    links.new(normal.outputs['Normal'], separate.inputs[0])
    normal_factor = nodes.new('ShaderNodeMath')
    normal_factor.operation = 'ADD'
    normal_factor.inputs[1].default_value = 1
    links.new(separate.outputs['X'], normal_factor.inputs[0])
    scale = nodes.new('ShaderNodeVectorMath')
    scale.operation = 'SCALE'
    links.new(multiply.outputs['Vector'], scale.inputs[0])
    links.new(normal_factor.outputs[0], scale.inputs['Scale'])
    links.new(scale.outputs['Vector'], emission.inputs['Color'])
    color_input = multiply.inputs[0]
del vertices, corners, base, xx, yy

window = bpy.context.window_manager.windows[0]
area = next(a for a in window.screen.areas if a.type == 'VIEW_3D')
region = next(r for r in area.regions if r.type == 'WINDOW')
space = area.spaces.active
space.shading.type = 'RENDERED'
space.shading.use_scene_world_render = True
space.shading.use_scene_lights_render = True
space.overlay.show_overlays = False
space.show_gizmo = False
space.camera = s.camera
space.region_3d.view_perspective = 'CAMERA'
space.region_3d.view_camera_zoom = 0
sampler = DedicatedMemory()
phases = [
    ('active_four', all_attributes, 2, 7),
    ('constant_gc_disabled', constant.outputs[0], 0, 7),
    ('constant_gc_enabled', constant.outputs[0], 2, 8),
    ('rebuilt_four', all_attributes, 2, 6),
    ('active_two_gc', partial_attributes, 2, 8),
    ('rebuilt_four_again', all_attributes, 2, 6),
    ('constant_gc_again', constant.outputs[0], 2, 8),
]
split_test = bool(os.environ.get('NPR_GC_SPLIT_TEST'))
if split_test:
    assert use_uv
    phases += [
        ('layers_idle_gc_disabled', all_attributes, 0, 7),
        ('layers_idle_gc_enabled', all_attributes, 2, 8),
        ('layers_rebuilt', all_attributes, 2, 6),
    ]
state = {'index': -1, 'draws': 0, 'since': time.monotonic(), 'pixels': None, 'done': False}
report = {'passed': False, 'pid': os.getpid(), 'binary': bpy.app.binary_path,
          'backend': None, 'vertices': len(mesh.vertices), 'corners': len(mesh.loops),
          'attribute_vbo_bytes_each': len(mesh.loops) * 16,
          'gc_timeout': 2, 'gc_collection_rate': 1, 'samples': [], 'phases': {}}
report['force_live_draw'] = bool(os.environ.get('NPR_GC_LIVE_DRAW'))
report['active_uv_and_tangent'] = use_uv
report['require_native_release_trace'] = bool(os.environ.get('NPR_GC_TRACE_REQUIRED'))
report['split_category_test'] = split_test


def on_draw():
    if bpy.context.area != area or state['done']:
        return
    state['draws'] += 1


def capture(phase):
    # POST_PIXEL can be drawing the transparent overlay attachment. Capture the composited
    # native window instead of treating that attachment as the EEVEE color image.
    path = OUT / (phase + '.png')
    with bpy.context.temp_override(window=window, area=area, region=region):
        camera.location.x = 0
        bpy.context.view_layer.update()
        area.tag_redraw()
        bpy.ops.wm.redraw_timer(type='DRAW_WIN_SWAP', iterations=2)
        bpy.ops.screen.screenshot(filepath=str(path))
    image = oiio.ImageInput.open(str(path))
    pixels = image.read_image(format='float')
    image.close()
    x = region.x + region.width//2
    y = pixels.shape[0] - region.y - region.height//2
    return pixels[y-64:y+64, x-64:x+64, :3].copy()


handler = bpy.types.SpaceView3D.draw_handler_add(on_draw, (), 'WINDOW', 'POST_PIXEL')


def exit_when_permitted():
    if not visible_windows() or (OUT / 'allow_exit').exists():
        bpy.ops.wm.quit_blender()
        return None
    return .5


def finish(error=None):
    state['done'] = True
    sampler.close()
    bpy.types.SpaceView3D.draw_handler_remove(handler, 'WINDOW')
    report['error'] = error
    report['passed'] = error is None
    report['visible_windows_at_end'] = visible_windows()
    (OUT / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('MESH_TIMED_GC_RESULT', json.dumps({k:v for k,v in report.items() if k != 'samples'}), flush=True)
    bpy.app.timers.register(exit_when_permitted, first_interval=.5)


def tick():
    try:
        now = time.monotonic()
        if state['index'] >= 0:
            phase, _, _, duration = phases[state['index']]
            report['samples'].append({'phase': phase, 'seconds': now-state['since'],
                                      'dedicated_bytes': sampler.read(), 'draws': state['draws']})
            if now-state['since'] < duration or state['draws'] < 4:
                if report['force_live_draw']:
                    # Reset accumulation without touching mesh data or its draw cache.
                    camera.location.x = .002 if state['draws'] % 2 else -.002
                area.tag_redraw()
                with bpy.context.temp_override(window=window, area=area, region=region):
                    bpy.ops.wm.redraw_timer(type='DRAW_WIN', iterations=1)
                return .25
            pixels = capture(phase)
            assert pixels is not None and np.isfinite(pixels).all(), phase
            np.save(OUT / (phase + '.npy'), pixels)
            values = [r['dedicated_bytes'] for r in report['samples']
                      if r['phase'] == phase and r['seconds'] >= duration-2]
            record = {'dedicated_bytes_median': float(np.median(values)),
                      'rgb_mean': pixels[..., :3].mean(axis=(0,1)).tolist(),
                      'draws': state['draws']}
            assert min(record['rgb_mean']) > .3 and max(record['rgb_mean']) < .9, record
            if state['index'] == 0:
                state['reference'] = pixels.copy()
            if phase == 'layers_idle_gc_disabled':
                state['layer_reference'] = pixels.copy()
            reference = state['layer_reference'] if phase.startswith('layers_idle') else state['reference']
            record['pixel_max_delta'] = float(np.abs(pixels-reference).max())
            assert record['pixel_max_delta'] <= 2/255, (phase, record)
            report['phases'][phase] = record
            print('GC_PHASE', phase, json.dumps(record), flush=True)
        state['index'] += 1
        if state['index'] == len(phases):
            p = report['phases']
            mem = lambda name: p[name]['dedicated_bytes_median']
            report['disabled_to_gc_drop'] = mem('constant_gc_disabled') - mem('constant_gc_enabled')
            report['rebuild_growth'] = mem('rebuilt_four') - mem('constant_gc_enabled')
            report['partial_gc_drop'] = mem('rebuilt_four') - mem('active_two_gc')
            report['second_gc_drop'] = mem('rebuilt_four_again') - mem('constant_gc_again')
            physical_ok = (report['disabled_to_gc_drop'] > 64*1024**2 and
                           report['rebuild_growth'] > 64*1024**2 and
                           32*1024**2 < report['partial_gc_drop'] < 160*1024**2 and
                           report['second_gc_drop'] > 64*1024**2)
            report['physical_memory_thresholds_met'] = physical_ok
            if report['require_native_release_trace']:
                # A driver may retain freed allocation/residency. Verify actual VBO destruction
                # independently; retain the OS counter result rather than redefining it as freed.
                log = '\n'.join((OUT / name).read_text(encoding='utf-8', errors='replace')
                                for name in ('stdout.log', 'stderr.log'))
                events = [tuple(map(int, match)) for match in re.findall(
                    r'mesh_gc name=GC_large_grid attributes=(\d+)->(\d+) released_uploaded_attribute_bytes=(\d+)', log)]
                report['native_release_events'] = events
                expected = report['attribute_vbo_bytes_each'] * 4
                assert sum(a == 4 and b == 0 and size == expected for a,b,size in events) >= 2, events
                assert any(a == 4 and b == 2 and size == expected for a,b,size in events), events
                if split_test:
                    layer_events = [tuple(map(int, match)) for match in re.findall(
                        r'mesh_gc_layers name=GC_large_grid attributes_retained=(\d+) released_uploaded_layer_bytes=(\d+)', log)]
                    report['layer_release_events'] = layer_events
                    before_layers = log.split('GC_PHASE_BEGIN layers_idle_gc_disabled', 1)[0]
                    assert 'mesh_gc_layers name=GC_large_grid' not in before_layers, before_layers[-2500:]
                    inverse = log.split('GC_PHASE_BEGIN layers_idle_gc_disabled', 1)[1].split('GC_PHASE_BEGIN layers_rebuilt', 1)[0]
                    assert 'mesh_gc name=GC_large_grid' not in inverse, inverse
                    assert layer_events and all(count == 4 and size > 0 for count,size in layer_events), layer_events
                    report['layer_gc_drop'] = mem('layers_idle_gc_disabled') - mem('layers_idle_gc_enabled')
            else:
                assert physical_ok, report['phases']
            finish()
            return None
        phase, source, timeout, _ = phases[state['index']]
        print('GC_PHASE_BEGIN', phase, flush=True)
        prefs.vbo_time_out = timeout
        if split_test:
            target = source if phase.startswith('layers_idle') else scale.outputs['Vector']
            if emission.inputs['Color'].links[0].from_socket != target:
                links.new(target, emission.inputs['Color'])
        if color_input.links[0].from_socket != source:
            links.new(source, color_input)
        state['since'], state['draws'] = now, 0
        report['backend'] = gpu.platform.backend_type_get()
        (OUT / 'progress.json').write_text(json.dumps({'pid': os.getpid(), 'phase': phase,
            'visible_windows': visible_windows()}), encoding='utf-8')
        area.tag_redraw()
        return .25
    except Exception:
        finish(traceback.format_exc())
        return None


(OUT / 'ready.json').write_text(json.dumps({'pid': os.getpid(), 'binary': bpy.app.binary_path,
    'visible_windows': visible_windows()}), encoding='utf-8')
bpy.app.timers.register(tick, first_interval=2)
