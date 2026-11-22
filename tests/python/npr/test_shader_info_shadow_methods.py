# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import json
import os
from pathlib import Path
import sys

import bpy
import gpu
import numpy as np

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_shadow_precision_penumbra import fixture, render_mask


METHODS = ("HARD", "PCF", "PCSS", "VSM", "VSSM", "MSM", "CHS")


def width(mask):
    H, W = mask.shape
    profile = mask[H // 3:2 * H // 3].mean(axis=0)
    # Only use the shadow boundary, not the edge of the receiving plane.
    profile = profile[W // 8:7 * W // 8]
    assert profile.max() > 0.97 and profile.min() < 0.03, (profile.min(), profile.max())
    return int(np.count_nonzero((profile > .1) & (profile < .9)))


def check_material_textures(scene, tree, node, out):
    combined = next(item for item in tree.nodes if item.bl_idname == "ShaderNodeCombineXYZ")
    texture_count = 12
    summed = None
    for index in range(texture_count):
        image = bpy.data.images.new(f"ShadowTexture{index}", 4, 4, float_buffer=True)
        value = (index + 1) / 32
        image.pixels.foreach_set([value, value, value, 1.0] * 16)
        texture = tree.nodes.new("ShaderNodeTexImage")
        texture.image = image
        if summed is None:
            summed = texture.outputs["Color"]
        else:
            add = tree.nodes.new("ShaderNodeMath")
            add.operation = "ADD"
            tree.links.new(summed, add.inputs[0])
            tree.links.new(texture.outputs["Color"], add.inputs[1])
            summed = add.outputs[0]
    mean = tree.nodes.new("ShaderNodeMath")
    mean.operation = "MULTIPLY"
    mean.inputs[1].default_value = 1.0 / texture_count
    tree.links.new(summed, mean.inputs[0])
    tree.links.new(mean.outputs[0], combined.inputs[1])
    scene.eevee.taa_render_samples = 1
    expected = (texture_count + 1) / 64
    for mode in ("TEMPORAL", "PCF", "VSM"):
        node.shadow_mode = mode
        pixels, _ = render_mask(scene, out / f"{mode}-material-textures.exr")
        error = float(np.max(np.abs(pixels[20:-20, 20:-20, 1] - expected)))
        assert error < .003, (mode, error)


def run(out):
    out.mkdir(parents=True, exist_ok=True)
    gpu.init()
    print("SHADOW_METHODS_BACKEND", gpu.platform.backend_type_get(), gpu.platform.renderer_get(), flush=True)
    scene = fixture(320, mode="PCF", quality=16, classify=True, radius=.20, resolution_limit=.006)
    material = bpy.data.objects["Receiver"].active_material
    node = next(n for n in material.node_tree.nodes if n.bl_idname == "ShaderNodeShaderInfo")
    caster = bpy.data.objects["Blocker"]
    metrics = {}
    wide_pcf = None
    for method in METHODS:
        node.shadow_mode = method
        assert node.inputs[3].enabled == (method not in ("HARD", "CHS"))
        assert node.inputs[4].enabled == (method == "CHS")
        node.inputs[3].default_value = 32
        metrics[method] = {}
        for label, height in (("contact", .04), ("raised", 1.0)):
            caster.location.z = height - 1.0
            scene.eevee.taa_render_samples = 32
            pixels, _ = render_mask(scene, out / f"{method}-{label}.exr")
            assert np.isfinite(pixels).all()
            assert pixels.min() >= -.002 and pixels.max() <= 1.002
            err = float(np.abs(pixels[:,:,0] - pixels[:,:,2]).max())
            assert err < .008, (method, label, "caster classification", err)
            metrics[method][label] = width(pixels[:,:,0])
            if method == "PCF" and label == "raised":
                wide_pcf = pixels[:,:,0]
            print("METHOD_WIDTH", method, label, metrics[method][label], flush=True)
    hard = metrics["HARD"]["raised"]
    for method in ("PCF","VSM","MSM","PCSS","VSSM","CHS"):
        assert metrics[method]["raised"] > hard + 2, (method, metrics)
    for method in ("PCSS","VSSM","CHS"):
        assert metrics[method]["raised"] > metrics[method]["contact"] + 2, (method, metrics)
    node.shadow_mode = "PCF"
    node.inputs[3].default_value = 0.0
    zero, _ = render_mask(scene, out / "PCF-zero.exr")
    assert abs(width(zero[:,:,0]) - hard) <= 1
    tree = material.node_tree
    position = tree.nodes.new("ShaderNodeNewGeometry")
    separate = tree.nodes.new("ShaderNodeSeparateXYZ")
    tree.links.new(position.outputs["Position"], separate.inputs[0])
    step = tree.nodes.new("ShaderNodeMath")
    step.operation = "GREATER_THAN"
    step.inputs[1].default_value = 0
    tree.links.new(separate.outputs["Y"], step.inputs[0])
    scale = tree.nodes.new("ShaderNodeMath")
    scale.operation = "MULTIPLY"
    scale.inputs[1].default_value = 32
    tree.links.new(step.outputs[0], scale.inputs[0])
    tree.links.new(scale.outputs[0], node.inputs["PCF Radius"])
    linked, _ = render_mask(scene, out / "PCF-linked.exr")
    assert np.abs(linked[40:90,:,0] - zero[40:90,:,0]).max() < .003
    assert np.abs(linked[230:280,:,0] - wide_pcf[230:280]).max() < .003
    node.shadow_mode = "CHS"
    node.inputs["Max Penumbra"].default_value = 0.0
    no_penumbra, _ = render_mask(scene, out / "CHS-zero.exr")
    assert np.abs(no_penumbra[:,:,0] - zero[:,:,0]).max() < .003
    check_material_textures(scene, tree, node, out)
    (out / "metrics.json").write_text(json.dumps(metrics, indent=2))
    print("SHADOW_METHODS_PASS", flush=True)


if __name__ == "__main__":
    run(Path(os.environ["BLENDER_TEST_OUTPUT_DIR"]))
