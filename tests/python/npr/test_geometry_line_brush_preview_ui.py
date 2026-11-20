"""Run in an isolated foreground window; exercise editing bursts and native menus."""
import argparse
import json
from pathlib import Path
import sys
import time
import traceback

import bpy
import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from test_geometry_line_brush_closure import scene
from bl_operators import npr_lines

parser = argparse.ArgumentParser()
parser.add_argument('--output', required=True)
parser.add_argument('--language', default='en_US')
parser.add_argument('--menus-only', action='store_true')
args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
out = Path(args.output)
out.mkdir(parents=True, exist_ok=True)
obj, tree, quick, settings, parameters, output = scene()
bpy.context.preferences.view.show_splash = False
bpy.context.preferences.view.smooth_view = 0
bpy.context.preferences.view.language = args.language
bpy.context.preferences.view.use_translate_interface = True
bpy.context.preferences.view.use_translate_tooltips = True
output.inputs['color'].default_value = (1, 0, 0, 1)
parameters.location = (-300, 0)
output.location = (0, 0)
for node in tree.nodes:
    node.select = node in (parameters, output)
tree.nodes.active = output
window = bpy.context.window_manager.windows[0]
area = max(window.screen.areas, key=lambda item: item.width * item.height)
area.type = 'NODE_EDITOR'
area.ui_type = 'NPRLineNodeTree'
area.spaces.active.pin = True
area.spaces.active.node_tree = tree
events = []
real_preview = npr_lines.region_preview
state = {'phase': 'warm', 'index': 0, 'burst': False, 'started': time.monotonic()}
menus = ('NODE_MT_add', 'NODE_MT_add', 'NODE_MT_add', 'NODE_MT_npr_line_node_brush',
         'NODE_MT_npr_line_node_input', 'NODE_MT_npr_line_node_math',
         'NODE_MT_npr_line_node_color_vector', 'NODE_MT_npr_line_node_setup', 'NODE_MT_category_layout')
if args.menus_only:
    output.hide = quick.hide = True
    state['phase'] = 'menu_prepare'


def evaluated(*args, **kwargs):
    begin = time.perf_counter()
    image = real_preview(*args, **kwargs)
    events.append({'ms': (time.perf_counter() - begin) * 1000, 'during_drag': state['burst']})
    return image


npr_lines.region_preview = evaluated


def escape():
    window.event_simulate(type='ESC', value='PRESS')
    window.event_simulate(type='ESC', value='RELEASE')


def swatch_has(channel):
    image = npr_lines.output_brush_preview(output)
    pixels = np.asarray(image.image_pixels_float[:]).reshape(64, 192, 4)
    return (pixels[:, :, channel] - pixels[:, :, 2]).max() > .8


