# University owned-world integration

## Current package

The ignored local output
`intermediate/university/University.skate` is a SKATE v9 package exported from
`blender/DIST_University_Owned.blend`. Its tracked expected contract is
`schemas/university_expected.json`.

| Property | Verified value |
| --- | ---: |
| Package bytes | 123,930,019 |
| Materials | 350 |
| Embedded textures | 325 |
| Decoded RGBA8 texture bytes | 222,193,664 |
| Indexed visual vertices | 2,081,271 |
| Visual indices | 4,936,851 |
| Render triangles | 1,645,617 |
| Collision triangles | 1,329,037 |
| Render chunks after clipping | 515 |
| Render triangles after clipping | 1,777,954 |
| Collision fallback cell size | 256 m |
| Collision chunks | 44 |

The continuous RenderWare collision build exceeds the native 16-bit cluster
index. The runtime and validator therefore use a spatial fallback. At the old
128 metre cell size University required 140 chunks, exceeding the runtime's
96-slot bound. A 256 metre cell size produces 44 chunks, keeps the largest
chunk below the existing 400,000-triangle safety bound, and retains every
collision triangle.

## Integrity boundary

The workflow rejects a package unless all of these remain equal to the
reviewed export:

- SKATE version, map name, file size, counts, maximum index, and bounds;
- material bytes and decoded texture bytes;
- expanded indexed triangle-corner records, including normals, both UV sets,
  and material IDs;
- collision records and authored feature records;
- downstream render-world and native collision-world counts.

The C++ validator is the actual engine loader linked against the same
owned-world library as the game. A smaller file alone is never accepted as
evidence of correctness.

## Known fidelity limits

- Retail simulation RX2 data is preserved but not decoded. Current collision
  is generated from structural presentation geometry.
- Twenty-three unresolved shared texture references use explicit fallback
  materials.
- The extracted stream does not currently provide owned-world grind rails,
  AI routes, hinged doors, or local lights.
- All current collision surfaces use the provisional concrete/polished
  surface mapping.
- No runtime object/instance table exists in SKATE v9. The importer does not
  retain authoritative retail instance references, so transforms are baked
  into vertices rather than inventing unsafe instance relationships.

These limits must be judged in the user-run visual/collision pass and must not
be hidden by successful telemetry.
