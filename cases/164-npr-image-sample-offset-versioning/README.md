# npr-image-sample-offset-versioning

## Test Content

Opens a legacy NPR test blend whose Image Sample offset sockets were saved with the old `Offset` identifier. The current Image Sample declaration uses the `Vector` identifier while displaying the socket as `Offset`.

## Pass Criteria

- `Image Sample.Offset[Vector]` is linked from `Closure Input.UV[Item_0]`.
- `Image Sample.001.Offset[Vector]` is linked from `Closure Input.001.UV[Item_0]`.
- All checked links are valid after the file is loaded.

## Entry Point

`run.py`
