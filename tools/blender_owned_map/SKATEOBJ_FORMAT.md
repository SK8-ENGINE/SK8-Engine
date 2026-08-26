# SKATEOBJ v1 prefab profile

`.skateobj` is the spawnable-object profile of the normal SKATE binary
package. Current exports use `SKATE13`; existing `SKATE12` objects remain
supported. It deliberately reuses SKATE's material, texture, render vertex,
collision triangle, grind-rail, compression, and extension records. This
keeps one serializer and one set of compatibility rules for map and prefab
geometry.

A v1 `.skateobj` package must:

- use the `.skateobj` filename extension and a supported SKATE package magic;
- contain exactly one `MOBJ` prefab root;
- have that root own every base render index and collision triangle;
- have that root own every authored grind rail in the package;
- contain no NPC routes, kinematic boxes, hinged doors, water basins,
  mirrors, puddles, or moving lights;
- use authored-point grind rails rather than extracted retail-native spline
  payloads.

Spawn-time geometry, collision, bounds, and grind points are local to the
prefab root. The root's authored origin is the pivot: loading normalizes the
root origin to zero and subtracts it from attached grind points. Each runtime
spawn receives a separate instance ID and authoritative transform; asset IDs
must never be used as instance IDs.

The map-wide environment fields physically present in the SKATE container are
ignored by the object-profile loader and should be emitted as deterministic
neutral defaults. They are tiny compared with mesh and texture payloads,
while reusing them avoids a second incompatible parser. Exporters should
include only materials and textures referenced by the prefab.

Future prefab behavior belongs in optional tagged extensions. Planned examples
include:

- `SNAP`: named placement/snap points and orientation;
- `TAGS`: category, search tags, author, and description;
- `THMB`: spawn-menu thumbnail;
- `COMP`: versioned gameplay component descriptors.

Unknown extensions retain the normal SKATE validation/skip behavior. A future
feature does not require changing the base geometry layout.
