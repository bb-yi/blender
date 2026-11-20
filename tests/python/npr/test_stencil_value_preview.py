"""GPU viewport preview regression. No Stencil Value reader is needed in the fixture."""

import argparse
import json
from pathlib import Path
import sys
import traceback

import bpy
import gpu
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_stencil_value as core


def arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--asset-dir', type=Path)
    parser.add_argument('--old-file', type=Path)
    parser.add_argument('--reopen', type=Path)
    return parser.parse_args(sys.argv[sys.argv.index('--') + 1:])


def compare_images(actual, expected):
    core.near([float(np.max(np.abs(actual - expected)))], [0], tolerance=0.003)


def main():
    args = arguments()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    gpu.init()
    report = {'backend': gpu.platform.backend_type_get(), 'renderer': gpu.platform.renderer_get(),
              'build': bpy.app.build_date.decode() + ' ' + bpy.app.build_time.decode(),
              'executable': bpy.app.binary_path, 'cases': {}, 'passed': False}
    buffers = []
    try:
        if args.reopen:
            bpy.ops.wm.open_mainfile(filepath=str(args.reopen))
            spaces = [area.spaces.active for area in bpy.context.screen.areas if area.type == 'VIEW_3D']
            sources = sorted(space.shading.stencil_preview_source for space in spaces)
            assert sources == ['FINAL', 'SNAPSHOT'], sources
            report['cases']['saved_sources_restored'] = sources
            report['passed'] = True
            print('STENCIL_VALUE_PREVIEW_PASSED', flush=True)
            return
        if args.old_file:
            bpy.ops.wm.open_mainfile(filepath=str(args.old_file))
            spaces = [area.spaces.active for area in bpy.context.screen.areas if area.type == 'VIEW_3D']
            assert spaces and all(space.shading.stencil_preview_source == 'SNAPSHOT' for space in spaces)
            report['cases']['old_file_defaults_to_snapshot'] = True

        scene = core.configure()
        scene.eevee.taa_samples = 1
        scene.eevee.use_taa_reprojection = False
        writer_mat = core.marker_material(reference=1, mode='STANDARD')
        writer = core.plane('Marker', writer_mat, size=2.4, z=0.1)
        receiver = core.plane('Receiver', core.material('Plain Surface', (1, 1, 1, 1)))
        assert all(node.bl_idname != 'ShaderNodeStencilValue'
                   for mat in bpy.data.materials if mat.use_nodes for node in mat.node_tree.nodes)
        area = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')

        def configure_space(area):
            space = area.spaces.active
            space.shading.type = 'RENDERED'
            space.shading.use_scene_world_render = True
            space.shading.use_scene_lights_render = True
            space.overlay.show_overlays = False
            return space

        space = configure_space(area)
        assert space.shading.stencil_preview_source == 'SNAPSHOT'
        buffers.extend((gpu.types.GPUOffScreen(128, 128, format='RGBA32F'),
                        gpu.types.GPUOffScreen(96, 64, format='RGBA32F')))

        def draw(name, source='SNAPSHOT', which=0, target_area=None, render_pass='STENCIL_VALUE'):
            target_area = target_area or area
            target_space = target_area.spaces.active
            target_space.shading.render_pass = render_pass
            target_space.shading.stencil_preview_source = source
            region = next(r for r in target_area.regions if r.type == 'WINDOW')
            buffer = buffers[which]
            bpy.context.view_layer.update()
            deps = bpy.context.evaluated_depsgraph_get()
            projection = scene.camera.calc_matrix_camera(deps, x=buffer.width, y=buffer.height)
            buffer.draw_view3d(scene, bpy.context.view_layer, target_space, region,
                               scene.camera.matrix_world.inverted(), projection,
                               do_color_management=False, draw_background=True)
            pixels = np.asarray(buffer.texture_color.read(), dtype=np.float32).reshape(
                buffer.height, buffer.width, 4).copy()
            assert np.isfinite(pixels).all(), name
            report['cases'][name] = {'extent': [buffer.width, buffer.height], 'source': source,
                                     'center': pixels[buffer.height // 2, buffer.width // 2, :3].tolist(),
                                     'checks': []}
            core.ACTIVE_CASE = report['cases'][name]
            print('STENCIL_PREVIEW_CASE', name, flush=True)
            return pixels

        def center(pixels):
            h, w = pixels.shape[:2]
            # Stay outside the repeated numerical glyphs, so this samples the label's color.
            return pixels[h // 2, (w // 2 // 32) * 32 + 1, :3]

        # Existing native preview is the color/number reference for all user values.
        references = []
        for value in range(16):
            writer_mat.stencil_reference = value
            pixels = draw(f'final_standard_{value:02}', 'FINAL')
            references.append(pixels)
            snapshot = draw(f'snapshot_standard_{value:02}')
            compare_images(snapshot, pixels)

        for mode in ('EARLY_MARKER', 'MARKER_ONLY'):
            writer_mat.stencil_mode = mode
            for value in range(16):
                writer_mat.stencil_reference = value
                snapshot = draw(f'{mode.lower()}_{value:02}')
                compare_images(snapshot, references[value])
            # This fixture's final attachment no longer contains the early mark.
            native = draw(mode.lower() + '_final_source', 'FINAL')
            compare_images(native, references[0])
            core.near(center(draw(mode.lower() + '_snapshot_restored')), center(references[15]))

        writer_mat.stencil_reference = 7
        for name, hidden in (('hidden', True), ('visible_again', False)):
            writer.hide_viewport = hidden
            core.near(center(draw(name)), center(references[0 if hidden else 7]))
        writer.location.x = 4
        core.near(center(draw('moved_out')), center(references[0]))
        writer.location.x = 0
        core.near(center(draw('moved_back')), center(references[7]))
        writer.keyframe_insert(data_path='location', frame=1)
        writer.location.x = 4
        writer.keyframe_insert(data_path='location', frame=2)
        for index, frame in enumerate((1, 2, 1)):
            scene.frame_set(frame)
            core.near(center(draw(f'frame_{index}_{frame}')), center(references[7 if frame == 1 else 0]))
        writer.animation_data_clear()
        writer.location.x = 0.45
        writer.location.y = 0.6
        writer_mat.stencil_mode = 'STANDARD'
        shifted_reference = draw('shifted_standard_reference', 'FINAL')
        writer_mat.stencil_mode = 'EARLY_MARKER'
        shifted_snapshot = draw('shifted_marker_snapshot')
        compare_images(shifted_snapshot, shifted_reference)
        writer.location.x = writer.location.y = 0
        writer_mat.stencil_mode = 'MARKER_ONLY'
        writer_mat.use_stencil = False
        core.near(center(draw('disabled_marker_only')), center(references[0]))
        writer_mat.use_stencil = True

        second_mat = core.marker_material('Later Marker', reference=3, order=10)
        second = core.plane('Later Marker', second_mat, size=2.4, z=0.2)
        core.near(center(draw('ordered_overlap')), center(references[3]))
        writer_mat.stencil_order = 20
        core.near(center(draw('reversed_order')), center(references[7]))
        second.hide_viewport = True
        core.near(center(draw('resize_snapshot', which=1)), center(references[7]))
        core.near(center(draw('resize_return')), center(references[7]))

        # Marker Only must be visible in the snapshot even without any depth-writing surface.
        receiver.hide_viewport = True
        core.near(center(draw('marker_only_without_receiver')), center(references[7]))
        writer.hide_viewport = True
        core.near(center(draw('empty_scene')), 0)
        writer.hide_viewport = False
        writer_mat.stencil_mode = 'EARLY_MARKER'
        writer_mat.surface_render_method = 'BLENDED'
        core.near(center(draw('forward_only')), center(references[7]))
        writer_mat.surface_render_method = 'DITHERED'
        writer_mat.use_raytrace_refraction = True
        scene.eevee.use_raytracing = True
        core.near(center(draw('refraction_only')), center(references[7]))
        writer_mat.use_raytrace_refraction = False
        writer_mat.stencil_mode = 'MARKER_ONLY'
        receiver.hide_viewport = False
        scene.eevee.use_raytracing = False

        # Leaving preview releases its demand. Returning must still generate this frame's value.
        draw('combined_without_reader', render_pass='COMBINED')
        writer_mat.stencil_reference = 9
        core.near(center(draw('preview_reenabled')), center(references[9]))

        scene.eevee.taa_samples = 16
        scene.eevee.use_taa_reprojection = True
        for index in range(4):
            core.near(center(draw(f'accumulated_snapshot_{index}')), center(references[9]))
        core.near(center(draw('accumulated_switch_to_final', 'FINAL')), center(references[0]))
        core.near(center(draw('accumulated_switch_to_snapshot')), center(references[9]))
        scene.eevee.taa_samples = 1
        scene.eevee.use_taa_reprojection = False

        # Source is a per-viewport setting; one view must not leak its source/extent into another.
        region = next(r for r in area.regions if r.type == 'WINDOW')
        with bpy.context.temp_override(area=area, region=region):
            bpy.ops.screen.area_split(direction='VERTICAL', factor=0.5)
        views = sorted((a for a in bpy.context.screen.areas if a.type == 'VIEW_3D'), key=lambda a: a.x)
        assert len(views) == 2
        area, other_area = views
        for view in views:
            configure_space(view)
        core.near(center(draw('view_one_snapshot', target_area=area)), center(references[9]))
        core.near(center(draw('view_two_final', 'FINAL', which=1, target_area=other_area)),
                  center(references[0]))
        core.near(center(draw('view_one_again', target_area=area)), center(references[9]))

        if args.asset_dir:
            args.asset_dir.mkdir(parents=True, exist_ok=True)
            for view in views:
                view.spaces.active.region_3d.view_perspective = 'CAMERA'
            scene['preview_help'] = 'Viewport Shading > Render Pass: Stencil Value > Source. Left snapshot, right final stencil.'
            bpy.ops.wm.save_as_mainfile(filepath=str(args.asset_dir / 'stencil_preview.blend'))
            report['saved_asset'] = str(args.asset_dir / 'stencil_preview.blend')
        report['passed'] = True
        print('STENCIL_VALUE_PREVIEW_PASSED', flush=True)
    except Exception:
        report['error'] = traceback.format_exc()
        raise
    finally:
        for buffer in buffers:
            buffer.free()
        (args.output_dir / 'verification.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
