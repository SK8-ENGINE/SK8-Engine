# Project-owned Blender map pipeline

This directory is an original Blender-to-SKATE pipeline for the owned world
runtime. It does not invoke, import, redistribute, or depend on ArenaBuilder.

The exporter treats the Blender file as authoring data and writes one
renderer-neutral package containing:

- visual triangles with base-texture UVs and independent lightmap UVs;
- losslessly compressed RGBA8 base textures and baked indirect-light
  textures;
- normal, packed ORM, emissive, cutout, and blended-transparent materials;
- native Skate 3 audio, physics, and contact-pattern channels per material;
- independent authoritative collision triangles;
- named grind centerlines;
- experimental native-AI skater route records with population, speed, and
  spacing controls;
- contact-driven hinged rigid doors;
- ordinary Blender Point, Spot, Area, and Sun lighting;
- spawn and accelerated day/night metadata, including complete day,
  twilight, and night palettes plus sun, moon, ambient, and sky colour.

## Install the addon

The single installable file is:

`tools/blender_owned_map/owned_world_material_addon.zip`

In Blender 5:

1. Open **Edit > Preferences > Get Extensions**.
2. Open the menu and select **Install from Disk**.
3. Choose `owned_world_material_addon.zip`.
4. Enable **Owned World Authoring** if Blender does not enable it
   automatically.
5. Return to the 3D View, press `N`, and open the **Skate 3 Map** tab.

The zip contains the complete SKATE exporter. End users do not need the
repository's Python files and do not need to use Blender's scripting
workspace.

## Existing Blender map: one-click workflow

An ordinary `.blend` file does not need to be rebuilt around SKATE
collections before export:

1. Open the map and save a working copy.
2. Select **Create / Select Spawn Locator**, then move the four-metre
   `OW_SPAWN` pad to the desired player start and rotate its arrow to set
   heading.
3. Choose **Export As...**.

Validate and Export automatically adopts visible mesh objects, reads normal
Principled BSDF base-colour, normal, emission, roughness, metallic and alpha
inputs, copies existing UVs into the required map channels, assigns sensible
Skate contact defaults from material names, generates subtle normal, packed
ORM, and emissive maps where shaders only provide scalar values, and
generates static collision from solid visible geometry. Existing collider
conventions such as
`Collider`, `Collision`, `UCX_`, `UBX_`, `USP_`, and `UCP_` are recognised as
collision-only proxies. Common foliage, decal, backdrop, probe, and shadow-
helper names remain presentation-only or are omitted as appropriate.

Point, Spot, Area, and Sun objects that are genuine Blender lights export
automatically. Empties merely named `Point Light`, `Spot Light`, or similar
are not converted into invented lights. Existing `OW_*` authoring metadata is
never replaced by automatic defaults.

**Auto Prepare Blender Map** exposes the same conversion before export so its
choices can be inspected. It adds each eligible object to exactly one of the
five map groups while preserving unrelated collection links, source meshes,
existing UVs, and shader nodes. A mesh with no source UVs receives empty
required layers and is reported for normal Blender unwrapping. When real
Point, Spot, or Area lights are present, day and night ambient defaults become
zero so the scene is not double-lit.

Grind splines and experimental NPC paths remain deliberate authoring inputs;
they cannot be inferred reliably from arbitrary visual geometry.

## New authored map: detailed UI workflow

1. Select **Prepare Scene**. This creates five exclusive map-group
   collections and the movable spawn locator.
2. Model or import your map normally.
3. In **Material Map Groups**, highlight a material in the list and use the
   buttons directly underneath it. **1 Presentation + Collision** is the
   default; **2 No Presentation** is for collider-only meshes; and **3 No
   Collision** is for decals, vegetation, support railings, and phase-through
   geometry. A button moves every mesh using the highlighted material, making
   material ownership visible without repeatedly selecting scene objects.
   Clicking a material row also selects all visible meshes using it in the
   3D View and makes the first match active.
4. Put grind curves in **4 Grinds**. Alternatively, mark rail-top edges
   Sharp and choose **Create Grind Splines from Sharp Edges**; connected
   edges are consolidated into curve splines automatically.
