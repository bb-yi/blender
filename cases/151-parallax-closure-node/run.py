import json
import math
from pathlib import Path

import bpy
from mathutils import Vector


CASE_DIR = Path(__file__).resolve().parent
OUT_DIR = CASE_DIR / "out"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def socket_names(sockets):
    return [socket.name for socket in sockets]


def make_image(name, width, height, kind):
    image = bpy.data.images.new(name, width=width, height=height, alpha=False, float_buffer=False)
    pixels = []
    for y in range(height):
        for x in range(width):
            u = x / max(width - 1, 1)
            v = y / max(height - 1, 1)
            if kind == "color":
                checker = ((x // 4) + (y // 4)) % 2
                r = 0.95 if checker else 0.08
                g = 0.15 + 0.75 * u
                b = 0.95 - 0.65 * v
            elif kind == "height":
                ridge = 0.5 + 0.5 * math.sin((u * 7.0 + v * 3.0) * math.tau)
                r = g = b = ridge
            else:
                raise ValueError(f"Unknown image kind: {kind}")
            pixels.extend((r, g, b, 1.0))
    image.pixels.foreach_set(pixels)
    image.pack()
    return image


def look_at(obj, target):
    direction = Vector(target) - obj.location
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()
    for datablock in (
        bpy.data.meshes,
        bpy.data.materials,
        bpy.data.images,
        bpy.data.cameras,
        bpy.data.worlds,
    ):
        for item in list(datablock):
            if item.users == 0:
                datablock.remove(item)


def setup_scene():
    clear_scene()
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for old_file in OUT_DIR.glob("*"):
        if old_file.is_file():
            old_file.unlink()

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 96
    scene.render.resolution_y = 96
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = False
    scene.render.image_settings.file_format = "PNG"
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0

    if scene.world is None:
        scene.world = bpy.data.worlds.new("ParallaxReleaseWorld")
    scene.world.color = (0.0, 0.0, 0.0)

    mesh = bpy.data.meshes.new("ParallaxReleasePlaneMesh")
    verts = [(-1.4, -1.0, 0.0), (1.4, -1.0, 0.0), (1.4, 1.0, 0.0), (-1.4, 1.0, 0.0)]
    mesh.from_pydata(verts, [], [(0, 1, 2, 3)])
    mesh.update()
    uv_layer = mesh.uv_layers.new(name="UVMap")
    for loop, uv in zip(uv_layer.data, [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]):
        loop.uv = uv
    obj = bpy.data.objects.new("ParallaxReleasePlane", mesh)
    bpy.context.collection.objects.link(obj)

    camera = bpy.data.objects.new("Camera", bpy.data.cameras.new("Camera"))
    bpy.context.collection.objects.link(camera)
    camera.location = (0.0, -3.0, 1.15)
    look_at(camera, (0.0, 0.0, 0.0))
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = 2.8
    scene.camera = camera

    return obj


def probe_node_interface():
    require(hasattr(bpy.types, "ShaderNodeParallax"), "ShaderNodeParallax RNA type missing")
    tree = bpy.data.node_groups.new("ParallaxReleaseSocketProbe", "ShaderNodeTree")
    probe = tree.nodes.new("ShaderNodeParallax")
    require(socket_names(probe.outputs) == ["UV", "Normal"], "Parallax outputs must be UV, Normal")

    mode_items = {item.identifier for item in probe.bl_rna.properties["mode"].enum_items}
    require(
        mode_items == {"PLANE_OFFSET", "OCCLUSION", "RELIEF", "SECANT_RELIEF"},
        f"Unexpected parallax modes: {mode_items}",
    )

    probe.mode = "PLANE_OFFSET"
    require(socket_names(probe.inputs) == ["UV", "Scale"], "Plane Offset input shape changed")
    require(hasattr(probe, "uv_map"), "Parallax node must expose a UV Map selector")
    probe.inputs["Scale"].default_value = -0.1
    require(probe.inputs["Scale"].default_value < 0.0, "Scale must allow negative values")

    probe.mode = "OCCLUSION"
    require(
        socket_names(probe.inputs)
        == ["Height Source", "UV", "Scale", "Offset", "Min Steps", "Max Steps"],
        "Parallax Occlusion must hide Refinement Steps",
    )
    require(socket_names(probe.outputs) == ["UV", "Normal"], "Occlusion outputs changed")

    probe.use_shadow = True
    require(
        socket_names(probe.inputs)
        == [
            "Height Source",
            "UV",
            "Scale",
            "Offset",
            "Min Steps",
            "Max Steps",
            "Sun Direction (World Space)",
        ],
        "Shadow input must be world-space and appended after Max Steps",
    )
    require(socket_names(probe.outputs) == ["UV", "Normal", "Shadow"], "Shadow output missing")

    probe.use_shadow = False
    probe.mode = "RELIEF"
    require("Refinement Steps" in probe.inputs, "Relief mode must expose Refinement Steps")
    probe.mode = "SECANT_RELIEF"
    require("Refinement Steps" in probe.inputs, "Secant Relief mode must expose Refinement Steps")


def probe_parallax_undo_roundtrip():
    bpy.context.preferences.edit.use_global_undo = True

    mesh = bpy.data.meshes.new("ParallaxUndoProbeMesh")
    mesh.from_pydata(
        [(-1.0, -1.0, 0.0), (1.0, -1.0, 0.0), (1.0, 1.0, 0.0), (-1.0, 1.0, 0.0)],
        [],
        [(0, 1, 2, 3)],
    )
    mesh.update()
    obj = bpy.data.objects.new("ParallaxUndoProbeObject", mesh)
    bpy.context.collection.objects.link(obj)
    bpy.context.view_layer.objects.active = obj

    material = bpy.data.materials.new("ParallaxUndoProbeMaterial")
    material.use_nodes = True
    obj.data.materials.append(material)

    parallax = material.node_tree.nodes.new("ShaderNodeParallax")
    parallax.mode = "OCCLUSION"
    parallax.use_shadow = True
    parallax.uv_map = "UVMap"

    bpy.ops.ed.undo_push(message="parallax node created")
    parallax.inputs["Scale"].default_value = 0.125
    bpy.ops.ed.undo_push(message="parallax scale changed")
    bpy.ops.ed.undo()

    material = bpy.data.materials.get("ParallaxUndoProbeMaterial")
    require(material is not None and material.node_tree is not None, "Material missing after undo")
    parallax_nodes = [
        node for node in material.node_tree.nodes if node.bl_idname == "ShaderNodeParallax"
    ]
    require(len(parallax_nodes) == 1, "Parallax node missing after undo")
    require(
        abs(parallax_nodes[0].inputs["Scale"].default_value - 0.05) < 1.0e-6,
        "Undo did not restore Parallax scale",
    )


def make_material(
    name,
    color_image,
    height_image,
    mode=None,
    scale=0.0,
    height_offset=0.0,
    uv_z=0.0,
    closure_image=False,
    closure_procedural=False,
    normal_as_color=False,
    use_shadow=False,
    sun_direction=(0.6, 0.0, 0.8),
    shadow_only=False,
):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    tree = material.node_tree
    nodes = tree.nodes
    links = tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    color_tex = nodes.new("ShaderNodeTexImage")
    color_tex.image = color_image
    color_tex.interpolation = "Closest"
    texcoord = nodes.new("ShaderNodeTexCoord")

    if shadow_only:
        emission.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    else:
        links.new(color_tex.outputs["Color"], emission.inputs["Color"])
    emission.inputs["Strength"].default_value = 1.0
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    if mode is None:
        links.new(texcoord.outputs["UV"], color_tex.inputs["Vector"])
        return material

    parallax = nodes.new("ShaderNodeParallax")
    parallax.mode = mode
    parallax.uv_map = "UVMap"
    parallax.inputs["Scale"].default_value = scale
    if "Offset" in parallax.inputs:
        parallax.inputs["Offset"].default_value = height_offset
    if "Min Steps" in parallax.inputs:
        parallax.inputs["Min Steps"].default_value = 6.0
    if "Max Steps" in parallax.inputs:
        parallax.inputs["Max Steps"].default_value = 32.0
    if "Refinement Steps" in parallax.inputs:
        parallax.inputs["Refinement Steps"].default_value = 4.0

    uv_link = texcoord.outputs["UV"]
    if abs(uv_z) > 1.0e-6:
        separate_uv = nodes.new("ShaderNodeSeparateXYZ")
        combine_uv = nodes.new("ShaderNodeCombineXYZ")
        combine_uv.inputs["Z"].default_value = uv_z
        links.new(texcoord.outputs["UV"], separate_uv.inputs["Vector"])
        links.new(separate_uv.outputs["X"], combine_uv.inputs["X"])
        links.new(separate_uv.outputs["Y"], combine_uv.inputs["Y"])
        uv_link = combine_uv.outputs["Vector"]

    links.new(uv_link, parallax.inputs["UV"])
    links.new(parallax.outputs["UV"], color_tex.inputs["Vector"])

    if normal_as_color:
        for link in list(emission.inputs["Color"].links):
            links.remove(link)
        links.new(parallax.outputs["Normal"], emission.inputs["Color"])

    if use_shadow:
        parallax.use_shadow = True
        parallax.inputs["Sun Direction (World Space)"].default_value = sun_direction
        if shadow_only:
            links.new(parallax.outputs["Shadow"], emission.inputs["Strength"])

    if "Height Source" in parallax.inputs:
        if closure_image:
            closure_input = nodes.new("NodeClosureInput")
            closure = nodes.new("NodeClosureOutput")
            closure_input.pair_with_output(closure)
            closure.input_items.new("VECTOR", "UV")
            closure.output_items.new("FLOAT", "Height")
            height_tex = nodes.new("ShaderNodeTexImage")
            height_tex.image = height_image
            height_tex.interpolation = "Linear"
            separate = nodes.new("ShaderNodeSeparateColor")
            links.new(closure_input.outputs["UV"], height_tex.inputs["Vector"])
            links.new(height_tex.outputs["Color"], separate.inputs["Color"])
            links.new(separate.outputs["Red"], closure.inputs["Height"])
            links.new(closure.outputs["Closure"], parallax.inputs["Height Source"])
        elif closure_procedural:
            closure_input = nodes.new("NodeClosureInput")
            closure = nodes.new("NodeClosureOutput")
            closure_input.pair_with_output(closure)
            closure.input_items.new("VECTOR", "UV")
            closure.output_items.new("FLOAT", "Height")
            wave = nodes.new("ShaderNodeTexWave")
            wave.wave_type = "BANDS"
            wave.bands_direction = "Z"
            wave.wave_profile = "SAW"
            wave.inputs["Scale"].default_value = 0.08
            wave.inputs["Distortion"].default_value = 0.0
            links.new(closure_input.outputs["UV"], wave.inputs["Vector"])
            links.new(wave.outputs["Fac"], closure.inputs["Height"])
            links.new(closure.outputs["Closure"], parallax.inputs["Height Source"])
        else:
            height = nodes.new("ShaderNodeImageToClosure")
            height.image = height_image
            height.colorspace = "Non-Color"
            links.new(height.outputs["Closure"], parallax.inputs["Height Source"])

    return material


def render_material(obj, material, name):
    obj.data.materials.clear()
    obj.data.materials.append(material)
    path = OUT_DIR / f"{name}.png"
    bpy.context.scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        pixels = list(image.pixels[:])
    finally:
        bpy.data.images.remove(image)
    return pixels


def mean_abs_diff(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def main():
    probe_node_interface()
    probe_parallax_undo_roundtrip()
    obj = setup_scene()
    color_image = make_image("ParallaxReleaseColor", 64, 64, "color")
    height_image = make_image("ParallaxReleaseHeight", 64, 64, "height")

    direct = render_material(obj, make_material("DirectUV", color_image, height_image), "direct_uv")
    scale_zero = render_material(
        obj,
        make_material("ParallaxScaleZero", color_image, height_image, "OCCLUSION", 0.0),
        "parallax_scale_zero",
    )
    plane_positive = render_material(
        obj,
        make_material("PlaneOffsetPositive", color_image, height_image, "PLANE_OFFSET", 0.12),
        "plane_offset_positive",
    )
    plane_negative = render_material(
        obj,
        make_material("PlaneOffsetNegative", color_image, height_image, "PLANE_OFFSET", -0.12),
        "plane_offset_negative",
    )
    occlusion = render_material(
        obj,
        make_material("ParallaxOcclusion", color_image, height_image, "OCCLUSION", 0.12),
        "parallax_occlusion",
    )
    occlusion_offset = render_material(
        obj,
        make_material(
            "ParallaxOcclusionOffset", color_image, height_image, "OCCLUSION", 0.12, height_offset=0.25
        ),
        "parallax_occlusion_offset",
    )
    closure_image = render_material(
        obj,
        make_material(
            "ParallaxClosureOutputHeight",
            color_image,
            height_image,
            "OCCLUSION",
            0.12,
            closure_image=True,
        ),
        "parallax_closure_output_height",
    )
    closure_procedural_z0 = render_material(
        obj,
        make_material(
            "ParallaxClosureProceduralHeightZ0",
            color_image,
            height_image,
            "OCCLUSION",
            0.12,
            uv_z=0.0,
            closure_procedural=True,
        ),
        "parallax_closure_procedural_height_z0",
    )
    closure_procedural_z1 = render_material(
        obj,
        make_material(
            "ParallaxClosureProceduralHeightZ1",
            color_image,
            height_image,
            "OCCLUSION",
            0.12,
            uv_z=1.0,
            closure_procedural=True,
        ),
        "parallax_closure_procedural_height_z1",
    )
    relief = render_material(
        obj,
        make_material("ReliefParallaxMapping", color_image, height_image, "RELIEF", 0.12),
        "relief_parallax_mapping",
    )
    secant = render_material(
        obj,
        make_material("SecantMethodReliefMapping", color_image, height_image, "SECANT_RELIEF", 0.12),
        "secant_method_relief_mapping",
    )
    normal_flat = render_material(
        obj,
        make_material("ParallaxNormalFlat", color_image, height_image, "OCCLUSION", 0.0, normal_as_color=True),
        "parallax_normal_flat",
    )
    normal_height = render_material(
        obj,
        make_material(
            "ParallaxNormalHeight", color_image, height_image, "OCCLUSION", 0.18, normal_as_color=True
        ),
        "parallax_normal_height",
    )
    shadow_up = render_material(
        obj,
        make_material(
            "ParallaxShadowUp",
            color_image,
            height_image,
            "OCCLUSION",
            0.18,
            use_shadow=True,
            sun_direction=(0.0, 0.0, 1.0),
            shadow_only=True,
        ),
        "parallax_shadow_up",
    )
    shadow_oblique = render_material(
        obj,
        make_material(
            "ParallaxShadowOblique",
            color_image,
            height_image,
            "OCCLUSION",
            0.18,
            use_shadow=True,
            sun_direction=(0.85, 0.0, 0.35),
            shadow_only=True,
        ),
        "parallax_shadow_oblique",
    )

    metrics = {
        "scale_zero_vs_direct": mean_abs_diff(scale_zero, direct),
        "plane_positive_vs_negative": mean_abs_diff(plane_positive, plane_negative),
        "occlusion_vs_direct": mean_abs_diff(occlusion, direct),
        "occlusion_offset_vs_occlusion": mean_abs_diff(occlusion_offset, occlusion),
        "closure_image_vs_direct": mean_abs_diff(closure_image, direct),
        "closure_procedural_z0_vs_direct": mean_abs_diff(closure_procedural_z0, direct),
        "closure_procedural_z1_vs_z0": mean_abs_diff(closure_procedural_z1,
                                                      closure_procedural_z0),
        "relief_vs_direct": mean_abs_diff(relief, direct),
        "secant_vs_direct": mean_abs_diff(secant, direct),
        "normal_height_vs_flat": mean_abs_diff(normal_height, normal_flat),
        "shadow_oblique_vs_up": mean_abs_diff(shadow_oblique, shadow_up),
    }

    require(metrics["scale_zero_vs_direct"] < 0.002, f"Scale zero changed UV: {metrics}")
    require(metrics["plane_positive_vs_negative"] > 0.002, f"Plane offset sign had no effect: {metrics}")
    require(metrics["occlusion_vs_direct"] > 0.002, f"Occlusion did not affect UV: {metrics}")
    require(metrics["occlusion_offset_vs_occlusion"] > 0.002, f"Offset did not affect occlusion: {metrics}")
    require(metrics["closure_image_vs_direct"] > 0.002, f"Closure Output height did not affect UV: {metrics}")
    require(metrics["closure_procedural_z0_vs_direct"] > 0.002,
            f"Closure procedural 3D height did not affect UV: {metrics}")
    require(
        metrics["closure_procedural_z1_vs_z0"] > 0.002,
        f"Closure procedural 3D height did not receive the Parallax UV Z coordinate: {metrics}",
    )
    require(metrics["relief_vs_direct"] > 0.002, f"Relief mode did not affect UV: {metrics}")
    require(metrics["secant_vs_direct"] > 0.002, f"Secant Relief mode did not affect UV: {metrics}")
    require(metrics["normal_height_vs_flat"] > 0.002, f"Normal output did not change with height: {metrics}")
    require(metrics["shadow_oblique_vs_up"] > 0.002, f"World-space shadow direction had no effect: {metrics}")

    report = {
        "status": "PASS",
        "undo_roundtrip": True,
        "metrics": metrics,
        "outputs": sorted(path.name for path in OUT_DIR.glob("*.png")),
    }
    (OUT_DIR / "validation.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("PARALLAX_CLOSURE_NODE_RELEASE_OK " + json.dumps(report, sort_keys=True), flush=True)


if __name__ == "__main__":
    main()
