"""Release test: Malt-style per-class outline width control.

Verifies the subversion 501.52 Outline Control changes:
  1. Node has 13 sockets (5 new Malt params), no legacy Width Variation.
  2. No full-screen outline regression (outline_width_unpack mask guard).
  3. Per-class edge_width independently controls outline thickness.
  4. Threshold range > 0 produces taper (different from hard switch).
"""

import hashlib
import json
from pathlib import Path

import bpy

CASE_DIR = Path(__file__).resolve().parent
ASSET_PATH = CASE_DIR / "assets" / "描边断口测试.blend"
OUT_DIR = CASE_DIR / "out"

# Socket names expected on the migrated node (subversion 501.52).
EXPECTED_SOCKETS = [
    "Line Color",
    "Line Alpha",
    "Line Width",
    "Depth Threshold",
    "Depth Threshold Range",
    "Depth Edge Width",
    "Normal Threshold",
    "Normal Threshold Range",
    "Normal Edge Width",
    "Outline ID",
    "ID Edge",
    "ID Edge Width",
    "Freestyle Edge",
]
LEGACY_SOCKET = "Width Variation"

# A render where the outline floods the entire screen (the mask bug) has ~100% red pixels.
# Normal outlines occupy a small fraction of the viewport.
FULL_SCREEN_RED_THRESHOLD = 0.60


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def load_scene():
    require(ASSET_PATH.exists(), f"missing asset: {ASSET_PATH}")
    bpy.ops.wm.open_mainfile(filepath=str(ASSET_PATH))


def configure_render():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 960
    scene.render.resolution_y = 540
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.use_compositing = False
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0


def find_outline_node():
    """Find the first ShaderNodeOutlineControl in any material node tree."""
    for mat in bpy.data.materials:
        if mat.node_tree is None:
            continue

        def search(ntree):
            for n in ntree.nodes:
                if n.bl_idname == "ShaderNodeOutlineControl":
                    return n
                if n.type == "GROUP" and n.node_tree:
                    r = search(n.node_tree)
                    if r:
                        return r
            return None

        node = search(mat.node_tree)
        if node:
            return node
    return None


def set_inputs(node, params):
    for name, value in params.items():
        if name not in node.inputs:
            raise AssertionError(f"socket '{name}' not found on Outline Control node")
        node.inputs[name].default_value = value


def render_and_analyze(label):
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

    total = width * height
    red_pixels = 0
    for i in range(0, len(pixels), 4):
        r, g, b = pixels[i], pixels[i + 1], pixels[i + 2]
        if r > 0.25 and r > g + 0.05 and r > b + 0.05:
            red_pixels += 1

    red_ratio = red_pixels / total

    with open(path, "rb") as f:
        md5 = hashlib.md5(f.read()).hexdigest()

    return {
        "label": label,
        "path": str(path),
        "width": width,
        "height": height,
        "red_pixels": red_pixels,
        "total": total,
        "red_ratio": red_ratio,
        "md5": md5,
    }


