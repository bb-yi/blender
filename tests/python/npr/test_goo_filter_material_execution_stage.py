import bpy
import gpu
import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from filter_graph_test_utils import (
    add_pass_input_image_sample,
    attach_filter_material as attach_filter_material_to_graph,
    clear_filter_graph,
)


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

    clear_filter_graph(scene)


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

    _, image_sample = add_pass_input_image_sample(nodes, links, location=(-40.0, 0.0))

    invert = nodes.new("ShaderNodeInvert")
    invert.location = (220.0, 0.0)
    invert.inputs["Fac"].default_value = 1.0

    links.new(image_sample.outputs["Color"], invert.inputs["Color"])
    links.new(invert.outputs["Color"], output.inputs["Color"])
    output.inputs["Alpha"].default_value = 1.0
    return material


def make_scale_filter_material(scale):
    material = bpy.data.materials.new(f"StageScale_{scale:.1f}")
    material.use_nodes = True
    material.eevee_domain = "FILTER"

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputFilter")
    _, image_sample = add_pass_input_image_sample(nodes, links, location=(-40.0, 0.0))

    multiply = nodes.new("ShaderNodeMixRGB")
    multiply.blend_type = "MULTIPLY"
    multiply.inputs[0].default_value = 1.0
    multiply.inputs[2].default_value = (scale, scale, scale, 1.0)

    links.new(image_sample.outputs["Color"], multiply.inputs[1])
    links.new(multiply.outputs["Color"], output.inputs["Color"])
    output.inputs["Alpha"].default_value = 1.0
    return material


def make_plane(material):
    bpy.ops.mesh.primitive_plane_add(size=4.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.active_object
    plane.data.materials.append(material)


def attach_filter_material(material):
    return attach_filter_material_to_graph(
        material, stage="BEFORE_COMPOSITE", scene_socket="Color Image"
    )


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
    _, _, stage_output = attach_filter_material(make_filter_material())

    assert stage_output.execution_stage == "BEFORE_COMPOSITE", (
        f"Expected graph output stage to be BEFORE_COMPOSITE, got {stage_output.execution_stage}"
    )

    for stage in ("BEFORE_VOLUME_FOG", "BEFORE_DEPTH_OF_FIELD", "BEFORE_COMPOSITE"):
        stage_output.execution_stage = stage
        color = sample_center_color(render_image())

        assert stage_output.execution_stage == stage, (
            f"Expected graph output stage round-trip to keep {stage}, got {stage_output.execution_stage}"
        )
        assert color[0] > 0.9 and color[1] > 0.9, (
            f"Expected filter output to invert the blue surface for stage {stage}, got {color}"
        )
        assert color[2] < 0.1, (
            f"Expected filter output to remove the blue channel for stage {stage}, got {color}"
        )

    clear_filter_graph()
    graph = None
    stage_outputs = []
    stage_scales = (
        ("BEFORE_VOLUME_FOG", 0.9),
        ("BEFORE_DEPTH_OF_FIELD", 0.8),
        ("BEFORE_COMPOSITE", 0.7),
        ("BEFORE_POSTFX", 0.6),
    )
    for stage, scale in stage_scales:
        graph, _, stage_output = attach_filter_material_to_graph(
            make_scale_filter_material(scale),
            stage=stage,
            scene_socket="Color Image",
            graph=graph,
        )
        assert stage_output.execution_stage == stage
        stage_outputs.append(stage_output)

    assert all(output.is_active_output for output in stage_outputs), (
        "Expected one active Filter Graph output for each execution stage"
    )

    color = sample_center_color(render_image())
    expected_blue = 0.9 * 0.8 * 0.7 * 0.6
    assert color[0] < 0.05 and color[1] < 0.05 and abs(color[2] - expected_blue) < 0.02, (
        "Expected all four stage-specific scales to produce a unique blue value, "
        f"got {color}"
    )

    print("__FILTER_STAGE_BACKEND__=" + gpu.platform.backend_type_get(), flush=True)
    print("__FILTER_STAGE_MULTISTAGE_DONE__", flush=True)


if __name__ == "__main__":
    main()
