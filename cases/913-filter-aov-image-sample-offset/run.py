from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "test" / "release"))

from release_case_utils import run_source_test

run_source_test(ROOT, "tests/python/npr/test_filter_aov_image_sample_offset.py")