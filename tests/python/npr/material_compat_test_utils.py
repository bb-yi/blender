# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Shared isolated-scene and linear-pixel helpers for material compatibility regressions.

Set BLENDER_TEST_OUTPUT_DIR to retain output in a chosen test directory. Tests must be
launched with isolated Blender user resources, including subprocesses which inherit them.
"""

import os
from pathlib import Path
import subprocess
import sys
import tempfile

import bpy
import numpy as np

_output_dir = os.environ.get("BLENDER_TEST_OUTPUT_DIR")
OUT = Path(_output_dir) if _output_dir else Path(tempfile.mkdtemp(prefix="blender-material-tests-"))
OUT.mkdir(parents=True, exist_ok=True)


def scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    scene.render.resolution_x, scene.render.resolution_y = 64, 48
    scene.render.resolution_percentage = 100
    scene.eevee.taa_render_samples = 1
    scene.view_settings.view_transform = 'Standard'
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.render.image_settings.color_depth = '32'
    bpy.ops.object.camera_add(location=(0, 0, 3))
    scene.camera = bpy.context.object
    scene.camera.data.type = 'ORTHO'
    scene.camera.data.ortho_scale = 2
    bpy.ops.mesh.primitive_plane_add(size=4)
    material = bpy.data.materials.new('Probe')
    material.use_nodes = True
    bpy.context.object.data.materials.append(material)
    material.node_tree.nodes.clear()
    output = material.node_tree.nodes.new('ShaderNodeOutputMaterial')
    emission = material.node_tree.nodes.new('ShaderNodeEmission')
    material.node_tree.links.new(emission.outputs[0], output.inputs['Surface'])
    return scene, material, emission


def glsl(tree, source, function):
    node = tree.nodes.new('ShaderNodeGLSLFunction')
    text = bpy.data.texts.new(function + '.glsl')
    text.write(source)
    node.source_mode = 'INTERNAL'
    node.script = text
    node.function_name = function
    tree.update_tag()
    tree.interface_update(bpy.context)
    bpy.context.view_layer.update()
    return node


def socket(sockets, key):
    return next(socket for socket in sockets if socket.identifier == key or socket.name == key)


def render(label):
    scene = bpy.context.scene
    scene.render.filepath = str(OUT / (label + '.exr'))
    bpy.ops.render.render(write_still=True)
    image = bpy.data.images.load(scene.render.filepath, check_existing=False)
    pixels = np.array(image.pixels[:]).reshape(image.size[1], image.size[0], 4)
    bpy.data.images.remove(image)
    assert np.isfinite(pixels).all(), label
    return pixels


def center(pixels):
    return pixels[pixels.shape[0] // 2, pixels.shape[1] // 2, :3]


def near(actual, expected, tolerance=.005):
    assert np.max(np.abs(np.array(actual) - expected)) < tolerance, (actual, expected)


def background_child(script, arguments=(), extra_options=()):
    """Run a fresh background process, preserving the parent's isolated user environment."""
    binary = Path(bpy.app.binary_path).resolve()
    if 'release' in [part.lower() for part in binary.parts]:
        raise RuntimeError('Child regression processes require an install/build/temp executable')
    command = [str(binary), '--background', '--factory-startup', '--python-exit-code', '1']
    if '--gpu-backend' in sys.argv:
        command += ['--gpu-backend', sys.argv[sys.argv.index('--gpu-backend') + 1]]
    command += list(extra_options) + ['--python', str(script), '--', *arguments]
    environment = os.environ.copy()
    environment['BLENDER_TEST_OUTPUT_DIR'] = str(OUT)
    process = subprocess.Popen(command, cwd=OUT, env=environment, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace')
    try:
        output, _ = process.communicate(timeout=180)
    except subprocess.TimeoutExpired:
        if os.name == 'nt':
            query = (f'Get-CimInstance Win32_Process -Filter "ProcessId = {process.pid}" | '
                     'Select-Object ProcessId,ExecutablePath,CommandLine | ConvertTo-Json')
            print(subprocess.check_output(['pwsh', '-NoProfile', '-Command', query], text=True), flush=True)
        process.kill()
        output, _ = process.communicate()
        raise AssertionError(f'Background child timed out: {command}\n{output[-4000:]}')
    assert process.returncode == 0, (command, process.returncode, output[-6000:])
    return output
