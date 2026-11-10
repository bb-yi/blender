# Draw Info legacy fixture

`draw_info_legacy_queries.blend` was saved by Blender 5.2.2 NPR `47732b164c81`
before the 502.57 output migration. It contains the old single-Result Draw Info
node for all 22 Query values, each linked to a Math input, plus a linked node in
a shader node group. It contains no user project data or external assets.

`test_draw_info.py` verifies that opening it preserves the selected meaning of
every link, including the node group, and repeats those checks after saving and
reopening the converted copy in the configured test output directory. Do not
resave the fixture with the new node implementation.

`draw_info_legacy_503_31.blend` contains the same 22 queries and shared-group link,
saved with Blender 5.3 RC NPR `0252dcf129e1` before the 503.32 migration. The same
test checks this fixture and its saved/reopened copy on Blender 5.3 and newer.
