# SPDX-FileCopyrightText: 2018-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.app.translations import contexts as i18n_contexts
from bpy.types import Panel
from rna_prop_ui import PropertyPanel
from bl_ui.space_properties import PropertiesAnimationMixin


class DataButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "data"

    @classmethod
    def poll(cls, context):
        engine = context.engine
        return context.light and (engine in cls.COMPAT_ENGINES)


class DATA_PT_context_light(DataButtonsPanel, Panel):
    bl_label = ""
    bl_options = {'HIDE_HEADER'}
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH',
    }

    def draw(self, context):
        layout = self.layout

        ob = context.object
        light = context.light
        space = context.space_data

        if ob:
            layout.template_ID(ob, "data")
        elif light:
            layout.template_ID(space, "pin_id")


class DATA_PT_preview(DataButtonsPanel, Panel):
    bl_label = "Preview"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE',
    }

    def draw(self, context):
        self.layout.template_preview(context.light)


class DATA_PT_light(DataButtonsPanel, Panel):
    bl_label = "Light"
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_WORKBENCH'}

    def draw(self, context):
        layout = self.layout

        light = context.light

        # Compact layout for node editor.
        if self.bl_space_type == 'PROPERTIES':
            layout.row().prop(light, "type", expand=True)
            layout.use_property_split = True
        else:
            layout.use_property_split = True
            layout.row().prop(light, "type")


class DATA_PT_EEVEE_light(DataButtonsPanel, Panel):
    bl_label = "Light"
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    def draw(self, context):
        layout = self.layout
        light = context.light

        # Compact layout for node editor.
        if self.bl_space_type == 'PROPERTIES':
            layout.row().prop(light, "type", expand=True)
            layout.use_property_split = True
        else:
            layout.use_property_split = True
            layout.row().prop(light, "type")

        col = layout.column()
        heading = col.column(align=True, heading="Temperature")
        row = heading.column(align=True).row(align=True)
        row.use_property_decorate = False
        row.prop(light, "use_temperature", text="")
        # Don't show color preview for now, it is grayed out so the color
        # is not accurate. Would not a change in the UI code to allow
        # non-editable colors to be displayed as is.
        if False:  # light.use_temperature:
            sub = row.split(factor=0.7, align=True)
            sub.active = light.use_temperature
            sub.prop(light, "temperature", text="")
            sub.prop(light, "temperature_color", text="")
        else:
            sub = row.row()
            sub.active = light.use_temperature
            sub.prop(light, "temperature", text="")
            row.prop_decorator(light, "temperature")

        if light.use_temperature:
            col.prop(light, "color", text="Tint")
        else:
            col.prop(light, "color", text="Color")

        layout.separator()

        col = layout.column()
        col.prop(light, "energy")
        col.prop(light, "exposure")
        col.prop(light, "normalize")
        col.prop(light, "lightgroup_id", text="Lightgroup ID")

        layout.separator()

        col = layout.column()
        if light.type in {'POINT', 'SPOT'}:
            col.prop(light, "shadow_soft_size", text="Radius")
            col.prop(light, "use_soft_falloff")
        elif light.type == 'SUN':
            col.prop(light, "angle")
        elif light.type == 'AREA':
            col.prop(light, "shape")

            sub = col.column(align=True)

            if light.shape in {'SQUARE', 'DISK'}:
                sub.prop(light, "size")
            elif light.shape in {'RECTANGLE', 'ELLIPSE'}:
                sub.prop(light, "size", text="Size X")
                sub.prop(light, "size_y", text="Y")


class DATA_PT_EEVEE_light_distance(DataButtonsPanel, Panel):
    bl_label = "Custom Distance"
    bl_parent_id = "DATA_PT_EEVEE_light"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    @classmethod
    def poll(cls, context):
        light = context.light
        engine = context.engine

        return (light and light.type != 'SUN') and (engine in cls.COMPAT_ENGINES)

    def draw_header(self, context):
        light = context.light

        layout = self.layout
        layout.prop(light, "use_custom_distance", text="")

    def draw(self, context):
        layout = self.layout
        light = context.light
        layout.active = light.use_custom_distance
        layout.use_property_split = True

        layout.prop(light, "cutoff_distance", text="Distance")


