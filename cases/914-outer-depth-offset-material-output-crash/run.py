from pathlib import Path

import bpy


CASE_DIR = Path(__file__).resolve().parent
ROOT = CASE_DIR.parents[3]
BLEND_FILES = sorted((CASE_DIR / "assets").glob("*.blend"))
OUTPUT_DIR = ROOT / "temp" / "release_test_outputs" / "depth_offset_material_output_crash"


def configure_render(use_shadows=True):
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 256
    scene.render.resolution_y = 256
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"

    scene.eevee.taa_samples = 1
    scene.eevee.taa_render_samples = 1
    scene.eevee.use_shadows = use_shadows
    if hasattr(scene.eevee, "use_taa_reprojection"):
        scene.eevee.use_taa_reprojection = False

    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0


def material_output_depth_offset_links(material):
    links = []
    if material.node_tree is None:
        return links
    for node in material.node_tree.nodes:
        if node.bl_idname != "ShaderNodeOutputMaterial":
            continue
        depth_offset = node.inputs.get("Depth Offset")
        surface = node.inputs.get("Surface")
        if (
            depth_offset is not None
            and depth_offset.is_linked
            and surface is not None
            and surface.is_linked
        ):
            links.extend(depth_offset.links)
    return links


def material_output_has_depth_offset_link(material):
    return bool(material_output_depth_offset_links(material))


def set_depth_offset_value(materials, value):
    updated = []
    for material in materials:
        for link in material_output_depth_offset_links(material):
            socket = link.from_socket
            if hasattr(socket, "default_value"):
                socket.default_value = value
                updated.append((material.name, link.from_node.name, socket.name))

    assert updated, (
        "Expected the repro blend's linked Depth Offset source to expose a writable default_value."
    )
    bpy.context.view_layer.update()
    print(f"DEPTH_OFFSET_SET value:{value:.6f} sources:{updated}")


def load_pixels(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        return list(image.pixels[:]), int(image.size[0]), int(image.size[1])
    finally:
        bpy.data.images.remove(image)


def region_stats(pixels, width, height, x0, y0, x1, y1):
    non_black = 0
    sum_r = 0.0
    sum_g = 0.0
    sum_b = 0.0
    sum_a = 0.0
    max_r = 0.0
    max_g = 0.0
    max_b = 0.0
    max_a = 0.0

    for y in range(y0, y1):
        for x in range(x0, x1):
            index = (y * width + x) * 4
            r = pixels[index]
            g = pixels[index + 1]
            b = pixels[index + 2]
            a = pixels[index + 3]
            sum_r += r
            sum_g += g
            sum_b += b
            sum_a += a
            max_r = max(max_r, r)
            max_g = max(max_g, g)
            max_b = max(max_b, b)
            max_a = max(max_a, a)
            if max(r, g, b) > 0.02:
                non_black += 1

    count = (x1 - x0) * (y1 - y0)

    return {
        "avg_r": sum_r / count,
        "avg_g": sum_g / count,
        "avg_b": sum_b / count,
        "avg_a": sum_a / count,
        "avg_luma": (sum_r + sum_g + sum_b) / (count * 3.0),
        "max_r": max_r,
        "max_g": max_g,
        "max_b": max_b,
        "max_a": max_a,
        "non_black_ratio": non_black / count,
    }


def render_and_sample(label, depth_offset_value, use_shadows):
    set_depth_offset_value(depth_offset_materials, depth_offset_value)
    configure_render(use_shadows=use_shadows)

    output_path = OUTPUT_DIR / f"depth_offset_material_output_{label}.png"
    bpy.context.scene.render.filepath = str(output_path)
    bpy.ops.render.render(write_still=True)
    assert output_path.exists(), f"Render did not write {output_path}"

    pixels, width, height = load_pixels(output_path)
    return region_stats(pixels, width, height, 88, 88, 168, 168)


assert len(BLEND_FILES) == 1, f"Expected exactly one repro blend in assets, got {BLEND_FILES}"
BLEND_PATH = BLEND_FILES[0]

bpy.ops.wm.open_mainfile(filepath=str(BLEND_PATH))

depth_offset_materials = [
    material for material in bpy.data.materials if material_output_has_depth_offset_link(material)
]
assert depth_offset_materials, (
    "Expected the repro blend to contain a material output with linked Surface and Depth Offset."
)

OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

positive_center = render_and_sample("positive_shadow", 0.42, True)
negative_center = render_and_sample("negative_no_shadow", -0.42, False)
print(
    "DEPTH_OFFSET_RENDER_STATS="
    f"positive_center_avg:{positive_center['avg_r']:.6f},"
    f"{positive_center['avg_g']:.6f},{positive_center['avg_b']:.6f},"
    f"{positive_center['avg_a']:.6f} "
    f"positive_center_luma:{positive_center['avg_luma']:.6f} "
    f"negative_center_avg:{negative_center['avg_r']:.6f},"
    f"{negative_center['avg_g']:.6f},{negative_center['avg_b']:.6f},"
    f"{negative_center['avg_a']:.6f} "
    f"negative_center_luma:{negative_center['avg_luma']:.6f}"
)

assert positive_center["avg_a"] >= 0.90 and positive_center["non_black_ratio"] >= 0.95, (
    "Expected positive Depth Offset to keep opaque visible output rather than a transparent/black "
    f"cutout, got stats {positive_center}"
)
assert positive_center["avg_g"] >= 0.70, (
    "Expected positive Depth Offset to show the green offset material in the center, "
    f"got stats {positive_center}"
)
assert positive_center["avg_g"] - max(positive_center["avg_r"], positive_center["avg_b"]) >= 0.10, (
    "Expected positive Depth Offset center to be green-dominant, "
    f"got stats {positive_center}"
)
assert negative_center["avg_a"] >= 0.90 and negative_center["avg_r"] >= 0.70, (
    "Expected negative Depth Offset to show the red plane in the center, "
    f"got stats {negative_center}"
)
assert negative_center["avg_r"] - max(negative_center["avg_g"], negative_center["avg_b"]) >= 0.20, (
    "Expected negative Depth Offset center to be red-dominant rather than the green offset "
    f"material, got stats {negative_center}"
)

print("DEPTH_OFFSET_MATERIAL_OUTPUT_CRASH_RELEASE_CASE_OK")
