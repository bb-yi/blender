# SPDX-License-Identifier: GPL-2.0-or-later
"""Windowed GPU regression and benchmark for viewport-only outline resolution.

Run in an isolated factory-startup UI process. All scenes and saved files are synthetic.
BLENDER_TEST_OUTPUT_DIR selects the output directory; BLENDER_OUTLINE_VIEWPORT_MODE
can select 'pixels' (default) or 'bench'.
"""

import importlib.util
import ctypes
import json
import os
from pathlib import Path
import subprocess
import time
import traceback

import bpy
import gpu
import numpy as np

OUTPUT = Path(os.environ['BLENDER_TEST_OUTPUT_DIR'])
OUTPUT.mkdir(parents=True, exist_ok=True)
ASSETS = Path(os.environ.get('BLENDER_TEST_ASSET_DIR', str(OUTPUT / 'assets')))
MODE = os.environ.get('BLENDER_OUTLINE_VIEWPORT_MODE', 'pixels')
REPORT = {'passed': False, 'mode': MODE, 'cases': {}, 'pid': os.getpid()}
SCALES = [('FULL', 1), ('THREE_QUARTERS', .75), ('HALF', .5), ('QUARTER', .25)]
spec = importlib.util.spec_from_file_location(
    'outline_fixture', Path(__file__).with_name('test_outline_performance.py'))
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


