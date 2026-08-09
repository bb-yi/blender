from pathlib import Path
import runpy

ROOT = Path(__file__).resolve().parents[4]
runpy.run_path(str(ROOT / 'test/verify_npr_displacement_render.py'), run_name='__main__')
