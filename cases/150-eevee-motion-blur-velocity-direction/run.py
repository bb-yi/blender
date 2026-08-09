import json
import math
from pathlib import Path

import bpy
from mathutils import Vector


CASE_DIR = Path(__file__).resolve().parent
OUT_DIR = CASE_DIR / "out"
SUMMARY_PATH = OUT_DIR / "summary.json"
RESOLUTION_X = 512
RESOLUTION_Y = 384
FRAME = 13

ROI_STATIC_LEFT = (0.00, 0.20, 0.32, 0.80)
ROI_MOVING_CENTER = (0.36, 0.20, 0.84, 0.86)
ROI_BACKGROUND_RIGHT = (0.86, 0.00, 1.00, 1.00)


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


def look_at(obj, target):
    direction = Vector(target) - obj.location
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


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
    scene.frame_start = 1
    scene.frame_end = 25
    scene.render.fps = 24
    scene.frame_set(FRAME)

    scene.render.resolution_x = RESOLUTION_X
    scene.render.resolution_y = RESOLUTION_Y
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.film_transparent = False
    scene.render.use_compositing = False
    if hasattr(scene, "compositing_node_group"):
        scene.compositing_node_group = None

    scene.render.motion_blur_shutter = 0.8
    set_if_available(scene.eevee, "motion_blur_steps", 1)
    set_if_available(scene.eevee, "taa_render_samples", 32)
    set_if_available(scene.eevee, "taa_samples", 32)
    set_if_available(scene.eevee, "use_taa_reprojection", False)
    set_if_available(scene.eevee, "use_raytracing", False)
    set_if_available(scene.eevee, "use_outline", False)

    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "Medium High Contrast"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0

    world = bpy.data.worlds.new("Motion Blur Velocity World")
    world.color = (1.0, 1.0, 1.0)
    scene.world = world
    return engine


def set_linear_keyframes(obj, data_path):
    obj.keyframe_insert(data_path=data_path, frame=1)
    obj.location.x = 1.8
    obj.keyframe_insert(data_path=data_path, frame=25)
    action = obj.animation_data.action
    fcurves = getattr(action, "fcurves", None)
    if fcurves is None and hasattr(action, "layers"):
        fcurves = []
        for layer in action.layers:
            for strip in layer.strips:
                for channelbag in strip.channelbags:
                    fcurves.extend(channelbag.fcurves)
    for fcurve in fcurves or []:
        for key in fcurve.keyframe_points:
            key.interpolation = "LINEAR"


def build_scene():
    material = bpy.data.materials.new("Shared Motion Material")
    material.use_nodes = True
    bsdf = material.node_tree.nodes.get("Principled BSDF")
    assert_true(bsdf is not None, "Expected default Principled BSDF")
    bsdf.inputs["Base Color"].default_value = (0.05, 0.35, 1.0, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.55

    bpy.ops.mesh.primitive_cube_add(size=1.3, location=(-2.6, 0.0, 0.0))
    static_cube = bpy.context.object
    static_cube.name = "static_shared_material_first"
    static_cube.data.materials.append(material)

    bpy.ops.mesh.primitive_uv_sphere_add(segments=48, ring_count=24, radius=0.75, location=(-1.2, 0.0, 0.0))
    moving_sphere = bpy.context.object
    moving_sphere.name = "moving_shared_material_second"
    moving_sphere.data.materials.append(material)
    set_linear_keyframes(moving_sphere, "location")

    bpy.ops.mesh.primitive_plane_add(size=7.0, location=(0.0, 0.0, -0.85))
    plane = bpy.context.object
    plane.name = "matte_ground"
    ground_mat = bpy.data.materials.new("Matte Ground Material")
    ground_mat.diffuse_color = (0.8, 0.8, 0.8, 1.0)
    plane.data.materials.append(ground_mat)

    bpy.ops.object.light_add(type="AREA", location=(0.0, -3.0, 5.0))
    light = bpy.context.object
    light.name = "soft_area"
    light.data.energy = 500.0
    light.data.size = 4.0

    bpy.ops.object.camera_add(location=(0.6, -6.0, 2.2))
    camera = bpy.context.object
    look_at(camera, (0.0, 0.0, 0.0))
    camera.data.lens = 55.0
    bpy.context.scene.camera = camera


def render_image(label, use_motion_blur):
    scene = bpy.context.scene
    scene.frame_set(FRAME)
    scene.render.use_motion_blur = use_motion_blur
    path = OUT_DIR / f"{label}.png"
    scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)
    assert_true(path.exists(), f"Render did not write {path}")
    return path


