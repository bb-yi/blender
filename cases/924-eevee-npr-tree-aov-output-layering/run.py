from pathlib import Path
import sys
from types import SimpleNamespace

import bpy
import bl_ui.node_add_menu_shader as shader_menu
import OpenImageIO as oiio


CASE_DIR = Path(__file__).resolve().parent
ROOT = CASE_DIR.parents[3]
OUT_DIR = CASE_DIR / "out"
RESOLUTION = 96
TOLERANCE = 0.02

sys.path.insert(0, str(ROOT / "blender_npr_post" / "tests" / "python" / "npr"))

from filter_graph_test_utils import attach_filter_material, clear_filter_graph


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def output_menu_nodes(shader_type):
    visible_nodes = []
    context = SimpleNamespace(
        engine="BLENDER_EEVEE",
        space_data=SimpleNamespace(
            tree_type="ShaderNodeTree",
            shader_type=shader_type,
            id=None,
            id_from=None,
        ),
    )

    class OutputMenuProbe:
        bl_label = "Output"
        layout = object()

        def node_operator(self, _layout, node_type, **kwargs):
            if kwargs.get("poll", True):
                visible_nodes.append(node_type)

        def draw_assets_for_catalog(self, _layout, _catalog_path):
            pass

    shader_menu.NODE_MT_shader_node_output_base.draw(OutputMenuProbe(), context)
    return visible_nodes


def check_aov_output_menu_visibility():
    npr_nodes = output_menu_nodes("NPR")
    filter_nodes = output_menu_nodes("FILTER")
    require(
        "ShaderNodeOutputAOV" in npr_nodes,
        f"AOV Output is missing from the NPR Tree Add menu: {npr_nodes}",
    )
    require(
        "ShaderNodeOutputAOV" not in filter_nodes,
        f"AOV Output must remain hidden from the Filter Tree Add menu: {filter_nodes}",
    )
    print(f"NPR_AOV_OUTPUT_MENU_NODES={npr_nodes}")


def configure_scene(aov_specs):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = RESOLUTION
    scene.render.resolution_y = RESOLUTION
    scene.render.resolution_percentage = 100
    scene.eevee.taa_samples = 1
    scene.eevee.taa_render_samples = 1
    if hasattr(scene.eevee, "use_taa_reprojection"):
        scene.eevee.use_taa_reprojection = False
    if hasattr(scene.eevee, "use_raytracing"):
        scene.eevee.use_raytracing = True
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0

    scene.world = bpy.data.worlds.new("World")
    scene.world.color = (0.0, 0.0, 0.0)
    clear_filter_graph(scene)

    view_layer = bpy.context.view_layer
    for name, aov_type in aov_specs:
        aov = view_layer.aovs.add()
        aov.name = name
        aov.type = aov_type

    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 4.0
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, 0.0, 5.0)
    scene.collection.objects.link(camera)
    scene.camera = camera
    return scene


def add_aov_output(nodes, aov_name, *, color=None, value=None):
    output = nodes.new("ShaderNodeOutputAOV")
    output.aov_name = aov_name
    if color is not None:
        output.inputs["Color"].default_value = (*color, 1.0)
    if value is not None:
        output.inputs["Value"].default_value = value
    return output


