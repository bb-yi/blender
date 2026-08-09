from pathlib import Path

import bpy


CASE_DIR = Path(__file__).resolve().parent
ROOT = CASE_DIR.parents[3]
OUTPUT_DIR = ROOT / "temp" / "release_test_outputs" / "eevee_material_ao_temporal_sampling"
ONE_SAMPLE_PATH = OUTPUT_DIR / "ao_1_sample.png"
SIXTY_FOUR_SAMPLE_PATH = OUTPUT_DIR / "ao_64_samples.png"


def create_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.eevee.use_taa_reprojection = True
    scene.render.resolution_x = 960
    scene.render.resolution_y = 540
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGB"
    scene.render.image_settings.color_depth = "8"
    scene.render.film_transparent = True
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    scene.world.color = (0.050876, 0.050876, 0.050876)

    bpy.ops.mesh.primitive_plane_add(size=2.0)
    plane = bpy.context.object
    plane.name = "AORegressionPlane"

    bpy.ops.mesh.primitive_monkey_add(location=(0.0, 0.0, 0.268133))
    suzanne = bpy.context.object
    suzanne.name = "AORegressionSuzanne"
    suzanne.rotation_euler = (-0.645249, 0.0, 0.0)
    suzanne.scale = (0.409391, 0.409391, 0.409391)
    for polygon in suzanne.data.polygons:
        polygon.use_smooth = True
    subdivision = suzanne.modifiers.new("Subdivision", "SUBSURF")
    subdivision.subdivision_type = "CATMULL_CLARK"
    subdivision.levels = 3
    subdivision.render_levels = 2

    material = bpy.data.materials.new("AORegressionMaterial")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    nodes.clear()
    output = nodes.new("ShaderNodeOutputMaterial")
    ambient_occlusion = nodes.new("ShaderNodeAmbientOcclusion")
    ambient_occlusion.samples = 16
    ambient_occlusion.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    ambient_occlusion.inputs["Distance"].default_value = 1.0
    power = nodes.new("ShaderNodeMath")
    power.operation = "POWER"
    power.inputs[1].default_value = 20.0
    material.node_tree.links.new(ambient_occlusion.outputs["Color"], power.inputs[0])
    material.node_tree.links.new(power.outputs[0], output.inputs["Surface"])
    suzanne.data.materials.append(material)

    bpy.ops.object.camera_add(location=(2.194889, -1.756353, 1.221310))
    camera = bpy.context.object
    camera.name = "AORegressionCamera"
    camera.rotation_euler = (1.242676, 0.0, 0.858703)
    camera.data.type = "PERSP"
    camera.data.lens = 50.0
    scene.camera = camera

    bpy.ops.object.light_add(type="AREA", location=(1.120336, 1.280069, 1.417600))
    key_light = bpy.context.object
    key_light.name = "AORegressionKey"
    key_light.rotation_euler = (0.0, 1.153450, 0.553961)
    key_light.scale = (2.336779, 2.336779, 2.336779)
    key_light.data.energy = 54.977871

    bpy.ops.object.light_add(type="AREA", location=(-1.779818, -0.980052, 1.417600))
    fill_light = bpy.context.object
    fill_light.name = "AORegressionFill"
    fill_light.rotation_euler = (0.629315, 1.281459, -1.792943)
    fill_light.scale = (1.711997, 1.711997, 1.711997)
    fill_light.data.energy = 35.342918

    return scene


def render_samples(scene, sample_count, output_path):
    scene.eevee.taa_render_samples = sample_count
    scene.render.filepath = str(output_path)
    result = bpy.ops.render.render(write_still=True)
    assert result == {"FINISHED"}, f"AO render failed for {sample_count} samples: {result}"
    assert output_path.exists(), f"AO render did not create {output_path}"


def load_pixels(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        image.colorspace_settings.name = "Non-Color"
        return list(image.pixels[:]), int(image.size[0]), int(image.size[1])
    finally:
        bpy.data.images.remove(image)


def luma(pixels, index):
    return 0.2126 * pixels[index] + 0.7152 * pixels[index + 1] + 0.0722 * pixels[index + 2]


def central_laplacian_energy(path):
    pixels, width, height = load_pixels(path)
    x0 = int(width * 0.33)
    x1 = int(width * 0.65)
    y0 = int(height * 0.52)
    y1 = int(height * 0.83)

    energy_sum = 0.0
    sample_count = 0
    luma_min = 1.0
    luma_max = 0.0
    for y in range(y0 + 1, y1 - 1):
        for x in range(x0 + 1, x1 - 1):
            center = luma(pixels, (y * width + x) * 4)
            if center <= 0.05 or center >= 0.98:
                continue
            left = luma(pixels, (y * width + x - 1) * 4)
            right = luma(pixels, (y * width + x + 1) * 4)
            down = luma(pixels, ((y - 1) * width + x) * 4)
            up = luma(pixels, ((y + 1) * width + x) * 4)
            energy_sum += abs(4.0 * center - left - right - down - up)
            sample_count += 1
            luma_min = min(luma_min, center)
            luma_max = max(luma_max, center)

    assert sample_count > 10000, (
        f"AO crop contains too few usable pixels: count={sample_count}, size={width}x{height}"
    )
    luma_range = luma_max - luma_min
    assert luma_range > 0.40, f"AO crop lacks expected contrast: range={luma_range:.6f}"
    return energy_sum / sample_count, sample_count, luma_range


OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
scene = create_scene()
render_samples(scene, 1, ONE_SAMPLE_PATH)
render_samples(scene, 64, SIXTY_FOUR_SAMPLE_PATH)

one_sample_energy, one_sample_pixels, one_sample_range = central_laplacian_energy(ONE_SAMPLE_PATH)
sixty_four_energy, sixty_four_pixels, sixty_four_range = central_laplacian_energy(
    SIXTY_FOUR_SAMPLE_PATH
)
convergence_ratio = sixty_four_energy / max(one_sample_energy, 1.0e-8)

print(f"EEVEE_AO_ONE_SAMPLE_ENERGY={one_sample_energy:.6f}")
print(f"EEVEE_AO_64_SAMPLE_ENERGY={sixty_four_energy:.6f}")
print(f"EEVEE_AO_CONVERGENCE_RATIO={convergence_ratio:.6f}")
print(f"EEVEE_AO_ONE_SAMPLE_PIXELS={one_sample_pixels}")
print(f"EEVEE_AO_64_SAMPLE_PIXELS={sixty_four_pixels}")
print(f"EEVEE_AO_ONE_SAMPLE_RANGE={one_sample_range:.6f}")
print(f"EEVEE_AO_64_SAMPLE_RANGE={sixty_four_range:.6f}")
print(f"EEVEE_AO_OUTPUT_DIR={OUTPUT_DIR}")

assert one_sample_energy > 0.15, (
    "The one-sample AO control is not noisy enough to exercise temporal convergence: "
    f"energy={one_sample_energy:.6f}"
)
assert sixty_four_energy < 0.06, (
    "The 64-sample AO render retains excessive high-frequency noise: "
    f"energy={sixty_four_energy:.6f}"
)
assert convergence_ratio < 0.12, (
    "The AO node did not converge sufficiently across Eevee render samples: "
    f"ratio={convergence_ratio:.6f}, one={one_sample_energy:.6f}, "
    f"sixty_four={sixty_four_energy:.6f}"
)

print("EEVEE_MATERIAL_AO_TEMPORAL_SAMPLING_OK")
