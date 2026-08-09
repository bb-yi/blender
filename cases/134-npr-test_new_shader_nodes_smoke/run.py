import bpy


NODE_SPECS = [
    ("ShaderNodeRenderTexture", ("Color", "Alpha")),
    ("GeometryNodeInputSceneTime", ("Frame", "Scaled Frame")),
    ("ShaderNodeWorldToTangent", None),
    ("ShaderNodeBasisTransform", ("Vector",)),
    ("ShaderNodeSdfPrimitive", ("Distance",)),
    ("ShaderNodeSdfOp", ("Distance",)),
    ("ShaderNodeSdfVectorOp", ("Vector",)),
    ("ShaderNodeTwirl", None),
    ("ShaderNodeWaterRipples", None),
    ("ShaderNodeTexHexagon", ("Value", "Color")),
    ("ShaderNodeSceneColor", ("Color Image", "Depth Image", "Normal Image", "Position Image")),
    ("ShaderNodeInputAOV", None),
    ("ShaderNodeOutputFilter", ("Color", "Alpha")),
    ("ShaderNodeOutlineControl", None),
    ("ShaderNodeNPR_Input", None),
    ("ShaderNodeNPR_ImageSample", None),
    ("ShaderNodeNPR_Refraction", None),
    ("ShaderNodeImageToClosure", None),
    ("ShaderNodePortalIn", None),
    ("ShaderNodePortalOut", None),
    ("ShaderNodeForeachLightInput", None),
    ("ShaderNodeForeachLightOutput", None),
]


def assert_socket_names(node, expected_names):
    if not expected_names:
        return
    socket_names = {socket.name for socket in node.inputs} | {socket.name for socket in node.outputs}
    for expected_name in expected_names:
        assert expected_name in socket_names, (
            f"{node.bl_idname} is missing expected socket {expected_name!r}; "
            f"available={sorted(socket_names)}"
        )


def assert_property_exists(node, property_name):
    assert property_name in node.bl_rna.properties, (
        f"{node.bl_idname} is missing RNA property {property_name!r}; "
        f"available={sorted(node.bl_rna.properties.keys())}"
    )


def assert_enum_switch(node, property_name):
    assert_property_exists(node, property_name)
    prop = node.bl_rna.properties[property_name]
    identifiers = [item.identifier for item in prop.enum_items if item.identifier]
    assert identifiers, f"{node.bl_idname}.{property_name} has no enum identifiers"
    original_value = getattr(node, property_name)
    for identifier in identifiers[: min(4, len(identifiers))]:
        setattr(node, property_name, identifier)
        assert getattr(node, property_name) == identifier, (
            f"{node.bl_idname}.{property_name} failed to switch to {identifier!r}"
        )
    setattr(node, property_name, original_value)


def assert_any_enum_switch(node, property_names):
    for property_name in property_names:
        if property_name in node.bl_rna.properties and hasattr(node.bl_rna.properties[property_name], "enum_items"):
            assert_enum_switch(node, property_name)
            return
    raise AssertionError(
        f"{node.bl_idname} is missing all expected enum properties {property_names!r}; "
        f"available={sorted(node.bl_rna.properties.keys())}"
    )


def main():
    material = bpy.data.materials.new("SmokeNodes")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    nodes.clear()

    created = {}
    for bl_idname, expected_sockets in NODE_SPECS:
        node = nodes.new(bl_idname)
        created[bl_idname] = node
        assert_socket_names(node, expected_sockets)

    assert_enum_switch(created["ShaderNodeBasisTransform"], "basis_input")
    assert_enum_switch(created["ShaderNodeBasisTransform"], "vector_type")
    assert_any_enum_switch(created["ShaderNodeSdfPrimitive"], ("primitive", "type", "mode"))
    assert_any_enum_switch(created["ShaderNodeSdfOp"], ("operation", "type", "mode"))
    assert_any_enum_switch(created["ShaderNodeSdfVectorOp"], ("operation", "type", "mode"))
    assert_any_enum_switch(created["ShaderNodeWaterRipples"], ("mode", "type"))
    assert_any_enum_switch(created["ShaderNodePortalIn"], ("socket_type", "data_type", "type"))
    assert_any_enum_switch(created["ShaderNodePortalOut"], ("socket_type", "data_type", "type"))
    assert_any_enum_switch(created["ShaderNodeImageToClosure"], ("texture_type", "interpolation", "extension"))

    # Smoke-check that AOV input still exposes a name property and For Each Light nodes create as a pair.
    assert_property_exists(created["ShaderNodeInputAOV"], "aov_name")


if __name__ == "__main__":
    main()
