import json
from pathlib import Path

import bpy
import OpenImageIO as oiio


CASE_DIR = Path(__file__).resolve().parent
OUT_DIR = CASE_DIR / "out"
OUT_DIR.mkdir(parents=True, exist_ok=True)
OUTPUT_PREFIX = "outline_pass_toggle"
MARKER = "EEVEE_OUTLINE_PASS_TOGGLE_COMPOSITOR_REFRESH_OK="


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def set_if_available(owner, name, value):
    if hasattr(owner, name):
        setattr(owner, name, value)


def make_outline_material():
    material = bpy.data.materials.new("Outline Pass Toggle Material")
    material.use_nodes = True
    material.surface_render_method = "DITHERED"
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (0.04, 0.16, 0.8, 1.0)
    emission.inputs["Strength"].default_value = 1.0
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    outline = nodes.new("ShaderNodeOutlineControl")
    outline.inputs["Line Color"].default_value = (1.0, 0.0, 0.0, 1.0)
    outline.inputs["Line Alpha"].default_value = 1.0
    outline.inputs["Line Width"].default_value = 8.0
    outline.inputs["Depth Threshold"].default_value = 0.1
    outline.inputs["Normal Threshold"].default_value = 0.5
    outline.inputs["Outline ID"].default_value = 1
    return material


