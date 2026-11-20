"""Persistent GPU viewport updates, image interpolation, and group instance identity."""
import argparse
import json
from pathlib import Path
import sys
import bpy
import gpu
import numpy as np
sys.path.insert(0,str(Path(__file__).parent))
from test_geometry_line_brush_regions import *

def viewport(scene,tree,out):
    area=next(a for a in bpy.context.screen.areas if a.type=='VIEW_3D')
    space=area.spaces.active;space.shading.type='RENDERED';space.overlay.show_overlays=False
    space.shading.use_scene_world_render=space.shading.use_scene_lights_render=True
    window_region=next(r for r in area.regions if r.type=='WINDOW')
    active_output(tree).mode='FULL'
    def draw(target,label):
        print('VIEWPORT_STAGE '+label,flush=True)
        bpy.context.view_layer.update()
        projection=scene.camera.calc_matrix_camera(bpy.context.evaluated_depsgraph_get(),x=192,y=192)
        target.draw_view3d(scene,bpy.context.view_layer,space,window_region,
                           scene.camera.matrix_world.inverted(),projection,
                           do_color_management=False,draw_background=False)
        pixels=np.asarray(target.texture_color.read(),dtype=np.float32).reshape(192,192,4).copy()
        require(np.isfinite(pixels).all(),'Viewport must contain finite pixels')
        np.save(out/(label+'.npy'),pixels)
        return pixels
    return draw

