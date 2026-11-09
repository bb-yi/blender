# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""GPU regressions for full-depth caster ownership and cached instance identities."""

import argparse
import json
import math
import sys
import time
from pathlib import Path

import bpy
import gpu
import numpy as np
from mathutils import Vector


SIZE = 192


def mesh_object(name, vertices, faces, material):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(vertices, [], faces)
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    mesh.materials.append(material)
    obj.shadow_terminator_geometry_offset = 0
    return obj


def fixture(mode, reset=True):
    if reset:
        bpy.ops.wm.read_factory_settings(use_empty=True)
    else:
        # Keep the active window/context used by the persistent offscreen viewport.
        bpy.ops.object.select_all(action='SELECT')
        bpy.ops.object.delete()
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    scene.render.resolution_x = scene.render.resolution_y = SIZE
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.render.image_settings.color_mode = 'RGBA'
    scene.render.image_settings.color_depth = '32'
    scene.eevee.taa_render_samples = 16
    scene.eevee.taa_samples = 1
    scene.eevee.use_taa_reprojection = False
    scene.eevee.use_shadow_jitter_viewport = False
    scene.eevee.shadow_ray_count = 1
    scene.eevee.shadow_step_count = 6
    scene.eevee.shadow_pool_size = '512'
    scene.world = bpy.data.worlds.new('BlackWorld')
    scene.world.color = (0, 0, 0)
    scene.view_settings.view_transform = 'Standard'
    scene.view_settings.look = 'None'
    mask = bpy.data.materials.new('Classification')
    mask.use_nodes = True
    nodes, links = mask.node_tree.nodes, mask.node_tree.links
    nodes.clear()
    output = nodes.new('ShaderNodeOutputMaterial')
    emission = nodes.new('ShaderNodeEmission')
    rgb = nodes.new('ShaderNodeCombineXYZ')
    info = nodes.new('ShaderNodeShaderInfo')
    info.shadow_mode = mode
    info.stable_shadow_samples = 16
    for channel, socket in enumerate(['Shadow', 'Self Shadow', 'Cast Shadow']):
        links.new(info.outputs[socket], rgb.inputs[channel])
    links.new(rgb.outputs[0], emission.inputs['Color'])
    links.new(emission.outputs[0], output.inputs['Surface'])
    receiver = mesh_object('A_Receiver',
                           [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0),
                            (.35,-.65,0),(.35,-.15,0),(.35,-.15,.4),(.35,-.65,.4)],
                           [(0,1,2,3),(4,5,6,7)], mask)
    opaque = bpy.data.materials.new('OpaqueCaster')
    opaque.use_nodes = True
    external = mesh_object('B_External',
                           [(.35,.15,0),(.35,.65,0),(.35,.65,.4),(.35,.15,.4)],
                           [(0,1,2,3)], opaque)
    light_data = bpy.data.lights.new('Sun', 'SUN')
    light = bpy.data.objects.new('Sun', light_data)
    scene.collection.objects.link(light)
    light.rotation_euler = (0, math.pi/4, 0)
    light_data.energy = 3
    light_data.angle = .02 if mode == 'SOFT_FILTERED' else 0
    light_data.use_shadow_jitter = False
    light_data.shadow_filter_radius = 0
    light_data.shadow_maximum_resolution = 0.0001
    camera_data = bpy.data.cameras.new('Camera')
    camera = bpy.data.objects.new('Camera', camera_data)
    scene.collection.objects.link(camera)
    camera.location = (0,0,4)
    camera_data.type = 'ORTHO'
    camera_data.ortho_scale = 2.2
    camera_data.clip_start = .01
    camera_data.clip_end = 20
    scene.camera = camera
    bpy.context.view_layer.update()
    return scene, receiver, external, light


