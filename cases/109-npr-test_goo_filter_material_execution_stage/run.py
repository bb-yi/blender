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
    / "test_goo_filter_material_execution_stage.py"
)
DEFAULT_CHILD_LOG = OUT_DIR / "filter_stage_multistage_default.log"
VULKAN_CHILD_LOG = OUT_DIR / "filter_stage_multistage_vulkan.log"
DONE_MARKER = "__FILTER_STAGE_MULTISTAGE_DONE__"
BACKEND_MARKER = "__FILTER_STAGE_BACKEND__="

ERROR_PATTERNS = (
    re.compile(r"\bERROR Feedback loop\b", re.IGNORECASE),
    re.compile(r"Validation failed.*feedback", re.IGNORECASE),
)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run_child_blender(extra_args, child_log):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    command = [
        bpy.app.binary_path,
        "--background",
        "--factory-startup",
        *extra_args,
        "--python-exit-code",
        "1",
        "--python",
        str(SOURCE_TEST),
    ]
    result = subprocess.run(
        command,
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=180,
    )
    child_log.write_text(result.stdout, encoding="utf-8")
    return result


def tail_text(text, line_count=80):
    return "\n".join(text.splitlines()[-line_count:])


def validate_probe(result, child_log, expected_backend):
    require(
        result.returncode == 0,
        f"child Blender failed with exit {result.returncode}"
        f"\n--- child tail ---\n{tail_text(result.stdout)}",
    )
    require(
        DONE_MARKER in result.stdout,
        f"child Blender did not finish the multi-stage probe"
        f"\n--- child tail ---\n{tail_text(result.stdout)}",
    )
    require(
        BACKEND_MARKER + expected_backend in result.stdout,
        f"child Blender did not use the {expected_backend} backend"
        f"\n--- child tail ---\n{tail_text(result.stdout)}",
    )

    feedback_errors = [
        line.strip()
        for line in result.stdout.splitlines()
        if any(pattern.search(line) for pattern in ERROR_PATTERNS)
    ]
    require(
        not feedback_errors,
        "filter stage feedback loop detected:\n"
        + "\n".join(feedback_errors)
        + f"\n--- child log ---\n{child_log}",
    )


def main():
    default_result = run_child_blender(["--debug-gpu"], DEFAULT_CHILD_LOG)
    print(f"filter_stage_multistage default_log={DEFAULT_CHILD_LOG}", flush=True)
    validate_probe(default_result, DEFAULT_CHILD_LOG, "OPENGL")

    vulkan_result = run_child_blender(
        ["--gpu-backend", "vulkan", "--debug-gpu"], VULKAN_CHILD_LOG
    )
    print(f"filter_stage_multistage vulkan_log={VULKAN_CHILD_LOG}", flush=True)
    validate_probe(vulkan_result, VULKAN_CHILD_LOG, "VULKAN")

    print("FILTER_STAGE_MULTISTAGE_RELEASE_OK", flush=True)


if __name__ == "__main__":
    main()
