# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise surface shadows in both modes, caster toggles, and closely spaced blockers."""

import argparse
import json
from pathlib import Path
import sys

import bpy
import gpu
from mathutils import Matrix
import numpy as np

parser = argparse.ArgumentParser()
parser.add_argument("--out", required=True, type=Path)
out = parser.parse_args(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []).out
out.mkdir(parents=True, exist_ok=True)
gpu.init()
assert gpu.platform.backend_type_get() == "VULKAN"
s = bpy.context.scene
e = s.eevee
assert e.is_hardware_raytracing_supported
s.render.engine = "BLENDER_EEVEE"
s.render.resolution_x = s.render.resolution_y = 192
s.render.resolution_percentage = 100
s.render.use_compositing = False
e.taa_samples = 1
e.use_taa_reprojection = False
e.use_raytracing = True
e.use_shadows = True
e.use_fast_gi = False
e.use_hardware_raytracing_shadows = True
e.ray_tracing_options.use_hardware_shadows = True
for feature in ("gi", "reflections", "refractions", "secondary_gi"):
    setattr(e, "use_hardware_raytracing_" + feature, False)
e.hardware_raytracing_shadow_samples = 1
e.hardware_raytracing_shadow_transparency = 1.0

for obj in list(s.objects):
    bpy.data.objects.remove(obj, do_unlink=True)
s.world = bpy.data.worlds.new("Shadow review dark world")
s.world.use_nodes = True
s.world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0


def mat(name, alpha=None):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    tree = material.node_tree
    if alpha is None:
        info = tree.nodes.new("ShaderNodeShaderInfo")
        emission = tree.nodes.new("ShaderNodeEmission")
        output = next(n for n in tree.nodes if n.type == "OUTPUT_MATERIAL")
        tree.links.new(info.outputs["Shadow"], emission.inputs["Color"])
        tree.links.new(emission.outputs["Emission"], output.inputs["Surface"])
    else:
        tree.nodes.get("Principled BSDF").inputs["Alpha"].default_value = alpha
        material.use_transparent_shadow = True
    return material


def plane(name, width, z, material, visible=True):
    r = width / 2
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata([(-r, -r, 0), (r, -r, 0), (r, r, 0), (-r, r, 0)], [], [(0, 1, 2, 3)])
    mesh.materials.append(material)
    obj = bpy.data.objects.new(name, mesh)
    s.collection.objects.link(obj)
    obj.location.z = z
    obj.visible_camera = visible
    obj.shadow_terminator_geometry_offset = 0
    return obj

receiver_mat = mat("Shadow output in Surface")
receiver = plane("Receiver", 3.0, 0.0, receiver_mat)
opaque = plane("Opaque blocker", 1.0, .5, mat("Opaque", 1.0), False)
translucent = plane("Transparent layer", 1.0, .5, mat("Half opacity", .5), False)
translucent.hide_viewport = True
light = bpy.data.objects.new("Sun", bpy.data.lights.new("Sun", "SUN"))
s.collection.objects.link(light)
light.location = (0, 0, 4)
light.data.angle = 0.0

camera = bpy.data.objects.new("Camera", bpy.data.cameras.new("Camera"))
s.collection.objects.link(camera)
camera.matrix_world = Matrix.Translation((0, 0, 4))
camera.data.type = "ORTHO"
# At a long clip range, the stored depth still resolves a 0.1 mm blocker.
camera.data.clip_end = 1000
camera.data.ortho_scale = 3.0
s.camera = camera
area = next(a for a in bpy.context.screen.areas if a.type == "VIEW_3D")
region = next(r for r in area.regions if r.type == "WINDOW")
space = area.spaces.active
space.shading.type = "RENDERED"
space.shading.use_scene_lights_render = True
space.shading.use_scene_world_render = True
space.shading.use_compositor = "DISABLED"
space.overlay.show_overlays = False
space.camera = camera
space.region_3d.view_perspective = "CAMERA"
offscreen = gpu.types.GPUOffScreen(192, 192, format="RGBA16F")
values = {}


def capture(name):
    bpy.context.view_layer.update()
    d = bpy.context.evaluated_depsgraph_get()
    projection = camera.calc_matrix_camera(d, x=192, y=192)
    print("REVIEW_BEGIN", name, flush=True)
    offscreen.draw_view3d(s, bpy.context.view_layer, space, region, camera.matrix_world.inverted(),
                          projection, do_color_management=False, draw_background=True)
    pixels = np.asarray(offscreen.texture_color.read(), dtype=np.float32).reshape(192, 192, 4)
    values[name] = float(np.mean(pixels[91:101,91:101,0]))
    print("REVIEW_RESULT", name, values[name], flush=True)


try:
    e.ray_tracing_method = "PROBE"
    capture("surface_probe")
    e.ray_tracing_method = "HARDWARE"
    capture("surface_hardware")

    tree = receiver_mat.node_tree
    output = next(n for n in tree.nodes if n.type == "OUTPUT_MATERIAL")
    old_surface = output.inputs["Surface"].links[0].from_socket
    info = next(n for n in tree.nodes if n.bl_idname == "ShaderNodeShaderInfo")
    emission = old_surface.node
    tree.links.new(info.outputs["Cast Shadow"], emission.inputs["Color"])
    capture("cast_shadow_only")
    tree.links.new(info.outputs["Self Shadow"], emission.inputs["Color"])
    capture("self_shadow_only")
    tree.links.new(info.outputs["Shadow"], emission.inputs["Color"])
    diffuse = tree.nodes.new("ShaderNodeBsdfDiffuse")
    diffuse.inputs["Color"].default_value = (1, 1, 1, 1)
    tree.links.new(diffuse.outputs[0], output.inputs["Surface"])
    capture("caster_enabled")
    opaque.visible_shadow = False
    capture("caster_disabled")
    opaque.visible_shadow = True
    capture("caster_reenabled")
    opaque.hide_viewport = True
    capture("caster_reference")
    opaque.hide_viewport = False
    tree.links.new(old_surface, output.inputs["Surface"])

    e.ray_tracing_method = "PROBE"
    translucent.hide_viewport = False
    opaque.location.z = .50005
    capture("opaque_behind_translucent")
    opaque.hide_viewport = True
    capture("translucent_only")

    translucent.hide_viewport = True
    opaque.hide_viewport = False
    opaque.location.z = .0001
    light.data.type = "POINT"
    light.data.energy = 1000
    light.data.shadow_soft_size = 0
    capture("point_close_contact")

    blended = mat("Blended receiver", .6)
    blended.surface_render_method = "BLENDED"
    receiver.data.materials[0] = blended
    capture("blended_close_contact")
    opaque.hide_viewport = True
    capture("blended_lit_reference")
finally:
    offscreen.free()

(out / "results.json").write_text(json.dumps(values, indent=2), encoding="utf-8")
for name in (
    "surface_probe",
    "surface_hardware",
    "cast_shadow_only",
    "caster_enabled",
    "caster_reenabled",
    "opaque_behind_translucent",
    "point_close_contact",
    "blended_close_contact",
):
    assert values[name] < .05, (name, values)
assert values["self_shadow_only"] > .95, values
assert values["caster_reference"] > .1, values
assert abs(values["caster_disabled"] - values["caster_reference"]) < .02, values
assert .47 < values["translucent_only"] < .53, values
assert values["blended_lit_reference"] > .1, values
print("HWRT_REVIEW_OK", flush=True)
