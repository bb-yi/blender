from __future__ import annotations

import math
from pathlib import Path

import bpy


RESOLUTION = 64
PIXEL_TOLERANCE = 0.08
MIN_VOLUME_DELTA = 0.005
TAA_RENDER_SAMPLES = 8
MAX_SAFE_RADIANCE = 8.0
OUT_DIR = Path(__file__).resolve().parent / "out"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def set_if_available(owner, name, value):
    if hasattr(owner, name):
        setattr(owner, name, value)


def configure_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = RESOLUTION
    scene.render.resolution_y = RESOLUTION
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.eevee.taa_samples = 1
    scene.eevee.taa_render_samples = 1
    set_if_available(scene.eevee, "use_taa_reprojection", False)
    # NPR Refraction layers are selected by Eevee's screen-transmission path, which is
    # enabled by the ray-tracing scene flag even though this case only validates the
    # existing screen-space feedback and froxel volume integration.
    set_if_available(scene.eevee, "use_raytracing", True)
    set_if_available(scene.eevee, "volumetric_samples", 32)
    set_if_available(scene.eevee, "volumetric_tile_size", "4")
    set_if_available(scene.eevee, "volumetric_start", 0.1)
    set_if_available(scene.eevee, "volumetric_end", 20.0)
    set_if_available(scene.eevee, "use_volume_custom_range", True)
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0

    world = bpy.data.worlds.new("NPR Refraction Volume World")
    world.use_nodes = True
    world_nodes = world.node_tree.nodes
    world_links = world.node_tree.links
    world_nodes.clear()
    world_output = world_nodes.new("ShaderNodeOutputWorld")
    background = world_nodes.new("ShaderNodeBackground")
    background.inputs["Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    background.inputs["Strength"].default_value = 0.0
    absorption = world_nodes.new("ShaderNodeVolumeAbsorption")
    absorption.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    absorption.inputs["Density"].default_value = 0.0
    scatter = world_nodes.new("ShaderNodeVolumeScatter")
    scatter.inputs["Color"].default_value = (0.05, 0.8, 0.1, 1.0)
    scatter.inputs["Density"].default_value = 0.0
    add_volume = world_nodes.new("ShaderNodeAddShader")
    world_links.new(background.outputs["Background"], world_output.inputs["Surface"])
    world_links.new(absorption.outputs["Volume"], add_volume.inputs[0])
    world_links.new(scatter.outputs["Volume"], add_volume.inputs[1])
    world_links.new(add_volume.outputs["Shader"], world_output.inputs["Volume"])
    scene.world = world

    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 4.0
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 5.0)
    scene.collection.objects.link(camera)
    scene.camera = camera

    return scene, absorption, scatter


def make_emission_material():
    material = bpy.data.materials.new("NPR Refraction Volume Back")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()
    output = nodes.new("ShaderNodeOutputMaterial")
    principled = nodes.new("ShaderNodeBsdfPrincipled")
    principled.inputs["Base Color"].default_value = (0.8, 0.12, 0.03, 1.0)
    principled.inputs["Roughness"].default_value = 0.8
    if "Emission Color" in principled.inputs:
        principled.inputs["Emission Color"].default_value = (0.8, 0.12, 0.03, 1.0)
    if "Emission Strength" in principled.inputs:
        principled.inputs["Emission Strength"].default_value = 1.0
    links.new(principled.outputs["BSDF"], output.inputs["Surface"])
    return material


def make_refraction_material():
    material = bpy.data.materials.new("NPR Refraction Volume Front")
    material.use_nodes = True
    material.surface_render_method = "DITHERED"
    set_if_available(material, "use_screen_refraction", True)
    set_if_available(material, "use_raytrace_refraction", True)

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()
    output = nodes.new("ShaderNodeOutputMaterial")
    principled = nodes.new("ShaderNodeBsdfPrincipled")
    principled.inputs["Base Color"].default_value = (0.0, 0.8, 0.0, 1.0)
    principled.inputs["Roughness"].default_value = 0.0
    set_if_available(principled, "Emission Color", (0.0, 0.8, 0.0, 1.0))
    set_if_available(principled, "Emission Strength", 1.0)
    principled.inputs["Transmission Weight"].default_value = 0.0
    links.new(principled.outputs["BSDF"], output.inputs["Surface"])

    npr_tree = bpy.data.node_groups.new("NPR Refraction Volume Tree", "ShaderNodeTree")
    refraction = npr_tree.nodes.new("ShaderNodeNPR_Refraction")
    npr_output = npr_tree.nodes.new("ShaderNodeNPR_Output")
    npr_tree.links.new(refraction.outputs["Combined Color"], npr_output.inputs["Color"])
    output.nprtree = npr_tree
    return material


def make_volume_material():
    material = bpy.data.materials.new("NPR Refraction Volume Mesh")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()
    output = nodes.new("ShaderNodeOutputMaterial")
    volume = nodes.new("ShaderNodeVolumeAbsorption")
    # White is the neutral absorption color in Eevee; use a tinted color so the
    # absorption-only path is actually flagged and contributes to transmittance.
    volume.inputs["Color"].default_value = (0.2, 0.65, 0.95, 1.0)
    volume.inputs["Density"].default_value = 0.8
    links.new(volume.outputs["Volume"], output.inputs["Volume"])
    return material


