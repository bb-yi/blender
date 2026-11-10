# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
import math
import time
from bpy.utils import previews
from bpy.props import BoolProperty, EnumProperty, IntProperty, StringProperty
from bpy.types import Operator
from .node import NodeAddOperator


# Preview pixels are runtime UI resources, never Images or scene data-blocks.
if '_brush_previews' in globals() and _brush_previews is not None:
    previews.remove(_brush_previews)
if '_clear_brush_preview_on_load' in globals():
    if _clear_brush_preview_on_load in bpy.app.handlers.load_pre:
        bpy.app.handlers.load_pre.remove(_clear_brush_preview_on_load)
if '_migrate_brush_fields_on_load' in globals():
    if _migrate_brush_fields_on_load in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(_migrate_brush_fields_on_load)
if '_update_output_brush_previews' in globals():
    if bpy.app.timers.is_registered(_update_output_brush_previews):
        bpy.app.timers.unregister(_update_output_brush_previews)
_brush_previews = None
_brush_preview_cache = {}
_region_preview_busy = False
_region_preview_cache = {}
_region_preview_pending = {}
_region_preview_errors = {}
_BRUSH_PREVIEW_IDLE_DELAY = .35

FIELD_BRUSH_TYPES = {'NPRLineNodeBrushOutput', 'NPRLineNodeBrushClosureOutput'}
BRUSH_TYPES = {'NPRLineNodeBrush'} | FIELD_BRUSH_TYPES


def add_brush_closure(tree, location=(0, 0)):
    """Two native zone boundaries; no Brush geometry link inside the closure."""
    parameters = tree.nodes.new('NPRLineNodeBrushClosureInput')
    output = tree.nodes.new('NPRLineNodeBrushClosureOutput')
    parameters.pair_with_output(output)
    parameters.location = (location[0] - 300, location[1])
    output.location = (location[0] + 300, location[1])
    parameters.width = 180
    output.width = 270
    return parameters, output


