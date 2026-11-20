# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Shared GLSL analysis must preserve instance values, defines, samplers and source refresh.

Run with isolated user resources and BLENDER_TEST_OUTPUT_DIR. The optional --stress-cache
argument churns more than 64 MiB of source/analysis storage before revisiting an earlier key.
"""

import gc
import json
import os
from pathlib import Path
import re
import sys

import bpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import OUT, center, glsl, near, render, scene, socket


def refresh(tree):
    tree.update_tag()
    tree.interface_update(bpy.context)
    bpy.context.view_layer.update()


def share_source(tree, original, function):
    node = tree.nodes.new('ShaderNodeGLSLFunction')
    node.source_mode = 'INTERNAL'
    node.script = original.script
    node.function_name = function
    refresh(tree)
    assert node.parse_status == 'READY', node.parse_status
    return node


def process_memory():
    """Record current Windows memory without asserting machine-dependent absolute limits."""
    if os.name != 'nt':
        return {}
    import ctypes
    from ctypes import wintypes

    class Counters(ctypes.Structure):
        _fields_ = [('cb', wintypes.DWORD), ('PageFaultCount', wintypes.DWORD)] + [
            (name, ctypes.c_size_t) for name in (
                'PeakWorkingSetSize', 'WorkingSetSize', 'QuotaPeakPagedPoolUsage',
                'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage',
                'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage', 'PrivateUsage')]

    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    psapi = ctypes.WinDLL('psapi', use_last_error=True)
    kernel.GetCurrentProcess.argtypes = []
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
    psapi.GetProcessMemoryInfo.restype = wintypes.BOOL
    counters = Counters()
    counters.cb = ctypes.sizeof(counters)
    if not psapi.GetProcessMemoryInfo(kernel.GetCurrentProcess(), ctypes.byref(counters), counters.cb):
        raise ctypes.WinError(ctypes.get_last_error())
    return {'private_bytes': counters.PrivateUsage, 'working_set_bytes': counters.WorkingSetSize,
            'peak_working_set_bytes': counters.PeakWorkingSetSize}


def stress_cache(tree, second, emission):
    """Keep declaration callbacks alive across cache saturation and an oversized miss."""
    import gpu

    s = bpy.context.scene
    s.eevee.taa_samples = 1
    s.eevee.use_taa_reprojection = False
    s.eevee.use_performance_profiler = True
    gpu.init()
    area = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    space = area.spaces.active
    space.shading.type = 'RENDERED'
    space.overlay.show_overlays = False
    buffer = gpu.types.GPUOffScreen(64, 48, format='RGBA32F')
    pressure_tree = bpy.data.node_groups.new('Retained GLSL declarations', 'ShaderNodeTree')
    pressure_texts = []
    source_bytes = 0
    rows = []
    completed = False

    def snapshot(label):
        # Force the small visible shader through material preparation. The pressure nodes are
        # never connected to it, so their large strings never reach GPU shader compilation.
        emission.inputs['Strength'].default_value = 1.001
        emission.inputs['Strength'].default_value = 1.0
        refresh(tree)
        depsgraph = bpy.context.evaluated_depsgraph_get()
        projection = s.camera.calc_matrix_camera(depsgraph, x=64, y=48)
        buffer.draw_view3d(s, bpy.context.view_layer, space, region,
                          s.camera.matrix_world.inverted(), projection,
                          do_color_management=False, draw_background=True)
        buffer.texture_color.read()
        profiler = s.eevee.performance_profiler_viewport_report
        match = re.search(r'glsl_hits=(\d+) glsl_misses=(\d+).*?cache_bytes=(\d+)', profiler)
        assert match, ('GLSL cache telemetry is missing', profiler)
        hits, misses, cache_bytes = map(int, match.groups())
        assert hits + misses > 0, ('No measured cache access', profiler)
        assert 0 < cache_bytes <= 64 * 1024 * 1024, cache_bytes
        row = {'label': label, 'retained_text_bytes': source_bytes,
               'glsl_hits': hits, 'glsl_misses': misses, 'cache_bytes': cache_bytes,
               **process_memory()}
        rows.append(row)
        (OUT / 'analysis_cache_stress.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
        print('GLSL_CACHE_STRESS', json.dumps(row), flush=True)

    def add_pressure(index, comment_bytes):
        nonlocal source_bytes
        source = '/* @glsl_defines v1\n@define PRESSURE int default=1 min=0 max=2\n*/\n'
        source += '/*' + str(index) + 'x' * comment_bytes + '*/\n'
        source += 'vec3 cache_pressure() { return vec3(.125 * float(PRESSURE)); }'
        node = glsl(pressure_tree, source, 'cache_pressure')
        node.display_mode = 'NODE'
        refresh(pressure_tree)
        assert node.parse_status == 'READY', node.parse_status
        assert len(node.define_values) == 1
        pressure_texts.append(node.script)
        source_bytes += len(source)  # The generated pressure source is ASCII.
        # Retain both node and Text until after all snapshots. The define panel's declaration
        # callback must hold UI metadata only, even if its analysis wasn't admitted to the LRU.

    try:
        snapshot('before_pressure')
        for index in range(20):
            add_pressure(index, 2 * 1024 * 1024)
        gc.collect()
        snapshot('retained_define_declarations_over_budget')
        # Source key plus stripped source alone exceed 64 MiB, independent of token accounting.
        add_pressure('oversized', 33 * 1024 * 1024)
        gc.collect()
        snapshot('retained_oversized_declaration')
        second.function_name = 'cached_probe'
        refresh(tree)
        socket(second.inputs, 'gain').default_value = .4
        near(center(render('cache_after_retained_eviction')), [.6] * 3)
        snapshot('old_key_pixels_preserved')
        completed = True
    finally:
        bpy.data.node_groups.remove(pressure_tree)
        for text in pressure_texts:
            bpy.data.texts.remove(text)
        pressure_texts.clear()
        source_bytes = 0
        gc.collect()
        try:
            if completed:
                snapshot('after_declaration_removal')
        finally:
            buffer.free()


def test_defines_functions_and_instances():
    _, material, emission = scene()
    tree = material.node_tree
    source = '''/* @glsl_defines v1
@define MODE int default=0 min=0 max=1
*/
vec3 cached_probe(float gain) {
#if MODE == 1
  return vec3(gain * 2.0);
#else
  return vec3(gain);
#endif
}
vec3 cached_other(float gain) { return vec3(gain * 3.0); }
'''
    first = glsl(tree, source, 'cached_probe')
    second = share_source(tree, first, 'cached_probe')
    socket(first.inputs, 'gain').default_value = .2
    socket(second.inputs, 'gain').default_value = .3
    second.define_values['MODE'].int_value = 1
    add = tree.nodes.new('ShaderNodeVectorMath')
    add.operation = 'ADD'
    tree.links.new(first.outputs[0], add.inputs[0])
    tree.links.new(second.outputs[0], add.inputs[1])
    tree.links.new(add.outputs[0], emission.inputs['Color'])
    near(center(render('cache_distinct_defines')), [.8] * 3)

    second.define_values['MODE'].int_value = 0
    near(center(render('cache_define_restore')), [.5] * 3)
    socket(second.inputs, 'gain').default_value = .4
    near(center(render('cache_distinct_values')), [.6] * 3)
    second.function_name = 'cached_other'
    refresh(tree)
    socket(second.inputs, 'gain').default_value = .4
    near(center(render('cache_distinct_entry')), [1.4] * 3)

    first.script.clear()
    first.script.write(source.replace('gain * 3.0', 'gain * 4.0'))
    refresh(tree)
    near(center(render('cache_text_edit')), [1.8] * 3)
    first.script.clear()
    first.script.write(source)
    refresh(tree)
    near(center(render('cache_text_restore')), [1.4] * 3)

    if '--stress-cache' in sys.argv:
        stress_cache(tree, second, emission)


def test_sampler_bindings_are_per_instance():
    _, material, emission = scene()
    tree = material.node_tree
    source = 'vec3 cached_sample(sampler2D image) { return texture(image, vec2(.5)).rgb; }'
    first = glsl(tree, source, 'cached_sample')
    second = share_source(tree, first, 'cached_sample')
    textures = []
    for index, color in enumerate(((.1, .2, .3), (.4, .3, .2))):
        image = bpy.data.images.new('Cache binding %d' % index, 1, 1, float_buffer=True)
        image.colorspace_settings.name = 'Non-Color'
        image.pixels = (*color, 1.0)
        texture = tree.nodes.new('ShaderNodeImageToClosure')
        texture.image = image
        textures.append(texture)
    tree.links.new(textures[0].outputs['Closure'], socket(first.inputs, 'image'))
    tree.links.new(textures[1].outputs['Closure'], socket(second.inputs, 'image'))
    add = tree.nodes.new('ShaderNodeVectorMath')
    add.operation = 'ADD'
    tree.links.new(first.outputs[0], add.inputs[0])
    tree.links.new(second.outputs[0], add.inputs[1])
    tree.links.new(add.outputs[0], emission.inputs['Color'])
    near(center(render('cache_distinct_samplers')), [.5] * 3)
    tree.links.new(textures[1].outputs['Closure'], socket(first.inputs, 'image'))
    near(center(render('cache_relinked_sampler')), [.8, .6, .4])
    for link in tuple(socket(second.inputs, 'image').links):
        tree.links.remove(link)
    near(center(render('cache_unconnected_sampler')), [1.4, 1.3, 1.2])
    # A source-analysis hit must still validate the current node's link dimensionality.
    textures[1].texture_type = 'CUBE_STRIP'
    first.function_name = ''
    first.function_name = 'cached_sample'
    refresh(tree)
    assert first.parse_status == 'ERROR', first.parse_status
    assert second.parse_status == 'READY', second.parse_status
    textures[1].texture_type = 'IMAGE_2D'
    first.function_name = ''
    first.function_name = 'cached_sample'
    refresh(tree)
    assert first.parse_status == 'READY', first.parse_status
    near(center(render('cache_sampler_recovered')), [1.4, 1.3, 1.2])


def test_external_source_refresh():
    _, material, emission = scene()
    tree = material.node_tree
    filepath = OUT / 'analysis_cache_external.glsl'
    filepath.write_text('vec3 cached_external() { return vec3(.25); }', encoding='utf-8')
    before = filepath.stat()
    node = tree.nodes.new('ShaderNodeGLSLFunction')
    node.source_mode = 'EXTERNAL'
    node.filepath = str(filepath)
    node.function_name = 'cached_external'
    refresh(tree)
    tree.links.new(node.outputs[0], emission.inputs['Color'])
    near(center(render('cache_external_before')), [.25] * 3)
    # Same path, size and timestamp: caching by path/mtime would incorrectly retain .25.
    filepath.write_text('vec3 cached_external() { return vec3(.75); }', encoding='utf-8')
    os.utime(filepath, ns=(before.st_atime_ns, before.st_mtime_ns))
    node.function_name = ''
    node.function_name = 'cached_external'
    refresh(tree)
    near(center(render('cache_external_after')), [.75] * 3)


test_defines_functions_and_instances()
test_sampler_bindings_are_per_instance()
test_external_source_refresh()
print('GLSL_ANALYSIS_CACHE_OK', flush=True)
