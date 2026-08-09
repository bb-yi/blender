import json
import re
import subprocess
import textwrap
from pathlib import Path

import bpy


CASE_DIR = Path(__file__).resolve().parent
ROOT = CASE_DIR.parents[3]
OUT_DIR = CASE_DIR / "out"
CHILD_SCRIPT = OUT_DIR / "outline_routing_child.py"
SUMMARY_PATH = OUT_DIR / "summary.json"

BACKEND_MARKER = "OUTLINE_RENDER_BACKEND="
RENDER_MARKER = "OUTLINE_RENDER_PARITY_OK"
VIEWPORT_MARKER = "OUTLINE_VIEWPORT_PARITY_OK"

ERROR_PATTERNS = (
    ("missing GPU binding", re.compile(r"ERROR Missing .* bind at slot", re.IGNORECASE)),
    ("GPU validation failure", re.compile(r"Validation failed", re.IGNORECASE)),
    ("Vulkan device lost", re.compile(r"VK_ERROR_DEVICE_LOST", re.IGNORECASE)),
    ("native access violation", re.compile(r"EXCEPTION_ACCESS_VIOLATION", re.IGNORECASE)),
    ("Blender crash", re.compile(r"Blender crashed", re.IGNORECASE)),
)


CHILD_SOURCE = r'''
import json
import os
import sys
from pathlib import Path

import bpy
import gpu
import OpenImageIO as oiio


CASE_DIR = Path(__file__).resolve().parent.parent
OUT_DIR = CASE_DIR / "out"
BACKEND_MARKER = "OUTLINE_RENDER_BACKEND="
RENDER_MARKER = "OUTLINE_RENDER_PARITY_OK"
VIEWPORT_MARKER = "OUTLINE_VIEWPORT_PARITY_OK"


def argument_value(name):
    if "--" not in sys.argv:
        raise AssertionError("child arguments are missing")
    arguments = sys.argv[sys.argv.index("--") + 1 :]
    index = arguments.index(name)
    return arguments[index + 1]


BACKEND = argument_value("--backend").upper()


def set_if_available(owner, name, value):
    if hasattr(owner, name):
        setattr(owner, name, value)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def cleanup_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)


def make_outline_material(name, render_method):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    material.surface_render_method = render_method
    if render_method == "BLENDED":
        set_if_available(material, "use_transparency_overlap", False)

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (0.04, 0.16, 0.8, 1.0)
    emission.inputs["Strength"].default_value = 1.0
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    outline = nodes.new("ShaderNodeOutlineControl")
    outline.inputs["Line Color"].default_value = (1.0, 0.0, 0.0, 1.0)
    outline.inputs["Line Alpha"].default_value = 1.0
    outline.inputs["Line Width"].default_value = 8.0
    outline.inputs["Depth Threshold"].default_value = 0.1
    outline.inputs["Normal Threshold"].default_value = 0.5
    outline.inputs["Outline ID"].default_value = 1
    return material


def build_scene(render_method):
    cleanup_scene()
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 160
    scene.render.resolution_y = 160
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.film_transparent = False
    scene.render.use_compositing = True
    scene.eevee.use_outline = True
    set_if_available(scene.eevee, "taa_samples", 1)
    set_if_available(scene.eevee, "taa_render_samples", 1)
    set_if_available(scene.eevee, "use_taa_reprojection", False)
    set_if_available(scene.eevee, "use_raytracing", False)
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0

    world = scene.world
    world.use_nodes = True
    background = world.node_tree.nodes.get("Background")
    background.inputs["Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    background.inputs["Strength"].default_value = 0.0

    bpy.ops.object.camera_add(location=(0.0, 0.0, 5.0))
    camera = bpy.context.object
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = 3.6
    scene.camera = camera

    bpy.ops.mesh.primitive_uv_sphere_add(
        segments=32,
        ring_count=16,
        radius=1.1,
        location=(0.0, 0.0, 0.0),
    )
    sphere = bpy.context.object
    sphere.name = "Outline Routing Sphere"
    for polygon in sphere.data.polygons:
        polygon.use_smooth = True
    sphere.data.materials.append(make_outline_material("Outline " + render_method, render_method))

    view_layer = bpy.context.view_layer
    view_layer.eevee.use_pass_outline = False


def setup_compositor(prefix, public_pass):
    scene = bpy.context.scene
    scene.use_nodes = True
    tree = bpy.data.node_groups.new("Outline Routing Compositor " + prefix, "CompositorNodeTree")
    scene.compositing_node_group = tree
    tree.nodes.clear()

    render_layers = tree.nodes.new("CompositorNodeRLayers")
    combined_socket = render_layers.outputs.get("Image")
    if combined_socket is None:
        combined_socket = render_layers.outputs.get("Combined")
    require(combined_socket is not None, "Compositor Combined/Image output is missing")
    outline_socket = render_layers.outputs.get("Outline")
    if public_pass:
        require(outline_socket is not None, "Compositor Outline output is missing")

    combined_output = tree.nodes.new("CompositorNodeOutputFile")
    combined_output.directory = str(OUT_DIR)
    combined_output.file_name = prefix + "_combined"
    combined_output.format.color_depth = "32"
    combined_output.file_output_items.clear()
    combined_output.file_output_items.new("RGBA", "Combined")
    tree.links.new(combined_socket, combined_output.inputs["Combined"])

    if outline_socket is not None:
        outline_output = tree.nodes.new("CompositorNodeOutputFile")
        outline_output.directory = str(OUT_DIR)
        outline_output.file_name = prefix + "_outline"
        outline_output.format.color_depth = "32"
        outline_output.file_output_items.clear()
        outline_output.file_output_items.new("RGBA", "Outline")
        tree.links.new(outline_socket, outline_output.inputs["Outline"])

    for old_path in OUT_DIR.glob(prefix + "_*.exr"):
        old_path.unlink()


def find_output(prefix, kind):
    paths = sorted(OUT_DIR.glob(prefix + "_" + kind + "*.exr"))
    require(paths, f"Compositor did not write {kind} output for {prefix}")
    return paths[-1]


def read_pixels(path):
    image_input = oiio.ImageInput.open(str(path))
    require(image_input is not None, f"Could not open EXR output: {path}")
    try:
        spec = image_input.spec()
        pixels = image_input.read_image(format=oiio.FLOAT)
        require(pixels is not None, f"Could not read EXR output: {path}")
        return pixels, int(spec.width), int(spec.height)
    finally:
        image_input.close()


def red_signal(pixels):
    flat = pixels.reshape((-1, pixels.shape[-1]))
    count = 0
    for pixel in flat:
        r, g, b = (float(pixel[0]), float(pixel[1]), float(pixel[2]))
        if r > 0.35 and r > g * 1.5 + 0.05 and r > b * 1.5 + 0.05:
            count += 1
    return count


def render_final(render_method, public_pass):
    build_scene(render_method)
    prefix = f"{BACKEND.lower()}_{render_method.lower()}_{'public' if public_pass else 'combined'}"
    bpy.context.view_layer.eevee.use_pass_outline = public_pass
    setup_compositor(prefix, public_pass)
    bpy.ops.render.render()

    combined_path = find_output(prefix, "combined")
    combined_pixels, width, height = read_pixels(combined_path)
    outline_path = None
    outline_red = 0
    if public_pass:
        outline_path = find_output(prefix, "outline")
        outline_pixels, outline_width, outline_height = read_pixels(outline_path)
        require((width, height) == (outline_width, outline_height), "Combined and Outline sizes differ")
        outline_red = red_signal(outline_pixels)

    result = {
        "backend": BACKEND,
        "render_method": render_method,
        "public_pass": public_pass,
        "combined_red": red_signal(combined_pixels),
        "outline_red": outline_red,
        "width": width,
        "height": height,
        "combined_path": str(combined_path),
        "outline_path": str(outline_path),
    }
    if public_pass:
        require(result["outline_red"] > 10, f"Outline pass is empty: {result}")
        require(
            result["combined_red"] <= 2,
            f"Outline pass leaked into Combined: {result}",
        )
    else:
        require(result["combined_red"] > 10, f"Combined outline is missing: {result}")
    return result


def find_view3d_context():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type != "VIEW_3D":
                continue
            for region in area.regions:
                if region.type == "WINDOW":
                    return window, window.screen, area, region, area.spaces.active
    raise AssertionError("No VIEW_3D window region found")


def viewport_red(public_pass):
    scene = bpy.context.scene
    scene.use_nodes = False
    scene.compositing_node_group = None
    scene.render.use_compositing = False
    bpy.context.view_layer.eevee.use_pass_outline = public_pass
    bpy.context.view_layer.update()
    window, screen, area, region, space = find_view3d_context()
    space.shading.type = "RENDERED"
    set_if_available(space.shading, "use_scene_world_render", True)
    set_if_available(space.shading, "use_scene_lights_render", True)
    set_if_available(space.overlay, "show_overlays", False)
    set_if_available(space.shading, "show_object_outline", False)
    area.tag_redraw()

    with bpy.context.temp_override(
        window=window, screen=screen, area=area, region=region, space_data=space
    ):
        if space.region_3d.view_perspective != "CAMERA":
            bpy.ops.view3d.view_camera()
        bpy.ops.wm.redraw_timer(type="DRAW_WIN_SWAP", iterations=16)
        bpy.ops.render.opengl(write_still=False, view_context=True)

    image = bpy.data.images.get("Render Result")
    require(image is not None, "Rendered Viewport did not produce Render Result")
    viewport_path = OUT_DIR / (
        f"{BACKEND.lower()}_viewport_{'public' if public_pass else 'combined'}.png"
    )
    image.save_render(str(viewport_path))
    captured = bpy.data.images.load(str(viewport_path), check_existing=False)
    try:
        pixels = list(captured.pixels[:])
        flat = [pixels[index:index + 4] for index in range(0, len(pixels), 4)]
        count = 0
        for pixel in flat:
            r, g, b = (float(pixel[0]), float(pixel[1]), float(pixel[2]))
            if r > 0.35 and r > g * 1.5 + 0.05 and r > b * 1.5 + 0.05:
                count += 1
    finally:
        bpy.data.images.remove(captured)
    print(
        f"OUTLINE_VIEWPORT_CAPTURE public={int(public_pass)} count={count} path={viewport_path}",
        flush=True,
    )
    return count


def run_viewport_probe():
    build_scene("DITHERED")
    enabled = viewport_red(True)
    disabled = viewport_red(False)
    require(enabled <= 2, f"Viewport outline leaked into Combined: enabled={enabled}")
    require(disabled > 10, f"Viewport Combined outline is missing: disabled={disabled}")
    print(f"OUTLINE_VIEWPORT_COUNTS enabled={enabled} disabled={disabled}", flush=True)


def main():
    if BACKEND == "VULKAN":
        require(
            gpu.platform.backend_type_get() == BACKEND,
            f"Requested {BACKEND}, got {gpu.platform.backend_type_get()}",
        )
    print(BACKEND_MARKER + BACKEND, flush=True)
    results = []
    for render_method in ("DITHERED", "BLENDED"):
        results.append(render_final(render_method, True))
        results.append(render_final(render_method, False))

    if BACKEND == "VULKAN":
        run_viewport_probe()
        print(VIEWPORT_MARKER, flush=True)

    summary = {
        "status": "PASS",
        "backend": BACKEND,
        "final_render": results,
    }
    if BACKEND == "VULKAN":
        summary["viewport"] = "PASS"
    (OUT_DIR / f"summary_{BACKEND.lower()}.json").write_text(
        json.dumps(summary, indent=2), encoding="utf-8"
    )
    print(RENDER_MARKER, flush=True)
    bpy.ops.wm.quit_blender()
    os._exit(0)


if __name__ == "__main__":
    main()
'''


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def tail_text(text, line_count=120):
    return "\n".join(text.splitlines()[-line_count:])


