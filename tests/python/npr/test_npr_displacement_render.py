import bpy
import os
import tempfile


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def configure_scene():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 64
    scene.render.resolution_y = 64
    scene.render.resolution_percentage = 100
    scene.eevee.taa_samples = 1
    scene.eevee.taa_render_samples = 1
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.world.use_nodes = False
    scene.world.color = (0.0, 0.0, 0.0)


def make_camera():
    camera_data = bpy.data.cameras.new("Camera")
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 5.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def make_material():
    material = bpy.data.materials.new("NPRDisplacementMaterial")
    material.use_nodes = True
    material.displacement_method = "DISPLACEMENT"
    material.max_vertex_displacement = 2.0

    nodes = material.node_tree.nodes
    links = material.node_tree.links

    output = next(node for node in nodes if node.bl_idname == "ShaderNodeOutputMaterial")
    principled = next(node for node in nodes if node.bl_idname == "ShaderNodeBsdfPrincipled")
    principled.inputs["Base Color"].default_value = (1.0, 0.0, 0.0, 1.0)
    principled.inputs["Roughness"].default_value = 1.0

    displacement = nodes.new("ShaderNodeDisplacement")
    displacement.inputs["Height"].default_value = 1.0
    displacement.inputs["Midlevel"].default_value = 0.0
    displacement.inputs["Scale"].default_value = 1.0
    links.new(displacement.outputs["Displacement"], output.inputs["Displacement"])

    npr_tree = bpy.data.node_groups.new("NPRDisplacementTree", "ShaderNodeTree")
    npr_nodes = npr_tree.nodes
    npr_links = npr_tree.links

    rgb = npr_nodes.new("ShaderNodeRGB")
    rgb.outputs["Color"].default_value = (0.0, 1.0, 0.0, 1.0)
    npr_output = npr_nodes.new("ShaderNodeNPR_Output")
    npr_links.new(rgb.outputs["Color"], npr_output.inputs["Color"])

    output.nprtree = npr_tree
    return material


def make_plane(material):
    bpy.ops.mesh.primitive_plane_add(size=2.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.active_object
    plane.data.materials.append(material)


def render_center_pixel():
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
        index = ((height // 2) * width + (width // 2)) * 4
        pixel = list(image.pixels[index:index + 4])
    finally:
        bpy.data.images.remove(image)
        if os.path.exists(filepath):
            os.remove(filepath)

    return pixel


def assert_green(pixel, label):
    r, g, b, _a = pixel
    assert g > 0.8, f"Expected {label} render to keep NPR green output, got {pixel}"
    assert r < 0.1, f"Expected {label} render to suppress red base shading, got {pixel}"
    assert b < 0.1, f"Expected {label} render to keep blue low, got {pixel}"


clear_scene()
configure_scene()
make_camera()
material = make_material()
make_plane(material)

material.displacement_method = "BUMP"
bump_pixel = render_center_pixel()
assert_green(bump_pixel, "bump")

material.displacement_method = "DISPLACEMENT"
displacement_pixel = render_center_pixel()
assert_green(displacement_pixel, "displacement")

print(f"NPR_BUMP_CENTER={bump_pixel}")
print(f"NPR_DISPLACEMENT_CENTER={displacement_pixel}")
