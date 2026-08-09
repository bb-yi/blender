import json
from pathlib import Path

import bpy


CASE_DIR = Path(__file__).resolve().parent
OUT_DIR = CASE_DIR / "out"
ONE_SAMPLE_PATH = OUT_DIR / "triangle_1_sample.png"
SIXTY_FOUR_SAMPLE_PATH = OUT_DIR / "triangle_64_samples.png"
OVERLAY_PATH = OUT_DIR / "triangle_red_cyan_overlay.png"
SUMMARY_PATH = OUT_DIR / "summary.json"

RESOLUTION = 256
CENTROID_LIMIT_PIXELS = 0.10
MIN_FRACTIONAL_ALPHA_PIXELS = 500
MIN_CHANGED_ALPHA_PIXELS = 500
MAX_ALPHA_MASS_RELATIVE_DELTA = 0.005
ALPHA_EPSILON = 1.0 / 255.0


def clear_generated_outputs():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for path in (
        ONE_SAMPLE_PATH,
        SIXTY_FOUR_SAMPLE_PATH,
        OVERLAY_PATH,
        SUMMARY_PATH,
    ):
        if path.exists():
            path.unlink()


def create_emission_material():
    material = bpy.data.materials.new("FilmJitterAlignmentEmission")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    emission.inputs["Strength"].default_value = 1.0
    material.node_tree.links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def create_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = RESOLUTION
    scene.render.resolution_y = RESOLUTION
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "8"
    scene.render.film_transparent = True
    scene.render.use_compositing = False
    scene.render.use_sequencer = False
    scene.render.use_motion_blur = False
    scene.render.filter_size = 1.5

    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0

    scene.eevee.use_taa_reprojection = False
    if hasattr(scene.eevee, "use_raytracing"):
        scene.eevee.use_raytracing = False
    if hasattr(scene.eevee, "use_outline"):
        scene.eevee.use_outline = False

    world = bpy.data.worlds.new("FilmJitterAlignmentWorld")
    world.color = (0.0, 0.0, 0.0)
    scene.world = world

    mesh = bpy.data.meshes.new("FilmJitterAlignmentTriangleMesh")
    mesh.from_pydata(
        [
            (-1.35, -1.15, 0.0),
            (1.20, -0.62, 0.0),
            (-0.28, 1.42, 0.0),
        ],
        [],
        [(0, 1, 2)],
    )
    mesh.update()

    triangle = bpy.data.objects.new("FilmJitterAlignmentTriangle", mesh)
    bpy.context.collection.objects.link(triangle)
    triangle.data.materials.append(create_emission_material())

    bpy.ops.object.camera_add(location=(0.0, 0.0, 5.0))
    camera = bpy.context.object
    camera.name = "FilmJitterAlignmentCamera"
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = 4.0
    scene.camera = camera
    return scene


def render_samples(scene, sample_count, output_path):
    scene.eevee.taa_render_samples = sample_count
    scene.render.filepath = str(output_path)
    result = bpy.ops.render.render(write_still=True)
    if result != {"FINISHED"}:
        raise AssertionError(f"Eevee render failed for {sample_count} samples: {result}")
    if not output_path.exists():
        raise AssertionError(f"Eevee render did not create {output_path}")


