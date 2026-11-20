# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Managed UI test: file browser, native panels, GPU half precision viewer and saved example.

Launch a dedicated install/build/temp Blender window with isolated resources and TEMP.
The Release case starts this window hidden on Windows. No existing Blender is operated on.
"""
import json
import os
from pathlib import Path
import runpy
import subprocess
import time
import traceback
from types import SimpleNamespace

import bpy
import gpu
import numpy as np

helpers = runpy.run_path(str(Path(__file__).with_name('test_compositor_lut.py')),
                        run_name='lut_test_helpers')
OUT = helpers['OUT']
for filename in ('ui_failure.txt', 'ui_results.json'):
    (OUT / filename).unlink(missing_ok=True)
assert 'release' not in [part.lower() for part in Path(bpy.app.binary_path).parts]
assert Path(os.environ['BLENDER_USER_CONFIG']).resolve().is_relative_to(
    Path(os.environ['TEMP']).resolve().parent)
bpy.context.preferences.view.show_splash = False
bpy.ops.wm.save_userpref()
scene = bpy.context.scene
scene.render.compositor_device = 'GPU'
scene.render.compositor_precision = 'AUTO'
scene.use_nodes = True
tree = bpy.data.node_groups.new('LUT Color Grade Example', 'CompositorNodeTree')
scene.compositing_node_group = tree
width, height = 128, 64
x = np.linspace(.01, .95, width, dtype=np.float32)
y = np.linspace(.03, .9, height, dtype=np.float32)
pixels = np.ones((height, width, 4), np.float32)
pixels[..., 0] = x
pixels[..., 1] = y[:, None]
pixels[..., 2] = .2 + .4 * (1 - x)
pixels[height // 2:, :, :3] *= .5
pixels[height // 2:, :, 3] = .5
image = bpy.data.images.new('LUT Test Chart', width=width, height=height,
                            alpha=True, float_buffer=True)
image.colorspace_settings.name = 'Non-Color'
image.alpha_mode = 'PREMUL'
image.pixels.foreach_set(pixels.ravel())
image.update()
source = tree.nodes.new('CompositorNodeImage')
source.image = image
source.location = (-380, 60)
lut = tree.nodes.new('CompositorNodeLUT')
lut.location = (0, 60)
lut.width = 280
lut.inputs['Strength'].default_value = .7
viewer = tree.nodes.new('CompositorNodeViewer')
viewer.location = (420, 60)
tree.links.new(source.outputs['Image'], lut.inputs['Image'])
tree.links.new(lut.outputs['Image'], viewer.inputs['Image'])
source_type = os.environ.get('LUT_UI_SOURCE', 'CUBE')
if source_type == 'IMAGE':
    image_helpers = runpy.run_path(str(Path(__file__).with_name('test_compositor_image_lut.py')),
                                  run_name='image_lut_test_helpers')
    path = OUT / 'warm_film_64.png'
    reference_path = image_helpers['atlas'](path, 64, 8)
    reference_lut = SimpleNamespace(filepath=str(reference_path), preset='SRGB',
                                    interpolation='AUTO', direction='FORWARD')
else:
    path = OUT / 'warm_film.cube'
    helpers['cube'](path, 17, 'look')
    reference_lut = lut
window = bpy.context.window_manager.windows[0]
area = max(window.screen.areas, key=lambda item: item.width * item.height)
# Regression: executing a browser before its first refresh must cancel safely.
area.type = 'FILE_BROWSER'
with bpy.context.temp_override(window=window, area=area):
    assert bpy.ops.file.execute() == {'CANCELLED'}
area.type = 'NODE_EDITOR'
area.spaces.active.tree_type = 'CompositorNodeTree'
area.spaces.active.pin = True
area.spaces.active.node_tree = tree
area.spaces.active.show_region_ui = False
area.spaces.active.show_backdrop = True
state = {'stage': -1, 'started': time.monotonic(), 'errors': [], 'renders': {}}


def quit_test():
    if os.name == 'nt':
        command = (f'Get-CimInstance Win32_Process -Filter "ProcessId = {os.getpid()}" | '
                   'Select-Object ProcessId,ExecutablePath,CommandLine | ConvertTo-Json -Compress')
        raw = subprocess.check_output(['pwsh', '-NoProfile', '-Command', command],
                                      encoding='utf-8').strip()
        identity = json.loads(raw)
        print('LUT_UI_PROCESS=' + raw, flush=True)
        assert Path(identity['ExecutablePath']).resolve() == Path(bpy.app.binary_path).resolve()
        assert 'test_compositor_lut_ui.py' in identity['CommandLine']
    bpy.ops.wm.quit_blender()


def node_context(node):
    region = next(region for region in area.regions if region.type == 'WINDOW')
    return bpy.context.temp_override(window=window, area=area, region=region, node=node)


def activate_viewer():
    with node_context(viewer):
        bpy.ops.node.activate_viewer()


def screenshot(name, selected=False):
    for node in tree.nodes:
        node.select = node == lut if selected else True
    tree.nodes.active = lut
    with node_context(lut):
        if selected:
            bpy.ops.node.view_selected()
        else:
            bpy.ops.node.view_all()
        bpy.ops.wm.redraw_timer(type='DRAW_WIN_SWAP', iterations=2)
        bpy.ops.screen.screenshot(filepath=str(OUT / name))


def start_render(device):
    scene.render.compositor_device = device
    state['output'].file_name = 'ui_render_' + device.lower()
    (OUT / (state['output'].file_name + '.exr')).unlink(missing_ok=True)
    with bpy.context.temp_override(window=window, area=area):
        result = bpy.ops.render.render('INVOKE_DEFAULT')
    assert result == {'RUNNING_MODAL'}, result


def check_render(device):
    filename = OUT / ('ui_render_' + device.lower() + '.exr')
    if bpy.app.is_job_running('RENDER') or not filename.exists():
        return False
    parts = helpers['read_parts'](filename)
    # Blender image pixels start at the bottom; OIIO reads EXR scanlines from the top.
    np.testing.assert_allclose(parts['Chart'], state['expected'][::-1], rtol=0, atol=8e-5)
    expected = helpers['reference'](reference_lut, parts['Render Layers'], .25)
    np.testing.assert_allclose(parts['Graded Render Layers'], expected, rtol=0, atol=8e-5)
    final_path = OUT / ('render_result_' + device.lower() + '.exr')
    bpy.data.images['Render Result'].save_render(str(final_path), scene=scene)
    final = next(iter(helpers['read_parts'](final_path).values()))
    np.testing.assert_allclose(final, expected, rtol=0, atol=8e-5)
    state['renders'][device] = float(np.max(np.abs(parts['Graded Render Layers'] - expected)))
    return True


def tick():
    try:
        (OUT / 'ui_progress.json').write_text(json.dumps({
            'stage': state['stage'], 'elapsed': time.monotonic() - state['started'],
            'lut_path': lut.filepath, 'last_error': state.get('last_error'),
            'viewer_images': [(item.name, list(item.size)) for item in bpy.data.images
                              if item.type == 'COMPOSITING'],
        }, indent=2), encoding='utf-8')
        if time.monotonic() - state['started'] > 120:
            raise AssertionError('LUT UI/viewer did not finish within 120 seconds')
        if state['stage'] == -1:
            # Dismiss first-launch setup/splash in this isolated test window.
            window.event_simulate(type='ESC', value='PRESS')
            window.event_simulate(type='ESC', value='RELEASE')
            state['stage'] = 0
            return .5
        if state['stage'] == 0:
            with node_context(lut):
                result = bpy.ops.node.lut_file('INVOKE_DEFAULT', action='OPEN')
                assert result == {'RUNNING_MODAL'}, result
            state['stage'] = 1
            return .5
        if state['stage'] == 1:
            for candidate_window in bpy.context.window_manager.windows:
                for browser in candidate_window.screen.areas:
                    if browser.type == 'FILE_BROWSER' and browser.spaces.active.params:
                        params = browser.spaces.active.params
                        params.directory = (str(OUT) + os.sep).encode('utf-8')
                        params.filename = path.name
                        # The asynchronous file operation must retain its original LUT target.
                        tree.nodes.active = source
                        state['browser_window'] = candidate_window
                        state['browser_area'] = browser
                        browser.tag_redraw()
                        state['stage'] = 'browser_ready'
                        return .5
            return .25
        if state['stage'] == 'browser_ready':
            browser_window, browser = state['browser_window'], state['browser_area']
            with bpy.context.temp_override(window=browser_window, area=browser):
                result = bpy.ops.file.execute()
            if result == {'CANCELLED'}:
                browser.tag_redraw()
                return .25
            assert result == {'FINISHED'}, result
            del state['browser_window'], state['browser_area']
            state['stage'] = 2
            return .5
        if state['stage'] == 2:
            if not lut.filepath:
                return .25
            assert Path(bpy.path.abspath(lut.filepath)).resolve() == path.resolve()
            assert len(lut.panel_states) == 2
            assert all(panel.is_collapsed for panel in lut.panel_states)
            state['expected'] = helpers['reference'](reference_lut, pixels, .7)
            activate_viewer()
            state['stage'] = 3
            return .5
        if state['stage'] == 5:
            if not check_render('GPU'):
                return .25
            start_render('CPU')
            state['stage'] = 6
            return .5
        if state['stage'] == 6:
            if not check_render('CPU'):
                return .25
            helpers['file_action'](lut, 'PACK')
            helpers['file_action'](state['render_lut'], 'PACK')
            image.pack()
            bpy.ops.wm.save_as_mainfile(filepath=str(OUT / 'lut_color_grade_example.blend'),
                                       check_existing=False)
            results = {'backend': gpu.platform.backend_type_get(), 'viewer_precision': 'AUTO (half)',
                       'source_type': source_type,
                       'maximum_errors': state['errors'], 'file_browser_target_retained': True,
                       'uninitialized_file_browser_cancelled': True,
                       'native_panels': 2, 'example_packed': bool(lut.is_packed),
                       'interactive_render_errors': state['renders']}
            (OUT / 'ui_results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
            print('COMPOSITOR_LUT_UI_OK=' + json.dumps(results), flush=True)
            quit_test()
            return None
        viewer_image = bpy.data.images.get('Viewer Node')
        if not viewer_image or list(viewer_image.size) != [width, height]:
            return .25
        actual = np.empty(width * height * 4, np.float32)
        viewer_image.pixels.foreach_get(actual)
        actual = actual.reshape(height, width, 4)
        error = float(np.max(np.abs(actual - state['expected'])))
        if error > .003:
            state['last_error'] = error
            return .25
        state['errors'].append(error)
        if state['stage'] == 3:
            screenshot('node_compact.png')
            for panel in lut.panel_states:
                panel.is_collapsed = False
            lut.inputs['Strength'].default_value = .25
            state['expected'] = helpers['reference'](reference_lut, pixels, .25)
            activate_viewer()
            state['stage'] = 4
            return .5
        screenshot('node_expanded.png', selected=True)
        scene.render.engine = 'BLENDER_EEVEE'
        scene.eevee.taa_render_samples = 1
        scene.render.resolution_x, scene.render.resolution_y = width, height
        scene.render.resolution_percentage = 100
        scene.render.compositor_precision = 'FULL'
        layers = tree.nodes.new('CompositorNodeRLayers')
        layers.location = (-380, -240)
        render_lut = tree.nodes.new('CompositorNodeLUT')
        render_lut.location = (0, -240)
        render_lut.filepath = lut.filepath
        render_lut.inputs['Strength'].default_value = .25
        output = tree.nodes.new('CompositorNodeOutputFile')
        output.location = (420, -240)
        output.directory = str(OUT)
        output.format.media_type = 'MULTI_LAYER_IMAGE'
        output.format.color_depth = '32'
        output.format.color_management = 'OVERRIDE'
        output.format.linear_colorspace_settings.name = 'Non-Color'
        output.file_output_items.clear()
        tree.interface.new_socket(name='Image', in_out='OUTPUT', socket_type='NodeSocketColor')
        final = tree.nodes.new('NodeGroupOutput')
        final.location = (760, -240)
        tree.links.new(render_lut.outputs['Image'], final.inputs['Image'])
        scene.render.image_settings.file_format = 'OPEN_EXR'
        scene.render.image_settings.color_mode = 'RGBA'
        scene.render.image_settings.color_depth = '32'
        scene.render.image_settings.color_management = 'OVERRIDE'
        scene.render.image_settings.linear_colorspace_settings.name = 'Non-Color'
        for name, socket in [('Chart', lut.outputs['Image']),
                             ('Render Layers', layers.outputs['Image']),
                             ('Graded Render Layers', render_lut.outputs['Image'])]:
            output.file_output_items.new('RGBA', name)
            tree.links.new(socket, output.inputs[name])
        tree.links.new(layers.outputs['Image'], render_lut.inputs['Image'])
        state['render_lut'], state['output'] = render_lut, output
        start_render('GPU')
        state['stage'] = 5
        return .5
    except Exception:
        failure = traceback.format_exc()
        print(failure, flush=True)
        (OUT / 'ui_failure.txt').write_text(failure + '\nstage=' + str(state['stage']), encoding='utf-8')
        quit_test()
        return None


bpy.app.timers.register(tick, first_interval=1.0)
