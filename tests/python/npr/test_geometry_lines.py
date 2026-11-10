"""Scene-level Geometry Lines integration tests; run in an isolated Blender process."""

import argparse
import json
import sys
from pathlib import Path

import bpy
import numpy as np


EDGE_IDS = ('outline', 'object', 'intersection', 'smoothing', 'material', 'normal', 'wire', 'marked')


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def socket(node, identifier):
    return next(item for item in node.inputs if item.identifier == identifier)


def set_value(node, identifier, value):
    socket(node, identifier).default_value = value


def make_source(tree, objects):
    node = tree.nodes.new('NPRLineNodeObjects')
    node.mode = 'OBJECTS'
    node.item_count = max(1, len(objects))
    for index, obj in enumerate(objects):
        set_value(node, 'object_' + str(index), obj)
    return node


def make_brush(tree, color=(0, 0, 0, 1), width=2):
    node = tree.nodes.new('NPRLineNodeBrush')
    node.wave_units='STROKE'  # Existing regression fixtures keep their original normalized wave.
    node.raster_size_response=False  # Keep existing analytic-width regression baselines.
    set_value(node, 'color', color)
    set_value(node, 'width', width)
    return node


def make_settings(tree, source, brush, types=('outline', 'object')):
    node = tree.nodes.new('NPRLineNodeSettings')
    for edge in EDGE_IDS:
        set_value(node, 'visible_' + edge, edge in types)
        set_value(node, 'hidden_' + edge, edge in types)
    tree.links.new(source.outputs[0], socket(node, 'objects'))
    tree.links.new(brush.outputs[0], socket(node, 'visible_brush'))
    return node


def active_output(tree):
    return next(node for node in tree.nodes if node.bl_idname == 'NPRLineNodeOutput' and node.is_active_output)


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    scene.render.resolution_x = scene.render.resolution_y = 192
    scene.render.resolution_percentage = 100
    scene.eevee.taa_samples = scene.eevee.taa_render_samples = 1
    scene.render.film_transparent = True
    scene.view_settings.view_transform = 'Standard'
    scene.view_settings.look = 'None'
    scene.world = bpy.data.worlds.new('Geometry Lines Test World')
    scene.world.color = (0.1, 0.1, 0.1)
    camera_data = bpy.data.cameras.new('Camera')
    camera_data.type = 'ORTHO'
    camera_data.ortho_scale = 4
    camera = bpy.data.objects.new('Camera', camera_data)
    scene.collection.objects.link(camera)
    camera.location = (0, 0, 5)
    scene.camera = camera
    return scene


def mesh_object(name, vertices, edges=(), faces=()):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(vertices, edges, faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    material = bpy.data.materials.new(name + ' Material')
    material.use_nodes = True
    material.node_tree.nodes.clear()
    output = material.node_tree.nodes.new('ShaderNodeOutputMaterial')
    emission = material.node_tree.nodes.new('ShaderNodeEmission')
    emission.inputs['Color'].default_value = (0.4, 0.4, 0.4, 1)
    material.node_tree.links.new(emission.outputs[0], output.inputs['Surface'])
    mesh.materials.append(material)
    return obj


def new_tree():
    tree = bpy.data.node_groups.new('Geometry Lines Test', 'NPRLineNodeTree')
    bpy.context.scene.eevee.geometry_line_tree = tree
    bpy.context.scene.eevee.use_geometry_lines = True
    bpy.context.view_layer.eevee.use_geometry_lines = True
    bpy.context.view_layer.eevee.use_pass_geometry_line = True
    tree.nodes.new('NPRLineNodeOutput')
    return tree


def render(directory, name):
    import OpenImageIO as oiio
    scene = bpy.context.scene
    scene.render.image_settings.media_type = 'MULTI_LAYER_IMAGE'
    scene.render.image_settings.file_format = 'OPEN_EXR_MULTILAYER'
    scene.render.image_settings.color_depth = '32'
    scene.render.filepath = str(directory / (name + '.exr'))
    bpy.context.view_layer.update()
    bpy.ops.render.render(write_still=True)
    image = oiio.ImageInput.open(scene.render.filepath)
    require(image is not None, 'Expected render EXR: ' + name)
    subimages = []
    index = 0
    while image.seek_subimage(index, 0):
        spec = image.spec()
        channels = tuple(spec.channelnames)
        pixels = np.asarray(image.read_image(format=oiio.FLOAT), dtype=np.float32).reshape(spec.height, spec.width, spec.nchannels)
        require(np.isfinite(pixels).all(), 'Non-finite render pixels: ' + name)
        subimages.append((channels, pixels, spec.get_string_attribute('oiio:subimagename')))
        index += 1
    image.close()

    def read_pass(pass_name, optional=False):
        for channels, pixels, subname in subimages:
            indices = []
            for channel in 'RGBA':
                candidates = [i for i, value in enumerate(channels)
                              if value.endswith('.' + pass_name + '.' + channel)
                              or (subname.endswith(pass_name) and value == channel)]
                if not candidates:
                    break
                indices.append(candidates[0])
            if len(indices) == 4:
                return pixels[:, :, indices]
        if optional:
            return np.zeros_like(subimages[0][1][:, :, :4])
        raise AssertionError(f'Missing {pass_name}: {[(n, c) for c, _, n in subimages]}')

    return read_pass('Combined'), read_pass('Geometry Line', optional=not scene.eevee.use_geometry_lines)


def test_nodes(directory):
    scene = reset()
    cube = mesh_object('Quad', [(-1,-1,0), (1,-1,0), (1,1,0), (-1,1,0)], faces=[(0,1,2,3)])
    cube.select_set(True)
    bpy.context.view_layer.objects.active = cube
    bpy.ops.scene.npr_line_tree_new()
    tree = scene.eevee.geometry_line_tree
    require(tree.bl_idname == 'NPRLineNodeTree', 'Scene must own native NPRLineNodeTree')
    require(len(tree.nodes) == 4 and len(tree.links) == 3, 'Default setup must contain four connected nodes')
    require(scene.eevee.use_geometry_lines and bpy.context.view_layer.eevee.use_pass_geometry_line,
            'Creating a default tree must enable scene rendering and the independent pass')
    source = next(n for n in tree.nodes if n.bl_idname == 'NPRLineNodeObjects')
    settings = next(n for n in tree.nodes if n.bl_idname == 'NPRLineNodeSettings')
    brush = next(n for n in tree.nodes if n.bl_idname == 'NPRLineNodeBrush')
    require(socket(source, 'object_0').default_value == cube, 'Selected objects must be captured by reference')
    require(socket(brush, 'width').default_value == 2, 'Default width must be 2 pixels')
    require(brush.raster_size_response, 'New brushes must use the calibrated raster response by default')
    require(not socket(settings, 'hidden_enabled').default_value, 'Hidden lines must be disabled by default')
    initial_output = active_output(tree)
    require(initial_output.mode == 'AUTO' and socket(initial_output, 'composite').default_value,
            'A new tree must automatically preview and composite ink')
    require(socket(initial_output, 'width_scale').default_value == 1 and
            not socket(initial_output, 'depth_of_field').default_value and
            not socket(initial_output, 'motion_blur').default_value,
            'Default output must use unscaled crisp ink without camera effects')
    cube.select_set(False)
    require(scene.eevee.geometry_line_tree == tree, 'Selection must not replace the scene graph')
    other_scene = bpy.data.scenes.new('Shared Scene')
    other_scene.eevee.geometry_line_tree = tree
    require(tree.users >= 2, 'Scene trees must support shared ID users')
    set_value(settings, 'visible_wire', True)
    settings.visible_overrides = {'WIRE'}
    tree.links.new(brush.outputs[0], socket(settings, 'visible_override_wire'))
    settings.visible_overrides = set()
    require(socket(settings, 'visible_override_wire').is_linked, 'Disabling an override must preserve its link')
    settings.visible_overrides = {'WIRE'}
    bpy.ops.node.npr_line_copy_visible(node_name=settings.name)
    require(socket(settings, 'hidden_wire').default_value, 'Visible settings must copy to hidden settings')
    require(socket(settings, 'hidden_override_wire').is_linked, 'Specific brush links must copy to hidden settings')
    brush.use_width_curve=brush.use_opacity_curve=True
    brush.raster_size_response=True
    brush.stroke_mode='RANDOM'
    brush.wave_units='STROKE'
    brush.override_stroke_connection=True
    set_value(brush,'stroke_split_angle',123)
    brush.width_curve.curves[0].points[1].location[1]=.7
    brush.width_curve.update()
    before = len(tree.nodes)
    bpy.ops.node.npr_line_make_brush_unique(node_name=settings.name)
    require(len(tree.nodes) == before + 1, 'Single-user operation must create a brush')
    independent = socket(settings, 'visible_brush').links[0].from_node
    require(independent != brush, 'The selected rule must use the independent brush')
    require(independent.use_width_curve and independent.use_opacity_curve and independent.raster_size_response and
            independent.stroke_mode=='RANDOM' and independent.override_stroke_connection and
            independent.wave_units=='STROKE',
            'Single-user copying must preserve native flags, orientation, wave units and connection overrides')
    require(abs(independent.width_curve.curves[0].points[1].location[1]-.7)<1e-6 and
            abs(socket(independent,'stroke_split_angle').default_value-123)<1e-6,
            'Single-user copying must preserve curve points and stroke parameters')
    independent.width_curve.curves[0].points[1].location[1]=.2
    independent.width_curve.update()
    require(abs(brush.width_curve.curves[0].points[1].location[1]-.7)<1e-6,
            'A copied curve must not edit the shared original')
    set_value(independent, 'width', 5)
    require(socket(brush, 'width').default_value == 2, 'Independent brush must not edit the shared original')
    # Native custom sockets must be legal node-group interface types.
    group = bpy.data.node_groups.new('Reusable Line Style', 'NPRLineNodeTree')
    group.interface.new_socket(name='Objects', in_out='INPUT', socket_type='NodeSocketNPRLineObjects')
    group.interface.new_socket(name='Brush', in_out='INPUT', socket_type='NodeSocketNPRLineBrush')
    group.interface.new_socket(name='Plan', in_out='OUTPUT', socket_type='NodeSocketNPRLinePlan')
    group.nodes.new('NodeGroupInput')
    group.nodes.new('NodeGroupOutput')
    group_node = tree.nodes.new('NPRLineNodeGroup')
    group_node.node_tree = group
    require(len(group_node.inputs) == 2 and len(group_node.outputs) == 1, 'Native line groups must expose their interface')
    for mode in ('NORMAL', 'MULTIPLE', 'SIMPLE'):
        brush.mode = mode
        require(socket(brush, 'image').is_unavailable == (mode == 'SIMPLE'),
                'Brush mode must show only relevant texture controls while preserving data')
        require(socket(brush, 'angle_random').is_unavailable == (mode == 'SIMPLE'),
                'Simple brush must hide stamp rotation controls')
    occlusion = tree.nodes.new('NPRLineNodeOcclusion')
    for mode in ('IGNORE', 'MASK', 'SOLID'):
        occlusion.mode = mode
        require(socket(occlusion, 'image').is_unavailable == (mode != 'MASK'),
                'Only Alpha Mask occlusion must expose image controls')
    output = active_output(tree)
    second_output = tree.nodes.new('NPRLineNodeOutput')
    require(second_output.is_active_output and not output.is_active_output,
            'Creating a new output must leave exactly one active root output')
    output.is_active_output = True
    require(output.is_active_output and not second_output.is_active_output,
            'Activating an output must deactivate the previous root output')
    tree.nodes.remove(second_output)
    require(settings.node_identifier > 0, 'Plan nodes need a stable readable identifier for runtime previews')
    tree.preview_solo_node = settings.node_identifier
    require(tree.preview_solo_node == settings.node_identifier, 'A line settings node must support temporary solo')
    tree.preview_solo_node = brush.node_identifier
    require(tree.preview_solo_node == settings.node_identifier, 'Brush nodes must not replace a valid plan solo')
    require(not tree.keys(), 'Solo preview must not store transient state in persistent custom properties')
    copied_tree = tree.copy()
    require(copied_tree.preview_solo_node == 0, 'Duplicating a tree must clear temporary solo state')
    tree.name = 'Saved Scene Lines'
    settings.name = 'Renamed Rule'
    filename = directory / 'geometry-lines-roundtrip.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(filename))
    bpy.ops.wm.open_mainfile(filepath=str(filename))
    restored = bpy.context.scene.eevee.geometry_line_tree
    require(restored.name == 'Saved Scene Lines', 'Scene tree must survive file reload')
    require(socket(restored.nodes['Renamed Rule'], 'visible_override_wire').is_linked, 'Override links must survive file reload')
    require(bpy.data.scenes['Shared Scene'].eevee.geometry_line_tree == restored, 'Shared tree identity must survive reload')
    require(restored.preview_solo_node == 0, 'Temporary solo preview must reset when opening a saved file')
    return {'nodes': 'passed', 'roundtrip': str(filename)}


def test_render(directory):
    scene = reset()
    quad = mesh_object('Quad', [(-1,-1,0), (1,-1,0), (1,1,0), (-1,1,0)], faces=[(0,1,2,3)])
    tree = new_tree()
    source = make_source(tree, [quad])
    brush = make_brush(tree, (0.8, 0.1, 0.05, 1), 3)
    settings = make_settings(tree, source, brush)
    output = active_output(tree)
    tree.links.new(settings.outputs[0], socket(output, 'plan'))
    combined, line = render(directory, 'basic')
    require(np.count_nonzero(line[:, :, 3] > 0.1) > 200, 'Four-node graph must render visible lines')
    require(float(line[:, :, 3].max()) > 0.9, 'Simple brush must produce opaque interior coverage')
    require(float(line[:20, :20, 3].max()) == 0, 'Line pass must have transparent background')
    output.mode = 'FAST'
    _, final_fast = render(directory, 'final_fast_mode')
    require(np.max(np.abs(line - final_fast)) < .002, 'Final renders must use full geometry even in Fast Preview mode')
    output.mode = 'AUTO'
    scene.eevee.use_geometry_lines = False
    base, _ = render(directory, 'disabled')
    expected = line + base * (1 - line[:, :, 3:4])
    require(np.max(np.abs(combined - expected)) < 0.006, 'Combined must apply premultiplied line alpha exactly once')
    scene.eevee.use_geometry_lines = True
    set_value(output, 'composite', False)
    only_pass, only_line = render(directory, 'pass_only')
    require(np.max(np.abs(only_pass - base)) < 0.002, 'Pass-only mode must preserve Combined')
    require(np.max(np.abs(only_line - line)) < 0.002, 'Pass-only mode must retain independent ink')
    set_value(output, 'composite', True)
    for mode in ('NORMAL', 'MULTIPLE'):
        brush.mode = mode
        # A continuously stamped round Normal tip can equal a Simple capsule.
        # Exercise its independent ellipse controls instead of requiring an artificial gap.
        set_value(brush, 'stretch', 2 if mode == 'NORMAL' else 1)
        _, changed = render(directory, mode.lower())
        require(np.count_nonzero(changed[:, :, 3] > 0.02) > 50, mode + ' brush must actually draw')
        require(np.count_nonzero(np.abs(changed[:, :, 3] - line[:, :, 3]) > 0.05) > 20, mode + ' must affect coverage')
    # Priority must claim scope even if that rule disables all edge types.
    brush.mode = 'SIMPLE'
    set_value(brush, 'stretch', 1)
    suppress = make_settings(tree, source, brush, types=())
    combine = tree.nodes.new('NPRLineNodeCombine')
    require(socket(combine, 'plans').is_multi_input, 'Combine must create a multi-input socket immediately')
    tree.links.new(settings.outputs[0], socket(combine, 'plans'))
    tree.links.new(suppress.outputs[0], socket(combine, 'plans'))
    require(len(socket(combine, 'plans').links) == 2, 'Adding a second rule must preserve the first link')
    tree.links.new(combine.outputs[0], socket(output, 'plan'))
    # The last link is topmost in Blender's multi-input UI.
    _, suppressed = render(directory, 'priority_suppressed')
    require(float(suppressed[:, :, 3].max()) == 0, 'High-priority disabled edge types must suppress lower rules')
    suppress.enabled = False
    _, fallback = render(directory, 'priority_fallback')
    require(np.count_nonzero(fallback[:, :, 3] > 0.1) > 200, 'Disabled rule must release scope to the next rule')
    return {'render': 'passed', 'ink_pixels': int(np.count_nonzero(line[:, :, 3] > 0.1))}


def set_types(settings, *types):
    for edge in EDGE_IDS:
        set_value(settings, 'visible_' + edge, edge in types)
        set_value(settings, 'hidden_' + edge, edge in types)


def setup_lines(objects, types=('outline', 'object'), color=(0.8, 0.1, 0.05, 1), width=3):
    tree = new_tree()
    source = make_source(tree, objects)
    brush = make_brush(tree, color, width)
    settings = make_settings(tree, source, brush, types=types)
    output = active_output(tree)
    tree.links.new(settings.outputs[0], socket(output, 'plan'))
    return tree, source, brush, settings, output


def uv_xy(obj, half_extent=1):
    layer = obj.data.uv_layers.new(name='UVMap')
    for loop in obj.data.loops:
        co = obj.data.vertices[loop.vertex_index].co
        layer.data[loop.index].uv = (co.x / (half_extent * 2) + 0.5, co.y / (half_extent * 2) + 0.5)


def alpha_image(name, half=False, transparent=False):
    image = bpy.data.images.new(name, width=8, height=8, alpha=True, float_buffer=True)
    pixels = np.ones((8, 8, 4), dtype=np.float32)
    if half:
        pixels[:, :4, 3] = 0
    if transparent:
        pixels[:, :, 3] = 0
    image.pixels.foreach_set(pixels.ravel())
    image.update()
    image.pack()
    return image


def test_geometry_features(directory):
    reset()
    obj = mesh_object('Regions', [(-1,-1,0), (0,-1,0), (1,-1,0),
                                  (-1,1,0), (0,1,0), (1,1,0)], faces=[(0,1,4,3), (1,2,5,4)])
    tree, source, brush, settings, output = setup_lines([obj], ('wire',))
    for polygon in obj.data.polygons:
        polygon.use_smooth = True
    _, wire = render(directory, 'feature_wire')
    require(float(wire[72, 72, 3]) < 0.01, 'Wire must not introduce triangulation diagonals')
    require(float(wire[96, 96, 3]) > 0.7, 'Wire must preserve the original shared polygon edge')
    seam = next(edge for edge in obj.data.edges if set(edge.vertices) == {1, 4})
    wire_mask=obj.data.attributes.new('pencil_wire_edge','BOOLEAN','EDGE')
    wire_mask.data[seam.index].value=True
    _,masked_wire=render(directory,'feature_wire_attribute')
    require(float(masked_wire[96,96,3])>.7 and np.count_nonzero(masked_wire[:,:,3]>.1)<500,
            'An explicit Boolean pencil_wire_edge attribute must select only marked wire edges')
    obj.data.attributes.remove(wire_mask)
    other = obj.data.materials[0].copy()
    other.name = 'Other Region Material'
    obj.data.materials.append(other)
    obj.data.polygons[1].material_index = 1
    set_types(settings, 'material')
    _, material = render(directory, 'feature_material')
    require(np.count_nonzero(material[:, :, 3] > 0.1) > 100, 'Different materials must create a shared boundary')
    require(np.count_nonzero(material[:, :, 3] > 0.1) < 500, 'Material-only output must not draw the outer contour')
    seam = next(edge for edge in obj.data.edges if set(edge.vertices) == {1, 4})
    freestyle = (obj.data.attributes.get('freestyle_edge') or
                 obj.data.attributes.new('freestyle_edge', 'BOOLEAN', 'EDGE'))
    freestyle.data[seam.index].value = True
    set_types(settings, 'marked')
    _, marked = render(directory, 'feature_marked')
    require(np.count_nonzero(marked[:, :, 3] > 0.1) > 100, 'Marked Edge must use Freestyle edge attributes')
    sharp = obj.data.attributes.get('sharp_edge') or obj.data.attributes.new('sharp_edge', 'BOOLEAN', 'EDGE')
    sharp.data[seam.index].value = True
    set_types(settings, 'smoothing')
    _, smoothing = render(directory, 'feature_smoothing')
    require(np.count_nonzero(smoothing[:, :, 3] > 0.1) > 100, 'Explicit sharp edges must produce smoothing boundaries')
    for vertex in obj.data.vertices:
        if vertex.co.x > 0:
            vertex.co.z = 1
    obj.data.update()
    set_types(settings, 'normal')
    set_value(settings, 'normal_min', 30)
    set_value(settings, 'normal_max', 60)
    _, angle = render(directory, 'feature_angle')
    require(np.count_nonzero(angle[:, :, 3] > 0.1) > 100, 'Normal-angle range must select the folded edge')
    set_value(settings, 'normal_min', 60)
    set_value(settings, 'normal_max', 80)
    _, rejected = render(directory, 'feature_angle_rejected')
    require(float(rejected[:, :, 3].max()) == 0, 'Normal-angle range must reject angles outside Min/Max')

    # Materials introduced by evaluated geometry must be resolved from the evaluated mesh,
    # even when the original object has no material slots at all.
    scene = reset()
    empty_mesh = bpy.data.meshes.new('Empty Procedural Input')
    procedural = bpy.data.objects.new('GN Material Regions', empty_mesh)
    scene.collection.objects.link(procedural)
    red = bpy.data.materials.new('GN Red Sides')
    blue = bpy.data.materials.new('GN Blue Front')
    group = bpy.data.node_groups.new('Procedural Material Fixture', 'GeometryNodeTree')
    group.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
    cube = group.nodes.new('GeometryNodeMeshCube')
    cube.inputs['Size'].default_value = (2,2,2)
    normal = group.nodes.new('GeometryNodeInputNormal')
    dot = group.nodes.new('ShaderNodeVectorMath')
    dot.operation = 'DOT_PRODUCT'
    dot.inputs[1].default_value = (0,0,1)
    positive_z = group.nodes.new('ShaderNodeMath')
    positive_z.operation = 'GREATER_THAN'
    positive_z.inputs[1].default_value = .5
    all_material = group.nodes.new('GeometryNodeSetMaterial')
    all_material.inputs['Material'].default_value = red
    face_material = group.nodes.new('GeometryNodeSetMaterial')
    face_material.inputs['Material'].default_value = blue
    group_output = group.nodes.new('NodeGroupOutput')
    group.links.new(cube.outputs['Mesh'], all_material.inputs['Geometry'])
    group.links.new(all_material.outputs['Geometry'], face_material.inputs['Geometry'])
    group.links.new(normal.outputs['Normal'], dot.inputs[0])
    group.links.new(dot.outputs['Value'], positive_z.inputs[0])
    group.links.new(positive_z.outputs[0], face_material.inputs['Selection'])
    group.links.new(face_material.outputs['Geometry'], group_output.inputs['Geometry'])
    modifier = procedural.modifiers.new('Generate Cube Materials', 'NODES')
    modifier.node_group = group
    tree, source, brush, settings, output = setup_lines([procedural], ('material',))
    _, procedural_boundary = render(directory, 'feature_gn_material_boundary')
    require(len(procedural.data.materials) == 0, 'Fixture must keep the original object without material slots')
    evaluated = procedural.evaluated_get(bpy.context.evaluated_depsgraph_get()).data
    require(len(evaluated.materials) >= 2, 'Geometry Nodes fixture must generate two evaluated materials')
    require(np.count_nonzero(procedural_boundary[:,:,3] > .1) > 200,
            'Geometry Nodes generated material regions must create material boundaries')
    set_value(settings, 'material_0', blue)
    _, filtered_generated = render(directory, 'feature_gn_material_filter')
    require(np.count_nonzero(filtered_generated[:,:,3] > .1) > 200,
            'Material filters must match materials introduced by evaluated Geometry Nodes')
    set_value(settings, 'material_0', bpy.data.materials.new('Unused GN Material'))
    _, absent_generated = render(directory, 'feature_gn_material_missing')
    require(float(absent_generated[:,:,3].max()) == 0,
            'A material absent from evaluated geometry must not match the procedural object')

    # Object Contour is an internal contour, including inside one object.
    reset()
    obj = mesh_object('Internal Contours',
                      [(-.5,-.5,0),(.5,-.5,0),(.5,.5,0),(-.5,.5,0),
                       (-1.5,-1.5,-1),(1.5,-1.5,-1),(1.5,1.5,-1),(-1.5,1.5,-1)],
                      faces=[(0,1,2,3),(4,5,6,7)])
    tree, source, brush, settings, output = setup_lines([obj], ('object',))
    _, internal = render(directory, 'feature_internal_contour')
    points = np.argwhere(internal[:, :, 3] > 0.1)
    require(len(points) > 100 and points.min() >= 67 and points.max() <= 124,
            'Internal contours must exist inside the same object without drawing its outer border')
    set_types(settings, 'outline')
    _, outline = render(directory, 'feature_outline')
    require(np.count_nonzero(outline[:, :, 3] > 0.1) > 300, 'Outer contour must draw the background boundary')
    require(float(outline[96, 72, 3]) < 0.01, 'Outline-only mode must exclude the internal contour')

    reset()
    obj = mesh_object('Crossing Surfaces',
                      [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0),
                       (0,-1,-1),(0,1,-1),(0,1,1),(0,-1,1)],
                      faces=[(0,1,2,3),(4,5,6,7)])
    tree, source, brush, settings, output = setup_lines([obj], ('intersection',))
    _, intersection = render(directory, 'feature_self_intersection')
    require(np.count_nonzero(intersection[:, :, 3] > 0.1) > 100, 'True 3D self-intersections must generate lines')
    set_value(settings, 'self_intersection', False)
    _, no_self = render(directory, 'feature_self_intersection_disabled')
    require(float(no_self[:, :, 3].max()) == 0, 'Self-intersection switch must suppress same-instance intersections')

    reset()
    obj = mesh_object('Shared Vertex Crossing',
                      [(0,0,0),(1,-1,0),(1,1,0),(1,0,-1),(1,0,1)],
                      faces=[(0,1,2),(0,3,4)])
    setup_lines([obj], ('intersection',))
    _, vertex_crossing = render(directory, 'feature_shared_vertex_intersection')
    require(np.count_nonzero(vertex_crossing[:,:,3] > .1) > 80,
            'Triangles sharing one vertex must retain a genuine three-dimensional intersection segment')
    reset()
    obj = mesh_object('Adjacent Triangle Wedge',
                      [(0,0,0),(1,-1,0),(1,1,0),(0,0,1)], faces=[(0,1,2),(0,2,3)])
    setup_lines([obj], ('intersection',))
    _, adjacent = render(directory, 'feature_adjacent_edge_not_intersection')
    require(float(adjacent[:,:,3].max()) == 0,
            'A shared topological edge between adjacent triangles must not become a self-intersection')

    # A loose wire also verifies that occluders outside the drawing scope still participate.
    reset()
    line_obj = mesh_object('Loose Wire', [(-1,0,0),(1,0,0)], edges=[(0,1)])
    blocker = mesh_object('Occluder', [(-.5,-.5,.5),(.5,-.5,.5),(.5,.5,.5),(-.5,.5,.5)], faces=[(0,1,2,3)])
    uv_xy(blocker, .5)
    tree, source, brush, settings, output = setup_lines([line_obj], ('wire',), (0,1,0,1), 4)
    hidden_brush = make_brush(tree, (1,0,0,1), 4)
    tree.links.new(hidden_brush.outputs[0], socket(settings, 'hidden_brush'))
    set_value(settings, 'hidden_enabled', True)
    _, hidden = render(directory, 'feature_hidden')
    require(float(hidden[96,96,0]) > .7 and float(hidden[96,60,1]) > .7,
            'Visibility intervals must use separate visible and hidden brushes')
    set_value(settings, 'hidden_enabled', False)
    _, visible = render(directory, 'feature_visible_only')
    require(float(visible[96,96,3]) < .01 and float(visible[96,60,3]) > .7,
            'An unselected occluder must cut visible lines without removing the visible ends')
    occlusion = tree.nodes.new('NPRLineNodeOcclusion')
    blocker_source = make_source(tree, [blocker])
    tree.links.new(blocker_source.outputs[0], socket(occlusion, 'objects'))
    tree.links.new(occlusion.outputs[0], socket(output, 'occlusion'))
    occlusion.mode = 'IGNORE'
    _, ignored = render(directory, 'feature_ignore_occlusion')
    require(float(ignored[96,96,1]) > .7, 'Ignore rule must restore visibility through the targeted occluder')
    occlusion.mode = 'MASK'
    set_value(occlusion, 'image', alpha_image('Half Occlusion', half=True))
    _, masked = render(directory, 'feature_masked_occlusion')
    require(float(masked[96,84,1]) > .7 and float(masked[96,108,3]) < .01,
            'Alpha mask must cut only opaque texels of the occluder')

    reset()
    obj = mesh_object('Masked Source', [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)], faces=[(0,1,2,3)])
    uv_xy(obj)
    tree, source, brush, settings, output = setup_lines([obj])
    occlusion = tree.nodes.new('NPRLineNodeOcclusion')
    occlusion.mode = 'MASK'
    set_value(occlusion, 'image', alpha_image('Transparent Source', transparent=True))
    tree.links.new(source.outputs[0], socket(occlusion, 'objects'))
    tree.links.new(occlusion.outputs[0], socket(output, 'occlusion'))
    _, masked_source = render(directory, 'feature_masked_source')
    require(float(masked_source[:, :, 3].max()) == 0, 'Transparent source coverage must not leave an opaque rectangular outline')
    scene = bpy.context.scene
    scene.render.use_motion_blur = True
    scene.render.motion_blur_shutter = .8
    scene.eevee.motion_blur_steps = 1
    scene.eevee.taa_render_samples = 2
    mutable_mask = socket(occlusion, 'image').default_value
    opaque_pixels = np.ones((8,8,4), dtype=np.float32)
    mutable_mask.pixels.foreach_set(opaque_pixels.ravel())
    mutable_mask.update()
    _, opaque_again = render(directory, 'feature_mask_dirty_opaque')
    require(np.count_nonzero(opaque_again[:,:,3] > .1) > 200,
            'Canonical crisp ink must consume updated opaque mask pixels while scene motion blur is on')
    opaque_pixels[:,:,3] = 0
    mutable_mask.pixels.foreach_set(opaque_pixels.ravel())
    mutable_mask.update()
    _, transparent_again = render(directory, 'feature_mask_dirty_transparent')
    require(float(transparent_again[:,:,3].max()) == 0,
            'Editing the same alpha image must invalidate canonical mask coverage for the next F12')
    # Closed surfaces exercise grazing faces that the planar fixtures cannot cover.
    # Exact axis alignment and a small rotation must both preserve a visible front silhouette.
    closed_views = 0
    for shape in ('sphere', 'cube'):
        for projection in ('ORTHO', 'PERSP'):
            scene = reset()
            scene.camera.data.type = projection
            if shape == 'sphere':
                bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=17)
            else:
                bpy.ops.mesh.primitive_cube_add(size=2)
            obj = bpy.context.object
            setup_lines([obj])
            for rotated in (False, True):
                if rotated:
                    obj.rotation_euler = (.013,.021,.017)
                name = f'feature_closed_{shape}_{projection.lower()}_{int(rotated)}'
                _, closed = render(directory, name)
                require(np.count_nonzero(closed[:,:,3] > .1) > 200,
                        'Closed surfaces must retain visible silhouettes with grazing faces: ' + name)
                closed_views += 1
    scene = reset()
    scene.camera.data.type = 'PERSP'
    scene.camera.data.clip_start = .1
    scene.camera.data.clip_end = 10
    obj = mesh_object('Clipped Wire', [(-.03,0,4.95),(.03,0,4.5)], edges=[(0,1)])
    setup_lines([obj], ('wire',))
    _, near_crossing = render(directory, 'feature_perspective_near_crossing')
    require(np.count_nonzero(near_crossing[:,:,3] > .1) > 20,
            'A perspective wire crossing the near plane must retain its visible section')
    for name, depth in (('near_outside', 4.95), ('far_outside', -15), ('behind_camera', 5.5)):
        for vertex in obj.data.vertices:
            vertex.co.z = depth
        obj.data.update()
        _, clipped = render(directory, 'feature_perspective_' + name)
        require(float(clipped[:,:,3].max()) == 0,
                'A wire entirely outside the camera clip range must emit no ink: ' + name)
    obj.data.vertices[0].co = (-.15,0,4)
    obj.data.vertices[1].co = (.15,0,4)
    obj.data.update()
    _, before_camera = render(directory, 'feature_perspective_camera_before')
    scene.camera.location.x = .1
    _, after_camera = render(directory, 'feature_perspective_camera_after')
    before_x = float(np.argwhere(before_camera[:,:,3] > .1)[:,1].mean())
    after_x = float(np.argwhere(after_camera[:,:,3] > .1)[:,1].mean())
    require(before_x - after_x > 20,
            'Moving a perspective camera must update projected ink in the expected direction')
    return {'geometry_features': 'passed', 'edge_types': 8, 'visibility_and_masks': 'passed',
            'closed_surface_views': closed_views, 'perspective_clipping_and_camera': 'passed'}


