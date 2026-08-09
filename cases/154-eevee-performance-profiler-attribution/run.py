import math
import re
import subprocess
import sys
import textwrap
from pathlib import Path

import bpy


CASE_DIR = Path(__file__).resolve().parent
ROOT = CASE_DIR.parents[3]
OUTPUT_DIR = ROOT / "temp" / "release_test_outputs" / "eevee_performance_profiler_attribution"
VIEWPORT_CHILD_SCRIPT = OUTPUT_DIR / "viewport_runtime_report_child.py"

sys.path.insert(0, str(ROOT / "test" / "release"))
from release_case_utils import source_repo_path  # noqa: E402

sys.path.insert(0, str(source_repo_path(ROOT) / "tests" / "python" / "npr"))
from filter_graph_test_utils import (  # noqa: E402
    add_pass_input_image_sample,
    attach_filter_material as attach_filter_material_to_graph,
    clear_filter_graph,
)


VIEWPORT_CHILD_SOURCE = r'''
import bpy
import math
import re


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


def find_outliner_area():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == "OUTLINER":
                return area
    raise RuntimeError("No OUTLINER area found")


bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

scene = bpy.context.scene
scene.render.engine = "BLENDER_EEVEE"
scene.frame_set(1)
scene.render.resolution_x = 96
scene.render.resolution_y = 96
scene.render.resolution_percentage = 100
scene.eevee.taa_samples = 1
scene.eevee.use_taa_reprojection = False
scene.eevee.use_performance_profiler = True
scene.eevee.use_performance_profiler_stage_list = False
scene.eevee.use_performance_profiler_time_sort = False
if hasattr(scene.eevee, "use_gtao"):
    scene.eevee.use_gtao = True

mat = bpy.data.materials.new("ViewportProfilerSurface")
mat.use_nodes = True
bsdf = mat.node_tree.nodes.get("Principled BSDF")
if bsdf:
    bsdf.inputs["Base Color"].default_value = (0.7, 0.55, 0.4, 1.0)


def add_viewport_glsl_function():
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    if bsdf is None:
        raise RuntimeError("Viewport profiler material has no Principled BSDF")
    glsl_text = bpy.data.texts.new("viewport_profiler_surface.glsl")
    glsl_text.write(
        "vec4 viewport_profiler_color(){\n"
        "  return vec4(0.7, 0.55, 0.4, 1.0);\n"
        "}\n"
    )
    glsl = mat.node_tree.nodes.new("ShaderNodeGLSLFunction")
    glsl.script = glsl_text
    glsl.function_name = "viewport_profiler_color"
    glsl.function_name = ""
    glsl.function_name = "viewport_profiler_color"
    mat.node_tree.interface_update(bpy.context)
    mat.node_tree.update_tag()
    bpy.context.view_layer.update()
    if glsl.parse_status != "READY":
        raise RuntimeError(f"Viewport GLSL Function parse status is {glsl.parse_status}")
    mat.node_tree.links.new(glsl.outputs["Result"], bsdf.inputs["Base Color"])

bpy.ops.mesh.primitive_plane_add(size=8.0, location=(0.0, 0.0, 0.0))
floor = bpy.context.object
floor.name = "ViewportProfilerFloor"
floor.data.materials.append(mat)

bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, 0.55))
cube = bpy.context.object
cube.name = "ViewportProfilerCube"
cube.data.materials.append(mat)

bpy.ops.object.light_add(type="SUN", location=(0.0, 0.0, 5.0), rotation=(0.9, 0.0, 0.4))
sun = bpy.context.object
sun.name = "ViewportProfilerSun"
sun.data.energy = 3.0

bpy.ops.object.lightprobe_add(type="PLANE", location=(0.0, 1.5, 1.2), rotation=(1.5708, 0.0, 0.0))
bpy.context.object.name = "ViewportProfilerPlanarProbe"

bpy.ops.object.camera_add(location=(0.0, -6.0, 3.2), rotation=(1.1, 0.0, 0.0))
scene.camera = bpy.context.object

window, screen, area, region = find_view3d_context()
space = area.spaces.active
space.shading.type = "RENDERED"
space.overlay.show_overlays = False
space.region_3d.view_perspective = "CAMERA"
outliner_area = find_outliner_area()
outliner_space = outliner_area.spaces.active
outliner_modes = {
    item.identifier for item in outliner_space.bl_rna.properties["display_mode"].enum_items
}
if "EEVEE_PERFORMANCE" not in outliner_modes:
    raise RuntimeError("OUTLINER display mode EEVEE_PERFORMANCE is unavailable")
outliner_space.display_mode = "EEVEE_PERFORMANCE"

state = {
    "phase": "warm",
    "warm_draws": 0,
    "source_before": None,
    "capture_before": None,
    "source_after_update": None,
    "capture_after_update": None,
    "glsl_draws": 0,
}


def draw_once():
    area.tag_redraw()
    outliner_area.tag_redraw()
    bpy.ops.wm.redraw_timer(type="DRAW_WIN_SWAP", iterations=1)


def report_serial(report):
    match = re.search(r"^Depsgraph Eval Serial:\s+(\d+)$", report, re.MULTILINE)
    return int(match.group(1)) if match else None


def report_capture_sequence(report):
    match = re.search(r"^Capture Sequence:\s+(\d+)$", report, re.MULTILINE)
    return int(match.group(1)) if match else None


def report_source_id(report):
    match = re.search(r"^Source ID:\s+(\d+)$", report, re.MULTILINE)
    return int(match.group(1)) if match else None


def one_sample_complete(report):
    return bool(
        re.search(r"^Sample Progress:\s+1/1$", report, re.MULTILINE)
        and re.search(r"^Sampling:\s+Complete$", report, re.MULTILINE)
    )


def assert_one_sample_complete(report, phase):
    if not one_sample_complete(report):
        bpy.ops.wm.quit_blender()
        raise RuntimeError(
            f"{phase} did not publish a completed 1/1 viewport sample:\n{report}"
        )


def assert_glsl_material_count(report, phase):
    if not re.search(r"GLSL Mats=1(?:\s|$)", report):
        bpy.ops.wm.quit_blender()
        raise RuntimeError(f"{phase} did not report one GLSL Function material:\n{report}")


def tick():
    if state["phase"] == "glsl_warm":
        draw_once()
        report = scene.eevee.performance_profiler_viewport_report
        if one_sample_complete(report) and re.search(r"GLSL Mats=1(?:\s|$)", report):
            state["phase"] = "glsl_resume"
            space.shading.type = "SOLID"
            return 0.2
        state["glsl_draws"] += 1
        if state["glsl_draws"] >= 40:
            bpy.ops.wm.quit_blender()
            raise RuntimeError(
                "Live GLSL material update did not publish a stable profiler report:\n"
                f"{report}"
            )
        return 0.03

    if state["phase"] == "glsl_resume":
        state["phase"] = "glsl_check_resumed"
        space.shading.type = "RENDERED"
        scene.frame_set(4)
        return 0.2

    if state["phase"] == "glsl_check_resumed":
        report = scene.eevee.performance_profiler_viewport_report
        assert_one_sample_complete(report, "GLSL Solid-to-Rendered update")
        assert_glsl_material_count(report, "GLSL Solid-to-Rendered update")
        print("VIEWPORT_PERFORMANCE_RUNTIME_REPORT_OK")
        print(report)
        bpy.ops.wm.quit_blender()
        return None

    if state["phase"] == "warm":
        draw_once()
        warm_report = scene.eevee.performance_profiler_viewport_report
        serial_before = report_serial(warm_report)
        source_before = report_source_id(warm_report)
        capture_before = report_capture_sequence(warm_report)
        if (
            serial_before is None
            or source_before is None
            or capture_before is None
            or not one_sample_complete(warm_report)
        ):
            state["warm_draws"] += 1
            if state["warm_draws"] >= 40:
                bpy.ops.wm.quit_blender()
                raise RuntimeError(
                    "Viewport profiler did not reach a completed 1/1 warm report:\n"
                    f"{warm_report}"
                )
            return 0.03

        assert_one_sample_complete(warm_report, "Warm viewport")
        state["source_before"] = source_before
        state["capture_before"] = capture_before
        state["phase"] = "check_update"
        # Return to the real event loop. With one sample and reprojection disabled, this scene
        # change receives one automatic EEVEE redraw and does not schedule an accumulation redraw.
        scene.frame_set(2)
        return 0.2

    if state["phase"] == "check_update":
        report_after_update = scene.eevee.performance_profiler_viewport_report
        assert_one_sample_complete(report_after_update, "Event-loop scene update")
        source_after_update = report_source_id(report_after_update)
        capture_after_update = report_capture_sequence(report_after_update)
        if (
            source_after_update != state["source_before"]
            or capture_after_update is None
            or capture_after_update <= state["capture_before"]
            or not re.search(r"^Frame:\s+2$", report_after_update, re.MULTILINE)
        ):
            bpy.ops.wm.quit_blender()
            raise RuntimeError(
                "The event-loop redraw after a scene update kept the old profiler capture: "
                f"source_before={state['source_before']} source_after={source_after_update} "
                f"capture_before={state['capture_before']} capture_after={capture_after_update}\n"
                f"{report_after_update}"
            )
        state["source_after_update"] = source_after_update
        state["capture_after_update"] = capture_after_update
        state["phase"] = "resume_update"
        # Let the event loop draw Solid once so the active EEVEE Instance is actually freed.
        space.shading.type = "SOLID"
        return 0.2

    if state["phase"] == "resume_update":
        state["phase"] = "check_resumed"
        space.shading.type = "RENDERED"
        scene.frame_set(3)
        return 0.2

    report = scene.eevee.performance_profiler_viewport_report
    assert_one_sample_complete(report, "Solid-to-Rendered event-loop update")
    source_resumed = report_source_id(report)
    capture_resumed = report_capture_sequence(report)
    if (
        source_resumed is None
        or capture_resumed is None
        or (
            source_resumed == state["source_after_update"]
            and capture_resumed <= state["capture_after_update"]
        )
        or not re.search(r"^Frame:\s+3$", report, re.MULTILINE)
    ):
        bpy.ops.wm.quit_blender()
        raise RuntimeError(
            "The recreated EEVEE Instance kept the old profiler capture: "
            f"source_before={state['source_after_update']} source_after={source_resumed} "
            f"capture_before={state['capture_after_update']} capture_after={capture_resumed}\n"
            f"{report}"
        )

    if outliner_space.display_mode != "EEVEE_PERFORMANCE":
        bpy.ops.wm.quit_blender()
        raise RuntimeError("OUTLINER left the EEVEE_PERFORMANCE structured snapshot view")
    required = [
        "Viewport Draw CPU:",
        "Average Draw CPU:",
        "Profiler Accounting CPU:",
        "Timing Domain: CPU wall time",
        "Timing Scope: Shared 3D draw cycle",
        "Accounting: Inclusive scopes",
        "Last Evaluation:",
        "Depsgraph Eval Serial:",
        "Source ID:",
        "Capture Sequence:",
        "Features:",
        "Draw.Sync.Shared",
        "Sync.Begin.World",
        "Sync.Begin.SceneModules",
        "Sync.Begin.ViewEffects",
        "Sync.Begin.NPRPost",
        "Sync.End.ShaderReadiness",
        "Sync.End.MaterialsVelocity",
        "Sync.End.VolumeShadowsLights",
        "Sync.End.FrameState",
        "Sync.End.NPRPost",
        "Sync.End.ProbesUniforms",
        "Draw.Sync.EngineSetup",
        "Draw.Sync.EngineInit",
        "Draw.Sync.ManagerBegin",
        "Draw.Sync.EngineBegin",
        "Draw.Sync.ModulesBegin",
        "Draw.Sync.ObjectIteration",
        "Draw.Sync.DupliExtraction",
        "Draw.Sync.DelayedExtraction",
        "Draw.Sync.ExtractionWait",
        "Draw.Sync.CurvesUpdate",
        "Draw.Sync.EngineEnd",
        "Draw.Sync.ManagerEnd",
        "Draw.Submission.Shared",
        "Draw.Submission.Framebuffer",
        "Draw.Submission.CallbacksPre",
        "Draw.Submission.EngineDraw",
        "Draw.Submission.CallbacksPost",
        "Draw.Submission.FramebufferRestore",
    ]
    missing = [text for text in required if text not in report]
    if missing:
        bpy.ops.wm.quit_blender()
        raise RuntimeError(f"Viewport performance report missed {missing}:\n{report}")
    forbidden = [
        "Hints:",
        "Top CPU Stages:",
        "Sync Texture Loads:",
        "Sync Objects:",
        "Film Outputs:",
        "Filter Costs:",
        "Render Texture Costs:",
    ]
    present = [text for text in forbidden if text in report]
    if present:
        bpy.ops.wm.quit_blender()
        raise RuntimeError(f"Viewport performance report kept removed sections {present}:\n{report}")

    def report_match(pattern, label):
        match = re.search(pattern, report)
        if not match:
            bpy.ops.wm.quit_blender()
            raise RuntimeError(f"Viewport performance report missed {label}:\n{report}")
        return match

    def stage_metrics(label):
        match = report_match(
            rf"-\s+{re.escape(label)}\s+([0-9.]+) ms\s+\|\s+avg\s+[0-9.]+\s+\|\s+calls\s+(\d+)",
            label,
        )
        return float(match.group(1)), int(match.group(2))

    draw_cpu = float(
        report_match(r"Viewport Draw CPU:\s+([0-9.]+) ms", "Viewport Draw CPU").group(1)
    )
    average_draw_cpu = float(
        report_match(r"Average Draw CPU:\s+([0-9.]+) ms", "Average Draw CPU").group(1)
    )
    profiler_accounting_cpu = float(
        report_match(
            r"Profiler Accounting CPU:\s+"
            r"([-+]?(?:(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][-+]?[0-9]+)?|"
            r"(?i:inf|nan))) ms "
            r"\(excluded from Draw CPU; report formatting and snapshot publication not included\)",
            "Profiler Accounting CPU",
        ).group(1)
    )
    int(
        report_match(
            r"Depsgraph Eval Serial:\s+(\d+)",
            "numeric Depsgraph Eval Serial",
        ).group(1)
    )
    if average_draw_cpu <= 0.0:
        bpy.ops.wm.quit_blender()
        raise RuntimeError(f"Average Draw CPU must be positive: {average_draw_cpu}\n{report}")
    if not math.isfinite(profiler_accounting_cpu) or profiler_accounting_cpu < 0.0:
        bpy.ops.wm.quit_blender()
        raise RuntimeError(
            "Profiler Accounting CPU must be finite and non-negative: "
            f"{profiler_accounting_cpu}\n{report}"
        )
    sync_cpu, sync_calls = stage_metrics("Draw.Sync.Shared")
    submission_cpu, submission_calls = stage_metrics("Draw.Submission.Shared")
    if draw_cpu <= 0.0 or sync_cpu <= 0.0 or submission_cpu <= 0.0:
        bpy.ops.wm.quit_blender()
        raise RuntimeError(
            f"Viewport draw roots must be non-zero: draw={draw_cpu} "
            f"sync={sync_cpu} submission={submission_cpu}\n{report}"
        )
    if sync_calls != 1 or submission_calls != 1:
        bpy.ops.wm.quit_blender()
        raise RuntimeError(
            f"Viewport draw roots must run once: sync={sync_calls} submission={submission_calls}\n"
            f"{report}"
        )
    if abs(draw_cpu - (sync_cpu + submission_cpu)) > 0.01:
        bpy.ops.wm.quit_blender()
        raise RuntimeError(
            "Viewport draw total does not reconcile with sync/submission totals:\n"
            f"draw={draw_cpu} sync={sync_cpu} submission={submission_cpu}\n{report}"
        )

    sync_children = [
        "Draw.Sync.EngineSetup",
        "Draw.Sync.EngineInit",
        "Draw.Sync.ManagerBegin",
        "Draw.Sync.EngineBegin",
        "Draw.Sync.ModulesBegin",
        "Draw.Sync.ObjectIteration",
        "Draw.Sync.DupliExtraction",
        "Draw.Sync.DelayedExtraction",
        "Draw.Sync.ExtractionWait",
        "Draw.Sync.CurvesUpdate",
        "Draw.Sync.EngineEnd",
        "Draw.Sync.ManagerEnd",
    ]
    submission_children = [
        "Draw.Submission.Framebuffer",
        "Draw.Submission.CallbacksPre",
        "Draw.Submission.EngineDraw",
        "Draw.Submission.CallbacksPost",
        "Draw.Submission.FramebufferRestore",
    ]
    sync_child_total = 0.0
    sync_child_values = {}
    for label in sync_children:
        value, calls = stage_metrics(label)
        if calls != 1:
            bpy.ops.wm.quit_blender()
            raise RuntimeError(f"Expected one {label} call, got {calls}:\n{report}")
        sync_child_values[label] = value
        sync_child_total += value
    submission_child_total = 0.0
    submission_child_values = {}
    for label in submission_children:
        value, calls = stage_metrics(label)
        if calls != 1:
            bpy.ops.wm.quit_blender()
            raise RuntimeError(f"Expected one {label} call, got {calls}:\n{report}")
        submission_child_values[label] = value
        submission_child_total += value
    if sync_child_values["Draw.Sync.ObjectIteration"] <= 0.0:
        bpy.ops.wm.quit_blender()
        raise RuntimeError(f"Object Iteration timing stayed zero in a populated scene:\n{report}")
    if submission_child_values["Draw.Submission.EngineDraw"] <= 0.0:
        bpy.ops.wm.quit_blender()
        raise RuntimeError(f"Engine Draw timing stayed zero:\n{report}")
    if sync_cpu >= 0.1 and sync_child_total < sync_cpu * 0.5:
        bpy.ops.wm.quit_blender()
        raise RuntimeError(
            f"Sync direct phases leave too much unaccounted time: "
            f"root={sync_cpu} children={sync_child_total}\n{report}"
        )
    if submission_cpu >= 0.1 and submission_child_total < submission_cpu * 0.5:
        bpy.ops.wm.quit_blender()
        raise RuntimeError(
            f"Submission direct phases leave too much unaccounted time: "
            f"root={submission_cpu} children={submission_child_total}\n{report}"
        )
    if sync_child_total > sync_cpu + 0.02:
        bpy.ops.wm.quit_blender()
        raise RuntimeError(
            f"Sync children exceed their root: root={sync_cpu} children={sync_child_total}\n{report}"
        )
    if submission_child_total > submission_cpu + 0.01:
        bpy.ops.wm.quit_blender()
        raise RuntimeError(
            "Submission children exceed their root: "
            f"root={submission_cpu} children={submission_child_total}\n{report}"
        )
    if not re.search(r"Last Evaluation:\s+[0-9.]+ ms \(not included in Draw CPU\)", report):
        bpy.ops.wm.quit_blender()
        raise RuntimeError(f"Last Evaluation inclusion semantics are missing:\n{report}")

    add_viewport_glsl_function()
    state["phase"] = "glsl_warm"
    area.tag_redraw()
    return 0.2


bpy.app.timers.register(tick, first_interval=0.2)
'''


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def set_engine(scene):
    engines = {item.identifier for item in scene.render.bl_rna.properties["engine"].enum_items}
    if "BLENDER_EEVEE" not in engines:
        raise RuntimeError("BLENDER_EEVEE render engine is unavailable")
    scene.render.engine = "BLENDER_EEVEE"


