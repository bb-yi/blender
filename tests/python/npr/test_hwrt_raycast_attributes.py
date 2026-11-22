"""Sample named attributes on the committed hardware Raycast triangle."""

import bpy
import gpu


def test_raycast_attributes():
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    assert scene.eevee.is_hardware_raytracing_supported, 'Vulkan ray queries are unavailable'
    scene.eevee.use_raytracing = False
    scene.eevee.taa_samples = 1
    scene.eevee.use_taa_reprojection = False
    for obj in list(scene.objects):
        bpy.data.objects.remove(obj, do_unlink=True)

    camera = bpy.data.objects.new('Camera', bpy.data.cameras.new('Camera'))
    camera.data.type = 'ORTHO'
    camera.data.ortho_scale = 1
    camera.location.z = 3
    scene.collection.objects.link(camera)
    scene.camera = camera

    material = bpy.data.materials.new('Attribute receiver')
    material.use_nodes = True
    tree = material.node_tree
    tree.nodes.clear()
    ray = tree.nodes.new('ShaderNodeRaycast')
    ray.tracing_mode = 'HARDWARE'
    position = tree.nodes.new('ShaderNodeCombineXYZ')
    position.inputs['X'].default_value = 10.5
    position.inputs['Y'].default_value = .5
    position.inputs['Z'].default_value = 2
    tree.links.new(position.outputs[0], ray.inputs['Position'])
    direction = tree.nodes.new('ShaderNodeCombineXYZ')
    direction.inputs['Z'].default_value = -1
    tree.links.new(direction.outputs[0], ray.inputs['Direction'])
    ray.inputs['Length'].default_value = 3
    for name, socket_type in [('Weight', 'FLOAT'), ('UV', 'VECTOR'), ('Paint', 'RGBA'),
                              ('Byte', 'RGBA')]:
        ray.sample_attribute_items.new(socket_type, name)
        ray.inputs[name].default_value = name
    ray.inputs['Weight'].default_value = 'Ab'
    ray.inputs['Paint'].default_value = 'BA'
    pack = tree.nodes.new('ShaderNodeCombineXYZ')
    tree.links.new(ray.outputs['Weight'], pack.inputs['X'])
    tree.links.new(ray.outputs['Paint Alpha'], pack.inputs['Y'])
    tree.links.new(ray.outputs['Byte Alpha'], pack.inputs['Z'])
    emission = tree.nodes.new('ShaderNodeEmission')
    output = tree.nodes.new('ShaderNodeOutputMaterial')
    tree.links.new(emission.outputs[0], output.inputs['Surface'])

    source_mesh = bpy.data.meshes.new('Receiver mesh')
    source_mesh.from_pydata([(-2, -2, 1), (2, -2, 1), (0, 2, 1)], [], [(0, 1, 2)])
    source_mesh.materials.append(material)
    source = bpy.data.objects.new('Receiver', source_mesh)
    scene.collection.objects.link(source)

    mesh = bpy.data.meshes.new('Two attribute triangles')
    mesh.from_pydata([(0, 0, 0), (2, 0, 0), (0, 2, 0), (2, 2, 0)], [],
                     [(0, 1, 2), (1, 3, 2)])
    mesh.materials.append(bpy.data.materials.new('Target A'))
    mesh.materials.append(bpy.data.materials.new('Target B'))
    mesh.polygons[1].material_index = 1
    weight = mesh.attributes.new('Ab', 'FLOAT', 'POINT')
    weight.data.foreach_set('value', (0.2, 0.6, 0.4, 0.8))
    uv = mesh.attributes.new('UV', 'FLOAT2', 'CORNER')
    uv.data.foreach_set('vector', (0, 0, 1, 0, 0, 1, .8, .4, .8, .4, .8, .4))
    paint = mesh.attributes.new('BA', 'FLOAT_COLOR', 'FACE')
    paint.data.foreach_set('color', (.1, .3, .6, .7, .8, .4, .2, .3))
    byte = mesh.attributes.new('Byte', 'BYTE_COLOR', 'CORNER')
    byte.data.foreach_set('color_srgb', (.5, .5, .5, .4) * 6)
    mesh.update()
    first = bpy.data.objects.new('Off-screen target', mesh)
    first.location.x = 10
    scene.collection.objects.link(first)
    second = bpy.data.objects.new('Off-screen linked instance', mesh)
    second.location.x = 20
    scene.collection.objects.link(second)

    area = next(area for area in bpy.context.screen.areas if area.type == 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    space = area.spaces.active
    space.shading.type = 'RENDERED'
    space.shading.use_compositor = 'DISABLED'
    space.overlay.show_overlays = False
    space.camera = camera
    resolution = 48
    target = gpu.types.GPUOffScreen(resolution, resolution, format='RGBA16F')

    def check(label, socket, expected):
        tree.links.new(socket, emission.inputs['Color'])
        bpy.context.view_layer.update()
        graph = bpy.context.evaluated_depsgraph_get()
        view = camera.evaluated_get(graph).matrix_world.inverted()
        projection = camera.calc_matrix_camera(graph, x=resolution, y=resolution)
        target.draw_view3d(scene, bpy.context.view_layer, space, region, view, projection,
                           do_color_management=False, draw_background=True)
        with target.bind():
            pixel = gpu.state.active_framebuffer_get().read_color(24, 24, 1, 1, 4, 0, 'FLOAT')
        rgb = tuple(pixel[0][0])[:3]
        print(label, rgb, flush=True)
        assert all(abs(value - ref) < .015 for value, ref in zip(rgb, expected)), (label, rgb, expected)

    try:
        check('Shared query samples Ab float and BA Alpha separately', pack.outputs[0], (.35, .7, .4))
        check('Corner UV interpolation', ray.outputs['UV'], (.25, .25, 0))
        check('Face color', ray.outputs['Paint'], (.1, .3, .6))
        check('Byte color decodes to linear', ray.outputs['Byte'], (.216,) * 3)
        position.inputs['X'].default_value = 21.5
        position.inputs['Y'].default_value = 1.5
        check('Instance and second material batch', pack.outputs[0], (.65, .3, .4))
        check('Corner UV across seam', ray.outputs['UV'], (.8, .4, 0))
        ray.only_local = True
        check('Only Local rejects other instances', pack.outputs[0], (0, 0, 0))
        ray.only_local = False
        mesh.attributes['Ab'].data.foreach_set('value', (.9,) * 4)
        mesh.update()
        check('Attribute values update', pack.outputs[0], (.9, .3, .4))
        mesh.attributes['Ab'].name = 'Renamed'
        mesh.update()
        ray.inputs['Paint'].default_value = 'Absent'
        check('Missing float and missing Alpha', pack.outputs[0], (0, 1, .4))
        ray.inputs['Weight'].default_value = 'Renamed'
        check('Renamed attribute request', pack.outputs[0], (.9, 1, .4))
        second.location.z = -.25
        check('Transform-only update reuses the attributes', pack.outputs[0], (.9, 1, .4))
        check('Missing attribute with no allocated table', ray.outputs['Paint Alpha'], (1, 1, 1))
        check('Disconnect last attribute output', ray.outputs['Is Hit'], (1, 1, 1))
        ray.tracing_mode = 'SCREEN'
        check('Screen-space fallback', pack.outputs[0], (0, 0, 0))
    finally:
        target.free()


if __name__ == '__main__':
    gpu.init()
    test_raycast_attributes()
