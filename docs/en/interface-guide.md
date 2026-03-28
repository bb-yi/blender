# Interface & Workflow

## 1. Material Selector Preview Toggle

### Purpose

Control whether material preview thumbnails are rendered in dropdown and search lists.

This is mainly used to reduce lag when opening the material selector in scenes with many materials.

### Entry Point

`Edit > Preferences > Editing > Objects > Materials > Material Selector Previews`

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-28-34.png" alt="alt text" style="border-radius: 10px;">
	<br>
</div>

### Behavior

- When enabled, the material selector shows previews according to the current logic.

- When disabled, the selector falls back to plain material icons and no longer triggers preview rendering in the dropdown list.

- The default value is enabled.

### Scope

- Only affects material previews inside dropdown lists such as `template_ID(...)`.

- Does not affect the large preview sphere in the `Material Properties` panel.

- Does not affect the actual material render result.

## 2. World Environment Exclude

### Behavior

- Lets you choose a collection that is not affected by the world environment.

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-29-43.png" alt="alt text" style="border-radius: 10px;">
	<br>
</div>