5. Optional/experimental: put AI-skater curves in **5 Pathing**. In
   **Object Properties > Owned World NPC Path**, choose the skater count,
   speed, and spacing. Enable **Cyclic U** on the Blender spline for a loop.
   The data exports, but reliable native AI route following is not yet part
   of the supported release feature set.
6. If validation reports missing UV layers, select the affected meshes and
   choose **Create Missing UV Layers**. Existing UVs are copied when
   possible; a mesh with no UVs still needs a normal Blender unwrap.
7. Choose **Create / Select Spawn Locator**, move its 4x4 pad where the
   player should appear, and rotate the arrow for heading.
8. Set the map name, daylight range, sky colours, and SKATE destination.
   Expand **Advanced Lighting** to author twilight/night palettes, sun and
   moon colours and strength, day/night ambient light, and default sky
   grading.
   Add ordinary Blender Point, Spot, or Area lights anywhere in the scene;
   their colour, power, range, softness, direction, and spot cone export
   automatically. A normal Blender Sun controls the world sunlight.
9. Choose **Validate Map**. Every blocking problem is shown directly in the
    panel with the object name and suggested correction. Collision validation
    scans the whole map in one pass rather than stopping at the first mesh.
10. Choose **Quick Export**, or **Export As...** to select another
    destination.

The same exporter is also available through **File > Export > Skate 3 Custom
Engine Map (.skate)**.

During export, Blender's status bar and the map panel show a live percentage
and the current collision, visual, texture, write, or cache stage. Large-map
geometry is packed in bulk with Blender's bundled NumPy rather than serialized
one Python vertex at a time. This keeps the same float32 SKATE records and is
byte-checked against the scalar fallback; it does not simplify meshes, reduce
texture resolution, or remove materials. GPU compute is not used because this
work is Blender data extraction and binary file packing, where avoiding Python
scalar overhead is substantially more useful than transferring the data to a
graphics device. Complete float32 vertex records are indexed exactly, so
shared corners no longer duplicate position, normal, UV, lightmap UV, and
material data. SKATE v11 then applies bounded lossless DEFLATE to RGBA8
textures, vertices, indices, and collision. The loader reconstructs and
validates the original runtime records; export does not simplify meshes,
reduce texture resolution, omit maps, quantize attributes, or merge UV/hard
normal seams. Extracted retail collision can additionally carry exact
per-triangle RenderWare edge/corner feature codes as face attributes; ordinary
authored maps continue to generate those codes automatically.

Maps can contain any number of local lights. The renderer dynamically keeps
the lights relevant to the current view active instead of evaluating every
distant city light for every pixel. Emissive materials remain separate: they
appear self-lit, while a Blender Light object is what illuminates surrounding
geometry and the skater.

### Export modes

- **Fast / Automatic** fingerprints the scene, reuses unchanged geometry,
  and performs a full rebuild only when content changed. This is the normal
  choice.
- **Force Full Rebuild** ignores the incremental cache.
- **Lighting / Spawn Only** updates fixed-size map, spawn, sky, and daylight
  metadata in an existing package. A valid prior automatic export is
  required.

The result can be copied into the game's `maps` folder and selected through
**Settings > Maps**. Every lighting value is the map's load-time default;
players can experiment non-destructively through **Settings > World** and
restore the authored values at any time. **Dynamic Lighting** independently
disables the moving sun, moon, ambient fill, and their world shadows while
leaving the clock, sky, baked lightmaps, emissive materials, and local lights
active.

## Material and physics UI

Select a material and open **Material Properties > Owned World Material**.
The panel contains:

- one-click concrete, asphalt, wood, metal, grass, tile, glass, ice, stair,
  water, instant-bail, alpha-decal, and tree/vegetation presets;
- Skate 3 wheel/grind sound, physics behaviour, and contact-pattern menus;
- collision, friction, and bounce controls;
- base colour, baked lighting, normal, ORM, and emissive image slots;
- roughness, metallic, baked-lighting strength, emissive strength, and
  transparency controls.

**Sync Materials from Shaders** deliberately refreshes the PBR Presentation
tab from selected meshes' Principled shaders. It also overwrites maps created
by Auto Prepare, so changing shader roughness, metallic, emission, alpha, or
the small **Generated Normal Strength** value can be propagated later without
touching authored texture maps.

