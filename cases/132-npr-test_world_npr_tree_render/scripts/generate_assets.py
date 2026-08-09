from pathlib import Path
import runpy

import bpy


CASE_DIR = Path(__file__).resolve().parents[1]
ROOT = CASE_DIR.parents[3]
TEST_SCRIPT = ROOT / "blender_5_1_port" / "tests" / "python" / "npr" / "test_world_npr_tree_render.py"
ASSETS_DIR = CASE_DIR / "assets"
OUT_DIR = CASE_DIR / "out" / "asset-previews"


module = runpy.run_path(str(TEST_SCRIPT))


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


def build_and_save(slug, build_fn):
    module["clear_scene"]()
    world_output = module["configure_scene"]()
    build_fn(world_output)
    bpy.context.view_layer.update()
    render_preview(slug)
    save_project(slug)


def build_solid(world_output):
    module["attach_solid_world_npr"](world_output)


def build_combined(world_output):
    module["attach_combined_color_world_npr"](world_output)


def build_image_sample_normal(world_output):
    module["attach_image_sample_world_npr"](world_output, "Normal", "WorldNormalImageSampleNPRTree")


def build_image_sample_position(world_output):
    module["attach_image_sample_world_npr"](
        world_output,
        "Position",
        "WorldPositionImageSampleNPRTree",
        -1.0,
    )


def build_image_sample_combined_offset(_world_output):
    world_output = module["use_directional_world_background"]()
    bpy.context.scene.camera.data.type = "PERSP"
    bpy.context.scene.camera.data.lens = 18.0
    module["attach_combined_color_image_sample_world_npr"](world_output, 24.0)


def main():
    build_and_save("010-world-solid-npr", build_solid)
    build_and_save("020-world-combined-color", build_combined)
    build_and_save("030-world-image-sample-normal", build_image_sample_normal)
    build_and_save("040-world-image-sample-position", build_image_sample_position)
    build_and_save("050-world-image-sample-combined-offset", build_image_sample_combined_offset)


if __name__ == "__main__":
    main()
