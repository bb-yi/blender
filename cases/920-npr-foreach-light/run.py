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
    / "test_npr_foreach_light.py"
)
BACKEND_MARKER = "NPR_FOREACH_LIGHT_BACKEND="
SUCCESS_MARKER = "NPR_FOREACH_LIGHT_OK"
TIMEOUT_SECONDS = 180


ERROR_PATTERNS = (
    ("GPU shader error", re.compile(r"gpu\.shader\s+\|\s+ERROR", re.IGNORECASE)),
    (
        "GPU backend error",
        re.compile(r"gpu\.(?:vulkan|opengl|metal)\s+\|\s+ERROR", re.IGNORECASE),
    ),
    ("missing GPU resource binding", re.compile(r"Missing (?:Texture|Buffer) bind", re.IGNORECASE)),
    ("shader compilation failure", re.compile(r"shader compilation (?:error|failed)", re.IGNORECASE)),
    ("shader syntax diagnostic", re.compile(r"Error:\s+C\d{4}|undefined variable|syntax error", re.IGNORECASE)),
    ("GPU validation failure", re.compile(r"Validation failed|validation error", re.IGNORECASE)),
    ("Vulkan device loss", re.compile(r"VK_ERROR_DEVICE_LOST", re.IGNORECASE)),
    ("native access violation", re.compile(r"EXCEPTION_ACCESS_VIOLATION", re.IGNORECASE)),
    ("Blender crash", re.compile(r"Blender crashed", re.IGNORECASE)),
)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def tail_text(text, line_count=160):
    return "\n".join(text.splitlines()[-line_count:])


def run_child(backend):
    label = backend.lower()
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    log_path = OUT_DIR / f"{label}.log"
    command = [
        bpy.app.binary_path,
        "--background",
        "--factory-startup",
        "--gpu-backend",
        label,
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
            "--expected-backend",
            backend,
            "--output-dir",
            str(OUT_DIR / label),
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
        f"child_backend={backend}\n"
        f"child_pid={process.pid}\n"
        f"child_executable={Path(command[0]).resolve()}\n"
        f"child_command={command_text}\n"
    )
    print(launch_record.rstrip(), flush=True)

    try:
        output, _unused = process.communicate(timeout=TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        process.kill()
        output, _unused = process.communicate()
        diagnostic = (
            f"TIMEOUT after {TIMEOUT_SECONDS}s; killed exact child PID {process.pid}\n"
        )
        log_path.write_text(launch_record + diagnostic + output, encoding="utf-8")
        raise AssertionError(
            f"{backend} child timed out; exact PID {process.pid} was terminated\n"
            f"executable={Path(command[0]).resolve()}\n"
            f"command={command_text}\n"
            f"--- child tail ---\n{tail_text(output)}"
        )

    log_path.write_text(launch_record + output, encoding="utf-8")
    require(
        process.returncode == 0,
        f"{backend} child exited with {process.returncode}\n--- child tail ---\n{tail_text(output)}",
    )
    for marker in (BACKEND_MARKER + backend, SUCCESS_MARKER):
        require(marker in output, f"{backend} child did not emit {marker!r}\n{tail_text(output)}")

    errors = []
    for line in output.splitlines():
        for error_label, pattern in ERROR_PATTERNS:
            if pattern.search(line):
                errors.append(f"{error_label}: {line.strip()}")
                break
    require(not errors, f"{backend} child reported GPU errors:\n" + "\n".join(errors))


def main():
    require(SOURCE_TEST.exists(), f"Source test not found: {SOURCE_TEST}")
    for backend in ("OPENGL", "VULKAN"):
        run_child(backend)
    print("NPR_FOREACH_LIGHT_RELEASE_OK", flush=True)


if __name__ == "__main__":
    main()
