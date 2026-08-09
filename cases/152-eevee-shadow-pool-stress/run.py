from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

import bpy


CASE_DIR = Path(__file__).resolve().parent
ASSET_PATH = CASE_DIR / "assets" / "shadow_pool_stress.blend"
OUT_DIR = CASE_DIR / "out"

POOL_FULL_RE = re.compile(r"Shadow buffer full|阴影缓冲满|阴影缓冲区满", re.IGNORECASE)
ALLOC_WARNING_RE = re.compile(
    r"Could not allocate shadow pool|Could not allocate shadow atlas|无法分配阴影|阴影图集",
    re.IGNORECASE,
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def quote_python_string(value: str) -> str:
    return repr(value)


def run_shadow_pool_render(size_mb: str) -> dict[str, object]:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    log_path = OUT_DIR / f"shadow_pool_{size_mb}.log"

    expr = (
        "import bpy; "
        "s=bpy.context.scene; "
        "s.render.engine='BLENDER_EEVEE'; "
        f"s.eevee.shadow_pool_size={quote_python_string(size_mb)}; "
        f"print('__SHADOW_POOL_TEST_START__ size={size_mb} lights=%d' % "
        "sum(1 for o in bpy.data.objects if o.type == 'LIGHT')); "
        "bpy.ops.render.render(write_still=False); "
        f"print('__SHADOW_POOL_TEST_DONE__ size={size_mb}')"
    )

    command = [
        bpy.app.binary_path,
        "--background",
        "--factory-startup",
        str(ASSET_PATH),
        "--python-exit-code",
        "1",
        "--python-expr",
        expr,
    ]
    env = os.environ.copy()
    env.setdefault("BLENDER_USER_CONFIG", str(OUT_DIR / "user_config"))
    env.setdefault("BLENDER_USER_SCRIPTS", str(OUT_DIR / "user_scripts"))
    env.setdefault("BLENDER_USER_DATAFILES", str(OUT_DIR / "user_datafiles"))

    result = subprocess.run(
        command,
        cwd=str(CASE_DIR),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    log_path.write_text(result.stdout, encoding="utf-8")
    return {
        "size_mb": size_mb,
        "returncode": result.returncode,
        "pool_full": bool(POOL_FULL_RE.search(result.stdout)),
        "allocation_warning": bool(ALLOC_WARNING_RE.search(result.stdout)),
        "done": f"__SHADOW_POOL_TEST_DONE__ size={size_mb}" in result.stdout,
        "log_path": log_path,
    }


def main() -> None:
    require(ASSET_PATH.exists(), f"Missing test asset: {ASSET_PATH}")

    results = [run_shadow_pool_render(size) for size in ("2048", "4096", "8192")]
    for result in results:
        print(
            "SHADOW_POOL_RESULT "
            f"size={result['size_mb']} "
            f"returncode={result['returncode']} "
            f"pool_full={result['pool_full']} "
            f"allocation_warning={result['allocation_warning']} "
            f"done={result['done']} "
            f"log={result['log_path']}"
        )

    by_size = {result["size_mb"]: result for result in results}

    control = by_size["2048"]
    require(control["done"], f"2048 MB control render did not finish: {control['log_path']}")
    require(control["pool_full"], "2048 MB control render unexpectedly fit without warning")

    for size in ("4096", "8192"):
        result = by_size[size]
        require(result["returncode"] == 0, f"{size} MB render failed: {result['log_path']}")
        require(result["done"], f"{size} MB render did not finish: {result['log_path']}")
        require(not result["pool_full"], f"{size} MB render reported shadow buffer full")
        require(not result["allocation_warning"], f"{size} MB render reported allocation warning")

    print("EEVEE_SHADOW_POOL_STRESS_OK=1")


if __name__ == "__main__":
    main()
