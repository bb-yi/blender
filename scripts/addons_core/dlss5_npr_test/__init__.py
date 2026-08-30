# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

bl_info = {
    "name": "DLSS5 NPR Test",
    "author": "Blender NPR",
    "version": (0, 1, 1),
    "blender": (5, 2, 0),
    "location": "Properties > Render",
    "description": "Run an offline DLSS-NR comparison from the active EEVEE scene",
    "support": "TESTING",
    "category": "Render",
}

import os
import re
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

import bpy
from bpy.props import (
    BoolProperty,
    EnumProperty,
    FloatProperty,
    IntProperty,
    PointerProperty,
    StringProperty,
)
from bpy.types import Operator, Panel, PropertyGroup


MODULE_NAME = __name__
EEVEE_ENGINES = {"BLENDER_EEVEE", "BLENDER_EEVEE_NEXT"}
HOST_RELATIVE = Path("temp") / "build" / "dlss5_host_mainfix" / "Release" / "dlss5_host.exe"
RUNTIME_RELATIVE = Path("temp") / "dlss5_runtime"
CONVERTER_RELATIVE = Path("temp") / "scripts" / "convert_dlss5_pass_exr.py"
COMPARE_RELATIVE = Path("temp") / "scripts" / "dlss5_compare.py"


def _workspace_root():
    binary_path = Path(bpy.app.binary_path).resolve()
    candidates = [binary_path.parent, binary_path.parent.parent]
    candidates.extend(binary_path.parents)
    for candidate in candidates:
        if (candidate / HOST_RELATIVE).is_file():
            return candidate
    return binary_path.parent


def _find_python():
    configured = shutil.which("python.exe") or shutil.which("python")
    if configured:
        return Path(configured)
    return Path(sys.executable)


def _default_core_dll():
    configured = os.environ.get("DLSS5_CORE_DLL", "")
    if configured and Path(configured).is_file():
        return configured
    driver_root = Path(r"C:\Windows\System32\DriverStore\FileRepository")
    if driver_root.is_dir():
        matches = sorted(driver_root.glob("nv_dispsi.inf_*\\nvngx.dll"))
        if matches:
            return str(matches[-1])
    return ""


def _clean_name(value):
    value = re.sub(r"[^0-9A-Za-z_.-]+", "_", value)
    return value.strip("._") or "scene"


def _run_process(executable, arguments, log_path, env=None):
    log_path.parent.mkdir(parents=True, exist_ok=True)
    process_env = os.environ.copy()
    if env:
        process_env.update(env)
    path_entries = [process_env.get("PATH", "")]
    if "--runtime-dir" in arguments:
        path_entries.insert(0, str(Path(arguments[arguments.index("--runtime-dir") + 1])))
    process_env["PATH"] = os.pathsep.join(entry for entry in path_entries if entry)
    with log_path.open("w", encoding="utf-8", errors="replace") as log:
        result = subprocess.run(
            [str(executable), *[str(argument) for argument in arguments]],
            cwd=str(_workspace_root()),
            stdout=log,
            stderr=subprocess.STDOUT,
            env=process_env,
            check=False,
        )
    if result.returncode != 0:
        raise RuntimeError(f"{Path(executable).name} failed with exit code {result.returncode}")


def _parse_metrics(log_path):
    text = log_path.read_text(encoding="utf-8", errors="replace")
    metrics = []
    for line in text.splitlines():
        if "same_resolution_vs_lanczos:" in line or "input_vs_lanczos:" in line:
            metrics.append(line.strip())
    return "\n".join(metrics)


def _configure_pass_outputs(scene, directory):
    scene.view_layers[0].use_pass_z = True
    scene.view_layers[0].use_pass_vector = True
    scene.use_nodes = True
    node_tree = bpy.data.node_groups.new("DLSS5TemporaryCompositor", "CompositorNodeTree")
    scene.compositing_node_group = node_tree
    nodes = node_tree.nodes
    links = node_tree.links
    layers = nodes.new("CompositorNodeRLayers")

    depth_output = nodes.new("CompositorNodeOutputFile")
    depth_output.directory = str(directory / "passes" / "input")
    depth_output.file_name = "depth"
    depth_output.format.media_type = "MULTI_LAYER_IMAGE"
    depth_output.format.file_format = "OPEN_EXR_MULTILAYER"
    depth_output.format.color_mode = "RGBA"
    depth_output.file_output_items.new("FLOAT", "Depth")
    links.new(layers.outputs["Depth"], depth_output.inputs["Depth"])

    vector_output = nodes.new("CompositorNodeOutputFile")
    vector_output.directory = str(directory / "passes" / "input")
    vector_output.file_name = "vector"
    vector_output.format.media_type = "MULTI_LAYER_IMAGE"
    vector_output.format.file_format = "OPEN_EXR_MULTILAYER"
    vector_output.format.color_mode = "RGBA"
    vector_output.file_output_items.new("VECTOR", "Vector")
    links.new(layers.outputs["Vector"], vector_output.inputs["Vector"])
    return node_tree, depth_output, vector_output


