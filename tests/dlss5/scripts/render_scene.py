import argparse
import math
import os
import struct
import sys

import bpy
from mathutils import Vector


def look_at(obj, target):
    obj.rotation_euler = (Vector(target) - obj.location).to_track_quat("-Z", "Y").to_euler()


def make_material(name, color, roughness=0.45, metallic=0.0):
    material = bpy.data.materials.new(name)
    material.diffuse_color = (*color, 1.0)
    material.use_nodes = True
    bsdf = material.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = (*color, 1.0)
    bsdf.inputs["Roughness"].default_value = roughness
    bsdf.inputs["Metallic"].default_value = metallic
    return material


def build_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.image_settings.file_format = "BMP"
    scene.render.image_settings.color_mode = "RGB"
    scene.render.film_transparent = False
    scene.world = bpy.data.worlds.new("DLSS5World")
    scene.world.color = (0.025, 0.025, 0.025)
    scene.world.use_nodes = True
    scene.world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.012, 0.018, 0.03, 1.0)
    scene.world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.25

    red = make_material("DLSS5_Red", (0.72, 0.055, 0.035), 0.28)
    cyan = make_material("DLSS5_Cyan", (0.02, 0.38, 0.62), 0.32, 0.1)
    gold = make_material("DLSS5_Gold", (0.75, 0.35, 0.035), 0.5)
    floor_mat = make_material("DLSS5_Floor", (0.08, 0.09, 0.11), 0.8)

    bpy.ops.mesh.primitive_monkey_add(location=(-0.9, 0.0, 1.15))
    monkey = bpy.context.object
    monkey.name = "DLSS5_Monkey"
    monkey.scale = (1.15, 1.15, 1.15)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    monkey.data.materials.append(red)
    bpy.ops.object.shade_smooth()
    modifier = monkey.modifiers.new("Subdivision", "SUBSURF")
    modifier.levels = 2
    modifier.render_levels = 2

    bpy.ops.mesh.primitive_torus_add(major_radius=0.72, minor_radius=0.22, location=(1.15, 0.15, 0.82))
    torus = bpy.context.object
    torus.name = "DLSS5_Torus"
    torus.rotation_euler = (math.radians(68), math.radians(8), math.radians(24))
    torus.data.materials.append(cyan)

    bpy.ops.mesh.primitive_cube_add(location=(0.0, 0.4, 0.35), scale=(3.5, 3.0, 0.18))
    floor = bpy.context.object
    floor.name = "DLSS5_Floor"
    floor.data.materials.append(floor_mat)
    bevel = floor.modifiers.new("Floor bevel", "BEVEL")
    bevel.width = 0.08
    bevel.segments = 3

    for x in (-2.7, 2.7):
        bpy.ops.mesh.primitive_cube_add(location=(x, 1.0, 1.4), scale=(0.14, 0.14, 1.4))
        pillar = bpy.context.object
        pillar.data.materials.append(gold)
        bevel = pillar.modifiers.new("Pillar bevel", "BEVEL")
        bevel.width = 0.08
        bevel.segments = 2

    bpy.ops.object.light_add(type="AREA", location=(0.0, -2.3, 5.2))
    key = bpy.context.object
    key.data.energy = 900.0
    key.data.shape = "DISK"
    key.data.size = 4.0
    look_at(key, (0.0, 0.0, 0.8))

    bpy.ops.object.light_add(type="AREA", location=(3.4, -1.0, 2.4))
    fill = bpy.context.object
    fill.data.energy = 500.0
    fill.data.color = (0.15, 0.35, 1.0)
    fill.data.size = 2.0
    look_at(fill, (0.0, 0.0, 1.0))

    bpy.ops.object.camera_add(location=(0.0, -7.6, 3.25))
    camera = bpy.context.object
    camera.data.lens = 52
    camera.data.sensor_width = 36
    look_at(camera, (0.0, 0.25, 1.05))
    scene.camera = camera

    scene.view_settings.look = "AgX - Medium High Contrast"
    return scene


