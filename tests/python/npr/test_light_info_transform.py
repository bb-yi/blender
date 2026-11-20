"""Render evaluated light matrices and Basis Transform against mathutils references.

Run with --output-dir PATH; --reopen PATH verifies the saved fixture in a fresh process.
The companion test_light_info_transform_window.py checks native rendered windows.
"""

import argparse
import json
import math
from pathlib import Path
import sys

import bpy
from mathutils import Matrix, Vector


ARGS = None
OUT = None
FIELDS = ("Origin", "X Axis", "Y Axis", "Z Axis")
LIGHT_INFO_NODE_TYPE = "ShaderNodeNPRLightInfo" if bpy.app.version >= (5, 3, 0) else "ShaderNodeLightInfo"
BASIC = ("Color", "Power", "Type", "Position", "Direction", "Radius", "Spot Size",
         "Sun Angle", "Visible")
VALUE = Vector((0.37, -0.24, 0.61))
COLS = 8
TILE = 24
SPECS = [(name, "RAW", name) for name in FIELDS]
SPECS += [(f"{kind}_{direction}", kind, direction)
          for kind in ("POINT", "VECTOR", "NORMAL")
          for direction in ("TO_BASIS", "FROM_BASIS", "ROUNDTRIP")]
SPECS += [("LegacyPosition", "RAW", "Position"), ("LegacyDirection", "RAW", "Direction"),
          ("ParameterOrigin", "PARAMETER", "Origin"),
          ("FallbackPass", "FALLBACK", "PASS_THROUGH"),
          ("FallbackZero", "FALLBACK", "ZERO")]
ROWS = math.ceil(len(SPECS) * 2 / COLS)
RESULTS = []


def args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--reopen")
    parser.add_argument("--legacy")
    return parser.parse_args(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])


def evaluated_matrix(obj):
    bpy.context.view_layer.update()
    return obj.evaluated_get(bpy.context.evaluated_depsgraph_get()).matrix_world.copy()


def light(name, kind="SUN", data=None):
    obj = bpy.data.objects.new(name, data or bpy.data.lights.new(name, kind))
    bpy.context.scene.collection.objects.link(obj)
    return obj


def basis(tree, info, kind, direction, previous=None):
    node = tree.nodes.new("ShaderNodeBasisTransform")
    node.vector_type = kind
    node.direction = direction
    node.basis_input = "XYZ"
    node.orthonormalize = False
    node.inputs["Vector"].default_value = VALUE
    if previous:
        tree.links.new(previous, node.inputs["Vector"])
    for field in FIELDS:
        tree.links.new(info.outputs[field], next(s for s in node.inputs if s.identifier == field))
    return node


def create_material(index, spec, obj, npr):
    label, kind, operation = spec
    material = bpy.data.materials.new(f"Probe_{index:02}_{label}")
    material.use_nodes = True
    tree = material.node_tree
    tree.nodes.clear()
    output = tree.nodes.new("ShaderNodeOutputMaterial")
    if npr:
        emission = tree.nodes.new("ShaderNodeEmission")
        tree.links.new(emission.outputs[0], output.inputs["Surface"])
        tree = bpy.data.node_groups.new(f"NPR_{label}", "ShaderNodeTree")
        destination = tree.nodes.new("ShaderNodeNPR_Output").inputs["Color"]
        output.nprtree = tree
    else:
        emission = tree.nodes.new("ShaderNodeEmission")
        tree.links.new(emission.outputs[0], output.inputs["Surface"])
        destination = emission.inputs["Color"]
    info = tree.nodes.new(LIGHT_INFO_NODE_TYPE)
    info.name = "LightInfo"
    info.light_object = obj
    if kind == "RAW":
        value = info.outputs[operation]
    elif kind == "PARAMETER":
        value = next(socket for socket in info.outputs
                     if socket.name == operation and socket.identifier.startswith("Shader Parameter "))
    else:
        transform = basis(tree, info, "VECTOR" if kind == "FALLBACK" else kind,
                          "TO_BASIS" if operation == "ROUNDTRIP" or kind == "FALLBACK" else operation)
        if kind == "FALLBACK":
            transform.fallback = operation
        if operation == "ROUNDTRIP":
            transform = basis(tree, info, kind, "FROM_BASIS", transform.outputs[0])
        value = transform.outputs[0]

    # Two tiles preserve precision through EEVEE's half-float color intermediates:
    # integer + 16 (exact) and fractional remainder [0, 1). Decode to signed float.
    floor = tree.nodes.new("ShaderNodeVectorMath")
    floor.operation = "FLOOR"
    tree.links.new(value, floor.inputs[0])
    encode = tree.nodes.new("ShaderNodeVectorMath")
    encode.name = "Encode"
    encode.operation = "ADD"
    encode.inputs[1].default_value = (16, 16, 16)
    tree.links.new(floor.outputs[0], encode.inputs[0])
    tree.links.new(encode.outputs[0], destination)
    fraction = material.copy()
    fraction.name = f"Fraction_{index:02}_{label}"
    fraction_tree = fraction.node_tree
    if npr:
        fraction_output = next(n for n in fraction_tree.nodes if n.type == "OUTPUT_MATERIAL")
        fraction_tree = tree.copy()
        fraction_output.nprtree = fraction_tree
    fraction_encode = fraction_tree.nodes["Encode"]
    fraction_encode.operation = "SUBTRACT"
    fraction_value = next(socket for socket in fraction_tree.nodes[value.node.name].outputs
                          if socket.identifier == value.identifier)
    fraction_tree.links.new(fraction_value,
                            fraction_encode.inputs[0])
    fraction_tree.links.new(fraction_tree.nodes[floor.name].outputs[0], fraction_encode.inputs[1])
    return material, fraction


