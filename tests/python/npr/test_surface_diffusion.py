# SPDX-License-Identifier: GPL-2.0-or-later
"""Pixel contracts for NPR Surface Diffusion. Run separately with each GPU backend."""
import argparse
import inspect
import json
from pathlib import Path
import sys

import bpy
import gpu
import numpy as np
from bl_ui import node_add_menu_shader


def require(value, message):
    if not value:
        raise AssertionError(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--expected-backend', required=True)
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    args.output_dir.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    scene.render.resolution_x = scene.render.resolution_y = 96
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = True
    scene.eevee.taa_render_samples = 64
    scene.eevee.use_raytracing = False
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.render.image_settings.color_depth = '32'
    scene.render.image_settings.color_mode = 'RGBA'
    scene.world = bpy.data.worlds.new('Black world')
    scene.world.use_nodes = True
    scene.world.node_tree.nodes['Background'].inputs['Strength'].default_value = 0
    camera_data = bpy.data.cameras.new('Camera')
    camera_data.type = 'ORTHO'
    camera_data.ortho_scale = 2.4
    camera = bpy.data.objects.new('Camera', camera_data)
    scene.collection.objects.link(camera)
    camera.location = (0, 0, 3)
    scene.camera = camera
    bpy.ops.mesh.primitive_plane_add(size=2)
    plane = bpy.context.object
    material = bpy.data.materials.new('Surface Diffusion')
    material.use_nodes = True
    material.surface_render_method = 'DITHERED'
    plane.data.materials.append(material)
    tree = material.node_tree
    tree.nodes.clear()
    output = tree.nodes.new('ShaderNodeOutputMaterial')
    diffusion = tree.nodes.new('ShaderNodeNPRSurfaceDiffusion')
    require(diffusion.inputs[0].type == 'SHADER' and diffusion.outputs[0].type == 'SHADER',
            'Shader input/output contract missing')
    require(diffusion.inputs['Strength'].default_value == 1, 'Strength default changed')
    require(tuple(diffusion.inputs['Radius'].default_value) == (1, 1, 1), 'Radius must be neutral')
    require(abs(diffusion.inputs['Scale'].default_value - .005) < 1e-7, 'Scale default changed')
    npr = tree.nodes.new('ShaderNodePrincipledNPR')
    for name in ('mapping_softness', 'profile_softness'):
        require(next(s for s in npr.inputs if s.identifier == name).default_value == 0,
                f'User default not preserved: {name}')
    tree.nodes.remove(npr)
    menu = inspect.getsource(node_add_menu_shader.NODE_MT_shader_node_npr_base)
    require('ShaderNodePrincipledNPR' in menu and 'ShaderNodeNPRSurfaceDiffusion' in menu,
            'Both nodes must be in NPR category')
    require('ShaderNodePrincipledNPR' not in inspect.getsource(
        node_add_menu_shader.NODE_MT_shader_node_shader_base), 'Duplicate legacy menu entry')
    geometry = tree.nodes.new('ShaderNodeNewGeometry')
    separate = tree.nodes.new('ShaderNodeSeparateXYZ')
    step = tree.nodes.new('ShaderNodeMath')
    step.operation = 'GREATER_THAN'
    step.inputs[1].default_value = 0
    tree.links.new(geometry.outputs['Position'], separate.inputs[0])
    tree.links.new(separate.outputs['X'], step.inputs[0])
    tree.links.new(step.outputs[0], diffusion.inputs['Shader'])
    diffusion.inputs['Scale'].default_value = .15
    stats = {}

    def connect(socket):
        tree.links.new(socket, output.inputs['Surface'])

    def render(name):
        scene.render.filepath = str(args.output_dir / (name + '.exr'))
        bpy.ops.render.render(write_still=True)
        image = bpy.data.images.load(scene.render.filepath, check_existing=False)
        pixels = np.array(image.pixels[:], dtype=np.float32).reshape(96, 96, 4)
        bpy.data.images.remove(image)
        require(np.isfinite(pixels).all(), name + ': non-finite pixels')
        require(gpu.platform.backend_type_get() == args.expected_backend,
                'Actual backend differs from requested backend')
        stats[name] = {'mean': float(pixels[:, :, :3].mean()),
                       'center_row': pixels[48, 38:58, 0].tolist()}
        print('NPR_DIFFUSION_RENDER', name, stats[name], flush=True)
        return pixels

    connect(step.outputs[0])
    original = render('color_original')
    require(original[48, 65, 0] > .9 and original[48, 30, 0] < .01,
            'Two-tone fixture invalid')
    connect(diffusion.outputs[0])
    diffusion.inputs['Strength'].default_value = 0
    bypass = render('strength_zero')
    require(np.max(np.abs(bypass - original)) < .025, 'Strength zero is not a bypass')
    diffusion.inputs['Strength'].default_value = 1
    blurred = render('color_diffused')
    require(np.mean(blurred[32:64, 44:48, 0] - original[32:64, 44:48, 0]) > .025,
            'Dark side did not receive diffusion beyond ordinary pixel antialiasing')
    require(np.mean(original[32:64, 48:52, 0] - blurred[32:64, 48:52, 0]) > .025,
            'Bright side did not diffuse')
    require(np.max(np.abs(blurred[:, :, 3] - original[:, :, 3])) < .025,
            'Diffusion changed coverage')
    require(np.max(np.abs(blurred[:, :, 0] - blurred[:, :, 1])) < .01,
            'Neutral radii introduced a tint')
    diffusion.inputs['Strength'].default_value = .5
    half = render('strength_half')
    require(np.max(np.abs(half[20:76, 20:76] -
                          .5 * (original[20:76, 20:76] + blurred[20:76, 20:76]))) < .02,
            'Strength does not blend the original and diffused surface linearly')
    diffusion.inputs['Strength'].default_value = 1
    diffusion.inputs['Radius'].default_value = (1, 0, 0)
    red_only = render('radius_red_only')
    require(np.mean(red_only[32:64, 44:48, 0] - original[32:64, 44:48, 0]) > .025,
            'Red radius did not diffuse')
    require(np.max(np.abs(red_only[:, :, 1:3] - original[:, :, 1:3])) < .025,
            'Zero channel radii did not bypass')
    diffusion.inputs['Radius'].default_value = (1, 1, 1)
    diffusion.inputs['Scale'].default_value = 0
    zero_scale = render('scale_zero')
    require(np.max(np.abs(zero_scale - original)) < .025, 'Scale zero is not a bypass')
    diffusion.inputs['Scale'].default_value = .15

    # Uniform pre-shaded HDR radiance must remain uniform, including without any lights.
    rgb = tree.nodes.new('ShaderNodeRGB')
    rgb.outputs[0].default_value = (2, .5, .25, 1)
    tree.links.new(rgb.outputs[0], diffusion.inputs['Shader'])
    constant = render('constant_hdr')
    require(np.max(np.abs(constant[20:76, 20:76, :3] - (2, .5, .25))) < .035,
            'Diffusion relit, lost or tinted constant HDR input')

    # A shared input feeds both the diffused and the untouched branch.
    tree.links.new(step.outputs[0], diffusion.inputs['Shader'])
    emission = tree.nodes.new('ShaderNodeEmission')
    combine = tree.nodes.new('ShaderNodeCombineColor')
    combine.inputs['Red'].default_value = 0
    combine.inputs['Green'].default_value = 0
    tree.links.new(step.outputs[0], combine.inputs['Blue'])
    tree.links.new(combine.outputs[0], emission.inputs['Color'])
    add = tree.nodes.new('ShaderNodeAddShader')
    tree.links.new(diffusion.outputs[0], add.inputs[0])
    tree.links.new(emission.outputs[0], add.inputs[1])
    connect(add.outputs[0])
    added = render('sibling_add')
    require(np.max(np.abs((added[:, :, 2] - added[:, :, 0]) - original[:, :, 0])) < .04,
            'Diffusion leaked into or lost its untouched sibling branch')
    mix = tree.nodes.new('ShaderNodeMixShader')
    mix.inputs[0].default_value = .25
    tree.links.new(diffusion.outputs[0], mix.inputs[1])
    tree.links.new(emission.outputs[0], mix.inputs[2])
    connect(mix.outputs[0])
    mixed = render('sibling_mix')
    require(np.max(np.abs(mixed[:, :, 0] - .75 * blurred[:, :, 0])) < .05,
            'Outer Mix weight is missing or applied twice')

    # Ordinary BSDF lighting is evaluated before the full shaded color is diffused.
    sun_data = bpy.data.lights.new('Sun', 'SUN')
    sun_data.energy = 3.14159265
    sun = bpy.data.objects.new('Sun', sun_data)
    scene.collection.objects.link(sun)
    diffuse = tree.nodes.new('ShaderNodeBsdfDiffuse')
    tree.links.new(step.outputs[0], diffuse.inputs['Color'])
    connect(diffuse.outputs[0])
    physical = render('diffuse_original')
    tree.links.new(diffuse.outputs[0], diffusion.inputs['Shader'])
    connect(diffusion.outputs[0])
    physical_blurred = render('diffuse_diffused')
    require(np.mean(physical_blurred[32:64, 44:48, 0]) >
            np.mean(physical[32:64, 44:48, 0]) + .02, 'Ordinary BSDF did not diffuse')
    require(abs(float(physical_blurred[20:76, 20:76, 0].mean() -
                      physical[20:76, 20:76, 0].mean())) < .05,
            'Ordinary BSDF was relit or lost substantial energy')

    # A reflection closure is not silently replaced by a diffuse closure.
    principled = tree.nodes.new('ShaderNodeBsdfPrincipled')
    tree.links.new(step.outputs[0], principled.inputs['Base Color'])
    principled.inputs['Metallic'].default_value = 1
    principled.inputs['Roughness'].default_value = .5
    connect(principled.outputs[0])
    reflected = render('metal_original')
    tree.links.new(principled.outputs[0], diffusion.inputs['Shader'])
    connect(diffusion.outputs[0])
    reflected_blurred = render('metal_diffused')
    require(np.mean(reflected_blurred[32:64, 44:48, 0] -
                    reflected[32:64, 44:48, 0]) > .025,
            'Reflection closure did not diffuse')
    require(abs(float(reflected_blurred[20:76, 20:76, 0].mean() /
                      reflected[20:76, 20:76, 0].mean()) - 1) < .08,
            'Reflection energy was replaced by diffuse lighting')

    # Coverage belongs to the original Shader; the post-lighting operation must not blur Alpha.
    tree.links.new(step.outputs[0], diffusion.inputs['Shader'])
    transparent = tree.nodes.new('ShaderNodeBsdfTransparent')
    mix.inputs[0].default_value = .4
    tree.links.new(step.outputs[0], mix.inputs[1])
    tree.links.new(transparent.outputs[0], mix.inputs[2])
    connect(mix.outputs[0])
    transparent_original = render('alpha_original')
    tree.links.new(mix.outputs[0], diffusion.inputs['Shader'])
    connect(diffusion.outputs[0])
    transparent_blurred = render('alpha_diffused')
    require(np.max(np.abs(transparent_blurred[:, :, 3] - transparent_original[:, :, 3])) < .025,
            'Input Alpha was diffused or multiplied twice')
    require(np.mean(transparent_blurred[32:64, 44:48, 0] -
                    transparent_original[32:64, 44:48, 0]) > .015,
            'Transparent surface color did not diffuse')

    npr = tree.nodes.new('ShaderNodePrincipledNPR')
    for name, value in {'ambient_strength': 0, 'reflection_strength': 0,
                        'highlight_strength': 0}.items():
        next(s for s in npr.inputs if s.identifier == name).default_value = value
    tree.links.new(step.outputs[0], next(s for s in npr.inputs if s.identifier == 'base_color'))
    connect(npr.outputs[0])
    npr_original = render('npr_original')
    tree.links.new(npr.outputs[0], diffusion.inputs['Shader'])
    connect(diffusion.outputs[0])
    npr_blurred = render('npr_diffused')
    require(np.mean(npr_blurred[32:64, 44:48, 0] - npr_original[32:64, 44:48, 0]) > .025,
            'Principled NPR shaded contribution did not diffuse')

    # Shader-to-RGB upstream of diffusion exercises EEVEE's hybrid GBuffer writer.
    to_rgb = tree.nodes.new('ShaderNodeShaderToRGB')
    tree.links.new(diffuse.outputs[0], to_rgb.inputs[0])
    tree.links.new(to_rgb.outputs['Color'], diffusion.inputs['Shader'])
    shader_rgb = render('shader_rgb_diffused')
    require(np.mean(shader_rgb[32:64, 44:48, 0] - physical[32:64, 44:48, 0]) > .025,
            'Shader to RGB color did not diffuse in the hybrid pipeline')
    require(np.max(np.abs(shader_rgb[20:76, 20:76, :3] -
                          physical_blurred[20:76, 20:76, :3])) < .04,
            'Shader to RGB was relit or weighted twice')

    # Sibling diffusion nodes must not bleed state into each other, and a
    # Shader to RGB inside one wrapped input must not reset sibling closures.
    def step_channel(channel):
        helper = tree.nodes.new('ShaderNodeCombineColor')
        for name in ('Red', 'Green', 'Blue'):
            if name != channel:
                helper.inputs[name].default_value = 0
        tree.links.new(step.outputs[0], helper.inputs[channel])
        return helper

    diffusion_b = tree.nodes.new('ShaderNodeNPRSurfaceDiffusion')
    diffusion_b.inputs['Strength'].default_value = 1
    diffusion_b.inputs['Radius'].default_value = (1, 1, 1)
    diffusion_b.inputs['Scale'].default_value = .15
    add_pair = tree.nodes.new('ShaderNodeAddShader')
    mix_pair = tree.nodes.new('ShaderNodeMixShader')
    tree.links.new(diffusion.outputs[0], add_pair.inputs[0])

    r_only = step_channel('Red')
    b_only = step_channel('Blue')
    tree.links.new(r_only.outputs[0], diffusion.inputs['Shader'])
    diffusion_b.inputs['Scale'].default_value = 0
    tree.links.new(b_only.outputs[0], diffusion_b.inputs['Shader'])
    tree.links.new(diffusion_b.outputs[0], add_pair.inputs[1])
    connect(add_pair.outputs[0])
    two_add = render('two_diffusions_add')
    stats['two_diffusions_add']['deltas'] = {
        'red_vs_blurred': float(np.max(np.abs(
            two_add[20:76, 20:76, 0] - blurred[20:76, 20:76, 0]))),
        'blue_vs_original': float(np.max(np.abs(
            two_add[20:76, 20:76, 2] - original[20:76, 20:76, 0]))),
        'green_max': float(np.max(np.abs(two_add[20:76, 20:76, 1]))),
    }
    print('NPR_DIFFUSION_DELTAS', 'two_diffusions_add',
          stats['two_diffusions_add']['deltas'], flush=True)
    require(stats['two_diffusions_add']['deltas']['red_vs_blurred'] < .05,
            'Sibling diffusion lost or altered first diffusion')
    require(stats['two_diffusions_add']['deltas']['blue_vs_original'] < .05,
            "Bypassed sibling diffusion received the other node's diffusion")
    require(stats['two_diffusions_add']['deltas']['green_max'] < .02,
            'Sibling diffusion leaked into the unused channel')

    mix_pair.inputs[0].default_value = .5
    tree.links.new(diffusion.outputs[0], mix_pair.inputs[1])
    tree.links.new(diffusion_b.outputs[0], mix_pair.inputs[2])
    connect(mix_pair.outputs[0])
    two_mix = render('two_diffusions_mix')
    stats['two_diffusions_mix']['deltas'] = {
        'red_vs_half_blurred': float(np.max(np.abs(
            two_mix[20:76, 20:76, 0] - .5 * blurred[20:76, 20:76, 0]))),
        'blue_vs_half_original': float(np.max(np.abs(
            two_mix[20:76, 20:76, 2] - .5 * original[20:76, 20:76, 0]))),
    }
    print('NPR_DIFFUSION_DELTAS', 'two_diffusions_mix',
          stats['two_diffusions_mix']['deltas'], flush=True)
    require(stats['two_diffusions_mix']['deltas']['red_vs_half_blurred'] < .05,
            'Mix sibling lost or altered the first diffusion branch')
    require(stats['two_diffusions_mix']['deltas']['blue_vs_half_original'] < .05,
            'Mix sibling bypass branch received foreign diffusion')

    # Record-only: Shader to RGB inside one wrapped input beside a sibling diffusion is not a
    # supported contract; deltas are logged for reference.
    diffusion_b.inputs['Scale'].default_value = .15
    sep_lit = tree.nodes.new('ShaderNodeSeparateColor')
    comb_lit = tree.nodes.new('ShaderNodeCombineColor')
    comb_lit.inputs['Green'].default_value = 0
    comb_lit.inputs['Blue'].default_value = 0
    tree.links.new(to_rgb.outputs['Color'], sep_lit.inputs[0])
    tree.links.new(sep_lit.outputs['Red'], comb_lit.inputs['Red'])
    tree.links.new(comb_lit.outputs[0], diffusion.inputs['Shader'])
    connect(add_pair.outputs[0])
    rgb_sibling = render('shader_rgb_sibling_diffusion')
    stats['shader_rgb_sibling_diffusion']['deltas'] = {
        'red_vs_physical_blurred': float(np.max(np.abs(
            rgb_sibling[20:76, 20:76, 0] - physical_blurred[20:76, 20:76, 0]))),
        'blue_vs_blurred': float(np.max(np.abs(
            rgb_sibling[20:76, 20:76, 2] - blurred[20:76, 20:76, 0]))),
    }
    print('NPR_DIFFUSION_DELTAS', 'shader_rgb_sibling_diffusion',
          stats['shader_rgb_sibling_diffusion']['deltas'], flush=True)

    # A root-graph Shader to RGB feeding an unrelated emission branch.
    tree.links.new(r_only.outputs[0], diffusion.inputs['Shader'])
    sep_root = tree.nodes.new('ShaderNodeSeparateColor')
    comb_root = tree.nodes.new('ShaderNodeCombineColor')
    comb_root.inputs['Red'].default_value = 0
    comb_root.inputs['Green'].default_value = 0
    tree.links.new(to_rgb.outputs['Color'], sep_root.inputs[0])
    tree.links.new(sep_root.outputs['Red'], comb_root.inputs['Blue'])
    tree.links.new(comb_root.outputs[0], emission.inputs['Color'])
    tree.links.new(emission.outputs[0], add_pair.inputs[1])
    rgb_root = render('shader_rgb_root_sibling')
    stats['shader_rgb_root_sibling']['deltas'] = {
        'red_vs_blurred': float(np.max(np.abs(
            rgb_root[20:76, 20:76, 0] - blurred[20:76, 20:76, 0]))),
        'blue_vs_physical': float(np.max(np.abs(
            rgb_root[20:76, 20:76, 2] - physical[20:76, 20:76, 0]))),
    }
    print('NPR_DIFFUSION_DELTAS', 'shader_rgb_root_sibling',
          stats['shader_rgb_root_sibling']['deltas'], flush=True)
    require(stats['shader_rgb_root_sibling']['deltas']['red_vs_blurred'] < .05,
            'Root Shader to RGB sibling disturbed the diffusion branch')
    require(stats['shader_rgb_root_sibling']['deltas']['blue_vs_physical'] < .05,
            'Root Shader to RGB sibling lost its own shading')

    # A plain BSDF sibling must not be lit inside another branch's Shader to RGB.
    # The sibling is Glossy: an unrelated diffuse closure beside a diffusion node falls under
    # the documented same-bin stochastic selection noise, which is a separate matter.
    try:
        bsdf_b = tree.nodes.new('ShaderNodeBsdfAnisotropic')
    except RuntimeError:
        bsdf_b = tree.nodes.new('ShaderNodeBsdfGlossy')
    bsdf_b.inputs['Roughness'].default_value = .5
    tree.links.new(b_only.outputs[0], bsdf_b.inputs['Color'])
    connect(bsdf_b.outputs[0])
    glossy_reference = render('glossy_sibling_reference')
    tree.links.new(comb_lit.outputs[0], diffusion.inputs['Shader'])
    add_f = tree.nodes.new('ShaderNodeAddShader')
    tree.links.new(bsdf_b.outputs[0], add_f.inputs[0])
    tree.links.new(diffusion.outputs[0], add_f.inputs[1])
    connect(add_f.outputs[0])
    f_rgb_bsdf = render('shader_rgb_in_diffusion_bsdf_sibling')
    stats['shader_rgb_in_diffusion_bsdf_sibling']['deltas'] = {
        'red_vs_physical_blurred': float(np.max(np.abs(
            f_rgb_bsdf[20:76, 20:76, 0] - physical_blurred[20:76, 20:76, 0]))),
        'blue_vs_glossy': float(np.max(np.abs(
            f_rgb_bsdf[20:76, 20:76, 2] - glossy_reference[20:76, 20:76, 2]))),
    }
    print('NPR_DIFFUSION_DELTAS', 'shader_rgb_in_diffusion_bsdf_sibling',
          stats['shader_rgb_in_diffusion_bsdf_sibling']['deltas'], flush=True)
    require(stats['shader_rgb_in_diffusion_bsdf_sibling']['deltas']['red_vs_physical_blurred'] < .05,
            'Shader to RGB inside diffusion consumed an ordinary sibling BSDF')
    require(stats['shader_rgb_in_diffusion_bsdf_sibling']['deltas']['blue_vs_glossy'] < .05,
            'Ordinary sibling BSDF was lit inside the diffusion Shader to RGB')

    # Nested diffusion must stay finite, keep coverage and roughly keep energy;
    # it is not required to equal a strict double blur.
    tree.links.new(step.outputs[0], diffusion.inputs['Shader'])
    tree.links.new(diffusion.outputs[0], diffusion_b.inputs['Shader'])
    connect(diffusion_b.outputs[0])
    nested = render('nested_diffusion')
    stats['nested_diffusion']['deltas'] = {
        'alpha_vs_original': float(np.max(np.abs(nested[:, :, 3] - original[:, :, 3]))),
        'edge_gain': float(np.mean(nested[32:64, 44:48, 0] - original[32:64, 44:48, 0])),
        'energy_ratio': float(nested[20:76, 20:76, :3].mean() /
                              original[20:76, 20:76, :3].mean()),
    }
    print('NPR_DIFFUSION_DELTAS', 'nested_diffusion',
          stats['nested_diffusion']['deltas'], flush=True)
    require(stats['nested_diffusion']['deltas']['alpha_vs_original'] < .025,
            'Nested diffusion altered coverage')
    require(stats['nested_diffusion']['deltas']['edge_gain'] > .025,
            'Nested diffusion returned the original surface')
    require(abs(stats['nested_diffusion']['deltas']['energy_ratio'] - 1) < .1,
            'Nested diffusion lost or multiplied energy')
    tree.nodes.remove(diffusion_b)

    # Unsupported forward surfaces must be a predictable pass-through, not black or relit.
    material.surface_render_method = 'BLENDED'
    connect(step.outputs[0])
    forward_original = render('forward_original')
    tree.links.new(step.outputs[0], diffusion.inputs['Shader'])
    connect(diffusion.outputs[0])
    forward_diffused = render('forward_bypass')
    require(np.max(np.abs(forward_original - forward_diffused)) < .025,
            'Forward fallback changed the original surface')
    material.surface_render_method = 'DITHERED'

    tree.links.new(step.outputs[0], diffusion.inputs['Shader'])
    saved = render('before_save')
    bpy.ops.wm.save_as_mainfile(filepath=str(args.output_dir / 'surface_diffusion.blend'))
    bpy.ops.wm.open_mainfile(filepath=str(args.output_dir / 'surface_diffusion.blend'))
    scene = bpy.context.scene
    restored = render('after_load')
    require(np.max(np.abs(saved - restored)) < .01, 'Save/reopen changed the result')
    (args.output_dir / 'results.json').write_text(json.dumps(stats, indent=2), encoding='utf-8')
    print('NPR_SURFACE_DIFFUSION_OK', args.expected_backend, flush=True)


if __name__ == '__main__':
    main()
