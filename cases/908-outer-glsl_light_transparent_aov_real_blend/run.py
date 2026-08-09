from pathlib import Path
import re
import subprocess
import textwrap

import bpy


CASE_DIR = Path(__file__).resolve().parent
ROOT = CASE_DIR.parents[3]
BLEND_PATH = CASE_DIR / "assets" / "灯光函数透明aov报错.blend"
OUTPUT_DIR = ROOT / "temp" / "release_test_outputs" / "glsl_light_transparent_aov_real_blend"
LOG_PATH = OUTPUT_DIR / "shader.log"

CHILD_SCRIPT = r'''
import bpy
from mathutils import Vector


def look_at(obj, target):
    direction = Vector(target) - obj.location
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


scene = bpy.context.scene
view_layer = bpy.context.view_layer

materials = [material for material in bpy.data.materials if material.node_tree is not None]
assert any(
    any(node.bl_idname == "ShaderNodeGLSLFunction" for node in material.node_tree.nodes)
    for material in materials
), "Expected a material with ShaderNodeGLSLFunction in the repro blend"
assert any(
    any(node.bl_idname == "ShaderNodeOutputAOV" for node in material.node_tree.nodes)
    for material in materials
), "Expected a material with ShaderNodeOutputAOV in the repro blend"
assert any(aov.name == "PBR" and aov.type == "COLOR" for aov in view_layer.aovs), (
    "Expected the repro blend to keep the PBR color AOV"
)
assert any(obj.type == "MESH" for obj in scene.objects), "Expected a mesh object in the repro blend"

scene.render.engine = "BLENDER_EEVEE"
scene.render.resolution_x = 16
scene.render.resolution_y = 16
scene.render.resolution_percentage = 100
scene.render.image_settings.file_format = "PNG"
scene.eevee.taa_samples = 1
scene.eevee.taa_render_samples = 1
scene.view_settings.view_transform = "Standard"
scene.view_settings.look = "None"
scene.view_settings.exposure = 0.0
scene.view_settings.gamma = 1.0

if scene.camera is None:
    bpy.ops.object.camera_add(location=(0.0, -6.0, 3.0))
    scene.camera = bpy.context.object
    look_at(scene.camera, (0.0, 0.0, 0.0))

if not any(obj.type == "LIGHT" for obj in scene.objects):
    bpy.ops.object.light_add(type="POINT", location=(0.0, -3.0, 4.0))

bpy.ops.render.render(write_still=False)
print("GLSL_LIGHT_TRANSPARENT_AOV_REPRO_OK")
'''

BAD_PATTERNS = [
    re.compile(r"gpu\.shader\s+\|\s+ERROR", re.IGNORECASE),
    re.compile(r"\bC0000\b"),
    re.compile(r"\bsyntax error\b", re.IGNORECASE),
    re.compile(r"unexpected\s+'\)'", re.IGNORECASE),
    re.compile(r"unexpected\s+\)", re.IGNORECASE),
]


def output_excerpt(output, max_lines=80):
    lines = output.splitlines()
    if len(lines) <= max_lines:
        return output
    head_count = max_lines // 2
    tail_count = max_lines - head_count
    excerpt = lines[:head_count]
    excerpt.append(f"... omitted {len(lines) - max_lines} lines ...")
    excerpt.extend(lines[-tail_count:])
    return "\n".join(excerpt)


assert BLEND_PATH.exists(), f"Missing blend file: {BLEND_PATH}"

OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

command = [
    bpy.app.binary_path,
    "--background",
    "--factory-startup",
    str(BLEND_PATH),
    "--log",
    "gpu.shader",
    "--log-level",
    "2",
    "--python-exit-code",
    "1",
    "--python-expr",
    textwrap.dedent(CHILD_SCRIPT),
]

result = subprocess.run(
    command,
    cwd=str(ROOT),
    capture_output=True,
    text=True,
    encoding="utf-8",
    errors="replace",
    check=False,
)

combined_output = result.stdout + result.stderr
LOG_PATH.write_text(combined_output, encoding="utf-8")

print(f"CHILD_EXIT_CODE={result.returncode}")
print(f"CHILD_LOG={LOG_PATH}")

assert result.returncode == 0, (
    "The repro blend render failed. Child Blender output excerpt:\n"
    + output_excerpt(combined_output)
)
assert "GLSL_LIGHT_TRANSPARENT_AOV_REPRO_OK" in combined_output, (
    "The repro blend render did not reach the success marker. Child Blender output excerpt:\n"
    + output_excerpt(combined_output)
)
assert "gpu_shader_material_glsl_light_access.glsl" in combined_output, (
    "The repro blend did not compile through the GLSL light-access helper path."
)

matches = []
for pattern in BAD_PATTERNS:
    matches.extend(pattern.findall(combined_output))

assert not matches, (
    "The repro blend shader log contains GPU shader syntax diagnostics. "
    f"Matches: {matches}\nChild Blender output excerpt:\n{output_excerpt(combined_output)}"
)

print("GLSL_LIGHT_TRANSPARENT_AOV_RELEASE_CASE_OK")
