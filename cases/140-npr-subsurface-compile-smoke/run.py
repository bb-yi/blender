import math
import os
import tempfile

import bpy


RESOLUTION = 96


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
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    scene.world.use_nodes = False
    scene.world.color = (0.0, 0.0, 0.0)


def make_camera():
    camera_data = bpy.data.cameras.new("Camera")
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, -4.0, 0.0)
    camera.rotation_euler = (math.radians(90.0), 0.0, 0.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def make_light():
    bpy.ops.object.light_add(type="POINT", location=(0.0, -3.0, 2.0))
    light = bpy.context.active_object
    light.data.energy = 600.0


def make_npr_tree():
    tree = bpy.data.node_groups.new("SubsurfaceNPRTree", "ShaderNodeTree")
    nodes = tree.nodes
    links = tree.links
    npr_input = nodes.new("ShaderNodeNPR_Input")
    npr_output = nodes.new("ShaderNodeNPR_Output")
    npr_output.location = (320.0, 0.0)
    links.new(npr_input.outputs["Combined Color"], npr_output.inputs["Color"])
    return tree


def set_input_if_exists(node, name, value):
    if name in node.inputs:
        node.inputs[name].default_value = value


def make_subsurface_material(npr_tree):
    material = bpy.data.materials.new("NPRSubsurfaceMaterial")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    principled = nodes["Principled BSDF"]
    principled.inputs["Base Color"].default_value = (1.0, 0.25, 0.16, 1.0)
    set_input_if_exists(principled, "Subsurface Weight", 0.55)
    set_input_if_exists(principled, "Subsurface Scale", 0.35)
    set_input_if_exists(principled, "Subsurface Radius", (1.0, 0.35, 0.2))
    set_input_if_exists(principled, "Roughness", 0.45)

    output = nodes["Material Output"]
    output.nprtree = npr_tree
    return material


def make_sphere(material):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16, radius=1.0, location=(0.0, 0.0, 0.0))
    sphere = bpy.context.active_object
    sphere.data.materials.append(material)


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


def brightness(rgb):
    return max(rgb)


clear_scene()
configure_scene()
make_camera()
make_light()
make_sphere(make_subsurface_material(make_npr_tree()))

pixels, width, height = render_pixels()
center = sample_rgb(pixels, width, height, 0.5, 0.5)
corner = sample_rgb(pixels, width, height, 0.05, 0.05)

print(f"NPR_SUBSURFACE_CENTER_RGB={center}")
print(f"NPR_SUBSURFACE_CORNER_RGB={corner}")

assert brightness(center) > 0.05, f"Expected visible NPR subsurface sphere center, got {center}"
assert brightness(corner) < 0.08, f"Expected background corner to stay near-black, got {corner}"
assert brightness(center) > brightness(corner) + 0.2, (
    f"Expected subsurface sphere center to stay much brighter than the background, "
    f"got center {center} and corner {corner}"
)
