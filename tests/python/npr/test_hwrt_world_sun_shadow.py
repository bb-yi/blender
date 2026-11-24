import sys
from pathlib import Path

import bpy
import numpy as np
from mathutils import Vector


output = Path(sys.argv[sys.argv.index('--') + 1]).resolve()
output.mkdir(parents=True, exist_ok=True)
scene = bpy.context.scene
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete()
scene.render.engine = 'BLENDER_EEVEE'
scene.render.resolution_x = scene.render.resolution_y = 96
scene.render.resolution_percentage = 100
scene.render.image_settings.file_format = 'OPEN_EXR'
settings = scene.eevee
settings.taa_render_samples = 2
settings.use_raytracing = True
settings.use_fast_gi = False
settings.ray_tracing_method = 'PROBE'
settings.ray_tracing_options.use_hardware_shadows = True
world = scene.world
world.sun_threshold = .01
world.sun_angle = 0
world.use_sun_shadow = True
tree = world.node_tree
background = tree.nodes.get('Background')
coordinates = tree.nodes.new('ShaderNodeTexCoord')
direction = tree.nodes.new('ShaderNodeVectorMath')
direction.operation = 'DOT_PRODUCT'
tree.links.new(coordinates.outputs['Generated'], direction.inputs[0])
spot = tree.nodes.new('ShaderNodeMath')
spot.operation = 'GREATER_THAN'
spot.inputs[1].default_value = .998
tree.links.new(direction.outputs['Value'], spot.inputs[0])
tree.links.new(spot.outputs[0], background.inputs['Color'])
background.inputs['Strength'].default_value = 1000
bpy.ops.object.camera_add(location=(0, 0, 5))
scene.camera = bpy.context.object
scene.camera.data.type = 'ORTHO'
scene.camera.data.ortho_scale = 3
bpy.ops.mesh.primitive_plane_add(size=5)
bpy.ops.mesh.primitive_plane_add(size=.55, location=(0, 0, 1))
caster = bpy.context.object
caster.visible_camera = False


def capture(name):
    scene.render.filepath = str(output / (name + '.exr'))
    bpy.ops.render.render(write_still=True)
    image = bpy.data.images.load(scene.render.filepath, check_existing=False)
    data = np.asarray(image.pixels[:]).reshape(96, 96, 4)[:, :, 0].copy()
    bpy.data.images.remove(image)
    return data


for sign in (1, -1):
    direction.inputs[1].default_value = Vector((.65 * sign, 0, 1)).normalized()
    caster.hide_render = True
    open_image = capture(f'open_{sign}')
    caster.hide_render = False
    closed_image = capture(f'shadow_{sign}')
    samples = []
    for center in (27, 69):
        area = np.s_[44:52, center - 3:center + 3]
        samples.append(float(closed_image[area].mean() / max(open_image[area].mean(), 1.e-8)))
    print('WORLD_SUN_SHADOW', sign, samples, float(open_image[48, 48]), flush=True)
    assert open_image[48, 48] > .05
    assert samples[0 if sign == 1 else 1] < .2 and samples[1 if sign == 1 else 0] > .9, samples
