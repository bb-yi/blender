"""Run inside an unpacked Linux/macOS Blender in the release CI job."""
import json
import math
import os
from pathlib import Path
import re
import tempfile

import bpy

expected = os.environ['NPR_RELEASE_EXPECTED_COMMIT']
actual = bpy.app.build_hash.decode('ascii')
assert len(actual) >= 12 and expected.startswith(actual), (expected, actual)
source = Path(__file__).resolve().parents[2]
header = (source / 'source/blender/blenkernel/BKE_blender_version.h').read_text()
version = int(re.search(r'^#define BLENDER_VERSION\s+(\d+)\s*$', header, re.M)[1])
patch = int(re.search(r'^#define BLENDER_VERSION_PATCH\s+(\d+)\s*$', header, re.M)[1])
assert bpy.app.version[:3] == (version // 100, version % 100, patch)

material = bpy.data.materials.new('ReleaseParserSmoke')
material.use_nodes = True
tree = material.node_tree
function = tree.nodes.new('ShaderNodeGLSLFunction')
function.display_mode = 'CODE'
function.source_code = 'float release_smoke(float x) { return x * 0.5; }'
function.function_name = 'release_smoke'
function.display_mode = 'NODE'
assert function.parse_status == 'READY', function.parse_status
assert len(function.inputs) == len(function.outputs) == 1
tree.nodes.new('ShaderNodeStencilValue')
bundle = tree.nodes.new('NodeCombineBundle')
assert len(bundle.outputs) == 1 and bundle.outputs[0].type == 'BUNDLE'

scene = bpy.context.scene
assert scene.camera is not None
scene.render.engine = 'CYCLES'
scene.cycles.device = 'CPU'
scene.cycles.samples = 1
scene.cycles.use_denoising = False
scene.render.resolution_x = scene.render.resolution_y = 16
scene.render.resolution_percentage = 100
scene.render.image_settings.file_format = 'PNG'
with tempfile.TemporaryDirectory(prefix='npr-release-smoke-') as temporary:
    image_path = Path(temporary) / 'cycles.png'
    scene.render.filepath = str(image_path)
    bpy.ops.render.render(write_still=True)
    image = bpy.data.images.load(str(image_path))
    assert tuple(image.size) == (16, 16)
    values = list(image.pixels)
    assert len(values) == 16 * 16 * 4 and all(math.isfinite(v) for v in values)
    rgb = [value for index, value in enumerate(values) if index % 4 != 3]
    assert max(rgb) - min(rgb) > 0.01, (min(rgb), max(rgb))
    bpy.data.images.remove(image)
print('NPR_RELEASE_PROGRAM_SMOKE=' + json.dumps({
    'expected_commit': expected, 'build_hash': actual,
    'version': bpy.app.version_string, 'binary': bpy.app.binary_path,
    'parser': 'PASS', 'stencil_bundle_nodes': 'PASS',
    'cycles_cpu_16x16_finite_nonconstant': 'PASS',
    'gpu_pixel_tests': 'NOT_RUN_ON_THIS_CI_PLATFORM',
}), flush=True)
