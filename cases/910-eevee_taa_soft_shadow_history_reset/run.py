from pathlib import Path
import subprocess
import textwrap

import bpy


CASE_DIR = Path(__file__).resolve().parent
ROOT = CASE_DIR.parents[3]
OUTPUT_DIR = ROOT / "temp" / "release_test_outputs" / "eevee_taa_soft_shadow_history_reset"
CHILD_SCRIPT = OUTPUT_DIR / "viewport_capture_child.py"

CHILD_SOURCE = r'''
import argparse
from pathlib import Path
import sys

import bpy
from mathutils import Vector


argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
parser = argparse.ArgumentParser()
parser.add_argument("--mode", choices=("candidate", "reference"), required=True)
parser.add_argument("--out", required=True)
args = parser.parse_args(argv)
out_path = Path(args.out)
out_path.parent.mkdir(parents=True, exist_ok=True)
probe_path = out_path.with_name(f"{out_path.stem}_probe.png")


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


def non_ui_content_ratio(pixels, width, height):
    x0 = int(width * 0.18)
    x1 = int(width * 0.82)
    y0 = int(height * 0.18)
    y1 = int(height * 0.82)
    count = 0
    lit = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            index = (y * width + x) * 4
            if max(pixels[index], pixels[index + 1], pixels[index + 2]) > 0.08:
                lit += 1
            count += 1
    return lit / max(count, 1)


def rendered_scene_metrics(pixels, width, height):
    x0 = int(width * 0.22)
    x1 = int(width * 0.82)
    y0 = int(height * 0.22)
    y1 = int(height * 0.82)
    count = 0
    bright = 0
    luma_min = 1.0
    luma_max = 0.0
    for y in range(y0, y1):
        for x in range(x0, x1):
            index = (y * width + x) * 4
            luma = 0.2126 * pixels[index] + 0.7152 * pixels[index + 1] + 0.0722 * pixels[index + 2]
            if luma > 0.62:
                bright += 1
            luma_min = min(luma_min, luma)
            luma_max = max(luma_max, luma)
            count += 1
    bright_ratio = bright / max(count, 1)
    return bright_ratio, luma_max - luma_min, luma_max


def viewport_has_rendered_scene(pixels, width, height):
    bright_ratio, luma_range, luma_max = rendered_scene_metrics(pixels, width, height)
    return bright_ratio > 0.02 and luma_range > 0.20 and luma_max > 0.70


bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

scene = bpy.context.scene
scene.render.engine = "BLENDER_EEVEE"
scene.eevee.use_taa_reprojection = True
scene.eevee.use_shadow_jitter_viewport = True
scene.eevee.use_shadows = True
scene.eevee.shadow_ray_count = 4
scene.eevee.shadow_step_count = 16
scene.eevee.taa_samples = 64
scene.render.resolution_x = 192
scene.render.resolution_y = 192
scene.render.resolution_percentage = 100
scene.view_settings.view_transform = "Standard"
scene.view_settings.look = "None"
scene.view_settings.exposure = 0.0
scene.view_settings.gamma = 1.0
scene.world.color = (0.02, 0.02, 0.02)

mat_ground = bpy.data.materials.new("TAAHistoryGround")
mat_ground.use_nodes = True
ground_bsdf = mat_ground.node_tree.nodes.get("Principled BSDF")
ground_bsdf.inputs["Base Color"].default_value = (0.78, 0.78, 0.78, 1.0)
ground_bsdf.inputs["Roughness"].default_value = 0.75

mat_sphere = bpy.data.materials.new("TAAHistoryCaster")
mat_sphere.use_nodes = True
sphere_bsdf = mat_sphere.node_tree.nodes.get("Principled BSDF")
sphere_bsdf.inputs["Base Color"].default_value = (0.95, 0.95, 0.95, 1.0)
sphere_bsdf.inputs["Roughness"].default_value = 0.5

initial_x = 0.0
moved_x = 1.3
plane_x = moved_x if args.mode == "reference" else initial_x

bpy.ops.mesh.primitive_plane_add(size=7.0, location=(plane_x, 0.0, 0.0))
plane = bpy.context.object
plane.name = "MovingShadowReceiver"
plane.visible_shadow = False
plane.data.materials.append(mat_ground)

bpy.ops.mesh.primitive_uv_sphere_add(segments=48, ring_count=24, radius=0.45, location=(0.0, 0.0, 0.55))
sphere = bpy.context.object
sphere.name = "SoftShadowCaster"
sphere.data.materials.append(mat_sphere)

bpy.ops.object.light_add(type="AREA", location=(-2.2, -3.0, 4.0))
light = bpy.context.object
light.name = "SoftShadowArea"
light.data.energy = 700.0
light.data.size = 3.0

bpy.ops.object.camera_add(location=(0.0, -5.4, 3.1))
camera = bpy.context.object
look_at(camera, (0.0, 0.0, 0.25))
camera.data.type = "ORTHO"
camera.data.ortho_scale = 4.2
scene.camera = camera

window, screen, area, region = find_view3d_context()
space = area.spaces.active
space.shading.type = "RENDERED"
space.overlay.show_overlays = False
space.region_3d.view_perspective = "CAMERA"

state = {"frames": 0}


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


def transform_receiver(target_x):
    plane.location.x = target_x
    bpy.context.view_layer.update()
    tag_redraw()
    print(f"RECEIVER_LOCATION_X={plane.location.x:.6f}")


def wait_for_rendered_scene(label):
    capture_viewport(probe_path)
    pixels, width, height = load_pixels(probe_path)
    bright_ratio, luma_range, luma_max = rendered_scene_metrics(pixels, width, height)
    content_ratio = non_ui_content_ratio(pixels, width, height)
    if viewport_has_rendered_scene(pixels, width, height):
        print(
            f"VIEWPORT_RENDER_READY label={label} bright_ratio={bright_ratio:.6f} "
            f"luma_range={luma_range:.6f} luma_max={luma_max:.6f} "
            f"content_ratio={content_ratio:.6f}"
        )
        return pixels, width, height, content_ratio
    state["ready_waits"] = state.get("ready_waits", 0) + 1
    if state["ready_waits"] > 160:
        bpy.ops.wm.quit_blender()
        raise RuntimeError(
            f"Viewport did not reach a rendered scene in time for {label}: "
            f"bright_ratio={bright_ratio:.6f}, luma_range={luma_range:.6f}, "
            f"luma_max={luma_max:.6f}, content_ratio={content_ratio:.6f}"
        )
    return None


def tick():
    tag_redraw()
    if state["frames"] == 0:
        window.event_simulate(type="ESC", value="PRESS")
        window.event_simulate(type="ESC", value="RELEASE")
    if args.mode == "candidate" and not state.get("warmed", False):
        if state["frames"] < 30:
            state["frames"] += 1
            return 0.03
        ready = wait_for_rendered_scene("candidate-warmup")
        if ready is None:
            state["frames"] += 1
            return 0.03
        state["warmed"] = True
        state["ready_waits"] = 0
        state["frames"] += 1
        transform_receiver(moved_x)
        return 0.03
    if args.mode == "candidate" and state["frames"] < 32:
        state["frames"] += 1
        return 0.03
    if args.mode == "reference" and state["frames"] < 8:
        state["frames"] += 1
        return 0.03

    ready = wait_for_rendered_scene(args.mode)
    if ready is None:
        state["frames"] += 1
        return 0.03
    pixels, width, height, content_ratio = ready

    probe_path.replace(out_path)
    print(
        f"VIEWPORT_CAPTURE_OK mode={args.mode} path={out_path} "
        f"content_ratio={content_ratio:.6f}"
    )
    bpy.ops.wm.quit_blender()
    return None


bpy.app.timers.register(tick, first_interval=0.2)
'''


