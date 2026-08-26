# SKATEOBJ v2 prefab profile

`.skateobj` is the spawnable-object profile of the normal SKATE binary
package. v2 uses `SKATE14` plus `MOBJ` schema 3. It reuses SKATE's material,
texture, render vertex, collision triangle, grind-rail, compression, physics,
and extension records, keeping one serializer and one compatibility boundary
for map and prefab geometry.

A v2 `.skateobj` package must:

- use the `.skateobj` filename extension, `SKATE14`, and MOBJ schema 3;
- contain one or more `MOBJ` prefab roots;
- have those roots collectively own every base render index, collision
  triangle, and authored grind rail exactly once;
- contain no NPC routes, kinematic boxes, hinged doors, water basins,
  mirrors, puddles, or moving lights;
- use authored-point grind rails or complete retail-native cubic spline
  payloads;
- contain only finite, bounded physics values accepted by the shared SKATE
  validator.

Each MOBJ root is a separate runtime object and, when physics is enabled, a
separate Box3D body. Multi-root assets therefore support structures such as a
cube pyramid whose cubes fall, collide, sleep, and topple independently.
Roots are never silently combined into a compound rigid body.

SKATEOBJ v2 may optionally carry the SKATE `BGRP` schema-1 extension.
Breakable roots remain separate Box3D bodies, but roots with the same
nonzero break group are released together after a qualifying player impact.
At instancing time, prefab-local group values are deterministically remapped
to unused map-wide values. Two copies of the same breakable prefab therefore
shatter independently. Adding another copy appends its bodies to the active
Box3D world and does not rebuild or repair existing instances.
Files without `BGRP` remain non-breakable and load exactly as before.

`OW_SPAWN` is the v2 prefab pivot. Loading subtracts its package-space
position from every root origin and grind rail. For native cubic splines this
adjusts the constant term and bounds while retaining the curve coefficients.
Spawn-time placement then adds one map-space position to the whole prefab
while retaining each root's relative transform. Editor rotation applies to
all native spline coefficients and recomputes their axis bounds from the
cubic extrema. Every runtime root receives a new instance ID; asset IDs must
never be reused as instance IDs.

## v1 compatibility

Existing v1 files remain supported. They use SKATE12 or SKATE13, contain
exactly one MOBJ root, and use that root's authored origin as the implicit
prefab pivot. MOBJ schema 1 and 2 contain no physics fields, so the loader
always defaults those objects to physics disabled. The tracked
`objects/test_grind_ledge.skateobj` is retained as a deterministic v12/v1
backward-compatibility fixture.

The map-wide environment fields physically present in the SKATE container are
ignored by the object-profile loader and should be emitted as deterministic
neutral defaults. Exporters should include only materials and textures
referenced by the prefab.

Future prefab behavior belongs in optional tagged extensions. Unknown
extension tags retain the normal SKATE validation/skip behavior; a future
feature does not require changing the base geometry layout.

The tracked `objects/box3d_glass_smash.skateobj` fixture contains 48
pre-fractured glass shards forming one flat frameless panel with no visible
texture pattern. Two uniform 1x1 RGBA swatches provide constant pane opacity
and fully transparent internal edge faces without procedural noise.
Each shard retains a closed collision prism, but its internal side faces use
a zero-alpha material so the fracture seams are not visible before impact.
Regenerate it deterministically with Blender 5 using
`tools/blender_owned_map/create_box3d_glass_smash.py`; the generated `.blend`
and export cache remain local-only.
