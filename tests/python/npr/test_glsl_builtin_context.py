# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import json
import os
import sys
import traceback
from pathlib import Path

import bpy
import gpu

ROOT = Path(__file__).resolve().parent
tag = sys.argv[sys.argv.index('--') + 1] if '--' in sys.argv else 'audit'
OUT = Path(os.environ.get('BLENDER_TEST_OUTPUT_DIR', str(ROOT.parents[3] / 'temp/glsl_builtin_regression'))) / tag
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(ROOT))
from filter_graph_test_utils import attach_filter_material

CASES = [
    ('invalid_source', 'SURFACE', 'return missing_glsl_audit_symbol;', None),
    ('control', 'SURFACE', 'return vec3(0.2, 0.4, 0.6);', [0.2, 0.4, 0.6]),
    ('position', 'SURFACE', 'return vec3(glsl_position().z + 0.2, 0.4, 0.6);', [0.2, 0.4, 0.6]),
    ('normal', 'SURFACE', 'return abs(glsl_normal());', [0, 0, 1]),
    ('true_normal', 'SURFACE', 'return abs(glsl_true_normal());', [0, 0, 1]),
    ('incoming', 'SURFACE', 'return abs(glsl_incoming());', [0, 0, 1]),
    ('ambient', 'SURFACE', 'return vec3(0.2, 0.4, 0.6) * float(length(glsl_ambient_lighting()) >= 0.0);', [0.2, 0.4, 0.6]),
    ('lights', 'SURFACE', 'return vec3(0.2, 0.4, 0.6) * float(glsl_light_count() == 0);', [0.2, 0.4, 0.6]),
    ('view_matrix', 'SURFACE', 'return vec3(0.2, 0.4, 0.6) * float(glsl_view_matrix()[3].z < -4.9);', [0.2, 0.4, 0.6]),
    ('projection_matrix', 'SURFACE', 'return vec3(0.2, 0.4, 0.6) * float(glsl_projection_matrix()[0][0] > 0.0);', [0.2, 0.4, 0.6]),
    ('model_matrix', 'SURFACE', 'return vec3(0.2, 0.4, 0.6) * float(glsl_model_matrix()[0][0] == 1.0);', [0.2, 0.4, 0.6]),
    ('normal_matrix', 'SURFACE', 'return vec3(0.2, 0.4, 0.6) * float(glsl_normal_matrix()[2][2] == 1.0);', [0.2, 0.4, 0.6]),
    ('stencil_uv', 'SURFACE', 'return vec3(glsl_stencil_uv(vec2(0.25, 0.75)), 0.5);', [0.25, 0.75, 0.5]),
    ('filter_control', 'FILTER', 'return vec3(0.2, 0.4, 0.6);', [0.2, 0.4, 0.6]),
    ('filter_view', 'FILTER', 'return vec3(0.2, 0.4, 0.6) * float(glsl_view_matrix_inverse()[3].z > 4.9);', [0.2, 0.4, 0.6]),
    ('displacement_projection', 'DISPLACEMENT', 'return vec3(0.0, 0.0, glsl_projection_matrix()[0][0] * 0.01);', [0.2, 0.4, 0.6]),
]

