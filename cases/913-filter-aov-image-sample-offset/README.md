# filter-aov-image-sample-offset

## Test Content

Creates a scene with a material-written color AOV that is dark on the left side and red on the right side. A Filter material reads that AOV through `AOV Input`, samples it with `Image Sample` in Pixel offset mode, and writes the sampled color to the filter output.

## Pass Criteria

The render must complete without shader errors. The center pixel must sample the shifted red AOV region, while a left-side sample remains dark.

## Test Entry

`run.py`

## Source Test

`blender_5_1_port\tests\python\npr\test_filter_aov_image_sample_offset.py`
