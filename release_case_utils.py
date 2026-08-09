import os
import runpy
import sys
from pathlib import Path


def _candidate_paths(root, names):
    paths = []
    for name in names:
        if not name:
            continue
        path = Path(name)
        if not path.is_absolute():
            path = root / path
        paths.append(path)
    return paths


def source_repo_path(root, relative_path=""):
    root = Path(root)
    exe_path = Path(sys.executable).as_posix().lower()
    is_mainfix = "mainfix" in exe_path

    env_override = os.environ.get("NPR_RELEASE_SOURCE_DIR")
    target_override = os.environ.get(
        "NPR_RELEASE_MAINFIX_SOURCE_DIR" if is_mainfix else "NPR_RELEASE_MAIN_SOURCE_DIR"
    )
    default_names = (
        ("blender_npr_post_mainfix", "blender_5_1_port_mainfix")
        if is_mainfix
        else ("blender_npr_post", "blender_5_1_port")
    )
    relative_path = Path(relative_path) if relative_path else None

    candidates = _candidate_paths(root, (env_override, target_override, *default_names))
    for candidate in candidates:
        if not candidate.exists():
            continue
        if relative_path is None or (candidate / relative_path).exists():
            return candidate

    checked = ", ".join(str(path) for path in candidates)
    suffix = f" containing {relative_path}" if relative_path else ""
    raise FileNotFoundError(f"Could not resolve release test source repo{suffix}. Checked: {checked}")


def run_source_test(root, relative_path):
    path = source_repo_path(root, relative_path) / relative_path
    runpy.run_path(str(path), run_name="__main__")
