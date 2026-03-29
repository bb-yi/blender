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
