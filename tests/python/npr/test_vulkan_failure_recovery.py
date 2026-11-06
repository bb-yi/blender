# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise real Vulkan submission/readback paths with failures, without resetting the GPU."""
import ctypes
import json
import os
from pathlib import Path
import platform
import subprocess
import sys

import bpy


def child(mode, out):
    if mode == 'null_execute':
        # A native thread bypasses ctypes' exception translation. Only this isolated child dies.
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.SetErrorMode(0x0002)
        kernel.CreateThread.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                                        ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)]
        kernel.CreateThread.restype = ctypes.c_void_p
        kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        thread_id = ctypes.c_uint32()
        handle = kernel.CreateThread(None, 0, None, None, 0, ctypes.byref(thread_id))
        assert handle, ctypes.get_last_error()
        kernel.WaitForSingleObject(handle, 20000)
        raise AssertionError('Null native entry point did not terminate the child')

    for path in out.glob('failed_frame_*.png'):
        path.unlink()
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    scene.render.resolution_x = 128
    scene.render.resolution_y = 96
    scene.render.resolution_percentage = 100
    scene.eevee.taa_render_samples = 4
    scene.eevee.dlss5_mode = 'OFF'
    scene.render.use_compositing = False
    scene.render.use_sequencer = False
    scene.render.filepath = str(out / 'failed_frame_')
    scene.render.image_settings.file_format = 'PNG'
    scene.frame_start, scene.frame_end = 1, 3
    bpy.ops.render.render()

    import gpu
    assert gpu.platform.backend_type_get() == 'VULKAN'
    if mode == 'external_signal':
        runtime = Path(os.environ.get('DLSS5_RUNTIME_DIR', str(Path(bpy.app.binary_path).parent / 'dlss5')))
        if 'NVIDIA' not in gpu.platform.vendor_get().upper() or not (runtime / 'nvngx_dlssnr.dll').is_file():
            print('VULKAN_FAILURE_CASE_SKIP external_signal: NVIDIA DLSSNR runtime required', flush=True)
            return
    import numpy as np
    data = gpu.types.Buffer('FLOAT', 16, [0.25, 0.5, 0.75, 1.0] * 4)
    texture = gpu.types.GPUTexture((2, 2), format='RGBA16F', data=data)
    initial = np.asarray(texture.read()).copy()
    assert np.max(initial) == 1.0 and np.min(initial) == 0.25

    written = []
    canceled = []
    bpy.app.handlers.render_write.append(lambda s, *_: written.append(s.frame_current))
    bpy.app.handlers.render_cancel.append(lambda s, *_: canceled.append(s.frame_current))
    if mode == 'normal':
        bpy.ops.render.render(animation=True)
        assert written == [1, 2, 3], written
        assert len(list(out.glob('failed_frame_*.png'))) == 3
    else:
        if mode == 'compositor_wait':
            scene.render.use_compositing = True
            scene.render.compositor_device = 'GPU'
            tree = bpy.data.node_groups.new('FailureCompositor', 'CompositorNodeTree')
            scene.compositing_node_group = tree
            layers = tree.nodes.new('CompositorNodeRLayers')
            output = tree.nodes.new('CompositorNodeOutputFile')
            output.directory = str(out / 'compositor')
            output.file_name = 'must_not_write'
            output.format.file_format = 'OPEN_EXR_MULTILAYER'
            output.file_output_items.clear()
            output.file_output_items.new('RGBA', 'Image')
            tree.links.new(layers.outputs['Image'], output.inputs['Image'])

            def fail_in_compositor(message):
                if 'Compositing' in message:
                    os.environ['BLENDER_VULKAN_TEST_FAILURE'] = 'wait'
            bpy.app.handlers.render_stats.append(fail_in_compositor)
        else:
            os.environ['BLENDER_VULKAN_TEST_FAILURE'] = mode
        if mode == 'wait':
            # Failed readback must not expose old GPU contents (or uninitialized host data).
            failed = np.asarray(texture.read()).copy()
            assert np.array_equal(failed, np.zeros_like(failed)), failed
        if mode == 'external_signal':
            scene.eevee.dlss5_mode = 'DLSSNR'
        try:
            bpy.ops.render.render(animation=True)
        except RuntimeError as error:
            assert 'Vulkan GPU operation failed' in str(error), str(error)
        assert not written, written
        assert canceled, 'Failed animation was not canceled'
        assert not list(out.glob('failed_frame_*')), 'Failed animation wrote output'
        assert not list((out / 'compositor').glob('*.exr')), 'GPU failure wrote compositor output'
        os.environ.pop('BLENDER_VULKAN_TEST_FAILURE')
        # The failure is persistent. Removing the test trigger must not resume the broken device.
        try:
            bpy.ops.render.render(animation=True)
        except RuntimeError as error:
            assert 'Vulkan GPU operation failed' in str(error), str(error)
        assert not written
    (out / 'result.json').write_text(json.dumps({'mode': mode, 'written': written,
                                                'canceled': canceled, 'passed': True}), encoding='utf-8')
    print('VULKAN_FAILURE_CASE_PASS', mode, flush=True)


