# SPDX-License-Identifier: GPL-2.0-or-later
"""Pixel tests for opted-in Forward backgrounds and reversible NPR layer scheduling."""
import json
import os
from pathlib import Path
import sys
import unittest

import bpy
import gpu
import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from test_outline_shell_refraction_layers import (
    setup_scene, emission_material, plane, render, center, pixel, OUT, RENDERS)


def forward_material(name, color, alpha=1.0, additive=False):
    mat = emission_material(name, color)
    mat.surface_render_method = 'BLENDED'
    if alpha < 1 or additive:
        tree = mat.node_tree
        output = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeOutputMaterial')
        emission = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeEmission')
        transparent = tree.nodes.new('ShaderNodeBsdfTransparent')
        mixer = tree.nodes.new('ShaderNodeAddShader' if additive else 'ShaderNodeMixShader')
        if additive:
            tree.links.new(emission.outputs[0], mixer.inputs[0])
            tree.links.new(transparent.outputs[0], mixer.inputs[1])
        else:
            mixer.inputs[0].default_value = 1 - alpha
            tree.links.new(emission.outputs[0], mixer.inputs[1])
            tree.links.new(transparent.outputs[0], mixer.inputs[2])
        tree.links.new(mixer.outputs[0], output.inputs['Surface'])
    return mat


def forward_plane(name, color, alpha=1, *, z=-1, layer=0, participate=True,
                  additive=False, x=0, size=3):
    mat = forward_material(name, color, alpha, additive)
    obj = plane(name, mat, z, size, layer)
    obj.location.x = x
    obj.use_npr_refraction_background = participate
    return obj


def glass(name='Glass', layer=1, z=0):
    return plane(name, emission_material(name, (0, 0, 0), refractive=True), z, 3, layer)


def fog(scene, strong=False):
    tree = scene.world.node_tree
    output = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeOutputWorld')
    absorption = tree.nodes.new('ShaderNodeVolumeAbsorption')
    absorption.inputs['Color'].default_value = (0.8, 0.15, 0.03, 1)
    absorption.inputs['Density'].default_value = 2 if strong else .06
    tree.links.new(absorption.outputs[0], output.inputs['Volume'])
    scene.eevee.volumetric_samples = 64
    scene.eevee.volumetric_tile_size = '4'
    scene.eevee.volumetric_start = .1
    scene.eevee.volumetric_end = 12
    if hasattr(scene.eevee, 'use_volume_custom_range'):
        scene.eevee.use_volume_custom_range = True


