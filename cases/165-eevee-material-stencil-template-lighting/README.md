# eevee-material-stencil-template-lighting

## Test Content

Builds a small EEVEE scene with a lit material surface and renders several material stencil/template configurations:

- Deferred `DITHERED` material without stencil.
- Deferred `DITHERED` material writing user stencil bits with `ALWAYS` / `REPLACE`.
- Deferred read-only stencil material with `ALWAYS`.
- Deferred read-only stencil material with `EQUAL` and no writer, which should be rejected.
- Forward `BLENDED` material writing stencil bits.

## Pass Criteria

- Deferred stencil writer lighting is close to the no-stencil baseline.
- Read-only `ALWAYS` stencil material remains close to the baseline.
- Read-only `EQUAL` with no matching stencil writer remains dark.
- Forward stencil writer remains close to the baseline.

## Entry Point

`run.py`
