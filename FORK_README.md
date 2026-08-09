# NPR Release Tests (orphan branch)

This branch is a **published snapshot** of the maintainer suite
`test/release` from the packaging workspace. It is **not** a second
source-of-truth inside the Blender source tree.

- Maintainer edits only: outer `test/release/`
- Republish: `tools/publish_npr_release_tests_branch.ps1 -Push`

## Fork usage

```bash
git fetch origin npr-release-tests
git checkout npr-release-tests

# list
python run_npr_release_tests.py --blender /path/to/npr-blender --list

# run all / one
python run_npr_release_tests.py --blender /path/to/npr-blender
python run_npr_release_tests.py --blender /path/to/npr-blender --name 926
```

Set `NPR_RELEASE_SOURCE_DIR` to your NPR Blender source checkout when cases
wrap `tests/python/npr/...` scripts that live in the source tree.