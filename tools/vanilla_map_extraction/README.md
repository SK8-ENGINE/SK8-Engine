# Skate 3 Vanilla Map Extraction

This workspace is intentionally separate from `Source` so retail-map research
does not overlap multiplayer or engine development.

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
- 2,075,425 render vertices and 1,645,617 render triangles.

The generated scene is `blender/DIST_University.blend`. The 2,046 available
images are packed into the `.blend`, and models are grouped by source stream
cell. Twenty-three material IDs refer to shared textures that were not found
as resources in the available base-game archives; these are clearly tagged
white fallback materials.

To rebuild without making a preview render:

```powershell
.\tools\Build-UniversityBlend.ps1
```

Open the generated scene in Blender and use Home/Frame All in the 3D viewport
to focus the complete district. Visual validation is intentionally left to the
user. Simulation resources are preserved under `Collision_RAW`, but collision
geometry is not decoded yet.

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
