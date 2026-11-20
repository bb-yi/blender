"""Compare bounded tessellation draws with full-budget draws of identical visible geometry.

Loose, unrendered vertices enlarge the reference bounds to include the camera. They force the
original maximum expansion without changing triangles, normals or explicit object coordinates.
Run with an isolated user directory, BLENDER_TEST_OUTPUT_DIR outside the source repository,
and either --gpu-backend opengl or vulkan.
"""
import json
import os
from pathlib import Path

import bpy
import gpu
import numpy as np


OUT = Path(os.environ["BLENDER_TEST_OUTPUT_DIR"])
OUT.mkdir(parents=True, exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
gpu.init()
scene = bpy.context.scene
scene.render.engine = 'BLENDER_EEVEE'
scene.render.resolution_x = scene.render.resolution_y = 192
scene.render.resolution_percentage = 100
scene.render.film_transparent = True
scene.render.image_settings.file_format = 'OPEN_EXR'
scene.render.image_settings.color_mode = 'RGBA'
scene.eevee.taa_render_samples = 8
scene.world = bpy.data.worlds.new('Black')
cam = bpy.data.objects.new('Camera', bpy.data.cameras.new('Camera'))
scene.collection.objects.link(cam)
scene.camera = cam
cam.location = (0, 0, 18)
cam.data.type = 'ORTHO'
cam.data.ortho_scale = 6

# Sloped, smooth triangles expose missing subtriangles, wrong vertex indexing and Phong changes.
positions = [(-1.4,-1.4,0), (1.4,-1.4,0.25), (1.4,1.4,0), (-1.4,1.4,-0.3), (0,0,0.8)]
faces = [(0,1,4), (1,2,4), (2,3,4), (3,0,4)]
meshes = []
for full in (False, True):
    mesh = bpy.data.meshes.new('Full' if full else 'Bounded')
    mesh.from_pydata(positions + ([(-1000,-1000,-1000),(1000,1000,1000)] if full else []), [], faces)
    for face in mesh.polygons:
        face.use_smooth = True
    mesh.update()
    meshes.append(mesh)
ob = bpy.data.objects.new('Surface', meshes[0])
scene.collection.objects.link(ob)
mat = bpy.data.materials.new('Shared tessellation')
mat.use_nodes = True
mat.displacement_method = 'BOTH'
mat.max_vertex_displacement = 0.5
nt = mat.node_tree
nt.nodes.clear()
out = nt.nodes.new('ShaderNodeOutputMaterial')
emit = nt.nodes.new('ShaderNodeEmission')
geometry = nt.nodes.new('ShaderNodeNewGeometry')
nt.links.new(geometry.outputs['Normal'], emit.inputs['Color'])
nt.links.new(emit.outputs[0], out.inputs['Surface'])
texcoord = nt.nodes.new('ShaderNodeTexCoord')
noise = nt.nodes.new('ShaderNodeTexNoise')
noise.inputs['Scale'].default_value = 1.7
nt.links.new(texcoord.outputs['Object'], noise.inputs['Vector'])
disp = nt.nodes.new('ShaderNodeDisplacement')
disp.inputs['Scale'].default_value = 0.4
nt.links.new(noise.outputs['Fac'], disp.inputs['Height'])
nt.links.new(disp.outputs[0], out.inputs['Displacement'])
tess = nt.nodes.new('ShaderNodeTessellationControl')
tess.normal_mode = 'GEOMETRY'
nt.links.new(tess.outputs[0], out.inputs['Tessellation'])
for mesh in meshes:
    mesh.materials.append(mat)

def settings(mode, maximum=14, minimum=1, phong=0.6, edge=400):
    tess.mode = mode
    for socket in tess.inputs:
        values = {'Max Level':maximum, 'Min Level':minimum, 'Near Distance':2,
                  'Far Distance':12, 'Phong Smoothing':phong, 'Edge Length':edge}
        if socket.name in values:
            socket.default_value = values[socket.name]

def render(name, full, objects):
    for obj in objects:
        obj.data = meshes[int(full)]
    bpy.context.view_layer.update()
    bpy.ops.render.render()
    path = str(OUT / (name + ('_full' if full else '_bounded') + '.exr'))
    bpy.data.images['Render Result'].save_render(path, scene=scene)
    image = bpy.data.images.load(path, check_existing=False)
    pixels = np.empty(len(image.pixels), dtype=np.float32)
    image.pixels.foreach_get(pixels)
    bpy.data.images.remove(image)
    assert pixels.size == scene.render.resolution_x * scene.render.resolution_y * 4
    assert np.isfinite(pixels).all()
    assert np.count_nonzero(pixels.reshape(-1,4)[:,3] > 0.5) > 50, name
    rgba = pixels.reshape(-1,4)
    assert float(np.std(rgba[rgba[:,3] > 0.99,:3],axis=0).max()) > 0.005, name
    return pixels

results = {}
def compare(name, objects=None):
    objects = objects or [ob]
    bounded = render(name, False, objects)
    full = render(name, True, objects)
    delta = np.abs(bounded-full)
    results[name] = {'max':float(delta.max()), 'mean':float(delta.mean())}
    assert results[name]['max'] < 0.002 and results[name]['mean'] < 0.00002, (name,results[name])
    print('TESS_BUDGET',name,results[name],flush=True)
    return bounded

settings('DISTANCE')
compare('distance_far')
for z in (3.0, 6.0, 8.0, 10.0, 11.8, 12.2, 14.0):
    cam.location.z = z
    compare('distance_'+str(z))
cam.location.z = 18
settings('DISTANCE',minimum=2)
compare('distance_even_min')
settings('SCREEN_EDGE')
compare('screen_small')
settings('SCREEN_EDGE',edge=45)
compare('screen_intermediate')
cam.data.type = 'PERSP'
cam.location.z = 9
compare('screen_perspective')
cam.data.type = 'ORTHO'
cam.location.z = 18
settings('DISTANCE')
ob.scale = (-0.7, 1.1, 0.8)
ob.rotation_euler = (0.2,0.3,0.4)
compare('mirrored_nonuniform')
ob.scale = (0.55,0.55,0.55)
ob.location.x = -1.3
ob.rotation_euler = (0,0,0)
other = ob.copy()
other.data = ob.data
other.location = (1.3,0,12)
scene.collection.objects.link(other)
compare('shared_material_near_far',[ob,other])
bpy.data.objects.remove(other,do_unlink=True)
ob.location = (0,0,0)
ob.scale = (1,1,1)
settings('DISTANCE',maximum=1)
compare('level_one')
settings('FIXED',maximum=8)
compare('fixed_eight')
settings('DISTANCE')
without_npr = render('npr_disabled',False,[ob])
npr = bpy.data.node_groups.new('Tessellation NPR','ShaderNodeTree')
npr_geometry = npr.nodes.new('ShaderNodeNewGeometry')
npr_output = npr.nodes.new('ShaderNodeNPR_Output')
scale = npr.nodes.new('ShaderNodeVectorMath')
scale.operation = 'SCALE'
scale.inputs['Scale'].default_value = 0.25
offset = npr.nodes.new('ShaderNodeVectorMath')
offset.operation = 'ADD'
offset.inputs[1].default_value = (0.6,0.5,0.4)
npr.links.new(npr_geometry.outputs['Normal'],scale.inputs[0])
npr.links.new(scale.outputs[0],offset.inputs[0])
npr.links.new(offset.outputs[0],npr_output.inputs['Color'])
out.nprtree = npr
with_npr = compare('npr_distance')
assert float(np.abs(with_npr-without_npr).mean()) > 0.02, 'NPR must shade, not silently fall back'
settings('SCREEN_EDGE')
compare('npr_screen')
out.nprtree = None
settings('DISTANCE')
subdiv = ob.modifiers.new('Subdivision','SUBSURF')
subdiv.levels = subdiv.render_levels = 2
compare('subdivision_modifier')
ob.modifiers.remove(subdiv)
# Render Texture reuses the draw commands with another camera. A far main camera must not
# truncate the geometry needed by a nearby capture camera.
settings('DISTANCE')
cam.data.ortho_scale = 12
capture_camera = bpy.data.objects.new('Capture camera', cam.data.copy())
scene.collection.objects.link(capture_camera)
capture_camera.location = (0,0,3)
capture_camera.data.ortho_scale = 4
rt = scene.eevee.render_textures.add()
rt.enabled = True
rt.camera = capture_camera
rt.resolution_x = rt.resolution_y = 128
rt.source, rt.format, rt.update_mode = 'COLOR', 'RGBA16F', 'EVERY_FRAME'
bpy.ops.mesh.primitive_plane_add(size=3, location=(3.5,0,0))
display = bpy.context.object
display_material = bpy.data.materials.new('Capture display')
display_material.use_nodes = True
display.data.materials.append(display_material)
dn = display_material.node_tree.nodes
dl = display_material.node_tree.links
de = dn.new('ShaderNodeEmission')
sample = dn.new('ShaderNodeRenderTexture')
sample.render_texture = f'RENDER_TEXTURE_{rt.uid}'
uv = dn.new('ShaderNodeTexCoord')
dl.new(uv.outputs['UV'],sample.inputs['Vector'])
dl.new(sample.outputs['Color'],de.inputs['Color'])
dl.new(de.outputs[0],dn.get('Material Output').inputs['Surface'])
compare('render_texture_near_camera')
capture_camera.location.z = 18
compare('render_texture_far_camera')
settings('SCREEN_EDGE', edge=45)
capture_camera.data.ortho_scale = 2
compare('render_texture_screen_camera')
# The capture changes CameraData but the LOD shader still uses the main Film's pixel extent.
# A smaller output must not reduce the submitted lattice below that shader's required levels.
rt.resolution_x = rt.resolution_y = 32
compare('render_texture_small_screen_camera')
rt.enabled = False
bpy.data.objects.remove(display,do_unlink=True)
bpy.data.objects.remove(capture_camera,do_unlink=True)
cam.data.ortho_scale = 6
source_collection = bpy.data.collections.new('Instance source')
scene.collection.objects.unlink(ob)
source_collection.objects.link(ob)
for x,z in ((-1.4,0),(1.4,12)):
    inst = bpy.data.objects.new('Collection instance',None)
    inst.instance_type = 'COLLECTION'
    inst.instance_collection = source_collection
    inst.location = (x,0,z)
    inst.scale = (0.5,0.5,0.5)
    scene.collection.objects.link(inst)
compare('collection_instances')
(OUT / (gpu.platform.backend_type_get().lower()+'.json')).write_text(json.dumps(results,indent=2))
print('TESS_BUDGET_PASS',len(results),flush=True)
