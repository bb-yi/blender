# SPDX-License-Identifier: GPL-2.0-or-later
"""Real GPU regressions for outline fast paths, seed competition and frame transitions.

All scenes are synthetic. Optional BLENDER_OUTLINE_REFERENCE_DIR compares the exact same
fixtures with an independently captured reference executable on the same GPU backend.
"""

import json
import os
from pathlib import Path
import time

import bpy
import gpu
import numpy as np
import OpenImageIO as oiio
from gpu_extras.batch import batch_for_shader


OUTPUT = Path(os.environ['BLENDER_TEST_OUTPUT_DIR'])
REFERENCE = os.environ.get('BLENDER_OUTLINE_REFERENCE_DIR')
REPORT = {'cases': {}, 'passed': False}


def outline_material(name, width=0.71, color=(1, 0, 0, 1), alpha=1, method='DITHERED'):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    material.surface_render_method = method
    if method == 'BLENDED':
        # Closed translucent surfaces in the reference build are not pixel deterministic.
        # Exact comparisons use a single layer; outline-visibility also checks transparency.
        material.use_transparency_overlap = False
    tree = material.node_tree
    tree.nodes.clear()
    output = tree.nodes.new('ShaderNodeOutputMaterial')
    emission = tree.nodes.new('ShaderNodeEmission')
    emission.inputs['Color'].default_value = (0, 0.06, 0.2, 1)
    surface = emission.outputs[0]
    if alpha < 1:
        transparent = tree.nodes.new('ShaderNodeBsdfTransparent')
        mix = tree.nodes.new('ShaderNodeMixShader')
        mix.inputs[0].default_value = alpha
        tree.links.new(transparent.outputs[0], mix.inputs[1])
        tree.links.new(surface, mix.inputs[2])
        surface = mix.outputs[0]
    tree.links.new(surface, output.inputs['Surface'])
    control = tree.nodes.new('ShaderNodeOutlineControl')
    for key, value in {'Line Color': color, 'Line Width': width, 'Line Alpha': 1,
                       'Depth Threshold': .1, 'Normal Threshold': .5,
                       'Depth Threshold Range': 0, 'Normal Threshold Range': 0,
                       'Depth Edge Width': 1, 'Normal Edge Width': 1,
                       'ID Edge Width': 1, 'ID Edge': True, 'Outline ID': 1,
                       'Freestyle Edge': True}.items():
        control.inputs[key].default_value = value
    return material, control


