from pathlib import Path

import bpy


BLEND_PATH = Path(__file__).resolve().parent / "assets" / "example_legacy_image_sample_offset.blend"


def socket_label(socket):
    return f"{socket.node.name}.{socket.name}[{socket.identifier}]"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def check_image_sample_link(tree, sample_name, closure_name):
    sample = tree.nodes.get(sample_name)
    closure = tree.nodes.get(closure_name)
    require(sample is not None, f"Missing node: {sample_name}")
    require(closure is not None, f"Missing node: {closure_name}")

    offset = sample.inputs.get("Vector")
    require(offset is not None, f"{sample_name} is missing current Vector offset input")
    require(offset.name == "Offset", f"{socket_label(offset)} should display as Offset")
    require(offset.is_linked, f"{socket_label(offset)} is not linked")

    links = list(offset.links)
    require(len(links) == 1, f"{socket_label(offset)} expected 1 link, got {len(links)}")
    link = links[0]
    require(link.is_valid, f"{socket_label(offset)} link is invalid")
    require(link.from_node == closure, f"{socket_label(offset)} is linked from {link.from_node.name}")
    require(link.from_socket.identifier == "Item_0", f"Unexpected source: {socket_label(link.from_socket)}")
    require(link.from_socket.name == "UV", f"Unexpected source display name: {socket_label(link.from_socket)}")


def main():
    require(BLEND_PATH.exists(), f"Missing test asset: {BLEND_PATH}")
    bpy.ops.wm.open_mainfile(filepath=str(BLEND_PATH))

    tree = bpy.data.node_groups.get("NPR Tree")
    require(tree is not None, "Missing NPR Tree node group")

    check_image_sample_link(tree, "Image Sample", "Closure Input")
    check_image_sample_link(tree, "Image Sample.001", "Closure Input.001")

    print("NPR_IMAGE_SAMPLE_OFFSET_VERSIONING_OK=1")


if __name__ == "__main__":
    main()
