import os
import tempfile

import bpy


RESOLUTION = 128


def _rot(value, shift):
    value &= 0xFFFFFFFF
    return ((value << shift) | (value >> (32 - shift))) & 0xFFFFFFFF


def _hash_int(value):
    """Python mirror of BLI_hash_int(value) / shader hash_uint2(value, 0u)."""
    mask = 0xFFFFFFFF
    a = b = c = (0xDEADBEEF + (2 << 2) + 13) & mask
    a = (a + value) & mask
    # Jenkins lookup3 final().
    c = (c ^ b) & mask
    c = (c - _rot(b, 14)) & mask
    a = (a ^ c) & mask
    a = (a - _rot(c, 11)) & mask
    b = (b ^ a) & mask
    b = (b - _rot(a, 25)) & mask
    c = (c ^ b) & mask
    c = (c - _rot(b, 16)) & mask
    a = (a ^ c) & mask
    a = (a - _rot(c, 4)) & mask
    b = (b ^ a) & mask
    b = (b - _rot(a, 14)) & mask
    c = (c ^ b) & mask
    c = (c - _rot(b, 24)) & mask
    return c


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()
    for material in list(bpy.data.materials):
        bpy.data.materials.remove(material)


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


def make_camera():
    camera_data = bpy.data.cameras.new("ReferencedObjectCamera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 3.0
    camera = bpy.data.objects.new("ReferencedObjectCamera", camera_data)
    camera.location = (0.0, 0.0, 4.0)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera


def make_light(name="ReferencedObjectLight", light_type="POINT"):
    light_data = bpy.data.lights.new(name, type=light_type)
    light_data.color = (0.2, 0.5, 0.9)
    light_data.energy = 4.0
    light_object = bpy.data.objects.new(name, light_data)
    light_object.location = (0.1, 0.2, 1.0)
    bpy.context.scene.collection.objects.link(light_object)
    return light_object


def make_plane(name, material, location=(0.0, 0.0, 0.0), size=1.0):
    bpy.ops.mesh.primitive_plane_add(size=size, location=location)
    plane = bpy.context.active_object
    plane.name = name
    plane.data.materials.append(material)
    return plane


def make_light_material(light_object, output_name):
    material = bpy.data.materials.new(f"ReferencedObject_{output_name}")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    info = nodes.new("ShaderNodeLightInfo")
    info.light_object = light_object
    links.new(info.outputs[output_name], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def render_pixels():
    scene = bpy.context.scene
    descriptor, filepath = tempfile.mkstemp(suffix=".exr")
    os.close(descriptor)
    scene.render.image_settings.file_format = "OPEN_EXR"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "32"
    scene.render.filepath = filepath
    bpy.ops.render.render(write_still=True)
    image = bpy.data.images.load(filepath, check_existing=False)
    try:
        return list(image.pixels[:])
    finally:
        bpy.data.images.remove(image)
        if os.path.exists(filepath):
            os.remove(filepath)


def center_rgb(pixels, x=RESOLUTION // 2, y=RESOLUTION // 2):
    index = (y * RESOLUTION + x) * 4
    return tuple(pixels[index:index + 3])


def assert_fixed_socket_abi():
    material = bpy.data.materials.new("ReferencedObjectSocketABI")
    material.use_nodes = True
    node = material.node_tree.nodes.new("ShaderNodeLightInfo")
    expected = {
        "Color",
        "Power",
        "Type",
        "Position",
        "Direction",
        "Radius",
        "Spot Size",
        "Sun Angle",
        "Visible",
    }
    assert {socket.name for socket in node.outputs} == expected

    for light_type in ("POINT", "SUN", "SPOT", "AREA"):
        data = bpy.data.lights.new(f"SocketABI_{light_type}", type=light_type)
        obj = bpy.data.objects.new(f"SocketABI_{light_type}", data)
        node.light_object = obj
        disabled = [socket.name for socket in node.outputs if not socket.enabled]
        assert not disabled, f"Light type {light_type} disabled sockets: {disabled}"


def assert_shared_target_and_no_recompile():
    clear_scene()
    configure_scene()
    make_camera()
    light_object = make_light()

    color_material = make_light_material(light_object, "Color")
    power_material = make_light_material(light_object, "Power")
    make_plane("SharedTargetColor", color_material, location=(-0.7, 0.0, 0.0))
    make_plane("SharedTargetPower", power_material, location=(0.7, 0.0, 0.0))

    pixels = render_pixels()
    left = center_rgb(pixels, RESOLUTION // 4)
    right = center_rgb(pixels, 3 * RESOLUTION // 4)
    assert all(abs(value - expected) < 0.05 for value, expected in zip(left, (0.2, 0.5, 0.9))), left
    assert all(value > 3.8 for value in right), right

    color_timestamp = color_material.shader_compile_timestamp
    power_timestamp = power_material.shader_compile_timestamp
    print(
        f"REFERENCED_OBJECT_SHADER_WARMUP_STATUS="
        f"{color_material.shader_compile_status},{power_material.shader_compile_status}"
    )

    for label, update in (
        ("transform", lambda: setattr(light_object, "location", (0.8, -0.1, 1.4))),
        ("color", lambda: setattr(light_object.data, "color", (0.9, 0.2, 0.1))),
        ("energy", lambda: setattr(light_object.data, "energy", 7.0)),
        ("type", lambda: setattr(light_object.data, "type", "AREA")),
        ("visibility", lambda: setattr(light_object, "hide_render", True)),
    ):
        update()
        bpy.context.view_layer.update()
        render_pixels()
        print(
            f"REFERENCED_OBJECT_TIMESTAMP_{label.upper()}="
            f"{color_material.shader_compile_timestamp},{power_material.shader_compile_timestamp}"
        )
        assert color_material.shader_compile_timestamp == color_timestamp
        assert power_material.shader_compile_timestamp == power_timestamp


def assert_attribute_and_reference_coexist():
    clear_scene()
    configure_scene()
    make_camera()
    light_object = make_light("AttributeAndReferenceLight")

    material = bpy.data.materials.new("AttributeAndReferenceMaterial")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()
    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    attribute = nodes.new("ShaderNodeAttribute")
    attribute.attribute_type = "OBJECT"
    attribute.attribute_name = "TestValue"
    info = nodes.new("ShaderNodeLightInfo")
    info.light_object = light_object
    multiply = nodes.new("ShaderNodeMath")
    multiply.operation = "MULTIPLY"
    links.new(attribute.outputs["Fac"], multiply.inputs[0])
    links.new(info.outputs["Power"], multiply.inputs[1])
    links.new(multiply.outputs["Value"], emission.inputs["Strength"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    plane = make_plane("AttributeAndReferencePlane", material)
    plane["TestValue"] = 0.5
    pixel = center_rgb(render_pixels())
    print(f"REFERENCED_OBJECT_WITH_ATTRIBUTE={pixel}")
    assert all(value > 1.8 for value in pixel), pixel


def assert_attribute_only_safe():
    clear_scene()
    configure_scene()
    make_camera()

    material = bpy.data.materials.new("ReferencedObjectAttributeOnly")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()
    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    attribute = nodes.new("ShaderNodeAttribute")
    attribute.attribute_type = "OBJECT"
    attribute.attribute_name = "TestValue"
    links.new(attribute.outputs["Fac"], emission.inputs["Strength"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    plane = make_plane("ReferencedObjectAttributeOnlyPlane", material)
    plane["TestValue"] = 0.4
    pixel = center_rgb(render_pixels())
    print(f"REFERENCED_OBJECT_ATTRIBUTE_ONLY={pixel}")
    assert all(abs(value - 0.4) < 0.05 for value in pixel), pixel


def assert_hash_slot_collision_render():
    clear_scene()
    configure_scene()
    make_camera()

    table_size = 4
    slots = {}
    collision = None
    for index in range(16):
        light = make_light(f"HashCollisionLight_{index}")
        slot = _hash_int(light.session_uid) & (table_size - 1)
        if slot in slots:
            collision = (slots[slot], light, slot)
            break
        slots[slot] = light

    assert collision is not None, "Expected two referenced lights to collide in a four-slot table"
    first_light, second_light, slot = collision
    first_light.data.color = (0.9, 0.1, 0.1)
    second_light.data.color = (0.1, 0.2, 0.9)

    first_material = make_light_material(first_light, "Color")
    second_material = make_light_material(second_light, "Color")
    make_plane("HashCollisionFirst", first_material, location=(-0.7, 0.0, 0.0))
    make_plane("HashCollisionSecond", second_material, location=(0.7, 0.0, 0.0))

    pixels = render_pixels()
    first_pixel = center_rgb(pixels, RESOLUTION // 4)
    second_pixel = center_rgb(pixels, 3 * RESOLUTION // 4)
    print(
        "REFERENCED_OBJECT_HASH_COLLISION="
        f"table_size={table_size},slot={slot},"
        "expected_collision_count=1,expected_max_probe=2,"
        f"uids={[first_light.session_uid, second_light.session_uid]},"
        f"pixels={first_pixel},{second_pixel}"
    )

    assert all(
        abs(value - expected) < 0.05
        for value, expected in zip(first_pixel, first_light.data.color)
    ), first_pixel
    assert all(
        abs(value - expected) < 0.05
        for value, expected in zip(second_pixel, second_light.data.color)
    ), second_pixel


def assert_empty_target_is_safe():
    clear_scene()
    configure_scene()
    make_camera()
    material = make_light_material(None, "Color")
    make_plane("EmptyReferencedPlane", material)
    pixel = center_rgb(render_pixels())
    print(f"REFERENCED_OBJECT_EMPTY_DEFAULT={pixel}")
    assert max(pixel) < 0.01, pixel


def assert_deleted_target_is_safe():
    clear_scene()
    configure_scene()
    make_camera()
    light_object = make_light("DeletedReferencedLight")
    material = make_light_material(light_object, "Color")
    make_plane("DeletedReferencedPlane", material)
    info = next(node for node in material.node_tree.nodes if node.bl_idname == "ShaderNodeLightInfo")
    old_uid = light_object.session_uid
    expected_color = tuple(light_object.data.color)
    initial_pixel = center_rgb(render_pixels())
    assert all(abs(value - expected) < 0.05 for value, expected in zip(initial_pixel, expected_color)), initial_pixel
    bpy.data.objects.remove(light_object, do_unlink=True)
    bpy.context.view_layer.update()
    pixel = center_rgb(render_pixels())
    print(f"REFERENCED_OBJECT_DELETED_DEFAULT={initial_pixel}->{pixel}")
    assert max(pixel) < 0.01, pixel

    replacement = make_light("ReboundReferencedLight")
    replacement.data.color = (0.1, 0.8, 0.3)
    assert replacement.session_uid != old_uid
    info.light_object = replacement
    bpy.context.view_layer.update()
    rebound = center_rgb(render_pixels())
    print(
        "REFERENCED_OBJECT_REBOUND="
        f"old_uid={old_uid},new_uid={replacement.session_uid},pixel={rebound}"
    )
    assert all(
        abs(value - expected) < 0.05
        for value, expected in zip(rebound, replacement.data.color)
    ), rebound


assert_fixed_socket_abi()
assert_shared_target_and_no_recompile()
assert_attribute_only_safe()
assert_attribute_and_reference_coexist()
assert_hash_slot_collision_render()
assert_empty_target_is_safe()
assert_deleted_target_is_safe()
print("REFERENCED_OBJECT_DATA_CHANNEL_SMOKE_OK")
