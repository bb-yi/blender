"""Actual Eevee Planar Probe pixels, lifecycle, references and unsupported-stage regressions.

Run with factory startup and isolated BLENDER_USER_* directories. The release case runs
OpenGL and Vulkan sequentially. PLANAR_PROBE_TEST_CASES can select named test groups.
"""

import json
import math as math_module
import os
from pathlib import Path
import sys
import time
import traceback
from types import SimpleNamespace

import bpy
import gpu
import numpy as np


OUT = Path(os.environ['BLENDER_TEST_OUTPUT_DIR'])
OUT.mkdir(parents=True, exist_ok=True)
gpu.init()
REPORT = {'backend': gpu.platform.backend_type_get(), 'renderer': gpu.platform.renderer_get(),
          'vendor': gpu.platform.vendor_get(), 'cases': {}, 'passed': False}
ACTIVE = None
COLORS = ((0.8, 0.04, 0.02), (0.03, 0.7, 0.08), (0.04, 0.07, 0.9), (0.7, 0.5, 0.03))


def configure():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    scene.render.resolution_x = scene.render.resolution_y = 48
    scene.render.resolution_percentage = 100
    scene.render.use_compositing = False
    scene.eevee.taa_render_samples = 1
    scene.eevee.use_raytracing = False
    scene.eevee.use_outline = False
    scene.view_settings.view_transform = 'Standard'
    scene.view_settings.look = 'None'
    scene.world = bpy.data.worlds.new('Planar World')
    scene.world.use_nodes = True
    scene.world.node_tree.nodes['Background'].inputs['Color'].default_value = (0.125, 0.25, 0.5, 1)
    scene.world.node_tree.nodes['Background'].inputs['Strength'].default_value = 1
    data = bpy.data.cameras.new('Main Camera')
    data.type = 'ORTHO'
    data.ortho_scale = 4
    camera = bpy.data.objects.new('Main Camera', data)
    camera.location = (0, 0, 5)
    scene.collection.objects.link(camera)
    scene.camera = camera
    return scene


def material(name='Planar Receiver', color=(1, 1, 1)):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    tree = mat.node_tree
    tree.nodes.clear()
    emission = tree.nodes.new('ShaderNodeEmission')
    emission.location = (160, 0)
    emission.inputs['Color'].default_value = (*color, 1)
    output = tree.nodes.new('ShaderNodeOutputMaterial')
    output.location = (400, 0)
    tree.links.new(emission.outputs[0], output.inputs['Surface'])
    return mat


def plane(name, mat, x=0, y=0, z=0, size=4):
    bpy.ops.mesh.primitive_plane_add(size=size, location=(x, y, z))
    obj = bpy.context.object
    obj.name = name
    obj.data.materials.append(mat)
    return obj


def probe(name='Planar', x=0, z=0):
    data = bpy.data.lightprobes.new(name, 'PLANE')
    data.influence_distance = 10
    data.clip_start = 0.001
    obj = bpy.data.objects.new(name, data)
    obj.location = (x, 0, z)
    obj.scale = (5, 5, 1)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def board(x=0, solid=None):
    objects = []
    for index, (dx, dy) in enumerate(((-1, -1), (1, -1), (-1, 1), (1, 1))):
        obj = plane('Capture Source ' + str(index), material('Emitter', solid or COLORS[index]),
                    x=x + dx, y=dy, z=2, size=2)
        obj.visible_camera = False
        objects.append(obj)
    return objects


def fixture(output='Color', selection='AUTO', method='DITHERED'):
    scene = configure()
    target = probe()
    sources = board()
    mat = material()
    mat.surface_render_method = method
    reader = mat.node_tree.nodes.new('ShaderNodePlanarProbe')
    reader.location = (-160, 0)
    reader.width = 240
    reader.selection = selection
    if selection == 'OBJECT':
        reader.probe_object = target
    wire_output(mat, reader.outputs[output])
    receiver = plane('Receiver', mat)
    receiver.hide_probe_plane = True
    return scene, mat, reader, target, receiver, sources


def wire_output(mat, socket):
    tree = mat.node_tree
    tree.links.new(socket, tree.nodes['Emission'].inputs['Color'])


def vector_input(tree, node, socket, value):
    vector = tree.nodes.new('ShaderNodeCombineXYZ')
    for axis, number in zip(('X', 'Y', 'Z'), value):
        vector.inputs[axis].default_value = number
    tree.links.new(vector.outputs[0], node.inputs[socket])
    return vector


def math(tree, operation, first, second):
    node = tree.nodes.new('ShaderNodeMath')
    node.operation = operation
    for index, value in enumerate((first, second)):
        if isinstance(value, (int, float)):
            node.inputs[index].default_value = value
        else:
            tree.links.new(value, node.inputs[index])
    return node.outputs[0]