def test_groups_layers_instances(directory):
    scene = reset()
    quad = mesh_object('Grouped Quad', [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)], faces=[(0,1,2,3)])
    tree, source, brush, settings, output = setup_lines([quad], width=4)
    _, reference = render(directory, 'group_reference')
    group = bpy.data.node_groups.new('Grouped Line Set', 'NPRLineNodeTree')
    group.interface.new_socket(name='Objects', in_out='INPUT', socket_type='NodeSocketNPRLineObjects')
    group.interface.new_socket(name='Brush', in_out='INPUT', socket_type='NodeSocketNPRLineBrush')
    group.interface.new_socket(name='Plan', in_out='OUTPUT', socket_type='NodeSocketNPRLinePlan')
    group_in = group.nodes.new('NodeGroupInput')
    group_out = group.nodes.new('NodeGroupOutput')
    grouped_settings = group.nodes.new('NPRLineNodeSettings')
    group.links.new(group_in.outputs['Objects'], socket(grouped_settings, 'objects'))
    group.links.new(group_in.outputs['Brush'], socket(grouped_settings, 'visible_brush'))
    group.links.new(grouped_settings.outputs[0], group_out.inputs['Plan'])
    group_node = tree.nodes.new('NPRLineNodeGroup')
    group_node.node_tree = group
    tree.links.new(source.outputs[0], group_node.inputs['Objects'])
    tree.links.new(brush.outputs[0], group_node.inputs['Brush'])
    tree.links.new(group_node.outputs['Plan'], socket(output, 'plan'))
    _, grouped = render(directory, 'grouped_plan')
    require(np.max(np.abs(reference - grouped)) < .002, 'Grouped descriptors must render the same line plan')
    # A scalar group input must resolve to the group's own default, then to its root instance.
    brush_group = bpy.data.node_groups.new('Grouped Brush', 'NPRLineNodeTree')
    width_socket = brush_group.interface.new_socket(name='Width', in_out='INPUT', socket_type='NodeSocketFloat')
    width_socket.default_value = 4
    brush_group.interface.new_socket(name='Brush', in_out='OUTPUT', socket_type='NodeSocketNPRLineBrush')
    brush_in = brush_group.nodes.new('NodeGroupInput')
    brush_out = brush_group.nodes.new('NodeGroupOutput')
    inner_brush = make_brush(brush_group, (0.8,.1,.05,1))
    brush_group.links.new(brush_in.outputs['Width'], socket(inner_brush, 'width'))
    brush_group.links.new(inner_brush.outputs[0], brush_out.inputs['Brush'])
    brush_instance = tree.nodes.new('NPRLineNodeGroup')
    brush_instance.node_tree = brush_group
    tree.links.new(brush_instance.outputs['Brush'], group_node.inputs['Brush'])
    _, grouped_brush = render(directory, 'grouped_brush')
    require(np.max(np.abs(reference - grouped_brush)) < .002, 'Scalar group defaults must drive nested brush parameters')
    brush_instance.inputs['Width'].default_value = 8
    _, wide = render(directory, 'grouped_width_changed')
    require(np.count_nonzero(wide[:,:,3] > .1) > np.count_nonzero(reference[:,:,3] > .1) * 1.5,
            'Changing the group width input must visibly update nested brushes')
    filename = directory / 'grouped-render-roundtrip.blend'
    np.save(directory / 'grouped-expected.npy', wide)
    bpy.ops.wm.save_as_mainfile(filepath=str(filename))
    bpy.ops.wm.open_mainfile(filepath=str(filename))
    _, reopened = render(directory, 'grouped_reopened')
    require(np.max(np.abs(wide - reopened)) < .002, 'Grouped rendering and scalar values must survive save/reopen')

    reset()
    quad = mesh_object('Layered Quad', [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)], faces=[(0,1,2,3)])
    tree, source, lower, settings, output = setup_lines([quad], color=(0,0,1,1), width=8)
    upper = make_brush(tree, (1,0,0,1), 3)
    upper_settings = make_settings(tree, source, upper)
    combine = tree.nodes.new('NPRLineNodeCombine')
    combine.mode = 'OVERLAY'
    bottom_link = tree.links.new(settings.outputs[0], socket(combine, 'plans'))
    top_link = tree.links.new(upper_settings.outputs[0], socket(combine, 'plans'))
    tree.links.new(combine.outputs[0], socket(output, 'plan'))
    _, layers = render(directory, 'overlay_order')
    require(float(layers[96,48,0]) > .9 and float(layers[96,45,2]) > .9,
            'Overlay must retain the wide lower brush and draw the upper narrow brush on top')
    bottom_link.swap_multi_input_sort_id(top_link)
    _, reordered = render(directory, 'overlay_reordered')
    require(float(reordered[96,48,2]) > .9 and float(reordered[96,48,0]) < .01,
            'Reordering overlay inputs must change which brush is on top')

    reset()
    left = mesh_object('Shared Mesh', [(-.35,-.35,0),(.35,-.35,0),(.35,.35,0),(-.35,.35,0)], faces=[(0,1,2,3)])
    collection = bpy.data.collections.new('Instance Source')
    bpy.context.scene.collection.objects.unlink(left)
    collection.objects.link(left)
    instance = bpy.data.objects.new('Collection Instance', None)
    instance.instance_type = 'COLLECTION'
    instance.instance_collection = collection
    bpy.context.scene.collection.objects.link(instance)
    instance.location.x = -.75
    tree, source, brush, settings, output = setup_lines([instance], width=3)
    _, frame_one = render(directory, 'instance_one')
    require(float(frame_one[:,:,3].max()) > .9, 'Object-list scopes must include a referenced collection instance')
    set_value(source, 'instances', False)
    _, excluded = render(directory, 'instance_disabled')
    require(float(excluded[:,:,3].max()) == 0, 'Include Instances must control generated geometry')
    set_value(source, 'instances', True)
    instance.keyframe_insert(data_path='location', frame=1)
    instance.location.x = .75
    instance.keyframe_insert(data_path='location', frame=2)
    bpy.context.scene.frame_set(2)
    _, frame_two = render(directory, 'instance_two')
    first_x = float(np.argwhere(frame_one[:,:,3] > .1)[:,1].mean())
    second_x = float(np.argwhere(frame_two[:,:,3] > .1)[:,1].mean())
    require(second_x - first_x > 60, 'Evaluated animated instance transforms must move the generated ink')
    bpy.context.scene.frame_set(1)
    _, frame_return = render(directory, 'instance_return')
    require(np.max(np.abs(frame_one - frame_return)) < .002, 'Returning to an animation frame must restore stable ink')

    reset()
    obj = mesh_object('Material Regions',
                      [(-1.5,-.5,0),(-.5,-.5,0),(-.5,.5,0),(-1.5,.5,0),
                       (.5,-.5,0),(1.5,-.5,0),(1.5,.5,0),(.5,.5,0)],
                      faces=[(0,1,2,3),(4,5,6,7)])
    other = obj.data.materials[0].copy()
    obj.data.materials.append(other)
    obj.data.polygons[1].material_index = 1
    tree, source, brush, settings, output = setup_lines([obj], ('wire',))
    set_value(settings, 'material_0', other)
    _, filtered = render(directory, 'material_scope')
    require(float(filtered[:,:96,3].max()) == 0 and float(filtered[:,96:,3].max()) > .9,
            'Material filter must restrict ink to faces using the referenced material')
    set_value(settings, 'exclude_materials', True)
    _, excluded_material = render(directory, 'material_scope_excluded')
    require(float(excluded_material[:,96:,3].max()) == 0 and float(excluded_material[:,:96,3].max()) > .9,
            'Exclude Materials must invert only the material region filter')
    set_value(source, 'exclude_0', obj)
    _, excluded_object = render(directory, 'object_scope_excluded')
    require(float(excluded_object[:,:,3].max()) == 0, 'Source object exclusions must emit an empty range')

    scene = reset()
    wire = mesh_object('Deforming Wire', [(-.8,0,0),(.8,0,0)], edges=[(0,1)])
    wire.shape_key_add(name='Basis')
    shifted_key = wire.shape_key_add(name='Translate X')
    for vertex in shifted_key.data:
        vertex.co.x += .3
    shifted_key.value = 0
    wire.active_shape_key_index = 0
    wire.show_only_shape_key = False
    rig_data = bpy.data.armatures.new('Wire Rig')
    rig = bpy.data.objects.new('Wire Rig', rig_data)
    scene.collection.objects.link(rig)
    rig.select_set(True)
    bpy.context.view_layer.objects.active = rig
    bpy.ops.object.mode_set(mode='EDIT')
    bone = rig_data.edit_bones.new('Stroke')
    bone.head, bone.tail = (0,0,-.5), (0,0,.5)
    bpy.ops.object.mode_set(mode='OBJECT')
    weights = wire.vertex_groups.new(name='Stroke')
    weights.add([0,1], 1, 'REPLACE')
    modifier = wire.modifiers.new('Evaluated Armature', 'ARMATURE')
    modifier.object = rig
    bpy.context.view_layer.update()
    evaluated_baseline = wire.evaluated_get(bpy.context.evaluated_depsgraph_get()).data
    require(abs(evaluated_baseline.vertices[0].co.x + .8) < 1e-5,
            'Deformation fixture baseline must evaluate the zero-valued Basis shape')
    setup_lines([wire], ('wire',))
    _, deform_base = render(directory, 'deform_baseline')
    shifted_key.value = 1
    _, deform_shape = render(directory, 'deform_shape_key')
    rig.pose.bones['Stroke'].location.x = .4
    _, deform_bone = render(directory, 'deform_armature')

    def center_x(pixels):
        alpha = pixels[:,:,3]
        return float((alpha * np.arange(alpha.shape[1])[None,:]).sum() / alpha.sum())

    require(abs(center_x(deform_shape) - center_x(deform_base) - 14.4) < .6,
            'Shape key evaluation must move ink by the expected 0.3 world units')
    require(abs(center_x(deform_bone) - center_x(deform_shape) - 19.2) < .6,
            'Armature modifier evaluation must move ink by the expected 0.4 world units')
    require(tuple(wire.location) == (0,0,0), 'Deformation fixture must keep the object transform unchanged')
    shifted_key.value = 0
    rig.pose.bones['Stroke'].location.x = 0
    _, deform_restored = render(directory, 'deform_restored')
    require(np.max(np.abs(deform_restored - deform_base)) < .002,
            'Resetting shape and bone deformation must restore stable baseline ink')
    return {'groups_and_scalar_inputs': 'passed', 'overlay_and_order': 'passed',
            'animated_instances': 'passed', 'material_and_object_filters': 'passed',
            'evaluated_shape_and_armature': 'passed'}


def test_filter_bridge(directory):
    scene = reset()
    quad = mesh_object('Filter Quad', [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)], faces=[(0,1,2,3)])
    tree, source, brush, settings, output = setup_lines([quad], color=(.8,.1,.05,1), width=4)
    set_value(output, 'composite', False)
    _, reference = render(directory, 'filter_reference')
    graph = bpy.data.node_groups.new('Geometry Line Filter Bridge', 'EeveeFilterGraphNodeTree')
    scene.eevee.filter_graph = graph
    source_node = graph.nodes.new('EeveeFilterGraphNodeSceneColor')
    require('Geometry Line Image' in source_node.outputs, 'Scene Color must expose the independent Geometry Line image')
    stage = graph.nodes.new('EeveeFilterGraphNodeStageOutput')
    graph.links.new(source_node.outputs['Geometry Line Image'], stage.inputs['Image'])
    for name in ('BEFORE_VOLUME_FOG', 'BEFORE_POSTFX', 'BEFORE_DEPTH_OF_FIELD', 'BEFORE_COMPOSITE'):
        stage.execution_stage = name
        stage.is_active_output = True
        combined, raw_line = render(directory, 'filter_' + name.lower())
        require(np.max(np.abs(raw_line - reference)) < .002,
                'Filter Graph must not mutate the independent line image at ' + name)
        require(np.max(np.abs(combined - reference)) < .006,
                'Filter Graph must receive premultiplied ink at ' + name)
    scene.render.use_motion_blur = True
    scene.render.motion_blur_shutter = .8
    scene.eevee.motion_blur_steps = 1
    scene.eevee.taa_render_samples = 4
    quad.location.x = -.6
    quad.keyframe_insert(data_path='location', frame=0)
    quad.location.x = .6
    quad.keyframe_insert(data_path='location', frame=2)
    for layer in quad.animation_data.action.layers:
        for strip in layer.strips:
            for channelbag in strip.channelbags:
                for curve in channelbag.fcurves:
                    for key in curve.keyframe_points:
                        key.interpolation = 'LINEAR'
    scene.frame_set(1)
    set_value(output, 'motion_blur', True)
    combined, canonical = render(directory, 'filter_animated_canonical')
    require(np.max(np.abs(canonical - reference)) < .002,
            'Animated Geometry Line pass must retain the canonical frame ink')
    # Filter inputs pass through Film's sample reconstruction; the independent line pass does not.
    # Permit its one-pixel edge filter while rejecting shutter displacement, energy loss, or a
    # change from premultiplied ink. The four one-sample stage comparisons above remain exact.
    canonical_alpha, combined_alpha = canonical[:,:,3], combined[:,:,3]
    source_mask = canonical_alpha > .001
    padded = np.pad(source_mask, 1)
    height, width = source_mask.shape
    expanded = np.logical_or.reduce([padded[y:y+height,x:x+width]
                                     for y in range(3) for x in range(3)])
    require(float(combined_alpha[~expanded].max()) < .002,
            'Filtered canonical ink may expand by one Film pixel, not shutter displacement')
    yy, xx = np.indices(canonical_alpha.shape)
    for coordinate in (xx, yy):
        raw_center = float((coordinate * canonical_alpha).sum() / canonical_alpha.sum())
        filtered_center = float((coordinate * combined_alpha).sum() / combined_alpha.sum())
        require(abs(raw_center - filtered_center) < .5,
                'Film reconstruction must keep the canonical ink centroid within half a pixel')
    require(abs(float(combined_alpha.sum() / canonical_alpha.sum()) - 1) < .01,
            'Film reconstruction must preserve canonical ink alpha energy')
    color_error = np.abs(combined[:,:,:3] - combined_alpha[:,:,None] * np.array((.8,.1,.05)))
    fringe = (combined_alpha > .01) & (combined_alpha < .99)
    require(np.max(color_error[fringe]) < .003 and np.max(color_error) < .006,
            'Filter Graph reconstruction must preserve premultiplied line color, including alpha-clamped interiors')
    scene.eevee.use_geometry_lines = False
    combined, _ = render(directory, 'filter_lines_disabled')
    require(float(combined.max()) == 0, 'Disabled geometry lines must produce transparent black in the Filter Graph')
    return {'filter_bridge': 'passed', 'filter_stages': 4, 'filter_animated_canonical': 'passed'}


