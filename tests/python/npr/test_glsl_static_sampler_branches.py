# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_compat_test_utils import *
s, m, e = scene()
# The inactive helper has an intentionally incompatible sampler dimensionality. It must not
# enter the node's sampler-call analysis or reach the backend after static specialization.
source = '''#define ALGORITHM 0
vec3 alternate(sampler3D volume) { return texture(volume,vec3(.5)).rgb; }
vec3 probe(sampler2D image) {
#if ALGORITHM == 1
  return alternate(image);
#else
  return vec3(.2,.4,.6);
#endif
}
'''
f = glsl(m.node_tree, source, 'probe')
m.node_tree.links.new(f.outputs[0], e.inputs['Color'])
near(center(render('inactive_sampler')), [.2, .4, .6])
assert f.parse_status == 'READY', (f.name, f.parse_status)
active = glsl(
    m.node_tree,
    source.replace(
        'ALGORITHM 0',
        'ALGORITHM 1').replace(
            'return alternate(image);',
            'return vec3(.6,.3,.2);'),
    'probe')
m.node_tree.links.new(active.outputs[0], e.inputs['Color'])
near(center(render('active_static_branch')), [.6, .3, .2])
# Unknown stage macros must retain backend preprocessing and must not be treated as false.
stage = glsl(m.node_tree, '''vec3 stage_probe() {
#ifdef GPU_FRAGMENT_SHADER
  return vec3(.3,.5,.7);
#else
  return vec3(0);
#endif
}''', 'stage_probe')
m.node_tree.links.new(stage.outputs[0], e.inputs['Color'])
near(center(render('unknown_backend_macro')), [.3, .5, .7])
chain = glsl(m.node_tree, '''vec3 chain_probe() {
#if 2 == 1 == 0
  return vec3(.25);
#else
  return vec3(.75);
#endif
}''', 'chain_probe')
m.node_tree.links.new(chain.outputs[0], e.inputs['Color'])
near(center(render('backend_comparison_associativity')), [.25] * 3)
print('STATIC_BRANCH_PASS', flush=True)

# Wide decimal literals must retain the GPU preprocessor's integer semantics.
for condition in ['4294967295 > 0', '0xffffffff == -1', '-1 < 1u']:
    values = []
    for expression in [condition, 'defined(GPU_FRAGMENT_SHADER) && (' + condition + ')']:
        text = 'vec3 integer_probe() {\n#if ' + expression + '\nreturn vec3(.2);\n#else\nreturn vec3(.8);\n#endif\n}'
        node = glsl(m.node_tree, text, 'integer_probe')
        m.node_tree.links.new(node.outputs[0], e.inputs['Color'])
        values.append(center(render('integer_semantics')))
    near(values[0], values[1])
print('STATIC_INTEGER_SEMANTICS_PASS', flush=True)
alias_source = '''#define BASE 1
#define ALIAS BASE
#undef BASE
#define BASE 0
vec3 alias_probe() {
#if ALIAS
 return vec3(.8);
#else
 return vec3(.2);
#endif
}'''
alias = glsl(m.node_tree, alias_source, 'alias_probe')
m.node_tree.links.new(alias.outputs[0], e.inputs['Color'])
near(center(render('macro_alias_redefinition')), [.2] * 3)
print('STATIC_ALIAS_SEMANTICS_PASS', flush=True)
