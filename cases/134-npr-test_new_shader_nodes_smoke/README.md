# New Shader Nodes Smoke

## Purpose

Verifies the newer NPR/Goo shader nodes are present at the Blender Python/RNA
level and still expose the public creation surface expected by release users.

The case creates the nodes directly with `nodes.new()` and checks key sockets
and representative enum or mode properties.

## Pass Criteria

- All listed node types can be created successfully.
- Key public sockets still exist for representative nodes.
- Representative enum or mode properties remain present and switchable through
  RNA without errors.

## Test Entry

`run.py`
