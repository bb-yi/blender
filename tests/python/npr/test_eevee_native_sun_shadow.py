# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check standard EEVEE Sun shadows in perspective with a wide clip range."""

import sys
import tempfile
from pathlib import Path

import bpy
from bpy_extras.object_utils import world_to_camera_view
from mathutils import Vector


def sample_ground(scene, camera, image, point):
    width, height = image.size
    ndc = world_to_camera_view(scene, camera, Vector(point))
    x, y = int(ndc.x * width), int(ndc.y * height)
    assert 2 <= x < width - 2 and 2 <= y < height - 2, (point, ndc)
    pixels = image.pixels
    total = 0.0
    for row in range(y - 2, y + 3):
        for col in range(x - 2, x + 3):
            index = 4 * (row * width + col)
            total += sum(pixels[index:index + 3]) / 3.0
    return total / 25.0


def main():
    out = Path(sys.argv[sys.argv.index("--") + 1])
    out.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    scene.render.resolution_x = 384
    scene.render.resolution_y = 384
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.eevee.use_shadows = True
    scene.eevee.taa_render_samples = 8

    bpy.ops.mesh.primitive_plane_add(size=12)
    bpy.ops.mesh.primitive_cube_add(size=1.4, location=(0, 0, 0.7))
    bpy.ops.object.camera_add(location=(4, -5, 7))
    camera = bpy.context.object
    camera.rotation_euler = (-camera.location).to_track_quat('-Z', 'Y').to_euler()
    camera.data.lens = 50.0
    camera.data.clip_start = 0.01
    camera.data.clip_end = 1000.0
    camera.data.ortho_scale = 7.0
    scene.camera = camera

    sun_data = bpy.data.lights.new("Sun", type='SUN')
    sun = bpy.data.objects.new("Sun", sun_data)
    scene.collection.objects.link(sun)
    sun.rotation_euler = Vector((1, 0, -1)).to_track_quat('-Z', 'Y').to_euler()
    sun_data.energy = 2.0
    sun_data.angle = 0.00918
    sun_data.shadow_maximum_resolution = 0.0001
    sun_data.use_shadow_jitter = False

    with tempfile.TemporaryDirectory(dir=out) as directory:
        for projection in ('PERSP', 'ORTHO'):
            camera.data.type = projection
            samples = []
            for shadow in (False, True):
                sun_data.use_shadow = shadow
                scene.render.filepath = str(Path(directory) / f"{projection}_{shadow}.exr")
                bpy.ops.render.render(write_still=True)
                image = bpy.data.images.load(scene.render.filepath, check_existing=False)
                try:
                    samples.append((
                        sample_ground(scene, camera, image, (1.23, 0, 0)),
                        sample_ground(scene, camera, image, (2.3, -1.4, 0)),
                    ))
                finally:
                    bpy.data.images.remove(image)

            (blocked_off, clear_off), (blocked_on, clear_on) = samples
            print(f"{projection}: shadowed={blocked_on:.4f}/{blocked_off:.4f}, "
                  f"unblocked={clear_on:.4f}/{clear_off:.4f}", flush=True)
            assert blocked_off > 0.05 and clear_off > 0.05, (projection, samples)
            assert blocked_on < 0.3 * blocked_off, (projection, samples)
            assert 0.9 * clear_off < clear_on < 1.1 * clear_off, (projection, samples)

    print("EEVEE_NATIVE_SUN_SHADOW_COMPLETE")


if __name__ == "__main__":
    main()