def _render_pass(scene, width, height, image_path, pass_directory, node_outputs):
    depth_output, vector_output = node_outputs
    scene.render.resolution_x = width
    scene.render.resolution_y = height
    scene.render.resolution_percentage = 100
    scene.render.image_settings.media_type = "IMAGE"
    scene.render.image_settings.file_format = "BMP"
    scene.render.image_settings.color_mode = "RGB"
    scene.render.filepath = str(image_path)
    pass_directory.mkdir(parents=True, exist_ok=True)
    depth_output.directory = str(pass_directory)
    vector_output.directory = str(pass_directory)
    depth_output.file_name = "depth"
    vector_output.file_name = "vector"
    bpy.ops.render.render(write_still=True)


def _load_image(path, name):
    if not path.is_file():
        raise RuntimeError(f"Image output not found: {path}")
    image = bpy.data.images.load(str(path), check_existing=False)
    image.name = name
    if tuple(image.size) == (0, 0) or not image.has_data:
        raise RuntimeError(f"Blender could not load image data: {path}")
    return image


class DLSS5NPRTestSettings(PropertyGroup):
    host_executable: StringProperty(
        name="Host",
        subtype="FILE_PATH",
        default="",
    )
    runtime_directory: StringProperty(
        name="Runtime",
        subtype="DIR_PATH",
        default="",
    )
    python_executable: StringProperty(
        name="Python",
        subtype="FILE_PATH",
        default="",
    )
    core_dll: StringProperty(
        name="NGX Core",
        subtype="FILE_PATH",
        default="",
    )
    output_directory: StringProperty(
        name="Output",
        subtype="DIR_PATH",
        default="",
    )
    input_scale: FloatProperty(
        name="Input Scale",
        description="Render the DLSS-NR input at this percentage of the scene resolution",
        min=0.25,
        max=1.0,
        default=0.5,
        step=10,
        precision=2,
    )
    frame: IntProperty(
        name="Frame",
        description="Frame to render, or zero for the current frame",
        min=0,
        default=0,
    )
    scene_name: StringProperty(name="Scene", default="")
    output_encoding: EnumProperty(
        name="Output Encoding",
        items=(
            ("RAW", "Preserve Input", "Keep the rendered BMP display encoding unchanged"),
            ("SRGB", "Encode sRGB", "Apply sRGB encoding to linear DLSS output"),
        ),
        default="RAW",
    )
    keep_compositor: BoolProperty(
        name="Keep Compositor",
        description="Keep the temporary pass-output compositor after the test",
        default=False,
    )
    last_output: StringProperty(name="Last Output", default="")
    last_metrics: StringProperty(name="Last Metrics", default="")
    last_status: StringProperty(name="Status", default="Not run")
    result_image: PointerProperty(type=bpy.types.Image)
    input_image: PointerProperty(type=bpy.types.Image)
    reference_image: PointerProperty(type=bpy.types.Image)


def _ensure_defaults(settings):
    root = _workspace_root()
    if not settings.host_executable:
        settings.host_executable = str(root / HOST_RELATIVE)
    if not settings.runtime_directory:
        settings.runtime_directory = str(root / RUNTIME_RELATIVE)
    if not settings.python_executable:
        settings.python_executable = str(_find_python())
    if not settings.core_dll:
        settings.core_dll = _default_core_dll()
    if not settings.output_directory:
        settings.output_directory = str(root / "temp" / "render_exports" / "dlss5" / "blender_ui")


