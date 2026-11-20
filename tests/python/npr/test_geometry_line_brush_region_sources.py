"""Additional real-pixel acceptance for source tracking and color composition."""
import argparse
import json
from pathlib import Path
import sys
import bpy
import numpy as np
sys.path.insert(0, str(Path(__file__).parent))
from test_geometry_line_brush_regions import *

def composition(out):
    reset()
    obj = mesh_object('Crossing stroke', [(-1.4,-1,0),(1.4,1,0),(-1.4,1,0),(1.4,-1,0)],
                      edges=[(0,1),(1,2),(2,3)])
    tree=new_tree(); start,end=region(tree)
    settings=make_settings(tree,make_source(tree,[obj]),end,('wire',))
    set_value(settings,'split_angle',180)
    output=active_output(tree);tree.links.new(settings.outputs[0],socket(output,'plan'))
    set_value(end,'width',7)
    info=stroke_info(tree)
    ramp=tree.nodes.new('ShaderNodeValToRGB');ramp.color_ramp.interpolation='CONSTANT'
    ramp.color_ramp.elements[0].color=(1,0,0,1)
    ramp.color_ramp.elements[1].position=.5;ramp.color_ramp.elements[1].color=(0,0,1,1)
    tree.links.new(info.outputs['Factor'],ramp.inputs[0]);tree.links.new(ramp.outputs[0],socket(end,'color'))
    opacity=tree.nodes.new('ShaderNodeMapRange')
    opacity.inputs['To Min'].default_value=.25;opacity.inputs['To Max'].default_value=.9
    tree.links.new(info.outputs['Factor'],opacity.inputs['Value']);tree.links.new(opacity.outputs[0],socket(end,'opacity'))
    for mode in (('SIMPLE','NORMAL','MULTIPLE') if hasattr(start, 'mode') else ('SIMPLE',)):
        if hasattr(start, 'mode'): start.mode=mode
        for samples in (1,4):
            set_value(output,'samples',samples)
            pixels=ink(out,f'composition-{mode}-{samples}')
            mask=pixels[:,:,3]>.02
            require(mask.sum()>500,'Crossing fixture must contain ink')
            require(np.max(np.abs(pixels[:,:,:3].sum(2)-pixels[:,:,3]))<.006,
                    'Color and coverage must belong to the same winning stroke')
            require(np.max(np.minimum(pixels[84:108,84:108,0],pixels[84:108,84:108,2]))<.006,
                    'Crossing strokes must not independently MAX RGB into magenta')
            ramp.color_ramp.elements[1].color=(1,0,0,1)
            constant=ink(out,f'composition-reference-{mode}-{samples}')
            require(np.max(np.abs(pixels[:,:,3]-constant[:,:,3]))<.003,
                    'Dynamic color cannot change opacity or coverage')
            ramp.color_ramp.elements[1].color=(0,0,1,1)
    # Guide-driven camera effects must preserve premultiplied color and alpha together.
    scene=bpy.context.scene
    if hasattr(start, 'mode'): start.mode='SIMPLE'
    set_value(output,'samples',2)
    scene.eevee.taa_render_samples=4
    camera=scene.camera.data;camera.type='PERSP';camera.lens=50
    camera.dof.use_dof=True;camera.dof.focus_distance=2;camera.dof.aperture_fstop=.5
    set_value(output,'depth_of_field',True)
    blurred,raw=render(out,'composition-dof')
    require(np.count_nonzero((raw[:,:,3]<.01)&(blurred[:,:,3]>.01))>30,'Dynamic color must consume line depth guides')
    require(np.max(np.abs(raw[:,:,:3].sum(2)-raw[:,:,3]))<.006,'DOF must preserve the unprocessed coherent color/coverage pass')
    # Native DOF stabilizes YCoCg history and clamps alpha independently. Its resolve is
    # approximate; require a valid premultiplied result rather than an exact linear identity.
    error=float(np.abs(blurred[:,:,:3].sum(2)-blurred[:,:,3]).max())
    print('NATIVE_DOF_COLOR_ALPHA_MAX_ERROR',error,flush=True)
    require(blurred[:,:,1].max()<.003 and error<.04,
            'Native DOF must retain bounded coherent color and coverage')
    set_value(output,'depth_of_field',False);camera.dof.use_dof=False
    scene.render.use_motion_blur=True;scene.render.motion_blur_shutter=.8;scene.eevee.motion_blur_steps=1
    obj.location.x=-.7;obj.keyframe_insert('location',frame=1)
    obj.location.x=.7;obj.keyframe_insert('location',frame=3);scene.frame_set(2)
    set_value(output,'motion_blur',True)
    moving,raw=render(out,'composition-motion')
    require(np.count_nonzero((raw[:,:,3]<.01)&(moving[:,:,3]>.01))>30,'Dynamic color must consume line motion guides')
    require(np.max(np.abs(moving[:,:,:3].sum(2)-moving[:,:,3]))<.015,'Motion must accumulate color and alpha from the same samples')
    return {'dynamic_composition_modes_samples_depth_motion':'passed'}

