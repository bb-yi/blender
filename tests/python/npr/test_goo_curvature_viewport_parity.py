import bpy
import os
import tempfile


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def make_camera():
    camera_data = bpy.data.cameras.new("Camera")
    camera = bpy.data.objects.new("Camera", camera_data)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera
    camera.location = (0.0, 0.0, 4.0)
    camera.rotation_euler = (0.0, 0.0, 0.0)
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 2.4
    return camera


def configure_scene():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 192
    scene.render.resolution_y = 192
    scene.render.resolution_percentage = 100
    scene.eevee.taa_render_samples = 1
    scene.eevee.taa_samples = 1
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.world.color = (0.0, 0.0, 0.0)


def make_curvature_material(name, output_name):
    material = bpy.data.materials.new(name)
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    output.location = (480.0, 0.0)

    emission = nodes.new("ShaderNodeEmission")
    emission.location = (240.0, 0.0)
    emission.inputs["Strength"].default_value = 1.0

    curvature = nodes.new("ShaderNodeCurvature")
    curvature.location = (0.0, 0.0)
    curvature.inputs["Samples"].default_value = 8.0
    curvature.inputs["Sample Radius"].default_value = 1.0
    curvature.inputs["Thickness"].default_value = 1.0
    curvature.inputs["Scale"].default_value = (1.0, 1.0, 0.0)

    links.new(curvature.outputs[output_name], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_plane(name):
    bpy.ops.mesh.primitive_plane_add(size=1.6, location=(0.0, 0.0, 0.0))
    obj = bpy.context.active_object
    obj.name = name
    return obj


def make_sphere(name):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=64, ring_count=32, radius=0.8, location=(0.0, 0.0, 0.0))
    obj = bpy.context.active_object
    obj.name = name
    return obj


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


def sample_pixel(pixels, width, height, x_ratio, y_ratio):
    x = min(width - 1, max(0, int(width * x_ratio)))
    y = min(height - 1, max(0, int(height * y_ratio)))
    pixel_index = (y * width + x) * 4
    return list(pixels[pixel_index:pixel_index + 4])


def assert_plane_vs_sphere_curvature():
    clear_scene()
    make_camera()
    configure_scene()

    plane = make_plane("CurvaturePlane")
    plane.data.materials.append(make_curvature_material("PlaneCurvature", "Scene Curvature"))
    plane_pixels, plane_width, plane_height = render_image()
    plane_center = sample_pixel(plane_pixels, plane_width, plane_height, 0.5, 0.5)
    plane_edge = sample_pixel(plane_pixels, plane_width, plane_height, 0.75, 0.5)

    clear_scene()
    make_camera()
    configure_scene()

    sphere = make_sphere("CurvatureSphere")
    sphere.data.materials.append(make_curvature_material("SphereCurvature", "Scene Curvature"))
    sphere_pixels, sphere_width, sphere_height = render_image()
    sphere_center = sample_pixel(sphere_pixels, sphere_width, sphere_height, 0.5, 0.5)
    sphere_mid = sample_pixel(sphere_pixels, sphere_width, sphere_height, 0.7, 0.5)

    plane_value = plane_center[0]
    sphere_value = sphere_center[0]
    plane_edge_value = plane_edge[0]
    sphere_mid_value = sphere_mid[0]

    assert plane_value < 0.05, f"Plane curvature should stay near zero, got {plane_center}"
    assert plane_edge_value < 0.05, f"Plane curvature should stay low away from center too, got {plane_edge}"
    assert sphere_value > plane_value + 0.1, (
        f"Sphere curvature should exceed plane curvature, got plane={plane_center} sphere={sphere_center}"
    )
    assert sphere_value > sphere_mid_value + 0.1, (
        f"Sphere curvature should peak toward the center, got center={sphere_center} mid={sphere_mid}"
    )


def assert_rim_profile():
    clear_scene()
    make_camera()
    configure_scene()

    plane = make_plane("RimPlane")
    plane.data.materials.append(make_curvature_material("PlaneRim", "Scene Rim"))
    plane_pixels, plane_width, plane_height = render_image()
    plane_center = sample_pixel(plane_pixels, plane_width, plane_height, 0.5, 0.5)

    clear_scene()
    make_camera()
    configure_scene()

    sphere = make_sphere("RimSphere")
    sphere.data.materials.append(make_curvature_material("SphereRim", "Scene Rim"))
    pixels, width, height = render_image()

    center = sample_pixel(pixels, width, height, 0.5, 0.5)
    mid = sample_pixel(pixels, width, height, 0.7, 0.5)
    edge = sample_pixel(pixels, width, height, 0.75, 0.5)
    silhouette = sample_pixel(pixels, width, height, 0.8, 0.5)

    assert plane_center[0] < 1e-5, f"Plane rim center should stay at zero, got {plane_center}"
    assert center[0] < 0.01, f"Sphere rim center should stay dark, got {center}"
    assert mid[0] > center[0] * 2.0, f"Sphere rim should build up away from center, got center={center} mid={mid}"
    assert edge[0] > center[0] * 5.0, f"Sphere rim edge should brighten, got center={center} edge={edge}"
    assert silhouette[0] > edge[0], (
        f"Sphere rim should continue rising toward the silhouette, got edge={edge} silhouette={silhouette}"
    )


assert hasattr(bpy.types, "ShaderNodeCurvature"), "ShaderNodeCurvature is not registered"

assert_plane_vs_sphere_curvature()
assert_rim_profile()
