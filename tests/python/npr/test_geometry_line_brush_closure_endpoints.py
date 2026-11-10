"""A tapered closure must stay inside the same path drawn at its maximum width."""
import argparse
import json
import math
from pathlib import Path
import sys

import bpy
import gpu
from mathutils import Vector
import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from test_geometry_line_brush_closure import scene, instance
from test_geometry_line_brush_regions import active_output, socket, set_value, require


def save_image(path, pixels):
    import OpenImageIO as oiio
    alpha = pixels[::-1, :, 3]
    rgb = np.ascontiguousarray(1 - alpha[:, :, None].repeat(3, axis=2), dtype=np.float32)
    output = oiio.ImageOutput.create(str(path))
    output.open(str(path), oiio.ImageSpec(rgb.shape[1], rgb.shape[0], 3, oiio.FLOAT))
    output.write_image(rgb)
    output.close()


def main(out, probe=False):
    gpu.init()
    obj, tree, quick, settings, parameters, output = scene()
    current = bpy.context.scene
    current.eevee.use_taa_reprojection = False
    current.eevee.taa_samples = 1
    active_output(tree).mode = 'FULL'
    wave = instance(tree, 'WAVE')
    wave.inputs['Amplitude'].default_value = 9.9
    wave.inputs['Wavelength'].default_value = -.1
    tree.links.new(parameters.outputs['Distance'], wave.inputs['Distance'])
    tree.links.new(wave.outputs['Offset'], socket(output, 'position_offset'))
    taper = instance(tree, 'TAPER')
    tree.links.new(parameters.outputs['Factor'], taper.inputs['Factor'])
    mapping = next(n.mapping for n in taper.node_tree.nodes if n.bl_idname == 'ShaderNodeFloatCurve')
    mapping.initialize()
    cpu_samples = [float(mapping.evaluate(mapping.curves[0], x)) for x in (0, 1e-7, .5, 1-1e-7, 1, 1+1e-7)]
    max_curve = max(mapping.evaluate(mapping.curves[0], i/256) for i in range(257))
    print('ENDPOINT_CURVE_CPU ' + json.dumps({'samples':cpu_samples,'max':max_curve}), flush=True)
    area = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
    space = area.spaces.active
    space.shading.type = 'RENDERED'
    space.shading.use_scene_world_render = space.shading.use_scene_lights_render = True
    space.overlay.show_overlays = False
    region = next(r for r in area.regions if r.type == 'WINDOW')
    target = gpu.types.GPUOffScreen(192, 192, format='RGBA32F')

    def draw():
        bpy.context.view_layer.update()
        projection = current.camera.calc_matrix_camera(bpy.context.evaluated_depsgraph_get(), x=192, y=192)
        target.draw_view3d(current, bpy.context.view_layer, space, region,
                           current.camera.matrix_world.inverted(), projection,
                           do_color_management=False, draw_background=False)
        pixels = np.asarray(target.texture_color.read(), dtype=np.float32).reshape(192,192,4).copy()
        require(np.isfinite(pixels).all(), 'Endpoint evaluation must stay finite')
        return pixels

    bad = []
    views = 0
    try:
        for camera_type in ('ORTHO', 'PERSP'):
            current.camera.data.type = camera_type
            current.camera.data.lens = 50
            for elevation in (15, 27, 39, 51, 63, 75):
                for azimuth in range(0, 360, 30):
                    a, e = math.radians(azimuth), math.radians(elevation)
                    current.camera.location = (5*math.cos(a)*math.cos(e),
                                               5*math.sin(a)*math.cos(e), 5*math.sin(e))
                    current.camera.rotation_euler = (-Vector(current.camera.location)).to_track_quat('-Z','Y').to_euler()
                    link = tree.links.new(taper.outputs['Width'], socket(output, 'width'))
                    tapered = draw()
                    tree.links.remove(link)
                    set_value(output, 'width', float(max_curve)*4)
                    maximum = draw()
                    views += 1
                    require(float(tapered[:,:,3].sum()) > 1 and float(maximum[:,:,3].sum()) > 1,
                            'Both comparison strokes must contain real ink at every tested view')
                    excess = (tapered[:,:,3] > maximum[:,:,3]+.05) & (tapered[:,:,3] > .1)
                    count = int(np.count_nonzero(excess))
                    if count > 5:
                        record = {'camera':camera_type,'azimuth':azimuth,'elevation':elevation,
                                  'excess_pixels':count,'taper_alpha':float(tapered[:,:,3].sum()),
                                  'max_width_alpha':float(maximum[:,:,3].sum())}
                        bad.append(record)
                        if len(bad) <= 3:
                            print('ENDPOINT_FAN_BAD ' + json.dumps(record), flush=True)
                            save_image(out/f'fan-{len(bad)}-taper.png', tapered)
                            save_image(out/f'fan-{len(bad)}-maximum.png', maximum)
                            tree.links.new(taper.outputs['Width'], socket(output,'width'))
                            bpy.ops.wm.save_as_mainfile(filepath=str(out/f'fan-{len(bad)}.blend'))
                    if bad and not probe:
                        break
                if bad and not probe:
                    break
            if bad and not probe:
                break
    finally:
        target.free()
    data = {'views':views,'bad':bad,'cpu_curve_samples':cpu_samples,'max_curve':float(max_curve)}
    (out/'endpoint-results.json').write_text(json.dumps(data,indent=2),encoding='utf8')
    require(not bad, 'Taper must stay inside the maximum-width stroke: ' + json.dumps(bad[:1]))
    print('BRUSH_CLOSURE_ENDPOINTS_OK ' + json.dumps({'views':views,'bad':len(bad)}), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output',required=True)
    parser.add_argument('--probe',action='store_true')
    args = parser.parse_args(sys.argv[sys.argv.index('--')+1:])
    out = Path(args.output)
    out.mkdir(parents=True,exist_ok=True)
    main(out,args.probe)