def write_child_script():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    CHILD_SCRIPT.write_text(textwrap.dedent(CHILD_SOURCE).strip() + "\n", encoding="utf-8")


def run_child(backend):
    command = [bpy.app.binary_path]
    if backend == "OPENGL":
        command.append("--background")
    command.extend(
        [
            "--factory-startup",
            "--gpu-backend",
            backend.lower(),
            "--python-exit-code",
            "1",
            "--python",
            str(CHILD_SCRIPT),
            "--",
            "--backend",
            backend,
        ]
    )
    if backend == "VULKAN":
        command.insert(4, "--debug-gpu")
    label = f"child_{backend.lower()}"
    log_path = OUT_DIR / f"{label}.log"
    process = subprocess.Popen(
        command,
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    command_text = subprocess.list2cmdline([str(value) for value in command])
    launch_record = (
        f"child_label={label}\n"
        f"child_pid={process.pid}\n"
        f"child_executable={Path(command[0]).resolve()}\n"
        f"child_command={command_text}\n"
    )
    try:
        output, _unused = process.communicate(timeout=180)
    except subprocess.TimeoutExpired:
        process.kill()
        output, _unused = process.communicate()
        log_path.write_text(
            launch_record + f"TIMEOUT after 180s; killed exact child PID {process.pid}\n" + output,
            encoding="utf-8",
        )
        raise AssertionError(
            f"{label} timed out; exact child PID {process.pid} was terminated\n"
            f"--- child tail ---\n{tail_text(output)}"
        )

    log_path.write_text(launch_record + output, encoding="utf-8")
    require(process.returncode == 0, f"{label} exited with {process.returncode}\n{tail_text(output)}")
    require(BACKEND_MARKER + backend in output, f"{label} did not confirm backend\n{tail_text(output)}")
    require(RENDER_MARKER in output, f"{label} did not complete final render parity\n{tail_text(output)}")
    if backend == "VULKAN":
        require(VIEWPORT_MARKER in output, f"{label} did not complete viewport parity\n{tail_text(output)}")

    errors = []
    for line in output.splitlines():
        for label_text, pattern in ERROR_PATTERNS:
            if pattern.search(line):
                errors.append(f"{label_text}: {line.strip()}")
                break
    require(not errors, "GPU/native errors reported:\n" + "\n".join(errors))
    print(f"{label}=PASS log={log_path}", flush=True)
    return {"backend": backend, "log": str(log_path)}


def main():
    write_child_script()
    children = [run_child("OPENGL"), run_child("VULKAN")]
    SUMMARY_PATH.write_text(
        json.dumps({"status": "PASS", "children": children}, indent=2), encoding="utf-8"
    )
    print(f"EEVEE_OUTLINE_ROUTING_FORWARD_PARITY_SUMMARY={SUMMARY_PATH}", flush=True)
    print("EEVEE_OUTLINE_ROUTING_FORWARD_PARITY_OK", flush=True)


if __name__ == "__main__":
    main()
