from pathlib import Path
import re
import subprocess

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
    / "test_eevee_native_camera_fx_outputs.py"
)
SCENARIOS = ("accumulation", "depth_of_field", "motion_blur")
BACKEND_MARKER = "NATIVE_CAMERA_FX_BACKEND="
SCENARIO_MARKER = "NATIVE_CAMERA_FX_SCENARIO="
SUCCESS_MARKER = "NATIVE_CAMERA_FX_OK"


ERROR_PATTERNS = (
    ("missing GPU binding", re.compile(r"ERROR Missing .* bind at slot", re.IGNORECASE)),
    ("GPU validation failure", re.compile(r"Validation failed|validation error", re.IGNORECASE)),
    ("Vulkan device lost", re.compile(r"VK_ERROR_DEVICE_LOST", re.IGNORECASE)),
    ("native access violation", re.compile(r"EXCEPTION_ACCESS_VIOLATION", re.IGNORECASE)),
    ("Blender crash", re.compile(r"Blender crashed", re.IGNORECASE)),
)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def tail_text(text, line_count=120):
    return "\n".join(text.splitlines()[-line_count:])


def run_child(backend, scenario):
    label = f"{backend.lower()}_{scenario}"
    child_output_dir = OUT_DIR / backend.lower() / scenario
    child_output_dir.mkdir(parents=True, exist_ok=True)
    log_path = OUT_DIR / f"{label}.log"
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
            "--",
            "--scenario",
            scenario,
            "--output-dir",
            str(child_output_dir),
        ]
    )

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
        output, _unused = process.communicate(timeout=120)
    except subprocess.TimeoutExpired:
        process.kill()
        output, _unused = process.communicate()
        diagnostic = f"TIMEOUT after 120s; killed exact child PID {process.pid}\n"
        log_path.write_text(launch_record + diagnostic + output, encoding="utf-8")
        raise AssertionError(
            f"{label} timed out; exact child PID {process.pid} was terminated\n"
            f"executable={Path(command[0]).resolve()}\n"
            f"command={command_text}\n"
            f"--- child tail ---\n{tail_text(output)}"
        )

    log_path.write_text(launch_record + output, encoding="utf-8")
    require(
        process.returncode == 0,
        f"{label} exited with {process.returncode}\n--- child tail ---\n{tail_text(output)}",
    )
    for marker in (BACKEND_MARKER + backend, SCENARIO_MARKER + scenario, SUCCESS_MARKER):
        require(marker in output, f"{label} did not emit {marker!r}\n{tail_text(output)}")

    errors = []
    for line in output.splitlines():
        for error_label, pattern in ERROR_PATTERNS:
            if pattern.search(line):
                errors.append(f"{error_label}: {line.strip()}")
                break
    require(not errors, f"{label} reported GPU errors:\n" + "\n".join(errors))


def main():
    require(SOURCE_TEST.exists(), f"Source test not found: {SOURCE_TEST}")
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    for backend in ("OPENGL", "VULKAN"):
        for scenario in SCENARIOS:
            run_child(backend, scenario)

    print("NATIVE_CAMERA_FX_RELEASE_OK", flush=True)


if __name__ == "__main__":
    main()
