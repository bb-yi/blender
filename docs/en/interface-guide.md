# Interface & Workflow Notes

## 1. Material Selector Preview Toggle

### Purpose

Controls whether material preview thumbnails are rendered in the material dropdown / search list.

This option is mainly used to reduce stutter caused by generating previews when opening the material selector in scenes with many materials.

### Entry Point

`Edit > Preferences > Editing > Objects > Materials > Material Selector Previews`

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-28-34.png" alt="Material Selector Previews" style="border-radius: 10px;">
	<br>
</div>

### Behavior

- When enabled, the material selector displays material previews using the current logic.

- When disabled, the material selector falls back to ordinary material icons and no longer triggers preview rendering in the dropdown list.

- The default value is enabled.

### Current Scope

- Currently only affects material preview display in dropdown selectors such as `template_ID(...)`.

- Does not affect the large preview sphere in the `Material Properties` panel.

- Does not affect the final rendering result of the material itself.

## 2. World Environment Exclude

### Purpose

Lets you choose a collection so objects in that collection are not affected by the world environment.

### Entry Point

`World Properties > Environment Lighting > Exclude Collection`

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-29-43.png" alt="World Environment Exclude" style="border-radius: 10px;">
	<br>
</div>

### Behavior

- The selected collection is excluded from world-environment lighting.

- This is useful when you want to separate environment-light influence between characters, foreground props, and background elements in the same scene.

## 3. Pose Bone Hide in Outliner

### Purpose

Adds a dedicated Outliner visibility flag to each `Pose Bone`, so large rigs can be kept cleaner in the Outliner without affecting the rig itself.

This is useful for hiding mechanism bones, helper bones, or low-level control layers while keeping the more important rig hierarchy readable.

### Entry Points

- `Bone Properties > Viewport Display > Hide in Outliner`
- `Outliner > Filter > Hidden PoseBones`

### Behavior

- Every `Pose Bone` has its own `Hide in Outliner` toggle.

- This toggle is enabled by default.

- `Hidden PoseBones` in the Outliner filter is also enabled by default, so existing rigs keep the same visible result until you disable that filter.

- Once `Outliner > Filter > Hidden PoseBones` is disabled, pose bones with `Hide in Outliner` enabled are hidden from the Outliner tree.

- If a hidden parent bone still has visible children, the visible children remain extracted in the tree so the whole branch does not disappear at once.

### Current Scope

- Applies only to `Pose Bone`

- Does not affect `Edit Bone`

- Only changes Outliner hierarchy visibility; it does not change transforms, animation, drivers, or rendering