def test_camera_effects(directory):
    scene = reset()
    scene.eevee.taa_render_samples = 8
    camera = scene.camera.data
    camera.type = 'PERSP'
    camera.lens = 50
    camera.dof.use_dof = True
    camera.dof.focus_distance = 2
    camera.dof.aperture_fstop = .5
    obj = mesh_object('Defocused Ink', [(0,-.8,0),(0,.8,0)], edges=[(0,1)])
    tree, source, brush, settings, output = setup_lines([obj], ('wire',), width=4)
    sharp, raw_sharp = render(directory, 'camera_dof_disabled')
    require(np.max(np.abs(sharp - raw_sharp)) < .006, 'Default camera effects must leave loose-wire ink crisp')
    set_value(output, 'depth_of_field', True)
    defocused, raw_defocused = render(directory, 'camera_dof_enabled')
    require(np.max(np.abs(raw_sharp - raw_defocused)) < .002, 'Depth of field must preserve the independent raw line pass')
    dof_spread = int(np.count_nonzero((raw_sharp[:,:,3] < .01) & (defocused[:,:,3] > .01)))
    require(dof_spread > 50, 'Enabled line depth of field must spread alpha outside the crisp line')
    repeated_dof, repeated_raw = render(directory, 'camera_dof_repeated')
    require(np.max(np.abs(repeated_dof - defocused)) < .01 and
            np.max(np.abs(repeated_raw - raw_sharp)) < .002,
            'Repeated final DOF renders must not accumulate previous-render history')
    camera.dof.focus_distance = 5
    focused, focused_raw = render(directory, 'camera_focus_changed')
    require(np.max(np.abs(focused_raw - raw_sharp)) < .002, 'Focus changes must preserve raw line geometry')
    require(np.count_nonzero((raw_sharp[:,:,3] < .01) & (focused[:,:,3] > .01)) < 20,
            'Changing focus to the ink plane must remove the old defocus halo')
    camera.dof.focus_distance = 2
    returned_dof, _ = render(directory, 'camera_focus_returned')
    require(np.max(np.abs(returned_dof - defocused)) < .01,
            'Returning to a focus distance must not retain the previous focus history')

    scene = reset()
    scene.render.use_motion_blur = True
    scene.render.motion_blur_shutter = .8
    scene.render.motion_blur_position = 'CENTER'
    scene.eevee.motion_blur_steps = 1
    scene.eevee.taa_render_samples = 8
    obj = mesh_object('Animated Ink', [(0,-.8,0),(0,.8,0)], edges=[(0,1)])
    obj.location.x = -1
    obj.keyframe_insert(data_path='location', frame=0)
    obj.location.x = 1
    obj.keyframe_insert(data_path='location', frame=2)
    for layer in obj.animation_data.action.layers:
        for strip in layer.strips:
            for channelbag in strip.channelbags:
                for curve in channelbag.fcurves:
                    for key in curve.keyframe_points:
                        key.interpolation = 'LINEAR'
    scene.frame_set(1)
    tree, source, brush, settings, output = setup_lines([obj], ('wire',), width=4)
    set_value(output, 'motion_blur', True)
    # The blurred result is the very first F12, so this cannot borrow viewport/render history.
    moving, raw_moving = render(directory, 'camera_motion_first_render')
    raw_center_x = float(np.argwhere(raw_moving[:,:,3] > .1)[:,1].mean())
    require(abs(raw_center_x - 95.5) < 1,
            'Raw motion line pass must use the canonical frame position, not the last shutter sample')
    set_value(output, 'motion_blur', False)
    crisp, raw_crisp = render(directory, 'camera_motion_disabled')
    require(np.max(np.abs(raw_moving - raw_crisp)) < .002, 'Motion blur must preserve the independent raw line pass')
    require(np.max(np.abs(crisp - raw_crisp)) < .006, 'Disabled line motion blur must preserve crisp ink')
    motion_spread = int(np.count_nonzero((raw_crisp[:,:,3] < .01) & (moving[:,:,3] > .01)))
    require(motion_spread > 50, 'The first final render must blur animated line alpha beyond the current position')
    spans = []

    def check_motion_support(pixels, label):
        points = np.argwhere(pixels[:,:,3] > .01)
        require(len(points) > 50, 'Animated ink must retain meaningful alpha: ' + label)
        low, high = int(points[:,1].min()), int(points[:,1].max())
        span = high - low + 1
        # One world unit per frame * .8 shutter * 48 px/world + 4 px ink = 42.4 px.
        # Permit raster/filter tails, but reject a whole-frame or last-shutter-only smear.
        require(25 <= span <= 56 and low < 85 and high > 106,
                f'Motion support must cover the complete shutter with the correct scale: {label}, x={low}..{high}')
        require(float(pixels[:,:,3].max()) > .04, 'Motion alpha must not be diluted by an oversized velocity: ' + label)
        spans.append(span)

    check_motion_support(moving, 'steps1')
    set_value(output, 'motion_blur', True)
    for steps in (2,4):
        scene.eevee.motion_blur_steps = steps
        scene.eevee.taa_render_samples = 8
        sampled, sampled_raw = render(directory, 'camera_motion_steps_' + str(steps))
        require(np.max(np.abs(sampled_raw - raw_crisp)) < .002,
                'Increasing shutter steps must preserve the canonical raw pass')
        check_motion_support(sampled, 'steps' + str(steps))
        repeated, repeated_raw = render(directory, 'camera_motion_steps_' + str(steps) + '_repeated')
        require(np.max(np.abs(repeated_raw - sampled_raw)) < .002 and
                np.max(np.abs(repeated - sampled)) < .01,
                'Repeated same-frame motion renders must reset temporal accumulation')
    require(max(spans) - min(spans) <= 8, 'Shutter subdivisions must not collapse motion support to the final segment')
    scene.frame_set(2)
    advanced, advanced_raw = render(directory, 'camera_motion_frame_advanced')
    advanced_center = float(np.argwhere(advanced_raw[:,:,3] > .1)[:,1].mean())
    require(abs(advanced_center - 143.5) < 1, 'Advancing the frame must update canonical line geometry')
    require(float(advanced[:,:105,3].max()) < .001, 'A new frame must not retain the preceding frame alpha trail')
    return {'camera_dof': 'passed', 'camera_motion_first_render': 'passed',
            'dof_spread_pixels': dof_spread, 'motion_spread_pixels': motion_spread,
            'motion_support_spans': spans, 'camera_temporal_history': 'passed'}


def test_brush_details(directory):
    reset()
    obj = mesh_object('Brush Path', [(-1,0,0),(1,0,0)], edges=[(0,1)])
    tree, source, brush, settings, output = setup_lines([obj], ('wire',), width=8)
    _, solid = render(directory, 'brush_solid')
    set_value(brush, 'opacity', .35)
    _, translucent = render(directory, 'brush_simple_opacity')
    center_alpha = translucent[96,70:121,3]
    print('GEOMETRY_LINES_SIMPLE_ALPHA ' + json.dumps({'requested': .35,
          'minimum': float(center_alpha.min()), 'maximum': float(center_alpha.max()),
          'mean': float(center_alpha.mean())}), flush=True)
    require(np.max(np.abs(center_alpha - .35)) < .02,
            'Simple brush opacity must remain 0.35 along a long straight stroke without overlapping segment buildup')
    for mode in ('NORMAL', 'MULTIPLE'):
        brush.mode = mode
        _, coverage = render(directory, 'brush_' + mode.lower() + '_opacity')
        require(float(coverage[:, :, 3].max()) <= .37,
                mode + ' must combine overlapping stamps by maximum coverage, matching the web reference')
        require(float(coverage[96, 70:121, 3].mean()) > .28,
                mode + ' must still cover the central straight stroke')
        set_value(brush, 'width', .08)
        _, subpixel = render(directory, 'brush_' + mode.lower() + '_tiny_tip')
        require(float(subpixel[:, :, 3].max()) == 0,
                mode + ' must suppress tips below the reference 0.05 pixel radius floor')
        set_value(brush, 'width', 8)
    brush.mode = 'SIMPLE'
    set_value(brush, 'opacity', 1)
    set_value(brush, 'dashed', True)
    set_value(brush, 'dash_length', 8)
    set_value(brush, 'dash_space', 8)
    _, dashed = render(directory, 'brush_dashed')
    solid_pixels = np.count_nonzero(solid[:,:,3] > .1)
    dashed_pixels = np.count_nonzero(dashed[:,:,3] > .1)
    require(solid_pixels * .3 < dashed_pixels < solid_pixels * .7,
            'Dashed brush must remove alternating arc-length intervals')
    set_value(brush, 'dashed', False)
    set_value(brush, 'shift_y', 12)
    _, shifted = render(directory, 'brush_offset')
    solid_center = np.argwhere(solid[:,:,3] > .1).mean(axis=0)
    shifted_center = np.argwhere(shifted[:,:,3] > .1).mean(axis=0)
    require(abs(abs(float(shifted_center[0] - solid_center[0])) - 12) < .5,
            'Path XY offset must move the actual ink by the requested pixel distance')
    set_value(brush, 'shift_y', 0)
    brush.mode = 'NORMAL'
    value_image = alpha_image('Invisible Brush Tip', transparent=True)
    set_value(brush, 'image', value_image)
    _, transparent = render(directory, 'brush_transparent_tip')
    require(float(transparent[:,:,3].max()) == 0, 'Normal brush must consume the tip image alpha')
    set_value(brush, 'image', alpha_image('Half Brush Tip', half=True))
    _, textured = render(directory, 'brush_textured_tip')
    require(np.count_nonzero(textured[:,:,3] > .1) > 50, 'Packed Normal brush texture must draw')
    brush.mode = 'MULTIPLE'
    set_value(brush, 'width_random', .5)
    set_value(brush, 'noise', 4)
    set_value(brush, 'seed', 19)
    _, seeded = render(directory, 'brush_seeded')
    _, same_seed = render(directory, 'brush_same_seed')
    require(np.max(np.abs(seeded - same_seed)) < .002, 'Fixed brush seeds must reproduce stable coverage')
    set_value(brush, 'seed', 20)
    _, other_seed = render(directory, 'brush_other_seed')
    require(np.count_nonzero(np.abs(seeded[:,:,3] - other_seed[:,:,3]) > .05) > 50,
            'Changing the fixed seed must change the enabled brush variation')
    brush.mode = 'SIMPLE'
    set_value(brush, 'width_random', 0)
    set_value(brush, 'noise', 0)
    set_value(brush, 'opacity', .35)
    second_rule = make_settings(tree, source, brush, types=('wire',))
    combine = tree.nodes.new('NPRLineNodeCombine')
    combine.mode = 'OVERLAY'
    tree.links.new(settings.outputs[0], socket(combine, 'plans'))
    tree.links.new(second_rule.outputs[0], socket(combine, 'plans'))
    tree.links.new(combine.outputs[0], socket(output, 'plan'))
    _, layered = render(directory, 'brush_translucent_overlay')
    require(np.max(np.abs(layered[96,70:121,3] - (1 - .65**2))) < .02,
            'Independent overlay rules must composite twice even when they share a Simple brush')
    combine.mode = 'PRIORITY'
    _, priority = render(directory, 'brush_translucent_priority')
    require(np.max(np.abs(priority[96,70:121,3] - .35)) < .02,
            'Priority rules sharing a source must apply translucent ink only once')
    return {'brush_textures_dashes_offsets_and_seeds': 'passed'}


def test_feedback(directory):
    reset()
    obj = mesh_object('Split Seams',
        [(-1,-1,0), (0,-1,0), (0,1,0), (-1,1,0),
         (0,-1,0), (1,-1,0), (1,1,0), (0,1,0)],
        faces=[(0,1,2,3), (4,5,6,7)])
    tree, source, brush, settings, output = setup_lines([obj])
    original_counts = (len(obj.data.vertices), len(obj.data.edges), len(obj.data.polygons))
    require(not socket(source, 'repair_seams').default_value,
            'Virtual seam adjacency must be explicitly enabled')
    tree.show_performance = True
    _, native = render(directory, 'seam_native')
    require(float(native[90:102, 94:98, 3].max()) > .5, 'Native disconnected seam must remain visible')
    set_value(source, 'repair_seams', True)
    _, repaired = render(directory, 'seam_virtual_adjacency')
    require(float(repaired[90:102, 94:98, 3].max()) == 0,
            'Exact opposite boundary pairs must act as a connected smooth interior edge')
    require(np.count_nonzero(repaired[:, :, 3] > .1) > 100, 'The true outer boundary must remain')
    require(original_counts == (len(obj.data.vertices), len(obj.data.edges), len(obj.data.polygons)),
            'Virtual adjacency must not modify the actual mesh')
    timing = tree.performance_info
    require('CPU' in timing and 'ms' in timing and 'segments' in timing,
            'Output timings must expose actual CPU measurements and geometry counts')
    require(not tree.keys(), 'Transient timing state must not create persistent custom properties')
    # A separate source using original topology must keep its seams when both
    # source interpretations share a geometry snapshot.
    native_source = make_source(tree, [obj])
    native_settings = make_settings(tree, native_source, brush)
    combine = tree.nodes.new('NPRLineNodeCombine')
    combine.mode = 'OVERLAY'
    tree.links.new(settings.outputs[0], socket(combine, 'plans'))
    tree.links.new(native_settings.outputs[0], socket(combine, 'plans'))
    tree.links.new(combine.outputs[0], socket(output, 'plan'))
    _, mixed = render(directory, 'seam_mixed_scopes')
    require(float(mixed[90:102, 94:98, 3].max()) > .5,
            'One repaired scope must not change the native topology of another scope')
    reset()
    ambiguous = mesh_object('Ambiguous Boundary',
        [(-1,-1,0), (0,-1,0), (0,1,0), (-1,1,0),
         (0,-1,0), (1,-1,0), (1,1,0), (0,1,0),
         (0,-1,0), (0,1,0), (0,0,1)],
        faces=[(0,1,2,3), (4,5,6,7), (8,9,10)])
    tree, source, brush, settings, output = setup_lines([ambiguous])
    _, ambiguous_native = render(directory, 'seam_ambiguous_native')
    set_value(source, 'repair_seams', True)
    _, ambiguous_repair = render(directory, 'seam_ambiguous_repair')
    require(np.max(np.abs(ambiguous_native - ambiguous_repair)) < .002,
            'Three coincident boundary edges must remain ambiguous and preserve the native result')
    # Two coincident sheets with opposite normals are intentionally separate.
    reset()
    sheets = mesh_object('Back To Back',
        [(-1,-1,0), (1,-1,0), (1,1,0), (-1,1,0)] * 2,
        faces=[(0,1,2,3), (7,6,5,4)])
    tree, source, brush, settings, output = setup_lines([sheets])
    set_value(source, 'repair_seams', True)
    _, sheet_ink = render(directory, 'seam_back_to_back')
    require(np.count_nonzero(sheet_ink[:, :, 3] > .1) > 100,
            'Coincident opposite-facing sheets must not be joined away')
    from bl_operators import npr_lines
    before = (len(bpy.data.images), len(bpy.data.objects), len(bpy.data.meshes))
    second = make_brush(tree, color=(1, 0, 0, 1), width=7)
    first_icon, second_icon = brush.brush_preview_icon, second.brush_preview_icon
    if not bpy.app.background:
        require(first_icon > 0 and second_icon > 0 and first_icon != second_icon,
                'Each native brush node must have an independently cached inline preview')
    require(brush.brush_preview_icon == first_icon, 'An unchanged node must reuse its preview')
    a = npr_lines.brush_preview(brush, 192, 64)
    b = npr_lines.brush_preview(second, 192, 64)
    require(a != b, 'Two nodes must own separate preview resources even in background mode')
    require(tuple(a.image_size) == (192, 64) and tuple(a.image_pixels_float) != tuple(b.image_pixels_float),
            'Inline previews must represent their own brush settings')
    require(before == (len(bpy.data.images), len(bpy.data.objects), len(bpy.data.meshes)),
            'Inline previews must not create scene data-blocks')
    npr_lines.clear_brush_preview()
    tree.show_performance = True
    tree_name, source_name = tree.name, source.name
    filename = directory / 'seam-preview-roundtrip.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(filename))
    bpy.ops.wm.open_mainfile(filepath=str(filename))
    restored = bpy.data.node_groups[tree_name]
    require(socket(restored.nodes[source_name], 'repair_seams').default_value,
            'Virtual seam interpretation must survive saving and reopening')
    require(not restored.show_performance and restored.performance_info == '',
            'Session timing enablement and measurements must reset after opening a file')
    return {'seam_adjacency_inline_preview_cpu_timings': 'passed'}


def test_pencil_controls(directory):
    reset()
    obj = mesh_object('Curve Stroke', [(-1,0,0),(1,0,0)], edges=[(0,1)])
    tree, source, brush, settings, output = setup_lines([obj], ('wire',), width=10)
    require(not brush.use_width_curve and not brush.use_opacity_curve and not brush.raster_size_response,
            'Existing brushes must preserve analytic width and linear reduction defaults')
    _, full = render(directory, 'curve_disabled')
    brush.use_width_curve = True
    require(len(brush.width_curve.curves[0].points)==3,
            'A new stroke envelope must contain one peak between two endpoints')
    # A vector-handled triangle gives an exact known envelope for GPU validation;
    # automatic Bezier handles intentionally produce a broader bell.
    for point in brush.width_curve.curves[0].points:
        point.handle_type='VECTOR'
    brush.width_curve.update()
    _, curved = render(directory, 'curve_width')
    require(float(curved[:,48:56,3].sum()) < float(full[:,48:56,3].sum())*.4,
            'Width curves must taper the beginning of actual GPU strokes')
    require(float(curved[:,90:102,3].sum()) > float(full[:,90:102,3].sum())*.8,
            'The middle of a bell curve must retain the brush width')
    brush.width_curve.curves[0].points[1].location[1] = .25
    brush.width_curve.update()
    tree.update_tag()
    _, thinner = render(directory, 'curve_width_edited')
    require(float(thinner[:,:,3].sum()) < float(curved[:,:,3].sum())*.4,
            'Editing a curve must invalidate drawing while preserving the geometric stroke')
    copied = tree.copy().nodes[brush.name]
    copied.width_curve.curves[0].points[1].location[1] = .8
    copied.width_curve.update()
    require(abs(brush.width_curve.curves[0].points[1].location[1]-.25) < 1e-6,
            'Copied brushes must own their width curves')
    brush.use_width_curve = False
    brush.use_opacity_curve = True
    brush.opacity_curve.curves[0].points[1].location[1] = .2
    brush.opacity_curve.update()
    tree.update_tag()
    _, alpha = render(directory, 'curve_opacity')
    require(.18 < float(alpha[:,:,3].max()) < .21,
            'Opacity curve values must be applied in the fragment shader')
    from bl_operators import npr_lines
    first = tuple(npr_lines.brush_preview(brush,192,64).image_pixels_float)
    brush.opacity_curve.curves[0].points[1].location[1] = .7
    brush.opacity_curve.update()
    second = tuple(npr_lines.brush_preview(brush,192,64).image_pixels_float)
    require(first != second, 'Inline previews must invalidate when curve points change')
    tree_name,brush_name = tree.name,brush.name
    filename = directory/'brush-curves-roundtrip.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(filename))
    bpy.ops.wm.open_mainfile(filepath=str(filename))
    restored = bpy.data.node_groups[tree_name].nodes[brush_name]
    require(restored.use_opacity_curve and not restored.use_width_curve and
            abs(restored.opacity_curve.curves[0].points[1].location[1]-.7) < 1e-6 and
            abs(restored.width_curve.curves[0].points[1].location[1]-.25) < 1e-6,
            'Both curve data and enable flags must survive saving and reopening')
    reset()
    obj = mesh_object('Split Endpoint Stroke', [(-1,0,0),(0,0,0),(0,0,0),(1,0,0)], edges=[(0,1),(2,3)])
    tree,source,brush,settings,output = setup_lines([obj], ('wire',), width=10)
    set_value(settings,'gap_join',0)
    set_value(brush,'taper_start',.5)
    set_value(brush,'taper_end',.5)
    _, disconnected = render(directory,'endpoint_topology')
    set_value(settings,'connect_coincident',True)
    _, joined = render(directory,'endpoint_coincident')
    require(float(disconnected[96,96,3]) < .5 and float(joined[96,96,3]) > .9,
            'Optional coincident endpoint joining must connect split vertices without a new gap')
    require(len(obj.data.vertices)==4, 'Stroke joining must not weld the mesh')
    reset()
    obj=mesh_object('Right Angle', [(-1,-1,0),(0,-1,0),(0,1,0)], edges=[(0,1),(1,2)])
    tree,source,brush,settings,output=setup_lines([obj],('wire',),width=6)
    set_value(settings,'gap_join',0)
    set_value(settings,'split_angle',90)
    tree.show_performance=True
    render(directory,'endpoint_ninety_degrees')
    require('1 strokes' in tree.performance_info, 'An exact 90 degree turn must not split at a 90 degree threshold')
    reset()
    obj=mesh_object('Raster Width', [(-1,0,0),(1,0,0)], edges=[(0,1)])
    tree,source,brush,settings,output=setup_lines([obj],('wire',),width=3)
    brush.raster_size_response=True
    set_value(output,'samples',2)
    _, raster=render(directory,'raster_size_response')
    require(abs(float(raster[88:104,96,3].sum())-4)<.03,
            'Calibrated Simple raster coverage must reproduce the measured 3px reference cross section')
    reset()
    obj=mesh_object('Offscreen Curve', [(-4,0,0),(4,0,0)], edges=[(0,1)])
    tree,source,brush,settings,output=setup_lines([obj],('wire',),width=10)
    set_value(brush,'taper_start',.5)
    set_value(brush,'taper_end',.5)
    _, automatic=render(directory,'offscreen_automatic')
    set_value(output,'offscreen_margin',150)
    _, continued=render(directory,'offscreen_continuation')
    require(float(continued[:,1,3].sum()) > float(automatic[:,1,3].sum())+.4,
            'Offscreen stroke continuation must keep a clipped taper from restarting at the frame')
    return {'pencil_curves_raster_coverage_endpoint_and_margin': 'passed'}


def test_pencil_dash_caps(directory):
    reset()
    obj = mesh_object('Rounded Dash Stroke', [(-1.5, 0, 0), (1.5, 0, 0)], edges=[(0, 1)])
    tree, source, brush, settings, output = setup_lines([obj], ('wire',), width=4)
    brush.raster_size_response = True
    set_value(brush, 'dashed', True)
    set_value(brush, 'dash_length', 9)
    set_value(brush, 'dash_space', 10)
    _, dashed = render(directory, 'pencil_rounded_dash_caps')
    # The first body ends at x=33. Its round cap covers x=34; the center
    # between that cap and the next body at x=43 must remain transparent.
    require(float(dashed[95, 34, 3]) > .9,
            'Simple dash endpoints must retain the round coverage measured in Pencil')
    require(float(dashed[95, 38, 3]) < .001,
            'Round dash caps must preserve the remaining gap')
    reset()
    obj = mesh_object('Closed Curve Stroke', [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)],
                      edges=[(0,1),(1,2),(2,3),(3,0)])
    tree, source, brush, settings, output = setup_lines([obj], ('wire',), width=10)
    brush.raster_size_response = True
    set_value(settings, 'split_angle', 180)
    _, full = render(directory, 'pencil_closed_curve_full')
    brush.use_width_curve = True
    for point in brush.width_curve.curves[0].points:
        point.location.y = .25
    brush.width_curve.update()
    _, reduced = render(directory, 'pencil_closed_curve_reduced')
    require(float(reduced[:,:,3].sum()) < float(full[:,:,3].sum()) * .4,
            'Explicit width curves must also affect closed strokes, as in Pencil')
    return {'pencil_simple_dash_round_caps': 'passed', 'pencil_closed_stroke_curves': 'passed'}