def parent():
    out = Path(os.environ['NPR_FAILURE_TEST_OUT']).resolve()
    out.mkdir(parents=True, exist_ok=True)
    binary = Path(bpy.app.binary_path).resolve()
    if 'release' in [part.lower() for part in binary.parts]:
        raise RuntimeError('Failure injection requires an install/build/temp executable')
    modes = ['normal', 'submit', 'wait', 'counter', 'counter_max', 'compositor_wait']
    if sys.platform == 'win32':
        modes.append('external_signal')
        if platform.machine().lower() in ('amd64', 'x86_64'):
            modes.append('null_execute')
    selected = os.environ.get('NPR_FAILURE_TEST_MODES')
    if selected:
        requested = selected.split(',')
        assert set(requested).issubset(modes)
        modes = requested
    results = []
    for mode in modes:
        case = out / mode
        case.mkdir(parents=True, exist_ok=True)
        env = os.environ.copy()
        env.pop('BLENDER_VULKAN_TEST_FAILURE', None)
        env['NPR_FAILURE_TEST_CHILD'] = mode
        env['NPR_FAILURE_TEST_OUT'] = str(case)
        for name, subdir in [('BLENDER_USER_RESOURCES', 'user'), ('BLENDER_USER_CONFIG', 'user/config'),
                             ('BLENDER_USER_SCRIPTS', 'user/scripts'), ('BLENDER_USER_DATAFILES', 'user/datafiles'),
                             ('BLENDER_USER_EXTENSIONS', 'user/extensions'), ('TEMP', 'tmp'), ('TMP', 'tmp'),
                             ('DLSS5_CACHE_DIR', 'ngx-cache')]:
            path = case / subdir
            path.mkdir(parents=True, exist_ok=True)
            env[name] = str(path)
        command = [bpy.app.binary_path, '--background', '--factory-startup', '--disable-autoexec',
                   '--gpu-backend', 'vulkan', '--python-exit-code', '1', '--python', str(Path(__file__).resolve())]
        with (case / 'console.log').open('wb') as logfile:
            process = subprocess.Popen(command, env=env, stdout=logfile, stderr=logfile)
            try:
                process.wait(timeout=120)
            except subprocess.TimeoutExpired:
                if sys.platform == 'win32':
                    query = (f'Get-CimInstance Win32_Process -Filter "ProcessId = {process.pid}" | '
                             'Select-Object ProcessId,ExecutablePath,CommandLine | ConvertTo-Json')
                    details = subprocess.check_output(['pwsh', '-NoProfile', '-Command', query],
                                                      text=True, encoding='utf-8')
                    print(details, flush=True)
                    info = json.loads(details)
                    assert Path(info['ExecutablePath']).resolve() == binary, details
                process.kill()
                process.wait()
                raise AssertionError(f'{mode}: isolated child {process.pid} timed out')
        log = (case / 'console.log').read_text(encoding='utf-8', errors='replace')
        if mode == 'external_signal' and 'VULKAN_FAILURE_CASE_SKIP external_signal:' in log:
            assert process.returncode == 0, log[-1500:]
            results.append({'mode': mode, 'skipped': True, 'reason': 'NVIDIA DLSSNR runtime required'})
            print('VULKAN_FAILURE_REGRESSION', mode, 'SKIP', flush=True)
            continue
        if mode == 'null_execute':
            assert process.returncode != 0
            paths = list((case / 'tmp').rglob('*.crash.txt'))
            assert len(paths) == 1, (paths, log[-1500:])
            crash = paths[0].read_text(encoding='utf-8', errors='replace')
            assert 'EXCEPTION_ACCESS_VIOLATION' in crash
            assert 'Exception context: RIP=0x0000000000000000' in crash
            assert 'Null execution return-address candidate:' in crash
            stack = crash.split('Stack trace:', 1)[1].split('Threads:', 1)[0]
            assert 'windows_exception_handler' not in stack, stack
        else:
            # Blender returns 1 for a reported background render failure even when Python catches
            # the operator exception. The success marker below distinguishes it from a test error.
            assert process.returncode == (0 if mode == 'normal' else 1), (mode, process.returncode, log[-4500:])
            assert 'VULKAN_FAILURE_CASE_PASS ' + mode in log, (mode, log[-4500:])
            failures = log.count('Vulkan GPU failure:')
            assert failures == (0 if mode == 'normal' else 1), (mode, failures)
            if mode == 'external_signal':
                assert 'Failed submission:' in log and 'signal=1' in log, log[-4500:]
            assert 'EXCEPTION_ACCESS_VIOLATION' not in log
        results.append({'mode': mode, 'passed': True, 'exit_code': process.returncode})
        print('VULKAN_FAILURE_REGRESSION', mode, 'PASS', flush=True)
    (out / 'summary.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    print('VULKAN_FAILURE_REGRESSION_PASS', flush=True)


if __name__ == '__main__':
    mode = os.environ.get('NPR_FAILURE_TEST_CHILD')
    if mode:
        child(mode, Path(os.environ['NPR_FAILURE_TEST_OUT']))
    else:
        parent()
