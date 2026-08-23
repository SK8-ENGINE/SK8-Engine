# Skate 3 Vanilla Map Extraction

This workspace lives only in the dedicated University linked worktree so
retail-map research does not overlap multiplayer, UI, or the primary checkout.
The original external extraction directory remains an untouched verified
backup.

Initial target: `DIST_MegaPark`.

Current target: the official base-game University district,
`DIST_University`. Blender visual testing is performed by the user.

Layout:

- `raw/mega_park/` — files extracted verbatim from
  `worldDIST_MegaPark.big`.
- `raw/hawaiian_dream/` — unrelated `DHS by DH13` community-map research;
  this was a false lead and is not Danny Way's Hawaiian Dream.
- `raw/university/` — the official Xbox 360 `DIST_University` archive
  extracted verbatim.
- `blender/` — generated Blender scenes and import scripts.
- `intermediate/` — decompressed RX2 resources, PNG textures, NumPy geometry,
  and generated manifests.
- `tools/` — preservation-first extraction and build code.

Retail game assets are local research inputs and should not be committed or
distributed.

## University extraction status

The official Xbox 360 base-game archive
`C:\sk83_recomp\assets\data\content\worldDIST_University.big` was extracted
into `raw/university/`. Its stream contains 647 files totaling 798,135,872
bytes:

- 2,665 unique presentation assets;
- 479 model resources producing 8,546 Blender mesh objects;
- 2,735 texture stream records producing 2,046 unique decoded textures;
- 1,013 preserved simulation/collision resources;
- 121 simulation resources containing 4,201 original grind rails and 27,008
  native cubic segments;
- 2,075,425 render vertices and 1,645,617 render triangles.

The generated extraction scene is `blender/DIST_University.blend`. The 2,046
available images are packed into the `.blend`, and models are grouped by
source stream cell. The current extraction resolves all 328 diffuse texture
IDs used by the University presentation meshes; there are no fallback
textures. It also preserves the complete bound retail channel table:
141 normal, 200 specular, 1,271 lightmap, 86 decal, 11 detail, 8 macro
overlay, 6 environment, 2 secondary-normal, and 1 noise texture IDs. Every
bound resource resolves to a decoded texture.

To rebuild without making a preview render:

```powershell
.\tools\Build-UniversityBlend.ps1
```

`blender/prepare_university_owned.py` converts that extraction scene into the
owned-world authoring contract and saves
`blender/DIST_University_Owned.blend`. It preserves base UVs, material
textures, transforms, and the full visual mesh. The configured UTT model
parser currently supplies geometry-derived averaged normals rather than
authoritative packed retail vertex normals. Tangent-space retail normal maps
are transported independently. The current owned package has:

- 526 materials and 462 embedded textures;
- 471 opaque, 50 alpha-mask, and 5 alpha-blended material variants;
- 2,987 mesh parts using 135 conventional retail normal maps;
- 2,081,271 indexed visual vertices and 1,645,617 render triangles;
- 1,133,642 cleaned retail collision triangles;
- 4,201 retail grind rails containing 27,008 exact native cubic segments;
- bounds from `(-727.373, -6.849, -1413.082)` to
  `(807.640, 296.100, 792.678)`;
- a runtime spawn at `(330, 133.006, -710)` on a flat wooden starting deck
  inside Super Ultra Mega Park.

The 1,013 retail simulation resources remain preserved. All 301 native
RenderWare `ClusteredMesh` sections are decoded into 1,133,649 source
triangles across 183 packed retail surfaces. Export rejects six degenerate
triangles and one same-wound duplicate. It now retains all 10,691 intentional
reverse-wound partners, which provide two-sided retail collision and were
previously a likely cause of dead patches. It also preserves every retail
RenderWare edge/corner feature code instead of regenerating smooth and hard
contact metadata from flattened Blender geometry. An offline verifier checks
geometry to 1e-6 metres, winding, counts, reverse-wound coverage, packed
audio/physics/pattern surface channels, and native edge-code multisets against
the retail RX2 source. AI routes, doors, and local lights are not yet
recovered.

