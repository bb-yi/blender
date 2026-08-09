from pathlib import Path
import runpy

import bpy


CASE_DIR = Path(__file__).resolve().parents[1]
ROOT = CASE_DIR.parents[3]
TEST_SCRIPT = ROOT / "blender_5_1_port" / "tests" / "python" / "npr" / "test_new_shader_nodes_render.py"
ASSETS_DIR = CASE_DIR / "assets"
OUT_DIR = CASE_DIR / "out" / "asset-previews"


module = runpy.run_path(str(TEST_SCRIPT))
module["RESOLUTION"] = 256

clear_scene = module["clear_scene"]
configure_scene = module["configure_scene"]
make_camera = module["make_camera"]
make_plane = module["make_plane"]
make_positioned_plane = module["make_positioned_plane"]
make_uv_plane = module["make_uv_plane"]
make_emission_material = module["make_emission_material"]
make_twirl_material = module["make_twirl_material"]
make_water_material = module["make_water_material"]
make_hexagon_material = module["make_hexagon_material"]
make_sdf_material = module["make_sdf_material"]
make_sdf_vector_material = module["make_sdf_vector_material"]
make_basis_transform_material = module["make_basis_transform_material"]
make_scene_time_material = module["make_scene_time_material"]
make_world_to_tangent_material = module["make_world_to_tangent_material"]
make_portal_material = module["make_portal_material"]
make_aov_writer_material = module["make_aov_writer_material"]
make_aov_filter_material = module["make_aov_filter_material"]
attach_filter_material = module["attach_filter_material"]
make_render_texture_material = module["make_render_texture_material"]
make_outline_material = module["make_outline_material"]
make_foreach_light_npr_material = module["make_foreach_light_npr_material"]
ensure_view_layer_aov = module["ensure_view_layer_aov"]
make_point_light = module["make_point_light"]


def ensure_dir(path):
    path.mkdir(parents=True, exist_ok=True)


def save_project(slug):
    asset_dir = ASSETS_DIR / slug
    ensure_dir(asset_dir)
    bpy.ops.wm.save_as_mainfile(filepath=str(asset_dir / "scene.blend"), copy=False)


def render_preview(slug):
    ensure_dir(OUT_DIR)
    scene = bpy.context.scene
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "8"
    scene.render.filepath = str(OUT_DIR / f"{slug}.png")
    bpy.ops.render.render(write_still=True)


def finalize(slug):
    bpy.context.view_layer.update()
    render_preview(slug)
    save_project(slug)


def build_twirl():
    clear_scene()
    configure_scene()
    make_camera(scale=4.5)
    make_positioned_plane(make_twirl_material(0.0), "TwirlBasePlane", (-1.25, 0.0, 0.0), size=1.8)
    make_positioned_plane(make_twirl_material(8.0), "TwirlEffectPlane", (1.25, 0.0, 0.0), size=1.8)
    finalize("010-twirl")


def build_water_ripples():
    clear_scene()
    configure_scene(frame=12)
    make_camera(scale=7.0)
    configs = [
        ("DROPS", (-1.8, 1.8, 0.0)),
        ("RIPPLES", (1.8, 1.8, 0.0)),
        ("FLOW", (-1.8, -1.8, 0.0)),
        ("CAUSTIC", (1.8, -1.8, 0.0)),
    ]
    for mode, location in configs:
        make_positioned_plane(make_water_material(mode, 0.35), f"Water{mode}Plane", location, size=2.2)
    finalize("020-water-ripples")


def build_hex_grid():
    clear_scene()
    configure_scene()
    make_camera(scale=4.5)
    make_positioned_plane(make_hexagon_material("HEX", use_color=False), "HexValuePlane", (-1.25, 0.0, 0.0), size=1.8)
    make_positioned_plane(make_hexagon_material("HEX", use_color=True), "HexColorPlane", (1.25, 0.0, 0.0), size=1.8)
    finalize("030-hex-grid-texture")


def build_sdf_primitive_operator():
    clear_scene()
    configure_scene()
    make_camera(scale=4.5)
    make_positioned_plane(make_sdf_material(0.0), "SDFBasePlane", (-1.25, 0.0, 0.0), size=1.8)
    make_positioned_plane(make_sdf_material(0.22), "SDFDilatedPlane", (1.25, 0.0, 0.0), size=1.8)
    finalize("040-sdf-primitive-operator")


