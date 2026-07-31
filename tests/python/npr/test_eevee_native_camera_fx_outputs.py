import argparse
import json
import sys
from pathlib import Path

import bpy
import gpu
import OpenImageIO as oiio


RESOLUTION_X = 256
RESOLUTION_Y = 192
MOTION_FRAME = 13


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def set_if_available(owner, name, value):
    if hasattr(owner, name):
        setattr(owner, name, value)


def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--scenario", choices=("accumulation", "depth_of_field", "motion_blur"), required=True
    )
    parser.add_argument("--output-dir", type=Path, required=True)
    arguments = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    return parser.parse_args(arguments)


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()

    scene = bpy.context.scene
    view_layer = bpy.context.view_layer
    while len(view_layer.native_postfx_outputs):
        view_layer.native_postfx_outputs.remove(0)
    scene.compositing_node_group = None


def configure_scene(samples=1):
    clear_scene()
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = RESOLUTION_X
    scene.render.resolution_y = RESOLUTION_Y
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = False
    scene.render.use_compositing = True
    scene.render.use_motion_blur = False
    scene.render.motion_blur_shutter = 0.8
    scene.eevee.taa_render_samples = samples
    scene.eevee.taa_samples = samples
    set_if_available(scene.eevee, "use_taa_reprojection", False)
    set_if_available(scene.eevee, "use_raytracing", False)
    set_if_available(scene.eevee, "use_outline", False)
    set_if_available(scene.eevee, "motion_blur_steps", 1)
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0

    world = bpy.data.worlds.new("Native Camera FX World")
    world.color = (0.0, 0.0, 0.0)
    scene.world = world
    return scene, bpy.context.view_layer


def add_native_output(view_layer, name, source, motion_blur=False, depth_of_field=False):
    output = view_layer.native_postfx_outputs.add()
    output.name = name
    output.enabled = True
    output.source = source
    output.use_motion_blur = motion_blur
    output.use_depth_of_field = depth_of_field
    require(output.is_valid, f"Native Camera FX output {name!r} is invalid")
    return output


def make_material(name, color, emission=False):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    if emission:
        shader = nodes.new("ShaderNodeEmission")
        shader.inputs["Color"].default_value = color
        shader.inputs["Strength"].default_value = 1.0
        links.new(shader.outputs["Emission"], output.inputs["Surface"])
    else:
        shader = nodes.new("ShaderNodeBsdfPrincipled")
        shader.inputs["Base Color"].default_value = color
        shader.inputs["Roughness"].default_value = 0.5
        links.new(shader.outputs["BSDF"], output.inputs["Surface"])
    return material


def assign_material(obj, material):
    obj.data.materials.clear()
    obj.data.materials.append(material)
    obj.active_material_index = 0
    for polygon in obj.data.polygons:
        polygon.material_index = 0


def add_camera(scene, location, camera_type="PERSP", ortho_scale=4.0):
    bpy.ops.object.camera_add(location=location)
    camera = bpy.context.object
    camera.name = "Native Camera FX Camera"
    camera.data.type = camera_type
    camera.data.ortho_scale = ortho_scale
    camera.data.lens = 50.0
    scene.camera = camera
    return camera


def set_linear_location_animation(obj, start, end):
    obj.location = start
    obj.keyframe_insert(data_path="location", frame=1)
    obj.location = end
    obj.keyframe_insert(data_path="location", frame=25)

    action = obj.animation_data.action
    fcurves = getattr(action, "fcurves", None)
    if fcurves is None and hasattr(action, "layers"):
        fcurves = []
        for layer in action.layers:
            for strip in layer.strips:
                for channelbag in strip.channelbags:
                    fcurves.extend(channelbag.fcurves)
    for fcurve in fcurves or []:
        for keyframe in fcurve.keyframe_points:
            keyframe.interpolation = "LINEAR"


