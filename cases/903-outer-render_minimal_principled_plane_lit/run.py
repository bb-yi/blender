from pathlib import Path
import runpy

ROOT = Path(__file__).resolve().parents[4]
runpy.run_path(str(ROOT / 'test/render_minimal_principled_plane_lit.py'), run_name='__main__')
