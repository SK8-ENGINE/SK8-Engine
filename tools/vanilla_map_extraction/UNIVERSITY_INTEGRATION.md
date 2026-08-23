# University owned-world integration

## Current package

The ignored local output
`intermediate/university/University.skate` is a SKATE v10 package exported from
`blender/DIST_University_Owned.blend`. Its tracked expected contract is
`schemas/university_expected.json`.

| Property | Verified value |
| --- | ---: |
| Package bytes | 121,962,132 |
| Materials | 515 |
| Opaque / mask / blend materials | 460 / 50 / 5 |
| Embedded textures | 327 |
| Decoded RGBA8 texture bytes | 222,980,096 |
| Indexed visual vertices | 2,081,271 |
| Visual indices | 4,936,851 |
| Render triangles | 1,645,617 |
| Collision triangles | 1,122,951 |
| Retail grind rails | 4,201 |
| Native cubic segments | 27,008 |
| Compiled native grind bytes | 4,023,600 |
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
- byte-exact retail grind records against the extraction manifest;
- decoded retail collision geometry, winding, and packed surface channels
  against all 301 source `ClusteredMesh` sections;
- downstream render-world, native grind-world, and collision-world counts.

The C++ validator is the actual engine loader linked against the same
owned-world library as the game. A smaller file alone is never accepted as
evidence of correctness.

For the material-binding correction, the old and new package are additionally
compared independent of vertex/index ordering and material IDs. The complete
position/normal/UV triangle multiset and decoded texture payloads must remain
identical. Material records and assignments are expected to differ because
the old retail parser shifted meshes after material groups without a diffuse
parameter and the old Blender preparation forced all alpha modes to opaque.

## Known fidelity limits

- Retail collision exports 1,122,951 triangles after rejecting six
  degenerate source triangles and 10,692 exact/opposite-wound duplicates from
  the complete 1,133,649-triangle extraction.
- The current UTT presentation parser computes averaged geometry normals;
  authoritative packed retail normals are not yet transported.
- AI routes, hinged doors, and local lights are not recovered yet.
- All 183 packed retail collision surfaces are preserved, including the three
  surface IDs that use native physics channel 13.
- No runtime object/instance table exists in SKATE v10. The importer does not
  retain authoritative retail instance references, so transforms are baked
  into vertices rather than inventing unsafe instance relationships.

These limits must be judged in the user-run visual/collision pass and must not
be hidden by successful telemetry.

## Permanent visual-check workflow

`Invoke-UniversityVisualCheck.ps1` is an offline preparation command for the
map agent. It performs stale exports, package checks, game compilation,
engine-side validation, and staging, and contains no game-launch path.

`Run-University-Visual-Check.bat` is the user-facing launch-only command. It
hash-checks the prepared executable/runtime/map, creates a timestamped runtime
log directory, and launches the already staged build. Agents must not execute
the BAT or `skate3.exe`; only the user decides visual correctness.
