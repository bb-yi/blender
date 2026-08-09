from pathlib import Path
import os
import sys

import bpy


CASE_DIR = Path(__file__).resolve().parent
OUT_DIR = CASE_DIR / "out"
VISIBILITY_MARGIN = 0.35


def set_eevee_engine(scene):
    for engine in ("BLENDER_EEVEE_NEXT", "BLENDER_EEVEE"):
        try:
            scene.render.engine = engine
            return
        except TypeError:
            pass
    raise RuntimeError("No Eevee render engine enum accepted")


def reset_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()

    scene = bpy.context.scene
    set_eevee_engine(scene)
    scene.render.resolution_x = 64
    scene.render.resolution_y = 64
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = False
    scene.render.image_settings.file_format = "PNG"
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0

    if scene.world is None:
        scene.world = bpy.data.worlds.new("CullWorld")
    scene.world.color = (0.0, 0.0, 0.0)

    if hasattr(scene, "eevee"):
        for name, value in (
            ("taa_render_samples", 1),
            ("taa_samples", 1),
            ("use_gtao", False),
            ("use_bloom", False),
            ("use_raytracing", False),
        ):
            if hasattr(scene.eevee, name):
                setattr(scene.eevee, name, value)

    camera_data = bpy.data.cameras.new("Camera")
    camera = bpy.data.objects.new("Camera", camera_data)
    bpy.context.collection.objects.link(camera)
    camera.location = (0.0, 0.0, 3.0)
    camera.rotation_euler = (0.0, 0.0, 0.0)
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 3.0
    scene.camera = camera
    return scene


def create_material(cull_method, render_method):
    mat = bpy.data.materials.new(f"surface_cull_{render_method}_{cull_method}")
    mat.use_nodes = True
    mat.surface_cull_method = cull_method
    mat.surface_render_method = render_method
    mat.diffuse_color = (1.0, 1.0, 1.0, 1.0)

    nodes = mat.node_tree.nodes
    nodes.clear()
    output = nodes.new(type="ShaderNodeOutputMaterial")
    emission = nodes.new(type="ShaderNodeEmission")
    emission.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    emission.inputs["Strength"].default_value = 1.0

    if render_method == "BLENDED":
        transparent = nodes.new(type="ShaderNodeBsdfTransparent")
        mix = nodes.new(type="ShaderNodeMixShader")
        mix.inputs["Fac"].default_value = 0.75
        mat.node_tree.links.new(transparent.outputs["BSDF"], mix.inputs[1])
        mat.node_tree.links.new(emission.outputs["Emission"], mix.inputs[2])
        mat.node_tree.links.new(mix.outputs["Shader"], output.inputs["Surface"])
    else:
        mat.node_tree.links.new(emission.outputs["Emission"], output.inputs["Surface"])

    return mat


def create_plane(front_facing, mat):
    mesh = bpy.data.meshes.new("SurfaceCullPlaneMesh")
    verts = [(-1.0, -1.0, 0.0), (1.0, -1.0, 0.0), (1.0, 1.0, 0.0), (-1.0, 1.0, 0.0)]
    face = (0, 1, 2, 3) if front_facing else (3, 2, 1, 0)
    mesh.from_pydata(verts, [], [face])
    mesh.update()
    obj = bpy.data.objects.new("SurfaceCullPlane", mesh)
    bpy.context.collection.objects.link(obj)
    obj.data.materials.append(mat)
    return obj


def render_brightness(scene, label):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    filepath = OUT_DIR / f"{label}.png"
    if filepath.exists():
        filepath.unlink()
    scene.render.filepath = os.fspath(filepath)
    bpy.ops.render.render(write_still=True)

    image = bpy.data.images.load(os.fspath(filepath), check_existing=False)
    width = int(scene.render.resolution_x * scene.render.resolution_percentage / 100)
    height = int(scene.render.resolution_y * scene.render.resolution_percentage / 100)
    pixels = list(image.pixels)
    total = 0.0
    count = 0
    min_x = width // 4
    max_x = width - min_x
    min_y = height // 4
    max_y = height - min_y
    for y in range(min_y, max_y):
        for x in range(min_x, max_x):
            index = (y * width + x) * 4
            total += (pixels[index] + pixels[index + 1] + pixels[index + 2]) / 3.0
            count += 1
    bpy.data.images.remove(image)
    return total / count


def expected_visible(cull_method, front_facing):
    if cull_method == "NONE":
        return True
    if cull_method == "BACK":
        return front_facing
    if cull_method == "FRONT":
        return not front_facing
    raise AssertionError(cull_method)


def render_case(render_method, cull_method, front_facing):
    scene = reset_scene()
    mat = create_material(cull_method, render_method)
    create_plane(front_facing, mat)
    facing_label = "front" if front_facing else "back"
    return render_brightness(scene, f"{render_method.lower()}_{facing_label}_{cull_method.lower()}")


def main():
    for old_file in OUT_DIR.glob("*.png"):
        old_file.unlink()

    background = render_brightness(reset_scene(), "background")
    failures = []

    print(f"SURFACE_CULL background={background:.4f}")
    for render_method in ("DITHERED", "BLENDED"):
        for front_facing in (True, False):
            for cull_method in ("NONE", "BACK", "FRONT"):
                brightness = render_case(render_method, cull_method, front_facing)
                visible = brightness > background + VISIBILITY_MARGIN
                expected = expected_visible(cull_method, front_facing)
                facing_label = "front" if front_facing else "back"
                print(
                    f"SURFACE_CULL render={render_method} facing={facing_label} "
                    f"cull={cull_method} brightness={brightness:.4f} "
                    f"visible={visible} expected={expected}"
                )
                if visible != expected:
                    failures.append(
                        (render_method, facing_label, cull_method, brightness, visible, expected)
                    )

    if failures:
        print("SURFACE_CULL failures:")
        for failure in failures:
            print(f"  {failure}")
        sys.exit(1)


if __name__ == "__main__":
    main()
