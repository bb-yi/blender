# SPDX-FileCopyrightText: 2025 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import Operator
from bpy_extras.node_utils import connect_sockets, find_base_socket_type

from ..utils.nodes import (
    nw_check,
    nw_check_active,
    nw_check_visible_outputs,
    nw_check_space_type,
    is_visible_socket,
    force_update,
)


#### ------------------------------ OPERATORS ------------------------------ ####

def is_light_tree(context):
    space = context.space_data
    if space.id is not None:
        return isinstance(space.id, bpy.types.Light)
    # A pinned editor or a scripted context may not have resolved its owner yet.
    return any(light.node_tree == space.node_tree for light in bpy.data.lights)


def get_output_type(context, socket, tree):
    space = context.space_data
    if tree != space.node_tree:
        return 'NodeGroupOutput'
    if tree.bl_idname == 'EeveeFilterGraphNodeTree':
        return 'EeveeFilterGraphNodeStageOutput'
    if tree.bl_idname == 'ShaderNodeTree':
        if is_light_tree(context):
            if socket.type != 'SHADER' and hasattr(bpy.types, 'ShaderNodeEeveeLightShaderOutput'):
                return 'ShaderNodeEeveeLightShaderOutput'
            return 'ShaderNodeOutputLight'
        return {'OBJECT': 'ShaderNodeOutputMaterial',
                'WORLD': 'ShaderNodeOutputWorld',
                'LINESTYLE': 'ShaderNodeOutputLineStyle',
                'FILTER': 'ShaderNodeOutputFilter',
                'NPR': 'ShaderNodeNPR_Output'}[space.shader_type]
    return {'CompositorNodeTree': 'NodeGroupOutput',
            'TextureNodeTree': 'TextureNodeOutput',
            'GeometryNodeTree': 'NodeGroupOutput'}[tree.bl_idname]


def find_output(tree, node_type, source):
    candidates = [n for n in tree.nodes if n.bl_idname == node_type
                  and getattr(n, 'is_active_output', True)]
    # Each Filter Graph execution stage has its own active output. Prefer the
    # stage already used by this branch, including connections via reroutes.
    pending = [source.node]
    visited = set()
    while pending:
        node = pending.pop(0)
        if node in visited:
            continue
        visited.add(node)
        if node in candidates:
            return node
        pending.extend(link.to_node for out in node.outputs for link in out.links)
    if candidates:
        return next((n for n in candidates if getattr(n, 'execution_stage', '') == 'BEFORE_POSTFX'),
                    candidates[0])
    node = tree.nodes.new(node_type)
    node.location = source.node.location.x + source.node.width + 80, source.node.location.y
    node.select = False
    return node


def connect_output(context, source, tree, *, preview=False):
    node_type = get_output_type(context, source, tree)
    output = find_output(tree, node_type, source)
    inputs = [s for s in output.inputs if is_visible_socket(s)]
    if node_type == 'NodeGroupOutput':
        target = next((s for s in inputs if s.type == source.type
                       and (not preview or tree.interface.items_tree[s.identifier].is_inspect_output)), None)
        if target is None:
            item = tree.interface.new_socket(
                name='(Viewer)' if preview else source.name,
                in_out='OUTPUT', socket_type=find_base_socket_type(source))
            item.is_inspect_output = preview
            target = output.inputs[item.identifier]
    elif node_type == 'ShaderNodeEeveeLightShaderOutput':
        name = source.name.removeprefix('Default ')
        target = next((s for s in inputs if s.name == name and s.type == source.type), None)
        if target is None:
            target = next((s for s in inputs if s.type == source.type), inputs[0])
    else:
        target = next((s for s in inputs if s.name == source.name), inputs[0])
    connect_sockets(source, target)
    return target


