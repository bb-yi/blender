import bpy
import os
import tempfile


RESOLUTION = 128


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

    while len(scene.eevee.overlay_inputs) > 0:
        scene.eevee.overlay_inputs.remove(0)


def make_camera():
    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 4.0
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 5.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def make_surface_material():
    material = bpy.data.materials.new("OverlayReceiver")
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (0.0, 0.0, 1.0, 1.0)

    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_plane(material):
    bpy.ops.mesh.primitive_plane_add(size=4.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.active_object
    plane.data.materials.append(material)


def make_overlay_image():
    image = bpy.data.images.new("OverlayColor", width=4, height=4, alpha=True, float_buffer=True)
    image.colorspace_settings.name = "Linear Rec.709"
    image.pixels = [1.0, 0.0, 0.0, 1.0] * (4 * 4)
    return image


def attach_overlay(image):
    entry = bpy.context.scene.eevee.overlay_inputs.add()
    entry.enabled = True
    entry.color_image = image
    entry.opacity = 1.0
    entry.alpha_mode = "STRAIGHT"
    return entry


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
    finally:
        bpy.data.images.remove(image)
        if os.path.exists(filepath):
            os.remove(filepath)

    return pixels


def sample_center_color(pixels):
    index = ((RESOLUTION // 2) * RESOLUTION + (RESOLUTION // 2)) * 4
    return list(pixels[index:index + 4])


def main():
    clear_scene()
    configure_scene()
    make_camera()
    make_plane(make_surface_material())

    base_pixels = render_image()
    base_color = sample_center_color(base_pixels)
    assert base_color[2] > 0.9, f"Expected blue baseline render, got {base_color}"

    overlay_image = make_overlay_image()
    entry = attach_overlay(overlay_image)

    assert tuple(round(v, 6) for v in entry.scale) == (1.0, 1.0), (
        f"Expected default overlay scale to be (1, 1), got {tuple(entry.scale)}"
    )

    entry.offset = (0.125, -0.25)
    entry.scale = (1.5, 0.75)
    entry.blend_mode = "SCREEN"

    assert tuple(round(v, 6) for v in entry.offset) == (0.125, -0.25), (
        f"Expected overlay offset round-trip, got {tuple(entry.offset)}"
    )
    assert tuple(round(v, 6) for v in entry.scale) == (1.5, 0.75), (
        f"Expected overlay scale round-trip, got {tuple(entry.scale)}"
    )
    assert entry.blend_mode == "SCREEN", f"Expected overlay blend mode round-trip, got {entry.blend_mode}"

    entry.offset = (0.0, 0.0)
    entry.scale = (1.0, 1.0)
    entry.blend_mode = "NORMAL"

    overlay_pixels = render_image()
    overlay_color = sample_center_color(overlay_pixels)

    assert overlay_color[0] > 0.9, f"Expected overlay render to be red, got {overlay_color}"
    assert overlay_color[2] < 0.1, f"Expected overlay to replace blue scene color, got {overlay_color}"


if __name__ == "__main__":
    main()
