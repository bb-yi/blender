import sys
from pathlib import Path

import bpy
import gpu


directory = Path(sys.argv[sys.argv.index('--') + 1]).resolve()
directory.mkdir(parents=True, exist_ok=True)
scene = bpy.context.scene
scene.render.engine = 'BLENDER_EEVEE'
scene.eevee.use_raytracing = True
scene.eevee.ray_tracing_method = 'SCREEN'
scene.eevee.ray_tracing_options.use_hardware_shadows = True
scene.eevee.taa_render_samples = 1
scene.render.resolution_x = scene.render.resolution_y = 32
scene.render.resolution_percentage = 100
scene.frame_start = 1
scene.frame_end = 3
scene.render.filepath = str(directory / 'strict_')
frames = [directory / f'strict_{frame:04}.png' for frame in range(1, 4)]
assert not any(path.exists() for path in frames), frames
probe = bpy.data.objects.new('Unsupported Mirror Probe', bpy.data.lightprobes.new('Mirror', 'PLANE'))
scene.collection.objects.link(probe)
for frame, hidden in ((1, True), (2, False)):
    probe.hide_render = hidden
    probe.keyframe_insert(data_path='hide_render', frame=frame)
scene.frame_set(1)
gpu.init()
supported = scene.eevee.is_hardware_raytracing_supported
try:
    bpy.ops.render.render(animation=True)
except RuntimeError as error:
    assert 'Hardware shadows' in str(error), error
else:
    raise AssertionError('Expected a render error')
assert [path.exists() for path in frames] == [supported, False, False], frames
print('STRICT_ANIMATION_OK', supported, flush=True)