def render(name):
    global ACTIVE
    bpy.context.view_layer.update()
    started = time.perf_counter()
    bpy.ops.render.render()
    scene = bpy.context.scene
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.render.image_settings.color_depth = '32'
    path = OUT / (name + '.exr')
    bpy.data.images['Render Result'].save_render(str(path))
    image = bpy.data.images.load(str(path), check_existing=False)
    width, height = image.size
    pixels = np.empty(width * height * 4, dtype=np.float32)
    image.pixels.foreach_get(pixels)
    bpy.data.images.remove(image)
    pixels = pixels.reshape(height, width, 4)
    assert np.isfinite(pixels).all(), name
    ACTIVE = {'extent': [width, height], 'seconds': time.perf_counter() - started, 'checks': []}
    REPORT['cases'][name] = ACTIVE
    print('PLANAR_CASE', name, ACTIVE['seconds'], flush=True)
    return pixels


def sample(pixels, u=0.5, v=0.5):
    h, w = pixels.shape[:2]
    return pixels[min(h - 1, int(v * h)), min(w - 1, int(u * w)), :3]


def near(actual, expected, tolerance=0.02):
    actual = np.asarray(actual)
    expected = np.asarray(expected)
    error = float(np.max(np.abs(actual - expected)))
    if ACTIVE is not None:
        entry = {'max_error': error, 'tolerance': tolerance}
        if actual.size <= 16:
            entry.update(actual=actual.tolist(), expected=expected.tolist())
        ACTIVE['checks'].append(entry)
    assert error <= tolerance, (actual, expected, error, tolerance)


def registration():
    scene, mat, node, target, receiver, sources = fixture()
    import bl_ui.node_add_menu_shader as shader_menu

    menu_nodes = []

    class InputMenu:
        bl_label = 'Input'
        layout = SimpleNamespace(separator=lambda: None)

        def node_operator(self, layout, node_type, **kwargs):
            if kwargs.get('poll', True):
                menu_nodes.append(node_type)

        def node_operator_with_outputs(self, context, layout, node_type, subnames, **kwargs):
            self.node_operator(layout, node_type, **kwargs)

        def draw_assets_for_catalog(self, layout, catalog_path):
            pass

        def draw_menu(self, layout, path):
            pass

    context = SimpleNamespace(
        engine='BLENDER_EEVEE',
        space_data=SimpleNamespace(tree_type='ShaderNodeTree', shader_type='OBJECT', id_from=None))
    shader_menu.NODE_MT_shader_node_input_base.draw(InputMenu(), context)
    assert menu_nodes.count('ShaderNodePlanarProbe') == 1
    assert menu_nodes.count('ShaderNodeLightProbeColor') == 1
    assert node.bl_label == 'Planar Probe'
    assert [s.name for s in node.inputs] == ['Position', 'Normal', 'UV']
    assert [s.name for s in node.outputs] == ['Color', 'Depth', 'UV', 'Is Valid']
    assert node.bl_rna.properties['selection'].name == 'Probe Selection'
    REPORT['interface'] = {'inputs': [s.name for s in node.inputs],
                           'outputs': [s.name for s in node.outputs],
                           'selection': node.bl_rna.properties['selection'].name}
    assert node.selection == 'AUTO'
    assert {x.identifier for x in node.bl_rna.properties['selection'].enum_items} == {'AUTO', 'OBJECT'}
    node.selection = 'OBJECT'
    node.probe_object = target
    saved = OUT / 'assets' / 'planar_probe_roundtrip.blend'
    saved.parent.mkdir(exist_ok=True)
    identifiers = [s.identifier for s in node.outputs]
    bpy.ops.wm.save_as_mainfile(filepath=str(saved))
    bpy.ops.wm.open_mainfile(filepath=str(saved))
    node = bpy.data.materials['Planar Receiver'].node_tree.nodes['Planar Probe']
    assert node.selection == 'OBJECT' and node.probe_object.name == 'Planar'
    assert identifiers == [s.identifier for s in node.outputs]
    assert node.outputs['Color'].is_linked
    near(sample(render('saved_roundtrip'), 0.25, 0.25), COLORS[0])