def test_pencil_geometry(directory):
    scene=reset()
    foreground=mesh_object('Contour Foreground',[(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)],faces=[(0,1,2,3)])
    mesh_object('Other Object Behind',[(-2,-2,-.2),(2,-2,-.2),(2,2,-.2),(-2,2,-.2)],faces=[(0,1,2,3)])
    tree,source,brush,settings,output=setup_lines([foreground],('outline',))
    _,legacy=render(directory,'contour_scene_scope')
    require(float(legacy[:,:,3].max())==0,'Legacy scene context must still classify a fully covered outline as interior')
    set_value(settings,'object_contours',True)
    _,outer=render(directory,'contour_object_scope')
    require(np.count_nonzero(outer[:,:,3]>.1)>100,'A different object behind a target must not turn its silhouette into an inner contour')
    set_value(settings,'visible_open_outline',False)
    _,closed_only=render(directory,'contour_open_outline_disabled')
    require(float(closed_only[:,:,3].max())==0,'Visible outline open edges must have an independent switch')
    set_value(settings,'visible_open_outline',True)
    set_types(settings,'object')
    _,inner=render(directory,'contour_other_object_is_not_self')
    require(float(inner[:,:,3].max())==0,'Per-object inner contours must exclude coverage from other objects')
    reset()
    obj=mesh_object('Open Split Surface',[(-1,-1,0),(0,-1,0),(0,1,0),(-1,1,0),(0,-1,0),(1,-1,0),(1,1,0),(0,1,0)],
                    faces=[(0,1,2,3),(4,5,6,7)])
    tree,source,brush,settings,output=setup_lines([obj],('outline',))
    set_value(settings,'object_contours',True)
    _,outer=render(directory,'contour_split_surface_outer')
    require(float(outer[90:102,95:97,3].max())==0,'A split coplanar seam covered on both sides belongs to the inner contour')
    set_types(settings,'object')
    _,inner=render(directory,'contour_split_surface_inner')
    require(float(inner[96,96,3])>.7,'Per-object classification must include incident faces in side coverage')
    set_types(settings,'smoothing')
    _,boundary=render(directory,'smoothing_open_edges_excluded')
    require(float(boundary[:,:,3].max())==0,'Open edges cannot form a boundary between adjacent smoothing groups')
    scene=reset()
    fold=mesh_object('Split Contact',[(-1,-1,0),(0,-1,0),(0,1,0),(-1,1,0),
                                    (0,-1,0),(.5,-1,.8660254),(.5,1,.8660254),(0,1,0)],
                     faces=[(0,1,2,3),(4,5,6,7)])
    tree,source,brush,settings,output=setup_lines([fold],('intersection',))
    _,front=render(directory,'intersection_split_contact_front')
    require(float(front[96,96,3])>.7,'Position-coincident split vertices must retain valid intersection contact strokes')
    from mathutils import Vector
    scene.camera.location=(3,0,.2)
    scene.camera.rotation_euler=(-scene.camera.location).to_track_quat('-Z','Y').to_euler()
    _,back=render(directory,'intersection_backfacing_source')
    require(float(back[:,:,3].max())==0,'An intersection with a backfacing source must not be emitted as a visible line')
    return {'pencil_contour_scope_smoothing_and_intersection_visibility':'passed'}


def test_stroke_orientation(directory):
    reset()
    obj=mesh_object('Orientation Stroke',[(0,-1,0),(0,1,0)],edges=[(0,1)])
    tree,source,brush,settings,output=setup_lines([obj],('wire',),width=8)
    brush.mode='NORMAL'
    set_value(brush,'stretch',3)
    brush.stroke_mode='RAKE'
    _,rake=render(directory,'orientation_follow_path')
    brush.stroke_mode='NORMAL'
    _,fixed=render(directory,'orientation_fixed_angle')
    require(float(fixed[96,:,3].sum()) > float(rake[96,:,3].sum())*2,
            'A fixed horizontal ellipse must remain horizontal on a vertical stroke')
    brush.stroke_mode='RANDOM'
    set_value(brush,'angle_random',180)
    set_value(brush,'seed',31)
    _,random=render(directory,'orientation_random_angle')
    _,repeat=render(directory,'orientation_random_repeat')
    require(np.max(np.abs(random-repeat))<.002,'Random orientation must reproduce the fixed seed')
    require(np.count_nonzero(np.abs(random[:,:,3]-fixed[:,:,3])>.05)>20,
            'Random orientation must change the enabled non-round brush coverage')
    return {'normal_rake_random_stroke_orientation':'passed'}


def test_multiple_tip(directory):
    reset()
    obj=mesh_object('Reusable Multiple Tip',[(-1,0,0),(1,0,0)],edges=[(0,1)])
    tree,source,brush,settings,output=setup_lines([obj],('wire',),width=48)
    brush.mode='MULTIPLE'
    brush.stroke_mode='NORMAL'
    for key,value in {'spacing':.02,'dot_size':5,'dot_size_random':0,
                      'dot_space_random_x':0,'dot_space_random_y':0,'dot_antialiasing':0}.items():
        set_value(brush,key,value)
    _,grid=render(directory,'multiple_fixed_grid')
    profile=grid[:,88:104,3].mean(axis=1)
    bands=int(np.count_nonzero(np.diff(np.pad((profile>.5).astype(np.int8),1))==1))
    require(bands>=4,'A fixed multiple tip must preserve parallel bands instead of new per-stamp random points')
    # A subpixel stamp lattice can ripple even with a fixed tip. Five ~1.8px
    # stamp steps repeat after ~9px; changing the tip at every stamp does not.
    require(float(np.abs(grid[78:114,70:104,3]-grid[78:114,79:113,3]).mean())<.004,
            'A fixed tip must repeat on the straight stroke stamp lattice')
    set_value(brush,'dot_space_y',.3)
    _,spaced=render(directory,'multiple_vertical_spacing')
    profile=spaced[:,88:104,3].mean(axis=1)
    spaced_bands=int(np.count_nonzero(np.diff(np.pad((profile>.5).astype(np.int8),1))==1))
    require(1<spaced_bands<bands,'Vertical spacing must change the number of dot rows independently')
    for key,value in {'dot_size_random':.8,'dot_space_random_x':1,'dot_space_random_y':1,'seed':27}.items():
        set_value(brush,key,value)
    _,random=render(directory,'multiple_seeded_tip')
    _,repeat=render(directory,'multiple_seeded_tip_repeat')
    require(float(np.max(np.abs(random-repeat)))<.002,'Reusing a fixed seed must reproduce the entire tip')
    set_value(brush,'seed',28)
    _,other=render(directory,'multiple_other_tip_seed')
    require(float(np.abs(random-other).sum())>10,'The tip seed must change the cached dot pattern')
    from bl_operators.npr_lines import brush_preview
    preview=tuple(brush_preview(brush).image_pixels_float[:])
    set_value(brush,'dot_size',12)
    updated=tuple(brush_preview(brush).image_pixels_float[:])
    require(preview!=updated,'The inline preview must respond to dot settings without scene evaluation')
    brush.mode='NORMAL'
    _,hard=render(directory,'tip_hard_edge')
    set_value(brush,'tip_reduction_start',0)
    _,soft=render(directory,'tip_radial_fade')
    require(float(soft[:,:,3].sum())<float(hard[:,:,3].sum())*.85,
            'Tip radial fade must reduce coverage separately from stroke start and end')
    set_value(brush,'tip_reduction_start',1)
    set_value(brush,'groove',1)
    set_value(brush,'groove_count',5)
    _,groove=render(directory,'tip_groove_five')
    set_value(brush,'groove_count',10)
    _,more_grooves=render(directory,'tip_groove_ten')
    require(float(np.abs(groove-more_grooves).sum())>10,'Groove count must change the tip boundary')
    filename=Path(directory)/'multiple-tip-roundtrip.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(filename))
    bpy.ops.wm.open_mainfile(filepath=str(filename))
    restored=next(
        n for n in bpy.context.scene.eevee.geometry_line_tree.nodes if n.bl_idname=='NPRLineNodeBrush')
    require(abs(socket(restored,'dot_size').default_value-12)<1e-5 and
            socket(restored,'groove_count').default_value==10,
            'Tip parameters must survive saving and reopening independently of visible mode')
    return {'cached_multiple_tip_spacing_seed_preview_and_radial_fade':'passed'}


def test_raster_pressure_and_visibility_grid(directory):
    scene=reset();scene.render.resolution_x=scene.render.resolution_y=256
    obj=mesh_object('Raster Pressure Reference',[(-1.5,-1.5,0),(1.5,-1.5,0),(-1.5,-1.25,0),(1.5,-1.25,0),(-1.5,-1,0),(1.5,-1,0)],
                    faces=[(0,1,3,2),(2,3,5,4)])
    marks=obj.data.attributes.new('freestyle_edge','BOOLEAN','EDGE')
    for edge in obj.data.edges:marks.data[edge.index].value=set(edge.vertices)=={2,3}
    tree,source,brush,settings,output=setup_lines([obj],('marked',),width=8)
    brush.raster_size_response=True;brush.use_width_curve=True
    set_value(settings,'direction',0);set_value(settings,'gap_join',0)
    points=brush.width_curve.curves[0].points
    while len(points)>2:points.remove(points[1])
    points[0].location=(0,0);points[-1].location=(1,1)
    for point in points:point.handle_type='VECTOR'
    brush.width_curve.update()
    profile=lambda a,x:float(a[190:230,x,3].sum())
    _,ramp=render(directory,'pressure_raster_ramp')
    require(abs(profile(ramp,48)-.18608767)<.025,'Start pressure must scale the raster radius domain, not the nominal width')
    brush.raster_size_response=False;_,analytic=render(directory,'pressure_analytic_ramp')
    require(abs(profile(analytic,48)-.6875)<.07,'Analytic width mode must retain its linear pressure interpretation')
    brush.raster_size_response=True
    image=bpy.data.images.new('Pressure Half Width',1,1,alpha=True,float_buffer=True)
    image.colorspace_settings.name='Non-Color';image.pixels.foreach_set([.5,.5,.5,1]);image.update()
    mapping=tree.nodes.new('NPRLineNodeMap');mapping.mode='SCREEN';set_value(mapping,'image',image)
    tree.links.new(mapping.outputs[0],socket(brush,'width_map'));_,mapped=render(directory,'pressure_width_map_composition')
    require(abs(profile(mapped,48)-profile(ramp,48)*.25)<.015,
            'Two subpixel radius factors must compose before squared opacity')
    tree.links.remove(socket(brush,'width_map').links[0])
    reduction=tree.nodes.new('NPRLineNodeReduction')
    for point in reduction.curve.curves[0].points:point.location[1]=.5;point.handle_type='VECTOR'
    reduction.curve.update();tree.links.new(reduction.outputs[0],socket(settings,'width_reduction'))
    _,distance=render(directory,'pressure_distance_composition')
    require(abs(profile(distance,48)-profile(mapped,48))<.015,'Distance width scaling must use the same composable radius domain')
    tree.links.remove(socket(settings,'width_reduction').links[0])
    brush.use_width_curve=False;set_value(brush,'width',3)
    blocker=mesh_object('Pressure Occluder',[(-1,-1.45,.5),(1,-1.45,.5),(1,-1.05,.5),(-1,-1.05,.5)],faces=[(0,1,2,3)])
    for index,expected in [(0,47.444682),(3,47.761571),(9,48.112013)]:
        for i in (0,3):blocker.data.vertices[i].co.x=-1+index*.1/64
        blocker.data.update();_,a=render(directory,'visibility_terminal_cell_'+str(index))
        band=a[198:218,24:96,3];_,xx=np.mgrid[198:218,24:96]
        center=float((band*xx).sum()/band.sum())
        require(abs(center-expected)<.045,'The terminal visibility cell must match the measured cut-boundary phase: '+str(center))
    for i in (0,3):blocker.data.vertices[i].co.x=36/64-2
    for i in (1,2):blocker.data.vertices[i].co.x=220/64-2
    blocker.data.update();brush.use_width_curve=True;set_value(brush,'width',8)
    _,tiny=render(directory,'pressure_short_visible_caps')
    require(abs(profile(tiny,33)-2.7783132)<.08 and abs(profile(tiny,222)-5.2614446)<.12,
            'Wider downstream caps must not inflate pressure on a short visible stroke')
    bpy.data.objects.remove(blocker,do_unlink=True)
    points[0].location=(0,.25);points[-1].location=(1,.25);points.new(.5,1)
    for point in points:point.handle_type='AUTO'
    brush.width_curve.update();_,bell=render(directory,'pressure_bell_table_domain')
    require(abs(profile(bell,48)-5.1183705)<.04 and abs(profile(bell,222)-1.6985068)<.04,
            'Raster pressure curves must address 256 cells, including their asymmetric terminal phase')
    brush.use_width_curve=False;brush.use_opacity_curve=True;set_value(brush,'width',3)
    alpha_points=brush.opacity_curve.curves[0].points
    while len(alpha_points)>2:alpha_points.remove(alpha_points[1])
    alpha_points[0].location=(0,0);alpha_points[-1].location=(1,1)
    for point in alpha_points:point.handle_type='VECTOR'
    brush.opacity_curve.update();_,alpha=render(directory,'pressure_opacity_table_domain')
    require(abs(profile(alpha,48)-.34509808)<.005 and abs(profile(alpha,222)-3.9842935)<.005,
            'Opacity curves must share the measured raster table domain')
    return {'raster_pressure_map_distance_composition_terminal_visibility_grid':'passed'}


def test_pixel_wave(directory):
    import math
    reset()
    obj=mesh_object('Pixel Wave',[(-1.5,0,0),(1.5,0,0)],edges=[(0,1)])
    tree,source,brush,settings,output=setup_lines([obj],('wire',),width=3)
    require(tree.nodes.new('NPRLineNodeBrush').wave_units=='PIXELS','New brushes must show pixel wavelengths by default')
    brush.wave_units='PIXELS';brush.raster_size_response=True
    set_value(settings,'direction',0);set_value(brush,'wave',5);set_value(brush,'wave_length',50)
    def samples(a,xs):
        band=a[64:128,xs,3];return (band*np.arange(64,128)[:,None]).sum(axis=0)/np.maximum(band.sum(axis=0),1e-20)
    xs=np.arange(32,160)
    _,base=render(directory,'wave_pixel_fifty')
    expected=95.5-5*np.sin((xs+.5-24)*2*np.pi/50)
    require(float(np.max(abs(samples(base,xs)-expected)))<.12,'Pixel waves must use output arc length, not normalized stroke progress')
    obj.data.vertices[1].co.x=0;obj.data.update();_,short=render(directory,'wave_pixel_short_same_wavelength')
    common=np.arange(32,88)
    require(float(np.max(abs(samples(short,common)-samples(base,common))))<.06,
            'Changing the far endpoint must not stretch a fixed pixel wavelength')
    obj.data.vertices[1].co.x=1.5;obj.data.update()
    set_value(brush,'wave_phase',math.pi/2);_,phase=render(directory,'wave_pixel_phase_quarter')
    require(float(np.max(abs(samples(phase,xs)-(95.5-5*np.cos((xs+.5-24)*2*np.pi/50)))))<.12,
            'Wave phase must follow the angle input')
    set_value(brush,'wave_phase',0);set_value(settings,'direction',180)
    _,reverse=render(directory,'wave_pixel_reverse_phase')
    require(float(np.max(abs(samples(reverse,xs)-(95.5-5*np.sin((168-xs-.5)*2*np.pi/50)))))<.12,
            'Reversing pen direction must reverse phase without flipping the stored offset normal')
    set_value(brush,'wave',0);set_value(brush,'offset',5)
    _,offset_reverse=render(directory,'wave_normal_offset_reverse')
    set_value(settings,'direction',0);_,offset=render(directory,'wave_normal_offset_forward')
    require(abs(float(samples(offset,np.array([96]))[0])-90.5)<.02 and
            float(np.max(abs(offset-offset_reverse)))<.002,'Normal Offset is a constant distance and must keep its side when the pen reverses')
    set_value(brush,'offset_random',4);_,random_offset=render(directory,'wave_normal_offset_random')
    set_value(brush,'seed',19);_,other_offset=render(directory,'wave_normal_offset_random_seed')
    require(float(np.abs(random_offset-other_offset).sum())>5,'Normal offset randomness must have its own seeded pixel distance')
    set_value(brush,'offset_random',0);set_value(brush,'seed',0)
    set_value(brush,'wave',5);set_value(brush,'wave_random',.5);set_value(brush,'wave_period_random',.5);set_value(brush,'wave_phase_random',.5)
    _,random=render(directory,'wave_coherent_random');_,repeat=render(directory,'wave_coherent_random_repeat')
    require(float(np.max(abs(random-repeat)))<.002,'Fixed-seed wave randomness must reproduce')
    set_value(brush,'seed',19);_,other=render(directory,'wave_coherent_random_seed')
    require(float(np.abs(other-random).sum())>5,'Changing the seed must change the configured wave variations')
    mapping=tree.nodes.new('NPRLineNodeMap');mapping.mode='SCREEN'
    image=bpy.data.images.new('Wave Gate Map',1,1,alpha=True,float_buffer=True)
    image.colorspace_settings.name='Non-Color';image.pixels.foreach_set([1,1,1,1]);image.update()
    set_value(mapping,'image',image);tree.links.new(mapping.outputs[0],socket(brush,'distortion_map'))
    set_value(brush,'distortion_map_amount',7)
    set_value(brush,'distortion_enabled',False);_,disabled=render(directory,'wave_map_gate_disabled')
    require(float(np.max(abs(disabled-offset)))<.002,'Disabling wave/map disturbance must preserve the independent normal offset')
    from bl_operators.npr_lines import brush_preview
    set_value(brush,'distortion_enabled',True)
    before=tuple(brush_preview(brush,192,64).image_pixels_float)
    set_value(brush,'wave_phase',1);after=tuple(brush_preview(brush,192,64).image_pixels_float)
    require(before!=after,'The local preview must respond to pixel-wave phase')
    brush.wave_units='STROKE';set_value(brush,'cycles',3)
    filename=Path(directory)/'wave-units-roundtrip.blend';bpy.ops.wm.save_as_mainfile(filepath=str(filename))
    bpy.ops.wm.open_mainfile(filepath=str(filename))
    brush=next(n for n in bpy.context.scene.eevee.geometry_line_tree.nodes if n.bl_idname=='NPRLineNodeBrush' and socket(n,'wave').default_value>0)
    require(brush.wave_units=='STROKE' and abs(socket(brush,'wave_phase').default_value-1)<1e-6,
            'Legacy wave units and hidden pixel parameters must survive save/reopen')

    reset()
    obj=mesh_object('Joined Offset',[(-1,0,0),(0,0,0),(0,1,0)],edges=[(0,1),(1,2)])
    tree,source,brush,settings,output=setup_lines([obj],('wire',),width=3)
    set_value(settings,'split_angle',180);set_value(settings,'direction',0);set_value(brush,'offset',5)
    _,corner=render(directory,'wave_offset_connected_corner')
    require(float(corner[92,92,3])>.9,'Connected offsets must share their corner position, not separate into independent translated edges')
    reset()
    obj=mesh_object('Closed Offset Basis',[(-1.5,-1.7,0),(1.5,-1.7,0),(1.5,-.8,0),(-1.5,-.8,0),
                                         (-1,-1.5,0),(1,-1.5,0),(1,-1,0),(-1,-1,0)],
                    faces=[(0,1,5,4),(1,2,6,5),(2,3,7,6),(3,0,4,7),(4,5,6,7)])
    marks=obj.data.attributes.new('freestyle_edge','BOOLEAN','EDGE')
    for edge in obj.data.edges:marks.data[edge.index].value=min(edge.vertices)>=4
    tree,source,brush,settings,output=setup_lines([obj],('marked',),width=3)
    brush.raster_size_response=True;set_value(settings,'split_angle',180);set_value(settings,'direction',0)
    set_value(brush,'offset',5);_,closed=render(directory,'wave_closed_offset_basis')
    hit=np.argwhere(closed[:,:,3]>.1)
    bounds=[int(hit[:,1].min()),int(hit[:,0].min()),int(hit[:,1].max()),int(hit[:,0].max())]
    require(bounds==[49,145,142,166],'Closed offset must use the measured interpolated unit normal, not an expanded miter or separated edges: '+str(bounds))
    set_value(settings,'clockwise',False);_,closed_reverse=render(directory,'wave_closed_offset_reverse')
    require(float(np.max(abs(closed-closed_reverse)))<.003,'Loop direction must preserve the side and shape of a constant normal offset')
    return {'pixel_wavelength_phase_coherent_random_offset_basis_gate_legacy_preview':'passed'}


