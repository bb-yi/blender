import bpy
import os
import tempfile


RESOLUTION = 256
ORTHO_SCALE = 8.0


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
    while len(scene.eevee.filter_materials) > 0:
        scene.eevee.filter_materials.remove(0)


def make_camera():
    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = ORTHO_SCALE
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 6.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def make_surface_material():
    material = bpy.data.materials.new("ShadowReceiver")
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    output.location = (420.0, 0.0)

    diffuse = nodes.new("ShaderNodeBsdfDiffuse")
    diffuse.location = (180.0, 0.0)
    diffuse.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    diffuse.inputs["Roughness"].default_value = 0.0

    links.new(diffuse.outputs["BSDF"], output.inputs["Surface"])
    return material


def make_filter_material():
    material = bpy.data.materials.new("StableShadowFilter")
    material.use_nodes = True
    material.eevee_domain = "FILTER"

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputFilter")
    output.location = (320.0, 0.0)

    scene_color = nodes.new("ShaderNodeSceneColor")
    scene_color.location = (0.0, 0.0)
    scene_color.source = "SHADOW"

    assert scene_color.source == "SHADOW", "Scene Color source should accept SHADOW"

    links.new(scene_color.outputs["Color"], output.inputs["Color"])
    links.new(scene_color.outputs["Alpha"], output.inputs["Alpha"])
    return material


def attach_filter_material(material):
    scene = bpy.context.scene
    filter_entry = scene.eevee.filter_materials.add()
    filter_entry.material = material
    filter_entry.enabled = True


def make_plane(material):
    bpy.ops.mesh.primitive_plane_add(size=8.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.active_object
    plane.data.materials.append(material)


def make_blocker():
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, 1.0))
    blocker = bpy.context.active_object
    blocker.scale = (0.5, 0.5, 1.0)
    blocker.name = "ShadowBlocker"


def make_light():
    light_data = bpy.data.lights.new("ShadowPoint", type="POINT")
    light_data.energy = 5000.0
    light_data.shadow_soft_size = 0.2
    light_data.use_shadow = True
    light = bpy.data.objects.new("ShadowPoint", light_data)
    light.location = (-4.0, 0.0, 4.0)
    bpy.context.scene.collection.objects.link(light)


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


def sample_world_point(pixels, width, height, world_x, world_y):
    x_ratio = (world_x / ORTHO_SCALE) + 0.5
    y_ratio = (world_y / ORTHO_SCALE) + 0.5
    x = min(width - 1, max(0, int(width * x_ratio)))
    y = min(height - 1, max(0, int(height * y_ratio)))
    index = (y * width + x) * 4
    return list(pixels[index:index + 4])


def build_scene():
    clear_scene()
    configure_scene()
    make_camera()
    make_plane(make_surface_material())
    make_blocker()
    make_light()
    attach_filter_material(make_filter_material())


def main():
    build_scene()
    pixels, width, height = render_image()

    lit = sample_world_point(pixels, width, height, -1.6, 0.0)
    shadowed = sample_world_point(pixels, width, height, 1.4, 0.0)

    assert lit[0] > 0.7, f"Expected lit shadow visibility near white, got {lit}"
    assert shadowed[0] < lit[0] - 0.1, (
        f"Expected shadowed region to be darker than lit region, got lit={lit} shadowed={shadowed}"
    )


if __name__ == "__main__":
    main()