def setup(npr=False):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = COLS * TILE
    scene.render.resolution_y = ROWS * TILE
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "OPEN_EXR"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "32"
    scene.eevee.taa_samples = 1
    scene.eevee.taa_render_samples = 1
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0
    scene.view_settings.gamma = 1
    world = bpy.data.worlds.new("BlackWorld")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0, 0, 0, 1)
    scene.world = world
    camera = bpy.data.objects.new("ProbeCamera", bpy.data.cameras.new("ProbeCamera"))
    scene.collection.objects.link(camera)
    camera.location = (0, 0, 20)
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = COLS
    scene.camera = camera
    obj = light("Target")
    param = obj.data.shader_parameters.new("Origin", type="VECTOR3")
    param.value_vector3 = (-0.18, 0.41, 0.29)
    for index, spec in enumerate(SPECS):
        for part, material in enumerate(create_material(index, spec, obj, npr)):
            tile = index * 2 + part
            x, y = tile % COLS, tile // COLS
            bpy.ops.mesh.primitive_plane_add(size=1.02, location=(x + 0.5 - COLS / 2,
                                                                y + 0.5 - ROWS / 2, 0))
            bpy.context.object.data.materials.append(material)
    return obj


def all_infos():
    trees = [material.node_tree for material in bpy.data.materials if material.use_nodes]
    trees += [tree for tree in bpy.data.node_groups if tree.bl_idname == "ShaderNodeTree"]
    return [node for tree in trees for node in tree.nodes if node.bl_idname == LIGHT_INFO_NODE_TYPE]


def expected(obj):
    if obj and obj.type != "LIGHT":
        obj = None
    matrix = evaluated_matrix(obj) if obj else Matrix(((0, 0, 0, 0),) * 4)
    linear = matrix.to_3x3()
    valid = abs(linear.determinant()) > 1e-8
    data = {"Origin": matrix.translation,
            **{f"{axis} Axis": linear.col[i] for i, axis in enumerate("XYZ")}}
    for kind in ("POINT", "VECTOR", "NORMAL"):
        for direction in ("TO_BASIS", "FROM_BASIS", "ROUNDTRIP"):
            if not valid:
                result = VALUE
            elif direction == "ROUNDTRIP":
                result = VALUE.normalized() if kind == "NORMAL" else VALUE
            elif kind == "POINT":
                result = (matrix.inverted() if direction == "TO_BASIS" else matrix) @ VALUE
            elif kind == "NORMAL":
                normal_matrix = linear.transposed() if direction == "TO_BASIS" else linear.inverted().transposed()
                result = (normal_matrix @ VALUE).normalized()
            else:
                result = (linear.inverted() if direction == "TO_BASIS" else linear) @ VALUE
            data[f"{kind}_{direction}"] = result
    data["FallbackPass"] = linear.inverted() @ VALUE if valid else VALUE
    data["FallbackZero"] = linear.inverted() @ VALUE if valid else Vector((0, 0, 0))
    data["LegacyPosition"] = matrix.translation if obj and obj.data.type != "SUN" else Vector((0, 0, 0))
    data["LegacyDirection"] = -linear.col[2].normalized() if obj and obj.data.type != "POINT" else Vector((0, 0, 0))
    data["ParameterOrigin"] = Vector(obj.data.shader_parameters[0].value_vector3) if obj else Vector((0, 0, 0))
    return {key: list(value) for key, value in data.items()}


