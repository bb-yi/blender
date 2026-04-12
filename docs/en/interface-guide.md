# Interface and Workflow Additions

## 1. Eevee Performance

### Purpose

Shows Eevee viewport / final-render performance statistics, stage breakdowns, and hints directly in the `Outliner`, making it easier to locate heavy parts of the pipeline.

<div align="center">
	<img src="images/placeholder_eevee_performance.png" alt="Eevee Performance" style="border-radius: 10px;">
	<br>
</div>

### Entry

- `Outliner > Display Mode > Eevee Performance`
- `Profiler` / `Pause` / `Sort by Time` in the `Outliner` header
- The `Eevee Performance` popover in the `Outliner` header

### Behavior

- Enabling `Profiler` starts collecting performance statistics and displays them in the `Outliner`
- `Pause` freezes live updates so the current result can be inspected
- `Sort by Time` sorts the stage list by CPU cost instead of fixed pipeline order
- `Average Window` sets the frame window used for smoothing statistics
- The tree currently includes groups such as `Viewport`, `Final Render`, `Metadata`, `Features`, `Stages`, and `Hints`

### Current Scope

- This is mainly a CPU-side Eevee stage profiler and hint view, not a full GPU profiler
- It is meaningful only for `Eevee`, not for `Cycles`

## 2. Material Selector Previews

### Purpose

Controls whether material previews are rendered in material drop-downs and search lists.

This is mainly useful when many materials exist and expanding the selector would otherwise trigger too many preview renders.

### Entry

`Edit > Preferences > Editing > Objects > Materials > Material Selector Previews`

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-28-34.png" alt="Material Selector Previews" style="border-radius: 10px;">
	<br>
</div>

### Behavior

- When enabled: the selector shows material previews as usual
- When disabled: the selector falls back to normal material icons and no longer triggers preview renders in the drop-down list
- Default value: enabled

### Current Scope

- Only affects `template_ID(...)` style material pickers in drop-down lists
- Does not affect the large preview sphere in `Material Properties`
- Does not affect the actual material render result

## 3. Material Face Culling

### Purpose

Adds clearer face-culling control for materials. In addition to the usual back-face culling, the NPR Port also supports `Front` culling.

<div align="center">
	<img src="images/placeholder_material_face_culling.png" alt="Material Face Culling" style="border-radius: 10px;">
	<br>
</div>

### Entry

`Material Properties > Settings > Culling > Camera`

### Available Modes

- `None`: render both front and back faces
- `Back`: hide back-facing faces
- `Front`: hide front-facing faces

### Notes

- `Front` is useful for shell-style effects, inside-view setups, or some inverted-outline style tricks
- `Shadow` and `Light Probe Volume` still keep their own culling controls

## 4. Eevee Lightgroup ID

### Purpose

Assigns an integer light-group ID to Eevee lights so the `Shader Info` node can filter direct-light evaluation by group.

### Entry

`Light Data > Light > Lightgroup ID`

### Behavior

- Default value: `0`
- When `Shader Info` also uses `Lightgroup = 0`, only lights with `Lightgroup ID = 0` are evaluated
- If a `Shader Info` node uses another integer value, only lights with the same ID are included
- This grouping currently affects only `Shader Info`, not the default Eevee material lighting path

## 5. Pose Bone Outliner Visibility

### Purpose

Adds a dedicated Outliner visibility flag to each `Pose Bone`, making it easier to organize complex rigs without changing rig behavior itself.

### Entry

- `Bone Properties > Viewport Display > Hide in Outliner`
- `Outliner > Filter > Hidden PoseBones`

### Behavior

- Every `Pose Bone` has its own `Hide in Outliner` toggle
- The toggle is enabled by default
- The `Hidden PoseBones` filter in the `Outliner` is also enabled by default, so existing rigs do not immediately change appearance
- After disabling `Outliner > Filter > Hidden PoseBones`, bones with `Hide in Outliner` enabled are hidden from the tree
- If a hidden parent still has visible child bones, those visible children remain in the tree instead of removing the whole hierarchy

### Current Scope

- Affects `Pose Bone` only
- Does not affect `Edit Bone`
- Changes only the Outliner hierarchy display, not transforms, animation, drivers, or rendering
