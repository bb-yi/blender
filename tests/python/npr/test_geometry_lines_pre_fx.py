"""Geometry Line samples belong to the Pre-FX stage before their camera effects."""
import argparse
import json
from pathlib import Path
import sys

import bpy
import numpy as np
import OpenImageIO as oiio

sys.path.insert(0, str(Path(__file__).parent))
from test_geometry_lines import reset, mesh_object, setup_lines, set_value, render, require


def stage_pixels(path):
    image = oiio.ImageInput.open(str(path))
    require(image is not None, 'Expected multilayer output')
    try:
        index = 0
        while image.seek_subimage(index, 0):
            spec = image.spec()
            channels = list(spec.channelnames)
            subname = spec.get_string_attribute('oiio:subimagename')
            indices = [next((i for i, name in enumerate(channels)
                             if name.endswith('.Pre-FX.' + component) or name == 'Pre-FX.' + component
                             or (subname.endswith('Pre-FX') and name == component)), -1)
                       for component in 'RGBA']
            if min(indices) >= 0:
                data = np.asarray(image.read_image(format=oiio.FLOAT), dtype=np.float32)
                return data.reshape(spec.height, spec.width, spec.nchannels)[:, :, indices]
            index += 1
    finally:
        image.close()
    raise AssertionError('Pre-FX pass is missing')


def main(out):
    scene = reset()
    obj = mesh_object('Pre-FX Line', [(-1, 0, 0), (1, 0, 0)], edges=[(0, 1)])
    tree, source, brush, settings, output = setup_lines([obj], ('wire',), width=6,
                                                       color=(.8, .05, .2, 1))
    bpy.context.view_layer.use_pass_pre_fx = True
    combined, raw = render(out, 'pre-fx-lines-plain')
    stage = stage_pixels(out / 'pre-fx-lines-plain.exr')
    require(raw[:, :, 3].sum() > 400, 'Fixture must draw real lines')
    require(np.abs(stage - combined).max() < .003,
            'Pre-FX must include Geometry Lines even without camera effects')
    scene.camera.data.type = 'PERSP'
    scene.camera.data.dof.use_dof = True
    scene.camera.data.dof.focus_distance = 2
    scene.camera.data.dof.aperture_fstop = .4
    set_value(output, 'depth_of_field', True)
    combined, raw = render(out, 'pre-fx-lines-dof')
    stage = stage_pixels(out / 'pre-fx-lines-dof.exr')
    require(np.abs(stage - raw).max() < .003,
            'Pre-FX must accumulate raw line color before line camera effects')
    require(np.count_nonzero((stage[:, :, 3] < .01) & (combined[:, :, 3] > .01)) > 20,
            'Combined must still apply the requested line depth of field')
    bpy.context.view_layer.use_pass_pre_fx = False
    disabled, _ = render(out, 'pre-fx-lines-disabled')
    require(np.abs(disabled - combined).max() < .003,
            'Requesting Pre-FX must not change Combined')
    bpy.context.view_layer.use_pass_pre_fx = True
    set_value(output, 'composite', False)
    combined, raw = render(out, 'pre-fx-lines-separate')
    stage = stage_pixels(out / 'pre-fx-lines-separate.exr')
    require(stage[:, :, 3].max() < .001 and combined[:, :, 3].max() < .001,
            'Separate-only Geometry Lines must stay out of Pre-FX and Combined')
    require(raw[:, :, 3].sum() > 400, 'Separate line pass must remain available')
    (out / 'results.json').write_text(json.dumps({'raw_line_stage_and_camera_effects': 'passed'}))
    print('GEOMETRY_LINES_PRE_FX_OK', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True)
    args = parser.parse_args(sys.argv[sys.argv.index('--')+1:])
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    main(out)