def build_file_output(scene, output_dir, scenario, pass_specs):
    output_dir.mkdir(parents=True, exist_ok=True)
    prefix = f"native_camera_fx_{scenario}"
    for path in output_dir.glob(prefix + "*"):
        path.unlink()

    tree = bpy.data.node_groups.new(f"Native Camera FX {scenario}", "CompositorNodeTree")
    scene.compositing_node_group = tree
    render_layers = tree.nodes.new("CompositorNodeRLayers")
    render_layers.layer = bpy.context.view_layer.name
    file_output = tree.nodes.new("CompositorNodeOutputFile")
    file_output.directory = str(output_dir)
    file_output.file_name = prefix
    file_output.format.file_format = "OPEN_EXR_MULTILAYER"
    file_output.file_output_items.clear()

    for socket_type, pass_name in pass_specs:
        require(
            pass_name in render_layers.outputs,
            f"Render Layers output {pass_name!r} is missing",
        )
        file_output.file_output_items.new(socket_type, pass_name)
        tree.links.new(render_layers.outputs[pass_name], file_output.inputs[pass_name])

    return prefix


def render_passes(scene, output_dir, scenario, pass_specs):
    prefix = build_file_output(scene, output_dir, scenario, pass_specs)
    bpy.ops.render.render()
    paths = sorted(output_dir.glob(prefix + "*.exr"))
    require(paths, f"Compositor did not write {prefix} EXR")
    return read_multilayer_exr(paths[-1]), paths[-1]


def read_multilayer_exr(path):
    image_input = oiio.ImageInput.open(str(path))
    require(image_input is not None, f"Could not open {path}")
    passes = {}
    try:
        subimage = 0
        while image_input.seek_subimage(subimage, 0):
            spec = image_input.spec()
            name = spec.get_string_attribute("oiio:subimagename")
            pixels = image_input.read_image(format=oiio.FLOAT)
            require(pixels is not None, f"Could not read {name!r} from {path}")
            passes[name] = pixels
            subimage += 1
    finally:
        image_input.close()
    return passes


def pass_pixels(passes, name):
    require(name in passes, f"EXR pass {name!r} is missing; found {sorted(passes)}")
    return passes[name]


def roi(pixels, bounds):
    height, width = pixels.shape[:2]
    x0, y0, x1, y1 = bounds
    return pixels[int(height * y0) : int(height * y1), int(width * x0) : int(width * x1)]


def scalar_stats(pixels):
    values = abs(pixels.reshape(-1))
    return {
        "max": float(values.max()),
        "mean": float(values.mean()),
        "nonzero_ratio": float((values > 1.0e-8).sum() / values.size),
    }


def difference_stats(first, second, bounds):
    first_roi = roi(first, bounds)
    second_roi = roi(second, bounds)
    channels = min(first_roi.shape[2], second_roi.shape[2], 3)
    difference = abs(first_roi[:, :, :channels] - second_roi[:, :, :channels]).max(axis=2)
    return {
        "mean": float(difference.mean()),
        "max": float(difference.max()),
        "ratio_gt3": float((difference > (3.0 / 255.0)).sum() / difference.size),
    }


def depth_difference_stats(first, second, bounds):
    first_roi = roi(first, bounds)
    second_roi = roi(second, bounds)
    difference = abs(first_roi[:, :, 0] - second_roi[:, :, 0])
    return {
        "mean": float(difference.mean()),
        "max": float(difference.max()),
        "ratio_gt1e5": float((difference > 1.0e-5).sum() / difference.size),
    }