def run_child(mode):
    output_path = OUTPUT_DIR / f"{mode}.png"
    command = [
        bpy.app.binary_path,
        "--factory-startup",
        "--enable-event-simulate",
        "--python",
        str(CHILD_SCRIPT),
        "--",
        "--mode",
        mode,
        "--out",
        str(output_path),
    ]
    result = subprocess.run(
        command,
        cwd=str(ROOT),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=45,
        check=False,
    )
    combined = result.stdout + result.stderr
    (OUTPUT_DIR / f"{mode}.log").write_text(combined, encoding="utf-8")
    assert result.returncode == 0, f"{mode} viewport capture failed:\n{combined[-3000:]}"
    assert f"VIEWPORT_CAPTURE_OK mode={mode}" in combined, (
        f"{mode} viewport capture did not reach success marker:\n{combined[-3000:]}"
    )
    assert output_path.exists(), f"{mode} viewport capture did not create {output_path}"
    return output_path


def load_pixels(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        return list(image.pixels[:]), image.size[0], image.size[1]
    finally:
        bpy.data.images.remove(image)


def luma(pixels, index):
    return 0.2126 * pixels[index] + 0.7152 * pixels[index + 1] + 0.0722 * pixels[index + 2]


def non_ui_content_ratio(pixels, width, height):
    x0 = int(width * 0.18)
    x1 = int(width * 0.82)
    y0 = int(height * 0.18)
    y1 = int(height * 0.82)
    count = 0
    lit = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            index = (y * width + x) * 4
            if max(pixels[index], pixels[index + 1], pixels[index + 2]) > 0.08:
                lit += 1
            count += 1
    return lit / max(count, 1)


OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
CHILD_SCRIPT.write_text(textwrap.dedent(CHILD_SOURCE), encoding="utf-8")

candidate_path = run_child("candidate")
reference_path = run_child("reference")

candidate, width, height = load_pixels(candidate_path)
reference, ref_width, ref_height = load_pixels(reference_path)
assert (width, height) == (ref_width, ref_height), (
    f"Viewport capture sizes differ: candidate={width}x{height} reference={ref_width}x{ref_height}"
)

candidate_content_ratio = non_ui_content_ratio(candidate, width, height)
reference_content_ratio = non_ui_content_ratio(reference, width, height)

x0 = int(width * 0.22)
x1 = int(width * 0.82)
y0 = int(height * 0.22)
y1 = int(height * 0.82)

diff_sum = 0.0
diff_max = 0.0
content_min = 1.0
content_max = 0.0
count = 0
for y in range(y0, y1):
    for x in range(x0, x1):
        index = (y * width + x) * 4
        candidate_luma = luma(candidate, index)
        reference_luma = luma(reference, index)
        diff = abs(candidate_luma - reference_luma)
        diff_sum += diff
        diff_max = max(diff_max, diff)
        content_min = min(content_min, candidate_luma)
        content_max = max(content_max, candidate_luma)
        count += 1

mean_diff = diff_sum / max(count, 1)
content_range = content_max - content_min

print(f"TAA_SOFT_SHADOW_CAPTURE_SIZE={width}x{height}")
print(f"TAA_SOFT_SHADOW_MEAN_DIFF={mean_diff:.6f}")
print(f"TAA_SOFT_SHADOW_MAX_DIFF={diff_max:.6f}")
print(f"TAA_SOFT_SHADOW_CONTENT_RANGE={content_range:.6f}")
print(f"TAA_SOFT_SHADOW_CONTENT_RATIO={candidate_content_ratio:.6f}")
print(f"TAA_SOFT_SHADOW_OUTPUT_DIR={OUTPUT_DIR}")

assert content_range > 0.08, (
    "Viewport capture does not contain enough lit/shadow scene contrast "
    f"(range={content_range:.6f})."
)
assert candidate_content_ratio > 0.18, (
    "Viewport capture still looks like a UI or empty frame instead of a rendered scene "
    f"(content_ratio={candidate_content_ratio:.6f}, reference={reference_content_ratio:.6f})."
)
assert mean_diff < 0.006, (
    "The first post-transform viewport frame differs too much from a fresh moved-position "
    f"reference (mean_diff={mean_diff:.6f}, max_diff={diff_max:.6f})."
)

print("EEVEE_TAA_SOFT_SHADOW_HISTORY_RESET_OK")
