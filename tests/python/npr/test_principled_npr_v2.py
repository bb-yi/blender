# SPDX-License-Identifier: GPL-2.0-or-later
"""Functional Principled NPR V2 contract tests; executed separately on each GPU backend."""
import argparse
import importlib.util
import json
import math
import re
from pathlib import Path
import sys

import bpy
from mathutils import Matrix


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def inp(node, identifier):
    return next(s for s in node.inputs if s.identifier == identifier)


def assign(node, **values):
    for identifier, value in values.items():
        inp(node, identifier).default_value = value


def load_render_helpers():
    path = Path(__file__).with_name("test_principled_npr_v1.py")
    spec = importlib.util.spec_from_file_location("npr_render_helpers", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def new_material():
    material = bpy.data.materials.new("PrincipledNPR_V2_Test")
    material.use_nodes = True
    material.surface_render_method = "DITHERED"
    tree = material.node_tree
    tree.nodes.clear()
    node = tree.nodes.new("ShaderNodePrincipledNPR")
    output = tree.nodes.new("ShaderNodeOutputMaterial")
    tree.links.new(node.outputs["Shader"], output.inputs["Surface"])
    return material, node, output


def check_shared_principled_math(h):
    # Exercise the actual shared source on the GPU, including the degenerate inputs from
    # upstream #162728. A render alone can hide NaNs when radiance is packed/clamped.
    shaders = Path(__file__).resolve().parents[3] / "source/blender/gpu/shaders"
    shared = (shaders / "material/gpu_shader_material_principled_shared.glsl").read_text()
    fast = (shaders / "common/gpu_shader_math_fast_lib.glsl").read_text()
    slab = re.search(r"float3 slab_transmittance_at_angle\([^}]+\}", shared).group()
    sqrt = re.search(r"float sqrt_fast\([^}]+\}", fast).group()
    code = (sqrt + "\n" + slab).replace("float3", "vec3")
    code = code.replace("sqrt_fast", "test_sqrt_fast")
    code = code.replace("slab_transmittance_at_angle", "test_slab_transmittance")
    code = code.replace("square(", "test_square(")
    code = "float test_square(float v) { return v * v; }\n" + code
    code += """
vec3 shared_math_probe(float ior_value, float cosine) {
  vec3 result = test_slab_transmittance(vec3(0.25, 0.5, 0.75), cosine, ior_value);
  return any(isnan(result)) || any(isinf(result)) ? vec3(1.0, 0.0, 0.0) : result;
}
"""
    h.clear_scene()
    h.configure_scene()
    h.make_camera()
    material, node, output = new_material()
    tree = material.node_tree
    tree.nodes.remove(node)
    probe = tree.nodes.new("ShaderNodeGLSLFunction")
    script = bpy.data.texts.new("principled_shared_math_probe.glsl")
    script.write(code)
    probe.script = script
    probe.function_name = "shared_math_probe"
    require(probe.parse_status == "READY", "Shared math probe did not parse")
    emission = tree.nodes.new("ShaderNodeEmission")
    tree.links.new(probe.outputs["Result"], emission.inputs["Color"])
    tree.links.new(emission.outputs[0], output.inputs["Surface"])
    h.make_sphere(material)
    for label, ior_value, cosine, expected in (
        ("zero_ior", 0.0, 1.0, (1.0, 1.0, 1.0)),
        ("grazing", 1.0, 0.0, (0.0, 0.0, 0.0)),
        ("normal", 1.5, 1.0, (0.25, 0.5, 0.75)),
    ):
        inp(probe, "In_ior_value").default_value = ior_value
        inp(probe, "In_cosine").default_value = cosine
        rgb = h.center_pixel(h.render_image("shared_principled_" + label))[:3]
        require(max(abs(a - b) for a, b in zip(rgb, expected)) < 0.025,
                f"Shared Principled math {label}: {rgb}, expected {expected}")


def check_standalone_rim(h):
    h.clear_scene()
    h.configure_scene()
    bpy.context.scene.eevee.taa_render_samples = 16
    configure_world((0, 0, 0))
    h.make_camera()
    material, principal, output = new_material()
    h.make_sphere(material)
    tree = material.node_tree
    rim = tree.nodes.new("ShaderNodeNPRRim")
    require(rim.rim_mode == "FRESNEL", "Standalone rim default mode changed")
    require([s.name for s in rim.outputs] == ["Color", "Factor"], "Standalone rim outputs missing")
    assign(principal, rim_strength=1, rim_pixel_width=4, rim_depth_threshold=.05,
           rim_depth_softness=.5, rim_samples=8)
    emission = tree.nodes.new("ShaderNodeEmission")
    tree.links.new(emission.outputs[0], output.inputs["Surface"])
    difference = tree.nodes.new("ShaderNodeVectorMath")
    difference.operation = "DISTANCE"
    tree.links.new(principal.outputs["Rim"], difference.inputs[0])
    tree.links.new(rim.outputs["Color"], difference.inputs[1])
    for mode in ("FRESNEL", "DEPTH"):
        rim.rim_mode = principal.rim_mode = mode
        require(inp(rim, "Thickness").is_unavailable == (mode == "DEPTH"), "Wrong Fresnel controls")
        require(inp(rim, "Width (Pixels)").is_unavailable == (mode != "DEPTH"), "Wrong Depth controls")
        tree.links.new(rim.outputs["Color"], emission.inputs["Color"])
        require(h.max_rgb(h.render_image(f"v2_standalone_rim_{mode}")) > .01, "Standalone rim is black")
        tree.links.new(difference.outputs["Value"], emission.inputs["Color"])
        require(h.max_rgb(h.render_image(f"v2_standalone_rim_parity_{mode}")) < .001,
                "Standalone and built-in rim algorithms disagree")
    tree.links.new(rim.outputs["Color"], emission.inputs["Color"])
    material.surface_render_method = "BLENDED"
    require(h.max_rgb(h.render_image("v2_standalone_rim_forward_disabled")) < .0001,
            "Standalone depth rim must be disabled on forward surfaces")
    material.surface_render_method = "DITHERED"
    tree.links.new(rim.outputs["Factor"], emission.inputs["Color"])
    factor = h.render_image("v2_standalone_rim_factor")
    require(.01 < h.max_rgb(factor) <= 1.001, "Invalid standalone rim factor")
    tree.links.new(rim.outputs["Color"], emission.inputs["Color"])
    for control, value in (("Mask", 0), ("Alpha", .5), ("Width (Pixels)", 0)):
        previous = inp(rim, control).default_value
        inp(rim, control).default_value = value
        require(h.max_rgb(h.render_image(f"v2_standalone_rim_disabled_{control}")) < .0001,
                f"Standalone depth rim ignores {control}")
        inp(rim, control).default_value = previous
    inp(rim, "Light Bias").default_value = 1
    inp(rim, "Light Factor").default_value = 0
    require(h.max_rgb(h.render_image("v2_standalone_rim_light_factor")) < .0001,
            "Standalone rim ignores Light Factor")
    inp(rim, "Light Bias").default_value = 0
    inp(rim, "Light Factor").default_value = 1
    material_name, node_name = material.name, rim.name
    bpy.ops.wm.save_as_mainfile(filepath=str(h.OUTPUT_DIR / "standalone_rim_roundtrip.blend"))
    bpy.ops.wm.open_mainfile(filepath=str(h.OUTPUT_DIR / "standalone_rim_roundtrip.blend"))
    restored = bpy.data.materials[material_name].node_tree.nodes[node_name]
    require(restored.rim_mode == "DEPTH" and restored.outputs["Color"].is_linked,
            "Standalone rim mode or links lost on readback")
    require(h.max_rgb(h.render_image("v2_standalone_rim_readback")) > .01,
            "Standalone rim stopped rendering after readback")


def check_diffuse_roughness_independence(h):
    h.clear_scene()
    h.configure_scene()
    configure_world((0, 0, 0))
    h.make_camera()
    material, node, output = new_material()
    h.make_sphere(material)
    h.add_point_light("Diffuse independence", (1, 1, 1), 80, (-2, -4, 2))
    node.shadow_mode = "NONE"
    assign(node, base_color=(.8, .8, .8, 1), highlight_strength=0,
           reflection_strength=0, ambient_strength=0, rim_strength=0)
    tree = material.node_tree
    emission = tree.nodes.new("ShaderNodeEmission")
    tree.links.new(node.outputs["Diffuse"], emission.inputs["Color"])
    for name, socket in (("diffuse", emission.outputs[0]), ("shader", node.outputs["Shader"])):
        tree.links.new(socket, output.inputs["Surface"])
        baseline = None
        for roughness in (0, .2, .5, 1):
            assign(node, roughness=roughness)
            pixels = h.render_image(f"v2_{name}_roughness_independent_{roughness}")
            require(all(math.isfinite(v) for v in pixels), "Non-finite diffuse output")
            require(h.max_rgb(pixels) > .01, "Diffuse independence fixture is black")
            if baseline is None:
                baseline = pixels
            else:
                error = max(abs(a - b) for a, b in zip(baseline, pixels))
                require(error < .001, f"{name} changed with roughness: {error}")
    tree.links.new(emission.outputs[0], output.inputs["Surface"])
    for control in ("metallic", "transmission_weight"):
        assign(node, **{control: 1})
        require(h.max_rgb(h.render_image(f"v2_diffuse_{control}_suppressed")) < .0001,
                f"Diffuse no longer respects {control}")
        assign(node, **{control: 0})


def check_zero_roughness_highlight(h):
    """Shadow/reference variation must not become hard-highlight coverage."""
    h.clear_scene()
    h.configure_scene()
    bpy.context.scene.eevee.taa_render_samples = 16
    configure_world((0, 0, 0))
    camera = h.make_camera()
    camera.data.type = "PERSP"
    camera.data.lens = 50
    camera.matrix_world = Matrix([
        (.761538, .648120, 0, -.124411),
        (-.324060, .380769, .866025, -.200145),
        (.561288, -.659512, .5, -2.337060), (0, 0, 0, 1)]).inverted()
    material, node, output = new_material()
    assign(node, base_color=(0, 0, 0, 1), roughness=0, profile_softness=0,
           ambient_strength=0, reflection_strength=0, rim_strength=0)
    node.shadow_mode = "ALL"
    node.mapping_stage = "TOTAL_LIGHTING"
    bpy.ops.mesh.primitive_monkey_add()
    monkey = bpy.context.object
    monkey.matrix_world = Matrix([
        (.409391, 0, 0, 0), (0, .327083, .246207, 0),
        (0, -.246207, .327083, .268133), (0, 0, 0, 1)])
    monkey.data.materials.append(material)
    subdivision = monkey.modifiers.new("Regression subdivision", "SUBSURF")
    subdivision.levels = subdivision.render_levels = 2
    for polygon in monkey.data.polygons:
        polygon.use_smooth = True
    for index, (energy, matrix) in enumerate([
        (54.97787, [( .805528, -1.229286, 1.816732, 1.120336),
                    (.498274, 1.987308, 1.123773, 1.280069),
                    (-2.136208, 0, .947181, 1.417600), (0, 0, 0, 1)]),
        (35.34292, [(-.107620, 1.137235, -1.275165, -1.779818),
                    (-.476459, -1.246984, -1.071891, -.980052),
                    (-1.640834, .287505, .394888, 1.417600), (0, 0, 0, 1)])]):
        light = h.add_point_light(f"Hard highlight {index}", (1, 1, 1), energy, (0, 0, 0))
        light.data.type = "AREA"
        light.matrix_world = Matrix(matrix)
        light.data.use_nodes = True
        tree = light.data.node_tree
        info = tree.nodes.new("ShaderNodeEeveeLightShaderInfo")
        light_output = tree.nodes.new("ShaderNodeEeveeLightShaderOutput")
        for source, target in [("Default Color", "Color"), ("Default Intensity", "Intensity"),
                               ("Default Attenuation", "Attenuation")]:
            tree.links.new(info.outputs[source], light_output.inputs[target])
    for roughness in (0, .05):
        assign(node, roughness=roughness)
        pixels = h.render_image(f"v2_hard_highlight_roughness_{roughness}")
        require(all(math.isfinite(v) for v in pixels), "Non-finite zero-roughness radiance")
        bright = sum(max(pixels[i:i + 3]) > 1 for i in range(0, len(pixels), 4))
        require(bright / (len(pixels) / 4) < .01,
                "Reference/shadow gradients created broad false hard highlights")
    assign(node, roughness=.2, profile_softness=1)
    require(h.max_rgb(h.render_image("v2_zero_roughness_highlight_control")) > .1,
            "Regression fixture lost its actual light response")
    # Reuse the occluding geometry and two finite lights to isolate the new switch.
    require(node.highlight_receive_shadows, "Highlights must receive shadows by default")
    assign(node, roughness=.59545457, profile_softness=0)
    tree = material.node_tree
    emission = tree.nodes.new("ShaderNodeEmission")
    tree.links.new(node.outputs["Highlight"], emission.inputs["Color"])
    tree.links.new(emission.outputs[0], output.inputs["Surface"])
    for model in ("DIRECTION", "INTEGRATED"):
        node.highlight_light_shape = model
        for stage in ("TOTAL_LIGHTING", "MAX_LIGHTING"):
            node.mapping_stage = stage
            node.shadow_mode = "NONE"
            node.highlight_receive_shadows = True
            reference = h.render_image(f"v2_highlight_no_shadow_{model}_{stage}")
            node.shadow_mode = "ALL"
            node.highlight_receive_shadows = False
            disabled = h.render_image(f"v2_highlight_shadow_disabled_{model}_{stage}")
            require(max(abs(a-b) for a,b in zip(reference, disabled)) < .001,
                    "Highlight shadow switch disagrees with unshadowed reference")
            node.highlight_receive_shadows = True
            enabled = h.render_image(f"v2_highlight_shadow_enabled_{model}_{stage}")
            require(h.rms_difference(enabled, disabled) > .0001,
                    "Highlight shadow switch has no visible effect")
    assign(node, base_color=(.8, .8, .8, 1))
    tree.links.new(node.outputs["Diffuse"], emission.inputs["Color"])
    diffuse_on = h.render_image("v2_highlight_switch_diffuse_on")
    node.highlight_receive_shadows = False
    diffuse_off = h.render_image("v2_highlight_switch_diffuse_off")
    require(max(abs(a-b) for a,b in zip(diffuse_on, diffuse_off)) < .001,
            "Highlight shadow switch changed diffuse shading")
    material_name, node_name = material.name, node.name
    path = h.OUTPUT_DIR / "highlight_shadow_switch.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(path))
    bpy.ops.wm.open_mainfile(filepath=str(path))
    require(not bpy.data.materials[material_name].node_tree.nodes[node_name].highlight_receive_shadows,
            "Highlight shadow switch was lost on readback")


def check_storage(node, output_dir):
    require(node.model_version == 2, "A new Principled NPR node must use V2")
    require(len(node.inputs) == 97, f"Unexpected V2 socket count {len(node.inputs)}")
    require([s.identifier for s in node.outputs] ==
            ["shader", "color", "alpha", "diffuse", "highlight", "rim", "emission"],
            "Output IDs changed")
    require(node.outputs[1].name == "Local Color", "Color must be labeled as a local result")
    require(node.coordinate_range == "FRONT", "V2 must default to the positive Lambert range")
    require(node.bl_rna.properties["coordinate_range"].default == "FRONT",
            "RNA must advertise the same positive Lambert default")
    require(abs(inp(node, "coordinate_offset").default_value - 0.45) < 1e-6,
            "New nodes must default to the Malt-like 0..0.1 transition")
    require(inp(node, "mapping_softness").default_value == 0,
            "New nodes must default to the user-selected hard color boundary")
    require(node.light_combine == "ADD" and node.use_all_lights, "V2 light defaults incorrect")
    require(inp(node, "profile_softness").default_value == 0, "Hard highlights must be default")
    require(inp(node, "intensity_influence").default_value == 0, "Energy bias must default off")
    require(node.energy_response_version == 2, "New nodes must use unified lighting")
    require(inp(node, "energy_influence").default_value == 0.5, "Energy response must default to 0.5")
    require(node.inputs[90].identifier == "energy_influence", "New input must be append-only")
    require(node.rim_mode == "FRESNEL", "The existing rim must remain the default")
    require({e.identifier for e in node.bl_rna.properties["rim_mode"].enum_items} ==
            {"FRESNEL", "DEPTH"}, "Rim must expose exactly Fresnel and Depth")
    require(node.highlight_light_shape == "DIRECTION", "New highlights must default to analytic GGX")
    require([s.identifier for s in node.inputs[91:95]] ==
            ["rim_pixel_width", "rim_depth_threshold", "rim_depth_softness", "rim_samples"],
            "Depth rim controls must be append-only")
    require([s.identifier for s in node.inputs[95:]] == ["flatten_strength", "flatten_range"],
            "Flatten inputs must be append-only")
    require(inp(node, "flatten_strength").default_value == 0, "Flattening must default off")
    require(node.mapping_stage == "TOTAL_LIGHTING", "Energy Sum must be the default")
    for stage in ("MAX_LIGHTING", "TOTAL_LIGHTING"):
        node.mapping_stage = stage
        require(node.mapping_stage == stage, "Mapping stage unavailable")
        require(inp(node, "energy_influence").is_unavailable,
                "Unified lighting must hide unused per-light energy controls")
    for stage in ("PER_LIGHT", "COMBINED"):
        try:
            node.mapping_stage = stage
        except TypeError:
            pass
        else:
            raise AssertionError("New nodes must expose exactly two lighting modes")
    require(inp(node, "ambient_strength").default_value == 0, "Indirect diffuse must default off")
    require(inp(node, "reflection_strength").default_value == 0, "Indirect reflection must default off")
    require({"NONE", "ALL", "CAST_ONLY", "SELF_ONLY"}.issubset(
        {e.identifier for e in node.bl_rna.properties["shadow_mode"].enum_items}),
        "Four shadow types are required")

    node.diffuse_mapping = "DRIVEN_RAMP"
    node.driven_stop_count = 3
    inp(node, "stop_position_1").default_value = 1.0
    inp(node, "stop_position_2").default_value = 1.0
    tree = node.id_data
    driver = tree.nodes.new("ShaderNodeValue")
    tree.links.new(driver.outputs[0], inp(node, "stop_offset_2"))
    node.driven_stop_count = 2
    require(inp(node, "stop_offset_2").is_linked, "Hiding a point deleted its link")
    node.driven_stop_count = 3
    node.diffuse_mapping = "SIMPLE"
    node.diffuse_mapping = "DRIVEN_RAMP"
    require(inp(node, "stop_position_1").default_value == 1.0, "Mode switch rewrote point positions")
    require(inp(node, "stop_offset_2").is_linked, "Mode switch lost the local-offset link")
    tree.nodes.remove(driver)
    node.diffuse_mapping = "RAMP"
    points = list(node.ramp_points)
    require(len(points) >= 2, "No stable ramp points")
    original = [(p.identifier, p.position) for p in points]
    path = points[0].path_from_id("position")
    require('ramp_points["' in path, f"Point animation uses an unstable index: {path}")
    points[0].position = 0.9
    points[1].position = 0.1
    require([p.identifier for p in node.ramp_points][:2] == [p[0] for p in original[:2]],
            "Dragging a point physically reordered storage")
    node.diffuse_mapping = "SIMPLE"


def configure_world(color):
    world = bpy.context.scene.world
    world.use_nodes = True
    background = world.node_tree.nodes.get("Background")
    background.inputs["Color"].default_value = (*color, 1)
    background.inputs["Strength"].default_value = 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--expected-backend", choices=("OPENGL", "VULKAN"), required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    args = parser.parse_args(argv)
    args.output_dir = args.output_dir.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    h = load_render_helpers()
    h.OUTPUT_DIR = args.output_dir
    h.clear_scene()
    h.configure_scene()
    h.make_camera()
    material, node, output = new_material()
    material.use_fake_user = True
    check_storage(node, args.output_dir)
    h.make_sphere(material)
    key = h.add_point_light("V2 key", (1, 1, 1), 100.0, (0, -4, 0))
    key.data.shadow_soft_size = 0
    # Keep the light's spatial falloff fixed when testing radiance linearity. EEVEE's
    # automatic influence radius scales with sqrt(power); changing it simultaneously
    # changes the native cutoff attenuation (about 2.102x for this 50 -> 100 W fixture).
    key.data.use_custom_distance = True
    key.data.cutoff_distance = 100.0
    configure_world((0, 0, 0))
    node.shadow_mode = "NONE"
    assign(node, ambient_strength=0, reflection_strength=0, base_color=(0.8, 0.8, 0.8, 1),
           energy_influence=0)  # Transport tests isolate the optional band-energy response.
    baseline = h.render_image("v2_backend")
    import gpu
    active = gpu.platform.backend_type_get()
    print("PRINCIPLED_NPR_V2_BACKEND=" + active, flush=True)
    require(active == args.expected_backend, f"Wrong GPU backend {active}")
    require(h.max_rgb(baseline) > 0.01, "V2 rendered no direct light")

    # A metal has no diffuse; disabling the lamp's diffuse must not remove its specular.
    assign(node, metallic=1, roughness=0.4)
    both = h.render_image("v2_specular_with_diffuse")
    key.data.diffuse_factor = 0
    specular_only = h.render_image("v2_specular_only")
    require(h.rms_difference(both, specular_only) < 0.002, "Diffuse lamp factor changed metallic specular")
    require(h.center_pixel(specular_only)[0] > 0.01, "Specular-only light was discarded")
    key.data.specular_factor = 0
    none = h.render_image("v2_light_off")
    require(h.center_pixel(none)[0] < 1e-4, "A no-light, no-emission metal emitted light")
    key.data.diffuse_factor = key.data.specular_factor = 1

    # Linear source energy, isolated from environment and source-dependent coordinate bias.
    key.data.energy = 50
    low = h.render_image("v2_energy_50")
    key.data.energy = 100
    high = h.render_image("v2_energy_100")
    ratio = h.center_pixel(high)[0] / max(h.center_pixel(low)[0], 1e-8)
    require(1.98 < ratio < 2.02, f"Nonlinear direct light response: {ratio}")

    # A black physical albedo must not discard a bright artistic Replace closure.
    assign(node, metallic=0, base_color=(0, 0, 0, 1), highlight_strength=0,
           shadow_color=(0.6, 0.6, 0.6, 1), lit_color=(0.6, 0.6, 0.6, 1))
    node.color_application = "REPLACE"
    replaced = h.render_image("v2_black_base_replace")
    require(h.center_pixel(replaced)[0] > 0.02, "Reservoir discarded black-albedo Replace")

    # Local Color is a data output, not an additional surface contribution.
    assign(node, base_color=(0.8, 0.8, 0.8, 1), highlight_strength=1)
    node.color_application = "MULTIPLY"
    shader_pixels = h.render_image("v2_shader_direct")
    tree = material.node_tree
    tree.links.remove(output.inputs["Surface"].links[0])
    emission = tree.nodes.new("ShaderNodeEmission")
    tree.links.new(node.outputs["Local Color"], emission.inputs["Color"])
    tree.links.new(emission.outputs[0], output.inputs["Surface"])
    local_pixels = h.render_image("v2_local_color_direct")
    require(h.rms_difference(shader_pixels, local_pixels) < 0.01,
            "Local Color added hidden closures or disagrees with isolated direct Shader")
    # Exercise every output argument together (the GPU function signature used
    # to exceed MAX_PARAMETER), and compare RGB within the same light sample.
    component_nodes = []
    total = node.outputs["Diffuse"]
    for name in ("Highlight", "Rim", "Emission"):
        add = tree.nodes.new("ShaderNodeVectorMath")
        add.operation = "ADD"
        tree.links.new(total, add.inputs[0])
        tree.links.new(node.outputs[name], add.inputs[1])
        total = add.outputs[0]
        component_nodes.append(add)
    error = tree.nodes.new("ShaderNodeVectorMath")
    error.operation = "DISTANCE"
    tree.links.new(total, error.inputs[0])
    tree.links.new(node.outputs["Local Color"], error.inputs[1])
    tree.links.new(error.outputs["Value"], emission.inputs["Color"])
    difference = h.render_image("v2_component_sum_error")
    require(h.max_rgb(difference) < 0.0001, "Component RGB sum differs from Local Color")
    for item in component_nodes + [error]:
        tree.nodes.remove(item)
    tree.nodes.remove(emission)
    tree.links.new(node.outputs["Shader"], output.inputs["Surface"])

    # Environment transport remains present, independently of direct light.
    assign(node, metallic=1, direct_strength=0, reflection_strength=0)
    configure_world((0.2, 0.4, 0.8))
    no_environment = h.render_image("v2_environment_off")
    inp(node, "reflection_strength").default_value = 1
    environment = h.render_image("v2_environment_on")
    require(h.center_pixel(environment)[2] > h.center_pixel(no_environment)[2] + 0.01,
            "Native reflection transport or indirect gain is missing")

    # Serialize the new data without touching user scenes or the immutable V1 fixture.
    node_name, material_name = node.name, material.name
    full_range_node = material.node_tree.nodes.new("ShaderNodePrincipledNPR")
    full_range_node.coordinate_range = "FULL"
    inp(full_range_node, "coordinate_offset").default_value = 0
    inp(full_range_node, "mapping_softness").default_value = 0.6
    full_range_name = full_range_node.name
    saved = args.output_dir / "v2_roundtrip.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(saved))
    bpy.ops.wm.open_mainfile(filepath=str(saved))
    loaded = bpy.data.materials[material_name].node_tree.nodes[node_name]
    require(loaded.model_version == 2, "V2 model version was lost on readback")
    require(loaded.coordinate_range == "FRONT", "Positive range changed on readback")
    require(loaded.id_data.nodes[full_range_name].coordinate_range == "FULL",
            "The new default must not overwrite an explicitly saved full range")
    require(abs(inp(loaded, "coordinate_offset").default_value - 0.45) < 1e-6,
            "Default boundary changed on readback")
    saved_settings = loaded.id_data.nodes[full_range_name]
    require(inp(saved_settings, "coordinate_offset").default_value == 0 and
            abs(inp(saved_settings, "mapping_softness").default_value - 0.6) < 1e-6,
            "New defaults overwrote explicitly saved mapping settings")
    require(len(loaded.inputs) == 97 and loaded.energy_response_version == 2,
            "V2 energy response or inputs changed on readback")
    check_diffuse_roughness_independence(h)
    check_zero_roughness_highlight(h)
    check_standalone_rim(h)
    check_shared_principled_math(h)
    report = {"backend": active, "build_hash": bpy.app.build_hash.decode(),
              "build_branch": bpy.app.build_branch.decode(), "linear_energy_ratio": ratio,
              "direct_local_rms": h.rms_difference(shader_pixels, local_pixels)}
    (args.output_dir / "v2_results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("PRINCIPLED_NPR_V2_OK", flush=True)


if __name__ == "__main__":
    main()
