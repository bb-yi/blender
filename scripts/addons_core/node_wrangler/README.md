# Running Tests

```
./utils/paths_test.py
```

# NPR editors

- In Filter Graphs, `O` connects the selected image to its stage output.
  `Ctrl+Shift+Left Click` previews a node and cycles its image outputs on repeated clicks.
  Existing downstream connections identify the execution stage; an unconnected branch
  uses the Before Post FX output when present, otherwise the first active output.
- In light shader trees, shader outputs connect to Light Output, while color and
  numeric outputs connect to Light Shader Output. Named Intensity and Attenuation
  sockets retain their meaning; other scalar outputs use Intensity. Preview also
  works inside light node groups, without replacing their regular group outputs.
- In EEVEE, `Ctrl+Shift+Left Click` on a 2D Image to Closure node adds an Evaluate
  Closure for preview. It uses the active UV map, or normalized screen coordinates
  in Filter Materials. Repeat the shortcut to switch between Color and Alpha
  without adding more preview nodes.
- Filter Graphs support the common layout, label, connection, reroute and cleanup
  tools. Cleanup preserves stage and AOV outputs. Image mixing is performed by
  Filter Materials; shader Mix/Math nodes cannot operate on graph image sockets.

The NPR operator regression is `tests/python/npr/test_node_wrangler_npr.py`.
Run it with an isolated user directory and `--background --factory-startup
--python-exit-code 1 --python <test path>`. It checks resulting links and preserved
outputs, and reports unsupported build capabilities separately.
