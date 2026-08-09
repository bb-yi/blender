"""Release smoke test for the atmospheric World and volumetric clouds."""

from __future__ import annotations

import hashlib
import json
import math
import os
import sys
import tempfile
from pathlib import Path

import bpy
from mathutils import Vector


sys.dont_write_bytecode = True
CASE_DIR = Path(__file__).resolve().parent
ROOT = CASE_DIR.parents[3]
ASSET_DIR = CASE_DIR / "assets"
MANIFEST_PATH = ASSET_DIR / "asset_manifest.json"
TOOL_DIR = ROOT / "tools" / "atmospheric_sky_clouds"
RESOLUTION = (48, 32)

EXPECTED_ASSETS = {
    "blue_noise",
    "cloud_detail",
    "cloud_shape",
    "moon_albedo",
    "sky_view",
    "stars",
    "transmittance",
    "weather",
}
VOLUME_ASSETS = {"cloud_detail", "cloud_shape", "sky_view"}

if str(TOOL_DIR) not in sys.path:
    sys.path.insert(0, str(TOOL_DIR))

from setup_world_scene import SAMPLER_BINDINGS, build_world, find_socket


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def validate_assets() -> dict:
    require(MANIFEST_PATH.is_file(), f"Asset manifest is missing: {MANIFEST_PATH}")
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    require(manifest.get("schema_version") == 1, "Expected asset manifest schema_version 1")

    assets = manifest.get("assets")
    require(isinstance(assets, dict), "Manifest assets must be an object")
    require(set(assets) == EXPECTED_ASSETS,
            f"Expected assets {sorted(EXPECTED_ASSETS)}, got {sorted(assets)}")
    require(set(SAMPLER_BINDINGS) == EXPECTED_ASSETS,
            "Production sampler bindings do not match the smoke asset contract")

    asset_root = ASSET_DIR.resolve()
    for asset_id in sorted(EXPECTED_ASSETS):
        entry = assets[asset_id]
        relative_path = Path(str(entry.get("path", "")))
        require(not relative_path.is_absolute() and ".." not in relative_path.parts,
                f"{asset_id}: unsafe asset path {relative_path}")
        path = (ASSET_DIR / relative_path).resolve()
        require(asset_root in path.parents, f"{asset_id}: asset escapes case directory")
        require(path.is_file(), f"{asset_id}: asset file is missing: {path}")
        require(path.stat().st_size == int(entry.get("byte_size", -1)),
                f"{asset_id}: byte size differs from manifest")
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        require(digest == entry.get("sha256"),
                f"{asset_id}: SHA-256 differs from manifest: {digest}")

        logical = [int(value) for value in entry.get("logical_size", [])]
        physical = [int(value) for value in entry.get("physical_size", [])]
        require(all(value > 0 for value in logical + physical),
                f"{asset_id}: dimensions must be positive")
        if asset_id in VOLUME_ASSETS:
            require(len(logical) == 3 and len(physical) == 2,
                    f"{asset_id}: expected a 3D logical texture in a 2D strip")
            require(physical == [logical[0] * logical[2], logical[1]],
                    f"{asset_id}: strip does not use physical_x = z * width + x")
            strip = entry.get("strip", {})
            require(strip.get("pixel_mapping") == "physical_x = z * width + x",
                    f"{asset_id}: missing strip pixel mapping contract")
        else:
            require(len(logical) == 2 and physical == logical,
                    f"{asset_id}: expected matching 2D logical/physical dimensions")

    return manifest


def configure_scene(scene: bpy.types.Scene) -> bpy.types.Object:
    scene.render.engine = "BLENDER_EEVEE"
    require(scene.render.engine == "BLENDER_EEVEE", "BLENDER_EEVEE is unavailable")
    scene.render.resolution_x, scene.render.resolution_y = RESOLUTION
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = False
    scene.view_settings.view_transform = "Standard"
    try:
        scene.view_settings.look = "None"
    except (TypeError, ValueError):
        pass
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    if hasattr(scene, "eevee"):
        if hasattr(scene.eevee, "taa_samples"):
            scene.eevee.taa_samples = 1
        if hasattr(scene.eevee, "taa_render_samples"):
            scene.eevee.taa_render_samples = 1

    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    camera_data = bpy.data.cameras.new("AtmosphericSkyCloudsReleaseCamera")
    camera_data.lens = 34.0
    camera_data.sensor_width = 36.0
    camera = bpy.data.objects.new("AtmosphericSkyCloudsReleaseCamera", camera_data)
    camera.location = (0.0, 0.0, 0.0)
    scene.collection.objects.link(camera)
    scene.camera = camera
    return camera


