from pathlib import Path

import bpy


CASE_DIR = Path(__file__).resolve().parent
ROOT = CASE_DIR.parents[3]
BLEND_PATH = CASE_DIR / "assets" / "透射AOV遮挡测试.blend"
OUTPUT_DIR = ROOT / "temp" / "release_test_outputs" / "transmission_aov_occlusion_real_blend"


def configure_render():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 128
    scene.render.resolution_y = 128
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.eevee.taa_samples = 1
    scene.eevee.taa_render_samples = 1
    scene.eevee.use_taa_reprojection = False
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0


def load_pixels(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        return list(image.pixels[:]), int(image.size[0]), int(image.size[1])
    finally:
        bpy.data.images.remove(image)


def sample_rgb(pixels, width, height, x_ratio, y_ratio):
    x = max(0, min(width - 1, int(width * x_ratio)))
    y = max(0, min(height - 1, int(height * y_ratio)))
    index = (y * width + x) * 4
    return pixels[index], pixels[index + 1], pixels[index + 2]


def region_stats(pixels, width, height, min_x_ratio, max_x_ratio, min_y_ratio, max_y_ratio):
    min_x = max(0, min(width - 1, int(width * min_x_ratio)))
    max_x = max(min_x + 1, min(width, int(width * max_x_ratio)))
    min_y = max(0, min(height - 1, int(height * min_y_ratio)))
    max_y = max(min_y + 1, min(height, int(height * max_y_ratio)))

    red = []
    green = []
    blue = []
    red_dominant = 0
    non_black = 0

    for y in range(min_y, max_y):
        row = y * width
        for x in range(min_x, max_x):
            index = (row + x) * 4
            r = pixels[index]
            g = pixels[index + 1]
            b = pixels[index + 2]
            red.append(r)
            green.append(g)
            blue.append(b)
            if max(r, g, b) > 0.08:
                non_black += 1
            if r > 0.18 and r > g * 2.5 and r > b * 2.5:
                red_dominant += 1

    count = len(red)
    return {
        "avg_r": sum(red) / count,
        "avg_g": sum(green) / count,
        "avg_b": sum(blue) / count,
        "max_r": max(red),
        "max_g": max(green),
        "max_b": max(blue),
        "red_dominant_ratio": red_dominant / count,
        "non_black_ratio": non_black / count,
    }


def material_has_aov_npr_tree(material_name, aov_name):
    material = bpy.data.materials.get(material_name)
    if material is None or material.node_tree is None:
        return False
    for node in material.node_tree.nodes:
        if node.bl_idname != "ShaderNodeOutputMaterial":
            continue
        npr_tree = getattr(node, "nprtree", None)
        if npr_tree is None:
            continue
        return any(
            n.bl_idname == "ShaderNodeInputAOV" and getattr(n, "aov_name", "") == aov_name
            for n in npr_tree.nodes
        )
    return False


assert BLEND_PATH.exists(), f"Missing blend file: {BLEND_PATH}"

bpy.ops.wm.open_mainfile(filepath=str(BLEND_PATH))
scene = bpy.context.scene
view_layer = bpy.context.view_layer

configure_render()

assert any(aov.name == "AOV_001" and aov.type == "COLOR" for aov in view_layer.aovs), (
    "Expected the repro blend to keep the red AOV_001 color AOV."
)
assert material_has_aov_npr_tree("Material.001", "AOV_001"), (
    "Expected the front sphere material to have an NPR Tree reading AOV_001."
)

OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
output_path = OUTPUT_DIR / "transmission_aov_occlusion_real_blend.png"
scene.render.filepath = str(output_path)
bpy.ops.render.render(write_still=True)

assert output_path.exists(), f"Render did not write {output_path}"

pixels, width, height = load_pixels(output_path)
center_rgb = sample_rgb(pixels, width, height, 0.5, 0.5)
corner_rgb = sample_rgb(pixels, width, height, 0.05, 0.05)
center_stats = region_stats(pixels, width, height, 0.42, 0.58, 0.42, 0.58)

corner_brightness = max(corner_rgb)

print(f"CENTER_RGB={center_rgb[0]:.6f},{center_rgb[1]:.6f},{center_rgb[2]:.6f}")
print(f"CORNER_RGB={corner_rgb[0]:.6f},{corner_rgb[1]:.6f},{corner_rgb[2]:.6f}")
print(
    "CENTER_STATS="
    f"avg:{center_stats['avg_r']:.6f},{center_stats['avg_g']:.6f},{center_stats['avg_b']:.6f} "
    f"max:{center_stats['max_r']:.6f},{center_stats['max_g']:.6f},{center_stats['max_b']:.6f} "
    f"red_ratio:{center_stats['red_dominant_ratio']:.6f} "
    f"non_black_ratio:{center_stats['non_black_ratio']:.6f}"
)

assert center_stats["non_black_ratio"] >= 0.20, (
    "Expected the sphere center region to read visible AOV data instead of black, "
    f"got stats {center_stats}"
)
assert center_stats["red_dominant_ratio"] >= 0.15, (
    "Expected the sphere center region to contain a red-dominant AOV read from Suzanne, "
    f"got stats {center_stats}"
)
assert center_stats["max_r"] >= 0.45, (
    "Expected a strong red AOV signal behind the transmissive sphere, "
    f"got stats {center_stats}"
)
assert center_stats["max_r"] > center_stats["max_g"] * 2.5, (
    "Expected red to dominate green in the AOV-read region, "
    f"got stats {center_stats}"
)
assert center_stats["max_r"] > center_stats["max_b"] * 2.5, (
    "Expected red to dominate blue in the AOV-read region, "
    f"got stats {center_stats}"
)
assert corner_brightness <= 0.10, (
    "Expected a background corner to stay dark, "
    f"got {corner_brightness:.6f} from {corner_rgb}"
)