class NODE_OT_link_to_output(Operator):
    """Link node to the group or node tree output"""
    bl_idname = "node.nw_link_out"
    bl_label = "Connect to Output"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        """Disabled for custom nodes as we do not know which nodes are outputs."""
        return (nw_check(cls, context)
                and nw_check_space_type(cls, context, {'ShaderNodeTree', 'CompositorNodeTree',
                                        'TextureNodeTree', 'GeometryNodeTree', 'EeveeFilterGraphNodeTree'})
                and nw_check_active(cls, context)
                and nw_check_visible_outputs(cls, context))

    def execute(self, context):
        tree = context.space_data.edit_tree
        nodes = tree.nodes
        active = nodes.active
        if (context.space_data.tree_type == 'EeveeFilterGraphNodeTree'
                or (context.space_data.tree_type == 'ShaderNodeTree'
                    and (is_light_tree(context)
                         or context.space_data.shader_type in {'FILTER', 'NPR'}
                         or context.space_data.edit_tree != context.space_data.node_tree))):
            source = next(s for s in active.outputs if is_visible_socket(s))
            connect_output(context, source, context.space_data.edit_tree)
            force_update(context)
            return {'FINISHED'}
        output_index = None
        tree_type = context.space_data.tree_type
        shader_outputs = {'OBJECT': 'ShaderNodeOutputMaterial',
                          'WORLD': 'ShaderNodeOutputWorld',
                          'LINESTYLE': 'ShaderNodeOutputLineStyle'}
        output_type = {
            'ShaderNodeTree': shader_outputs[context.space_data.shader_type],
            'CompositorNodeTree': 'NodeGroupOutput',
            'TextureNodeTree': 'TextureNodeOutput',
            'GeometryNodeTree': 'NodeGroupOutput',
        }[tree_type]
        for node in nodes:
            # check whether the node is an output node and,
            # if supported, whether it's the active one
            if node.rna_type.identifier == output_type \
               and (node.is_active_output if hasattr(node, 'is_active_output')
                    else True):
                output_node = node
                break
        else:  # No output node exists
            bpy.ops.node.select_all(action="DESELECT")
            output_node = nodes.new(output_type)
            output_node.location.x = active.location.x + active.dimensions.x + 80
            output_node.location.y = active.location.y

        if active.outputs:
            for i, output_socket in enumerate(active.outputs):
                if is_visible_socket(output_socket):
                    output_index = i
                    break
            for i, output_socket in enumerate(active.outputs):
                if output_socket.type == output_node.inputs[0].type and is_visible_socket(output_socket):
                    output_index = i
                    break

            out_input_index = 0
            if tree_type == 'ShaderNodeTree':
                if active.outputs[output_index].name == 'Volume':
                    out_input_index = 1
                elif active.outputs[output_index].name == 'Displacement':
                    out_input_index = 2
            elif tree_type == 'GeometryNodeTree':
                if active.outputs[output_index].type != 'GEOMETRY':
                    return {'CANCELLED'}
            connect_sockets(active.outputs[output_index], output_node.inputs[out_input_index])

        force_update(context)  # View-port render does not update.

        return {'FINISHED'}


_preview_sockets = {}


class NODE_OT_nw_preview(Operator):
    """Preview a Filter Graph image or light shader output; click again to cycle sockets"""
    bl_idname = 'node.nw_preview'
    bl_label = 'Preview Filter Graph / Light Node'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return (nw_check(cls, context)
                and (context.space_data.tree_type == 'EeveeFilterGraphNodeTree'
                     or (context.space_data.tree_type == 'ShaderNodeTree' and is_light_tree(context))))

    def execute(self, context):
        space = context.space_data
        tree = space.edit_tree
        active = tree.nodes.active
        if active is None:
            return {'CANCELLED'}
        sockets = [s for s in active.outputs if is_visible_socket(s)]
        if not sockets:
            return {'CANCELLED'}
        source = sockets[0]
        # Keep this transient per editor: light sockets can stay connected to
        # different output inputs, so links alone cannot identify the last click.
        key = space.as_pointer()
        previous = _preview_sockets.get(key)
        for i, socket in enumerate(sockets):
            if previous == (tree.as_pointer(), active.as_pointer(), socket.identifier):
                source = sockets[(i + 1) % len(sockets)]
                break
        selected_socket = (tree.as_pointer(), active.as_pointer(), source.identifier)
        # Route through the edited group instances, from the current tree to root.
        path = list(space.path)
        for index in range(len(path) - 1, 0, -1):
            group_tree = path[index].node_tree
            target = connect_output(context, source, group_tree, preview=True)
            parent_tree = path[index - 1].node_tree
            group = parent_tree.nodes.active
            if group is None or getattr(group, 'node_tree', None) != group_tree:
                group = next(n for n in parent_tree.nodes if getattr(n, 'node_tree', None) == group_tree)
            source = group.outputs[target.identifier]
        connect_output(context, source, space.node_tree)
        _preview_sockets[key] = selected_socket
        tree.nodes.active = active
        active.select = True
        force_update(context)
        return {'FINISHED'}

    def invoke(self, context, event):
        selected = bpy.ops.node.select(
            location=(event.mouse_region_x, event.mouse_region_y), extend=False, socket_select=True)
        if 'FINISHED' not in selected:
            return {'PASS_THROUGH'}
        return self.execute(context)
