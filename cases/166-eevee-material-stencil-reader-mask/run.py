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


def set_surface_mode(mat):
    if hasattr(mat, "surface_render_method"):
        mat.surface_render_method = "DITHERED"
    if hasattr(mat, "blend_method"):
        mat.blend_method = "HASHED"


def make_principled_material(name, color):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    set_surface_mode(mat)
    mat.use_color_write = True
    mat.use_depth_write = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    require(bsdf is not None, f"{name} is missing Principled BSDF")
    bsdf.inputs["Base Color"].default_value = color
    bsdf.inputs["Roughness"].default_value = 0.5
    return mat


def make_scene(reader_test="EQUAL", writer_stencil=True, reader_stencil=True):
    reset_scene()
    configure_scene()

    writer_mat = make_principled_material("writer_stencil_plane", (0.55, 0.55, 0.55, 1.0))
    writer_mat.use_stencil = writer_stencil
    writer_mat.stencil_reference = 1
    writer_mat.stencil_read_mask = 15
    writer_mat.stencil_write_mask = 15
    writer_mat.stencil_test = "ALWAYS"
    writer_mat.stencil_pass_op = "REPLACE"
    writer_mat.stencil_fail_op = "KEEP"
    writer_mat.stencil_zfail_op = "KEEP"

    reader_mat = make_principled_material("reader_stencil_sphere", (1.0, 0.02, 0.02, 1.0))
    reader_mat.use_stencil = reader_stencil
    reader_mat.stencil_reference = 1
    reader_mat.stencil_read_mask = 15
    reader_mat.stencil_write_mask = 15
    reader_mat.stencil_test = reader_test
    reader_mat.stencil_pass_op = "KEEP"
    reader_mat.stencil_fail_op = "KEEP"
    reader_mat.stencil_zfail_op = "KEEP"

    bpy.ops.mesh.primitive_plane_add(size=1.15, location=(0.0, 0.35, 0.0), rotation=(math.radians(90), 0, 0))
    plane = bpy.context.object
    plane.name = "Stencil Writer Plane"
    plane.data.materials.append(writer_mat)

    bpy.ops.mesh.primitive_uv_sphere_add(
        segments=64, ring_count=32, radius=0.8, location=(0.0, -0.15, 0.0)
    )
    sphere = bpy.context.object
    sphere.name = "Stencil Reader Sphere"
    sphere.data.materials.append(reader_mat)

    bpy.ops.object.light_add(type="AREA", location=(0.0, -3.0, 2.0))
    light = bpy.context.object
    light.data.energy = 550.0
    light.data.size = 3.0

    bpy.ops.object.camera_add(location=(0.0, -4.2, 0.0))
    camera = bpy.context.object
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = 2.35
    look_at(camera, (0.0, 0.0, 0.0))
    bpy.context.scene.camera = camera
    bpy.context.view_layer.update()


def render_and_count_red(name):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    path = OUT_DIR / f"{name}.png"
    scene = bpy.context.scene
    scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)

    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        pixels = list(image.pixels)
        require(pixels, f"{name} produced an empty render")
        width, height = image.size
        red_pixels = 0
        bright_pixels = 0
        for index in range(0, len(pixels), 4):
            r, g, b, _a = pixels[index : index + 4]
            if r + g + b > 0.12:
                bright_pixels += 1
            if r > 0.18 and r > g * 1.35 and r > b * 1.35:
                red_pixels += 1
        print(
            "STENCIL_READER_MASK_CASE",
            name,
            f"red_pixels={red_pixels}",
            f"bright_pixels={bright_pixels}",
            f"size={width}x{height}",
            f"file={path}",
        )
        return red_pixels
    finally:
        bpy.data.images.remove(image)


def run_variant(name, reader_test="EQUAL", writer_stencil=True, reader_stencil=True):
    make_scene(reader_test=reader_test, writer_stencil=writer_stencil, reader_stencil=reader_stencil)
    return render_and_count_red(name)


def main():
    masked = run_variant("baseline_masked")
    reader_never = run_variant("reader_never", reader_test="NEVER")
    writer_off = run_variant("writer_off", writer_stencil=False)
    reader_off = run_variant("reader_off", reader_stencil=False)

    require(reader_off > 2500, f"Reader without stencil should be visibly red: {reader_off}")
    require(masked > 500, f"Masked reader should still be visible inside the writer mask: {masked}")
    require(
        masked < reader_off * 0.85,
        f"Masked reader should be smaller than the unmasked reader: {masked} vs {reader_off}",
    )
    require(
        reader_never <= max(20, reader_off * 0.03),
        f"NEVER stencil test should hide the reader: {reader_never} vs {reader_off}",
    )
    require(
        writer_off <= max(20, reader_off * 0.03),
        f"Missing writer stencil should hide the reader: {writer_off} vs {reader_off}",
    )
    print("EEVEE_MATERIAL_STENCIL_READER_MASK_OK=1")


if __name__ == "__main__":
    main()
