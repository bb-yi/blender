from pathlib import Path
import argparse
import subprocess
import sys
import textwrap

import bpy


CASE_DIR = Path(__file__).resolve().parent
ROOT = CASE_DIR.parents[3]
OUTPUT_DIR = ROOT / "temp" / "release_test_outputs" / (
    "eevee_viewport_transparent_jittered_shadow_accumulates"
)
CHILD_SCRIPT = OUTPUT_DIR / "viewport_accumulation_child.py"

CHILD_SOURCE = r'''
import argparse
from pathlib import Path
import sys

import bpy
from mathutils import Vector


argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
parser = argparse.ArgumentParser()
parser.add_argument("--early", required=True)
parser.add_argument("--late", required=True)
args = parser.parse_args(argv)
early_path = Path(args.early)
late_path = Path(args.late)
early_probe = early_path.with_name(f"{early_path.stem}_probe.png")
late_probe = late_path.with_name(f"{late_path.stem}_probe.png")
early_path.parent.mkdir(parents=True, exist_ok=True)


def look_at(obj, target):
    direction = Vector(target) - obj.location
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


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


def load_pixels(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        return list(image.pixels[:]), int(image.size[0]), int(image.size[1])
    finally:
        bpy.data.images.remove(image)


def red_content_ratio(pixels, width, height):
    x0 = int(width * 0.34)
    x1 = int(width * 0.66)
    y0 = int(height * 0.26)
    y1 = int(height * 0.74)
    count = 0
    red = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            index = (y * width + x) * 4
            r, g, b = pixels[index], pixels[index + 1], pixels[index + 2]
            if r > 0.08 and r > g * 1.15 and r > b * 1.15:
                red += 1
            count += 1
    return red / max(count, 1)


def tag_redraw():
    for win in bpy.context.window_manager.windows:
        for screen_area in win.screen.areas:
            if screen_area.type == "VIEW_3D":
                screen_area.tag_redraw()


def capture_viewport(path):
    bpy.ops.wm.redraw_timer(type="DRAW_WIN_SWAP", iterations=2)
    with bpy.context.temp_override(window=window, screen=screen, area=area, region=region):
        result = bpy.ops.screen.screenshot("EXEC_DEFAULT", filepath=str(path))
    if result != {"FINISHED"}:
        raise RuntimeError(f"Viewport screenshot failed: {result}")


def capture_if_ready(probe_path, final_path):
    capture_viewport(probe_path)
    pixels, width, height = load_pixels(probe_path)
    ratio = red_content_ratio(pixels, width, height)
    if ratio < 0.04:
        return False, ratio
    probe_path.replace(final_path)
    return True, ratio


bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

scene = bpy.context.scene
scene.render.engine = "BLENDER_EEVEE"
scene.eevee.use_taa_reprojection = False
scene.eevee.use_shadow_jitter_viewport = True
scene.eevee.use_shadows = True
scene.eevee.shadow_ray_count = 1
scene.eevee.shadow_step_count = 6
scene.eevee.taa_samples = 128
scene.render.resolution_x = 256
scene.render.resolution_y = 256
scene.render.resolution_percentage = 100
scene.view_settings.view_transform = "Standard"
scene.view_settings.look = "None"
scene.view_settings.exposure = 0.0
scene.view_settings.gamma = 1.0
scene.world.color = (0.45, 0.45, 0.45)

mat_ground = bpy.data.materials.new("AccumulationGround")
mat_ground.use_nodes = True
ground_bsdf = mat_ground.node_tree.nodes.get("Principled BSDF")
ground_bsdf.inputs["Base Color"].default_value = (0.72, 0.72, 0.72, 1.0)
ground_bsdf.inputs["Roughness"].default_value = 0.8

mat_transparent = bpy.data.materials.new("AccumulationHashedTransparent")
mat_transparent.use_nodes = True
mat_transparent.blend_method = "HASHED"
mat_transparent.use_transparent_shadow = True
transparent_bsdf = mat_transparent.node_tree.nodes.get("Principled BSDF")
transparent_bsdf.inputs["Base Color"].default_value = (1.0, 0.0, 0.0, 1.0)
transparent_bsdf.inputs["Alpha"].default_value = 0.38
transparent_bsdf.inputs["Roughness"].default_value = 0.65

bpy.ops.mesh.primitive_plane_add(size=5.0, location=(0.0, 0.0, 0.0))
plane = bpy.context.object
plane.name = "AccumulationShadowReceiver"
plane.data.materials.append(mat_ground)

bpy.ops.mesh.primitive_uv_sphere_add(
    segments=64, ring_count=32, radius=0.95, location=(0.0, 0.0, 0.95)
)
sphere = bpy.context.object
sphere.name = "AccumulationTransparentCaster"
sphere.data.materials.append(mat_transparent)

bpy.ops.object.light_add(type="AREA", location=(-2.5, -3.0, 4.0))
light = bpy.context.object
light.name = "AccumulationAreaLight"
light.data.energy = 550.0
light.data.size = 3.0

bpy.ops.object.camera_add(location=(0.0, -5.5, 1.35))
camera = bpy.context.object
look_at(camera, (0.0, 0.0, 0.85))
camera.data.type = "ORTHO"
camera.data.ortho_scale = 3.0
scene.camera = camera

window, screen, area, region = find_view3d_context()
space = area.spaces.active
space.shading.type = "RENDERED"
space.overlay.show_overlays = False
space.region_3d.view_perspective = "CAMERA"

state = {
    "frames": 0,
    "after_early": 0,
    "captured_early": False,
    "ready_waits": 0,
}


def tick():
    tag_redraw()
    if state["frames"] == 0:
        window.event_simulate(type="ESC", value="PRESS")
        window.event_simulate(type="ESC", value="RELEASE")

    state["frames"] += 1
    if not state["captured_early"]:
        if state["frames"] < 6:
            return 0.03
        ready, ratio = capture_if_ready(early_probe, early_path)
        if not ready:
            state["ready_waits"] += 1
            if state["ready_waits"] > 120:
                bpy.ops.wm.quit_blender()
                raise RuntimeError(f"Viewport never showed red transparent content: ratio={ratio:.6f}")
            return 0.03
        state["captured_early"] = True
        print(
            f"VIEWPORT_TRANSPARENT_JITTER_EARLY_OK frame={state['frames']} "
            f"red_ratio={ratio:.6f} path={early_path}"
        )
        return 0.03

    state["after_early"] += 1
    if state["after_early"] < 96:
        return 0.03

    ready, ratio = capture_if_ready(late_probe, late_path)
    if not ready:
        bpy.ops.wm.quit_blender()
        raise RuntimeError(f"Late viewport capture lost red transparent content: ratio={ratio:.6f}")

    print(
        f"VIEWPORT_TRANSPARENT_JITTER_LATE_OK frame={state['frames']} "
        f"red_ratio={ratio:.6f} path={late_path}"
    )
    bpy.ops.wm.quit_blender()
    return None


bpy.app.timers.register(tick, first_interval=0.2)
'''