For a physical door, select the complete door-leaf mesh and open **Physics
Properties > Owned World Physics**. Choose **Hinged Door**, place the 3D
cursor on its hinge, and select **Set Hinge From 3D Cursor**. The same panel
controls limits, mass, damping, self-closing strength, maximum speed, push
response, friction, and bounce.

## Advanced Blender scene contract

Create these exclusive collections:

- `OW_GROUP_1_PRESENTATION_COLLISION`: default rendered and collidable meshes.
- `OW_GROUP_2_NO_PRESENTATION`: collision-only meshes. Each object has an
  `ow_material` custom property naming an exported presentation material.
- `OW_GROUP_3_NO_COLLISION`: rendered decals, vegetation, support railings,
  and phase-through meshes.
- `OW_GROUP_4_GRINDS`: curve objects whose splines become grind centerlines.
  Normal curves export as authored point paths. Extracted retail curves carry
  protected `skate3_retail_grind_*` provenance and exact native cubic
  payloads; moving their points or handles causes export to fail instead of
  silently replacing the original spline.
- `OW_GROUP_5_PATHING`: optional/experimental route records for native AI
  skaters. Object
  properties `ow_npc_skater_count`, `ow_npc_speed`, and
  `ow_npc_spawn_spacing` configure each route.

Create a movable mesh named `OW_SPAWN`; its world position and Z rotation
define spawn position and heading. Every presentation mesh needs `UVMap` and
`Lightmap` UV layers. Materials identify images with `ow_albedo_image`,
`ow_lightmap_image`, `ow_normal_image`, `ow_orm_image`, and
`ow_emissive_image`. Optional material properties include `ow_friction`,
`ow_restitution`, `ow_flags`, `ow_roughness`, `ow_emissive`,
`ow_baked_strength`, and `ow_display_color`. SKATE v2 also reads
`ow_alpha_mode`, `ow_alpha_cutoff`, `ow_audio_surface`,
`ow_physics_surface`, `ow_surface_pattern`, and `ow_collision_enabled`.
Scene property `ow_cycle_seconds` controls the day/night duration; set it to
zero to freeze lighting at `ow_start_hour`. Set `ow_cycle_ping_pong` with
`ow_end_hour` to animate from the start hour to the end hour and back without
traversing the unused part of the 24-hour clock.

Imported Skate 3 lightmaps must set `ow_lightmap_encoding` to
`skate3_retail_sqrt_linear_over_4`. The addon then preserves their RGBA8
pages byte-for-byte and applies the console's `encoded²` energy scale in the
exported material. Ordinary Blender bakes retain the authored
`encoded² * 4` decode, so the two sources do not accidentally differ by
four times their lighting energy.

To author a door, keep its mesh in Group 1, choose **Hinged Door** in
**Object Properties > Physics > Owned World Physics**, place the 3D cursor
on the hinge line, and click **Set Hinge From 3D Cursor**. The panel also
exports opening limits, mass, angular damping, friction, and restitution.
Door leaves are removed from static visual/collision output and exported as
independent hinge-local rigid bodies. A static collision proxy must not also
contain the marked leaf.

## Owned World Materials reference

The extension's Material Properties panel supports:

- all 94 native Skate 3 audio surfaces;
- all 13 native physics behaviors;
- all 16 native contact patterns;
- presets for concrete, asphalt, wood, metal, grass, tile, glass, ice,
  stairs, water, instant-bail hazards, alpha decals, and vegetation;
- normal, ORM, and emissive texture slots;
- opaque, alpha-cutout, and alpha-blended modes; and
- an independent collision-enabled switch.

Its Object Physics panel supports:

- static or hinged-door body type;
- 3D-cursor hinge placement and world-space hinge axis;
- opening limits and initial angle; and
- mass, angular damping, friction, and bounce.

Visual PBR and gameplay contact are intentionally separate. A material can
look like polished metal while using any desired wheel/grind sound and
physics behavior.

Blender is Z-up; SKATE is right-handed Y-up. The exporter applies
`(x, y, z) -> (x, z, -y)` to geometry, normals, the spawn, and grind paths.

`UVMap` is the repeating base-material layer. The included generator authors
it at a deterministic four-Blender-unit tile scale, so texel density does not
change with object size. `Lightmap` remains a separate non-overlapping atlas.

