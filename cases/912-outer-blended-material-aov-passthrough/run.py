from pathlib import Path

import bpy


CASE_DIR = Path(__file__).resolve().parent
ROOT = CASE_DIR.parents[3]
BLEND_PATH = CASE_DIR / "assets" / "blended_material_aov_passthrough.blend"
OUTPUT_DIR = ROOT / "temp" / "release_test_outputs" / "blended_material_aov_passthrough"


def configure_render():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 256
    scene.render.resolution_y = 256
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    if hasattr(scene.render, "use_compositing"):
        scene.render.use_compositing = True

    scene.eevee.taa_samples = 16
    scene.eevee.taa_render_samples = 16
    if hasattr(scene.eevee, "use_taa_reprojection"):
        scene.eevee.use_taa_reprojection = False

    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0


def principled_alpha_values(material):
    if material.node_tree is None:
        return []
    values = []
    for node in material.node_tree.nodes:
        if node.bl_idname != "ShaderNodeBsdfPrincipled":
            continue
        alpha_socket = node.inputs.get("Alpha")
        if alpha_socket is not None and hasattr(alpha_socket, "default_value"):
            values.append(float(alpha_socket.default_value))
    return values


def material_has_aov_output(material):
    return material.node_tree is not None and any(
        node.bl_idname == "ShaderNodeOutputAOV" for node in material.node_tree.nodes
    )


def load_pixels(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        return list(image.pixels[:]), int(image.size[0]), int(image.size[1])
    finally:
        bpy.data.images.remove(image)


def image_stats(pixels, width, height):
    red_dominant = 0
    non_black = 0
    sum_r = 0.0
    sum_g = 0.0
    sum_b = 0.0
    max_r = 0.0
    max_g = 0.0
    max_b = 0.0
    count = width * height

    for index in range(0, len(pixels), 4):
        r = pixels[index]
        g = pixels[index + 1]
        b = pixels[index + 2]
        sum_r += r
        sum_g += g
        sum_b += b
        max_r = max(max_r, r)
        max_g = max(max_g, g)
        max_b = max(max_b, b)
        if max(r, g, b) > 0.02:
            non_black += 1
        if r > 0.25 and g < 0.15 and b < 0.15:
            red_dominant += 1

    return {
        "avg_r": sum_r / count,
        "avg_g": sum_g / count,
        "avg_b": sum_b / count,
        "max_r": max_r,
        "max_g": max_g,
        "max_b": max_b,
        "red_dominant_ratio": red_dominant / count,
        "non_black_ratio": non_black / count,
    }


assert BLEND_PATH.exists(), f"Missing blend file: {BLEND_PATH}"

bpy.ops.wm.open_mainfile(filepath=str(BLEND_PATH))
scene = bpy.context.scene
view_layer = bpy.context.view_layer

assert scene.use_nodes, "Expected the repro blend to keep compositor nodes enabled."
assert any(aov.name == "AOV" and aov.type == "COLOR" for aov in view_layer.aovs), (
    "Expected the repro blend to keep the AOV color pass."
)

blended_materials = [
    material
    for material in bpy.data.materials
    if getattr(material, "surface_render_method", None) == "BLENDED"
]
assert blended_materials, "Expected a foreground material using BLENDED surface rendering."

opaque_blended_materials = [
    material
    for material in blended_materials
    if getattr(material, "blend_method", None) == "BLEND"
    and any(abs(alpha - 1.0) <= 1.0e-6 for alpha in principled_alpha_values(material))
]
assert opaque_blended_materials, (
    "Expected a BLENDED / BLEND foreground material with Principled Alpha 1.0."
)

assert any(material_has_aov_output(material) for material in bpy.data.materials), (
    "Expected a material with ShaderNodeOutputAOV in the repro blend."
)

configure_render()

OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
output_path = OUTPUT_DIR / "blended_material_aov_passthrough.png"
scene.render.filepath = str(output_path)
bpy.ops.render.render(write_still=True)

assert output_path.exists(), f"Render did not write {output_path}"

pixels, width, height = load_pixels(output_path)
stats = image_stats(pixels, width, height)

print(
    "AOV_PASSTHROUGH_STATS="
    f"avg:{stats['avg_r']:.6f},{stats['avg_g']:.6f},{stats['avg_b']:.6f} "
    f"max:{stats['max_r']:.6f},{stats['max_g']:.6f},{stats['max_b']:.6f} "
    f"red_ratio:{stats['red_dominant_ratio']:.6f} "
    f"non_black_ratio:{stats['non_black_ratio']:.6f}"
)

assert stats["non_black_ratio"] >= 0.08, (
    "Expected visible AOV output instead of a black image, "
    f"got stats {stats}"
)
assert stats["red_dominant_ratio"] >= 0.08, (
    "Expected a substantial red-dominant AOV region, "
    f"got stats {stats}"
)
assert stats["max_r"] >= 0.45, f"Expected a strong red AOV signal, got stats {stats}"
assert stats["avg_r"] > stats["avg_g"] + 0.05, (
    "Expected red to dominate green in the AOV output, "
    f"got stats {stats}"
)
assert stats["avg_r"] > stats["avg_b"] + 0.05, (
    "Expected red to dominate blue in the AOV output, "
    f"got stats {stats}"
)

print("BLENDED_MATERIAL_AOV_PASSTHROUGH_RELEASE_CASE_OK")