def run_child():
    early_path = OUTPUT_DIR / "early.png"
    late_path = OUTPUT_DIR / "late.png"
    command = [
        bpy.app.binary_path,
        "--factory-startup",
        "--enable-event-simulate",
        "--python",
        str(CHILD_SCRIPT),
        "--",
        "--early",
        str(early_path),
        "--late",
        str(late_path),
    ]
    result = subprocess.run(
        command,
        cwd=str(ROOT),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=70,
        check=False,
    )
    combined = result.stdout + result.stderr
    (OUTPUT_DIR / "viewport_accumulation_child.log").write_text(combined, encoding="utf-8")
    assert result.returncode == 0, f"viewport accumulation child failed:\n{combined[-4000:]}"
    assert "VIEWPORT_TRANSPARENT_JITTER_EARLY_OK" in combined, (
        f"early viewport capture did not succeed:\n{combined[-4000:]}"
    )
    assert "VIEWPORT_TRANSPARENT_JITTER_LATE_OK" in combined, (
        f"late viewport capture did not succeed:\n{combined[-4000:]}"
    )
    assert early_path.exists(), f"early viewport capture missing: {early_path}"
    assert late_path.exists(), f"late viewport capture missing: {late_path}"
    return early_path, late_path


def load_pixels(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        return list(image.pixels[:]), int(image.size[0]), int(image.size[1])
    finally:
        bpy.data.images.remove(image)


def luma(pixels, index):
    return 0.2126 * pixels[index] + 0.7152 * pixels[index + 1] + 0.0722 * pixels[index + 2]


def red_content_ratio(pixels, width, height):
    x0 = int(width * 0.34)
    x1 = int(width * 0.66)
    y0 = int(height * 0.26)
    y1 = int(height * 0.74)
    count = 0
    red = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            index = (y * width + x) * 4
            r, g, b = pixels[index], pixels[index + 1], pixels[index + 2]
            if r > 0.08 and r > g * 1.15 and r > b * 1.15:
                red += 1
            count += 1
    return red / max(count, 1)


def high_frequency_noise(pixels, width, height):
    x0 = int(width * 0.34)
    x1 = int(width * 0.66)
    y0 = int(height * 0.26)
    y1 = int(height * 0.74)
    diff_sum = 0.0
    count = 0
    for y in range(y0, y1 - 1):
        for x in range(x0, x1 - 1):
            index = (y * width + x) * 4
            right = (y * width + x + 1) * 4
            up = ((y + 1) * width + x) * 4
            center_luma = luma(pixels, index)
            diff_sum += abs(center_luma - luma(pixels, right))
            diff_sum += abs(center_luma - luma(pixels, up))
            count += 2
    return diff_sum / max(count, 1)


OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
CHILD_SCRIPT.write_text(textwrap.dedent(CHILD_SOURCE), encoding="utf-8")

early_path, late_path = run_child()
early_pixels, width, height = load_pixels(early_path)
late_pixels, late_width, late_height = load_pixels(late_path)
assert (width, height) == (late_width, late_height), (
    f"viewport capture sizes differ: early={width}x{height} late={late_width}x{late_height}"
)

early_red = red_content_ratio(early_pixels, width, height)
late_red = red_content_ratio(late_pixels, width, height)
early_noise = high_frequency_noise(early_pixels, width, height)
late_noise = high_frequency_noise(late_pixels, width, height)
noise_ratio = late_noise / max(early_noise, 1.0e-8)

print(f"TRANSPARENT_JITTER_VIEWPORT_CAPTURE_SIZE={width}x{height}")
print(f"TRANSPARENT_JITTER_VIEWPORT_EARLY_RED_RATIO={early_red:.6f}")
print(f"TRANSPARENT_JITTER_VIEWPORT_LATE_RED_RATIO={late_red:.6f}")
print(f"TRANSPARENT_JITTER_VIEWPORT_EARLY_NOISE={early_noise:.6f}")
print(f"TRANSPARENT_JITTER_VIEWPORT_LATE_NOISE={late_noise:.6f}")
print(f"TRANSPARENT_JITTER_VIEWPORT_NOISE_RATIO={noise_ratio:.6f}")
print(f"TRANSPARENT_JITTER_VIEWPORT_OUTPUT_DIR={OUTPUT_DIR}")

assert early_red > 0.04, f"early capture does not contain enough red object pixels: {early_red:.6f}"
assert late_red > 0.04, f"late capture does not contain enough red object pixels: {late_red:.6f}"
assert early_noise > 0.004, (
    f"early capture is not noisy enough to validate accumulation: {early_noise:.6f}"
)
assert noise_ratio < 0.78, (
    "transparent jittered shadow viewport did not accumulate enough; "
    f"early_noise={early_noise:.6f}, late_noise={late_noise:.6f}, ratio={noise_ratio:.6f}"
)

print("EEVEE_VIEWPORT_TRANSPARENT_JITTERED_SHADOW_ACCUMULATES_OK")
