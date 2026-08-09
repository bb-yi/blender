from pathlib import Path
import re
import subprocess
import sys
import textwrap

import bpy


CASE_DIR = Path(__file__).resolve().parent
ROOT = CASE_DIR.parents[3]
OUT_DIR = CASE_DIR / "out"
SOURCE_TEST = (
    ROOT
    / "blender_npr_post"
    / "tests"
    / "python"
    / "npr"
    / "test_goo_screenspace_info_viewport_parity.py"
)
VIEWPORT_SCRIPT = OUT_DIR / "screenspace_info_viewport_child.py"
PIXEL_MARKER = "SCREENSPACE_INFO_VIEWPORT_PARITY_OK"
BACKEND_MARKER = "SCREENSPACE_INFO_BACKEND="
VIEWPORT_MARKER = "SCREENSPACE_INFO_RENDERED_VIEWPORT_OK"


VIEWPORT_SOURCE = r"""
import sys

import bpy
import gpu


def argument_value(name):
    if "--" not in sys.argv:
        raise AssertionError("viewport child arguments are missing")
    arguments = sys.argv[sys.argv.index("--") + 1 :]
    index = arguments.index(name)
    return arguments[index + 1]


def assign_single_material(obj, material):
    obj.data.materials.clear()
    obj.data.materials.append(material)
    obj.active_material_index = 0
    for polygon in obj.data.polygons:
        polygon.material_index = 0

    assert len(obj.data.materials) == 1
    assert obj.active_material == material
    assert all(polygon.material_index == 0 for polygon in obj.data.polygons)


def main():
    use_raytracing = argument_value("--raytracing") == "1"

    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.eevee.use_raytracing = use_raytracing
    if hasattr(scene.eevee, "ray_tracing_method"):
        scene.eevee.ray_tracing_method = "SCREEN"

    bpy.ops.mesh.primitive_cube_add(size=2.0, location=(0.0, 0.0, 0.0))
    cube = bpy.context.active_object
    cube.name = "ScreenSpaceInfoViewportCube"

    material = bpy.data.materials.new("ScreenSpaceInfoViewportMaterial")
    material.use_nodes = True
    material.surface_render_method = "DITHERED"
    material.use_raytrace_refraction = use_raytracing
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    screenspace = nodes.new("ShaderNodeScreenspaceInfo")
    links.new(screenspace.outputs["Scene Color"], output.inputs["Surface"])
    assign_single_material(cube, material)

    view_areas = []
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == "VIEW_3D":
                area.spaces.active.shading.type = "RENDERED"
                area.tag_redraw()
                view_areas.append(area)

    assert view_areas, "factory startup did not create a 3D viewport"
    assert gpu.platform.backend_type_get() == "VULKAN"
    print("SCREENSPACE_INFO_BACKEND=VULKAN", flush=True)
    print(f"SCREENSPACE_INFO_VIEWPORT_RAYTRACING={int(use_raytracing)}", flush=True)

    state = {"ticks": 0}

    def finish_after_rendered_frames():
        state["ticks"] += 1
        bpy.context.view_layer.update()
        for area in view_areas:
            area.tag_redraw()

        if state["ticks"] < 40:
            return 0.1

        print("SCREENSPACE_INFO_RENDERED_VIEWPORT_OK", flush=True)
        bpy.ops.wm.quit_blender()
        return None

    bpy.app.timers.register(finish_after_rendered_frames, first_interval=0.1)


if __name__ == "__main__":
    main()
"""


ERROR_PATTERNS = (
    ("missing GPU binding", re.compile(r"ERROR Missing .* bind at slot", re.IGNORECASE)),
    ("GPU validation failure", re.compile(r"Validation failed", re.IGNORECASE)),
    ("Vulkan device lost", re.compile(r"VK_ERROR_DEVICE_LOST", re.IGNORECASE)),
    ("native access violation", re.compile(r"EXCEPTION_ACCESS_VIOLATION", re.IGNORECASE)),
    ("Blender crash", re.compile(r"Blender crashed", re.IGNORECASE)),
)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def tail_text(text, line_count=100):
    return "\n".join(text.splitlines()[-line_count:])


