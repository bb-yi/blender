# SPDX-License-Identifier: GPL-2.0-or-later
"""Pixel regressions for outline region masks, priorities, identity and expansion masks.

Run only in an isolated Blender configuration. All files and scenes are synthetic.
"""

import json
import os
from pathlib import Path
import sys
import unittest

import bpy
import gpu
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_outline_visibility as visibility
import test_outline_performance as performance

OUTPUT = Path(os.environ['BLENDER_TEST_OUTPUT_DIR'])
REPORT = {'cases': {}, 'passed': False}
SIZE = 256
CENTER = (slice(80, 176), slice(117, 139))


def setup():
    if bpy.app.background:
        scene = visibility.setup_scene()
    else:
        # Keep the owned test window and its timer context alive between fixtures.
        for obj in list(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        scene = bpy.context.scene
        scene.render.engine = 'BLENDER_EEVEE'
        scene.render.resolution_x = scene.render.resolution_y = SIZE
        scene.render.resolution_percentage = 100
        scene.render.use_compositing = False
        scene.eevee.use_outline = True
        scene.eevee.outline_surface_identity = 'LEGACY'
        scene.eevee.outline_viewport_resolution = 'FULL'
        scene.eevee.taa_render_samples = scene.eevee.taa_samples = 1
        scene.eevee.use_taa_reprojection = False
        scene.world = bpy.data.worlds.new('Window outline controls world')
        scene.world.use_nodes = True
        scene.world.node_tree.nodes['Background'].inputs['Strength'].default_value = 0
        camera = bpy.data.objects.new('Camera', bpy.data.cameras.new('Camera'))
        scene.collection.objects.link(camera)
        camera.location = (0, 0, 5)
        camera.data.type = 'ORTHO'
        camera.data.ortho_scale = 4
        scene.camera = camera
    scene.eevee.use_raytracing = False
    scene.view_layers[0].eevee.use_pass_outline = True
    scene.view_layers[0].use_pass_z = True
    scene.view_layers[0].use_pass_vector = True
    scene.render.image_settings.media_type = 'MULTI_LAYER_IMAGE'
    scene.render.image_settings.file_format = 'OPEN_EXR_MULTILAYER'
    return scene


def material(name, width=12, outline_id=7, alpha=1, method='DITHERED'):
    mat = visibility.material(name, outline=True, width=width, outline_id=outline_id,
                              alpha=alpha, method=method)
    if method == 'BLENDED':
        mat.use_transparency_overlap = False
    return mat, mat.node_tree.nodes.get('Outline Control')


def render(label):
    OUTPUT.mkdir(parents=True, exist_ok=True)
    path = OUTPUT / (label + '.exr')
    bpy.context.scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)
    pixels, names = performance.exr_pixels(path)
    outline = performance.outline_channels(pixels, names)
    REPORT['cases'][label] = {'alpha_sum': float(outline[..., 3].sum()),
                              'covered': int((outline[..., 3] > 0).sum())}
    print('OUTLINE_CONTROLS_RENDER', label, REPORT['cases'][label], flush=True)
    return outline


