# EEVEE Outline pass toggle compositor refresh

## Test Content

This case builds an EEVEE scene with a blue sphere and a red Outline Control
result. Its compositor keeps the Render Layers Image connected to the Alpha
Over background, connects the Outline pass to the foreground, and writes the
Alpha Over result to a File Output node.

The scene is rendered once with `use_pass_outline=True`. The test then changes
only `use_pass_outline` to `False`: it does not edit Alpha Over, reconnect a
socket, change frame, or rebuild the compositor. It inspects both the original
and dependency-graph-evaluated node trees before rendering the same output a
second time.

## Pass Criteria

- The enabled state has an Outline socket and linked Alpha Over foreground in
  both original and evaluated compositor trees.
- The enabled output contains visible red outline pixels.
- Disabling the pass removes the Outline socket and its link from both trees
  without any unrelated node edit.
- Alpha Over's factor and foreground default remain unchanged.
- The second render overwrites the same output with pure white pixels and no
  red outline, proving the compositor refreshed without manual reconnection.
