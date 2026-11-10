"""Pre-camera-FX stage pass: shared samples, output isolation, filters, and lifecycle."""
import json
import os
from pathlib import Path
import subprocess
import time

import bpy

OUT = Path(os.environ["PRE_FX_TEST_OUT"])
OUT.mkdir(parents=True, exist_ok=True)
if not os.environ.get("PRE_FX_TEST_CHILD"):
    assert os.environ.get("BLENDER_USER_RESOURCES"), "User resources must be isolated"
    env = dict(os.environ, PRE_FX_TEST_CHILD="1")
    proc = subprocess.run([bpy.app.binary_path, "--background", "--factory-startup",
                           "--gpu-backend", os.environ.get("PRE_FX_TEST_BACKEND", "vulkan"), "--python-exit-code", "1",
                           "--python", str(Path(__file__).resolve())], env=env)
    if proc.returncode:
        raise RuntimeError(f"Pre-FX regression child failed: {proc.returncode}")
    print("PRE_FX_STAGE_PASS_OK")
    raise SystemExit(0)

import numpy as np
import OpenImageIO as oiio

report = {"renders": {}, "checks": {}, "dlss_requested": bool(os.environ.get("PRE_FX_TEST_DLSS"))}


def flush():
    (OUT / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")


def read_exr(path):
    image = oiio.ImageInput.open(str(path))
    assert image, str(path)
    channels = {}
    subimage = 0
    while image.seek_subimage(subimage, 0):
        spec = image.spec()
        data = image.read_image(format="float")
        assert np.isfinite(data).all(), str(path)
        view = spec.get_string_attribute("view", "")
        for i, name in enumerate(spec.channelnames):
            if view and "." in name:
                prefix, component = name.rsplit(".", 1)
                name = f"{prefix}.{view}.{component}"
            channels[name] = data[..., i].copy()
        subimage += 1
    image.close()
    assert channels, str(path)
    return channels


def rgba(channels, layer="ViewLayer", name="Combined", view=""):
    # read_exr normalizes per-part view metadata into the lookup key.
    suffix = f".{view}" if view else ""
    keys = [f"{layer}.{name}{suffix}.{c}" for c in "RGBA"]
    assert all(k in channels for k in keys), (keys, list(channels))
    return np.stack([channels[k] for k in keys], axis=-1)


def compare(name, actual, expected, exact=False):
    assert actual.shape == expected.shape, (name, actual.shape, expected.shape)
    error = np.abs(actual - expected)
    entry = {"mean": float(error.mean()), "max": float(error.max()), "shape": list(actual.shape)}
    report["checks"][name] = entry
    flush()
    assert entry["max"] <= (0 if exact else 0.005), (name, entry)
    assert entry["mean"] <= (0 if exact else 0.0001), (name, entry)


bpy.ops.wm.read_factory_settings(use_empty=True)
s = bpy.context.scene
s.render.engine = "BLENDER_EEVEE"
s.render.resolution_x, s.render.resolution_y, s.render.resolution_percentage = 192, 128, 100
s.eevee.taa_render_samples = 32
s.eevee.dlss5_mode = "OFF"
s.eevee.dlss_sr_render_quality = "OFF"
s.eevee.motion_blur_steps = 3
s.render.motion_blur_shutter = 0.8
s.render.film_transparent = True
s.render.image_settings.media_type = "MULTI_LAYER_IMAGE"
s.render.image_settings.file_format = "OPEN_EXR_MULTILAYER"
s.render.image_settings.views_format = "MULTIVIEW"
s.render.image_settings.color_depth = "32"
s.world = bpy.data.worlds.new("PreFXWorld")
s.world.use_nodes = True
s.world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.2
bpy.ops.object.camera_add(location=(0, 0, 9))
camera = s.camera = bpy.context.object
camera.data.lens = 45
camera.data.dof.focus_distance = 6
camera.data.dof.aperture_fstop = 0.7
camera.keyframe_insert("location", frame=1)
camera.location.x = 0.4
camera.keyframe_insert("location", frame=3)

for i, (x, z) in enumerate(((-1.1, 0), (1.0, 3))):
    bpy.ops.mesh.primitive_cube_add(size=1.2, location=(x, 0, z))
    ob = bpy.context.object
    ob.name = f"PreFXObject{i}"
    ob.rotation_euler = (0.2, 0.3, 0.15)
    ob.keyframe_insert("location", frame=1)
    ob.location.x += 0.8
    ob.keyframe_insert("location", frame=3)
    mat = bpy.data.materials.new(f"PreFXMaterial{i}")
    mat.use_nodes = True
    tree = mat.node_tree
    if i == 1:
        bsdf = tree.nodes.new("ShaderNodePrincipledNPR")
        tree.links.new(bsdf.outputs[0], tree.nodes["Material Output"].inputs["Surface"])
    else:
        bsdf = tree.nodes["Principled BSDF"]
        bsdf.inputs["Emission Color"].default_value = (0.05, 0.4, 0.9, 1)
        bsdf.inputs["Emission Strength"].default_value = 0.8
    ob.data.materials.append(mat)
bpy.ops.object.light_add(type="AREA", location=(0, 2, 7))
bpy.context.object.data.energy = 700
bpy.context.object.data.size = 4
s.frame_set(2, subframe=0.25)
layer = s.view_layers[0]
assert not layer.use_pass_pre_fx, "New pass must default to off"
layer.use_pass_z = True
layer.use_pass_pre_fx = True
tree = bpy.data.node_groups.new("PreFXComposite", "CompositorNodeTree")
s.compositing_node_group = tree
rl = tree.nodes.new("CompositorNodeRLayers")
rl.layer = layer.name
assert "Pre-FX" in rl.outputs, list(rl.outputs.keys())
fo = tree.nodes.new("CompositorNodeOutputFile")
fo.format.media_type = "IMAGE"
fo.format.file_format = "OPEN_EXR"
fo.format.color_depth = "32"
fo.file_output_items.clear()
fo.file_output_items.new("RGBA", "pre_fx")
fo.file_name = "compositor_"
tree.links.new(rl.outputs["Pre-FX"], fo.inputs["pre_fx"])


def render(label, dof, motion, enabled=True):
    camera.data.dof.use_dof = dof
    s.render.use_motion_blur = motion
    for vl in s.view_layers:
        vl.use_pass_pre_fx = enabled
    # Re-enabling a dynamic pass must expose the socket and preserve the File Output link.
    if enabled and not fo.inputs["pre_fx"].is_linked:
        tree.links.new(rl.outputs["Pre-FX"], fo.inputs["pre_fx"])
    directory = OUT / label
    directory.mkdir(exist_ok=True)
    for filename in ("layers.exr", "compositor_pre_fx.exr"):
        (directory / filename).unlink(missing_ok=True)
    fo.directory = str(directory)
    fo.mute = not enabled
    s.render.filepath = str(directory / "layers.exr")
    before = (s.frame_current, s.frame_subframe, camera.data.dof.use_dof, s.render.use_motion_blur)
    messages = []
    def on_stats(message):
        if message and message.startswith("Rendering "):
            messages.append(message)
    bpy.app.handlers.render_stats.append(on_stats)
    start = time.perf_counter()
    try:
        assert bpy.ops.render.render(write_still=True) == {"FINISHED"}, label
    finally:
        bpy.app.handlers.render_stats.remove(on_stats)
    assert not any("Pre-FX" in message for message in messages), "Unexpected companion render"
    assert before == (s.frame_current, s.frame_subframe, camera.data.dof.use_dof,
                      s.render.use_motion_blur), "Render modified time or effect settings"
    channels = read_exr(directory / "layers.exr")
    report["renders"][label] = {"seconds": time.perf_counter() - start,
                                 "channels": list(channels), "render_stats": messages,
                                 "nr": s.eevee.dlss5_render_status,
                                 "sr": s.eevee.dlss_sr_render_status}
    flush()
    if enabled and not s.render.use_multiview:
        comp = read_exr(directory / "compositor_pre_fx.exr")
        image = np.stack([comp[c] for c in "RGBA"], axis=-1)
        compare(label + "_compositor", image, rgba(channels, name="Pre-FX"))
    return channels


reference = render("reference", False, False)
compare("no_fx_copy", rgba(reference, name="Pre-FX"), rgba(reference), exact=True)
assert np.any(rgba(reference)[..., 3] == 0), "Scene must exercise transparency"
for label, dof, motion in (("dof", True, False), ("motion", False, True), ("both", True, True)):
    actual = render(label, dof, motion)
    if not motion:
        compare(label + "_reference", rgba(actual, name="Pre-FX"), rgba(reference))
    difference = float(np.abs(rgba(actual) - rgba(actual, name="Pre-FX")).mean())
    assert difference > 0.0001, (label, "Fixture did not produce visible blur", difference)
    disabled = render(label + "_disabled", dof, motion, False)
    compare(label + "_combined_unchanged", rgba(actual), rgba(disabled), exact=True)
    for key in actual:
        if ".Depth." in key:
            compare(label + "_depth_unchanged", actual[key], disabled[key], exact=True)
    assert not any(".Pre-FX." in k for k in disabled)

# With the feature off, an ordinary AOV with this name must not be overwritten.
aov = layer.aovs.add()
aov.name, aov.type = "Pre-FX", "COLOR"
aov_nodes = []
for mat in bpy.data.materials:
    if not mat.use_nodes:
        continue
    node = mat.node_tree.nodes.new("ShaderNodeOutputAOV")
    node.aov_name = "Pre-FX"
    node.inputs["Color"].default_value = (0.125, 0.25, 0.5, 1)
    aov_nodes.append((mat, node))
aov_result = render("disabled_same_name_aov", False, False, False)
interior = rgba(reference)[..., 3] > 0.9999
# Keep the constant-color assertion away from the AOV reconstruction filter at silhouettes.
interior = np.lib.stride_tricks.sliding_window_view(np.pad(interior, 3), (7, 7)).all(axis=(-2, -1))
assert interior.any()
expected = np.broadcast_to(np.array([0.125, 0.25, 0.5]), rgba(aov_result, name="Pre-FX")[interior, :3].shape)
compare("disabled_same_name_aov", rgba(aov_result, name="Pre-FX")[interior, :3], expected)
for mat, node in aov_nodes:
    mat.node_tree.nodes.remove(node)
layer.aovs.remove(aov)

s.eevee.use_bokeh_jittered = True
jitter_ref = render("jitter_reference", False, False)
jitter = render("jitter_both", True, True)
assert np.abs(rgba(jitter, name="Pre-FX") - rgba(jitter_ref)).mean() > .0001, "Sampling blur must remain"
for position in ("START", "END"):
    s.render.motion_blur_position = position
    result = render("shutter_" + position, True, True)
    assert np.abs(rgba(result, name="Pre-FX") - rgba(jitter_ref)).mean() > .0001
s.render.motion_blur_position = "CENTER"
s.eevee.use_bokeh_jittered = False

for frame in (3, 4):
    s.frame_set(frame, subframe=0.125)
    ref = render(f"frame_{frame}_reference", False, False)
    result = render(f"frame_{frame}", True, False)
    compare(f"frame_{frame}", rgba(result, name="Pre-FX"), rgba(ref, name="Pre-FX"))

s.render.use_border = True
s.render.use_crop_to_border = True
s.render.border_min_x, s.render.border_max_x = 0.2, 0.8
s.render.border_min_y, s.render.border_max_y = 0.15, 0.85
ref = render("crop_reference", False, False)
result = render("crop", True, False)
compare("crop", rgba(result, name="Pre-FX"), rgba(ref, name="Pre-FX"))
s.render.use_border = False
s.render.use_crop_to_border = False

second = s.view_layers.new("Second")
second.use_pass_pre_fx = True
ref = render("layers_reference", False, False)
result = render("layers", True, False)
for name in (layer.name, second.name):
    compare("layer_" + name, rgba(result, name, "Pre-FX"), rgba(ref, name))
s.view_layers.remove(second)

s.render.use_multiview = True
ref = render("stereo_reference", False, False)
result = render("stereo", True, False)
for view in ("left", "right"):
    compare("stereo_" + view, rgba(result, name="Pre-FX", view=view), rgba(ref, view=view))
s.render.use_multiview = False

if os.environ.get("PRE_FX_TEST_DLSS"):
    # Existing SR deliberately falls back for transparent backgrounds; test real SR separately.
    s.render.film_transparent = False
    s.eevee.dlss_sr_render_quality = "QUALITY"
    ref = render("sr_reference", False, False)
    assert s.eevee.dlss_sr_render_status.startswith("Completed"), s.eevee.dlss_sr_render_status
    result = render("sr_both", True, False)
    assert s.eevee.dlss_sr_render_status.startswith("Completed"), s.eevee.dlss_sr_render_status
    compare("sr_reference", rgba(result, name="Pre-FX"), rgba(ref, name="Pre-FX"))
    disabled = render("sr_disabled", True, False, False)
    compare("sr_combined_unchanged", rgba(result), rgba(disabled), exact=True)
    s.eevee.dlss_sr_render_quality = "OFF"
    s.eevee.dlss5_mode = "DLSSNR"
    ref = render("nr_reference", False, False)
    assert s.eevee.dlss5_render_status.startswith("Completed"), s.eevee.dlss5_render_status
    result = render("nr_both", True, False)
    assert s.eevee.dlss5_render_status.startswith("Completed"), s.eevee.dlss5_render_status
    compare("nr_reference", rgba(result, name="Pre-FX"), rgba(ref, name="Pre-FX"))
    disabled = render("nr_disabled", True, False, False)
    compare("nr_combined_unchanged", rgba(result), rgba(disabled), exact=True)
    s.eevee.dlss5_mode = "OFF"
    s.render.film_transparent = True

layer.use_pass_pre_fx = True
fo.mute = False
if not fo.inputs['pre_fx'].is_linked:
    tree.links.new(rl.outputs['Pre-FX'], fo.inputs['pre_fx'])
assets = OUT / "assets"
assets.mkdir(parents=True, exist_ok=True)
path = assets / "pre_fx_regression.blend"
s.frame_set(2, subframe=0.25)
s.camera.data.dof.use_dof = True
s.render.use_motion_blur = True
example_output = OUT / "example"
example_output.mkdir(exist_ok=True)
fo.directory = str(example_output)
s.render.filepath = str(example_output / "layers.exr")
bpy.ops.wm.save_as_mainfile(filepath=str(path))
node_name = rl.name
bpy.ops.wm.open_mainfile(filepath=str(path))
assert bpy.context.scene.view_layers[0].use_pass_pre_fx
assert "Pre-FX" in bpy.context.scene.compositing_node_group.nodes[node_name].outputs
s = bpy.context.scene
layer = s.view_layers[0]
camera = s.camera
tree = s.compositing_node_group
rl = tree.nodes[node_name]
fo = next(node for node in tree.nodes if node.bl_idname == 'CompositorNodeOutputFile')
report["save_reload"] = True

# The capture boundary is observable independently of camera effects.
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from filter_graph_test_utils import attach_filter_material, clear_filter_graph
before_filters = render("filter_reference", False, False)
for stage in ("BEFORE_POSTFX", "BEFORE_DEPTH_OF_FIELD", "BEFORE_COMPOSITE"):
    material = bpy.data.materials.new("Stage " + stage)
    material.use_nodes = True
    material.eevee_domain = "FILTER"
    material.node_tree.nodes.clear()
    output = material.node_tree.nodes.new("ShaderNodeOutputFilter")
    output.inputs['Color'].default_value = (.25, .5, 2., 1.)
    output.inputs['Alpha'].default_value = 1.
    attach_filter_material(material, stage=stage)
    result = render("filter_" + stage, False, False)
    expected = np.broadcast_to(np.array([.25, .5, 2., 1.]), rgba(result).shape)
    compare(stage + "_combined", rgba(result), expected)
    compare(stage + "_pre_fx", rgba(result, name="Pre-FX"),
            expected if stage == "BEFORE_POSTFX" else rgba(before_filters, name="Pre-FX"))
    clear_filter_graph()

# Raw outlines enter the stage only when they also participate in Combined.
s.eevee.use_outline = True
control = bpy.data.materials['PreFXMaterial0'].node_tree.nodes.new('ShaderNodeOutlineControl')
for key, value in {'Line Color': (1., 0., 0., 1.), 'Line Width': 2., 'Line Alpha': 1.,
                   'Depth Threshold': .1, 'Normal Threshold': .5,
                   'ID Edge': True, 'Outline ID': 1}.items():
    control.inputs[key].default_value = value
for separate in (False, True):
    layer.eevee.use_pass_outline = separate
    ref = render('outline_reference_' + str(separate), False, False)
    result = render('outline_stage_' + str(separate), True, False)
    sharp = rgba(result, name='Pre-FX')
    red = (sharp[..., 0] > .3) & (sharp[..., 0] > 2*sharp[..., 1]) & (sharp[..., 0] > 2*sharp[..., 2])
    if not separate:
        assert red.sum() > 20, ('Missing raw outline', red.sum())
    else:
        assert red.sum() < 20, ('Outline must stay in its separate pass', red.sum())
        compare('separate_outline_stage', sharp, rgba(ref))
    disabled = render('outline_disabled_' + str(separate), True, False, False)
    compare('outline_combined_' + str(separate), rgba(result), rgba(disabled), exact=True)
s.eevee.use_outline = False
layer.eevee.use_pass_outline = False

# Participating volumes must be shaded once and remain in the stage image.
bpy.ops.mesh.primitive_cube_add(size=6, location=(0, 0, 1))
volume = bpy.context.object
material = bpy.data.materials.new('StageVolume')
material.use_nodes = True
material.node_tree.nodes.clear()
shader = material.node_tree.nodes.new('ShaderNodeVolumePrincipled')
shader.inputs['Density'].default_value = .08
shader.inputs['Color'].default_value = (.1, .4, .8, 1.)
shader.inputs['Emission Strength'].default_value = .05
output = material.node_tree.nodes.new('ShaderNodeOutputMaterial')
material.node_tree.links.new(shader.outputs['Volume'], output.inputs['Volume'])
volume.data.materials.append(material)
ref = render('volume_reference', False, False)
result = render('volume_stage', True, False)
compare('volume_stage_reference', rgba(result, name='Pre-FX'), rgba(ref))
disabled = render('volume_disabled', True, False, False)
compare('volume_combined', rgba(result), rgba(disabled), exact=True)
bpy.data.objects.remove(volume, do_unlink=True)

# A second camera's capture must not change or be duplicated by enabling the stage pass.
capture = s.eevee.render_textures.add()
capture.name = 'PreFXStageCapture'
capture.enabled = True
capture.camera = camera
capture.resolution_x, capture.resolution_y = 64, 48
capture.source = 'COLOR'
capture.use_compositor_output = True
capture.update_mode = 'EVERY_FRAME'
result = render('render_texture_stage', True, False)
disabled = render('render_texture_disabled', True, False, False)
for key in result:
    if '.Pre-FX.' not in key:
        compare('render_texture_unchanged_' + key, result[key], disabled[key], exact=True)
s.eevee.render_textures.remove(len(s.eevee.render_textures) - 1)

# Repeated allocation, pass disabling and changing dimensions must not retain stale output.
for width, height in ((128, 96), (256, 128), (192, 128)):
    s.render.resolution_x, s.render.resolution_y = width, height
    enabled = render(f"resize_{width}_{height}", True, False)
    disabled = render(f"resize_disabled_{width}_{height}", True, False, False)
    compare(f"resize_combined_{width}", rgba(enabled), rgba(disabled), exact=True)
    assert rgba(enabled, name="Pre-FX").shape == (height, width, 4)
s.render.resolution_x, s.render.resolution_y = 192, 128
layer.use_pass_pre_fx = True
report["stage_semantics"] = True
flush()
report["passed"] = True
flush()
print("PRE_FX_STAGE_PASS_OK")