results = []
for name, domain, body, expected in CASES:
    print('AUDIT_BEGIN', name, flush=True)
    try:
        bpy.ops.wm.read_factory_settings(use_empty=True)
        s = bpy.context.scene
        s.render.engine = 'BLENDER_EEVEE'
        s.render.resolution_x = s.render.resolution_y = 32
        s.render.resolution_percentage = 100
        s.render.use_compositing = False
        s.eevee.taa_samples = s.eevee.taa_render_samples = 1
        s.eevee.use_performance_profiler = True
        s.world = bpy.data.worlds.new('World')
        s.world.color = (0, 0, 0)
        camera_data = bpy.data.cameras.new('Camera')
        camera_data.type = 'ORTHO'
        camera_data.ortho_scale = 4
        camera = bpy.data.objects.new('Camera', camera_data)
        camera.location.z = 5
        s.collection.objects.link(camera)
        s.camera = camera
        bpy.ops.mesh.primitive_plane_add(size=4)
        plane = bpy.context.object
        material = bpy.data.materials.new(name)
        material.use_nodes = True
        material.node_tree.nodes.clear()
        nodes = material.node_tree.nodes
        links = material.node_tree.links
        shader = nodes.new('ShaderNodeGLSLFunction')
        source = bpy.data.texts.new(name + '.glsl')
        source.write('vec3 audit(){\n' + body + '\n}\n')
        shader.script = source
        shader.function_name = 'audit'
        if domain == 'FILTER':
            material.eevee_domain = 'FILTER'
            output = nodes.new('ShaderNodeOutputFilter')
            output.inputs['Alpha'].default_value = 1
            links.new(shader.outputs['Result'], output.inputs['Color'])
            attach_filter_material(material)
        else:
            output = nodes.new('ShaderNodeOutputMaterial')
            emission = nodes.new('ShaderNodeEmission')
            emission.inputs['Color'].default_value = (0.2, 0.4, 0.6, 1)
            links.new(emission.outputs[0], output.inputs['Surface'])
            if domain == 'DISPLACEMENT':
                material.displacement_method = 'DISPLACEMENT'
                links.new(shader.outputs['Result'], output.inputs['Displacement'])
            else:
                links.new(shader.outputs['Result'], emission.inputs['Color'])
            plane.data.materials.append(material)
        material.node_tree.update_tag()
        bpy.context.view_layer.update()
        parse_before = shader.parse_status
        s.render.image_settings.file_format = 'OPEN_EXR'
        s.render.image_settings.color_mode = 'RGBA'
        s.render.image_settings.color_depth = '32'
        path = OUT / (name + '.exr')
        bpy.ops.render.render()
        bpy.data.images['Render Result'].save_render(str(path))
        im = bpy.data.images.load(str(path), check_existing=False)
        index = ((im.size[1] // 2) * im.size[0] + im.size[0] // 2) * 4
        pixel = list(im.pixels[index:index+4])
        bpy.data.images.remove(im)
        report = s.eevee.performance_profiler_render_report
        if expected is not None:
            assert not shader.error_message, shader.error_message
        record = {'case': name, 'domain': domain, 'pixel': pixel, 'expected': expected,
                  'pixel_pass': bool(shader.error_message) if expected is None else all(abs(pixel[i]-expected[i]) < 0.08 for i in range(3)),
                  'parse_before': parse_before, 'parse_after': shader.parse_status,
                  'error_message': shader.error_message,
                  'shader_requests': [x.strip() for x in report.splitlines() if 'failed=' in x]}
        if expected is None:
            assert 'missing_glsl_audit_symbol' in shader.error_message, record
            source.clear()
            source.write('vec3 audit(){ return vec3(0.2, 0.4, 0.6); }')
            shader.function_name = ''
            shader.function_name = 'audit'
            material.node_tree.update_tag()
            bpy.context.view_layer.update()
            bpy.ops.render.render()
            assert not shader.error_message, shader.error_message
            bpy.data.images['Render Result'].save_render(str(path))
            im = bpy.data.images.load(str(path), check_existing=False)
            recovered = list(im.pixels[index:index+4])
            bpy.data.images.remove(im)
            assert all(abs(recovered[i] - (0.2, 0.4, 0.6)[i]) < 0.08 for i in range(3)), recovered
            record['recovered_pixel'] = recovered
    except Exception:
        record = {'case': name, 'harness_error': traceback.format_exc()}
    results.append(record)
    print('AUDIT_RESULT', json.dumps(record), flush=True)
    summary = {'version': bpy.app.version_string, 'build_hash': bpy.app.build_hash.decode(),
               'backend': gpu.platform.backend_type_get(), 'results': results}
    (OUT / 'results.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
print('AUDIT_COMPLETE', flush=True)

assert len(results) == len(CASES) and all(r.get('pixel_pass') for r in results), results
print('GLSL_BUILTIN_CONTEXT_OK', flush=True)