class DLSS5NPR_OT_run_test(Operator):
    bl_idname = "dlss5_npr.run_test"
    bl_label = "Run DLSSNR Test"
    bl_options = {"REGISTER"}

    @classmethod
    def poll(cls, context):
        return bool(context.scene and context.scene.render.engine in EEVEE_ENGINES)

    def execute(self, context):
        scene = context.scene
        settings = scene.dlss5_npr_test
        _ensure_defaults(settings)

        host = Path(bpy.path.abspath(settings.host_executable))
        runtime = Path(bpy.path.abspath(settings.runtime_directory))
        python = Path(bpy.path.abspath(settings.python_executable))
        converter = _workspace_root() / CONVERTER_RELATIVE
        compare = _workspace_root() / COMPARE_RELATIVE
        if not host.is_file():
            self.report({"ERROR"}, f"DLSS5 host not found: {host}")
            return {"CANCELLED"}
        if not runtime.is_dir():
            self.report({"ERROR"}, f"DLSS5 runtime directory not found: {runtime}")
            return {"CANCELLED"}
        if not python.is_file():
            self.report({"ERROR"}, f"Python executable not found: {python}")
            return {"CANCELLED"}
        if not converter.is_file() or not compare.is_file():
            self.report({"ERROR"}, "DLSS5 helper scripts are missing")
            return {"CANCELLED"}

        base_output = Path(bpy.path.abspath(settings.output_directory))
        run_directory = base_output / (
            f"{_clean_name(scene.name)}-{datetime.now().strftime('%Y%m%d-%H%M%S')}"
        )
        input_directory = run_directory / "input"
        truth_directory = run_directory / "truth"
        host_directory = run_directory / "host"
        for directory in (input_directory, truth_directory, host_directory):
            directory.mkdir(parents=True, exist_ok=True)

        original = {
            "frame": scene.frame_current,
            "resolution_x": scene.render.resolution_x,
            "resolution_y": scene.render.resolution_y,
            "resolution_percentage": scene.render.resolution_percentage,
            "filepath": scene.render.filepath,
            "media_type": scene.render.image_settings.media_type,
            "file_format": scene.render.image_settings.file_format,
            "color_mode": scene.render.image_settings.color_mode,
            "use_nodes": scene.use_nodes,
            "compositing_node_group": scene.compositing_node_group,
            "engine": scene.render.engine,
            "view_layer_passes": [
                (view_layer, view_layer.use_pass_z, view_layer.use_pass_vector)
                for view_layer in scene.view_layers
            ],
        }
        node_tree = None
        try:
            if settings.frame:
                scene.frame_set(settings.frame)
            input_width = max(2, round(scene.render.resolution_x * settings.input_scale))
            input_height = max(2, round(scene.render.resolution_y * settings.input_scale))
            truth_width = scene.render.resolution_x
            truth_height = scene.render.resolution_y
            node_tree, depth_output, vector_output = _configure_pass_outputs(scene, input_directory)
            _render_pass(
                scene,
                input_width,
                input_height,
                input_directory / "input.bmp",
                input_directory / "passes" / "input",
                (depth_output, vector_output),
            )
            _render_pass(
                scene,
                truth_width,
                truth_height,
                truth_directory / "ground_truth.bmp",
                truth_directory / "passes" / "ground_truth",
                (depth_output, vector_output),
            )

            _run_process(
                python,
                [
                    converter,
                    "--depth-exr",
                    input_directory / "passes" / "input" / "depth.exr",
                    "--depth-out",
                    input_directory / "depth.dlss5p32",
                    "--depth-near",
                    "0.1",
                    "--depth-far",
                    "1000.0",
                    "--depth-reverse-z",
                    "--motion-exr",
                    input_directory / "passes" / "input" / "vector.exr",
                    "--motion-out",
                    input_directory / "motion.dlss5v32",
                ],
                run_directory / "convert.log",
            )

            environment = {}
            if settings.core_dll:
                environment["DLSS5_CORE_DLL"] = bpy.path.abspath(settings.core_dll)
            host_output = host_directory / "dlssnr.ppm"
            _run_process(
                host,
                [
                    "--input",
                    input_directory / "input.bmp",
                    "--depth-f32",
                    input_directory / "depth.dlss5p32",
                    "--motion-f32",
                    input_directory / "motion.dlss5v32",
                    "--output",
                    host_output,
                    "--raw-output",
                    host_directory / "dlssnr.rgba32f",
                    "--runtime-dir",
                    runtime,
                    "--output-encoding",
                    settings.output_encoding.lower(),
                ],
                run_directory / "host.log",
                environment,
            )
            _run_process(
                python,
                [
                    compare,
                    host_directory,
                    "--input",
                    "..\\input\\input.bmp",
                    "--truth",
                    "..\\truth\\ground_truth.bmp",
                    "--result",
                    "dlssnr.ppm",
                ],
                run_directory / "compare.log",
            )

            preview_output = host_directory / "dlssnr.png"
            if not preview_output.is_file():
                raise RuntimeError(f"DLSSNR preview was not generated: {preview_output}")
            settings.last_output = str(preview_output)
            settings.last_metrics = _parse_metrics(run_directory / "compare.log")
            settings.last_status = f"PASS: {input_width}x{input_height} -> same resolution"
            settings.result_image = _load_image(preview_output, "DLSS5 NR Result")
            settings.input_image = _load_image(input_directory / "input.bmp", "DLSS5 Input")
            settings.reference_image = _load_image(
                truth_directory / "ground_truth.bmp", "DLSS5 Ground Truth"
            )
            self.report({"INFO"}, settings.last_status)
        except Exception as error:
            settings.last_status = f"ERROR: {error}"
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}
        finally:
            scene.frame_set(original["frame"])
            scene.render.resolution_x = original["resolution_x"]
            scene.render.resolution_y = original["resolution_y"]
            scene.render.resolution_percentage = original["resolution_percentage"]
            scene.render.filepath = original["filepath"]
            scene.render.image_settings.media_type = original["media_type"]
            scene.render.image_settings.file_format = original["file_format"]
            scene.render.image_settings.color_mode = original["color_mode"]
            scene.render.engine = original["engine"]
            for view_layer, use_pass_z, use_pass_vector in original["view_layer_passes"]:
                view_layer.use_pass_z = use_pass_z
                view_layer.use_pass_vector = use_pass_vector
            if not settings.keep_compositor:
                scene.use_nodes = original["use_nodes"]
                scene.compositing_node_group = original["compositing_node_group"]
                if node_tree and node_tree.name in bpy.data.node_groups:
                    bpy.data.node_groups.remove(node_tree)
        return {"FINISHED"}