def coordinates():
    scene, mat, node, target, receiver, sources = fixture()
    pixels = render('orthographic_color')
    for index, (u, v) in enumerate(((0.25, 0.25), (0.75, 0.25), (0.25, 0.75), (0.75, 0.75))):
        near(sample(pixels, u, v), COLORS[index])
    for source in sources:
        source.data.materials[0].node_tree.nodes['Emission'].inputs['Color'].default_value = (8, 4, 2, 1)
    near(sample(render('linear_hdr_color')), (8, 4, 2), 0.02)
    for source, color in zip(sources, COLORS):
        source.data.materials[0].node_tree.nodes['Emission'].inputs['Color'].default_value = (*color, 1)
    wire_output(mat, node.outputs['UV'])
    original_uv = sample(render('automatic_uv_reference')).copy()
    vector_input(mat.node_tree, node, 'Normal', (0.25, 0, 1))
    warped_uv = sample(render('automatic_uv_normal_warp'))
    delta = float(np.max(np.abs(warped_uv - original_uv)))
    ACTIVE['checks'].append({'actual_delta': delta, 'minimum': 0.001})
    assert delta > 0.001, (original_uv, warped_uv)
    for link in list(node.inputs['Normal'].links):
        mat.node_tree.links.remove(link)
    wire_output(mat, node.outputs['Depth'])
    near(sample(render('orthographic_depth')), 7, 0.01)
    wire_output(mat, math(mat.node_tree, 'GREATER_THAN', node.outputs['Depth'], 0))
    near(sample(render('orthographic_depth_mask')), 1, 0.001)
    wire_output(mat, node.outputs['Is Valid'])
    near(sample(render('orthographic_valid')), 1, 0.001)

    wire_output(mat, node.outputs['Color'])
    second = mat.node_tree.nodes.new('ShaderNodePlanarProbe')
    mat.node_tree.links.new(node.outputs['UV'], second.inputs['UV'])
    wire_output(mat, second.outputs['Color'])
    near(render('uv_roundtrip')[4:-4, 4:-4, :3], pixels[4:-4, 4:-4, :3], 0.001)
    offset = mat.node_tree.nodes.new('ShaderNodeVectorMath')
    offset.operation = 'ADD'
    offset.inputs[1].default_value = (0.5, 0, 0)
    mat.node_tree.links.new(node.outputs['UV'], offset.inputs[0])
    mat.node_tree.links.new(offset.outputs['Vector'], second.inputs['UV'])
    shifted = render('uv_vector_math_offset')
    near(sample(shifted, 0.25, 0.25), COLORS[1])
    near(sample(shifted, 0.75, 0.25), 0, 0.001)

    vector_input(mat.node_tree, second, 'UV', (0.25, 0.75, 0))
    near(sample(render('explicit_uv')), COLORS[2])
    wire_output(mat, second.outputs['UV'])
    near(sample(render('explicit_uv_passthrough')), (0.25, 0.75, 0), 0.001)
    vector_input(mat.node_tree, second, 'Normal', (0.25, 0.1, 1))
    near(sample(render('explicit_uv_ignores_normal_warp')), (0.25, 0.75, 0), 0.001)
    for link in list(second.inputs['Normal'].links):
        mat.node_tree.links.remove(link)
    wire_output(mat, second.outputs['Depth'])
    near(sample(render('explicit_depth')), 7, 0.01)
    vector_input(mat.node_tree, second, 'UV', (-0.01, 0.5, 0))
    wire_output(mat, second.outputs['Is Valid'])
    near(sample(render('outside_uv')), 0, 0.001)
    for uv in ((0, 0, 0), (1, 1, 0)):
        vector_input(mat.node_tree, second, 'UV', uv)
        near(sample(render('edge_' + str(int(uv[0])))), 1, 0.001)

    # Camera shift and projection changes must retain the UV input/output contract.
    for camera_type in ('PERSP', 'ORTHO'):
        scene.camera.data.type = camera_type
        scene.camera.data.lens = 42
        scene.camera.data.shift_x = 0.09
        scene.camera.data.shift_y = -0.07
        scene.render.resolution_x, scene.render.resolution_y = 61, 43
        wire_output(mat, node.outputs['Color'])
        baseline = render('shifted_' + camera_type)
        for link in list(second.inputs['UV'].links):
            mat.node_tree.links.remove(link)
        mat.node_tree.links.new(node.outputs['UV'], second.inputs['UV'])
        wire_output(mat, second.outputs['Color'])
        near(render('shifted_roundtrip_' + camera_type)[5:-5, 5:-5, :3],
             baseline[5:-5, 5:-5, :3], 0.001)
        wire_output(mat, second.outputs['Depth'])
        near(sample(render('shifted_depth_' + camera_type)), 7, 0.02)

    scene.camera.location.z = -5
    scene.camera.rotation_euler.y = math_module.pi
    scene.camera.data.shift_x = scene.camera.data.shift_y = 0
    for obj in sources:
        obj.location.z = -2
    second.selection = 'OBJECT'
    second.probe_object = target
    vector_input(mat.node_tree, second, 'UV', (0.5, 0.5, 0))
    vector_input(mat.node_tree, second, 'Normal', (0, 0, -1))
    wire_output(mat, second.outputs['Depth'])
    near(sample(render('camera_crosses_plane_depth')), 7, 0.02)


