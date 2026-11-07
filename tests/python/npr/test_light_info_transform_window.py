"""Native rendered-window test. Captures the actual window framebuffer, not GPUOffScreen."""
import argparse
import json
import math
import os
import re
from pathlib import Path
import sys
import time
import traceback

import bpy
import gpu
import OpenImageIO as oiio
from mathutils import Vector

parser = argparse.ArgumentParser()
parser.add_argument("--output-dir", required=True)
parser.add_argument("--npr", action="store_true")
args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:])
OUT = Path(args.output_dir).resolve()
OUT.mkdir(parents=True, exist_ok=True)
assert not bpy.app.background
assert os.environ.get("BLENDER_USER_RESOURCES")
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.context.preferences.use_preferences_save = False
bpy.context.preferences.view.show_splash = False
bpy.context.preferences.filepaths.use_auto_save_temporary_files = False
bpy.context.preferences.filepaths.temporary_directory = str(OUT)
scene = bpy.context.scene
scene.render.engine = "BLENDER_EEVEE"
scene.eevee.taa_samples = 1
scene.eevee.use_taa_reprojection = False
scene.eevee.use_performance_profiler = True
scene.view_settings.view_transform = "Standard"
scene.view_settings.look = "None"
world = bpy.data.worlds.new("WindowWorld")
world.use_nodes = True
world.node_tree.nodes["Background"].inputs["Color"].default_value = (0, 0, 0, 1)
scene.world = world
camera = bpy.data.objects.new("WindowCamera", bpy.data.cameras.new("WindowCamera"))
scene.collection.objects.link(camera)
scene.camera = camera
camera.location = (0, 0, 5)
camera.data.type = "ORTHO"
camera.data.ortho_scale = 3
light = bpy.data.objects.new("WindowLight", bpy.data.lights.new("WindowLight", "POINT"))
scene.collection.objects.link(light)
light.location = (0.3, -0.2, 0.1)
light.rotation_euler = (0.2, 0.4, -0.3)
parent = bpy.data.objects.new("WindowParent", None)
scene.collection.objects.link(parent)
material = bpy.data.materials.new("LightSpaceWindow")
material.use_nodes = True
tree = material.node_tree
tree.nodes.clear()
output = tree.nodes.new("ShaderNodeOutputMaterial")
output.location = (750, 0)
emission = tree.nodes.new("ShaderNodeEmission")
emission.location = (530, 0)
tree.links.new(emission.outputs[0], output.inputs["Surface"])
destination = emission.inputs["Color"]
if args.npr:
    tree = bpy.data.node_groups.new("LightSpaceWindowNPR", "ShaderNodeTree")
    destination = tree.nodes.new("ShaderNodeNPR_Output").inputs["Color"]
    output.nprtree = tree
info = tree.nodes.new("ShaderNodeLightInfo")
info.light_object = light
info.location = (-400, 0)
transform_panel = next(p for p in info.panel_states if p.identifier == 3)
assert transform_panel.is_collapsed, "Transform panel must start collapsed"
transform_panel.is_collapsed = False
for panel in info.panel_states:
    if panel.identifier != 3:
        panel.is_collapsed = True
basis = tree.nodes.new("ShaderNodeBasisTransform")
basis.location = (-80, 0)
basis.vector_type = "POINT"
basis.direction = "TO_BASIS"
basis.orthonormalize = False
basis.inputs["Vector"].default_value = (1.2, -0.7, 0.6)
for name in ("Origin", "X Axis", "Y Axis", "Z Axis"):
    tree.links.new(info.outputs[name], basis.inputs[name])
encode = tree.nodes.new("ShaderNodeVectorMath")
encode.location = (220, 0)
encode.operation = "SCALE"
encode.inputs[3].default_value = 0.12
tree.links.new(basis.outputs[0], encode.inputs[0])
bias = tree.nodes.new("ShaderNodeVectorMath")
bias.location = (380, -150)
bias.operation = "ADD"
bias.inputs[1].default_value = (0.5, 0.5, 0.5)
tree.links.new(encode.outputs[0], bias.inputs[0])
tree.links.new(bias.outputs[0], destination)
bpy.ops.mesh.primitive_plane_add(size=100)
bpy.context.view_layer.objects.active.data.materials.append(material)
window = bpy.context.window_manager.windows[0]
area = next(a for a in window.screen.areas if a.type == "VIEW_3D")
region = next(r for r in area.regions if r.type == "WINDOW")
space = area.spaces.active
space.shading.type = "RENDERED"
space.shading.use_scene_world_render = True
space.shading.use_scene_lights_render = True
space.overlay.show_overlays = False
space.show_gizmo = False
with bpy.context.temp_override(window=window, area=area, region=region):
    space.show_region_toolbar = False
    space.show_region_ui = False
space.region_3d.view_perspective = "CAMERA"
editor = next(a for a in window.screen.areas if a.type == "PROPERTIES")
editor.type = "NODE_EDITOR"
editor.ui_type = "ShaderNodeTree"
editor.spaces.active.shader_type = "NPR" if args.npr else "OBJECT"
with bpy.context.temp_override(window=window, area=editor,
                               region=next(r for r in editor.regions if r.type == "WINDOW")):
    bpy.ops.node.view_all()
