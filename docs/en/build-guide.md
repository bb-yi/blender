# Local Build Workflow

This page is specific to the current Blender NPR workspace:

- Source tree: `E:\blender_bulid_test\blender_npr_bulid\blender_5_1_port`
- Parallel mainline worktree: `E:\blender_bulid_test\blender_npr_bulid\blender_5_1_port_mainfix`
- Default build tree: `build_windows_x64_vc17_Release_5_1_port_clean_ninja`
- Default install tree: `install_windows_x64_vc17_Release_5_1_port_clean`
- Mainfix build tree: `build_windows_x64_vc17_Release_5_1_port_mainfix_ninja`
- Mainfix install tree: `install_windows_x64_vc17_Release_5_1_port_mainfix`

The goal is to keep local collaboration on one consistent incremental build workflow instead of repeatedly reconfiguring CMake, creating new build folders, or debugging environment mistakes as if they were source failures.

## Fixed Rules

1. Reuse the existing Ninja build trees; do not create new `build_*` folders casually.
2. Prefer incremental builds; do not jump to `reconfigure-clean` unless the cache is truly broken.
3. Validate from the install tree, not from the build-tree executable.
4. Non-interactive runs should always include `--no-pause`.
5. Never run concurrent builds in the same build tree. Check for stale `cmake.exe` / `ninja.exe` first.

## Daily Commands

### Default / Daily Testing

```powershell
.\build_ninja_sccache_poll.bat install --no-pause
```

- Default mode keeps `WITH_CYCLES=OFF`
- Output goes to `install_windows_x64_vc17_Release_5_1_port_clean`

### Parallel Mainline Mainfix

```powershell
.\build_ninja_sccache_poll.bat mainfix install --no-pause
```

- Uses `blender_5_1_port_mainfix`
- Outputs to `install_windows_x64_vc17_Release_5_1_port_mainfix`
- Does not overwrite the default install tree

### Release Build / Cycles Enabled

```powershell
.\build_ninja_sccache_poll.bat with-cycles install --no-pause
```

### If Unity Objects Go Stale

```powershell
.\build_ninja_sccache.bat clean-unity install --no-pause
```

## Validation

After a build, check the install-tree binary first:

```powershell
& ".\install_windows_x64_vc17_Release_5_1_port_clean\blender.exe" --background --factory-startup --version
```

Or for mainfix:

```powershell
& ".\install_windows_x64_vc17_Release_5_1_port_mainfix\blender.exe" --background --factory-startup --version
```

Timestamps alone are not enough. Add at least one background smoke test before treating the install tree as validated.

## Common Problems

### `INSTALL` Fails Because Files Are In Use

The usual cause is that the install-tree `blender.exe` is still running. Close the matching install-tree Blender first, then rerun `install`.

### `build_ninja_sccache_poll.bat` Looks Stuck

- Check whether `cmake.exe`, `ninja.exe`, or `cl.exe` are still active
- Inspect `temp\codex-build-logs`
- Do not start a second build chain on the same build tree

### `sccache` or `cl.exe` Returns `-1`

Reduce concurrency and continue incrementally, for example:

```powershell
$env:BUILD_JOBS='8'
.\build_ninja_sccache_poll.bat mainfix install --no-pause
```

### Windows Precompiled Libs Not Found

`blender_5_1_port_mainfix\lib\windows_x64` must remain usable. The current safe setup is:

- Keep the `windows_x64` root as a real directory
- Reuse child library folders with junctions that point to `blender_5_1_port\lib\windows_x64\...`

Do not make the entire `windows_x64` root itself a junction.
