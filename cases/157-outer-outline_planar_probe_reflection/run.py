from pathlib import Path

import bpy


CASE_DIR = Path(__file__).resolve().parent
ASSET_PATH = CASE_DIR / "assets" / "描边平面探头测试.blend"
OUT_DIR = CASE_DIR / "out"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def set_if_available(owner, name, value):
    if hasattr(owner, name):
        setattr(owner, name, value)


def load_scene():
    require(ASSET_PATH.exists(), f"missing asset: {ASSET_PATH}")
    bpy.ops.wm.open_mainfile(filepath=str(ASSET_PATH))


def configure_render():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 768
    scene.render.resolution_y = 432
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.use_compositing = False
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
    ):
        set_if_available(eevee, attr, value)


def find_outline_materials():
    direct = 0
    npr = 0
    for obj in bpy.data.objects:
        if obj.type != "MESH" or obj.data is None:
            continue
        for material in obj.data.materials:
            if material is None or material.node_tree is None:
                continue
            if any(node.bl_idname == "ShaderNodeOutlineControl" for node in material.node_tree.nodes):
                direct += 1
            for node in material.node_tree.nodes:
                if node.bl_idname != "ShaderNodeOutputMaterial":
                    continue
                nprtree = getattr(node, "nprtree", None)
                if nprtree is not None and any(
                    npr_node.bl_idname == "ShaderNodeOutlineControl" for npr_node in nprtree.nodes
                ):
                    npr += 1
    return direct, npr


def render_image(label):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    path = OUT_DIR / f"{label}.png"
    bpy.context.scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)
    require(path.exists(), f"render did not write {path}")
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        pixels = list(image.pixels[:])
        width, height = map(int, image.size)
    finally:
        bpy.data.images.remove(image)
    return path, pixels, width, height


def region_stats(pixels, width, height, x0, y0, x1, y1):
    require(0 <= x0 < x1 <= width, f"invalid x range: {x0}..{x1} for width {width}")
    require(0 <= y0 < y1 <= height, f"invalid y range: {y0}..{y1} for height {height}")
    # Blender image pixel storage starts from the bottom row, while the test windows are easier
    # to define from the top of the rendered image.
    y0, y1 = height - y1, height - y0
    sums = [0.0, 0.0, 0.0]
    red_pixels = 0
    total = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            i = (y * width + x) * 4
            r, g, b = pixels[i : i + 3]
            sums[0] += r
            sums[1] += g
            sums[2] += b
            total += 1
            if r > 0.25 and r > g + 0.05 and r > b + 0.05:
                red_pixels += 1
    return {
        "rgb": [channel / total for channel in sums],
        "red_pixels": red_pixels,
        "total": total,
    }


def main():
    load_scene()
    configure_render()
    direct_outline, npr_outline = find_outline_materials()
    require(direct_outline == 1, f"expected one direct outline material, found {direct_outline}")
    require(npr_outline == 0, f"expected no NPR outline material, found {npr_outline}")

    path, pixels, width, height = render_image("outline_planar_probe_reflection")
    top = region_stats(pixels, width, height, 200, 20, 560, 190)
    bottom = region_stats(pixels, width, height, 200, 190, 560, 410)
    top_monkey = region_stats(pixels, width, height, 250, 25, 530, 165)
    bottom_reflection = region_stats(pixels, width, height, 250, 210, 530, 390)

    print(
        "outline_planar_probe_reflection: "
        f"top_red={top['red_pixels']} bottom_red={bottom['red_pixels']} "
        f"top_monkey={top_monkey['red_pixels']} bottom_reflection={bottom_reflection['red_pixels']} "
        f"path={path}",
        flush=True,
    )

    require(top_monkey["red_pixels"] > 12000, f"top monkey outline is too weak: {top_monkey['red_pixels']}")
    require(bottom_reflection["red_pixels"] < 100, f"bottom reflection still has too much red outline: {bottom_reflection['red_pixels']}")
    require(top["red_pixels"] > 14000, f"top region is unexpectedly weak: {top['red_pixels']}")
    require(bottom["red_pixels"] < 1000, f"bottom region has too much red overall: {bottom['red_pixels']}")

    summary = {
        "status": "PASS",
        "asset": str(ASSET_PATH),
        "path": str(path),
        "top": top,
        "bottom": bottom,
        "top_monkey": top_monkey,
        "bottom_reflection": bottom_reflection,
    }
    summary_path = OUT_DIR / "summary.json"
    summary_path.write_text(__import__("json").dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8")
    print("OUTLINE_PLANAR_PROBE_REFLECTION_RELEASE_OK", flush=True)


if __name__ == "__main__":
    main()
