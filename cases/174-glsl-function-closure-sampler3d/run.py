import importlib.util
from pathlib import Path
import subprocess
import sys
import traceback

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "test" / "release"))

import bpy
import gpu

from release_case_utils import run_source_test, source_repo_path


SOURCE_RELATIVE = "tests/python/npr/test_glsl_function_closure_sampler3d.py"
SUCCESS_MARKER = "GLSL_CLOSURE_SAMPLER_UNIFORM_VIEWPORT_OK"


def child_mode():
    return "--child" in sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else False


def child_argument(name):
    arguments = sys.argv[sys.argv.index("--") + 1 :]
    return arguments[arguments.index(name) + 1]


def load_source_module():
    source_path = source_repo_path(ROOT, SOURCE_RELATIVE) / SOURCE_RELATIVE
    spec = importlib.util.spec_from_file_location("closure_sampler_source_test", source_path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"Could not load source test: {source_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run_viewport_child():
    expected_backend = child_argument("--expected-backend")
    source = load_source_module()
    source.configure_scene()
    source.clear_scene()
    material = source.make_closure_sampler_alpha_material("numeric")
    plane = source.build_plane(material)

    view_areas = []
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == "VIEW_3D":
                area.spaces.active.shading.type = "RENDERED"
                area.tag_redraw()
                view_areas.append(area)

    if not view_areas:
        raise AssertionError("Factory startup did not create a 3D viewport")
    actual_backend = gpu.platform.backend_type_get()
    if actual_backend != expected_backend:
        raise AssertionError(f"Expected {expected_backend}, got {actual_backend}")

    state = {"phase": "warmup", "ticks": 0, "stable_ticks": 0, "timestamp": 0}

    def finish_failure(message):
        print(f"GLSL_CLOSURE_SAMPLER_UNIFORM_VIEWPORT_FAIL={message}", flush=True)
        bpy.ops.wm.quit_blender()
        return None

    def evaluated_compile_state():
        depsgraph = bpy.context.evaluated_depsgraph_get()
        evaluated_material = plane.evaluated_get(depsgraph).active_material
        return (
            evaluated_material.shader_compile_status,
            evaluated_material.shader_compile_timestamp,
        )

    def tick():
        try:
            state["ticks"] += 1
            bpy.context.view_layer.update()
            for area in view_areas:
                area.tag_redraw()

            status, timestamp = evaluated_compile_state()
            compiler_pending = bpy.app.is_job_running("SHADER_COMPILATION")
            stable = status == "COMPILED" and timestamp > 0 and not compiler_pending

            if state["phase"] == "warmup":
                state["stable_ticks"] = state["stable_ticks"] + 1 if stable else 0
                if state["stable_ticks"] >= 8:
                    state["timestamp"] = timestamp
                    output_2d = material.node_tree.nodes["Sampler 2D Output"]
                    output_3d = material.node_tree.nodes["Sampler 3D Output"]
                    output_2d.inputs["Color"].default_value = (0.25, 0.4, 0.5, 0.6)
                    output_2d.inputs["Alpha"].default_value = 0.5
                    output_3d.inputs["Color"].default_value = (0.1, 0.3, 0.6, 0.4)
                    output_3d.inputs["Alpha"].default_value = 0.25
                    material.node_tree.update_tag()
                    bpy.context.view_layer.update()
                    state["phase"] = "updated"
                    state["ticks"] = 0
                    state["stable_ticks"] = 0
                    print(
                        "GLSL_CLOSURE_SAMPLER_UNIFORM_VIEWPORT_WARMUP="
                        f"{expected_backend},timestamp={timestamp}",
                        flush=True,
                    )
            else:
                if timestamp > state["timestamp"]:
                    return finish_failure(
                        f"{expected_backend},timestamp={state['timestamp']}->{timestamp}"
                    )
                same_pass = stable and timestamp == state["timestamp"]
                state["stable_ticks"] = state["stable_ticks"] + 1 if same_pass else 0
                if state["stable_ticks"] >= 12:
                    print(
                        f"{SUCCESS_MARKER}={expected_backend},"
                        f"timestamp={state['timestamp']}->{timestamp}",
                        flush=True,
                    )
                    bpy.ops.wm.quit_blender()
                    return None

            if state["ticks"] > 240:
                return finish_failure(
                    f"{expected_backend},timeout,status={status},timestamp={timestamp},"
                    f"pending={compiler_pending}"
                )
            return 0.1
        except Exception:
            traceback.print_exc()
            return finish_failure(f"{expected_backend},exception")

    bpy.app.timers.register(tick, first_interval=0.1)


def run_child_backend(backend):
    command = [
        bpy.app.binary_path,
        "--factory-startup",
        "--gpu-backend",
        backend.lower(),
    ]
    if backend == "VULKAN":
        command.append("--debug-gpu")
    command.extend(
        [
            "--python-exit-code",
            "1",
            "--python",
            str(Path(__file__).resolve()),
            "--",
            "--child",
            "--expected-backend",
            backend,
        ]
    )
    process = subprocess.Popen(
        command,
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    try:
        output, _unused = process.communicate(timeout=90)
    except subprocess.TimeoutExpired:
        process.kill()
        output, _unused = process.communicate()
        raise AssertionError(
            f"{backend} viewport child timed out; killed exact PID {process.pid}\n{output[-8000:]}"
        )
    print(output, end="", flush=True)
    if process.returncode != 0 or f"{SUCCESS_MARKER}={backend}" not in output:
        raise AssertionError(
            f"{backend} viewport uniform probe failed with {process.returncode}\n{output[-8000:]}"
        )


def main():
    if child_mode():
        run_viewport_child()
        return
    run_source_test(ROOT, SOURCE_RELATIVE)
    run_child_backend("OPENGL")
    run_child_backend("VULKAN")
    print("GLSL_CLOSURE_SAMPLER_RELEASE_OK", flush=True)


if __name__ == "__main__":
    main()
