# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import os
from pathlib import Path
from tempfile import TemporaryDirectory

import bpy


def test_render_animation(output_dir):
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    size = 64
    scene.render.resolution_x = size
    scene.render.resolution_y = size
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.filepath = str(output_dir / "frame")
    scene.eevee.taa_render_samples = 4
    scene.frame_start = 1
    scene.frame_end = 5

    bpy.ops.object.camera_add(location=(0.0, 0.0, 4.0))
    scene.camera = bpy.context.object
    scene.camera.data.type = "ORTHO"
    scene.camera.data.ortho_scale = 3.0

    bpy.ops.object.light_add(type="POINT", location=(0.0, 0.0, 2.0))
    light = bpy.context.object
    light.data.energy = 120.0
    light.data.use_shadow = False
    light.keyframe_insert(data_path="location", frame=1)
    light.location.x = 0.65
    light.keyframe_insert(data_path="location", frame=5)

    bpy.ops.mesh.primitive_plane_add(size=2.0)
    material = bpy.data.materials.new("NativeLightAccumulationRegression")
    material.use_nodes = True
    tree = material.node_tree
    tree.nodes.clear()
    surface = tree.nodes.new("ShaderNodeOutputMaterial")
    accumulation = tree.nodes.new("ShaderNodeLightAccumulation")
    evaluate = tree.nodes.new("ShaderNodeLightEvaluation")
    tree.links.new(evaluate.outputs["Factor"], accumulation.inputs["Diffuse Light"])
    tree.links.new(accumulation.outputs["Shader"], surface.inputs["Surface"])
    bpy.context.object.data.materials.append(material)

    assert bpy.ops.render.render(animation=True) == {"FINISHED"}
    for frame_number in range(scene.frame_start, scene.frame_end + 1):
        frame = output_dir / f"frame{frame_number:04d}.png"
        image = bpy.data.images.load(str(frame), check_existing=False)
        try:
            index = 4 * ((size // 2) * size + size // 2)
            rgb = list(image.pixels[index:index + 3])
            assert max(rgb) > 0.015, f"{frame}: center pixel is unlit: {rgb}"
        finally:
            bpy.data.images.remove(image)


if __name__ == "__main__":
    output_dir = Path(os.environ["BLENDER_TEST_OUTPUT_DIR"])
    output_dir.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(dir=output_dir) as run_dir:
        test_render_animation(Path(run_dir))
