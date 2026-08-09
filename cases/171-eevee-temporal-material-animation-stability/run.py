from pathlib import Path
import json
import statistics
import subprocess
import sys
import textwrap

import bpy


CASE_DIR = Path(__file__).resolve().parent
ROOT = CASE_DIR.parents[3]
OUTPUT_DIR = ROOT / "temp" / "release_test_outputs" / (
    "eevee_temporal_material_animation_stability"
)
CHILD_SCRIPT = OUTPUT_DIR / "viewport_playback_child.py"
METADATA_PATH = OUTPUT_DIR / "metadata.json"
STAGES = ("control", "animated", "empty_action", "scene_time")
CAPTURE_COUNT = 8

CHILD_SOURCE = r'''
import argparse
import json
from pathlib import Path
import sys

import bpy


argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
parser = argparse.ArgumentParser()
parser.add_argument("--out-dir", required=True)
args = parser.parse_args(argv)
out_dir = Path(args.out_dir)
out_dir.mkdir(parents=True, exist_ok=True)
metadata_path = out_dir / "metadata.json"

stages = ("control", "animated", "empty_action", "scene_time")
capture_count = 8
warmup_frames = 16


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


def tag_redraw():
    for candidate_window in bpy.context.window_manager.windows:
        for candidate_area in candidate_window.screen.areas:
            if candidate_area.type == "VIEW_3D":
                candidate_area.tag_redraw()


def capture_viewport(path):
    with bpy.context.temp_override(window=window, screen=screen, area=area, region=region):
        result = bpy.ops.screen.screenshot("EXEC_DEFAULT", filepath=str(path))
    if result != {"FINISHED"}:
        raise RuntimeError(f"Viewport screenshot failed: {result}")


def load_pixels(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        return list(image.pixels[:]), int(image.size[0]), int(image.size[1])
    finally:
        bpy.data.images.remove(image)


def rendered_content_ready(path):
    capture_viewport(path)
    pixels, width, height = load_pixels(path)
    x0 = int(width * 0.30)
    x1 = int(width * 0.70)
    y0 = int(height * 0.30)
    y1 = int(height * 0.70)
    luma_min = 1.0
    luma_max = 0.0
    for y in range(y0, y1):
        for x in range(x0, x1):
            index = (y * width + x) * 4
            luma = (
                0.2126 * pixels[index]
                + 0.7152 * pixels[index + 1]
                + 0.0722 * pixels[index + 2]
            )
            luma_min = min(luma_min, luma)
            luma_max = max(luma_max, luma)
    return luma_max > 0.70 and (luma_max - luma_min) > 0.55


def action_fcurve_count(action):
    return sum(
        len(channelbag.fcurves)
        for layer in action.layers
        for strip in layer.strips
        for channelbag in strip.channelbags
    )


def set_playback(enabled):
    if bool(screen.is_animation_playing) == enabled:
        return
    with bpy.context.temp_override(window=window, screen=screen, area=area, region=region):
        result = bpy.ops.screen.animation_play()
    if result != {"FINISHED"} or bool(screen.is_animation_playing) != enabled:
        raise RuntimeError(
            f"Could not set playback={enabled}: result={result}, "
            f"is_animation_playing={screen.is_animation_playing}"
        )


bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

scene = bpy.context.scene
scene.render.engine = "BLENDER_EEVEE"
scene.eevee.use_taa_reprojection = True
scene.eevee.taa_samples = 64
scene.frame_start = 1
scene.frame_end = 240
scene.render.resolution_x = 256
scene.render.resolution_y = 256
scene.render.resolution_percentage = 100
scene.view_settings.view_transform = "Standard"
scene.view_settings.look = "None"
scene.view_settings.exposure = 0.0
scene.view_settings.gamma = 1.0

if hasattr(scene.eevee, "use_shadows"):
    scene.eevee.use_shadows = False
if hasattr(scene.eevee, "use_raytracing"):
    scene.eevee.use_raytracing = False

scene.world.use_nodes = True
background = scene.world.node_tree.nodes.get("Background")
background.inputs["Color"].default_value = (0.0, 0.0, 0.0, 1.0)
background.inputs["Strength"].default_value = 0.0

material = bpy.data.materials.new("TemporalPlaybackMaterial")
material.use_nodes = True
nodes = material.node_tree.nodes
nodes.clear()
output = nodes.new("ShaderNodeOutputMaterial")
emission = nodes.new("ShaderNodeEmission")
emission.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
emission.inputs["Strength"].default_value = 1.0
material.node_tree.links.new(emission.outputs["Emission"], output.inputs["Surface"])

bpy.ops.mesh.primitive_plane_add(size=2.0, location=(0.0, 0.0, 0.0))
strip = bpy.context.object
strip.name = "TemporalStabilityStrip"
strip.scale = (0.48, 1.55, 1.0)
strip.data.materials.append(material)

bpy.ops.object.camera_add(location=(0.0, 0.0, 5.0))
camera = bpy.context.object
camera.name = "TemporalStabilityCamera"
camera.data.type = "ORTHO"
camera.data.ortho_scale = 4.0
scene.camera = camera

window, screen, area, region = find_view3d_context()
space = area.spaces.active
space.shading.type = "RENDERED"
space.overlay.show_overlays = False
space.region_3d.view_perspective = "CAMERA"

metadata = {
    "build_hash": bpy.app.build_hash.decode("ascii", errors="replace"),
    "stages": {},
}
state = {
    "ready": False,
    "ready_waits": 0,
    "stage_index": -1,
    "settle_ticks": 0,
    "playback_started": False,
    "last_frame": None,
    "distinct_frames": 0,
    "capture_index": 0,
    "action": None,
}
probe_path = out_dir / "readiness_probe.png"


def configure_stage(stage):
    if stage == "control":
        if material.animation_data is not None:
            raise RuntimeError("Control stage unexpectedly has animation data")
        metadata["stages"][stage] = {"action": False, "fcurves": 0, "frames": []}
    elif stage == "animated":
        material.diffuse_color = (0.2, 0.3, 0.4, 1.0)
        material.keyframe_insert(data_path="diffuse_color", index=0, frame=1)
        material.keyframe_insert(data_path="diffuse_color", index=0, frame=240)
        action = material.animation_data.action
        curve_count = action_fcurve_count(action)
        if curve_count != 1:
            raise RuntimeError(f"Expected one material FCurve, found {curve_count}")
        state["action"] = action
        metadata["stages"][stage] = {
            "action": True,
            "fcurves": curve_count,
            "frames": [],
        }
    elif stage == "empty_action":
        action = state["action"]
        material.keyframe_delete(data_path="diffuse_color", index=0, frame=1)
        material.keyframe_delete(data_path="diffuse_color", index=0, frame=240)
        if material.animation_data.action != action:
            raise RuntimeError("Deleting the last keyframe did not retain the assigned Action")
        curve_count = action_fcurve_count(action)
        if curve_count != 0:
            raise RuntimeError(f"Expected an empty retained Action, found {curve_count} FCurves")
        metadata["stages"][stage] = {
            "action": True,
            "fcurves": curve_count,
            "frames": [],
        }
    else:
        scene_time = nodes.new("GeometryNodeInputSceneTime")
        zero_time = nodes.new("ShaderNodeMath")
        zero_time.operation = "MULTIPLY"
        zero_time.inputs[1].default_value = 0.0
        constant_strength = nodes.new("ShaderNodeMath")
        constant_strength.operation = "ADD"
        constant_strength.inputs[1].default_value = 1.0
        material.node_tree.links.new(scene_time.outputs["Frame"], zero_time.inputs[0])
        material.node_tree.links.new(zero_time.outputs[0], constant_strength.inputs[0])
        material.node_tree.links.new(constant_strength.outputs[0], emission.inputs["Strength"])
        metadata["stages"][stage] = {
            "action": True,
            "fcurves": action_fcurve_count(state["action"]),
            "scene_time": True,
            "frames": [],
        }


def begin_next_stage():
    set_playback(False)
    state["stage_index"] += 1
    if state["stage_index"] >= len(stages):
        metadata_path.write_text(json.dumps(metadata, indent=2), encoding="utf-8")
        print(f"TEMPORAL_PLAYBACK_CAPTURE_OK metadata={metadata_path}")
        bpy.ops.wm.quit_blender()
        return

    stage = stages[state["stage_index"]]
    configure_stage(stage)
    scene.frame_set(1)
    state["settle_ticks"] = 5
    state["playback_started"] = False
    state["last_frame"] = None
    state["distinct_frames"] = 0
    state["capture_index"] = 0
    tag_redraw()
    print(f"TEMPORAL_PLAYBACK_STAGE_BEGIN stage={stage}")


def tick():
    tag_redraw()
    if not state["ready"]:
        if state["ready_waits"] == 0:
            window.event_simulate(type="ESC", value="PRESS")
            window.event_simulate(type="ESC", value="RELEASE")
        state["ready_waits"] += 1
        if state["ready_waits"] < 20:
            return 0.03
        if not rendered_content_ready(probe_path):
            if state["ready_waits"] > 160:
                bpy.ops.wm.quit_blender()
                raise RuntimeError("Rendered viewport did not become ready")
            return 0.03
        state["ready"] = True
        begin_next_stage()
        return 0.03

    if state["settle_ticks"] > 0:
        state["settle_ticks"] -= 1
        return 0.03

    if not state["playback_started"]:
        set_playback(True)
        state["playback_started"] = True
        return 0.03

    frame = int(scene.frame_current)
    if frame == state["last_frame"]:
        return 0.01
    state["last_frame"] = frame
    state["distinct_frames"] += 1

    if state["distinct_frames"] <= warmup_frames:
        return 0.01

    stage = stages[state["stage_index"]]
    capture_index = state["capture_index"]
    capture_path = out_dir / f"{stage}_{capture_index:02d}.png"
    capture_viewport(capture_path)
    metadata["stages"][stage]["frames"].append(frame)
    state["capture_index"] += 1

    if state["capture_index"] >= capture_count:
        print(
            f"TEMPORAL_PLAYBACK_STAGE_CAPTURED stage={stage} "
            f"frames={metadata['stages'][stage]['frames']}"
        )
        begin_next_stage()
    return 0.01


bpy.app.timers.register(tick, first_interval=0.2)
'''


