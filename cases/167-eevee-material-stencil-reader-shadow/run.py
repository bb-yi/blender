from __future__ import annotations

import math
from pathlib import Path

import bpy
import mathutils


OUT_DIR = Path(__file__).resolve().parent / "out"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def reset_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()
    for material in list(bpy.data.materials):
        bpy.data.materials.remove(material)
    for image in list(bpy.data.images):
        bpy.data.images.remove(image)


def look_at(obj, target):
    direction = mathutils.Vector(target) - obj.location
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def configure_scene():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 320
    scene.render.resolution_y = 240
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0
    scene.view_settings.gamma = 1
    if hasattr(scene, "eevee"):
        if hasattr(scene.eevee, "taa_render_samples"):
            scene.eevee.taa_render_samples = 16
        if hasattr(scene.eevee, "taa_samples"):
            scene.eevee.taa_samples = 16
        if hasattr(scene.eevee, "use_gtao"):
            scene.eevee.use_gtao = False
    if scene.world is None:
        scene.world = bpy.data.worlds.new("World")
    scene.world.color = (0.0, 0.0, 0.0)


def make_mat(name, color):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    if hasattr(mat, "surface_render_method"):
        mat.surface_render_method = "DITHERED"
    if hasattr(mat, "blend_method"):
        mat.blend_method = "HASHED"
    mat.use_color_write = True
    mat.use_depth_write = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    require(bsdf is not None, f"{name} is missing Principled BSDF")
    bsdf.inputs["Base Color"].default_value = color
    bsdf.inputs["Roughness"].default_value = 0.55
    return mat


def make_scene(reader_shadow=True):
    reset_scene()
    configure_scene()

    floor_mat = make_mat("large_shadow_receiver", (0.72, 0.72, 0.72, 1.0))
    writer_mat = make_mat("small_stencil_writer", (0.72, 0.72, 0.72, 1.0))
    writer_mat.use_stencil = True
    writer_mat.stencil_reference = 1
    writer_mat.stencil_read_mask = 15
    writer_mat.stencil_write_mask = 15
    writer_mat.stencil_test = "ALWAYS"
    writer_mat.stencil_pass_op = "REPLACE"
    writer_mat.stencil_fail_op = "KEEP"
    writer_mat.stencil_zfail_op = "KEEP"

    reader_mat = make_mat("masked_reader", (1.0, 0.02, 0.02, 1.0))
    reader_mat.use_stencil = True
    reader_mat.stencil_reference = 1
    reader_mat.stencil_read_mask = 15
    reader_mat.stencil_write_mask = 15
    reader_mat.stencil_test = "EQUAL"
    reader_mat.stencil_pass_op = "KEEP"
    reader_mat.stencil_fail_op = "KEEP"
    reader_mat.stencil_zfail_op = "KEEP"

    bpy.ops.mesh.primitive_plane_add(size=4.0, location=(0.0, 0.0, -0.03))
    floor = bpy.context.object
    floor.name = "Shadow Receiver Floor"
    floor.data.materials.append(floor_mat)

    bpy.ops.mesh.primitive_plane_add(size=1.25, location=(0.0, 0.0, 0.0))
    writer = bpy.context.object
    writer.name = "Stencil Writer Patch"
    writer.data.materials.append(writer_mat)

    bpy.ops.mesh.primitive_uv_sphere_add(
        segments=64, ring_count=32, radius=0.8, location=(0.0, 0.0, 0.62)
    )
    reader = bpy.context.object
    reader.name = "Stencil Reader Sphere"
    reader.visible_shadow = reader_shadow
    reader.data.materials.append(reader_mat)

    bpy.ops.object.light_add(type="AREA", location=(-2.0, -3.0, 4.0))
    light = bpy.context.object
    light.data.energy = 650.0
    light.data.size = 2.0

    bpy.ops.object.camera_add(location=(2.15, -3.0, 1.55))
    camera = bpy.context.object
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = 2.6
    look_at(camera, (0.0, 0.0, 0.25))
    bpy.context.scene.camera = camera
    bpy.context.view_layer.update()


def render_png(name):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    path = OUT_DIR / f"{name}.png"
    bpy.context.scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)
    return path


def load_pixels(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        return image.size[:], list(image.pixels)
    finally:
        bpy.data.images.remove(image)


def render_variant(name, reader_shadow=True):
    make_scene(reader_shadow=reader_shadow)
    path = render_png(name)
    size, pixels = load_pixels(path)
    return path, size, pixels


def analyze_pair(base_pixels, no_shadow_pixels):
    require(len(base_pixels) == len(no_shadow_pixels), "Render sizes do not match")
    red_count = 0
    floor_count = 0
    floor_abs_delta = 0.0
    floor_dark_delta = 0
    for index in range(0, len(base_pixels), 4):
        r, g, b, _a = base_pixels[index : index + 4]
        nr, ng, nb, _na = no_shadow_pixels[index : index + 4]
        is_red = r > 0.18 and r > g * 1.35 and r > b * 1.35
        if is_red:
            red_count += 1
            continue
        is_floor = (r + g + b) > 0.55 and abs(r - g) < 0.08 and abs(g - b) < 0.08
        if not is_floor:
            continue
        lum = (r + g + b) / 3.0
        no_shadow_lum = (nr + ng + nb) / 3.0
        delta = abs(lum - no_shadow_lum)
        floor_abs_delta += delta
        floor_count += 1
        if no_shadow_lum - lum > 0.08:
            floor_dark_delta += 1
    mean_delta = floor_abs_delta / max(floor_count, 1)
    return red_count, floor_count, mean_delta, floor_dark_delta


def main():
    base_path, size, base_pixels = render_variant("baseline_reader_shadow", reader_shadow=True)
    no_shadow_path, no_shadow_size, no_shadow_pixels = render_variant(
        "explicit_reader_no_shadow", reader_shadow=False
    )
    require(size == no_shadow_size, f"Render sizes differ: {size} vs {no_shadow_size}")

    red_count, floor_count, mean_delta, floor_dark_delta = analyze_pair(base_pixels, no_shadow_pixels)
    print(
        "STENCIL_READER_SHADOW_CASE",
        f"red_count={red_count}",
        f"floor_count={floor_count}",
        f"mean_delta={mean_delta:.6f}",
        f"floor_dark_delta={floor_dark_delta}",
        f"baseline={base_path}",
        f"no_shadow={no_shadow_path}",
    )

    require(red_count > 500, f"Masked reader should remain visible: red_count={red_count}")
    require(floor_count > 5000, f"Expected enough visible floor pixels: floor_count={floor_count}")
    require(
        mean_delta > 0.025,
        "Masked reader should still cast a normal EEVEE shadow; default render is too close to "
        f"explicit shadow-off: mean_delta={mean_delta:.6f}",
    )
    require(
        floor_dark_delta > floor_count * 0.04,
        "Masked reader shadow was effectively removed compared with explicit shadow-off: "
        f"{floor_dark_delta}/{floor_count}",
    )
    print("EEVEE_MATERIAL_STENCIL_READER_SHADOW_OK=1")


if __name__ == "__main__":
    main()