def test_color_attributes(directory):
    scene=reset()
    obj=mesh_object('Attribute Internal Edge',[(-1.5,-.5,0),(1.5,-.5,0),(-1.5,0,0),(1.5,0,0),(-1.5,.5,0),(1.5,.5,0)],
                    faces=[(0,1,3,2),(2,3,5,4)])
    marks=obj.data.attributes.new('freestyle_edge','BOOLEAN','EDGE')
    for edge in obj.data.edges:marks.data[edge.index].value=set(edge.vertices)=={2,3}
    tree,source,brush,settings,output=setup_lines([obj],('marked',),width=8,color=(0,1,0,1))
    brush.raster_size_response=True
    mapping=tree.nodes.new('NPRLineNodeMap');mapping.mode='ATTRIBUTE'
    attribute=obj.data.color_attributes.new(name='Weight',type='FLOAT_COLOR',domain='CORNER')
    def fill(attr,rgba):
        for value in attr.data:value.color=rgba
        obj.data.update()
    fill(attribute,(.5,.5,.5,0))
    tree.links.new(mapping.outputs[0],socket(brush,'width_map'))
    profile=lambda a:float(a[48:144,80:112,3].sum()/32)
    _,a=render(directory,'attribute_corner_width')
    require(abs(profile(a)-4)<.03,'A float corner attribute must drive width without using its alpha')
    fill(attribute,(1,0,0,0));tree.links.remove(socket(brush,'width_map').links[0])
    tree.links.new(mapping.outputs[0],socket(brush,'color_map'))
    _,a=render(directory,'attribute_color_ignores_alpha')
    require(float(a[95,96,0])>.99 and float(a[95,96,1])<.001,
            'Attribute color maps must use RGB even when attribute alpha is zero')
    set_value(brush,'color_map_strength',.5);_,a=render(directory,'attribute_color_strength')
    require(abs(float(a[95,96,0])-.5)<.003 and abs(float(a[95,96,1])-.5)<.003,'Attribute color strength must blend with the base color')
    set_value(brush,'color_map_strength',1)
    for poly in obj.data.polygons:
        for loop in poly.loop_indices:attribute.data[loop].color=(1,0,0,0) if poly.index==0 else (0,0,1,0)
    obj.data.update();_,a=render(directory,'attribute_corner_seam')
    require(float(a[95,96,0])>.99 and float(a[95,96,2])<.002,'Corner colors must not average across a face seam')
    other=bpy.data.materials.new('Attribute Owner Material');obj.data.materials.append(other)
    obj.data.polygons[1].material_index=1;obj.data.update();set_value(settings,'material_0',other)
    _,a=render(directory,'attribute_material_owner')
    require(float(a[95,96,2])>.99 and float(a[95,96,0])<.002,'The attribute face must belong to the rule material scope')
    set_value(settings,'material_0',None)
    tree.links.remove(socket(brush,'color_map').links[0])
    brush.mode='NORMAL';brush.raster_size_response=False
    tree.links.new(mapping.outputs[0],socket(brush,'brush_map'));fill(attribute,(.25,.25,.25,0))
    _,a=render(directory,'attribute_brush_coverage')
    require(abs(float(a[:,:,3].max())-.25)<.003,'Color attributes must also modulate brush coverage')
    tree.links.remove(socket(brush,'brush_map').links[0]);brush.mode='SIMPLE'
    fill(attribute,(.5,.5,.5,0));tree.links.new(mapping.outputs[0],socket(brush,'distortion_map'))
    set_value(brush,'distortion_map_amount',10)
    center=lambda a:float(np.dot(a[:,96,3],np.arange(192))/a[:,96,3].sum())
    _,a=render(directory,'attribute_distortion')
    require(abs(center(a)-90.5)<.04,'Attribute RGB must offset the path along its normal')
    mapping.attribute_selection='NAME';set_value(mapping,'attribute_name','Missing')
    _,a=render(directory,'attribute_missing_distortion')
    require(abs(center(a)-95.5)<.04,'A missing attribute must evaluate to black for displacement')
    tree.links.remove(socket(brush,'distortion_map').links[0]);tree.links.new(mapping.outputs[0],socket(brush,'width_map'))
    _,a=render(directory,'attribute_missing_width')
    require(float(a[:,:,3].max())==0,'A missing width attribute must evaluate to black, matching the reference')
    tree.links.remove(socket(brush,'width_map').links[0])
    obj.data.color_attributes.remove(attribute)
    byte=obj.data.color_attributes.new(name='Byte',type='BYTE_COLOR',domain='CORNER')
    for value in byte.data:value.color_srgb=(.5,.5,.5,0)
    obj.data.update();set_value(mapping,'attribute_name','Byte')
    brush.raster_size_response=True;tree.links.new(mapping.outputs[0],socket(brush,'width_map'))
    _,a=render(directory,'attribute_byte_linear_width')
    require(abs(profile(a)-1.158606)<.025,'Byte colors must be converted from sRGB to linear before width evaluation')
    obj.data.color_attributes.remove(byte)
    byte_point=obj.data.color_attributes.new(name='Byte Point',type='BYTE_COLOR',domain='POINT')
    for value in byte_point.data:value.color_srgb=(.5,.5,.5,0)
    obj.data.update();set_value(mapping,'attribute_name','Byte Point');_,a=render(directory,'attribute_byte_point')
    require(abs(profile(a)-1.158606)<.025,'Byte point colors must retain the same linear conversion when adapted to corners')
    obj.data.color_attributes.remove(byte_point)
    first=obj.data.color_attributes.new(name='First',type='FLOAT_COLOR',domain='CORNER');fill(first,(.25,.25,.25,1))
    second=obj.data.color_attributes.new(name='Second',type='FLOAT_COLOR',domain='POINT');fill(second,(1,1,1,1))
    tree.links.remove(socket(brush,'width_map').links[0]);tree.links.new(mapping.outputs[0],socket(brush,'distortion_map'))
    brush.raster_size_response=False;mapping.attribute_selection='INDEX'
    set_value(mapping,'attribute_index',0);_,a=render(directory,'attribute_index_zero')
    require(abs(center(a)-93)<.04,'Color index zero must follow Color Attributes order, across corner and point domains')
    set_value(mapping,'attribute_index',1);_,a=render(directory,'attribute_index_one')
    require(abs(center(a)-85.5)<.04,'Color index one must address the second color attribute')
    tree.links.remove(socket(brush,'distortion_map').links[0]);tree.links.new(mapping.outputs[0],socket(brush,'color_map'))
    mapping.attribute_selection='NAME';set_value(mapping,'attribute_name','Second')
    for i,value in enumerate(second.data):
        u=obj.data.vertices[i].co.x/3+.5;value.color=(u,0,1-u,0)
    obj.data.update();_,a=render(directory,'attribute_point_gradient')
    for x in (48,96,144):
        require(abs(float(a[95,x,0])-(x+.5-24)/144)<.015,'Point colors must interpolate along the source edge')
    scene.camera.data.type='PERSP'
    for v in obj.data.vertices:v.co.z=1-(v.co.x/3+.5)*4
    obj.data.update();_,a=render(directory,'attribute_perspective_gradient')
    from mathutils import Vector
    projection=scene.camera.calc_matrix_camera(bpy.context.evaluated_depsgraph_get(),x=192,y=192)@scene.camera.matrix_world.inverted()
    pa,pb=[projection@Vector((*obj.data.vertices[i].co,1)) for i in (2,3)]
    for x in (48,96,128):
        t=((x+.5)/192*2-1-pa.x/pa.w)/(pb.x/pb.w-pa.x/pa.w)
        world_t=t*pa.w/(pb.w*(1-t)+pa.w*t)
        require(abs(float(a[95,x,0])-world_t)<.02,'Attribute colors must retain perspective-correct interpolation')

    geometry_group=bpy.data.node_groups.new('Animated Evaluated Color','GeometryNodeTree')
    geometry_group.interface.new_socket(name='Geometry',in_out='INPUT',socket_type='NodeSocketGeometry')
    geometry_group.interface.new_socket(name='Geometry',in_out='OUTPUT',socket_type='NodeSocketGeometry')
    gi=geometry_group.nodes.new('NodeGroupInput');go=geometry_group.nodes.new('NodeGroupOutput')
    store=geometry_group.nodes.new('GeometryNodeStoreNamedAttribute');store.data_type='FLOAT_COLOR';store.domain='POINT'
    store.inputs['Name'].default_value='Animated'
    time_node=geometry_group.nodes.new('GeometryNodeInputSceneTime')
    scale=geometry_group.nodes.new('ShaderNodeMath');scale.operation='MULTIPLY';scale.inputs[1].default_value=.25
    combine=geometry_group.nodes.new('FunctionNodeCombineColor');combine.mode='RGB'
    geometry_group.links.new(time_node.outputs['Frame'],scale.inputs[0])
    for channel in ('Red','Green','Blue'):geometry_group.links.new(scale.outputs[0],combine.inputs[channel])
    geometry_group.links.new(combine.outputs[0],store.inputs['Value'])
    geometry_group.links.new(gi.outputs['Geometry'],store.inputs['Geometry']);geometry_group.links.new(store.outputs['Geometry'],go.inputs['Geometry'])
    modifier=obj.modifiers.new('Animated Color Attribute','NODES');modifier.node_group=geometry_group
    tree.links.remove(socket(brush,'color_map').links[0]);tree.links.new(mapping.outputs[0],socket(brush,'width_map'))
    brush.raster_size_response=True;set_value(mapping,'attribute_name','Animated')
    scene.frame_set(1);_,a=render(directory,'attribute_evaluated_frame_one')
    require(abs(profile(a)-1.5)<.04,'Evaluated geometry-node color attributes must be available at the first frame')
    scene.frame_set(3);_,a=render(directory,'attribute_evaluated_frame_three')
    require(abs(profile(a)-6.5)<.04,'Animated evaluated attribute changes must invalidate paint data')

    reset()
    obj=mesh_object('Loose Point Colors',[(-4,0,0),(4,0,0)],edges=[(0,1)])
    attribute=obj.data.color_attributes.new(name='Loose',type='FLOAT_COLOR',domain='POINT')
    attribute.data[0].color=(0,0,1,0);attribute.data[1].color=(1,0,0,0);obj.data.update()
    tree,source,brush,settings,output=setup_lines([obj],('wire',),width=8)
    mapping=tree.nodes.new('NPRLineNodeMap');mapping.mode='ATTRIBUTE';mapping.attribute_selection='NAME';set_value(mapping,'attribute_name','Loose')
    tree.links.new(mapping.outputs[0],socket(brush,'color_map'));_,plain=render(directory,'attribute_loose_clipped')
    for x in (16,96,176):
        require(abs(float(plain[95,x,0])-(x+.5+96)/384)<.02,
                'Clipped loose edges must preserve point-attribute source parameters without triangles')
    group=bpy.data.node_groups.new('Shared Attribute Selection','NPRLineNodeTree')
    group.interface.new_socket(name='Name',in_out='INPUT',socket_type='NodeSocketString')
    group.interface.new_socket(name='Map',in_out='OUTPUT',socket_type='NodeSocketNPRLineMap')
    gi=group.nodes.new('NodeGroupInput');go=group.nodes.new('NodeGroupOutput');inner=group.nodes.new('NPRLineNodeMap')
    inner.mode='ATTRIBUTE';inner.attribute_selection='NAME'
    group.links.new(gi.outputs['Name'],socket(inner,'attribute_name'));group.links.new(inner.outputs[0],go.inputs['Map'])
    instance=tree.nodes.new('NPRLineNodeGroup');instance.node_tree=group;instance.inputs['Name'].default_value='Loose'
    tree.links.new(instance.outputs['Map'],socket(brush,'color_map'));_,grouped=render(directory,'attribute_group_name')
    require(float(np.max(abs(grouped-plain)))<.002,'Group string bindings must select the same attribute')
    filename=Path(directory)/'attribute-map-roundtrip.blend';bpy.ops.wm.save_as_mainfile(filepath=str(filename))
    bpy.ops.wm.open_mainfile(filepath=str(filename));_,reopened=render(directory,'attribute_group_reopened')
    require(float(np.max(abs(reopened-grouped)))<.002,'Attribute source and selector must survive saving and reopening')
    return {'color_attributes_domains_linear_byte_all_maps_indices_perspective_loose_groups':'passed'}


def test_detail_maps(directory):
    scene=reset()
    obj=mesh_object('Mapped Internal Edge',[(-1.5,-.5,0),(1.5,-.5,0),(-1.5,0,0),(1.5,0,0),(-1.5,.5,0),(1.5,.5,0)],
                    faces=[(0,1,3,2),(2,3,5,4)])
    marks=obj.data.attributes.new('freestyle_edge','BOOLEAN','EDGE')
    for edge in obj.data.edges:marks.data[edge.index].value=set(edge.vertices)=={2,3}
    uv=obj.data.uv_layers.new(name='MapUV')
    for loop in obj.data.loops:uv.data[loop.index].uv=(obj.data.vertices[loop.vertex_index].co.x/3+.5,.25)
    tree,source,brush,settings,output=setup_lines([obj],('marked',),width=8)
    brush.raster_size_response=True
    mapping=tree.nodes.new('NPRLineNodeMap');mapping.filter_mode='POINT'
    mapping.wrap_u=mapping.wrap_v='CLAMP'
    images={}
    for name,rgb,alpha in [('white',(1,1,1),1),('black',(0,0,0),1),('red',(1,0,0),1),
                          ('green',(0,1,0),1),('blue',(0,0,1),1),('clear',(1,1,1),0)]:
        image=bpy.data.images.new('Detail Map '+name,2,2,alpha=True,float_buffer=True)
        image.colorspace_settings.name='Non-Color';image.pixels.foreach_set([*rgb,alpha]*4);image.update();images[name]=image
    profile=lambda a:float(a[48:144,80:112,3].sum()/32)
    tree.links.new(mapping.outputs[0],socket(brush,'width_map'))
    # Frozen measurements from the unchanged signed renderer: primary RGB channels are equal.
    for color in ('red','green','blue'):
        set_value(mapping,'image',images[color]);_,a=render(directory,'detail_width_'+color)
        require(abs(profile(a)-2.333334)<.03,'Size maps must scale the raster radius and average RGB equally: '+color+' '+str(profile(a)))
    set_value(brush,'width',3);set_value(mapping,'image',images['red'])
    _,a=render(directory,'detail_width_subpixel_radius')
    require(abs(profile(a)-.6944447)<.02,'A mapped raster radius below one must retain the measured squared-opacity response')
    tree.links.remove(socket(brush,'width_map').links[0])
    brush.mode='NORMAL';brush.stroke_mode='NORMAL';brush.raster_size_response=False
    set_value(brush,'width',8)
    tree.links.new(mapping.outputs[0],socket(brush,'brush_map'))
    for color,expected in [('white',1),('red',1/3),('clear',1)]:
        set_value(mapping,'image',images[color]);_,a=render(directory,'detail_brush_'+color)
        require(abs(float(a[:,:,3].max())-expected)<.003,'Brush maps must use RGB without image alpha: '+color)
    set_value(mapping,'image',images['black']);set_value(brush,'brush_map_opacity',.5)
    _,a=render(directory,'detail_brush_half_strength')
    require(abs(float(a[:,:,3].max())-.5)<.003,'Brush-map strength must blend with the original tip')
    mapping.mute=True;_,a=render(directory,'detail_brush_muted')
    require(float(a[:,:,3].max())>.99,'A muted brush map must restore the untextured tip')
    mapping.mute=False;set_value(brush,'brush_map_opacity',1)
    vertical=bpy.data.images.new('Vertical Brush Map',2,2,alpha=True,float_buffer=True)
    vertical.colorspace_settings.name='Non-Color';vertical.pixels.foreach_set([0,0,0,1]*2+[1,1,1,1]*2);vertical.update()
    set_value(mapping,'image',vertical);_,a=render(directory,'detail_brush_tip_vertical')
    require(float(a[90:95,80:112,3].sum())>40 and float(a[97:103,80:112,3].max())<.002,
            'Brush maps must use the actual scene tip orientation, independently of geometry UV selection')
    set_value(mapping,'rotation',180);_,a=render(directory,'detail_brush_tip_rotated')
    require(float(a[97:103,80:112,3].sum())>40 and float(a[90:95,80:112,3].max())<.002,
            'Shared map transforms must rotate the tip mask')
    set_value(mapping,'rotation',0)
    from bl_operators.npr_lines import brush_preview
    set_value(mapping,'image',images['red']);before=tuple(brush_preview(brush,192,64).image_pixels_float)
    set_value(mapping,'image',images['white']);after=tuple(brush_preview(brush,192,64).image_pixels_float)
    require(before!=after,'The node-local preview must respond to its linked brush map')
    tree.links.remove(socket(brush,'brush_map').links[0]);brush.mode='SIMPLE'
    set_value(brush,'width',4);set_value(brush,'distortion_map_amount',10)
    _,base=render(directory,'detail_distortion_base')
    tree.links.new(mapping.outputs[0],socket(brush,'distortion_map'))
    def center(a,x):
        weights=a[:,x,3];return float(np.dot(weights,np.arange(len(weights)))/weights.sum())
    base_y=center(base,96)
    for color,offset in [('white',10),('red',10/3),('clear',10),('black',0)]:
        set_value(mapping,'image',images[color]);_,a=render(directory,'detail_distortion_'+color)
        require(abs(center(a,96)-(base_y-offset))<.04,'A map must displace along the path normal by mean RGB: '+color)
    pattern=bpy.data.images.new('Detail Gradient',2,1,alpha=True,float_buffer=True)
    pattern.colorspace_settings.name='Non-Color';pattern.pixels.foreach_set([0,0,0,1,1,1,1,1]);pattern.update()
    set_value(mapping,'image',pattern);_,a=render(directory,'detail_distortion_surface_uv')
    require(abs(center(a,64)-base_y)<.03 and abs(center(a,128)-(base_y-10))<.03,
            'Distortion must follow the selected surface UV along the source path')
    mapping.uv_selection='NAME';set_value(mapping,'uv_name','Missing Map UV')
    _,a=render(directory,'detail_distortion_missing_uv')
    require(float(np.max(abs(a-base)))<.002,'Missing source UV must keep the original path')
    mapping.mode='SCREEN';set_value(mapping,'image',images['white'])
    set_value(brush,'distortion_map_amount',160);obj.location.y=-5
    _,a=render(directory,'detail_distortion_offscreen_margin')
    require(float(a[:,:,3].max())>.99 and abs(center(a,96)-175.5)<.05,
            'A large map displacement must retain a source edge outside the default clip margin')
    obj.location.y=0;set_value(brush,'distortion_map_amount',10)
    group=bpy.data.node_groups.new('Reusable Brush Map','NPRLineNodeTree')
    image_socket=group.interface.new_socket(name='Image',in_out='INPUT',socket_type='NodeSocketImage')
    group.interface.new_socket(name='Map',in_out='OUTPUT',socket_type='NodeSocketNPRLineMap')
    group_in=group.nodes.new('NodeGroupInput');group_out=group.nodes.new('NodeGroupOutput')
    inner=group.nodes.new('NPRLineNodeMap');inner.filter_mode='POINT'
    group.links.new(group_in.outputs['Image'],socket(inner,'image'));group.links.new(inner.outputs[0],group_out.inputs['Map'])
    instance=tree.nodes.new('NPRLineNodeGroup');instance.node_tree=group
    instance.inputs[image_socket.identifier].default_value=images['red']
    tree.links.remove(socket(brush,'distortion_map').links[0]);brush.mode='NORMAL'
    tree.links.new(instance.outputs['Map'],socket(brush,'brush_map'))
    _,grouped=render(directory,'detail_brush_group_image_binding')
    require(abs(float(grouped[:,:,3].max())-1/3)<.003,'A grouped map must resolve its outer image input')
    preview_red=tuple(brush_preview(brush,192,64).image_pixels_float)
    instance.inputs[image_socket.identifier].default_value=images['white']
    preview_white=tuple(brush_preview(brush,192,64).image_pixels_float)
    require(preview_red!=preview_white,'The node preview must resolve image bindings through map groups')
    instance.inputs[image_socket.identifier].default_value=images['red']
    for image in images.values():image.pack()
    pattern.pack();vertical.pack()
    filename=Path(directory)/'detail-maps-roundtrip.blend';bpy.ops.wm.save_as_mainfile(filepath=str(filename))
    bpy.ops.wm.open_mainfile(filepath=str(filename));_,reopened=render(directory,'detail_maps_group_reopened')
    require(float(np.max(abs(reopened-grouped)))<.002,'Detail maps and group image references must survive save and reopen')
    return {'brush_maps_distortion_maps_rgb_radius_groups_preview_and_roundtrip':'passed'}


def test_intersection_secondary_owner(directory):
    scene=reset()
    scene.render.resolution_x=scene.render.resolution_y=256
    scene.camera.location=(0,-5,5)
    scene.camera.rotation_euler=(-scene.camera.location).to_track_quat('-Z','Y').to_euler()
    a=mesh_object('A Cutting Surface',[(-.8,-.4,0),(0,-.4,0),(0,.4,0),(-.8,.4,0)],faces=[(0,1,2,3)])
    b=mesh_object('B Intersection and Mark',[(-.6,0,-.3),(0,0,-.3),(0,0,.3),(-.6,0,.3),
        (.02,-.25,-.15),(.6,-.25,-.15),(.02,-.05,.05),(.6,-.05,.05),(.02,.15,.25),(.6,.15,.25)],
        faces=[(0,1,2,3),(4,5,7,6),(6,7,9,8)])
    marks=b.data.attributes.new('freestyle_edge','BOOLEAN','EDGE')
    for edge in b.data.edges:marks.data[edge.index].value=set(edge.vertices)=={6,7}
    tree,source,brush,settings,output=setup_lines([a,b],('intersection','marked'),width=8)
    brush.raster_size_response=True;brush.use_width_curve=True
    points=brush.width_curve.curves[0].points
    while len(points)>2:points.remove(points[1])
    points[0].location=(0,0);points[-1].location=(1,0);points.new(.5,1)
    for point in points:point.handle_type='AUTO'
    brush.width_curve.update()
    for key,value in {'connect_objects':False,'connect_coincident':True,'screen_space_join':True,
                      'gap_join':2,'split_angle':180,'direction':0,'object_contours':True,
                      'contour_priority':True}.items():set_value(settings,key,value)
    set_value(output,'samples',1)
    _,line=render(directory,'intersection_secondary_owner')
    # Measured normal 4.1.0 API output: both belong to one tapered stroke even with
    # cross-object welding off, since the intersection also belongs to object B.
    require(float(line[124,130,3])>.95 and float(line[131,125,3])>.95,
            'An intersection must join the marked stroke on either participating surface')
    require(float(line[129,164,3])<.05,
            'The joined stroke must use a common taper rather than separate peak widths')
    return {'intersection_far_join_matches_either_surface_owner':'passed'}


def test_closed_tip_anchor(directory):
    scene=reset()
    scene.render.resolution_x=scene.render.resolution_y=256
    obj=mesh_object('Closed Tip Anchor',[(-1.2,-.65,0),(1.2,-.65,0),(1.2,.65,0),(-1.2,.65,0)],
                    edges=[(0,1),(1,2),(2,3),(3,0)])
    tree,source,brush,settings,output=setup_lines([obj],('wire',),width=16)
    brush.mode='MULTIPLE';brush.stroke_mode='NORMAL';brush.raster_size_response=True
    set_value(brush,'spacing',0);set_value(settings,'split_angle',180);set_value(output,'samples',1)
    orientations=[]
    for clockwise in (True,False):
        set_value(settings,'clockwise',clockwise)
        frames=[]
        for direction in (0,-30,45):
            set_value(settings,'direction',direction)
            _,line=render(directory,f'closed_tip_anchor_{int(clockwise)}_{direction}')
            frames.append(line)
        require(all(np.max(np.abs(frames[0]-line))<.002 for line in frames[1:]),
                'A closed tip must keep the same anchor edge while its midpoint remains the projected minimum')
        orientations.append(frames[0])
    require(np.count_nonzero(np.abs(orientations[0]-orientations[1])>.05)>100,
            'Reversing a closed stroke must seed from the other endpoint of the same anchor edge')
    return {'closed_tip_midpoint_anchor_and_reverse_endpoint':'passed'}


def test_textured_sample_maximum(directory):
    scene=reset()
    obj=mesh_object('Subpixel Maximum',[(-.5,0,0),(.5,0,0)],edges=[(0,1)])
    tree,source,brush,settings,output=setup_lines([obj],('wire',),width=3)
    brush.mode='NORMAL'
    brush.stroke_mode='NORMAL'
    brush.raster_size_response=True
    for key,value in {'spacing':0,'groove':.7,'groove_count':5,
                      'tip_reduction_start':.1,'tip_reduction_end':.8,'opacity':.7}.items():
        set_value(brush,key,value)
    world_per_pixel=scene.camera.data.ortho_scale/scene.render.resolution_x
    for samples in (2,3,4):
        set_value(output,'samples',1)
        references=[]
        for y in range(samples):
            for x in range(samples):
                dx=(x+.5)/samples-.5
                dy=(y+.5)/samples-.5
                obj.location=(-dx*world_per_pixel,-dy*world_per_pixel,0)
                _,line=render(directory,f'sample_max_reference_{samples}_{x}_{y}')
                references.append(line)
        expected=np.mean(references,axis=0)
        obj.location=(0,0,0)
        set_value(output,'samples',samples)
        _,actual=render(directory,f'sample_max_resolved_{samples}')
        require(float(expected[:,:,3].max())>.1,'The subpixel reference must contain visible ink')
        error=float(np.max(np.abs(expected-actual)))
        require(error<.0025,
                f'{samples}x sampling must average the maximum of all stamps per subpixel, error {error}')
    return {'textured_subpixel_maximum_before_average':'passed'}


def test_tip_reference_previews(directory):
    from bl_operators.npr_lines import _tip_mask
    # Measured alpha bytes from normal public Pencil 4.1.0 previews.
    # The 256px preview has a 3px margin around the canonical 250px tip.
    cases = [
        ({'groove': 0, 'groove_count': 5, 'tip_reduction_start': 0, 'tip_reduction_end': 1}, False, [(115, 4, 8), (128, 62, 207), (181, 101, 210), (73, 148, 219), (124, 187, 207), (134, 245, 8), (125, 125, 255), (0, 0, 0), (125, 50, 176), (225, 125, 79)]),
        ({'groove': 0, 'groove_count': 5, 'tip_reduction_start': 0.8, 'tip_reduction_end': 0.2}, False, [(116, 28, 247), (153, 72, 66), (182, 104, 72), (66, 145, 75), (95, 177, 68), (133, 221, 247), (125, 125, 0), (0, 0, 0), (125, 50, 136), (225, 125, 255)]),
        ({'groove': 0.7, 'groove_count': 5, 'tip_reduction_start': 1, 'tip_reduction_end': 1}, False, [(73, 11, 255), (142, 67, 255), (133, 110, 255), (59, 145, 255), (121, 178, 255), (117, 248, 255), (125, 125, 255), (0, 0, 0), (125, 50, 255), (225, 125, 0)]),
        ({'groove': 0.7, 'groove_count': 9, 'tip_reduction_start': 1, 'tip_reduction_end': 1}, False, [(99, 3, 255), (107, 68, 255), (59, 106, 255), (52, 140, 255), (112, 175, 255), (111, 248, 255), (125, 125, 255), (0, 0, 0), (125, 50, 255), (225, 125, 0)]),
        ({'groove': 0.5, 'groove_count': 7, 'tip_reduction_start': 0.1, 'tip_reduction_end': 0.9}, False, [(96, 21, 8), (134, 73, 165), (20, 104, 13), (95, 143, 244), (49, 176, 76), (115, 232, 8), (125, 125, 255), (0, 0, 0), (125, 50, 65), (225, 125, 36)]),
        ({'dot_size': 5, 'dot_space_random_x': 1, 'dot_space_random_y': 0, 'dot_size_random': 0, 'dot_antialiasing': 0.5, 'dot_space_x': 0.1, 'dot_space_y': 0.1}, True, [(77, 9, 11), (146, 65, 36), (124, 110, 34), (126, 135, 140), (162, 183, 166), (172, 240, 14), (125, 125, 255), (0, 0, 0), (125, 50, 0), (225, 125, 255)]),
        ({'dot_size': 5, 'dot_space_random_x': 0, 'dot_space_random_y': 1, 'dot_size_random': 0, 'dot_antialiasing': 0.5, 'dot_space_x': 0.1, 'dot_space_y': 0.1}, True, [(114, 0, 127), (177, 60, 184), (227, 90, 55), (76, 126, 41), (81, 178, 9), (161, 244, 56), (125, 125, 174), (0, 0, 0), (125, 50, 55), (225, 125, 0)]),
        ({'dot_size': 5, 'dot_space_random_x': 1, 'dot_space_random_y': 1, 'dot_size_random': 0.8, 'dot_antialiasing': 0.5, 'dot_space_x': 0.1, 'dot_space_y': 0.1}, True, [(114, 0, 21), (222, 61, 37), (99, 87, 59), (129, 122, 33), (57, 179, 14), (164, 243, 34), (125, 125, 0), (0, 0, 0), (125, 50, 0), (225, 125, 0)]),
        ({'dot_size': 16, 'dot_space_random_x': 1, 'dot_space_random_y': 1, 'dot_size_random': 0.8, 'dot_antialiasing': 0.5, 'dot_space_x': 0.1, 'dot_space_y': 0.1}, True, [(100, 2, 215), (112, 76, 107), (9, 100, 51), (190, 153, 30), (45, 191, 12), (135, 249, 230), (125, 125, 82), (0, 0, 0), (125, 50, 255), (225, 125, 255)]),
        ({'dot_size': 16, 'dot_space_random_x': 0.3, 'dot_space_random_y': 0.8, 'dot_size_random': 0.4, 'dot_antialiasing': 0.5, 'dot_space_x': 0.1, 'dot_space_y': 0.1}, True, [(100, 2, 229), (86, 64, 29), (35, 95, 90), (124, 139, 79), (203, 192, 15), (143, 248, 224), (125, 125, 251), (0, 0, 0), (125, 50, 255), (225, 125, 255)]),
    ]
    for values, multiple, pixels in cases:
        size, mask = _tip_mask(values, multiple, 250)
        alpha = np.asarray(mask).reshape(size, size)
        require(all(abs(float(alpha[y, x])*255 - expected) < 1.01 for x, y, expected in pixels),
                f'Tip preview must match measured point, groove and radial kernels: {values}')
    return {'brush_tip_measured_preview_pixels': 'passed'}

