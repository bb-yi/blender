import json
from pathlib import Path

import bpy
import OpenImageIO as oiio


CASE_DIR = Path(__file__).resolve().parent
OUT_DIR = CASE_DIR / "out"
OUTPUT_PATH = OUT_DIR / "forward_blended_normal_pass.exr"
SUMMARY_PATH = OUT_DIR / "summary.json"


def set_if_available(owner, name, value):
    if hasattr(owner, name):
        setattr(owner, name, value)


def assert_true(condition, message):
    if not condition:
        raise AssertionError(message)


def configure_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 256
    scene.render.resolution_y = 256
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.eevee.taa_samples = 1
    scene.eevee.taa_render_samples = 1
    scene.eevee.use_taa_reprojection = False
    set_if_available(scene.eevee, "use_raytracing", False)
    set_if_available(scene.eevee, "use_outline", False)
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    scene.render.film_transparent = False

    view_layer = bpy.context.view_layer
    view_layer.use_pass_normal = True

    world = bpy.data.worlds.new("Forward Normal World")
    world.color = (0.0, 0.0, 0.0)
    scene.world = world

    bpy.ops.object.light_add(type="AREA", location=(0.0, -2.0, 3.0))
    light = bpy.context.object
    light.name = "Forward Normal Light"
    light.data.energy = 400.0
    light.data.size = 3.0

    bpy.ops.object.camera_add(location=(0.0, -4.0, 2.2), rotation=(1.1, 0.0, 0.0))
    camera = bpy.context.object
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = 3.0
    scene.camera = camera


def make_blended_material():
    material = bpy.data.materials.new("BLENDED Normal Pass")
    material.use_nodes = True
    material.surface_render_method = "BLENDED"
    set_if_available(material, "use_screen_refraction", False)
    set_if_available(material, "use_raytrace_refraction", False)

    bsdf = material.node_tree.nodes.get("Principled BSDF")
    assert_true(bsdf is not None, "Expected default Principled BSDF")
    bsdf.inputs["Base Color"].default_value = (0.7, 0.2, 0.2, 1.0)
    bsdf.inputs["Alpha"].default_value = 0.55
    if "Roughness" in bsdf.inputs:
        bsdf.inputs["Roughness"].default_value = 0.4
    return material


def build_scene():
    material = make_blended_material()
    bpy.ops.mesh.primitive_plane_add(size=2.2, location=(0.0, 0.0, 0.0), rotation=(0.55, 0.0, 0.0))
    plane = bpy.context.object
    plane.name = "Tilted BLENDED Normal Plane"
    plane.data.materials.append(material)


def render_normal_pass():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    scene = bpy.context.scene
    scene.use_nodes = True

    tree = bpy.data.node_groups.new("Forward Normal Pass Compositor", "CompositorNodeTree")
    scene.compositing_node_group = tree
    tree.nodes.clear()

    render_layers = tree.nodes.new("CompositorNodeRLayers")
    output = tree.nodes.new("CompositorNodeOutputFile")
    output.directory = str(OUT_DIR)
    output.file_name = OUTPUT_PATH.stem
    output.file_output_items.clear()
    output.file_output_items.new("RGBA", "Normal")
    tree.links.new(render_layers.outputs["Normal"], output.inputs["Normal"])

    for path in OUT_DIR.glob(f"{OUTPUT_PATH.stem}*"):
        path.unlink()

    bpy.ops.render.render()

    rendered_files = sorted(OUT_DIR.glob(f"{OUTPUT_PATH.stem}*.exr"))
    assert_true(rendered_files, "Compositor did not write the Normal pass EXR")
    rendered_files[-1].replace(OUTPUT_PATH)
    assert_true(OUTPUT_PATH.exists(), f"Render did not write {OUTPUT_PATH}")

    return load_pixels(OUTPUT_PATH)


def load_pixels(path):
    image_input = oiio.ImageInput.open(str(path))
    assert_true(image_input is not None, f"Could not open EXR output: {path}")
    try:
        spec = image_input.spec()
        pixels = image_input.read_image(format=oiio.FLOAT)
        assert_true(pixels is not None, f"Could not read EXR pixels: {path}")
        return pixels, int(spec.width), int(spec.height)
    finally:
        image_input.close()


def sample_rgb(pixels, width, height, x_ratio, y_ratio):
    x = max(0, min(width - 1, int(width * x_ratio)))
    y = max(0, min(height - 1, int(height * y_ratio)))
    return tuple(float(v) for v in pixels[y, x, :3])


def validate_output():
    pixels, width, height = render_normal_pass()
    center = sample_rgb(pixels, width, height, 0.5, 0.5)
    corner = sample_rgb(pixels, width, height, 0.05, 0.05)

    center_luma = max(center)
    corner_luma = max(corner)
    delta = sum(abs(center[i] - corner[i]) for i in range(3))

    assert_true(center_luma > 0.15, f"BLENDED material Normal pass was too dark: center={center}")
    assert_true(delta > 0.15, f"Normal pass did not separate material from background: center={center}, corner={corner}")

    summary = {
        "status": "PASS",
        "source": "Render Layers Normal pass via CompositorNodeOutputFile",
        "image": str(OUTPUT_PATH),
        "center_rgb": list(center),
        "corner_rgb": list(corner),
        "center_luma": center_luma,
        "corner_luma": corner_luma,
        "delta": delta,
    }
    SUMMARY_PATH.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"FORWARD_NORMAL_RENDER_PASS_SUMMARY={SUMMARY_PATH}")


def main():
    configure_scene()
    build_scene()
    validate_output()


if __name__ == "__main__":
    main()
