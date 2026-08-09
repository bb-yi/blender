from pathlib import Path
import sys


CASE_DIR = Path(__file__).resolve().parent
ROOT = CASE_DIR.parents[3]
sys.path.insert(0, str(ROOT / "test" / "release"))

from release_case_utils import run_source_test


run_source_test(ROOT, "tests/python/npr/test_scene_time_node_compat.py")