def test_multiple_dot_kernel(directory):
    reset()
    obj=mesh_object('Isolated Multiple Stamp',[(-.002,0,0),(.002,0,0)],edges=[(0,1)])
    tree,source,brush,settings,output=setup_lines([obj],('wire',),width=128)
    brush.mode='MULTIPLE'
    brush.stroke_mode='NORMAL'
    brush.raster_size_response=False
    values={'spacing':1,'dot_size':16,'dot_size_random':0,'dot_space_x':1,'dot_space_y':1,
            'dot_space_random_x':0,'dot_space_random_y':0,'dot_antialiasing':2}
    for key,value in values.items():set_value(brush,key,value)
    _,soft=render(directory,'multiple_dot_soft_two')
    # A scene selects a translated mask variant. The stroke center can fall between
    # dots; the measured soft-dot peak is ~.4627 wherever that dot lands.
    peak=float(soft[:,:,3].max())
    require(.44<peak<.49, f'The two-radius dot must retain its measured peak density, got {peak}')
    set_value(brush,'dot_antialiasing',5)
    _,wide=render(directory,'multiple_dot_soft_five')
    require(.13<float(wide[:,:,3].max())<.16,'Large dot antialiasing must retain the recovered low peak density')
    set_value(brush,'dot_antialiasing',0)
    _,hard=render(directory,'multiple_dot_hard')
    require(float(hard[:,:,3].max())>.99,'A hard dot must preserve its opaque core')
    for key,value in {'dot_antialiasing':2,'dot_space_x':0,'dot_space_y':0}.items():set_value(brush,key,value)
    _,overlap=render(directory,'multiple_dot_additive_overlap')
    require(float(overlap[:,:,3].max())>peak+.2,'Overlapping dots must add their coverage before saturation')
    require(float(overlap[:,:,3].max())<=1.0001,'Dot coverage must saturate at one')
    from bl_operators.npr_lines import _tip_mask
    # Normal public Pencil previews: transparent background, no random size or spacing.
    # Preview has a 3px margin; a 250px local tip shares the 256px reference sampling grid.
    values.update(dot_antialiasing=2)
    resolution,pixels=_tip_mask(values,True,250)
    alpha=np.asarray(pixels).reshape(resolution,resolution)
    require(abs(float(alpha[125,125])-.46274513)<1/255,
            'The node preview must use the same measured dot kernel as the scene renderer')
    return {'multiple_dot_recovered_kernel_overlap_and_preview':'passed'}


def test_brush_connections_and_linked_limits(directory):
    reset()
    obj=mesh_object('Independent Brush Connections',[(-1,-1,0),(0,-1,0),(0,1,0)],edges=[(0,1),(1,2)])
    tree,source,brush,settings,output=setup_lines([obj],('wire',),width=6)
    tree.show_performance=True
    set_value(settings,'split_angle',30)
    set_value(settings,'gap_join',0)
    _,base=render(directory,'brush_connection_rule_default')
    require('2 strokes' in tree.performance_info,'Disabled brush overrides must use the rule split angle')
    brush.override_stroke_connection=True
    set_value(brush,'stroke_split_angle',90)
    set_value(brush,'stroke_gap_join',0)
    _,joined=render(directory,'brush_connection_override')
    require('1 strokes' in tree.performance_info,'The brush split angle must override the rule for its own segments')
    other=make_brush(tree,color=(0,0,1,1),width=2)
    other_rule=make_settings(tree,source,other,('wire',))
    set_value(other_rule,'split_angle',30)
    set_value(other_rule,'gap_join',0)
    combine=tree.nodes.new('NPRLineNodeCombine');combine.mode='OVERLAY'
    tree.links.new(settings.outputs[0],socket(combine,'plans'))
    tree.links.new(other_rule.outputs[0],socket(combine,'plans'))
    tree.links.new(combine.outputs[0],socket(output,'plan'))
    render(directory,'brush_connection_independent_styles')
    require('3 strokes' in tree.performance_info,'A second brush must retain its own rule connection defaults')
    set_value(brush,'stroke_minimum_length',1000)
    render(directory,'brush_connection_minimum')
    require('2 strokes' in tree.performance_info,'Per-brush minimum length must not suppress another brush')
    brush.override_stroke_connection=False
    render(directory,'brush_connection_disabled')
    require('4 strokes' in tree.performance_info,'Disabling the brush override must restore rule defaults')
    brush.override_stroke_connection=True
    set_value(brush,'stroke_minimum_length',0)
    tree.links.new(settings.outputs[0],socket(output,'plan'))
    set_value(brush,'taper_start',.4)
    set_value(brush,'stroke_direction',0)
    _,forward=render(directory,'brush_connection_direction_forward')
    set_value(brush,'stroke_direction',180)
    _,backward=render(directory,'brush_connection_direction_reverse')
    require(float(np.abs(forward-backward).sum())>30,'A per-brush direction must reverse its start envelope')

    reset()
    obj=mesh_object('Per-Type Brush Connections',[(-1.5,-1,0),(-.5,-1,0),(-.5,1,0),
                                                (.5,-1,0),(1.5,-1,0),(1.5,1,0)],
                    edges=[(0,1),(1,2),(3,4),(4,5)])
    marks=obj.data.attributes.new('freestyle_edge','BOOLEAN','EDGE')
    for edge in obj.data.edges:marks.data[edge.index].value=all(v<3 for v in edge.vertices)
    tree,source,brush,settings,output=setup_lines([obj],('wire','marked'),width=4)
    tree.show_performance=True
    set_value(settings,'split_angle',30);set_value(settings,'gap_join',0)
    specific=make_brush(tree,color=(1,0,0,1),width=4)
    specific.override_stroke_connection=True
    set_value(specific,'stroke_split_angle',90);set_value(specific,'stroke_gap_join',0)
    settings.visible_overrides={'MARKED'}
    tree.links.new(specific.outputs[0],socket(settings,'visible_override_marked'))
    render(directory,'brush_connection_specific_type')
    require('3 strokes' in tree.performance_info,'A single rule must partition different per-type brush connections')
    set_value(specific,'stroke_minimum_length',1000)
    render(directory,'brush_connection_specific_minimum')
    require('2 strokes' in tree.performance_info,'A per-type brush minimum must leave the default brush intact')
    set_value(specific,'stroke_minimum_length',0)
    settings.hidden_overrides={'MARKED'}
    tree.links.new(specific.outputs[0],socket(settings,'hidden_override_marked'))
    set_value(settings,'hidden_enabled',True)
    mesh_object('Hidden Stroke Occluder',[(-2,-2,.2),(2,-2,.2),(2,2,.2),(-2,2,.2)],faces=[(0,1,2,3)])
    render(directory,'brush_connection_hidden_specific')
    require('3 strokes' in tree.performance_info,'Hidden per-type brushes must use their own connection override')

    reset()
    loop=mesh_object('Loop Connection',[(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)],edges=[(0,1),(1,2),(2,3),(3,0)])
    tree,source,brush,settings,output=setup_lines([loop],('wire',),width=3)
    brush.override_stroke_connection=True
    for key,value in {'stroke_split_angle':180,'dashed':True,'dash_length':11,'dash_space':8}.items():set_value(brush,key,value)
    set_value(brush,'stroke_clockwise',True)
    _,clockwise=render(directory,'brush_connection_loop_clockwise')
    set_value(brush,'stroke_clockwise',False)
    _,counter=render(directory,'brush_connection_loop_counterclockwise')
    require(float(np.abs(clockwise-counter).sum())>30,'The brush must control closed-loop direction independently')

    reset()
    obj=mesh_object('Linked Dot Limits',[(-1,0,0),(1,0,0)],edges=[(0,1)])
    tree,source,unused,settings,output=setup_lines([obj],('wire',))
    group=bpy.data.node_groups.new('Linked Multiple Limits','NPRLineNodeTree')
    field=group.interface.new_socket(name='Space',in_out='INPUT',socket_type='NodeSocketFloat')
    field.default_value=-.1
    group.interface.new_socket(name='Brush',in_out='OUTPUT',socket_type='NodeSocketNPRLineBrush')
    inputs=group.nodes.new('NodeGroupInput');outputs=group.nodes.new('NodeGroupOutput')
    inner=make_brush(group,width=48);inner.mode='MULTIPLE';inner.stroke_mode='NORMAL'
    for key,value in {'dot_size':5,'dot_size_random':0,'dot_space_random_x':0,'dot_space_random_y':0}.items():set_value(inner,key,value)
    group.links.new(inputs.outputs['Space'],socket(inner,'dot_space_x'))
    group.links.new(inner.outputs[0],outputs.inputs['Brush'])
    instance=tree.nodes.new('NPRLineNodeGroup');instance.node_tree=group
    tree.links.new(instance.outputs['Brush'],socket(settings,'visible_brush'))
    _,negative=render(directory,'brush_group_negative_spacing')
    instance.inputs['Space'].default_value=0
    _,zero=render(directory,'brush_group_zero_spacing')
    require(float(np.max(np.abs(negative-zero)))<.002,'Linked negative spacing must clamp safely before building a tip lattice')
    from bl_operators.npr_lines import _tip_mask
    require(_tip_mask({'dot_size':5,'dot_space_x':-.1},True)==_tip_mask({'dot_size':5,'dot_space_x':0},True),
            'Preview and render must apply the same linked-value limits')
    return {'per_brush_connections_direction_and_group_limits':'passed'}


def test_distance_reduction(directory):
    scene=reset()
    obj=mesh_object('Distance Line',[(-1,0,0),(1,0,0)],edges=[(0,1)])
    tree,source,brush,settings,output=setup_lines([obj],('wire',),width=8)
    _,plain=render(directory,'distance_plain')
    reduction=tree.nodes.new('NPRLineNodeReduction')
    require(len(reduction.curve.curves[0].points)==3,'A distance node must have an editable default falloff curve')

    def curve(node,start,end):
        points=node.curve.curves[0].points
        while len(points)>2:points.remove(points[1])
        points[0].location=(0,start);points[-1].location=(1,end)
        for point in points:point.handle_type='VECTOR'
        node.curve.update()

    curve(reduction,1,0)
    set_value(reduction,'start',0);set_value(reduction,'end',10)
    tree.links.new(reduction.outputs[0],socket(settings,'width_reduction'))
    _,half_width=render(directory,'distance_camera_width')
    profile=lambda image:float(image[:,96,3].sum())
    require(abs(profile(half_width)/profile(plain)-.5)<.03,'World-space camera distance must scale stroke width')
    reduction.mute=True
    _,muted=render(directory,'distance_muted_descriptor')
    require(float(np.max(np.abs(muted-plain)))<.002,'Muting a distance descriptor must return the neutral factor')
    reduction.mute=False
    tree.links.new(reduction.outputs[0],socket(settings,'opacity_reduction'))
    _,both=render(directory,'distance_shared_width_opacity')
    require(abs(float(both[:,:,3].max())-.5)<.02,'One shared distance node must drive width and opacity independently')
    tree.links.remove(socket(settings,'width_reduction').links[0])
    curve(reduction,.25,.25)
    _,quarter_alpha=render(directory,'distance_quarter_opacity')
    require(float(np.max(np.abs(quarter_alpha-plain*.25)))<.002,'Opacity distance changes must leave stroke geometry intact')
    curve(reduction,.75,.75)
    _,three_quarter=render(directory,'distance_opacity_curve_edit')
    require(float(np.max(np.abs(three_quarter-plain*.75)))<.002,'Editing a shared distance curve must update the next draw')
    tree.links.remove(socket(settings,'opacity_reduction').links[0])
    tree.links.new(reduction.outputs[0],socket(settings,'width_reduction'))
    curve(reduction,0,0)
    for mode in ('SIMPLE','NORMAL','MULTIPLE'):
        brush.mode=mode
        _,empty=render(directory,'distance_zero_'+mode.lower())
        require(float(empty[:,:,3].max())==0,'A zero distance width must not leave pen dots in '+mode)
    brush.mode='SIMPLE'
    curve(reduction,1,0)
    set_value(reduction,'start',6);set_value(reduction,'end',6)
    _,step_near=render(directory,'distance_equal_endpoints_near')
    require(abs(profile(step_near)-profile(plain))<.03,'An equal start/end range must have a defined near-side factor')
    set_value(reduction,'start',4);set_value(reduction,'end',4)
    _,step_far=render(directory,'distance_equal_endpoints_far')
    require(float(step_far[:,:,3].max())==0,'An equal start/end range must have a defined far-side factor')

    set_value(reduction,'start',0);set_value(reduction,'end',10)
    reduction.mode='OBJECT'
    _,missing=render(directory,'distance_missing_reference')
    require(float(np.max(np.abs(missing-plain)))<.002,'A missing object reference must leave the style unattenuated')
    parent=bpy.data.objects.new('Distance Anchor Parent',None)
    anchor=bpy.data.objects.new('Distance Anchor',None)
    scene.collection.objects.link(parent);scene.collection.objects.link(anchor)
    anchor.parent=parent;anchor.location.z=5
    set_value(reduction,'object',anchor)
    _,object_distance=render(directory,'distance_object_origin')
    require(float(np.max(np.abs(object_distance-half_width)))<.002,'Reference object and camera at the same position must agree')
    scene.camera.location.z=6
    _,moved_camera=render(directory,'distance_object_independent_camera')
    require(float(np.max(np.abs(moved_camera-object_distance)))<.002,'Object reduction must be independent of orthographic camera distance')
    target=bpy.data.objects.new('Constrained Distance Target',None);scene.collection.objects.link(target)
    target.location.z=8
    constraint=anchor.constraints.new('COPY_LOCATION');constraint.target=target
    _,constrained=render(directory,'distance_constrained_reference')
    require(profile(constrained)<profile(object_distance)*.6,'Reference-object constraints must be evaluated before distance reduction')
    anchor.constraints.remove(constraint)
    parent.location.z=0;parent.keyframe_insert(data_path='location',index=2,frame=1)
    parent.location.z=3;parent.keyframe_insert(data_path='location',index=2,frame=2)
    scene.frame_set(1)
    _,frame_one=render(directory,'distance_reference_frame_one')
    scene.frame_set(2)
    _,frame_two=render(directory,'distance_reference_frame_two')
    require(profile(frame_two)<profile(frame_one)*.6,'An animated parent must update the evaluated reference origin')

    # Descriptor outputs and scalar values must both survive a nested group boundary.
    group=bpy.data.node_groups.new('Shared Distance Group','NPRLineNodeTree')
    group.interface.new_socket(name='Reduction',in_out='OUTPUT',socket_type='NodeSocketNPRLineReduction')
    group_out=group.nodes.new('NodeGroupOutput')
    inner=group.nodes.new('NPRLineNodeReduction');curve(inner,.5,.5)
    group.links.new(inner.outputs[0],group_out.inputs['Reduction'])
    instance=tree.nodes.new('NPRLineNodeGroup');instance.node_tree=group
    tree.links.new(instance.outputs['Reduction'],socket(settings,'width_reduction'))
    _,grouped=render(directory,'distance_grouped_descriptor')
    require(abs(profile(grouped)/profile(plain)-.5)<.03,'A grouped reduction descriptor must retain its curve')
    copy=group.copy();copied=next(n for n in copy.nodes if n.bl_idname=='NPRLineNodeReduction')
    copied.curve.curves[0].points[0].location[1]=.2;copied.curve.update()
    require(abs(inner.curve.curves[0].points[0].location[1]-.5)<1e-6,'Reduction curve copies must be independent')
    filename=Path(directory)/'distance-reduction-roundtrip.blend'
    np.save(Path(directory)/'distance-expected.npy',grouped)
    bpy.ops.wm.save_as_mainfile(filepath=str(filename))
    bpy.ops.wm.open_mainfile(filepath=str(filename))
    _,reopened=render(directory,'distance_grouped_reopened')
    require(float(np.max(np.abs(grouped-reopened)))<.002,'Grouped reduction and reference data must survive a saved file')

    scene=reset()
    obj=mesh_object('Perspective Distance',[(-.5,0,1),(1.5,0,-3)],edges=[(0,1)])
    tree,source,brush,settings,output=setup_lines([obj],('wire',),width=10)
    scene.camera.data.type='PERSP'
    reduction=tree.nodes.new('NPRLineNodeReduction');curve(reduction,1,0)
    set_value(reduction,'start',0);set_value(reduction,'end',10)
    tree.links.new(reduction.outputs[0],socket(settings,'width_reduction'))
    _,perspective=render(directory,'distance_perspective_correct')
    from mathutils import Vector
    projection=scene.camera.calc_matrix_camera(bpy.context.evaluated_depsgraph_get(),x=192,y=192)@scene.camera.matrix_world.inverted()
    a,b=[projection@Vector((*v.co,1)) for v in obj.data.vertices]
    a,b=a.xyz/a.w,b.xyz/b.w
    inverse=projection.inverted()
    for x in (85,100,115):
        t=((x+.5)/192*2-1-a.x)/(b.x-a.x)
        require(0<t<1,'Perspective distance test must sample the projected segment interior')
        ndc=a.lerp(b,t)
        world=inverse@Vector((*ndc,1));world=world.xyz/world.w
        expected=10*max(0,1-(world-scene.camera.location).length/10)
        measured=float(perspective[:,x,3].sum())
        require(abs(expected-measured)<.15,f'Perspective reduction must use world distance at x={x}: expected {expected}, got {measured}')

    scene=reset()
    obj=mesh_object('Independent Hidden Reduction',[(-1,0,0),(1,0,0)],edges=[(0,1)])
    tree,source,brush,settings,output=setup_lines([obj],('wire',),width=8)
    set_value(settings,'hidden_enabled',True)
    mesh_object('Half Occluder',[(-2,-1,.3),(0,-1,.3),(0,1,.3),(-2,1,.3)],faces=[(0,1,2,3)])
    visible=tree.nodes.new('NPRLineNodeReduction');curve(visible,.5,.5)
    hidden=tree.nodes.new('NPRLineNodeReduction');curve(hidden,.25,.25)
    tree.links.new(visible.outputs[0],socket(settings,'width_reduction'))
    _,unlinked_hidden=render(directory,'distance_visible_only')
    require(abs(float(unlinked_hidden[:,60,3].sum())-8)<.1 and abs(float(unlinked_hidden[:,130,3].sum())-4)<.1,
            'Unconnected hidden reduction must remain independent of visible reduction')
    tree.links.new(hidden.outputs[0],socket(settings,'hidden_width_reduction'))
    tree.links.new(visible.outputs[0],socket(settings,'opacity_reduction'))
    tree.links.new(hidden.outputs[0],socket(settings,'hidden_opacity_reduction'))
    _,independent=render(directory,'distance_independent_visible_hidden')
    require(abs(float(independent[:,60,3].max())-.25)<.01 and abs(float(independent[:,130,3].max())-.5)<.01,
            'Visible and hidden opacity must use separate reduction descriptors')
    require(abs(float(independent[:,60,3].sum())-.5)<.1 and abs(float(independent[:,130,3].sum())-2)<.1,
            'Visible and hidden widths must use separate reduction descriptors')
    return {'distance_reduction_camera_object_animation_groups_and_perspective':'passed'}


