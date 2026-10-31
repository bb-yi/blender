# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""File LUT integration: numeric results, masks, alpha, reload, packing and cold reopen.

Run in isolated user resources with BLENDER_TEST_OUTPUT_DIR outside the source tree.
Each invocation checks CPU and GPU compositing on the selected OpenGL/Vulkan backend.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

import bpy
import gpu
import numpy as np

_dll_directories = []
if os.environ.get('BLENDER_TEST_LIBDIR'):
    _libraries = Path(os.environ['BLENDER_TEST_LIBDIR'])
    sys.path[:0] = [str(_libraries / 'opencolorio/lib/site-packages'),
                   str(_libraries / 'OpenImageIO/lib/python3.13/site-packages')]
    if os.name == 'nt':
        for _directory in _libraries.glob('*/bin'):
            _dll_directories.append(os.add_dll_directory(str(_directory)))

import OpenImageIO as oiio
import PyOpenColorIO as ocio

OUT = Path(os.environ['BLENDER_TEST_OUTPUT_DIR']).resolve()
OUT.mkdir(parents=True, exist_ok=True)
CONFIG = ocio.Config.CreateFromFile(str(Path(bpy.utils.resource_path('LOCAL')) /
                                      'datafiles/colormanagement/config.ocio'))
W = H = 16


def cube(path, size=2, kind='identity', domain=False):
    with path.open('w', encoding='utf-8', newline='\n') as stream:
        stream.write(f'TITLE "LUT Integration"\nLUT_3D_SIZE {size}\n')
        if domain:
            stream.write('DOMAIN_MIN -1 -1 -1\nDOMAIN_MAX 2 2 2\n')
        for b in np.linspace(0, 1, size):
            for g in np.linspace(0, 1, size):
                for r in np.linspace(0, 1, size):
                    value = ((g * 0.75 + b * 0.25, b * b, r * g)
                             if kind == 'look' else (r, g, b))
                    stream.write(' '.join(f'{v:.9f}' for v in value) + '\n')


def file_action(node, action, **kwargs):
    with bpy.context.temp_override(node=node):
        result = bpy.ops.node.lut_file(action=action, **kwargs)
    assert result == {'FINISHED'}, (action, result)


def read_parts(path):
    image = oiio.ImageInput.open(str(path))
    assert image is not None, (path, oiio.geterror())
    parts = {}
    index = 0
    while image.seek_subimage(index, 0):
        spec = image.spec()
        channels = [name.rsplit('.', 1)[-1].upper() for name in spec.channelnames]
        name = spec.get_string_attribute('name') or spec.channelnames[0].rsplit('.', 1)[0]
        parts[name] = image.read_image(format='float')[..., [channels.index(c) for c in 'RGBA']]
        index += 1
    image.close()
    return parts


def render(scene, output, name, device='CPU'):
    scene.render.compositor_device = device
    output.file_name = name
    bpy.ops.render.render()
    return read_parts(OUT / (name + '.exr'))


def reference(node, pixels, factor=1.0):
    group = ocio.GroupTransform()
    if node.preset != 'RAW':
        source = (node.input_color_space if node.preset == 'CUSTOM' else
                  'sRGB' if node.preset == 'SRGB' else 'scene_linear')
        target = node.output_color_space if node.preset == 'CUSTOM' and not node.use_same_color_space else source
        if node.direction == 'INVERSE':
            source, target = target, source
        group.appendTransform(ocio.ColorSpaceTransform(src='scene_linear', dst=source))
    lut = ocio.FileTransform(src=bpy.path.abspath(node.filepath))
    lut.setInterpolation({'AUTO': ocio.INTERP_BEST, 'LINEAR': ocio.INTERP_LINEAR,
                          'TETRAHEDRAL': ocio.INTERP_TETRAHEDRAL}[node.interpolation])
    if node.direction == 'INVERSE':
        lut.setDirection(ocio.TRANSFORM_DIR_INVERSE)
    group.appendTransform(lut)
    if node.preset != 'RAW':
        group.appendTransform(ocio.ColorSpaceTransform(src=target, dst='scene_linear'))
    processor = CONFIG.getProcessor(group).getDefaultCPUProcessor()
    expected = pixels.copy()
    weights = np.broadcast_to(factor, pixels.shape[:-1])
    for y, x in np.ndindex(pixels.shape[:2]):
        alpha = float(pixels[y, x, 3])
        if alpha == 0 or weights[y, x] == 0:
            continue
        rgb = processor.applyRGB(list(map(float, pixels[y, x, :3] / alpha)))
        expected[y, x, :3] += weights[y, x] * (np.array(rgb) * alpha - pixels[y, x, :3])
    return expected


