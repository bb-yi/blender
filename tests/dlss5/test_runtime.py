# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Validate the installed DLSSNR runtime and failure recovery in isolated processes.

Run with Blender --background --factory-startup --python-exit-code 1 --python <this file>.
DLSS5_TEST_OUT selects an artifact directory outside the source repository.
"""

import json
import os
from pathlib import Path
import subprocess

import bpy
import numpy as np


def child_test(mode, out):
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    scene.render.resolution_x = 128
    scene.render.resolution_y = 96
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = True
    scene.eevee.taa_render_samples = 4
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.render.image_settings.color_depth = '32'
    scene.render.image_settings.color_mode = 'RGBA'
    pixels = {}
    states = {}
    for label, setting, intensity in [('off', 'OFF', 1.0), ('on', 'DLSSNR', 1.0),
                                       ('off_again', 'OFF', 1.0), ('zero', 'DLSSNR', 0.0)]:
        scene.eevee.dlss5_mode = setting
        scene.eevee.dlss5_intensity = intensity
        scene.render.filepath = str(out / (label + '.exr'))
        bpy.ops.render.render(write_still=True)
        image = bpy.data.images.load(scene.render.filepath, check_existing=False)
        data = np.empty(len(image.pixels), dtype=np.float32)
        image.pixels.foreach_get(data)
        assert tuple(image.size) == (128, 96)
        data = data.reshape(96, 128, 4)
        assert np.isfinite(data).all(), 'Non-finite render pixels'
        pixels[label] = data.copy()
        states[label] = scene.eevee.dlss5_render_status
        bpy.data.images.remove(image)

    assert np.array_equal(pixels['off'], pixels['off_again']), 'OFF did not restore native output'
    assert np.array_equal(pixels['off'], pixels['zero']), 'Zero intensity did not bypass NR'
    assert np.array_equal(pixels['off'][:, :, 3], pixels['on'][:, :, 3]), 'Alpha changed'
    difference = float(np.abs(pixels['off'][:, :, :3] - pixels['on'][:, :, :3]).mean())
    if mode in ('normal', 'dll_directory_collision'):
        assert states['on'].startswith('Completed'), states['on']
        assert difference > 1e-5, 'NR produced no measurable effect'
    else:
        assert difference == 0.0, 'Failure did not preserve native output'
        expected = {
            'cache_error': 'path error',
            'missing_runtime': 'nvngx_dlssnr.dll',
            'sync_init': 'sync initialization failed',
            'output_signal': 'output signal failed',
            'completion_signal': 'completion signal failed',
        }[mode]
        assert expected.lower() in states['on'].lower(), states['on']
    result = {'mode': mode, 'passed': True, 'rgb_mean_difference': difference, 'states': states}
    (out / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print('DLSS5_RELEASE_RESULT ' + json.dumps(result), flush=True)


def parent_test():
    if not hasattr(bpy.context.scene.eevee, 'dlss5_mode'):
        print('DLSS5_NOT_IN_BUILD: case not applicable')
        return
    binary = Path(bpy.app.binary_path)
    runtime = binary.parent / 'dlss5'
    for name in ('nvngx_dlssnr.dll', 'nvngx.dll'):
        assert (runtime / name).is_file(), 'Missing packaged runtime: ' + str(runtime / name)
    default_out = Path(__file__).resolve().parents[3] / 'test/dlss5/out/runtime-release'
    out = Path(os.environ.get('DLSS5_TEST_OUT', str(default_out)))
    out.mkdir(parents=True, exist_ok=True)
    modes = ('normal', 'dll_directory_collision', 'cache_error', 'missing_runtime',
             'sync_init', 'output_signal', 'completion_signal')
    results = []
    for mode in modes:
        case_out = out / mode
        case_out.mkdir(parents=True, exist_ok=True)
        env = os.environ.copy()
        for name in ('DLSS5_RUNTIME_DIR', 'DLSS5_CORE_DLL', 'BLENDER_DLSS5_TEST_FAILURE'):
            env.pop(name, None)
        env['DLSS5_TEST_CHILD'] = mode
        env['DLSS5_TEST_OUT'] = str(case_out)
        env['BLENDER_USER_RESOURCES'] = str(case_out / 'user')
        env['DLSS5_CACHE_DIR'] = str(case_out / 'cache')
        env['TEMP'] = env['TMP'] = str(case_out / 'tmp')
        (case_out / 'tmp').mkdir(exist_ok=True)
        if mode == 'cache_error':
            blocked = case_out / 'cache-is-a-file'
            blocked.write_text('Intentional filesystem failure', encoding='utf-8')
            env['DLSS5_CACHE_DIR'] = str(blocked)
        elif mode == 'missing_runtime':
            empty = case_out / 'empty-runtime'
            empty.mkdir(exist_ok=True)
            env['DLSS5_RUNTIME_DIR'] = str(empty)
        elif mode == 'dll_directory_collision':
            isolated = case_out / 'runtime'
            isolated.mkdir(exist_ok=True)
            for name in ('nvngx_dlssnr.dll', 'nvngx.dll'):
                link = isolated / name
                if not link.exists():
                    os.link(runtime / name, link)
            (isolated / 'ngx-data').write_text('DLL directory must not store cache', encoding='utf-8')
            env['DLSS5_RUNTIME_DIR'] = str(isolated)
        elif mode in ('sync_init', 'output_signal', 'completion_signal'):
            env['BLENDER_DLSS5_TEST_FAILURE'] = mode
        command = [str(binary), '--background', '--factory-startup', '--gpu-backend', 'vulkan',
                   '--python-exit-code', '1', '--python', str(Path(__file__).resolve())]
        process = subprocess.run(command, env=env, capture_output=True, timeout=90)
        log = (process.stdout + process.stderr).decode('utf-8', errors='replace')
        (case_out / 'process.txt').write_text(log, encoding='utf-8')
        assert process.returncode == 0, f'{mode}: exit={process.returncode}\n{log[-2500:]}'
        assert 'DLSS5_RELEASE_RESULT ' in log, f'{mode}: result marker missing'
        result = json.loads((case_out / 'result.json').read_text(encoding='utf-8'))
        results.append(result)
        print(f'DLSS5_RELEASE_CASE {mode}: PASS', flush=True)
    (out / 'summary.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    print('DLSS5_RELEASE_RUNTIME_PASS', flush=True)


if __name__ == '__main__':
    if os.environ.get('DLSS5_TEST_CHILD'):
        child_test(os.environ['DLSS5_TEST_CHILD'], Path(os.environ['DLSS5_TEST_OUT']))
    else:
        parent_test()
