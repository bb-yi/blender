from pathlib import Path

import bpy


ROOT = Path(__file__).resolve().parents[4]
OUT_DIR = ROOT / "temp" / "scene_time_node_compat"
FIXTURE_DIR = ROOT / "test" / "scene_time_node_compat" / "assets"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def socket_names(sockets):
    return [socket.name for socket in sockets]


def verify_geometry_values():
    scene = bpy.context.scene
    scene.frame_start = 10
    scene.frame_end = 30
    scene.render.fps = 24
    scene.render.fps_base = 1.0
    scene.render.frame_map_old = 2
    scene.render.frame_map_new = 1
    scene.frame_set(14, subframe=0.25)

    mesh = bpy.data.meshes.new("SceneTimeGeometryMesh")
    mesh.from_pydata([(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)], [], [(0, 1, 2)])
    obj = bpy.data.objects.new("SceneTimeGeometryObject", mesh)
    scene.collection.objects.link(obj)

    tree = bpy.data.node_groups.new("SceneTimeGeometryTree", "GeometryNodeTree")
    tree.interface.new_socket(name="Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
    tree.interface.new_socket(name="Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    nodes = tree.nodes
    links = tree.links
    group_input = nodes.new("NodeGroupInput")
    group_output = nodes.new("NodeGroupOutput")
    scene_time = nodes.new("GeometryNodeInputSceneTime")
    scene_time.inputs["Scale"].default_value = -2.0

    previous_geometry = group_input.outputs["Geometry"]
    for output_name in ("Frame", "Seconds", "Timeline", "Scaled Frame"):
        store = nodes.new("GeometryNodeStoreNamedAttribute")
        store.data_type = "FLOAT"
        store.domain = "POINT"
        store.inputs["Name"].default_value = "scene_time_" + output_name.lower().replace(" ", "_")
        links.new(previous_geometry, store.inputs["Geometry"])
        links.new(scene_time.outputs[output_name], store.inputs["Value"])
        previous_geometry = store.outputs["Geometry"]
    links.new(previous_geometry, group_output.inputs["Geometry"])

    modifier = obj.modifiers.new("SceneTimeGeometryModifier", "NODES")
    modifier.node_group = tree

    depsgraph = bpy.context.evaluated_depsgraph_get()
    depsgraph.update()
    evaluated_mesh = obj.evaluated_get(depsgraph).data

    expected = {
        "scene_time_frame": 14.25,
        "scene_time_seconds": 14.25 / 24.0,
        "scene_time_timeline": (14.25 - 10.0) / (30.0 - 10.0),
        "scene_time_scaled_frame": 14.25 / -2.0,
    }
    for attribute_name, expected_value in expected.items():
        require(attribute_name in evaluated_mesh.attributes,
                f"Geometry Scene Time attribute is missing: {attribute_name}")
        value = evaluated_mesh.attributes[attribute_name].data[0].value
        require(abs(value - expected_value) < 1e-5,
                f"Geometry Scene Time value mismatch for {attribute_name}: "
                f"expected {expected_value}, got {value}")


def verify_migrated_fixture(path, material_name, require_scale_link, require_all_output_links):
    require(path.is_file(), f"Scene Time migration fixture is missing: {path}")
    bpy.ops.wm.open_mainfile(filepath=str(path))
    material = bpy.data.materials.get(material_name)
    require(material is not None, f"Fixture material is missing: {material_name}")
    tree = material.node_tree
    scene_time_nodes = [
        node for node in tree.nodes if node.bl_idname == "GeometryNodeInputSceneTime"
    ]
    require(len(scene_time_nodes) == 1, f"Fixture did not migrate to one official node: {path}")
    require(
        not any(node.bl_idname == "ShaderNodeSceneTime" for node in tree.nodes),
        f"Legacy ShaderNodeSceneTime remains in fixture: {path}",
    )
    scene_time = scene_time_nodes[0]
    require(socket_names(scene_time.inputs) == ["Scale"], "Migrated Scale input is missing")
    require(
        socket_names(scene_time.outputs)
        == ["Frame", "Seconds", "Timeline", "Scaled Frame"],
        "Migrated Scene Time outputs are incomplete",
    )
    require(
        scene_time.inputs["Scale"].is_linked == require_scale_link,
        f"Migrated Scale link state is wrong: {path}",
    )
    if require_all_output_links:
        require(
            all(scene_time.outputs[name].is_linked
                for name in ("Frame", "Seconds", "Timeline", "Scaled Frame")),
            f"Migrated Scene Time output links were lost: {path}",
        )
    else:
        require(
            all(scene_time.outputs[name].is_linked for name in ("Frame", "Seconds")),
            f"Legacy official output links were lost: {path}",
        )


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)

    material = bpy.data.materials.new("SceneTimeNodeCompatibility")
    material.use_fake_user = True
    material.use_nodes = True
    tree = material.node_tree
    tree.nodes.clear()

    scene_time = tree.nodes.new("GeometryNodeInputSceneTime")
    require(socket_names(scene_time.inputs) == ["Scale"], "Scene Time Scale input is missing")
    require(
        socket_names(scene_time.outputs)
        == ["Frame", "Seconds", "Timeline", "Scaled Frame"],
        f"Unexpected Scene Time outputs: {socket_names(scene_time.outputs)}",
    )
    require(abs(scene_time.inputs["Scale"].default_value - 1.0) < 1e-6,
            "Scene Time Scale default is not 1.0")
    scene_time.inputs["Scale"].default_value = -3.0
    require(abs(scene_time.inputs["Scale"].default_value + 3.0) < 1e-6,
            "Scene Time Scale does not preserve negative values")

    scale = tree.nodes.new("ShaderNodeValue")
    scale.outputs["Value"].default_value = -2.0
    tree.links.new(scale.outputs["Value"], scene_time.inputs["Scale"])
    for output_name in ("Frame", "Seconds", "Timeline", "Scaled Frame"):
        sink = tree.nodes.new("ShaderNodeMath")
        tree.links.new(scene_time.outputs[output_name], sink.inputs[0])

    group_tree = bpy.data.node_groups.new("SceneTimeNodeGroup", "ShaderNodeTree")
    group_tree.use_fake_user = True
    group_node = group_tree.nodes.new("GeometryNodeInputSceneTime")
    require(socket_names(group_node.outputs) == socket_names(scene_time.outputs),
            "Scene Time node group interface differs from material interface")

    blend_path = OUT_DIR / "scene_time_node_compat.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(blend_path))
    bpy.ops.wm.open_mainfile(filepath=str(blend_path))

    reloaded = bpy.data.materials["SceneTimeNodeCompatibility"].node_tree
    reloaded_scene_time = next(
        node for node in reloaded.nodes if node.bl_idname == "GeometryNodeInputSceneTime"
    )
    require(socket_names(reloaded_scene_time.inputs) == ["Scale"],
            "Reloaded Scene Time Scale input is missing")
    require(socket_names(reloaded_scene_time.outputs) == socket_names(scene_time.outputs),
            "Reloaded Scene Time outputs changed")
    require(reloaded_scene_time.inputs["Scale"].is_linked,
            "Reloaded Scale link was lost")
    require(
        all(reloaded_scene_time.outputs[name].is_linked
            for name in ("Frame", "Seconds", "Timeline", "Scaled Frame")),
        "Reloaded Scene Time output link was lost",
    )

    verify_geometry_values()

    verify_migrated_fixture(
        FIXTURE_DIR / "legacy_scene_time.blend",
        "LegacySceneTimeMaterial",
        require_scale_link=True,
        require_all_output_links=True,
    )
    verify_migrated_fixture(
        FIXTURE_DIR / "legacy_official_scene_time.blend",
        "LegacyOfficialSceneTimeMaterial",
        require_scale_link=False,
        require_all_output_links=False,
    )

    print(
        f"SCENE_TIME_NODE_COMPAT_OK path={blend_path} fixtures={FIXTURE_DIR}", flush=True
    )


if __name__ == "__main__":
    main()
