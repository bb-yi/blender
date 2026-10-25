"""Commit-gate regressions for stencil UVs, texture bindings and Cycles fallback."""

import argparse
import json
from pathlib import Path
import sys
import traceback

import bpy
import gpu
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_stencil_value as core


def compare_images(actual, expected):
    core.near([float(np.max(np.abs(actual - expected)))], [0], tolerance=0.00001)


def capture_uv(out, report):
    scene = core.configure()
    scene.render.resolution_x, scene.render.resolution_y = 383, 257
    display_mat = core.material('Capture Display', (0, 0, 0, 1))
    core.plane('Capture Display', display_mat)
    dt = display_mat.node_tree
    texture = dt.nodes.new('ShaderNodeRenderTexture')
    coordinates = dt.nodes.new('ShaderNodeTexCoord')
    dt.links.new(coordinates.outputs['UV'], texture.inputs['Vector'])
    dt.links.new(texture.outputs['Color'], dt.nodes['Emission'].inputs['Color'])

    capture_camera = bpy.data.objects.new('Capture Camera', scene.camera.data.copy())
    capture_camera.location = (8, 0, 5)
    scene.collection.objects.link(capture_camera)
    capture_mat = core.reader_material('Capture Reader', raw=True)
    core.plane('Capture Receiver', capture_mat, x=8)
    core.plane('Capture Marker', core.marker_material(reference=4), size=1.8, x=8.25, z=0.3)
    core.plane('Capture Stripe', core.marker_material('Capture Stripe', reference=6, order=2),
               size=0.7, x=8.8, z=0.4)
    capture = scene.eevee.render_textures.add()
    capture.name = 'UV identity capture'
    capture.enabled = True
    capture.camera = capture_camera
    capture.source = 'COLOR'
    capture.format = 'RGBA32F'
    capture.update_mode = 'EVERY_FRAME'
    texture.render_texture = f'RENDER_TEXTURE_{capture.uid}'

    # A different value read in the main view catches stale capture snapshots after pool release.
    core.plane('Main Marker', core.marker_material('Main Marker', reference=11), size=3.8, z=0.3)
    inset_mat = core.reader_material('Main View Inset', raw=True)
    inset_mat.use_depth_write = False
    inset_mat.ztest_mode = 'ALWAYS'
    core.plane('Main View Inset', inset_mat, size=0.35, x=1.5, z=0.5)
    tree = capture_mat.node_tree
    value = tree.nodes['Stencil Value']
    info = tree.nodes.new('ShaderNodeRenderInfo')
    for index, (width, height, crop, overscan) in enumerate((
            (79, 53, False, False), (157, 41, False, True),
            (53, 127, True, True), (193, 37, True, False), (79, 53, False, False))):
        capture.resolution_x, capture.resolution_y = width, height
        scene.render.use_border = scene.render.use_crop_to_border = crop
        scene.render.border_min_x, scene.render.border_max_x = 0.17, 0.91
        scene.render.border_min_y, scene.render.border_max_y = 0.23, 0.87
        scene.eevee.use_overscan = overscan
        scene.eevee.overscan_size = 12
        for link in list(value.inputs['UV'].links):
            tree.links.remove(link)
        reference = core.render(f'capture_uv_{index}_default', out, report)
        # The displayed capture must contain nonzero labels, independently of the main inset.
        assert np.count_nonzero((reference[..., 0] > 0.20) & (reference[..., 0] < 0.50)) > 100
        assert np.count_nonzero(np.abs(reference[..., 0] - 11 / 15) < 0.002) > 100
        tree.links.new(info.outputs['Frag Coord'], value.inputs['UV'])
        actual = core.render(f'capture_uv_{index}_connected', out, report)
        compare_images(actual, reference)
        report['cases'][f'capture_uv_{index}_connected']['capture_extent'] = [width, height]
        report['cases'][f'capture_uv_{index}_connected']['crop'] = crop
        report['cases'][f'capture_uv_{index}_connected']['overscan'] = overscan

    offset = tree.nodes.new('ShaderNodeVectorMath')
    offset.operation = 'ADD'
    offset.inputs[1].default_value = (100, 0, 0)
    tree.links.new(info.outputs['Frag Coord'], offset.inputs[0])
    tree.links.new(offset.outputs[0], value.inputs['UV'])
    pixels = core.render('capture_uv_actual_texture_bounds', out, report)
    assert np.count_nonzero((pixels[..., 0] > 0.20) & (pixels[..., 0] < 0.50)) == 0
    assert np.count_nonzero(np.abs(pixels[..., 0] - 11 / 15) < 0.002) > 100


def sampler_material(count):
    mat = core.reader_material('Texture and Stencil', raw=True)
    tree = mat.node_tree
    value = tree.nodes['Stencil Value']
    result = core.math_node(tree, 'MULTIPLY', value.outputs[0], 0.25 / 15)
    average = 0
    for index in range(count):
        color = (index + 1) * 0.4 / (count + 2)
        average += color / count
        image = bpy.data.images.new('Unique Image ' + str(index), width=2, height=2, float_buffer=True)
        image.colorspace_settings.name = 'Non-Color'
        image.pixels.foreach_set([color, color, color, 1] * 4)
        image.update()
        texture = tree.nodes.new('ShaderNodeTexImage')
        texture.image = image
        term = core.math_node(tree, 'MULTIPLY', texture.outputs['Color'], 1 / count)
        result = core.math_node(tree, 'ADD', result, term)
    tree.links.new(result, tree.nodes['Emission'].inputs['Color'])
    return mat, average


