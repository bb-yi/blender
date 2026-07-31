import os
import tempfile

import bpy
import gpu


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def configure_scene(use_raytracing):
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 128
    scene.render.resolution_y = 128
    scene.render.resolution_percentage = 100
    scene.eevee.taa_render_samples = 1
    scene.eevee.taa_samples = 1
    scene.eevee.use_raytracing = use_raytracing
    if hasattr(scene.eevee, "ray_tracing_method"):
        scene.eevee.ray_tracing_method = "SCREEN"
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    if scene.world is not None:
        scene.world.color = (0.0, 0.0, 0.0)


def make_camera():
    camera_data = bpy.data.cameras.new("Camera")
    camera = bpy.data.objects.new("Camera", camera_data)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera
    camera.location = (0.0, 0.0, 4.0)
    camera.rotation_euler = (0.0, 0.0, 0.0)
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 2.0


def make_plane(name, z_location):
    bpy.ops.mesh.primitive_plane_add(size=2.0, location=(0.0, 0.0, z_location))
    obj = bpy.context.active_object
    obj.name = name
    return obj


def assign_single_material(obj, material):
    obj.data.materials.clear()
    obj.data.materials.append(material)
    obj.active_material_index = 0
    for polygon in obj.data.polygons:
        polygon.material_index = 0

    assert len(obj.data.materials) == 1, f"{obj.name}: expected exactly one material slot"
    assert obj.active_material == material, f"{obj.name}: active material assignment failed"
    assert all(polygon.material_index == 0 for polygon in obj.data.polygons), (
        f"{obj.name}: not every polygon uses material slot 0"
    )


def make_emission_material(name, color):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = color
    emission.inputs["Strength"].default_value = 1.0
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_screenspace_material(
    name, surface_render_method, explicit_view_position=None, direct_scene_color=False
):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    material.surface_render_method = surface_render_method
    material.use_raytrace_refraction = surface_render_method == "DITHERED"

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    output.location = (500.0, 0.0)

    emission = nodes.new("ShaderNodeEmission")
    emission.location = (260.0, 0.0)
    emission.inputs["Strength"].default_value = 1.0

    screenspace = nodes.new("ShaderNodeScreenspaceInfo")
    screenspace.location = (0.0, 0.0)

    if explicit_view_position is not None:
        combine_xyz = nodes.new("ShaderNodeCombineXYZ")
        combine_xyz.location = (-220.0, 0.0)
        combine_xyz.inputs["X"].default_value = explicit_view_position[0]
        combine_xyz.inputs["Y"].default_value = explicit_view_position[1]
        combine_xyz.inputs["Z"].default_value = explicit_view_position[2]
        links.new(combine_xyz.outputs["Vector"], screenspace.inputs["View Position"])

    if direct_scene_color:
        links.new(screenspace.outputs["Scene Color"], output.inputs["Surface"])
    else:
        links.new(screenspace.outputs["Scene Color"], emission.inputs["Color"])
        links.new(emission.outputs["Emission"], output.inputs["Surface"])

    return material, screenspace, emission, output


def render_center_rgb():
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
        width = image.size[0]
        height = image.size[1]
        pixel_index = ((height // 2) * width + (width // 2)) * 4
        return list(image.pixels[pixel_index : pixel_index + 4])
    finally:
        bpy.data.images.remove(image)
        if os.path.exists(filepath):
            os.remove(filepath)


def assert_is_red(pixel, label):
    r, g, b, _a = pixel
    assert r > 0.5, f"{label}: expected strong red, got {pixel}"
    assert g < 0.2, f"{label}: expected low green, got {pixel}"
    assert b < 0.2, f"{label}: expected low blue, got {pixel}"


def assert_is_non_black(pixel, label):
    assert max(pixel[:3]) > 0.05, f"{label}: expected non-black depth, got {pixel}"


def run_case(
    label,
    surface_render_method,
    use_raytracing,
    explicit_view_position=None,
    direct_scene_color=False,
    check_depth=True,
):
    clear_scene()
    configure_scene(use_raytracing)
    make_camera()

    back_plane = make_plane(f"Back_{label}", 0.0)
    assign_single_material(
        back_plane, make_emission_material(f"BackMaterial_{label}", (1.0, 0.0, 0.0, 1.0))
    )

    front_plane = make_plane(f"Front_{label}", 0.1)
    front_material, screenspace, emission, output = make_screenspace_material(
        f"FrontMaterial_{label}",
        surface_render_method,
        explicit_view_position,
        direct_scene_color,
    )
    assign_single_material(front_plane, front_material)

    color_pixel = render_center_rgb()
    assert_is_red(color_pixel, f"{label} scene color")

    if not check_depth:
        return

    front_material.node_tree.links.clear()
    front_material.node_tree.links.new(screenspace.outputs["Scene Depth"], emission.inputs["Color"])
    front_material.node_tree.links.new(emission.outputs["Emission"], output.inputs["Surface"])

    depth_pixel = render_center_rgb()
    assert_is_non_black(depth_pixel, f"{label} scene depth")


def main():
    assert hasattr(bpy.types, "ShaderNodeScreenspaceInfo"), (
        "ShaderNodeScreenspaceInfo is not registered"
    )

    run_case("blended_current_pixel", "BLENDED", False)
    run_case("blended_explicit_view_position", "BLENDED", False, (0.0, 0.0, -4.0))
    run_case("dithered_current_pixel", "DITHERED", True)
    run_case("dithered_explicit_view_position", "DITHERED", True, (0.0, 0.0, -4.0))
    run_case(
        "dithered_direct_current_pixel",
        "DITHERED",
        True,
        direct_scene_color=True,
        check_depth=False,
    )
    run_case(
        "dithered_direct_explicit_view_position",
        "DITHERED",
        True,
        (0.0, 0.0, -4.0),
        direct_scene_color=True,
        check_depth=False,
    )

    print("SCREENSPACE_INFO_BACKEND=" + gpu.platform.backend_type_get(), flush=True)
    print("SCREENSPACE_INFO_VIEWPORT_PARITY_OK", flush=True)


if __name__ == "__main__":
    main()