def references():
    scene, mat, node, target, receiver, sources = fixture('Is Valid')
    vector_input(mat.node_tree, node, 'Position', (20, 20, 0))
    vector_input(mat.node_tree, node, 'UV', (0.25, 0.25, 0))
    near(sample(render('auto_outside_influence')), 0, 0.001)
    node.selection = 'OBJECT'
    node.probe_object = target
    wire_output(mat, node.outputs['Color'])
    near(sample(render('object_outside_influence')), COLORS[0])
    target.name = 'Renamed Planar'
    near(sample(render('renamed_reference')), COLORS[0])
    duplicate = target.copy()
    duplicate.data = target.data.copy()
    duplicate.location.z = 0.25
    scene.collection.objects.link(duplicate)
    node.probe_object = duplicate
    wire_output(mat, node.outputs['Depth'])
    near(sample(render('copied_probe_has_distinct_identity')), 6.5, 0.02)
    node.probe_object = target
    bpy.data.objects.remove(duplicate, do_unlink=True)
    target.hide_render = True
    wire_output(mat, node.outputs['Is Valid'])
    near(sample(render('hidden_reference')), 0, 0.001)
    target.hide_render = False
    near(sample(render('restored_reference')), 1, 0.001)
    target.data = bpy.data.lightprobes.new('Now A Sphere', 'SPHERE')
    near(sample(render('changed_probe_type')), 0, 0.001)
    target.data = bpy.data.lightprobes.new('Planar Again', 'PLANE')
    near(sample(render('restored_probe_type')), 1, 0.001)
    bpy.data.objects.remove(target, do_unlink=True)
    probe('Different Object')
    assert node.selection == 'OBJECT' and node.probe_object is None
    near(sample(render('deleted_reference_no_auto_fallback')), 0, 0.001)
    node.selection = 'AUTO'
    for link in list(node.inputs['Position'].links):
        mat.node_tree.links.remove(link)
    near(sample(render('auto_after_delete')), 1, 0.001)
    for obj in list(scene.objects):
        if obj.type == 'LIGHT_PROBE':
            bpy.data.objects.remove(obj, do_unlink=True)
    near(sample(render('no_probes')), 0, 0.001)


def instances():
    scene, mat, node, target, receiver, sources = fixture('Is Valid', 'OBJECT')
    collection = bpy.data.collections.new('Probe Instances')
    collection.objects.link(target)
    scene.collection.objects.unlink(target)
    empties = []
    for index in range(2):
        instance = bpy.data.objects.new('Probe Instance ' + str(index), None)
        instance.instance_type = 'COLLECTION'
        instance.instance_collection = collection
        scene.collection.objects.link(instance)
        empties.append(instance)
    vector_input(mat.node_tree, node, 'UV', (0.25, 0.25, 0))
    near(sample(render('parallel_instance_tie_valid')), 1, 0.001)
    wire_output(mat, node.outputs['Depth'])
    near(sample(render('instance_depth')), 7, 0.02)
    empties[1].location.z = 0.25
    empties[0].hide_render = True
    near(sample(render('instance_follows_remaining_capture')), 6.5, 0.02)
    empties[0].hide_render = False
    empties[1].location.z = 0.5
    vector_input(mat.node_tree, node, 'Position', (0, 0, 0.25))
    # Equal native scores and local distances must resolve by persistent instance identity.
    near(sample(render('instance_persistent_tie_break')), 7, 0.02)
    scene.collection.objects.unlink(empties[0])
    scene.collection.objects.link(empties[0])
    near(sample(render('instance_tie_after_relink')), 7, 0.02)
    scene.collection.objects.link(target)
    near(sample(render('non_instance_object_priority')), 7, 0.02)

    scene, mat, node, target, receiver, sources = fixture('Is Valid', 'OBJECT')
    total = node.outputs['Is Valid']
    for index in range(16):
        candidate = probe('Additional Probe ' + str(index))
        reader = mat.node_tree.nodes.new('ShaderNodePlanarProbe')
        reader.selection = 'OBJECT'
        reader.probe_object = candidate
        total = math(mat.node_tree, 'ADD', total, reader.outputs['Is Valid'])
    wire_output(mat, total)
    near(sample(render('seventeen_probes_sixteen_captured')), 16, 0.001)


def background_and_capture():
    scene, mat, node, target, receiver, sources = fixture('Color', 'OBJECT')
    for obj in sources:
        bpy.data.objects.remove(obj, do_unlink=True)
    wire_output(mat, node.outputs['Is Valid'])
    near(sample(render('background_capture_valid')), 1, 0.001)
    wire_output(mat, math(mat.node_tree, 'GREATER_THAN', node.outputs['Depth'], 0))
    near(sample(render('background_depth_mask_zero')), 0, 0.001)
    wire_output(mat, node.outputs['Depth'])
    near(sample(render('background_depth_zero')), 0, 0.001)
    wire_output(mat, node.outputs['Color'])
    near(sample(render('background_color')), (0.125, 0.25, 0.5))

    # A captured object's own Planar Probe is unavailable, including the NPR capture pass.
    for use_npr in (False, True):
        source_mat = material('Capture Guard')
        tree = source_mat.node_tree
        if use_npr:
            tree = bpy.data.node_groups.new('Capture Guard NPR', 'ShaderNodeTree')
            output = tree.nodes.new('ShaderNodeNPR_Output')
        else:
            output = tree.nodes['Emission']
        reader = tree.nodes.new('ShaderNodePlanarProbe')
        guarded = math(tree, 'ADD', 0.2, math(tree, 'MULTIPLY', reader.outputs['Is Valid'], 0.6))
        tree.links.new(guarded, output.inputs['Color'])
        if use_npr:
            source_mat.node_tree.nodes['Material Output'].nprtree = tree
        # Native Eevee deliberately omits NPR passes for camera-hidden objects. Put the
        # source behind the real camera so the reflected camera sees a genuine NPR pass.
        obj = plane('Guarded Captured Object', source_mat, z=7, size=4)
        near(sample(render('capture_guard_' + str(use_npr))), 0.2)
        bpy.data.objects.remove(obj, do_unlink=True)