def configure_common_scene(scene, resolution=96, samples=1):
    set_engine(scene)
    scene.frame_set(1)
    scene.render.resolution_x = resolution
    scene.render.resolution_y = resolution
    scene.render.resolution_percentage = 100
    scene.render.use_compositing = False
    scene.eevee.taa_render_samples = samples
    scene.eevee.use_performance_profiler = True
    scene.eevee.use_performance_profiler_stage_list = True
    scene.eevee.use_performance_profiler_time_sort = False


def make_text_block(name, source):
    text = bpy.data.texts.get(name)
    if text is None:
        text = bpy.data.texts.new(name)
    else:
        text.clear()
    text.write(source)
    return text


def refresh_glsl_node(node):
    current_name = node.function_name
    node.function_name = ""
    node.function_name = current_name
    node.id_data.interface_update(bpy.context)
    node.id_data.update_tag()
    bpy.context.view_layer.update()


def make_mat(name, color):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    if bsdf:
        bsdf.inputs["Base Color"].default_value = color
        bsdf.inputs["Roughness"].default_value = 0.55
    return mat


def add_light(name, light_type, location, rotation=None, energy=500.0):
    data = bpy.data.lights.new(name, light_type)
    data.energy = energy
    if hasattr(data, "use_shadow"):
        data.use_shadow = True
    obj = bpy.data.objects.new(name, data)
    bpy.context.collection.objects.link(obj)
    obj.location = location
    if rotation:
        obj.rotation_euler = rotation
    return obj


