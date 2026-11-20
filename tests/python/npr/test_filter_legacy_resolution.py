# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import *
path = Path(__file__).resolve().parent / 'data/filter_graph_50254.blend'
bpy.ops.wm.open_mainfile(filepath=str(path))
p = next(n for n in bpy.context.scene.eevee.filter_graph.nodes if n.bl_idname == 'EeveeFilterGraphNodeFilterMaterial')
assert p.resolution_basis == 'STAGE' and p.resolution_scale == .125, (p.resolution_basis, p.resolution_scale)
near(center(render('legacy_filter')), [8, 6, 0], .02)
print('LEGACY_FILTER_PASS', flush=True)
