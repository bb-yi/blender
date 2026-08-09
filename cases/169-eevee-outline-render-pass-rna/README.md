# eevee-outline-render-pass-rna

## Test content

Verify that the EEVEE View Layer Outline pass shown in the Data passes panel is backed by the
`ViewLayerEEVEE.use_pass_outline` RNA property.

## Pass criteria

- `ViewLayer.eevee.use_pass_outline` is present in the registered RNA properties.
- The property accepts both `True` and `False` and reads each value back unchanged.
- The script prints `EEVEE_OUTLINE_RENDER_PASS_RNA_OK=1`.

## Entry point

`run.py`
