from pathlib import Path
import runpy

import bpy


CASE_DIR = Path(__file__).resolve().parents[1]
ROOT = CASE_DIR.parents[3]
TEST_SCRIPT = ROOT / "blender_5_1_port" / "tests" / "python" / "npr" / "test_goo_scene_color_position_filter.py"
ASSETS_DIR = CASE_DIR / "assets"
OUT_DIR = CASE_DIR / "out" / "asset-previews"


module = runpy.run_path(str(TEST_SCRIPT))
module["RESOLUTION"] = 256


def ensure_dir(path):
    path.mkdir(parents=True, exist_ok=True)


def render_preview(slug):
    ensure_dir(OUT_DIR)
    scene = bpy.context.scene
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "8"
    scene.render.filepath = str(OUT_DIR / f"{slug}.png")
    bpy.ops.render.render(write_still=True)


def save_project(slug):
    asset_dir = ASSETS_DIR / slug
    ensure_dir(asset_dir)
    bpy.ops.wm.save_as_mainfile(filepath=str(asset_dir / "scene.blend"), copy=False)


def main():
    module["clear_scene"]()
    module["configure_scene"]()
    module["make_camera"]()
    module["make_plane"](module["make_surface_material"]())
    module["attach_filter_material"](module["make_filter_material"]())
    bpy.context.view_layer.update()

    slug = "010-scene-color-position-filter"
    render_preview(slug)
    save_project(slug)


if __name__ == "__main__":
    main()