state = {"step": 0, "draws": 0, "since": time.monotonic(), "preparation": []}
report = {"passed": False, "pid": os.getpid(), "backend": None, "npr": args.npr, "cases": []}
started = time.monotonic()
stamps = None
labels = ("initial", "translation", "local_z_roll", "object_scale", "parent_shear",
          "constraint", "parent_animation")


def on_draw():
    if bpy.context.area == area:
        state["draws"] += 1
        text = scene.eevee.performance_profiler_viewport_report
        match = re.search(r"material_hits=(\d+) builds=(\d+) pass_hits=(\d+) pass_misses=(\d+)", text)
        if match:
            state["preparation"].append(list(map(int, match.groups())))


handler = bpy.types.SpaceView3D.draw_handler_add(on_draw, (), "WINDOW", "POST_PIXEL")


def srgb(value):
    return value * 12.92 if value <= 0.0031308 else 1.055 * value ** (1 / 2.4) - 0.055


def timer():
    global stamps
    try:
        assert time.monotonic() - started < 150, "Native viewport timeout"
        area.tag_redraw()
        if state["draws"] < 15 or time.monotonic() - state["since"] < 1.2:
            return 0.08
        gpu.init()
        report["backend"] = gpu.platform.backend_type_get()
        matrix = light.evaluated_get(bpy.context.evaluated_depsgraph_get()).matrix_world
        value = matrix.inverted() @ Vector((1.2, -0.7, 0.6))
        expected = [srgb(v * 0.12 + 0.5) for v in value]
        # Read the native window screenshot: POST_PIXEL's active framebuffer can be
        # an empty intermediate on this backend, while the composited window is valid.
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screenshot(filepath=str(OUT / "current.png"))
        reader = oiio.ImageInput.open(str(OUT / "current.png"))
        pixels = reader.read_image(format="float")
        reader.close()
        actual = pixels[pixels.shape[0] - region.y - region.height // 2,
                        region.x + region.width // 2, :3].tolist()
        if actual is None or max(abs(a - b) for a, b in zip(actual, expected)) > 0.006:
            report["awaiting"] = {"actual": actual, "expected": expected, "step": state["step"]}
            (OUT / "progress.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
            if not (OUT / "awaiting.png").exists():
                with bpy.context.temp_override(window=window, area=area, region=region):
                    bpy.ops.screen.screenshot(filepath=str(OUT / "awaiting.png"))
            return 0.1
        if stamps is None:
            stamps = material.shader_compile_timestamp
        assert material.shader_compile_timestamp == stamps, "Viewport transform recompiled shader"
        if state["step"] > 0:
            assert state["preparation"], "Missing material preparation telemetry"
            assert all(row[1] == 0 and row[3] == 0 for row in state["preparation"]), state["preparation"]
        label = labels[state["step"]]
        report["cases"].append({"label": label, "actual": actual, "expected": expected,
                                "timestamp": stamps, "draws": state["draws"],
                                "material_preparation": state["preparation"]})
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screenshot(filepath=str(OUT / (label + ".png")))
        if state["step"] == 0:
            light.location = (-0.2, 0.3, -0.1)
        elif state["step"] == 1:
            light.rotation_euler.rotate_axis("Z", 0.8)
        elif state["step"] == 2:
            light.scale = (1.1, 0.9, 1.2)
        elif state["step"] == 3:
            light.parent = parent
            parent.scale = (-1.6, 0.7, 1.2)
            parent.rotation_euler = (0.4, -0.3, 0.2)
        elif state["step"] == 4:
            target = bpy.data.objects.new("WindowConstraint", None)
            scene.collection.objects.link(target)
            target.location = (0.1, -0.2, 0.3)
            light.constraints.new("COPY_LOCATION").target = target
        elif state["step"] == 5:
            parent.keyframe_insert("rotation_euler", frame=1)
            parent.rotation_euler.z += 0.8
            parent.keyframe_insert("rotation_euler", frame=3)
            scene.frame_set(2)
        else:
            report["passed"] = True
            print("LIGHT_INFO_TRANSFORM_WINDOW_OK", report["backend"], flush=True)
            area.type = "NODE_EDITOR"
            area.ui_type = "ShaderNodeTree"
            area.spaces.active.shader_type = "NPR" if args.npr else "OBJECT"
            with bpy.context.temp_override(window=window, area=area,
                                           region=next(r for r in area.regions if r.type == "WINDOW")):
                bpy.ops.node.select_all(action="SELECT")
                bpy.ops.node.view_all()
            bpy.app.timers.register(capture_nodes, first_interval=1.0)
            return None
        bpy.context.view_layer.update()
        state.update(step=state["step"] + 1, draws=0, since=time.monotonic(), preparation=[])
        return 0.08
    except Exception:
        report["error"] = traceback.format_exc()
        print(report["error"], flush=True)
        finish()
        return None


def capture_nodes():
    with bpy.context.temp_override(window=window, area=area,
                                   region=next(r for r in area.regions if r.type == "WINDOW")):
        bpy.ops.screen.screenshot(filepath=str(OUT / "node_connections.png"))
    finish()
    return None


def finish():
    bpy.types.SpaceView3D.draw_handler_remove(handler, "WINDOW")
    (OUT / "window.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    # This is the isolated synthetic test process, never an existing user instance.
    bpy.app.timers.register(lambda: bpy.ops.wm.quit_blender() and None, first_interval=0.2)


bpy.app.timers.register(timer, first_interval=0.5)
