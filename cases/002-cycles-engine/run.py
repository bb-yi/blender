import bpy

bpy.context.scene.render.engine = 'CYCLES'
assert bpy.context.scene.render.engine == 'CYCLES', "Cycles render engine is not available"
print('CYCLES_OK=True')
