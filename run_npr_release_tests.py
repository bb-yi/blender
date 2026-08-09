#!/usr/bin/env python3
"""Cross-platform NPR release-test runner.

Canonical location (this workspace): test/release/
Fork users get the same tree from the orphan branch npr-release-tests
(published from this folder — not a second copy inside blender_npr_post).

Usage:
  python run_npr_release_tests.py --blender /path/to/blender
  python run_npr_release_tests.py --blender blender.exe --name 926
  python run_npr_release_tests.py --list
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Iterable


SUITE_DIR = Path(__file__).resolve().parent
DEFAULT_MANIFEST = SUITE_DIR / "release_tests.json"


@dataclass
class TestCase:
    name: str
    type: str
    path: Path | None = None
    expr: str | None = None
    case_dir: Path | None = None
    purpose: str = ""
    pass_criteria: str = ""


def _load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def _workspace_root() -> Path:
    """Parent of test/ when suite lives at <ws>/test/release; else suite dir itself (orphan branch)."""
    if SUITE_DIR.name == "release" and SUITE_DIR.parent.name == "test":
        return SUITE_DIR.parents[1]
    return SUITE_DIR


def _case_glob_root() -> Path:
    """Directory used to resolve manifest patterns."""
    # Maintainer workspace: patterns are test/release/cases/*
    # Orphan branch root: patterns rewritten to cases/*
    if (SUITE_DIR / "cases").is_dir() and not (SUITE_DIR / "test").is_dir():
        return SUITE_DIR
    return _workspace_root()


def _expand_case_glob(root: Path, pattern: str) -> list[Path]:
    full = Path(pattern)
    if not full.is_absolute():
        full = root / pattern
    if full.name == "*":
        parent = full.parent
        if not parent.is_dir():
            raise FileNotFoundError(f"Case glob parent missing: {parent}")
        return sorted([p for p in parent.iterdir() if p.is_dir()], key=lambda p: p.name)
    return sorted([p for p in full.parent.glob(full.name) if p.is_dir()], key=lambda p: p.name)


def _load_case(case_dir: Path) -> TestCase:
    case_json = case_dir / "case.json"
    readme = case_dir / "README.md"
    if not case_json.is_file():
        raise FileNotFoundError(f"Missing case.json: {case_dir}")
    if not readme.is_file():
        raise FileNotFoundError(f"Missing README.md: {case_dir}")

    data = _load_json(case_json)
    name = str(data.get("name") or "").strip()
    ctype = str(data.get("type") or "").strip()
    if not name or not ctype:
        raise ValueError(f"case.json missing name/type: {case_json}")

    purpose = str(data.get("purpose") or "")
    pass_criteria = str(data.get("passCriteria") or data.get("pass_criteria") or "")

    if ctype == "python-expr":
        return TestCase(
            name=name,
            type="python-expr",
            expr=str(data.get("expr") or ""),
            case_dir=case_dir,
            purpose=purpose,
            pass_criteria=pass_criteria,
        )
    if ctype == "blender-python":
        script = data.get("script") or data.get("path") or "run.py"
        path = Path(str(script))
        if not path.is_absolute():
            path = case_dir / path
        if not path.is_file():
            raise FileNotFoundError(f"Missing case script: {path}")
        return TestCase(
            name=name,
            type="blender-python",
            path=path.resolve(),
            case_dir=case_dir,
            purpose=purpose,
            pass_criteria=pass_criteria,
        )
    raise ValueError(f"Unsupported case type {ctype!r} in {case_json}")


def _normalize_patterns(patterns: list[str]) -> list[str]:
    """Map maintainer patterns onto orphan-branch layout when needed."""
    out = []
    for pattern in patterns:
        p = pattern.replace("\\", "/")
        if (SUITE_DIR / "cases").is_dir() and p.startswith("test/release/"):
            # orphan branch: suite root == release tree
            p = p[len("test/release/") :]
        out.append(p)
    return out


def discover_tests(manifest_path: Path, suite: str, name_filter: str = "") -> list[TestCase]:
    root = _case_glob_root()
    manifest = _load_json(manifest_path)
    suite_name = suite or manifest.get("defaultSuite") or "release"
    entries = manifest.get("suites", {}).get(suite_name)
    if entries is None:
        raise KeyError(f"Suite {suite_name!r} not defined in {manifest_path}")

    tests: list[TestCase] = []
    for entry in entries:
        etype = entry.get("type")
        if etype == "case-folder-glob":
            for pattern in _normalize_patterns(list(entry.get("patterns") or [])):
                for case_dir in _expand_case_glob(root, pattern):
                    tests.append(_load_case(case_dir))
        elif etype == "case-folder":
            case_dir = Path(entry["path"])
            if not case_dir.is_absolute():
                case_dir = root / case_dir
            tests.append(_load_case(case_dir))
        elif etype == "python-expr":
            tests.append(
                TestCase(name=str(entry["name"]), type="python-expr", expr=str(entry.get("expr") or ""))
            )
        elif etype == "blender-python":
            path = Path(entry["path"])
            if not path.is_absolute():
                path = root / path
            tests.append(TestCase(name=str(entry["name"]), type="blender-python", path=path))
        else:
            raise ValueError(f"Unsupported manifest entry type: {etype!r}")

    if name_filter:
        needle = name_filter.lower()
        tests = [
            t
            for t in tests
            if needle in t.name.lower()
            or (t.case_dir and needle in t.case_dir.name.lower())
            or (t.path and needle in str(t.path).lower())
        ]
        if not tests:
            raise RuntimeError(f"No release tests matched filter: {name_filter}")
    return tests


def _safe_name(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", name).strip("_") or "test"


def run_blender(
    blender: Path,
    *,
    script: Path | None = None,
    expr: str | None = None,
    extra_args: Iterable[str] = (),
    timeout: float | None = None,
) -> tuple[int, str]:
    cmd = [str(blender), "--background", "--factory-startup", "--python-exit-code", "1"]
    cmd.extend(extra_args)
    if expr is not None:
        cmd.extend(["--python-expr", expr])
    elif script is not None:
        cmd.extend(["--python", str(script)])
    else:
        raise ValueError("script or expr required")

    env = os.environ.copy()
    # Help wrapper cases find blender_npr_post under the packaging workspace.
    ws = _workspace_root()
    env.setdefault("NPR_RELEASE_SOURCE_DIR", str(ws / "blender_npr_post") if (ws / "blender_npr_post").is_dir() else str(ws))

    proc = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=env,
        timeout=timeout,
    )
    out = (proc.stdout or "") + (proc.stderr or "")
    return proc.returncode, out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run NPR release cases from test/release (canonical).")
    parser.add_argument(
        "--blender",
        default=os.environ.get("NPR_RELEASE_BLENDER", ""),
        help="Path to blender executable (or set NPR_RELEASE_BLENDER).",
    )
    parser.add_argument("--manifest", default=str(DEFAULT_MANIFEST))
    parser.add_argument("--suite", default="")
    parser.add_argument("--name", default="", help="Substring filter on case name/path")
    parser.add_argument("--log-dir", default=str(SUITE_DIR / "logs"))
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--continue-on-failure", action="store_true")
    parser.add_argument("--timeout", type=float, default=0.0, help="Per-case timeout seconds (0=none)")
    args = parser.parse_args(argv)

    manifest_path = Path(args.manifest).resolve()
    tests = discover_tests(manifest_path, args.suite, args.name)

    if args.list:
        for t in tests:
            loc = t.case_dir or t.path or ""
            print(f"{t.name}\t{loc}")
        print(f"TOTAL {len(tests)}")
        return 0

    if not args.blender:
        print("ERROR: provide --blender path/to/blender or set NPR_RELEASE_BLENDER", file=sys.stderr)
        return 2
    blender = Path(args.blender).expanduser().resolve()
    if not blender.is_file():
        print(f"ERROR: Blender executable not found: {blender}", file=sys.stderr)
        return 2

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    run_log_dir = Path(args.log_dir).expanduser().resolve() / stamp
    run_log_dir.mkdir(parents=True, exist_ok=True)

    print(f"[INFO] Blender  : {blender}")
    print(f"[INFO] Manifest : {manifest_path}")
    print(f"[INFO] SuiteDir : {SUITE_DIR}")
    print(f"[INFO] Tests    : {len(tests)}")
    print(f"[INFO] Logs     : {run_log_dir}")

    results = []
    failed = 0
    t0 = time.time()
    timeout = args.timeout if args.timeout > 0 else None

    for index, test in enumerate(tests, 1):
        print(f"[{index}/{len(tests)}] {test.name}")
        log_path = run_log_dir / f"{index:03d}_{_safe_name(test.name)}.log"
        try:
            if test.type == "python-expr":
                code, out = run_blender(blender, expr=test.expr or "", timeout=timeout)
            elif test.type == "blender-python":
                code, out = run_blender(blender, script=test.path, timeout=timeout)
            else:
                raise ValueError(f"Unsupported test type: {test.type}")
        except subprocess.TimeoutExpired as exc:
            code = 124
            out = (exc.stdout or "") + (exc.stderr or "") + f"\nTIMEOUT after {timeout}s\n"

        log_path.write_text(out, encoding="utf-8")
        has_traceback = "Traceback (most recent call last)" in out
        ok = code == 0 and not has_traceback
        status = "PASS" if ok else "FAIL"
        if not ok:
            failed += 1
        print(f"  {status} exit={code} log={log_path.name}")
        results.append(
            {
                "name": test.name,
                "status": status,
                "exitCode": code,
                "log": str(log_path),
                "purpose": test.purpose,
                "passCriteria": test.pass_criteria,
            }
        )
        if not ok and not args.continue_on_failure:
            break

    summary = {
        "blender": str(blender),
        "manifest": str(manifest_path),
        "suiteDir": str(SUITE_DIR),
        "total": len(tests),
        "ran": len(results),
        "failed": failed,
        "elapsedSec": round(time.time() - t0, 3),
        "results": results,
    }
    summary_path = run_log_dir / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"[INFO] Summary  : {summary_path}")
    print(f"[INFO] Failed   : {failed}/{len(results)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