def main():
    load_scene()
    configure_render()

    node = find_outline_node()
    require(node is not None, "no ShaderNodeOutlineControl found in scene")

    # --- Check 1: socket migration ---
    actual_sockets = [s.name for s in node.inputs]
    require(
        len(actual_sockets) == 13,
        f"expected 13 sockets, found {len(actual_sockets)}: {actual_sockets}",
    )
    for name in EXPECTED_SOCKETS:
        require(name in actual_sockets, f"missing expected socket: {name}")
    require(
        LEGACY_SOCKET not in actual_sockets,
        f"legacy '{LEGACY_SOCKET}' socket should have been removed by versioning",
    )

    # --- Check 2: full-screen regression guard ---
    # Baseline: all defaults (edge_width=1.0, range=0.0 = hard switch).
    # With ID Edge on, most boundaries are ID edges. This should NOT flood the screen.
    set_inputs(node, {
        "ID Edge": True,
        "Depth Threshold": 0.1, "Depth Threshold Range": 0.0, "Depth Edge Width": 1.0,
        "Normal Threshold": 0.5, "Normal Threshold Range": 0.0, "Normal Edge Width": 1.0,
        "ID Edge Width": 1.0,
    })
    baseline = render_and_analyze("01_baseline")
    require(
        baseline["red_ratio"] < FULL_SCREEN_RED_THRESHOLD,
        f"FULL-SCREEN REGRESSION: baseline red_ratio={baseline['red_ratio']:.3f} >= "
        f"{FULL_SCREEN_RED_THRESHOLD}. The outline_width_unpack mask is likely missing.",
    )

    # --- Check 3: per-class edge_width independence ---
    # Disable ID Edge to isolate depth + normal edges.
    set_inputs(node, {
        "ID Edge": False,
        "Depth Threshold": 0.1, "Depth Threshold Range": 0.0, "Depth Edge Width": 1.0,
        "Normal Threshold": 0.5, "Normal Threshold Range": 0.0, "Normal Edge Width": 1.0,
        "ID Edge Width": 1.0,
    })
    no_id_baseline = render_and_analyze("02_no_id_baseline")

    # Depth Edge Width = 0 should reduce red pixels.
    set_inputs(node, {
        "ID Edge": False,
        "Depth Threshold": 0.1, "Depth Threshold Range": 0.0, "Depth Edge Width": 0.0,
        "Normal Threshold": 0.5, "Normal Threshold Range": 0.0, "Normal Edge Width": 1.0,
        "ID Edge Width": 1.0,
    })
    depth_width_zero = render_and_analyze("03_depth_width_zero")
    require(
        depth_width_zero["red_pixels"] < no_id_baseline["red_pixels"],
        f"Depth Edge Width=0 did not reduce red pixels: "
        f"baseline={no_id_baseline['red_pixels']}, depth_zero={depth_width_zero['red_pixels']}",
    )

    # Normal Edge Width = 0 should reduce red pixels.
    set_inputs(node, {
        "ID Edge": False,
        "Depth Threshold": 0.1, "Depth Threshold Range": 0.0, "Depth Edge Width": 1.0,
        "Normal Threshold": 0.5, "Normal Threshold Range": 0.0, "Normal Edge Width": 0.0,
        "ID Edge Width": 1.0,
    })
    normal_width_zero = render_and_analyze("04_normal_width_zero")
    require(
        normal_width_zero["red_pixels"] < no_id_baseline["red_pixels"],
        f"Normal Edge Width=0 did not reduce red pixels: "
        f"baseline={no_id_baseline['red_pixels']}, normal_zero={normal_width_zero['red_pixels']}",
    )

    # --- Check 4: taper vs hard switch ---
    # Depth Threshold Range > 0 (taper) should produce a different image than range=0.
    set_inputs(node, {
        "ID Edge": False,
        "Depth Threshold": 0.1, "Depth Threshold Range": 0.5, "Depth Edge Width": 1.0,
        "Normal Threshold": 0.5, "Normal Threshold Range": 0.0, "Normal Edge Width": 1.0,
        "ID Edge Width": 1.0,
    })
    depth_taper = render_and_analyze("05_depth_taper")
    require(
        depth_taper["md5"] != no_id_baseline["md5"],
        "Depth Threshold Range=0.5 (taper) produced identical image to range=0.0 (hard switch) — "
        "taper path is not active",
    )

    # --- Summary ---
    summary = {
        "status": "PASS",
        "asset": str(ASSET_PATH),
        "socket_count": len(actual_sockets),
        "sockets": actual_sockets,
        "legacy_socket_removed": LEGACY_SOCKET not in actual_sockets,
        "results": {
            "baseline": baseline,
            "no_id_baseline": no_id_baseline,
            "depth_width_zero": depth_width_zero,
            "normal_width_zero": normal_width_zero,
            "depth_taper": depth_taper,
        },
    }
    summary_path = OUT_DIR / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8")

    print(
        "OUTLINE_MALT_WIDTH_CONTROL: "
        f"sockets={len(actual_sockets)} "
        f"baseline_red={baseline['red_ratio']:.3f} "
        f"depth_zero_red={depth_width_zero['red_pixels']} "
        f"normal_zero_red={normal_width_zero['red_pixels']} "
        f"taper_diff={depth_taper['md5'] != no_id_baseline['md5']}",
        flush=True,
    )
    print("OUTLINE_MALT_WIDTH_CONTROL_RELEASE_OK", flush=True)


if __name__ == "__main__":
    main()
