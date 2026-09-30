import argparse
import math
import os
from pathlib import Path
import sys

import bpy
from mathutils import Vector


RENDER_SIZE = 64
SUCCESS_MARKER = "PRINCIPLED_NPR_V1_OK"
BACKEND_MARKER = "PRINCIPLED_NPR_V1_BACKEND="
OUTPUT_DIR = None
LEGACY_TEMPLATE = None


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def socket_by_identifier(node, identifier):
    return next((socket for socket in node.inputs if socket.identifier == identifier), None)


def parse_args():
    argv = sys.argv
    argv = argv[argv.index("--") + 1 :] if "--" in argv else []
    parser = argparse.ArgumentParser()
    parser.add_argument("--expected-backend", required=True, choices=("OPENGL", "VULKAN"))
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--legacy-template", type=Path)
    return parser.parse_args(argv)


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)


def configure_scene():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = RENDER_SIZE
    scene.render.resolution_y = RENDER_SIZE
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "OPEN_EXR"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "32"
    scene.eevee.taa_samples = 1
    scene.eevee.taa_render_samples = 1
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    scene.world.use_nodes = False
    scene.world.color = (0.0, 0.0, 0.0)
    return scene


def make_camera():
    camera_data = bpy.data.cameras.new("PrincipledNPRCamera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 3.0
    camera = bpy.data.objects.new("PrincipledNPRCamera", camera_data)
    camera.location = (0.0, -6.0, 0.0)
    camera.rotation_euler = (
        Vector((0.0, 0.0, 0.0)) - camera.location
    ).to_track_quat("-Z", "Y").to_euler()
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera
    return camera


def make_material(name="PrincipledNPRV1Material"):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    if hasattr(material, "surface_render_method"):
        material.surface_render_method = "BLENDED"
    elif hasattr(material, "blend_method"):
        material.blend_method = "BLEND"
    tree = material.node_tree
    tree.nodes.clear()
    node = tree.nodes.new("ShaderNodePrincipledNPR")
    if getattr(node, "model_version", 1) >= 2:
        # A V1 test must use a serialized V1 node, not mutate a newly-created V2
        # node's read-only version or apply the old contract to new defaults.
        bpy.data.materials.remove(material)
        template_path = LEGACY_TEMPLATE
        if template_path is None:
            value = os.environ.get("BLENDER_NPR_V1_BASELINE")
            template_path = Path(value) if value else None
        require(template_path is not None and template_path.is_file(),
                "A captured V1 template is required to test legacy compatibility")
        with bpy.data.libraries.load(str(template_path), link=False) as (source, target):
            require("PrincipledNPR_V1_Baseline" in source.materials, "V1 template material missing")
            target.materials = ["PrincipledNPR_V1_Baseline"]
        material = target.materials[0]
        material.name = name
        material.surface_render_method = "BLENDED"
        tree = material.node_tree
        node = tree.nodes.get("PrincipledNPR_V1_Baseline")
        require(node is not None and node.model_version == 1, "V1 file was silently upgraded")
        node.name = "Principled NPR V1"
        output = next(item for item in tree.nodes if item.bl_idname == "ShaderNodeOutputMaterial")
        return material, node, output
    node.name = "Principled NPR V1"
    output = tree.nodes.new("ShaderNodeOutputMaterial")
    tree.links.new(node.outputs["Shader"], output.inputs["Surface"])
    return material, node, output


def make_sphere(material):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=48, ring_count=24)
    sphere = bpy.context.object
    sphere.name = "PrincipledNPRSphere"
    for polygon in sphere.data.polygons:
        polygon.use_smooth = True
    sphere.data.materials.append(material)
    return sphere


def add_point_light(name, color, energy, location):
    data = bpy.data.lights.new(name, "POINT")
    data.color = color
    data.energy = energy
    data.lightgroup_id = 0
    data.use_shadow = True
    light = bpy.data.objects.new(name, data)
    light.location = location
    bpy.context.scene.collection.objects.link(light)
    return light


