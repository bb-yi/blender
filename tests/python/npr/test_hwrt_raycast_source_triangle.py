"""Keep close hits on other triangles of the same mesh after skipping the source triangle."""

import sys

import bpy
import gpu


def test_source_triangle():
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    if not scene.eevee.is_hardware_raytracing_supported:
        print('Skipping hardware Raycast test: Vulkan ray queries are unavailable')
        return

    scene.eevee.use_raytracing = False
    scene.eevee.taa_samples = 1
    scene.eevee.use_taa_reprojection = False
    for obj in list(scene.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    camera = bpy.data.objects.new('Camera', bpy.data.cameras.new('Camera'))
    camera.data.type = 'ORTHO'
    camera.data.ortho_scale = 1
    camera.location.z = 2
    scene.collection.objects.link(camera)
    scene.camera = camera

    material = bpy.data.materials.new('Raycast source')
    material.use_nodes = True
    material.use_backface_culling = True
    tree = material.node_tree
    tree.nodes.clear()
    geom = tree.nodes.new('ShaderNodeNewGeometry')
    ray = tree.nodes.new('ShaderNodeRaycast')
    ray.tracing_mode = 'HARDWARE'
    gap = 2.0 ** -22
    length = gap * 4
    direction = tree.nodes.new('ShaderNodeCombineXYZ')
    direction.inputs['Z'].default_value = -1
    tree.links.new(direction.outputs[0], ray.inputs['Direction'])
    ray.inputs['Length'].default_value = length
    tree.links.new(geom.outputs['Position'], ray.inputs['Position'])

    pack = tree.nodes.new('ShaderNodeCombineXYZ')
    scale = tree.nodes.new('ShaderNodeMath')
    scale.operation = 'MULTIPLY'
    scale.inputs[1].default_value = 1 / length
    tree.links.new(ray.outputs['Is Hit'], pack.inputs[0])
    tree.links.new(ray.outputs['Self Hit'], pack.inputs[1])
    tree.links.new(ray.outputs['Hit Distance'], scale.inputs[0])
    tree.links.new(scale.outputs[0], pack.inputs[2])
    emission = tree.nodes.new('ShaderNodeEmission')
    tree.links.new(pack.outputs[0], emission.inputs['Color'])
    output = tree.nodes.new('ShaderNodeOutputMaterial')
    tree.links.new(emission.outputs[0], output.inputs['Surface'])
    second_material = material.copy()

    source = bpy.data.objects.new('Source', bpy.data.meshes.new('Empty'))
    scene.collection.objects.link(source)

    def put_mesh(rear_slot=None):
        mesh = bpy.data.meshes.new('Raycast triangles')
        vertices = [(-1, -1, 1), (1, -1, 1), (0, 1.5, 1)]
        faces = [(0, 1, 2)]
        if rear_slot is not None:
            vertices.extend((x, y, 1 - gap) for x, y, _ in vertices[:3])
            faces.append((5, 4, 3))
        mesh.from_pydata(vertices, [], faces)
        mesh.materials.append(material)
        mesh.materials.append(second_material)
        if rear_slot is not None:
            mesh.polygons[1].material_index = rear_slot
        mesh.update()
        source.data = mesh

    area = next(area for area in bpy.context.screen.areas if area.type == 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    space = area.spaces.active
    space.shading.type = 'RENDERED'
    space.shading.use_scene_lights_render = True
    space.shading.use_scene_world_render = True
    space.shading.use_compositor = 'DISABLED'
    space.overlay.show_overlays = False
    space.camera = camera
    space.region_3d.view_perspective = 'CAMERA'
    resolution = 96
    target = gpu.types.GPUOffScreen(resolution, resolution, format='RGBA16F')

    def check(label, expected):
        bpy.context.view_layer.update()
        graph = bpy.context.evaluated_depsgraph_get()
        view = camera.evaluated_get(graph).matrix_world.inverted()
        projection = camera.calc_matrix_camera(graph, x=resolution, y=resolution)
        target.draw_view3d(scene, bpy.context.view_layer, space, region, view, projection,
                           do_color_management=False, draw_background=True)
        with target.bind():
            pixel = gpu.state.active_framebuffer_get().read_color(49, 47, 1, 1, 4, 0, 'FLOAT')
        rgb = tuple(pixel[0][0])[:3]
        print(label, rgb, flush=True)
        assert all(abs(value - ref) < .02 for value, ref in zip(rgb, expected)), (label, rgb, expected)

    try:
        put_mesh()
        check('Only the source triangle, global query', (0, 0, 1))
        ray.only_local = True
        check('Only the source triangle, local query', (0, 0, 1))
        put_mesh(rear_slot=0)
        check('Nearby triangle in the same batch', (1, 1, .25))
        put_mesh(rear_slot=1)
        check('Nearby triangle with the same primitive ID in a different batch', (1, 1, .25))
        ray.only_local = False
        check('Nearby triangle, global query', (1, 1, .25))
        put_mesh()
        shift = tree.nodes.new('ShaderNodeVectorMath')
        shift.operation = 'ADD'
        shift.inputs[1].default_value = (0, 0, gap)
        tree.links.new(geom.outputs['Position'], shift.inputs[0])
        tree.links.new(shift.outputs[0], ray.inputs['Position'])
        check('Explicitly offset origin can hit the original triangle', (1, 1, .25))
    finally:
        target.free()


if __name__ == '__main__':
    gpu.init()
    test_source_triangle()