def region_surface(kind='flat'):
    """One instance with hard face IDs, optionally stepped, folded or sloped."""
    mat, control = material('Region surface')
    if kind == 'step':
        vertices = [(-1.2, -1, 0), (0, -1, 0), (0, 1, 0), (-1.2, 1, 0),
                    (0, -1, .35), (1.2, -1, .35), (1.2, 1, .35), (0, 1, .35)]
        faces = [(0, 1, 2, 3), (4, 5, 6, 7)]
    elif kind == 'slope':
        vertices = [(x, y, x * .5) for y in (-1, 1) for x in (-1.2, 0, .06, 1.2)]
        faces = [(0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6)]
    else:
        heights = (0, 0, .8) if kind == 'crease' else (0, 0, 0)
        vertices = [(x, y, z) for y in (-1, 1) for x, z in zip((-1.2, 0, 1.2), heights)]
        faces = [(0, 1, 4, 3), (1, 2, 5, 4)]
    mesh = bpy.data.meshes.new('Synthetic region mesh')
    mesh.from_pydata(vertices, [], faces)
    regions = mesh.attributes.new('outline_test_region', 'FLOAT', 'FACE')
    for index, value in enumerate(regions.data):
        value.value = index + 1
    obj = bpy.data.objects.new('Synthetic region surface', mesh)
    bpy.context.scene.collection.objects.link(obj)
    mesh.materials.append(mat)
    attr = mat.node_tree.nodes.new('ShaderNodeAttribute')
    attr.attribute_name = regions.name
    mat.node_tree.links.new(attr.outputs['Fac'], control.inputs['Outline ID'])
    if kind == 'slope':
        # Only the narrow middle region emits a line. Its wide line must be allowed to
        # reach the nearer third region on this same continuous surface.
        enabled = mesh.attributes.new('outline_test_seed', 'FLOAT', 'FACE')
        for index, value in enumerate(enabled.data):
            value.value = float(index == 1)
        seed_attr = mat.node_tree.nodes.new('ShaderNodeAttribute')
        seed_attr.attribute_name = enabled.name
        mat.node_tree.links.new(seed_attr.outputs['Fac'], control.inputs['ID Edge'])
    control.inputs['Depth Threshold'].default_value = .1
    control.inputs['Normal Threshold'].default_value = .05
    control.inputs['Depth Edge Width'].default_value = 0
    control.inputs['Normal Edge Width'].default_value = 0
    return obj, mat, control


def receiving_mask_surface(alpha=1, method='DITHERED'):
    source, source_node = material('Red marked source', width=24, outline_id=1)
    source_node.inputs['ID Edge'].default_value = False
    source_node.inputs['Freestyle Edge'].default_value = True
    receiver, receiver_node = material('Mask receiver', width=0, outline_id=2,
                                        alpha=alpha, method=method)
    receiver_node.inputs['Line Alpha'].default_value = 0
    vertices = [(x, y, 0) for y in (-1, 1) for x in (-1.2, -.1, 0, 1.2)]
    mesh = bpy.data.meshes.new('Mask surface')
    mesh.from_pydata(vertices, [], [(0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6)])
    mesh.materials.append(source)
    mesh.materials.append(receiver)
    mesh.polygons[2].material_index = 1
    marks = mesh.attributes.new('freestyle_edge', 'BOOLEAN', 'EDGE')
    for edge in mesh.edges:
        marks.data[edge.index].value = set(edge.vertices) == {1, 5}
    obj = bpy.data.objects.new('Mask surface', mesh)
    bpy.context.scene.collection.objects.link(obj)
    return obj, source_node, receiver_node


def image_texture(mat, values):
    values = np.asarray(values, dtype=np.float32)
    height, width = values.shape
    image = bpy.data.images.new('Outline control test image', width=width, height=height)
    # Color-space changes reload generated buffers; configure it before authoring pixels.
    image.colorspace_settings.name = 'Non-Color'
    rgba = np.ones((height, width, 4), dtype=np.float32)
    rgba[..., :3] = values[..., None]
    image.pixels.foreach_set(rgba.ravel())
    image.update()
    image.pack()
    actual = np.asarray(image.pixels[:]).reshape(height, width, 4)[..., 0]
    np.testing.assert_allclose(actual, values, atol=1 / 255)
    texture = mat.node_tree.nodes.new('ShaderNodeTexImage')
    texture.image = image
    texture.interpolation = 'Closest'
    texture.extension = 'EXTEND'
    coords = mat.node_tree.nodes.new('ShaderNodeTexCoord')
    mat.node_tree.links.new(coords.outputs['Generated'], texture.inputs['Vector'])
    return texture


