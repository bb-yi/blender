import json
from pathlib import Path

import bpy


CASE_DIR = Path(__file__).resolve().parent
OUT_DIR = CASE_DIR / "out"
RESOLUTION = 96
EXPECTED_ENUMS = {
    "LESS",
    "GREATER",
    "LESS_EQUAL",
    "GREATER_EQUAL",
    "EQUAL",
    "NOT_EQUAL",
    "ALWAYS",
    "NEVER",
}
EXPECTED_BACK_VISIBLE = {"GREATER", "GREATER_EQUAL", "NOT_EQUAL", "ALWAYS"}


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


def configure_scene(resolution=RESOLUTION, outline=False):
    clear_scene()
    scene = bpy.context.scene
    engine = select_eevee_engine(scene)
    scene.render.resolution_x = resolution
    scene.render.resolution_y = resolution
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
    set_if_available(scene.eevee, "use_outline", outline)

    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0

    world = bpy.data.worlds.new("Material ZTest World")
    world.color = (0.0, 0.0, 0.0)
    scene.world = world

    view_layer_eevee = getattr(bpy.context.view_layer, "eevee", None)
    if view_layer_eevee is not None:
        set_if_available(view_layer_eevee, "use_pass_outline", False)
        set_if_available(view_layer_eevee, "use_outline_in_combined", True)

    return engine


def setup_camera(scale=3.0):
    bpy.ops.object.camera_add(location=(0.0, 0.0, 4.0), rotation=(0.0, 0.0, 0.0))
    camera = bpy.context.object
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = scale
    bpy.context.scene.camera = camera

    bpy.ops.object.light_add(type="AREA", location=(0.0, 0.0, 3.0))
    light = bpy.context.object
    light.name = "Material ZTest Area Light"
    light.data.energy = 600.0
    light.data.size = 4.0


def make_material(name, color, ztest_mode="LESS_EQUAL", forward=False, outline=False):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    material.ztest_mode = ztest_mode
    if forward:
        material.surface_render_method = "BLENDED"
        set_if_available(material, "use_transparency_overlap", False)

    nodes = material.node_tree.nodes
    bsdf = nodes.get("Principled BSDF")
    if bsdf is not None:
        bsdf.inputs["Base Color"].default_value = color
        bsdf.inputs["Alpha"].default_value = color[3]
        if "Roughness" in bsdf.inputs:
            bsdf.inputs["Roughness"].default_value = 0.5

    if outline:
        outline_node = nodes.new("ShaderNodeOutlineControl")
        outline_node.inputs["Line Color"].default_value = (1.0, 1.0, 1.0, 1.0)
        outline_node.inputs["Line Alpha"].default_value = 1.0
        outline_node.inputs["Line Width"].default_value = 7.0
        outline_node.inputs["Depth Threshold"].default_value = 0.1
        outline_node.inputs["Normal Threshold"].default_value = 0.5
        outline_node.inputs["Outline ID"].default_value = 1
    return material


def add_plane(name, z, material, size=2.0):
    bpy.ops.mesh.primitive_plane_add(size=size, location=(0.0, 0.0, z))
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


def sample_rgb(path, x_ratio=0.5, y_ratio=0.5):
    pixels, width, height = load_pixels(path)
    x = max(0, min(width - 1, int(width * x_ratio)))
    y = max(0, min(height - 1, int(height * y_ratio)))
    index = (y * width + x) * 4
    return tuple(pixels[index : index + 3])


def dominant_channel(rgb):
    return max(range(3), key=lambda index: rgb[index])


def assert_color(label, rgb, expected_channel):
    channel = dominant_channel(rgb)
    assert_true(
        channel == expected_channel and rgb[expected_channel] > 0.12,
        f"{label}: expected dominant channel {expected_channel}, got rgb={rgb}",
    )


def count_bright_pixels(path, threshold=0.65):
    pixels, width, height = load_pixels(path)
    count = 0
    for index in range(0, len(pixels), 4):
        r, g, b = pixels[index : index + 3]
        if r > threshold and g > threshold and b > threshold:
            count += 1
    return {"path": str(path), "width": width, "height": height, "count": count}


def validate_rna_and_persistence():
    material = bpy.data.materials.new("RNA ZTest")
    material.use_fake_user = True
    assert_true(
        material.ztest_mode == "LESS_EQUAL",
        f"default ztest_mode was {material.ztest_mode!r}",
    )

    enum_items = {
        item.identifier
        for item in material.bl_rna.properties["ztest_mode"].enum_items
        if item.identifier
    }
    assert_true(
        EXPECTED_ENUMS.issubset(enum_items),
        f"missing enum items: {sorted(EXPECTED_ENUMS - enum_items)}",
    )

    for mode in sorted(EXPECTED_ENUMS):
        material.ztest_mode = mode
        assert_true(material.ztest_mode == mode, f"failed to set {mode}")

    blend_path = OUT_DIR / "material_ztest_persistence.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(blend_path))
    bpy.ops.wm.open_mainfile(filepath=str(blend_path))
    reopened = bpy.data.materials["RNA ZTest"]
    assert_true(
        reopened.ztest_mode == "NOT_EQUAL",
        f"persisted mode was {reopened.ztest_mode!r}",
    )
    return {"blend": str(blend_path), "persisted": reopened.ztest_mode}


def validate_gpu_python_api():
    import gpu

    doc = gpu.state.depth_test_set.__doc__ or ""
    assert_true("NOT_EQUAL" in doc, "gpu.state.depth_test_set docs do not expose NOT_EQUAL")


def validate_depth_matrix(label_prefix, forward):
    results = {}
    for mode in sorted(EXPECTED_ENUMS):
        configure_scene()
        setup_camera()
        add_plane(
            f"{label_prefix}_front_red_{mode}",
            1.0,
            make_material(f"{label_prefix}_front_red_mat_{mode}", (1.0, 0.0, 0.0, 1.0)),
        )
        add_plane(
            f"{label_prefix}_back_blue_{mode}",
            0.0,
            make_material(
                f"{label_prefix}_back_blue_mat_{mode}",
                (0.0, 0.0, 1.0, 1.0),
                mode,
                forward=forward,
            ),
        )
        path = render_image(f"{label_prefix}_{mode.lower()}")
        rgb = sample_rgb(path)
        expected_channel = 2 if mode in EXPECTED_BACK_VISIBLE else 0
        assert_color(f"{label_prefix} {mode}", rgb, expected_channel)
        results[mode] = {
            "path": str(path),
            "rgb": list(rgb),
            "expected_channel": expected_channel,
        }
    return results


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    configure_scene()
    rna_result = validate_rna_and_persistence()
    validate_gpu_python_api()
    opaque_results = validate_depth_matrix("opaque", forward=False)
    transparent_results = validate_depth_matrix("transparent", forward=True)

    summary = {
        "status": "PASS",
        "tested_modes": sorted(EXPECTED_ENUMS),
        "rna": rna_result,
        "opaque": opaque_results,
        "transparent": transparent_results,
    }
    summary_path = OUT_DIR / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"MATERIAL_ZTEST_SUMMARY={summary_path}")


if __name__ == "__main__":
    main()
