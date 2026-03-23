import bpy
import os
import tempfile


RESOLUTION = 256
ORTHO_SCALE = 8.0
REPEAT_COUNT = 3
TOLERANCE = 1e-5


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def configure_scene():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = RESOLUTION
    scene.render.resolution_y = RESOLUTION
    scene.render.resolution_percentage = 100
    scene.eevee.taa_samples = 1
    scene.eevee.taa_render_samples = 1
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.world.use_nodes = False
    scene.world.color = (0.0, 0.0, 0.0)


def make_camera():
    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = ORTHO_SCALE
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 6.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def make_shadow_material():
    return make_shadow_material_with_settings()


def make_shadow_material_with_settings(shadow_mode="STABLE", stable_samples=32):
    material = bpy.data.materials.new("StableShadowMaterial")
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    shader_info = nodes.new("ShaderNodeShaderInfo")
    shader_info.shadow_mode = shadow_mode
    shader_info.stable_shadow_samples = stable_samples

    links.new(shader_info.outputs["Shadow"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def assert_default_shadow_mode_uses_builtin():
    material = bpy.data.materials.new("ShaderInfoDefaultModeMaterial")
    material.use_nodes = True
    shader_info = material.node_tree.nodes.new("ShaderNodeShaderInfo")
    assert shader_info.shadow_mode == "TEMPORAL", (
        f"Shader Info should default to the built-in shadow mode, got {shader_info.shadow_mode}"
    )


def make_threshold_material():
    material = bpy.data.materials.new("StableShadowThresholdMaterial")
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Strength"].default_value = 1.0
    shader_info = nodes.new("ShaderNodeShaderInfo")
    shader_info.shadow_mode = "STABLE"
    shader_info.stable_shadow_samples = 32
    threshold = nodes.new("ShaderNodeMath")
    threshold.operation = "GREATER_THAN"
    threshold.inputs[1].default_value = 0.8

    links.new(shader_info.outputs["Shadow"], threshold.inputs[0])
    links.new(threshold.outputs["Value"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_plane(material):
    bpy.ops.mesh.primitive_plane_add(size=8.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.active_object
    plane.data.materials.append(material)


def make_blocker():
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, 1.0))
    blocker = bpy.context.active_object
    blocker.scale = (0.5, 0.5, 1.0)
    return blocker


def make_soft_point_light():
    light_data = bpy.data.lights.new("StableShadowPoint", type="POINT")
    light_data.energy = 5000.0
    light_data.shadow_soft_size = 1.2
    light_data.use_shadow = True
    light = bpy.data.objects.new("StableShadowPoint", light_data)
    light.location = (-4.0, 0.0, 4.0)
    bpy.context.scene.collection.objects.link(light)
    return light


def render_image():
    scene = bpy.context.scene
    file_descriptor, filepath = tempfile.mkstemp(suffix=".exr")
    os.close(file_descriptor)

    scene.render.image_settings.file_format = "OPEN_EXR"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "32"
    scene.render.filepath = filepath

    bpy.ops.render.render(write_still=False)
    bpy.data.images["Render Result"].save_render(filepath)

    image = bpy.data.images.load(filepath, check_existing=False)
    try:
        pixels = list(image.pixels[:])
        width = image.size[0]
        height = image.size[1]
    finally:
        bpy.data.images.remove(image)
        if os.path.exists(filepath):
            os.remove(filepath)

    return pixels, width, height


def sample_world_point(pixels, width, height, world_x, world_y=0.0):
    x_ratio = (world_x / ORTHO_SCALE) + 0.5
    y_ratio = (world_y / ORTHO_SCALE) + 0.5
    x = min(width - 1, max(0, int(width * x_ratio)))
    y = min(height - 1, max(0, int(height * y_ratio)))
    index = (y * width + x) * 4
    return list(pixels[index:index + 4])


def render_samples(material_factory, with_blocker):
    values = []
    for _ in range(REPEAT_COUNT):
        clear_scene()
        configure_scene()
        make_camera()
        make_plane(material_factory())
        if with_blocker:
            make_blocker()
        make_soft_point_light()
        pixels, width, height = render_image()
        values.append(
            {
                "dark": sample_world_point(pixels, width, height, 0.5)[0],
                "penumbra": sample_world_point(pixels, width, height, 2.0)[0],
                "bright": sample_world_point(pixels, width, height, 2.8)[0],
            }
        )
    return values


def render_profile(material_factory, with_blocker, sample_count=24):
    clear_scene()
    configure_scene()
    make_camera()
    make_plane(material_factory())
    if with_blocker:
        make_blocker()
    make_soft_point_light()
    pixels, width, height = render_image()

    values = []
    for index in range(sample_count):
        world_x = 0.8 + (2.4 * index / max(1, sample_count - 1))
        values.append(sample_world_point(pixels, width, height, world_x)[0])
    return values


def assert_repeated_values_stable(samples, key):
    baseline = samples[0][key]
    for index, sample in enumerate(samples[1:], start=1):
        assert abs(sample[key] - baseline) <= TOLERANCE, (
            f"{key} should stay stable across repeated renders, "
            f"got baseline={baseline} sample_{index}={sample[key]}"
        )


def assert_shadow_mask_stability():
    clear_samples = render_samples(make_shadow_material, with_blocker=False)
    blocked_samples = render_samples(make_shadow_material, with_blocker=True)

    for key in ("dark", "penumbra", "bright"):
        assert_repeated_values_stable(clear_samples, key)
        assert_repeated_values_stable(blocked_samples, key)

    clear = clear_samples[0]
    blocked = blocked_samples[0]

    assert clear["dark"] > 0.95 and clear["penumbra"] > 0.95 and clear["bright"] > 0.95, (
        f"Unblocked shadow mask should stay near white, got {clear}"
    )
    assert blocked["dark"] < blocked["penumbra"] < blocked["bright"], (
        f"Blocked shadow mask should form a grayscale penumbra ramp, got {blocked}"
    )
    assert 0.15 < blocked["penumbra"] < 0.85, (
        f"Penumbra sample should stay inside the grayscale range, got {blocked['penumbra']}"
    )


def assert_threshold_shadow_stability():
    clear_samples = render_samples(make_threshold_material, with_blocker=False)
    blocked_samples = render_samples(make_threshold_material, with_blocker=True)

    for key in ("dark", "penumbra", "bright"):
        assert_repeated_values_stable(clear_samples, key)
        assert_repeated_values_stable(blocked_samples, key)

    clear = clear_samples[0]
    blocked = blocked_samples[0]

    assert clear["dark"] > 0.95 and clear["penumbra"] > 0.95 and clear["bright"] > 0.95, (
        f"Thresholded shadow should stay white without blocker, got {clear}"
    )
    assert blocked["dark"] < 0.05, f"Dark sample should fall below threshold, got {blocked}"
    assert blocked["penumbra"] < 0.05, f"Penumbra threshold result should stay stable dark, got {blocked}"
    assert blocked["bright"] > 0.95, f"Bright sample should stay above threshold, got {blocked}"


def assert_stable_sample_count_improves_gradient():
    low_values = render_profile(
        lambda: make_shadow_material_with_settings(shadow_mode="STABLE", stable_samples=8),
        with_blocker=True,
    )
    high_values = render_profile(
        lambda: make_shadow_material_with_settings(shadow_mode="STABLE", stable_samples=32),
        with_blocker=True,
    )

    low_unique = {
        round(value, 3) for value in low_values if 0.05 < value < 0.95
    }
    high_unique = {
        round(value, 3) for value in high_values if 0.05 < value < 0.95
    }

    assert len(high_unique) > len(low_unique), (
        f"Higher stable sample counts should produce more grayscale levels, "
        f"got low={sorted(low_unique)} high={sorted(high_unique)}"
    )


def assert_temporal_shadow_mode_renders():
    temporal_values = render_profile(
        lambda: make_shadow_material_with_settings(shadow_mode="TEMPORAL", stable_samples=32),
        with_blocker=True,
    )

    assert min(temporal_values) < 0.2, f"Temporal shadow mode should still produce dark regions, got {temporal_values}"
    assert max(temporal_values) > 0.8, (
        f"Temporal shadow mode should still produce lit regions, got {temporal_values}"
    )


assert hasattr(bpy.types, "ShaderNodeShaderInfo"), "ShaderNodeShaderInfo is not registered"
assert "shadow_mode" in bpy.types.ShaderNodeShaderInfo.bl_rna.properties, "shadow_mode property is missing"
assert (
    "stable_shadow_samples" in bpy.types.ShaderNodeShaderInfo.bl_rna.properties
), "stable_shadow_samples property is missing"

assert_default_shadow_mode_uses_builtin()
assert_shadow_mask_stability()
assert_threshold_shadow_stability()
assert_stable_sample_count_improves_gradient()
assert_temporal_shadow_mode_renders()

print("PASS")
