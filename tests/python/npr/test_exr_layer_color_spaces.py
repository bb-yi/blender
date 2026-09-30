# SPDX-License-Identifier: GPL-2.0-or-later
"""Per-layer EXR output and per-part input color-space regressions.

Run in an isolated Blender user directory with --output-dir outside the source tree,
or import this module and call run_tests(output_dir). Requires NumPy, OIIO and OCIO.
"""
import argparse
import json
from pathlib import Path
import sys
import unittest

import bpy
import numpy as np
import OpenImageIO as oiio
import PyOpenColorIO as ocio

OUT = None
LEGACY_FILE = (Path(__file__).resolve().parents[2] /
               "files/compositor/exr_layer_color_spaces/legacy-settings.blend")
RGBA = np.array([0.18, -0.125, 1.5, 0.35], dtype=np.float32)
IDS = {"sRGB": "srgb_rec709_display", "Linear Rec.709": "lin_rec709_scene",
       "ACEScg": "lin_ap1_scene", "Non-Color": "data"}
CONFIG = ocio.Config.CreateFromFile(str(Path(bpy.utils.resource_path('LOCAL')) /
                                      "datafiles/colormanagement/config.ocio"))


def transformed(value, target):
    value = np.array(value, dtype=np.float32)
    alpha = value[3]
    if alpha not in (0.0, 1.0):
        value[:3] /= alpha
    value = np.array(CONFIG.getProcessor("Linear Rec.709", target)
                     .getDefaultCPUProcessor().applyRGBA(list(map(float, value))), np.float32)
    if alpha not in (0.0, 1.0):
        value[:3] *= alpha
    return value


def read_parts(path):
    inp = oiio.ImageInput.open(str(path))
    if inp is None:
        raise AssertionError(f"Cannot open {path}: {oiio.geterror()}")
    parts = {}
    index = 0
    while inp.seek_subimage(index, 0):
        spec = inp.spec()
        names = list(spec.channelnames)
        name = spec.get_string_attribute("name") or names[0].rsplit('.', 1)[0]
        tag = spec.get_string_attribute("colorInteropID") or spec.get_string_attribute("oiio:ColorSpace")
        pixels = inp.read_image(format='float')
        parts[name] = dict(tag=tag, names=names, pixels=pixels.copy(),
                           formats=[str(f) for f in spec.channelformats] or [str(spec.format)] * spec.nchannels,
                           attrs={a.name: str(a.value) for a in spec.extra_attribs})
        index += 1
    inp.close()
    return parts


def values(part, channels="RGBA"):
    suffixes = [n.rsplit('.', 1)[-1].upper() for n in part['names']]
    return part['pixels'][..., [suffixes.index(c) for c in channels]]


