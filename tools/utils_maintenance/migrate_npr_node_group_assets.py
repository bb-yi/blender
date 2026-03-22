#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Migrate the 4.4 NPR node-group asset bundle into the 5.1 asset format.

Run with Blender:

  blender --background --factory-startup --python \
    tools/utils_maintenance/migrate_npr_node_group_assets.py -- \
    <input_blend> <output_blend>

The migration currently performs two tasks:
1. Convert legacy shader repeat-zone nodes that load as ``NodeUndefined`` in 5.1
   into the generic 5.1 repeat-zone node types.
2. Strip unrelated non-asset demo node-groups, keeping only asset node-groups and
   their recursive dependencies.
"""

from __future__ import annotations

import sys
from pathlib import Path

import bpy


ASSET_GROUP_NAMES = {
    "Cavity",
    "Co-Planar Edge Detection",
    "Curvature",
    "Kuwahara",
    "Shading Models",
    "Surface Curvature",
}

NODE_PROPS_TO_COPY = (
    "color",
    "hide",
    "label",
    "mute",
    "name",
    "parent",
    "show_options",
    "show_preview",
    "show_texture",
    "use_alpha",
    "use_clamp",
    "use_custom_color",
)

SOCKET_TYPE_MAP = {
    "BOOLEAN": "BOOLEAN",
    "INT": "INT",
    "RGBA": "RGBA",
    "ROTATION": "ROTATION",
    "STRING": "STRING",
    "VALUE": "FLOAT",
    "VECTOR": "VECTOR",
}


def parse_args() -> tuple[Path, Path]:
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1 :]
    else:
        argv = []

    if len(argv) != 2:
        raise SystemExit(
            "Usage: blender --background --python migrate_npr_node_group_assets.py -- "
            "<input_blend> <output_blend>"
        )

    input_path = Path(argv[0]).resolve()
    output_path = Path(argv[1]).resolve()
    return input_path, output_path


def copy_node_props(old_node: bpy.types.Node, new_node: bpy.types.Node) -> None:
    for attr in NODE_PROPS_TO_COPY:
        if hasattr(old_node, attr) and hasattr(new_node, attr):
            try:
                setattr(new_node, attr, getattr(old_node, attr))
            except (AttributeError, TypeError, ValueError):
                pass


def copy_socket_defaults(
    old_sockets: bpy.types.NodeInputs | bpy.types.NodeOutputs,
    new_sockets: bpy.types.NodeInputs | bpy.types.NodeOutputs,
) -> None:
    for old_socket in old_sockets:
        if old_socket.bl_idname == "NodeSocketVirtual" or old_socket.name == "":
            continue

        try:
            new_socket = new_sockets[old_socket.name]
        except KeyError:
            continue

        if not hasattr(old_socket, "default_value") or not hasattr(new_socket, "default_value"):
            continue

        try:
            old_value = old_socket.default_value
            if new_socket.type == "INT" and isinstance(old_value, float):
                new_socket.default_value = round(old_value)
            else:
                new_socket.default_value = old_value
        except (AttributeError, TypeError, ValueError):
            pass


def copy_links(
    tree: bpy.types.NodeTree,
    old_node: bpy.types.Node,
    new_node: bpy.types.Node,
    *,
    use_inputs: bool,
) -> None:
    if use_inputs:
        for old_socket in old_node.inputs:
            if old_socket.bl_idname == "NodeSocketVirtual" or old_socket.name == "":
                continue
            for link in list(old_socket.links):
                try:
                    new_socket = new_node.inputs[old_socket.name]
                    if new_socket.hide or not new_socket.enabled:
                        continue
                    tree.links.new(link.from_socket, new_socket)
                except (KeyError, RuntimeError):
                    continue
    else:
        for old_socket in old_node.outputs:
            if old_socket.bl_idname == "NodeSocketVirtual" or old_socket.name == "":
                continue
            for link in list(old_socket.links):
                try:
                    new_socket = new_node.outputs[old_socket.name]
                    if new_socket.hide or not new_socket.enabled:
                        continue
                    new_link = tree.links.new(new_socket, link.to_socket)
                    if link.to_socket.is_multi_input:
                        new_link.swap_multi_input_sort_id(link)
                except (KeyError, RuntimeError):
                    continue


def old_repeat_item_specs(old_output_node: bpy.types.Node) -> list[tuple[str, str]]:
    specs: list[tuple[str, str]] = []
    for socket in old_output_node.inputs:
        if socket.bl_idname == "NodeSocketVirtual" or socket.name == "":
            continue
        try:
            socket_type = SOCKET_TYPE_MAP[socket.type]
        except KeyError as ex:
            raise RuntimeError(
                f"Unsupported repeat socket type {socket.type!r} on {old_output_node.name!r}"
            ) from ex
        specs.append((socket.name, socket_type))
    return specs


def legacy_repeat_nodes(
    tree: bpy.types.NodeTree,
) -> tuple[list[bpy.types.Node], list[bpy.types.Node]]:
    repeat_inputs = []
    repeat_outputs = []
    for node in tree.nodes:
        if node.bl_idname != "NodeUndefined":
            continue
        if node.name == "Repeat Input":
            repeat_inputs.append(node)
        elif node.name == "Repeat Output":
            repeat_outputs.append(node)

    repeat_inputs.sort(key=lambda node: (round(node.location.x), round(node.location.y), node.name))
    repeat_outputs.sort(key=lambda node: (round(node.location.x), round(node.location.y), node.name))
    return repeat_inputs, repeat_outputs


def convert_repeat_zone_pair(
    tree: bpy.types.NodeTree,
    old_input: bpy.types.Node,
    old_output: bpy.types.Node,
) -> None:
    new_input = tree.nodes.new("GeometryNodeRepeatInput")
    new_output = tree.nodes.new("GeometryNodeRepeatOutput")
    new_input.pair_with_output(new_output)

    new_input.location_absolute = old_input.location_absolute
    new_output.location_absolute = old_output.location_absolute

    copy_node_props(old_input, new_input)
    copy_node_props(old_output, new_output)

    new_output.repeat_items.clear()
    for socket_name, socket_type in old_repeat_item_specs(old_output):
        new_output.repeat_items.new(socket_type, socket_name)

    copy_socket_defaults(old_input.inputs, new_input.inputs)
    copy_socket_defaults(old_input.outputs, new_input.outputs)
    copy_socket_defaults(old_output.inputs, new_output.inputs)
    copy_socket_defaults(old_output.outputs, new_output.outputs)

    copy_links(tree, old_input, new_input, use_inputs=True)
    copy_links(tree, old_input, new_input, use_inputs=False)
    copy_links(tree, old_output, new_output, use_inputs=True)
    copy_links(tree, old_output, new_output, use_inputs=False)

    tree.nodes.remove(old_input)
    tree.nodes.remove(old_output)


def convert_legacy_repeat_zones(tree: bpy.types.NodeTree) -> int:
    repeat_inputs, repeat_outputs = legacy_repeat_nodes(tree)
    if not repeat_inputs and not repeat_outputs:
        return 0
    if len(repeat_inputs) != len(repeat_outputs):
        raise RuntimeError(
            f"Repeat-zone conversion failed for {tree.name!r}: "
            f"{len(repeat_inputs)} inputs vs {len(repeat_outputs)} outputs"
        )

    converted = 0
    for old_input, old_output in zip(repeat_inputs, repeat_outputs):
        convert_repeat_zone_pair(tree, old_input, old_output)
        converted += 1
    return converted


def referenced_groups(group: bpy.types.NodeTree, result: set[str]) -> None:
    if group.name in result:
        return
    result.add(group.name)
    for node in group.nodes:
        if node.bl_idname != "ShaderNodeGroup":
            continue
        if node.node_tree is None:
            continue
        referenced_groups(node.node_tree, result)


def prune_unrelated_node_groups() -> None:
    keep = set()
    for name in ASSET_GROUP_NAMES:
        group = bpy.data.node_groups.get(name)
        if group is None:
            raise RuntimeError(f"Expected asset group {name!r} is missing")
        referenced_groups(group, keep)

    for group in list(bpy.data.node_groups):
        if group.name not in keep:
            bpy.data.node_groups.remove(group)


def ensure_expected_assets() -> None:
    found = {group.name for group in bpy.data.node_groups if group.asset_data is not None}
    missing = sorted(ASSET_GROUP_NAMES - found)
    if missing:
        raise RuntimeError(f"Missing expected asset groups after migration: {missing!r}")


def main() -> None:
    input_path, output_path = parse_args()
    bpy.ops.wm.open_mainfile(filepath=str(input_path), load_ui=False)

    converted_pairs = 0
    for group in bpy.data.node_groups:
        if group.bl_idname != "ShaderNodeTree":
            continue
        converted_pairs += convert_legacy_repeat_zones(group)

    prune_unrelated_node_groups()
    ensure_expected_assets()

    output_path.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.save_as_mainfile(
        filepath=str(output_path),
        check_existing=False,
        compress=False,
        copy=False,
    )

    print(
        f"Migrated NPR asset bundle to {output_path} with {converted_pairs} converted repeat-zone pair(s)."
    )


if __name__ == "__main__":
    main()
