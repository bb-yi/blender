# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Run in an isolated window, using actual deferred viewport compilation.

A headless invocation launches an isolated window child. Its timer exits on completion.
An independent material sharing the queued pass proves the old failure is finalized
after the edit; assertions reject a synchronous draw or a test that never observes QUEUED.
"""
import json
import os
import sys
import time
import traceback
from pathlib import Path

import bpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import OUT

if bpy.app.background:
    # The release runner is headless; use an isolated window child for this specific test.
    import subprocess
    binary = Path(bpy.app.binary_path).resolve()
    if 'release' in [part.lower() for part in binary.parts]:
        raise RuntimeError('Window child tests require an install/build/temp executable')
    temporary = OUT / 'async_temp'
    temporary.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment['BLENDER_TEST_OUTPUT_DIR'] = str(OUT)
    environment['TEMP'] = environment['TMP'] = str(temporary)
    command = [str(binary), '--factory-startup', '--python-exit-code', '1',
               '--window-geometry', '0', '0', '640', '480']
    if '--gpu-backend' in sys.argv:
        command += ['--gpu-backend', sys.argv[sys.argv.index('--gpu-backend') + 1]]
    command += ['--python', str(Path(__file__).resolve())]
    process = subprocess.Popen(command, env=environment, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace')
    try:
        output, _ = process.communicate(timeout=150)
    except subprocess.TimeoutExpired:
        if os.name == 'nt':
            query = (f'Get-CimInstance Win32_Process -Filter "ProcessId = {process.pid}" | '
                     'Select-Object ProcessId,ExecutablePath,CommandLine | ConvertTo-Json')
            print(subprocess.check_output(['pwsh', '-NoProfile', '-Command', query], text=True), flush=True)
        process.kill()
        output, _ = process.communicate()
        raise AssertionError('Async window test timed out: ' + output[-6000:])
    print(output, flush=True)
    assert process.returncode == 0, (process.returncode, output[-6000:])
    result = json.loads((OUT / 'async_results.json').read_text(encoding='utf-8'))
    assert result['success'], result
    assert 'SCRIPT_EXPRESSION_ASYNC_ERRORS_OK' in output, output[-6000:]
else:
    import gpu
    if '--gpu-backend' in sys.argv:
        requested_backend = sys.argv[sys.argv.index('--gpu-backend') + 1].upper()
        assert gpu.platform.backend_type_get() == requested_backend, gpu.platform.backend_type_get()
    sc = bpy.context.scene
    for obj in list(sc.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    sc.render.engine = 'BLENDER_EEVEE'
    sc.view_settings.view_transform = 'Standard'
    camera_data = bpy.data.cameras.new('Test camera')
    camera = bpy.data.objects.new('Test camera', camera_data)
    sc.collection.objects.link(camera)
    camera.location = (0, 0, 3)
    camera_data.type = 'ORTHO'
    camera_data.ortho_scale = 2
    sc.camera = camera
    mesh = bpy.data.meshes.new('Test plane')
    mesh.from_pydata([(-2, -2, 0), (2, -2, 0), (2, 2, 0), (-2, 2, 0)], [], [(0, 1, 2, 3)])
    target_object = bpy.data.objects.new('Test plane', mesh)
    sc.collection.objects.link(target_object)
    material = bpy.data.materials.new('Async expression')
    material.use_nodes = True
    mesh.materials.append(material)
    material.node_tree.nodes.clear()
    output = material.node_tree.nodes.new('ShaderNodeOutputMaterial')
    emission = material.node_tree.nodes.new('ShaderNodeEmission')
    material.node_tree.links.new(emission.outputs[0], output.inputs['Surface'])
    sc.eevee.taa_samples = 1
    sc.eevee.use_taa_reprojection = False
    sc.eevee.use_outline = False
    sc.eevee.use_shadows = False
    target_object.scale = (.2, .2, .2)
    target_object.location.x = -.5
    tree = material.node_tree
    node = tree.nodes.new('ShaderNodeScriptExpression')
    node.expression = '0.3'
    tree.links.new(node.outputs[0], emission.inputs['Color'])
    control_object = target_object.copy()
    control_object.data = target_object.data.copy()
    sc.collection.objects.link(control_object)
    control_object.location.x = .5
    control = material.copy()
    control_object.data.materials[0] = control
    control_node = control.node_tree.nodes[node.name]

    window = bpy.context.window
    area = next(a for a in window.screen.areas if a.type == 'VIEW_3D')
    space = area.spaces.active
    space.shading.type = 'RENDERED'
    space.shading.use_scene_world_render = True
    space.overlay.show_overlays = False
    space.region_3d.view_perspective = 'CAMERA'

    state = 'warmup'
    deadline = time.monotonic() + 120
    records = []
    stable_draws = 0
    stamp = None
    old_symbol = 'missing_async_before_edit'
    new_symbol = 'missing_async_after_edit'


    def status(mat):
        return mat.evaluated_get(bpy.context.evaluated_depsgraph_get()).shader_compile_status


    def finish(success, error=''):
        result = {'success': success, 'backend': gpu.platform.backend_type_get(),
                  'renderer': gpu.platform.renderer_get(),
                  'build_hash': bpy.app.build_hash.decode(),
                  'build_date_utc': bpy.app.build_date.decode() + ' ' + bpy.app.build_time.decode(),
                  'records': records, 'error': error}
        (OUT / 'async_results.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        print('SCRIPT_EXPRESSION_ASYNC_ERRORS_OK' if success else 'SCRIPT_EXPRESSION_ASYNC_ERRORS_FAILED',
              result, flush=True)
        if success:
            bpy.ops.wm.quit_blender()
        else:
            # A timer exception alone does not fail the process; exit only this isolated test process.
            os._exit(1)


    def tick():
        global state, stable_draws, stamp
        try:
            assert time.monotonic() < deadline, (state, status(material), node.error_message)
            # A normal window draw uses DRWContext::VIEWPORT, allowing deferred materials.
            with bpy.context.temp_override(window=window, area=area):
                bpy.ops.wm.redraw_timer(type='DRAW_WIN_SWAP', iterations=1)
            target_status = status(material)
            if state == 'warmup':
                if target_status != 'COMPILED' or status(control) != 'COMPILED':
                    return .05
                node.expression = old_symbol
                control_node.expression = old_symbol
                # Inspect and edit in the same callback, before an automatic redraw can finalize it.
                with bpy.context.temp_override(window=window, area=area):
                    bpy.ops.wm.redraw_timer(type='DRAW_WIN_SWAP', iterations=1)
                target_status = status(material)
                assert target_status == 'QUEUED', ('Synchronous path or failed async fixture', target_status)
                records.append({'event': 'before_edit', 'status': target_status,
                                'control_status': status(control)})
                node.expression = '0.4'
                assert node.error_message == '', node.error_message
                records.append({'event': 'edited', 'expression': node.expression})
                state = 'wait_old_failure'
            elif state == 'wait_old_failure':
                assert node.error_message == '', node.error_message
                if target_status != 'COMPILED' or status(control) != 'FAILED':
                    return .05
                if old_symbol not in control_node.error_message:
                    return .05
                records.append({'event': 'old_failure_after_edit', 'status': status(control),
                                'error': control_node.error_message, 'target_error': node.error_message})
                control_node.expression = '0.4'
                node.expression = new_symbol
                state = 'wait_new_failure'
            elif state == 'wait_new_failure':
                if target_status != 'FAILED' or new_symbol not in node.error_message:
                    return .05
                assert old_symbol not in node.error_message, node.error_message
                assert control_node.error_message == '', control_node.error_message
                records.append({'event': 'new_failure', 'error': node.error_message})
                node.expression = '0.5'
                state = 'recover'
            elif state == 'recover':
                assert node.error_message == '', node.error_message
                if target_status != 'COMPILED':
                    return .05
                stamp = material.evaluated_get(bpy.context.evaluated_depsgraph_get()).shader_compile_timestamp
                stable_draws = 0
                state = 'stable'
            elif state == 'stable':
                assert node.error_message == '' and control_node.error_message == ''
                after = material.evaluated_get(bpy.context.evaluated_depsgraph_get()).shader_compile_timestamp
                assert after == stamp and stamp > 0, (stamp, after)
                stable_draws += 1
                if stable_draws == 12:
                    records.append({'event': 'stable', 'draws': stable_draws, 'timestamp': stamp})
                    finish(True)
                    return None
            area.tag_redraw()
            return .05
        except BaseException:
            finish(False, traceback.format_exc())
            return None


    bpy.app.timers.register(tick, first_interval=.2)