def load_pixels(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        return list(image.pixels[:]), int(image.size[0]), int(image.size[1])
    finally:
        bpy.data.images.remove(image)


def diff_metrics(path_a, path_b, roi):
    pixels_a, width, height = load_pixels(path_a)
    pixels_b, width_b, height_b = load_pixels(path_b)
    assert_true((width, height) == (width_b, height_b), "Render sizes differ")

    x0 = int(width * roi[0])
    y0 = int(height * roi[1])
    x1 = int(width * roi[2])
    y1 = int(height * roi[3])
    count = 0
    gt3 = 0
    diff_sum = 0.0
    diff_max = 0.0
    for y in range(y0, y1):
        for x in range(x0, x1):
            index = (y * width + x) * 4
            diff = max(abs(pixels_a[index + channel] - pixels_b[index + channel]) for channel in range(3))
            diff_sum += diff
            diff_max = max(diff_max, diff)
            if diff > (3.0 / 255.0):
                gt3 += 1
            count += 1

    return {
        "mean": diff_sum / max(count, 1),
        "max": diff_max,
        "ratio_gt3": gt3 / max(count, 1),
        "pixels": count,
    }


def validate_motion_blur():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for path in OUT_DIR.glob("*.png"):
        path.unlink()
    if SUMMARY_PATH.exists():
        SUMMARY_PATH.unlink()

    off_path = render_image("motion_blur_off", False)
    on_path = render_image("motion_blur_on", True)

    static_metrics = diff_metrics(on_path, off_path, ROI_STATIC_LEFT)
    moving_metrics = diff_metrics(on_path, off_path, ROI_MOVING_CENTER)
    background_metrics = diff_metrics(on_path, off_path, ROI_BACKGROUND_RIGHT)

    assert_true(
        moving_metrics["mean"] > 0.010,
        f"Moving object did not show a clear motion blur footprint: {moving_metrics}",
    )
    assert_true(
        moving_metrics["ratio_gt3"] > 0.10,
        f"Moving object blur footprint is too small: {moving_metrics}",
    )
    assert_true(
        static_metrics["mean"] < 0.003 and static_metrics["ratio_gt3"] < 0.02,
        f"Static object changed too much between motion on/off renders: {static_metrics}",
    )
    assert_true(
        background_metrics["mean"] < 0.003 and background_metrics["ratio_gt3"] < 0.02,
        f"Background changed too much between motion on/off renders: {background_metrics}",
    )
    assert_true(
        moving_metrics["mean"] > static_metrics["mean"] * 8.0,
        f"Motion blur leaked into the static object: moving={moving_metrics}, static={static_metrics}",
    )
    assert_true(
        moving_metrics["mean"] > background_metrics["mean"] * 8.0,
        f"Motion blur leaked into the background: moving={moving_metrics}, background={background_metrics}",
    )

    return {
        "motion_blur_on": str(on_path),
        "motion_blur_off": str(off_path),
        "static_left": static_metrics,
        "moving_center": moving_metrics,
        "background_right": background_metrics,
    }


def main():
    engine = configure_scene()
    build_scene()
    result = validate_motion_blur()
    summary = {
        "status": "PASS",
        "engine": engine,
        "frame": FRAME,
        "resolution": [RESOLUTION_X, RESOLUTION_Y],
        "motion_blur_shutter": bpy.context.scene.render.motion_blur_shutter,
        "result": result,
    }
    SUMMARY_PATH.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"EEVEE_MOTION_BLUR_VELOCITY_DIRECTION_SUMMARY={SUMMARY_PATH}")


if __name__ == "__main__":
    main()