def rendering_paths():
    scene, mat, node, target, receiver, sources = fixture('Color', 'OBJECT', 'BLENDED')
    near(sample(render('forward_receiver'), 0.25, 0.25), COLORS[0])
    mat.surface_render_method = 'DITHERED'
    tree = bpy.data.node_groups.new('Planar NPR Tree', 'ShaderNodeTree')
    reader = tree.nodes.new('ShaderNodePlanarProbe')
    output = tree.nodes.new('ShaderNodeNPR_Output')
    tree.links.new(reader.outputs['Color'], output.inputs['Color'])
    mat.node_tree.nodes['Material Output'].nprtree = tree
    near(sample(render('npr_receiver'), 0.25, 0.25), COLORS[0])
    mat.node_tree.nodes['Material Output'].nprtree = None

    # Both native probe tables, generated GLSL helpers and image samplers share one material.
    tree = mat.node_tree
    text = bpy.data.texts.new('planar_values.glsl')
    text.write('vec3 planar_values(vec3 color, vec3 environment) { return color + environment * 0.001; }')
    function = tree.nodes.new('ShaderNodeGLSLFunction')
    function.source_mode = 'INTERNAL'
    function.script = text
    function.function_name = 'planar_values'
    tree.interface_update(bpy.context)
    bpy.context.view_layer.update()
    environment = tree.nodes.new('ShaderNodeLightProbeColor')
    tree.links.new(node.outputs['Color'], function.inputs['color'])
    tree.links.new(environment.outputs['Reflection'], function.inputs['environment'])
    result = function.outputs[0]
    for index in range(6):
        image = bpy.data.images.new('Constant Image ' + str(index), width=1, height=1)
        image.pixels = (0, 0, 0, 1)
        image.pack()
        tex = tree.nodes.new('ShaderNodeTexImage')
        tex.image = image
        add = tree.nodes.new('ShaderNodeVectorMath')
        add.operation = 'ADD'
        tree.links.new(result, add.inputs[0])
        tree.links.new(tex.outputs['Color'], add.inputs[1])
        result = add.outputs[0]
    wire_output(mat, result)
    near(sample(render('mixed_probe_glsl_image_resources'), 0.25, 0.25), COLORS[0])
    emission = tree.nodes.new('ShaderNodeEmission')
    tree.links.new(result, emission.inputs['Color'])
    to_rgb = tree.nodes.new('ShaderNodeShaderToRGB')
    tree.links.new(emission.outputs[0], to_rgb.inputs['Shader'])
    wire_output(mat, to_rgb.outputs['Color'])
    near(sample(render('shader_to_rgb_mixed_planar_resources'), 0.25, 0.25), COLORS[0])


def unsupported_contexts():
    scene, mat, node, target, receiver, sources = fixture()
    group = bpy.data.node_groups.new('Shared Planar Context', 'ShaderNodeTree')
    group.interface.new_socket(name='Valid', in_out='OUTPUT', socket_type='NodeSocketFloat')
    inner = group.nodes.new('ShaderNodePlanarProbe')
    output = group.nodes.new('NodeGroupOutput')
    group.links.new(inner.outputs['Is Valid'], output.inputs['Valid'])
    call = mat.node_tree.nodes.new('ShaderNodeGroup')
    call.node_tree = group
    wire_output(mat, call.outputs['Valid'])
    world_tree = scene.world.node_tree
    world_call = world_tree.nodes.new('ShaderNodeGroup')
    world_call.node_tree = group
    world_tree.links.new(world_call.outputs['Valid'], world_tree.nodes['Background'].inputs['Color'])
    near(sample(render('shared_group_surface_valid')), 1, 0.001)
    receiver.hide_render = True
    near(sample(render('shared_group_world_invalid')), 0, 0.001)
    receiver.hide_render = False
    # A volume graph sees zero rather than a surface capture from the same scene.
    volume_mat = bpy.data.materials.new('Unsupported Volume Context')
    volume_mat.use_nodes = True
    volume_tree = volume_mat.node_tree
    volume_tree.nodes.clear()
    volume_output = volume_tree.nodes.new('ShaderNodeOutputMaterial')
    volume = volume_tree.nodes.new('ShaderNodeVolumePrincipled')
    volume_reader = volume_tree.nodes.new('ShaderNodePlanarProbe')
    volume_tree.links.new(volume_reader.outputs['Is Valid'], volume.inputs['Density'])
    volume_tree.links.new(volume.outputs[0], volume_output.inputs['Volume'])
    bpy.ops.mesh.primitive_cube_add(size=2, location=(0, 0, 1))
    bpy.context.object.data.materials.append(volume_mat)
    near(sample(render('volume_density_is_zero')), 1, 0.001)

    scene, mat, node, target, receiver, sources = fixture('Is Valid')
    filter_mat = bpy.data.materials.new('Unsupported Filter Context')
    filter_mat.eevee_domain = 'FILTER'
    filter_tree = filter_mat.node_tree
    filter_tree.nodes.clear()
    filter_reader = filter_tree.nodes.new('ShaderNodePlanarProbe')
    filter_output = filter_tree.nodes.new('ShaderNodeOutputFilter')
    filter_output.inputs['Alpha'].default_value = 1
    filter_tree.links.new(filter_reader.outputs['Is Valid'], filter_output.inputs['Color'])
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from filter_graph_test_utils import attach_filter_material
    attach_filter_material(filter_mat)
    near(sample(render('filter_context_is_zero')), 0, 0.001)

    scene, mat, node, target, receiver, sources = fixture('Is Valid')
    scene.world.node_tree.nodes['Background'].inputs['Strength'].default_value = 0
    for obj in sources:
        obj.hide_render = True
    tree = mat.node_tree
    diffuse = tree.nodes.new('ShaderNodeBsdfDiffuse')
    albedo = math(tree, 'SUBTRACT', 0.8, math(tree, 'MULTIPLY', node.outputs['Is Valid'], 0.001))
    tree.links.new(albedo, diffuse.inputs['Color'])
    tree.links.new(diffuse.outputs[0], tree.nodes['Material Output'].inputs['Surface'])
    bpy.ops.object.light_add(type='SUN', location=(0, 0, 4))
    sun = bpy.context.object.data
    sun.energy = 3
    sun.use_shadow = False
    sun.use_nodes = True
    light_tree = sun.node_tree
    light_output = next(n for n in light_tree.nodes if n.bl_idname == 'ShaderNodeEeveeLightShaderOutput')
    for link in list(light_output.inputs['Color'].links):
        light_tree.links.remove(link)
    light_output.inputs['Color'].default_value = (1, 1, 1, 1)
    assert float(sample(render('light_shader_positive_control'))[0]) > 0.2
    light_reader = light_tree.nodes.new('ShaderNodePlanarProbe')
    light_tree.links.new(light_reader.outputs['Is Valid'], light_output.inputs['Color'])
    near(sample(render('embedded_light_context_is_zero')), 0, 0.005)


