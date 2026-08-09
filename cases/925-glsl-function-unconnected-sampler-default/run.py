from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "test" / "release"))

import bpy

from release_case_utils import source_repo_path


SOURCE_RELATIVE = "tests/python/npr/test_glsl_function_unconnected_sampler_default.py"
SUCCESS_MARKER = "GLSL_UNCONNECTED_SAMPLER_DEFAULT_OK"
BACKEND_MARKER = "GLSL_UNCONNECTED_SAMPLER_BACKEND="
DUMP_MATERIAL_NAME = "ZZ925UnconnectedSamplerQueries"
OUT_DIR = Path(__file__).resolve().parent / "out"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def tail_text(text, line_count=160):
    return "\n".join(text.splitlines()[-line_count:])


def inspect_shader_dump(output_dir):
    shader_dir = output_dir / "Shaders"
    shader_paths = sorted(shader_dir.glob("*.glsl"))
    require(shader_paths, f"No shader sources were written under {shader_dir}")

    candidates = []
    for path in shader_paths:
        source = path.read_text(encoding="utf-8", errors="replace")
        if all(
            marker in source
            for marker in (
                "fallback_queries",
                "glsl_sampler_fallback2d_",
                "glsl_sampler_fallback3d_",
            )
        ):
            candidates.append((path, source))
    require(candidates, "No shader dump contains both constant-white fallback helpers")

    failures = []
    for path, source in candidates:
        function_signature = re.search(
            r"\b[A-Za-z_][A-Za-z0-9_]*fallback_queries\s*\(\s*float\s+image\s*,\s*float\s+volume\s*\)",
            source,
        )
        helper_2d = re.search(
            r"vec4\s+glsl_sampler_fallback2d_[A-Za-z0-9_]+\s*\(\s*vec2\s+\w+\s*\)\s*\{\s*return\s+vec4\(1\.0\)\s*;\s*\}",
            source,
            re.DOTALL,
        )
        helper_3d = re.search(
            r"vec4\s+glsl_sampler_fallback3d_[A-Za-z0-9_]+\s*\(\s*vec3\s+\w+\s*\)\s*\{\s*return\s+vec4\(1\.0\)\s*;\s*\}",
            source,
            re.DOTALL,
        )
        stale_sampler_parameter = re.search(
            r"\bsampler(?:2D|3D)\s+(?:image|volume|alias_image|alias_volume)\b", source
        )
        stale_sampling_call = re.search(
            r"\b(?:texture|textureLod|textureGrad|textureSize|texelFetch|textureGather)\s*\(\s*(?:image|volume|alias_image|alias_volume)\b",
            source,
        )
        if not function_signature:
            failures.append(f"{path.name}: fallback_queries does not use float dummy inputs")
        elif not helper_2d or not helper_3d:
            failures.append(f"{path.name}: a constant-white helper body is missing")
        elif stale_sampler_parameter:
            failures.append(
                f"{path.name}: fallback still declares a sampler parameter: "
                f"{stale_sampler_parameter.group(0)}"
            )
        elif stale_sampling_call:
            failures.append(
                f"{path.name}: fallback still contains a texture call: {stale_sampling_call.group(0)}"
            )
        else:
            preserved = output_dir / "fallback_codegen.glsl"
            shutil.copyfile(path, preserved)
            return preserved
    raise AssertionError("No shader dump passed fallback codegen checks:\n" + "\n".join(failures))


def run_backend(backend):
    require(backend in {"OPENGL", "VULKAN"}, f"Unsupported backend {backend}")
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    output_dir = (OUT_DIR / backend.lower()).resolve()
    require(output_dir.parent == OUT_DIR.resolve(), f"Invalid output directory {output_dir}")
    if output_dir.exists():
        shutil.rmtree(output_dir)
    (output_dir / "Shaders").mkdir(parents=True)

    source_root = source_repo_path(ROOT, SOURCE_RELATIVE)
    source_path = source_root / SOURCE_RELATIVE
    require(source_path.is_file(), f"Source test does not exist: {source_path}")
    command = [
        bpy.app.binary_path,
        "--background",
        "--factory-startup",
        "--gpu-backend",
        backend.lower(),
        "--debug-gpu",
        "--debug-gpu-shader-source",
        f"*{DUMP_MATERIAL_NAME}*",
        "--python-exit-code",
        "1",
        "--python",
        str(source_path),
    ]
    process = subprocess.Popen(
        command,
        cwd=str(output_dir),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    try:
        output, _unused = process.communicate(timeout=180)
    except subprocess.TimeoutExpired:
        process.kill()
        output, _unused = process.communicate()
        raise AssertionError(
            f"{backend} child timed out; killed exact PID {process.pid}\n{tail_text(output)}"
        )

    log_path = OUT_DIR / f"{backend.lower()}.log"
    log_path.write_text(output, encoding="utf-8")
    print(output, end="", flush=True)
    require(
        process.returncode == 0,
        f"{backend} child failed with {process.returncode}\n{tail_text(output)}",
    )
    require(
        f"{BACKEND_MARKER}{backend}" in output,
        f"{backend} child did not confirm the requested backend",
    )
    require(SUCCESS_MARKER in output, f"{backend} child did not report success")
    preserved_shader = inspect_shader_dump(output_dir)
    print(f"GLSL_UNCONNECTED_SAMPLER_CODEGEN_{backend}={preserved_shader}", flush=True)


def main():
    run_backend("OPENGL")
    run_backend("VULKAN")
    print("GLSL_UNCONNECTED_SAMPLER_RELEASE_OK", flush=True)


if __name__ == "__main__":
    main()
