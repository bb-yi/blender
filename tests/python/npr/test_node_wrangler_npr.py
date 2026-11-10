# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Run in an isolated Blender UI context; checks actual Node Wrangler operators."""

import json
import sys
from pathlib import Path

import addon_utils
import bpy


def run():
    addon_dir = Path(bpy.utils.system_resource('SCRIPTS')) / 'addons_core'
    sys.path.insert(0, str(addon_dir))
    addon_utils.enable('node_wrangler', default_set=True)
    from node_wrangler.operators.link_to_output import _preview_sockets, is_light_tree

    area = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
    area.type = 'NODE_EDITOR'
    region = next(r for r in area.regions if r.type == 'WINDOW')
    checks = []
    skipped = []

    def select(tree, *nodes):
        for node in tree.nodes:
            node.select = node in nodes
        tree.nodes.active = nodes[0]

    def finished(op, **kwargs):
        status = op(**kwargs)
        assert status == {'FINISHED'}, (op, status)

    def checked(name):
        checks.append(name)

    with bpy.context.temp_override(area=area, region=region):
        space = area.spaces.active
        tree = bpy.data.node_groups.new('NW Filter Test', 'EeveeFilterGraphNodeTree')
        bpy.context.scene.eevee.filter_graph = tree
        space.tree_type = tree.bl_idname
        space.pin = True
        space.node_tree = tree
        scene = tree.nodes.new('EeveeFilterGraphNodeSceneColor')
        stage = tree.nodes.new('EeveeFilterGraphNodeStageOutput')
        select(tree, scene)
        finished(bpy.ops.node.nw_link_out)
        assert stage.inputs[0].links[0].from_socket == scene.outputs[0]
        checked('filter_connect_to_stage')

        _preview_sockets.clear()
        for index in (0, 1, 2, 3, 0):
            finished(bpy.ops.node.nw_preview)
            assert stage.inputs[0].links[0].from_socket == scene.outputs[index]
        checked('filter_preview_cycles_all_images')

        second_stage = tree.nodes.new('EeveeFilterGraphNodeStageOutput')
        second_stage.execution_stage = 'BEFORE_VOLUME_FOG'
        reroute = tree.nodes.new('NodeReroute')
        tree.links.new(scene.outputs[0], reroute.inputs[0])
        tree.links.new(reroute.outputs[0], second_stage.inputs[0])
        tree.links.remove(stage.inputs[0].links[0])
        select(tree, scene)
        finished(bpy.ops.node.nw_link_out)
        assert second_stage.inputs[0].links[0].from_node == scene
        assert not stage.inputs[0].is_linked
        checked('filter_preserves_connected_execution_stage')

        aov = tree.nodes.new('EeveeFilterGraphNodeAOVOutput')
        tree.links.new(scene.outputs[0], aov.inputs[0])
        unused = tree.nodes.new('EeveeFilterGraphNodeAOVInput')
        unused_name = unused.name
        finished(bpy.ops.node.nw_del_unused, delete_muted=False)
        assert unused_name not in tree.nodes
        assert stage.name in tree.nodes and second_stage.name in tree.nodes and aov.name in tree.nodes
        assert aov.inputs[0].links[0].from_node == scene
        checked('filter_cleanup_preserves_stages_aovs_and_sources')

        select(tree, scene)
        finished(bpy.ops.node.nw_add_reroutes, option='LINKED')
        assert second_stage.inputs[0].links[0].from_node.type == 'REROUTE'
        finished(bpy.ops.node.nw_align_nodes)
        assert not bpy.ops.node.nw_merge_nodes.poll()
        assert not bpy.ops.node.nw_lazy_mix.poll()
        checked('filter_reroutes_alignment_and_image_merge_guard')

        material = bpy.data.materials.new('NW Filter Material')
        filter_pass = tree.nodes.new('EeveeFilterGraphNodeFilterMaterial')
        filter_pass.material = material
        select(tree, filter_pass, scene)
        finished(bpy.ops.node.nw_reset_nodes)
        assert filter_pass.material == material
        checked('filter_reset_preserves_pass_material')

        if hasattr(bpy.types, 'EeveeFilterGraphNodeGroup'):
            # A new group has no outputs: preview must add an image interface socket.
            child = bpy.data.node_groups.new('NW Filter Child', tree.bl_idname)
            inner = child.nodes.new('EeveeFilterGraphNodeSceneColor')
            group = tree.nodes.new('EeveeFilterGraphNodeGroup')
            group.node_tree = child
            select(tree, group)
            space.path.append(child, node=group)
            select(child, inner)
            _preview_sockets.clear()
            finished(bpy.ops.node.nw_preview)
            group_output = next(n for n in child.nodes if n.type == 'GROUP_OUTPUT')
            assert group_output.inputs[0].links[0].from_socket == inner.outputs[0]
            assert any(s.item_type == 'SOCKET' and s.is_inspect_output for s in child.interface.items_tree)
            assert any(link.from_node == group and link.to_node.bl_idname == stage.bl_idname for link in tree.links)
            finished(bpy.ops.node.nw_preview)
            assert group_output.inputs[0].links[0].from_socket == inner.outputs[1]
            assert len([s for s in child.interface.items_tree if s.item_type == 'SOCKET']) == 1
            checked('nested_filter_preview_reuses_image_interface')
            space.path.pop()
        else:
            skipped.append('Filter Graph groups are not supported by this Blender build')

        # Light data ownership is resolved through the editor, including pinning.
        light = bpy.data.lights.new('NW Light Test', 'POINT')
        light.use_nodes = True
        obj = bpy.data.objects.new('NW Light Test', light)
        bpy.context.scene.collection.objects.link(obj)
        bpy.context.view_layer.objects.active = obj
        space.pin = False
        space.tree_type = 'ShaderNodeTree'
        space.shader_type = 'OBJECT'
        space.pin = True
        space.node_tree = light.node_tree
        assert is_light_tree(bpy.context)
        tree = light.node_tree
        info = tree.nodes.new('ShaderNodeEeveeLightShaderInfo')
        select(tree, info)
        finished(bpy.ops.node.nw_link_out)
        output = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeEeveeLightShaderOutput')
        assert output.inputs['Color'].links[0].from_socket == info.outputs['Default Color']
        assert not any(n.bl_idname == 'ShaderNodeOutputMaterial' for n in tree.nodes)
        checked('light_connects_custom_output_without_material_output')

        _preview_sockets.clear()
        for socket_name, target_name in (
                ('Default Color', 'Color'), ('Default Intensity', 'Intensity'),
                ('Default Attenuation', 'Attenuation'), ('Distance', 'Intensity')):
            finished(bpy.ops.node.nw_preview)
            assert output.inputs[target_name].links[0].from_socket == info.outputs[socket_name]
        checked('light_preview_cycles_color_intensity_attenuation')

        emission = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeEmission')
        select(tree, emission)
        finished(bpy.ops.node.nw_link_out)
        conventional = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeOutputLight')
        assert conventional.inputs[0].links[0].from_node == emission
        finished(bpy.ops.node.nw_del_unused, delete_muted=False)
        assert output.name in tree.nodes and conventional.name in tree.nodes
        checked('traditional_light_output_and_cleanup')

        value1 = tree.nodes.new('ShaderNodeValue')
        value2 = tree.nodes.new('ShaderNodeValue')
        select(tree, value1, value2)
        finished(bpy.ops.node.nw_merge_nodes, mode='ADD', merge_type='AUTO')
        math = tree.nodes.active
        assert math.bl_idname == 'ShaderNodeMath' and math.operation == 'ADD'
        assert all(s.is_linked for s in list(math.inputs)[:2])
        checked('light_merge_math')

        child = bpy.data.node_groups.new('NW Light Child', 'ShaderNodeTree')
        value = child.nodes.new('ShaderNodeValue')
        item = child.interface.new_socket(name='Production', in_out='OUTPUT', socket_type='NodeSocketFloat')
        group_output = child.nodes.new('NodeGroupOutput')
        child.links.new(value.outputs[0], group_output.inputs[item.identifier])
        group = tree.nodes.new('ShaderNodeGroup')
        group.node_tree = child
        select(tree, group)
        space.path.append(child, node=group)
        select(child, value)
        _preview_sockets.clear()
        finished(bpy.ops.node.nw_preview)
        assert group_output.inputs[item.identifier].links[0].from_node == value
        viewers = [s for s in child.interface.items_tree if s.item_type == 'SOCKET' and s.is_inspect_output]
        assert len(viewers) == 1
        assert group_output.inputs[viewers[0].identifier].links[0].from_node == value
        assert output.inputs['Intensity'].links[0].from_socket == group.outputs[viewers[0].identifier]
        finished(bpy.ops.node.nw_preview)
        assert len([s for s in child.interface.items_tree if s.item_type == 'SOCKET']) == 2
        checked('nested_light_preview_preserves_production_output_and_reuses_viewer')
        space.path.pop()

        # Standard material and World output behavior must remain available.
        space.pin = False
        mesh = bpy.data.meshes.new('NW Mesh')
        obj = bpy.data.objects.new('NW Material Object', mesh)
        bpy.context.scene.collection.objects.link(obj)
        bpy.context.view_layer.objects.active = obj
        mat = bpy.data.materials.new('NW Standard Material')
        mat.use_nodes = True
        mesh.materials.append(mat)
        space.node_tree = mat.node_tree
        space.pin = True
        tree = mat.node_tree
        shader = next(n for n in tree.nodes if n.bl_idname == 'ShaderNodeBsdfPrincipled')
        select(tree, shader)
        finished(bpy.ops.node.nw_link_out)
        assert next(n for n in tree.nodes if n.type == 'OUTPUT_MATERIAL').inputs[0].links[0].from_node == shader
        assert not bpy.ops.node.nw_preview.poll()
        checked('standard_material_output_and_preview_fallback')

    print('NODE_WRANGLER_NPR_RESULT=' + json.dumps({'passed': len(checks), 'checks': checks, 'skipped': skipped}))


if __name__ == '__main__':
    run()
