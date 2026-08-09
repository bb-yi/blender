import os
import tempfile

import bpy


RESOLUTION = 96


def view3d_spaces():
    return [
        area.spaces.active
        for screen in bpy.data.screens
        for area in screen.areas
        if area.type == "VIEW_3D" and area.spaces.active is not None
    ]


def assert_depth_render_pass_rna():
    spaces = view3d_spaces()
    assert spaces, "Factory startup should provide at least one VIEW_3D space"

    enum_items = spaces[0].shading.bl_rna.properties["render_pass"].enum_items
    identifiers = {item.identifier for item in enum_items if item.identifier}
    assert "DEPTH" in identifiers, f"View3DShading.render_pass is missing DEPTH: {sorted(identifiers)}"

    for space in spaces:
        space.shading.type = "RENDERED"
        space.shading.render_pass = "DEPTH"
        assert space.shading.render_pass == "DEPTH", (
            "View3DShading.render_pass should keep DEPTH instead of falling back to "
            f"{space.shading.render_pass!r}"
        )

    print(f"VIEW3D_DEPTH_RENDER_PASS_SPACES={len(spaces)}")


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
    camera_data.ortho_scale = 4.0
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 6.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def make_emission_material(name, color):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = color
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_plane(name, location, size, material):
    bpy.ops.mesh.primitive_plane_add(size=size, location=location)
    plane = bpy.context.active_object
    plane.name = name
    plane.data.materials.append(material)
    return plane


def render_pixels():
    file_descriptor, filepath = tempfile.mkstemp(suffix=".exr")
    os.close(file_descriptor)

    scene = bpy.context.scene
    scene.render.image_settings.file_format = "OPEN_EXR"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "32"
    scene.render.filepath = filepath

    bpy.ops.render.render(write_still=False)
    bpy.data.images["Render Result"].save_render(filepath)

    image = bpy.data.images.load(filepath, check_existing=False)
    try:
        pixels = list(image.pixels[:])
        width = int(image.size[0])
        height = int(image.size[1])
    finally:
        bpy.data.images.remove(image)
        if os.path.exists(filepath):
            os.remove(filepath)
    return pixels, width, height


def sample_rgb(pixels, width, height, x_ratio, y_ratio):
    x = max(0, min(width - 1, int(width * x_ratio)))
    y = max(0, min(height - 1, int(height * y_ratio)))
    index = (y * width + x) * 4
    return pixels[index:index + 3]


assert_depth_render_pass_rna()

clear_scene()
configure_scene()
make_camera()
make_plane("BackgroundPlane", (0.0, 0.0, -1.0), 3.5, make_emission_material("BlueBack", (0.0, 0.0, 1.0, 1.0)))
make_plane("ForegroundPlane", (-0.7, 0.0, 0.2), 1.0, make_emission_material("RedFront", (1.0, 0.0, 0.0, 1.0)))

pixels, width, height = render_pixels()
front_rgb = sample_rgb(pixels, width, height, 0.32, 0.5)
back_rgb = sample_rgb(pixels, width, height, 0.72, 0.5)

print(f"VIEW3D_DEPTH_RENDER_SMOKE_FRONT={front_rgb}")
print(f"VIEW3D_DEPTH_RENDER_SMOKE_BACK={back_rgb}")

assert front_rgb[0] > 0.5 and front_rgb[2] < 0.2, f"Expected red foreground sample, got {front_rgb}"
assert back_rgb[2] > 0.5 and back_rgb[0] < 0.2, f"Expected blue background sample, got {back_rgb}"