def run_accumulation(output_dir):
    scene, view_layer = configure_scene(samples=1)
    view_layer.use_pass_z = True
    view_layer.use_pass_normal = True
    add_native_output(view_layer, "NativeDepth", "DEPTH")
    add_native_output(view_layer, "NativeNormal", "NORMAL")

    camera = add_camera(scene, (0.0, 0.0, 4.0), camera_type="ORTHO", ortho_scale=3.0)
    camera.rotation_euler = (0.0, 0.0, 0.0)
    bpy.ops.mesh.primitive_plane_add(size=2.0, location=(0.0, 0.0, 0.0), rotation=(0.25, 0.2, 0.0))
    plane = bpy.context.object
    assign_material(plane, make_material("Accumulation Material", (0.7, 0.2, 0.1, 1.0)))

    passes, path = render_passes(
        scene,
        output_dir,
        "accumulation",
        (
            ("FLOAT", "Depth"),
            ("FLOAT", "NativeDepth"),
            ("VECTOR", "Normal"),
            ("VECTOR", "NativeNormal"),
        ),
    )
    native_depth = pass_pixels(passes, "NativeDepth")
    raw_normal = pass_pixels(passes, "Normal")
    native_normal = pass_pixels(passes, "NativeNormal")

    depth_center = scalar_stats(roi(native_depth, (0.35, 0.35, 0.65, 0.65)))
    depth_corner = scalar_stats(roi(native_depth, (0.0, 0.0, 0.12, 0.12)))
    normal_center = scalar_stats(roi(native_normal, (0.35, 0.35, 0.65, 0.65)))
    normal_corner = scalar_stats(roi(native_normal, (0.0, 0.0, 0.12, 0.12)))
    normal_delta = difference_stats(raw_normal, native_normal, (0.35, 0.35, 0.65, 0.65))

    require(depth_center["max"] > 1.0e-4, f"Native depth stayed black: {depth_center}")
    require(
        depth_center["nonzero_ratio"] > 0.95,
        f"Native depth surface was incomplete: {depth_center}",
    )
    require(depth_corner["max"] < 1.0e-6, f"Native depth background was not zero: {depth_corner}")
    require(normal_center["mean"] > 0.15, f"Native normal stayed black: {normal_center}")
    require(
        normal_corner["max"] < 1.0e-5,
        f"Native normal background was not zero: {normal_corner}",
    )
    require(normal_delta["mean"] < 0.02, f"Native normal diverged from Normal pass: {normal_delta}")

    return {
        "output": str(path),
        "depth_center": depth_center,
        "depth_corner": depth_corner,
        "normal_center": normal_center,
        "normal_corner": normal_corner,
        "normal_delta": normal_delta,
    }


def run_depth_of_field(output_dir):
    scene, view_layer = configure_scene(samples=16)
    add_native_output(view_layer, "DepthNoFX", "DEPTH")
    add_native_output(view_layer, "DepthDOF", "DEPTH", depth_of_field=True)

    camera = add_camera(scene, (0.0, 0.0, 8.0))
    camera.rotation_euler = (0.0, 0.0, 0.0)
    camera.data.dof.use_dof = True
    camera.data.dof.aperture_fstop = 0.25

    focus_material = make_material("Focused Surface", (1.0, 0.05, 0.02, 1.0))
    defocus_material = make_material("Defocused Surface", (0.02, 1.0, 0.05, 1.0))

    bpy.ops.mesh.primitive_uv_sphere_add(
        segments=48, ring_count=24, radius=1.0, location=(-1.45, 0.0, 0.0)
    )
    focus_object = bpy.context.object
    assign_material(focus_object, focus_material)
    camera.data.dof.focus_object = focus_object

    bpy.ops.mesh.primitive_uv_sphere_add(
        segments=48, ring_count=24, radius=1.6, location=(1.55, 0.0, -4.0)
    )
    defocus_object = bpy.context.object
    assign_material(defocus_object, defocus_material)

    passes, path = render_passes(
        scene,
        output_dir,
        "depth_of_field",
        (("FLOAT", "DepthNoFX"), ("FLOAT", "DepthDOF")),
    )
    no_fx = pass_pixels(passes, "DepthNoFX")
    dof = pass_pixels(passes, "DepthDOF")
    no_fx_stats = scalar_stats(no_fx)
    dof_stats = scalar_stats(dof)
    focus_delta = depth_difference_stats(no_fx, dof, (0.05, 0.15, 0.48, 0.85))
    defocus_delta = depth_difference_stats(no_fx, dof, (0.50, 0.10, 0.95, 0.90))

    require(no_fx_stats["max"] > 1.0e-4, f"DepthNoFX stayed black: {no_fx_stats}")
    require(dof_stats["max"] > 1.0e-4, f"DepthDOF stayed black: {dof_stats}")
    require(
        defocus_delta["mean"] > 1.0e-5,
        f"DOF did not change the defocused object: {defocus_delta}",
    )
    require(
        defocus_delta["ratio_gt1e5"] > 0.03,
        f"DOF footprint was too small: {defocus_delta}",
    )
    require(
        defocus_delta["mean"] > focus_delta["mean"] * 1.5,
        f"DOF did not preserve the focused object: focus={focus_delta}, defocus={defocus_delta}",
    )

    return {
        "output": str(path),
        "no_fx": no_fx_stats,
        "dof": dof_stats,
        "focus_delta": focus_delta,
        "defocus_delta": defocus_delta,
    }


