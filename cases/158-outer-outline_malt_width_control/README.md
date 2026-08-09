# outer-outline_malt_width_control

## Test Content

Loads the copied `描边断口测试.blend` asset and exercises the Malt-style per-class outline width control introduced in subversion 501.52. The test:

1. **Socket migration check**: verifies the Outline Control node has 13 sockets (the old 8 + 5 new Malt params: Depth/Normal Threshold Range, Depth/Normal/ID Edge Width) and that the legacy `Width Variation` socket is gone.

2. **Full-screen regression guard**: renders the baseline scene (all edge_width=1.0, range=0.0) and asserts the red outline pixel ratio is well below 100% — this catches the `outline_width_unpack` mask regression where non-zero high bytes in info.R decoded line_width to ~65534px and flooded JFA coverage to the entire screen.

3. **Per-class edge_width independence**: with ID Edge disabled (isolating depth+normal edges), setting Depth Edge Width or Normal Edge Width to 0 produces a measurably different image than the baseline (fewer red pixels), proving each class's width factor takes effect independently.

4. **Taper vs hard switch**: setting Depth Threshold Range > 0 (linear taper) produces a different image than range=0 (hard switch), proving the taper path is active.

## Pass Criteria

- The asset opens in a factory-startup background Blender session.
- The Outline Control node has exactly 13 input sockets.
- The legacy `Width Variation` socket is absent.
- Baseline render (edge_width=1.0, range=0.0) has red pixel ratio < 60% (not full-screen).
- Depth Edge Width = 0 (ID Edge off) produces fewer red pixels than baseline.
- Normal Edge Width = 0 (ID Edge off) produces fewer red pixels than baseline.
- Depth Threshold Range = 0.5 produces a different image than range = 0.0.

## Test Entry

`run.py`

## Original Asset

`test\描边断口测试.blend`