def build_scene(width=0.71, size=(269, 157), *, alpha=1, method='DITHERED', dense=False,
                plane_only=False, ribbon=False, mark_edges=False, reset=True):
    if reset:
        bpy.ops.wm.read_factory_settings(use_empty=True)
    else:
        # Keep the live test window/context valid while building a synthetic viewport fixture.
        for obj in list(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    scene.render.resolution_x, scene.render.resolution_y = size
    scene.render.resolution_percentage = 100
    scene.render.image_settings.media_type = 'MULTI_LAYER_IMAGE'
    scene.render.image_settings.file_format = 'OPEN_EXR_MULTILAYER'
    scene.render.image_settings.color_mode = 'RGBA'
    scene.render.image_settings.color_depth = '32'
    scene.render.use_compositing = False
    scene.eevee.use_outline = True
    scene.eevee.use_raytracing = False
    scene.eevee.taa_render_samples = 1
    scene.eevee.taa_samples = 1
    scene.eevee.use_taa_reprojection = False
    scene.render.film_transparent = True
    scene.view_settings.view_transform = 'Standard'
    scene.view_settings.look = 'None'
    scene.world = bpy.data.worlds.new('Outline Performance World')
    scene.world.use_nodes = True
    scene.world.node_tree.nodes['Background'].inputs['Strength'].default_value = 0
    layer = scene.view_layers[0]
    layer.use_pass_z = True
    layer.use_pass_vector = True
    layer.eevee.use_pass_outline = True
    camera = bpy.data.objects.new('Camera', bpy.data.cameras.new('Camera'))
    scene.collection.objects.link(camera)
    camera.location = (0, 0, 6)
    camera.data.type = 'ORTHO'
    camera.data.ortho_scale = 4
    scene.camera = camera
    material, control = outline_material('Synthetic Outline', width, alpha=alpha, method=method)
    if ribbon:
        vertices = []
        faces = []
        for row in range(13):
            y = -1 + row / 6
            angle = np.deg2rad(45 + row * 3)
            vertices.extend([(-.9, y, 0), (0, y, 0),
                             (.9 * float(np.cos(angle)), y, .9 * float(np.sin(angle)))])
        for row in range(12):
            for column in range(2):
                a = row * 3 + column
                faces.append((a, a + 1, a + 4, a + 3))
        mesh = bpy.data.meshes.new('Single Sided Taper Mesh')
        mesh.from_pydata(vertices, [], faces)
        obj = bpy.data.objects.new('Single Sided Taper', mesh)
        scene.collection.objects.link(obj)
        mesh.materials.append(material)
    elif plane_only:
        bpy.ops.mesh.primitive_plane_add(size=1.5)
        bpy.context.view_layer.objects.active.data.materials.append(material)
        marks = bpy.context.view_layer.objects.active.data.attributes.new('freestyle_edge', 'BOOLEAN', 'EDGE')
        for edge in marks.data:
            edge.value = True
    elif dense:
        for row in range(6):
            for col in range(10):
                bpy.ops.mesh.primitive_uv_sphere_add(segments=16, ring_count=8, radius=.145,
                                                   location=(-1.7 + col * .38, -.9 + row * .36, 0))
                bpy.context.view_layer.objects.active.data.materials.append(material)
                if mark_edges:
                    mesh = bpy.context.view_layer.objects.active.data
                    marks = mesh.attributes.new('freestyle_edge', 'BOOLEAN', 'EDGE')
                    for index, edge in enumerate(marks.data):
                        edge.value = index % 5 == 0
    else:
        bpy.ops.mesh.primitive_uv_sphere_add(segments=24, ring_count=12, radius=.69,
                                           location=(-.5, 0, 0))
        bpy.context.view_layer.objects.active.data.materials.append(material)
        bpy.ops.mesh.primitive_cube_add(size=.9, location=(.55, -.12, .15))
        cube = bpy.context.view_layer.objects.active
        cube.rotation_euler = (.2, .35, .15)
        cube.data.materials.append(material)
        marks = cube.data.attributes.new('freestyle_edge', 'BOOLEAN', 'EDGE')
        for index, edge in enumerate(marks.data):
            edge.value = (index % 3 == 0)
    scene.frame_set(79)
    bpy.context.view_layer.update()
    return scene, control


def exr_pixels(path):
    reader = oiio.ImageInput.open(str(path))
    if reader is None:
        raise AssertionError(oiio.geterror())
    try:
        parts = []
        names = []
        subimage = 0
        while reader.seek_subimage(subimage, 0):
            spec = reader.spec()
            parts.append(np.asarray(reader.read_image(subimage, 0, 0, spec.nchannels, oiio.FLOAT),
                                    dtype=np.float32))
            prefix = spec.get_string_attribute('oiio:subimagename')
            names.extend(name if '.' in name or not prefix else prefix + '.' + name
                         for name in spec.channelnames)
            subimage += 1
        pixels = np.concatenate(parts, axis=2)
    finally:
        reader.close()
    if not np.isfinite(pixels).all():
        raise AssertionError(f'Non-finite output: {path}')
    return pixels, names


def outline_channels(pixels, names):
    indices = [i for i, name in enumerate(names) if '.Outline.' in name]
    if len(indices) != 4:
        raise AssertionError(f'Expected RGBA Outline pass, found {names}')
    return pixels[..., indices]


def record(label, pixels, names):
    np.savez_compressed(OUTPUT / (label + '.npz'), pixels=pixels, names=names)
    data = {'shape': list(pixels.shape), 'channels': names}
    if REFERENCE:
        with np.load(Path(REFERENCE) / (label + '.npz')) as reference:
            if list(reference['names']) != names:
                raise AssertionError(f'Pass layout changed: {label}')
            expected = reference['pixels']
        data['max_error'] = float(np.max(np.abs(pixels - expected)))
        data['changed_values'] = int(np.count_nonzero(pixels != expected))
        if data['changed_values']:
            REPORT.setdefault('parity_failures', []).append(label)
    REPORT['cases'][label] = data
    print('OUTLINE_PERFORMANCE_CASE', label, json.dumps(data), flush=True)


def render(label):
    path = OUTPUT / (label + '.exr')
    bpy.context.scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)
    pixels, names = exr_pixels(path)
    record(label, pixels, names)
    return outline_channels(pixels, names)


