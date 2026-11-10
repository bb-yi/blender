"""Thick constant fields must cover the same pixels as the quick continuous brush."""
import argparse
import json
from pathlib import Path
import sys

import bpy
import gpu
from mathutils import Matrix
import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from test_geometry_line_brush_closure import scene
from test_geometry_line_brush_regions import socket


def main(out, probe=False):
    obj, tree, quick, settings, parameters, output = scene()
    current = bpy.context.scene
    current.eevee.taa_samples = 1
    current.eevee.use_taa_reprojection = False
    current.camera.data.type = 'PERSP'
    bpy.ops.mesh.primitive_monkey_add()
    monkey = bpy.context.object
    monkey.matrix_world = Matrix(((.4093911648, 0, 0, 0),
                                  (0, .3270829916, .2462069541, 0),
                                  (0, -.2462069541, .3270829916, .2681329846),
                                  (0, 0, 0, 1)))
    subdivision = monkey.modifiers.new('Subdivision', 'SUBSURF')
    subdivision.levels = 3
    subdivision.render_levels = 2
    bpy.ops.mesh.primitive_plane_add(size=2)
    source = next(node for node in tree.nodes if node.bl_idname == 'NPRLineNodeObjects')
    socket(source, 'object_0').default_value = monkey
    for kind in ('outline', 'object', 'wire'):
        socket(settings, 'visible_' + kind).default_value = kind != 'wire'
    root_output = next(node for node in tree.nodes if node.bl_idname == 'NPRLineNodeOutput')
    socket(root_output, 'composite').default_value = False
    graph = bpy.data.node_groups.new('Seam line pass', 'EeveeFilterGraphNodeTree')
    current.eevee.filter_graph = graph
    image = graph.nodes.new('EeveeFilterGraphNodeSceneColor')
    stage = graph.nodes.new('EeveeFilterGraphNodeStageOutput')
    stage.execution_stage = 'BEFORE_COMPOSITE'
    stage.is_active_output = True
    graph.links.new(image.outputs['Geometry Line Image'], stage.inputs['Image'])
    color = (.11548926, .05529060, .66310292, 1)
    socket(output, 'color').default_value = socket(quick, 'color').default_value = color
    quick.raster_size_response = True
    area = next(area for area in bpy.context.screen.areas if area.type == 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    space = area.spaces.active
    space.shading.type = 'RENDERED'
    space.shading.use_scene_world_render = space.shading.use_scene_lights_render = True
    space.overlay.show_overlays = False
    width, height = 1664, 580
    view = Matrix(((.8171438575, -.5764338374, -.0000003475, .1396127939),
                   (.4363579452, .6185739636, .6534203291, -.2735040784),
                   (-.3766532838, -.5339385867, .7569953799, -1.6883218288), (0, 0, 0, 1)))
    base_projection = Matrix(((1.3888888359, 0, 0, 0), (0, 3.9846746922, 0, 0),
                              (0, 0, -1.0000200272, -.0200002007), (0, 0, -1, 0)))
    gpu.init()
    target = gpu.types.GPUOffScreen(width, height, format='RGBA32F')

    def draw(brush, projection):
        tree.links.new(brush.outputs[0], socket(settings, 'visible_brush'))
        bpy.context.view_layer.update()
        target.draw_view3d(current, bpy.context.view_layer, space, region, view, projection,
                           do_color_management=False, draw_background=False)
        pixels = np.asarray(target.texture_color.read(), dtype=np.float32).reshape(height, width, 4).copy()
        assert np.isfinite(pixels).all() and pixels[:, :, 3].sum() > 100, 'Must draw real finite ink'
        return pixels

    cases = []
    try:
        for line_width in (2, 12, 22.9, 40):
            socket(output, 'width').default_value = socket(quick, 'width').default_value = line_width
            for opacity in (1, .35):
                socket(output, 'opacity').default_value = socket(quick, 'opacity').default_value = opacity
                for shift_x, shift_y in ((0, 0), (.37, .19), (-.25, -.32)):
                    projection = base_projection.copy()
                    projection[0][2] = -2*shift_x/width
                    projection[1][2] = -2*shift_y/height
                    actual = draw(output, projection)
                    reference = draw(quick, projection)
                    missing = (reference[:, :, 3] > opacity*.99) & (actual[:, :, 3] < reference[:, :, 3]-.01)
                    error = float(np.abs(actual-reference).max())
                    mean_error = float(np.abs(actual-reference).mean())
                    entry = {'width':line_width, 'opacity':opacity, 'shift':[shift_x,shift_y],
                             'missing_interior_pixels':int(missing.sum()), 'maximum_rgba_error':error,
                             'mean_rgba_error':mean_error}
                    cases.append(entry)
                    # The two brush representations can differ at half-float AA
                    # edges. Bound those small errors and reject missing interior
                    # coverage independently; a clipped seam is a full-alpha loss.
                    failed = error >= .01 or mean_error >= 1e-5 or missing.any()
                    if failed:
                        if sum(item['maximum_rgba_error'] >= .01 for item in cases) <= 2:
                            np.save(out/f'closure-failure-{len(cases)}.npy', actual)
                            np.save(out/f'quick-failure-{len(cases)}.npy', reference)
                        print('BRUSH_SEAM_BAD '+json.dumps(entry), flush=True)
                        if not probe:
                            break
                if not probe and failed:
                    break
            if not probe and failed:
                break
    finally:
        target.free()
    bad = [entry for entry in cases if entry['maximum_rgba_error'] >= .01 or
           entry['mean_rgba_error'] >= 1e-5 or entry['missing_interior_pixels']]
    result = {'backend':gpu.platform.backend_type_get(), 'cases':cases, 'bad_cases':len(bad)}
    (out/'seam-results.json').write_text(json.dumps(result,indent=2),encoding='utf8')
    assert not bad, 'Constant closure fields must not add capsule seams: '+json.dumps(bad[:1])
    print('BRUSH_CLOSURE_SEAMS_OK '+json.dumps({'cases':len(cases),'maximum_rgba_error':max(e['maximum_rgba_error'] for e in cases)}),flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True)
    parser.add_argument('--probe', action='store_true')
    args = parser.parse_args(sys.argv[sys.argv.index('--')+1:])
    directory = Path(args.output)
    directory.mkdir(parents=True,exist_ok=True)
    main(directory,args.probe)
