import math
import sys
from pathlib import Path

import bpy
import gpu
import numpy as np
from mathutils import Vector


def material(obj, name):
    result = bpy.data.materials.new(name)
    result.use_nodes = True
    obj.data.materials.append(result)
    return result, result.node_tree.nodes['Principled BSDF']


directory = Path(sys.argv[sys.argv.index('--') + 1]).resolve()
directory.mkdir(parents=True, exist_ok=True)
gpu.init()
scene = bpy.context.scene
for obj in list(scene.objects):
    bpy.data.objects.remove(obj, do_unlink=True)
scene.render.engine = 'BLENDER_EEVEE'
scene.render.resolution_x = scene.render.resolution_y = 64
scene.render.resolution_percentage = 100
scene.render.image_settings.file_format = 'OPEN_EXR'
scene.render.use_compositing = False
scene.world.node_tree.nodes['Background'].inputs['Strength'].default_value = 0
settings = scene.eevee
settings.taa_render_samples = 1
settings.use_raytracing = True
settings.use_fast_gi = False
settings.ray_tracing_method = 'HARDWARE'
settings.use_hardware_raytracing_shadows = True
settings.use_hardware_raytracing_reflections = True
settings.hardware_raytracing_shadow_transparency = 0
settings.ray_tracing_options.resolution_scale = '1'
settings.ray_tracing_options.use_denoise = False
refracted = 'refract' in sys.argv[sys.argv.index('--') + 2:]
bpy.ops.object.camera_add(location=(0, 0, 10))
scene.camera = bpy.context.object
scene.camera.data.type = 'ORTHO'
scene.camera.data.ortho_scale = 2
if refracted:
    settings.use_hardware_raytracing_refractions = True
    bpy.ops.mesh.primitive_cube_add(size=1)
    slab = bpy.context.object
    slab.scale = (3, 3, .5)
    slab.visible_shadow = False
    glass, principled = material(slab, 'Refracting slab')
    glass.use_raytrace_refraction = True
    glass_shader = glass.node_tree.nodes.new('ShaderNodeBsdfGlass')
    glass_shader.inputs['Roughness'].default_value = 0
    glass.node_tree.links.new(glass_shader.outputs[0], glass.node_tree.nodes['Material Output'].inputs['Surface'])
    bpy.ops.mesh.primitive_plane_add(size=8, location=(0, 0, -2))
else:
    bpy.ops.mesh.primitive_plane_add(size=6, rotation=(0, math.pi / 4, 0))
    _, mirror = material(bpy.context.object, 'Mirror')
    mirror.inputs['Metallic'].default_value = 1
    mirror.inputs['Roughness'].default_value = 0
    bpy.ops.mesh.primitive_plane_add(size=8, location=(4, 0, 0), rotation=(0, -math.pi / 2, 0))
material(bpy.context.object, 'Reflected wall')
bpy.ops.mesh.primitive_plane_add(size=2 if refracted else 3.5, location=(2, 0, 0 if refracted else 3))
bpy.context.object.visible_camera = False
tree_material, shader = material(bpy.context.object, 'Computed overhang')
tree_material.use_transparent_shadow = True
tree = tree_material.node_tree
threshold = tree.nodes.new('ShaderNodeMath')
threshold.operation = 'GREATER_THAN'
texture_coordinates = tree.nodes.new('ShaderNodeTexCoord')
separate = tree.nodes.new('ShaderNodeSeparateXYZ')
tree.links.new(texture_coordinates.outputs['UV'], separate.inputs[0])
tree.links.new(separate.outputs['Y'], threshold.inputs[0])
tree.links.new(threshold.outputs[0], shader.inputs['Alpha'])
for kind in ('SUN', 'POINT'):
    bpy.ops.object.light_add(type=kind, location=(6, 0, 4) if refracted else (0, 0, 6))
    light = bpy.context.object
    light.rotation_euler = Vector((-6, 0, -6) if refracted else (4, 0, -6)).to_track_quat('-Z', 'Y').to_euler()
    light.data.energy = 6 if kind == 'SUN' else 1500
    if kind == 'SUN':
        light.data.angle = 0
    else:
        light.data.shadow_soft_size = 0
    samples = []
    for label, cutoff in (('lit', 2), ('blocked', -1)):
        threshold.inputs[1].default_value = cutoff
        path = directory / f'{kind}_{label}.exr'
        scene.render.filepath = str(path)
        print('SECONDARY_STATE', kind, label, flush=True)
        bpy.ops.render.render(write_still=True)
        image = bpy.data.images.load(str(path), check_existing=False)
        pixels = np.asarray(image.pixels[:]).reshape(64, 64, 4)
        samples.append(float(np.mean(pixels[24:40, 24:40, :3])))
        bpy.data.images.remove(image)
    print('SECONDARY_PIXELS', kind, samples, flush=True)
    assert samples[0] > .01 and samples[1] < samples[0] * .1, samples
    bpy.data.objects.remove(light, do_unlink=True)
