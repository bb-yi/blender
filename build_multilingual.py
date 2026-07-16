#!/usr/bin/env python3
"""Build the Chinese and English documentation as one deployable site."""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent
SITE_DIR = ROOT / "site"


def run_mkdocs(config: str, destination: Path, label: str) -> None:
    command = [
        sys.executable,
        "-m",
        "mkdocs",
        "build",
        "--strict",
        "--config-file",
        str(ROOT / config),
        "--site-dir",
        str(destination),
    ]
    print(f"[build] {label}: {' '.join(command)}")
    subprocess.run(command, cwd=ROOT, check=True)


def build_multilingual() -> None:
    if SITE_DIR.parent != ROOT or SITE_DIR.name != "site":
        raise RuntimeError(f"Refusing to clean unexpected path: {SITE_DIR}")

    if SITE_DIR.exists():
        shutil.rmtree(SITE_DIR)

    run_mkdocs("mkdocs.yml", SITE_DIR, "Chinese")
    run_mkdocs("mkdocs.en.yml", SITE_DIR / "en", "English")

    required = (
        SITE_DIR / "index.html",
        SITE_DIR / "release.html",
        SITE_DIR / "en" / "index.html",
        SITE_DIR / "en" / "release.html",
    )
    missing = [str(path.relative_to(ROOT)) for path in required if not path.is_file()]
    if missing:
        raise RuntimeError(f"Build completed without required files: {', '.join(missing)}")

    print(f"[build] Bilingual site ready: {SITE_DIR}")


def main() -> int:
    try:
        build_multilingual()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"[build] Failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
