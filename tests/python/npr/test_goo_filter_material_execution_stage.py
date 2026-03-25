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

    while len(scene.eevee.filter_materials) > 0:
        scene.eevee.filter_materials.remove(0)


def make_camera():
    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 4.0
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 5.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def make_surface_material():
    material = bpy.data.materials.new("StageSurface")
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (0.0, 0.0, 1.0, 1.0)

    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_filter_material():
    material = bpy.data.materials.new("StageFilter")
    material.use_nodes = True
    material.eevee_domain = "FILTER"

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputFilter")
    output.location = (420.0, 0.0)

    scene_color = nodes.new("ShaderNodeSceneColor")
    scene_color.location = (0.0, 0.0)
    scene_color.source = "COLOR"

    invert = nodes.new("ShaderNodeInvert")
    invert.location = (220.0, 0.0)
    invert.inputs["Fac"].default_value = 1.0

    links.new(scene_color.outputs["Color"], invert.inputs["Color"])
    links.new(invert.outputs["Color"], output.inputs["Color"])
    output.inputs["Alpha"].default_value = 1.0
    return material


def make_plane(material):
    bpy.ops.mesh.primitive_plane_add(size=4.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.active_object
    plane.data.materials.append(material)


def attach_filter_material(material):
    entry = bpy.context.scene.eevee.filter_materials.add()
    entry.material = material
    entry.enabled = True
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
    filter_entry = attach_filter_material(make_filter_material())

    assert filter_entry.execution_stage == "BEFORE_COMPOSITE", (
        f"Expected default filter stage to be BEFORE_COMPOSITE, got {filter_entry.execution_stage}"
    )

    for stage in ("BEFORE_VOLUME_FOG", "BEFORE_DEPTH_OF_FIELD", "BEFORE_COMPOSITE"):
        filter_entry.execution_stage = stage
        color = sample_center_color(render_image())

        assert filter_entry.execution_stage == stage, (
            f"Expected filter stage round-trip to keep {stage}, got {filter_entry.execution_stage}"
        )
        assert color[0] > 0.9 and color[1] > 0.9, (
            f"Expected filter output to invert the blue surface for stage {stage}, got {color}"
        )
        assert color[2] < 0.1, (
            f"Expected filter output to remove the blue channel for stage {stage}, got {color}"
        )


if __name__ == "__main__":
    main()