def stage_value(report, label):
    match = re.search(rf"-\s+{re.escape(label)}\s+([0-9.]+)\s+(?:CPU\s+)?ms", report)
    if not match:
        raise AssertionError(f"Missing stage line: {label}")
    return float(match.group(1))


def stage_call_count(report, label):
    match = re.search(
        rf"-\s+{re.escape(label)}\s+[0-9.]+\s+(?:CPU\s+)?ms\s+\((\d+)\s+calls?",
        report,
    )
    if not match:
        raise AssertionError(f"Missing stage call count: {label}")
    return int(match.group(1))


def require_all(report, required):
    for text in required:
        if text not in report:
            raise AssertionError(f"Missing report text: {text}")


def require_absent(report, forbidden):
    present = [text for text in forbidden if text in report]
    if present:
        raise AssertionError(f"Report kept removed sections/text: {present}")


REPORT_NUMBER = (
    r"[-+]?(?:(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][-+]?[0-9]+)?|(?i:inf|nan))"
)


def finite_nonnegative(value, label):
    parsed = float(value)
    if not math.isfinite(parsed) or parsed < 0.0:
        raise AssertionError(f"{label} must be finite and non-negative, got {value}")
    return parsed


def assert_shader_waits_if_present(report):
    if "Shader Waits:" not in report:
        return
    match = re.search(
        rf"^[ \t]*-[ \t]+waits=(\d+)[ \t]+cpu=({REPORT_NUMBER}) ms[ \t]+"
        rf"frame_share=({REPORT_NUMBER})%[ \t]+queued_shaders=(\d+)[ \t]+"
        rf"queued_textures=(\d+)[ \t]*$",
        report,
        re.MULTILINE,
    )
    if not match:
        raise AssertionError("Shader Waits section does not match its measured-value format")
    if int(match.group(1)) <= 0:
        raise AssertionError("Shader Waits must contain at least one wait")
    finite_nonnegative(match.group(2), "Shader Waits cpu")
    finite_nonnegative(match.group(3), "Shader Waits frame_share")