def render_cases():
    scene, control = build_scene()
    thin = {}
    for label, width in [('empty', 0), ('thin_quarter', .25), ('thin_scene', .71),
                         ('thin_one', 1), ('thin_below_one', .999), ('above_one', 1.001),
                         ('wide_two', 2), ('wide_eight', 8), ('wide_thirtytwo', 32),
                         ('thin_after_wide', .71), ('empty_after_wide', 0)]:
        control.inputs['Line Width'].default_value = width
        image = render(label)
        if width == 0 and np.any(image):
            raise AssertionError('Disabled outlines left nonzero output')
        if 0 < width <= 1:
            thin[label] = image
            if not np.any(image[..., :3] > 0):
                raise AssertionError(f'Thin outline disappeared: {label}')
    np.testing.assert_array_equal(thin['thin_scene'], thin['thin_after_wide'])
    mask_a = thin['thin_quarter'][..., 0] > 0
    mask_b = thin['thin_scene'][..., 0] > 0
    np.testing.assert_array_equal(mask_a, mask_b)
    energy_ratio = thin['thin_scene'][..., 0].sum() / thin['thin_quarter'][..., 0].sum()
    if abs(float(energy_ratio) - .71 / .25) > .02:
        raise AssertionError(f'Thin outline opacity is not proportional to width: {energy_ratio}')

    control.inputs['Line Width'].default_value = 8
    control.inputs['Depth Edge Width'].default_value = .35
    control.inputs['Normal Edge Width'].default_value = .8
    control.inputs['ID Edge Width'].default_value = .5
    control.inputs['Depth Threshold Range'].default_value = .45
    control.inputs['Normal Threshold Range'].default_value = .3
    render('varying_factors')
    control.inputs['Line Width'].default_value = .71
    render('thin_varying_factors')

    tree = control.id_data
    coord = tree.nodes.new('ShaderNodeTexCoord')
    separate = tree.nodes.new('ShaderNodeSeparateXYZ')
    scale = tree.nodes.new('ShaderNodeMath')
    scale.operation = 'MULTIPLY_ADD'
    scale.inputs[1].default_value = 18
    scale.inputs[2].default_value = .2
    tree.links.new(coord.outputs['Generated'], separate.inputs[0])
    tree.links.new(separate.outputs['X'], scale.inputs[0])
    tree.links.new(scale.outputs[0], control.inputs['Line Width'])
    render('texture_width')
    tree.links.remove(control.inputs['Line Width'].links[0])
    for frame, width in [(1, .71), (2, 8), (3, .25)]:
        control.inputs['Line Width'].default_value = width
        control.inputs['Line Width'].keyframe_insert('default_value', frame=frame)
    for frame in [1, 2, 3, 1]:
        scene.frame_set(frame)
        render('animated_' + str(frame))

    for label, method, alpha in [('blended_half', 'BLENDED', .5),
                                  ('blended_zero', 'BLENDED', 0),
                                  ('dithered_half', 'DITHERED', .5)]:
        build_scene(8, alpha=alpha, method=method, plane_only=method == 'BLENDED')
        image = render(label)
        if alpha == 0 and np.any(image):
            raise AssertionError('Transparent material left outline residue')
        if alpha > 0 and not np.any(image[..., :3] > 0):
            raise AssertionError('Partially transparent outline disappeared')

    scene, control = build_scene(8)
    other, _ = outline_material('Wide Neighbor', 24, (0, 1, 0, 1))
    other.node_tree.nodes['Outline Control'].inputs['Outline ID'].default_value = 2
    bpy.ops.mesh.primitive_uv_sphere_add(segments=16, ring_count=8, radius=.5,
                                       location=(-1.1, .12, -.1))
    bpy.context.view_layer.objects.active.data.materials.append(other)
    render('seed_competition')
    scene.render.resolution_x, scene.render.resolution_y = 65, 33
    render('small_odd_extent')
    scene.render.resolution_x, scene.render.resolution_y = 1, 1
    render('minimum_render_extent')
    scene, control = build_scene(8, ribbon=True)
    control.inputs['Depth Threshold'].default_value = 1
    control.inputs['Normal Threshold Range'].default_value = .3
    control.inputs['ID Edge'].default_value = False
    control.inputs['Freestyle Edge'].default_value = False
    image = render('single_sided_taper')
    if not np.any(image[..., :3] > 0):
        raise AssertionError('Single-sided taper fixture has no outline')