class DATA_PT_EEVEE_light_shadow(DataButtonsPanel, Panel):
    bl_label = "Shadow"
    bl_parent_id = "DATA_PT_EEVEE_light"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    def draw_header(self, context):
        light = context.light
        self.layout.prop(light, "use_shadow", text="")

    def draw(self, context):
        layout = self.layout
        light = context.light
        layout.use_property_split = True
        layout.active = context.scene.eevee.use_shadows and light.use_shadow

        col = layout.column(align=False, heading="Jitter")
        row = col.row(align=True)
        sub = row.row(align=True)
        sub.prop(light, "use_shadow_jitter", text="")
        sub = sub.row(align=True)
        sub.active = light.use_shadow_jitter
        sub.prop(light, "shadow_jitter_overblur", text="Overblur")

        col = layout.column()
        col.prop(light, "shadow_filter_radius", text="Filter")

        sub = col.column(align=True)
        row = sub.row(align=True)
        row.prop(light, "shadow_maximum_resolution", text="Resolution Limit")
        if light.type != 'SUN':
            row.prop(light, "use_absolute_resolution", text="", icon='DRIVER_DISTANCE')
        if light.type == 'SUN':
            sub = sub.column(align=True)
            sub.prop(light, "shadow_map_scale", text="Shadow Map Scale")


class DATA_PT_EEVEE_light_influence(DataButtonsPanel, Panel):
    bl_label = "Influence"
    bl_parent_id = "DATA_PT_EEVEE_light"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    def draw(self, context):
        layout = self.layout
        ob = context.object
        light = context.light
        layout.use_property_split = True

        col = layout.column(align=True)

        sub = col.column(align=True)
        sub.active = ob is None or ob.visible_diffuse
        sub.prop(light, "diffuse_factor", text="Diffuse")

        sub = col.column(align=True)
        sub.active = ob is None or ob.visible_glossy
        sub.prop(light, "specular_factor", text="Glossy")

        sub = col.column(align=True)
        sub.active = ob is None or ob.visible_transmission
        sub.prop(light, "transmission_factor", text="Transmission")

        sub = col.column(align=True)
        sub.active = ob is None or ob.visible_volume_scatter
        sub.prop(light, "volume_factor", text="Volume Scatter", text_ctxt=i18n_contexts.id_id)


class DATA_PT_spot(DataButtonsPanel, Panel):
    bl_label = "Beam Shape"
    bl_parent_id = "DATA_PT_EEVEE_light"
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH',
    }

    @classmethod
    def poll(cls, context):
        light = context.light
        engine = context.engine
        return (light and light.type == 'SPOT') and (engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        light = context.light

        col = layout.column()

        col.prop(light, "spot_size", text="Angle")
        col.prop(light, "spot_blend", text="Blend", slider=True)

        col.prop(light, "show_cone")


class DATA_PT_light_animation(DataButtonsPanel, PropertiesAnimationMixin, PropertyPanel, Panel):
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH',
    }

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        # DataButtonsPanel.poll ensures this is not None.
        light = context.light

        col = layout.column(align=True)
        col.label(text="Light")
        self.draw_action_and_slot_selector(context, col, light)

        if node_tree := light.node_tree:
            col = layout.column(align=True)
            col.label(text="Shader Node Tree")
            self.draw_action_and_slot_selector(context, col, node_tree)


_shader_parameter_values = {
    'FLOAT': 'value_float',
    'INT': 'value_int',
    'BOOLEAN': 'value_bool',
    'VECTOR2': 'value_vector2',
    'VECTOR3': 'value_vector3',
    'VECTOR4': 'value_vector4',
    'COLOR': 'value_color',
}


class DATA_UL_light_shader_parameters(bpy.types.UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname):
        row = layout.row(align=True)
        row.prop(item, "name", text="", emboss=False)
        row.prop(item, "type", text="")


