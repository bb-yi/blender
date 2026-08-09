import bpy


IMAGE_SIZE = 32


def configure_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    assert scene.render.engine == "CYCLES", "Cycles render engine is not available"
    scene.cycles.samples = 1
    scene.cycles.use_denoising = False
    scene.render.bake.use_pass_direct = False
    scene.render.bake.use_pass_indirect = False
    scene.render.bake.use_pass_color = True


def create_uv_plane():
    mesh = bpy.data.meshes.new("BakePlaneMesh")
    verts = [(-1.0, -1.0, 0.0), (1.0, -1.0, 0.0), (1.0, 1.0, 0.0), (-1.0, 1.0, 0.0)]
    faces = [(0, 1, 2, 3)]
    mesh.from_pydata(verts, [], faces)
    mesh.update()

    uv_layer = mesh.uv_layers.new(name="UVMap")
    for loop, uv in zip(uv_layer.data, [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]):
        loop.uv = uv

    obj = bpy.data.objects.new("BakePlane", mesh)
    bpy.context.scene.collection.objects.link(obj)
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    return obj


def create_bake_material(image):
    material = bpy.data.materials.new("BakeRedMaterial")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    principled = nodes.new("ShaderNodeBsdfPrincipled")
    principled.inputs["Base Color"].default_value = (1.0, 0.05, 0.02, 1.0)
    principled.inputs["Roughness"].default_value = 0.5
    image_node = nodes.new("ShaderNodeTexImage")
    image_node.image = image
    nodes.active = image_node
    links.new(principled.outputs["BSDF"], output.inputs["Surface"])
    return material


configure_scene()
target_image = bpy.data.images.new("CyclesBakeTarget", IMAGE_SIZE, IMAGE_SIZE, alpha=True, float_buffer=True)
plane = create_uv_plane()
plane.data.materials.append(create_bake_material(target_image))

bpy.ops.object.bake(type="DIFFUSE", pass_filter={"COLOR"}, margin=0, use_clear=True)

pixels = list(target_image.pixels[:])
valid_pixels = []
for index in range(0, len(pixels), 4):
    rgba = pixels[index:index + 4]
    if rgba[3] > 0.5:
        valid_pixels.append(rgba)

assert len(valid_pixels) > (IMAGE_SIZE * IMAGE_SIZE) // 2, (
    f"Expected bake to fill most of the image, got {len(valid_pixels)} valid pixels"
)

mean_red = sum(pixel[0] for pixel in valid_pixels) / len(valid_pixels)
mean_green = sum(pixel[1] for pixel in valid_pixels) / len(valid_pixels)
mean_blue = sum(pixel[2] for pixel in valid_pixels) / len(valid_pixels)

print(f"CYCLES_BAKE_VALID_PIXELS={len(valid_pixels)}")
print(f"CYCLES_BAKE_MEAN_RGB={mean_red:.6f},{mean_green:.6f},{mean_blue:.6f}")

assert mean_red > 0.45, f"Expected red bake result, got mean red {mean_red:.6f}"
assert mean_red > mean_green + 0.25, (
    f"Expected red to dominate green after bake, got R={mean_red:.6f} G={mean_green:.6f}"
)
assert mean_red > mean_blue + 0.25, (
    f"Expected red to dominate blue after bake, got R={mean_red:.6f} B={mean_blue:.6f}"
)