def numeric_boundaries():
    scene, mat, node, target, receiver, sources = fixture('Is Valid', 'OBJECT')
    vector_input(mat.node_tree, node, 'Normal', (0, 0, 0))
    near(sample(render('zero_normal_invalid')), 0, 0.001)
    for link in list(node.inputs['Normal'].links):
        mat.node_tree.links.remove(link)
    scene.camera.data.type = 'PERSP'
    vector_input(mat.node_tree, node, 'Position', (0, 0, 5))
    near(sample(render('projection_at_camera_invalid')), 0, 0.001)
    for link in list(node.inputs['Position'].links):
        mat.node_tree.links.remove(link)
    vector_input(mat.node_tree, node, 'UV', (0.25, 0.25, 0))
    original_scale = target.scale.copy()
    for axis in (0, 2):
        target.scale[axis] = 0
        near(sample(render('singular_probe_axis_' + str(axis))), 0, 0.001)
        target.scale = original_scale
        near(sample(render('restored_probe_axis_' + str(axis))), 1, 0.001)
    for link in list(node.inputs['UV'].links):
        mat.node_tree.links.remove(link)
    for bits, label in (('0x7fc00000u', 'nan'), ('0x7f800000u', 'infinity')):
        text = bpy.data.texts.new('invalid_planar_' + label + '.glsl')
        text.write('vec3 invalid_planar_input() { return vec3(uintBitsToFloat(' + bits + ')); }')
        function = mat.node_tree.nodes.new('ShaderNodeGLSLFunction')
        function.source_mode = 'INTERNAL'
        function.script = text
        function.function_name = 'invalid_planar_input'
        mat.node_tree.interface_update(bpy.context)
        bpy.context.view_layer.update()
        for socket in ('Position', 'Normal', 'UV'):
            mat.node_tree.links.new(function.outputs[0], node.inputs[socket])
            near(sample(render(label + '_' + socket + '_invalid')), 0, 0.001)
            for link in list(node.inputs[socket].links):
                mat.node_tree.links.remove(link)


def inactive_graphs():
    scene = configure()
    mat = material('Inactive Planar Graph', (0.1, 0.4, 0.8))
    tree = mat.node_tree
    active_output = tree.nodes['Material Output']
    unused = tree.nodes.new('ShaderNodePlanarProbe')
    unused.selection = 'OBJECT'
    inactive_output = tree.nodes.new('ShaderNodeOutputMaterial')
    opaque = tree.nodes.new('ShaderNodeBsdfPrincipled')
    tree.links.new(unused.outputs['Is Valid'], opaque.inputs['Alpha'])
    tree.links.new(opaque.outputs[0], inactive_output.inputs['Surface'])
    inactive_output.is_active_output = False
    active_output.is_active_output = True
    plane('Unrelated Opaque Receiver', mat)
    near(sample(render('inactive_alpha_dependency_ignored')), (0.1, 0.4, 0.8), 0.005)


