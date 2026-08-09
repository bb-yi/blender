import json
import math
from pathlib import Path

import bpy


CASE_DIR = Path(__file__).resolve().parent
OUT_DIR = CASE_DIR / "out"
RESOLUTION = 256


def assert_true(condition, message):
    if not condition:
        raise AssertionError(message)


def set_if_available(owner, name, value):
    if hasattr(owner, name):
        setattr(owner, name, value)


def set_input_default(node, name, value):
    socket = node.inputs.get(name)
    if socket is not None:
        socket.default_value = value


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
        bpy.data.images,
        bpy.data.lights,
        bpy.data.cameras,
        bpy.data.worlds,
    ):
        for datablock in list(collection):
            if datablock.users == 0:
                collection.remove(datablock)


def setup_render():
    clear_scene()
    scene = bpy.context.scene
    select_eevee_engine(scene)
    scene.render.resolution_x = RESOLUTION
    scene.render.resolution_y = RESOLUTION
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.film_transparent = False
    scene.render.use_compositing = False
    if hasattr(scene, "compositing_node_group"):
        scene.compositing_node_group = None

    set_if_available(scene.eevee, "taa_samples", 16)
    set_if_available(scene.eevee, "taa_render_samples", 16)
    set_if_available(scene.eevee, "use_taa_reprojection", False)
    set_if_available(scene.eevee, "use_raytracing", False)
    set_if_available(scene.eevee, "use_shadows", False)
    set_if_available(scene.eevee, "use_gtao", False)
    set_if_available(scene.eevee, "use_bloom", False)
    set_if_available(scene.eevee, "use_ssr", False)

    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0

    world = bpy.data.worlds.new("Depth Offset Lighting Toggle World")
    world.color = (0.0, 0.0, 0.0)
    scene.world = world


def add_camera():
    bpy.ops.object.camera_add(
        location=(0.2, -5.6, 1.6),
        rotation=(math.radians(76.0), 0.0, math.radians(2.0)),
    )
    camera = bpy.context.object
    camera.data.lens = 50.0
    bpy.context.scene.camera = camera


def add_light():
    bpy.ops.object.light_add(type="POINT", location=(2.2, -1.8, 2.4))
    light = bpy.context.object
    light.data.energy = 1800.0
    light.data.shadow_soft_size = 0.0
    light.data.use_shadow = False


def make_material(name, depth_offset, affect_lighting, force_forward):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    material.depth_offset_affect_lighting = affect_lighting
    if force_forward:
        material.surface_render_method = "BLENDED"

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    output = nodes.get("Material Output")
    principled = nodes.get("Principled BSDF")
    assert_true(output is not None, "Material Output node not found")
    assert_true(principled is not None, "Principled BSDF node not found")

    set_input_default(principled, "Base Color", (0.85, 0.85, 0.85, 1.0))
    set_input_default(principled, "Roughness", 0.2)
    set_input_default(principled, "Specular IOR Level", 0.5)

    if depth_offset is not None:
        value = nodes.new("ShaderNodeValue")
        value.outputs[0].default_value = depth_offset
        depth_socket = output.inputs.get("Depth Offset")
        assert_true(depth_socket is not None, "Material Output is missing Depth Offset input")
        links.new(value.outputs[0], depth_socket)
        assert_true(depth_socket.is_linked, "Depth Offset socket was not linked")

    return material


def add_subject(material):
    bpy.ops.mesh.primitive_monkey_add(location=(0.0, 0.0, 0.0), size=1.15)
    obj = bpy.context.object
    obj.rotation_euler = (math.radians(10.0), 0.0, math.radians(24.0))
    obj.data.materials.append(material)
    for polygon in obj.data.polygons:
        polygon.use_smooth = True


def build_scene(label, depth_offset, affect_lighting, force_forward):
    setup_render()
    add_camera()
    add_light()
    add_subject(
        make_material(
            f"depth_offset_lighting_{label}",
            depth_offset,
            affect_lighting,
            force_forward,
        )
    )


def render_pixels(label):
    path = OUT_DIR / f"{label}.png"
    bpy.context.scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)
    assert_true(path.exists(), f"Render did not write {path}")
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        return path, list(image.pixels[:])
    finally:
        bpy.data.images.remove(image)


def render_case(label, depth_offset, affect_lighting, force_forward):
    build_scene(label, depth_offset, affect_lighting, force_forward)
    return render_pixels(label)


def measure_difference(baseline_pixels, test_pixels):
    squared_error = 0.0
    max_error = 0.0
    sample_count = 0
    object_pixel_count = 0
    changed_pixel_count = 0

    for i in range(0, len(baseline_pixels), 4):
        baseline_luma = (
            baseline_pixels[i] * 0.2126
            + baseline_pixels[i + 1] * 0.7152
            + baseline_pixels[i + 2] * 0.0722
        )
        if baseline_luma < 0.02:
            continue

        object_pixel_count += 1
        pixel_max_error = 0.0
        for channel in range(3):
            delta = test_pixels[i + channel] - baseline_pixels[i + channel]
            squared_error += delta * delta
            pixel_max_error = max(pixel_max_error, abs(delta))
            sample_count += 1

        max_error = max(max_error, pixel_max_error)
        if pixel_max_error > 0.02:
            changed_pixel_count += 1

    rms = math.sqrt(squared_error / sample_count) if sample_count else 0.0
    changed_ratio = changed_pixel_count / object_pixel_count if object_pixel_count else 0.0
    return {"rms": rms, "max": max_error, "changed_ratio": changed_ratio}


def assert_lighting_toggle(prefix, force_forward):
    _, baseline = render_case(f"{prefix}_baseline", None, False, force_forward)
    off_path, off = render_case(f"{prefix}_offset_toggle_off", 2.0, False, force_forward)
    on_path, on = render_case(f"{prefix}_offset_toggle_on", 2.0, True, force_forward)

    off_diff = measure_difference(baseline, off)
    on_diff = measure_difference(baseline, on)

    assert_true(
        off_diff["rms"] <= 0.01 and off_diff["changed_ratio"] <= 0.01,
        f"{prefix}: disabled lighting toggle changed lighting too much: {off_diff}",
    )
    assert_true(
        on_diff["rms"] >= 0.02 and on_diff["changed_ratio"] >= 0.02,
        f"{prefix}: enabled lighting toggle did not measurably affect lighting: {on_diff}",
    )

    return {
        "off_path": str(off_path),
        "on_path": str(on_path),
        "off_diff": off_diff,
        "on_diff": on_diff,
    }


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    results = {
        "deferred": assert_lighting_toggle("deferred", False),
        "forward": assert_lighting_toggle("forward", True),
    }
    summary = {"status": "PASS", "results": results}
    summary_path = OUT_DIR / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"DEPTH_OFFSET_LIGHTING_POSITION_TOGGLE_SUMMARY={summary_path}")


if __name__ == "__main__":
    main()