def point_camera(camera: bpy.types.Object, direction: Vector) -> None:
    camera.rotation_euler = direction.normalized().to_track_quat("-Z", "Y").to_euler()


def set_input(node: bpy.types.Node, name: str, value) -> None:
    find_socket(node.inputs, name).default_value = value


def unlink_time_inputs(node: bpy.types.Node) -> None:
    tree = node.id_data
    for link in list(tree.links):
        if link.to_node == node and link.to_socket.identifier in {"In_time_seconds", "In_frame"}:
            tree.links.remove(link)


def render_pixels(scene: bpy.types.Scene) -> list[float]:
    file_descriptor, filepath = tempfile.mkstemp(suffix=".exr")
    os.close(file_descriptor)
    loaded = None
    try:
        scene.render.image_settings.file_format = "OPEN_EXR"
        scene.render.image_settings.color_mode = "RGBA"
        scene.render.image_settings.color_depth = "32"
        scene.render.filepath = filepath
        result = bpy.ops.render.render(write_still=False)
        require(result == {"FINISHED"}, f"Eevee render operator returned {result}")
        render_result = bpy.data.images.get("Render Result")
        require(render_result is not None, "Eevee did not produce Render Result")
        render_result.save_render(filepath, scene=scene)
        loaded = bpy.data.images.load(filepath, check_existing=False)
        width, height = map(int, loaded.size)
        require((width, height) == RESOLUTION,
                f"Expected render size {RESOLUTION}, got {(width, height)}")
        pixels = list(loaded.pixels[:])
        require(len(pixels) == width * height * 4,
                f"Unexpected pixel count {len(pixels)}")
        require(all(math.isfinite(value) for value in pixels),
                "Render contains a non-finite channel")
        return pixels
    finally:
        if loaded is not None:
            bpy.data.images.remove(loaded)
        if os.path.exists(filepath):
            os.remove(filepath)


def rgb_values(pixels: list[float]) -> list[float]:
    return [pixels[index + channel]
            for index in range(0, len(pixels), 4)
            for channel in range(3)]


def mean_luma(pixels: list[float]) -> float:
    count = len(pixels) // 4
    total = 0.0
    for index in range(0, len(pixels), 4):
        total += (pixels[index] * 0.2126 +
                  pixels[index + 1] * 0.7152 +
                  pixels[index + 2] * 0.0722)
    return total / count


def mean_abs_rgb_difference(first: list[float], second: list[float]) -> float:
    require(len(first) == len(second), "Render sizes differ")
    total = 0.0
    count = 0
    for index in range(0, len(first), 4):
        for channel in range(3):
            total += abs(first[index + channel] - second[index + channel])
            count += 1
    return total / count


def validate_world_boundary(result: dict, manifest: dict) -> None:
    world = result["world"]
    glsl = result["glsl"]
    require(glsl.parse_status == "READY",
            f"Expected World GLSL parse READY, got {glsl.parse_status}")
    require(glsl.id_data is world.node_tree, "GLSL Function is not in the World node tree")
    require(len(result["image_nodes"]) == len(EXPECTED_ASSETS),
            "Not all smoke assets have Image to Closure nodes")

    for identifier in ("Result", "out_cloud_alpha", "out_density", "out_steps"):
        find_socket(glsl.outputs, identifier)
    official_runtime_inputs = {
        "official_noise_scale": "VECTOR",
        "official_noise_offset": "VECTOR",
        "official_noise_speed": "VALUE",
        "official_density_multiplier": "VALUE",
        "official_light_absorption": "VALUE",
        "official_light_intensity": "VALUE",
        "official_cloud_color": "RGBA",
        "official_light_color": "RGBA",
        "official_ambient_color": "RGBA",
        "official_ray_steps": "INT",
        "official_light_steps": "INT",
    }
    for identifier, socket_type in official_runtime_inputs.items():
        socket = find_socket(glsl.inputs, identifier)
        require(socket.type == socket_type,
                f"{identifier}: expected {socket_type}, got {socket.type}")
    for asset_id, sampler_name in SAMPLER_BINDINGS.items():
        socket = find_socket(glsl.inputs, sampler_name)
        require(socket.type == "CLOSURE" and socket.is_linked,
                f"{sampler_name}: expected a linked Closure sampler socket")
        image_node = result["image_nodes"][asset_id]
        image = result["images"][asset_id]
        physical = tuple(int(value) for value in manifest["assets"][asset_id]["physical_size"])
        require(tuple(image.size) == physical,
                f"{asset_id}: Blender loaded {tuple(image.size)}, expected {physical}")
        require(image.colorspace_settings.name == "Non-Color",
                f"{asset_id}: expected Non-Color, got {image.colorspace_settings.name}")
        expected_type = "LUT_STRIP_3D" if asset_id in VOLUME_ASSETS else "IMAGE_2D"
        require(image_node.texture_type == expected_type,
                f"{asset_id}: expected {expected_type}, got {image_node.texture_type}")