def compare(actual, reference, label):
    assert all(math.isfinite(x) for x in actual), (label, actual)
    error = max(abs(a - e) for a, e in zip(actual, reference))
    assert all(abs(a - e) <= 5e-4 + 1e-4 * abs(e) for a, e in zip(actual, reference)), (label, actual, reference)
    return error


def check(label, obj):
    reference = expected(obj)
    path = OUT / f"{label}.exr"
    bpy.context.scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)
    image = bpy.data.images.load(str(path), check_existing=False)
    pixels = list(image.pixels[:])
    width = image.size[0]
    maximum = 0
    for index, spec in enumerate(SPECS):
        parts = []
        for part in (0, 1):
            tile = index * 2 + part
            x = (tile % COLS) * TILE + TILE // 2
            y = (tile // COLS) * TILE + TILE // 2
            offset = (y * width + x) * 4
            parts.append(pixels[offset:offset + 3])
        actual = [round(integer) - 16 + fraction for integer, fraction in zip(*parts)]
        maximum = max(maximum, compare(actual, reference[spec[0]], f"{label}/{spec[0]}"))
    bpy.data.images.remove(image)
    RESULTS.append({"case": label, "max_error": maximum, "expected": reference})
    print(f"LIGHT_INFO_TRANSFORM_PIXEL_OK {label} max_error={maximum:.8g}", flush=True)


def interface_checks():
    info = all_infos()[0]
    assert tuple(s.identifier for s in info.outputs[:9]) == BASIC
    assert tuple(s.identifier for s in info.outputs[9:13]) == FIELDS
    for node in all_infos():
        assert all(node.outputs[name].enabled for name in BASIC + FIELDS)
    assert len([s for s in info.outputs if s.name == "Origin"]) == 2
    assert info.outputs["Origin"].identifier == "Origin"
    assert next(p for p in info.panel_states if p.identifier == 3).is_collapsed
    info.mode = "PARAMETER"
    assert all(info.outputs[field].enabled for field in FIELDS)
    info.mode = "BASIC"


def test_suite(npr=False):
    obj = setup(npr)
    interface_checks()
    obj.location = (0.23, -0.36, 0.47)
    obj.rotation_euler = (0.31, -0.42, 0.58)
    obj.scale = (1.3, 0.8, 1.1)
    for kind in ("POINT", "SUN", "SPOT", "AREA"):
        obj.data.type = kind
        check(f"{'npr' if npr else 'surface'}_{kind.lower()}", obj)
    stamps = {m.name: m.shader_compile_timestamp for m in bpy.data.materials}
    for label, update in (
        ("translation", lambda: setattr(obj, "location", (-0.31, 0.14, 0.72))),
        ("roll", lambda: obj.rotation_euler.rotate_axis("Z", 0.73)),
        ("negative_scale", lambda: setattr(obj, "scale", (-1.2, 0.7, 1.4))),
        ("hidden", lambda: setattr(obj, "hide_render", True)),
    ):
        update()
        check(f"{'npr' if npr else 'surface'}_{label}", obj)
        assert stamps == {m.name: m.shader_compile_timestamp for m in bpy.data.materials}, label
    parent = bpy.data.objects.new("Parent", None)
    bpy.context.scene.collection.objects.link(parent)
    parent.location = (0.2, -0.1, 0.3)
    parent.scale = (1.7, 0.6, 1.1)
    parent.rotation_euler = (0.2, 0.3, -0.4)
    obj.parent = parent
    check("npr_parent_shear" if npr else "surface_parent_shear", obj)
    linear = evaluated_matrix(obj).to_3x3()
    assert abs(linear.col[0].normalized().dot(linear.col[1].normalized())) > 0.05
    target = bpy.data.objects.new("ConstraintTarget", None)
    bpy.context.scene.collection.objects.link(target)
    target.location = (-0.4, 0.3, 0.6)
    constraint = obj.constraints.new("COPY_LOCATION")
    constraint.target = target
    check("npr_constraint" if npr else "surface_constraint", obj)
    parent.keyframe_insert("rotation_euler", frame=1)
    parent.rotation_euler.z += 0.9
    parent.keyframe_insert("rotation_euler", frame=3)
    bpy.context.scene.frame_set(2)
    check("npr_animation" if npr else "surface_animation", obj)
    assert stamps == {m.name: m.shader_compile_timestamp for m in bpy.data.materials}, "parent/constraint/animation recompile"
    shared = light("SharedDataTarget", data=obj.data)
    shared.location = (0.61, 0.22, -0.17)
    shared.rotation_euler = (-0.5, 0.3, 0.8)
    for node in all_infos():
        node.light_object = shared
    check("npr_shared_data" if npr else "surface_shared_data", shared)
    # Re-select the original evaluated constrained/animated transform before saving.
    for node in all_infos():
        node.light_object = obj
    fixture = OUT / ("npr_roundtrip.blend" if npr else "surface_roundtrip.blend")
    bpy.ops.wm.save_as_mainfile(filepath=str(fixture), check_existing=False)
    (OUT / (fixture.stem + ".json")).write_text(json.dumps(expected(obj), indent=2), encoding="utf-8")
    obj.scale = (0, 0, 0)
    check("npr_singular" if npr else "surface_singular", obj)
    for node in all_infos():
        node.light_object = None
    check("npr_missing" if npr else "surface_missing", None)
    for node in all_infos():
        node.light_object = shared
    bpy.data.objects.remove(shared, do_unlink=True)
    check("npr_deleted" if npr else "surface_deleted", None)
    # RNA's pointer type is Object; the shader must also reject non-light records.
    for node in all_infos():
        node.light_object = parent
    check("npr_wrong_type" if npr else "surface_wrong_type", parent)


def reopen(path):
    bpy.ops.wm.open_mainfile(filepath=str(path))
    interface_checks()
    obj = bpy.data.objects["Target"]
    compare_ref = json.loads(path.with_suffix(".json").read_text(encoding="utf-8"))
    for key, value in expected(obj).items():
        compare(value, compare_ref[key], f"persisted/{key}")
    for node in all_infos():
        assert node.light_object == obj
        parameter = next(s for s in node.outputs if s.name == "Origin" and
                         s.identifier.startswith("Shader Parameter "))
        assert parameter.enabled
    check("fresh_process_" + path.stem, obj)


def legacy(path):
    bpy.ops.wm.open_mainfile(filepath=str(path))
    node = bpy.data.materials["LegacyTransform"].node_tree.nodes["LegacyInfo"]
    assert tuple(s.identifier for s in node.outputs[:9]) == BASIC
    assert all(node.outputs[field].enabled for field in FIELDS)
    assert node.outputs["Position"].is_linked
    assert node.outputs["Direction"].is_linked
    parameter = next(s for s in node.outputs if s.identifier.startswith("Shader Parameter ")
                     and s.name == "Origin")
    assert parameter.is_linked
    assert node.light_object == bpy.data.objects["LegacySun"]
    assert tuple(node.light_object.location) == (1.0, 2.0, 3.0)
    print("LIGHT_INFO_TRANSFORM_LEGACY_OK", flush=True)


def main():
    global ARGS, OUT
    ARGS = args()
    OUT = Path(ARGS.output_dir).resolve()
    OUT.mkdir(parents=True, exist_ok=True)
    if ARGS.legacy:
        legacy(Path(ARGS.legacy).resolve())
    elif ARGS.reopen:
        reopen(Path(ARGS.reopen).resolve())
    else:
        test_suite()
        test_suite(npr=True)
    import gpu
    gpu.init()
    (OUT / "results.json").write_text(json.dumps({"backend": gpu.platform.backend_type_get(),
                                                  "cases": RESULTS}, indent=2), encoding="utf-8")
    print("LIGHT_INFO_TRANSFORM_OK", flush=True)


if __name__ == "__main__":
    main()
