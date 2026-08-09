# Release Test Suite

**This folder is the single source of truth** for release validation in the packaging
workspace. Do not maintain a second copy under `blender_npr_post/tests/`.

## Maintainer (this workspace)

Run all release tests with:

```powershell
.\run_release_tests.bat --no-pause
```

Cross-platform / selective runner (same suite):

```powershell
python test\release\run_npr_release_tests.py --blender install_windows_x64_vc17_Release_npr_post_main\blender.exe --list
python test\release\run_npr_release_tests.py --blender install_windows_x64_vc17_Release_npr_post_main\blender.exe --name 926
```

The full release suite expects the install tree to come from `with-cycles install`.

`package_current_release.bat` runs the same suite before packaging. The suite is defined by
`release_tests.json`, which scans the case folders in `cases\`.

Each test case has its own folder:

```text
test\release\cases\<case-name>\
  case.json   # machine-readable runner config
  run.py      # Blender Python entry point
  README.md   # what this test checks and pass criteria
```

Case entry points may wrap tests that still live in their native locations:

- Blender repository tests stay under `blender_npr_post\tests\...` (or legacy `blender_5_1_port\tests\...`).
- Local workspace probes stay under the outer `test\...` tree.
- Branch-only or experimental probes are not part of the release suite until they are promoted into
  `test\release\cases\`.

Logs are written to `test\release\logs\<timestamp>\`.

## Fork users (no outer workspace)

Publish a **snapshot orphan branch** from this folder (does not create an in-tree copy):

```powershell
.\tools\publish_npr_release_tests_branch.ps1 -NoPause
.\tools\publish_npr_release_tests_branch.ps1 -Push -NoPause
```

Forks then:

```bash
git fetch origin npr-release-tests
git checkout npr-release-tests
python run_npr_release_tests.py --blender /path/to/npr-blender --list
```
