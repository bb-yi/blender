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
parser.add_argument("--legacy-overlap", action="store_true")
args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
out = args.out
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

    # Dirty tags alone must preserve the shadow, while vertex edits at the same object
    # transform must update the acceleration structure.
    opaque.data.update()
    capture("unchanged_geometry_update")
    for vertex in opaque.data.vertices:
        vertex.co.x += 3.0
    opaque.data.update()
    capture("vertex_edit_away")
    for vertex in opaque.data.vertices:
        vertex.co.x -= 3.0
    opaque.data.update()
    capture("vertex_edit_back")

    # A concave quad can choose the other diagonal while keeping its vertex and triangle counts.
    # The resulting index stream must be rebuilt even though its Vulkan layout still matches.
    opaque.data.calc_loop_triangles()
    original_triangles = tuple(tuple(tri.loops) for tri in opaque.data.loop_triangles)
    corner = opaque.data.vertices[1].co.copy()
    opaque.data.vertices[1].co = (-0.28, 0.24, 0)
    opaque.data.update()
    opaque.data.calc_loop_triangles()
    new_triangles = tuple(tuple(tri.loops) for tri in opaque.data.loop_triangles)
    assert len(new_triangles) == len(original_triangles)
    assert new_triangles != original_triangles
    capture("same_count_topology_away")
    opaque.data.vertices[1].co = corner
    opaque.data.update()
    capture("same_count_topology_back")

    # A new triangle layout must replace the refittable BLAS and its primitive ranges.
    original_mesh = opaque.data
    edited_mesh = bpy.data.meshes.new("Topology edit")
    edited_mesh.from_pydata([(2, -0.5, 0), (3, -0.5, 0), (2, 0.5, 0)], [], [(0, 1, 2)])
    edited_mesh.materials.append(original_mesh.materials[0])
    opaque.data = edited_mesh
    capture("topology_edit_away")
    opaque.data = original_mesh
    capture("topology_edit_back")

    # A small mesh uses 16-bit indices. Only its second material's subrange covers the receiver.
    material_mesh = bpy.data.meshes.new("Shared index buffer")
    quad = [(-0.5, -0.5, 0), (0.5, -0.5, 0), (0.5, 0.5, 0), (-0.5, 0.5, 0)]
    material_mesh.from_pydata([(x + 2, y, z) for x, y, z in quad] + quad, [],
                              [(0, 1, 2, 3), (4, 5, 6, 7)])
    material_mesh.materials.append(original_mesh.materials[0])
    material_mesh.materials.append(mat("Second material", 1.0))
    material_mesh.polygons[1].material_index = 1
    opaque.data = material_mesh
    capture("material_subrange")
    for vertex in material_mesh.vertices[4:]:
        vertex.co.x += 3.0
    material_mesh.update()
    capture("material_subrange_away")
    for vertex in material_mesh.vertices[4:]:
        vertex.co.x -= 3.0
    material_mesh.update()
    capture("material_subrange_refit")
    opaque.data = original_mesh

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
    "unchanged_geometry_update",
    "vertex_edit_back",
    "same_count_topology_back",
    "topology_edit_back",
    "material_subrange",
    "material_subrange_refit",
    "blended_close_contact",
):
    assert values[name] < .05, (name, values)
assert values["self_shadow_only"] > .95, values
assert values["vertex_edit_away"] > .95, values
assert values["same_count_topology_away"] > .95, values
assert values["topology_edit_away"] > .95, values
assert values["material_subrange_away"] > .95, values
assert values["caster_reference"] > .1, values
assert abs(values["caster_disabled"] - values["caster_reference"]) < .02, values
assert .47 < values["translucent_only"] < .53, values
assert values["blended_lit_reference"] > .1, values


