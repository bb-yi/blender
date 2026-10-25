"""Real viewport compilation: queued/failed markers write nothing and recover in the next draw."""

import json
import os
import subprocess
import sys
import time
import traceback
from pathlib import Path

import bpy
import gpu
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_stencil_value import arguments, configure, marker_material, material, math_node, plane
from test_stencil_value_regressions import marker_geometry

args = arguments()
gpu.init()
backend = gpu.platform.backend_type_get().lower()
out = args.output_dir or Path(__file__).resolve().parents[4] / 'temp/npr_stencil_read' / ('gpu_' + backend) / 'async'
out.mkdir(parents=True, exist_ok=True)

if bpy.app.background:
    binary = Path(bpy.app.binary_path).resolve()
    assert 'release' not in [part.lower() for part in binary.parts], binary
    # Create only the isolated test preference file, so the first-run setup does not cover the
    # viewport. Never let this test write the real user configuration.
    sandbox_config = Path(os.environ['BLENDER_USER_CONFIG']).resolve()
    assert sandbox_config.is_relative_to(Path(__file__).resolve().parents[4]), sandbox_config
    bpy.context.preferences.view.show_splash = False
    bpy.ops.wm.save_userpref()
    environment = os.environ.copy()
    temporary = out / 'process_temp'
    temporary.mkdir(parents=True, exist_ok=True)
    environment['TEMP'] = environment['TMP'] = str(temporary)
    command = [str(binary), '--factory-startup', '--enable-event-simulate', '--gpu-backend', backend,
               '--python-exit-code', '1', '--window-geometry', '0', '0', '640', '480',
               '--python', str(Path(__file__).resolve()), '--', '--phase', 'async', '--output-dir', str(out),
               '--geometry', args.geometry]
    with (out / 'window.log').open('w', encoding='utf-8') as log:
        process = subprocess.Popen(command, env=environment, stdout=log, stderr=subprocess.STDOUT)
        try:
            process.wait(timeout=150)
        except subprocess.TimeoutExpired:
            # The window normally exits through its own bounded timer. A stuck GUI process must
            # be reported for inspection, not externally terminated by a background test runner.
            raise AssertionError(f'Isolated stencil viewport timed out; inspect PID {process.pid}, '
                                 f'executable {binary}, log {out / "window.log"}') from None
    assert process.returncode == 0, (process.returncode, (out / 'window.log').read_text()[-8000:])
    result = json.loads((out / 'verification.json').read_text())
    assert result['passed'], result
    assert any(r.get('status') == 'QUEUED' for r in result['records']), result
    print('STENCIL_VALUE_ASYNC_GPU_TEST_PASSED', flush=True)
