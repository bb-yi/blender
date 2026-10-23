# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Expression error readback, compiler diagnostics and edit recovery."""
import sys
from pathlib import Path

import bpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import scene, render, center, near

sc, material, emission = scene()
tree = material.node_tree
node = tree.nodes.new('ShaderNodeScriptExpression')
tree.links.new(node.outputs[0], emission.inputs['Color'])
node.expression = 'sin('
assert 'balanced' in node.error_message, node.error_message
near(center(render('syntax_fallback')), [0, 0, 0])
node.expression = '0.2'
assert node.error_message == '', node.error_message
near(center(render('syntax_recovery')), [.2] * 3)

node.expression = 'missing_expression_variable + 1.0'
near(center(render('compiler_error')), [1, 0, 1])
assert 'compilation failed' in node.error_message, node.error_message
assert 'missing_expression_variable' in node.error_message, node.error_message
print('COMPILER_DIAGNOSTIC:', node.error_message, flush=True)
node.expression = '0.3'
assert node.error_message == '', node.error_message
near(center(render('compiler_recovery')), [.3] * 3)
assert node.error_message == '', node.error_message
print('SCRIPT_EXPRESSION_ERRORS_OK', flush=True)
