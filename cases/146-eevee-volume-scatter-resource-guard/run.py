from pathlib import Path
import struct

import bpy


CASE_DIR = Path(__file__).resolve().parent
OUT_DIR = CASE_DIR / "out"
OUTPUT_PATH = OUT_DIR / "volume_scatter_resource_guard.png"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    scene = bpy.context.scene
    require(scene is not None, "No active scene")
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 16
    scene.render.resolution_y = 16
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.film_transparent = False
    scene.render.filepath = str(OUTPUT_PATH)

    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)

    bpy.ops.object.camera_add(location=(0.0, 0.0, 5.0))
    camera = bpy.context.object
    scene.camera = camera

    world = scene.world or bpy.data.worlds.new("VolumeScatterResourceGuardWorld")
    scene.world = world
    world.use_nodes = True
    nodes = world.node_tree.nodes
    links = world.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputWorld")
    background = nodes.new("ShaderNodeBackground")
    background.inputs["Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    background.inputs["Strength"].default_value = 0.0
    absorption = nodes.new("ShaderNodeVolumeAbsorption")
    absorption.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    absorption.inputs["Density"].default_value = 0.1
    links.new(background.outputs["Background"], output.inputs["Surface"])
    links.new(absorption.outputs["Volume"], output.inputs["Volume"])

    require(len(bpy.data.objects) == 1, "The guard scene must contain only its camera")
    require(not any(obj.type == "LIGHT" for obj in bpy.data.objects), "Unexpected light in guard scene")
    require(not any(node.bl_idname == "ShaderNodeVolumeScatter" for node in nodes),
            "Unexpected volume scatter node in guard scene")

    bpy.ops.render.render(write_still=True)
    require(OUTPUT_PATH.is_file(), f"Render output was not written: {OUTPUT_PATH}")
    with OUTPUT_PATH.open("rb") as stream:
        require(stream.read(8) == b"\x89PNG\r\n\x1a\n", "Render output is not a PNG")
        chunk_length = struct.unpack(">I", stream.read(4))[0]
        chunk_type = stream.read(4)
        require(chunk_type == b"IHDR" and chunk_length >= 8, "PNG has no valid IHDR chunk")
        width, height = struct.unpack(">II", stream.read(8))
    require((width, height) == (16, 16), f"Unexpected output size: {(width, height)}")
    print("EEVEE_VOLUME_SCATTER_RESOURCE_GUARD_OK", flush=True)


if __name__ == "__main__":
    main()