def read_image(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    pixels = np.asarray(image.pixels[:], dtype=np.float32).reshape(SIZE,SIZE,4).copy()
    bpy.data.images.remove(image)
    assert np.isfinite(pixels).all(), 'Non-finite image'
    return pixels[:,:,:3]


def sample_region(pixels, ymin, ymax):
    y,x = np.mgrid[:SIZE,:SIZE]
    wx = ((x+.5)/SIZE-.5)*2.2
    wy = ((y+.5)/SIZE-.5)*2.2
    return pixels[(wx>.08)&(wx<.24)&(wy>ymin)&(wy<ymax)]


def check(pixels):
    assert np.isfinite(pixels).all(), 'Non-finite classification image'
    own = sample_region(pixels, -.55, -.25)
    other = sample_region(pixels, .25, .55)
    expected_own = np.array([0,0,1], dtype=np.float32)
    expected_other = np.array([0,1,0], dtype=np.float32)
    return {'self_error':float(np.abs(own-expected_own).mean()),
            'cast_error':float(np.abs(other-expected_other).mean()),
            'conservation_error':float(np.abs(own[:,1]+own[:,2]-own[:,0]-1).max())}


def run(args):
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    cases, failures = [], []

    def record(name, pixels):
        values = check(pixels)
        cases.append({'case':name, **values})
        if max(values.values())>.005:
            failures.append(name)
        print('SHADOW_CLASSIFICATION_CASE', json.dumps(cases[-1]), flush=True)

    def render(name):
        bpy.context.view_layer.update()
        started = time.perf_counter()
        bpy.ops.render.render()
        path = out/(name+'.exr')
        bpy.data.images['Render Result'].save_render(str(path),scene=bpy.context.scene)
        pixels = read_image(path)
        print('SHADOW_CLASSIFICATION_SECONDS', name, time.perf_counter()-started, flush=True)
        return pixels

    modes = [args.mode] if args.mode else (['SOFT_FILTERED'] if args.viewport else ['TEMPORAL','SOFT_FILTERED'])
    for mode in modes:
        scene, receiver, external, light = fixture(mode,reset=not args.viewport)
        offscreen = None
        if args.viewport:
            area = next(a for a in bpy.context.screen.areas if a.type=='VIEW_3D')
            region = next(r for r in area.regions if r.type=='WINDOW')
            space = area.spaces.active
            space.shading.type = 'RENDERED'
            space.shading.use_scene_world_render = True
            space.shading.use_scene_lights_render = True
            space.overlay.show_overlays = False
            offscreen = gpu.types.GPUOffScreen(SIZE,SIZE,format='RGBA32F')

            def draw(name):
                bpy.context.view_layer.update()
                deps = bpy.context.evaluated_depsgraph_get()
                projection = scene.camera.calc_matrix_camera(deps,x=SIZE,y=SIZE)
                offscreen.draw_view3d(scene,bpy.context.view_layer,space,region,
                                     scene.camera.matrix_world.normalized().inverted(),projection,
                                     do_color_management=False,draw_background=True)
                pixels = np.array(offscreen.texture_color.read(),dtype=np.float32).reshape(SIZE,SIZE,4)
                assert np.isfinite(pixels).all(), 'Non-finite viewport image'
                return pixels[:,:,:3]
        else:
            draw = render
        try:
            baseline = draw(mode+'_baseline')
            record(mode+'_baseline', baseline)
            L = light.matrix_world.to_quaternion() @ Vector((0,0,1))
            bpy.ops.mesh.primitive_cube_add(size=.2,location=-L*1000+Vector((0,5,0)))
            rear = bpy.context.object
            rear.name = 'Z_UnrelatedRearCaster'
            for distance in [10,100,1000]:
                rear.location = -L*distance+Vector((0,5,0))
                pixels = draw(mode+'_rear_'+str(distance))
                record(mode+'_rear_'+str(distance), pixels)
                delta = float(np.abs(sample_region(pixels,.25,.55)-sample_region(baseline,.25,.55)).mean())
                cases[-1]['rear_cast_delta'] = delta
                if delta>.005:
                    failures.append(mode+'_rear_delta_'+str(distance))
            rear.visible_shadow = False
            record(mode+'_rear_disabled', draw(mode+'_rear_disabled'))
            if args.viewport:
                if mode == 'TEMPORAL':
                    for angle in [-1,0,1]:
                        light.rotation_euler.z = math.radians(angle)
                        record('viewport_sun_z_'+str(angle), draw('rotated'))
                for index in range(3):
                    scene.collection.objects.unlink(receiver)
                    scene.collection.objects.link(receiver)
                    scene.collection.objects.unlink(external)
                    scene.collection.objects.link(external)
                    record('viewport_reordered_'+str(index), draw('reordered'))
                bpy.data.objects.remove(rear,do_unlink=True)
                record('viewport_deleted_rear', draw('deleted'))
            else:
                # A transparent caster must agree in both passes, including discarded fragments.
                material = external.data.materials[0]
                nodes, links = material.node_tree.nodes, material.node_tree.links
                nodes.clear()
                output = nodes.new('ShaderNodeOutputMaterial')
                mix = nodes.new('ShaderNodeMixShader')
                mix.inputs[0].default_value = .5
                opaque = nodes.new('ShaderNodeEmission')
                transparent = nodes.new('ShaderNodeBsdfTransparent')
                links.new(opaque.outputs[0],mix.inputs[1])
                links.new(transparent.outputs[0],mix.inputs[2])
                links.new(mix.outputs[0],output.inputs['Surface'])
                scene.eevee.taa_render_samples = 64
                pixels = draw(mode+'_transparent')
                values = sample_region(pixels,.25,.55)
                residual = float(np.abs(values[:,1]+values[:,2]-values[:,0]-1).mean())
                cases.append({'case':mode+'_transparent','conservation_error':residual,
                              'mean_visibility':float(values[:,0].mean()),
                              'self_visibility':float(values[:,1].mean())})
                if residual>.005 or float(values[:,1].mean())<.995 or not .1<float(values[:,0].mean())<.9:
                    failures.append(mode+'_transparent')
        finally:
            if offscreen:
                offscreen.free()
    result = {'backend':gpu.platform.backend_type_get(),'cases':cases,'failures':failures}
    (out/'summary.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    assert not failures, failures
    print('SHADOW_CASTER_CLASSIFICATION_COMPLETE', flush=True)


if __name__=='__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--out',required=True)
    parser.add_argument('--viewport',action='store_true')
    parser.add_argument('--mode',choices=['TEMPORAL','SOFT_FILTERED'])
    run(parser.parse_args(sys.argv[sys.argv.index('--')+1:] if '--' in sys.argv else []))