def main(out):
    gpu.init();results={}
    obj,tree,quick,settings,start,end=line_scene();scene=bpy.context.scene
    tree.links.new(end.outputs[0],socket(settings,'visible_brush'));set_value(end,'width',8)
    image=bpy.data.images.new('Mutable image',1,1,alpha=True,float_buffer=True)
    image.colorspace_settings.name='Non-Color';image.pixels[:]=(1,0,0,1);image.update()
    texture=tree.nodes.new('ShaderNodeTexImage');texture.image=image
    tree.links.new(texture.outputs['Color'],socket(end,'color'))
    target=gpu.types.GPUOffScreen(192,192,format='RGBA32F');draw=viewport(scene,tree,out)
    try:
        base=draw(target,'image-red')
        image.pixels[:]=(0,1,0,1);image.update()
        changed=draw(target,'image-green')
        require(np.abs(base-changed).sum()>100,'Image pixel update must affect the same viewport engine')
        require(changed[:,:,1].sum()>changed[:,:,0].sum()*5,'Updated image must render green')
        fresh=gpu.types.GPUOffScreen(192,192,format='RGBA32F')
        try:require(np.abs(draw(fresh,'image-cold')-changed).max()<.003,'Image hot update must match a fresh engine')
        finally:fresh.free()
        tree.links.new(texture.outputs['Alpha'],socket(end,'width'))
        image.pixels[:]=(0,1,0,.25);image.update();thin=draw(target,'image-width-thin')
        image.pixels[:]=(0,1,0,1);image.update();thick=draw(target,'image-width-thick')
        require(np.abs(thin-thick).sum()>20,'Width image contents must invalidate expanded instances')
        results['persistent_image_color_and_width_updates']='passed'
    finally:target.free()

    # Same material node tree and same viewport engine must observe both AOV values and write validity.
    reset();scene=bpy.context.scene
    obj=mesh_object('AOV persistent source',[(-1.5,-.8,0),(1.5,-.8,0),(1.5,.8,0),(-1.5,.8,0)],faces=[(0,1,2,3)])
    tree=new_tree();start,end=region(tree);set_value(end,'width',4)
    settings=make_settings(tree,make_source(tree,[obj]),end,('outline',));tree.links.new(settings.outputs[0],socket(active_output(tree),'plan'))
    material=obj.data.materials[0]
    for name,kind in [('ColorAOV','COLOR'),('WidthAOV','VALUE')]:
        entry=bpy.context.view_layer.aovs.add();entry.name=name;entry.type=kind
    color_writer=material.node_tree.nodes.new('ShaderNodeOutputAOV');color_writer.aov_name='ColorAOV';color_writer.inputs['Color'].default_value=(1,0,0,1)
    width_writer=material.node_tree.nodes.new('ShaderNodeOutputAOV');width_writer.aov_name='WidthAOV';width_writer.inputs['Value'].default_value=2
    reader=tree.nodes.new('NPRLineNodeMaterialAOV');set_value(reader,'name','ColorAOV');set_value(reader,'color',(1,0,1,1))
    tree.links.new(reader.outputs['Color'],socket(end,'color'))
    width=tree.nodes.new('NPRLineNodeMaterialAOV');width.mode='VALUE';set_value(width,'name','WidthAOV');set_value(width,'value',10)
    tree.links.new(width.outputs['Value'],socket(end,'width'))
    target=gpu.types.GPUOffScreen(192,192,format='RGBA32F');draw=viewport(scene,tree,out)
    try:
        base=draw(target,'aov-red-width2')
        color_writer.inputs['Color'].default_value=(0,1,0,1)
        changed=draw(target,'aov-green-width2')
        # Flat gray surface is present. Test outline columns outside its coverage.
        require(np.abs(base-changed).sum()>100,'Material AOV edits must repaint a persistent viewport')
        color_writer.inputs['Color'].default_value=(0,0,0,1)
        black=draw(target,'aov-legitimate-black')
        require(np.abs(black[:,:,0]-black[:,:,1]).max()<.003 and
                np.abs(black[:,:,2]-black[:,:,1]).max()<.003,
                'A written black AOV must remain a value, not use the magenta fallback')
        color_writer.inputs['Color'].default_value=(0,1,0,1)
        width_writer.inputs['Value'].default_value=7
        wider=draw(target,'aov-green-width7')
        require(wider[:,:,3].sum()>changed[:,:,3].sum()+200,'AOV width edits must re-expand a persistent viewport')
        width_writer.inputs['Value'].default_value=0
        zero=draw(target,'aov-legitimate-zero-width')
        require((zero[:,:,1]-zero[:,:,0]).max()<.003,
                'A written zero width must remove the green ink rather than use fallback width')
        width_writer.inputs['Value'].default_value=7
        fresh=gpu.types.GPUOffScreen(192,192,format='RGBA32F')
        try:require(np.abs(draw(fresh,'aov-width-cold')-wider).max()<.006,'AOV hot edits must match a fresh engine')
        finally:fresh.free()
        material.node_tree.nodes.remove(color_writer)
        missing=draw(target,'aov-unwritten-fallback')
        require(np.abs(missing-wider).sum()>100,'AOV writer removal must clear validity immediately')
        results['persistent_aov_color_width_write_validity_updates']='passed'
    finally:target.free()

    obj,tree,quick,settings,start,end=line_scene()
    tree.links.new(end.outputs[0],socket(settings,'visible_brush'))
    group=bpy.data.node_groups.new('Instance field','NPRLineNodeTree')
    group.interface.new_socket(name='Color',in_out='INPUT',socket_type='NodeSocketColor')
    group.interface.new_socket(name='Color',in_out='OUTPUT',socket_type='NodeSocketColor')
    gi=group.nodes.new('NodeGroupInput');go=group.nodes.new('NodeGroupOutput');reroute=group.nodes.new('NodeReroute')
    group.links.new(gi.outputs[0],reroute.inputs[0]);group.links.new(reroute.outputs[0],go.inputs[0])
    instances=[]
    for col in ((.8,.2,.1,1),(.1,.7,.4,1)):
        instance=tree.nodes.new('NPRLineNodeGroup');instance.node_tree=group;instance.inputs[0].default_value=col;instances.append(instance)
    mix=tree.nodes.new('ShaderNodeMix');mix.data_type='RGBA';mix.inputs[0].default_value=.25
    for i,instance in enumerate(instances):tree.links.new(instance.outputs[0],mix.inputs[i+6])
    tree.links.new(mix.outputs[2],socket(end,'color'))
    for blend in ('MIX','MULTIPLY','ADD','SUBTRACT','SCREEN'):
        mix.blend_type=blend
        p=ink(out,'group-mix-'+blend);mask=p[:,:,3]>.9
        a=np.array((.8,.2,.1));b=np.array((.1,.7,.4));factor=.25
        mixed={'MIX':b,'MULTIPLY':a*b,'ADD':a+b,'SUBTRACT':a-b,'SCREEN':1-(1-a)*(1-b)}[blend]
        expected=a*(1-factor)+mixed*factor
        require(np.max(np.abs(p[:,:,:3][mask]/p[:,:,3][mask,None]-expected))<.004,'Mix mode and per-instance group inputs must evaluate correctly '+blend)
    results['independent_group_instances_reroutes_mix_blends']='passed'
    texture=tree.nodes.new('ShaderNodeTexImage');texture.extension='EXTEND'
    texture.inputs[0].default_value=(.3,.5,0)
    image=bpy.data.images.new('Interpolation source',2,2,alpha=True,float_buffer=True)
    image.colorspace_settings.name='Non-Color'
    image.pixels.foreach_set(np.array([1,0,0,1,0,0,1,1]*2,dtype=np.float32));image.update();texture.image=image
    tree.links.new(texture.outputs['Color'],socket(end,'color'))
    sampled={}
    for interpolation in ('Closest','Linear','Cubic','Smart'):
        texture.interpolation=interpolation
        pixels=ink(out,'interpolation-'+interpolation);mask=pixels[:,:,3]>.9
        sampled[interpolation]=np.median(pixels[:,:,:3][mask]/pixels[:,:,3][mask,None],axis=0)
    require(np.max(np.abs(sampled['Closest']-(1,0,0)))<.003,'Closest must select the first source texel')
    require(np.max(np.abs(sampled['Linear']-(.9,0,.1)))<.004,'Linear must use bilinear source interpolation')
    require(np.max(np.abs(sampled['Cubic']-sampled['Smart']))<.003,'Smart must use the available cubic GPU sampling')
    require(sampled['Cubic'][2]>.03 and sampled['Cubic'][2]<.4,'Cubic must sample the adjacent source texel')
    results['image_closest_linear_cubic_smart']='passed'
    (out/'results.json').write_text(json.dumps(results,indent=2),encoding='utf8')
    print('BRUSH_REGION_VIEWPORT_OK '+json.dumps(results),flush=True)

if __name__ == '__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--output',required=True)
    out=Path(parser.parse_args(sys.argv[sys.argv.index('--')+1:]).output);out.mkdir(parents=True,exist_ok=True)
    main(out)