class OutlineControlsTest(unittest.TestCase):
    def test_curved_texture_regions(self):
        scene = setup()
        mat, control = material('Curved texture regions', width=36)
        bpy.ops.mesh.primitive_uv_sphere_add(segments=64, ring_count=32, radius=.9)
        obj = bpy.context.object
        obj.data.materials.append(mat)
        for face in obj.data.polygons:
            face.use_smooth = True
        values = np.zeros((1, 20), dtype=np.float32)
        values[:, 14:16] = .5
        values[:, 16:] = 1
        texture = image_texture(mat, values)
        multiply = mat.node_tree.nodes.new('ShaderNodeMath')
        multiply.operation = 'MULTIPLY'
        multiply.inputs[1].default_value = 2
        add = mat.node_tree.nodes.new('ShaderNodeMath')
        add.operation = 'ADD'
        add.inputs[1].default_value = 1
        compare = mat.node_tree.nodes.new('ShaderNodeMath')
        compare.operation = 'COMPARE'
        compare.inputs[1].default_value = .5
        compare.inputs[2].default_value = .05
        mat.node_tree.links.new(texture.outputs['Color'], multiply.inputs[0])
        mat.node_tree.links.new(multiply.outputs[0], add.inputs[0])
        mat.node_tree.links.new(add.outputs[0], control.inputs['Outline ID'])
        mat.node_tree.links.new(texture.outputs['Color'], compare.inputs[0])
        mat.node_tree.links.new(compare.outputs[0], control.inputs['ID Edge'])
        legacy = render('curved_texture_legacy')
        scene.eevee.outline_surface_identity = 'OBJECT_INSTANCE'
        instance = render('curved_texture_instance')
        roi = (slice(112, 144), slice(146, 150))
        self.assertEqual(float(legacy[roi][..., 3].sum()), 0)
        self.assertGreater(instance[roi][..., 3].sum(), 40)

    def test_texture_expansion_mask(self):
        setup()
        _, _, receiver = receiving_mask_surface()
        reference = render('texture_mask_reference')
        mat = bpy.data.materials['Mask receiver']
        values = np.zeros((8, 8), dtype=np.float32)
        values[4:] = 1
        texture = image_texture(mat, values)
        mat.node_tree.links.new(texture.outputs['Color'], receiver.inputs['Expansion Mask'])
        result = render('texture_mask_result')
        # OpenImageIO returns top-to-bottom rows; Blender's authored image pixels start below.
        protected = (slice(144, 171), slice(129, 135))
        allowed = (slice(85, 112), slice(129, 135))
        self.assertGreater(reference[protected][..., 3].sum(), 50)
        self.assertEqual(float(result[protected][..., 3].sum()), 0)
        np.testing.assert_array_equal(reference[allowed], result[allowed])

    def test_visible_seed_competition(self):
        scene = setup()
        near_mat, near_node = material('Near marked red', width=6, outline_id=2)
        near_node.inputs['ID Edge'].default_value = False
        near_node.inputs['Freestyle Edge'].default_value = True
        mesh = bpy.data.meshes.new('Near marked surface')
        mesh.from_pydata([(x, y, 1) for y in (-1, 1) for x in (-.8, .65, .8)],
                         [], [(0, 1, 4, 3), (1, 2, 5, 4)])
        obj = bpy.data.objects.new('Near marked surface', mesh)
        scene.collection.objects.link(obj)
        mesh.materials.append(near_mat)
        marks = mesh.attributes.new('freestyle_edge', 'BOOLEAN', 'EDGE')
        for edge in mesh.edges:
            marks.data[edge.index].value = set(edge.vertices) == {1, 4}
        far_mat, far_node = material('Far wide green', width=64, outline_id=3)
        far_node.inputs['Line Color'].default_value = (0, 1, 0, 1)
        far = visibility.plane('Far wide', far_mat, x=1.7, z=0, size=1.6)
        roi = (slice(86, 170), slice(164, 175))
        far.hide_render = True
        reference = render('competition_near_only_legacy')
        far.hide_render = False
        legacy = render('competition_legacy')
        reference_red = reference[roi][..., 0] > .1
        lost = reference_red & (legacy[roi][..., 0] <= .1)
        self.assertGreater(int(lost.sum()), 200)
        scene.eevee.outline_surface_identity = 'OBJECT_INSTANCE'
        far.hide_render = True
        reference = render('competition_near_only_instance')
        far.hide_render = False
        visible = render('competition_instance')
        np.testing.assert_array_equal(reference[roi], visible[roi])
        REPORT['competition'] = {'legacy_lost_pixels': int(lost.sum()), 'instance_lost_pixels': 0}

    def test_instance_width_coverage(self):
        scene = setup()
        scene.eevee.outline_surface_identity = 'OBJECT_INSTANCE'
        wide_mat, _ = material('Wide', width=30, outline_id=1)
        visibility.plane('Wide', wide_mat, x=-.9, size=1.6)
        narrow_mat, narrow = material('Narrow', width=2, outline_id=2)
        neighbor = visibility.plane('Neighbor', narrow_mat, x=.9, size=1.6)
        neighbor.hide_render = True
        reference = render('instance_wide_reference')
        gap = (slice(90, 165), slice(122, 134))
        self.assertGreater((reference[gap][..., 3] > .1).sum(), 500)
        neighbor.hide_render = False
        for factor in (0, .25, 1):
            narrow.inputs['ID Edge Width'].default_value = factor
            result = render('instance_wide_neighbor_' + str(factor))
            lost = (reference[gap][..., 3] > .1) & (result[gap][..., 3] <= .1)
            self.assertEqual(int(lost.sum()), 0)

    def test_old_file_defaults(self):
        path = Path(__file__).resolve().parent / 'data' / 'outline_legacy_50202.blend'
        bpy.ops.wm.open_mainfile(filepath=str(path))
        self.assertEqual(bpy.context.scene.eevee.outline_surface_identity, 'LEGACY')
        self.assertEqual(bpy.context.scene.eevee.outline_viewport_resolution, 'FULL')
        found = 0
        for mat in bpy.data.materials:
            if not mat.node_tree:
                continue
            for node in mat.node_tree.nodes:
                if node.bl_idname == 'ShaderNodeOutlineControl':
                    found += 1
                    self.assertEqual(node.id_combine, 'LEGACY')
                    self.assertEqual(node.edge_priority, 'LEGACY')
                    self.assertEqual(node.inputs['Expansion Mask'].default_value, 1)
                    for name in ('Line Width', 'Outline ID', 'Depth Threshold Range', 'Freestyle Edge'):
                        self.assertIn(name, node.inputs)
        self.assertGreater(found, 0)

    def test_legacy_outline_and_pre_fx_passes(self):
        path = Path(__file__).resolve().parent / 'data' / 'legacy_52_outline_pre_fx.blend'
        bpy.ops.wm.open_mainfile(filepath=str(path))
        layer = bpy.context.scene.view_layers[0]
        self.assertTrue(layer.eevee.use_pass_outline)
        self.assertTrue(layer.use_pass_pre_fx)

    def test_defaults_and_file_roundtrip(self):
        scene = setup()
        _, mat, control = region_surface()
        self.assertEqual(scene.eevee.outline_surface_identity, 'LEGACY')
        self.assertEqual(control.id_combine, 'LEGACY')
        self.assertEqual(control.edge_priority, 'LEGACY')
        self.assertEqual(control.inputs['Expansion Mask'].default_value, 1)
        original_identifiers = [socket.identifier for socket in control.inputs]
        scene.eevee.outline_surface_identity = 'OBJECT_INSTANCE'
        control.id_combine = 'DEPTH_OR_NORMAL'
        control.edge_priority = 'ID'
        control.inputs['Expansion Mask'].default_value = .75
        control.inputs['Line Width'].default_value = 6
        control.inputs['Line Width'].keyframe_insert(data_path='default_value', frame=1)
        control.inputs['Line Width'].default_value = 12
        control.inputs['Line Width'].keyframe_insert(data_path='default_value', frame=2)
        scene.frame_set(2)
        before = render('roundtrip_before')
        assets = Path(os.environ.get('BLENDER_TEST_ASSET_DIR', str(OUTPUT / 'assets')))
        assets.mkdir(parents=True, exist_ok=True)
        path = assets / 'outline_controls.blend'
        material_name = mat.name
        bpy.ops.wm.save_as_mainfile(filepath=str(path))
        bpy.ops.wm.open_mainfile(filepath=str(path))
        scene = bpy.context.scene
        control = bpy.data.materials[material_name].node_tree.nodes.get('Outline Control')
        self.assertEqual(scene.eevee.outline_surface_identity, 'OBJECT_INSTANCE')
        self.assertEqual(control.id_combine, 'DEPTH_OR_NORMAL')
        self.assertEqual(control.edge_priority, 'ID')
        self.assertEqual(control.inputs['Expansion Mask'].default_value, .75)
        self.assertTrue(control.inputs['Outline ID'].is_linked)
        self.assertEqual([socket.identifier for socket in control.inputs], original_identifiers)
        scene.frame_set(1)
        self.assertEqual(control.inputs['Line Width'].default_value, 6)
        scene.frame_set(2)
        np.testing.assert_array_equal(before, render('roundtrip_after'))

    def test_id_masks(self):
        setup()
        _, _, control = region_surface('flat')
        reference = render('flat_original')
        self.assertGreater(reference[CENTER][..., 3].sum(), 500)
        for mode in ('DEPTH', 'NORMAL', 'DEPTH_OR_NORMAL'):
            control.id_combine = mode
            masked = render('flat_' + mode)
            self.assertEqual(float(masked[CENTER][..., 3].sum()), 0)
        control.id_combine = 'LEGACY'
        np.testing.assert_array_equal(reference, render('flat_return_original'))
        for kind, mode in [('step', 'DEPTH'), ('crease', 'NORMAL')]:
            setup()
            _, _, control = region_surface(kind)
            control.id_combine = mode
            masked = render(kind + '_masked')
            self.assertGreater(masked[CENTER][..., 3].sum(), 300)
            control.id_combine = 'DEPTH_OR_NORMAL'
            combined = render(kind + '_combined')
            np.testing.assert_array_equal(masked[CENTER], combined[CENTER])

    def test_category_priority(self):
        setup()
        _, _, control = region_surface('crease')
        control.inputs['ID Edge Width'].default_value = .25
        control.inputs['Normal Edge Width'].default_value = 1
        original = render('priority_original')
        control.edge_priority = 'ID'
        thin = render('priority_id')
        self.assertLess(thin[CENTER][..., 3].sum(), original[CENTER][..., 3].sum() * .65)
        control.edge_priority = 'NORMAL'
        np.testing.assert_array_equal(original, render('priority_normal'))
        control.edge_priority = 'DEPTH'
        np.testing.assert_array_equal(original, render('priority_missing_fallback'))
        setup()
        _, _, control = region_surface('step')
        control.inputs['Depth Edge Width'].default_value = .25
        original = render('priority_step_original')
        control.edge_priority = 'DEPTH'
        thin = render('priority_depth')
        self.assertLess(thin[CENTER][..., 3].sum(), original[CENTER][..., 3].sum() * .65)

    def test_instance_identity(self):
        scene = setup()
        _, _, control = region_surface('slope')
        legacy = render('slope_legacy')
        scene.eevee.outline_surface_identity = 'OBJECT_INSTANCE'
        instance = render('slope_instance')
        self.assertGreater(instance[CENTER][..., 3].sum(), legacy[CENTER][..., 3].sum() * 1.3)
        scene.eevee.outline_surface_identity = 'LEGACY'
        np.testing.assert_array_equal(legacy, render('slope_return_legacy'))
        scene = setup()
        far_mat, far_node = material('Far red', width=64, outline_id=7)
        far = visibility.plane('Far', far_mat, x=-.9, z=0, size=1.6)
        near_mat, near_node = material('Near unlined', width=1, outline_id=7)
        near_node.inputs['ID Edge'].default_value = False
        near = far.copy()
        near.data = far.data
        near.name = 'Linked near instance'
        scene.collection.objects.link(near)
        near.location = (.9, 0, .5)
        near.material_slots[0].link = 'OBJECT'
        near.material_slots[0].material = near_mat
        roi = (slice(86, 170), slice(140, 149))
        legacy = render('shared_id_legacy')
        self.assertGreater(legacy[roi][..., 3].sum(), 500)
        scene.eevee.outline_surface_identity = 'OBJECT_INSTANCE'
        instance = render('shared_id_instance')
        self.assertEqual(float(instance[roi][..., 3].sum()), 0)

    def test_expansion_receiver_mask(self):
        for method in ('DITHERED', 'BLENDED'):
            setup()
            _, _, receiver = receiving_mask_surface(method=method)
            reference = render('mask_allowed_' + method)
            roi = (slice(70, 186), slice(129, 135))
            self.assertGreater(reference[roi][..., 3].sum(), 100)
            receiver.inputs['Expansion Mask'].default_value = 0
            blocked = render('mask_blocked_' + method)
            self.assertEqual(float(blocked[roi][..., 3].sum()), 0)
            np.testing.assert_array_equal(reference[:, :126], blocked[:, :126])
            receiver.inputs['Expansion Mask'].default_value = .5
            half = render('mask_half_' + method)
            ratio = float(half[roi][..., 3].sum() / reference[roi][..., 3].sum())
            self.assertAlmostEqual(ratio, .5, delta=.01)
            receiver.inputs['Expansion Mask'].default_value = 1
            np.testing.assert_array_equal(reference, render('mask_restored_' + method))
            setup()
            _, _, receiver = receiving_mask_surface(alpha=0, method=method)
            reference = render('transparent_allowed_' + method)
            receiver.inputs['Expansion Mask'].default_value = 0
            np.testing.assert_array_equal(reference, render('transparent_mask_' + method))
        setup()
        _, _, receiver = receiving_mask_surface(alpha=.5, method='BLENDED')
        reference = render('translucent_allowed')
        receiver.inputs['Expansion Mask'].default_value = 0
        masked = render('translucent_mask')
        ratio = float(masked[roi][..., 3].sum() / reference[roi][..., 3].sum())
        self.assertAlmostEqual(ratio, .5, delta=.02)

    def test_refraction_receiver_mask(self):
        for identity in ('LEGACY', 'OBJECT_INSTANCE'):
            for alpha in (1, 0):
                scene = setup()
                scene.eevee.use_raytracing = True
                scene.eevee.outline_surface_identity = identity
                _, _, receiver = receiving_mask_surface(alpha=alpha)
                bpy.data.materials['Mask receiver'].use_raytrace_refraction = True
                label = f'refraction_mask_{identity}_{alpha}'
                reference = render(label + '_allow')
                receiver.inputs['Expansion Mask'].default_value = 0
                blocked = render(label + '_block')
                roi = (slice(80, 176), slice(129, 135))
                self.assertGreater(reference[roi][..., 3].sum(), 100)
                if alpha == 1:
                    self.assertEqual(float(blocked[roi][..., 3].sum()), 0)
                    np.testing.assert_array_equal(reference[:, :126], blocked[:, :126])
                else:
                    np.testing.assert_array_equal(reference, blocked)


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    gpu.init()
    REPORT['backend'] = gpu.platform.backend_type_get()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(OutlineControlsTest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    REPORT['passed'] = result.wasSuccessful()
    REPORT['tests_run'] = result.testsRun
    (OUTPUT / 'summary.json').write_text(json.dumps(REPORT, indent=2))
    assert result.wasSuccessful(), 'Outline control regressions failed'
    print('OUTLINE_CONTROLS_OK', flush=True)


if __name__ == '__main__':
    main()
