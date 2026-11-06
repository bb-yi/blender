# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Windows Vulkan NR animation regression; launch with isolated Blender user resources."""
import json
import os
from pathlib import Path
import re
import subprocess

import bpy
import numpy as np

DEFAULT_OUT = Path(__file__).resolve().parents[3] / 'test/dlss5_animation/out/regression'
OUT = Path(os.environ.get('DLSS_ANIMATION_TEST_OUT', DEFAULT_OUT))
OUT.mkdir(parents=True, exist_ok=True)
HAS_PRE_FX = hasattr(bpy.context.view_layer, 'use_pass_pre_fx')


def child():
    import OpenImageIO as oiio
    from mathutils import Vector
    scenario = os.environ['DLSS_ANIMATION_CHILD']
    target = OUT / scenario
    target.mkdir(exist_ok=True)
    s = bpy.context.scene
    s.render.engine = 'BLENDER_EEVEE'
    s.render.resolution_x, s.render.resolution_y, s.render.resolution_percentage = 192, 128, 100
    s.eevee.taa_render_samples = 8
    s.eevee.dlss5_mode = 'DLSSNR'
    s.render.use_compositing = False
    s.render.use_sequencer = False
    s.render.image_settings.media_type = 'MULTI_LAYER_IMAGE'
    s.render.image_settings.file_format = 'OPEN_EXR_MULTILAYER'
    s.render.image_settings.color_depth = '32'
    s.render.image_settings.color_mode = 'RGBA'
    s.frame_start, s.frame_end = 1, 5
    s.world.color = (.1, .1, .1)
    cube = bpy.data.objects['Cube']
    cube.scale = (.7, .7, .7)
    cube.keyframe_insert('location', frame=1)
    cube.location.x = .25
    cube.keyframe_insert('location', frame=5)
    cube.shape_key_add(name='Basis')
    key = cube.shape_key_add(name='Bend')
    for point in key.data:
        if point.co.z > 0:
            point.co.x += .2
    key.value = 0
    key.keyframe_insert('value', frame=1)
    key.value = .8
    key.keyframe_insert('value', frame=5)
    material = bpy.data.materials.new('Texture detail')
    material.use_nodes = True
    nodes = material.node_tree.nodes
    noise = nodes.new('ShaderNodeTexNoise')
    noise.inputs['Scale'].default_value = 12
    material.node_tree.links.new(noise.outputs['Color'],nodes.get('Principled BSDF').inputs['Base Color'])
    cube.data.materials.clear()
    cube.data.materials.append(material)
    camera = s.camera
    camera.location = (3,-6,2.4)
    camera.rotation_euler = (Vector((0,0,0))-camera.location).to_track_quat('-Z','Y').to_euler()
    camera.keyframe_insert('location',frame=1)
    camera.location.x += .2
    camera.keyframe_insert('location',frame=5)
    camera.data.dof.use_dof = True
    camera.data.dof.focus_object = cube
    camera.data.dof.aperture_fstop = 2
    if scenario == 'motion_blur':
        s.render.use_motion_blur = True
    expected_motion = {}
    if scenario == 'motion':
        from bpy_extras.object_utils import world_to_camera_view
        camera.animation_data_clear()
        camera.location = (0,0,5)
        camera.rotation_euler = (0,0,0)
        camera.data.type = 'ORTHO'
        camera.data.ortho_scale = 4
        camera.data.dof.use_dof = False
        cube.data.shape_keys.animation_data_clear()
        key.value = 0
        coords = {}
        for frame in range(0,6):
            s.frame_set(frame)
            coords[frame] = world_to_camera_view(s,camera,cube.matrix_world.translation)
        for frame in range(1,6):
            expected_motion[frame] = ((coords[frame-1].x-coords[frame].x)*192,
                                      (coords[frame-1].y-coords[frame].y)*128,
                                      int(coords[frame].x*192),int(coords[frame].y*128))
        directory = target/'inputs'
        directory.mkdir(exist_ok=True)
        os.environ['BLENDER_DLSS_NR_DUMP_INPUTS'] = str(directory)
    if HAS_PRE_FX and scenario in ('prefx', 'layers'):
        s.view_layers[0].use_pass_pre_fx = True
    if scenario == 'layers':
        s.view_layers.new('Second')
    if scenario == 'skip':
        s.frame_step = 2
    if scenario == 'reset':
        os.environ['BLENDER_DLSS_NR_FORCE_RESET'] = '1'
    if scenario == 'cache_off':
        os.environ['BLENDER_DLSS_DISABLE_SESSION_CACHE'] = '1'
    if scenario == 'camera':
        first = s.timeline_markers.new('First',frame=1)
        first.camera = camera
        other = camera.copy()
        other.data = camera.data.copy()
        s.collection.objects.link(other)
        marker = s.timeline_markers.new('Cut',frame=3)
        marker.camera = other
    if scenario == 'settings':
        def settings(scene):
            scene.eevee.dlss5_intensity = 1 if scene.frame_current < 3 else .5
        bpy.app.handlers.render_pre.append(settings)

    secondary = None
    if scenario == 'compositor':
        secondary = s.copy()
        secondary.name = 'NR Secondary Scene'
        tree = bpy.data.node_groups.new('NR Scene Composite', 'CompositorNodeTree')
        tree.interface.new_socket(name='Image', in_out='OUTPUT', socket_type='NodeSocketColor')
        s.compositing_node_group = tree
        s.render.use_compositing = True
        primary_node = tree.nodes.new('CompositorNodeRLayers')
        secondary_node = tree.nodes.new('CompositorNodeRLayers')
        secondary_node.scene = secondary
        mix = tree.nodes.new('CompositorNodeAlphaOver')
        mix.inputs['Factor'].default_value = .5
        output = tree.nodes.new('NodeGroupOutput')
        tree.links.new(primary_node.outputs['Image'], mix.inputs['Background'])
        tree.links.new(secondary_node.outputs['Image'], mix.inputs['Foreground'])
        tree.links.new(mix.outputs[0], output.inputs['Image'])

    if scenario == 'missing_mask':
        s.eevee.dlss5_mask_aov = 'No Such Value AOV'
    if scenario == 'small':
        s.render.resolution_x = s.render.resolution_y = 32
    if scenario == 'guide_failure':
        def fail_one_frame(scene):
            if scene.frame_current == 2:
                os.environ['BLENDER_DLSS_NR_TEST_GUIDE_FAILURE'] = '1'
            else:
                os.environ.pop('BLENDER_DLSS_NR_TEST_GUIDE_FAILURE', None)
        bpy.app.handlers.render_pre.append(fail_one_frame)

    visits = []
    frame_times = {}
    if scenario == 'backward':
        s.view_layers[0].use_pass_vector = False
        def start_frame(_scene):
            visits.clear()
        def changed(scene, _depsgraph=None):
            if scene == s:
                visits.append(scene.frame_current + scene.frame_subframe)
        bpy.app.handlers.render_pre.append(start_frame)
        bpy.app.handlers.frame_change_post.append(changed)

    statuses = []
    def written(scene):
        statuses.append(scene.eevee.dlss5_render_status)
        if scenario == 'backward':
            frame_times[scene.frame_current] = list(visits)
            assert visits and max(visits) <= scene.frame_current, (scene.frame_current, visits)
            assert scene.frame_current-1 in visits, (scene.frame_current, visits)
    bpy.app.handlers.render_write.append(written)
    arrays = {}
    repeats = 2 if scenario == 'temporal' else 1
    for run in range(repeats):
        s.render.filepath = str(target / f'run{run}_')
        bpy.ops.render.render(animation=True)
        for frame in range(1,6,s.frame_step):
            image = oiio.ImageInput.open(str(target/f'run{run}_{frame:04}.exr'))
            assert image
            spec = image.spec()
            data = image.read_image(format='float')
            assert np.isfinite(data).all()
            names = list(spec.channelnames)
            prefix = 'Composite.Combined.' if scenario == 'compositor' else 'ViewLayer.Combined.'
            rgba = np.stack([data[...,names.index(prefix+c)] for c in 'RGBA'],axis=-1)
            arrays[f'{run}_{frame}'] = rgba
            image.close()
    bpy.app.handlers.render_write.remove(written)
    if scenario in ('missing_mask', 'small'):
        assert statuses and all(v.startswith('Bypassed:') for v in statuses), statuses
    elif scenario == 'guide_failure':
        assert len(statuses) == 5 and 'guide initialization failed' in statuses[1], statuses
        assert all(statuses[i].startswith('Completed') for i in (0,2,3,4)), statuses
    else:
        assert statuses and all(v.startswith('Completed') for v in statuses), statuses
    if secondary:
        assert secondary.eevee.dlss5_render_status.startswith('Completed')
    if frame_times:
        assert len(frame_times) == 5, frame_times
        for frame, times in frame_times.items():
            assert times and max(times) <= frame and frame-1 in times, (frame, times)
        (target/'frame_times.json').write_text(json.dumps(frame_times,indent=2),encoding='utf-8')
    if scenario in ('missing_mask','small','guide_failure'):
        os.environ.pop('BLENDER_DLSS_NR_TEST_GUIDE_FAILURE',None)
        frame = 2 if scenario == 'guide_failure' else 1
        s.eevee.dlss5_mode = 'OFF'
        s.frame_set(frame)
        s.render.filepath = str(target/'native.exr')
        bpy.ops.render.render(write_still=True)
        image = oiio.ImageInput.open(s.render.filepath)
        names = list(image.spec().channelnames)
        data = image.read_image(format='float')
        native = np.stack([data[...,names.index('ViewLayer.Combined.'+c)] for c in 'RGBA'],axis=-1)
        image.close()
        np.testing.assert_allclose(arrays[f'0_{frame}'],native,rtol=.003,atol=.003)
    if repeats == 2:
        for frame in range(1,6):
            np.testing.assert_allclose(arrays[f'0_{frame}'],arrays[f'1_{frame}'],rtol=.003,atol=.003)
    if expected_motion:
        measured = {}
        for frame,(dx,dy,x,y) in expected_motion.items():
            motion = np.fromfile(target/'inputs'/f'{frame}_motion.f32',np.float32).reshape(128,192,2)
            assert np.isfinite(motion).all()
            np.testing.assert_allclose(motion[y,x], [dx,dy],rtol=0,atol=.02)
            measured[frame] = dict(expected=[dx,dy],actual=motion[y,x].tolist())
        assert measured[3]['actual'][0] < -.1, measured
        (target/'motion.json').write_text(json.dumps(measured,indent=2),encoding='utf-8')
    np.savez_compressed(target/'pixels.npz',**arrays)
    print('DLSS_HISTORY_CHILD_PASS',scenario,flush=True)


