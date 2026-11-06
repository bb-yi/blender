"""Run with --profile-gpu and an output directory as cwd; GPU timings are in profile.json."""
import json
from pathlib import Path
import sys
import time

import bpy
import gpu

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_stencil_value import arguments, configure, material, marker_material, math_node, plane

args = arguments()
gpu.init()
backend = gpu.platform.backend_type_get().lower()
out = args.output_dir or Path.cwd()
out.mkdir(parents=True, exist_ok=True)
scene = configure()
width, height = args.benchmark_width or 1920, args.benchmark_height or 1080
scene.render.resolution_x, scene.render.resolution_y = width, height
variant = args.benchmark_variant
scene.eevee.stencil_surface_resolution = {'half': 'HALF', 'quarter': 'QUARTER'}.get(variant, 'FULL')
writer_mat = marker_material('BenchmarkWriter', reference=1)
verts, faces = [], []
for strand in range(32):
    base = len(verts)
    for step in range(17):
        x = -1.5 + strand * .09375 + .03 * (step / 16) ** 2
        y = -1.7 + step * 3.4 / 16
        verts.extend([(x, y, .3), (x + .04, y, .3)])
    for step in range(16):
        a = base + step * 2
        faces.append((a, a + 1, a + 3, a + 2))
mesh = bpy.data.meshes.new('Benchmark hair strips')
mesh.from_pydata(verts, [], faces)
caster = bpy.data.objects.new(mesh.name, mesh)
scene.collection.objects.link(caster)
mesh.materials.append(writer_mat)
reader_mat = material('BenchmarkReceiver')
tree = reader_mat.node_tree
sample = tree.nodes.new('ShaderNodeStencilValue')
sample.source = 'STENCIL_SNAPSHOT' if variant == 'disabled' else 'WRITER_SURFACE'
value = math_node(tree, 'DIVIDE', sample.outputs['Value'], 15)
if variant != 'disabled':
    depth = math_node(tree, 'DIVIDE', sample.outputs['Depth'], 10)
    value = math_node(tree, 'ADD', value, depth)
tree.links.new(value, tree.nodes.get('Emission').inputs['Color'])
plane('Benchmark receiver', reader_mat)
times = []
for index in range(15):
    start = time.perf_counter()
    bpy.ops.render.render()
    elapsed = time.perf_counter() - start
    if index >= 3:
        times.append(elapsed)
divisor = {'FULL': 1, 'HALF': 2, 'QUARTER': 4}[scene.eevee.stencil_surface_resolution]
extent = [(width + divisor - 1) // divisor, (height + divisor - 1) // divisor]
report = {'build': bpy.app.build_hash.decode(), 'backend': backend, 'variant': variant,
          'renderer': gpu.platform.renderer_get(), 'vendor': gpu.platform.vendor_get(),
          'resolution': [width, height], 'writer_draw_batches': 1, 'writer_triangles': 1024,
          'capture_extent': extent if variant != 'disabled' else [0, 0],
          'logical_buffer_bytes': extent[0] * extent[1] * 5 if variant != 'disabled' else 0,
          'warm_render_seconds': times,
          'gpu_timing_source': ('GPU timestamp debug groups, profile.json pid=1; dur is microseconds'
                               if backend == 'opengl' else
                               'CPU submission scopes only; GPU timing is unavailable')}
(out / 'benchmark.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print('STENCIL_WRITER_BENCHMARK_COMPLETE', json.dumps(report), flush=True)
