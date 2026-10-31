"""Use the established real-window async test with writer depth gating enabled."""
from pathlib import Path
import runpy
import sys

if '--phase' in sys.argv:
    sys.argv[sys.argv.index('--phase') + 1] = 'writer_surface_async'
else:
    if '--' not in sys.argv:
        sys.argv.append('--')
    sys.argv.extend(['--phase', 'writer_surface_async'])
runpy.run_path(str(Path(__file__).with_name('test_stencil_value_async.py')), run_name='__main__')