def load_pixels(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        image.colorspace_settings.name = "Non-Color"
        return list(image.pixels[:]), int(image.size[0]), int(image.size[1])
    finally:
        bpy.data.images.remove(image)


def alpha_metrics(pixels, width, height):
    alpha_mass = 0.0
    weighted_x = 0.0
    weighted_y = 0.0
    fractional_pixels = 0
    foreground_pixels = 0
    min_x = width
    min_y = height
    max_x = -1
    max_y = -1

    for y in range(height):
        for x in range(width):
            alpha = pixels[(y * width + x) * 4 + 3]
            alpha_mass += alpha
            weighted_x += x * alpha
            weighted_y += y * alpha
            if ALPHA_EPSILON < alpha < (1.0 - ALPHA_EPSILON):
                fractional_pixels += 1
            if alpha > ALPHA_EPSILON:
                foreground_pixels += 1
                min_x = min(min_x, x)
                min_y = min(min_y, y)
                max_x = max(max_x, x)
                max_y = max(max_y, y)

    if alpha_mass <= 0.0 or foreground_pixels == 0:
        raise AssertionError("Rendered triangle has no alpha coverage")

    return {
        "centroid": [weighted_x / alpha_mass, weighted_y / alpha_mass],
        "alpha_mass": alpha_mass,
        "fractional_alpha_pixels": fractional_pixels,
        "foreground_pixels": foreground_pixels,
        "bbox": [min_x, min_y, max_x, max_y],
    }


def create_red_cyan_overlay(one_pixels, many_pixels, width, height):
    overlay_pixels = [0.0] * (width * height * 4)
    for pixel_index in range(width * height):
        source_index = pixel_index * 4
        one_alpha = one_pixels[source_index + 3]
        many_alpha = many_pixels[source_index + 3]
        overlay_pixels[source_index] = one_alpha
        overlay_pixels[source_index + 1] = many_alpha
        overlay_pixels[source_index + 2] = many_alpha
        overlay_pixels[source_index + 3] = 1.0

    image = bpy.data.images.new(
        "FilmJitterAlignmentOverlay",
        width=width,
        height=height,
        alpha=True,
        float_buffer=False,
    )
    try:
        image.colorspace_settings.name = "Non-Color"
        image.pixels.foreach_set(overlay_pixels)
        image.filepath_raw = str(OVERLAY_PATH)
        image.file_format = "PNG"
        image.save()
    finally:
        bpy.data.images.remove(image)

    if not OVERLAY_PATH.exists():
        raise AssertionError(f"Overlay image was not created: {OVERLAY_PATH}")


def validate_alignment():
    scene = create_scene()
    render_samples(scene, 1, ONE_SAMPLE_PATH)
    render_samples(scene, 64, SIXTY_FOUR_SAMPLE_PATH)

    one_pixels, width, height = load_pixels(ONE_SAMPLE_PATH)
    many_pixels, many_width, many_height = load_pixels(SIXTY_FOUR_SAMPLE_PATH)
    if (width, height) != (many_width, many_height):
        raise AssertionError(
            f"Render sizes differ: one={width}x{height}, many={many_width}x{many_height}"
        )
    if (width, height) != (RESOLUTION, RESOLUTION):
        raise AssertionError(f"Unexpected render size: {width}x{height}")

    one_metrics = alpha_metrics(one_pixels, width, height)
    many_metrics = alpha_metrics(many_pixels, width, height)
    create_red_cyan_overlay(one_pixels, many_pixels, width, height)

    delta_x = many_metrics["centroid"][0] - one_metrics["centroid"][0]
    delta_y = many_metrics["centroid"][1] - one_metrics["centroid"][1]
    mass_relative_delta = abs(
        many_metrics["alpha_mass"] - one_metrics["alpha_mass"]
    ) / one_metrics["alpha_mass"]
    changed_alpha_pixels = sum(
        1
        for pixel_index in range(width * height)
        if abs(
            one_pixels[pixel_index * 4 + 3] - many_pixels[pixel_index * 4 + 3]
        )
        > ALPHA_EPSILON
    )

    failures = []

    def require(condition, message):
        if not condition:
            failures.append(message)

    require(
        one_metrics["alpha_mass"] > 5000.0,
        f"Triangle foreground is too small: alpha_mass={one_metrics['alpha_mass']:.6f}",
    )
    require(
        all(
            (
                one_metrics["bbox"][0] > 8,
                one_metrics["bbox"][1] > 8,
                one_metrics["bbox"][2] < width - 9,
                one_metrics["bbox"][3] < height - 9,
            )
        ),
        f"Triangle touches the image boundary: bbox={one_metrics['bbox']}",
    )
    require(
        many_metrics["fractional_alpha_pixels"] >= MIN_FRACTIONAL_ALPHA_PIXELS,
        "64-sample render lacks reconstructed edge coverage: "
        f"fractional={many_metrics['fractional_alpha_pixels']}",
    )
    require(
        changed_alpha_pixels >= MIN_CHANGED_ALPHA_PIXELS,
        f"1-sample and 64-sample alpha are too similar: changed={changed_alpha_pixels}",
    )
    require(
        mass_relative_delta < MAX_ALPHA_MASS_RELATIVE_DELTA,
        f"Alpha mass changed too much: relative_delta={mass_relative_delta:.6f}",
    )
    require(
        abs(delta_x) < CENTROID_LIMIT_PIXELS,
        f"Film jitter left an X offset of {delta_x:+.6f} pixels",
    )
    require(
        abs(delta_y) < CENTROID_LIMIT_PIXELS,
        f"Film jitter left a Y offset of {delta_y:+.6f} pixels",
    )

    build_hash = bpy.app.build_hash
    if isinstance(build_hash, bytes):
        build_hash = build_hash.decode("ascii", errors="replace")

    summary = {
        "status": "FAIL" if failures else "PASS",
        "build_hash": build_hash,
        "resolution": [width, height],
        "sample_counts": [1, 64],
        "centroid_limit_pixels": CENTROID_LIMIT_PIXELS,
        "one_sample": one_metrics,
        "sixty_four_samples": many_metrics,
        "centroid_delta_pixels": [delta_x, delta_y],
        "alpha_mass_relative_delta": mass_relative_delta,
        "changed_alpha_pixels_gt_1_over_255": changed_alpha_pixels,
        "outputs": {
            "one_sample": str(ONE_SAMPLE_PATH),
            "sixty_four_samples": str(SIXTY_FOUR_SAMPLE_PATH),
            "red_cyan_overlay": str(OVERLAY_PATH),
        },
        "failures": failures,
    }
    SUMMARY_PATH.write_text(json.dumps(summary, indent=2), encoding="utf-8")

    print(f"EEVEE_FILM_JITTER_CENTROID_DELTA_X={delta_x:+.6f}")
    print(f"EEVEE_FILM_JITTER_CENTROID_DELTA_Y={delta_y:+.6f}")
    print(
        "EEVEE_FILM_JITTER_FRACTIONAL_ALPHA_PIXELS="
        f"{many_metrics['fractional_alpha_pixels']}"
    )
    print(f"EEVEE_FILM_JITTER_CHANGED_ALPHA_PIXELS={changed_alpha_pixels}")
    print(f"EEVEE_FILM_JITTER_ALPHA_MASS_RELATIVE_DELTA={mass_relative_delta:.6f}")
    print(f"EEVEE_FILM_JITTER_SUMMARY={SUMMARY_PATH}")
    print(f"EEVEE_FILM_JITTER_OVERLAY={OVERLAY_PATH}")

    if failures:
        raise AssertionError("; ".join(failures))


def main():
    clear_generated_outputs()
    validate_alignment()
    print("EEVEE_FILM_JITTER_RECONSTRUCTION_ALIGNMENT_OK")


if __name__ == "__main__":
    main()