def run_child():
    command = [
        bpy.app.binary_path,
        "--factory-startup",
        "--enable-event-simulate",
        "--python",
        str(CHILD_SCRIPT),
        "--",
        "--out-dir",
        str(OUTPUT_DIR),
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
    (OUTPUT_DIR / "viewport_playback_child.log").write_text(combined, encoding="utf-8")
    assert result.returncode == 0, f"viewport playback child failed:\n{combined[-5000:]}"
    assert "TEMPORAL_PLAYBACK_CAPTURE_OK" in combined, (
        f"viewport playback child did not reach success marker:\n{combined[-5000:]}"
    )


def load_pixels(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        return list(image.pixels[:]), int(image.size[0]), int(image.size[1])
    finally:
        bpy.data.images.remove(image)


def luma(pixels, index):
    return 0.2126 * pixels[index] + 0.7152 * pixels[index + 1] + 0.0722 * pixels[index + 2]


def vertical_edge_position(path):
    pixels, width, height = load_pixels(path)
    x0 = int(width * 0.30)
    x1 = int(width * 0.50)
    y0 = int(height * 0.34)
    y1 = int(height * 0.66)
    profile = []
    for x in range(x0, x1 + 1):
        value = 0.0
        for y in range(y0, y1):
            value += luma(pixels, (y * width + x) * 4)
        profile.append(value / max(y1 - y0, 1))

    gradients = [max(profile[i + 1] - profile[i], 0.0) for i in range(len(profile) - 1)]
    peak = max(range(len(gradients)), key=gradients.__getitem__)
    start = max(0, peak - 4)
    end = min(len(gradients), peak + 5)
    weight_sum = sum(gradients[start:end])
    assert weight_sum > 0.20, f"No high-contrast emission edge found in {path}"
    return sum((x0 + i + 0.5) * gradients[i] for i in range(start, end)) / weight_sum


OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
for old_capture in OUTPUT_DIR.glob("*.png"):
    old_capture.unlink()
if METADATA_PATH.exists():
    METADATA_PATH.unlink()
CHILD_SCRIPT.write_text(textwrap.dedent(CHILD_SOURCE), encoding="utf-8")

run_child()
assert METADATA_PATH.exists(), f"Missing child metadata: {METADATA_PATH}"
metadata = json.loads(METADATA_PATH.read_text(encoding="utf-8"))

spans = {}
for stage in STAGES:
    stage_data = metadata["stages"][stage]
    frames = stage_data["frames"]
    assert len(frames) == CAPTURE_COUNT, f"{stage} captured {len(frames)} frames"
    assert len(set(frames)) == CAPTURE_COUNT, f"{stage} did not capture distinct playback frames"
    positions = [
        vertical_edge_position(OUTPUT_DIR / f"{stage}_{index:02d}.png")
        for index in range(CAPTURE_COUNT)
    ]
    span = max(positions) - min(positions)
    deviation = statistics.pstdev(positions)
    spans[stage] = span
    print(f"TEMPORAL_MATERIAL_{stage.upper()}_EDGE_POSITIONS={positions}")
    print(f"TEMPORAL_MATERIAL_{stage.upper()}_EDGE_SPAN={span:.6f}")
    print(f"TEMPORAL_MATERIAL_{stage.upper()}_EDGE_STDDEV={deviation:.6f}")

assert metadata["stages"]["control"]["action"] is False
assert metadata["stages"]["animated"]["fcurves"] == 1
assert metadata["stages"]["empty_action"]["action"] is True
assert metadata["stages"]["empty_action"]["fcurves"] == 0

control_span = spans["control"]
assert control_span < 0.30, (
    f"No-animation playback control is not stable: edge_span={control_span:.6f}"
)
for stage in ("animated", "empty_action"):
    assert spans[stage] < 0.30, (
        f"Temporal reprojection exposed subpixel jitter during {stage} playback: "
        f"edge_span={spans[stage]:.6f}, control={control_span:.6f}"
    )
    assert abs(spans[stage] - control_span) <= 0.12, (
        f"Temporal reprojection did not retain control-like sampling during {stage} playback: "
        f"edge_span={spans[stage]:.6f}, control={control_span:.6f}"
    )
assert metadata["stages"]["scene_time"]["scene_time"] is True
assert spans["scene_time"] < 0.05, (
    "Scene Time history invalidation exposed changing subpixel jitter: "
    f"edge_span={spans['scene_time']:.6f}"
)

print(f"TEMPORAL_MATERIAL_BUILD_HASH={metadata['build_hash']}")
print(f"TEMPORAL_MATERIAL_OUTPUT_DIR={OUTPUT_DIR}")
print("EEVEE_TEMPORAL_MATERIAL_ANIMATION_STABILITY_OK")
