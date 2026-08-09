from pathlib import Path
import subprocess

import bpy


CASE_DIR = Path(__file__).resolve().parent
ROOT = CASE_DIR.parents[3]
BLEND_PATH = CASE_DIR / "assets" / "aov_self_raytracing.blend"
OUT_DIR = CASE_DIR / "out"
CHILD_SCRIPT = OUT_DIR / "viewport_self_aov_raytracing_child.py"
CHILD_LOG = OUT_DIR / "viewport_self_aov_raytracing_child.log"


CHILD_SOURCE = r'''
import argparse
from pathlib import Path
import sys

import bpy


argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
parser = argparse.ArgumentParser()
parser.add_argument("--blend", required=True)
parser.add_argument("--viewport", required=True)
parser.add_argument("--render", required=True)
args = parser.parse_args(argv)

blend_path = Path(args.blend)
viewport_path = Path(args.viewport)
render_path = Path(args.render)
viewport_path.parent.mkdir(parents=True, exist_ok=True)


def find_view3d_context():
    for window in bpy.context.window_manager.windows:
        screen = window.screen
        for area in screen.areas:
            if area.type != "VIEW_3D":
                continue
            for region in area.regions:
                if region.type == "WINDOW":
                    return window, screen, area, region
    raise RuntimeError("No VIEW_3D window context found")


def configure_scene():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 512
    scene.render.resolution_y = 512
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    if hasattr(scene.eevee, "use_raytracing"):
        scene.eevee.use_raytracing = True
    if hasattr(scene.eevee, "taa_samples"):
        scene.eevee.taa_samples = 8
    if hasattr(scene.eevee, "taa_render_samples"):
        scene.eevee.taa_render_samples = 8
    if hasattr(scene.eevee, "use_taa_reprojection"):
        scene.eevee.use_taa_reprojection = False
    return scene


def configure_viewport():
    window, screen, area, region = find_view3d_context()
    space = area.spaces.active
    space.shading.type = "RENDERED"
    if hasattr(space.shading, "use_scene_world_render"):
        space.shading.use_scene_world_render = True
    if hasattr(space.shading, "use_scene_lights_render"):
        space.shading.use_scene_lights_render = True
    if hasattr(space, "overlay"):
        space.overlay.show_overlays = False
    if space.region_3d is not None:
        space.region_3d.view_perspective = "CAMERA"
    return window, screen, area, region


def load_pixels(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        return list(image.pixels[:]), int(image.size[0]), int(image.size[1])
    finally:
        bpy.data.images.remove(image)


def region_stats_top_origin(path, left, right, top, bottom):
    pixels, width, height = load_pixels(path)
    x0 = int(width * left)
    x1 = int(width * right)
    y0 = int(height * (1.0 - bottom))
    y1 = int(height * (1.0 - top))
    count = 0
    total = 0.0
    max_v = 0.0
    non_black = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            index = (y * width + x) * 4
            v = (pixels[index] + pixels[index + 1] + pixels[index + 2]) / 3.0
            total += v
            max_v = max(max_v, v)
            if v > 0.08:
                non_black += 1
            count += 1
    return {
        "avg": total / count,
        "max": max_v,
        "non_black_ratio": non_black / count,
        "width": width,
        "height": height,
    }


def material_has_self_aov_setup(aov_name):
    for material in bpy.data.materials:
        if material.node_tree is None:
            continue
        has_surface_output = any(
            node.bl_idname == "ShaderNodeOutputAOV" and getattr(node, "aov_name", "") == aov_name
            for node in material.node_tree.nodes
        )
        has_npr_input = False
        for node in material.node_tree.nodes:
            npr_tree = getattr(node, "nprtree", None)
            if npr_tree is None:
                continue
            has_npr_input |= any(
                npr_node.bl_idname == "ShaderNodeInputAOV"
                and getattr(npr_node, "aov_name", "") == aov_name
                for npr_node in npr_tree.nodes
            )
        if has_surface_output and has_npr_input:
            return True
    return False


bpy.ops.wm.open_mainfile(filepath=str(blend_path))
scene = configure_scene()
window, screen, area, region = configure_viewport()

assert any(aov.name == "AOV" and aov.type == "COLOR" for aov in bpy.context.view_layer.aovs), (
    "Expected repro scene to define color AOV named AOV"
)
assert material_has_self_aov_setup("AOV"), (
    "Expected a material to write AOV and read the same AOV from its NPR Tree"
)

scene.render.filepath = str(render_path)
bpy.ops.render.render(write_still=True)
render_stats = region_stats_top_origin(render_path, 0.36, 0.66, 0.20, 0.62)
assert render_stats["avg"] > 0.55 and render_stats["non_black_ratio"] > 0.85, (
    f"Expected final render self-AOV region to be non-black, got {render_stats}"
)

state = {"ticks": 0}


def tick():
    state["ticks"] += 1
    for win in bpy.context.window_manager.windows:
        for screen_area in win.screen.areas:
            if screen_area.type == "VIEW_3D":
                screen_area.tag_redraw()
    if state["ticks"] < 24:
        return 0.05

    bpy.ops.wm.redraw_timer(type="DRAW_WIN_SWAP", iterations=4)
    with bpy.context.temp_override(window=window, screen=screen, area=area, region=region):
        result = bpy.ops.screen.screenshot("EXEC_DEFAULT", filepath=str(viewport_path))
    if result != {"FINISHED"}:
        raise RuntimeError(f"Viewport screenshot failed: {result}")

    viewport_stats = region_stats_top_origin(viewport_path, 0.47, 0.58, 0.15, 0.30)
    print(f"SELF_AOV_RAYTRACING_RENDER_STATS={render_stats}")
    print(f"SELF_AOV_RAYTRACING_VIEWPORT_STATS={viewport_stats}")
    assert viewport_stats["avg"] > 0.45, (
        f"Expected viewport Suzanne self-AOV region to be non-black, got {viewport_stats}"
    )
    assert viewport_stats["non_black_ratio"] > 0.80, (
        f"Expected enough visible self-AOV pixels in viewport, got {viewport_stats}"
    )
    assert viewport_stats["max"] > 0.70, (
        f"Expected a strong self-AOV checker signal in viewport, got {viewport_stats}"
    )
    print("VIEWPORT_SELF_AOV_RAYTRACING_OK")
    bpy.ops.wm.quit_blender()
    return None


bpy.app.timers.register(tick, first_interval=0.2)
'''


