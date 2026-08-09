# Eevee Shadow Pool Stress

## Test Content

This case renders a 257-light Eevee scene with multiple shadow pool sizes.

The scene is expected to exceed the 2048 MB shadow pool and to fit in 4096 MB
and 8192 MB on hardware that supports the requested texture array allocation.

## Pass Criteria

- The 2048 MB control render reports a shadow buffer full warning.
- The 4096 MB render completes without shadow buffer full warnings.
- The 8192 MB render completes without shadow buffer full warnings.
- The 4096 MB and 8192 MB renders do not report shadow atlas allocation
  downgrade warnings.

## Test Entry

`run.py`
