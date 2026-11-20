# NPR topology, ownership and migration regressions

Run `test_node_ownership_and_versioning.py` once per case in a separate Blender
process. Set all `BLENDER_USER_*` directories to isolated locations and direct
`--output-dir` outside the source checkout. No existing files are loaded.

Example (replace paths with local isolated locations):

```text
blender --background --factory-startup --disable-autoexec --python-exit-code 17 --python tests/python/npr/test_node_ownership_and_versioning.py -- mask --output-dir /tmp/npr-regression
```

The output directory can alternatively be supplied with `NPR_REVIEW_OUTPUT_DIR`.

## Cases and assertions

- `scene_copy`: EMPTY and ordinary Scene copying add one user per Filter Material
  entry and Filter Graph pointer. Duplicate material entries and empty entries
  are included; deleting each scene copy restores the starting counts.
- `mask`: list pointer assignment and replacement, standalone node duplicate,
  clipboard copy/paste, embedded material and standalone node-group copying,
  item append/move/middle removal, repeated dependency-graph evaluation,
  selection replace/clear, save/reload and deleting the referenced object all
  preserve the expected ID users and pointer remapping.
- `mask_render`: runs the existing Filter Object Mask pixel regression in
  OBJECT_LIST mode, then asserts that render localization/teardown does not
  change object users. The enabled center is approximately 0.6, the disabled
  and background samples are black, and the mask follows the moved object.
- `portal`: renders thirteen graph variants, including Math, reroute and
  multi-Portal cycles, muted Math/In/Out, empty/duplicate/unmatched names, type
  mismatch, fan-out and cache invalidation after rename/type change/removing
  a duplicate In. Normal and fan-out center pixels must be 0.75 and 1.5;
  cache restoration must recover 0.75. Cyclic/invalid variants must complete
  without exhausting memory, hanging or crashing. Physical `link.is_valid`
  is not a cycle oracle because the cycle may close on an implicit edge.
- `versioning`: saves real object and pose-bone motion paths, then creates
  synthetic DNA/subversion fixtures. Official-layout 44/45 preserve valid
  camera-space paths; pre-44 files and old NPR 44/45 remove those paths.
  NPR 46/current retain them. World-space paths always survive. Missing
  shadow-map scale initializes to 1; an existing 2.5 survives. A second
  save/reload must be idempotent.

## Resource limits and scope

**Run `portal` only against the repaired binary with externally enforced
process-memory and wall-clock limits.** A historical broken binary can allocate
tens of gigabytes while hashing a cycle. On Windows, assign the process to a Job
with a process-memory limit before resuming its suspended main thread. Validation for this change used a 3072 MiB process limit and
120/180-second wall-clock limits. On other platforms, use the equivalent process/container
limits. Blender's factory-startup flag alone does not isolate extension files.

The migration fixtures use equal-length DNA field renames and GLOB subversion
changes to test migration boundaries. They are not full exports from official
Blender 5.2.1/5.2.2 and do not replace genuine cross-version interoperability
coverage. UI Undo/Redo and the complete release suite are outside these cases.

A passing case prints `REVIEW_PASS <case>`; exit status alone is insufficient
if the wrong script or case was invoked.
