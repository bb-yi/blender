# SPDX-License-Identifier: GPL-2.0-or-later
"""Read two actual window framebuffers while switching legacy/layered scheduling."""
import json
import os
from pathlib import Path
import sys
import time
import traceback

import bpy
import gpu
import numpy as np

bpy.context.preferences.view.show_splash = False
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).parent))
from test_npr_forward_refraction import setup_scene, forward_plane, glass

OUT = Path(os.environ['BLENDER_TEST_OUTPUT_DIR'])
OUT.mkdir(parents=True, exist_ok=True)
window = bpy.context.window or bpy.context.window_manager.windows[0]
window.scene = bpy.data.scenes.new('NPR Forward viewport validation')
screen = window.screen
first = next(a for a in screen.areas if a.type == 'VIEW_3D')
first_region = next(r for r in first.regions if r.type == 'WINDOW')
with bpy.context.temp_override(window=window, area=first, region=first_region):
    scene, _ = setup_scene(reset=False)
    producer = forward_plane('Viewport alpha background', (.9, .1, .15), .5)
refractor = None
bpy.context.view_layer.update()
scene.eevee.npr_forward_refraction_mode = 'LEGACY'
scene.eevee.taa_samples = 8
with bpy.context.temp_override(window=window, area=first):
    bpy.ops.screen.area_split(direction='VERTICAL', factor=.55)
areas = [a for a in screen.areas if a.type == 'VIEW_3D']
assert len(areas) >= 2
for area in areas:
    space = area.spaces.active
    space.shading.type = 'RENDERED'
    space.shading.render_pass = 'COMBINED'
    space.shading.use_scene_world_render = True
    space.shading.use_scene_lights_render = True
    space.overlay.show_overlays = False
    space.use_local_camera = True
    space.camera = scene.camera
    region = next(r for r in area.regions if r.type == 'WINDOW')
    with bpy.context.temp_override(window=window, area=area, region=region):
        if space.region_3d.view_perspective != 'CAMERA':
            bpy.ops.view3d.view_camera()
        space.region_3d.view_perspective = 'CAMERA'

phases = ['reference', 'legacy', 'layered', 'rollback', 'layered_again', 'resized']
state = {'phase': 0, 'frames': {}, 'results': {}, 'start': time.monotonic()}
print('NPR_FORWARD_VIEWPORT_SETUP', producer.name, list(producer.location),
      producer.visible_get(view_layer=window.view_layer), scene.camera.name,
      list(scene.camera.location), flush=True)


def draw():
    area = bpy.context.area
    if area not in areas or bpy.context.region.type != 'WINDOW':
        return
    region = bpy.context.region
    key = str(area.as_pointer())
    state['frames'].setdefault(key, []).append(1)


handler = bpy.types.SpaceView3D.draw_handler_add(draw, (), 'WINDOW', 'POST_PIXEL')


def finish(error=None):
    bpy.types.SpaceView3D.draw_handler_remove(handler, 'WINDOW')
    report = {'backend': gpu.platform.backend_type_get(), 'views': len(areas),
              'results': state['results'], 'error': error,
              'resize_before': state.get('resize_before'), 'resize_after': [a.width for a in areas]}
    (OUT / 'viewport_summary.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('NPR_FORWARD_VIEWPORT_' + ('FAIL' if error else 'OK'), flush=True)
    bpy.ops.wm.quit_blender()


def tick():
    global refractor
    try:
        if time.monotonic() - state['start'] > 60:
            raise RuntimeError('Actual viewport test timed out')
        if state.get('resize_pending'):
            with bpy.context.temp_override(window=window, area=None, region=None):
                if not bpy.ops.screen.area_move.poll():
                    state['resize_attempt'] += 1
                    assert state['resize_attempt'] <= 6, 'Could not activate the owned window border'
                    x, y = state['resize_coords']
                    offset = (0, -1, 1, -2, 2, 0)[state['resize_attempt'] - 1]
                    window.event_simulate(type='MOUSEMOVE', value='NOTHING', x=x+offset, y=y)
                    return .25
                result = bpy.ops.screen.area_move(x=state['resize_coords'][0],
                    y=state['resize_coords'][1], delta=70)
                assert 'FINISHED' in result, result
            state['resize_pending'] = False
            state['frames'] = {}
            return .25
        for area in areas:
            area.tag_redraw()
        if any(len(state['frames'].get(str(a.as_pointer()), [])) < 12 for a in areas):
            return .25
        label = phases[state['phase']]
        screenshot = OUT / (label + '.png')
        with bpy.context.temp_override(window=window):
            bpy.ops.screen.screenshot(filepath=str(screenshot))
        image = bpy.data.images.load(str(screenshot), check_existing=False)
        width, height = image.size
        data = np.array(image.pixels[:], dtype=np.float32).reshape(height, width, 4)
        captures = {}
        for area in areas:
            region = next(r for r in area.regions if r.type == 'WINDOW')
            x, y = region.x + region.width // 2, region.y + region.height // 2
            captures[str(area.as_pointer())] = data[y-4:y+4, x-4:x+4].mean(axis=(0, 1)).tolist()
        bpy.data.images.remove(image)
        if label == 'reference' and any(value[0] < value[1] for value in captures.values()):
            print('NPR_FORWARD_VIEWPORT_WAIT', captures, flush=True)
            return .5
        state['results'][label] = captures
        if label == 'resized':
            assert state['resize_before'] != [a.width for a in areas], 'Actual viewport extents did not change'
        if label in ('layered', 'layered_again', 'resized'):
            for key, value in captures.items():
                np.testing.assert_allclose(value[:3], state['results']['reference'][key][:3], atol=.04, rtol=0)
        elif label == 'legacy':
            for key, value in captures.items():
                assert np.max(np.abs(np.array(value[:3]) - state['results']['reference'][key][:3])) > .06
        elif label == 'rollback':
            for key, value in captures.items():
                np.testing.assert_allclose(value, state['results']['legacy'][key], atol=.025, rtol=0)
        print('NPR_FORWARD_VIEWPORT_PIXEL', label, captures, flush=True)
        state['phase'] += 1
        if state['phase'] == len(phases):
            finish()
            return None
        label = phases[state['phase']]
        if refractor is None:
            with bpy.context.temp_override(window=window, area=first, region=first_region):
                refractor = glass()
        scene.eevee.npr_forward_refraction_mode = 'LAYERED' if label.startswith('layered') or label == 'resized' else 'LEGACY'
        if label == 'resized':
            left = min(areas, key=lambda a: a.x)
            state['resize_before'] = [a.width for a in areas]
            state['resize_coords'] = (left.x + left.width - 1, left.y + left.height // 2)
            state['resize_pending'] = True
            state['resize_attempt'] = 0
            assert bpy.app.use_event_simulate, 'Start this owned UI test with --enable-event-simulate'
            window.event_simulate(type='MOUSEMOVE', value='NOTHING',
                x=state['resize_coords'][0], y=state['resize_coords'][1])
        bpy.context.view_layer.update()
        state['frames'] = {}
        return .25
    except Exception:
        finish(traceback.format_exc())
        return None


bpy.app.timers.register(tick, first_interval=.5)
