import json
from pathlib import Path

import bpy


CASE_DIR = Path(__file__).resolve().parent
OUT_DIR = CASE_DIR / "out"
RESOLUTION = 96

CHANNEL_BLUE = 2


def assert_true(condition, message):
    if not condition:
        raise AssertionError(message)


def set_if_available(owner, name, value):
    if hasattr(owner, name):
        setattr(owner, name, value)


def select_eevee_engine(scene):
    engines = {item.identifier for item in scene.render.bl_rna.properties["engine"].enum_items}
    for engine in ("BLENDER_EEVEE_NEXT", "BLENDER_EEVEE"):
        if engine in engines:
            scene.render.engine = engine
            return engine
    raise AssertionError(f"No Eevee render engine found in {sorted(engines)}")


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()
    for collection in (
        bpy.data.meshes,
        bpy.data.materials,
        bpy.data.lights,
        bpy.data.cameras,
        bpy.data.images,
        bpy.data.worlds,
    ):
        for datablock in list(collection):
            if datablock.users == 0:
                collection.remove(datablock)


def configure_scene():
    clear_scene()
    scene = bpy.context.scene
    engine = select_eevee_engine(scene)
    scene.render.resolution_x = RESOLUTION
    scene.render.resolution_y = RESOLUTION
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.film_transparent = False
    scene.render.use_compositing = False
    if hasattr(scene, "compositing_node_group"):
        scene.compositing_node_group = None

    set_if_available(scene.eevee, "taa_samples", 1)
    set_if_available(scene.eevee, "taa_render_samples", 1)
    set_if_available(scene.eevee, "use_taa_reprojection", False)
    set_if_available(scene.eevee, "use_raytracing", False)
    set_if_available(scene.eevee, "use_outline", False)

    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0

    world = bpy.data.worlds.new("Material Stencil Portal World")
    world.color = (0.0, 0.0, 0.0)
    world.use_nodes = True
    nodes = world.node_tree.nodes
    links = world.node_tree.links
    nodes.clear()
    background = nodes.new("ShaderNodeBackground")
    background.inputs["Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    background.inputs["Strength"].default_value = 0.0
    output = nodes.new("ShaderNodeOutputWorld")
    links.new(background.outputs["Background"], output.inputs["Surface"])
    scene.world = world

    view_layer_eevee = getattr(bpy.context.view_layer, "eevee", None)
    if view_layer_eevee is not None:
        set_if_available(view_layer_eevee, "use_pass_outline", False)
        set_if_available(view_layer_eevee, "use_outline_in_combined", True)

    return engine


def setup_camera():
    bpy.ops.object.camera_add(location=(0.0, 0.0, 4.0), rotation=(0.0, 0.0, 0.0))
    camera = bpy.context.object
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = 3.0
    bpy.context.scene.camera = camera


def make_material(
    name,
    color,
    *,
    surface_render_method="DITHERED",
    color_write=True,
    depth_write=True,
    stencil=None,
):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    material.surface_render_method = surface_render_method
    material.use_color_write = color_write
    material.use_depth_write = depth_write

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = color
    emission.inputs["Strength"].default_value = 1.0
    output = nodes.new("ShaderNodeOutputMaterial")
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    if stencil is not None:
        material.use_stencil = True
        material.stencil_order = stencil.get("order", 0)
        material.stencil_reference = stencil.get("reference", 0)
        material.stencil_read_mask = stencil.get("read_mask", 15)
        material.stencil_write_mask = stencil.get("write_mask", 15)
        material.stencil_test = stencil.get("test", "ALWAYS")
        material.stencil_pass_op = stencil.get("pass_op", "KEEP")
        material.stencil_fail_op = stencil.get("fail_op", "KEEP")
        material.stencil_zfail_op = stencil.get("zfail_op", "KEEP")

    return material


def add_plane(name, location, material, size=2.0):
    bpy.ops.mesh.primitive_plane_add(size=size, location=location)
    obj = bpy.context.object
    obj.name = name
    obj.data.materials.append(material)
    return obj


def render_image(label):
    path = OUT_DIR / f"{label}.png"
    bpy.context.scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)
    assert_true(path.exists(), f"Render did not write {path}")
    return path


def load_pixels(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        return list(image.pixels[:]), int(image.size[0]), int(image.size[1])
    finally:
        bpy.data.images.remove(image)


def sample_rgb(path, x_ratio, y_ratio=0.5):
    pixels, width, height = load_pixels(path)
    x = max(0, min(width - 1, int(width * x_ratio)))
    y = max(0, min(height - 1, int(height * y_ratio)))
    index = (y * width + x) * 4
    return tuple(pixels[index : index + 3])


def dominant_channel(rgb):
    return max(range(3), key=lambda index: rgb[index])


def assert_channel(label, rgb, expected_channel):
    channel = dominant_channel(rgb)
    assert_true(
        channel == expected_channel and rgb[expected_channel] > 0.35,
        f"{label}: expected channel {expected_channel}, got rgb={rgb}",
    )


def assert_black(label, rgb):
    assert_true(
        max(rgb) < 0.08,
        f"{label}: expected black, got rgb={rgb}",
    )


def validate_stencil_portal():
    configure_scene()
    setup_camera()

    reader = make_material(
        "portal_reader_blue",
        (0.0, 0.0, 1.0, 1.0),
        stencil={
            "order": 0,
            "reference": 1,
            "test": "EQUAL",
            "pass_op": "KEEP",
            "write_mask": 0,
        },
    )
    writer = make_material(
        "portal_writer_mask",
        (1.0, 1.0, 1.0, 1.0),
        color_write=False,
        depth_write=False,
        stencil={
            "order": -1,
            "reference": 1,
            "test": "ALWAYS",
            "pass_op": "REPLACE",
        },
    )
    occluder = make_material(
        "portal_occluder_depth_only",
        (1.0, 0.0, 0.0, 1.0),
        color_write=False,
        depth_write=True,
    )

    add_plane("reader_blue", (0.0, 0.0, 0.0), reader, size=2.6)
    add_plane("writer_stencil", (0.0, 0.0, 1.0), writer, size=2.6)
    add_plane("right_half_depth_occluder", (0.72, 0.0, 2.0), occluder, size=1.35)

    path = render_image("stencil_portal")
    left_rgb = sample_rgb(path, 0.25)
    right_rgb = sample_rgb(path, 0.75)

    assert_channel("left half opened by stencil writer", left_rgb, CHANNEL_BLUE)
    assert_black("right half depth occluder blocks stencil writer", right_rgb)
    return {
        "path": str(path),
        "left_rgb": list(left_rgb),
        "right_rgb": list(right_rgb),
    }


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    probe = bpy.data.materials.new("RNA Stencil Portal Probe")
    for attr in (
        "use_color_write",
        "use_depth_write",
        "use_stencil",
        "stencil_order",
        "stencil_reference",
        "stencil_read_mask",
        "stencil_write_mask",
        "stencil_test",
        "stencil_pass_op",
        "stencil_fail_op",
        "stencil_zfail_op",
    ):
        assert_true(hasattr(probe, attr), f"Missing material RNA property: {attr}")
    bpy.data.materials.remove(probe)

    engine = configure_scene()
    result = validate_stencil_portal()
    summary = {
        "status": "PASS",
        "engine": engine,
        "portal": result,
    }
    summary_path = OUT_DIR / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"MATERIAL_STENCIL_PORTAL_SUMMARY={summary_path}")


if __name__ == "__main__":
    main()