def run_motion_blur(output_dir):
    scene, view_layer = configure_scene(samples=32)
    scene.render.use_motion_blur = True
    add_native_output(view_layer, "EmissionNoFX", "EMISSION")
    add_native_output(view_layer, "EmissionMotion", "EMISSION", motion_blur=True)

    camera = add_camera(scene, (0.0, 0.0, 8.0), camera_type="ORTHO", ortho_scale=4.0)
    camera.rotation_euler = (0.0, 0.0, 0.0)
    emission = make_material("Motion Emission", (0.05, 0.35, 1.0, 1.0), emission=True)

    bpy.ops.mesh.primitive_cube_add(size=1.2, location=(-2.0, 0.0, 0.0))
    static_cube = bpy.context.object
    assign_material(static_cube, emission)

    bpy.ops.mesh.primitive_uv_sphere_add(
        segments=48, ring_count=24, radius=0.75, location=(-2.0, 0.0, 0.0)
    )
    moving_sphere = bpy.context.object
    assign_material(moving_sphere, emission)
    set_linear_location_animation(moving_sphere, (-2.0, 0.0, 0.0), (2.0, 0.0, 0.0))
    scene.frame_set(MOTION_FRAME)

    scene.render.use_motion_blur = False
    bpy.ops.render.render()
    scene.render.use_motion_blur = True

    passes, path = render_passes(
        scene,
        output_dir,
        "motion_blur",
        (("RGBA", "EmissionNoFX"), ("RGBA", "EmissionMotion")),
    )
    no_fx = pass_pixels(passes, "EmissionNoFX")
    motion = pass_pixels(passes, "EmissionMotion")
    static_delta = difference_stats(no_fx, motion, (0.11, 0.47, 0.14, 0.53))
    moving_delta = difference_stats(no_fx, motion, (0.35, 0.15, 0.70, 0.85))
    background_delta = difference_stats(no_fx, motion, (0.88, 0.0, 1.0, 1.0))
    print(
        "NATIVE_CAMERA_FX_MOTION_METRICS="
        + json.dumps(
            {
                "static": static_delta,
                "moving": moving_delta,
                "background": background_delta,
            }
        ),
        flush=True,
    )

    require(moving_delta["mean"] > 0.01, f"Motion blur was too weak: {moving_delta}")
    require(
        moving_delta["ratio_gt3"] > 0.10,
        f"Motion blur footprint was too small: {moving_delta}",
    )
    require(
        static_delta["mean"] < 0.003 and static_delta["ratio_gt3"] < 0.02,
        f"Motion blur changed the static object: {static_delta}",
    )
    require(
        background_delta["mean"] < 0.003 and background_delta["ratio_gt3"] < 0.02,
        f"Motion blur changed the background: {background_delta}",
    )

    return {
        "output": str(path),
        "static_delta": static_delta,
        "moving_delta": moving_delta,
        "background_delta": background_delta,
    }


def main():
    arguments = parse_arguments()
    output_dir = arguments.output_dir.resolve()
    runners = {
        "accumulation": run_accumulation,
        "depth_of_field": run_depth_of_field,
        "motion_blur": run_motion_blur,
    }
    result = runners[arguments.scenario](output_dir)
    summary_path = output_dir / f"{arguments.scenario}_summary.json"
    summary = {
        "status": "PASS",
        "backend": gpu.platform.backend_type_get(),
        "scenario": arguments.scenario,
        "result": result,
    }
    summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print("NATIVE_CAMERA_FX_BACKEND=" + summary["backend"], flush=True)
    print("NATIVE_CAMERA_FX_SCENARIO=" + arguments.scenario, flush=True)
    print("NATIVE_CAMERA_FX_SUMMARY=" + str(summary_path), flush=True)
    print("NATIVE_CAMERA_FX_OK", flush=True)


if __name__ == "__main__":
    main()