def build_sdf_vector_operator():
    clear_scene()
    configure_scene()
    make_camera(scale=2.5)
    make_plane(make_sdf_vector_material())
    finalize("050-sdf-vector-operator")


def build_basis_transform():
    clear_scene()
    configure_scene()
    make_camera(scale=4.5)
    make_positioned_plane(make_basis_transform_material("TO_BASIS"), "BasisToPlane", (-1.25, 0.0, 0.0), size=1.8)
    make_positioned_plane(make_basis_transform_material("FROM_BASIS"), "BasisFromPlane", (1.25, 0.0, 0.0), size=1.8)
    finalize("060-basis-transform")


def build_scene_time():
    clear_scene()
    configure_scene(frame=20)
    make_camera(scale=2.5)
    make_plane(make_scene_time_material(40.0))
    finalize("070-scene-time")


def build_world_to_tangent():
    clear_scene()
    configure_scene()
    make_camera(scale=2.5)
    make_uv_plane(make_world_to_tangent_material())
    finalize("080-world-to-tangent")


def build_portal():
    clear_scene()
    configure_scene()
    make_camera(scale=2.5)
    make_plane(make_portal_material())
    finalize("090-portal-in-out")


def build_aov():
    clear_scene()
    configure_scene()
    make_camera(scale=2.5)
    ensure_view_layer_aov("NewNodeAOVColor", "COLOR")
    ensure_view_layer_aov("NewNodeAOVValue", "VALUE")
    make_plane(make_aov_writer_material())
    attach_filter_material(make_aov_filter_material("NewNodeAOVColor", "Color"))
    finalize("100-aov-input-output")


def build_render_texture():
    clear_scene()
    configure_scene()
    make_camera(scale=2.0)

    capture_camera_data = bpy.data.cameras.new("RenderTextureCaptureCamera")
    capture_camera_data.type = "ORTHO"
    capture_camera_data.ortho_scale = 2.0
    capture_camera = bpy.data.objects.new("RenderTextureCaptureCamera", capture_camera_data)
    capture_camera.location = (3.0, 0.0, 4.0)
    bpy.context.scene.collection.objects.link(capture_camera)

    render_texture = bpy.context.scene.eevee.render_textures.add()
    render_texture.name = "RenderTextureCase"
    render_texture.enabled = True
    render_texture.camera = capture_camera
    render_texture.source = "COLOR"
    render_texture.resolution_x = 64
    render_texture.resolution_y = 64
    render_texture.update_mode = "EVERY_FRAME"
    render_texture.format = "RGBA16F"

    make_positioned_plane(
        make_emission_material("RenderTextureCaptureRed", (1.0, 0.0, 0.0, 1.0)),
        "RenderTextureCapturePlane",
        (3.0, 0.0, 0.0),
        size=1.5,
    )
    make_positioned_plane(
        make_render_texture_material(render_texture.uid),
        "RenderTextureDisplayPlane",
        (0.0, 0.0, 0.0),
        size=1.0,
    )
    finalize("110-render-texture")


def build_outline():
    clear_scene()
    configure_scene()
    bpy.context.scene.eevee.use_outline = True
    make_camera(scale=4.5)
    make_positioned_plane(make_outline_material(0.0), "OutlineOffPlane", (-1.25, 0.0, 0.0), size=1.8)
    make_positioned_plane(make_outline_material(6.0), "OutlineOnPlane", (1.25, 0.0, 0.0), size=1.8)
    finalize("120-outline-control")


def build_foreach_light():
    clear_scene()
    configure_scene()
    make_camera(scale=2.5)
    make_plane(make_foreach_light_npr_material())
    make_point_light("ForeachRed", (-0.8, -0.4, 1.6), (1.0, 0.0, 0.0))
    make_point_light("ForeachGreen", (0.8, 0.4, 1.6), (0.0, 1.0, 0.0))
    finalize("130-foreach-light")


def main():
    build_twirl()
    build_water_ripples()
    build_hex_grid()
    build_sdf_primitive_operator()
    build_sdf_vector_operator()
    build_basis_transform()
    build_scene_time()
    build_world_to_tangent()
    build_portal()
    build_aov()
    build_render_texture()
    build_outline()
    build_foreach_light()


if __name__ == "__main__":
    main()
