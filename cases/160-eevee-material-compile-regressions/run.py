from pathlib import Path
import re
import subprocess
import textwrap

import bpy


CASE_DIR = Path(__file__).resolve().parent
ROOT = CASE_DIR.parents[3]
OUT_DIR = CASE_DIR / "out"
CHILD_SCRIPT = OUT_DIR / "material_compile_probe_child.py"
DEFAULT_CHILD_LOG = OUT_DIR / "material_compile_probe_default.log"
VULKAN_CHILD_LOG = OUT_DIR / "material_compile_probe_vulkan.log"
DONE_MARKER = "__MATERIAL_COMPILE_PROBE_DONE__"
BACKEND_MARKER = "__MATERIAL_COMPILE_PROBE_BACKEND__="


CHILD_SOURCE = r"""
import bpy
import gpu


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def configure_scene():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 96
    scene.render.resolution_y = 96
    scene.render.resolution_percentage = 100
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    if scene.world is not None:
        scene.world.color = (0.0, 0.0, 0.0)

    eevee = scene.eevee
    for name, value in (
        ("taa_render_samples", 1),
        ("taa_samples", 1),
        ("use_gtao", False),
        ("use_bloom", False),
        ("use_raytracing", True),
    ):
        if hasattr(eevee, name):
            setattr(eevee, name, value)


def make_camera():
    camera_data = bpy.data.cameras.new("Camera")
    camera = bpy.data.objects.new("Camera", camera_data)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera
    camera.location = (0.0, 0.0, 4.0)
    camera.rotation_euler = (0.0, 0.0, 0.0)
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 3.0


def make_light():
    light_data = bpy.data.lights.new("CompileProbeLight", "POINT")
    light = bpy.data.objects.new("CompileProbeLight", light_data)
    bpy.context.scene.collection.objects.link(light)
    light.location = (0.0, 0.0, 3.0)
    light_data.energy = 500.0
    light_data.use_shadow = True


def make_image(name, color):
    image = bpy.data.images.new(name, width=2, height=2, alpha=True)
    image.pixels.foreach_set(color * 4)
    return image


def make_texture_chain(nodes, links, x_offset):
    colors = (
        (1.0, 0.1, 0.1, 1.0),
        (0.1, 1.0, 0.1, 1.0),
        (0.1, 0.1, 1.0, 1.0),
        (1.0, 1.0, 0.1, 1.0),
    )
    previous = None
    for index, color in enumerate(colors):
        image = make_image(f"CompileProbeImage_{x_offset}_{index}", color)
        tex = nodes.new("ShaderNodeTexImage")
        tex.image = image
        tex.location = (x_offset, 140.0 - index * 180.0)
        if previous is None:
            previous = tex.outputs["Color"]
            continue
        mix = nodes.new("ShaderNodeMixRGB")
        mix.inputs[0].default_value = 0.5
        mix.location = (x_offset + 260.0 + index * 140.0, 80.0 - index * 120.0)
        links.new(previous, mix.inputs[1])
        links.new(tex.outputs["Color"], mix.inputs[2])
        previous = mix.outputs["Color"]
    return previous


def make_shader_info_material():
    material = bpy.data.materials.new("ShaderInfoSoftFilteredSamplerStress")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    shader_info = nodes.new("ShaderNodeShaderInfo")
    shader_info.shadow_mode = "SOFT_FILTERED"
    if hasattr(shader_info, "stable_shadow_samples"):
        shader_info.stable_shadow_samples = 8

    output.location = (980.0, 0.0)
    emission.location = (720.0, 0.0)
    shader_info.location = (360.0, -260.0)

    texture_color = make_texture_chain(nodes, links, -360.0)
    links.new(texture_color, emission.inputs["Color"])
    links.new(shader_info.outputs["Shadow"], emission.inputs["Strength"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_curvature_material():
    material = bpy.data.materials.new("CurvatureHizSamplerStress")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    curvature = nodes.new("ShaderNodeCurvature")
    curvature.inputs["Samples"].default_value = 8.0
    curvature.inputs["Sample Radius"].default_value = 1.0
    curvature.inputs["Thickness"].default_value = 1.0
    curvature.inputs["Scale"].default_value = (1.0, 1.0, 0.0)

    output.location = (980.0, 0.0)
    emission.location = (720.0, 0.0)
    curvature.location = (360.0, -260.0)

    texture_color = make_texture_chain(nodes, links, -360.0)
    links.new(texture_color, emission.inputs["Color"])
    links.new(curvature.outputs["Scene Curvature"], emission.inputs["Strength"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_world_to_tangent_material():
    material = bpy.data.materials.new("WorldToTangentCompileProbe")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    world_to_tangent = nodes.new("ShaderNodeWorldToTangent")

    output.location = (780.0, 0.0)
    emission.location = (520.0, 0.0)
    world_to_tangent.location = (220.0, 0.0)

    world_to_tangent.inputs["Vector"].default_value = (0.0, 0.0, 1.0)
    links.new(world_to_tangent.outputs["Vector"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def make_mesh_volume_material():
    material = bpy.data.materials.new("MeshVolumeCompileProbe")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    volume = nodes.new("ShaderNodeVolumePrincipled")

    output.location = (820.0, 0.0)
    emission.location = (520.0, 120.0)
    volume.location = (520.0, -120.0)

    emission.inputs["Color"].default_value = (0.15, 0.25, 1.0, 1.0)
    emission.inputs["Strength"].default_value = 0.15
    if "Color" in volume.inputs:
        volume.inputs["Color"].default_value = (0.2, 0.45, 1.0, 1.0)
    if "Density" in volume.inputs:
        volume.inputs["Density"].default_value = 0.2

    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    links.new(volume.outputs["Volume"], output.inputs["Volume"])
    return material


def make_plane(name, location, material):
    bpy.ops.mesh.primitive_plane_add(size=1.0, location=location)
    obj = bpy.context.object
    obj.name = name
    obj.data.materials.append(material)


def make_sphere(name, location, material):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16, radius=0.45, location=location)
    obj = bpy.context.object
    obj.name = name
    obj.data.materials.append(material)


def make_cube(name, location, material):
    bpy.ops.mesh.primitive_cube_add(size=0.72, location=location)
    obj = bpy.context.object
    obj.name = name
    obj.data.materials.append(material)


def main():
    clear_scene()
    configure_scene()
    make_camera()
    make_light()

    assert hasattr(bpy.types, "ShaderNodeShaderInfo"), "ShaderNodeShaderInfo is not registered"
    assert hasattr(bpy.types, "ShaderNodeCurvature"), "ShaderNodeCurvature is not registered"
    assert hasattr(bpy.types, "ShaderNodeWorldToTangent"), (
        "ShaderNodeWorldToTangent is not registered"
    )

    make_plane("ShaderInfoSamplerPlane", (-0.95, 0.55, 0.0), make_shader_info_material())
    make_sphere("CurvatureSamplerSphere", (0.95, 0.55, 0.0), make_curvature_material())
    make_plane("WorldToTangentPlane", (-0.95, -0.65, 0.0), make_world_to_tangent_material())
    make_cube("MeshVolumeCube", (0.95, -0.65, 0.0), make_mesh_volume_material())

    bpy.ops.render.render(write_still=False)
    print("__MATERIAL_COMPILE_PROBE_BACKEND__=" + gpu.platform.backend_type_get(), flush=True)
    print("__MATERIAL_COMPILE_PROBE_DONE__", flush=True)


if __name__ == "__main__":
    main()
"""


