# Eevee NPR Instance Attribute Stability

## Test Content

The test creates two source planes whose material has an NPR Tree that reads an instance-domain
`NPR_ID` color. Geometry Nodes references both planes as instances, stores different `ID` and
`NPR_ID` colors on the Instance domain, joins the instances, and assigns a replacement material
that reads `ID` through an Instancer Attribute node. It renders once, moves both source objects,
then renders again.

## Pass Criteria

In both renders, the left instance must contain more than 400 red pixels and fewer than 20 blue
pixels, while the right instance must contain more than 400 blue pixels and fewer than 20 red
pixels. This proves that source-object updates neither redirect the lookup to stale buffer entries
nor swap attributes between instances when Surface and NPR materials request different attributes.