The SKATE v11 package is
`intermediate/university/University.skate`. It is 130,320,474 bytes after
adding retail normal maps, restoring two-sided collision, and preserving
native collision feature codes, and losslessly
decodes to the counts and bounds above. The offline engine validator also
compiles it into 515 render chunks, a 4,023,600-byte native
`tSplineData` blob, and one continuous native collision mesh containing
32,768 clusters.

### Exact retail grind splines

University's 121 spline-bearing simulation RX2 resources use Pegasus
`RWOBJECTTYPE_SPLINEDATA` (`0x00EB0004`). Extraction retains every rail,
including repeated retail IDs with different geometry. Each segment preserves
the original 120-byte cubic coefficient/auxiliary payload; only relocated
runtime links are discarded and regenerated by the engine.

Blender displays these as orange Bezier curves in `Retail_Grinds`, and the
owned scene links them into `OW_GROUP_4_GRINDS`. The exporter verifies that
the curve points and handles still represent the stored retail cubic before
writing SKATE v11. `verify_university_grinds.py` independently reconstructs
the expected package section from `manifest.json` and requires an exact SHA-256
match. Current verified totals are 4,201 rails, 27,008 segments, and 372
closed loops.

### Exact retail material binding and foliage alpha

The first Blender and in-game passes exposed a source-extraction fault:
material parameter records are not stored in render-mesh order. Positional
matching therefore assigned fundamentally wrong textures to nearly every
mesh; the Blender addon, SKATE exporter, engine loader, and renderer were all
faithfully carrying those already-wrong assignments.

The extractor now follows the authoritative RX2 reference chain. Each
material `Name` record carries a 64-bit GUID, the `0x00EB000B` external
reference table maps that GUID to a local `0x00EB0066` material handle, and
each `0x00EB0023` mesh descriptor names the handle it draws with. This also
preserves distinct local material instances that share a GUID. All 479 model
resources and all 8,546 mesh parts resolve through this chain with no
fallback.

Foliage alpha is derived only after the exact material is known. The current
scene contains 6,389 opaque, 2,118 alpha-mask, and 39 alpha-blended mesh
parts. The Blender importer stores the retail GUID, local handle, source
material-group index, texture ID, shader, and alpha mode on every object. The
offline validator checks those fields and selected non-positional regression
bindings.

The manifest retains all known retail texture roles. The current conservative
normal pass binds 135 conventional tangent-space maps to 2,987 mesh parts and
proves that the same 135 IDs reach linear SKATE normal slots. Seven
shader-specific resources remain recorded but intentionally unbound: two
non-neutral generic defaults, the primary/secondary Skate 3 water pair, and
three unusual palm maps. Specular, lightmap, decal, detail, macro-overlay,
environment, noise, and secondary-normal behavior still needs semantic
reconstruction before claiming 1:1 visual parity.

### Point-level collision diagnosis

`tools/inspect_university_point.py` compares package collision and render
triangles around any local telemetry coordinate and maps collision triangles
back to their original retail `cSim` asset. It also decodes the material's
runtime RenderWare surface channels, keeping package object IDs distinct from
retail packed surface values.

```powershell
python .\tools\vanilla_map_extraction\tools\inspect_university_point.py `
  --point 255.25 74.34 -625.60 --radius 4
