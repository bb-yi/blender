"""Draw through sparse sampler units without increasing the sampler count limit.

Run with --background --factory-startup --gpu-backend opengl --python-exit-code 1
--python this_file -- --output-dir <isolated output directory>.
"""

import argparse
import ctypes
import json
from pathlib import Path
import sys

import bpy
import gpu
import numpy as np
from gpu_extras.batch import batch_for_shader


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    args.output_dir.mkdir(parents=True, exist_ok=True)
    gpu.init()
    assert gpu.platform.backend_type_get() == 'OPENGL'

    gl = ctypes.WinDLL('opengl32.dll')
    gl.glGetIntegerv.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_int)]
    gl.wglGetProcAddress.argtypes = [ctypes.c_char_p]
    gl.wglGetProcAddress.restype = ctypes.c_void_p

    def proc(name, result, *arguments):
        address = gl.wglGetProcAddress(name.encode())
        assert address not in (None, 0, 1, 2, 3, ctypes.c_void_p(-1).value), name
        return ctypes.WINFUNCTYPE(result, *arguments)(address)

    uniform_location = proc('glGetUniformLocation', ctypes.c_int, ctypes.c_uint, ctypes.c_char_p)
    uniform_value = proc('glGetUniformiv', None, ctypes.c_uint, ctypes.c_int,
                         ctypes.POINTER(ctypes.c_int))
    combined = ctypes.c_int()
    gl.glGetIntegerv(0x8B4D, ctypes.byref(combined))
    capacity = min(combined.value, 64)
    report = {'backend': gpu.platform.backend_type_get(), 'build': bpy.app.build_hash.decode(),
              'combined_units': combined.value, 'cache_capacity': capacity,
              'per_stage_count': gpu.capabilities.max_textures_get(), 'cases': []}
    expected = np.array([0.125, 0.375, 0.625, 1.0], dtype=np.float32)
    texture = gpu.types.GPUTexture((1, 1), format='RGBA32F',
                                   data=gpu.types.Buffer('FLOAT', 4, expected))
    texture_b = gpu.types.GPUTexture((1, 1), format='RGBA32F',
                                     data=gpu.types.Buffer('FLOAT', 4, expected * 0.5))
    target = gpu.types.GPUTexture((4, 4), format='RGBA32F')
    framebuffer = gpu.types.GPUFrameBuffer(color_slots=target)
    previous_viewport = gpu.state.viewport_get()
    try:
        with framebuffer.bind():
            gpu.state.viewport_set(0, 0, 4, 4)
            gpu.state.blend_set('NONE')
            gpu.state.depth_test_set('NONE')
            for slot in dict.fromkeys((0, 31, 32, 35, 47, capacity - 1, 0)):
                if not 0 <= slot < capacity:
                    continue
                name = f'binding_unit_{slot}'
                info = gpu.types.GPUShaderCreateInfo()
                info.vertex_in(0, 'VEC2', 'position')
                info.sampler(slot, 'FLOAT_2D', name)
                info.fragment_out(0, 'VEC4', 'fragColor')
                info.vertex_source('void main() { gl_Position = vec4(position, 0.0, 1.0); }')
                info.fragment_source(f'void main() {{ fragColor = texture({name}, vec2(0.5)); }}')
                shader = gpu.shader.create_from_info(info)
                batch = batch_for_shader(shader, 'TRIS',
                                         {'position': ((-1, -1), (3, -1), (-1, 3))})
                for replacement, source in enumerate((texture, texture_b, texture)):
                    shader.bind()
                    shader.uniform_sampler(name, source)
                    # Query the current actual program; GPUShader.program is a deprecated stub.
                    program = ctypes.c_int()
                    gl.glGetIntegerv(0x8B8D, ctypes.byref(program))
                    location = uniform_location(program.value, name.encode())
                    assert location >= 0, (slot, program.value)
                    physical_unit = ctypes.c_int(-1)
                    uniform_value(program.value, location, ctypes.byref(physical_unit))
                    assert physical_unit.value == slot, (slot, physical_unit.value)
                    framebuffer.clear(color=(0, 0, 0, 0))
                    batch.draw(shader)
                    pixels = np.array(framebuffer.read_color(0, 0, 4, 4, 4, 0, 'FLOAT'))
                    wanted = expected * (0.5 if replacement == 1 else 1)
                    error = float(np.max(np.abs(pixels - wanted)))
                    assert error < 1e-6, (slot, replacement, error)
                    report['cases'].append({'unit': slot, 'replacement': replacement,
                                            'max_error': error})
                del batch, shader
    finally:
        gpu.state.viewport_set(*previous_viewport)
    report['passed'] = True
    (args.output_dir / 'verification.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f"SPARSE_TEXTURE_UNITS_OK cases={len(report['cases'])} capacity={capacity}", flush=True)


if __name__ == '__main__':
    main()