def add_shadow_caster():
    bpy.ops.mesh.primitive_cube_add(size=0.8, location=(0.8, -2.0, 1.0))
    caster = bpy.context.object
    caster.name = "PrincipledNPRShadowCaster"
    material = bpy.data.materials.new("PrincipledNPRShadowCasterMaterial")
    material.use_nodes = True
    principled = material.node_tree.nodes.get("Principled BSDF")
    principled.inputs["Base Color"].default_value = (0.15, 0.15, 0.15, 1.0)
    caster.data.materials.append(material)
    return caster


def render_image(label):
    require(OUTPUT_DIR is not None, "Output directory was not configured")
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    path = OUTPUT_DIR / f"{label}.exr"
    bpy.context.scene.render.filepath = str(path)
    bpy.context.view_layer.update()
    result = bpy.ops.render.render(write_still=False)
    require("FINISHED" in result, f"{label}: render returned {result}")
    render_result = bpy.data.images.get("Render Result")
    require(render_result is not None, f"{label}: Render Result missing")
    render_result.save_render(str(path))
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        width, height = image.size[:]
        pixels = list(image.pixels[:])
    finally:
        bpy.data.images.remove(image)
    require((width, height) == (RENDER_SIZE, RENDER_SIZE), f"{label}: wrong size")
    require(len(pixels) == width * height * 4, f"{label}: no pixels")
    require(all(math.isfinite(value) for value in pixels), f"{label}: non-finite pixels")
    return pixels


def rms_difference(first, second):
    require(len(first) == len(second), "Pixel buffers have different lengths")
    return math.sqrt(sum((a - b) ** 2 for a, b in zip(first, second)) / len(first))


def mean_rgb(pixels):
    count = len(pixels) // 4
    return sum(pixels[index] + pixels[index + 1] + pixels[index + 2]
               for index in range(0, len(pixels), 4)) / (count * 3)


def max_rgb(pixels):
    return max(max(pixels[index:index + 3]) for index in range(0, len(pixels), 4))


def highlight_coverage(pixels, threshold=0.05):
    return sum(
        1 for index in range(0, len(pixels), 4) if max(pixels[index:index + 3]) > threshold
    )


