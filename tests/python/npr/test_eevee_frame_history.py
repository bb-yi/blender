import csv
import json
import sys
from pathlib import Path

import bpy
from bl_ui import space_outliner
from bpy_extras.eevee_frame_profile import export_frames, rank_frame, stage_times


def descendants(node):
    yield node
    for child in node['children']:
        yield from descendants(child)


def validate_wall_time(frame):
    timing = frame['engine_stages_ms']
    assert timing['Engine.Depsgraph'] > 0
    assert timing['Engine.ResultSetup'] > 0
    assert timing['Engine.ResultMerge'] > 0
    assert timing['Engine.DrawSetup'] > 0
    assert timing['Engine.Cleanup'] > 0
    assert frame['preparation_ms'] > 0
    assert abs(sum(stage_times(frame).values()) - frame['render_ms']) < 0.01
    for view in frame['views']:
        assert view['readback_count'] > 0 and view['readback_bytes'] > 0
        root = view['tree']
        for node in descendants(root):
            children_ms = sum(child['cpu_ms'] for child in node['children'])
            assert abs(node['cpu_ms'] - node['self_ms'] - children_ms) < 0.01, node['stage']
        work, = [node for node in root['children'] if node['stage'] == 'Sample.Work']
        sample_stages = {child['stage'] for child in work['children']}
        assert {'MainView', 'Sample.QueueWait', 'Sample.Submit'} <= sample_stages
        readback, = [node for node in root['children'] if node['stage'] == 'ReadResult']
        assert any(node['stage'] == 'Pass.Readback: Combined' for node in readback['children'])


def main():
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    scene.render.resolution_x = scene.render.resolution_y = 48
    scene.render.resolution_percentage = 100
    scene.eevee.taa_render_samples = 2
    scene.frame_start, scene.frame_end = 1, 3
    output = Path(sys.argv[sys.argv.index('--') + 1])
    output.mkdir(parents=True, exist_ok=True)
    scene.render.filepath = str(output / 'frame-')
    bpy.ops.object.lightprobe_add(type='PLANE')
    bpy.context.object.name = '计时测试 Mirror'
    scene.eevee.use_performance_profiler = True
    scene.eevee.use_performance_profiler_gpu = True
    assert not scene.eevee.use_performance_profiler_pause
    assert not hasattr(space_outliner, 'OUTLINER_OT_eevee_frame_history')
    assert not hasattr(space_outliner, 'OUTLINER_OT_eevee_export_frames')
    assert json.loads(scene.eevee.performance_profiler_frame_history) == []
    assert bpy.ops.render.render(animation=True) == {'FINISHED'}
    frames = json.loads(scene.eevee.performance_profiler_frame_history)
    export_frames(output / 'history.json', frames)
    assert [row['frame'] for row in frames] == [1, 2, 3]
    assert [row['first_capture'] for row in frames] == [True, False, False]
    assert len(set(row['sequence'] for row in frames)) == 1
    assert all(row['render_ms'] > 0 and row['saving_ms'] >= 0 for row in frames)
    measured_gpu_intervals = 0
    for frame in frames:
        view, = frame['views']
        assert view['samples'] == 2 and view['init_ms'] >= 0
        nodes = list(descendants(view['tree']))
        assert all(node['self_ms'] <= node['cpu_ms'] + 0.01 for node in nodes)
        assert any(node['stage'].startswith('Planar.Capture: 计时测试 Mirror') for node in nodes)
        measured_gpu_intervals += sum(node['gpu_ms'] is not None and node['gpu_ms'] > 0 for node in nodes)
        assert all(node['gpu_ms'] is None or node['gpu_calls'] == node['calls'] for node in nodes)
        assert all(node['gpu_ms'] is None for node in nodes if node['stage'].startswith('Sync.'))
        validate_wall_time(frame)
    assert measured_gpu_intervals > 0
    assert not rank_frame(frames, 1)[1]
    export_frames(output / 'history.csv', frames)
    assert json.loads((output / 'history.json').read_text('utf-8'))['frames'] == frames
    with (output / 'history.csv').open(encoding='utf-8-sig') as stream:
        assert [int(row['frame']) for row in csv.DictReader(stream)] == [1, 2, 3]
    scene.eevee.use_performance_profiler_gpu = False
    assert bpy.ops.render.render() == {'FINISHED'}
    latest, = json.loads(scene.eevee.performance_profiler_latest_frame)
    assert latest['saving_ms'] is None and latest['run'] != frames[-1]['run']
    validate_wall_time(latest)
    assert all(node['gpu_ms'] is None for node in descendants(latest['views'][0]['tree']))
    scene.eevee.use_performance_profiler = False
    assert bpy.ops.render.render() == {'FINISHED'}
    assert len(json.loads(scene.eevee.performance_profiler_frame_history)) == 4
    print('EEVEE_FRAME_HISTORY_OK', flush=True)


main()