def viewport():
    window = bpy.context.window_manager.windows[0]
    area = next(area for area in window.screen.areas if area.type == 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    space = area.spaces.active
    space.shading.type = 'RENDERED'
    space.shading.use_scene_world_render = True
    space.shading.use_scene_lights_render = True
    space.overlay.show_overlays = False
    space.show_gizmo = False
    space.show_region_toolbar = False
    space.show_region_ui = False
    space.region_3d.view_perspective = 'CAMERA'
    bpy.context.scene.view_layers[0].eevee.use_pass_outline = False
    return window, area, region, space


def image(scene, size=(269, 157), expect_surface=True):
    window, area, region, space = viewport()
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    projection = scene.camera.calc_matrix_camera(depsgraph, x=size[0], y=size[1])
    target = gpu.types.GPUOffScreen(*size, format='RGBA16F')
    try:
        previous = None
        stable = 0
        for attempt in range(600):
            target.draw_view3d(scene, bpy.context.view_layer, space, region,
                              scene.camera.matrix_world.inverted(), projection,
                              do_color_management=False, draw_background=True)
            pixels = np.array(target.texture_color.read(), dtype=np.float32).reshape(*size[::-1], 4)
            stable = stable + 1 if previous is not None and np.array_equal(previous, pixels) else 0
            # Allow asynchronous material compilation to finish before accepting a stable result.
            surface = np.any((pixels[..., 2] > .04) & (pixels[..., 2] > 2 * pixels[..., 1]) &
                             (pixels[..., 2] > 2 * pixels[..., 0]))
            if stable >= 2 and (surface or not expect_surface or max(size) <= 5):
                assert np.isfinite(pixels).all()
                return pixels
            previous = pixels
            time.sleep(.02)
        raise AssertionError('Viewport did not settle to a finite rendered surface')
    finally:
        target.free()


def record(name, pixels):
    np.save(OUTPUT / (name + '.npy'), pixels)
    red = pixels[..., 0]
    REPORT['cases'][name] = {'shape': list(pixels.shape), 'red_sum': float(red.sum()),
                             'red_pixels': int(np.count_nonzero(red > .001))}


def pixel_cases():
    assert bpy.context.scene.eevee.outline_viewport_resolution == 'FULL'
    prop = bpy.context.scene.eevee.bl_rna.properties['outline_viewport_resolution']
    assert [(item.identifier, item.value) for item in prop.enum_items] == [
        ('FULL', 0), ('THREE_QUARTERS', 1), ('HALF', 2), ('QUARTER', 3)]
    scene, control = fixture.build_scene(8, plane_only=True, reset=False)
    references = {}
    for width in [0, .25, .71, 1, 1.001, 8, 32]:
        control.inputs['Line Width'].default_value = width
        for setting, scale in SCALES:
            scene.eevee.outline_viewport_resolution = setting
            pixels = image(scene)
            label = f'width_{width}_{setting}'
            record(label, pixels)
            if width == 0:
                assert not pixels[..., 0].any(), label
            else:
                assert pixels[..., 0].max() > .001, label
            if width == 8:
                # Integrated premultiplied coverage of one straight vertical border, in screen pixels.
                coverage = float(pixels[pixels.shape[0] // 2, :pixels.shape[1] // 2, 0].sum())
                REPORT['cases'][label]['screen_width_coverage'] = coverage
                references[setting] = coverage
            if setting == 'FULL':
                original = pixels
        scene.eevee.outline_viewport_resolution = 'FULL'
        np.testing.assert_array_equal(image(scene), original)
    for setting, coverage in references.items():
        ratio = coverage / references['FULL']
        REPORT['cases']['width_8_' + setting]['coverage_ratio'] = ratio
        assert .7 < ratio < 1.3, (setting, ratio)

    control.inputs['Line Width'].default_value = 8
    for setting, scale in SCALES:
        scene.eevee.outline_viewport_resolution = setting
        for size in [(1, 1), (3, 5), (65, 33)]:
            pixels = image(scene, size)
            record(f'extent_{size[0]}_{size[1]}_{setting}', pixels)
    control.inputs['Line Width'].default_value = 8
    control.inputs['Depth Edge Width'].default_value = .3
    control.inputs['ID Edge Width'].default_value = .7
    for setting, scale in SCALES:
        scene.eevee.outline_viewport_resolution = setting
        record('varying_' + setting, image(scene))

    # A nearer, unoutlined surface must continue to hide interpolated outlines.
    bpy.ops.mesh.primitive_plane_add(size=.65, location=(-.75, 0, .5))
    blocker = bpy.context.object
    blocker_material, blocker_control = fixture.outline_material('Foreground', 0)
    blocker.data.materials.append(blocker_material)
    for setting, scale in SCALES:
        scene.eevee.outline_viewport_resolution = setting
        pixels = image(scene)
        record('occlusion_' + setting, pixels)
        # Camera ortho scale is 4, so the foreground center lies near x=84.
        assert not np.any(pixels[74:83, 77:88, 0] > .001), setting
    for alpha in [.5, 0]:
        scene, control = fixture.build_scene(8, alpha=alpha, method='BLENDED',
                                              plane_only=True, reset=False)
        for setting, scale in SCALES:
            scene.eevee.outline_viewport_resolution = setting
            pixels = image(scene, expect_surface=alpha > 0)
            record(f'alpha_{alpha}_{setting}', pixels)
            assert bool(pixels[..., 0].max() > .001) == (alpha > 0)

    # Viewport setting must not alter final render color/alpha/depth/velocity/Outline channels.
    scene, control = fixture.build_scene(8, ribbon=True, reset=False)
    scene.view_layers[0].eevee.use_pass_outline = True
    final_reference = None
    for setting in ['FULL', 'QUARTER', 'THREE_QUARTERS', 'HALF', 'FULL']:
        scene.eevee.outline_viewport_resolution = setting
        path = OUTPUT / ('final_' + setting + '.exr')
        scene.render.filepath = str(path)
        bpy.ops.render.render(write_still=True)
        pixels, names = fixture.exr_pixels(path)
        if final_reference is None:
            final_reference = pixels
        np.testing.assert_array_equal(pixels, final_reference)
    REPORT['final_render_unchanged'] = True
    # Save/reopen only this synthetic scene. No user asset is loaded into this process.
    scene.eevee.outline_viewport_resolution = 'HALF'
    ASSETS.mkdir(parents=True, exist_ok=True)
    path = ASSETS / 'synthetic_resolution.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(path))
    # Reopen in a child process, keeping the active UI callback and its region alive.
    result = subprocess.run([bpy.app.binary_path, '--background', '--factory-startup', str(path),
        '--python-exit-code', '1', '--python-expr',
        "import bpy; assert bpy.context.scene.eevee.outline_viewport_resolution == 'HALF'; "
        "print('OUTLINE_RESOLUTION_ROUNDTRIP_OK')"], check=True, capture_output=True, text=True)
    assert 'OUTLINE_RESOLUTION_ROUNDTRIP_OK' in result.stdout, result.stdout + result.stderr
    REPORT['setting_roundtrip'] = True


def benchmark():
    scene, control = fixture.build_scene(size=(1597, 555), dense=True, mark_edges=True, reset=False)
    window, area, region, space = viewport()
    REPORT['extent'] = [region.width, region.height]
    for case, width, factor in [('thin', .71, 1), ('wide', 8, 1), ('varying', 8, .7)]:
        control.inputs['Line Width'].default_value = width
        control.inputs['ID Edge Width'].default_value = factor
        control.inputs['Depth Edge Width'].default_value = factor
        control.inputs['Normal Threshold Range'].default_value = .3 if case == 'varying' else 0
        for setting, scale in SCALES + [('FULL', 1)]:
            scene.eevee.outline_viewport_resolution = setting
            image(scene, (1597, 555))
            samples = []
            foreground_samples = 0
            started_at = time.time()
            for repeat in range(100):
                scene.camera.location.x = .0005 if repeat % 2 else -.0005
                bpy.context.view_layer.update()
                area.tag_redraw()
                start = time.perf_counter()
                bpy.ops.wm.redraw_timer(type='DRAW_WIN_SWAP', iterations=1)
                gpu.state.active_framebuffer_get().read_color(0, 0, 1, 1, 4, 0, 'FLOAT')
                elapsed = (time.perf_counter() - start) * 1000
                if repeat >= 10:
                    samples.append(elapsed)
                    foreground = ctypes.c_ulong()
                    ctypes.windll.user32.GetWindowThreadProcessId(
                        ctypes.windll.user32.GetForegroundWindow(), ctypes.byref(foreground))
                    foreground_samples += foreground.value == os.getpid()
            label = case + '_' + setting
            if label in REPORT['cases']:
                label += '_repeat'
            REPORT['cases'][label] = {'samples_ms': samples, 'median_ms': float(np.median(samples)),
                                     'p95_ms': float(np.percentile(samples, 95)),
                                     'started_at': started_at, 'ended_at': time.time(),
                                     'foreground_samples': foreground_samples}
            print('OUTLINE_RESOLUTION_BENCH', label, REPORT['cases'][label]['median_ms'], flush=True)


def run():
    clock_process = clock_file = None
    try:
        gpu.init()
        assert not bpy.app.background
        REPORT['backend'] = gpu.platform.backend_type_get()
        expected = os.environ.get('BLENDER_OUTLINE_EXPECT_BACKEND')
        if expected:
            assert REPORT['backend'] == expected, REPORT['backend']
        REPORT['renderer'] = gpu.platform.renderer_get()
        REPORT['binary'] = bpy.app.binary_path
        REPORT['build_hash'] = bpy.app.build_hash.decode()
        window, area, region, space = viewport()
        with bpy.context.temp_override(window=window, area=area, region=region):
            if MODE == 'bench':
                clock_file = (OUTPUT / 'gpu_clocks.csv').open('w', encoding='utf-8')
                clock_process = subprocess.Popen([r'C:\Windows\System32\nvidia-smi.exe',
                    '--query-gpu=timestamp,pstate,clocks.sm,clocks.mem,utilization.gpu,power.draw',
                    '--format=csv,noheader,nounits', '-lms', '250'], stdout=clock_file,
                    stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
                benchmark()
            else:
                pixel_cases()
        REPORT['passed'] = True
        print('OUTLINE_VIEWPORT_RESOLUTION_OK', REPORT['backend'], MODE, flush=True)
    except Exception:
        REPORT['error'] = traceback.format_exc()
        print(REPORT['error'], flush=True)
    finally:
        if clock_process:
            clock_process.terminate()
            clock_process.wait(timeout=10)
            clock_file.close()
        (OUTPUT / 'summary.json').write_text(json.dumps(REPORT, indent=2), encoding='utf-8')
        assert not bpy.data.filepath or Path(bpy.data.filepath).parent == ASSETS
        recovery = OUTPUT / 'recovery'
        recovery.mkdir(exist_ok=True)
        bpy.context.preferences.filepaths.temporary_directory = str(recovery)
        # Let the test callback release its temporary RNA/GPU references before shutdown.
        bpy.app.timers.register(quit_after_test, first_interval=.1)


def quit_after_test():
    bpy.ops.wm.quit_blender()


if __name__ == '__main__':
    bpy.context.preferences.view.show_splash = False
    bpy.app.timers.register(run, first_interval=.3)
