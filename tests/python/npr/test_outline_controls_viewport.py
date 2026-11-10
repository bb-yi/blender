# SPDX-License-Identifier: GPL-2.0-or-later
"""Windowed, synthetic coverage of outline controls at all viewport resolutions."""
import json
import os
from pathlib import Path
import sys
import time
import traceback

import bpy
import gpu
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_outline_controls as controls
import test_outline_viewport_resolution as viewport
import test_outline_visibility as visibility

OUTPUT = Path(os.environ['BLENDER_TEST_OUTPUT_DIR'])
OUTPUT.mkdir(parents=True, exist_ok=True)
REPORT = {'passed': False, 'cases': {}, 'pid': os.getpid()}


def image(label, expected=None):
    scene = bpy.context.scene
    # A cold material variant may still be compiling after the first visible frame.
    deadline = time.monotonic() + 90
    previous = None
    while True:
        # Submit the actual window to let asynchronously compiled material replacements
        # become active. A stable blue surface alone can still be the previous material.
        window, area, region, space = viewport.viewport()
        with bpy.context.temp_override(window=window, area=area, region=region):
            area.tag_redraw()
            bpy.ops.wm.redraw_timer(type='DRAW_WIN_SWAP', iterations=1)
            gpu.state.active_framebuffer_get().read_color(0, 0, 1, 1, 4, 0, 'FLOAT')
        pixels = viewport.image(scene, (256, 256))
        if (expected is None or expected(pixels)) and previous is not None and np.array_equal(previous, pixels):
            break
        assert time.monotonic() < deadline, 'Viewport never reached the requested state: ' + label
        previous = pixels
        time.sleep(.03)
    np.save(OUTPUT / (label + '.npy'), pixels)
    REPORT['cases'][label] = {'red_pixels': int(visibility.red_mask(pixels).sum())}
    return pixels


def run():
    try:
        gpu.init()
        assert not bpy.app.background
        REPORT['backend'] = gpu.platform.backend_type_get()
        for identity in ('LEGACY', 'OBJECT_INSTANCE'):
            controls.setup()
            _, _, receiver = controls.receiving_mask_surface()
            scene = bpy.context.scene
            scene.eevee.outline_surface_identity = identity
            roi = (slice(70, 186), slice(129, 135))
            for scale, _ in viewport.SCALES:
                scene.eevee.outline_viewport_resolution = scale
                receiver.inputs['Expansion Mask'].default_value = 1
                reference = image(f'mask_allowed_{identity}_{scale}',
                                  lambda pixels: visibility.red_mask(pixels)[roi].sum() > 40)
                assert visibility.red_mask(reference)[roi].sum() > 40, (identity, scale)
                receiver.inputs['Expansion Mask'].default_value = 0
                blocked = image(f'mask_blocked_{identity}_{scale}',
                                lambda pixels: visibility.red_mask(pixels)[roi].sum() == 0)
                assert visibility.red_mask(blocked)[roi].sum() == 0, (identity, scale)
                np.testing.assert_array_equal(reference[:, :126], blocked[:, :126])
        controls.setup()
        _, _, control = controls.region_surface('flat')
        scene = bpy.context.scene
        for scale, _ in viewport.SCALES:
            scene.eevee.outline_viewport_resolution = scale
            control.id_combine = 'LEGACY'
            reference = image('flat_original_' + scale,
                              lambda pixels: visibility.red_mask(pixels)[controls.CENTER].sum() > 200)
            assert visibility.red_mask(reference)[controls.CENTER].sum() > 200, scale
            # With independent depth width zero this only changes the detector variant,
            # so pure ID lines must remain identical without any Freestyle fallback.
            control.inputs['Depth Threshold Range'].default_value = .1
            original_detector = image('flat_original_detector_' + scale,
                                      lambda pixels: visibility.red_mask(pixels)[controls.CENTER].sum() > 200)
            np.testing.assert_array_equal(reference, original_detector)
            control.inputs['Depth Threshold Range'].default_value = 0
            control.id_combine = 'DEPTH_OR_NORMAL'
            result = image('flat_masked_' + scale,
                           lambda pixels: visibility.red_mask(pixels)[controls.CENTER].sum() == 0)
            assert visibility.red_mask(result)[controls.CENTER].sum() == 0, scale
        # Final renders remain full resolution with the new controls active.
        scene = controls.setup()
        _, _, control = controls.region_surface('crease')
        scene.eevee.outline_surface_identity = 'OBJECT_INSTANCE'
        control.edge_priority = 'ID'
        control.inputs['Expansion Mask'].default_value = .5
        reference = None
        for scale, _ in viewport.SCALES:
            scene.eevee.outline_viewport_resolution = scale
            controls.render('controlled_final_' + scale)
            result, names = controls.performance.exr_pixels(OUTPUT / ('controlled_final_' + scale + '.exr'))
            if reference is None:
                reference = result
            np.testing.assert_array_equal(reference, result)
        REPORT['final_render_unchanged'] = True
        REPORT['passed'] = True
        print('OUTLINE_CONTROLS_VIEWPORT_OK', REPORT['backend'], flush=True)
    except Exception:
        REPORT['error'] = traceback.format_exc()
        print(REPORT['error'], flush=True)
    finally:
        (OUTPUT / 'summary.json').write_text(json.dumps(REPORT, indent=2))
        recovery = OUTPUT / 'recovery'
        recovery.mkdir(exist_ok=True)
        bpy.context.preferences.filepaths.temporary_directory = str(recovery)
        bpy.app.timers.register(quit_test, first_interval=.1)


def quit_test():
    assert not bpy.data.filepath
    bpy.ops.wm.quit_blender()


if __name__ == '__main__':
    bpy.context.preferences.view.show_splash = False
    bpy.app.timers.register(run, first_interval=.3)