def assert_material_sync_if_present(report, *, required=False):
    if "Material Sync:" not in report:
        if required:
            raise AssertionError("Missing required Material Sync section")
        return
    match = re.search(
        r"^[ \t]*-[ \t]+Total requests=(\d+)[ \t]+shader_queued=(\d+)[ \t]+"
        r"optimize_queued=(\d+)[ \t]+fallbacks=(\d+)[ \t]+failed=(\d+)[ \t]*$",
        report,
        re.MULTILINE,
    )
    if not match:
        raise AssertionError("Material Sync section does not match its aggregate format")
    if int(match.group(1)) <= 0:
        raise AssertionError("Material Sync Total requests must be positive")


def assert_pass_readback_if_present(report, *, required=False):
    if "Pass Readback:" not in report:
        if required:
            raise AssertionError("Missing required Pass Readback section")
        return
    total_match = re.search(
        rf"^[ \t]*-[ \t]+Total passes=(\d+)[ \t]+pixels=({REPORT_NUMBER})M[ \t]+"
        rf"values=({REPORT_NUMBER})M[ \t]+data=({REPORT_NUMBER})MB[ \t]+"
        rf"cpu=({REPORT_NUMBER}) ms[ \t]*$",
        report,
        re.MULTILINE,
    )
    if not total_match:
        raise AssertionError("Pass Readback section does not match its aggregate format")
    total_passes = int(total_match.group(1))
    if total_passes <= 0:
        raise AssertionError("Pass Readback Total passes must be positive")
    finite_nonnegative(total_match.group(2), "Pass Readback total pixels")
    finite_nonnegative(total_match.group(3), "Pass Readback total values")
    total_data_mb = finite_nonnegative(total_match.group(4), "Pass Readback total data")
    finite_nonnegative(total_match.group(5), "Pass Readback total cpu")
    if total_data_mb <= 0.0:
        raise AssertionError("Pass Readback total data must be positive for this 96px test render")

    detail_matches = re.findall(
        rf"^[ \t]*-[ \t]+(?:RenderPass|AOV|NativePostFX)[ \t]+passes=(\d+)[ \t]+"
        rf"cpu=({REPORT_NUMBER}) ms[ \t]+readback_share=({REPORT_NUMBER})%[ \t]+"
        rf"pixels=({REPORT_NUMBER})M[ \t]+values=({REPORT_NUMBER})M[ \t]+"
        rf"data=({REPORT_NUMBER})MB[ \t]+names=\[.*\][ \t]*$",
        report,
        re.MULTILINE,
    )
    if not detail_matches:
        raise AssertionError("Pass Readback section has no typed readback rows")
    if sum(int(values[0]) for values in detail_matches) != total_passes:
        raise AssertionError("Pass Readback typed pass counts do not match Total passes")
    for values in detail_matches:
        if int(values[0]) <= 0:
            raise AssertionError("Pass Readback typed pass count must be positive")
        for value, label in zip(
            values[1:],
            ("cpu", "readback_share", "pixels", "values", "data"),
        ):
            finite_nonnegative(value, f"Pass Readback detail {label}")