class DLSS5NPR_OT_show_image(Operator):
    bl_idname = "dlss5_npr.show_image"
    bl_label = "Show Image"
    bl_options = {"REGISTER"}

    image_kind: EnumProperty(
        items=(
            ("RESULT", "DLSSNR", "Show the DLSSNR output"),
            ("INPUT", "Input", "Show the low-resolution input"),
            ("REFERENCE", "Ground Truth", "Show the reference render"),
        ),
        default="RESULT",
        options={"HIDDEN"},
    )

    @classmethod
    def poll(cls, context):
        return bool(context.scene and context.scene.dlss5_npr_test.last_status)

    def execute(self, context):
        settings = context.scene.dlss5_npr_test
        image = {
            "RESULT": settings.result_image,
            "INPUT": settings.input_image,
            "REFERENCE": settings.reference_image,
        }[self.image_kind]
        if image is None:
            self.report({"ERROR"}, "The selected test image is not loaded")
            return {"CANCELLED"}
        path = Path(bpy.path.abspath(image.filepath))
        if not path.is_file():
            self.report({"ERROR"}, f"Image not found: {path}")
            return {"CANCELLED"}
        for area in context.screen.areas:
            if area.type == "IMAGE_EDITOR":
                area.spaces.active.image = image
                return {"FINISHED"}
        context.area.type = "IMAGE_EDITOR"
        context.area.spaces.active.image = image
        return {"FINISHED"}


class DLSS5NPR_OT_reset_paths(Operator):
    bl_idname = "dlss5_npr.reset_paths"
    bl_label = "Reset Paths"
    bl_options = {"REGISTER"}

    def execute(self, context):
        settings = context.scene.dlss5_npr_test
        settings.host_executable = ""
        settings.runtime_directory = ""
        settings.python_executable = ""
        settings.core_dll = ""
        settings.output_directory = ""
        _ensure_defaults(settings)
        settings.last_status = "Paths reset"
        return {"FINISHED"}