def main() -> None:
    manifest = validate_assets()
    bpy.ops.wm.read_homefile(use_factory_startup=True)
    result = build_world(MANIFEST_PATH, "Preview", "AtmosphericSkyCloudsReleaseWorld")
    validate_world_boundary(result, manifest)

    scene = bpy.context.scene
    scene.world = result["world"]
    camera = configure_scene(scene)
    glsl = result["glsl"]
    unlink_time_inputs(glsl)
    set_input(glsl, "day_length_seconds", 24.0)
    set_input(glsl, "time_offset_hours", 0.0)
    set_input(glsl, "sun_azimuth", 0.0)
    set_input(glsl, "quality", 0)
    set_input(glsl, "jitter_mode", 0)
    set_input(glsl, "frame", 0.0)
    set_input(glsl, "coverage", 0.0)
    set_input(glsl, "debug_mode", 1)
    point_camera(camera, Vector((0.0, 0.15, 1.0)))

    set_input(glsl, "time_seconds", 12.0)
    noon = render_pixels(scene)
    set_input(glsl, "time_seconds", 0.0)
    midnight = render_pixels(scene)
    noon_luma = mean_luma(noon)
    midnight_luma = mean_luma(midnight)
    day_night_delta = mean_abs_rgb_difference(noon, midnight)
    require(max(rgb_values(noon)) > 0.02, "Noon atmosphere render is empty")
    require(max(rgb_values(midnight)) > 0.001, "Midnight atmosphere render is empty")
    require(noon_luma > midnight_luma * 1.10,
            f"Time control did not make noon brighter: {noon_luma} vs {midnight_luma}")
    require(day_night_delta > 0.01,
            f"Time control did not change atmosphere pixels enough: {day_night_delta}")

    set_input(glsl, "time_seconds", 9.0)
    set_input(glsl, "frame", 216.0)
    set_input(glsl, "coverage", 0.68)
    set_input(glsl, "density_scale", 1.05)
    set_input(glsl, "erosion_strength", 0.22)
    set_input(glsl, "debug_mode", 2)
    point_camera(camera, Vector((0.0, 1.0, 0.42)))
    cloud = render_pixels(scene)
    cloud_red = cloud[0::4]
    cloud_min = min(cloud_red)
    cloud_max = max(cloud_red)
    cloud_mean = sum(cloud_red) / len(cloud_red)
    grayscale_error = max(
        abs(cloud[index] - cloud[index + channel])
        for index in range(0, len(cloud), 4)
        for channel in (1, 2)
    )
    require(cloud_min >= -0.002 and cloud_max <= 1.002,
            f"Cloud Alpha left [0, 1]: min={cloud_min}, max={cloud_max}")
    require(cloud_max > 0.12, f"Cloud raymarch produced no body: max={cloud_max}")
    require(cloud_min < 0.08, f"Cloud raymarch produced no clear holes: min={cloud_min}")
    require(cloud_max - cloud_min > 0.10,
            f"Cloud Alpha is too flat: min={cloud_min}, max={cloud_max}")
    require(grayscale_error < 0.002,
            f"Cloud Alpha debug output is not grayscale: error={grayscale_error}")

    print(
        "ATMOSPHERIC_SKY_CLOUDS_RELEASE_OK "
        f"parse={glsl.parse_status} assets={len(EXPECTED_ASSETS)} "
        f"noon_luma={noon_luma:.6f} midnight_luma={midnight_luma:.6f} "
        f"day_night_delta={day_night_delta:.6f} "
        f"cloud_min={cloud_min:.6f} cloud_max={cloud_max:.6f} "
        f"cloud_mean={cloud_mean:.6f}"
    )


if __name__ == "__main__":
    main()