def brush_effect_group(effect):
    """Reusable ordinary node groups, with no special brush-effect evaluator."""
    for tree in bpy.data.node_groups:
        if tree.bl_idname == 'NPRLineNodeTree' and tree.get('npr_brush_effect') == effect:
            return tree
    title = {'WAVE': 'Wave', 'DASH': 'Dashes', 'TAPER': 'Taper', 'RANDOM': 'Per Stroke Random'}[effect]
    tree = bpy.data.node_groups.new(title, 'NPRLineNodeTree')
    tree['npr_brush_effect'] = effect

    def interface(name, value=0, output=False, vector=False):
        socket = tree.interface.new_socket(name=name, in_out='OUTPUT' if output else 'INPUT',
                    socket_type='NodeSocketVector' if vector else 'NodeSocketFloat')
        if not output:
            socket.default_value = value
        return socket

    inputs = {'WAVE': [('Distance', 0), ('Amplitude', 8), ('Wavelength', 64), ('Phase', 0)],
              'DASH': [('Distance', 0), ('Length', 8), ('Gap', 5), ('Phase', 0)],
              'TAPER': [('Factor', 0), ('Width', 4)],
              'RANDOM': [('ID', 0), ('Seed', 0), ('Min', .5), ('Max', 1.5)]}[effect]
    for name, default in inputs:
        interface(name, default)
    interface({'WAVE': 'Offset', 'DASH': 'Opacity', 'TAPER': 'Width', 'RANDOM': 'Value'}[effect],
              output=True, vector=effect == 'WAVE')
    source = tree.nodes.new('NodeGroupInput')
    target = tree.nodes.new('NodeGroupOutput')
    source.location = (-700, 120)
    target.location = (600, 120)

    def connect(value, socket):
        if isinstance(value, bpy.types.NodeSocket):
            tree.links.new(value, socket)
        else:
            socket.default_value = value

    def math_node(operation, *values):
        node = tree.nodes.new('ShaderNodeMath')
        node.operation = operation
        index = len(tree.nodes) - 3
        node.location = (-450 + (index % 4) * 210, 120 - (index // 4) * 190)
        for socket, value in zip(node.inputs, values):
            connect(value, socket)
        return node.outputs[0]

    p = source.outputs
    if effect == 'WAVE':
        phase = math_node('ADD', math_node('DIVIDE', p['Distance'], p['Wavelength']), p['Phase'])
        sine = math_node('SINE', math_node('MULTIPLY', phase, math.tau))
        amplitude = math_node('MULTIPLY', sine, p['Amplitude'])
        vector = tree.nodes.new('ShaderNodeCombineXYZ')
        vector.location = (380, 120)
        tree.links.new(amplitude, vector.inputs['Y'])
        value = vector.outputs[0]
    elif effect == 'DASH':
        phase = math_node('ADD', p['Distance'], p['Phase'])
        period = math_node('ADD', p['Length'], p['Gap'])
        value = math_node('LESS_THAN', math_node('FLOORED_MODULO', phase, period), p['Length'])
    elif effect == 'TAPER':
        curve = tree.nodes.new('ShaderNodeFloatCurve')
        curve.location = (-420, 120)
        mapping = curve.mapping
        mapping.curves[0].points[0].location = (0, 0)
        mapping.curves[0].points[-1].location = (1, 0)
        mapping.curves[0].points.new(.2, 1)
        mapping.curves[0].points.new(.8, 1)
        mapping.update()
        tree.links.new(p['Factor'], curve.inputs['Value'])
        # Apply pressure in the same raster size domain as the quick brush.
        # A plain width multiplier makes thin ends broader than Pencil's curve.
        domain = math_node('MINIMUM', p['Width'],
                           math_node('MULTIPLY_ADD', p['Width'], .5, 1))
        pressure = math_node('MULTIPLY', curve.outputs[0], domain)
        value = math_node('MAXIMUM', pressure, math_node('MULTIPLY_ADD', pressure, 2, -2))
    else:
        vector = tree.nodes.new('ShaderNodeCombineXYZ')
        vector.location = (-420, 120)
        tree.links.new(p['ID'], vector.inputs['X'])
        tree.links.new(p['Seed'], vector.inputs['Y'])
        noise = tree.nodes.new('ShaderNodeTexWhiteNoise')
        noise.noise_dimensions = '2D'
        noise.location = (-200, 120)
        tree.links.new(vector.outputs[0], noise.inputs['Vector'])
        value = math_node('ADD', p['Min'], math_node('MULTIPLY', noise.outputs['Value'],
                          math_node('SUBTRACT', p['Max'], p['Min'])))
    tree.links.new(value, target.inputs[0])
    return tree


class NODE_OT_npr_line_brush_effect(NodeAddOperator, Operator):
    bl_idname = 'node.npr_line_brush_effect'
    bl_label = 'Add Brush Effect Group'
    bl_description = 'Add an editable node group for a brush effect'
    bl_options = {'REGISTER', 'UNDO'}
    use_transform: BoolProperty(name='Use Transform', default=True)
    effect: EnumProperty(name='Effect', translation_context='NPRBrushEffect',
                        items=(('WAVE', 'Wave', 'Distance to a normal offset'),
                               ('DASH', 'Dashes', 'Distance to an opacity mask'),
                               ('TAPER', 'Taper', 'Factor through an editable Float Curve'),
                               ('RANDOM', 'Per Stroke Random', 'ID and seed to a value range')))

    @classmethod
    def poll(cls, context):
        space = context.space_data
        return space and space.type == 'NODE_EDITOR' and space.tree_type == 'NPRLineNodeTree'

    def execute(self, context):
        group = brush_effect_group(self.effect)
        self.deselect_nodes(context)
        node = self.create_node(context, 'NPRLineNodeGroup')
        if node is None:
            return {'CANCELLED'}
        node.node_tree = group
        return {'FINISHED'}


def _preview_parent_groups(node):
    """Capture the edited instance now; deferred evaluation has no editor context."""
    space = bpy.context.space_data
    if not space or space.type != 'NODE_EDITOR' or space.edit_tree != node.id_data:
        return ()
    path = tuple(space.path)
    parents = []
    for previous, current in zip(path, path[1:]):
        parent = previous.node_tree.nodes.active
        if not parent or parent.bl_idname != 'NPRLineNodeGroup' or parent.node_tree != current.node_tree:
            return ()
        parents.append(parent)
    return tuple(parents)


def _preview_key(node, parents):
    return (node.as_pointer(), tuple(parent.as_pointer() for parent in parents))


def _preview_sampling(node, parents):
    root = parents[0].id_data if parents else node.id_data
    output = next((n for n in root.nodes if n.bl_idname == 'NPRLineNodeOutput' and n.is_active_output), None)
    quality = next((s for s in output.inputs if s.identifier == 'field_sample_length'), None) if output else None
    return float(_socket_value(quality) or .9) if quality else .9


def region_preview(node, length=256, width=512, height=160, parents=None):
    """Render a disposable local stroke through the scene's actual brush evaluator.

    Preview fitting happens after evaluation; the requested length stays in pixels.
    No user scene, object, image, node tree, or file is saved or edited.
    """
    global _brush_previews, _region_preview_busy
    if _region_preview_busy:
        return None
    import numpy as np
    _region_preview_busy = True
    parents = _preview_parent_groups(node) if parents is None else parents
    scene = tree = obj = mesh = camera = camera_data = None
    trees = []
    try:
        scene = bpy.data.scenes.new('Brush Region Preview')
        scene.render.engine = 'BLENDER_EEVEE'
        render_width = int(length) + 64
        render_height = 128
        scene.render.resolution_x = render_width
        scene.render.resolution_y = render_height
        scene.render.resolution_percentage = 100
        scene.render.film_transparent = True
        scene.eevee.taa_samples = scene.eevee.taa_render_samples = 1
        camera_data = bpy.data.cameras.new('Brush Preview Camera')
        camera_data.type = 'ORTHO'
        camera_data.ortho_scale = render_width
        camera = bpy.data.objects.new('Brush Preview Camera', camera_data)
        scene.collection.objects.link(camera)
        camera.location = (0, 0, 5)
        scene.camera = camera
        mesh = bpy.data.meshes.new('Brush Preview Local Stroke')
        mesh.from_pydata([(-length / 2, 0, 0), (length / 2, 0, 0)], [(0, 1)], [])
        obj = bpy.data.objects.new('Brush Preview Local Stroke', mesh)
        scene.collection.objects.link(obj)
        tree = node.id_data.copy()
        trees.append(tree)
        selected = tree.nodes[node.name].outputs[0]
        original_tree = node.id_data
        # Carry the selected Brush through copies of its ancestor groups, retaining
        # linked fields and per-instance defaults instead of baking scalar guesses.
        for parent in reversed(parents):
            if parent.node_tree != original_tree:
                raise ReferenceError('Edited Brush Output group path changed')
            interface = tree.interface.new_socket(name='Brush Preview', in_out='OUTPUT',
                                                  socket_type='NodeSocketNPRLineBrush')
            group_output = tree.nodes.new('NodeGroupOutput')
            group_output.is_active_output = True
            tree.links.new(selected, input_socket(group_output, interface.identifier))
            parent_tree = parent.id_data.copy()
            trees.append(parent_tree)
            instance = parent_tree.nodes[parent.name]
            instance.node_tree = tree
            selected = next(s for s in instance.outputs if s.identifier == interface.identifier)
            tree = parent_tree
            original_tree = parent.id_data
        output = tree.nodes.new('NPRLineNodeOutput')
        output.is_active_output = True
        input_socket(output, 'field_sample_length').default_value = _preview_sampling(node, parents)
        source = tree.nodes.new('NPRLineNodeObjects')
        source.mode = 'OBJECTS'
        input_socket(source, 'object_0').default_value = obj
        settings = tree.nodes.new('NPRLineNodeSettings')
        for socket in settings.inputs:
            if socket.identifier.startswith(('visible_', 'hidden_')) and socket.type == 'BOOLEAN':
                socket.default_value = socket.identifier == 'visible_wire'
        tree.links.new(source.outputs[0], input_socket(settings, 'objects'))
        tree.links.new(selected, input_socket(settings, 'visible_brush'))
        tree.links.new(settings.outputs[0], input_socket(output, 'plan'))
        scene.eevee.geometry_line_tree = tree
        scene.eevee.use_geometry_lines = True
        scene.view_layers[0].eevee.use_pass_geometry_line = True
        layer = np.asarray(tree.preview_pixels(scene, render_width, render_height), dtype=np.float32)
        layer = layer.reshape(render_height, render_width, 4)[::-1].copy()
        signature = _region_signature(node, parents)
        key = _preview_key(node, parents)
        _region_preview_cache[key] = (signature, layer)
        preview = _output_preview_swatch(node, width, height, signature, layer, key)
        if _clear_brush_preview_on_load not in bpy.app.handlers.load_pre:
            bpy.app.handlers.load_pre.append(_clear_brush_preview_on_load)
        return preview
    finally:
        if scene is not None:
            bpy.data.scenes.remove(scene)
        for copied_tree in reversed(trees):
            bpy.data.node_groups.remove(copied_tree)
        for collection, value in ((bpy.data.objects, obj), (bpy.data.objects, camera), (bpy.data.meshes, mesh),
                                  (bpy.data.cameras, camera_data)):
            if value is not None:
                collection.remove(value)
        _region_preview_busy = False


def _region_signature(node, parents=()):
    """Only upstream fields and brush settings affect the local test stroke."""
    seen = set()
    def signature(item):
        pointer = item.as_pointer()
        if pointer in seen:
            return ('reference', pointer)
        seen.add(pointer)
        values = []
        for socket in (*item.inputs, *item.outputs):
            if socket.is_output:
                value = getattr(socket, 'default_value', None)
                if value is not None:
                    values.append((socket.identifier, _preview_value_key(value)))
                continue
            if socket.is_linked:
                values.append((socket.identifier, tuple((link.from_socket.identifier,
                               signature(link.from_node)) for link in socket.links)))
            else:
                value = getattr(socket, 'default_value', None)
                values.append((socket.identifier, _preview_value_key(value)))
        properties = tuple((name, getattr(item, name)) for name in (
            'mode', 'mute', 'operation', 'data_type', 'blend_type', 'factor_mode', 'clamp_factor',
            'clamp_result', 'use_clamp', 'interpolation_type', 'clamp', 'interpolation',
            'extension', 'projection', 'raster_size_response', 'stroke_mode', 'wave_units',
            'override_stroke_connection', 'preview_length', 'enabled', 'uv_selection',
            'wrap_u', 'wrap_v', 'filter_mode') if hasattr(item, name))
        ramp = getattr(item, 'color_ramp', None)
        color_ramp = ((ramp.interpolation, ramp.color_mode, ramp.hue_interpolation,
                       tuple((e.position, tuple(e.color)) for e in ramp.elements)) if ramp else None)
        group = getattr(item, 'node_tree', None)
        nested = tuple(signature(n) for n in group.nodes) if group else None
        image = getattr(item, 'image', None)
        image_key = _preview_value_key(image)
        curves = tuple(_curve_signature(item, c) for c in ('width', 'opacity'))
        mapping = _mapping_signature(getattr(item, 'mapping', None))
        return (pointer, properties, tuple(values), color_ramp, nested, image_key, curves, mapping,
                getattr(item, 'noise_dimensions', None))
    return (signature(node), tuple(signature(parent) for parent in parents), _preview_sampling(node, parents))


def _output_preview_swatch(node, width, height, signature=None, layer=None, cache_key=None):
    """All swatch sizes share one evaluated stroke; fitting never changes field inputs."""
    global _brush_previews
    import numpy as np
    if _brush_previews is None:
        _brush_previews = previews.new()
    key = f'region_{cache_key or _preview_key(node, ())}_{width}_{height}'
    if key not in _brush_previews:
        if len(_brush_previews) >= 64:
            oldest = next(iter(_brush_previews))
            del _brush_previews[oldest]
            _brush_preview_cache.pop(oldest, None)
        _brush_previews.new(key)
    cached = _brush_preview_cache.get(key)
    if cached and cached[0] == signature:
        return _brush_previews[key]
    yy, xx = np.mgrid[:height, :width]
    pixels = np.ones((height, width, 4), dtype=np.float32)
    pixels[:, :, :3] = np.where((xx // 12 + yy // 12) % 2, .90, .98)[:, :, None]
    pixels[height // 2, :, :3] = (.35, .45, .55)
    if layer is not None:
        render_height, render_width = layer.shape[:2]
        scale = min(width / render_width, height / render_height)
        draw_width, draw_height = int(render_width * scale), int(render_height * scale)
        xs = np.minimum((np.arange(draw_width) / scale).astype(int), render_width - 1)
        ys = np.minimum((np.arange(draw_height) / scale).astype(int), render_height - 1)
        sampled = layer[ys[:, None], xs[None, :]]
        alpha = sampled[:, :, 3:4]
        rgb = np.divide(sampled[:, :, :3], np.maximum(alpha, 1e-10))
        rgb = np.where(rgb < .0031308, rgb * 12.92, 1.055 * np.maximum(rgb, 0) ** (1 / 2.4) - .055)
        left, bottom = (width - draw_width) // 2, (height - draw_height) // 2
        target = pixels[bottom:bottom + draw_height, left:left + draw_width, :3]
        target[:] = target * (1 - alpha) + rgb * alpha
    preview = _brush_previews[key]
    preview.image_size = (width, height)
    preview.image_pixels_float = pixels[::-1].reshape(-1).tolist()
    _brush_preview_cache[key] = (signature, time.monotonic())
    return preview


def _redraw_brush_nodes():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'NODE_EDITOR' and area.ui_type == 'NPRLineNodeTree':
                area.tag_redraw()


def output_brush_preview(node, width=192, height=64):
    """Queue real GPU evaluation outside drawing and return a cached inline swatch."""
    parents = _preview_parent_groups(node)
    signature = _region_signature(node, parents)
    cache_key = _preview_key(node, parents)
    cached = _region_preview_cache.get(cache_key)
    if not _region_preview_busy:
        if cached is not None and cached[0] == signature:
            _region_preview_pending.pop(cache_key, None)
        else:
            pending = _region_preview_pending.get(cache_key)
            if pending is None or pending[3] != signature:
                _region_preview_pending[cache_key] = (node.id_data, node.name, parents, signature,
                                                     time.monotonic())
            if not bpy.app.timers.is_registered(_update_output_brush_previews):
                bpy.app.timers.register(_update_output_brush_previews,
                                        first_interval=_BRUSH_PREVIEW_IDLE_DELAY)
    if _clear_brush_preview_on_load not in bpy.app.handlers.load_pre:
        bpy.app.handlers.load_pre.append(_clear_brush_preview_on_load)
    return _output_preview_swatch(node, width, height, *(cached if cached else (None, None)), cache_key)


def _update_output_brush_previews():
    if not _region_preview_pending:
        return None
    if _region_preview_busy or bpy.app.is_job_running('RENDER'):
        return .2
    key = next(reversed(_region_preview_pending))
    tree, name, parents, signature, changed_at = _region_preview_pending[key]
    # A timer alone is not a debounce: a continuous slider drag would otherwise
    # run synchronous offscreen drawing every few events on the UI thread.
    if not bpy.app.background:
        remaining = _BRUSH_PREVIEW_IDLE_DELAY - (time.monotonic() - changed_at)
        if remaining > 0:
            return remaining
        if any(op.bl_idname.startswith(('TRANSFORM_OT_', 'NODE_OT_link', 'NODE_OT_resize'))
               for window in bpy.context.window_manager.windows for op in window.modal_operators):
            return .1
    _region_preview_pending.pop(key)
    try:
        node = tree.nodes.get(name)
        if node is not None and _preview_key(node, parents) == key:
            current_signature = _region_signature(node, parents)
            if not bpy.app.background and current_signature != signature:
                _region_preview_pending[key] = (tree, name, parents, current_signature, time.monotonic())
                return _BRUSH_PREVIEW_IDLE_DELAY
            region_preview(node, node.preview_length, 192, 64, parents)
            _region_preview_errors.pop(key, None)
            while len(_region_preview_cache) > 64:
                del _region_preview_cache[next(iter(_region_preview_cache))]
    except ReferenceError:
        # Undo and node/group deletion can invalidate a queued RNA handle before
        # its timer runs. Discard that request; a live node queues its own preview.
        _region_preview_errors.pop(key, None)
        _region_preview_cache.pop(key, None)
    except (RuntimeError, ValueError) as error:
        _region_preview_errors[key] = str(error)
        _region_preview_cache[key] = (signature, None)
        print('Brush Output preview: ' + str(error))
    _redraw_brush_nodes()
    return .15 if _region_preview_pending else None


def _socket_value(socket, seen=None, parents=()):
    seen = set() if seen is None else seen
    key = (socket.as_pointer(), tuple(n.as_pointer() for n in parents))
    if key in seen:
        return None
    seen.add(key)
    if not socket.is_output and socket.is_linked:
        return _socket_value(socket.links[0].from_socket, seen, parents)
    node = socket.node
    if node.bl_idname == 'NodeReroute':
        return _socket_value(node.inputs[0], seen, parents)
    if socket.is_output and node.bl_idname == 'NPRLineNodeGroup' and node.node_tree:
        if node in parents:
            return None
        output = next((item for item in node.node_tree.nodes if item.bl_idname == 'NodeGroupOutput'
                       and item.is_active_output), None) or next(
                       (item for item in node.node_tree.nodes if item.bl_idname == 'NodeGroupOutput'), None)
        target = next((item for item in output.inputs if item.identifier == socket.identifier), None) if output else None
        return _socket_value(target, seen, parents + (node,)) if target else None
    if socket.is_output and node.bl_idname == 'NodeGroupInput':
        if parents and parents[-1].node_tree == node.id_data:
            target = next((item for item in parents[-1].inputs if item.identifier == socket.identifier), None)
            return _socket_value(target, seen, parents[:-1]) if target else None
        space = bpy.context.space_data
        if space and space.type == 'NODE_EDITOR':
            path = tuple(space.path)
            for index, entry in enumerate(path):
                if index and entry.node_tree == node.id_data:
                    parent_tree = path[index - 1].node_tree
                    parent = parent_tree.nodes.active
                    if parent and parent.bl_idname == 'NPRLineNodeGroup' and parent.node_tree == node.id_data:
                        target = next((item for item in parent.inputs if item.identifier == socket.identifier), None)
                        return _socket_value(target, seen) if target else None
        interface = next((item for item in node.id_data.interface.items_tree
                          if item.item_type == 'SOCKET' and item.in_out == 'INPUT'
                          and item.identifier == socket.identifier), None)
        return getattr(interface, 'default_value', None)
    if socket.is_output and node.bl_idname == 'NPRLineNodeMap':
        if node.mute or not getattr(node, 'is_enabled', True) or not getattr(node, 'enabled', True):
            return None
        result = {'source': node.mode, 'wrap_u': node.wrap_u, 'wrap_v': node.wrap_v, 'filter': node.filter_mode}
        for item in node.inputs:
            value = _socket_value(item, seen.copy(), parents)
            if value is not None:
                result[item.identifier] = value
        return result
    return getattr(socket, 'default_value', None)


def _noise(seed):
    seed &= 0xffffffff
    seed ^= seed >> 16
    seed = (seed * 0x7feb352d) & 0xffffffff
    seed ^= seed >> 15
    seed = (seed * 0x846ca68b) & 0xffffffff
    seed ^= seed >> 16
    return (seed >> 8) / 16777216.0


def _noise_array(value):
    import numpy as np
    value = np.asarray(value, dtype=np.uint32)
    value = (value ^ (value >> 16)) * np.uint32(0x7feb352d)
    value = (value ^ (value >> 15)) * np.uint32(0x846ca68b)
    return ((value ^ (value >> 16)) >> 8).astype(np.float64) / 16777216.0


def _wave_noise(x, y, cell_size, seed):
    result = 0
    for side in (-.25, .25):
        px, py = x / max(cell_size, .001) + side, y / max(cell_size, .001) + side
        ix, iy = math.floor(px), math.floor(py)
        tx, ty = px - ix, py - iy
        key = seed + ix * 73856093 + iy * 19349663
        a = _noise(key) * (1 - tx) + _noise(key + 73856093) * tx
        b = _noise(key + 19349663) * (1 - tx) + _noise(key + 73856093 + 19349663) * tx
        result += (a * (1 - ty) + b * ty) * 2 - 1
    return result * .5


def _pixel_wave(arc, x, y, values, seed):
    length = max(5, float(values.get('wave_length', 100)))
    phase = arc * 6.2831854820251465 / length + float(values.get('wave_phase', 0))
    phase += (2 * _noise(seed + 91) - 1) * float(values.get('wave_phase_random', 0)) * 6.2831854820251465
    if values.get('wave_period_random', 0):
        phase += _wave_noise(x, y, length * .5, seed + 7777) * float(values['wave_period_random']) * 3.1415927410125732
    amplitude = float(values.get('wave', 0))
    if values.get('wave_random', 0):
        amplitude *= 1 + _wave_noise(x, y, length * .5, seed + 77) * float(values['wave_random'])
    return math.sin(phase) * amplitude


def _envelope(arc, length, start, end):
    return ((min(1.0, arc / max(length * start, 1e-6)) if start else 1.0) *
            (min(1.0, (length - arc) / max(length * end, 1e-6)) if end else 1.0))


def _curve_signature(node, channel):
    if not getattr(node, 'use_' + channel + '_curve', False):
        return None
    return _mapping_signature(getattr(node, channel + '_curve'))


def _mapping_signature(mapping):
    if mapping is None:
        return None
    return (mapping.extend, mapping.use_clip, mapping.clip_min_x, mapping.clip_max_x,
            mapping.clip_min_y, mapping.clip_max_y,
            tuple((p.location[0], p.location[1], p.handle_type) for p in mapping.curves[0].points))


def _curve_table(node, channel):
    if not getattr(node, 'use_' + channel + '_curve', False):
        return None
    mapping = getattr(node, channel + '_curve')
    mapping.initialize()
    return [max(0.0, min(1.0, mapping.evaluate(mapping.curves[0], i / 255))) for i in range(256)]


def _table_envelope(table, arc, length, start, end, raster=False):
    if table is None:
        return _envelope(arc, length, start, end)
    t = min(255, max(0.0, min(1.0, arc / max(length, 1e-8))) * (256 if raster else 255))
    a = int(t)
    return table[a] + (table[min(a + 1, 255)] - table[a]) * (t - a)


def _raster_size(radius, textured=False):
    width = radius * 2
    if textured:
        domain = width if width < 2 else width * .5 + 1
        return max(1, domain), domain * domain if domain < 1 else 1
    physical = 1 if width < 1 else 2 * width - 1 if width < 2 else width + 1
    return physical * .5, width * width if width < 1 else 1


def _scale_width(radius, factor, raster):
    if not raster:
        return radius * factor
    width = radius * 2
    domain = (width if width < 2 else width * .5 + 1) * factor
    return (domain if domain < 2 else domain * 2 - 2) * .5


_tip_random_values = None


def _tip_random_table():
    import numpy as np
    global _tip_random_values
    if _tip_random_values is None:
        state = 0
        values = []
        for _ in range(65536):
            state = (state * 2061 + 7) & 65535
            values.append(state)
        _tip_random_values = np.asarray(values, dtype=np.int64)
    return _tip_random_values


def _tip_spatial_noise(a, b, cell, state=0, suffix=0):
    import numpy as np
    table = _tip_random_table()
    total = np.full(np.shape(a[0]), -1, dtype=np.float32)
    inverse = np.float32(1 / max(.001, cell))
    for (x, y), extra in ((a, 0), (b, 16384)):
        x, y = np.float32(x * inverse), np.float32(y * inverse)
        ix = np.trunc(x).astype(np.int64) - (x < 0)
        iy = np.trunc(y).astype(np.int64) - (y < 0)
        tx, ty = np.float32(x - ix), np.float32(y - iy)
        for dx, dy, weight in ((0, 0, (1-tx)*(1-ty)), (1, 0, (1-ty)*tx),
                               (0, 1, (1-tx)*ty), (1, 1, ty*tx)):
            gx, gy = ix + dx, iy + dy + 32768
            hx, hy = gx >> 16, gy >> 16
            cx = np.abs(hx) * np.where(hx > 0, 255, 13)
            cy = np.abs(hy) * np.where(hy > 0, 19, 127)
            rx, ry = table[gx & 65535], table[gy & 65535]
            index = ((ry + 1) * rx + ry + cx + cy + extra) & 65535
            random = ((index + state + suffix) ^ suffix) & 65535
            for _ in range(4):random = (random * 2061 + 7) & 65535
            total = np.float32(total + np.float32(random) * np.float32(1/65536) * weight)
    signed = np.where(total <= 0, (total + 1)*(total + 1) - 1, 1 - (1 - total)*(1 - total))
    return np.float32((signed + 1) * .5)


def _tip_mask(values, multiple, resolution=64, phase=0):
    """Canonical preview uses phase zero; rendering selects a mask per stroke."""
    import numpy as np
    def number(name, default):
        value = float(values.get(name, default))
        return value if math.isfinite(value) else default
    clamp = lambda value: max(0.0, min(1.0, value))
    table = _tip_random_table()
    radius = min(100, max(.1, number('dot_size', 16))) * .02
    space_x, space_y = clamp(number('dot_space_x', .1)), clamp(number('dot_space_y', .1))
    step_x, step_y = 2 * (radius + space_x), 2 * (radius + space_y)
    random_x, random_y = clamp(number('dot_space_random_x', 1)), clamp(number('dot_space_random_y', 1))
    size_random = clamp(number('dot_size_random', .8))
    texel = 2 / resolution
    aa = max(0, min(10, number('dot_antialiasing', .5)))
    inner = .9 * (1 - aa)
    if inner < 0:
        inner /= 1 - inner * .5
    core = math.copysign(inner * inner, inner)
    outer = 1 + .75 * aa
    end = clamp(number('tip_reduction_end', 1))
    start = clamp(number('tip_reduction_start', 1))
    groove, count = clamp(number('groove', 0)), min(20, max(3, int(number('groove_count', 5))))
    softness = clamp(number('softness', 0))
    yy, xx = np.mgrid[:resolution, :resolution]
    px, py = np.float32((xx + .5) * texel - 1), np.float32((yy + .5) * texel - 1)
    if phase > 0 and number('angle_random', 0) != 0:
        random = (int(table[(phase+0x309) & 65535])*2061 + 7) & 65535
        degrees = np.float32(np.float32(random/32768 - 1)*np.float32(number('angle_random', 0))*.5)
        angle = np.float32(degrees*np.float32(math.pi/180))
        c, s = np.float32(math.cos(float(angle))), np.float32(math.sin(float(angle)))
        px, py = np.float32(c*px-s*py), np.float32(s*px+c*py)
    distance2 = np.float32(px*px + py*py)
    sx, sy = px, py
    if groove:
        length = np.sqrt(distance2)
        nx = np.where(length > 0, px/np.maximum(length, 1e-30), 1)
        ny = np.where(length > 0, py/np.maximum(length, 1e-30), 0)
        def direction_noise(cell, suffix):
            shift = np.float32(cell*.25)
            return _tip_spatial_noise((nx-shift, ny-shift), (nx+shift, ny+shift), cell, suffix=suffix)
        theta = np.float32(np.arctan2(px, py) + np.float32(math.pi))
        angular = np.float32(np.float32(count) * np.float32(direction_noise(.125, 0)*.25 + theta) * np.float32(1/math.tau))
        wave = np.abs(np.float32(angular - np.trunc(angular))*2 - 1)
        amount = np.float32(np.float32(direction_noise(.05, 100)*.5 + 1.25) * wave*wave*wave*np.float32(groove))
        distance2 = np.float32(distance2*np.float32(1 + amount*1.25))
        sx, sy = np.float32(px*np.float32(1 - amount*.25)), np.float32(py*np.float32(1 - amount*.25))
    a, b = np.float32(start*start), np.float32(end*end)
    fade = (distance2 <= b).astype(np.float32) if a == b else np.clip((b-distance2)/(b-a), 0, 1)
    alpha = np.float32(.5 - (fade+.5)*(fade-.5)*(fade-1.5)*np.float32(4/3))
    alpha = np.where(distance2 < 1, alpha, 0)
    if softness:
        alpha *= np.clip((1 - np.sqrt(distance2)) / softness + .5, 0, 1)
    if multiple:
        if phase > 0:
            ry = (int(table[(phase+0x24d83) & 65535])*2061 + 7) & 65535
            rx = (ry*2061 + 7) & 65535
            sx = np.float32(sx + np.float32((rx/32768 - 1)*256))
            sy = np.float32(sy + np.float32((ry/32768 - 1)*256))
        dots = np.zeros_like(px)
        ix, iy = np.floor(sx / step_x + .5).astype(int), np.floor(sy / step_y + .5).astype(int)
        span_x = math.ceil(radius * (outer + size_random) / step_x + random_x * .5)
        span_y = math.ceil(radius * (outer + size_random) / step_y + random_y * .5)
        for dy in range(-span_y, span_y + 1):
            for dx in range(-span_x, span_x + 1):
                cx, cy = ix + dx, iy + dy
                n = (table[(phase+0x3d8545+(cx ^ cy)) & 65535] + table[(cx+0xb40) & 65535] + table[(cy+0x438) & 65535]) & 65535
                r1 = (n*2061 + 7) & 65535
                r2 = (r1*2061 + 7) & 65535
                r3 = (r2*2061 + 7) & 65535
                center_x = (cx + (r1/65536 - .5)*random_x)*step_x
                center_y = (cy + (r2/65536 - .5)*random_y)*step_y
                variation = (r3/32768 - 1)*size_random
                r2 = (radius * (outer + variation)) ** 2
                q = ((sx - center_x) ** 2 + (sy - center_y) ** 2) / np.maximum(r2, 1e-30)
                b = core * (1 + variation)
                t = np.sqrt(np.clip((q - b) / np.maximum(1 - b, 1e-30), 0, 1))
                coverage = np.where(q <= b, 1, .5 + 1.3333333730697632 * (t + .5) * (t - .5) * (t - 1.5))
                dots += np.where((q < 1) & (r2 > 0), coverage, 0)
        alpha *= np.minimum(dots, 1)
    return resolution, (np.floor(np.clip(alpha, 0, 1)*255 + .5)/255).reshape(-1).tolist()


def _sample_preview_map(texture, settings, u, v):
    """The shared map's transform and explicit texel wrapping, using image float pixels."""
    import numpy as np
    u = u * settings.get('tiling_u', 1) + settings.get('offset_u', 0)
    v = v * settings.get('tiling_v', 1) + settings.get('offset_v', 0)
    angle = math.radians(settings.get('rotation', 0))
    c, s = math.cos(angle), math.sin(angle)
    u, v = c * (u - .5) - s * (v - .5) + .5, s * (u - .5) + c * (v - .5) + .5
    u, v = np.asarray(u), np.asarray(v)
    valid = np.isfinite(u) & np.isfinite(v)
    for axis, mode in ((0, settings['wrap_u']), (1, settings['wrap_v'])):
        p = u if axis == 0 else v
        if mode == 'CLIP':valid &= (p >= 0) & (p <= 1)
        if mode == 'REPEAT':p = p % 1
        elif mode == 'MIRROR':p = 1 - np.abs(p % 2 - 1)
        elif mode == 'MIRROR_ONCE':p = np.clip(np.abs(p), 0, 1)
        else:p = np.clip(p, 0, 1)
        if axis == 0:u = p
        else:v = p
    height, width = texture.shape[:2]

    def wrap(p, size, mode):
        if mode == 'REPEAT':return p % size
        if mode == 'MIRROR':
            value = p % (size * 2)
            return np.where(value < size, value, size * 2 - 1 - value)
        return np.clip(p, 0, size - 1)

    def fetch(x, y):
        inside = np.ones_like(valid)
        if settings['wrap_u'] == 'CLIP':inside &= (x >= 0) & (x < width)
        if settings['wrap_v'] == 'CLIP':inside &= (y >= 0) & (y < height)
        values = texture[wrap(y, height, settings['wrap_v']), wrap(x, width, settings['wrap_u'])]
        return np.where(inside[..., None], values, 0)

    x, y = np.where(valid, u, 0) * width, np.where(valid, v, 0) * height
    if settings['filter'] == 'POINT':
        result = fetch(np.floor(x).astype(int), np.floor(y).astype(int))
    else:
        x, y = x - .5, y - .5
        ix, iy = np.floor(x).astype(int), np.floor(y).astype(int)
        tx, ty = (x - ix)[..., None], (y - iy)[..., None]
        result = (fetch(ix, iy) * (1 - tx) + fetch(ix + 1, iy) * tx) * (1 - ty) + (
                  fetch(ix, iy + 1) * (1 - tx) + fetch(ix + 1, iy + 1) * tx) * ty
    return np.where(valid[..., None], result, 0)


def _preview_value_key(value):
    if isinstance(value, bpy.types.Image):
        return (value.as_pointer(), value.update_revision, tuple(value.size),
                value.colorspace_settings.name, value.alpha_mode)
    if isinstance(value, bpy.types.ID):
        return value.as_pointer()
    if isinstance(value, dict):
        return tuple((key, _preview_value_key(item)) for key, item in sorted(value.items()))
    if isinstance(value, (str, int, float, bool, type(None))):
        return value
    return tuple(value)


def brush_preview(node, width=384, height=128):
    """Rasterize a bounded local sample using the brush's coverage and noise rules.

    No evaluated meshes, render engine, image data-blocks or scene updates are needed.
    The sample path and its seed are fixed, so style changes are easy to compare.
    """
    global _brush_previews, _brush_preview_cache
    values = {}
    for socket in node.inputs:
        value = _socket_value(socket)
        if value is not None:
            values[socket.identifier] = tuple(value) if socket.type == 'RGBA' else value
    image = values.get('image') if node.mode != 'SIMPLE' else None
    brush_map = values.get('brush_map') if node.mode != 'SIMPLE' else None
    map_image = brush_map.get('image') if brush_map and brush_map.get('source') != 'ATTRIBUTE' else None
    preview_images = [item for item in (image, map_image) if item]
    key = (node.as_pointer(), node.mode, tuple((k, _preview_value_key(v))
                                             for k, v in values.items()),
           tuple(tuple(item.size) for item in preview_images),
           _curve_signature(node, 'width'), _curve_signature(node, 'opacity'),
           getattr(node, 'raster_size_response', False), getattr(node, 'stroke_mode', 'RAKE'),
           getattr(node, 'wave_units', 'STROKE'))
    cache_name = f'{node.as_pointer()}_{width}_{height}'
    cached = _brush_preview_cache.get(cache_name)
    if _brush_previews is not None and cached and key == cached[0] and (
            not preview_images or time.monotonic() - cached[1] < .25):
        return _brush_previews[cache_name]
    if _brush_previews is None:
        _brush_previews = previews.new()
    if cache_name not in _brush_previews:
        if len(_brush_previews) >= 64:
            oldest = next(iter(_brush_previews))
            del _brush_previews[oldest]
            _brush_preview_cache.pop(oldest, None)
        _brush_previews.new(cache_name)
    if _clear_brush_preview_on_load not in bpy.app.handlers.load_pre:
        bpy.app.handlers.load_pre.append(_clear_brush_preview_on_load)
    import numpy as np
    yy, xx = np.mgrid[:height, :width]
    gray = np.where((xx // 12 + yy // 12) % 2, .90, .98)
    pixels = np.ones((height, width, 4), dtype=np.float32)
    pixels[:, :, :3] = gray[:, :, None]
    def number(name, fallback=0.0):
        value = float(values.get(name, fallback))
        return value if math.isfinite(value) else fallback
    width_table, opacity_table = _curve_table(node, 'width'), _curve_table(node, 'opacity')
    raster = getattr(node, 'raster_size_response', False)
    width_envelope = lambda arc: _table_envelope(width_table, arc, length, number('taper_start'), number('taper_end'), raster)
    color = values.get('color', (0, 0, 0, 1))
    rgb = tuple(12.92 * c if c < 0.0031308 else 1.055 * max(c, 0.0) ** (1 / 2.4) - 0.055
                for c in color[:3])
    opacity = number('opacity', 1) * color[3]
    seed = int(number('seed'))
    # Very large brushes are fitted to this small swatch; scene widths remain unchanged.
    brush_width = min(48.0, max(0.001, number('width', 2)))
    radius = brush_width * 0.5 * (1 - number('width_random') * _noise(seed))
    copies = min(32, max(1, int(number('copies', 1))))
    multiple = node.mode == 'MULTIPLE'
    simple = node.mode == 'SIMPLE'
    stroke_mode = getattr(node, 'stroke_mode', 'RAKE')
    coverage_alpha = np.zeros((height, width), dtype=np.float64)
    stretch = min(100.0, max(0.01, number('stretch', 1))) if not simple else 1.0
    fit = min(1.0, 24.0 / max(radius * max(1.0, stretch), 1e-5))
    radius *= fit
    brush_width *= fit
    tip_mask = (multiple or number('groove') or number('tip_reduction_start', 1) < 1 or
                number('tip_reduction_end', 1) < 1) and not simple
    if tip_mask:
        tip_cache = {}
        def sample_tip(ux, uy, stamp_radius, phase):
            if not multiple and number('angle_random') == 0:phase = 0
            if phase not in tip_cache:
                tip_size, mask_values = _tip_mask(values, multiple, phase=phase)
                tip_levels = [np.asarray(mask_values).reshape(tip_size, tip_size)]
                while tip_levels[-1].shape[0] > 1:
                    previous = tip_levels[-1]
                    tip_levels.append(previous.reshape(previous.shape[0] // 2, 2,
                                                        previous.shape[1] // 2, 2).mean(axis=(1, 3)))
                tip_cache[phase] = tip_levels
            tip_levels = tip_cache[phase]
            tip_size = tip_levels[0].shape[0]
            lod = max(0.0, min(len(tip_levels) - 1.0,
                              math.log2(tip_size / max(2 * stamp_radius * min(1.0, stretch), 1e-5))))
            if raster:lod = 0.0
            first = int(lod)

            def bilinear(level):
                size = level.shape[0]
                px, py = ux * size - .5, uy * size - .5
                ix, iy = np.floor(px).astype(int), np.floor(py).astype(int)
                fx, fy = px - ix, py - iy
                x0, x1 = np.clip(ix, 0, size - 1), np.clip(ix + 1, 0, size - 1)
                y0, y1 = np.clip(iy, 0, size - 1), np.clip(iy + 1, 0, size - 1)
                return ((level[y0, x0] * (1 - fx) + level[y0, x1] * fx) * (1 - fy) +
                        (level[y1, x0] * (1 - fx) + level[y1, x1] * fx) * fy)

            a = bilinear(tip_levels[first])
            return a if first == len(tip_levels) - 1 else a + (bilinear(tip_levels[first + 1]) - a) * (lod - first)
    length = width - 64.0
    spacing = 0.9 if simple else (max(0.35, brush_width * number('spacing')) if number('spacing') > 0 else 0)
    footprint = max(4, (radius * 2 + 2) ** 2 * max(1.0, stretch))
    budget = max(2, int(min(4096, 500000 / footprint) / copies))
    count = max(2, math.ceil(length / .9) if simple else int(np.float32(length)*2) + 1)
    stride = 1 if simple else max(1, math.ceil(spacing / (length / count)))
    if simple:
        count = min(count, budget)
    else:
        stride = max(stride, math.ceil(count / budget))
    texture = None
    map_pixels = None
    if map_image and map_image.has_data and map_image.size[0] and map_image.size[1]:
        iw, ih = map_image.size[:]
        channels = max(1, map_image.channels)
        source = np.empty(iw * ih * channels, dtype=np.float32)
        map_image.pixels.foreach_get(source)
        source = source.reshape(ih, iw, channels)
        map_pixels = np.repeat(source[:, :, :1], 3, axis=2) if channels < 3 else source[:, :, :3]
    if image and image.size[0] and image.size[1] and image.has_data:
        iw, ih = image.size
        channels = max(1, len(image.pixels) // (iw * ih))
        source_pixels = np.empty(len(image.pixels), dtype=np.float32)
        image.pixels.foreach_get(source_pixels)
        alpha = source_pixels.reshape(ih, iw, channels)[:, :, 3] if channels == 4 else np.ones((ih, iw))
        texture = alpha[np.ix_(np.minimum(ih - 1, np.arange(64) * ih // 64),
                               np.minimum(iw - 1, np.arange(64) * iw // 64))]

    def opacity_envelope(arc):
        if opacity_table is not None:
            return np.interp(np.clip(arc / max(length, 1e-8) * (256 / 255 if raster else 1), 0, 1),
                             np.linspace(0, 1, 256), opacity_table)
        return (np.minimum(1, arc / max(length * number('alpha_start'), 1e-6)) if number('alpha_start') else 1) * (
            np.minimum(1, (length - arc) / max(length * number('alpha_end'), 1e-6)) if number('alpha_end') else 1)

    def point(t, copy):
        arc = length * t
        x = 32 + arc
        y = height * .5 + height * (13 / 128) * math.sin(t * math.tau)
        dy = height * (13 / 128) * math.tau * math.cos(t * math.tau)
        scale = math.hypot(length, dy)
        offset = number('offset') + (2 * _noise(seed + copy * 27 + 131) - 1) * number('offset_random')
        if values.get('distortion_enabled', True):
            if getattr(node, 'wave_units', 'STROKE') == 'PIXELS':
                offset += _pixel_wave(arc, x, y, values, seed + copy * 27)
            else:
                offset += math.sin(t * math.tau * number('cycles', 3)) * number('wave')
                offset += (2 * _noise(seed + copy * 27 + int(arc / 8)) - 1) * number('noise')
        x += -dy / max(scale, 1e-8) * offset + number('shift_x')
        y += length / max(scale, 1e-8) * offset + number('shift_y')
        if t <= 0:
            x -= number('extend')
        elif t >= 1:
            x += number('extend')
        return x, y

    phase_ids = []
    phase_state = (seed + 127) & 65535
    for _ in range(10):
        phase_state = (phase_state*2061 + 7) & 65535
        phase_ids.append(phase_state)
    random_table = _tip_random_table()
    copy_state = seed & 65535
    for copy in range(copies):
        copy_state = (copy_state*2061 + 7) & 65535
        noise_state = copy_state
        for _ in range(9):noise_state = (noise_state*2061 + 7) & 65535
        start, end = (np.float32(32), np.float32(height*.5)), (np.float32(width-32), np.float32(height*.5))
        phase_index = min(9, max(0, int(_tip_spatial_noise(start, end, 8, noise_state, 0x226d0)*10)))
        angle_offset = (2*float(_tip_spatial_noise(start, end, 8, noise_state, 0xc0ece))-1)*number('angle_random')*.5 if number('angle_random') else 0
        sample_grid = range(0, count, stride)
        if not simple:sample_grid = (*sample_grid, count)
        for sample in sample_grid:
            t = sample / count
            arc = length * t
            ax, ay = point(t, copy)
            bx, by = point(min(1, t + 1 / count), copy) if simple else (ax, ay)
            stamp_seed = seed + int(arc) * 7
            r = radius if simple and raster else _scale_width(radius, width_envelope(arc), raster)
            next_r = _scale_width(radius, width_envelope(min(length, arc + length / count)), raster) if simple and not raster else r
            sample_step = length / count
            if simple and raster and values.get('dashed', False):
                period = max(.001, number('dash_length', 8) + number('dash_space', 5))
                phase = number('dash_phase')
                dash_start = math.floor((arc + phase) / period) * period - phase
                if dash_start + number('dash_length', 8) < arc:
                    dash_start += period
                first = max(arc, dash_start)
                last = min(arc + sample_step, dash_start + number('dash_length', 8))
                if first > last:
                    continue
                u0, u1 = (first - arc) / sample_step, (last - arc) / sample_step
                vx, vy = bx - ax, by - ay
                bx, by = ax + vx * u1, ay + vy * u1
                ax, ay = ax + vx * u0, ay + vy * u0
                arc, sample_step = first, last - first
            cx, cy = ax, ay
            angle = math.atan2(height * (13 / 128) * math.tau * math.cos(t * math.tau), length)
            if stroke_mode != 'RAKE':angle = 0
            angle += math.radians(number('angle') + angle_offset)
            layer = phase_index
            if not simple and stroke_mode == 'RANDOM':
                y = height - ay
                random = int(random_table[((int(y)*2697+int(ax))*3) & 65535])
                for _ in range(int(ax-y) & 3):random = (random*2061 + 7) & 65535
                random = (random*2061 + 7) & 65535
                layer = min(9, int(random*10/65536))
                random = (random*2061 + 7) & 65535
                angle -= (random/32768-1)*math.pi
            if not simple and r < .05:
                continue
            cos_a, sin_a = math.cos(angle), math.sin(angle)
            bound_r = _raster_size(max(r, next_r), not simple)[0] if raster else max(r, next_r)
            bound_x = bound_r + 1 if simple else bound_r * math.hypot(stretch * cos_a, sin_a) + 1
            bound_y = bound_r + 1 if simple else bound_r * math.hypot(stretch * sin_a, cos_a) + 1
            left, right = max(0, math.floor(min(cx, bx) - bound_x)), min(width, math.ceil(max(cx, bx) + bound_x))
            bottom, top = max(0, math.floor(min(cy, by) - bound_y)), min(height, math.ceil(max(cy, by) + bound_y))
            if left >= right or bottom >= top:
                continue
            sy, sx = np.mgrid[bottom:top, left:right]
            dx, dy = sx + .5 - cx, sy + .5 - cy
            sample_arc = dash_arc = arc
            if simple:
                vx, vy = bx - ax, by - ay
                raw_u = (dx * vx + dy * vy) / max(vx * vx + vy * vy, 1e-8)
                u = np.clip(raw_u, 0, 1)
                distance = np.hypot(dx - vx * u, dy - vy * u)
                local_radius = r + (next_r - r) * u
                if raster:
                    pressure_arc = arc + sample_step * raw_u
                    if width_table is not None:
                        pressure = np.interp(np.clip(pressure_arc / max(length, 1e-8) * 256 / 255, 0, 1),
                                             np.linspace(0, 1, 256), width_table)
                    else:
                        pressure = (np.clip(pressure_arc / max(length * number('taper_start'), 1e-6), 0, 1) if number('taper_start') else 1) * (
                            np.clip((length - pressure_arc) / max(length * number('taper_end'), 1e-6), 0, 1) if number('taper_end') else 1)
                    line_width = radius * 2
                    domain = (line_width if line_width < 2 else line_width * .5 + 1) * pressure
                    physical_radius = np.where(domain < 1, 1, 2 * domain - 1) * .5
                    size_alpha = np.where(domain < 1, domain * domain, 1)
                else:
                    physical_radius, size_alpha = local_radius, 1
                coverage = np.clip(physical_radius - distance + .5, 0, 1) * size_alpha
                sample_arc += sample_step * u
                dash_arc += sample_step * raw_u
                local_x, local_y = u, distance / np.maximum(local_radius, 1e-5)
            else:
                physical_r, size_alpha = _raster_size(r, True) if raster else (r, 1)
                local_x = (cos_a * dx + sin_a * dy) / max(physical_r * stretch, 1e-5)
                local_y = (-sin_a * dx + cos_a * dy) / max(physical_r, 1e-5)
                coverage = np.clip((1 - np.hypot(local_x, local_y)) /
                                   max(1 / max(physical_r, .1), number('softness')) + .5, 0, 1)
                ux, uy = (local_x + 1) * .5, (local_y + 1) * .5
                inside = (ux >= 0) & (ux <= 1) & (uy >= 0) & (uy <= 1)
                if tip_mask:
                    coverage = np.where(inside, sample_tip(ux, uy, physical_r, phase_ids[layer]), 0)
                    ellipse_distance = np.hypot(local_x, local_y) * physical_r
                    edge_distance = physical_r - ellipse_distance
                    if stretch != 1:
                        edge_distance *= np.hypot(dx, dy) / np.maximum(ellipse_distance, 1e-20)
                    coverage *= np.clip(edge_distance, 0, 1)
                if texture is not None:
                    mapped = np.where(inside, texture[np.clip((uy * 64).astype(int), 0, 63),
                                                      np.clip((ux * 64).astype(int), 0, 63)], 0)
                    coverage = coverage * mapped if tip_mask else mapped
                if map_pixels is not None:
                    sampled = _sample_preview_map(map_pixels, brush_map, ux, uy)
                    mask = np.clip(sampled.mean(axis=2), 0, 1)
                    amount = np.clip(number('brush_map_opacity', 1), 0, 1)
                    coverage *= 1 - amount + amount * mask
                coverage *= size_alpha
            if values.get('dashed', False) and not (simple and raster):
                coverage = np.where((dash_arc + number('dash_phase')) % max(.001, number('dash_length', 8) +
                                     number('dash_space', 5)) < number('dash_length', 8), coverage, 0)
            alpha = coverage * opacity * opacity_envelope(sample_arc)
            if number('grain'):
                grain_seed = (np.uint32(stamp_seed & 0xffffffff) + np.maximum(0, np.floor((local_x + 2) * 67)).astype(np.uint32) +
                              np.maximum(0, np.floor((local_y + 2) * 71)).astype(np.uint32) * np.uint32(131))
                alpha *= 1 - number('grain') * _noise_array(grain_seed)
            target = coverage_alpha[bottom:top, left:right]
            np.maximum(target, np.clip(alpha, 0, 1), out=target)
    # Accumulate maximum coverage within a style, then apply color once.
    pixels[:, :, :3] = pixels[:, :, :3] * (1 - coverage_alpha[:, :, None]) + np.asarray(rgb) * coverage_alpha[:, :, None]
    preview = _brush_previews[cache_name]
    preview.image_size = (width, height)
    preview.image_pixels_float = pixels.reshape(-1).tolist()
    _brush_preview_cache[cache_name] = (key, time.monotonic())
    return preview


def clear_brush_preview():
    global _brush_previews, _brush_preview_cache
    if _brush_previews is not None:
        previews.remove(_brush_previews)
        _brush_previews = None
    _brush_preview_cache = {}
    _region_preview_cache.clear()
    _region_preview_pending.clear()
    _region_preview_errors.clear()
    if bpy.app.timers.is_registered(_update_output_brush_previews):
        bpy.app.timers.unregister(_update_output_brush_previews)


@bpy.app.handlers.persistent
def _clear_brush_preview_on_load(_):
    clear_brush_preview()


def input_socket(node, identifier):
    return next(socket for socket in node.inputs if socket.identifier == identifier)


def _copy_brush_settings(source, target, skip=()):
    """Shared by single-user copies and conversion of the experimental Brush chain."""
    for name in ('mode', 'raster_size_response', 'stroke_mode', 'wave_units',
                 'override_stroke_connection', 'use_width_curve', 'use_opacity_curve'):
        setattr(target, name, getattr(source, name))
    for name in ('width_curve', 'opacity_curve'):
        old, new = getattr(source, name), getattr(target, name)
        for prop in ('use_clip', 'clip_min_x', 'clip_max_x', 'clip_min_y', 'clip_max_y', 'extend'):
            setattr(new, prop, getattr(old, prop))
        old_points, points = old.curves[0].points, new.curves[0].points
        while len(points) > 2:
            points.remove(points[1])
        points[0].location, points[-1].location = old_points[0].location[:], old_points[-1].location[:]
        for point in list(old_points)[1:-1]:
            points.new(*point.location)
        for a, b in zip(old_points, points):
            b.handle_type = a.handle_type
        new.update()
    for socket in source.inputs:
        if socket.identifier in skip:
            continue
        copy = next((s for s in target.inputs if s.identifier == socket.identifier), None)
        if copy is not None:
            if hasattr(socket, 'default_value'):
                value = socket.default_value
                copy.default_value = value[:] if hasattr(value, '__len__') and not isinstance(value, str) else value
            if socket.is_linked:
                target.id_data.links.new(socket.links[0].from_socket, copy)


def migrate_brush_fields(tree):
    """Convert the former local Brush chain to ordinary vector fields, in memory."""
    legacy = {'NPRLineNodeBrushInput', 'NPRLineNodeSetPosition', 'NPRLineNodeResample'}
    if not any(n.bl_idname in legacy for n in tree.nodes):
        return 0
    positions = {}
    roots = {}
    intervals = {}
    zero = None

    def original():
        nonlocal zero
        if zero is None:
            info = tree.nodes.new('NPRLineNodeStrokeInfo')
            info.location = (-1000, 300)
            zero = info.outputs['Position']
        return zero

    def source(socket):
        seen = set()
        while socket.is_linked:
            socket = socket.links[0].from_socket
            node = socket.node
            if node.as_pointer() in seen:
                raise ValueError('Cyclic legacy Brush chain')
            seen.add(node.as_pointer())
            if node.bl_idname != 'NodeReroute':
                return node
            socket = node.inputs[0]
        return None

    # Validate the complete convertible chain before changing any sockets. Cross-group
    # legacy Brush routing keeps its original evaluator instead of a partial conversion.
    checked = set()
    visiting = set()
    def validate(node):
        if node is None or node.as_pointer() in checked:
            return
        key = node.as_pointer()
        if key in visiting:
            raise ValueError('Cyclic legacy Brush chain')
        visiting.add(key)
        if node.bl_idname in {'NPRLineNodeSetPosition', 'NPRLineNodeResample'}:
            validate(source(input_socket(node, 'brush')))
        elif node.bl_idname not in {'NPRLineNodeBrushInput', 'NPRLineNodeBrush'}:
            raise ValueError('Legacy Brush routing through a group keeps its original evaluator')
        visiting.remove(key)
        checked.add(key)

    for node in tree.nodes:
        if (node.bl_idname in {'NPRLineNodeBrushOutput', 'NPRLineNodeStrokeInfo'} and
                not node.use_field_inputs):
            validate(source(input_socket(node, 'brush')))

    def connect(value, target):
        if isinstance(value, bpy.types.NodeSocket):
            tree.links.new(value, target)
        else:
            target.default_value = value

    def value(socket):
        return socket.links[0].from_socket if socket.is_linked else socket.default_value[:]

    def vector(operation, a, b, location):
        node = tree.nodes.new('ShaderNodeVectorMath')
        node.operation = operation
        node.location = location
        connect(a, node.inputs[0])
        connect(b, node.inputs[1])
        return node.outputs['Vector']

    def position(node):
        key = node.as_pointer()
        if key in positions:
            if positions[key] is None:
                raise ValueError('Cyclic legacy position field')
            return positions[key]
        positions[key] = None
        if node.bl_idname in {'NPRLineNodeBrushInput', 'NPRLineNodeBrush'}:
            result = original()
            roots[key], intervals[key] = node, .9
        elif node.bl_idname in {'NPRLineNodeSetPosition', 'NPRLineNodeResample'}:
            incoming = source(input_socket(node, 'brush'))
            if incoming is None:
                raise ValueError('Legacy Brush chain has no source')
            before = position(incoming)
            roots[key], intervals[key] = roots[incoming.as_pointer()], intervals[incoming.as_pointer()]
            result = before
            if not node.mute and node.bl_idname == 'NPRLineNodeResample':
                intervals[key] = float(_socket_value(input_socket(node, 'length')) or .9)
            if not node.mute and node.bl_idname == 'NPRLineNodeSetPosition':
                desired = input_socket(node, 'position')
                result = vector('ADD', value(desired) if desired.is_linked else before,
                                value(input_socket(node, 'offset')), tuple(node.location))
                selection = input_socket(node, 'selection')
                if selection.is_linked:
                    mix = tree.nodes.new('ShaderNodeMix')
                    mix.data_type = 'VECTOR'
                    mix.location = (node.location.x + 200, node.location.y)
                    tree.links.new(selection.links[0].from_socket, mix.inputs[0])
                    connect(before, mix.inputs[4])
                    connect(result, mix.inputs[5])
                    result = mix.outputs[1]
                elif not selection.default_value:
                    result = before
        else:
            raise ValueError('Legacy Brush routing through a group requires its original evaluator')
        positions[key] = result
        return result

    # Preserve old stage Position reads before removing their Brush input sockets.
    for info in list(tree.nodes):
        if info.bl_idname != 'NPRLineNodeStrokeInfo' or info.use_field_inputs:
            continue
        brush = source(input_socket(info, 'brush'))
        current = position(brush) if brush else original()
        for link in list(info.outputs['Position'].links):
            tree.links.new(current, link.to_socket)
        info.use_field_inputs = True

    converted = 0
    for output in list(tree.nodes):
        if output.bl_idname != 'NPRLineNodeBrushOutput' or output.use_field_inputs:
            continue
        incoming = source(input_socket(output, 'brush'))
        if incoming is None:
            output.use_field_inputs = True
            converted += 1
            continue
        current = position(incoming)
        base = roots[incoming.as_pointer()]
        _copy_brush_settings(base, output, skip={'color', 'width', 'opacity'})
        offset = vector('SUBTRACT', current, original(),
                        (output.location.x - 200, output.location.y - 220))
        tree.links.new(offset, input_socket(output, 'position_offset'))
        input_socket(output, 'sample_length').default_value = intervals[incoming.as_pointer()]
        output.use_field_inputs = True
        converted += 1
    # Remove only nodes from this experiment whose Brush result is no longer used.
    for _ in range(len(tree.nodes)):
        unused = [n for n in tree.nodes if n.bl_idname in legacy and
                  not any(s.is_linked for s in n.outputs)]
        if not unused:
            break
        for node in unused:
            tree.nodes.remove(node)
    return converted


@bpy.app.handlers.persistent
def _migrate_brush_fields_on_load(_):
    for tree in bpy.data.node_groups:
        if tree.bl_idname == 'NPRLineNodeTree' and tree.is_editable:
            try:
                migrate_brush_fields(tree)
            except (ValueError, RuntimeError) as error:
                print('Legacy Brush field migration: ' + str(error))


def current_tree(context):
    space = context.space_data
    if space and space.type == 'NODE_EDITOR' and space.edit_tree:
        if space.edit_tree.bl_idname == 'NPRLineNodeTree':
            return space.edit_tree
    return context.scene.eevee.geometry_line_tree


def set_objects(node, objects, exclude=False):
    prefix = 'exclude_' if exclude else 'object_'
    count_prop = 'exclude_count' if exclude else 'item_count'
    existing = {s.default_value for s in node.inputs
                if s.identifier.startswith(prefix) and s.type == 'OBJECT' and s.default_value}
    new_objects = [obj for obj in objects if obj not in existing]
    for obj in new_objects:
        empty = next((s for s in node.inputs if s.identifier.startswith(prefix)
                      and s.type == 'OBJECT' and not s.is_linked and s.default_value is None), None)
        if empty is None:
            index = getattr(node, count_prop)
            setattr(node, count_prop, index + 1)
            empty = input_socket(node, prefix + str(index))
        empty.default_value = obj
    if not exclude:
        node.mode = 'OBJECTS'


def add_line_group(tree, objects=(), source=None):
    if source is None:
        source = tree.nodes.new('NPRLineNodeObjects')
        if objects:
            set_objects(source, objects)
    brush = tree.nodes.new('NPRLineNodeBrush')
    settings = tree.nodes.new('NPRLineNodeSettings')
    output = next((n for n in tree.nodes if n.bl_idname == 'NPRLineNodeOutput'
                   and n.is_active_output), None)
    if output is None:
        output = tree.nodes.new('NPRLineNodeOutput')
        output.location = (700, 0)
    x, y = output.location
    settings.location = (x - 310, y)
    source.location = (x - 920, y + 80)
    brush.location = (x - 610, y - 120)
    tree.links.new(source.outputs[0], input_socket(settings, 'objects'))
    tree.links.new(brush.outputs[0], input_socket(settings, 'visible_brush'))
    destination = input_socket(output, 'plan')
    if destination.is_linked:
        old_link = destination.links[0]
        previous = old_link.from_node
        if previous.bl_idname == 'NPRLineNodeCombine' and previous.mode == 'PRIORITY':
            combine = previous
        else:
            old_socket = old_link.from_socket
            tree.links.remove(old_link)
            combine = tree.nodes.new('NPRLineNodeCombine')
            combine.location = (x - 310, y)
            tree.links.new(old_socket, input_socket(combine, 'plans'))
            tree.links.new(combine.outputs[0], destination)
        destination = input_socket(combine, 'plans')
        settings.location = (x - 620, y - 450)
        source.location = (x - 1230, y - 370)
        brush.location = (x - 930, y - 570)
    tree.links.new(settings.outputs[0], destination)
    for node in tree.nodes:
        node.select = False
    settings.select = True
    tree.nodes.active = settings
    tree.update_tag()
    return source, brush, settings, output


class SCENE_OT_npr_line_tree_new(Operator):
    bl_idname = 'scene.npr_line_tree_new'
    bl_label = 'New Geometry Lines'
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        scene = context.scene
        tree = bpy.data.node_groups.new('Geometry Lines', 'NPRLineNodeTree')
        scene.eevee.geometry_line_tree = tree
        scene.eevee.use_geometry_lines = True
        context.view_layer.eevee.use_pass_geometry_line = True
        add_line_group(tree, tuple(context.selected_objects))
        if context.space_data and context.space_data.type == 'NODE_EDITOR':
            context.space_data.tree_type = 'NPRLineNodeTree'
        return {'FINISHED'}


class SCENE_OT_npr_line_tree_copy(Operator):
    bl_idname = 'scene.npr_line_tree_copy'
    bl_label = 'Copy Geometry Lines'
    bl_description = 'Give this scene an independent copy of its geometry line node tree'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return context.scene.eevee.geometry_line_tree is not None

    def execute(self, context):
        original = context.scene.eevee.geometry_line_tree
        tree = original.copy()
        tree.name = original.name + ' Copy'
        context.scene.eevee.geometry_line_tree = tree
        return {'FINISHED'}


def _inline_brush_preview_icon(node):
    preview = output_brush_preview if node.bl_idname in FIELD_BRUSH_TYPES else brush_preview
    return preview(node, width=192, height=64).icon_id


def _redraw_line_timings():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'NODE_EDITOR' and area.ui_type == 'NPRLineNodeTree':
                tree = window.scene.eevee.geometry_line_tree
                if (tree and getattr(tree, 'show_performance', False)) or (
                        area.spaces.active.edit_tree and any(
                            n.bl_idname in FIELD_BRUSH_TYPES
                            for n in area.spaces.active.edit_tree.nodes)):
                    area.tag_redraw()
    return .25


class NODE_OT_npr_line_brush_preview(Operator):
    bl_idname = 'node.npr_line_brush_preview'
    bl_label = 'Brush Preview'
    bl_description = 'Preview this brush on a local sample stroke'
    node_name: StringProperty()
    length: IntProperty(name='Length (px)', default=256, min=16, max=4096)

    def invoke(self, context, event):
        tree = current_tree(context)
        node = tree.nodes.get(self.node_name) if tree else None
        if node is None or node.bl_idname not in BRUSH_TYPES:
            return {'CANCELLED'}
        if node.bl_idname in FIELD_BRUSH_TYPES:
            self.length = node.preview_length
            return context.window_manager.invoke_props_dialog(self, width=420)
        return context.window_manager.invoke_popup(self, width=480)

    def draw(self, context):
        tree = current_tree(context)
        node = tree.nodes.get(self.node_name) if tree else None
        if node is None:
            return
        layout = self.layout
        if node.bl_idname in FIELD_BRUSH_TYPES:
            layout.prop(self, 'length')
            layout.label(text='Scene UV, color attributes and AOV use fallback values')
            return
        layout.label(text=node.label or node.name, icon='BRUSH_DATA')
        layout.template_icon(icon_value=brush_preview(node).icon_id, scale=18.0)
        layout.label(text='Local sample curve; large widths are fitted to the swatch', icon='INFO')
        layout.label(text='Open the Brush Preview sidebar to compare live edits')

    def execute(self, context):
        tree = current_tree(context)
        node = tree.nodes.get(self.node_name) if tree else None
        if node is None or node.bl_idname not in BRUSH_TYPES:
            return {'CANCELLED'}
        if node.bl_idname in FIELD_BRUSH_TYPES:
            node.preview_length = self.length
            output_brush_preview(node)
            _redraw_brush_nodes()
        else:
            brush_preview(node)
        return {'FINISHED'}

    @classmethod
    def register(cls):
        bpy.types.NPRLineNodeBrush.brush_preview_icon = IntProperty(
            get=_inline_brush_preview_icon, options={'HIDDEN', 'SKIP_SAVE'})
        for name in FIELD_BRUSH_TYPES:
            node_type = getattr(bpy.types, name)
            node_type.brush_preview_icon = IntProperty(
                get=_inline_brush_preview_icon, options={'HIDDEN', 'SKIP_SAVE'})
            node_type.preview_length = IntProperty(
                name='Preview Length (px)', default=256, min=16, max=4096)
        if _migrate_brush_fields_on_load not in bpy.app.handlers.load_post:
            bpy.app.handlers.load_post.append(_migrate_brush_fields_on_load)
        if not bpy.app.timers.is_registered(_redraw_line_timings):
            bpy.app.timers.register(_redraw_line_timings, persistent=True)

    @classmethod
    def unregister(cls):
        if hasattr(bpy.types.NPRLineNodeBrush, 'brush_preview_icon'):
            del bpy.types.NPRLineNodeBrush.brush_preview_icon
        for type_name in FIELD_BRUSH_TYPES:
            node_type = getattr(bpy.types, type_name)
            for name in ('brush_preview_icon', 'preview_length'):
                if hasattr(node_type, name):
                    delattr(node_type, name)
        if _migrate_brush_fields_on_load in bpy.app.handlers.load_post:
            bpy.app.handlers.load_post.remove(_migrate_brush_fields_on_load)
        if bpy.app.timers.is_registered(_redraw_line_timings):
            bpy.app.timers.unregister(_redraw_line_timings)
        clear_brush_preview()
        if _clear_brush_preview_on_load in bpy.app.handlers.load_pre:
            bpy.app.handlers.load_pre.remove(_clear_brush_preview_on_load)


class NODE_OT_npr_line_solo(Operator):
    bl_idname = 'node.npr_line_solo'
    bl_label = 'Solo Line Preview'
    bl_description = 'Temporarily isolate this rule in the viewport; final renders always use the complete tree'
    node_name: StringProperty()
    clear: BoolProperty(default=False)

    @classmethod
    def poll(cls, context):
        return context.scene.eevee.geometry_line_tree is not None

    def execute(self, context):
        tree = context.scene.eevee.geometry_line_tree
        if self.clear:
            tree.preview_solo_node = 0
        else:
            edit_tree = current_tree(context)
            if edit_tree != tree:
                self.report({'INFO'}, 'Return to the scene tree to isolate a line rule')
                return {'CANCELLED'}
            node = tree.nodes.get(self.node_name) if self.node_name else tree.nodes.active
            if node is None or node.bl_idname not in {'NPRLineNodeSettings', 'NPRLineNodeCombine'}:
                self.report({'INFO'}, 'Select Line Settings or Combine Lines to isolate')
                return {'CANCELLED'}
            tree.preview_solo_node = 0 if tree.preview_solo_node == node.node_identifier else node.node_identifier
        for window in context.window_manager.windows:
            for area in window.screen.areas:
                if area.type in {'VIEW_3D', 'NODE_EDITOR'}:
                    area.tag_redraw()
        return {'FINISHED'}


class NODE_OT_npr_line_add_group(Operator):
    bl_idname = 'node.npr_line_add_group'
    bl_label = 'Add Line Group'
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        tree = current_tree(context)
        if tree is None:
            return bpy.ops.scene.npr_line_tree_new()
        active = tree.nodes.active
        source = active if active and active.bl_idname == 'NPRLineNodeObjects' else None
        add_line_group(tree, tuple(context.selected_objects), source)
        return {'FINISHED'}


class NODE_OT_npr_line_add_selected(Operator):
    bl_idname = 'node.npr_line_add_selected'
    bl_label = 'Add Selected Line Objects'
    bl_options = {'REGISTER', 'UNDO'}
    node_name: StringProperty()
    exclude: BoolProperty(default=False)

    def execute(self, context):
        tree = current_tree(context)
        node = tree.nodes.get(self.node_name) if tree else None
        if node is None or node.bl_idname != 'NPRLineNodeObjects':
            return {'CANCELLED'}
        set_objects(node, tuple(context.selected_objects), self.exclude)
        return {'FINISHED'}


class NODE_OT_npr_line_copy_visible(Operator):
    bl_idname = 'node.npr_line_copy_visible'
    bl_label = 'Copy Visible Line Settings'
    bl_options = {'REGISTER', 'UNDO'}
    node_name: StringProperty()

    def execute(self, context):
        tree = current_tree(context)
        node = tree.nodes.get(self.node_name) if tree else None
        if node is None or node.bl_idname != 'NPRLineNodeSettings':
            return {'CANCELLED'}
        for edge in ('outline', 'object', 'intersection', 'smoothing', 'material', 'normal', 'wire', 'marked'):
            input_socket(node, 'hidden_' + edge).default_value = input_socket(node, 'visible_' + edge).default_value
        node.hidden_overrides = node.visible_overrides
        for edge in ('outline', 'object', 'intersection', 'smoothing', 'material', 'normal', 'wire', 'marked'):
            source = input_socket(node, 'visible_override_' + edge)
            target = input_socket(node, 'hidden_override_' + edge)
            for link in list(target.links):
                tree.links.remove(link)
            if source.is_linked:
                tree.links.new(source.links[0].from_socket, target)
        return {'FINISHED'}


class NODE_OT_npr_line_make_brush_unique(Operator):
    bl_idname = 'node.npr_line_make_brush_unique'
    bl_label = 'Make Line Brush Single User'
    bl_options = {'REGISTER', 'UNDO'}
    node_name: StringProperty()
    socket_identifier: StringProperty(default='visible_brush')

    def execute(self, context):
        tree = current_tree(context)
        node = tree.nodes.get(self.node_name) if tree and self.node_name else tree.nodes.active if tree else None
        if node is None or node.bl_idname != 'NPRLineNodeSettings':
            return {'CANCELLED'}
        target = input_socket(node, self.socket_identifier)
        if not target.is_linked or target.links[0].from_node.bl_idname not in BRUSH_TYPES:
            return {'CANCELLED'}
        original = target.links[0].from_node
        if original.bl_idname == 'NPRLineNodeBrushClosureOutput':
            space = context.space_data
            if not space or space.type != 'NODE_EDITOR' or space.edit_tree != tree:
                self.report({'INFO'}, 'Open this brush closure in the node editor to copy it')
                return {'CANCELLED'}
            start = next((n for n in tree.nodes if n.bl_idname == 'NPRLineNodeBrushClosureInput'
                          and n.paired_output == original), None)
            if start is None:
                return {'CANCELLED'}
            connected = {start, original}
            pending = [original]
            while pending:
                for socket in pending.pop().inputs:
                    for link in socket.links:
                        if link.from_node not in connected:
                            connected.add(link.from_node)
                            pending.append(link.from_node)
            for candidate in tree.nodes:
                candidate.select = candidate in connected
            tree.nodes.active = original
            previous = set(tree.nodes)
            bpy.ops.node.duplicate('EXEC_DEFAULT')
            copies = set(tree.nodes) - previous
            brush = next(n for n in copies if n.bl_idname == original.bl_idname)
            brush.preview_length = original.preview_length
            for copy in copies:
                copy.location.y -= 400
            tree.links.new(brush.outputs[0], target)
            tree.nodes.active = brush
            return {'FINISHED'}
        brush = tree.nodes.new(original.bl_idname)
        brush.name = original.name + ' Copy'
        brush.label = (original.label or original.name) + ' · ' + bpy.app.translations.pgettext_iface('Copy')
        _copy_brush_settings(original, brush)
        if original.bl_idname == 'NPRLineNodeBrushOutput':
            brush.preview_length = original.preview_length
        brush.parent = original.parent
        brush.location = (original.location.x, original.location.y - max(320, original.dimensions.y + 60))
        brush.width = original.width
        for source_panel, target_panel in zip(original.panel_states, brush.panel_states):
            target_panel.is_collapsed = source_panel.is_collapsed
        tree.links.new(brush.outputs[0], target)
        for candidate in tree.nodes:
            candidate.select = False
        brush.select = True
        tree.nodes.active = brush
        return {'FINISHED'}


class NODE_OT_npr_line_move_rule(Operator):
    bl_idname = 'node.npr_line_move_rule'
    bl_label = 'Move Geometry Line Rule'
    bl_options = {'REGISTER', 'UNDO'}
    node_name: StringProperty()
    sort_id: IntProperty()
    direction: IntProperty(default=-1)

    def execute(self, context):
        tree = current_tree(context)
        node = tree.nodes.get(self.node_name) if tree else None
        if node is None or node.bl_idname != 'NPRLineNodeCombine':
            return {'CANCELLED'}
        links = sorted(
            input_socket(node, 'plans').links,
            key=lambda link: link.multi_input_sort_id, reverse=True)
        index = next((i for i, link in enumerate(links) if link.multi_input_sort_id == self.sort_id), -1)
        other = index + self.direction
        if index < 0 or not 0 <= other < len(links):
            return {'CANCELLED'}
        links[index].swap_multi_input_sort_id(links[other])
        tree.update_tag()
        return {'FINISHED'}


classes = (
    NODE_OT_npr_line_brush_effect,
    SCENE_OT_npr_line_tree_new,
    SCENE_OT_npr_line_tree_copy,
    NODE_OT_npr_line_brush_preview,
    NODE_OT_npr_line_solo,
    NODE_OT_npr_line_add_group,
    NODE_OT_npr_line_add_selected,
    NODE_OT_npr_line_copy_visible,
    NODE_OT_npr_line_make_brush_unique,
    NODE_OT_npr_line_move_rule,
)