def source_tracking(out):
    reset()
    obj=mesh_object('UV Surface',[(-1.5,-.8,0),(1.5,-.8,0),(1.5,.8,0),(-1.5,.8,0)],faces=[(0,1,2,3)])
    uv=obj.data.uv_layers.new(name='UV')
    attr=obj.data.color_attributes.new(name='SourceColor',type='FLOAT_COLOR',domain='CORNER')
    for item,col,coordinate in zip(uv.data,attr.data,((0,0),(1,0),(1,1),(0,1))):
        item.uv=coordinate;col.color=(1-coordinate[0],0,coordinate[0],1)
    tree=new_tree();start,end=region(tree);set_value(end,'width',3)
    settings=make_settings(tree,make_source(tree,[obj]),end,('outline',));set_value(settings,'split_angle',180)
    tree.links.new(settings.outputs[0],socket(active_output(tree),'plan'))
    uvnode=tree.nodes.new('NPRLineNodeUVMap');set_value(uvnode,'name','UV')
    texture=tree.nodes.new('ShaderNodeTexImage');texture.extension='EXTEND'
    image=bpy.data.images.new('Linear red blue',64,8,alpha=True,float_buffer=True);image.colorspace_settings.name='Non-Color'
    data=np.zeros((8,64,4),np.float32);data[:,:,0]=1-(np.arange(64)+.5)/64;data[:,:,2]=(np.arange(64)+.5)/64;data[:,:,3]=1
    image.pixels.foreach_set(data.reshape(-1));image.update();texture.image=image
    tree.links.new(uvnode.outputs['UV'],texture.inputs[0])
    attribute=tree.nodes.new('NPRLineNodeColorAttribute');set_value(attribute,'name','SourceColor')
    for label,node in [('uv',texture),('attribute',attribute)]:
        tree.links.new(node.outputs['Color'],socket(end,'color'))
        set_value(end,'position_offset',(0,0,0));reference=ink(out,label+'-source-base')
        # X advances around the closed contour; all screen-space interior samples should keep their source color.
        set_value(end,'position_offset',(24,0,0));shift=ink(out,label+'-source-x-remap')
        common=(reference[:,:,3]>.8)&(shift[:,:,3]>.8)
        ref=reference[:,:,:3][common]/reference[:,:,3][common,None]
        actual=shift[:,:,:3][common]/shift[:,:,3][common,None]
        require(common.sum()>300 and np.abs(actual-ref).mean()<.03,'X remap must resample the mapped source surface '+label)
        set_value(end,'position_offset',(0,10,0));shift=ink(out,label+'-source-y-offset')
        # Horizontal edges retain U (and thus R/B) after moving off the mesh surface.
        for x in (50,75,115,140):
            ink_pixels=shift[:,x,:];mask=ink_pixels[:,3]>.8
            ratios=ink_pixels[mask,2]/ink_pixels[mask,3]
            require(len(ratios)>0 and abs(float(np.median(ratios))-(x+.5-24)/144)<.06,
                    'Y offset must retain source U and color '+label)
    return {'uv_attribute_x_remap_y_source_tracking':'passed'}

def material_boundaries(out):
    reset()
    obj=mesh_object('Two material source',[(-1.5,-.8,0),(0,-.8,0),(1.5,-.8,0),(-1.5,.8,0),(0,.8,0),(1.5,.8,0)],
                    faces=[(0,1,4,3),(1,2,5,4)])
    other=obj.data.materials[0].copy();other.name='Right Source';obj.data.materials.append(other);obj.data.polygons[1].material_index=1
    aov=bpy.context.view_layer.aovs.add();aov.name='SourceAOV';aov.type='COLOR'
    for mat,color in zip(obj.data.materials,((1,0,0,1),(0,0,1,1))):
        writer=mat.node_tree.nodes.new('ShaderNodeOutputAOV');writer.aov_name='SourceAOV';writer.inputs['Color'].default_value=color
    tree=new_tree();start,end=region(tree);set_value(end,'width',4)
    settings=make_settings(tree,make_source(tree,[obj]),end,('outline','material'))
    set_value(settings,'object_contours',True);tree.links.new(settings.outputs[0],socket(active_output(tree),'plan'))
    reader=tree.nodes.new('NPRLineNodeMaterialAOV');set_value(reader,'name','SourceAOV');set_value(reader,'color',(0,1,0,1))
    tree.links.new(reader.outputs['Color'],socket(end,'color'))
    pixels=ink(out,'aov-material-boundaries')
    require(pixels[:,:75,0].sum()>pixels[:,:75,2].sum()*8,'Left material identity must remain red')
    require(pixels[:,115:,2].sum()>pixels[:,115:,0].sum()*8,'Right material identity must remain blue')
    require(pixels[:,:,1].sum()<10,'Material seams cannot lose written AOV validity')
    blocker=mesh_object('Partial foreground',[(-.6,-2,.5),(.6,-2,.5),(.6,2,.5),(-.6,2,.5)],faces=[(0,1,2,3)])
    writer=blocker.data.materials[0].node_tree.nodes.new('ShaderNodeOutputAOV');writer.aov_name='SourceAOV';writer.inputs['Color'].default_value=(1,1,0,1)
    set_value(settings,'hidden_enabled',True);tree.links.new(end.outputs[0],socket(settings,'hidden_brush'))
    pixels=ink(out,'aov-partial-foreground')
    require(pixels[:,80:112,1].sum()>60,'Hidden part must use explicit fallback')
    require(pixels[:,:60,1].sum()<3 and pixels[:,132:,1].sum()<3,'Visible parts must keep their own AOV near occlusion')
    return {'aov_same_object_material_seams_partial_occlusion':'passed'}

