# SPDX-License-Identifier: GPL-2.0-or-later
"""Rendered regressions for outline source geometry, coverage and marked-edge displacement.

Run in an isolated Blender process with BLENDER_TEST_OUTPUT_DIR set. Each GPU backend runs
the same pixel assertions; no user scene or preferences are loaded or saved.
"""

import json
import os
from pathlib import Path
import sys
import unittest

import bpy
import gpu
import numpy as np


RESOLUTION = 256
OUTPUT = Path(os.environ['BLENDER_TEST_OUTPUT_DIR'])
REPORT = {}


def setup_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    scene.render.resolution_x = scene.render.resolution_y = RESOLUTION
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.render.image_settings.color_mode = 'RGBA'
    scene.render.image_settings.color_depth = '32'
    scene.render.use_compositing = False
    scene.eevee.use_outline = True
    scene.eevee.use_raytracing = True
    scene.eevee.taa_render_samples = 1
    scene.eevee.use_taa_reprojection = False
    scene.world = bpy.data.worlds.new('Outline World')
    scene.world.use_nodes = True
    scene.world.node_tree.nodes['Background'].inputs['Strength'].default_value = 0
    camera = bpy.data.objects.new('Camera', bpy.data.cameras.new('Camera'))
    scene.collection.objects.link(camera)
    camera.location = (0, 0, 5)
    camera.data.type = 'ORTHO'
    camera.data.ortho_scale = 4
    scene.camera = camera
    return scene


def material(name, *, outline=False, width=10, outline_id=1, alpha=1, method='DITHERED'):
    result = bpy.data.materials.new(name)
    result.use_nodes = True
    result.surface_render_method = method
    tree = result.node_tree
    tree.nodes.clear()
    output = tree.nodes.new('ShaderNodeOutputMaterial')
    emission = tree.nodes.new('ShaderNodeEmission')
    emission.inputs['Color'].default_value = (0.04, 0.1, 0.3, 1)
    surface = emission.outputs[0]
    if alpha < 1:
        transparent = tree.nodes.new('ShaderNodeBsdfTransparent')
        mix = tree.nodes.new('ShaderNodeMixShader')
        mix.inputs[0].default_value = alpha
        tree.links.new(transparent.outputs[0], mix.inputs[1])
        tree.links.new(surface, mix.inputs[2])
        surface = mix.outputs[0]
    tree.links.new(surface, output.inputs['Surface'])
    if outline:
        node = tree.nodes.new('ShaderNodeOutlineControl')
        for key, value in {'Line Color': (1, 0, 0, 1), 'Line Width': width,
                           'Depth Threshold': 1, 'Normal Threshold': 1,
                           'Outline ID': outline_id, 'ID Edge': True,
                           'Freestyle Edge': False}.items():
            node.inputs[key].default_value = value
    return result


def plane(name, mat, *, x=0, z=0, size=2):
    bpy.ops.mesh.primitive_plane_add(size=size, location=(x, 0, z))
    obj = bpy.context.object
    obj.name = name
    obj.data.materials.append(mat)
    return obj


def red_mask(pixels):
    return ((pixels[..., 0] > .1) & (pixels[..., 0] > 3 * pixels[..., 1]) &
            (pixels[..., 0] > 2 * pixels[..., 2]))


def render(label):
    OUTPUT.mkdir(parents=True, exist_ok=True)
    path = OUTPUT / (label + '.exr')
    scene = bpy.context.scene
    scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        pixels = np.array(image.pixels[:], dtype=np.float32).reshape(
            RESOLUTION, RESOLUTION, 4)
    finally:
        bpy.data.images.remove(image)
    if not np.isfinite(pixels).all():
        raise AssertionError(f'Non-finite pixels in {label}')
    mask = red_mask(pixels)
    REPORT[label] = {'red_pixels': int(mask.sum()), 'red_sum': float(pixels[..., 0][mask].sum())}
    print('OUTLINE_VISIBILITY_RENDER', label, REPORT[label], flush=True)
    return pixels