class ExrLayerColorSpaces(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=False)
        self.scene = bpy.context.scene
        self.scene.render.engine = 'BLENDER_EEVEE'
        self.scene.render.resolution_x = self.scene.render.resolution_y = 32
        self.scene.render.resolution_percentage = 100
        self.scene.render.compositor_device = 'CPU'
        self.scene.render.compositor_precision = 'FULL'
        self.scene.eevee.taa_render_samples = 1
        self.scene.view_settings.view_transform = 'Standard'
        self.scene.use_nodes = True
        self.tree = bpy.data.node_groups.new("EXR Layer Color Test", "CompositorNodeTree")
        self.scene.compositing_node_group = self.tree
        self.image = bpy.data.images.new("Float Source", width=32, height=32,
                                          alpha=True, float_buffer=True)
        self.image.colorspace_settings.name = 'Linear Rec.709'
        self.image.alpha_mode = 'CHANNEL_PACKED'
        self.image.pixels.foreach_set(np.tile(RGBA, 32 * 32))
        self.image.update()
        self.source = self.tree.nodes.new('CompositorNodeImage')
        self.source.image = self.image
        self.output = self.tree.nodes.new('CompositorNodeOutputFile')
        self.output.directory = str(OUT)
        self.output.file_name = self._testMethodName
        self.output.format.media_type = 'MULTI_LAYER_IMAGE'
        self.output.format.color_management = 'OVERRIDE'
        self.output.format.linear_colorspace_settings.name = 'sRGB'
        self.output.format.color_depth = '32'
        self.output.format.exr_codec = 'ZIP'
        self.output.format.use_exr_interleave = True
        self.output.file_output_items.clear()

    def add(self, name, space=None, kind='RGBA', source=None):
        item = self.output.file_output_items.new(kind, name)
        self.assertFalse(item.override_color_space)
        if space is not None:
            item.override_color_space = True
            item.format.linear_colorspace_settings.name = space
        self.tree.links.new(source or self.source.outputs['Image'], self.output.inputs[name])
        return item

    def render(self, suffix=''):
        self.output.file_name = self._testMethodName + suffix
        path = OUT / (self.output.file_name + '.exr')
        if path.exists():
            path.unlink()
        bpy.ops.render.render()
        self.assertTrue(path.exists(), str(path))
        return path, read_parts(path)

    def assert_color(self, part, space, atol=2e-5):
        self.assertEqual(part['tag'], IDS[space])
        expected = RGBA if space == 'Non-Color' else transformed(RGBA, space)
        np.testing.assert_allclose(values(part), np.broadcast_to(expected, values(part).shape),
                                   rtol=0, atol=atol)

    def test_01_mixed_spaces_and_data(self):
        self.add('DataFirst', 'Non-Color')
        self.add('Inherited')
        for name in ['Linear Rec.709', 'sRGB', 'ACEScg']:
            self.add(name, name)
        _, parts = self.render()
        self.assertEqual(next(iter(parts)), 'Inherited')
        self.assertEqual(len(parts), 5)
        self.assertTrue(self.output.format.use_exr_interleave, 'Stored layout must not be changed')
        for name, space in [('DataFirst', 'Non-Color'), ('Inherited', 'sRGB'),
                             ('Linear Rec.709', 'Linear Rec.709'), ('sRGB', 'sRGB'), ('ACEScg', 'ACEScg')]:
            self.assert_color(parts[name], space)
        np.testing.assert_array_equal(values(parts['DataFirst']), np.broadcast_to(RGBA, (32, 32, 4)))

    def test_02_socket_conversions(self):
        self.add('AOV_002', kind='VECTOR')
        self.add('AOV_004', kind='FLOAT')
        normal = self.tree.nodes.new('ShaderNodeVectorMath')
        normal.operation = 'NORMALIZE'
        self.tree.links.new(self.source.outputs['Image'], normal.inputs[0])
        self.add('AOV_003', 'sRGB', source=normal.outputs['Vector'])
        _, parts = self.render()
        self.assertEqual(parts['AOV_002']['tag'], 'data')
        np.testing.assert_array_equal(values(parts['AOV_002'], 'XYZ'), np.broadcast_to(RGBA[:3], (32, 32, 3)))
        self.assertEqual(parts['AOV_004']['tag'], 'data')
        expected_luma = np.dot(RGBA[:3], [0.2126, 0.7152, 0.0722])
        np.testing.assert_allclose(values(parts['AOV_004'], 'V'), expected_luma, atol=3e-5)
        normalized = np.r_[RGBA[:3] / np.linalg.norm(RGBA[:3]), 1.0]
        np.testing.assert_allclose(values(parts['AOV_003']), np.broadcast_to(transformed(normalized, 'sRGB'), (32,32,4)), atol=2e-5)

    def test_03_half_and_dwab(self):
        self.add('Raw', 'Non-Color')
        self.add('Color', 'sRGB')
        self.output.format.color_depth = '16'
        for codec, tolerance in [('ZIP', 0.001), ('DWAB', 0.025)]:
            self.output.format.exr_codec = codec
            _, parts = self.render('_' + codec)
            self.assertTrue(all(f == 'float' for f in parts['Raw']['formats']))
            self.assertTrue(all(f == 'half' for f in parts['Color']['formats']))
            np.testing.assert_array_equal(values(parts['Raw']), np.broadcast_to(RGBA, (32,32,4)))
            self.assert_color(parts['Color'], 'sRGB', tolerance)

    def test_04_follow_scene_retains_settings(self):
        item = self.add('Color', 'ACEScg')
        self.output.format.use_exr_interleave = False
        self.output.format.color_management = 'FOLLOW_SCENE'
        _, parts = self.render('_follow')
        self.assert_color(parts['Color'], 'Linear Rec.709')
        self.assertTrue(item.override_color_space)
        self.assertEqual(item.format.linear_colorspace_settings.name, 'ACEScg')
        self.output.format.color_management = 'OVERRIDE'
        _, parts = self.render('_override')
        self.assert_color(parts['Color'], 'ACEScg')

    def test_05_all_data(self):
        self.add('First', 'Non-Color')
        self.add('Vector', kind='VECTOR')
        _, parts = self.render()
        self.assertEqual(next(iter(parts)), 'First')
        self.assertTrue(all(p['tag'] == 'data' for p in parts.values()))

    def test_06_copy_save_reload(self):
        self.add('SRGB', 'sRGB')
        self.add('Raw', 'Non-Color')
        copied = self.tree.copy()
        copied_output = copied.nodes.get(self.output.name)
        self.assertTrue(copied_output.file_output_items[0].override_color_space)
        self.assertEqual(copied_output.file_output_items[1].format.linear_colorspace_settings.name, 'Non-Color')
        self.assertTrue(self.image.use_exr_part_color_spaces)
        self.image.use_exr_part_color_spaces = False
        path = OUT / 'settings.blend'
        bpy.ops.wm.save_as_mainfile(filepath=str(path))
        bpy.ops.wm.open_mainfile(filepath=str(path))
        tree = bpy.context.scene.compositing_node_group
        fo = next(n for n in tree.nodes if n.bl_idname == 'CompositorNodeOutputFile')
        self.assertEqual([(x.override_color_space, x.format.linear_colorspace_settings.name) for x in fo.file_output_items], [(True, 'sRGB'), (True, 'Non-Color')])
        self.assertFalse(bpy.data.images['Float Source'].use_exr_part_color_spaces)

    def test_07_roundtrip_and_global_override(self):
        self.add('Linear', 'Linear Rec.709')
        self.add('Encoded', 'sRGB')
        self.add('ACES', 'ACEScg')
        self.add('Raw', 'Non-Color')
        path, _ = self.render('_source')
        loaded = bpy.data.images.load(str(path), check_existing=False)
        self.assertTrue(loaded.use_exr_part_color_spaces)
        self.assertEqual(loaded.alpha_mode, 'PREMUL')
        node = self.tree.nodes.new('CompositorNodeImage')
        node.image = loaded
        self.output.file_output_items.clear()
        for name in ['Linear', 'Encoded', 'ACES', 'Raw']:
            self.assertIn(name, node.outputs.keys())
            self.add(name, 'Non-Color', source=node.outputs[name])
        _, parts = self.render('_read_per_part')
        for name in ['Linear', 'Encoded', 'ACES', 'Raw']:
            self.assert_color(parts[name], 'Non-Color', 3e-5)
        loaded.use_exr_part_color_spaces = False
        loaded.colorspace_settings.name = 'Linear Rec.709'
        _, parts = self.render('_read_global')
        np.testing.assert_allclose(values(parts['Encoded']), np.broadcast_to(transformed(RGBA, 'sRGB'), (32,32,4)), atol=3e-5)

    def test_08_crypto_and_pick_are_user_controlled(self):
        layer = self.scene.view_layers[0]
        layer.use_pass_cryptomatte_object = True
        layer.use_pass_cryptomatte_material = True
        rl = self.tree.nodes.new('CompositorNodeRLayers')
        crypto = self.tree.nodes.new('CompositorNodeCryptomatteV2')
        crypto.source = 'RENDER'
        crypto.scene = self.scene
        crypto.layer_name = layer.name + '.CryptoObject'
        for name, socket in [('Object', rl.outputs['CryptoObject00']),
                              ('Material', rl.outputs['CryptoMaterial00']),
                              ('Pick', crypto.outputs['Pick'])]:
            self.add(name + 'Raw', 'Non-Color', source=socket)
            self.add(name + 'Color', 'sRGB', source=socket)
            self.add(name + 'Inherit', source=socket)
        _, parts = self.render()
        for name in ['Object', 'Material', 'Pick']:
            raw, color = parts[name + 'Raw'], parts[name + 'Color']
            self.assertEqual(raw['tag'], 'data')
            self.assertEqual(color['tag'], 'srgb_rec709_display')
            source = values(raw).copy()
            reference = source.copy()
            alpha = reference[..., 3:4].copy()
            divisor = np.where((alpha == 0) | (alpha == 1), 1.0, alpha)
            reference[..., :3] /= divisor
            CONFIG.getProcessor('Linear Rec.709', 'sRGB').getDefaultCPUProcessor().apply(
                ocio.PackedImageDesc(reference, 32, 32, 4))
            reference[..., :3] *= divisor
            np.testing.assert_allclose(values(color), reference, rtol=3e-5, atol=2e-5)
            np.testing.assert_array_equal(values(color), values(parts[name + 'Inherit']))
        attrs = next(iter(parts.values()))['attrs']
        self.assertTrue(any(k.startswith('cryptomatte/') and k.endswith('/manifest') for k in attrs))
        self.assertTrue(np.any(values(parts['PickRaw'])[..., 1:3] != 0), 'Pick must contain real crypto data')

    def test_09_multiview(self):
        self.scene.render.use_multiview = True
        self.output.format.views_format = 'MULTIVIEW'
        self.add('Raw', 'Non-Color')
        self.add('Color', 'sRGB')
        _, parts = self.render()
        self.assertEqual(len(parts), 4)
        for name, part in parts.items():
            self.assert_color(part, 'Non-Color' if name.startswith('Raw') else 'sRGB')

    def test_10_gpu(self):
        self.scene.render.compositor_device = 'GPU'
        self.add('Raw', 'Non-Color')
        self.add('SRGB', 'sRGB')
        _, parts = self.render()
        self.assert_color(parts['Raw'], 'Non-Color')
        self.assert_color(parts['SRGB'], 'sRGB')

    def test_11_legacy_file(self):
        bpy.ops.wm.open_mainfile(filepath=str(LEGACY_FILE))
        tree = bpy.context.scene.compositing_node_group
        output = next(n for n in tree.nodes if n.bl_idname == 'CompositorNodeOutputFile')
        self.assertFalse(output.file_output_items[0].override_color_space)
        self.assertEqual(output.format.linear_colorspace_settings.name, 'sRGB')
        image = bpy.data.images['Legacy Image']
        self.assertFalse(image.use_exr_part_color_spaces)
        self.assertEqual(image.colorspace_settings.name, 'sRGB')

    def test_12_invalid_space_stops_file(self):
        import ctypes
        self.add('Valid', 'sRGB')
        item = self.add('Invalid', 'sRGB')
        # Simulate a saved color space that disappeared from a custom OCIO config.
        ctypes.memmove(item.format.linear_colorspace_settings.as_pointer(), b'Missing_EXR_Test_Space\0', 23)
        path = OUT / (self._testMethodName + '.exr')
        if path.exists():
            path.unlink()
        bpy.ops.render.render()
        self.assertFalse(path.exists(), 'A partially valid file must not be written')

    def test_13_undo(self):
        self.add('Layer', 'sRGB')
        self.tree.use_fake_user = True
        bpy.context.preferences.edit.use_global_undo = True
        bpy.ops.ed.undo_push(message='EXR layer sRGB')
        self.output.file_output_items[0].format.linear_colorspace_settings.name = 'ACEScg'
        bpy.ops.ed.undo_push(message='EXR layer ACEScg')
        bpy.ops.ed.undo()
        tree = bpy.context.scene.compositing_node_group
        output = next(n for n in tree.nodes if n.bl_idname == 'CompositorNodeOutputFile')
        self.assertTrue(output.file_output_items[0].override_color_space)
        self.assertEqual(output.file_output_items[0].format.linear_colorspace_settings.name, 'sRGB')

    def test_14_external_metadata_fallback(self):
        # An independent writer supplies tagged, untagged, unknown, and uppercase data parts.
        path = OUT / 'external_metadata.exr'
        definitions = [('Tagged', 'srgb_rec709_display'), ('Missing', None),
                       ('Unknown', 'test:unrecognized_space'), ('UpperData', 'data')]
        specs = []
        for name, tag in definitions:
            spec = oiio.ImageSpec(32, 32, 4, 'float')
            spec.channelnames = [name + '.' + c for c in 'RGBA']
            spec.attribute('name', name)
            if tag is not None:
                spec.attribute('colorInteropID', tag)
            specs.append(spec)
        writer = oiio.ImageOutput.create(str(path))
        self.assertTrue(writer.open(str(path), specs), writer.geterror())
        for index, (_, tag) in enumerate(definitions):
            if index:
                self.assertTrue(writer.open(str(path), specs[index], 'AppendSubimage'), writer.geterror())
            value = RGBA if tag == 'data' else transformed(RGBA, 'sRGB')
            self.assertTrue(writer.write_image(np.broadcast_to(value, (32, 32, 4)).copy()), writer.geterror())
        self.assertTrue(writer.close())
        parts = read_parts(path)
        self.assertNotIn('colorInteropID', parts['Missing']['attrs'])
        loaded = bpy.data.images.load(str(path), check_existing=False)
        self.assertEqual(loaded.alpha_mode, 'PREMUL')
        node = self.tree.nodes.new('CompositorNodeImage')
        node.image = loaded
        for name, _ in definitions:
            self.add(name, 'Non-Color', source=node.outputs[name])
        _, converted = self.render()
        for part in converted.values():
            self.assert_color(part, 'Non-Color', 3e-5)

    def test_15_generic_ocio_identifiers(self):
        spaces = [('NoID', 'Linear FilmLight E-Gamut'), ('OCIOID', 'Linear Apple Wide Gamut')]
        self.assertEqual(CONFIG.getColorSpace(spaces[0][1]).getInteropID(), '')
        for name, space in spaces:
            self.add(name, space)
        _, parts = self.render()
        self.assertNotIn('colorInteropID', parts['NoID']['attrs'])
        for name, space in spaces:
            if name == 'OCIOID':
                self.assertEqual(parts[name]['tag'], CONFIG.getColorSpace(space).getInteropID())
            np.testing.assert_allclose(values(parts[name]), np.broadcast_to(transformed(RGBA, space), (32, 32, 4)), atol=2e-5)

    def test_16_display_settings_are_not_baked(self):
        self.add('Encoded', 'sRGB')
        self.add('Raw', 'Non-Color')
        self.scene.view_settings.view_transform = 'AgX'
        self.scene.view_settings.exposure = 3.0
        self.scene.view_settings.gamma = 1.8
        _, parts = self.render()
        self.assert_color(parts['Encoded'], 'sRGB')
        self.assert_color(parts['Raw'], 'Non-Color')

    def test_17_alpha_roundtrip(self):
        pixels = np.tile(RGBA, (32, 32, 1))
        pixels[:, :8, 3] = 0.0
        pixels[:, 8:16, 3] = 1.0
        pixels[:, 16:24, 3] = 0.01
        self.image.pixels.foreach_set(pixels.ravel())
        self.image.update()
        self.add('Encoded', 'sRGB')
        self.add('Raw', 'Non-Color')
        path, parts = self.render('_source')
        # EXR scanlines are top down, but this source is constant vertically.
        np.testing.assert_array_equal(values(parts['Encoded'])[..., 3], pixels[..., 3])
        np.testing.assert_array_equal(values(parts['Raw']), pixels)
        loaded = bpy.data.images.load(str(path), check_existing=False)
        self.assertEqual(loaded.alpha_mode, 'PREMUL')
        node = self.tree.nodes.new('CompositorNodeImage')
        node.image = loaded
        self.output.file_output_items.clear()
        self.add('Restored', 'Non-Color', source=node.outputs['Encoded'])
        _, restored = self.render('_read')
        np.testing.assert_allclose(values(restored['Restored']), pixels, rtol=0, atol=3e-5)

    def test_18_opaque_and_transparent_preview(self):
        pixels = np.tile(np.array([0.18, 0.32, 0.65, 1.0], np.float32), (32, 32, 1))
        pixels[:, 16:, 3] = 0.35
        self.image.pixels.foreach_set(pixels.ravel())
        self.image.update()
        for name in ['Linear Rec.709', 'sRGB', 'ACEScg', 'Non-Color']:
            self.add(name, name)
        _, parts = self.render()
        for name, part in parts.items():
            for x in (0, 31):
                expected = pixels[0, x] if name == 'Non-Color' else transformed(pixels[0, x], name)
                np.testing.assert_allclose(values(part)[0, x], expected, rtol=0, atol=2e-5)


def run_tests(output_dir):
    global OUT
    OUT = Path(output_dir).resolve()
    OUT.mkdir(parents=True, exist_ok=True)
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ExrLayerColorSpaces))
    (OUT / 'result.json').write_text(json.dumps({'tests': result.testsRun, 'failures': len(result.failures),
                                               'errors': len(result.errors), 'success': result.wasSuccessful()}, indent=2), encoding='utf-8')
    if not result.wasSuccessful():
        raise SystemExit(1)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])
    run_tests(args.output_dir)
