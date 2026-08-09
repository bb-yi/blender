from pathlib import Path

import bpy


ROOT = Path(__file__).resolve().parents[4]
OUTPUT_DIR = ROOT / "temp" / "release_test_outputs" / "outline_raytrace_transmission_occlusion"
OUTLINE_THRESHOLD = 0.2


def configure_render():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 960
    scene.render.resolution_y = 540
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.eevee.taa_samples = 1
    scene.eevee.taa_render_samples = 1
    scene.eevee.use_taa_reprojection = False
    scene.eevee.use_raytracing = True
    scene.eevee.ray_tracing_method = "SCREEN"
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0


def set_plane_raytrace_transmission(enabled):
    material = bpy.data.materials.get("Material.001")
    assert material is not None, "Expected foreground plane material Material.001"
    assert hasattr(material, "use_screen_refraction"), "Material missing use_screen_refraction"
    assert hasattr(material, "use_raytrace_refraction"), "Material missing use_raytrace_refraction"
    material.use_screen_refraction = enabled
    material.use_raytrace_refraction = enabled


def render_variant(blend_name, output_name, raytrace_transmission):
    blend_path = ROOT / "test" / blend_name
    assert blend_path.exists(), f"Missing blend file: {blend_path}"

    bpy.ops.wm.open_mainfile(filepath=str(blend_path))
    configure_render()
    set_plane_raytrace_transmission(raytrace_transmission)

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    output_path = OUTPUT_DIR / output_name
    bpy.context.scene.render.filepath = str(output_path)
    bpy.ops.render.render(write_still=True)
    assert output_path.exists(), f"Render did not write {output_path}"
    return output_path


def load_pixels(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        return list(image.pixels[:]), int(image.size[0]), int(image.size[1])
    finally:
        bpy.data.images.remove(image)


def is_bright_outline(pixels, index):
    return max(pixels[index], pixels[index + 1], pixels[index + 2]) > OUTLINE_THRESHOLD


def bright_mask_stats(path, roi):
    pixels, width, height = load_pixels(path)
    x0 = max(0, min(width, int(width * roi[0])))
    y0 = max(0, min(height, int(height * roi[1])))
    x1 = max(0, min(width, int(width * roi[2])))
    y1 = max(0, min(height, int(height * roi[3])))
    count = 0
    area = max(1, (x1 - x0) * (y1 - y0))

    for y in range(y0, y1):
        row = y * width
        for x in range(x0, x1):
            index = (row + x) * 4
            if is_bright_outline(pixels, index):
                count += 1

    return {
        "path": path,
        "width": width,
        "height": height,
        "count": count,
        "area": area,
        "ratio": count / area,
    }


def compare_bright_masks(path_a, path_b, roi):
    pixels_a, width_a, height_a = load_pixels(path_a)
    pixels_b, width_b, height_b = load_pixels(path_b)
    assert (width_a, height_a) == (width_b, height_b), (
        f"Render sizes differ: {path_a} is {width_a}x{height_a}, "
        f"{path_b} is {width_b}x{height_b}"
    )

    x0 = max(0, min(width_a, int(width_a * roi[0])))
    y0 = max(0, min(height_a, int(height_a * roi[1])))
    x1 = max(0, min(width_a, int(width_a * roi[2])))
    y1 = max(0, min(height_a, int(height_a * roi[3])))
    count_a = 0
    count_b = 0
    different = 0

    for y in range(y0, y1):
        row = y * width_a
        for x in range(x0, x1):
            index = (row + x) * 4
            bright_a = is_bright_outline(pixels_a, index)
            bright_b = is_bright_outline(pixels_b, index)
            count_a += int(bright_a)
            count_b += int(bright_b)
            different += int(bright_a != bright_b)

    union = max(1, count_a + count_b - (count_a + count_b - different) // 2)
    relative_count_delta = abs(count_a - count_b) / max(count_a, count_b, 1)
    diff_ratio = different / union
    return {
        "path_a": path_a,
        "path_b": path_b,
        "count_a": count_a,
        "count_b": count_b,
        "different": different,
        "relative_count_delta": relative_count_delta,
        "diff_ratio": diff_ratio,
    }


half_on = render_variant("描边透射遮挡测试.blend", "half_rt_on.png", True)
half_off = render_variant("描边透射遮挡测试.blend", "half_rt_off.png", False)
covered_on = render_variant("折射通道测试.blend", "covered_rt_on.png", True)

visible_roi = (0.48, 0.06, 0.94, 0.94)
covered_roi = (0.24, 0.08, 0.86, 0.94)

half_stats_on = bright_mask_stats(half_on, visible_roi)
half_stats_off = bright_mask_stats(half_off, visible_roi)
print("half_rt_on", half_stats_on)
print("half_rt_off", half_stats_off)

assert half_stats_on["count"] > 5000, (
    "Raytrace Transmission ON should keep visible Suzanne contour and internal outlines, "
    f"got {half_stats_on['count']} bright outline pixels in {half_on}"
)
assert half_stats_off["count"] > 5000, (
    "Raytrace Transmission OFF baseline should contain visible Suzanne outlines, "
    f"got {half_stats_off['count']} bright outline pixels in {half_off}"
)

mask_compare = compare_bright_masks(half_on, half_off, visible_roi)
print("half_on_off_mask_compare", mask_compare)
assert mask_compare["relative_count_delta"] <= 0.05, (
    "Raytrace Transmission ON/OFF should keep a similar amount of visible Suzanne outlines, "
    f"got comparison {mask_compare}"
)
assert mask_compare["diff_ratio"] <= 0.05, (
    "Raytrace Transmission ON/OFF visible outline masks diverged, "
    f"got comparison {mask_compare}"
)

covered_stats = bright_mask_stats(covered_on, covered_roi)
print("covered_rt_on", covered_stats)
assert covered_stats["count"] <= 100, (
    "Covered Suzanne outlines should be occluded by the foreground raytrace-transmission plane, "
    f"got {covered_stats['count']} bright outline pixels in {covered_on}"
)