def dependencies():
    for sink in ('alpha', 'transparent_mix', 'group', 'bundle', 'glsl',
                 'depth_offset', 'displacement', 'tessellation'):
        scene, mat, node, target, receiver, sources = fixture('Color')
        tree = mat.node_tree
        source = node.outputs['Is Valid']
        output = tree.nodes['Material Output']
        if sink == 'group':
            group = bpy.data.node_groups.new('Nested Planar', 'ShaderNodeTree')
            group.interface.new_socket(name='Value', in_out='OUTPUT', socket_type='NodeSocketFloat')
            inner = group.nodes.new('ShaderNodePlanarProbe')
            reroute = group.nodes.new('NodeReroute')
            group.links.new(inner.outputs['Is Valid'], reroute.inputs[0])
            group.links.new(reroute.outputs[0], group.nodes.new('NodeGroupOutput').inputs['Value'])
            call = tree.nodes.new('ShaderNodeGroup')
            call.node_tree = group
            source = call.outputs['Value']
        elif sink == 'bundle':
            combine = tree.nodes.new('NodeCombineBundle')
            combine.bundle_items.new('FLOAT', 'Value')
            separate = tree.nodes.new('NodeSeparateBundle')
            separate.bundle_items.new('FLOAT', 'Value')
            tree.links.new(source, combine.inputs['Value'])
            tree.links.new(combine.outputs['Bundle'], separate.inputs['Bundle'])
            source = separate.outputs['Value']
        elif sink == 'glsl':
            text = bpy.data.texts.new('planar_alpha.glsl')
            text.write('float planar_alpha(float value) { return value; }')
            function = tree.nodes.new('ShaderNodeGLSLFunction')
            function.source_mode = 'INTERNAL'
            function.script = text
            function.function_name = 'planar_alpha'
            tree.interface_update(bpy.context)
            bpy.context.view_layer.update()
            tree.links.new(source, function.inputs['value'])
            source = function.outputs[0]
        if sink in ('alpha', 'group', 'bundle', 'glsl'):
            principled = tree.nodes.new('ShaderNodeBsdfPrincipled')
            tree.links.new(source, principled.inputs['Alpha'])
            tree.links.new(principled.outputs[0], output.inputs['Surface'])
        elif sink == 'transparent_mix':
            mix = tree.nodes.new('ShaderNodeMixShader')
            transparent = tree.nodes.new('ShaderNodeBsdfTransparent')
            tree.links.new(source, mix.inputs[0])
            tree.links.new(tree.nodes['Emission'].outputs[0], mix.inputs[1])
            tree.links.new(transparent.outputs[0], mix.inputs[2])
            tree.links.new(mix.outputs[0], output.inputs['Surface'])
        elif sink == 'depth_offset':
            tree.links.new(source, output.inputs['Depth Offset'])
        elif sink == 'displacement':
            mat.displacement_method = 'BOTH'
            tree.links.new(source, output.inputs['Displacement'])
        else:
            tessellation = tree.nodes.new('ShaderNodeTessellationControl')
            tree.links.new(source, tessellation.inputs['Max Level'])
            tree.links.new(tessellation.outputs[0], output.inputs['Tessellation'])
        near(sample(render('reject_' + sink)), (1, 0, 1), 0.01)

    # Interpolating between proven-opaque closures is a color operation, not coverage.
    scene, mat, node, target, receiver, sources = fixture('Color')
    tree = mat.node_tree
    first = tree.nodes['Emission']
    first.inputs['Color'].default_value = (0, 0.3, 0, 1)
    for link in list(first.inputs['Color'].links):
        tree.links.remove(link)
    second = tree.nodes.new('ShaderNodeEmission')
    second.inputs['Color'].default_value = (0.3, 0, 0, 1)
    mix = tree.nodes.new('ShaderNodeMixShader')
    tree.links.new(node.outputs['Is Valid'], mix.inputs[0])
    tree.links.new(first.outputs[0], mix.inputs[1])
    tree.links.new(second.outputs[0], mix.inputs[2])
    tree.links.new(mix.outputs[0], tree.nodes['Material Output'].inputs['Surface'])
    near(sample(render('opaque_mix_allowed')), (0.3, 0, 0), 0.01)


def cycles():
    scene, mat, node, target, receiver, sources = fixture('Color', 'OBJECT')
    # Deliberately nonzero downstream defaults detect missing output mappings in Cycles.
    emission = mat.node_tree.nodes['Emission']
    emission.inputs['Color'].default_value = (0.8, 0.4, 0.2, 1)
    scene.render.engine = 'CYCLES'
    scene.cycles.device = 'CPU'
    scene.cycles.samples = 1
    scene.cycles.use_denoising = False
    for output in ('Color', 'Depth', 'UV', 'Is Valid'):
        wire_output(mat, node.outputs[output])
        near(sample(render('cycles_zero_' + output.replace(' ', '_'))), 0, 0.001)
    scene.render.engine = 'BLENDER_EEVEE'
    wire_output(mat, node.outputs['Color'])
    assert node.selection == 'OBJECT' and node.probe_object == target
    near(sample(render('cycles_back_to_eevee'), 0.25, 0.25), COLORS[0])