else:
    bpy.context.preferences.view.show_splash = False
    scene = configure(reset=False)
    scene.eevee.taa_samples = 1
    scene.eevee.use_taa_reprojection = False
    marker_mat = marker_material('AsyncMarker')
    tree = marker_mat.node_tree
    expression = tree.nodes.new('ShaderNodeScriptExpression')
    expression.expression = '1.0'
    transparent = tree.nodes.new('ShaderNodeBsdfTransparent')
    mix = tree.nodes.new('ShaderNodeMixShader')
    tree.links.new(expression.outputs[0], mix.inputs[0])
    tree.links.new(transparent.outputs[0], mix.inputs[1])
    tree.links.new(tree.nodes.get('Emission').outputs[0], mix.inputs[2])
    tree.links.new(mix.outputs[0], tree.nodes.get('Material Output').inputs['Surface'])
    marker_geometry(args.geometry, marker_mat)
    reader = material('AsyncReader', (0, 0, 0, 1))
    rt = reader.node_tree
    stencil = rt.nodes.new('ShaderNodeStencilValue')
    mask = math_node(rt, 'GREATER_THAN', stencil.outputs['Value'], 0.5)
    blue = rt.nodes.new('ShaderNodeCombineXYZ')
    rt.links.new(mask, blue.inputs['Z'])
    rt.links.new(blue.outputs[0], rt.nodes.get('Emission').inputs['Color'])
    plane('Reader', reader)
    window = bpy.context.window
    area = next(a for a in window.screen.areas if a.type == 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    space.shading.type = 'RENDERED'
    space.shading.use_scene_world_render = True
    space.shading.use_scene_lights_render = True
    space.overlay.show_overlays = False
    space.region_3d.view_perspective = 'CAMERA'
    latest_pixel = None
    state = 'warmup'
    deadline = time.monotonic() + 120
    records = []
    stable = 0
    warmup_draws = 0

    def dismiss_intro():
        window.event_simulate(type='ESC', value='PRESS')
        window.event_simulate(type='ESC', value='RELEASE')
        return None

    def capture_pixel():
        global latest_pixel
        # POST_PIXEL can be the transparent overlay framebuffer. Read the displayed window after
        # the real viewport draw instead; this does not invoke a synchronous offscreen render.
        path = out / 'displayed_frame.png'
        with bpy.context.temp_override(window=window):
            bpy.ops.screen.screenshot(filepath=str(path))
        image = bpy.data.images.load(str(path), check_existing=False)
        width, height = image.size
        pixels = np.array(image.pixels[:], dtype=np.float32).reshape(height, width, 4)
        x = int((region.x + region.width / 2) * width / window.width)
        y = int((region.y + region.height / 2) * height / window.height)
        latest_pixel = pixels[y, x, :3].tolist()
        bpy.data.images.remove(image)

    def status():
        return marker_mat.evaluated_get(bpy.context.evaluated_depsgraph_get()).shader_compile_status

    def finish(passed, error=''):
        result = {'passed': passed, 'backend': backend, 'geometry': args.geometry,
                  'build_hash': bpy.app.build_hash.decode(),
                  'build_date_utc': bpy.app.build_date.decode() + ' ' + bpy.app.build_time.decode(),
                  'records': records, 'error': error}
        (out / 'verification.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        print('STENCIL_ASYNC_FINISHED', json.dumps(result), flush=True)
        if passed:
            bpy.ops.wm.quit_blender()
        else:
            os._exit(1)

    def redraw():
        with bpy.context.temp_override(window=window, area=area):
            bpy.ops.wm.redraw_timer(type='DRAW_WIN_SWAP', iterations=1)

    def tick():
        global state, stable, warmup_draws
        try:
            assert time.monotonic() < deadline, (state, status(), latest_pixel)
            before_draw = status()
            redraw()
            capture_pixel()
            current = status()
            if state == 'warmup':
                if current != 'COMPILED' or latest_pixel is None or latest_pixel[2] < 0.98:
                    warmup_draws += 1
                    if warmup_draws % 10 == 0:
                        print('STENCIL_ASYNC_WARMUP', current, latest_pixel, flush=True)
                    return 0.05
                expression.expression = 'missing_async_stencil_marker'
                redraw()
                current = status()
                assert current == 'QUEUED', ('Real deferred compilation required', current)
                capture_pixel()
                assert max(latest_pixel) < 0.02, ('Queued marker wrote a placeholder/stale mask', latest_pixel)
                records.append({'event': 'queued', 'status': current, 'pixel': latest_pixel})
                state = 'failure'
            elif state == 'failure':
                if current != 'FAILED':
                    return 0.05
                assert max(latest_pixel) < 0.02, latest_pixel
                records.append({'event': 'failed', 'status': current, 'pixel': latest_pixel,
                                'diagnostic': expression.error_message})
                marker_mat.use_stencil = False
                redraw()
                capture_pixel()
                assert max(latest_pixel) < 0.02, ('Disabled failed marker became visible', latest_pixel)
                records.append({'event': 'failed_disabled', 'status': status(), 'pixel': latest_pixel})
                marker_mat.use_stencil = True
                redraw()
                capture_pixel()
                assert max(latest_pixel) < 0.02, ('Failed marker wrote after re-enable', latest_pixel)
                expression.expression = '1.0'
                state = 'recover'
            elif state == 'recover':
                if before_draw != 'COMPILED':
                    return 0.05
                assert latest_pixel[2] > 0.98 and max(latest_pixel[:2]) < 0.02, latest_pixel
                records.append({'event': 'recovered', 'status': current, 'pixel': latest_pixel})
                state = 'stable'
            else:
                assert current == 'COMPILED' and latest_pixel[2] > 0.98, (current, latest_pixel)
                stable += 1
                if stable == 10:
                    finish(True)
                    return None
            area.tag_redraw()
            return 0.05
        except BaseException:
            finish(False, traceback.format_exc())
            return None

    bpy.app.timers.register(dismiss_intro, first_interval=0.1)
    bpy.app.timers.register(tick, first_interval=0.4)
