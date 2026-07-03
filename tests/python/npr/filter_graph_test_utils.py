import bpy


def clear_filter_graph(scene=None):
    scene = scene or bpy.context.scene
    graph = getattr(scene.eevee, "filter_graph", None)
    if graph is None:
        return
    scene.eevee.filter_graph = None
    if graph.users == 0:
        bpy.data.node_groups.remove(graph)


def refresh_tree(tree):
    tree.interface_update(bpy.context)
    tree.update_tag()
    bpy.context.view_layer.update()


def add_pass_input_image_sample(nodes, links, location=(0.0, 0.0)):
    pass_input = nodes.new("ShaderNodeFilterGraphInput")
    pass_input.location = location

    image_sample = nodes.new("ShaderNodeNPR_ImageSample")
    image_sample.location = (location[0] + 220.0, location[1])

    links.new(pass_input.outputs["Image"], image_sample.inputs["Image"])
    return pass_input, image_sample


def new_filter_graph(scene=None):
    scene = scene or bpy.context.scene
    clear_filter_graph(scene)
    graph = bpy.data.node_groups.new(name="Eevee Filter Graph", type="EeveeFilterGraphNodeTree")
    scene.eevee.filter_graph = graph
    refresh_tree(graph)
    return graph


def attach_filter_material(
        material,
        *,
        stage="BEFORE_COMPOSITE",
        scene_socket=None,
        aov_name=None,
        aov_socket="Color",
        graph=None):
    scene = bpy.context.scene
    graph = graph or getattr(scene.eevee, "filter_graph", None) or new_filter_graph(scene)

    nodes = graph.nodes
    links = graph.links

    source_node = None
    source_socket = None
    if scene_socket is not None:
        source_node = nodes.new("EeveeFilterGraphNodeSceneColor")
        source_node.location = (-520.0, 0.0)
        source_socket = source_node.outputs[scene_socket]
    elif aov_name is not None:
        source_node = nodes.new("EeveeFilterGraphNodeAOVInput")
        source_node.location = (-520.0, 0.0)
        source_node.aov_name = aov_name
        source_socket = source_node.outputs[aov_socket]

    filter_pass = nodes.new("EeveeFilterGraphNodeFilterMaterial")
    filter_pass.location = (-180.0, 0.0)
    filter_pass.material = material
    refresh_tree(graph)

    if source_socket is not None:
        if "Image" not in filter_pass.inputs:
            raise AssertionError(
                f"Filter Pass for {material.name!r} has no Image input after material sync"
            )
        links.new(source_socket, filter_pass.inputs["Image"])

    stage_output = nodes.new("EeveeFilterGraphNodeStageOutput")
    stage_output.location = (200.0, 0.0)
    stage_output.execution_stage = stage
    stage_output.is_active_output = True

    if "Image" not in filter_pass.outputs:
        raise AssertionError(f"Filter Pass for {material.name!r} has no Image output")
    links.new(filter_pass.outputs["Image"], stage_output.inputs["Image"])

    refresh_tree(graph)
    return graph, filter_pass, stage_output
