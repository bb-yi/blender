# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import json
import os
from pathlib import Path
import sys

import bpy
import numpy as np

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_shadow_precision_penumbra import fixture, render_mask


def measure(out):
    size = 384
    scene = fixture(size, radius=0.0, resolution_limit=0.006, classify=False)
    scene.eevee.shadow_ray_count = 8
    scene.eevee.shadow_step_count = 8
    tree = bpy.data.objects["Receiver"].active_material.node_tree
    node = next(n for n in tree.nodes if n.bl_idname == "ShaderNodeShaderInfo")
    radius = node.inputs["PCF Radius"]
    assert radius.enabled and radius.default_value == -1.0

    def image(label, samples):
        scene.eevee.taa_render_samples = samples
        rgba, _ = render_mask(scene, out / (label + ".exr"))
        return rgba[:, :, 0]

    inherited = image("inherited", 64)
    radius.default_value = 0
    sharp = image("radius0", 64)
    assert np.max(np.abs(sharp - inherited)) < 0.002, "Zero-radius light should match a zero override"

    def edge_width(mask):
        profile = mask[size // 4:3 * size // 4, :].mean(axis=0)
        assert profile.max() > 0.98 and profile.min() < 0.02, "Missing lit or shadowed interior"
        return int(np.count_nonzero((profile > 0.10) & (profile < 0.90)))

    radius.default_value = 8
    middle = image("radius8", 64)
    radius.default_value = 32
    levels = {n: image(f"radius32_s{n}", n) for n in (1, 8, 64, 256)}
    reference = levels[256]
    band = (reference > 0.05) & (reference < 0.95)
    assert band.sum() > size
    errors = {n: float(np.abs(pixels - reference)[band].mean()) for n, pixels in levels.items()}
    widths = [edge_width(pixels) for pixels in (sharp, middle, levels[64])]
    assert widths[0] < widths[1] < widths[2], widths
    assert errors[64] < errors[8] < errors[1], errors

    # Different radii at the top and bottom of one material rule out a material-wide uniform.
    xyz = tree.nodes.new("ShaderNodeSeparateXYZ")
    pos = tree.nodes.new("ShaderNodeNewGeometry")
    tree.links.new(pos.outputs["Position"], xyz.inputs[0])
    step = tree.nodes.new("ShaderNodeMath")
    step.operation = "GREATER_THAN"
    step.inputs[1].default_value = 0.0
    tree.links.new(xyz.outputs["Y"], step.inputs[0])
    scale = tree.nodes.new("ShaderNodeMath")
    scale.operation = "MULTIPLY"
    scale.inputs[1].default_value = 32.0
    tree.links.new(step.outputs[0], scale.inputs[0])
    tree.links.new(scale.outputs[0], radius)
    linked = image("linked_0_32", 64)
    for rows, expected in ((slice(size // 8, size // 3), sharp),
                           (slice(size * 2 // 3, size * 7 // 8), levels[64])):
        assert np.max(np.abs(linked[rows] - expected[rows])) < 0.002

    # Request caster ownership on the same node and confirm that it leaves visibility alone.
    combine = next(n for n in tree.nodes if n.bl_idname == "ShaderNodeCombineXYZ")
    tree.links.new(node.outputs["Cast Shadow"], combine.inputs[2])
    scene.eevee.taa_render_samples = 64
    classified, _ = render_mask(scene, out / "classified.exr")
    assert np.max(np.abs(classified[:, :, 0] - linked)) < 0.002
    assert np.max(np.abs(classified[:, :, 2] - linked)) < 0.003
    result = {"radii": [0, 8, 32], "width_10_to_90_pixels": widths, "mae_vs_256_in_penumbra": errors}
    (out / "report.json").write_text(json.dumps(result, indent=2))
    print("PCF_RADIUS_PASS", result, flush=True)


if __name__ == "__main__":
    out = Path(os.environ["BLENDER_TEST_OUTPUT_DIR"])
    out.mkdir(parents=True, exist_ok=True)
    measure(out)
