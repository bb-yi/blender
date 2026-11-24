import runpy
import sys
from pathlib import Path

import bpy


helpers = runpy.run_path(str(Path(__file__).with_name('test_glsl_function_light_access_render.py')))
directory = Path(sys.argv[sys.argv.index('--') + 1]).resolve()
directory.mkdir(parents=True, exist_ok=True)
for raw in (False, True):
    body = ('return vec4(uintBitsToFloat(texelFetch(shadow_atlas_tx, ivec3(0), 0).r));' if raw else
            '/* shadow_atlas_tx */ float my_shadow_atlas_tx_scale = 0.75; return vec4(my_shadow_atlas_tx_scale);')
    name = 'Atlas reader' if raw else 'Ordinary GLSL'
    material = helpers['make_glsl_light_material'](
        name, 'DITHERED', name, 'shade', f'vec4 shade(vec3 normal) {{ {body} }}')
    helpers['build_scene'](material)
    bpy.ops.object.light_add(type='SUN')
    scene = bpy.context.scene
    scene.eevee.use_raytracing = True
    scene.eevee.ray_tracing_method = 'SCREEN'
    scene.eevee.ray_tracing_options.use_hardware_shadows = True
    image = directory / f'{name}.png'
    assert not image.exists(), image
    scene.render.filepath = str(image)
    try:
        bpy.ops.render.render(write_still=True)
    except RuntimeError as error:
        assert raw and f'custom shader reads shadow-map texels ({name})' in str(error), error
    assert image.exists() == (not raw), image
surface = bpy.data.materials.new('Diffuse receiver')
helpers['build_scene'](surface)
lamp = helpers['make_light']('SUN')
helpers['configure_light_shader_output'](lamp, (1, 1, 1, 1))
tree = lamp.data.node_tree
lookup = tree.nodes.new('ShaderNodeGLSLFunction')
lookup.script = bpy.data.texts.new('Lamp visibility')
lookup.script.write('float lamp_mask() { return glsl_light_shadow(0, vec3(0, 0, 1)); }')
lookup.function_name = 'lamp_mask'
helpers['refresh_glsl_node'](lookup)
tree.links.new(lookup.outputs['Result'], helpers['find_node'](tree, 'ShaderNodeEeveeLightShaderOutput').inputs['Intensity'])
image = directory / 'light_shader.png'
assert not image.exists(), image
scene.render.filepath = str(image)
try:
    bpy.ops.render.render(write_still=True)
except RuntimeError as error:
    assert f'light shader requires shadow-map data ({lamp.name})' in str(error), error
else:
    raise AssertionError('Rendered a light shader that needs native shadow-map data')
assert not image.exists(), image
print('GLSL_SHADOW_DEPENDENCIES_OK', flush=True)