def compare_jfa_kernels():
    """Compare actual packed and fallback production kernels, including invisible seed winners."""
    source_path = Path(__file__).resolve().parents[3] / (
        'source/blender/draw/engines/eevee/shaders/eevee_outline_jfa_step_comp.glsl')
    source = source_path.read_text(encoding='utf-8')
    source = source.replace('#include "infos/eevee_outline_infos.hh"', '')
    init_source = source_path.with_name('eevee_outline_jfa_init_frag.glsl').read_text(encoding='utf-8')
    init_source = init_source.replace('#include "infos/eevee_outline_infos.hh"', '')
    kernels = []
    initializers = []
    for packed in (False, True):
        info = gpu.types.GPUShaderCreateInfo()
        info.local_group_size(16, 16, 1)
        fmt, kind = ('RG32UI', 'UINT_2D') if packed else ('RG32F', 'FLOAT_2D')
        info.image(0, fmt, kind, 'jfa_in_img', qualifiers={'READ'})
        info.image(1, fmt, kind, 'jfa_out_img', qualifiers={'WRITE'})
        if not packed:
            info.sampler(23, 'FLOAT_2D', 'outline_seed_tx')
        info.push_constant('INT', 'jfa_step_size')
        prefix = ''
        if packed:
            prefix += '#define OUTLINE_JFA_PACKED\n'
        info.compute_source(prefix + source)
        kernels.append(gpu.shader.create_from_info(info))
        init_info = gpu.types.GPUShaderCreateInfo()
        init_info.vertex_in(0, 'VEC2', 'position')
        init_info.vertex_source('void main() { gl_Position = vec4(position, 0.0, 1.0); }')
        init_info.fragment_out(0, 'UVEC2' if packed else 'VEC2', 'out_jfa_coord')
        init_info.sampler(23, 'FLOAT_2D', 'outline_seed_tx')
        init_info.fragment_source(prefix + init_source)
        shader = gpu.shader.create_from_info(init_info)
        batch = batch_for_shader(shader, 'TRIS', {'position': [(-1, -1), (3, -1), (-1, 3)]})
        initializers.append((shader, batch))
    decode_info = gpu.types.GPUShaderCreateInfo()
    decode_info.vertex_in(0, 'VEC2', 'position')
    decode_info.vertex_source('void main() { gl_Position = vec4(position, 0.0, 1.0); }')
    decode_info.fragment_out(0, 'VEC2', 'out_coord')
    decode_info.sampler(0, 'UINT_2D', 'packed_tx')
    decode_info.fragment_source('''void main() {
      uint p = texelFetch(packed_tx, ivec2(gl_FragCoord.xy), 0).r;
      out_coord = p == 0xffffffffu ? vec2(-1e10) : vec2(uvec2(p & 0xffffu, p >> 16u)) + 0.5;
    }''')
    decoder = gpu.shader.create_from_info(decode_info)
    decode_batch = batch_for_shader(decoder, 'TRIS', {'position': [(-1, -1), (3, -1), (-1, 3)]})
    rng = np.random.default_rng(20261103)
    checks = 0
    for width, height in [(1, 1), (17, 13), (65, 33)]:
        for trial in range(12):
            seeds = np.zeros((height, width, 4), dtype=np.float32)
            seeds[..., 0] = rng.choice([0, .25, .71, 1], size=(height, width))
            seeds[..., 3] = rng.choice([0, .25, .71, 1, 1.001, 2, 8, 32],
                                       size=(height, width))
            seeds[rng.random((height, width)) > .25] = 0
            if trial == 0:
                seeds[:] = 0
            seeds = seeds.astype(np.float16).astype(np.float32)
            seed_texture = gpu.types.GPUTexture((width, height), format='RGBA16F',
                data=gpu.types.Buffer('FLOAT', seeds.shape, seeds))
            tables = []
            for packed, fmt in enumerate(('RG32F', 'RG32UI')):
                pair = [gpu.types.GPUTexture((width, height), format=fmt),
                        gpu.types.GPUTexture((width, height), format=fmt)]
                shader, batch = initializers[packed]
                framebuffer = gpu.types.GPUFrameBuffer(color_slots=pair[0])
                viewport = gpu.state.viewport_get()
                with framebuffer.bind():
                    gpu.state.viewport_set(0, 0, width, height)
                    gpu.state.blend_set('NONE')
                    gpu.state.depth_test_set('NONE')
                    shader.uniform_sampler('outline_seed_tx', seed_texture)
                    batch.draw(shader)
                    # Synchronize raster initialization before the first image-load dispatch.
                    initial = framebuffer.read_color(0, 0, width, height, 2, 0,
                                                     'UINT' if packed else 'FLOAT')
                gpu.state.viewport_set(*viewport)
                tables.append(pair)
            decoded_texture = gpu.types.GPUTexture((width, height), format='RG32F')
            decode_framebuffer = gpu.types.GPUFrameBuffer(color_slots=decoded_texture)
            largest = 1 << (max(width, height) - 1).bit_length()
            steps = [1]
            step = largest // 2
            while step:
                steps.append(step)
                step //= 2
            for step in steps:
                for packed, kernel in enumerate(kernels):
                    kernel.image('jfa_in_img', tables[packed][0])
                    kernel.image('jfa_out_img', tables[packed][1])
                    if not packed:
                        kernel.uniform_sampler('outline_seed_tx', seed_texture)
                    kernel.uniform_int('jfa_step_size', step)
                    gpu.compute.dispatch(kernel, (width + 15) // 16, (height + 15) // 16, 1)
                    tables[packed].reverse()
                legacy = np.array(tables[0][0].read(), dtype=np.float32).reshape(height, width, 2)
                # Decode on GPU: this version's Python texture readback omits RG32UI support.
                with decode_framebuffer.bind():
                    gpu.state.viewport_set(0, 0, width, height)
                    decoder.uniform_sampler('packed_tx', tables[1][0])
                    decode_batch.draw(decoder)
                    decoded = np.array(decode_framebuffer.read_color(0, 0, width, height, 2, 0, 'FLOAT'),
                                       dtype=np.float32).reshape(height, width, 2)
                np.testing.assert_array_equal(legacy, decoded)
                checks += 1
    REPORT['jfa_kernel_checks'] = checks
    print('OUTLINE_JFA_KERNEL_PARITY_OK', checks, flush=True)


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    start = time.perf_counter()
    try:
        gpu.init()
        REPORT.update(backend=gpu.platform.backend_type_get(), gpu=gpu.platform.renderer_get(),
                      build_hash=bpy.app.build_hash.decode(), reference=REFERENCE)
        compare_jfa_kernels()
        render_cases()
        if REPORT.get('parity_failures'):
            raise AssertionError('Pixel parity failed: ' + ', '.join(REPORT['parity_failures']))
        REPORT['passed'] = True
        print('OUTLINE_PERFORMANCE_OK', flush=True)
    finally:
        REPORT['elapsed_seconds'] = time.perf_counter() - start
        (OUTPUT / 'summary.json').write_text(json.dumps(REPORT, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
