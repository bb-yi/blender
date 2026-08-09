from pathlib import Path

import bpy


CASE_DIR = Path(__file__).resolve().parent
ROOT = CASE_DIR.parents[3]
BLEND_PATH = CASE_DIR / "assets" / "Filter Object Mask测试.blend"
OUTPUT_DIR = ROOT / "temp" / "release_test_outputs" / "filter_object_mask_real_blend"


def configure_render():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
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


def brightness(rgb):
    return max(rgb)


def bright_mask_stats(pixels, width, height, threshold):
    count = 0
    min_x = width
    min_y = height
    max_x = -1
    max_y = -1

    for y in range(height):
        row = y * width
        for x in range(width):
            index = (row + x) * 4
            if max(pixels[index], pixels[index + 1], pixels[index + 2]) < threshold:
                continue
            count += 1
            min_x = min(min_x, x)
            min_y = min(min_y, y)
            max_x = max(max_x, x)
            max_y = max(max_y, y)

    return {
        "count": count,
        "bbox_width": 0 if max_x < min_x else (max_x - min_x + 1),
        "bbox_height": 0 if max_y < min_y else (max_y - min_y + 1),
    }


assert BLEND_PATH.exists(), f"Missing blend file: {BLEND_PATH}"

bpy.ops.wm.open_mainfile(filepath=str(BLEND_PATH))
scene = bpy.context.scene
view_layer = bpy.context.view_layer

configure_render()

assert view_layer.use_pass_cryptomatte_object, (
    "The repro blend must keep ViewLayer.use_pass_cryptomatte_object enabled."
)
filter_graph = scene.eevee.filter_graph
assert filter_graph is not None, "Expected the legacy filter material stack to convert to a Filter Graph."
assert any(node.bl_idname == "EeveeFilterGraphNodeFilterMaterial" for node in filter_graph.nodes), (
    "Expected the converted Filter Graph to contain at least one Filter Pass node."
)
filter_materials = [
    material
    for material in bpy.data.materials
    if getattr(material, "eevee_domain", None) == "FILTER" and material.node_tree is not None
]
assert filter_materials, "Expected the repro blend to contain a converted Filter material."
for material in filter_materials:
    scene_color_nodes = [
        node for node in material.node_tree.nodes if node.bl_idname == "ShaderNodeSceneColor"
    ]
    assert not scene_color_nodes, (
        "Legacy Scene Color nodes must be lifted out of Filter materials during conversion; "
        f"{material.name} still contains {[node.name for node in scene_color_nodes]}."
    )
    assert any(
        node.bl_idname == "ShaderNodeFilterGraphInput" for node in material.node_tree.nodes
    ), f"Expected {material.name} to receive scene images through a Filter Graph Pass Input node."
    assert any(
        node.bl_idname == "ShaderNodeNPR_ImageSample" for node in material.node_tree.nodes
    ), f"Expected {material.name} to sample the converted Scene Color alpha through Image Sample."

OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
output_path = OUTPUT_DIR / "filter_object_mask_real_blend.png"
scene.render.filepath = str(output_path)
bpy.ops.render.render(write_still=True)

assert output_path.exists(), f"Render did not write {output_path}"

pixels, width, height = load_pixels(output_path)
center_rgb = sample_rgb(pixels, width, height, 0.5, 0.5)
corner_rgb = sample_rgb(pixels, width, height, 0.05, 0.05)
mask_stats = bright_mask_stats(pixels, width, height, 0.75)

center_brightness = brightness(center_rgb)
corner_brightness = brightness(corner_rgb)

print(f"CENTER_RGB={center_rgb[0]:.6f},{center_rgb[1]:.6f},{center_rgb[2]:.6f}")
print(f"CORNER_RGB={corner_rgb[0]:.6f},{corner_rgb[1]:.6f},{corner_rgb[2]:.6f}")
print(f"CENTER_BRIGHTNESS={center_brightness:.6f}")
print(f"CORNER_BRIGHTNESS={corner_brightness:.6f}")
print(
    "MASK_STATS="
    f"count:{mask_stats['count']} bbox_width:{mask_stats['bbox_width']} "
    f"bbox_height:{mask_stats['bbox_height']}"
)

assert center_brightness >= 0.75, (
    "Expected the selected Suzanne mask to make the center pixel bright in the repro blend, "
    f"got {center_brightness:.6f} from {center_rgb}"
)
assert corner_brightness <= 0.10, (
    "Expected a background corner to stay dark in the repro blend, "
    f"got {corner_brightness:.6f} from {corner_rgb}"
)
assert mask_stats["count"] >= 2048, (
    "Expected the repro blend to produce a substantial bright object mask area, "
    f"got mask stats {mask_stats}"
)
assert mask_stats["bbox_width"] >= width // 4, (
    "Expected the bright mask to span a meaningful width in the repro blend, "
    f"got mask stats {mask_stats} for render size {width}x{height}"
)
assert mask_stats["bbox_height"] >= height // 4, (
    "Expected the bright mask to span a meaningful height in the repro blend, "
    f"got mask stats {mask_stats} for render size {width}x{height}"
)