ERROR_PATTERNS = (
    ("sampler overflow", re.compile(r"uses too many samplers", re.IGNORECASE)),
    ("missing hiz_tx", re.compile(r'undefined variable\s+"hiz_tx"', re.IGNORECASE)),
    ("missing node_tree", re.compile(r'undefined variable\s+"node_tree"', re.IGNORECASE)),
    ("missing generated sampler", re.compile(r'undefined variable\s+"samp[0-9]+"', re.IGNORECASE)),
    ("missing BSL resource helper", re.compile(r'undefined variable\s+"(?:drw_resource_id_raw|object_infos_get|drw_object_infos)"', re.IGNORECASE)),
    ("missing world-to-tangent node", re.compile(r'undefined variable\s+"node_world_to_tangent"', re.IGNORECASE)),
    (
        "overlapping GPU resource binding",
        re.compile(
            r"Validation failed\s*:\s*Overlapping\b.*\bat binding location\b",
            re.IGNORECASE,
        ),
    ),
    (
        "missing GPU resource binding",
        re.compile(
            r"\bERROR Missing (?:Texture|Image|Uniform Buffer|Storage Buffer) bind at slot\b",
            re.IGNORECASE,
        ),
    ),
)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def write_child_script():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    CHILD_SCRIPT.write_text(textwrap.dedent(CHILD_SOURCE).strip() + "\n", encoding="utf-8")


def run_child_blender(extra_args, child_log):
    command = [
        bpy.app.binary_path,
        "--background",
        "--factory-startup",
        *extra_args,
        "--python-exit-code",
        "1",
        "--python",
        str(CHILD_SCRIPT),
    ]
    result = subprocess.run(
        command,
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=120,
    )
    child_log.write_text(result.stdout, encoding="utf-8")
    return result


def find_shader_compile_regressions(output):
    matches = []
    for line in output.splitlines():
        for label, pattern in ERROR_PATTERNS:
            if pattern.search(line):
                matches.append(f"{label}: {line.strip()}")
                break
    return matches


def tail_text(text, line_count=80):
    lines = text.splitlines()
    return "\n".join(lines[-line_count:])


def validate_probe(result, child_log, expected_backend=None):
    require(
        result.returncode == 0,
        "child Blender failed with exit "
        f"{result.returncode}\n--- child tail ---\n{tail_text(result.stdout)}",
    )
    require(
        DONE_MARKER in result.stdout,
        f"child Blender did not finish the render probe\n--- child tail ---\n{tail_text(result.stdout)}",
    )
    if expected_backend is not None:
        require(
            BACKEND_MARKER + expected_backend in result.stdout,
            f"child Blender did not use the {expected_backend} backend"
            f"\n--- child tail ---\n{tail_text(result.stdout)}",
        )

    matches = find_shader_compile_regressions(result.stdout)
    require(
        not matches,
        "shader compiler regression detected:\n"
        + "\n".join(matches)
        + f"\n--- child log ---\n{child_log}",
    )


def main():
    write_child_script()

    default_result = run_child_blender([], DEFAULT_CHILD_LOG)
    print(
        f"eevee_material_compile_regressions default_log={DEFAULT_CHILD_LOG}", flush=True
    )
    validate_probe(default_result, DEFAULT_CHILD_LOG)

    vulkan_result = run_child_blender(
        ["--gpu-backend", "vulkan", "--debug-gpu"], VULKAN_CHILD_LOG
    )
    print(
        f"eevee_material_compile_regressions vulkan_log={VULKAN_CHILD_LOG}", flush=True
    )
    validate_probe(vulkan_result, VULKAN_CHILD_LOG, expected_backend="VULKAN")

    print("EEVEE_MATERIAL_COMPILE_REGRESSIONS_RELEASE_OK", flush=True)


if __name__ == "__main__":
    main()