class LIGHT_OT_shader_parameter_edit(bpy.types.Operator):
    bl_idname = "light.shader_parameter_edit"
    bl_label = "Edit Light Shader Parameters"
    bl_options = {'REGISTER', 'UNDO'}

    action: bpy.props.EnumProperty(items=(
        ('ADD', "Add", "Add a float parameter"),
        ('REMOVE', "Remove", "Remove the parameter and its local drivers; preserve shared Actions"),
        ('DUPLICATE', "Duplicate", "Duplicate values without copying animation"),
        ('UP', "Move Up", "Move the selected parameter up"),
        ('DOWN', "Move Down", "Move the selected parameter down"),
    ))

    @classmethod
    def poll(cls, context):
        # Operator search also polls from editors without the Properties context members.
        light = getattr(context, "light", None)
        return light is not None and light.is_editable and light.override_library is None

    def execute(self, context):
        light = getattr(context, "light", None)
        if light is None:
            return {'CANCELLED'}
        parameters = light.shader_parameters
        index = light.active_shader_parameter_index
        if self.action == 'ADD':
            parameters.new("Parameter")
        elif 0 <= index < len(parameters):
            parameter = parameters[index]
            if self.action == 'REMOVE':
                parameters.remove(parameter)
            elif self.action == 'DUPLICATE':
                parameters.duplicate(parameter)
            else:
                target = index + (-1 if self.action == 'UP' else 1)
                if 0 <= target < len(parameters):
                    parameters.move(index, target)
        return {'FINISHED'}


class DATA_PT_EEVEE_shader_parameters(DataButtonsPanel, Panel):
    bl_label = "Shader Parameters"
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    def draw(self, context):
        layout = self.layout
        light = context.light
        row = layout.row()
        row.template_list("DATA_UL_light_shader_parameters", "", light, "shader_parameters",
                          light, "active_shader_parameter_index", rows=4)
        col = row.column(align=True)
        for action, icon in (('ADD', 'ADD'), ('REMOVE', 'REMOVE'),
                             ('DUPLICATE', 'DUPLICATE'), ('UP', 'TRIA_UP'), ('DOWN', 'TRIA_DOWN')):
            col.operator("light.shader_parameter_edit", text="", icon=icon).action = action
        index = light.active_shader_parameter_index
        if 0 <= index < len(light.shader_parameters):
            parameter = light.shader_parameters[index]
            col = layout.column()
            col.use_property_split = True
            col.use_property_decorate = True
            col.prop(parameter, _shader_parameter_values[parameter.type], text="Value",
                     slider=parameter.subtype == 'FACTOR')
            header, settings = layout.panel("light_shader_parameter_settings", default_closed=True)
            header.label(text="Parameter Settings")
            if settings:
                settings.use_property_split = True
                settings.use_property_decorate = False
                settings.prop(parameter, "description")
                if parameter.type not in {'BOOLEAN', 'COLOR'}:
                    settings.prop(parameter, "subtype")
                if parameter.type != 'BOOLEAN':
                    settings.prop(parameter, "range_min")
                    settings.prop(parameter, "range_max")
                    settings.prop(parameter, "use_hard_limits")


class DATA_PT_custom_props_light(DataButtonsPanel, PropertyPanel, Panel):
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH',
    }
    _context_path = "object.data"
    _property_type = bpy.types.Light


classes = (
    DATA_UL_light_shader_parameters,
    LIGHT_OT_shader_parameter_edit,
    DATA_PT_context_light,
    DATA_PT_preview,
    DATA_PT_light,
    DATA_PT_EEVEE_light,
    DATA_PT_spot,
    DATA_PT_EEVEE_light_shadow,
    DATA_PT_EEVEE_light_influence,
    DATA_PT_EEVEE_light_distance,
    DATA_PT_light_animation,
    DATA_PT_EEVEE_shader_parameters,
    DATA_PT_custom_props_light,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
