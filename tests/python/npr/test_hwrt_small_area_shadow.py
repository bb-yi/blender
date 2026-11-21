# SPDX-License-Identifier: GPL-2.0-or-later

"""Normalized small area lights keep their illumination and can cast hard HWRT shadows."""

import argparse
import json
from pathlib import Path
import sys

import bpy
import gpu
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_shadow_precision_penumbra import configure, receiver_material


def plane(scene, name, x_min, x_max, z, material):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata([(x_min, -1, z), (x_max, -1, z), (x_max, 1, z), (x_min, 1, z)],
                     [], [(0, 1, 2, 3)])
    mesh.materials.append(material)
    obj = bpy.data.objects.new(name, mesh)
    obj.shadow_terminator_geometry_offset = 0
    scene.collection.objects.link(obj)
    return obj


def capture(view, label, out, saved):
    scene, space, region, camera, offscreen = view
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    width, height = offscreen.width, offscreen.height
    projection = camera.calc_matrix_camera(depsgraph, x=width, y=height)
    offscreen.draw_view3d(scene, bpy.context.view_layer, space, region,
                          camera.matrix_world.inverted(), projection, do_color_management=False)
    pixels = np.asarray(offscreen.texture_color.read(), dtype=np.float32).reshape(height, width, 4)
    assert np.isfinite(pixels).all(), label
    profile = pixels[height // 4:3 * height // 4, :, 0].mean(axis=0)
    lit = float(profile[3 * width // 4:7 * width // 8].mean())
    shaded = float(profile[width // 8:width // 4].mean())
    mid = profile[width // 4:3 * width // 4]
    result = {"lit": lit, "shaded": shaded,
              "penumbra_pixels": int(((mid > 0.1 * lit) & (mid < 0.9 * lit)).sum())}
    if saved:
        image = bpy.data.images.new(label, width, height, alpha=True, float_buffer=True)
        image.pixels.foreach_set(pixels.ravel())
        scene.render.image_settings.file_format = "PNG"
        image.save_render(str(out / (label + ".png")), scene=scene)
        bpy.data.images.remove(image)
    print("SMALL_AREA", label, result, flush=True)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:])
    args.out.mkdir(parents=True, exist_ok=True)
    width, height = 768, 256
    scene = configure(width)
    gpu.init()
    assert gpu.platform.backend_type_get() == "VULKAN"
    e = scene.eevee
    assert e.is_hardware_raytracing_supported
    e.use_shadows = True
    e.use_raytracing = True
    e.ray_tracing_method = "PROBE"
    e.use_fast_gi = False
    e.hardware_raytracing_shadow_samples = 16
    scene.render.use_compositing = False
    scene.world.use_nodes = True
    scene.world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0
    material = receiver_material("NATIVE_BSDF", 1)
    plane(scene, "Receiver", -1, 1, 0, material)
    plane(scene, "Blocker", -1, 0, 1, material.copy()).visible_camera = False

    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 0.24
    camera = bpy.data.objects.new("Camera", camera_data)
    scene.collection.objects.link(camera)
    camera.location.z = 4
    scene.camera = camera

    light = bpy.data.lights.new("Area", "AREA")
    emitter = bpy.data.objects.new("Area", light)
    scene.collection.objects.link(emitter)
    emitter.location.z = 2
    emitter.scale = (0.5, 0.5, 1)
    light.energy = 100
    light.normalize = True
    light.use_shadow_jitter = False
    light.shadow_filter_radius = 0
    light.shadow_maximum_resolution = 0.0001

    area = next(item for item in bpy.context.screen.areas if item.type == "VIEW_3D")
    region = next(item for item in area.regions if item.type == "WINDOW")
    space = area.spaces.active
    space.shading.type = "RENDERED"
    space.shading.use_scene_world_render = True
    space.shading.use_scene_lights_render = True
    space.shading.use_compositor = "DISABLED"
    space.overlay.show_overlays = False
    space.camera = camera
    space.region_3d.view_perspective = "CAMERA"
    offscreen = gpu.types.GPUOffScreen(width, height, format="RGBA16F")
    view = scene, space, region, camera, offscreen
    report = {}
    try:
        for mode in ("maps", "hwrt"):
            e.ray_tracing_options.use_hardware_shadows = mode == "hwrt"
            for size in (0.024, 0.0128, 0.012, 0.006, 0.002, 0.0):
                light.size = size
                label = f"{mode}_{size:g}"
                report[label] = capture(view, label, args.out, size in (0.0128, 0.012, 0.002, 0.0))
        light.use_shadow = False
        light.size = 0.002
        report["no_shadow_small"] = capture(view, "no_shadow_small", args.out, False)
        light.use_shadow = True
        light.shape = "RECTANGLE"
        light.size = 0.0
        light.size_y = 0.024
        report["hwrt_zero_width"] = capture(view, "hwrt_zero_width", args.out, False)
        light.normalize = False
        report["unnormalized_zero_width"] = capture(view, "unnormalized_zero_width", args.out, False)
        light.normalize = True
        light.shape = "SQUARE"
        material.surface_render_method = "BLENDED"
        report["hwrt_blended_0"] = capture(view, "hwrt_blended_0", args.out, False)
    finally:
        offscreen.free()

    (args.out / "results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    for mode in ("maps", "hwrt"):
        reference = report[f"{mode}_0.024"]["lit"]
        assert reference > 0.05, report
        for size in (0.012, 0.006, 0.002, 0.0):
            assert abs(report[f"{mode}_{size:g}"]["lit"] / reference - 1) < 0.05, report
    soft = report["hwrt_0.024"]["penumbra_pixels"]
    assert soft >= 8, report
    assert 0 < report["hwrt_0.002"]["penumbra_pixels"] < soft / 3, report
    for key in ("hwrt_0", "hwrt_zero_width", "hwrt_blended_0"):
        assert report[key]["lit"] > 0.05, report
        assert report[key]["penumbra_pixels"] <= 1, report
        assert report[key]["shaded"] < 0.01 * report[key]["lit"], report
    assert report["no_shadow_small"]["lit"] > 0.05, report
    assert report["unnormalized_zero_width"]["lit"] < 0.001, report
    print("SMALL_AREA_OK", flush=True)


if __name__ == "__main__":
    main()
