import bpy

assert bpy.context.scene is not None, "Blender startup did not create an active scene"
print('STARTUP_OK=1')
