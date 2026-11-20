# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check sampler helper failure cleanup after the child allocator has shut down."""

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import OUT, background_child

if '--memory-child' in sys.argv:
    import test_glsl_function_closure_sampler3d as sampler_case

    sampler_case.main()
    # Repeated failures must not accumulate orphan links, while valid compilation still works.
    for _ in range(3):
        sampler_case.test_unsupported_helper_node_fails_readably()
        sampler_case.test_three_dimensional_coordinate_probe()
    print('SAMPLER_MEMORY_CHILD_PASS', flush=True)
else:
    output = background_child(Path(__file__).resolve(), ['--memory-child'], ['--debug-memory'])
    (OUT / 'sampler_failure_memory.log').write_text(output, encoding='utf-8')
    assert 'SAMPLER_MEMORY_CHILD_PASS' in output, output[-4000:]
    assert 'Switching to fully guarded memory allocator.' in output, output[-4000:]
    assert output.count('GLSL_SAMPLER3D_UNSUPPORTED_FALLBACK=') == 4, output[-4000:]
    assert 'Not freed memory blocks' not in output, output[-5000:]
    assert 'negative refcount' not in output, output[-5000:]
    print('SAMPLER_FAILURE_MEMORY_PASS', flush=True)
