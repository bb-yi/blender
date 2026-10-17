# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import *
from filter_graph_test_utils import attach_filter_material, refresh_tree
s, m, e = scene()
fm = bpy.data.materials.new('Dimensions')
fm.eevee_domain = 'FILTER'
fm.node_tree.nodes.clear()
ri = fm.node_tree.nodes.new('ShaderNodeRenderInfo')
inp = fm.node_tree.nodes.new('ShaderNodeFilterGraphInput')
sample = fm.node_tree.nodes.new('ShaderNodeNPR_ImageSample')
fm.node_tree.links.new(inp.outputs['Image'], sample.inputs['Image'])
# Keep Image as a real material input while outputting dimensions.
zero = fm.node_tree.nodes.new('ShaderNodeVectorMath')
zero.operation = 'SCALE'
zero.inputs['Scale'].default_value = 0
fm.node_tree.links.new(sample.outputs[0], zero.inputs[0])
add = fm.node_tree.nodes.new('ShaderNodeVectorMath')
add.operation = 'ADD'
fm.node_tree.links.new(zero.outputs[0], add.inputs[0])
fm.node_tree.links.new(ri.outputs['Resolution'], add.inputs[1])
fo = fm.node_tree.nodes.new('ShaderNodeOutputFilter')
fo.inputs['Alpha'].default_value = 1
fm.node_tree.links.new(add.outputs[0], fo.inputs['Color'])
graph, p1, out = attach_filter_material(fm, scene_socket='Color Image')
p2 = graph.nodes.new('EeveeFilterGraphNodeFilterMaterial')
p2.material = fm
refresh_tree(graph)
graph.links.new(p1.outputs['Image'], p2.inputs['Image'])
graph.links.new(p2.outputs['Image'], out.inputs['Image'])
cases = [('STAGE', .125, 'INPUT', .125, (1, 1)),
         ('STAGE', .5, 'INPUT', .5, (16, 12)),
         ('STAGE', .5, 'STAGE', .5, (32, 24)),
         ('STAGE', 1 / 32, 'INPUT', 1, (2, 2)),
         ('STAGE', .3, 'INPUT', .5, (10, 8)),
         ('STAGE', .5, 'INPUT', 2, (64, 48))]
for i, (b1, v1, b2, v2, expected) in enumerate(cases):
    p1.resolution_basis = b1
    p1.resolution_scale = v1
    p2.resolution_basis = b2
    p2.resolution_scale = v2
    actual = center(render('filter_' + str(i)))
    near(actual, [*expected, 0], .03)
s.render.resolution_percentage = 50
p1.resolution_basis = 'STAGE'
p1.resolution_scale = 1
p2.resolution_basis = 'INPUT'
p2.resolution_scale = .5
near(center(render('filter_render_percentage')), [16, 12, 0], .03)
s.render.resolution_percentage = 100
p2.resolution_basis = 'FIXED'
p2.resolution_width = 13
p2.resolution_height = 7
near(center(render('filter_fixed')), [13, 7, 0], .03)
# The graph AOV output must resample the complete 13x7 result back to the full render size.
aov = s.view_layers[0].aovs.add()
aov.name = 'ScaledAOV'
aov.type = 'COLOR'
aov_node = graph.nodes.new('EeveeFilterGraphNodeAOVOutput')
aov_node.aov_name = aov.name
aov_node.execution_stage = 'BEFORE_COMPOSITE'
graph.links.new(p2.outputs['Image'], aov_node.inputs['Color'])
s.use_nodes = True
ct = bpy.data.node_groups.new('AOVCheck', 'CompositorNodeTree')
s.compositing_node_group = ct
rl = ct.nodes.new('CompositorNodeRLayers')
file = ct.nodes.new('CompositorNodeOutputFile')
file.directory = str(OUT)
file.file_name = 'scaled_aov'
file.format.file_format = 'OPEN_EXR_MULTILAYER'
file.file_output_items.clear()
file.file_output_items.new('RGBA', 'ScaledAOV')
ct.links.new(rl.outputs['ScaledAOV'], file.inputs['ScaledAOV'])
render('filter_aov')
import OpenImageIO as oiio
reader = oiio.ImageInput.open(str(OUT / 'scaled_aov.exr'))
assert reader is not None
data = reader.read_image(format='float')
reader.close()
assert data.shape[:2] == (48, 64), data.shape
for y, x in [(2, 2), (24, 32), (45, 61)]:
    near(data[y, x, :3], [13, 7, 0], .03)
s.compositing_node_group = None
s.use_nodes = False
inp.interface_items.new('Full Size')
refresh_tree(graph)
source = next(n for n in graph.nodes if n.bl_idname == 'EeveeFilterGraphNodeSceneColor')
graph.links.new(source.outputs['Color Image'], p2.inputs['Full Size'])
reference = p2.inputs['Full Size'].identifier
p1.resolution_basis = 'STAGE'
p1.resolution_scale = .5
p2.resolution_basis = 'INPUT'
p2.resolution_scale = .5
p2.resolution_input = reference
near(center(render('filter_reference')), [32, 24, 0], .03)
inp.interface_items.move(1, 0)
assert p2.resolution_input == reference
near(center(render('filter_reference_reordered')), [32, 24, 0], .03)
inp.interface_items[0].name = 'Renamed Reference'
near(center(render('filter_reference_renamed')), [32, 24, 0], .03)
p2.resolution_basis = 'FIXED'
p2.resolution_width = 13
p2.resolution_height = 7
# Normalized coordinates must cover the entire image at a reduced pass size.
fm.node_tree.links.new(ri.outputs['Frag Coord'], fo.inputs['Color'])
a = render('filter_uv')
assert .4 < a[24, 32, 0] < .6 and .4 < a[24, 32, 1] < .6, center(a)
assets = OUT / 'assets'
assets.mkdir(exist_ok=True)
bpy.ops.wm.save_as_mainfile(filepath=str(assets / 'filter.blend'))
bpy.ops.wm.open_mainfile(filepath=str(assets / 'filter.blend'))
near(center(render('filter_reopened')), center(a), .01)
print('FILTER_RESOLUTION_PASS', flush=True)
