import sys
from pathlib import Path

import bpy
import gpu
import numpy as np


def check_scene(directory, forward):
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
    settings.use_fast_gi = False
    settings.use_raytracing = True
    settings.ray_tracing_method = 'SCREEN'
    settings.ray_tracing_options.use_hardware_shadows = True
    settings.hardware_raytracing_shadow_transparency = 0
    settings.hardware_raytracing_shadow_samples = 1
    bpy.ops.object.camera_add(location=(0, 0, 5))
    scene.camera = bpy.context.object
    scene.camera.data.type = 'ORTHO'
    scene.camera.data.ortho_scale = 2
    bpy.ops.object.light_add(type='SUN')
    bpy.context.object.data.angle = 0
    bpy.context.object.data.energy = 3
    bpy.ops.mesh.primitive_plane_add(size=4)
    receiver = bpy.context.object
    receiver_material = bpy.data.materials.new('Diffuse receiver')
    receiver_material.use_nodes = True
    receiver.data.materials.append(receiver_material)
    if forward:
        receiver_material.surface_render_method = 'BLENDED'
        receiver_material.node_tree.nodes['Principled BSDF'].inputs['Alpha'].default_value = .7
        receiver.visible_shadow = False
        front = receiver.copy()
        scene.collection.objects.link(front)
        front.location.z = .3
    bpy.ops.mesh.primitive_plane_add(size=2, location=(0, 0, 1))
    caster = bpy.context.object
    caster.visible_camera = False
    material = bpy.data.materials.new('Computed coverage')
    material.use_nodes = True
    material.use_transparent_shadow = True
    caster.data.materials.append(material)
    tree = material.node_tree
    alpha = tree.nodes.get('Principled BSDF').inputs['Alpha']
    coordinate = tree.nodes.new('ShaderNodeTexCoord')
    separate = tree.nodes.new('ShaderNodeSeparateXYZ')
    threshold = tree.nodes.new('ShaderNodeMath')
    threshold.operation = 'GREATER_THAN'
    threshold.inputs[1].default_value = .5
    tree.links.new(coordinate.outputs['UV'], separate.inputs[0])
    tree.links.new(separate.outputs[0], threshold.inputs[0])
    tree.links.new(threshold.outputs[0], alpha)

    def sample(label):
        path = directory / (label + '.exr')
        scene.render.filepath = str(path)
        bpy.ops.render.render(write_still=True)
        image = bpy.data.images.load(str(path), check_existing=False)
        pixels = np.asarray(image.pixels[:]).reshape(64, 64, 4)
        intensities = [float(np.mean(pixels[24:40, column:column + 6, 0])) for column in (12, 46)]
        bpy.data.images.remove(image)
        print('SHADOW_ALPHA', label, intensities, flush=True)
        return np.array(intensities)

    caster.hide_render = True
    reference = sample('open')
    assert min(reference) > .1, reference
    caster.hide_render = False
    cutout = sample('computed') / reference
    assert cutout[0] > .95 and cutout[1] < .05, cutout
    tree.links.new(separate.outputs[0], alpha)
    gradient = sample('gradient') / reference
    assert .69 < gradient[0] < .83 and .17 < gradient[1] < .31, gradient
    group = bpy.data.node_groups.new('Coverage curve', 'ShaderNodeTree')
    group.interface.new_socket(name='Coordinate', in_out='INPUT', socket_type='NodeSocketFloat')
    group.interface.new_socket(name='Coverage', in_out='OUTPUT', socket_type='NodeSocketFloat')
    entry = group.nodes.new('NodeGroupInput')
    output = group.nodes.new('NodeGroupOutput')
    ramp = group.nodes.new('ShaderNodeValToRGB')
    ramp.color_ramp.interpolation = 'CONSTANT'
    ramp.color_ramp.elements[1].position = .5
    group.links.new(entry.outputs[0], ramp.inputs[0])
    group.links.new(ramp.outputs[0], output.inputs[0])
    grouped = tree.nodes.new('ShaderNodeGroup')
    wrapper = bpy.data.node_groups.new('Nested coverage', 'ShaderNodeTree')
    wrapper.interface.new_socket(name='Coordinate', in_out='INPUT', socket_type='NodeSocketFloat')
    wrapper.interface.new_socket(name='Coverage', in_out='OUTPUT', socket_type='NodeSocketFloat')
    inner = wrapper.nodes.new('ShaderNodeGroup')
    inner.node_tree = group
    wrapper.links.new(wrapper.nodes.new('NodeGroupInput').outputs[0], inner.inputs[0])
    wrapper.links.new(inner.outputs[0], wrapper.nodes.new('NodeGroupOutput').inputs[0])
    grouped.node_tree = wrapper
    outer = tree.nodes.new('ShaderNodeMath')
    outer.operation = 'SUBTRACT'
    outer.inputs[0].default_value = 1
    tree.links.new(separate.outputs[0], outer.inputs[1])
    tree.links.new(outer.outputs[0], grouped.inputs[0])
    tree.links.new(grouped.outputs[0], alpha)
    inverted = sample('group_ramp_computed_uv') / reference
    assert inverted[0] < .05 and inverted[1] > .95, inverted
    colors = caster.data.color_attributes.new(name='shadow-color', type='FLOAT_COLOR', domain='CORNER')
    for loop, item in zip(caster.data.loops, colors.data):
        value = float(caster.data.vertices[loop.vertex_index].co.x > 0)
        item.color = (value, value, value, 1)
    attribute = tree.nodes.new('ShaderNodeVertexColor')
    attribute.layer_name = colors.name
    tree.links.new(attribute.outputs['Color'], grouped.inputs[0])
    caster.scale.x = -1
    vertex_shadow = sample('mirrored_vertex_color') / reference
    assert vertex_shadow[0] < .05 and vertex_shadow[1] > .95, vertex_shadow
    caster.scale.x = 1
    tree.links.remove(alpha.links[0])
    alpha.default_value = .25
    constant = sample('constant_quarter_coverage') / reference
    assert np.allclose(constant, .75, atol=.03), constant
    image = bpy.data.images.new('Split alpha', width=8, height=8, alpha=True, float_buffer=True)
    texels = np.ones((8, 8, 4), dtype=np.float32)
    texels[:, :4, 3] = .25
    texels[:, 4:, 3] = .75
    image.pixels.foreach_set(texels.ravel())
    image.update()
    image.pack()
    texture = tree.nodes.new('ShaderNodeTexImage')
    texture.image = image
    for interpolation in ('Closest', 'Linear'):
        texture.interpolation = interpolation
        tree.links.new(texture.outputs['Alpha'], alpha)
        direct = sample('image_direct_' + interpolation) / reference
        multiply = tree.nodes.new('ShaderNodeMath')
        multiply.operation = 'MULTIPLY'
        multiply.inputs[1].default_value = 1
        tree.links.new(texture.outputs['Alpha'], multiply.inputs[0])
        tree.links.new(multiply.outputs[0], alpha)
        replay = sample('image_replay_' + interpolation) / reference
        assert np.allclose(direct, [.75, .25], atol=.035), direct
        assert np.allclose(replay, direct, atol=.035), (replay, direct)
    tree.links.new(texture.outputs['Alpha'], alpha)
    mix = tree.nodes.new('ShaderNodeMixShader')
    mix.inputs[0].default_value = .5
    tree.links.new(tree.nodes['Principled BSDF'].outputs[0], mix.inputs[1])
    tree.links.new(tree.nodes.new('ShaderNodeBsdfDiffuse').outputs[0], mix.inputs[2])
    surface = tree.nodes['Material Output'].inputs['Surface']
    tree.links.new(mix.outputs[0], surface)
    mixed = sample('mixed_image_closure') / reference
    assert np.allclose(mixed, [.375, .125], atol=.035), mixed
    tree.links.new(tree.nodes['Principled BSDF'].outputs[0], surface)
    fine = bpy.data.images.new('Subpixel alpha stripes', width=512, height=16, alpha=True)
    stripes = np.ones((16, 512, 4), dtype=np.float32)
    stripes[:, ::2, 3] = 0
    fine.pixels.foreach_set(stripes.ravel())
    fine.update()
    fine.pack()
    texture.image = fine
    texture.interpolation = 'Linear'
    tree.links.new(texture.outputs['Alpha'], multiply.inputs[0])
    tree.links.new(multiply.outputs[0], alpha)
    for frame in (1, 2):
        scene.frame_set(frame)
        scene.camera.location.x += .0025
        filtered = sample(f'minified_{frame}') / reference
        assert np.allclose(filtered, .5, atol=.035), filtered
    scene.camera.location.x = 0
    scene.frame_set(1)
    for frame, opacity in ((1, .25), (2, .75)):
        values = np.ones((8, 8, 4), dtype=np.float32)
        values[:, :, 3] = opacity
        image.pixels.foreach_set(values.ravel())
        image.update()
        image.file_format = 'PNG'
        image.filepath_raw = str(directory / f'foliage_{frame:04}.png')
        image.save()
    texture.image = bpy.data.images.load(str(directory / 'foliage_0001.png'), check_existing=False)
    texture.image.source = 'SEQUENCE'
    texture.image_user.frame_duration = 2
    texture.image_user.use_auto_refresh = True
    for frame, expected in ((1, .75), (2, .25), (1, .75)):
        scene.frame_set(frame)
        animated = sample(f'animated_texture_{frame}') / reference
        assert np.allclose(animated, expected, atol=.035), (frame, animated)
    tree.links.new(coordinate.outputs['Window'], separate.inputs[0])
    tree.links.new(threshold.outputs[0], alpha)
    window = sample('camera_window') / reference
    assert window[0] > .95 and window[1] < .05, window
    tree.links.new(coordinate.outputs['UV'], separate.inputs[0])
    transparent = tree.nodes.new('ShaderNodeBsdfTransparent')
    transparent.inputs['Color'].default_value = (.9, .25, .05, 1)
    tree.links.new(transparent.outputs[0], mix.inputs[1])
    tree.links.new(threshold.outputs[0], mix.inputs[0])
    tree.links.new(mix.outputs[0], surface)
    settings.hardware_raytracing_shadow_color_intensity = 1
    sample('colored_transmission')
    tinted = bpy.data.images.load(str(directory / 'colored_transmission.exr'), check_existing=False)
    left = np.asarray(tinted.pixels[:]).reshape(64, 64, 4)[24:40, 12:18, :3].mean(axis=(0, 1))
    bpy.data.images.remove(tinted)
    assert left[0] > 2 * left[1] and left[1] > 2 * left[2], left
    tree.links.new(tree.nodes['Principled BSDF'].outputs[0], surface)
    tree.links.new(threshold.outputs[0], alpha)
    layers = []
    for index in range(10):
        duplicate = caster.copy()
        scene.collection.objects.link(duplicate)
        duplicate.location.z += .1 * (index + 1)
        layers.append(duplicate)
    bpy.ops.mesh.primitive_plane_add(size=2, location=(0, 0, 2.25))
    bpy.context.object.visible_camera = False
    behind = sample('wall_behind_eleven_cutouts') / reference
    assert max(behind) < .05, behind
    bpy.context.object.hide_render = True
    for duplicate in layers:
        duplicate.hide_render = True
    lamp = next(obj for obj in scene.objects if obj.type == 'LIGHT')
    lamp.data = bpy.data.lights.new('Soft spot', 'SPOT')
    lamp.location = (0, 0, 4)
    lamp.data.energy = 800
    lamp.data.spot_size = 1.5
    lamp.data.shadow_soft_size = .4
    settings.hardware_raytracing_shadow_samples = 16
    caster.hide_render = True
    lit = sample('spot_open')
    caster.hide_render = False
    blocked = sample('spot_cutout') / lit
    assert blocked[0] > .92 and blocked[1] < .08, blocked


directory = Path(sys.argv[sys.argv.index('--') + 1]).resolve()
directory.mkdir(parents=True, exist_ok=True)
check_scene(directory, 'forward' in sys.argv[sys.argv.index('--') + 2:])
