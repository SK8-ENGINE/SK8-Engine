# University owned-world integration

## Current package

The ignored local output
`intermediate/university/University.skate` is a SKATE v11 package exported from
`blender/DIST_University_Owned.blend`. Its tracked expected contract is
`schemas/university_expected.json`.

| Property | Verified value |
| --- | ---: |
| Package bytes | 130,320,474 |
| Materials | 526 |
| Opaque / mask / blend materials | 471 / 50 / 5 |
| Embedded textures | 462 |
| Decoded RGBA8 texture bytes | 253,257,728 |
| Retail normal maps / mapped mesh parts | 135 / 2,987 |
| Indexed visual vertices | 2,081,271 |
| Visual indices | 4,936,851 |
| Render triangles | 1,645,617 |
| Collision triangles | 1,133,642 |
| Retail grind rails | 4,201 |
| Native cubic segments | 27,008 |
| Compiled native grind bytes | 4,023,600 |
| Render chunks after clipping | 515 |
| Render triangles after clipping | 1,777,954 |
| Native collision clusters | 4,346 |
| Compiled native collision bytes | 23,993,440 |
| Top-level collision meshes | 1 |

The continuous RenderWare collision build sizes KD leaves by the native
255-local-vertex and 65,520-aligned-byte cluster limits. It no longer cuts
continuous geometry at an arbitrary 64 triangles. University therefore
installs as one authoritative collision volume with 4,346 clusters rather
than the old 44-volume spatial fallback. The reported Mega Park ramp strip
around local `(255.27, 74.36, -628.31)` was also decoded from the compiled blob:
the reported contact triangle and its surrounding downhill rows now reside
together in cluster 1,209 with their retail winding, surface `0x0083`, and
edge codes unchanged.

## Integrity boundary

The workflow rejects a package unless all of these remain equal to the
reviewed export:

- SKATE version, map name, file size, counts, maximum index, and bounds;
- material bytes and decoded texture bytes;
- expanded indexed triangle-corner records, including normals, both UV sets,
  and material IDs;
- collision records and authored feature records;
- byte-exact retail grind records against the extraction manifest;
- decoded retail collision geometry, winding, packed surface channels, and
  per-triangle edge/corner feature codes against all 301 source
  `ClusteredMesh` sections;
- all 141 source normal IDs, the seven explicit special-map exclusions, and
  all 135 selected linear normal textures through the package material table;
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

- Retail collision exports 1,133,642 triangles after rejecting six
  degenerate source triangles and one same-wound duplicate. It retains 10,691
  reverse-wound retail partners so intentional two-sided patches do not become
  one-sided dead spots. SKATE v11 also transports all native RenderWare edge
  codes through Blender face attributes so hard/smooth edge and vertex
  contacts are not guessed during engine compilation.
- The current UTT presentation parser computes averaged geometry normals;
  authoritative packed retail normals are not yet transported.
- Conventional retail tangent-space normal maps are transported. Seven
  shader-specific default/water/palm resources remain recorded but unbound;
  specular, lightmap, detail, decal, macro-overlay, environment, and noise
  semantics are not reconstructed yet.
- AI routes, hinged doors, and local lights are not recovered yet.
- All 183 packed retail collision surfaces are preserved, including the three
  surface IDs that use native physics channel 13.
- No runtime object/instance table exists in SKATE v11. The importer does not
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