if os.environ.get('DLSS_ANIMATION_CHILD'):
    child()
else:
    assert os.environ.get('BLENDER_USER_RESOURCES'), 'User resources must be isolated'
    reports = {'pre_fx_available': HAS_PRE_FX}
    scenarios = ['temporal', 'reset', 'cache_off', 'skip', 'camera', 'settings',
                 'layers', 'motion_blur', 'motion', 'compositor', 'backward',
                 'missing_mask', 'small', 'guide_failure']
    if HAS_PRE_FX:
        scenarios.append('prefx')
    selected = os.environ.get('DLSS_ANIMATION_SCENARIOS')
    if selected:
        requested = set(selected.split(','))
        assert requested <= set(scenarios), requested
        scenarios = [name for name in scenarios if name in requested]
    reports['executed_scenarios'] = scenarios
    for scenario in scenarios:
        env = dict(os.environ,DLSS_ANIMATION_CHILD=scenario,DLSS_ANIMATION_TEST_OUT=str(OUT),
                   DLSS5_CACHE_DIR=str(OUT/'ngx-cache'))
        env.pop('BLENDER_DLSS_NR_FORCE_RESET',None)
        env.pop('BLENDER_DLSS_NR_DUMP_INPUTS',None)
        env.pop('BLENDER_DLSS_DISABLE_SESSION_CACHE',None)
        env.pop('BLENDER_DLSS_NR_TEST_GUIDE_FAILURE',None)
        proc = subprocess.run([bpy.app.binary_path,'--background','--factory-startup',
                               '--gpu-backend','vulkan','--log','eevee','--log-level','info',
                               '--python-exit-code','1','--python',str(Path(__file__).resolve())],
                              env=env,capture_output=True,timeout=180)
        log = (proc.stdout+proc.stderr).decode('utf-8',errors='replace')
        (OUT/(scenario+'.log')).write_text(log,encoding='utf-8')
        assert proc.returncode == 0 and 'DLSS_HISTORY_CHILD_PASS' in log, (scenario,log[-5000:])
        resets = re.findall(r'DLSS_HISTORY .*?reset=(\d)',log)
        if scenario == 'temporal':
            assert resets == ['1','0','0','0','0']*2,resets
        elif scenario == 'skip':
            assert resets == ['1','1','1'],resets
        elif scenario == 'reset':
            assert resets == ['1']*5,resets
        elif scenario in ('camera','settings'):
            assert resets == ['1','0','1','0','0'],(scenario,resets)
        elif scenario == 'prefx':
            assert resets == ['1','1']+['0','0']*4,resets
        elif scenario == 'layers':
            streams = 3 if HAS_PRE_FX else 2
            assert resets == ['1']*streams+['0']*(streams*4),resets
        elif scenario == 'compositor':
            assert resets == ['1','1']+['0','0']*4,resets
        elif scenario == 'guide_failure':
            assert resets == ['1','0','1','0','0'],resets
        elif scenario in ('missing_mask','small'):
            assert not resets and 'DLSS_GUIDE begin' not in log,(scenario,resets)
        else:
            assert resets == ['1','0','0','0','0'],(scenario,resets)
        reports[scenario] = dict(pass_status=True,resets=resets)
    if 'temporal' in scenarios and 'reset' in scenarios:
        with np.load(OUT/'temporal/pixels.npz') as temporal, np.load(OUT/'reset/pixels.npz') as reset:
            np.testing.assert_allclose(temporal['0_1'],reset['0_1'],rtol=.003,atol=.003)
            difference = np.mean([np.abs(temporal[f'0_{f}']-reset[f'0_{f}']).mean() for f in range(2,6)])
            assert difference > 1e-6, ('History produced no pixel effect',difference)
            reports['history_rgb_difference'] = float(difference)
            if 'cache_off' in scenarios:
                with np.load(OUT/'cache_off/pixels.npz') as uncached:
                    for frame in range(1,6):
                        np.testing.assert_allclose(temporal[f'0_{frame}'],uncached[f'0_{frame}'],rtol=.003,atol=.003)
            if HAS_PRE_FX and 'prefx' in scenarios:
                with np.load(OUT/'prefx/pixels.npz') as prefx:
                    for frame in range(1,6):
                        np.testing.assert_allclose(temporal[f'0_{frame}'],prefx[f'0_{frame}'],rtol=.003,atol=.003)
    (OUT/'report.json').write_text(json.dumps(reports,indent=2),encoding='utf-8')
    print('DLSS_ANIMATION_HISTORY_PASS',flush=True)
