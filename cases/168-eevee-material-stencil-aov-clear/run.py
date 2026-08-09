import os
import sys
from pathlib import Path

import bpy


CASE_DIR = Path(__file__).resolve().parent
ROOT = CASE_DIR.parents[3]
OUT_DIR = CASE_DIR / "out"
sys.path.insert(0, str(ROOT / "blender_npr_post" / "tests" / "python" / "npr"))

from filter_graph_test_utils import attach_filter_material, clear_filter_graph


RESOLUTION = 96
AOV_NAME = "StencilClearAOV"


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def configure_scene():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = RESOLUTION
    scene.render.resolution_y = RESOLUTION
    scene.render.resolution_percentage = 100
    scene.eevee.taa_samples = 1
    scene.eevee.taa_render_samples = 1
    if hasattr(scene.eevee, "use_taa_reprojection"):
        scene.eevee.use_taa_reprojection = False
    if hasattr(scene.eevee, "use_raytracing"):
        scene.eevee.use_raytracing = True
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.world.color = (0.0, 0.0, 0.0)
    clear_filter_graph(scene)

    view_layer = bpy.context.view_layer
    while len(view_layer.aovs) > 0:
        view_layer.aovs.remove(view_layer.aovs[0])
    aov = view_layer.aovs.add()
    aov.name = AOV_NAME
    aov.type = "COLOR"


def make_camera():
    cam_data = bpy.data.cameras.new("Camera")
    cam_data.type = "ORTHO"
    cam_data.ortho_scale = 4.0
    cam = bpy.data.objects.new("Camera", cam_data)
    cam.location = (0.0, 0.0, 5.0)
    bpy.context.scene.collection.objects.link(cam)
    bpy.context.scene.camera = cam


def make_aov_writer_material():
    mat = bpy.data.materials.new("BackAOVWriter")
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    links = mat.node_tree.links
    nodes.clear()

    out = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    links.new(emission.outputs["Emission"], out.inputs["Surface"])

    aov_out = nodes.new("ShaderNodeOutputAOV")
    aov_out.aov_name = AOV_NAME
    aov_out.inputs["Color"].default_value = (1.0, 0.0, 0.0, 1.0)
    return mat


def make_front_material(use_stencil):
    mat = bpy.data.materials.new("FrontStencilReadOnly" if use_stencil else "FrontPlain")
    mat.use_nodes = True
    mat.surface_render_method = "DITHERED"
    if hasattr(mat, "use_screen_refraction"):
        mat.use_screen_refraction = True
    if hasattr(mat, "use_raytrace_refraction"):
        mat.use_raytrace_refraction = True
    mat.use_stencil = use_stencil
    if use_stencil:
        mat.stencil_test = "ALWAYS"
        mat.stencil_pass_op = "KEEP"
        mat.stencil_fail_op = "KEEP"
        mat.stencil_zfail_op = "KEEP"
        mat.stencil_reference = 1
        mat.stencil_read_mask = 15
        mat.stencil_write_mask = 15

    nodes = mat.node_tree.nodes
    links = mat.node_tree.links
    nodes.clear()
    out = nodes.new("ShaderNodeOutputMaterial")
    bsdf = nodes.new("ShaderNodeBsdfPrincipled")
    bsdf.inputs["Base Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    bsdf.inputs["Alpha"].default_value = 1.0
    if "Transmission Weight" in bsdf.inputs:
        bsdf.inputs["Transmission Weight"].default_value = 1.0
    if "Roughness" in bsdf.inputs:
        bsdf.inputs["Roughness"].default_value = 0.0
    links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    return mat


def make_filter_material():
    mat = bpy.data.materials.new("ShowAOV")
    mat.use_nodes = True
    mat.eevee_domain = "FILTER"
    nodes = mat.node_tree.nodes
    links = mat.node_tree.links
    nodes.clear()
    inp = nodes.new("ShaderNodeFilterGraphInput")
    out = nodes.new("ShaderNodeOutputFilter")
    out.inputs["Alpha"].default_value = 1.0
    links.new(inp.outputs["Image"], out.inputs["Color"])
    return mat


def add_plane(name, size, z, material):
    bpy.ops.mesh.primitive_plane_add(size=size, location=(0.0, 0.0, z))
    obj = bpy.context.object
    obj.name = name
    obj.data.materials.append(material)
    return obj


def render_pixels(name, use_stencil):
    clear_scene()
    configure_scene()
    make_camera()
    add_plane("BackAOV", 4.0, 0.0, make_aov_writer_material())
    add_plane("Front", 2.0, 1.0, make_front_material(use_stencil))
    attach_filter_material(make_filter_material(), stage="BEFORE_COMPOSITE", aov_name=AOV_NAME)
    bpy.context.view_layer.update()

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    path = OUT_DIR / f"{name}.exr"
    if path.exists():
        path.unlink()
    scene = bpy.context.scene
    scene.render.image_settings.file_format = "OPEN_EXR"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=False)
    bpy.data.images["Render Result"].save_render(str(path))
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        return list(image.pixels[:])
    finally:
        bpy.data.images.remove(image)


def sample(pixels, x, y):
    idx = (y * RESOLUTION + x) * 4
    return list(pixels[idx:idx + 4])


def assert_red(label, color):
    assert color[0] > 0.8 and color[1] < 0.1 and color[2] < 0.1, (
        f"{label} should show the red back-layer AOV, got {color}"
    )


def assert_black(label, color):
    assert max(color[0], color[1], color[2]) < 0.1, (
        f"{label} should have the stale AOV cleared to black, got {color}"
    )


def main():
    plain = render_pixels("plain_front_aov_clear", False)
    stencil = render_pixels("stencil_front_aov_clear", True)
    center = (RESOLUTION // 2, RESOLUTION // 2)
    corner = (RESOLUTION // 8, RESOLUTION // 8)

    plain_center = sample(plain, *center)
    plain_corner = sample(plain, *corner)
    stencil_center = sample(stencil, *center)
    stencil_corner = sample(stencil, *corner)

    print(f"PLAIN_CENTER={plain_center} PLAIN_CORNER={plain_corner}")
    print(f"STENCIL_CENTER={stencil_center} STENCIL_CORNER={stencil_corner}")

    assert_black("plain center", plain_center)
    assert_red("plain corner", plain_corner)
    assert_black("stencil center", stencil_center)
    assert_red("stencil corner", stencil_corner)
    print("EEVEE_MATERIAL_STENCIL_AOV_CLEAR_OK")


if __name__ == "__main__":
    main()