def center_pixel(pixels):
    index = ((RENDER_SIZE // 2) * RENDER_SIZE + RENDER_SIZE // 2) * 4
    return pixels[index:index + 4]


def assert_changed(label, first, second, minimum=0.003):
    difference = rms_difference(first, second)
    print(f"PRINCIPLED_NPR_V1_{label.upper()}_RMS={difference:.8f}", flush=True)
    require(difference > minimum, f"{label}: expected RMS > {minimum}, got {difference}")


def assert_storage_and_sockets():
    material, node, _output = make_material("PrincipledNPRStorageProbe")
    material.use_fake_user = True
    require(len(node.inputs) == 56, f"Expected 56 inputs, got {len(node.inputs)}")
    require([socket.identifier for socket in list(node.outputs)[:3]] == ["shader", "color", "alpha"]
            and all(socket.is_unavailable for socket in list(node.outputs)[3:]),
            "Unexpected output contract")
    require(node.diffuse_mapping == "SIMPLE", "Unexpected diffuse default")
    require(node.specular_mapping == "SIMPLE", "Unexpected specular default")
    require(node.mapping_stage == "PER_LIGHT", "Unexpected mapping-stage default")
    require(node.light_combine == "STRONGEST", "Unexpected light-combine default")
    require(node.inputs["Intensity Influence"].default_value == 1.0,
            "Light energy must drive the NPR boundary by default")
    require(node.inputs["Highlight Strength"].default_value == 1.0, "Highlight must default on")
    require(abs(node.inputs["Roughness"].default_value - 0.4) < 1e-6,
            "Unexpected default PBR roughness")
    require(abs(node.inputs["Highlight Size"].default_value - 0.5) < 1e-6,
            "Unexpected default highlight size")
    require(abs(node.inputs["Highlight Softness"].default_value - 0.08) < 1e-6,
            "Unexpected default highlight softness")
    require(node.inputs["Reflection Strength"].default_value == 0.0, "Reflection must default off")
    require(node.inputs["Ambient Strength"].default_value == 0.0, "Ambient must default off")
    require(node.inputs["Rim Strength"].default_value == 0.0, "Rim must default off")
    legacy_highlight_roughness = socket_by_identifier(node, "highlight_roughness")
    require(legacy_highlight_roughness is not None and legacy_highlight_roughness.is_unavailable,
            "Highlight Roughness must not be a public control")
    require(not node.inputs["Highlight Offset"].is_unavailable,
            "Highlight Offset must be a public control")
    require(len(node.diffuse_ramp.elements) == 2, "Diffuse ramp was not initialized")
    require(len(node.specular_ramp.elements) == 2, "Specular ramp was not initialized")

    node.diffuse_mapping = "DRIVEN_RAMP"
    node.driven_stop_count = 2
    require(
        [round(node.inputs[f"Stop {i} Position"].default_value, 5) for i in range(1, 3)] ==
        [0.0, 1.0],
        "Two-stop Driven Ramp defaults must span the full range",
    )
    node.driven_stop_count = 3
    require(
        [round(node.inputs[f"Stop {i} Position"].default_value, 5) for i in range(1, 4)] ==
        [0.0, 0.5, 1.0],
        "Three-stop Driven Ramp defaults must be evenly spaced",
    )
    node.driven_stop_count = 5
    require(
        [round(node.inputs[f"Stop {i} Position"].default_value, 5) for i in range(1, 6)] ==
        [0.0, 0.25, 0.5, 0.75, 1.0],
        "Five-stop Driven Ramp defaults must be evenly spaced",
    )

    value = material.node_tree.nodes.new("ShaderNodeValue")
    node.driven_stop_count = 3
    stop = node.inputs["Stop 3 Position"]
    material.node_tree.links.new(value.outputs["Value"], stop)
    require(stop.enabled and stop.is_linked, "Stop 3 link was not created")
    node.driven_stop_count = 2
    require(not stop.enabled and stop.is_linked, "Hidden Stop 3 did not preserve its link")
    node.driven_stop_count = 3
    require(stop.enabled and stop.is_linked, "Restored Stop 3 lost its link")

    save_path = OUTPUT_DIR / "storage_roundtrip.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(save_path))
    bpy.ops.wm.open_mainfile(filepath=str(save_path))
    material = bpy.data.materials["PrincipledNPRStorageProbe"]
    node = material.node_tree.nodes["Principled NPR V1"]
    require(node.diffuse_mapping == "DRIVEN_RAMP", "Diffuse mapping did not survive reload")
    require(node.driven_stop_count == 3, "Stop count did not survive reload")
    require(node.inputs["Stop 3 Position"].is_linked, "Stop link did not survive reload")


def setup_render_scene():
    clear_scene()
    configure_scene()
    make_camera()
    material, node, output = make_material()
    make_sphere(material)
    key = add_point_light("Key", (1.0, 0.28, 0.08), 1500.0, (0.0, -4.0, 2.0))
    add_shadow_caster()
    node.inputs["Base Color"].default_value = (0.7, 0.12, 0.03, 1.0)
    node.shadow_mode = "NONE"
    node.coordinate_range = "FULL"
    return material, node, output, key


def test_light_energy_boundary(node, key):
    node.diffuse_mapping = "SIMPLE"
    node.coordinate_range = "FRONT"
    node.color_application = "REPLACE"
    node.inputs["Shadow Color"].default_value = (0.02, 0.02, 0.02, 1.0)
    node.inputs["Lit Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    node.inputs["Boundary"].default_value = 0.5
    node.inputs["Softness"].default_value = 0.01
    node.inputs["Highlight Strength"].default_value = 0.0
    node.inputs["Reflection Strength"].default_value = 0.0
    node.inputs["Ambient Strength"].default_value = 0.0
    node.inputs["Metallic"].default_value = 0.0

    results = []
    base_energy = 1500.0
    for multiplier, label in ((0.25, "low"), (1.0, "medium"), (4.0, "high")):
        key.data.energy = base_energy * multiplier
        pixels = render_image(f"light_energy_{label}")
        results.append((mean_rgb(pixels), highlight_coverage(pixels, threshold=0.25)))
    print(f"PRINCIPLED_NPR_V1_LIGHT_ENERGY_RESULTS={results}", flush=True)
    require(
        results[0][0] < results[1][0] < results[2][0],
        f"Light energy did not monotonically brighten the NPR response: {results}",
    )
    require(
        results[0][1] <= results[1][1] <= results[2][1],
        f"Light energy did not monotonically move the diffuse boundary: {results}",
    )
    key.data.energy = 1500.0


def test_diffuse_mapping(node, key):
    test_light_energy_boundary(node, key)
    node.diffuse_mapping = "SIMPLE"
    node.color_application = "MULTIPLY"
    simple = render_image("diffuse_simple")

    node.diffuse_mapping = "RAMP"
    node.diffuse_ramp.elements[0].color = (0.05, 0.05, 0.2, 0.4)
    node.diffuse_ramp.elements[1].color = (1.0, 0.8, 0.1, 1.0)
    ramp = render_image("diffuse_ramp")
    assert_changed("simple_to_ramp", simple, ramp)

    node.diffuse_mapping = "DRIVEN_RAMP"
    node.driven_stop_count = 3
    node.driven_interpolation = "EASE"
    node.color_application = "REPLACE"
    for index, (color, position) in enumerate((
        ((0.03, 0.01, 0.08, 1.0), 0.1),
        ((0.4, 0.02, 0.01, 0.7), 0.45),
        ((1.0, 0.6, 0.08, 1.0), 0.8),
    )):
        node.inputs[f"Stop {index + 1} Color"].default_value = color
        node.inputs[f"Stop {index + 1} Position"].default_value = position
    driven = render_image("diffuse_driven_ease_replace")
    assert_changed("ramp_to_driven", ramp, driven)

    node.driven_interpolation = "CONSTANT"
    constant = render_image("diffuse_driven_constant_replace")
    node.driven_interpolation = "LINEAR"
    linear = render_image("diffuse_driven_linear_replace")
    assert_changed("driven_constant_linear", constant, linear, minimum=0.0005)
    assert_changed("driven_ease_linear", driven, linear, minimum=0.0005)
    node.driven_interpolation = "EASE"

    node.inputs["Stop 1 Color"].default_value[3] = 0.0
    node.inputs["Stop 2 Color"].default_value[3] = 0.0
    node.inputs["Stop 3 Color"].default_value[3] = 0.0
    alpha_zero = render_image("diffuse_driven_alpha_zero")
    assert_changed("driven_alpha", driven, alpha_zero)

    node.inputs["Stop 1 Color"].default_value[3] = 1.0
    node.inputs["Stop 2 Color"].default_value[3] = 1.0
    node.inputs["Stop 3 Color"].default_value[3] = 1.0
    node.color_application = "MULTIPLY"
    multiplied = render_image("diffuse_driven_multiply")
    assert_changed("multiply_replace", driven, multiplied)

    node.diffuse_mapping = "SIMPLE"
    node.color_application = "MULTIPLY"
    node.coordinate_range = "FRONT"
    node.inputs["Coordinate Scale"].default_value = 1.0
    node.inputs["Coordinate Offset"].default_value = 0.0
    front = render_image("coordinate_front")
    node.coordinate_range = "FULL"
    full = render_image("coordinate_full")
    assert_changed("front_full", front, full)
    node.coordinate_range = "FRONT"
    node.inputs["Coordinate Scale"].default_value = -1.5
    node.inputs["Coordinate Offset"].default_value = 1.2
    transformed = render_image("coordinate_unclamped_transform")
    assert_changed("coordinate_transform", front, transformed)
    node.inputs["Coordinate Scale"].default_value = 3.0
    node.inputs["Coordinate Offset"].default_value = 0.0
    expanded = render_image("coordinate_expanded")
    assert_changed("coordinate_expanded", front, expanded, minimum=0.0005)
    node.inputs["Coordinate Scale"].default_value = 1.0
    node.inputs["Coordinate Offset"].default_value = 0.0


def test_multilight(node):
    add_point_light("Fill", (0.05, 0.2, 1.0), 900.0, (2.0, -2.5, -1.0))
    node.diffuse_mapping = "RAMP"
    node.diffuse_ramp.elements[0].color = (0.1, 0.1, 0.1, 1.0)
    node.diffuse_ramp.elements[1].color = (1.0, 1.0, 1.0, 1.0)
    results = {}
    for stage in ("PER_LIGHT", "COMBINED"):
        for combine in ("ADD", "STRONGEST"):
            node.mapping_stage = stage
            node.light_combine = combine
            label = f"multilight_{stage.lower()}_{combine.lower()}"
            results[label] = render_image(label)
    require(
        max(rms_difference(a, b) for index, a in enumerate(results.values())
            for b in list(results.values())[index + 1 :]) > 0.003,
        "Multi-light modes produced no visible difference",
    )
    assert_changed(
        "per_light_add_strongest",
        results["multilight_per_light_add"],
        results["multilight_per_light_strongest"],
        minimum=0.0005,
    )
    assert_changed(
        "combined_add_strongest",
        results["multilight_combined_add"],
        results["multilight_combined_strongest"],
        minimum=0.0005,
    )
    node.mapping_stage = "PER_LIGHT"
    node.light_combine = "ADD"
    node.inputs["Direct Strength"].default_value = 4.0
    hdr = render_image("multilight_add_hdr")
    require(max_rgb(hdr) > 1.0, "Add mode clamped HDR output")
    node.inputs["Direct Strength"].default_value = 1.0
    node.mapping_stage = "PER_LIGHT"
    node.light_combine = "STRONGEST"


def test_shader_color_parity(material, node, output):
    node.diffuse_mapping = "SIMPLE"
    node.shadow_mode = "CAST_ONLY"
    shader_pixels = render_image("shader_output")
    tree = material.node_tree
    tree.links.remove(output.inputs["Surface"].links[0])
    emission = tree.nodes.new("ShaderNodeEmission")
    emission.inputs["Strength"].default_value = 1.0
    tree.links.new(node.outputs["Color"], emission.inputs["Color"])
    tree.links.new(emission.outputs["Emission"], output.inputs["Surface"])
    color_pixels = render_image("color_output")
    difference = rms_difference(shader_pixels, color_pixels)
    print(f"PRINCIPLED_NPR_V1_SHADER_COLOR_RMS={difference:.8f}", flush=True)
    require(difference < 0.02, f"Shader/Color outputs diverged: RMS={difference}")
    for link in list(output.inputs["Surface"].links):
        tree.links.remove(link)
    tree.links.new(node.outputs["Shader"], output.inputs["Surface"])
    tree.nodes.remove(emission)
    node.shadow_mode = "ALL"


def test_specular_environment_rim(material, node):
    tree = material.node_tree
    node.diffuse_mapping = "SIMPLE"
    node.specular_mapping = "SIMPLE"
    node.color_application = "MULTIPLY"
    node.shadow_mode = "NONE"
    node.inputs["Base Color"].default_value = (0.78, 0.05, 0.02, 1.0)
    node.inputs["Highlight Strength"].default_value = 1.0
    node.inputs["Reflection Strength"].default_value = 0.0
    node.inputs["Ambient Strength"].default_value = 0.0
    node.inputs["Direct Strength"].default_value = 1.0

    metallic_value = tree.nodes.new("ShaderNodeValue")
    roughness_value = tree.nodes.new("ShaderNodeValue")
    tree.links.new(metallic_value.outputs["Value"], node.inputs["Metallic"])
    tree.links.new(roughness_value.outputs["Value"], node.inputs["Roughness"])

    metallic_value.outputs["Value"].default_value = 0.0
    roughness_value.outputs["Value"].default_value = 0.15
    dielectric = render_image("pbr_dielectric")
    metallic_value.outputs["Value"].default_value = 1.0
    metal = render_image("pbr_metallic")
    assert_changed("pbr_metallic", dielectric, metal, minimum=0.0005)
    metal_center = center_pixel(metal)
    require(
        metal_center[0] > metal_center[1] * 1.2 and metal_center[0] > metal_center[2] * 1.2,
        f"Metallic highlight was not tinted by Base Color: {metal_center}",
    )

    roughness_value.outputs["Value"].default_value = 0.85
    rough = render_image("pbr_roughness_high")
    assert_changed("pbr_roughness", metal, rough, minimum=0.0005)
    require(max_rgb(metal) > max_rgb(rough), "Higher roughness did not reduce highlight peak")
    require(
        highlight_coverage(rough) > highlight_coverage(metal),
        "Higher roughness did not broaden the base highlight",
    )

    for link in list(node.inputs["Metallic"].links) + list(node.inputs["Roughness"].links):
        tree.links.remove(link)
    tree.nodes.remove(metallic_value)
    tree.nodes.remove(roughness_value)
    node.inputs["Metallic"].default_value = 0.0
    node.inputs["Roughness"].default_value = 0.4

    node.inputs["Highlight Strength"].default_value = 1.0
    default_highlight = render_image("highlight_default")
    node.inputs["Highlight Strength"].default_value = 0.0
    no_highlight = render_image("highlight_off")
    assert_changed("highlight_default_enable", no_highlight, default_highlight, minimum=0.0005)
    node.inputs["Highlight Strength"].default_value = 3.0
    node.inputs["Highlight Size"].default_value = 0.90
    node.inputs["Highlight Softness"].default_value = 0.04
    isotropic = render_image("highlight_simple_isotropic")
    assert_changed("highlight_enable", no_highlight, isotropic)
    node.inputs["Highlight Size"].default_value = 0.05
    small = render_image("highlight_size_small")
    assert_changed("highlight_size", isotropic, small, minimum=0.0005)
    node.inputs["Highlight Size"].default_value = 0.15
    node.inputs["Roughness"].default_value = 0.85
    softness_value = tree.nodes.new("ShaderNodeValue")
    tree.links.new(softness_value.outputs["Value"], node.inputs["Highlight Softness"])
    softness_value.outputs["Value"].default_value = 0.04
    softness_base = render_image("highlight_softness_base")
    softness_value.outputs["Value"].default_value = 0.80
    soft_edge = render_image("highlight_softness_wide")
    assert_changed("highlight_softness", softness_base, soft_edge, minimum=0.0005)
    tree.links.remove(node.inputs["Highlight Softness"].links[0])
    tree.nodes.remove(softness_value)
    node.inputs["Highlight Softness"].default_value = 0.04
    node.inputs["Roughness"].default_value = 0.4
    node.inputs["Anisotropy"].default_value = 1.0
    node.inputs["Anisotropy Rotation"].default_value = 0.25
    anisotropic = render_image("highlight_anisotropic")
    assert_changed("highlight_anisotropy", isotropic, anisotropic, minimum=0.0005)
    node.specular_mapping = "RAMP"
    node.specular_ramp.elements[0].color = (0.0, 0.0, 0.0, 0.0)
    node.specular_ramp.elements[1].color = (0.1, 0.8, 1.0, 1.0)
    specular_ramp = render_image("highlight_ramp")
    assert_changed("highlight_ramp", anisotropic, specular_ramp, minimum=0.0005)
    node.inputs["Highlight Offset"].default_value = -0.50
    offset = render_image("highlight_offset")
    assert_changed("highlight_offset", specular_ramp, offset, minimum=0.0005)
    node.inputs["Highlight Size"].default_value = 0.15
    ramp_small = render_image("highlight_ramp_size_small")
    assert_changed("highlight_ramp_size", specular_ramp, ramp_small, minimum=0.0005)
    node.inputs["Highlight Size"].default_value = 0.55
    node.inputs["Highlight Softness"].default_value = 0.30
    ramp_soft = render_image("highlight_ramp_softness_wide")
    assert_changed("highlight_ramp_softness", specular_ramp, ramp_soft, minimum=0.0005)

    node.specular_mapping = "SIMPLE"
    node.inputs["Highlight Offset"].default_value = 0.0
    node.inputs["Highlight Strength"].default_value = 1.0
    node.inputs["Highlight Size"].default_value = 0.55
    node.inputs["Highlight Softness"].default_value = 0.04

    node.inputs["Direct Strength"].default_value = 0.0
    black = render_image("effects_off")
    node.inputs["Rim Strength"].default_value = 2.0
    node.inputs["Thickness"].default_value = 0.3
    node.inputs["Length"].default_value = 0.7
    rim = render_image("rim_malt_controls")
    assert_changed("rim_enable", black, rim)
    node.inputs["Rim Strength"].default_value = 0.0
    node.inputs["Emission Color"].default_value = (0.0, 0.8, 0.1, 1.0)
    node.inputs["Emission Strength"].default_value = 2.0
    emission = render_image("emission")
    assert_changed("emission_enable", black, emission)
    node.inputs["Emission Strength"].default_value = 0.0
    node.inputs["Direct Strength"].default_value = 1.0


def test_environment(node):
    scene = bpy.context.scene
    world = scene.world
    world.use_nodes = True
    world_nodes = world.node_tree.nodes
    world_links = world.node_tree.links
    world_nodes.clear()
    world_output = world_nodes.new("ShaderNodeOutputWorld")
    background = world_nodes.new("ShaderNodeBackground")
    background.inputs["Color"].default_value = (0.02, 0.6, 0.95, 1.0)
    background.inputs["Strength"].default_value = 1.0
    world_links.new(background.outputs["Background"], world_output.inputs["Surface"])

    node.inputs["Direct Strength"].default_value = 0.0
    node.inputs["Highlight Strength"].default_value = 0.0
    node.inputs["Rim Strength"].default_value = 0.0
    node.inputs["Emission Strength"].default_value = 0.0
    node.inputs["Metallic"].default_value = 1.0
    node.inputs["Reflection Strength"].default_value = 0.0
    node.inputs["Ambient Strength"].default_value = 0.0
    off = render_image("environment_off")

    node.inputs["Reflection Strength"].default_value = 1.0
    reflection = render_image("environment_reflection")
    assert_changed("environment_reflection_enable", off, reflection, minimum=0.0005)

    node.inputs["Reflection Strength"].default_value = 0.0
    node.inputs["Metallic"].default_value = 0.0
    node.inputs["Ambient Strength"].default_value = 1.0
    ambient = render_image("environment_ambient")
    assert_changed("environment_ambient_enable", off, ambient, minimum=0.0005)

    node.inputs["Ambient Strength"].default_value = 0.0
    node.inputs["Direct Strength"].default_value = 1.0
    node.inputs["Metallic"].default_value = 0.0


def test_shadow_modes(node):
    results = {}
    for mode in ("NONE", "CAST_ONLY", "ALL"):
        node.shadow_mode = mode
        results[mode] = render_image(f"shadow_mode_{mode.lower()}")
    assert_changed("shadow_none_cast_only", results["NONE"], results["CAST_ONLY"], minimum=0.0005)
    assert_changed("shadow_none_all", results["NONE"], results["ALL"], minimum=0.0005)
    assert_changed("shadow_cast_only_all", results["CAST_ONLY"], results["ALL"], minimum=0.0001)
    node.shadow_mode = "ALL"


def test_lightgroup(node):
    lights = [obj for obj in bpy.context.scene.objects if obj.type == "LIGHT"]
    for light in lights:
        light.data.lightgroup_id = 7
    node.lightgroup_id = 0
    no_match = render_image("lightgroup_no_match")
    node.lightgroup_id = 7
    match = render_image("lightgroup_match")
    assert_changed("lightgroup_filter", no_match, match, minimum=0.0005)
    for light in lights:
        light.data.lightgroup_id = 0
    node.lightgroup_id = 0


def test_alpha(material, node, output):
    scene = bpy.context.scene
    scene.world.use_nodes = False
    scene.world.color = (0.0, 0.0, 0.0)
    shadow_caster = bpy.data.objects.get("PrincipledNPRShadowCaster")
    if shadow_caster is not None:
        shadow_caster.hide_render = True
    tree = material.node_tree
    tree.links.remove(output.inputs["Surface"].links[0])
    emission = tree.nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    tree.links.new(node.outputs["Alpha"], emission.inputs["Strength"])
    tree.links.new(emission.outputs["Emission"], output.inputs["Surface"])

    node.inputs["Alpha"].default_value = 0.35
    scaled = render_image("alpha_output_035")
    node.inputs["Alpha"].default_value = 1.0
    full = render_image("alpha_output_100")
    scaled_center = center_pixel(scaled)
    full_center = center_pixel(full)
    ratio = scaled_center[0] / max(full_center[0], 1e-8)
    print(
        f"PRINCIPLED_NPR_V1_ALPHA_OUTPUT_CENTER={scaled_center}/{full_center}",
        flush=True,
    )
    print(f"PRINCIPLED_NPR_V1_ALPHA_OUTPUT_RATIO={ratio:.8f}", flush=True)
    require(0.30 < ratio < 0.40, f"Alpha output did not preserve material Alpha: {ratio}")

    tree.links.remove(output.inputs["Surface"].links[0])
    tree.links.new(node.outputs["Shader"], output.inputs["Surface"])
    tree.nodes.remove(emission)
    if shadow_caster is not None:
        shadow_caster.hide_render = False


def test_thirty_two_lights(node):
    for obj in list(bpy.context.scene.objects):
        if obj.type == "LIGHT":
            bpy.data.objects.remove(obj, do_unlink=True)
    for index in range(32):
        angle = index * math.tau / 32.0
        add_point_light(
            f"Stress {index:02d}",
            (0.25 + 0.75 * (index % 3 == 0), 0.25 + 0.75 * (index % 3 == 1), 0.25 + 0.75 * (index % 3 == 2)),
            80.0,
            (3.0 * math.cos(angle), -2.0, 3.0 * math.sin(angle)),
        )
    node.mapping_stage = "PER_LIGHT"
    node.light_combine = "ADD"
    pixels = render_image("thirty_two_lights")
    require(mean_rgb(pixels) > 0.005, "32-light render was empty")


def main():
    global OUTPUT_DIR
    global LEGACY_TEMPLATE
    args = parse_args()
    LEGACY_TEMPLATE = args.legacy_template
    OUTPUT_DIR = args.output_dir.resolve()
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    assert_storage_and_sockets()
    material, node, output, key = setup_render_scene()

    baseline = render_image("backend_probe")
    require(mean_rgb(baseline) > 0.001, "Backend probe was empty")
    import gpu
    active_backend = gpu.platform.backend_type_get()
    print(BACKEND_MARKER + active_backend, flush=True)
    require(active_backend == args.expected_backend, f"Expected {args.expected_backend}, got {active_backend}")

    test_diffuse_mapping(node, key)
    test_multilight(node)
    test_shader_color_parity(material, node, output)
    test_specular_environment_rim(material, node)
    test_environment(node)
    test_shadow_modes(node)
    test_lightgroup(node)
    test_alpha(material, node, output)
    test_thirty_two_lights(node)
    print(SUCCESS_MARKER, flush=True)


if __name__ == "__main__":
    main()