def samplers(out, report):
    for method in ('DITHERED', 'BLENDED'):
        for count in (1, 4, 12):
            core.configure()
            mat, average = sampler_material(count)
            mat.surface_render_method = method
            core.plane('Receiver', mat)
            core.plane('Marker', core.marker_material(reference=9), size=2.4, z=0.3)
            pixels = core.render(f'samplers_{method}_{count}', out, report)
            core.near(core.sample(pixels)[0], average + 9 * 0.25 / 15, tolerance=0.003)
            core.near(core.sample(pixels, 1.5)[0], average, tolerance=0.003)

    # Equal shader graphs can share a GPUPass, but each material still needs its own snapshot bind.
    core.configure()
    first, average = sampler_material(1)
    second = first.copy()
    left = core.plane('Original Reader', first, size=1.5, x=-0.9)
    core.plane('Cached Reader', second, size=1.5, x=0.9)
    marker_mat = core.marker_material(reference=9)
    core.plane('Marker', marker_mat, size=1.5, x=0.9, z=0.3)
    pixels = core.render('cached_pass_distinct_materials', out, report)
    core.near(core.sample(pixels, -0.9)[0], average, tolerance=0.003)
    core.near(core.sample(pixels, 0.9)[0], average + 0.15, tolerance=0.003)
    left.hide_render = True
    marker_mat.stencil_reference = 3
    pixels = core.render('cached_pass_only_reader_updates', out, report)
    core.near(core.sample(pixels, 0.9)[0], average + 0.05, tolerance=0.003)

    core.configure()
    mat, _ = sampler_material(80)
    receiver = core.plane('Overflow Receiver', mat)
    core.plane('Marker', core.marker_material(reference=9), size=2.4, z=0.3)
    pixels = core.render('sampler_exhaustion_is_rejected', out, report)
    core.near(core.sample(pixels)[0], [1, 0, 1])
    receiver.data.materials[0], average = sampler_material(1)
    pixels = core.render('sampler_exhaustion_recovers', out, report)
    core.near(core.sample(pixels)[0], average + 9 * 0.25 / 15, tolerance=0.003)


def cycles(out, report):
    scene = core.configure()
    assert hasattr(scene, 'cycles'), 'Requires the registered with-cycles build'
    scene.render.engine = 'CYCLES'
    scene.cycles.device = 'CPU'
    scene.cycles.samples = 1
    scene.cycles.use_denoising = False
    scene.render.threads_mode = 'FIXED'
    scene.render.threads = 4
    mat = core.material('Cycles Zero Fallback')
    tree = mat.node_tree
    value = tree.nodes.new('ShaderNodeStencilValue')
    signal = core.math_node(tree, 'ADD', value.outputs[0], 0.25)
    tree.links.new(signal, tree.nodes['Emission'].inputs['Color'])
    core.plane('Cycles Receiver', mat)
    pixels = core.render('cycles_stencil_value_is_zero', out, report)
    core.near(core.sample(pixels)[0], 0.25)


def bundle_integration(out, report):
    core.configure()
    mat = core.material('Bundle Texture Stencil')
    tree = mat.node_tree
    code = bpy.data.texts.new('Stencil Data Integration')
    code.write('struct Data { float mask; };\n'
               'void apply_mark(Data data, sampler2D image, out vec3 color) {\n'
               '  color = texture(image, vec2(0.5)).rgb + vec3(data.mask / 60.0);\n}\n')
    function = tree.nodes.new('ShaderNodeGLSLFunction')
    function.source_mode = 'INTERNAL'
    function.script = code
    function.function_name = 'apply_mark'
    tree.interface_update(bpy.context)
    bpy.context.view_layer.update()
    assert function.inputs.get('data') is not None, 'Requires the integrated Bundle implementation'
    combine = tree.nodes.new('NodeCombineBundle')
    combine.bundle_items.new('FLOAT', 'mask')
    value = tree.nodes.new('ShaderNodeStencilValue')
    tree.links.new(value.outputs[0], combine.inputs['mask'])
    tree.links.new(combine.outputs[0], function.inputs['data'])
    image = bpy.data.images.new('Bundle Integration Image', width=2, height=2, float_buffer=True)
    image.colorspace_settings.name = 'Non-Color'
    image.pixels.foreach_set([0.1, 0.2, 0.3, 1] * 4)
    image.update()
    sample = tree.nodes.new('ShaderNodeImageToClosure')
    sample.image = image
    tree.links.new(sample.outputs[0], function.inputs['image'])
    tree.links.new(function.outputs['color'], tree.nodes['Emission'].inputs['Color'])
    core.plane('Receiver', mat)
    core.plane('Marker', core.marker_material(reference=9), size=2.4, z=0.3)
    pixels = core.render('bundle_texture_stencil_same_frame', out, report)
    core.near(core.sample(pixels)[0], [0.25, 0.35, 0.45], tolerance=0.003)
    core.near(core.sample(pixels, 1.5)[0], [0.1, 0.2, 0.3], tolerance=0.003)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--phase', choices=('views', 'samplers', 'cycles', 'integration', 'all'), default='all')
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    args.output_dir.mkdir(parents=True, exist_ok=True)
    gpu.init()
    report = {'backend': gpu.platform.backend_type_get(), 'renderer': gpu.platform.renderer_get(),
              'build': bpy.app.build_date.decode() + ' ' + bpy.app.build_time.decode(),
              'executable': bpy.app.binary_path, 'cases': {}, 'passed': False}
    try:
        selected = {'views': capture_uv, 'samplers': samplers, 'cycles': cycles,
                    'integration': bundle_integration}
        for phase in (('views', 'samplers', 'cycles') if args.phase == 'all' else (args.phase,)):
            selected[phase](args.output_dir, report)
        report['passed'] = True
        print('STENCIL_VALUE_REVIEW_PASSED', flush=True)
    except Exception:
        report['error'] = traceback.format_exc()
        raise
    finally:
        (args.output_dir / 'verification.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