def make_surface_material(name, surface_aovs, build_npr):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    material_output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    links.new(emission.outputs["Emission"], material_output.inputs["Surface"])

    for aov_name, color in surface_aovs.items():
        add_aov_output(nodes, aov_name, color=color)

    npr_tree = bpy.data.node_groups.new(f"{name} NPR Tree", "ShaderNodeTree")
    npr_output = npr_tree.nodes.new("ShaderNodeNPR_Output")
    npr_output.inputs["Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    build_npr(npr_tree)
    material_output.nprtree = npr_tree
    return material


def make_transmission_material(name, build_npr):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    material.surface_render_method = "DITHERED"
    if hasattr(material, "use_screen_refraction"):
        material.use_screen_refraction = True
    if hasattr(material, "use_raytrace_refraction"):
        material.use_raytrace_refraction = True

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()
    material_output = nodes.new("ShaderNodeOutputMaterial")
    principled = nodes.new("ShaderNodeBsdfPrincipled")
    principled.inputs["Base Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    principled.inputs["Roughness"].default_value = 0.0
    principled.inputs["Transmission Weight"].default_value = 1.0
    links.new(principled.outputs["BSDF"], material_output.inputs["Surface"])

    npr_tree = bpy.data.node_groups.new(f"{name} NPR Tree", "ShaderNodeTree")
    npr_output = npr_tree.nodes.new("ShaderNodeNPR_Output")
    npr_output.inputs["Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    build_npr(npr_tree)
    material_output.nprtree = npr_tree
    return material


def add_plane(name, size, z, material):
    bpy.ops.mesh.primitive_plane_add(size=size, location=(0.0, 0.0, z))
    plane = bpy.context.object
    plane.name = name
    plane.data.materials.append(material)
    return plane


def make_filter_material(name):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    material.eevee_domain = "FILTER"
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()
    graph_input = nodes.new("ShaderNodeFilterGraphInput")
    output = nodes.new("ShaderNodeOutputFilter")
    output.inputs["Alpha"].default_value = 1.0
    links.new(graph_input.outputs["Image"], output.inputs["Color"])
    return material


def read_multilayer_exr(path):
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


def render_passes(scene, prefix, pass_specs):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for path in OUT_DIR.glob(prefix + "*.exr"):
        path.unlink()

    tree = bpy.data.node_groups.new(f"{prefix} Compositor", "CompositorNodeTree")
    scene.compositing_node_group = tree
    render_layers = tree.nodes.new("CompositorNodeRLayers")
    render_layers.layer = bpy.context.view_layer.name
    output = tree.nodes.new("CompositorNodeOutputFile")
    output.directory = str(OUT_DIR)
    output.file_name = prefix
    output.format.file_format = "OPEN_EXR_MULTILAYER"
    output.file_output_items.clear()

    for socket_type, pass_name in pass_specs:
        require(pass_name in render_layers.outputs, f"Missing Render Layers output {pass_name!r}")
        output.file_output_items.new(socket_type, pass_name)
        tree.links.new(render_layers.outputs[pass_name], output.inputs[pass_name])

    bpy.context.view_layer.update()
    bpy.ops.render.render()
    paths = sorted(OUT_DIR.glob(prefix + "*.exr"))
    require(paths, f"Compositor did not write {prefix} EXR")
    return read_multilayer_exr(paths[-1])


def sample(pass_pixels, x_ratio, y_ratio):
    height, width = pass_pixels.shape[:2]
    x = max(0, min(width - 1, int(width * x_ratio)))
    y = max(0, min(height - 1, int(height * y_ratio)))
    pixel = pass_pixels[y, x]
    if hasattr(pixel, "__len__"):
        return tuple(float(channel) for channel in pixel)
    return (float(pixel),)


def require_pass(passes, name):
    require(name in passes, f"Missing EXR pass {name!r}; found {sorted(passes)}")
    return passes[name]


def assert_rgb(label, actual, expected):
    require(len(actual) >= 3, f"{label} has fewer than three channels: {actual}")
    error = max(abs(actual[index] - expected[index]) for index in range(3))
    require(error <= TOLERANCE, f"{label}: expected {expected}, got {actual}, error {error}")


def assert_value(label, actual, expected):
    error = abs(actual[0] - expected)
    require(error <= TOLERANCE, f"{label}: expected {expected}, got {actual}, error {error}")


def run_opaque_scenario():
    aov_specs = [
        ("NPROutColor", "COLOR"),
        ("NPROutValue", "VALUE"),
        ("SurfaceSource", "COLOR"),
        ("NPRCopy", "COLOR"),
        ("Overwrite", "COLOR"),
    ]
    scene = configure_scene(aov_specs)

    def build_npr(tree):
        nodes = tree.nodes
        links = tree.links
        add_aov_output(nodes, "NPROutColor", color=(1.0, 0.0, 0.0))
        add_aov_output(nodes, "NPROutValue", value=0.625)
        aov_input = nodes.new("ShaderNodeInputAOV")
        aov_input.aov_name = "SurfaceSource"
        copied = add_aov_output(nodes, "NPRCopy")
        links.new(aov_input.outputs["Color"], copied.inputs["Color"])
        add_aov_output(nodes, "Overwrite", color=(1.0, 0.0, 0.0))

    material = make_surface_material(
        "Opaque NPR AOV Writer",
        {"SurfaceSource": (0.0, 1.0, 0.0), "Overwrite": (0.0, 1.0, 0.0)},
        build_npr,
    )
    add_plane("Opaque NPR Plane", 4.0, 0.0, material)
    attach_filter_material(
        make_filter_material("Opaque NPR AOV Filter"),
        stage="BEFORE_COMPOSITE",
        aov_name="NPROutColor",
    )

    pass_specs = [("RGBA", "Image")]
    pass_specs.extend(("FLOAT" if kind == "VALUE" else "RGBA", name) for name, kind in aov_specs)
    passes = render_passes(scene, "opaque_npr_aov", pass_specs)
    center = (0.5, 0.5)
    results = {
        name: sample(require_pass(passes, name), *center)
        for name in ("Image", "NPROutColor", "NPROutValue", "SurfaceSource", "NPRCopy", "Overwrite")
    }
    print(f"OPAQUE_NPR_AOV_RESULTS={results}")

    assert_rgb("direct NPR Color", results["NPROutColor"], (1.0, 0.0, 0.0))
    assert_value("direct NPR Value", results["NPROutValue"], 0.625)
    assert_rgb("surface source", results["SurfaceSource"], (0.0, 1.0, 0.0))
    assert_rgb("current-surface NPR copy", results["NPRCopy"], (0.0, 1.0, 0.0))
    assert_rgb("NPR overwrite", results["Overwrite"], (1.0, 0.0, 0.0))
    assert_rgb("Filter Graph NPR Color", results["Image"], (1.0, 0.0, 0.0))


def run_layered_scenario():
    aov_specs = [
        ("LayerSource", "COLOR"),
        ("LayerRead", "COLOR"),
        ("LayerOverwrite", "COLOR"),
    ]
    scene = configure_scene(aov_specs)

    back = make_surface_material(
        "Back Layer AOV Writer",
        {"LayerSource": (0.0, 1.0, 0.0), "LayerOverwrite": (0.0, 0.0, 1.0)},
        lambda tree: None,
    )

    def build_front_npr(tree):
        nodes = tree.nodes
        links = tree.links
        aov_input = nodes.new("ShaderNodeInputAOV")
        aov_input.aov_name = "LayerSource"
        layer_read = add_aov_output(nodes, "LayerRead")
        links.new(aov_input.outputs["Color"], layer_read.inputs["Color"])
        add_aov_output(nodes, "LayerOverwrite", color=(1.0, 0.0, 0.0))

    front = make_transmission_material("Front NPR Transmission", build_front_npr)
    add_plane("Back Layer", 4.0, 0.0, back)
    add_plane("Front Layer", 2.0, 1.0, front)
    attach_filter_material(
        make_filter_material("Layer Read Filter"),
        stage="BEFORE_COMPOSITE",
        aov_name="LayerRead",
    )

    pass_specs = [("RGBA", "Image")]
    pass_specs.extend(("RGBA", name) for name, _kind in aov_specs)
    passes = render_passes(scene, "layered_npr_aov", pass_specs)
    center = (0.5, 0.5)
    corner = (0.1, 0.1)
    results = {}
    for name in ("Image", "LayerSource", "LayerRead", "LayerOverwrite"):
        pixels = require_pass(passes, name)
        results[f"{name}_center"] = sample(pixels, *center)
        results[f"{name}_corner"] = sample(pixels, *corner)
    print(f"LAYERED_NPR_AOV_RESULTS={results}")

    assert_rgb("back source at center", results["LayerSource_center"], (0.0, 1.0, 0.0))
    assert_rgb("back source at corner", results["LayerSource_corner"], (0.0, 1.0, 0.0))
    assert_rgb("front NPR layer read", results["LayerRead_center"], (0.0, 1.0, 0.0))
    assert_rgb("outside front layer read", results["LayerRead_corner"], (0.0, 0.0, 0.0))
    assert_rgb("front NPR layer overwrite", results["LayerOverwrite_center"], (1.0, 0.0, 0.0))
    assert_rgb("preserved back overwrite", results["LayerOverwrite_corner"], (0.0, 0.0, 1.0))
    assert_rgb("Filter Graph layered center", results["Image_center"], (0.0, 1.0, 0.0))
    assert_rgb("Filter Graph layered corner", results["Image_corner"], (0.0, 0.0, 0.0))


def main():
    check_aov_output_menu_visibility()
    run_opaque_scenario()
    run_layered_scenario()
    print("EEVEE_NPR_TREE_AOV_OUTPUT_LAYERING_OK")


if __name__ == "__main__":
    main()