class ForwardRefractionTest(unittest.TestCase):
    def setUp(self):
        self.scene, self.back = setup_scene()
        self.scene.eevee.npr_forward_refraction_mode = 'LAYERED'

    def same(self, a, b, tolerance=.02):
        np.testing.assert_allclose(a[56:72, 56:72], b[56:72, 56:72], atol=tolerance, rtol=0)

    def compare_background(self, label, alpha=1, additive=False, volume=False):
        forward_plane(label, (.9, .1, .15), alpha, additive=additive)
        if volume:
            fog(self.scene)
        reference = render(label + '_reference')
        refractor = glass()
        layered = render(label + '_layered', preview=True)
        self.same(layered, reference)
        self.scene.eevee.npr_forward_refraction_mode = 'LEGACY'
        traditional = render(label + '_legacy')
        self.assertGreater(float(np.max(np.abs(center(traditional)[:3] - center(reference)[:3]))), .05)
        self.scene.eevee.npr_forward_refraction_mode = 'LAYERED'
        self.same(render(label + '_again'), reference)
        return refractor, reference

    def test_opaque(self):
        self.compare_background('opaque')

    def test_alpha(self):
        self.compare_background('alpha', .45)

    def test_additive(self):
        self.compare_background('additive', additive=True)

    def test_volume_opaque(self):
        self.compare_background('volume_opaque', volume=True)

    def test_volume_alpha(self):
        self.compare_background('volume_alpha', .45, volume=True)

    def test_sparse_multiple_layers(self):
        forward_plane('back', (.8, .1, .05), .4, layer=-2, z=-1.5)
        forward_plane('middle', (.1, .2, .9), .3, layer=5, z=-.5)
        reference = render('sparse_reference')
        glass('Glass A', 0, -1)
        glass('Glass B', 12, 0)
        glass('Glass C', 20, .3)
        glass('Glass D', 25, .6)
        self.same(render('sparse_four_layers'), reference)

    def test_same_layer_does_not_read_itself(self):
        forward_plane('same', (.9, .1, .1), layer=0)
        glass(layer=0)
        a = render('same_layer_new')
        self.scene.eevee.npr_forward_refraction_mode = 'LEGACY'
        self.same(a, render('same_layer_old'))

    def test_no_deferred_background(self):
        bpy.data.objects.remove(self.back, do_unlink=True)
        forward_plane('only_forward', (.8, .3, .1), .5)
        reference = render('no_deferred_reference')
        glass()
        self.same(reference, render('no_deferred_layered'))

    def test_foreground_preserved(self):
        forward_plane('back', (.8, .1, .1), .5)
        forward_plane('foreground', (.1, .3, .9), .6, z=1, participate=False)
        reference = render('foreground_reference')
        glass()
        self.same(reference, render('foreground_layered'))

    def test_shared_material_and_participation(self):
        left = forward_plane('left', (.9, .1, .1), x=-.7, size=1.2)
        right = forward_plane('right', (.9, .1, .1), x=.7, size=1.2, participate=False)
        right.data.materials[0] = left.data.materials[0]
        glass()
        a = render('shared_material')
        np.testing.assert_allclose(pixel(a, -.7)[:3], (.9, .1, .1), atol=.02, rtol=0)
        np.testing.assert_allclose(pixel(a, .7)[:3], (.05, .65, .15), atol=.02, rtol=0)
        right.use_npr_refraction_background = True
        b = render('shared_material_enabled')
        np.testing.assert_allclose(pixel(b, .7)[:3], (.9, .1, .1), atol=.02, rtol=0)

    def test_dynamic_layer_hide_and_alpha(self):
        obj = forward_plane('moving', (.8, .2, .1), .5)
        reference = render('dynamic_reference')
        glass(layer=5)
        self.same(render('dynamic_initial'), reference)
        obj.refraction_layer_index = 6
        wrong_layer = render('dynamic_foreground_layer')
        self.assertGreater(float(np.max(np.abs(center(wrong_layer)[:3] - center(reference)[:3]))), .05)
        obj.refraction_layer_index = 0
        self.same(render('dynamic_restored'), reference)
        obj.hide_render = True
        hidden = render('dynamic_hidden')
        np.testing.assert_allclose(center(hidden)[:3], (.05, .65, .15), atol=.02, rtol=0)
        obj.hide_render = False
        self.same(render('dynamic_unhidden'), reference)

    def test_uv_offset(self):
        forward_plane('left', (.9, .1, .1), x=-.7, size=1.3)
        forward_plane('right', (.1, .2, .9), x=.7, size=1.3)
        obj = glass()
        tree = obj.data.materials[0].node_tree
        output = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeOutputMaterial')
        npr = output.nprtree
        source = next(n for n in npr.nodes if n.bl_idname == 'ShaderNodeNPR_Refraction')
        target = next(n for n in npr.nodes if n.bl_idname == 'ShaderNodeNPR_Output')
        sample = npr.nodes.new('ShaderNodeNPR_ImageSample')
        sample.offset_type = 'UV'
        sample.inputs[1].default_value = (.73, .5, 0)
        npr.links.new(source.outputs['Combined Color'], sample.inputs[0])
        npr.links.new(sample.outputs[0], target.inputs['Color'])
        a = render('uv_offset')
        self.assertGreater(float(pixel(a, -.7)[2]), .7)

    def test_volume_uv_offset_transparent_world(self):
        self.scene.render.film_transparent = True
        self.scene.eevee.taa_render_samples = 8
        bpy.data.objects.remove(self.back, do_unlink=True)
        fog(self.scene, strong=True)
        forward_plane('Fog UV left', (.9, .1, .05), .5, x=-.7, size=1.3)
        forward_plane('Fog UV right', (.1, .4, .9), .5, x=.7, size=1.3)
        reference = render('fog_uv_reference')
        obj = glass()
        output = next(n for n in obj.data.materials[0].node_tree.nodes
                      if n.bl_idname == 'ShaderNodeOutputMaterial')
        tree = output.nprtree
        source = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeNPR_Refraction')
        target = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeNPR_Output')
        sample = tree.nodes.new('ShaderNodeNPR_ImageSample')
        sample.offset_type = 'UV'
        sample.inputs[1].default_value = (.73, .5, 0)
        tree.links.new(source.outputs['Combined Color'], sample.inputs[0])
        tree.links.new(sample.outputs[0], target.inputs['Color'])
        actual = render('fog_uv_layered')
        self.assertTrue(np.isfinite(actual).all())
        np.testing.assert_allclose(pixel(actual, -.7), pixel(reference, .7), atol=.02, rtol=0)

    def test_deletion_releases_layer_routing(self):
        producer = forward_plane('Deleted producer', (.8, .2, .1), .5)
        refractor = glass()
        render('delete_active')
        bpy.data.objects.remove(producer, do_unlink=True)
        layered = render('delete_last_producer')
        self.scene.eevee.npr_forward_refraction_mode = 'LEGACY'
        self.same(layered, render('delete_last_producer_legacy'))
        self.scene.eevee.npr_forward_refraction_mode = 'LAYERED'
        forward_plane('Replacement producer', (.8, .2, .1), .5)
        bpy.data.objects.remove(refractor, do_unlink=True)
        layered = render('delete_last_consumer')
        self.scene.eevee.npr_forward_refraction_mode = 'LEGACY'
        self.same(layered, render('delete_last_consumer_legacy'))

    def test_depth_offset_with_volume(self):
        forward_plane('Offset depth background', (.8, .2, .1), .5)
        fog(self.scene)
        reference = render('offset_depth_reference')
        obj = glass()
        tree = obj.data.materials[0].node_tree
        output = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeOutputMaterial')
        value = tree.nodes.new('ShaderNodeValue')
        value.outputs[0].default_value = -.2
        tree.links.new(value.outputs[0], output.inputs['Depth Offset'])
        self.same(reference, render('offset_depth_volume'))

    def test_save_reload_and_rollback(self):
        obj = forward_plane('saved', (.8, .3, .1), .5)
        glass()
        a = render('roundtrip_before')
        path = OUT / 'forward_refraction_example.blend'
        bpy.ops.wm.save_as_mainfile(filepath=str(path))
        bpy.ops.wm.open_mainfile(filepath=str(path))
        self.assertEqual(bpy.context.scene.eevee.npr_forward_refraction_mode, 'LAYERED')
        self.assertTrue(bpy.data.objects['saved'].use_npr_refraction_background)
        self.same(a, render('roundtrip_after'))
        bpy.context.scene.eevee.npr_forward_refraction_mode = 'LEGACY'
        b = render('roundtrip_legacy')
        bpy.context.scene.eevee.npr_forward_refraction_mode = 'LAYERED'
        self.same(a, render('roundtrip_reenabled'))
        bpy.context.scene.eevee.npr_forward_refraction_mode = 'LEGACY'
        self.same(b, render('roundtrip_legacy_again'))

    def test_strong_volume_and_transparent_film(self):
        self.scene.render.film_transparent = True
        bpy.data.objects.remove(self.back, do_unlink=True)
        forward_plane('strong_background', (.9, .5, .1), .5)
        fog(self.scene, strong=True)
        reference = render('strong_reference')
        glass()
        a = render('strong_volume')
        self.assertTrue(np.isfinite(a).all())
        self.assertLess(float(np.abs(a).max()), 100)
        self.same(a, reference)

    def test_hdr_uv_offset_with_local_absorption(self):
        from filter_graph_test_utils import attach_filter_material, add_pass_input_image_sample
        self.scene.render.film_transparent = True
        forward_plane('HDR clear background', (1000, 1000, 1000), x=.7, size=1.3)
        forward_plane('Absorbed background', (.8, .2, .1), x=-.7, size=1.3)
        bpy.ops.mesh.primitive_cube_add(size=2, location=(-.7, 0, 2.5))
        box = bpy.context.object
        box.scale = (.55, .65, 1.5)
        material = bpy.data.materials.new('Local colored absorption')
        material.use_nodes = True
        tree = material.node_tree
        tree.nodes.clear()
        output = tree.nodes.new('ShaderNodeOutputMaterial')
        absorption = tree.nodes.new('ShaderNodeVolumeAbsorption')
        absorption.inputs['Color'].default_value = (.8, .15, .03, 1)
        absorption.inputs['Density'].default_value = 2.5
        tree.links.new(absorption.outputs[0], output.inputs['Volume'])
        box.data.materials.append(material)
        self.scene.eevee.volumetric_samples = 64
        self.scene.eevee.volumetric_tile_size = '4'
        self.scene.eevee.volumetric_start = .1
        self.scene.eevee.volumetric_end = 12
        self.scene.eevee.use_volume_custom_range = True
        reference = render('hdr_absorption_reference')
        self.assertGreater(float(pixel(reference, .7)[0]), 500)
        self.assertLess(float(pixel(reference, -.7)[1]), .05)
        obj = glass()
        output = next(n for n in obj.data.materials[0].node_tree.nodes
                      if n.bl_idname == 'ShaderNodeOutputMaterial')
        tree = output.nprtree
        source = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeNPR_Refraction')
        target = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeNPR_Output')
        sample = tree.nodes.new('ShaderNodeNPR_ImageSample')
        sample.offset_type = 'UV'
        sample.inputs[1].default_value = (.73, .5, 0)
        tree.links.new(source.outputs['Combined Color'], sample.inputs[0])
        tree.links.new(sample.outputs[0], target.inputs['Color'])
        actual = render('hdr_absorption_offset')
        self.assertTrue(np.isfinite(actual).all())
        self.assertLessEqual(float(np.abs(actual).max()), 65504)
        material = bpy.data.materials.new('HDR pre-volume identity')
        material.eevee_domain = 'FILTER'
        material.node_tree.nodes.clear()
        _, sample = add_pass_input_image_sample(material.node_tree.nodes, material.node_tree.links)
        target = material.node_tree.nodes.new('ShaderNodeOutputFilter')
        material.node_tree.links.new(sample.outputs['Color'], target.inputs['Color'])
        material.node_tree.links.new(sample.outputs['Alpha'], target.inputs['Alpha'])
        attach_filter_material(material, stage='BEFORE_VOLUME_FOG', scene_socket='Color Image')
        self.assertTrue(np.isfinite(render('hdr_absorption_filter')).all())

    def test_defaults(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        self.assertEqual(bpy.context.scene.eevee.npr_forward_refraction_mode, 'LEGACY')
        obj = bpy.data.objects.new('Default', None)
        self.assertFalse(obj.use_npr_refraction_background)

    def test_colored_transmittance(self):
        obj = forward_plane('colored_transmittance', (.3, .1, .05), .4)
        node = next(n for n in obj.data.materials[0].node_tree.nodes
                    if n.bl_idname == 'ShaderNodeBsdfTransparent')
        node.inputs['Color'].default_value = (.2, .8, .4, 1)
        reference = render('colored_transmittance_reference')
        glass()
        self.same(reference, render('colored_transmittance_layered'))

    def test_holdout_background(self):
        self.scene.render.film_transparent = True
        obj = forward_plane('Holdout background', (.8, .2, .1), .4)
        tree = obj.data.materials[0].node_tree
        holdout = tree.nodes.new('ShaderNodeHoldout')
        mix = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeMixShader')
        tree.links.new(holdout.outputs[0], mix.inputs[1])
        reference = render('holdout_reference')
        glass()
        self.same(reference, render('holdout_layered'))

    def test_deferred_shell_volume_coverage(self):
        from test_outline_shell_refraction_layers import sphere, shell_node
        for mode, alpha in (('CLIP', .25), ('CLIP', .75), ('DITHERED', .45)):
            with self.subTest(mode=mode, alpha=alpha):
                self.scene, self.back = setup_scene()
                self.scene.eevee.npr_forward_refraction_mode = 'LAYERED'
                # Activate layering away from the checked shell, isolating Deferred fog coverage.
                forward_plane('Coverage producer', (.8, .2, .1), .5, x=10)
                fog(self.scene)
                mat = emission_material('Coverage emission', (.1, .2, .8), outline=(1, 0, 0))
                mat.use_raytrace_refraction = True
                sphere('Deferred coverage', mat, location=(0, 0, -.7), radius=.6, layer=1)
                shell = shell_node(mat)
                mat.outline_shell_transparency_method = mode
                shell.inputs['Alpha'].default_value = alpha
                label = 'coverage_' + mode.lower() + '_' + str(alpha)
                reference = render(label + '_reference')
                consumer = glass(layer=3, z=.3)
                actual = render(label + '_layered')
                np.testing.assert_allclose(actual[24:104, 24:104], reference[24:104, 24:104],
                                           atol=.02, rtol=0)
                bpy.data.objects.remove(consumer, do_unlink=True)

    def test_before_volume_filter(self):
        from filter_graph_test_utils import attach_filter_material, add_pass_input_image_sample
        forward_plane('filtered_background', (.8, .2, .1), .5)
        fog(self.scene)
        reference = render('filtered_reference')
        glass()
        material = bpy.data.materials.new('Identity pre-volume filter')
        material.eevee_domain = 'FILTER'
        tree = material.node_tree
        tree.nodes.clear()
        _, sample = add_pass_input_image_sample(tree.nodes, tree.links)
        output = tree.nodes.new('ShaderNodeOutputFilter')
        tree.links.new(sample.outputs['Color'], output.inputs['Color'])
        tree.links.new(sample.outputs['Alpha'], output.inputs['Alpha'])
        attach_filter_material(material, stage='BEFORE_VOLUME_FOG', scene_socket='Color Image')
        self.same(reference, render('identity_pre_volume_filter'))

    def test_color_capture_isolation(self):
        from test_render_texture_updates import capture, filter_samples
        from filter_graph_test_utils import attach_filter_material
        forward_plane('capture_background', (.8, .2, .1), .5)
        fog(self.scene)
        self.scene.eevee.taa_render_samples = 8
        glass()
        reference = render('capture_reference')
        target = capture(self.scene, 'COLOR', mode='EVERY_SAMPLE', size=(128, 128))
        material = bpy.data.materials.new('Capture reader')
        material.eevee_domain = 'FILTER'
        material.node_tree.nodes.clear()
        filter_samples(material.node_tree, [(target, 1)])
        attach_filter_material(material)
        self.same(reference, render('capture_layered'))

    def test_collection_instances(self):
        source = forward_plane('instance_source', (.7, .15, .1), .5)
        collection = bpy.data.collections.new('Forward instances')
        for owner in list(source.users_collection):
            owner.objects.unlink(source)
        collection.objects.link(source)
        empty = bpy.data.objects.new('Collection instance', None)
        empty.instance_type = 'COLLECTION'
        empty.instance_collection = collection
        self.scene.collection.objects.link(empty)
        reference = render('collection_instance_reference')
        glass()
        self.same(reference, render('collection_instance_layered'))

    def test_background_position(self):
        forward_plane('position_background', (.8, .2, .1))
        obj = glass()
        output = next(n for n in obj.data.materials[0].node_tree.nodes
                      if n.bl_idname == 'ShaderNodeOutputMaterial')
        tree = output.nprtree
        source = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeNPR_Refraction')
        target = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeNPR_Output')
        encode = tree.nodes.new('ShaderNodeVectorMath')
        encode.operation = 'ADD'
        encode.inputs[1].default_value = (0, 0, 2)
        tree.links.new(source.outputs['Position'], encode.inputs[0])
        tree.links.new(encode.outputs[0], target.inputs['Color'])
        self.assertAlmostEqual(float(center(render('opaque_background_position'))[2]), 1, delta=.04)

    def test_alpha_endpoints(self):
        obj = forward_plane('alpha_endpoints', (.8, .2, .1), .5)
        glass()
        mixer = next(n for n in obj.data.materials[0].node_tree.nodes
                     if n.bl_idname == 'ShaderNodeMixShader')
        for amount in (0, 1, 0, .5):
            mixer.inputs[0].default_value = 1 - amount
            actual = center(render('alpha_endpoint_' + str(amount)))[:3]
            expected = np.array((.8, .2, .1)) * amount + np.array((.05, .65, .15)) * (1 - amount)
            np.testing.assert_allclose(actual, expected, atol=.02, rtol=0)

    def test_geometry_nodes_instance(self):
        source = forward_plane('GN instance source', (.7, .15, .1), .5)
        for collection in list(source.users_collection):
            collection.objects.unlink(source)
        mesh = bpy.data.meshes.new('Instance point')
        mesh.from_pydata([(0, 0, 0)], [], [])
        host = bpy.data.objects.new('Geometry Nodes host', mesh)
        self.scene.collection.objects.link(host)
        tree = bpy.data.node_groups.new('Forward instance graph', 'GeometryNodeTree')
        tree.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
        tree.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
        input_node = tree.nodes.new('NodeGroupInput')
        output_node = tree.nodes.new('NodeGroupOutput')
        info = tree.nodes.new('GeometryNodeObjectInfo')
        info.inputs['Object'].default_value = source
        info.inputs['As Instance'].default_value = True
        instances = tree.nodes.new('GeometryNodeInstanceOnPoints')
        tree.links.new(input_node.outputs['Geometry'], instances.inputs['Points'])
        tree.links.new(info.outputs['Geometry'], instances.inputs['Instance'])
        tree.links.new(instances.outputs['Instances'], output_node.inputs['Geometry'])
        host.modifiers.new('Forward instances', 'NODES').node_group = tree
        reference = render('gn_reference')
        glass()
        self.same(reference, render('gn_layered'))

    def test_unused_refraction_node_preserves_legacy_schedule(self):
        from filter_graph_test_utils import attach_filter_material
        forward_plane('Unused-reader background', (.8, .2, .1))
        unused = glass()
        unused.location.x = 10
        output = next(n for n in unused.data.materials[0].node_tree.nodes
                      if n.bl_idname == 'ShaderNodeOutputMaterial')
        for link in list(output.nprtree.links):
            output.nprtree.links.remove(link)
        material = bpy.data.materials.new('Pre-volume control')
        material.eevee_domain = 'FILTER'
        material.node_tree.nodes.clear()
        target = material.node_tree.nodes.new('ShaderNodeOutputFilter')
        target.inputs['Color'].default_value = (.1, .1, .1, 1)
        target.inputs['Alpha'].default_value = 1
        attach_filter_material(material, stage='BEFORE_VOLUME_FOG')
        self.scene.eevee.npr_forward_refraction_mode = 'LEGACY'
        reference = render('unused_reader_legacy')
        self.scene.eevee.npr_forward_refraction_mode = 'LAYERED'
        self.same(reference, render('unused_reader_layered'))

    def test_muted_group_preserves_legacy_schedule(self):
        from filter_graph_test_utils import attach_filter_material
        forward_plane('Muted-reader background', (.8, .2, .1))
        obj = glass()
        obj.location.x = 10
        output = next(n for n in obj.data.materials[0].node_tree.nodes
                      if n.bl_idname == 'ShaderNodeOutputMaterial')
        tree = output.nprtree
        for link in list(tree.links):
            tree.links.remove(link)
        group = bpy.data.node_groups.new('Muted refraction group', 'ShaderNodeTree')
        group.interface.new_socket(name='Color', in_out='INPUT', socket_type='NodeSocketColor')
        group.interface.new_socket(name='Color', in_out='OUTPUT', socket_type='NodeSocketColor')
        group.nodes.new('NodeGroupInput')
        group_out = group.nodes.new('NodeGroupOutput')
        refraction = group.nodes.new('ShaderNodeNPR_Refraction')
        group.links.new(refraction.outputs['Combined Color'], group_out.inputs['Color'])
        instance = tree.nodes.new('ShaderNodeGroup')
        instance.node_tree = group
        instance.mute = True
        target = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeNPR_Output')
        tree.links.new(instance.outputs['Color'], target.inputs['Color'])
        material = bpy.data.materials.new('Muted-group pre-volume control')
        material.eevee_domain = 'FILTER'
        material.node_tree.nodes.clear()
        filter_out = material.node_tree.nodes.new('ShaderNodeOutputFilter')
        filter_out.inputs['Color'].default_value = (.1, .1, .1, 1)
        filter_out.inputs['Alpha'].default_value = 1
        attach_filter_material(material, stage='BEFORE_VOLUME_FOG')
        self.scene.eevee.npr_forward_refraction_mode = 'LEGACY'
        reference = render('muted_group_legacy')
        self.scene.eevee.npr_forward_refraction_mode = 'LAYERED'
        self.same(reference, render('muted_group_layered'))

    def test_unused_group_input_preserves_legacy_schedule(self):
        from filter_graph_test_utils import attach_filter_material
        forward_plane('Unused-input background', (.8, .2, .1))
        obj = glass()
        obj.location.x = 10
        output = next(n for n in obj.data.materials[0].node_tree.nodes
                      if n.bl_idname == 'ShaderNodeOutputMaterial')
        tree = output.nprtree
        for link in list(tree.links):
            tree.links.remove(link)
        group = bpy.data.node_groups.new('Group input reachability', 'ShaderNodeTree')
        group.interface.new_socket(name='Color', in_out='INPUT', socket_type='NodeSocketColor')
        group.interface.new_socket(name='Color', in_out='OUTPUT', socket_type='NodeSocketColor')
        group_in = group.nodes.new('NodeGroupInput')
        group_out = group.nodes.new('NodeGroupOutput')
        instance = tree.nodes.new('ShaderNodeGroup')
        instance.node_tree = group
        source = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeNPR_Refraction')
        sample = tree.nodes.new('ShaderNodeNPR_ImageSample')
        tree.links.new(source.outputs['Combined Color'], sample.inputs[0])
        tree.links.new(sample.outputs['Color'], instance.inputs['Color'])
        target = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeNPR_Output')
        tree.links.new(instance.outputs['Color'], target.inputs['Color'])
        material = bpy.data.materials.new('Unused-input pre-volume control')
        material.eevee_domain = 'FILTER'
        material.node_tree.nodes.clear()
        filter_out = material.node_tree.nodes.new('ShaderNodeOutputFilter')
        filter_out.inputs['Color'].default_value = (.1, .1, .1, 1)
        filter_out.inputs['Alpha'].default_value = 1
        attach_filter_material(material, stage='BEFORE_VOLUME_FOG')
        self.scene.eevee.npr_forward_refraction_mode = 'LEGACY'
        reference = render('unused_group_input_legacy')
        self.scene.eevee.npr_forward_refraction_mode = 'LAYERED'
        self.same(reference, render('unused_group_input_layered'))
        group.links.new(group_in.outputs['Color'], group_out.inputs['Color'])
        actual = render('used_group_input_layered')
        np.testing.assert_allclose(center(actual)[:3], (.1, .1, .1), atol=.02, rtol=0)

    def test_forward_outline_shell(self):
        from test_outline_shell_refraction_layers import sphere, red_mask
        mat = emission_material('Forward outlined sphere', (.1, .2, .8), outline=(1, 0, 0))
        mat.surface_render_method = 'BLENDED'
        mat.outline_shell_render_method = 'FORWARD'
        obj = sphere('Forward outlined sphere', mat, location=(0, 0, -1), radius=.6)
        obj.use_npr_refraction_background = True
        reference = render('forward_shell_reference')
        self.assertGreater(int(red_mask(reference).sum()), 10)
        glass()
        actual = render('forward_shell_layered')
        self.same(reference, actual)
        self.assertGreater(int(red_mask(actual).sum()), 10)
        np.testing.assert_allclose(actual[32:96, 32:96], reference[32:96, 32:96], atol=.02, rtol=0)

    def test_stencil_snapshot(self):
        from test_stencil_value import marker_material
        plane('Early stencil marker', marker_material(reference=7), -1.5, 3)
        obj = forward_plane('Stencil background', (.8, .2, .1), .5)
        tree = obj.data.materials[0].node_tree
        value = tree.nodes.new('ShaderNodeStencilValue')
        divide = tree.nodes.new('ShaderNodeMath')
        divide.operation = 'DIVIDE'
        divide.inputs[1].default_value = 7
        tree.links.new(value.outputs['Value'], divide.inputs[0])
        emission = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeEmission')
        tree.links.new(divide.outputs[0], emission.inputs['Strength'])
        reference = render('stencil_reference')
        self.assertGreater(float(center(reference)[0]), .2)
        glass()
        self.same(reference, render('stencil_layered'))

    def test_scattering_multiple_layers(self):
        forward_plane('Scattering background', (.8, .3, .1), .5)
        fog(self.scene)
        tree = self.scene.world.node_tree
        absorption = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeVolumeAbsorption')
        scatter = tree.nodes.new('ShaderNodeVolumeScatter')
        scatter.inputs['Color'].default_value = (.2, .4, .8, 1)
        scatter.inputs['Density'].default_value = .04
        add = tree.nodes.new('ShaderNodeAddShader')
        tree.links.new(absorption.outputs[0], add.inputs[0])
        tree.links.new(scatter.outputs[0], add.inputs[1])
        output = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeOutputWorld')
        tree.links.new(add.outputs[0], output.inputs['Volume'])
        self.scene.eevee.taa_render_samples = 8
        reference = render('scattering_reference')
        glass('Scatter glass A', 1, -.5)
        glass('Scatter glass B', 3, 0)
        self.same(reference, render('scattering_two_layers'))

    def test_transparent_render_pass(self):
        import time
        from test_filter_graph_aov_output import read_multilayer_exr
        layer = bpy.context.view_layer
        (layer.eevee if hasattr(layer, 'eevee') else layer).use_pass_transparent = True
        forward_plane('Transparent background', (.9, .1, .15), .5)
        forward_plane('Transparent foreground', (.1, .3, .9), .5, z=1, participate=False)
        self.scene.use_nodes = True
        tree = bpy.data.node_groups.new('Transparent pass verification', 'CompositorNodeTree')
        self.scene.compositing_node_group = tree
        layers = tree.nodes.new('CompositorNodeRLayers')
        output = tree.nodes.new('CompositorNodeOutputFile')
        output.directory = str(OUT)
        output.format.file_format = 'OPEN_EXR_MULTILAYER'
        output.file_output_items.clear()
        output.file_output_items.new('RGBA', 'Transparent')
        tree.links.new(layers.outputs['Transparent'], output.inputs['Transparent'])
        values = []
        for label in ('transparent_reference', 'transparent_layered'):
            if label.endswith('layered'):
                glass()
            output.file_name = label + '_pass'
            start = time.time()
            render(label)
            paths = list(OUT.glob(label + '_pass*.exr'))
            self.assertTrue(paths)
            path = max(paths, key=lambda p: p.stat().st_mtime)
            self.assertGreaterEqual(path.stat().st_mtime, start - 1)
            passes = read_multilayer_exr(path)
            data = next(v for k, v in passes.items() if 'Transparent' in k)
            values.append(np.array(data)[56:72, 56:72].mean(axis=(0, 1)))
        np.testing.assert_allclose(values[0], values[1], atol=.02, rtol=0)


if __name__ == '__main__':
    gpu.init()
    backend = gpu.platform.backend_type_get()
    print('NPR_FORWARD_REFRACTION_BACKEND=' + backend, flush=True)
    selected = os.environ.get('NPR_FORWARD_TEST', '')
    suite = unittest.TestSuite(ForwardRefractionTest(name) for name in selected.split(',')) if selected else unittest.defaultTestLoader.loadTestsFromTestCase(ForwardRefractionTest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    (OUT / 'summary.json').write_text(json.dumps({'backend': backend, 'tests': result.testsRun,
        'failures': len(result.failures), 'errors': len(result.errors), 'renders': RENDERS}, indent=2), encoding='utf-8')
    if not result.wasSuccessful():
        raise SystemExit(1)
    print('NPR_FORWARD_REFRACTION_OK', flush=True)
