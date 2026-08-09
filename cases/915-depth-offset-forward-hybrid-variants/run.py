import json
from pathlib import Path

import bpy


CASE_DIR = Path(__file__).resolve().parent
OUT_DIR = CASE_DIR / "out"
RESOLUTION = 128


def assert_true(condition, message):
    if not condition:
        raise AssertionError(message)


def set_if_available(owner, name, value):
    if hasattr(owner, name):
        setattr(owner, name, value)


def select_eevee_engine(scene):
    engines = {item.identifier for item in scene.render.bl_rna.properties["engine"].enum_items}
    for engine in ("BLENDER_EEVEE_NEXT", "BLENDER_EEVEE"):
        if engine in engines:
            scene.render.engine = engine
            return engine
    raise AssertionError(f"No Eevee render engine found in {sorted(engines)}")


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()
    for collection in (
        bpy.data.meshes,
        bpy.data.materials,
        bpy.data.lights,
        bpy.data.cameras,
        bpy.data.images,
        bpy.data.worlds,
    ):
        for datablock in list(collection):
            if datablock.users == 0:
                collection.remove(datablock)


def configure_scene():
    clear_scene()
    scene = bpy.context.scene
    engine = select_eevee_engine(scene)
    scene.render.resolution_x = RESOLUTION
    scene.render.resolution_y = RESOLUTION
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.film_transparent = False
    scene.render.use_compositing = False
    if hasattr(scene, "compositing_node_group"):
        scene.compositing_node_group = None

    set_if_available(scene.eevee, "taa_samples", 1)
    set_if_available(scene.eevee, "taa_render_samples", 1)
    set_if_available(scene.eevee, "use_taa_reprojection", False)
    set_if_available(scene.eevee, "use_raytracing", False)
    set_if_available(scene.eevee, "use_shadows", False)
    set_if_available(scene.eevee, "use_outline", False)

    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0

    world = bpy.data.worlds.new("Depth Offset Variant World")
    world.use_nodes = True
    nodes = world.node_tree.nodes
    links = world.node_tree.links
    nodes.clear()
    bg = nodes.new("ShaderNodeBackground")
    bg.inputs["Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    bg.inputs["Strength"].default_value = 0.0
    out = nodes.new("ShaderNodeOutputWorld")
    links.new(bg.outputs["Background"], out.inputs["Surface"])
    scene.world = world
    return engine


def setup_camera():
    bpy.ops.object.camera_add(location=(0.0, 0.0, 4.0), rotation=(0.0, 0.0, 0.0))
    camera = bpy.context.object
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = 2.6
    bpy.context.scene.camera = camera


def add_area_light():
    bpy.ops.object.light_add(type="AREA", location=(0.0, 0.0, 3.0))
    light = bpy.context.object
    light.data.energy = 600.0
    light.data.size = 4.0
    return light


def add_plane(name, z, material, size=2.0):
    bpy.ops.mesh.primitive_plane_add(size=size, location=(0.0, 0.0, z))
    obj = bpy.context.object
    obj.name = name
    obj.data.materials.append(material)
    return obj


def make_emission_material(name, color):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = color
    emission.inputs["Strength"].default_value = 1.0
    out = nodes.new("ShaderNodeOutputMaterial")
    links.new(emission.outputs["Emission"], out.inputs["Surface"])
    return material


def link_depth_offset(material, value):
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    output = next(node for node in nodes if node.bl_idname == "ShaderNodeOutputMaterial")
    depth_socket = output.inputs.get("Depth Offset")
    assert_true(depth_socket is not None, "Material Output is missing Depth Offset input")
    value_node = nodes.new("ShaderNodeValue")
    value_node.outputs[0].default_value = value
    links.new(value_node.outputs[0], depth_socket)
    assert_true(depth_socket.is_linked, "Depth Offset socket was not linked")
    return value_node


def make_hybrid_depth_offset_material():
    material = bpy.data.materials.new("Hybrid ShaderToRGB Depth Offset")
    material.use_nodes = True
    material.depth_offset_affect_lighting = False
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    diffuse = nodes.new("ShaderNodeBsdfDiffuse")
    diffuse.inputs["Color"].default_value = (0.0, 1.0, 0.0, 1.0)
    shader_to_rgb = nodes.new("ShaderNodeShaderToRGB")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Strength"].default_value = 1.0
    out = nodes.new("ShaderNodeOutputMaterial")

    links.new(diffuse.outputs["BSDF"], shader_to_rgb.inputs["Shader"])
    links.new(shader_to_rgb.outputs["Color"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], out.inputs["Surface"])
    link_depth_offset(material, 0.12)

    assert_true(
        any(node.bl_idname == "ShaderNodeShaderToRGB" for node in nodes),
        "Hybrid test material did not contain Shader To RGB",
    )
    assert_true(out.inputs["Depth Offset"].is_linked, "Hybrid Depth Offset input is not linked")
    return material


def make_forward_depth_offset_material():
    material = make_emission_material("Forward Blended Depth Offset", (0.0, 1.0, 0.0, 1.0))
    material.surface_render_method = "BLENDED"
    link_depth_offset(material, 0.12)

    output = next(
        node for node in material.node_tree.nodes if node.bl_idname == "ShaderNodeOutputMaterial"
    )
    assert_true(material.surface_render_method == "BLENDED", "Forward material is not BLENDED")
    assert_true(out_socket_is_linked(output, "Depth Offset"), "Forward Depth Offset input is not linked")
    return material


def out_socket_is_linked(output_node, name):
    socket = output_node.inputs.get(name)
    return socket is not None and socket.is_linked


def render_image(label):
    path = OUT_DIR / f"{label}.png"
    bpy.context.scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)
    assert_true(path.exists(), f"Render did not write {path}")
    return path


