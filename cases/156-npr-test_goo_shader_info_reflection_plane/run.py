from pathlib import Path

import bpy


CASE_DIR = Path(__file__).resolve().parent
ASSET_PATH = CASE_DIR / "assets" / "shader info 反射平面测试.blend"
OUT_DIR = CASE_DIR / "out"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def configure_render():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 512
    scene.render.resolution_y = 288
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0

    eevee = scene.eevee
    for attr, value in (
        ("taa_render_samples", 16),
        ("taa_samples", 8),
        ("use_gtao", False),
        ("use_bloom", False),
        ("use_raytracing", True),
    ):
        if hasattr(eevee, attr):
            setattr(eevee, attr, value)


def load_asset():
    require(ASSET_PATH.exists(), f"missing reflection asset: {ASSET_PATH}")
    bpy.ops.wm.open_mainfile(filepath=str(ASSET_PATH))
    configure_render()


def find_shader_info_paths():
    direct_paths = []
    npr_paths = []
    for obj in bpy.data.objects:
        if obj.type != "MESH" or obj.data is None:
            continue
        for material in obj.data.materials:
            if material is None or material.node_tree is None:
                continue
            direct_count = sum(1 for node in material.node_tree.nodes if node.bl_idname == "ShaderNodeShaderInfo")
            if direct_count:
                direct_paths.append((material.name, direct_count))
            for node in material.node_tree.nodes:
                if node.bl_idname != "ShaderNodeOutputMaterial":
                    continue
                nprtree = getattr(node, "nprtree", None)
                if nprtree is None:
                    continue
                npr_count = sum(1 for npr_node in nprtree.nodes if npr_node.bl_idname == "ShaderNodeShaderInfo")
                if npr_count:
                    npr_paths.append((material.name, nprtree.name, npr_count))
    return direct_paths, npr_paths


def render_pixels(label):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    path = OUT_DIR / f"{label}.png"
    bpy.context.scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        pixels = list(image.pixels[:])
        width, height = image.size
    finally:
        bpy.data.images.remove(image)
    return width, height, pixels, path


def region_luma(pixels, width, height, x0, y0, x1, y1):
    require(0 <= x0 < x1 <= width, f"invalid x-range: {x0}..{x1} for width {width}")
    require(0 <= y0 < y1 <= height, f"invalid y-range: {y0}..{y1} for height {height}")
    values = []
    for y in range(y0, y1):
        for x in range(x0, x1):
            index = (y * width + x) * 4
            r, g, b = pixels[index : index + 3]
            values.append(r * 0.2126 + g * 0.7152 + b * 0.0722)
    return sum(values) / len(values)


def main():
    load_asset()
    direct_paths, npr_paths = find_shader_info_paths()
    require(
        len(direct_paths) == 1 and direct_paths[0][1] == 1,
        f"expected one direct Shader Info material path, found {direct_paths}",
    )
    require(
        len(npr_paths) == 1 and npr_paths[0][2] == 1,
        f"expected one NPR Shader Info material path, found {npr_paths}",
    )

    width, height, pixels, path = render_pixels("shader_info_reflection_plane")
    left_reflection = region_luma(pixels, width, height, 120, 35, 210, 105)
    right_reflection = region_luma(pixels, width, height, 325, 35, 415, 105)
    left_monkey = region_luma(pixels, width, height, 105, 170, 185, 230)

    print(
        "shader_info_reflection_plane: "
        f"left_reflection={left_reflection:.6f} "
        f"right_reflection={right_reflection:.6f} "
        f"left_monkey={left_monkey:.6f} "
        f"path={path}",
        flush=True,
    )

    require(left_monkey > 0.2, f"left monkey should remain lit: {left_monkey}")
    require(left_reflection > 0.15, f"left reflection stayed too dark: {left_reflection}")
    require(
        left_reflection > right_reflection + 0.08,
        f"left reflection did not improve enough over the NPR control: "
        f"left={left_reflection}, right={right_reflection}",
    )
    require(right_reflection > 0.04, f"right reflection control is unexpectedly dark: {right_reflection}")

    print("NPR_SHADER_INFO_REFLECTION_PLANE_RELEASE_OK", flush=True)


if __name__ == "__main__":
    main()