def add_plane(name, size, z, material):
    bpy.ops.mesh.primitive_plane_add(size=size, location=(0.0, 0.0, z))
    plane = bpy.context.object
    plane.name = name
    plane.data.materials.append(material)
    return plane


def add_front_light(scene):
    light_data = bpy.data.lights.new("NPR Refraction Front Light", "POINT")
    light_data.energy = 1000.0
    light = bpy.data.objects.new("NPR Refraction Front Light", light_data)
    light.location = (0.0, 0.0, 4.0)
    scene.collection.objects.link(light)
    return light


def add_render_texture_capture(scene, material):
    camera_data = bpy.data.cameras.new("NPR Refraction Volume Capture Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 4.0
    camera = bpy.data.objects.new("NPR Refraction Volume Capture Camera", camera_data)
    camera.location = (6.0, 0.0, 5.0)
    scene.collection.objects.link(camera)

    render_texture = scene.eevee.render_textures.add()
    render_texture.name = "NPR Refraction Volume Capture"
    render_texture.enabled = False
    render_texture.camera = camera
    render_texture.source = "COLOR"
    render_texture.resolution_x = RESOLUTION
    render_texture.resolution_y = RESOLUTION
    render_texture.update_mode = "EVERY_FRAME"
    render_texture.format = "RGBA16F"

    capture_plane = add_plane("NPR Refraction Volume Capture Geometry", 4.0, 0.0, material)
    capture_plane.location.x = 6.0
    return render_texture


def render_center(scene, name, return_max_rgb=False):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    original_file_format = scene.render.image_settings.file_format
    original_color_depth = scene.render.image_settings.color_depth
    if return_max_rgb:
        scene.render.image_settings.file_format = "OPEN_EXR"
        scene.render.image_settings.color_depth = "32"
    extension = "exr" if return_max_rgb else "png"
    output_path = OUT_DIR / f"{name}.{extension}"
    scene.render.filepath = str(output_path)
    bpy.context.view_layer.update()
    try:
        result = bpy.ops.render.render(write_still=True)
        require("FINISHED" in result, f"Render failed: {result}")
        require(output_path.is_file(), f"Render output is missing: {output_path}")
        image = bpy.data.images.load(str(output_path), check_existing=False)
        try:
            require(
                tuple(image.size) == (RESOLUTION, RESOLUTION),
                f"Unexpected render size: {tuple(image.size)}",
            )
            pixels = [float(channel) for channel in image.pixels[:]]
        finally:
            bpy.data.images.remove(image)
    finally:
        scene.render.image_settings.file_format = original_file_format
        scene.render.image_settings.color_depth = original_color_depth

    require(
        len(pixels) == RESOLUTION * RESOLUTION * 4,
        f"Unexpected rendered pixel count: {len(pixels)}",
    )
    require(
        all(math.isfinite(channel) for channel in pixels),
        f"Rendered image contains NaN or infinity: {name}",
    )
    max_rgb = max(
        pixels[index]
        for index in range(len(pixels))
        if index % 4 != 3
    )
    center = ((RESOLUTION // 2) * RESOLUTION + (RESOLUTION // 2)) * 4
    value = tuple(float(pixels[center + channel]) for channel in range(4))
    return (value, max_rgb) if return_max_rgb else value


def rgba_distance(a, b):
    return max(abs(a[index] - b[index]) for index in range(4))


def main():
    scene, absorption, scatter = configure_scene()
    back_material = make_emission_material()
    front_material = make_refraction_material()
    volume_material = make_volume_material()
    back_plane = add_plane("NPR Refraction Volume Back", 4.0, 0.0, back_material)
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, 1.5))
    volume_cube = bpy.context.object
    volume_cube.name = "NPR Refraction Mesh Volume"
    volume_cube.dimensions = (4.0, 4.0, 2.5)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    volume_cube.data.materials.append(volume_material)
    back_layer = add_plane("NPR Refraction Volume Layer 0", 2.5, 2.0, front_material)
    front_layer = add_plane("NPR Refraction Volume Layer 1", 1.5, 3.0, front_material)
    back_layer.refraction_layer_index = 0
    front_layer.refraction_layer_index = 1

    absorption.inputs["Density"].default_value = 0.35
    back_layer.hide_render = True
    front_layer.hide_render = True
    background_volume = render_center(scene, "background_volume")

    back_layer.hide_render = False
    front_layer.hide_render = False
    two_layer_volume = render_center(scene, "two_layer_volume")

    front_layer.hide_render = True
    one_layer_volume = render_center(scene, "one_layer_volume")

    absorption.inputs["Density"].default_value = 0.0
    volume_cube.hide_render = True
    one_layer_no_volume = render_center(scene, "one_layer_no_volume")

    back_layer.hide_render = True
    no_layer_no_volume = render_center(scene, "no_layer_no_volume")
    back_layer.hide_render = False

    scene.render.film_transparent = True
    scatter.inputs["Density"].default_value = 0.6
    back_plane.hide_render = True
    back_layer.hide_render = True
    background_miss_volume = render_center(scene, "background_miss_volume")

    back_layer.hide_render = False
    background_miss_one_layer = render_center(scene, "background_miss_one_layer")

    scatter.inputs["Density"].default_value = 0.0
    background_miss_no_volume = render_center(scene, "background_miss_no_volume")
    add_front_light(scene)
    background_miss_no_volume_lit = render_center(scene, "background_miss_no_volume_lit")

    scene.render.film_transparent = False
    scene.eevee.taa_render_samples = TAA_RENDER_SAMPLES
    absorption.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    absorption.inputs["Density"].default_value = 0.35
    scatter.inputs["Density"].default_value = 0.0
    back_plane.hide_render = False
    back_layer.hide_render = False
    front_layer.hide_render = False
    volume_cube.hide_render = False

    render_texture = add_render_texture_capture(scene, back_material)
    taa_without_capture = render_center(scene, "taa_without_render_texture")
    render_texture.enabled = True
    taa_with_capture = render_center(scene, "taa_with_render_texture")

    scene.eevee.taa_render_samples = 1
    render_texture.enabled = False
    absorption.inputs["Color"].default_value = (0.001, 0.2, 1.0, 1.0)
    absorption.inputs["Density"].default_value = 8.0
    volume_cube.hide_render = True
    strong_absorption, strong_absorption_max = render_center(
        scene, "strong_colored_absorption", return_max_rgb=True
    )

    require(
        rgba_distance(background_volume, one_layer_volume) <= PIXEL_TOLERANCE,
        f"One-layer volume mismatch: background={background_volume}, one_layer={one_layer_volume}",
    )
    require(
        rgba_distance(background_volume, two_layer_volume) <= PIXEL_TOLERANCE,
        f"Two-layer volume mismatch: background={background_volume}, two_layer={two_layer_volume}",
    )
    require(
        rgba_distance(one_layer_volume, one_layer_no_volume) >= MIN_VOLUME_DELTA,
        f"Volume had no measurable effect: volume={one_layer_volume}, no_volume={one_layer_no_volume}",
    )
    require(
        rgba_distance(one_layer_no_volume, no_layer_no_volume) <= PIXEL_TOLERANCE,
        "No-volume refraction changed the background: "
        f"one_layer={one_layer_no_volume}, no_layer={no_layer_no_volume}",
    )
    require(
        rgba_distance(background_miss_volume, background_miss_one_layer) <= PIXEL_TOLERANCE,
        "Background-miss volume mismatch: "
        f"background={background_miss_volume}, one_layer={background_miss_one_layer}",
    )
    require(
        background_miss_one_layer[3] >= MIN_VOLUME_DELTA,
        f"Background-miss volume stayed transparent: {background_miss_one_layer}",
    )
    require(
        rgba_distance(background_miss_one_layer, background_miss_no_volume) >= MIN_VOLUME_DELTA,
        "Background-miss volume had no measurable effect: "
        f"volume={background_miss_one_layer}, no_volume={background_miss_no_volume}",
    )
    require(
        rgba_distance(background_miss_no_volume, background_miss_no_volume_lit) <= MIN_VOLUME_DELTA,
        "Background-miss refraction leaked current-layer lighting: "
        f"unlit={background_miss_no_volume}, lit={background_miss_no_volume_lit}",
    )
    require(
        max(background_miss_no_volume_lit) <= MIN_VOLUME_DELTA,
        f"Lit refraction layer did not preserve the transparent world: {background_miss_no_volume_lit}",
    )
    require(
        rgba_distance(taa_without_capture, taa_with_capture) <= PIXEL_TOLERANCE,
        "Render Texture capture polluted the TAA main-view volume result: "
        f"without_capture={taa_without_capture}, with_capture={taa_with_capture}",
    )
    require(
        strong_absorption_max <= MAX_SAFE_RADIANCE,
        "Strong colored absorption produced unstable radiance: "
        f"center={strong_absorption}, max_rgb={strong_absorption_max}",
    )
    print(
        "NPR_REFRACTION_VOLUME_RESULTS="
        f"background={background_volume} one_layer={one_layer_volume} "
        f"two_layer={two_layer_volume} no_volume={one_layer_no_volume} "
        f"miss_background={background_miss_volume} "
        f"miss_one_layer={background_miss_one_layer} miss_no_volume={background_miss_no_volume} "
        f"miss_no_volume_lit={background_miss_no_volume_lit} "
        f"no_layer_no_volume={no_layer_no_volume} "
        f"taa_without_capture={taa_without_capture} taa_with_capture={taa_with_capture} "
        f"strong_absorption={strong_absorption} strong_absorption_max={strong_absorption_max}"
    )
    print("EEVEE_NPR_REFRACTION_VOLUME_OK")


if __name__ == "__main__":
    main()