def tick():
    try:
        assert time.monotonic() - state['started'] < 150, 'UI acceptance timed out'
        region = next(item for item in area.regions if item.type == 'WINDOW')
        with bpy.context.temp_override(window=window, area=area, region=region):
            phase = state['phase']
            if phase == 'menu_prepare':
                assert [socket.identifier for socket in parameters.outputs] == ['Distance', 'Factor', 'Length', 'ID', 'Closed']
                assert not hasattr(parameters, 'parameters')
                if args.language == 'zh_HANS':
                    translate = bpy.app.translations.pgettext_iface
                    for original, expected, context in (
                            ('Brush','笔刷','*'), ('Input','输入','*'), ('Math','运算','*'),
                            ('Color and Vector','颜色与矢量','*'), ('Line Setup','描边','*'),
                            ('Material AOV','材质 AOV','*'), ('Brush Closure','笔刷闭包','*'),
                            ('Closed','闭合','*'),
                            ('Wave','波纹','NPRBrushEffect'), ('Dashes','虚线','NPRBrushEffect'),
                            ('Taper','起收笔','NPRBrushEffect'), ('Per Stroke Random','逐笔随机','NPRBrushEffect')):
                        assert translate(original,context) == expected,(original,translate(original,context))
                state['group'] = npr_lines.brush_effect_group('WAVE')
                state.update(phase='menu', index=0)
                bpy.ops.node.view_selected()
            elif phase == 'warm':
                bpy.ops.node.view_selected()
                if not swatch_has(0):
                    return .1
                state.update(phase='drag', index=0, baseline=len(events), burst=True)
            elif phase == 'drag':
                index = state['index'] + 1
                output.inputs['color'].default_value = (1-index/20, index/20, 0, 1)
                output.inputs['width'].default_value = 2+index/20
                area.tag_redraw()
                state['index'] = index
                if index == 20:
                    assert len(events) == state['baseline'], 'Preview drawing blocked a continuous edit'
                    state.update(phase='settle', burst=False, stopped=time.monotonic())
            elif phase == 'settle':
                if not swatch_has(1):
                    return .05
                assert len(events) == state['baseline']+1, 'Edits must coalesce into one evaluation'
                assert not any(event['during_drag'] for event in events), 'No evaluation may interrupt the drag'
                state.update(phase='move', index=0, baseline=len(events), refreshed_ms=(time.monotonic()-state['stopped'])*1000)
            elif phase == 'move':
                output.location.x += 1
                area.tag_redraw()
                state['index'] += 1
                if state['index'] == 12:
                    state.update(phase='move_settle', stopped=time.monotonic())
            elif phase == 'move_settle':
                if time.monotonic()-state['stopped'] < .6:
                    return .05
                assert len(events) == state['baseline'], 'Moving a node must reuse its preview'
                group = npr_lines.brush_effect_group('WAVE')
                state['group'] = group
                state.update(phase='menu', index=0)
            elif phase == 'menu':
                # This is the same native menu invoked by the Node Editor Shift+A keymap.
                assert bpy.ops.wm.call_menu('INVOKE_DEFAULT', name=menus[state['index']]) in ({'INTERFACE'}, {'FINISHED'})
                state['phase'] = 'menu_close'
                return .2
            elif phase == 'menu_close':
                name = 'shift-a' if state['index'] < 3 else menus[state['index']]
                bpy.ops.screen.screenshot(filepath=str(out/(name+'.png')))
                escape()
                state['index'] += 1
                state['phase'] = 'menu' if state['index'] < len(menus) else 'effects_prepare'
                return .2
            elif phase == 'effects_prepare':
                bpy.ops.wm.call_menu('INVOKE_DEFAULT', name='NODE_MT_npr_line_node_brush')
                state['phase'] = 'effects_open'
                return .2
            elif phase == 'effects_open':
                for kind in ('UP_ARROW', 'RIGHT_ARROW'):
                    window.event_simulate(type=kind, value='PRESS')
                    window.event_simulate(type=kind, value='RELEASE')
                state['phase'] = 'effects_capture'
                return .2
            elif phase == 'effects_capture':
                bpy.ops.screen.screenshot(filepath=str(out/'brush-effects.png'))
                escape()
                state['phase'] = 'effects_close'
                return .2
            elif phase == 'effects_close':
                escape()
                state['phase'] = 'group_menu'
                return .2
            elif phase == 'group_menu':
                assert bpy.ops.wm.call_menu('INVOKE_DEFAULT', name='NODE_MT_group_add') in ({'INTERFACE'}, {'FINISHED'})
                state['phase'] = 'group_close'
                return .2
            elif phase == 'group_close':
                bpy.ops.screen.screenshot(filepath=str(out/'groups.png'))
                escape()
                state['phase'] = 'group_add'
                return .2
            elif phase == 'group_add':
                group = state['group']
                assert bpy.ops.node.add_node('EXEC_DEFAULT', type='NPRLineNodeGroup', use_transform=False,
                    settings=[{'name':'node_tree', 'value':f'bpy.data.node_groups[{group.name!r}]'}]) == {'FINISHED'}
                assert tree.nodes.active.node_tree == group
                assert abs(tree.nodes.active.inputs['Amplitude'].default_value-8) < .001
                assert bpy.ops.node.add_empty_group('EXEC_DEFAULT', use_transform=False) == {'FINISHED'}
                assert tree.nodes.active.bl_idname == 'NPRLineNodeGroup'
                assert {node.bl_idname for node in tree.nodes.active.node_tree.nodes} == {'NodeGroupInput', 'NodeGroupOutput'}
                results = {'language':args.language, 'menus_only':args.menus_only,
                           'native_add_menu_opens':3, 'submenus':list(menus[3:]),
                           'native_group_menu_existing_and_new':'passed', 'evaluations':events}
                if not args.menus_only:
                    results.update(continuous_edits=20,evaluations_during_drag=0,evaluations_after_drag=1,
                                   refresh_after_stop_ms=state['refreshed_ms'],node_moves_without_evaluation=12)
                (out/'results.json').write_text(json.dumps(results,indent=2),encoding='utf8')
                print('BRUSH_PREVIEW_UI_OK '+json.dumps(results),flush=True)
                bpy.ops.wm.quit_blender()
                return None
        return .05
    except Exception:
        (out/'error.txt').write_text(traceback.format_exc(),encoding='utf8')
        traceback.print_exc()
        bpy.ops.wm.quit_blender()
        return None


bpy.app.timers.register(tick, first_interval=.5)
