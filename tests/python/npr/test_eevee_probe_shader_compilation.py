# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Compile sphere and planar probe lighting and render a short native-light animation.

The shared shadow tracer previously crashed shaderc during planar SPIR-V optimization,
including scenes which only requested a sphere probe. Pass --debug-gpu-shader-debug-info to
bypass the SPIR-V disk cache and force compilation; ordinary repeated runs cover cache reuse.
"""

import os
from pathlib import Path
from tempfile import TemporaryDirectory

import bpy


def render_probe(kind, directory):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = scene.render.resolution_y = 64
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.filepath = str(directory / "frame")
    scene.eevee.taa_render_samples = 2
    scene.eevee.use_raytracing = kind == "PLANE"
    scene.frame_start, scene.frame_end = 1, 3

    world = bpy.data.worlds.new("Probe World")
    scene.world = world
    world.color = (0.02, 0.02, 0.02)
    bpy.ops.object.camera_add(location=(0.0, 0.0, 5.0))
    scene.camera = bpy.context.object
    scene.camera.data.type = "ORTHO"
    scene.camera.data.ortho_scale = 3.0
    bpy.ops.object.light_add(type="POINT", location=(0.0, 0.0, 2.0))
    light = bpy.context.object
    light.data.energy = 150.0
    light.keyframe_insert("location", frame=1)
    light.location.x = 0.5
    light.keyframe_insert("location", frame=3)

    bpy.ops.mesh.primitive_plane_add(size=2.0)
    material = bpy.data.materials.new("Native Light Probe Regression")
    material.use_nodes = True
    tree = material.node_tree
    tree.nodes.clear()
    surface = tree.nodes.new("ShaderNodeOutputMaterial")
    accumulation = tree.nodes.new("ShaderNodeLightAccumulation")
    evaluate = tree.nodes.new("ShaderNodeLightEvaluation")
    tree.links.new(evaluate.outputs["Factor"], accumulation.inputs["Diffuse Light"])
    tree.links.new(accumulation.outputs[0], surface.inputs["Surface"])
    bpy.context.object.data.materials.append(material)

    bpy.ops.object.lightprobe_add(type=kind, location=(0.0, 0.0, -0.1))
    probe = bpy.context.object
    probe.data.influence_distance = 10.0
    probe.scale = (3.0, 3.0, 3.0)
    assert bpy.ops.render.render(animation=True) == {"FINISHED"}
    for frame in range(1, 4):
        image = bpy.data.images.load(str(directory / f"frame{frame:04d}.png"), check_existing=False)
        try:
            assert tuple(image.size) == (64, 64)
            index = 4 * (32 * 64 + 32)
            rgb = list(image.pixels[index:index + 3])
            assert min(rgb) > 0.01, f"{kind} frame {frame}: unlit or shader-error pixel {rgb}"
        finally:
            bpy.data.images.remove(image)
    print("PROBE_ANIMATION_PASS", kind, flush=True)


if __name__ == "__main__":
    output_dir = Path(os.environ["BLENDER_TEST_OUTPUT_DIR"])
    output_dir.mkdir(parents=True, exist_ok=True)
    for kind in ("SPHERE", "PLANE"):
        with TemporaryDirectory(dir=output_dir, prefix=kind) as directory:
            render_probe(kind, Path(directory))
