# University owned-world integration

## Current package

The ignored local output
`intermediate/university/University.skate` is a SKATE v12 package exported from
`blender/DIST_University_Owned.blend`. Its tracked expected contract is
`schemas/university_expected.json`.

| Property | Verified value |
| --- | ---: |
| Package bytes | 254,686,430 |
| Materials | 8,729 |
| Retail draw definitions | 8,546 |
| Opaque / mask / blend materials | 6,572 / 2,118 / 39 |
| Embedded textures | 2,046 |
| Named retail texture bindings | 40,663 |
| Retail parameter entries | 67,475 |
| Decoded RGBA8 texture bytes | 760,748,032 |
| Retail normal maps / mapped mesh parts | 135 / 2,987 |
| Retail lightmaps / mapped mesh parts | 1,270 / 8,489 |
| Exact decoded retail lightmap bytes | 393,134,080 |
| Indexed visual vertices | 2,139,713 |
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

The v11 lightmapped baseline was 195,460,271 bytes. The v12 package is larger
because it adds 314 previously omitted textures, complete named bindings and
parameters for every retail draw, decal UVs, packed tangent frames, and the
compressed `WMET` extraction manifest. Its first full-fidelity draft was
301,970,382 bytes; packing the tangent frame to retail-equivalent SNORM8 and
fixing UV-layer aliasing reduced it to 254,686,430 bytes without reducing
texture resolution or geometry. All 1,270 selected lightmap pages remain
byte-equal to the decoded source images after Blender's documented
bottom-row-first storage conversion.

The retail-frame pass restores the source normal/tangent/handedness on 3,121
mesh parts. The addon reconstructs the binormal carried by the compact SKATE
runtime record, allowing the loader to recover the original tangent instead
of treating the retail usage-6 tangent as though it were already a binormal.
The expanded 1,579,005 affected triangle corners are checked against the
source-derived frame by a dedicated package verifier. This fixes
geometry-shaped black lighting masks on full environment materials while
leaving simple diffuse materials and ordinary custom-map tangent generation
unchanged.

Retail vertex declarations contain a second TEXCOORD on 8,541 mesh parts.
The extraction now decodes both explicit `SHORT2N` declarations and the
previously unclassified `SHORT4` world layout whose `xy` components are the
lightmap unwrap and whose signs also carry tangent handedness. Static world
unwraps use the game's `abs(uv)` rule. Thirty-two water/ocean mesh parts are
kept recorded but deliberately unbound because those shader families do not
consume a reliable static indirect lightmap in the renderer; one sign mesh
has a retail lightmap parameter but no secondary TEXCOORD.

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
- expanded indexed triangle-corner records, including normals, base/lightmap/
  decal UVs, packed tangent frame, and material IDs;
- collision records and authored feature records;
- byte-exact retail grind records against the extraction manifest;
- decoded retail collision geometry, winding, packed surface channels, and
  per-triangle edge/corner feature codes against all 301 source
  `ClusteredMesh` sections;
- all 141 source normal IDs, the seven explicit special-map exclusions, and
  all 135 selected linear normal textures through the package material table;
- all 1,271 source lightmap IDs, the 1,270 selected byte-exact lightmap
  payloads, 8,489 bound mesh parts, exact second-UV bounds, and the 33
  explicit shader/no-UV exclusions;
- all 8,546 retail material identities, shader names/families, GUIDs, local
  handles, group indices, 40,663 role bindings, 67,475 parameter entries, and
  per-mesh source provenance;
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
  one-sided dead spots. SKATE v12 also transports all native RenderWare edge
  codes through Blender face attributes so hard/smooth edge and vertex
  contacts are not guessed during engine compilation.
- The current UTT presentation parser computes averaged geometry normals;
  authoritative packed retail normals are not yet transported.
- All discovered texture roles and shader parameters are transported.
  Conventional normal/lightmap slots remain available for generic authored
  rendering, while the exact family path consumes named retail channels.
  Retail cube-map face/array shape is not yet represented; environment
  bindings currently resolve to their decoded 2D image and use the renderer's
  neutral cube fallback.
- AI routes, hinged doors, and local lights are not recovered yet.
- All 183 packed retail collision surfaces are preserved, including the three
  surface IDs that use native physics channel 13.
- The `WMET` extension retains source stream, declaration, offset, bounds,
  simulation, collision, grind, and texture metadata. A hot runtime
  object/instance table is not yet reconstructed, so presentation transforms
  remain baked into vertices.

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