class DLSS5NPR_OT_detect_paths(Operator):
    bl_idname = "dlss5_npr.detect_paths"
    bl_label = "Detect Paths"
    bl_options = {"REGISTER"}

    def execute(self, context):
        settings = context.scene.dlss5_npr_test
        _ensure_defaults(settings)
        settings.last_status = "Paths detected"
        return {"FINISHED"}


class DLSS5NPR_OT_open_output(Operator):
    bl_idname = "dlss5_npr.open_output"
    bl_label = "Open Output Folder"
    bl_options = {"REGISTER"}

    @classmethod
    def poll(cls, context):
        if not context.scene:
            return False
        settings = context.scene.dlss5_npr_test
        return bool(settings.last_output or settings.output_directory)

    def execute(self, context):
        settings = context.scene.dlss5_npr_test
        output_path = Path(bpy.path.abspath(settings.last_output or settings.output_directory))
        directory = output_path if output_path.is_dir() else output_path.parent
        if not directory.is_dir():
            self.report({"ERROR"}, f"Output directory not found: {directory}")
            return {"CANCELLED"}
        if os.name != "nt":
            self.report({"ERROR"}, "Opening the output folder is only supported on Windows")
            return {"CANCELLED"}
        os.startfile(str(directory))
        return {"FINISHED"}


class DLSS5NPR_PT_render(Panel):
    bl_label = "DLSS5 NPR"
    bl_idname = "DLSS5NPR_PT_render"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "render"

    @classmethod
    def poll(cls, context):
        return bool(context.scene and context.scene.render.engine in EEVEE_ENGINES)

    def draw(self, context):
        layout = self.layout
        try:
            settings = getattr(context.scene, "dlss5_npr_test", None)
            if settings is None:
                layout.label(text="DLSS5 NPR Test is not initialized")
                layout.label(text="Disable and re-enable the add-on, then restart Blender")
                return
        except Exception as error:
            layout.label(text="DLSS5 NPR initialization error")
            layout.label(text=str(error)[:160])
            return
        layout.use_property_split = True
        layout.use_property_decorate = False

        row = layout.row(align=True)
        row.operator(DLSS5NPR_OT_run_test.bl_idname, icon="RENDER_STILL")
        show = row.operator(DLSS5NPR_OT_show_image.bl_idname, text="DLSSNR", icon="IMAGE_DATA")
        show.image_kind = "RESULT"

        row = layout.row(align=True)
        show = row.operator(DLSS5NPR_OT_show_image.bl_idname, text="Input", icon="IMAGE")
        show.image_kind = "INPUT"
        show = row.operator(
            DLSS5NPR_OT_show_image.bl_idname, text="Ground Truth", icon="IMAGE"
        )
        show.image_kind = "REFERENCE"

        layout.prop(settings, "input_scale")
        layout.prop(settings, "frame")
        layout.prop(settings, "output_encoding")
        layout.prop(settings, "keep_compositor")

        row = layout.row(align=True)
        row.operator(DLSS5NPR_OT_open_output.bl_idname, icon="FILE_FOLDER")
        row.operator(DLSS5NPR_OT_detect_paths.bl_idname, icon="VIEWZOOM")
        row.operator(DLSS5NPR_OT_reset_paths.bl_idname, icon="FILE_REFRESH")

        box = layout.box()
        box.label(text="External Runtime")
        box.prop(settings, "host_executable")
        box.prop(settings, "runtime_directory")
        box.prop(settings, "python_executable")
        box.prop(settings, "core_dll")
        box.prop(settings, "output_directory")
        if not settings.host_executable or not settings.runtime_directory:
            box.label(text="Click Detect Paths or Run DLSSNR Test to fill empty paths")

        if settings.last_status:
            layout.label(text=settings.last_status)
        if settings.last_metrics:
            for line in settings.last_metrics.splitlines():
                layout.label(text=line)


CLASSES = (
    DLSS5NPRTestSettings,
    DLSS5NPR_OT_run_test,
    DLSS5NPR_OT_show_image,
    DLSS5NPR_OT_reset_paths,
    DLSS5NPR_OT_detect_paths,
    DLSS5NPR_OT_open_output,
    DLSS5NPR_PT_render,
)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.dlss5_npr_test = PointerProperty(type=DLSS5NPRTestSettings)


def unregister():
    del bpy.types.Scene.dlss5_npr_test
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
