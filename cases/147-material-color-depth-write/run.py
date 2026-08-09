import json
from pathlib import Path

import bpy


CASE_DIR = Path(__file__).resolve().parent
OUT_DIR = CASE_DIR / "out"
RESOLUTION = 96

CHANNEL_RED = 0
CHANNEL_GREEN = 1
CHANNEL_BLUE = 2

CASES = [
    # (label, front_color_write, front_depth_write, expected: "red"|"blue"|"black")
    ("color_on_depth_on", True, True, "red"),
    ("color_off_depth_on", False, True, "black"),
    ("color_on_depth_off", True, False, "red"),
    ("color_off_depth_off", False, False, "blue"),
]


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


def configure_scene(resolution=RESOLUTION):
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
    set_if_available(scene.eevee, "use_outline", False)

    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0

    world = bpy.data.worlds.new("Material Write World")
    world.color = (0.0, 0.0, 0.0)
    world.use_nodes = True
    nodes = world.node_tree.nodes
    links = world.node_tree.links
    nodes.clear()
    bg = nodes.new("ShaderNodeBackground")
    bg.inputs["Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    bg.inputs["Strength"].default_value = 0.0
    out = nodes.new("ShaderNodeOutputWorld")
    links.new(bg.outputs["Background"], out.inputs["Surface"])
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


def make_material(name, color, color_write=True, depth_write=True):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    material.use_color_write = color_write
    material.use_depth_write = depth_write

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = color
    emission.inputs["Strength"].default_value = 1.0
    out = nodes.new("ShaderNodeOutputMaterial")
    links.new(emission.outputs["Emission"], out.inputs["Surface"])
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


def classify_color(rgb, threshold=0.12, dark_threshold=0.05):
    r, g, b = rgb
    if max(r, g, b) < dark_threshold:
        return "black"
    if r > threshold and r > b:
        return "red"
    if b > threshold and b > r:
        return "blue"
    return f"unknown(r={r:.3f},g={g:.3f},b={b:.3f})"


def validate_rna_and_persistence():
    material = bpy.data.materials.new("RNA Write")
    material.use_fake_user = True

    assert_true(
        material.use_color_write is True,
        f"default use_color_write was {material.use_color_write!r}",
    )
    assert_true(
        material.use_depth_write is True,
        f"default use_depth_write was {material.use_depth_write!r}",
    )

    for value in (False, True, False):
        material.use_color_write = value
        assert_true(material.use_color_write == value, f"failed to set use_color_write={value}")
    for value in (False, True, False):
        material.use_depth_write = value
        assert_true(material.use_depth_write == value, f"failed to set use_depth_write={value}")

    blend_path = OUT_DIR / "material_color_depth_write_persistence.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(blend_path))
    bpy.ops.wm.open_mainfile(filepath=str(blend_path))
    reopened = bpy.data.materials["RNA Write"]
    assert_true(
        reopened.use_color_write is False,
        f"persisted use_color_write was {reopened.use_color_write!r}",
    )
    assert_true(
        reopened.use_depth_write is False,
        f"persisted use_depth_write was {reopened.use_depth_write!r}",
    )
    return {
        "blend": str(blend_path),
        "use_color_write": reopened.use_color_write,
        "use_depth_write": reopened.use_depth_write,
    }


def validate_render_matrix():
    results = {}
    for label, color_write, depth_write, expected in CASES:
        configure_scene()
        setup_camera()
        # Back plane at z=0 always renders with defaults.
        add_plane(
            f"back_blue_{label}",
            0.0,
            make_material(f"back_blue_mat_{label}", (0.0, 0.0, 1.0, 1.0)),
        )
        # Front plane at z=1 has the configured write state.
        add_plane(
            f"front_red_{label}",
            1.0,
            make_material(
                f"front_red_mat_{label}",
                (1.0, 0.0, 0.0, 1.0),
                color_write=color_write,
                depth_write=depth_write,
            ),
        )
        path = render_image(label)
        rgb = sample_rgb(path)
        actual = classify_color(rgb)
        assert_true(
            actual == expected,
            f"{label}: expected center pixel {expected}, got {actual} rgb={rgb}",
        )
        results[label] = {
            "path": str(path),
            "rgb": list(rgb),
            "expected": expected,
            "actual": actual,
            "color_write": color_write,
            "depth_write": depth_write,
        }
    return results


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    configure_scene()
    rna_result = validate_rna_and_persistence()
    matrix_result = validate_render_matrix()

    summary = {
        "status": "PASS",
        "rna": rna_result,
        "matrix": matrix_result,
    }
    summary_path = OUT_DIR / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"MATERIAL_COLOR_DEPTH_WRITE_SUMMARY={summary_path}")


if __name__ == "__main__":
    main()