def style_interfaces(out):
    obj,tree,quick,settings,start,end=line_scene()
    tree.links.new(end.outputs[0],socket(settings,'visible_brush'))
    set_value(end,'color',(0,.8,.1,1));set_value(quick,'color',(0,.8,.1,1))
    reference=ink(out,'interface-visible')
    settings.visible_overrides={'WIRE'}
    tree.links.new(end.outputs[0],socket(settings,'visible_override_wire'))
    set_value(end,'width',6);set_value(quick,'width',6)
    specific=ink(out,'interface-visible-override')
    tree.links.new(quick.outputs[0],socket(settings,'visible_override_wire'))
    require(np.abs(specific-ink(out,'interface-visible-quick')).max()<.003,'Region must replace per-type visible Brush directly')
    mesh_object('Hidden blocker',[(-2,-2,.5),(2,-2,.5),(2,2,.5),(-2,2,.5)],faces=[(0,1,2,3)])
    set_value(settings,'hidden_enabled',True);tree.links.new(end.outputs[0],socket(settings,'hidden_brush'))
    settings.hidden_overrides={'WIRE'};tree.links.new(end.outputs[0],socket(settings,'hidden_override_wire'))
    hidden=ink(out,'interface-hidden-override')
    tree.links.new(quick.outputs[0],socket(settings,'hidden_override_wire'))
    require(np.abs(hidden-ink(out,'interface-hidden-quick')).max()<.003,'Region must replace per-type hidden Brush directly')
    require(hidden[:,:,3].sum()>reference[:,:,3].sum(),'Hidden override must draw the requested wider line')
    tree.links.new(end.outputs[0],socket(settings,'hidden_override_wire'))
    if start.bl_idname == 'NPRLineNodeBrushClosureInput':
        return {'visible_hidden_per_type_interface_interchange': 'passed'}
    for node in (start,quick):
        set_value(node,'dashed',True)
        set_value(node,'dash_length',7);set_value(node,'dash_space',5)
        set_value(node,'taper_start',.2);set_value(node,'taper_end',.2)
    dashed=ink(out,'interface-dashed-taper')
    tree.links.new(quick.outputs[0],socket(settings,'hidden_override_wire'))
    require(np.abs(dashed-ink(out,'interface-dashed-quick')).max()<.003,'Region must inherit dashes and end taper')
    return {'visible_hidden_per_type_interface_interchange_dash_taper':'passed'}

def image_borders(out):
    obj,tree,quick,settings,start,end=line_scene()
    tree.links.new(end.outputs[0],socket(settings,'visible_brush'))
    image=bpy.data.images.new('Periodic Border Colors',2,1,alpha=True,float_buffer=True)
    image.colorspace_settings.name='Non-Color'
    image.pixels.foreach_set([1,0,0,1,0,0,1,1]);image.update()
    texture=tree.nodes.new('ShaderNodeTexImage');texture.image=image
    tree.links.new(texture.outputs['Color'],socket(end,'color'))
    for interpolation in ('Linear','Cubic','Smart'):
        texture.interpolation=interpolation
        texture.extension='REPEAT'
        for u in (0,1):
            texture.inputs['Vector'].default_value=(u,.5,0)
            actual=ink(out,f'image-repeat-border-{interpolation}-{u}')
            require(np.abs(actual[95,96,:3]-np.array([.5,0,.5])).max()<.003,
                    'Image filtering must wrap neighboring texels across a repeat seam')
    for extension,blue in (('EXTEND',1/48),('MIRROR',1/24)):
        texture.interpolation='Cubic';texture.extension=extension
        texture.inputs['Vector'].default_value=(0,.5,0)
        actual=ink(out,'image-cubic-border-'+extension)
        require(np.abs(actual[95,96,:3]-np.array([1-blue,0,blue])).max()<.003,
                'Cubic border taps must honor the selected extension independently')
    return {'image_repeat_linear_cubic_smart_and_mirror_borders':'passed'}


if __name__ == '__main__':
    args=argparse.ArgumentParser();args.add_argument('--output',required=True)
    out=Path(args.parse_args(sys.argv[sys.argv.index('--')+1:]).output);out.mkdir(parents=True,exist_ok=True)
    results={}
    for function in (composition,source_tracking,material_boundaries,style_interfaces,image_borders):
        results.update(function(out))
    (out/'results.json').write_text(json.dumps(results,indent=2),encoding='utf8')
    print('BRUSH_REGION_SOURCES_OK '+json.dumps(results),flush=True)