def run_child():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    CHILD_SCRIPT.write_text(CHILD_SOURCE, encoding="utf-8")

    viewport_path = OUT_DIR / "viewport.png"
    render_path = OUT_DIR / "render.png"
    for path in (viewport_path, render_path, CHILD_LOG):
        if path.exists():
            path.unlink()

    command = [
        bpy.app.binary_path,
        "--factory-startup",
        "--enable-event-simulate",
        "--python-exit-code",
        "1",
        "--python",
        str(CHILD_SCRIPT),
        "--",
        "--blend",
        str(BLEND_PATH),
        "--viewport",
        str(viewport_path),
        "--render",
        str(render_path),
    ]
    result = subprocess.run(
        command,
        cwd=str(ROOT),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=90,
        check=False,
    )
    combined = result.stdout + result.stderr
    CHILD_LOG.write_text(combined, encoding="utf-8")
    assert result.returncode == 0, f"Viewport child failed:\n{combined[-5000:]}"
    assert "VIEWPORT_SELF_AOV_RAYTRACING_OK" in combined, (
        f"Viewport child did not report success:\n{combined[-5000:]}"
    )
    assert viewport_path.exists(), f"Viewport screenshot missing: {viewport_path}"
    assert render_path.exists(), f"Render output missing: {render_path}"
    print(combined)


assert BLEND_PATH.exists(), f"Missing blend asset: {BLEND_PATH}"
run_child()