def test_surface_width_maps(directory):
    from mathutils import Vector
    scene=reset()
    quad=mesh_object('Mapped Quad',[(-1,-.5,0),(1,-.5,0),(1,.5,0),(-1,.5,0)],faces=[(0,1,2,3)])
    marks=quad.data.attributes.new('freestyle_edge','BOOLEAN','EDGE')
    for edge in quad.data.edges:marks.data[edge.index].value=set(edge.vertices)=={0,1}
    uv=quad.data.uv_layers.new(name='Gradient')
    for point,value in zip(uv.data,[(0,.5),(1,.5),(1,1),(0,1)]):point.uv=value
    other=quad.data.uv_layers.new(name='Other')
    for point in other.data:point.uv=(.75,.5)
    quad.data.update()
    tree,source,brush,settings,output=setup_lines([quad],('marked',),width=8)
    _,plain=render(directory,'width_map_plain')
    image=bpy.data.images.new('Width Ramp',2,1,alpha=True,float_buffer=True)
    image.colorspace_settings.name='Non-Color'
    image.pixels.foreach_set([.25,.25,.25,1,.75,.75,.75,1]);image.update()
    mapping=tree.nodes.new('NPRLineNodeMap')
    set_value(mapping,'image',image);mapping.wrap_u=mapping.wrap_v='CLAMP'
    tree.links.new(mapping.outputs[0],socket(brush,'width_map'))
    _,mapped=render(directory,'width_map_uv_gradient')
    profile=lambda line,x:float(line[:,x,3].sum())
    require(abs(profile(mapped,72)-8*((72.5-48)/96))<.12 and abs(profile(mapped,120)-6)<.12,
            'Width must follow the evaluated corner UV image, not screen coordinates')
    set_value(brush,'width_map_strength',.5)
    _,strength=render(directory,'width_map_strength')
    require(abs(profile(strength,72)-(4+profile(mapped,72)*.5))<.12,'Width-map strength must blend toward the base width')
    set_value(brush,'width_map_strength',1)
    mapping.mute=True
    _,muted=render(directory,'width_map_muted')
    require(float(np.max(np.abs(muted-plain)))<.002,'Muting a map must restore the base width')
    mapping.mute=False;mapping.uv_selection='NAME';set_value(mapping,'uv_name','Other')
    _,named=render(directory,'width_map_named_uv')
    require(abs(profile(named,96)-6)<.1,'Named UV selection must use the requested corner layer')
    mapping.uv_selection='INDEX';set_value(mapping,'uv_index',1)
    _,indexed=render(directory,'width_map_indexed_uv')
    require(float(np.max(np.abs(indexed-named)))<.002,'UV index must follow the mesh UV layer order')
    mapping.uv_selection='NAME';set_value(mapping,'uv_name','Missing Layer')
    _,missing=render(directory,'width_map_missing_uv')
    require(float(np.max(np.abs(missing-plain)))<.002,'Missing source UV must remain neutral rather than erase geometry')

    set_value(mapping,'uv_name','Other');mapping.filter_mode='POINT'
    for point in other.data:point.uv=(1.25,.5)
    quad.data.update()
    for mode,expected in [('REPEAT',2),('CLAMP',6),('MIRROR',6),('CLIP',0)]:
        mapping.wrap_u=mode
        _,line=render(directory,'width_map_wrap_'+mode.lower())
        require(abs(profile(line,96)-expected)<.12,'Texture U boundary mode must be honored: '+mode)
    for point in other.data:point.uv=(-1.25,.5)
    quad.data.update();mapping.wrap_u='MIRROR_ONCE'
    _,once=render(directory,'width_map_wrap_mirror_once')
    require(abs(profile(once,96)-6)<.12,'Mirror Once must reflect once, then clamp')
    for point in other.data:point.uv=(.5,.5)
    quad.data.update();mapping.wrap_u='CLAMP'
    _,point=render(directory,'width_map_point_filter')
    mapping.filter_mode='BILINEAR'
    _,linear=render(directory,'width_map_bilinear_filter')
    require(abs(profile(point,96)-6)<.1 and abs(profile(linear,96)-4)<.1,'Point and bilinear filters must sample different values at a texel boundary')
    for point in other.data:point.uv=(.25,.5)
    quad.data.update()
    set_value(mapping,'tiling_u',2)
    _,tiled=render(directory,'width_map_tiling')
    require(abs(profile(tiled,96)-4)<.1,'Map tiling must transform UV coordinates')
    set_value(mapping,'tiling_u',1);set_value(mapping,'offset_u',.5)
    _,offset=render(directory,'width_map_offset')
    require(abs(profile(offset,96)-6)<.1,'Map offset must transform UV coordinates')
    set_value(mapping,'offset_u',0);set_value(mapping,'rotation',180)
    _,rotated=render(directory,'width_map_rotation')
    require(abs(profile(rotated,96)-6)<.1,'Map rotation must pivot around the texture center')
    set_value(mapping,'rotation',0);mapping.mode='SCREEN'
    _,screen=render(directory,'width_map_screen_coordinates')
    require(abs(profile(screen,72)-8*(72.5/192))<.12,'Screen maps must use normalized screen coordinates')

    mapping.mode='UV';mapping.uv_selection='INDEX';set_value(mapping,'uv_index',0)
    image.pixels.foreach_set([0,0,0,1]*2);image.update()
    for mode in ('SIMPLE','NORMAL','MULTIPLE'):
        brush.mode=mode
        _,empty=render(directory,'width_map_black_'+mode.lower())
        require(float(empty[:,:,3].max())==0,'A black width map must not leave isolated pen dots in '+mode)
    brush.mode='SIMPLE'
    image.pixels.foreach_set([.25,.25,.25,1,.75,.75,.75,1]);image.update()

    def section(line,a,b,t):
        camera=scene.camera
        projection=camera.calc_matrix_camera(bpy.context.evaluated_depsgraph_get(),x=192,y=192)@camera.matrix_world.inverted()
        def project(p):
            clip=projection@Vector((*p,1));p=clip.xyz/clip.w
            return np.array([(p.x*.5+.5)*192,(.5-p.y*.5)*192])
        pa,pb,center=project(a),project(b),project(a.lerp(b,t))
        tangent=(pb-pa)/np.linalg.norm(pb-pa);normal=np.array([-tangent[1],tangent[0]])
        positions=center[None,:]+np.arange(-12,12,.125)[:,None]*normal[None,:]-.5
        x,y=positions[:,0],positions[:,1];ix,iy=np.floor(x).astype(int),np.floor(y).astype(int)
        tx,ty=x-ix,y-iy;alpha=line[:,:,3]
        values=(alpha[iy,ix]*(1-tx)*(1-ty)+alpha[iy,ix+1]*tx*(1-ty)+
                alpha[iy+1,ix]*(1-tx)*ty+alpha[iy+1,ix+1]*tx*ty)
        return float(values.sum()*.125)

    scene.camera.location=(2,-3,5)
    scene.camera.rotation_euler=(-scene.camera.location).to_track_quat('-Z','Y').to_euler()
    scene.camera.data.type='PERSP'
    _,orbit=render(directory,'width_map_camera_orbit')
    a,b=[quad.matrix_world@quad.data.vertices[i].co for i in (0,1)]
    for t,expected in [(.25,2),(.75,6)]:require(abs(section(orbit,a,b,t)-expected)<.3,'Surface width at fixed UV must survive camera rotation')
    quad.shape_key_add(name='Basis')
    deform=quad.shape_key_add(name='Deform')
    for point in deform.data:
        point.co.z+=(point.co.x+1)*.5
        point.co.x*=.8
    deform.value=1
    _,deformed=render(directory,'width_map_evaluated_deformation')
    evaluated=quad.evaluated_get(bpy.context.evaluated_depsgraph_get())
    a,b=[evaluated.matrix_world@evaluated.data.vertices[i].co.copy() for i in (0,1)]
    for t,expected in [(.25,2),(.75,6)]:require(abs(section(deformed,a,b,t)-expected)<.3,'Evaluated deformation must retain the geometry-anchored UV width')
    quad.shape_key_clear()

    # A depth-varying source edge requires perspective-correct UV interpolation.
    scene.camera.location=(0,0,5);scene.camera.rotation_euler=(0,0,0)
    for vertex,co in zip(quad.data.vertices,[(-.5,-.5,1),(1.5,-.5,-3),(1.5,.5,-3),(-.5,.5,1)]):vertex.co=co
    quad.data.update()
    _,perspective=render(directory,'width_map_perspective_uv')
    a,b=[quad.matrix_world@quad.data.vertices[i].co for i in (0,1)]
    for t,expected in [(.25,2),(.75,6)]:require(abs(section(perspective,a,b,t)-expected)<.3,'Surface UV must interpolate in geometry space under perspective')
    blocker=mesh_object('UV Line Occluder',[(-3,-3,2),(3,-3,2),(3,3,2),(-3,3,2)],faces=[(0,1,2,3)])
    set_value(settings,'visible_marked',False);set_value(settings,'hidden_enabled',True)
    _,hidden_surface=render(directory,'width_map_hidden_source_uv')
    for t,expected in [(.25,2),(.75,6)]:require(abs(section(hidden_surface,a,b,t)-expected)<.3,'Hidden lines must retain their source UV width')
    blocker.hide_render=True;set_value(settings,'visible_marked',True);set_value(settings,'hidden_enabled',False)

    group=bpy.data.node_groups.new('Shared Width Mapping','NPRLineNodeTree')
    group.interface.new_socket(name='Map',in_out='OUTPUT',socket_type='NodeSocketNPRLineMap')
    output_node=group.nodes.new('NodeGroupOutput');inner=group.nodes.new('NPRLineNodeMap')
    set_value(inner,'image',image);inner.wrap_u=inner.wrap_v='CLAMP'
    group.links.new(inner.outputs[0],output_node.inputs['Map'])
    instance=tree.nodes.new('NPRLineNodeGroup');instance.node_tree=group
    tree.links.new(instance.outputs['Map'],socket(brush,'width_map'))
    _,grouped=render(directory,'width_map_grouped')
    require(float(np.max(np.abs(grouped-perspective)))<.002,'A grouped mapping descriptor must retain the image and coordinate settings')
    index_field=group.interface.new_socket(name='UV Index',in_out='INPUT',socket_type='NodeSocketInt')
    index_field.default_value=2147483647
    group_input=group.nodes.new('NodeGroupInput')
    group.links.new(group_input.outputs['UV Index'],socket(inner,'uv_index'))
    instance.inputs['UV Index'].default_value=2147483647
    _,bounded_index=render(directory,'width_map_linked_index_range')
    set_value(brush,'width_map_strength',0)
    _,without_map=render(directory,'width_map_linked_index_neutral')
    require(float(np.max(np.abs(bounded_index-without_map)))<.002,
            'An extreme linked UV index must clamp before integer conversion and use the missing-layer fallback')
    set_value(brush,'width_map_strength',1);instance.inputs['UV Index'].default_value=0
    _,grouped=render(directory,'width_map_grouped_index_restored')
    filename=Path(directory)/'width-map-roundtrip.blend'
    image.pack()
    np.save(Path(directory)/'width-map-expected.npy',grouped)
    bpy.ops.wm.save_as_mainfile(filepath=str(filename))
    bpy.ops.wm.open_mainfile(filepath=str(filename))
    _,reopened=render(directory,'width_map_reopened')
    require(float(np.max(np.abs(grouped-reopened)))<.002,'Map descriptions and corner UVs must survive saving and reopening')

    scene=reset()
    obj=mesh_object('UV Seam Ownership',[(-1,-1,0),(0,-1,0),(1,-1,0),(-1,1,0),(0,1,0),(1,1,0)],
                    faces=[(0,1,4,3),(1,2,5,4)])
    material_b=bpy.data.materials.new('UV Owner B');obj.data.materials.append(material_b)
    obj.data.polygons[1].material_index=1
    marks=obj.data.attributes.new('freestyle_edge','BOOLEAN','EDGE')
    for edge in obj.data.edges:marks.data[edge.index].value=set(edge.vertices)=={1,4}
    uv=obj.data.uv_layers.new(name='UVMap')
    for polygon in obj.data.polygons:
        for corner in polygon.loop_indices:uv.data[corner].uv=(.25 if polygon.index==0 else .75,.5)
    obj.data.update()
    tree,source,brush,settings,output=setup_lines([obj],('marked',),width=8)
    set_value(settings,'material_0',material_b)
    image=bpy.data.images.new('Seam Ramp',2,1,alpha=True,float_buffer=True)
    image.colorspace_settings.name='Non-Color';image.pixels.foreach_set([.25,.25,.25,1,.75,.75,.75,1]);image.update()
    mapping=tree.nodes.new('NPRLineNodeMap');set_value(mapping,'image',image)
    mapping.wrap_u=mapping.wrap_v='CLAMP'
    tree.links.new(mapping.outputs[0],socket(brush,'width_map'))
    _,seam=render(directory,'width_map_material_uv_owner')
    require(abs(float(seam[96,:,3].sum())-6)<.12,'A material-filtered edge must use the UVs of the owning face at a seam')

    # Two styles in the same submission must bind their own source images.
    tree.nodes.remove(mapping)
    set_value(settings,'material_0',None)
    _,baseline=render(directory,'width_map_no_mapping_after_remove')
    black=bpy.data.images.new('Black Width',1,1,alpha=True,float_buffer=True)
    black.colorspace_settings.name='Non-Color';black.pixels.foreach_set([0,0,0,1]);black.update()
    white=bpy.data.images.new('White Width',1,1,alpha=True,float_buffer=True)
    white.colorspace_settings.name='Non-Color';white.pixels.foreach_set([1,1,1,1]);white.update()
    first=tree.nodes.new('NPRLineNodeMap');set_value(first,'image',black)
    tree.links.new(first.outputs[0],socket(brush,'width_map'))
    other_brush=make_brush(tree,color=socket(brush,'color').default_value[:],width=8)
    second=tree.nodes.new('NPRLineNodeMap');set_value(second,'image',white)
    tree.links.new(second.outputs[0],socket(other_brush,'width_map'))
    other_rule=make_settings(tree,source,other_brush,('marked',))
    combine=tree.nodes.new('NPRLineNodeCombine');combine.mode='OVERLAY'
    tree.links.new(settings.outputs[0],socket(combine,'plans'));tree.links.new(other_rule.outputs[0],socket(combine,'plans'))
    tree.links.new(combine.outputs[0],socket(output,'plan'))
    _,two_styles=render(directory,'width_map_distinct_style_images')
    require(float(np.max(np.abs(two_styles-baseline)))<.002,'Brush dispatches must bind distinct width-map images without changing geometry')

    scene=reset()
    plane=mesh_object('Mapped Intersection Plane',[(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)],faces=[(0,1,2,3)])
    uv=plane.data.uv_layers.new(name='UVMap')
    for point,value in zip(uv.data,[(0,0),(1,0),(1,1),(0,1)]):point.uv=value
    plane.data.update()
    mesh_object('Other Intersection Plane',[(0,-1,-1),(0,1,-1),(0,1,1),(0,-1,1)],faces=[(0,1,2,3)])
    tree,source,brush,settings,output=setup_lines([plane],('intersection',),width=8)
    scene.camera.location=(3,-5,5)
    scene.camera.rotation_euler=(-scene.camera.location).to_track_quat('-Z','Y').to_euler()
    image=bpy.data.images.new('Contact Ramp',2,1,alpha=True,float_buffer=True)
    image.colorspace_settings.name='Non-Color';image.pixels.foreach_set([.25,.25,.25,1,.75,.75,.75,1]);image.update()
    mapping=tree.nodes.new('NPRLineNodeMap');set_value(mapping,'image',image)
    mapping.wrap_u=mapping.wrap_v='CLAMP'
    tree.links.new(mapping.outputs[0],socket(brush,'width_map'))
    _,contact=render(directory,'width_map_intersection_surface')
    require(abs(section(contact,Vector((0,-1,0)),Vector((0,1,0)),.5)-4)<.3,
            'Intersection strokes must use the rule-owning source surface UVs')
    plane.data.calc_loop_triangles()
    shared=set(plane.data.loop_triangles[0].vertices)&set(plane.data.loop_triangles[1].vertices)
    for i,point in enumerate(uv.data):point.uv=(.25 if plane.data.loops[i].vertex_index in shared else .75,.5)
    plane.data.update()
    _,pieces=render(directory,'width_map_intersection_uv_triangle_pieces')
    require(abs(section(pieces,Vector((0,-1,0)),Vector((0,1,0)),.5)-2)<.3,
            'Intersection merging must retain piecewise UV interpolation across triangulation boundaries')

    scene=reset()
    folded=mesh_object('Nonplanar Attribute Face',[(-1,-1,3),(1,-1,-3),(1,1,.5),(-1,1,-1)],faces=[(0,1,2,3)])
    marks=folded.data.attributes.new('freestyle_edge','BOOLEAN','EDGE')
    for edge in folded.data.edges:marks.data[edge.index].value=set(edge.vertices)=={1,2}
    uv=folded.data.uv_layers.new(name='UVMap')
    for point,value in zip(uv.data,[(.25,.5),(.25,.5),(.75,.5),(.25,.5)]):point.uv=value
    folded.data.update()
    tree,source,brush,settings,output=setup_lines([folded],('marked',),width=8)
    image=bpy.data.images.new('Folded Ramp',2,1,alpha=True,float_buffer=True)
    image.colorspace_settings.name='Non-Color';image.pixels.foreach_set([.25,.25,.25,1,.75,.75,.75,1]);image.update()
    mapping=tree.nodes.new('NPRLineNodeMap');set_value(mapping,'image',image)
    mapping.wrap_u=mapping.wrap_v='CLAMP'
    tree.links.new(mapping.outputs[0],socket(brush,'width_map'))
    _,nonplanar=render(directory,'width_map_nonplanar_source_triangle')
    require(abs(section(nonplanar,Vector((1,-1,-3)),Vector((1,1,.5)),.5)-4)<.3,
            'A nonplanar polygon must sample the containing 3D triangle, not an overlapping plane projection')
    return {'surface_width_maps_uv_screen_wrap_filter_perspective_groups':'passed'}


def test_pencil_contour_priority(directory):
    scene=reset()
    obj=mesh_object('Contour Ownership Quad',[(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)],faces=[(0,1,2,3)])
    marks=obj.data.attributes.new('freestyle_edge','BOOLEAN','EDGE')
    for value in marks.data:value.value=True
    tree,source,brush,settings,output=setup_lines([obj],('wire','marked'))
    _,native=render(directory,'contour_priority_native_edges')
    require(native[:,:,3].max()>.9,'The existing independent edge mode must remain available')
    set_value(settings,'contour_priority',True)
    _,suppressed=render(directory,'contour_priority_suppress_fallback')
    require(suppressed[:,:,3].max()<.001,'A disabled contour must not reappear as wire or marked edges')
    set_types(settings,'outline','wire','marked')
    _,outline=render(directory,'contour_priority_outline_enabled')
    require(outline[:,:,3].max()>.9,'Enabling the owning contour category must restore the boundary')

    reset()
    obj=mesh_object('Contour Ownership Internal Edge',[(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)],faces=[(0,1,2),(0,2,3)])
    for poly in obj.data.polygons:poly.use_smooth=True
    marks=obj.data.attributes.new('freestyle_edge','BOOLEAN','EDGE')
    for value in marks.data:value.value=True
    tree,source,brush,settings,output=setup_lines([obj],('wire','marked'))
    set_value(settings,'contour_priority',True)
    _,internal=render(directory,'contour_priority_internal_edge')
    require(float(internal[96,96,3])>.9 and float(internal[144,96,3])<.001,
            'Contour ownership must retain the internal polygon edge')
    mesh_object('Contour Ownership Occluder',[(-2,-2,.2),(2,-2,.2),(2,2,.2),(-2,2,.2)],faces=[(0,1,2,3)])
    set_value(settings,'hidden_enabled',True)
    _,hidden=render(directory,'contour_priority_hidden_internal')
    require(float(hidden[96,96,3])>.9 and float(hidden[144,96,3])<.001,
            'Hidden wire and marked edges must respect the same contour ownership')
    set_value(settings,'hidden_outline',True)
    set_value(settings,'object_contours',True)
    _,hidden_outline=render(directory,'contour_priority_hidden_outline')
    require(float(hidden_outline[144,96,3])>.9,'Per-object hidden contours must remain independently configurable')
    return {'pencil_contour_ownership_and_independent_native_edges':'passed'}


def test_pencil_far_connections(directory):
    for name,vertices,edges,marked in [
        ('cross_type_flow',[(-1,-1,0),(0,-1,0),(.005,-1,0),(1,-1,0),(.005,-1.01,0),(.871,-.51,0)],[(0,1),(2,3),(4,5)],True),
        ('whole_path_tangent',[(-1,-1,0),(-.0011,-1,0),(0,-.9989,0),(.005,-1,0),(1,-1,0),(.005,-.995,0),(.705,-.295,0)],[(0,1),(1,2),(3,4),(5,6)],False),
    ]:
        reset()
        obj=mesh_object(name,vertices,edges=edges)
        if marked:
            marks=obj.data.attributes.new('freestyle_edge','BOOLEAN','EDGE')
            for edge in obj.data.edges:marks.data[edge.index].value=set(edge.vertices)=={2,3}
        tree,source,brush,settings,output=setup_lines([obj],('wire','marked'),width=8)
        tree.show_performance=True
        set_value(settings,'gap_join',2);set_value(settings,'intersection_join',0)
        set_value(settings,'split_angle',90)
        brush.use_width_curve=False
        set_value(brush,'taper_start',.4);set_value(brush,'taper_end',.4)
        _,line=render(directory,'pencil_far_'+name)
        require('2 strokes' in tree.performance_info and float(line[146,104,3])>.8,
                'Far connection must prefer the smooth whole-path continuation: '+name+' '+str(float(line[146,104,3])))

    reset()
    obj=mesh_object('False Short Loop',[(-.2,-.5,0),(0,-.5,0),(.2,-.5,0)],edges=[(0,1),(1,2)])
    tree,source,brush,settings,output=setup_lines([obj],('wire',),width=8)
    set_value(settings,'gap_join',32);set_value(settings,'split_angle',180)
    brush.use_width_curve=False;set_value(brush,'taper_start',.4);set_value(brush,'taper_end',.4)
    _,line=render(directory,'pencil_far_reject_false_short_loop')
    require(float(line[118,96,3])>.9 and float(line[118,88,3])<.5,
            'A nearly straight short path must keep open ends when a large join radius includes both tips')
    return {'pencil_far_quartic_score_path_tangents_and_closure':'passed'}


def test_subpixel_topology(directory):
    import math
    scene=reset()
    obj=mesh_object('Foreshortened Connector',
                    [(-1,0,-.5),(0,0,-.5),(.0012,0,.5),(1,0,.5)],edges=[(0,1),(1,2),(2,3)])
    tree,source,brush,settings,output=setup_lines([obj],('wire',),width=4)
    tree.show_performance=True
    set_value(settings,'gap_join',0);set_value(settings,'split_angle',90)
    brush.use_width_curve=False;brush.raster_size_response=False
    set_value(brush,'taper_start',.3);set_value(brush,'taper_end',.3)
    for i,angle in enumerate((0,.0001,.0002,.0004,math.atan(.0012))):
        scene.camera.location=(5*math.sin(angle),0,5*math.cos(angle))
        scene.camera.rotation_euler=(-scene.camera.location).to_track_quat('-Z','Y').to_euler()
        _,line=render(directory,'subpixel_topology_'+str(i))
        require('1 strokes' in tree.performance_info,
                'A foreshortened source edge must preserve topology as its projected length crosses 0.05px: '+tree.performance_info)
        require(float(line[96,96,3])>.85,'Foreshortening must not introduce two new tapered caps at the middle of a path')
    reset()
    obj=mesh_object('Isolated Projected Point',[(0,0,-.5),(0,0,.5)],edges=[(0,1)])
    setup_lines([obj],('wire',),width=4)
    _,point=render(directory,'subpixel_topology_isolated_point')
    require(float(point[:,:,3].max())==0,
            'A fully collapsed isolated edge must not introduce a new pen dot; only its connectivity is retained')
    return {'subpixel_source_topology_and_camera_orbit_continuity':'passed'}


def test_pencil_path_continuity(directory):
    cases = [
        ('edge_on_connector', [(-.05,1,0),(0,0,0),(.0011,0,0),(-.1,-1,0)], 90, 1),
        ('genuine_corner', [(-1,0,0),(0,0,0),(-.3,-1,0)], 90, 2),
        ('protected_short_end', [(-1,0,0),(0,0,0),(0,.05,0)], 30, 1),
        ('nearby_sharp_turns', [(-1,0,0),(0,0,0),(0,2/48,0),(1,2/48,0)], 30, 2),
        ('subpixel_step', [(-1,0,0),(0,0,0),(0,2/48,0),(1,2/48,0)], 70, 1),
    ]
    for name,vertices,angle,expected in cases:
        reset()
        obj=mesh_object(name,vertices,edges=[(i,i+1) for i in range(len(vertices)-1)])
        tree,source,brush,settings,output=setup_lines([obj],('wire',),width=3)
        tree.show_performance=True
        set_value(settings,'split_angle',angle);set_value(settings,'gap_join',0)
        brush.use_width_curve=True;brush.raster_size_response=True
        render(directory,'pencil_continuity_'+name)
        require(f'{expected} strokes' in tree.performance_info,
                'Path turns must ignore edge-on direction noise and protect short ends: '+name+' '+tree.performance_info)

    reset()
    vertices=[(-1.5+3*i/599,.1 if i%2 else -.1,0) for i in range(600)]
    obj=mesh_object('Long Sharp Path',vertices,edges=[(i,i+1) for i in range(599)])
    tree,source,brush,settings,output=setup_lines([obj],('wire',),width=2)
    tree.show_performance=True
    set_value(settings,'split_angle',90);set_value(settings,'gap_join',0)
    render(directory,'pencil_continuity_long_sharp_path')
    require('599 strokes' in tree.performance_info,'Long paths must retain every genuine corner without recursive stack growth')

    reset()
    obj=mesh_object('Depth Separated Gap',[(-1,0,0),(-.01,0,0),(.01,0,-1),(1,0,-1)],edges=[(0,1),(2,3)])
    tree,source,brush,settings,output=setup_lines([obj],('wire',),width=3)
    tree.show_performance=True
    set_value(settings,'split_angle',90)
    render(directory,'pencil_continuity_screen_gap')
    require('1 strokes' in tree.performance_info,'Screen gap joins must not depend on scene scale or camera depth')
    set_value(settings,'screen_space_join',False)
    render(directory,'pencil_continuity_depth_limited_gap')
    require('2 strokes' in tree.performance_info,'Explicit depth-limited joining must remain available')

    reset()
    obj=mesh_object('Subpixel Multiple',[(-1,0,0),(1,0,0)],edges=[(0,1)])
    tree,source,brush,settings,output=setup_lines([obj],('wire',),width=2)
    brush.mode='MULTIPLE';brush.raster_size_response=True
    for key,value in {'dot_size':16,'dot_size_random':0,'dot_space_x':.1,'dot_space_y':.1,
                      'dot_space_random_x':0,'dot_space_random_y':0,'spacing':.02}.items():set_value(brush,key,value)
    _,line=render(directory,'pencil_continuity_multiple_two_pixels')
    require(float(line[93:99,70:122,3].sum())>10,
            'Minified Multiple tips must retain coverage instead of disappearing between texture samples')
    return {'pencil_path_tangents_protected_ends_and_minified_brush':'passed'}


