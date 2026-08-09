from pathlib import Path
import math
import shutil

import bpy


CASE_DIR = Path(__file__).resolve().parent
ASSET_PATH = CASE_DIR / "assets" / "灯光节点反射探头测试.blend"
OUT_DIR = CASE_DIR / "out"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def configure_render():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 256
    scene.render.resolution_y = 144
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = False
    scene.render.image_settings.file_format = "PNG"
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0

    eevee = scene.eevee
    for attr, value in (
        ("taa_render_samples", 16),
        ("taa_samples", 8),
        ("use_gtao", False),
        ("use_bloom", False),
        ("use_raytracing", True),
        ("gi_cubemap_resolution", "256"),
    ):
        if hasattr(eevee, attr):
            setattr(eevee, attr, value)

    ray_options = getattr(eevee, "ray_tracing_options", None)
    if ray_options is not None:
        for attr, value in (
            ("trace_max_roughness", 1.0),
            ("screen_trace_quality", 1.0),
            ("screen_trace_thickness", 1.0),
            ("use_denoise", False),
            ("denoise_temporal", False),
            ("denoise_spatial", False),
            ("denoise_bilateral", False),
        ):
            if hasattr(ray_options, attr):
                setattr(ray_options, attr, value)


def find_light_shader_nodes():
    outputs = []
    infos = []
    for light in bpy.data.lights:
        tree = light.node_tree
        if tree is None:
            continue
        for node in tree.nodes:
            if node.bl_idname == "ShaderNodeEeveeLightShaderOutput":
                outputs.append(node)
            elif node.bl_idname == "ShaderNodeEeveeLightShaderInfo":
                infos.append(node)
    require(outputs, "asset has no Light Shader Output node")
    require(infos, "asset has no Light Shader Info node")
    return outputs, infos


def render_pixels(label):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    bpy.context.view_layer.update()
    path = OUT_DIR / f"{label}.png"
    bpy.context.scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        pixels = list(image.pixels[:])
        size = tuple(image.size)
    finally:
        bpy.data.images.remove(image)
    return size, pixels, path


def luma_at(pixels, width, x, y):
    index = (y * width + x) * 4
    return pixels[index] * 0.2126 + pixels[index + 1] * 0.7152 + pixels[index + 2] * 0.0722


def sphere_mask(size, pixels):
    width, height = size
    points = []
    for y in range(int(height * 0.16), int(height * 0.93)):
        for x in range(int(width * 0.28), int(width * 0.74)):
            index = (y * width + x) * 4
            r, g, b = pixels[index : index + 3]
            # The reflected metal sphere is the only large neutral bright object in
            # this crop. Keep the threshold low enough for dark checker squares.
            if max(r, g, b) > 0.035 and abs(r - g) < 0.08 and abs(g - b) < 0.08:
                points.append((x, y))
    require(len(points) > 300, f"could not isolate enough reflected sphere pixels: {len(points)}")
    return points


def region_stats(size, pixels, points):
    width, _height = size
    values = [luma_at(pixels, width, x, y) for x, y in points]
    values.sort()
    count = len(values)
    mean = sum(values) / count
    low = sum(values[: max(1, count // 10)]) / max(1, count // 10)
    high = sum(values[-max(1, count // 10) :]) / max(1, count // 10)

    horizontal_deltas = []
    horizontal_second_deltas = []
    point_set = set(points)
    for x, y in points:
        nx = x + 8
        if (nx, y) in point_set:
            horizontal_deltas.append(abs(luma_at(pixels, width, x, y) - luma_at(pixels, width, nx, y)))
        px = x - 8
        if (px, y) in point_set and (nx, y) in point_set:
            horizontal_second_deltas.append(
                abs(
                    luma_at(pixels, width, px, y)
                    - 2.0 * luma_at(pixels, width, x, y)
                    + luma_at(pixels, width, nx, y)
                )
            )
    horizontal_mean_delta = sum(horizontal_deltas) / max(1, len(horizontal_deltas))
    horizontal_second_delta = sum(horizontal_second_deltas) / max(1, len(horizontal_second_deltas))

    return {
        "count": count,
        "mean": mean,
        "low": low,
        "high": high,
        "contrast": high - low,
        "horizontal_mean_delta": horizontal_mean_delta,
        "horizontal_second_delta": horizontal_second_delta,
    }


def disable_custom_light_shader():
    for light in bpy.data.lights:
        tree = light.node_tree
        if tree is None:
            continue
        for node in list(tree.nodes):
            if node.bl_idname.startswith("ShaderNodeEeveeLightShader"):
                tree.nodes.remove(node)


def load_asset():
    require(ASSET_PATH.exists(), f"missing probe asset: {ASSET_PATH}")
    bpy.ops.wm.open_mainfile(filepath=str(ASSET_PATH))
    configure_render()
    outputs, infos = find_light_shader_nodes()
    require(len(outputs) == 1, f"expected one Light Shader Output node, found {len(outputs)}")
    require(len(infos) == 1, f"expected one Light Shader Info node, found {len(infos)}")
    require(
        any(obj.type == "LIGHT_PROBE" and obj.data.type == "SPHERE" for obj in bpy.context.scene.objects),
        "asset has no Sphere Probe",
    )
    require(bpy.context.scene.camera is not None, "asset has no active camera")


def main():
    load_asset()
    size_custom, pixels_custom, custom_path = render_pixels("probe_reflection_checker_custom")
    points = sphere_mask(size_custom, pixels_custom)
    custom_stats = region_stats(size_custom, pixels_custom, points)

    load_asset()
    disable_custom_light_shader()
    size_disabled, pixels_disabled, disabled_path = render_pixels("probe_reflection_checker_disabled")
    require(size_disabled == size_custom, "custom and disabled renders have different sizes")
    disabled_stats = region_stats(size_disabled, pixels_disabled, points)

    print(
        "probe_reflection_checker: "
        f"custom={custom_stats} disabled={disabled_stats} "
        f"custom_path={custom_path} disabled_path={disabled_path}",
        flush=True,
    )

    require(custom_stats["mean"] > 0.08, f"custom reflected sphere is too dark: {custom_stats}")
    require(custom_stats["contrast"] > 0.16, f"custom reflected checker contrast is too low: {custom_stats}")
    require(
        custom_stats["horizontal_mean_delta"] > 0.018,
        f"custom reflected checker has too little local variation: {custom_stats}",
    )
    require(
        custom_stats["horizontal_second_delta"] > 0.04,
        f"custom reflected checker has too little high-frequency variation: {custom_stats}",
    )
    require(
        custom_stats["horizontal_second_delta"] > disabled_stats["horizontal_second_delta"] * 2.0 + 0.01,
        f"disabling custom light shader did not reduce enough checker high-frequency variation: "
        f"custom={custom_stats}, disabled={disabled_stats}",
    )

    print("EEVEE_LIGHT_SHADER_PROBE_REFLECTION_RELEASE_OK", flush=True)


if __name__ == "__main__":
    main()