def setup():
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    scene.eevee.taa_render_samples = 1
    scene.render.resolution_x = W
    scene.render.resolution_y = H
    scene.render.resolution_percentage = 100
    scene.render.compositor_precision = 'FULL'
    scene.use_nodes = True
    tree = bpy.data.node_groups.new('LUT Integration', 'CompositorNodeTree')
    scene.compositing_node_group = tree
    source_image = bpy.data.images.new('LUT Source', width=W, height=H, alpha=True, float_buffer=True)
    source_image.colorspace_settings.name = 'Non-Color'
    source_image.alpha_mode = 'PREMUL'
    samples = np.array([[.12, .3, .7, 1], [.2, .05, .15, .4], [.01, .02, .03, .1],
                        [0, 0, 0, 0], [-.05, .2, 1.4, 1], [.5, .5, .5, 1],
                        [.91, .09, .37, 1], [.4, .21, .07, .8]], np.float32)
    source_image.pixels.foreach_set(np.tile(samples, (W * H // len(samples), 1)).ravel())
    source_image.update()
    source = tree.nodes.new('CompositorNodeImage')
    source.image = source_image
    output = tree.nodes.new('CompositorNodeOutputFile')
    output.directory = str(OUT)
    output.format.media_type = 'MULTI_LAYER_IMAGE'
    output.format.color_depth = '32'
    output.format.color_management = 'OVERRIDE'
    output.format.linear_colorspace_settings.name = 'Non-Color'
    output.file_output_items.clear()
    return scene, tree, source, output


def main():
    if '--cold' in sys.argv:
        scene = bpy.context.scene
        tree = scene.compositing_node_group
        lut = tree.nodes['Packed Survivor']
        assert lut.is_packed and not Path(bpy.path.abspath(lut.filepath)).exists()
        output = next(node for node in tree.nodes if node.bl_idname == 'CompositorNodeOutputFile')
        output.directory = str(OUT)
        parts = render(scene, output, 'cold', os.environ.get('LUT_COLD_DEVICE', 'CPU'))
        expected = np.load(OUT.parent / 'packed_expected.npy')
        np.testing.assert_allclose(parts['Packed'], expected, rtol=0, atol=4e-5)
        print('COMPOSITOR_LUT_COLD_OK', flush=True)
        return

    scene, tree, source, output = setup()
    nodes = {}

    def capture(name, socket):
        output.file_output_items.new('RGBA', name)
        tree.links.new(socket, output.inputs[name])

    def add(name, path, preset='RAW', interpolation='LINEAR', factor=1.0):
        node = tree.nodes.new('CompositorNodeLUT')
        node.name = name
        node.filepath = str(path)
        node.preset = preset
        node.interpolation = interpolation
        node.inputs['Strength'].default_value = factor
        tree.links.new(source.outputs['Image'], node.inputs['Image'])
        capture(name, node.outputs['Image'])
        nodes[name] = node
        return node

    capture('Original', source.outputs['Image'])
    for size in (17, 33, 65):
        path = OUT / f'identity{size}.cube'
        cube(path, size)
        add(f'Identity{size}', path)
    look = OUT / 'look.cube'
    cube(look, 17, 'look')
    unicode_path = OUT / '调色 LUT 空格.cube'
    unicode_path.write_bytes(look.read_bytes())
    add('UnicodePath', unicode_path)
    for interpolation in ('LINEAR', 'TETRAHEDRAL', 'AUTO'):
        add(interpolation, look, interpolation=interpolation)
    add('sRGB', look, 'SRGB')
    add('SceneLinear', look, 'LINEAR')
    custom = add('Custom', look, 'CUSTOM')
    custom.input_color_space = 'sRGB'
    custom.output_color_space = 'Linear Rec.709'
    same_space = add('CustomSameSpace', look, 'CUSTOM')
    same_space.input_color_space = 'ACEScg'
    assert same_space.use_same_color_space
    add('HalfStrength', look, factor=.5)
    mask_image = bpy.data.images.new('LUT Mask', width=W, height=H, float_buffer=True)
    mask_image.colorspace_settings.name = 'Non-Color'
    mask = np.broadcast_to(np.linspace(0, 1, W, dtype=np.float32), (H, W))
    mask_image.pixels.foreach_set(np.stack((mask, mask, mask, np.ones_like(mask)), axis=-1).ravel())
    mask_image.update()
    mask_node = tree.nodes.new('CompositorNodeImage')
    mask_node.image = mask_image
    masked = add('Mask', look)
    tree.links.new(mask_node.outputs['Image'], masked.inputs['Strength'])
    domain = OUT / 'domain.cube'
    cube(domain, 17, domain=True)
    add('Domain', domain)
    one_d = OUT / 'one_d.cube'
    one_d.write_text('LUT_1D_SIZE 3\n0 0 0\n0.2 0.3 0.4\n0.8 0.9 1\n', encoding='utf-8')
    add('OneD', one_d)
    large_one_d = OUT / 'large_one_d.cube'
    with large_one_d.open('w', encoding='utf-8', newline='\n') as stream:
        stream.write('LUT_1D_SIZE 65536\n')
        for value in np.linspace(0, 1, 65536):
            stream.write(f'{value * value:.9f} {np.sqrt(value):.9f} {value * .75:.9f}\n')
    add('LargeOneD', large_one_d)
    add('Inverse', one_d).direction = 'INVERSE'
    forward_custom = add('ForwardCustom1D', one_d, 'CUSTOM')
    forward_custom.input_color_space = 'sRGB'
    forward_custom.output_color_space = 'Linear Rec.709'
    inverse_custom = add('CustomRoundTrip', one_d, 'CUSTOM')
    inverse_custom.input_color_space = 'sRGB'
    inverse_custom.output_color_space = 'Linear Rec.709'
    inverse_custom.direction = 'INVERSE'
    tree.links.new(forward_custom.outputs['Image'], inverse_custom.inputs['Image'])
    combined = OUT / 'combined.cube'
    combined.write_text('LUT_1D_SIZE 2\nLUT_1D_INPUT_RANGE 0 1\nLUT_3D_SIZE 2\n'
                        '0 0 0\n1 1 1\n' + ''.join(
                            f'{r} {g} {b}\n' for b in (0, 1) for g in (0, 1) for r in (0, 1)),
                        encoding='utf-8')
    add('Combined', combined)
    add('BypassMissing', OUT / 'missing.cube', factor=0)
    add('Missing', OUT / 'missing.cube')
    bad = OUT / 'bad.cube'
    bad.write_text('LUT_3D_SIZE 2\n1 0 0\n', encoding='utf-8')
    add('Malformed', bad)
    # The disconnected color input exercises the single-value execution path.
    constant = add('Constant', look)
    tree.links.remove(constant.inputs['Image'].links[0])
    constant.inputs['Image'].default_value = (.1, .2, .3, .5)

    # Exercise the actual preset operator and its alpha conversions with an editable outer LUT.
    area = bpy.context.screen.areas[0]
    original_area_type = area.type
    area.type = 'NODE_EDITOR'
    area.spaces.active.tree_type = 'CompositorNodeTree'
    area.spaces.active.pin = True
    area.spaces.active.node_tree = tree
    previous = {node.as_pointer() for node in tree.nodes}
    with bpy.context.temp_override(area=area):
        assert bpy.ops.node.add_display_lut(configure_output=True) == {'FINISHED'}
    created = [node for node in tree.nodes if node.as_pointer() not in previous]
    display_lut = next(node for node in created if node.bl_idname == 'CompositorNodeLUT')
    display_lut.filepath = str(look)
    display_lut.interpolation = 'LINEAR'
    before = next(node for node in created if node.bl_idname == 'CompositorNodeGroup' and
                  node.node_tree.name.startswith('Scene to AgX'))
    after = next(node for node in created if node.bl_idname == 'CompositorNodeGroup' and
                 node.node_tree.name.startswith('sRGB Display'))
    tree.links.new(source.outputs['Image'], before.inputs['Image'])
    capture('DisplaySetup', after.outputs['Image'])
    assert scene.view_settings.view_transform == 'Standard'
    area.type = original_area_type

    errors = {}
    cpu = render(scene, output, 'cpu')
    original = cpu['Original']
    display = CONFIG.getProcessor(ocio.DisplayViewTransform(
        src='scene_linear', display='sRGB', view='AgX')).getDefaultCPUProcessor()
    lookup = CONFIG.getProcessor(ocio.FileTransform(
        src=str(look), interpolation=ocio.INTERP_LINEAR)).getDefaultCPUProcessor()
    decode = CONFIG.getProcessor('sRGB', 'scene_linear').getDefaultCPUProcessor()
    display_expected = original.copy()
    for y, x in np.ndindex(original.shape[:2]):
        alpha = float(original[y, x, 3])
        if alpha:
            rgb = display.applyRGB(list(map(float, original[y, x, :3] / alpha)))
            rgb = decode.applyRGB(lookup.applyRGB(rgb))
            display_expected[y, x, :3] = np.array(rgb) * alpha
    np.testing.assert_allclose(cpu['DisplaySetup'], display_expected, rtol=0, atol=8e-5,
                               err_msg='Display setup')
    for name, node in nodes.items():
        if name in {'Missing', 'Malformed', 'BypassMissing'}:
            expected = original
        elif name == 'Constant':
            pixels = np.broadcast_to(np.array([.1, .2, .3, .5], np.float32), original.shape)
            expected = reference(node, pixels)
        elif name == 'CustomRoundTrip':
            expected = reference(node, reference(forward_custom, original))
        elif name == 'UnicodePath':
            expected = reference(nodes['LINEAR'], original)
        else:
            expected = reference(node, original, mask if name == 'Mask' else
                                 .5 if name == 'HalfStrength' else 1.0)
        np.testing.assert_allclose(cpu[name], expected, rtol=0, atol=4e-5, err_msg=name)
        errors[name] = float(np.max(np.abs(cpu[name] - expected)))
    valid = (original[..., 3] > 0) & np.all((original[..., :3] >= 0) &
                 (original[..., :3] <= original[..., 3:4]), axis=-1)
    np.testing.assert_allclose(cpu['CustomRoundTrip'][valid], original[valid], rtol=0, atol=8e-5,
                               err_msg='Inverse must exchange authored LUT color spaces')
    accelerated = render(scene, output, 'gpu', 'GPU')
    backend = gpu.platform.backend_type_get()
    print('COMPOSITOR_LUT_BACKEND=' + backend, flush=True)
    for name in cpu:
        np.testing.assert_allclose(accelerated[name], cpu[name], rtol=0, atol=8e-5, err_msg=name)
    # Same path, byte count, and mtime: only explicit reload distinguishes the changed contents.
    reload_path = OUT / 'reload.cube'
    cube(reload_path, 2)
    reload_node = add('Reload', reload_path)
    before = render(scene, output, 'reload_before')['Reload']
    stamp = reload_path.stat()
    data = reload_path.read_text(encoding='utf-8')
    reload_path.write_text(data.replace('1.000000000', '0.500000000'), encoding='utf-8', newline='\n')
    os.utime(reload_path, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
    file_action(reload_node, 'RELOAD')
    after = render(scene, output, 'reload_after')['Reload']
    assert np.max(np.abs(after - before)) > .1, 'Reload retained the old OCIO file cache'

    # A packed node survives copying and deletion of the original node and external file.
    file_action(reload_node, 'PACK')
    assert reload_node.is_packed
    copied_tree = tree.copy()
    packed_images = (source.image, mask_image)
    survivor = copied_tree.nodes['Reload']
    survivor.name = 'Packed Survivor'
    file_action(reload_node, 'CLEAR')
    bpy.data.node_groups.remove(tree)
    scene.compositing_node_group = copied_tree
    output = next(n for n in copied_tree.nodes if n.bl_idname == 'CompositorNodeOutputFile')
    output.file_output_items.clear()
    output.file_output_items.new('RGBA', 'Packed')
    copied_tree.links.new(survivor.outputs['Image'], output.inputs['Packed'])
    for node in list(copied_tree.nodes):
        if node.bl_idname == 'CompositorNodeLUT' and node != survivor:
            copied_tree.nodes.remove(node)
    for image in packed_images:
        image.pack()
    packed_data = reload_path.read_bytes()
    reload_path.write_bytes(packed_data + b'\n# newer external file\n')
    try:
        file_action(survivor, 'UNPACK')
    except RuntimeError:
        pass
    else:
        raise AssertionError('Unpack overwrote a different external file')
    assert survivor.is_packed and reload_path.read_bytes().endswith(b'# newer external file\n')
    exported = OUT / 'unpacked_export.cube'
    survivor.filepath = str(exported)
    assert survivor.is_packed, 'Changing a packed file reference discarded its contents'
    file_action(survivor, 'UNPACK')
    assert not survivor.is_packed and exported.read_bytes() == packed_data
    bpy.ops.file.pack_all()
    assert survivor.is_packed, 'Pack Resources did not include the LUT'
    reload_path = exported
    reload_path.unlink()
    immediate = render(scene, output, 'packed_global_immediate')['Packed']
    np.testing.assert_allclose(immediate, after, rtol=0, atol=4e-5,
                               err_msg='Global packing did not update evaluated LUT data')
    saved_reference = survivor.filepath
    survivor.filepath = ''
    no_reference = render(scene, output, 'packed_no_reference')['Packed']
    np.testing.assert_allclose(no_reference, after, rtol=0, atol=4e-5)
    survivor.filepath = saved_reference
    blend = OUT / 'packed.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(blend), check_existing=False)
    file_action(survivor, 'RELATIVE')
    assert survivor.filepath.startswith('//') and survivor.is_packed
    bpy.ops.wm.save_as_mainfile(filepath=str(blend), check_existing=False)
    external_paths = {Path(path).resolve() for path in bpy.utils.blend_paths(absolute=True, packed=False)}
    assert reload_path.resolve() not in external_paths, 'Packed LUT reported as a missing external resource'
    expected = render(scene, output, 'packed_live')['Packed']
    np.testing.assert_allclose(expected, after, rtol=0, atol=4e-5)
    np.save(OUT / 'packed_expected.npy', expected)
    cold_dir = OUT / 'cold_process'
    cold_dir.mkdir(exist_ok=True)
    for device in ('CPU', 'GPU'):
        env = os.environ.copy()
        env['BLENDER_TEST_OUTPUT_DIR'] = str(cold_dir)
        env['LUT_COLD_DEVICE'] = device
        sandbox = Path(os.environ['TEMP']).resolve().parent / ('cold_' + device.lower()) / 'user'
        for key, suffix in [('RESOURCES', ''), ('CONFIG', 'config'), ('SCRIPTS', 'scripts'),
                            ('DATAFILES', 'datafiles'), ('EXTENSIONS', 'extensions')]:
            path = sandbox / suffix
            path.mkdir(parents=True, exist_ok=True)
            env['BLENDER_USER_' + key] = str(path)
        temporary = sandbox.parent / 'tmp'
        temporary.mkdir(exist_ok=True)
        env['TEMP'] = env['TMP'] = str(temporary)
        command = [bpy.app.binary_path, '--background', '--factory-startup', '--gpu-backend',
                   backend.lower(), str(blend), '--python-exit-code', '1', '--python',
                   str(Path(__file__).resolve()), '--', '--cold']
        child = subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, encoding='utf-8', errors='replace')
        try:
            child_output, _ = child.communicate(timeout=180)
        except subprocess.TimeoutExpired:
            if os.name == 'nt':
                query = (f'Get-CimInstance Win32_Process -Filter "ProcessId = {child.pid}" | '
                         'Select-Object ProcessId,ExecutablePath,CommandLine | ConvertTo-Json -Compress')
                snapshot = subprocess.check_output(['pwsh', '-NoProfile', '-Command', query],
                                                    encoding='utf-8').strip()
                print(snapshot, flush=True)
                identity = json.loads(snapshot)
                assert Path(identity['ExecutablePath']).resolve() == Path(bpy.app.binary_path).resolve()
                assert 'test_compositor_lut.py' in identity['CommandLine']
            child.kill()
            child_output, _ = child.communicate()
            (OUT / ('cold_' + device.lower() + '.log')).write_text(child_output, encoding='utf-8')
            raise AssertionError(f'Managed LUT cold reopen timed out: PID {child.pid}')
        (OUT / ('cold_' + device.lower() + '.log')).write_text(
            child_output, encoding='utf-8')
        assert child.returncode == 0 and 'COMPOSITOR_LUT_COLD_OK' in child_output, (
            device, child.returncode, child_output[-5000:])
    (OUT / 'results.json').write_text(json.dumps({'backend': backend, 'cpu_errors': errors,
                                                'cpu_gpu_max_error': max(float(np.max(np.abs(
                                                    cpu[n] - accelerated[n]))) for n in cpu),
                                                'cold_reopen': ['CPU', 'GPU']}, indent=2), encoding='utf-8')
    print('COMPOSITOR_LUT_OK', flush=True)


if __name__ == '__main__':
    main()