def test_surface_color_maps(directory):
    scene=reset()
    quad=mesh_object('Color Mapped Quad',[(-1,-.5,0),(1,-.5,0),(1,.5,0),(-1,.5,0)],faces=[(0,1,2,3)])
    marks=quad.data.attributes.new('freestyle_edge','BOOLEAN','EDGE')
    for edge in quad.data.edges:marks.data[edge.index].value=set(edge.vertices)=={0,1}
    uv=quad.data.uv_layers.new(name='Gradient')
    for point,value in zip(uv.data,[(0,.5),(1,.5),(1,1),(0,1)]):point.uv=value
    other=quad.data.uv_layers.new(name='Blue Only')
    for point in other.data:point.uv=(.75,.5)
    quad.data.update()
    tree,source,brush,settings,output=setup_lines([quad],('marked',),color=(0,1,0,1),width=8)
    set_value(brush,'opacity',.5)
    _,plain=render(directory,'color_map_plain')
    image=bpy.data.images.new('Color Ramp',2,1,alpha=True,float_buffer=True)
    image.colorspace_settings.name='Non-Color'
    image.pixels.foreach_set([1,0,0,1,0,0,1,1]);image.update()
    mapping=tree.nodes.new('NPRLineNodeMap');set_value(mapping,'image',image)
    mapping.wrap_u=mapping.wrap_v='CLAMP'
    tree.links.new(mapping.outputs[0],socket(brush,'color_map'))
    _,mapped=render(directory,'color_map_uv_gradient')
    require(float(np.max(np.abs(mapped[:,:,3]-plain[:,:,3])))<.002,'A color map must not change brush coverage or accumulate opacity')
    for x in (72,96,120):
        u=(x+.5-48)/96;blue=max(0,min(1,u*2-.5))
        expected=np.array([1-blue,0,blue])*.5
        require(float(np.max(np.abs(mapped[120,x,:3]-expected)))<.015,'Simple color UV must remain continuous along the path')
    require(float(np.max(np.abs(mapped[:,:,:3].sum(axis=2)-mapped[:,:,3])))<.002,
            'Color must correspond to the winning coverage instead of independent RGB maxima')
    set_value(brush,'color_map_strength',.5)
    _,blend=render(directory,'color_map_strength')
    require(float(np.max(np.abs(blend[:,:,:3]-(mapped[:,:,:3]+plain[:,:,:3])*.5)))<.002,'Color map strength must blend with the base color')
    mapping.uv_selection='NAME';set_value(mapping,'uv_name','Blue Only')
    set_value(brush,'color_map_strength',1)
    _,named=render(directory,'color_map_named_uv')
    require(abs(float(named[120,96,2])-.5)<.002 and float(named[120,96,0])<.002,'Color maps must select their own UV layer')
    set_value(mapping,'uv_name','No Such Color UV')
    _,missing=render(directory,'color_map_missing_uv')
    require(float(np.max(np.abs(missing-plain)))<.002,'Missing UVs must retain the base color')
    set_value(mapping,'uv_name','Gradient')
    image.pixels.foreach_set([1,0,0,.5]*2);image.update()
    _,alpha_blend=render(directory,'color_map_image_alpha')
    require(float(np.max(np.abs(alpha_blend[120,96]-np.array([.5,0,0,.5]))))<.002,
            'Pencil Color Map must use image RGB independently of image alpha and stroke opacity')
    image.pixels.foreach_set([1,0,0,0]*2);image.update()
    _,clear_image=render(directory,'color_map_zero_image_alpha')
    require(float(np.max(np.abs(clear_image-alpha_blend)))<.002,
            'A zero image alpha must not discard valid RGB or change Pencil Color Map strength')
    image.pixels.foreach_set([.5,.5,.5,1]*2);image.update()
    tree.links.new(mapping.outputs[0],socket(brush,'width_map'))
    _,shared=render(directory,'color_and_width_shared_map')
    require(abs(float(shared[:,96,3].sum())-2)<.1 and
            float(np.max(np.abs(shared[120,96]-np.array([.25,.25,.25,.5]))))<.002,
            'One shared map must independently drive width and color')
    tree.links.remove(socket(brush,'width_map').links[0])
    mapping.mute=True
    _,muted=render(directory,'color_map_muted')
    require(float(np.max(np.abs(muted-plain)))<.002,'Muting a color map must restore the base style')
    mapping.mute=False
    for mode in ('SIMPLE','NORMAL','MULTIPLE'):
        brush.mode=mode
        _,line=render(directory,'color_map_mode_'+mode.lower())
        alpha=line[:,:,3];mask=alpha>.01
        require(bool(mask.any()) and float(np.max(np.abs(line[:,:,:3][mask]-alpha[mask,None]*.5)))<.002,
                'Mapped color must remain premultiplied in '+mode)
    brush.mode='SIMPLE'
    group=bpy.data.node_groups.new('Reusable Color Map','NPRLineNodeTree')
    group.interface.new_socket(name='Map',in_out='OUTPUT',socket_type='NodeSocketNPRLineMap')
    out_node=group.nodes.new('NodeGroupOutput');inner=group.nodes.new('NPRLineNodeMap')
    set_value(inner,'image',image);inner.wrap_u=inner.wrap_v='CLAMP'
    group.links.new(inner.outputs[0],out_node.inputs['Map'])
    instance=tree.nodes.new('NPRLineNodeGroup');instance.node_tree=group
    tree.links.new(instance.outputs['Map'],socket(brush,'color_map'))
    _,grouped=render(directory,'color_map_grouped')
    image.pack();np.save(Path(directory)/'color-map-expected.npy',grouped)
    filename=Path(directory)/'color-map-roundtrip.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(filename));bpy.ops.wm.open_mainfile(filepath=str(filename))
    _,reopened=render(directory,'color_map_reopened')
    require(float(np.max(np.abs(reopened-grouped)))<.002,'Grouped color mapping must survive a saved file')

    scene=reset()
    horizontal=mesh_object('Crossing Color Horizontal',[(-1,0,0),(1,0,0),(1,.5,0),(-1,.5,0)],faces=[(0,1,2,3)])
    vertical=mesh_object('Crossing Color Vertical',[(0,-1,0),(.5,-1,0),(.5,1,0),(0,1,0)],faces=[(0,1,2,3)])
    for obj,edge_indices,u in [(horizontal,{0,1},.25),(vertical,{0,3},.75)]:
        mark=obj.data.attributes.new('freestyle_edge','BOOLEAN','EDGE')
        for edge in obj.data.edges:mark.data[edge.index].value=set(edge.vertices)==edge_indices
        uv=obj.data.uv_layers.new(name='UVMap')
        for point in uv.data:point.uv=(u,.5)
        obj.data.update()
    tree,source,brush,settings,output=setup_lines([horizontal,vertical],('marked',),color=(0,1,0,1),width=12)
    set_value(brush,'opacity',.5)
    ignore=tree.nodes.new('NPRLineNodeOcclusion');ignore.mode='IGNORE'
    tree.links.new(source.outputs[0],socket(ignore,'objects'));tree.links.new(ignore.outputs[0],socket(output,'occlusion'))
    image=bpy.data.images.new('Crossing Red Blue',2,1,alpha=True,float_buffer=True)
    image.colorspace_settings.name='Non-Color';image.pixels.foreach_set([1,0,0,1,0,0,1,1]);image.update()
    mapping=tree.nodes.new('NPRLineNodeMap');set_value(mapping,'image',image)
    mapping.wrap_u=mapping.wrap_v='CLAMP'
    tree.links.new(mapping.outputs[0],socket(brush,'color_map'))
    _,cross=render(directory,'color_map_crossing_max_coverage')
    p=cross[96,96]
    require(abs(float(p[3])-.5)<.002 and abs(float(p[:3].sum())-.5)<.002 and min(float(p[0]),float(p[2]))<.002,
            'Crossing mapped stamps must keep one winning hue and apply opacity only once')
    return {'surface_color_maps_premultiplication_uv_blending_and_overlap':'passed'}


def test_contour_visible_owner(directory):
    scene = reset()
    scene.render.resolution_x = scene.render.resolution_y = 256
    target = mesh_object('Target Front And Back',
                         [(-1,-1,.5),(0,-1,.5),(0,1,.5),(-1,1,.5),
                          (-1,-1,-.5),(1,-1,-.5),(1,1,-.5),(-1,1,-.5)],
                         faces=[(0,1,2,3),(4,5,6,7)])
    blocker = mesh_object('Other Surface', [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)],
                          faces=[(0,1,2,3)])
    tree, source, brush, settings, output = setup_lines([target], ('outline',), width=2)
    set_value(settings, 'object_contours', True)
    set_value(settings, 'contour_priority', True)
    blocker.hide_render = True
    _, clear_outer = render(directory, 'contour_owner_clear_outer')
    require(float(clear_outer[128,128,3]) == 0,
            'A visible same-object surface beside an edge is an internal contour')
    blocker.hide_render = False
    _, blocked_outer = render(directory, 'contour_owner_other_surface_outer')
    require(float(blocked_outer[128,128,3]) > .5,
            'An occluded same-object side surface must not suppress the outer contour')
    set_value(settings, 'visible_outline', False)
    set_value(settings, 'visible_object', True)
    _, blocked_inner = render(directory, 'contour_owner_other_surface_not_inner')
    require(float(blocked_inner[128,128,3]) == 0,
            'The side surface hidden by another object must not create an internal contour')
    blocker.hide_render = True
    _, clear_inner = render(directory, 'contour_owner_clear_inner')
    require(float(clear_inner[128,128,3]) > .5,
            'Removing the blocker must restore the same-object internal contour')
    return {'per_object_contours_use_visible_surface_ownership': 'passed'}


def test_narrow_contour_gap(directory):
    scene = reset()
    scene.render.resolution_x = scene.render.resolution_y = 256
    obj = mesh_object('Subpixel Background Slit',
                      [(-1.5,-1,0),(-.005,-1,0),(-.005,1,0),(-1.5,1,0),
                       (.005,-1,0),(1.5,-1,0),(1.5,1,0),(.005,1,0)],
                      faces=[(0,1,2,3),(4,5,6,7)])
    tree, source, brush, settings, output = setup_lines([obj], ('outline',), width=2)
    set_value(settings, 'object_contours', True)
    set_value(settings, 'contour_priority', True)
    _, outline = render(directory, 'contour_subpixel_background_gap')
    require(float(outline[128, 128, 3]) > .5,
            'A subpixel background slit must retain its outer contour edges')
    set_value(settings, 'visible_outline', False)
    set_value(settings, 'visible_object', True)
    _, inner = render(directory, 'contour_subpixel_gap_not_internal')
    require(float(inner[:, :, 3].max()) == 0,
            'Two disconnected planar regions must not turn a background slit into an internal contour')
    return {'subpixel_background_gap_contour_classification': 'passed'}


def test_geometric_normals_ignore_shading_normals(directory):
    reset()
    bpy.ops.mesh.primitive_cube_add(size=1.5, rotation=(.3, .45, .2))
    obj = bpy.context.object
    obj.name = 'Geometric Normals Cube'
    for face in obj.data.polygons:
        face.use_smooth = True
    tree, source, brush, settings, output = setup_lines([obj], width=3)
    set_value(settings, 'normal_min', 45)
    set_value(settings, 'normal_max', 135)
    for label, enabled in [('contours', ('outline', 'object')), ('angle', ('normal',))]:
        custom = obj.data.attributes.get('custom_normal')
        if custom:
            obj.data.attributes.remove(custom)
        obj.data.update()
        for edge in EDGE_IDS:
            set_value(settings, 'visible_' + edge, edge in enabled)
        _, baseline = render(directory, 'geometry_normals_' + label)
        require(int((baseline[:, :, 3] > .1).sum()) > 100,
                'The geometric normal fixture must contain visible ' + label)
        obj.data.normals_split_custom_set([(0, 0, 1)] * len(obj.data.loops))
        obj.data.update()
        _, custom_lines = render(directory, 'geometry_normals_custom_' + label)
        require(float(np.max(np.abs(baseline - custom_lines))) < .002,
                'Custom shading normals must not change geometric ' + label)
    return {'geometric_contours_and_angles_ignore_custom_shading_normals': 'passed'}


def test_disconnected_graph(directory):
    reset()
    obj = mesh_object('Disconnect Quad', [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)], faces=[(0,1,2,3)])
    tree, source, brush, settings, output = setup_lines([obj])
    _, reference = render(directory, 'disconnect_reference')
    empty_style = make_settings(tree, source, brush)
    tree.links.remove(socket(empty_style, 'visible_brush').links[0])
    # This disconnected branch must neither execute nor invalidate the connected branch.
    _, unused = render(directory, 'disconnect_unused_branch')
    require(np.max(np.abs(reference - unused)) < .002,
            'An unconnected rule without a brush must not affect the active output')
    combine = tree.nodes.new('NPRLineNodeCombine')
    tree.links.new(settings.outputs[0], socket(combine, 'plans'))
    tree.links.new(empty_style.outputs[0], socket(combine, 'plans'))
    tree.links.new(combine.outputs[0], socket(output, 'plan'))
    _, suppressed = render(directory, 'disconnect_high_brush')
    require(float(suppressed[:,:,3].max()) == 0,
            'A higher-priority missing brush is empty ink and must not fall back to a lower rule')
    empty_style.enabled = False
    _, fallback = render(directory, 'disconnect_disabled_rule')
    require(np.max(np.abs(reference - fallback)) < .002,
            'Disabling an empty-style rule must release scope to the valid lower rule')
    empty_style.enabled = True
    empty_style.mute = True
    _, muted_rule = render(directory, 'disconnect_muted_rule')
    require(np.max(np.abs(reference - muted_rule)) < .002,
            'Muting a rule without a compatible internal plan link must release its scope')
    group = bpy.data.node_groups.new('Plan Pass Through', 'NPRLineNodeTree')
    group.interface.new_socket(name='Plan', in_out='INPUT', socket_type='NodeSocketNPRLinePlan')
    group.interface.new_socket(name='Plan', in_out='OUTPUT', socket_type='NodeSocketNPRLinePlan')
    group_in = group.nodes.new('NodeGroupInput')
    group_out = group.nodes.new('NodeGroupOutput')
    group.links.new(group_in.outputs['Plan'], group_out.inputs['Plan'])
    group_node = tree.nodes.new('NPRLineNodeGroup')
    group_node.node_tree = group
    tree.links.new(settings.outputs[0], group_node.inputs['Plan'])
    tree.links.new(group_node.outputs['Plan'], socket(output, 'plan'))
    _, grouped = render(directory, 'disconnect_passthrough_group')
    require(np.max(np.abs(reference - grouped)) < .002, 'A plan passthrough group must preserve ink')
    group_node.mute = True
    _, muted_group = render(directory, 'disconnect_muted_group')
    require(np.max(np.abs(reference - muted_group)) < .002,
            'A muted group must use compatible native descriptor internal links')
    tree.links.remove(socket(output, 'plan').links[0])
    _, no_output_plan = render(directory, 'disconnect_output_plan')
    require(float(no_output_plan[:,:,3].max()) == 0,
            'An output without a plan must produce empty ink rather than executing all rules')
    tree.links.new(settings.outputs[0], socket(output, 'plan'))
    tree.links.remove(socket(settings, 'objects').links[0])
    _, no_scope = render(directory, 'disconnect_source_scope')
    require(float(no_scope[:,:,3].max()) == 0,
            'A rule without an object range must not implicitly select the whole scene')
    return {'disconnected_graphs_and_missing_styles': 'passed'}


def test_viewport_map_image_changes(directory):
    """Reuse the viewport engine: F12 creates a new engine and cannot test stale expansion."""
    import gpu
    gpu.init()
    scene = reset()
    obj = mesh_object('Hot Map Wire', [(-1.5, 0, 0), (1.5, 0, 0)], edges=[(0, 1)])
    tree, source, brush, settings, output = setup_lines([obj], ('wire',), width=16)
    output.mode = 'FULL'
    set_value(brush, 'color', (1, 0, 1, 1))
    images = []
    for name, value in [('Thin Map', .25), ('Thick Map', 1.0)]:
        image = bpy.data.images.new(name, 1, 1, alpha=True, float_buffer=True)
        image.colorspace_settings.name = 'Non-Color'
        image.pixels[:] = (value, value, value, 1)
        image.update()
        images.append(image)
    mapping = tree.nodes.new('NPRLineNodeMap')
    mapping.mode = 'SCREEN'
    area = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
    space = area.spaces.active
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space.shading.type = 'RENDERED'
    space.overlay.show_overlays = False
    bpy.context.view_layer.update()
    view = scene.camera.matrix_world.inverted()
    projection = scene.camera.calc_matrix_camera(bpy.context.evaluated_depsgraph_get(), x=192, y=192)

    def draw(offscreen, name):
        bpy.context.view_layer.update()
        offscreen.draw_view3d(scene, bpy.context.view_layer, space, region, view, projection,
                             do_color_management=False, draw_background=False)
        with offscreen.bind():
            pixels = np.asarray(gpu.state.active_framebuffer_get().read_color(
                0, 0, 192, 192, 4, 0, 'FLOAT')).copy().reshape(192, 192, 4)
        require(np.isfinite(pixels).all(), 'Viewport map update produced non-finite pixels')
        np.save(directory / (name + '.npy'), pixels)
        return pixels

    for field in ('width_map', 'distortion_map'):
        set_value(mapping, 'image', images[0])
        link = tree.links.new(mapping.outputs[0], socket(brush, field))
        set_value(brush, 'distortion_map_amount', 12)
        cached = gpu.types.GPUOffScreen(192, 192, format='RGBA32F')
        try:
            original = draw(cached, 'hot_' + field + '_original')
            set_value(mapping, 'image', images[1])
            changed = draw(cached, 'hot_' + field + '_changed')
            fresh = gpu.types.GPUOffScreen(192, 192, format='RGBA32F')
            try:
                expected = draw(fresh, 'hot_' + field + '_cold_reference')
            finally:
                fresh.free()
            require(float(np.abs(expected - original).sum()) > 50,
                    'The image replacement must materially change ' + field)
            require(float(np.max(np.abs(changed - expected))) < .002,
                    'Changing the ' + field + ' image must invalidate expanded GPU strokes')
        finally:
            cached.free()
        tree.links.remove(link)
    # Disable and re-enable the same engine after allocating all four coverage attachments.
    brush.mode = 'MULTIPLE'
    brush.raster_size_response = True
    set_value(output, 'samples', 4)
    cached = gpu.types.GPUOffScreen(192, 192, format='RGBA32F')
    try:
        active = draw(cached, 'hot_coverage_enabled')
        require(float(active[:, :, 3].max()) > .5, 'Packed viewport coverage must draw')
        scene.eevee.use_geometry_lines = False
        disabled = draw(cached, 'hot_coverage_disabled')
        require(float(disabled[:, :, 3].max()) == 0, 'Disabling geometry lines must remove the ink')
        scene.eevee.use_geometry_lines = True
        restored = draw(cached, 'hot_coverage_reenabled')
        require(float(np.max(np.abs(restored - active))) < .002,
                'Re-enabling geometry lines must recreate packed coverage resources')
    finally:
        cached.free()
    return {'viewport_image_replacement_and_packed_coverage_lifetime': 'passed'}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True)
    parser.add_argument('--phase', choices=('nodes', 'render', 'features', 'advanced', 'filter', 'camera', 'brush', 'feedback', 'pencil', 'reduction', 'maps', 'color', 'continuity', 'far', 'classification', 'dot', 'subpixel', 'detail_maps', 'attributes', 'wave', 'pressure', 'disconnected', 'geometry_normals', 'viewport_cache', 'all'), default='all')
    parser.add_argument('--reopen', help='Verify a generated group scene in a new process')
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])
    directory = Path(args.output).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    results = {}
    if args.reopen:
        filename = Path(args.reopen).resolve()
        bpy.ops.wm.open_mainfile(filepath=str(filename))
        _, reopened = render(directory, 'cold_grouped_reopened')
        expected = np.load(filename.parent / 'grouped-expected.npy')
        require(np.max(np.abs(expected - reopened)) < .002,
                'A new Blender process must restore group rendering and scalar values')
        results['cold_group_roundtrip'] = 'passed'
        print('GEOMETRY_LINES_TEST_OK ' + json.dumps(results), flush=True)
        return
    if args.phase in ('nodes', 'all'):
        results.update(test_nodes(directory))
    if args.phase in ('render', 'all'):
        results.update(test_render(directory))
    if args.phase in ('features', 'all'):
        results.update(test_geometry_features(directory))
    if args.phase in ('advanced', 'all'):
        results.update(test_groups_layers_instances(directory))
    if args.phase in ('filter', 'all'):
        results.update(test_filter_bridge(directory))
    if args.phase in ('camera', 'all'):
        results.update(test_camera_effects(directory))
    if args.phase in ('brush', 'all'):
        results.update(test_brush_details(directory))
    if args.phase in ('feedback', 'all'):
        results.update(test_feedback(directory))
    if args.phase in ('pencil', 'all'):
        results.update(test_pencil_controls(directory))
        results.update(test_pencil_dash_caps(directory))
        results.update(test_pencil_geometry(directory))
        results.update(test_stroke_orientation(directory))
        results.update(test_multiple_tip(directory))
        results.update(test_brush_connections_and_linked_limits(directory))
    if args.phase in ('disconnected', 'all'):
        results.update(test_disconnected_graph(directory))
    if args.phase in ('reduction', 'all'):
        results.update(test_distance_reduction(directory))
    if args.phase in ('maps', 'all'):
        results.update(test_surface_width_maps(directory))
    if args.phase in ('color', 'all'):
        results.update(test_surface_color_maps(directory))
    if args.phase in ('continuity', 'all'):
        results.update(test_pencil_path_continuity(directory))
    if args.phase in ('far', 'all'):
        results.update(test_pencil_far_connections(directory))
        results.update(test_intersection_secondary_owner(directory))
    if args.phase in ('classification', 'all'):
        results.update(test_pencil_contour_priority(directory))
        results.update(test_narrow_contour_gap(directory))
        results.update(test_contour_visible_owner(directory))
    if args.phase in ('dot', 'all'):
        results.update(test_multiple_dot_kernel(directory))
        results.update(test_tip_reference_previews(directory))
        results.update(test_textured_sample_maximum(directory))
        results.update(test_closed_tip_anchor(directory))
    if args.phase in ('subpixel', 'all'):
        results.update(test_subpixel_topology(directory))
    if args.phase in ('detail_maps', 'all'):
        results.update(test_detail_maps(directory))
    if args.phase in ('attributes', 'all'):
        results.update(test_color_attributes(directory))
    if args.phase in ('wave', 'all'):
        results.update(test_pixel_wave(directory))
    if args.phase in ('pressure', 'all'):
        results.update(test_raster_pressure_and_visibility_grid(directory))
    if args.phase in ('geometry_normals', 'all'):
        results.update(test_geometric_normals_ignore_shading_normals(directory))
    if args.phase in ('viewport_cache', 'all'):
        results.update(test_viewport_map_image_changes(directory))
    results['blender'] = bpy.app.version_string
    results['build_hash'] = bpy.app.build_hash.decode()
    if args.phase != 'nodes':
        import gpu
        results['backend'] = gpu.platform.backend_type_get()
        results['renderer'] = gpu.platform.renderer_get()
    with (directory / 'results.json').open('w', encoding='utf-8') as out:
        json.dump(results, out, ensure_ascii=False, indent=2)
    print('GEOMETRY_LINES_TEST_OK ' + json.dumps(results, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
