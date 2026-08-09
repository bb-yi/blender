import shutil
from pathlib import Path

import bpy


ROOT = Path(__file__).resolve().parents[4]
OUTPUT_DIR = ROOT / "temp" / "release_test_outputs" / "pr18_npr_asset_import_policy"
EXPECTED_NPR_CATALOG_ID = "ee51ee5f-d28f-4de4-8881-54dbd865436e"
EXPECTED_GROUPS = {"Cavity", "Curvature", "Kuwahara", "Shading Models", "Surface Curvature"}


def installed_asset_bundle_path():
    version_dir = f"{bpy.app.version[0]}.{bpy.app.version[1]}"
    return Path(bpy.app.binary_path).resolve().parent / version_dir / "datafiles" / "assets" / "nodes" / "npr_node_groups.blend"


def inspect_installed_npr_bundle(path):
    bpy.ops.wm.open_mainfile(filepath=str(path), load_ui=False)
    available = {group.name for group in bpy.data.node_groups}
    missing = EXPECTED_GROUPS - available
    assert not missing, f"Installed NPR asset bundle is missing groups: {sorted(missing)}"

    for group_name in EXPECTED_GROUPS:
        group = bpy.data.node_groups[group_name]
        assert group.asset_data is not None, f"{group_name} is not marked as an asset"
        catalog_id = str(group.asset_data.catalog_id)
        assert catalog_id == EXPECTED_NPR_CATALOG_ID, (
            f"{group_name} catalog id changed to {catalog_id}; expected {EXPECTED_NPR_CATALOG_ID}"
        )


def append_node_group(path, group_name, reuse_local_id):
    bpy.ops.wm.append(
        filepath=str(path / "NodeTree" / group_name),
        directory=str(path / "NodeTree"),
        filename=group_name,
        do_reuse_local_id=reuse_local_id,
    )


def assert_builtin_reuse_import(path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    append_node_group(path, "Cavity", True)
    append_node_group(path, "Cavity", True)
    names = sorted(group.name for group in bpy.data.node_groups if group.name.startswith("Cavity"))
    print(f"PR18_BUILTIN_REUSE_GROUPS={names}")
    assert names == ["Cavity"], f"Expected reuse import to keep one Cavity group, got {names}"


def create_external_asset_blend(path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    group = bpy.data.node_groups.new("ExternalPlainGroup", "ShaderNodeTree")
    group.asset_mark()
    bpy.ops.wm.save_as_mainfile(filepath=str(path))


def assert_external_default_append_duplicates(path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    append_node_group(path, "ExternalPlainGroup", False)
    append_node_group(path, "ExternalPlainGroup", False)
    names = sorted(group.name for group in bpy.data.node_groups if group.name.startswith("ExternalPlainGroup"))
    print(f"PR18_EXTERNAL_DEFAULT_APPEND_GROUPS={names}")
    assert names == ["ExternalPlainGroup", "ExternalPlainGroup.001"], (
        f"Default append for external groups should create independent copies, got {names}"
    )


bundle = installed_asset_bundle_path()
assert bundle.exists(), f"Missing installed NPR asset bundle: {bundle}"

inspect_installed_npr_bundle(bundle)
assert_builtin_reuse_import(bundle)

if OUTPUT_DIR.exists():
    shutil.rmtree(OUTPUT_DIR)
OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
external_blend = OUTPUT_DIR / "external_node_group.blend"
create_external_asset_blend(external_blend)
assert external_blend.exists(), f"Failed to create external asset blend: {external_blend}"
assert_external_default_append_duplicates(external_blend)