def load_pixels(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        return list(image.pixels[:]), int(image.size[0]), int(image.size[1])
    finally:
        bpy.data.images.remove(image)


def region_stats(path, x0=46, y0=46, x1=82, y1=82):
    pixels, width, height = load_pixels(path)
    x0 = max(0, min(width, x0))
    x1 = max(0, min(width, x1))
    y0 = max(0, min(height, y0))
    y1 = max(0, min(height, y1))
    count = (x1 - x0) * (y1 - y0)
    assert_true(count > 0, "Sample region was empty")

    sums = [0.0, 0.0, 0.0, 0.0]
    non_black = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            index = (y * width + x) * 4
            rgba = pixels[index : index + 4]
            for channel in range(4):
                sums[channel] += rgba[channel]
            if max(rgba[:3]) > 0.02:
                non_black += 1

    return {
        "avg_r": sums[0] / count,
        "avg_g": sums[1] / count,
        "avg_b": sums[2] / count,
        "avg_a": sums[3] / count,
        "non_black_ratio": non_black / count,
    }


def assert_green_visible(label, stats):
    assert_true(stats["avg_a"] >= 0.90, f"{label}: expected opaque output, got {stats}")
    assert_true(
        stats["non_black_ratio"] >= 0.95,
        f"{label}: expected non-black center region, got {stats}",
    )
    assert_true(stats["avg_g"] >= 0.20, f"{label}: expected green output, got {stats}")
    assert_true(
        stats["avg_g"] > max(stats["avg_r"], stats["avg_b"]) + 0.08,
        f"{label}: expected green-dominant output, got {stats}",
    )


def render_variant(label, material_factory):
    configure_scene()
    setup_camera()
    add_area_light()
    add_plane(
        f"{label}_back_red",
        0.0,
        make_emission_material(f"{label}_back_red_mat", (1.0, 0.0, 0.0, 1.0)),
        2.2,
    )
    material = material_factory()
    add_plane(f"{label}_front_green", 0.08, material, 1.4)
    path = render_image(label)
    stats = region_stats(path)
    assert_green_visible(label, stats)
    return {"path": str(path), "stats": stats}


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    hybrid_result = render_variant(
        "hybrid_shader_to_rgb_depth_offset", make_hybrid_depth_offset_material
    )
    forward_result = render_variant("forward_blended_depth_offset", make_forward_depth_offset_material)

    summary = {
        "status": "PASS",
        "hybrid": hybrid_result,
        "forward": forward_result,
    }
    summary_path = OUT_DIR / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"DEPTH_OFFSET_FORWARD_HYBRID_VARIANTS_SUMMARY={summary_path}")


if __name__ == "__main__":
    main()
