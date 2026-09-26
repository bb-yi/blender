# SPDX-License-Identifier: GPL-2.0-or-later
"""Functional Principled NPR V2 contract tests; executed separately on each GPU backend."""
import argparse
import importlib.util
import json
import math
from pathlib import Path
import sys

import bpy


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


def check_storage(node, output_dir):
    require(node.model_version == 2, "A new Principled NPR node must use V2")
    require(len(node.inputs) == 100, f"Unexpected V2 socket count {len(node.inputs)}")
    require([s.identifier for s in node.outputs] == ["shader", "color", "alpha"], "Output IDs changed")
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
    require(node.highlight_light_shape == "DIRECTION", "New highlights must default to analytic GGX")
    require([s.identifier for s in node.inputs[91:94]] ==
            ["rim_pixel_width", "rim_depth_threshold", "rim_depth_softness"],
            "Depth rim controls must be append-only")
    require([s.identifier for s in node.inputs[94:98]] ==
            ["goo_rim_samples", "goo_rim_radius", "goo_rim_thickness", "goo_rim_scale"],
            "Goo rim controls must be append-only")
    require([s.identifier for s in node.inputs[98:]] == ["flatten_strength", "flatten_range"],
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
    require(inp(node, "ambient_strength").default_value == 1, "Indirect diffuse must default on")
    require(inp(node, "reflection_strength").default_value == 1, "Indirect reflection must default on")
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
    require(len(loaded.inputs) == 100 and loaded.energy_response_version == 2,
            "V2 energy response or inputs changed on readback")
    report = {"backend": active, "build_hash": bpy.app.build_hash.decode(),
              "build_branch": bpy.app.build_branch.decode(), "linear_energy_ratio": ratio,
              "direct_local_rms": h.rms_difference(shader_pixels, local_pixels)}
    (args.output_dir / "v2_results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("PRINCIPLED_NPR_V2_OK", flush=True)


if __name__ == "__main__":
    main()