def require_final_render_source(report):
    for label in ("View Layer", "Render View"):
        match = re.search(
            rf"^[ \t]*{re.escape(label)}:[ \t]*(\S(?:.*\S)?)[ \t]*$",
            report,
            re.MULTILINE,
        )
        if not match:
            raise AssertionError(f"Missing or empty final-render source field: {label}")


def assert_viewport_runtime_report():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    VIEWPORT_CHILD_SCRIPT.write_text(textwrap.dedent(VIEWPORT_CHILD_SOURCE), encoding="utf-8")
    command = [
        bpy.app.binary_path,
        "--factory-startup",
        "--enable-event-simulate",
        "--python-exit-code",
        "1",
        "--python",
        str(VIEWPORT_CHILD_SCRIPT),
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
    (OUTPUT_DIR / "viewport_runtime_report_child.log").write_text(combined, encoding="utf-8")
    if result.returncode != 0:
        raise AssertionError(f"Viewport profiler runtime-report child failed:\n{combined[-4000:]}")
    if "VIEWPORT_PERFORMANCE_RUNTIME_REPORT_OK" not in combined:
        raise AssertionError(
            f"Viewport profiler runtime-report child missed success marker:\n{combined[-4000:]}"
        )


def render_shadow_probe_report():
    clear_scene()

    scene = bpy.context.scene
    configure_common_scene(scene, resolution=96, samples=2)
    if hasattr(scene.eevee, "use_gtao"):
        scene.eevee.use_gtao = True

    world = scene.world or bpy.data.worlds.new("World")
    scene.world = world
    world.color = (0.03, 0.035, 0.04)

    floor_mat = make_mat("Floor", (0.55, 0.55, 0.55, 1.0))
    cube_mat = make_mat("Cube", (0.8, 0.25, 0.18, 1.0))

    bpy.ops.mesh.primitive_plane_add(size=8.0, location=(0.0, 0.0, 0.0))
    floor = bpy.context.object
    floor.name = "ProfilerFloor"
    floor.data.materials.append(floor_mat)

    for x in (-1.0, 1.0):
        bpy.ops.mesh.primitive_cube_add(size=1.0, location=(x, 0.0, 0.55))
        cube = bpy.context.object
        cube.name = f"ProfilerCube{x:+.0f}"
        cube.data.materials.append(cube_mat)

    add_light("PointCostHigh", "POINT", (0.0, -2.5, 3.0), energy=700.0)
    add_light("AreaCostHigh", "AREA", (2.5, 1.5, 4.0), rotation=(1.0, 0.0, 2.3), energy=450.0)
    add_light("SunCost", "SUN", (0.0, 0.0, 5.0), rotation=(0.8, 0.0, 0.55), energy=2.5)

    bpy.ops.object.lightprobe_add(
        type="PLANE", location=(0.0, 1.5, 1.2), rotation=(1.5708, 0.0, 0.0)
    )
    bpy.context.object.name = "ProfilerPlanarProbe"

    bpy.ops.object.camera_add(location=(0.0, -6.0, 3.2), rotation=(1.1, 0.0, 0.0))
    scene.camera = bpy.context.object

    bpy.ops.render.render(write_still=False)
    return scene.eevee.performance_profiler_render_report


def make_surface_material():
    material = bpy.data.materials.new("ProfilerSurface")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (0.1, 0.3, 0.8, 1.0)
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_filter_material():
    material = bpy.data.materials.new("ProfilerFilterGLSLPositionAOV")
    material.use_nodes = True
    material.eevee_domain = "FILTER"
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputFilter")
    output.inputs["Alpha"].default_value = 1.0

    _, image_sample = add_pass_input_image_sample(nodes, links)

    glsl = nodes.new("ShaderNodeGLSLFunction")
    glsl.script = make_text_block(
        "profiler_filter_costs_release.glsl",
        "vec4 encode_position(vec4 p){\n"
        "  return vec4(p.x * 0.1 + 0.5, p.z * 0.1 + 0.5, 0.25, 1.0);\n"
        "}\n",
    )
    glsl.function_name = "encode_position"
    refresh_glsl_node(glsl)

    links.new(image_sample.outputs["Color"], glsl.inputs["p"])
    links.new(glsl.outputs["Result"], output.inputs["Color"])
    return material


def render_filter_report():
    clear_scene()

    scene = bpy.context.scene
    configure_common_scene(scene, resolution=96, samples=1)
    clear_filter_graph(scene)

    view_layer = bpy.context.view_layer
    if "ProfilerAOV" not in view_layer.aovs:
        aov = view_layer.aovs.add()
        aov.name = "ProfilerAOV"
        aov.type = "COLOR"

    native_outputs = view_layer.native_postfx_outputs
    while len(native_outputs):
        native_outputs.remove(0)
    native_output = native_outputs.add()
    native_output.name = "ProfilerNativeNormal"
    native_output.source = "NORMAL"
    native_output.use_motion_blur = False
    native_output.use_depth_of_field = False
    if not native_output.is_valid:
        raise AssertionError("ProfilerNativeNormal native post-FX output should be valid")

    bpy.ops.mesh.primitive_plane_add(size=4.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.object
    plane.data.materials.append(make_surface_material())

    bpy.ops.object.camera_add(location=(0.0, -5.0, 2.0), rotation=(1.2, 0.0, 0.0))
    scene.camera = bpy.context.object

    attach_filter_material_to_graph(
        make_filter_material(), stage="BEFORE_COMPOSITE", scene_socket="Position Image"
    )

    bpy.ops.render.render(write_still=False)
    return scene.eevee.performance_profiler_render_report


def assert_shadow_probe_report(report):
    require_final_render_source(report)
    require_absent(
        report,
        [
            "Sync Texture Loads:",
            "Top CPU Stages:",
            "Sync Objects:",
            "Film Outputs:",
            "Filter Costs:",
            "Render Texture Costs:",
            "Hints:",
            "sync_end_share=",
        ],
    )
    require_all(
        report,
        [
            "Timing Domain: CPU wall time",
            "Timing Scope: EEVEE render samples + sync/submission/readback (excludes Instance init and shader setup)",
            "Accounting: Inclusive scopes",
            "Shadow.TilemapSetup",
            "Shadow.TilemapUpdate",
            "Shadow.UpdateFinish",
            "Shadow.Surface",
            "ms/call",
            "ms/sample",
            "Shadow Contexts:",
            "MainView",
            "Shadow Lights:",
            "tilemap_view_share=",
            "sync_dirty_tilemaps=",
            "Probe Costs:",
            "Planar Probes",
            "estimated_work=",
        ],
    )

    values = {
        "Shadow.TilemapSetup": stage_value(report, "Shadow.TilemapSetup"),
        "Shadow.TilemapUpdate": stage_value(report, "Shadow.TilemapUpdate"),
        "Shadow.UpdateFinish": stage_value(report, "Shadow.UpdateFinish"),
        "Shadow.Surface": stage_value(report, "Shadow.Surface"),
    }
    if all(value <= 0.0 for value in values.values()):
        raise AssertionError(f"Shadow sub-stage timings stayed zero: {values}")

    tilemap_update_calls = stage_call_count(report, "Shadow.TilemapUpdate")
    update_finish_calls = stage_call_count(report, "Shadow.UpdateFinish")
    if tilemap_update_calls <= 0 or update_finish_calls <= 0:
        raise AssertionError(
            f"Expected shadow update stages to be called: "
            f"update={tilemap_update_calls}, finish={update_finish_calls}"
        )

    light_lines = [
        line
        for line in report.splitlines()
        if line.strip().startswith("- ") and "tilemap_view_share=" in line
    ]
    if not light_lines:
        raise AssertionError("No Shadow Lights entries found")

    expected_names = {"PointCostHigh", "AreaCostHigh", "SunCost"}
    found_names = {line.strip().split(" type=", 1)[0].removeprefix("- ") for line in light_lines}
    missing = expected_names - found_names
    if missing:
        raise AssertionError(f"Missing expected Shadow Lights entries: {sorted(missing)}")

    assert_shader_waits_if_present(report)
    assert_material_sync_if_present(report)
    assert_pass_readback_if_present(report)


def assert_filter_report(report):
    require_final_render_source(report)
    require_absent(
        report,
        [
            "Sync Texture Loads:",
            "Top CPU Stages:",
            "Sync Objects:",
            "Film Outputs:",
            "Filter Costs:",
            "Render Texture Costs:",
            "Hints:",
            "sync_end_share=",
        ],
    )
    require_all(
        report,
        [
            "Timing Domain: CPU wall time",
            "Timing Scope: EEVEE render samples + sync/submission/readback (excludes Instance init and shader setup)",
            "Accounting: Inclusive scopes",
            "Features:",
            "Filters=1",
            "Filter.BeforeComposite",
            "ReadResult",
        ],
    )
    if stage_call_count(report, "ReadResult") <= 0:
        raise AssertionError("Expected ReadResult stage to run for final render output")
    assert_shader_waits_if_present(report)
    assert_material_sync_if_present(report, required=True)
    assert_pass_readback_if_present(report, required=True)


def main():
    bpy.ops.wm.read_homefile(use_factory_startup=True)

    shadow_report = render_shadow_probe_report()
    print(shadow_report)
    assert_shadow_probe_report(shadow_report)

    filter_report = render_filter_report()
    print(filter_report)
    assert_filter_report(filter_report)

    assert_viewport_runtime_report()

    print("EEVEE_PERFORMANCE_PROFILER_ATTRIBUTION_RELEASE_OK")


if __name__ == "__main__":
    main()