def test_cast_overlap():
    for obj in list(s.objects):
        if obj.type == "MESH":
            bpy.data.objects.remove(obj, do_unlink=True)

    def quad(x, y, z, size):
        return [(x - size, y - size, z), (x + size, y - size, z),
                (x + size, y + size, z), (x - size, y + size, z)]

    rows = (-1.2, -.4, .4, 1.2)
    vertices = quad(0, 0, 0, 1.5)
    for y in (rows[1], rows[3]):
        vertices += quad(1.6, y, .8, .24)
    mesh = bpy.data.meshes.new("Receiver and self blockers")
    mesh.from_pydata(vertices, [], [(i, i + 1, i + 2, i + 3) for i in range(0, len(vertices), 4)])
    mesh.materials.append(receiver_mat)
    own = bpy.data.objects.new(mesh.name, mesh)
    s.collection.objects.link(own)
    own.shadow_terminator_geometry_offset = 0
    external_mat = bpy.data.materials.new("External opaque blocker")
    external_own = plane("External only", .48, 1.2, external_mat, False)
    external_own.location.xy = (2.4, rows[2])
    external_overlap = bpy.data.objects.new("External overlap instance", external_own.data)
    s.collection.objects.link(external_overlap)
    external_overlap.location = (2.4, rows[3], 1.2)
    external_overlap.visible_camera = False
    sun = bpy.data.lights.new("Overlap sun", "SUN")
    sun.angle = 0
    light.data = sun
    light.rotation_euler = (0, np.arctan(2.0), 0)
    e.ray_tracing_method = "HARDWARE"
    tree = receiver_mat.node_tree
    tree.nodes.clear()
    info = tree.nodes.new("ShaderNodeShaderInfo")
    channels = tree.nodes.new("ShaderNodeCombineColor")
    emission = tree.nodes.new("ShaderNodeEmission")
    output = tree.nodes.new("ShaderNodeOutputMaterial")
    for index, name in enumerate(("Shadow", "Self Shadow", "Cast Shadow")):
        tree.links.new(info.outputs[name], channels.inputs[index])
    tree.links.new(channels.outputs[0], emission.inputs["Color"])
    tree.links.new(emission.outputs[0], output.inputs["Surface"])
    target = gpu.types.GPUOffScreen(192, 192, format="RGBA16F")
    report = {}

    def check(name, expected=None):
        bpy.context.view_layer.update()
        projection = camera.calc_matrix_camera(bpy.context.evaluated_depsgraph_get(), x=192, y=192)
        target.draw_view3d(s, bpy.context.view_layer, space, region, camera.matrix_world.inverted(),
                           projection, do_color_management=False, draw_background=True)
        pixels = np.asarray(target.texture_color.read(), dtype=np.float32).reshape(192, 192, 4)
        samples = []
        for y in rows:
            row = int((y / 3 + .5) * 192)
            samples.append(pixels[row - 2:row + 3, 94:99, :3].mean(axis=(0, 1)))
        samples = np.asarray(samples)
        assert np.isfinite(samples).all(), (name, samples)
        report[name] = samples.tolist()
        if expected is not None:
            assert np.allclose(samples, expected, atol=.02), (name, samples, expected)
        return samples

    legacy_expected = [[1, 1, 1], [0, 0, 1], [0, 1, 0], [0, 0, 1]]
    expected = [row[:] for row in legacy_expected]
    if not args.legacy_overlap:
        expected[3][2] = 0
    try:
        check("sun", expected)
        e.ray_tracing_method = 'SCREEN'
        receiver_mat.surface_render_method = 'BLENDED'
        check("forward_sun", expected)
        receiver_mat.surface_render_method = 'DITHERED'
        e.ray_tracing_method = 'HARDWARE'
        link = bpy.data.collections.new("Only self blocks this light")
        link.objects.link(own)
        light.light_linking.blocker_collection = link
        check("shadow_link", [[1, 1, 1], [0, 0, 1], [1, 1, 1], [0, 0, 1]])
        light.light_linking.blocker_collection = None

        external_overlap.hide_viewport = True
        shared_instance = bpy.data.objects.new("Blocker sharing the receiver mesh", own.data)
        s.collection.objects.link(shared_instance)
        shared_instance.location = (4, 2.4, .4)
        shared_instance.rotation_euler.z = np.pi
        shared_instance.visible_camera = False
        check("shared_receiver_mesh", expected)
        shared_instance.hide_viewport = True

        transparent = plane("Translucent overlap", .48, 1.2, mat("Overlap alpha", .5), False)
        transparent.location.xy = (2.4, rows[3])
        check("transparent", legacy_expected)
        transparent.hide_viewport = True
        external_overlap.hide_viewport = False

        e.use_hardware_raytracing_shadows = False
        check("native")
        e.use_hardware_raytracing_shadows = True

        area_data = bpy.data.lights.new("Overlap area", "AREA")
        area_data.energy = 1500
        area_data.size = 0
        light.data = area_data
        light.location = (20, 0, 10)
        light.rotation_euler = (0, np.arctan(2.0), 0)
        area = check("area")
        assert np.allclose(area[:, 0], [1, 0, 0, 0], atol=.02), area
        assert np.allclose(area[[0, 2], 1], [1, 1], atol=.02), area
        assert np.all(area[[1, 3], 1] < .5), area
        assert np.allclose(area[[0, 1], 2], [1, 1], atol=.02), area
        assert area[2, 2] < .5, area
        assert abs(area[3, 2] - (1 if args.legacy_overlap else area[3, 1])) < .02, area
    finally:
        target.free()
        (out / "overlap.json").write_text(json.dumps(report, indent=2), encoding="utf-8")


test_cast_overlap()
print("HWRT_REVIEW_OK", flush=True)