def write_viewport_script():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    VIEWPORT_SCRIPT.write_text(textwrap.dedent(VIEWPORT_SOURCE).strip() + "\n", encoding="utf-8")


def run_child(label, command, timeout_seconds):
    log_path = OUT_DIR / f"{label}.log"
    command_text = subprocess.list2cmdline([str(value) for value in command])
    process = subprocess.Popen(
        command,
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    launch_record = (
        f"child_label={label}\n"
        f"child_pid={process.pid}\n"
        f"child_executable={Path(command[0]).resolve()}\n"
        f"child_command={command_text}\n"
    )
    print(launch_record.rstrip(), flush=True)

    try:
        output, _unused = process.communicate(timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        process.kill()
        output, _unused = process.communicate()
        diagnostic = (
            f"TIMEOUT after {timeout_seconds}s; killed exact child PID {process.pid}\n"
        )
        log_path.write_text(launch_record + diagnostic + output, encoding="utf-8")
        raise AssertionError(
            f"{label} timed out; exact child PID {process.pid} was terminated\n"
            f"executable={Path(command[0]).resolve()}\n"
            f"command={command_text}\n"
            f"--- child tail ---\n{tail_text(output)}"
        )

    log_path.write_text(launch_record + output, encoding="utf-8")
    return process.returncode, output, log_path


def find_gpu_errors(output):
    matches = []
    for line in output.splitlines():
        for label, pattern in ERROR_PATTERNS:
            if pattern.search(line):
                matches.append(f"{label}: {line.strip()}")
                break
    return matches


def validate_child(label, returncode, output, log_path, markers):
    require(
        returncode == 0,
        f"{label} exited with {returncode}\n--- child tail ---\n{tail_text(output)}",
    )
    for marker in markers:
        require(
            marker in output,
            f"{label} did not emit {marker!r}\n--- child tail ---\n{tail_text(output)}",
        )

    errors = find_gpu_errors(output)
    require(
        not errors,
        f"{label} reported GPU errors:\n" + "\n".join(errors) + f"\nchild_log={log_path}",
    )


def run_pixel_probe(backend):
    command = [
        bpy.app.binary_path,
        "--background",
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
            str(SOURCE_TEST),
        ]
    )
    label = f"pixel_{backend.lower()}"
    returncode, output, log_path = run_child(label, command, 120)
    validate_child(
        label,
        returncode,
        output,
        log_path,
        (BACKEND_MARKER + backend, PIXEL_MARKER),
    )


def run_viewport_probe(use_raytracing):
    mode = "raytracing_on" if use_raytracing else "raytracing_off"
    label = f"viewport_vulkan_{mode}"
    command = [
        bpy.app.binary_path,
        "--factory-startup",
        "--gpu-backend",
        "vulkan",
        "--debug-gpu",
        "--python-exit-code",
        "1",
        "--python",
        str(VIEWPORT_SCRIPT),
        "--",
        "--raytracing",
        "1" if use_raytracing else "0",
    ]
    returncode, output, log_path = run_child(label, command, 90)
    validate_child(
        label,
        returncode,
        output,
        log_path,
        (
            BACKEND_MARKER + "VULKAN",
            f"SCREENSPACE_INFO_VIEWPORT_RAYTRACING={int(use_raytracing)}",
            VIEWPORT_MARKER,
        ),
    )


def main():
    require(SOURCE_TEST.exists(), f"source test not found: {SOURCE_TEST}")
    write_viewport_script()

    run_pixel_probe("OPENGL")
    run_pixel_probe("VULKAN")
    run_viewport_probe(False)
    run_viewport_probe(True)

    print("SCREENSPACE_INFO_RELEASE_PARITY_OK", flush=True)


if __name__ == "__main__":
    main()