def write_rgba32f(path, image):
    pixels = list(image.pixels)
    with open(path, "wb") as handle:
        handle.write(struct.pack("<8sII", b"DLSS5F32", image.size[0], image.size[1]))
        handle.write(struct.pack(f"<{len(pixels)}f", *pixels))


def render(scene,
           path,
           exr_path,
           linear_path,
           depth_output,
           vector_output,
           pass_prefix,
           width,
           height):
    scene.render.resolution_x = width
    scene.render.resolution_y = height
    scene.render.resolution_percentage = 100

    scene.render.image_settings.file_format = "BMP"
    scene.render.image_settings.color_mode = "RGB"
    scene.render.filepath = os.path.abspath(path)
    bpy.ops.render.render(write_still=True)

    scene.render.image_settings.file_format = "OPEN_EXR"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "16"
    scene.render.filepath = os.path.abspath(exr_path)
    pass_directory = os.path.join(
        os.path.dirname(path), "passes", pass_prefix.rstrip("_")
    )
    os.makedirs(pass_directory, exist_ok=True)
    depth_output.directory = os.path.abspath(pass_directory)
    vector_output.directory = os.path.abspath(pass_directory)
    depth_output.file_name = "depth"
    vector_output.file_name = "vector"
    bpy.ops.render.render(write_still=True)

    image = bpy.data.images.load(os.path.abspath(exr_path), check_existing=False)
    try:
        if tuple(image.size) != (width, height) or len(image.pixels) != width * height * 4:
            raise RuntimeError("Loaded EXR has unexpected dimensions or pixel count")
        write_rgba32f(linear_path, image)
    finally:
        bpy.data.images.remove(image)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--outdir", default=os.environ.get("DLSS5_OUTDIR"))
    parser.add_argument("--preset", choices=("both", "input", "ground_truth"), default="both")
    argv = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    args = parser.parse_args(argv)
    if not args.outdir:
        parser.error("set --outdir or DLSS5_OUTDIR")
    os.makedirs(args.outdir, exist_ok=True)
    scene = build_scene()
    scene.view_layers[0].use_pass_z = True
    scene.view_layers[0].use_pass_vector = True
    scene.use_nodes = True
    node_tree = bpy.data.node_groups.new("DLSS5Compositor", "CompositorNodeTree")
    scene.compositing_node_group = node_tree
    nodes = node_tree.nodes
    links = node_tree.links
    layers = nodes.new("CompositorNodeRLayers")

    depth_output = nodes.new("CompositorNodeOutputFile")
    depth_output.directory = os.path.abspath(args.outdir)
    depth_output.format.file_format = "OPEN_EXR_MULTILAYER"
    depth_output.format.color_mode = "RGBA"
    depth_output.file_output_items.new("FLOAT", "Depth")
    links.new(layers.outputs["Depth"], depth_output.inputs["Depth"])

    vector_output = nodes.new("CompositorNodeOutputFile")
    vector_output.directory = os.path.abspath(args.outdir)
    vector_output.format.file_format = "OPEN_EXR_MULTILAYER"
    vector_output.format.color_mode = "RGBA"
    vector_output.file_output_items.new("VECTOR", "Vector")
    links.new(layers.outputs["Vector"], vector_output.inputs["Vector"])

    if args.preset in ("both", "input"):
        render(
            scene,
            os.path.join(args.outdir, "input_256x144.bmp"),
            os.path.join(args.outdir, "input_256x144.exr"),
            os.path.join(args.outdir, "input_256x144.rgba32f"),
            depth_output,
            vector_output,
            "input_",
            256,
            144,
        )
    if args.preset in ("both", "ground_truth"):
        render(
            scene,
            os.path.join(args.outdir, "ground_truth_512x288.bmp"),
            os.path.join(args.outdir, "ground_truth_512x288.exr"),
            os.path.join(args.outdir, "ground_truth_512x288.rgba32f"),
            depth_output,
            vector_output,
            "ground_truth_",
            512,
            288,
        )


if __name__ == "__main__":
    main()