def viewport_render(label):
    scene = bpy.context.scene
    scene.eevee.taa_samples = 1
    gpu.init()
    area = next(area for area in bpy.context.screen.areas if area.type == 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    space = area.spaces.active
    space.shading.type = 'RENDERED'
    space.overlay.show_overlays = False
    space.shading.use_scene_world_render = True
    space.shading.use_scene_lights_render = True
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    projection = scene.camera.calc_matrix_camera(depsgraph, x=RESOLUTION, y=RESOLUTION)
    buffer = gpu.types.GPUOffScreen(RESOLUTION, RESOLUTION, format='RGBA16F')
    try:
        buffer.draw_view3d(scene, bpy.context.view_layer, space, region,
                           scene.camera.matrix_world.inverted(), projection,
                           do_color_management=False, draw_background=True)
        pixels = np.array(buffer.texture_color.read(), dtype=np.float32).reshape(
            RESOLUTION, RESOLUTION, 4)
    finally:
        buffer.free()
    if not np.isfinite(pixels).all():
        raise AssertionError(f'Non-finite viewport pixels in {label}')
    np.save(OUTPUT / (label + '.npy'), pixels)
    mask = red_mask(pixels)
    REPORT[label] = {'red_pixels': int(mask.sum())}
    print('OUTLINE_VISIBILITY_VIEWPORT', label, REPORT[label], flush=True)
    return pixels


class OutlineVisibilityTest(unittest.TestCase):
    def test_transmission_foreground_and_background(self):
        setup_scene()
        plane('Outlined', material('Body', outline=True))
        blocker_material = material('Blocker')
        blocker = plane('Blocker', blocker_material, size=8)
        backgrounds = []
        for position, z in (('behind', -1), ('front', 1)):
            blocker.location.z = z
            for enabled in (False, True):
                blocker_material.use_raytrace_refraction = enabled
                pixels = render(f'transmission_{position}_{enabled}')
                mask = red_mask(pixels)
                if position == 'front':
                    self.assertEqual(int(mask.sum()), 0, 'Foreground leaked background outline')
                else:
                    self.assertGreater(int(mask.sum()), 2000)
                    backgrounds.append(mask)
        np.testing.assert_array_equal(backgrounds[0], backgrounds[1])

    def test_blended_outline_is_independent_of_background(self):
        values = {}
        for alpha in (0, .5, 1):
            for background in (False, True):
                setup_scene()
                if background:
                    background_mat = material('Background')
                    # Match the world color so this compares geometry/depth, not color blending.
                    background_mat.node_tree.nodes.get('Emission').inputs['Color'].default_value = (0, 0, 0, 1)
                    plane('Background', background_mat, z=-1, size=8)
                plane('Outlined', material('Body', outline=True, alpha=alpha, method='BLENDED'))
                pixels = render(f'blended_alpha_{alpha}_background_{background}')
                mask = red_mask(pixels)
                values[alpha, background] = (pixels, mask)
                if alpha == 0:
                    self.assertEqual(int(mask.sum()), 0, 'Fully transparent surface left outline')
                else:
                    self.assertGreater(int(mask.sum()), 2000, 'Visible blended outline disappeared')
        for alpha in (.5, 1):
            np.testing.assert_array_equal(values[alpha, False][1], values[alpha, True][1])
        half, mask = values[.5, False]
        full, _ = values[1, False]
        self.assertAlmostEqual(float(half[..., 0][mask].sum() / full[..., 0][mask].sum()),
                               .5, delta=.04)

    def test_inactive_and_narrow_seeds_do_not_cut_wide_outline(self):
        setup_scene()
        plane('Wide', material('Wide', outline=True, width=30), x=-.9, size=1.6)
        neighbor_material = material('Neighbor', outline=True, width=2, outline_id=2)
        neighbor = plane('Neighbor', neighbor_material, x=.9, size=1.6)
        node = neighbor_material.node_tree.nodes.get('Outline Control')
        neighbor.hide_render = True
        reference = red_mask(render('wide_without_neighbor'))
        # The gap contains no geometry from either plane. Adding a neighbor must not erase ink.
        gap = (slice(90, 165), slice(122, 134))
        self.assertGreater(int(reference[gap].sum()), 500)
        neighbor.hide_render = False
        for factor in (0, .25, 1):
            node.inputs['ID Edge Width'].default_value = factor
            pixels = render(f'wide_with_neighbor_factor_{factor}')
            lost = reference[gap] & ~red_mask(pixels)[gap]
            self.assertEqual(int(lost.sum()), 0, f'Neighbor factor {factor} erased wide contour')

    def test_freestyle_follows_material_displacement(self):
        for mode in ('constant', 'attribute', 'normal'):
            with self.subTest(mode=mode):
                setup_scene()
                mat = material('Marked', outline=True, width=6)
                tree = mat.node_tree
                outline = tree.nodes.get('Outline Control')
                outline.inputs['ID Edge'].default_value = False
                outline.inputs['Freestyle Edge'].default_value = True
                obj = plane('Marked', mat, x=-.5, size=1.5)
                marks = obj.data.attributes.new('freestyle_edge', 'BOOLEAN', 'EDGE')
                for value in marks.data:
                    value.value = True
                mat.displacement_method = 'BOTH'
                vector = (1, 0, 0) if mode != 'normal' else (0, 0, .25)
                if mode == 'attribute':
                    attr = obj.data.attributes.new('edge_offset', 'FLOAT_VECTOR', 'POINT')
                    for value in attr.data:
                        value.vector = vector
                    source = tree.nodes.new('ShaderNodeAttribute')
                    source.attribute_name = 'edge_offset'
                    socket = source.outputs['Vector']
                elif mode == 'normal':
                    geometry = tree.nodes.new('ShaderNodeNewGeometry')
                    source = tree.nodes.new('ShaderNodeVectorMath')
                    source.operation = 'SCALE'
                    source.inputs['Scale'].default_value = .25
                    tree.links.new(geometry.outputs['Normal'], source.inputs[0])
                    socket = source.outputs[0]
                else:
                    source = tree.nodes.new('ShaderNodeCombineXYZ')
                    source.inputs['X'].default_value = 1
                    socket = source.outputs[0]
                link = tree.links.new(socket, tree.nodes['Material Output'].inputs['Displacement'])
                displaced = render('freestyle_' + mode + '_displaced')
                tree.links.remove(link)
                obj.location.x += vector[0]
                obj.location.z += vector[2]
                translated = render('freestyle_' + mode + '_translated')
                self.assertGreater(int(red_mask(translated).sum()), 800)
                np.testing.assert_allclose(displaced, translated, atol=.005, rtol=0,
                                           err_msg=f'Freestyle {mode} displacement differs from geometry')

    def test_freestyle_respects_face_materials(self):
        for subdiv in (False, True):
            with self.subTest(subdiv=subdiv):
                setup_scene()
                bpy.context.preferences.system.use_gpu_subdivision = True
                gpu.init()
                base = material('Base', outline=True, width=6)
                displaced = material('Displaced', outline=True, width=6, outline_id=2)
                for mat in (base, displaced):
                    outline = mat.node_tree.nodes.get('Outline Control')
                    outline.inputs['ID Edge'].default_value = False
                    outline.inputs['Freestyle Edge'].default_value = True
                displaced.displacement_method = 'BOTH'
                tree = displaced.node_tree
                vector = tree.nodes.new('ShaderNodeCombineXYZ')
                vector.inputs['X'].default_value = 1
                tree.links.new(vector.outputs[0], tree.nodes['Material Output'].inputs['Displacement'])
                mesh = bpy.data.meshes.new('Two material quads')
                mesh.from_pydata([
                    (-1.25, -1.25, 0), (1.25, -1.25, 0), (1.25, 1.25, 0), (-1.25, 1.25, 0),
                    (.25, -.25, .1), (.75, -.25, .1), (.75, .25, .1), (.25, .25, .1),
                ], [], [(0, 1, 2, 3), (4, 5, 6, 7)])
                obj = bpy.data.objects.new('Two materials', mesh)
                bpy.context.scene.collection.objects.link(obj)
                mesh.materials.append(base)
                mesh.materials.append(base)  # An unused slot between the two used materials.
                mesh.materials.append(displaced)
                mesh.polygons[1].material_index = 2
                marks = mesh.attributes.new('freestyle_edge', 'BOOLEAN', 'EDGE')
                for edge, value in zip(mesh.edges, marks.data):
                    value.value = edge.vertices[0] >= 4
                if subdiv:
                    modifier = obj.modifiers.new('Subdiv', 'SUBSURF')
                    modifier.subdivision_type = 'SIMPLE'
                    modifier.levels = modifier.render_levels = 2
                label = 'freestyle_multimaterial_' + ('subdiv' if subdiv else 'mesh')
                reference = render(label)
                # The marked quad moved from x=.5 to x=1.5. The base has no marked edges.
                mask = red_mask(reference)
                self.assertEqual(int(mask[100:155, 135:184].sum()), 0,
                                 'Marked edges were also drawn with the base material')
                self.assertGreater(int(mask[:, 200:].sum()), 500)
                live = viewport_render(label + '_viewport')
                self.assertEqual(int(red_mask(live)[100:155, 135:184].sum()), 0)
                self.assertGreater(int(red_mask(live)[:, 200:].sum()), 500)
                # Reassign slots without changing the rendered materials. Cached edge ownership
                # must update along with the surface batches.
                mesh.materials[0], mesh.materials[2] = displaced, base
                mesh.polygons[0].material_index = 2
                mesh.polygons[1].material_index = 0
                mesh.update()
                np.testing.assert_allclose(render(label + '_reassigned'), reference,
                                           atol=.005, rtol=0)
                np.testing.assert_allclose(viewport_render(label + '_reassigned_viewport'), live,
                                           atol=.005, rtol=0)
                # Removing the unused slot must keep the marked quad's displaced outline intact.
                mesh.materials.pop(index=1)
                mesh.update()
                final = red_mask(render(label + '_slot_removed'))
                np.testing.assert_array_equal(final, mask)
                np.testing.assert_array_equal(red_mask(viewport_render(label + '_removed_viewport')),
                                              red_mask(live))
                # Render the edit-mesh cache too, including the GPU subdivision path.
                bpy.context.view_layer.objects.active = obj
                obj.select_set(True)
                bpy.ops.object.mode_set(mode='EDIT')
                try:
                    edit = red_mask(viewport_render(label + '_edit_viewport'))
                    self.assertEqual(int(edit[100:155, 135:184].sum()), 0)
                    self.assertGreater(int(edit[:, 200:].sum()), 500)
                finally:
                    bpy.ops.object.mode_set(mode='OBJECT')


def main():
    args = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
    suite = unittest.defaultTestLoader.loadTestsFromNames(args, module=sys.modules[__name__]) if args else \
        unittest.defaultTestLoader.loadTestsFromTestCase(OutlineVisibilityTest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    (OUTPUT / 'summary.json').write_text(json.dumps({
        'backend': gpu.platform.backend_type_get(),
        'binary': bpy.app.binary_path,
        'build_hash': bpy.app.build_hash.decode(),
        'success': result.wasSuccessful(),
        'cases': REPORT,
    }, indent=2), encoding='utf-8')
    if not result.wasSuccessful():
        raise AssertionError('Outline visibility regression failed')
    print('OUTLINE_VISIBILITY_OK', flush=True)


if __name__ == '__main__':
    main()
