import bpy
import os
import tempfile


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def configure_scene():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 160
    scene.render.resolution_y = 96
    scene.render.resolution_percentage = 100
    scene.eevee.taa_samples = 1
    scene.eevee.taa_render_samples = 1
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"

    world = scene.world
    world.use_nodes = True
    nodes = world.node_tree.nodes
    links = world.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputWorld")
    background = nodes.new("ShaderNodeBackground")
    background.inputs["Color"].default_value = (0.0, 1.0, 0.0, 1.0)
    background.inputs["Strength"].default_value = 2.0
    links.new(background.outputs["Background"], output.inputs["Surface"])


def make_camera():
    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 4.0
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, -4.0, 0.0)
    camera.rotation_euler = (1.57079632679, 0.0, 0.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def make_diffuse_material(name):
    material = bpy.data.materials.new(name)
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    diffuse = nodes.new("ShaderNodeBsdfDiffuse")
    diffuse.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)

    links.new(diffuse.outputs["BSDF"], output.inputs["Surface"])
    return material


def make_uv_sphere(name, location, material):
    bpy.ops.mesh.primitive_uv_sphere_add(radius=0.45, location=location)
    obj = bpy.context.active_object
    obj.name = name
    obj.data.materials.clear()
    obj.data.materials.append(material)
    return obj


def render_pixels():
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

        def sample(pixel_x, pixel_y):
            index = (pixel_y * width + pixel_x) * 4
            return list(image.pixels[index:index + 4])

        return {
            "left": sample(width // 4, height // 2),
            "right": sample((width * 3) // 4, height // 2),
        }
    finally:
        bpy.data.images.remove(image)
        if os.path.exists(filepath):
            os.remove(filepath)


clear_scene()
configure_scene()
make_camera()

material = make_diffuse_material("DiffuseWhite")

make_uv_sphere("IncludedSphere", (-0.9, 0.0, 0.0), material)
excluded = make_uv_sphere("ExcludedSphere", (0.9, 0.0, 0.0), material)

exclude_collection = bpy.data.collections.new("EnvironmentExcluded")
bpy.context.scene.collection.children.link(exclude_collection)
for collection in tuple(excluded.users_collection):
    collection.objects.unlink(excluded)
exclude_collection.objects.link(excluded)
bpy.context.scene.world.environment_exclusion_collection = exclude_collection

bpy.context.view_layer.update()
pixels = render_pixels()

left = pixels["left"]
right = pixels["right"]

assert left[1] > 0.12, (
    f"Expected non-excluded sphere to receive green environment light, got {left}"
)
assert right[1] < 0.03, f"Expected excluded sphere to lose environment lighting, got {right}"
assert left[1] > right[1] + 0.08, (
    f"Expected clear environment-lighting difference, got left={left}, right={right}"
)
