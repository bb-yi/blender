from __future__ import annotations

import math
from dataclasses import dataclass
from pathlib import Path

import bpy


OUT_DIR = Path(__file__).resolve().parent / "out"


@dataclass
class Case:
    name: str
    surface_render_method: str = "DITHERED"
    blend_method: str = "HASHED"
    use_color_write: bool = True
    use_depth_write: bool = True
    use_stencil: bool = False
    stencil_test: str = "ALWAYS"
    stencil_pass_op: str = "KEEP"
    stencil_fail_op: str = "KEEP"
    stencil_zfail_op: str = "KEEP"
    stencil_reference: int = 1
    stencil_read_mask: int = 15
    stencil_write_mask: int = 15


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def reset_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()
    for material in list(bpy.data.materials):
        bpy.data.materials.remove(material)


def configure_scene():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 320
    scene.render.resolution_y = 240
    scene.render.resolution_percentage = 100
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0
    scene.view_settings.gamma = 1
    if hasattr(scene, "eevee"):
        if hasattr(scene.eevee, "taa_render_samples"):
            scene.eevee.taa_render_samples = 16
        if hasattr(scene.eevee, "use_gtao"):
            scene.eevee.use_gtao = False
    if scene.world is None:
        scene.world = bpy.data.worlds.new("World")
    scene.world.color = (0.0, 0.0, 0.0)


def make_material(case: Case):
    mat = bpy.data.materials.new(case.name)
    mat.use_nodes = True
    mat.surface_render_method = case.surface_render_method
    mat.blend_method = case.blend_method
    mat.use_color_write = case.use_color_write
    mat.use_depth_write = case.use_depth_write
    mat.use_stencil = case.use_stencil
    mat.stencil_test = case.stencil_test
    mat.stencil_pass_op = case.stencil_pass_op
    mat.stencil_fail_op = case.stencil_fail_op
    mat.stencil_zfail_op = case.stencil_zfail_op
    mat.stencil_reference = case.stencil_reference
    mat.stencil_read_mask = case.stencil_read_mask
    mat.stencil_write_mask = case.stencil_write_mask

    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    require(bsdf is not None, "Missing Principled BSDF in generated test material")
    bsdf.inputs["Base Color"].default_value = (0.8, 0.72, 0.56, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.55
    bsdf.inputs["Alpha"].default_value = 1.0
    return mat


def make_scene(case: Case):
    reset_scene()
    configure_scene()
    mat = make_material(case)

    bpy.ops.mesh.primitive_uv_sphere_add(segments=64, ring_count=32, radius=1.0, location=(0, 0, 0))
    sphere = bpy.context.object
    sphere.data.materials.append(mat)

    bpy.ops.object.light_add(type="AREA", location=(-2.2, -3.0, 4.0))
    light = bpy.context.object
    light.data.energy = 700.0
    light.data.size = 4.0

    bpy.ops.object.camera_add(location=(0, -4.5, 1.2), rotation=(math.radians(76), 0, 0))
    camera = bpy.context.object
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = 2.6
    bpy.context.scene.camera = camera
    bpy.context.view_layer.update()


def render_mean(case: Case):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    path = OUT_DIR / f"{case.name}.exr"
    scene = bpy.context.scene
    scene.render.image_settings.file_format = "OPEN_EXR"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "32"
    scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=False)
    bpy.data.images["Render Result"].save_render(str(path))
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        pixels = list(image.pixels)
        require(pixels, f"{case.name} produced an empty render")
        total = len(pixels) // 4
        mean_rgb = 0.0
        max_rgb = 0.0
        for index in range(0, len(pixels), 4):
            r, g, b, _a = pixels[index : index + 4]
            mean_rgb += (r + g + b) / 3.0
            max_rgb = max(max_rgb, r, g, b)
        center_index = (image.size[1] // 2 * image.size[0] + image.size[0] // 2) * 4
        center_rgb = pixels[center_index : center_index + 3]
        return mean_rgb / total, max_rgb, center_rgb
    finally:
        bpy.data.images.remove(image)


def run_case(case: Case):
    make_scene(case)
    mean_rgb, max_rgb, center_rgb = render_mean(case)
    print(
        "STENCIL_TEMPLATE_CASE",
        case.name,
        f"mean_rgb={mean_rgb:.6f}",
        f"max_rgb={max_rgb:.6f}",
        f"center_rgb={center_rgb}",
    )
    return mean_rgb


def main():
    baseline = run_case(Case("baseline_dithered"))
    deferred_writer = run_case(Case("writer_dithered", use_stencil=True, stencil_pass_op="REPLACE"))
    readonly_always = run_case(Case("readonly_always_dithered", use_stencil=True))
    readonly_missing = run_case(
        Case("readonly_equal_missing_dithered", use_stencil=True, stencil_test="EQUAL")
    )
    forward_writer = run_case(
        Case(
            "writer_blended",
            surface_render_method="BLENDED",
            blend_method="BLEND",
            use_stencil=True,
            stencil_pass_op="REPLACE",
        )
    )

    require(baseline > 0.2, f"Baseline render is unexpectedly dark: {baseline:.6f}")
    require(
        deferred_writer >= baseline * 0.85,
        f"Deferred stencil writer lost lighting: {deferred_writer:.6f} vs {baseline:.6f}",
    )
    require(
        readonly_always >= baseline * 0.85,
        f"Read-only ALWAYS stencil lost lighting: {readonly_always:.6f} vs {baseline:.6f}",
    )
    require(
        readonly_missing <= baseline * 0.25,
        f"Missing EQUAL stencil read should stay dark: {readonly_missing:.6f} vs {baseline:.6f}",
    )
    require(
        forward_writer >= baseline * 0.85,
        f"Forward stencil writer lost lighting: {forward_writer:.6f} vs {baseline:.6f}",
    )
    print("EEVEE_MATERIAL_STENCIL_TEMPLATE_LIGHTING_OK=1")


if __name__ == "__main__":
    main()
