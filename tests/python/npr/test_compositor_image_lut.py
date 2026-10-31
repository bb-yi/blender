# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Sliced image LUT integration, compared to independent equivalent .cube tables."""
import json
import os
from pathlib import Path
import runpy
import sys

import bpy
import gpu
import numpy as np

helpers = runpy.run_path(str(Path(__file__).with_name('test_compositor_lut.py')),
                        run_name='lut_test_helpers')
OUT = helpers['OUT']
oiio = helpers['oiio']


def atlas(path, size=16, columns=None, dtype='uint8', kind='look', flip_y=False, flip_green=False):
    """Write a top-left atlas and a .cube from the same quantized samples, without reading it back."""
    columns = columns or size
    assert size % columns == 0
    axis = np.linspace(0, 1, size, dtype=np.float32)
    b, g, r = np.meshgrid(axis, axis, axis, indexing='ij')
    if kind == 'identity':
        rgb = np.stack((r, g, b), axis=-1)
    elif kind == 'hdr':
        rgb = np.stack((r * 1.2 - .1, g * g * 1.1, .2 + .7 * b), axis=-1)
    else:
        rgb = np.stack((.1 + .7 * g + .1 * b, .05 + .7 * b * b,
                        .1 + .6 * r * g + .1 * r), axis=-1)
    alpha = ((np.indices((size, size, size)).sum(axis=0) % 4) / 3).astype(np.float32)
    table = np.concatenate((rgb, alpha[..., None]), axis=-1)
    if dtype != 'float':
        maximum = 255 if dtype == 'uint8' else 65535
        encoded = np.rint(table * maximum).astype(dtype)
        table = encoded.astype(np.float32) / maximum
    else:
        encoded = table
    image = np.empty((size * (size // columns), size * columns, 4), dtype=encoded.dtype)
    for blue in range(size):
        row, column = divmod(blue, columns)
        image[row * size:(row + 1) * size, column * size:(column + 1) * size] = (
            encoded[blue, ::-1] if flip_green else encoded[blue])
    if flip_y:
        image = image[::-1].copy()
    writer = oiio.ImageOutput.create(str(path))
    assert writer, (path, oiio.geterror())
    spec = oiio.ImageSpec(image.shape[1], image.shape[0], 4, dtype)
    spec.attribute('oiio:ColorSpace', 'sRGB')
    spec.attribute('oiio:UnassociatedAlpha', 1)
    if path.suffix == '.exr':
        spec.attribute('compression', 'none')
    assert writer.open(str(path), spec), writer.geterror()
    assert writer.write_image(image), writer.geterror()
    assert writer.close(), writer.geterror()
    reference = path.with_name(path.name + '.cube')
    with reference.open('w', encoding='utf-8', newline='\n') as stream:
        stream.write(f'LUT_3D_SIZE {size}\n')
        np.savetxt(stream, table[..., :3].reshape(-1, 3), fmt='%.9f')
    return reference


def main():
    if '--cold' in sys.argv:
        scene = bpy.context.scene
        tree = scene.compositing_node_group
        lut = tree.nodes['Packed Image']
        assert lut.is_packed and lut.image_flip_y
        assert not Path(bpy.path.abspath(lut.filepath)).exists()
        output = next(node for node in tree.nodes if node.bl_idname == 'CompositorNodeOutputFile')
        output.directory = str(OUT)
        actual = helpers['render'](scene, output, 'cold', os.environ['LUT_COLD_DEVICE'])['Packed']
        np.testing.assert_allclose(actual, np.load(OUT.parent / 'packed_image_expected.npy'),
                                   rtol=0, atol=8e-5)
        print('COMPOSITOR_IMAGE_LUT_COLD_OK', flush=True)
        return

    scene, tree, source, output = helpers['setup']()
    random = np.random.default_rng(1942)
    pixels = random.uniform(.01, .99, (16, 16, 4)).astype(np.float32)
    pixels[..., 3] = random.choice(np.array([.1, .4, 1], np.float32), (16, 16))
    pixels[..., :3] *= pixels[..., 3:4]
    # Exact endpoints, near slice boundaries, transparent pixels, and out-of-range values.
    pixels[0, :8] = np.array([[0, 0, 0, 1], [1, 1, 1, 1], [1, 0, 1, 1],
                              [.3, .6, 7 / 15 - 1e-6, 1], [.3, .6, 7 / 15 + 1e-6, 1],
                              [.03, .06, .09, .1], [.2, .1, .3, 0], [-.1, .5, 1.4, 1]], np.float32)
    source.image.pixels.foreach_set(pixels.ravel())
    source.image.update()

    def capture(name, socket):
        output.file_output_items.new('RGBA', name)
        tree.links.new(socket, output.inputs[name])

    capture('Original', source.outputs['Image'])
    references = {}
    nodes = {}
    fixtures = [('Strip16PNG8', 'png', 16, 16, 'uint8', 'identity'),
                ('Strip32PNG8', 'png', 32, 32, 'uint8', 'look'),
                ('Strip64PNG16', 'png', 64, 64, 'uint16', 'look'),
                ('VerticalPNG16', 'png', 16, 1, 'uint16', 'look'),
                ('Grid64PNG8', 'png', 64, 8, 'uint8', 'look'),
                ('Rectangle32PNG8', 'png', 32, 8, 'uint8', 'look'),
                ('GridTIFF16', 'tiff', 16, 4, 'uint16', 'look'),
                ('GridEXR32HDR', 'exr', 8, 4, 'float', 'hdr')]
    for name, extension, size, columns, dtype, kind in fixtures:
        path = OUT / (name + '.' + extension)
        reference_path = atlas(path, size, columns, dtype, kind)
        reference_node = tree.nodes.new('CompositorNodeLUT')
        reference_node.filepath = str(reference_path)
        reference_node.preset = 'RAW'
        for method in ('LINEAR', 'TETRAHEDRAL', 'AUTO'):
            key = name + '_' + method
            node = tree.nodes.new('CompositorNodeLUT')
            node.filepath, node.preset, node.interpolation = str(path), 'RAW', method
            tree.links.new(source.outputs['Image'], node.inputs['Image'])
            capture(key, node.outputs['Image'])
            nodes[key], references[key] = node, (reference_node, method)

    inverse = tree.nodes.new('CompositorNodeLUT')
    inverse.filepath, inverse.preset, inverse.direction = str(OUT / 'Strip16PNG8.png'), 'RAW', 'INVERSE'
    tree.links.new(source.outputs['Image'], inverse.inputs['Image'])
    capture('ImageInverse', inverse.outputs['Image'])
    inverse_reference = tree.nodes.new('CompositorNodeLUT')
    inverse_reference.filepath = str(OUT / 'Strip16PNG8.png.cube')
    inverse_reference.preset, inverse_reference.direction = 'RAW', 'INVERSE'
    references['ImageInverse'] = (inverse_reference, 'AUTO')

    mask = np.broadcast_to(np.linspace(0, 1, 16, dtype=np.float32), (16, 16))
    mask_image = bpy.data.images.new('Image LUT Mask', width=16, height=16, float_buffer=True)
    mask_image.colorspace_settings.name = 'Non-Color'
    mask_image.pixels.foreach_set(np.stack((mask, mask, mask, np.ones_like(mask)), axis=-1).ravel())
    mask_image.update()
    mask_source = tree.nodes.new('CompositorNodeImage')
    mask_source.image = mask_image
    masked = tree.nodes.new('CompositorNodeLUT')
    masked.filepath, masked.preset, masked.interpolation = str(OUT / 'Strip32PNG8.png'), 'RAW', 'LINEAR'
    tree.links.new(source.outputs['Image'], masked.inputs['Image'])
    tree.links.new(mask_source.outputs['Image'], masked.inputs['Strength'])
    capture('ImageMask', masked.outputs['Image'])
    references['ImageMask'] = references['Strip32PNG8_LINEAR']

    for key, flip_y, flip_green in [('FlipY', True, False), ('FlipGreen', False, True),
                                     ('FlipBoth', True, True)]:
        path = OUT / (key + '.png')
        reference_path = atlas(path, 16, 4, 'uint16', flip_y=flip_y, flip_green=flip_green)
        node = tree.nodes.new('CompositorNodeLUT')
        node.filepath, node.preset = str(path), 'SRGB'
        node.image_flip_y, node.image_flip_green = flip_y, flip_green
        node.inputs['Strength'].default_value = .65
        tree.links.new(source.outputs['Image'], node.inputs['Image'])
        capture(key, node.outputs['Image'])
        reference_node = tree.nodes.new('CompositorNodeLUT')
        reference_node.filepath, reference_node.preset = str(reference_path), 'SRGB'
        nodes[key], references[key] = node, (reference_node, 'AUTO')

    invalid = OUT / 'invalid_layout.png'
    writer = oiio.ImageOutput.create(str(invalid))
    assert writer.open(str(invalid), oiio.ImageSpec(31, 16, 3, 'uint8'))
    assert writer.write_image(np.zeros((16, 31, 3), np.uint8))
    assert writer.close()
    for key, path in [('BadDimensions', invalid), ('BadPNG', OUT / 'truncated.png')]:
        if key == 'BadPNG':
            path.write_bytes(b'\x89PNG\r\n\x1a\n')
        node = tree.nodes.new('CompositorNodeLUT')
        node.filepath = str(path)
        tree.links.new(source.outputs['Image'], node.inputs['Image'])
        capture(key, node.outputs['Image'])

    cpu = helpers['render'](scene, output, 'images_cpu')
    errors = {}
    for name, (reference_node, method) in references.items():
        reference_node.interpolation = method
        factor = mask if name == 'ImageMask' else .65 if name.startswith('Flip') else 1
        expected = helpers['reference'](reference_node, cpu['Original'], factor)
        np.testing.assert_allclose(cpu[name], expected, rtol=0, atol=4e-5, err_msg=name)
        errors[name] = float(np.max(np.abs(cpu[name] - expected)))
    for name in ('BadDimensions', 'BadPNG'):
        np.testing.assert_array_equal(cpu[name], cpu['Original'], err_msg=name)
    accelerated = helpers['render'](scene, output, 'images_gpu', 'GPU')
    for name in cpu:
        np.testing.assert_allclose(accelerated[name], cpu[name], rtol=0, atol=8e-5, err_msg=name)

    survivor = nodes['FlipY']
    output.file_output_items.clear()
    capture('Packed', survivor.outputs['Image'])
    survivor.image_flip_y = False
    wrong_orientation = helpers['render'](scene, output, 'image_orientation_changed', 'GPU')['Packed']
    assert float(np.max(np.abs(wrong_orientation - cpu['FlipY']))) > .02
    survivor.image_flip_y = True
    np.testing.assert_allclose(helpers['render'](scene, output, 'image_orientation_restored', 'GPU')['Packed'],
                               cpu['FlipY'], rtol=0, atol=8e-5)
    # Same pathname, size, and timestamp; the explicit reload must update even packed image bytes.
    reload_path = OUT / 'reload_image.exr'
    atlas(reload_path, 8, 4, 'float', kind='identity')
    helpers['file_action'](survivor, 'OPEN', filepath=str(reload_path))
    helpers['file_action'](survivor, 'PACK')
    survivor.image_flip_y = False
    survivor.inputs['Strength'].default_value = 1
    before = helpers['render'](scene, output, 'image_reload_before')['Packed']
    stamp = reload_path.stat()
    atlas(reload_path, 8, 4, 'float', kind='look')
    assert reload_path.stat().st_size == stamp.st_size
    os.utime(reload_path, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
    helpers['file_action'](survivor, 'RELOAD')
    after = helpers['render'](scene, output, 'image_reload_after')['Packed']
    assert float(np.max(np.abs(after - before))) > .05
    expected_node = tree.nodes.new('CompositorNodeLUT')
    expected_node.filepath, expected_node.preset = str(reload_path) + '.cube', 'SRGB'
    expected_node.interpolation = survivor.interpolation
    np.testing.assert_allclose(after, helpers['reference'](expected_node, cpu['Original']),
                               rtol=0, atol=8e-5)

    packed_path = OUT / 'packed_image.png'
    atlas(packed_path, 16, 4, 'uint16', flip_y=True)
    helpers['file_action'](survivor, 'OPEN', filepath=str(packed_path))
    survivor.image_flip_y = True
    survivor.name = 'Packed Image'
    for node in list(tree.nodes):
        if node not in (survivor, source, output):
            tree.nodes.remove(node)
    source.image.pack()
    bpy.ops.file.pack_all()
    assert survivor.is_packed
    expected = helpers['render'](scene, output, 'image_packed')['Packed']
    np.save(OUT / 'packed_image_expected.npy', expected)
    survivor.filepath = ''
    np.testing.assert_allclose(helpers['render'](scene, output, 'image_packed_no_path')['Packed'],
                               expected, rtol=0, atol=8e-5)
    survivor.filepath = str(packed_path)
    packed_path.replace(OUT / 'packed_image_external_moved.png')
    tree_copy = tree.copy()
    scene.compositing_node_group = tree_copy
    bpy.data.node_groups.remove(tree)
    assert tree_copy.nodes['Packed Image'].is_packed
    np.testing.assert_allclose(helpers['render'](scene, output=next(
        n for n in tree_copy.nodes if n.bl_idname == 'CompositorNodeOutputFile'),
        name='image_packed_copy')['Packed'], expected, rtol=0, atol=8e-5)
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT / 'image_packed.blend'), check_existing=False)
    result = {'backend': gpu.platform.backend_type_get(), 'reference_errors': errors,
              'cpu_gpu_max_error': max(float(np.max(np.abs(cpu[n] - accelerated[n]))) for n in cpu),
              'packed_copy_without_external_file': True}
    (OUT / 'image_results.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print('COMPOSITOR_IMAGE_LUT_OK=' + json.dumps(result), flush=True)


if __name__ == '__main__':
    main()
