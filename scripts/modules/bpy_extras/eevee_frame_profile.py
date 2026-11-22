import csv
import json
import statistics
from pathlib import Path


def stage_times(frame):
    values = {key: frame[key] for key in ('compositor_ms', 'file_output_ms', 'unclassified_ms')}
    remaining = frame['engine_ms']
    for name, duration in frame.get('engine_stages_ms', {}).items():
        values[name] = duration
        remaining -= duration

    def visit(node, path):
        key = path + '/' + node['stage']
        values[key] = values.get(key, 0.0) + node['self_ms']
        for child in node['children']:
            visit(child, key)

    for view in frame['views']:
        path = view['layer'] + (':' + view['view'] if view['view'] else '')
        remaining -= view['init_ms'] + view['tree']['cpu_ms']
        values[path + '/Instance.Init'] = view['init_ms']
        visit(view['tree'], path)
    values['Engine.Other'] = max(0.0, remaining)
    return values


def rank_frame(history, index):
    frame = history[index]
    values = stage_times(frame)
    top = sorted(values.items(), key=lambda item: -item[1])
    def settings(record):
        return record['sequence'], [(v['layer'], v['view'], v['width'], v['height'], v['samples'])
                                    for v in record['views']]
    candidates = [other for other in history[:index] if not other['first_capture'] and
                  settings(other) == settings(frame)]
    if not candidates or frame['first_capture']:
        return top, []
    baseline = [stage_times(other) for other in candidates[-8:]]
    deltas = [(key, value - statistics.median(prev.get(key, 0.0) for prev in baseline))
              for key, value in values.items()]
    return top, sorted((row for row in deltas if row[1] > 0), key=lambda row: -row[1])


def export_frames(path, frames):
    path = Path(path)
    if path.suffix.lower() == '.json':
        with path.open('w', encoding='utf-8') as output:
            json.dump({'schema': 'eevee.final-frames.v1', 'frames': frames}, output,
                      ensure_ascii=False, separators=(',', ':'), allow_nan=False)
        return
    if path.suffix.lower() != '.csv':
        raise ValueError('Choose a .csv or .json filename')
    with path.open('w', encoding='utf-8-sig', newline='') as output:
        writer = csv.writer(output)
        fields = ('frame', 'run', 'sequence', 'first_capture', 'render_ms', 'engine_ms',
                  'compositor_ms', 'file_output_ms', 'unclassified_ms', 'saving_ms')
        writer.writerow((*fields, 'top_self_stage', 'top_self_ms', 'largest_increase', 'increase_ms', 'preparation_ms'))
        for index, frame in enumerate(frames):
            top, delta = rank_frame(frames, index)
            writer.writerow((*[frame[field] for field in fields], *top[0], *(delta[0] if delta else ('', '')),
                             frame.get('preparation_ms')))
