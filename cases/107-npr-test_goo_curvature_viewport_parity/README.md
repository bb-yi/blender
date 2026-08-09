# npr-test_goo_curvature_viewport_parity

## Test Content

Verifies the Curvature node's `Scene Curvature` and `Scene Rim` outputs keep
the expected shape profile in Eevee.

The case renders a plane and a sphere:

- `Scene Curvature`: the plane should stay near zero, while the sphere center
  should be clearly brighter than both the plane and the sphere's middle band.
- `Scene Rim`: the center should stay dark and the value should rise toward the
  silhouette.

## Pass Criteria

- Plane curvature center and edge are both below `0.05`.
- Sphere curvature center is at least `0.1` above the plane center.
- Sphere curvature center is at least `0.05` above the average of four middle
  band samples and is at least `3x` that middle-band average.
- Plane rim center is below `0.001`.
- Sphere rim center is below `0.01`, and the mid, edge, and silhouette samples
  increase outward in order.

## Test Entry

`run.py`

## Original Test

`blender_5_1_port\tests\python\npr\test_goo_curvature_viewport_parity.py`