def lifecycle():
    scene, mat, node, target, receiver, sources = fixture('Color', 'OBJECT')
    target.scale.x = 20
    # The same probe array is overwritten by a different camera before the main camera runs.
    capture_camera = bpy.data.objects.new('RT Camera', scene.camera.data.copy())
    capture_camera.location = (8, 0, 5)
    scene.collection.objects.link(capture_camera)
    board(x=8, solid=(0.1, 0.2, 0.8))
    capture_mat = material('RT Receiver')
    capture_node = capture_mat.node_tree.nodes.new('ShaderNodePlanarProbe')
    wire_output(capture_mat, capture_node.outputs['Color'])
    capture_receiver = plane('RT Receiver', capture_mat, x=8)
    capture_receiver.hide_probe_plane = True
    entry = scene.eevee.render_textures.add()
    entry.name = 'Planar Lifecycle Capture'
    entry.enabled = True
    entry.camera = capture_camera
    entry.resolution_x, entry.resolution_y = 37, 29
    entry.format = 'RGBA32F'
    entry.update_mode = 'EVERY_SAMPLE'
    inset_mat = material('RT Display')
    texture = inset_mat.node_tree.nodes.new('ShaderNodeRenderTexture')
    texture.render_texture = 'RENDER_TEXTURE_' + str(entry.uid)
    vector_input(inset_mat.node_tree, texture, 'Vector', (0.5, 0.5, 0))
    wire_output(inset_mat, texture.outputs['Color'])
    inset = plane('RT Display', inset_mat, x=1.5, y=1.5, z=0.2, size=0.5)
    inset.hide_probe_plane = True
    for index, source in enumerate(('COLOR', 'NORMAL', 'DEPTH', 'COLOR')):
        entry.source = source
        scene.frame_set(index + 1)
        pixels = render('rt_lifecycle_' + str(index) + '_' + source)
        near(sample(pixels, 0.25, 0.25), COLORS[0])
        if source == 'COLOR':
            near(sample(pixels, 0.875, 0.875), (0.1, 0.2, 0.8))
    entry.enabled = False
    near(sample(render('rt_skipped_main_still_current'), 0.25, 0.25), COLORS[0])


def viewport():
    global ACTIVE
    scene, mat, node, target, receiver, sources = fixture('Color', 'OBJECT')
    scene.eevee.taa_samples = 1
    scene.eevee.use_taa_reprojection = False
    area = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    space.shading.type = 'RENDERED'
    space.shading.use_scene_world_render = True
    space.shading.use_scene_lights_render = True
    space.overlay.show_overlays = False
    second = mat.node_tree.nodes.new('ShaderNodePlanarProbe')
    second.selection = 'OBJECT'
    second.probe_object = target
    mat.node_tree.links.new(node.outputs['UV'], second.inputs['UV'])
    for index, (width, height) in enumerate(((48, 48), (61, 43), (48, 48))):
        buffer = gpu.types.GPUOffScreen(width, height, format='RGBA32F')
        try:
            def draw(socket):
                wire_output(mat, socket)
                bpy.context.view_layer.update()
                depsgraph = bpy.context.evaluated_depsgraph_get()
                projection = scene.camera.calc_matrix_camera(depsgraph, x=width, y=height)
                buffer.draw_view3d(scene, bpy.context.view_layer, space, region,
                                   scene.camera.matrix_world.inverted(), projection,
                                   do_color_management=False, draw_background=True)
                pixels = np.asarray(buffer.texture_color.read(), dtype=np.float32).reshape(
                    height, width, 4).copy()
                assert np.isfinite(pixels).all()
                return pixels

            name = 'viewport_resize_' + str(index)
            ACTIVE = {'extent': [width, height], 'checks': []}
            REPORT['cases'][name] = ACTIVE
            pixels = draw(node.outputs['Color'])
            near(sample(pixels, 0.25, 0.25), COLORS[0])
            near(sample(pixels, 0.75, 0.75), COLORS[3])
            near(draw(second.outputs['Color'])[4:-4, 4:-4, :3],
                 pixels[4:-4, 4:-4, :3], 0.001)
            print('PLANAR_CASE', name, flush=True)
        finally:
            buffer.free()


TESTS = {name: globals()[name] for name in (
    'registration', 'coordinates', 'references', 'instances', 'background_and_capture',
    'rendering_paths', 'unsupported_contexts', 'numeric_boundaries', 'inactive_graphs',
    'dependencies', 'cycles', 'lifecycle', 'viewport')}


def main():
    requested = os.environ.get('PLANAR_PROBE_TEST_CASES', '')
    names = requested.split(',') if requested else list(TESTS)
    REPORT['groups'] = names
    try:
        for name in names:
            print('PLANAR_GROUP', name, flush=True)
            TESTS[name]()
        REPORT['passed'] = True
    except Exception:
        REPORT['error'] = traceback.format_exc()
        raise
    finally:
        (OUT / 'summary.json').write_text(json.dumps(REPORT, indent=2), encoding='utf-8')
    print('PLANAR_PROBE_PASS', flush=True)


if __name__ == '__main__':
    main()