def build_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 96
    scene.render.resolution_y = 96
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = False
    scene.render.use_compositing = True
    scene.eevee.use_outline = True
    set_if_available(scene.eevee, "taa_samples", 1)
    set_if_available(scene.eevee, "taa_render_samples", 1)
    set_if_available(scene.eevee, "use_taa_reprojection", False)
    set_if_available(scene.eevee, "use_raytracing", False)
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0

    world = scene.world
    world.use_nodes = True
    background = world.node_tree.nodes.get("Background")
    background.inputs["Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    background.inputs["Strength"].default_value = 0.0

    bpy.ops.object.camera_add(location=(0.0, 0.0, 5.0))
    camera = bpy.context.object
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = 3.5
    scene.camera = camera

    bpy.ops.mesh.primitive_uv_sphere_add(
        segments=32,
        ring_count=16,
        radius=1.0,
        location=(0.0, 0.0, 0.0),
    )
    sphere = bpy.context.object
    for polygon in sphere.data.polygons:
        polygon.use_smooth = True
    sphere.data.materials.append(make_outline_material())

    view_layer = bpy.context.view_layer
    view_layer.eevee.use_pass_outline = True

    tree = bpy.data.node_groups.new(
        "Outline Pass Toggle Compositor", "CompositorNodeTree"
    )
    scene.compositing_node_group = tree
    render_layers = tree.nodes.new("CompositorNodeRLayers")
    alpha_over = tree.nodes.new("CompositorNodeAlphaOver")
    alpha_over.inputs["Background"].default_value = (1.0, 1.0, 1.0, 1.0)
    alpha_over.inputs["Foreground"].default_value = (1.0, 1.0, 1.0, 1.0)
    file_output = tree.nodes.new("CompositorNodeOutputFile")
    file_output.directory = str(OUT_DIR)
    file_output.file_name = OUTPUT_PREFIX
    file_output.format.file_format = "OPEN_EXR_MULTILAYER"
    file_output.format.color_depth = "32"
    file_output.file_output_items.clear()
    file_output.file_output_items.new("RGBA", "Image")

    tree.links.new(render_layers.outputs["Image"], alpha_over.inputs["Background"])
    tree.links.new(render_layers.outputs["Outline"], alpha_over.inputs["Foreground"])
    tree.links.new(alpha_over.outputs["Image"], file_output.inputs["Image"])

    for old_path in OUT_DIR.glob(OUTPUT_PREFIX + "*.exr"):
        old_path.unlink()

    bpy.context.view_layer.update()
    return scene, view_layer, tree, render_layers, alpha_over


def evaluated_nodes(scene):
    depsgraph = bpy.context.evaluated_depsgraph_get()
    evaluated_scene = scene.evaluated_get(depsgraph)
    evaluated_tree = evaluated_scene.compositing_node_group
    require(evaluated_tree is not None, "Evaluated compositor tree is missing")
    evaluated_render_layers = next(
        node
        for node in evaluated_tree.nodes
        if node.bl_idname == "CompositorNodeRLayers"
    )
    evaluated_alpha_over = next(
        node
        for node in evaluated_tree.nodes
        if node.bl_idname == "CompositorNodeAlphaOver"
    )
    return evaluated_tree, evaluated_render_layers, evaluated_alpha_over


def find_output():
    paths = sorted(OUT_DIR.glob(OUTPUT_PREFIX + "*.exr"))
    require(paths, "Compositor File Output did not write the expected EXR")
    return paths[-1]


def analyze_output(path):
    image_input = oiio.ImageInput.open(str(path))
    require(image_input is not None, f"Could not open compositor output: {path}")
    try:
        spec = image_input.spec()
        pixels = image_input.read_image(format=oiio.FLOAT)
        require(pixels is not None, f"Could not read compositor output: {path}")
    finally:
        image_input.close()

    flat = pixels.reshape((-1, pixels.shape[-1]))
    red_count = sum(
        1
        for pixel in flat
        if float(pixel[0]) > 0.35
        and float(pixel[0]) > float(pixel[1]) * 1.5 + 0.05
        and float(pixel[0]) > float(pixel[2]) * 1.5 + 0.05
    )
    non_white_count = sum(
        1
        for pixel in flat
        if any(abs(float(channel) - 1.0) > 1.0e-4 for channel in pixel[:4])
    )
    return {
        "path": str(path),
        "size": [int(spec.width), int(spec.height)],
        "red_count": red_count,
        "non_white_count": non_white_count,
    }


def main():
    scene, view_layer, tree, render_layers, alpha_over = build_scene()
    factor_before = float(alpha_over.inputs["Fac"].default_value)
    foreground_default_before = tuple(alpha_over.inputs["Foreground"].default_value)

    evaluated_tree, evaluated_render_layers, evaluated_alpha_over = evaluated_nodes(scene)
    require(render_layers.outputs.get("Outline") is not None, "Original Outline socket is missing")
    require(
        evaluated_render_layers.outputs.get("Outline") is not None,
        "Evaluated Outline socket is missing before disable",
    )
    require(alpha_over.inputs["Foreground"].is_linked, "Original foreground is not linked")
    require(
        evaluated_alpha_over.inputs["Foreground"].is_linked,
        "Evaluated foreground is not linked before disable",
    )
    require(len(tree.links) == 3, f"Expected 3 original links, got {len(tree.links)}")
    require(
        len(evaluated_tree.links) == 3,
        f"Expected 3 evaluated links, got {len(evaluated_tree.links)}",
    )

    bpy.ops.render.render()
    output_path = find_output()
    enabled = analyze_output(output_path)
    enabled_mtime_ns = output_path.stat().st_mtime_ns
    require(enabled["red_count"] > 10, f"Enabled Outline output is empty: {enabled}")

    view_layer.eevee.use_pass_outline = False
    bpy.context.view_layer.update()

    evaluated_tree, evaluated_render_layers, evaluated_alpha_over = evaluated_nodes(scene)
    require(render_layers.outputs.get("Outline") is None, "Original Outline socket survived disable")
    require(
        evaluated_render_layers.outputs.get("Outline") is None,
        "Evaluated Outline socket survived disable",
    )
    require(not alpha_over.inputs["Foreground"].is_linked, "Original foreground stayed linked")
    require(
        not evaluated_alpha_over.inputs["Foreground"].is_linked,
        "Evaluated foreground stayed linked after disable",
    )
    require(len(tree.links) == 2, f"Expected 2 original links, got {len(tree.links)}")
    require(
        len(evaluated_tree.links) == 2,
        f"Expected 2 evaluated links, got {len(evaluated_tree.links)}",
    )
    require(
        float(alpha_over.inputs["Fac"].default_value) == factor_before,
        "Alpha Over factor changed during pass toggle",
    )
    require(
        tuple(alpha_over.inputs["Foreground"].default_value) == foreground_default_before,
        "Alpha Over foreground default changed during pass toggle",
    )

    bpy.ops.render.render()
    output_path = find_output()
    require(
        output_path.stat().st_mtime_ns > enabled_mtime_ns,
        "Second render did not overwrite the compositor output",
    )
    disabled = analyze_output(output_path)
    require(disabled["red_count"] == 0, f"Disabled output retained red outline: {disabled}")
    require(
        disabled["non_white_count"] == 0,
        f"Disabled output is not pure white: {disabled}",
    )

    result = {
        "enabled": enabled,
        "disabled": disabled,
        "original_links_after_disable": len(tree.links),
        "evaluated_links_after_disable": len(evaluated_tree.links),
        "factor_unchanged": True,
        "foreground_default_unchanged": True,
    }
    print(MARKER + json.dumps(result, sort_keys=True), flush=True)


main()
