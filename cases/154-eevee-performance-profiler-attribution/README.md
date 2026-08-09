# eevee-performance-profiler-attribution

## Test Content

This case validates the Eevee performance profiler report contract used by the Outliner performance view.

It creates minimal scenes at runtime:
- A shadow/probe scene with several shadow-casting lights and one planar probe.
- A Filter/native-output scene that exercises Filter and final ReadResult stages.
- A small viewport scene that validates the shared Draw Sync and Draw Submission ledger, then adds
  a live GLSL Function surface material while profiling remains active.
- The live Eevee Performance Outliner display while structured snapshots are being published.

## Pass Criteria

The profiler render report must:
- Declare CPU wall-time timing, identify that engine initialization/shader setup are outside the final-render timer, and state that scope rows are inclusive.
- Identify every final-render result with non-empty `View Layer` and `Render View` source fields.
- Keep shadow sub-stage rows parseable with `ms`, `ms/call`, and `ms/sample`.
- Report per-light shadow attribution with `tilemap_view_share` and `sync_dirty_tilemaps`.
- Report probe work including planar probe rendering.
- Keep Filter and final output activity represented by normal stage rows such as `Filter.BeforeComposite` and `ReadResult`.
- Keep measured `Shader Waits` optional, but when present report non-negative CPU and `frame_share` values.
- Keep measured `Material Sync` optional for the shadow scene, but require a positive `Total requests` whenever it appears; require it for the Filter scene.
- Require the Filter scene's `Pass Readback` aggregate and typed rows, with positive `Total passes` and data plus non-negative CPU/readback-share values.
- Omit duplicate or speculative sections that do not add stable measured attribution: `Top CPU Stages`, `Sync Objects`, `Film Outputs`, `Filter Costs`, `Render Texture Costs`, standalone `Sync Texture Loads`, and `Hints`.

The viewport report must:
- Declare `Timing Domain: CPU wall time` and identify the shared draw-cycle coverage, so the values are not mistaken for GPU duration or a complete UI frame time.
- Report the latest `Viewport Draw CPU` separately from its smoothed `Average Draw CPU` value.
- Report a finite, non-negative `Profiler Accounting CPU`, state that it is excluded from Draw CPU, and state that report formatting/snapshot publication are outside that value.
- State that stage rows use inclusive accounting, so nested parent and child values must not be added together.
- Reconcile `Viewport Draw CPU` exactly with `Draw.Sync.Shared + Draw.Submission.Shared` within report rounding error.
- Record one call for each shared root and each direct sync/submission phase.
- Expose EEVEE Begin/End Sync module groups for World, scene modules, view/effects, NPR/post,
  shader readiness, materials/velocity, volume/shadows/lights, frame state, and probes/uniforms.
- Keep direct child totals within their measured parent root, so nested stages are not double-counted.
- Report the last dependency-graph evaluation separately, state that it is excluded from Draw CPU,
  and label the dependency-graph value as an evaluation serial rather than an update count.
- Publish `Sample Progress: 1/1` and `Sampling: Complete` after the viewport converges.
- After convergence, publish a known scene update and a newer profiler capture sequence without
  active redraw polling.
- Continue doing so after a Solid-to-Rendered transition recreates the EEVEE Instance, without
  requiring active redraw polling to escape the profiler cadence window; a newly created source
  may restart its capture sequence but must have a new Source ID.
- After a stable baseline report, accept a live GLSL Function material update and report
  `GLSL Mats=1` after EEVEE Instance recreation without dereferencing stale GPU material
  generated-source data or crashing.
- Keep the Outliner in `EEVEE_PERFORMANCE` mode and redraw it while snapshots update without a
  runtime failure.

The script prints `EEVEE_PERFORMANCE_PROFILER_ATTRIBUTION_RELEASE_OK` after all assertions pass.

## Entry Point

`run.py`
