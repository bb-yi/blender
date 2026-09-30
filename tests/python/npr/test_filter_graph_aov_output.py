import bpy
import os
import sys
import tempfile
from pathlib import Path

import OpenImageIO as oiio

sys.path.insert(0, str(Path(__file__).resolve().parent))

from filter_graph_test_utils import (
    add_pass_input_image_sample,
    clear_filter_graph,
    new_filter_graph,
    refresh_tree,
)


RESOLUTION = 64
AOV_NAME = "GraphAOVOutput"
DONE_MARKER = "__FILTER_GRAPH_AOV_OUTPUT_DONE__"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


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

    scene.use_nodes = True
    compositor_tree = bpy.data.node_groups.new("FilterGraphAOVOutput", "CompositorNodeTree")
    scene.compositing_node_group = compositor_tree

    clear_filter_graph(scene)

    view_layer = bpy.context.view_layer
    while len(view_layer.aovs) > 0:
        view_layer.aovs.remove(view_layer.aovs[0])
    aov = view_layer.aovs.add()
    aov.name = AOV_NAME
    aov.type = "COLOR"


def make_camera():
    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 4.0
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 5.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def make_plane_material():
    material = bpy.data.materials.new("AOVSource")
    material.use_nodes = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (0.25, 0.5, 0.75, 1.0)
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    return material


def make_filter_material():
    """Filter pass outputs a recognizable constant color regardless of its image input."""
    material = bpy.data.materials.new("FilterGraphAOVOutput")
    material.use_nodes = True
    material.eevee_domain = "FILTER"

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputFilter")
    output.inputs["Alpha"].default_value = 1.0

    _, image_sample = add_pass_input_image_sample(nodes, links, location=(-260.0, 0.0))

    multiply = nodes.new("ShaderNodeMixRGB")
    multiply.blend_type = "MIX"
    multiply.inputs[0].default_value = 1.0
    multiply.inputs[2].default_value = (0.9, 0.6, 0.3, 1.0)

    links.new(image_sample.outputs["Color"], multiply.inputs[1])
    links.new(multiply.outputs["Color"], output.inputs["Color"])

    return material


def build_graph():
    scene = bpy.context.scene
    graph = new_filter_graph(scene)
    nodes = graph.nodes
    links = graph.links

    filter_pass = nodes.new("EeveeFilterGraphNodeFilterMaterial")
    filter_pass.location = (-400.0, 0.0)
    filter_pass.material = make_filter_material()
    refresh_tree(graph)
    require(
        "Image" in filter_pass.inputs,
        "Filter Pass has no Image input after material sync",
    )

    aov_output = nodes.new("EeveeFilterGraphNodeAOVOutput")
    aov_output.location = (0.0, 0.0)
    aov_output.aov_name = AOV_NAME
    require("Color" in aov_output.inputs, "AOV Output has no Color input")
    require(
        aov_output.execution_stage == "BEFORE_COMPOSITE",
        f"AOV Output default execution stage is {aov_output.execution_stage!r}",
    )

    links.new(filter_pass.outputs["Image"], aov_output.inputs["Color"])

    refresh_tree(graph)
    return graph


def make_plane(material):
    bpy.ops.mesh.primitive_plane_add(size=4.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.active_object
    plane.data.materials.append(material)
    return plane


def build_compositor_aov_capture(tree, output_dir):
    """Route the view-layer AOV into a compositor file output, verifying compositor access."""
    render_layers = tree.nodes.new("CompositorNodeRLayers")
    render_layers.layer = bpy.context.view_layer.name
    require(
        AOV_NAME in render_layers.outputs,
        f"Render Layers has no {AOV_NAME!r} output; found AOV sockets only",
    )

    file_output = tree.nodes.new("CompositorNodeOutputFile")
    file_output.directory = str(output_dir)
    file_output.file_name = "filter_graph_aov_output"
    file_output.format.file_format = "OPEN_EXR_MULTILAYER"
    file_output.file_output_items.clear()
    file_output.file_output_items.new("RGBA", AOV_NAME)

    tree.links.new(render_layers.outputs[AOV_NAME], file_output.inputs[AOV_NAME])
    return file_output


def read_multilayer_exr(path):
    require(path is not None and path.exists(), f"Compositor EXR missing: {path}")
    image_input = oiio.ImageInput.open(str(path))
    require(image_input is not None, f"Could not open {path}")
    passes = {}
    try:
        subimage = 0
        while image_input.seek_subimage(subimage, 0):
            spec = image_input.spec()
            name = spec.get_string_attribute("oiio:subimagename")
            pixels = image_input.read_image(format=oiio.FLOAT)
            require(pixels is not None, f"Could not read {name!r} from {path}")
            passes[name] = pixels
            subimage += 1
    finally:
        image_input.close()
    return passes


def render_compositor_aov():
    output_dir = Path(tempfile.mkdtemp(prefix="filter_graph_aov_output_"))
    build_compositor_aov_capture(bpy.context.scene.compositing_node_group, output_dir)

    bpy.context.view_layer.update()
    bpy.ops.render.render()

    paths = sorted(output_dir.glob("*.exr"))
    require(len(paths) == 1, f"Expected exactly one compositor EXR, found: {paths}")
    passes = read_multilayer_exr(paths[0])
    require(AOV_NAME in passes, f"AOV {AOV_NAME!r} missing from compositor EXR; found {sorted(passes)}")

    import shutil

    shutil.rmtree(output_dir, ignore_errors=True)

    return passes[AOV_NAME]


def sample_color(pixels, x, y):
    height, width = pixels.shape[:2]
    x = min(x, width - 1)
    y_flip = height - 1 - min(y, height - 1)
    return list(pixels[y_flip, x][:4])


def main():
    clear_scene()
    configure_scene()
    make_camera()
    make_plane(make_plane_material())
    build_graph()

    pixels = render_compositor_aov()

    center = sample_color(pixels, RESOLUTION // 2, RESOLUTION // 2)
    corner = sample_color(pixels, 3, 3)

    print(f"FILTER_GRAPH_AOV_OUTPUT_CENTER={center}")
    print(f"FILTER_GRAPH_AOV_OUTPUT_CORNER={corner}")

    require(
        center[0] > 0.8 and abs(center[1] - 0.6) < 0.05 and abs(center[2] - 0.3) < 0.05,
        f"Expected AOV center to hold the filter pass output (0.9, 0.6, 0.3), got {center}",
    )
    require(
        corner[0] > 0.8,
        f"Expected full-extent AOV write including plane corner pixel, got {corner}",
    )

    print(DONE_MARKER)


if __name__ == "__main__":
    main()
