# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Exercise viewport draw order and recovery after a failed instance upload."""

import json
import os
import re
import time
import traceback
from pathlib import Path

import bpy
import gpu


OUTPUT = Path(os.environ["BLENDER_TEST_OUTPUT_DIR"])
OUTPUT.mkdir(parents=True, exist_ok=True)
FAILURE = "BLENDER_DRAW_TEST_INSTANCE_UPLOAD_FAILURE"
COUNT = 1024
REPORT = {"backend": gpu.platform.backend_type_get(), "captures": {}}
bpy.context.preferences.view.show_splash = False
bpy.context.preferences.view.use_save_prompt = False
os.environ["BLENDER_INSTANCE_CACHE"] = "1"
os.environ["BLENDER_INSTANCE_CACHE_GPU"] = "1"


def fixture(scene):
    for ob in list(scene.objects):
        bpy.data.objects.remove(ob, do_unlink=True)

    mesh = bpy.data.meshes.new("Prototype")
    mesh.from_pydata([(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)], [], [(0, 1, 2, 3)])
    prototype = bpy.data.objects.new("Prototype", mesh)
    scene.collection.objects.link(prototype)
    prototype.location.y = 1000

    material = bpy.data.materials.new("Instance tint")
    material.use_nodes = True
    nodes, links = material.node_tree.nodes, material.node_tree.links
    nodes.clear()
    attribute = nodes.new("ShaderNodeAttribute")
    attribute.attribute_type = "INSTANCER"
    attribute.attribute_name = "tint"
    emission = nodes.new("ShaderNodeEmission")
    surface = nodes.new("ShaderNodeOutputMaterial")
    links.new(attribute.outputs["Color"], emission.inputs["Color"])
    links.new(emission.outputs[0], surface.inputs["Surface"])
    mesh.materials.append(material)

    points = bpy.data.meshes.new("Points")
    # The two visible plates overlap at exactly the same depth. Their coordinates and
    # evaluation order intentionally disagree with ascending spatial order.
    points.from_pydata(
        [(0.25, 0, 0), (-0.25, 0, 0)] + [(0, 60 + i, 0) for i in range(COUNT - 2)], [], [])
    ids = points.attributes.new("id", "INT", "POINT")
    ids.data.foreach_set("value", list(range(COUNT)))
    colors = points.attributes.new("tint", "FLOAT_COLOR", "POINT")
    colors.data.foreach_set("color", [1, 0, 0, 1, 0, 1, 0, 1] + [1.0] * (4 * (COUNT - 2)))
    points.update()
    emitter = bpy.data.objects.new("Emitter", points)
    scene.collection.objects.link(emitter)
    tree = bpy.data.node_groups.new("Instancing", "GeometryNodeTree")
    for direction in ("INPUT", "OUTPUT"):
        tree.interface.new_socket(name="Geometry", in_out=direction, socket_type="NodeSocketGeometry")
    source = tree.nodes.new("NodeGroupInput")
    output = tree.nodes.new("NodeGroupOutput")
    info = tree.nodes.new("GeometryNodeObjectInfo")
    info.inputs["As Instance"].default_value = True
    info.inputs["Object"].default_value = prototype
    instance = tree.nodes.new("GeometryNodeInstanceOnPoints")
    instance.inputs["Pick Instance"].default_value = True
    tree.links.new(source.outputs[0], instance.inputs["Points"])
    tree.links.new(info.outputs["Geometry"], instance.inputs["Instance"])
    tree.links.new(instance.outputs["Instances"], output.inputs[0])
    emitter.modifiers.new("Instancing", "NODES").node_group = tree

    camera = bpy.data.objects.new("Camera", bpy.data.cameras.new("Camera"))
    scene.collection.objects.link(camera)
    camera.location.z = 6
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = 4
    scene.camera = camera


def run():
    scene = bpy.context.scene
    fixture(scene)
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = scene.render.resolution_y = 512
    scene.render.preview_pixel_size = "1"
    scene.eevee.use_shadows = False
    scene.eevee.use_raytracing = False
    scene.eevee.taa_samples = 1
    scene.eevee.use_taa_reprojection = False
    scene.eevee.use_performance_profiler = True
    scene.view_settings.view_transform = "Standard"

    area = next(a for a in bpy.context.screen.areas if a.type == "VIEW_3D")
    region = next(r for r in area.regions if r.type == "WINDOW")
    space = area.spaces.active
    space.overlay.show_overlays = False
    space.show_gizmo = False
    space.region_3d.view_perspective = "CAMERA"
    space.region_3d.view_camera_zoom = 12
    space.shading.use_compositor = "DISABLED"
    space.shading.type = "RENDERED"
    sequence = 0

    def capture(label, reused):
        nonlocal sequence
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            area.tag_redraw()
            yield .08
            profile = scene.eevee.performance_profiler_viewport_report
            frame = re.search(r"^Capture Sequence: (\d+)$", profile, re.MULTILINE)
            stats = re.search(r"\bcompact=(\d+) gpu_reused=(\d+) upload_bytes=(\d+)", profile)
            ready = "shader_queued=0 optimize_queued=0 fallbacks=0 failed=0" in profile
            if frame and int(frame[1]) > sequence and stats and ready and "Sampling: Complete" in profile:
                sequence = int(frame[1])
                compact, actual_reuse, upload_bytes = map(int, stats.groups())
                assert compact == COUNT, profile
                if actual_reuse == reused:
                    REPORT["captures"][label] = [sequence, actual_reuse, upload_bytes]
                    return
        raise AssertionError((label, reused, profile))

    def check_overlap(name):
        path = OUTPUT / (name + ".png")
        # The captured CPU sync can precede presentation of the new Vulkan image.
        for _ in range(12):
            area.tag_redraw()
            yield .2
            with bpy.context.temp_override(area=area, region=region):
                assert bpy.ops.screen.screenshot_area(filepath=str(path)) == {"FINISHED"}
            image = bpy.data.images.load(str(path), check_existing=False)
            try:
                w, h = image.size
                start = (w * (h // 2) + w // 2) * 4
                pixel = tuple(image.pixels[start:start + 3])
            finally:
                bpy.data.images.remove(image)
            if pixel[1] > .95 and max(pixel[0], pixel[2]) < .05:
                return
        raise AssertionError((name, pixel))

    yield from capture("warm", COUNT)
    yield from check_overlap("original_order")
    os.environ[FAILURE] = "1"
    yield from capture("failed_first", 0)
    yield from capture("failed_second", 0)
    os.environ.pop(FAILURE)
    yield from capture("recovery_first", 0)
    yield from capture("recovery_second", 0)
    yield from capture("reused_again", COUNT)
    yield from check_overlap("recovered_order")
    REPORT["passed"] = True


job = run()


def tick():
    try:
        return next(job)
    except StopIteration:
        pass
    except Exception:
        REPORT["error"] = traceback.format_exc()
        print(REPORT["error"], flush=True)
    finally:
        if "error" in REPORT:
            os.environ.pop(FAILURE, None)
    (OUTPUT / "report.json").write_text(json.dumps(REPORT, indent=2), encoding="utf-8")
    if not REPORT.get("passed"):
        os._exit(1)
    print("INSTANCE_VIEWPORT_CACHE_OK", REPORT["backend"], flush=True)
    bpy.ops.wm.quit_blender()
    return None


bpy.app.timers.register(tick, first_interval=.35)