Group 1 intentionally uses the same evaluated object for presentation and
collision, while Group 2 supports independent collision proxies. During
build, the exporter evaluates the complete modifier stack (including
Solidify), transformed geometry, and final split/smooth loop normals without
destructively changing the source `.blend`. Prefer continuous rideable
surfaces and simple exterior shells; do not stack
intersecting solid boxes to form stairs or leave bottom faces coplanar with a
floor. Set `ow_upward_surface=true` on floor/ramp-only proxies.

The exporter safely omits zero-area and exact/opposite-wound duplicate
collision triangles from ordinary authored maps and reports every affected
object together. It does not edit the Blender mesh. An imported retail
collision object may opt into
`ow_preserve_opposite_wound_collision=true`; in that mode cyclic
same-wound copies are still removed, but reverse-wound partners are retained
for intentional two-sided collision. Do not enable this globally to mask
messy authored proxies. Non-finite coordinates and downward/vertical
triangles on an object marked **Rideable Top Surface** remain blocking errors
because they can create invalid contacts, instant bails, or inverted ramps.

At runtime, an ordinary map is compiled into one continuous native
RenderWare collision mesh with its own KD tree. This preserves shared-edge
adjacency across floors and ramps. Spatial chunking is used only if an
exceptionally large map exceeds the continuous native format's limits; the
fallback is recorded in `logs/skate3_*.log`.

## Rebuild the included Feature Park from the command line

From the canonical development workspace root:

```powershell
& 'C:\Program Files\Blender Foundation\Blender 5.1\blender.exe' `
  --background `
  --python Skate3CustomEngineLayer\tools\blender_owned_map\create_feature_park.py
```

In the development workspace this creates:

- `owned/maps/source/blender_bake_showcase.blend`
- `owned/maps/blender_bake_showcase.skate`

The generator performs a real 1024-square Cycles indirect-light bake and
builds the 360 by 336 metre `blender_feature_park`. The feature zones remain
centred within a broad flat safety apron. From spawn, the central
runway progresses through:

1. rough asphalt;
2. polished concrete;
3. wood ramp;
4. metal sheet;
5. ceramic tile;
6. slow grass; and
7. slippery ice.

The park also contains a bank, quarterpipe, stairs, manual pad, ledges,
straight/down/curved grind rails, transparent glass, alpha-cutout fencing,
normal/ORM test surfaces, emissive cyan/amber panels, a Point/Spot/Area light
lab, a heavy self-closing physics door, a cyan/amber baked-indirect alcove, and
a separate instant-bail hazard.

Water, mirrors, reflective puddles, weather, and moving platforms are omitted
because those runtime experiments do not yet have Blender addon authoring
contracts.

## Advanced command-line and incremental export cache

Normal exports create a local
`<package>.skate.export-cache.json` manifest. Before rebuilding, the exporter
fingerprints evaluated visual geometry, transforms, UVs, normals, material
assignments and properties, referenced image sources, collision, grinds, and
local lights.

- An unchanged scene reuses the existing static package and patches only
  spawn, sky, and day/night metadata.
- Changed content invalidates the fingerprint and performs a full export.
- `--metadata-only` skips the content fingerprint and patches the fixed-size
  metadata block. Use it when only spawn, sky, cycle duration, start/end hour,
  ping-pong mode, or orbit azimuth changed.
- `--force` always performs a full rebuild.
- `--adopt-existing-cache` migrates a package known to match its Blender
  source after verifying package counts and recording a static-payload hash.

Example fast lighting-only update:

```powershell
& 'C:\Program Files\Blender Foundation\Blender 5.0\blender.exe' `
  --background owned\maps\source\blender_bake_showcase.blend `
  --python tools\blender_owned_map\export_skate.py -- `
  owned\maps\blender_bake_showcase.skate --metadata-only
```

Cache manifests are generated and gitignored. If a package changes outside
the exporter, its file identity no longer matches the manifest and the
exporter verifies the static payload hash before reuse.

Addon contributors rebuild the installable archive with:

```powershell
.\tools\blender_owned_map\Build-Addon.ps1
```

That archive intentionally contains only `__init__.py`,
`blender_manifest.toml`, and the shared `exporter.py`. The Windows release
packager runs this step automatically before copying the addon.

The exporter never requires Skate 3 retail files. SKATE packages may embed
third-party textures or models, so only distribute a package when you have
permission to distribute all of its contents.