```

The game-side `native-collision-line-hit` telemetry records decoded triangles
hit by native line or box queries within four metres of the player. Those
queries are useful for support/raycast diagnosis but are not evidence that a
triangle physically stopped the skater.

`native-collision-line-contact` aggregates every unique successful triangle
test in that path over each telemetry interval. It reports hit frequency,
triangle normal/vertices, the native start-plus-delta query, hit fraction,
resolved hit position, and result normal. Direction vectors are never
translated as points, so the reported delta length is the actual probe length
and a blocking side face cannot be hidden by whichever support triangle
happened to run last.

The `accepted` count is recorded later, after Skate applies the native
audio/physics/pattern masks for the enclosing query. A geometric triangle hit
with `accepted=0` was rejected before it could influence that query.
`selected` is stricter: it increments only when the accepted candidate
replaces the native result collector's current closest contact. Blocking
diagnosis must use `selected`, because an accepted triangle can still lose to
a closer candidate.

`native-collision-physical-contact` separately brackets RenderWare
`PrimitivePairIntersect` and records only successful triangle-volume
intersections within four metres of the player. It reports the exact triangle,
contact frequency, the other primitive's flags/radius, and player distance.
Both paths are read-only instrumentation: neither filters triangles nor alters
contact results.

## User-run University visual check

From the root of the dedicated University worktree, run:

```powershell
.\Run-University-Visual-Check.bat
```

The BAT only launches the build previously prepared and validated offline by
the map agent. It never builds, exports, copies, or deploys content. Before
launch it verifies SHA-256 hashes for the prepared executable, runtime, and
University package, then enables owned collision, map-loader telemetry, and
renderer performance telemetry.

Every invocation uses
`out/university-visual-check/runs/<yyyyMMdd_HHmmss>/`. Its `logs/` directory
contains the exact launch arguments, prepared hashes/commit metadata, and
`skate3_university.log`. The launcher prints the exact path before starting
the game and reports failures clearly.

Agents must never execute this `.bat` file or launch `skate3.exe`. Offline
preparation is performed separately with:

```powershell
.\tools\vanilla_map_extraction\tools\Invoke-UniversityVisualCheck.ps1
```

That command rebuilds stale Blender/package outputs, compiles the game,
validates the actual engine loader/render/collision worlds, and stages
`out/university-visual-check/prepared/`, but cannot launch the game.

Visual correctness remains a user decision. Passing loader, counts, bounds,
hash, render-chunk, collision-chunk, and telemetry checks does not prove that
the map looks or skates correctly.

## Hawaiian Dream extraction status

The local `DHS by DH13` package has been extracted into
`raw/hawaiian_dream/extracted/`. This package is a 2026 community-built map
and was incorrectly identified as Hawaiian Dream during the first pass. It is
not the official Danny Way DLC. Its world stream is `DIST_DHS 32221` and
contains:

- 20 spatial cells plus a global presentation stream;
- 74 presentation model assets producing 497 Blender mesh objects;
- 108 decoded texture assets;
- 58 preserved simulation/collision assets;
- 1,566,707 render vertices and 1,086,530 render triangles.

The generated scene is `blender/Danny_Way_Hawaiian_Dream.blend`. Images are
packed into the `.blend`, and objects are grouped by source stream cell. The
`Collision_RAW` collection contains metadata objects pointing to the preserved
simulation RX2 files; collision geometry is not decoded yet.

To rebuild without making a preview render:

```powershell
.\tools\Build-HawaiianDreamBlend.ps1
```

Open the generated scene in Blender and use Home/Frame All in the 3D viewport
to focus the complete map. Visual validation is intentionally left to the
user.

## Mega Park extraction status

`worldDIST_MegaPark.big` has been extracted into `raw/mega_park/`. The archive
contains 26 files (3,591,912 bytes):

- 6 presentation model assets;
- 48 presentation texture assets;
- 4 additional presentation/simulation-type assets;
- 19 simulation assets;
- stream manifests and spatial metadata.

Mega Park is represented as a separate world layer that occupies six cells
also present in `DIST_University`:

- `50_-150`
- `50_-50`
- `150_-150`
- `150_-50`
- `250_-150`
- `250_-50`

The coordinate-cell XSF files in the Mega Park archive are empty 128-byte
stream headers; its real payload is in `cPres_Global.xsf` and
`cSim_Global.xsf`. The matching University cells contain approximately
21.8 MB of presentation and simulation payloads. A complete Blender
reconstruction will therefore combine the Mega Park layer with only those
University cells and any asset IDs they reference, rather than extracting the
entire 761 MB University archive.

## Build the first Blender scene

Run:

```powershell
.\tools\Build-MegaParkBlend.ps1
```

This performs two local stages:

1. `prepare_mega_park.py` decompresses both CPU and GPU sections of each
   streamed resource, preserves the original reconstructed RX2 files, decodes
   textures to PNG, and writes lossless mesh arrays plus `manifest.json`.
2. Blender runs headlessly and creates `blender/DIST_MegaPark.blend` plus a
   preview render.

The first scene imports the six world-space presentation sectors as separate
material mesh parts with their original asset IDs, source offsets, UVs, and
diffuse textures. Simulation RX2 resources are retained and represented in a
`Collision_RAW` collection, but their collision geometry still needs a proper
decoder.
