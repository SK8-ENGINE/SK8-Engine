# Project-owned Blender map pipeline

This directory is an original Blender-to-SKATE pipeline for the owned world
runtime. It does not invoke, import, redistribute, or depend on ArenaBuilder.

The exporter treats the Blender file as authoring data and writes one
renderer-neutral package containing:

- visual triangles with base-texture UVs and independent lightmap UVs;
- embedded RGBA8 base textures and baked indirect-light textures;
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

## First map: UI workflow

1. Select **Prepare Scene**. This creates the visual, collision, grind, and
   NPC-path collections plus an `OW_SPAWN` marker.
2. Model or import your map normally.
3. Select rendered mesh objects and choose **Visual**.
4. Select collision mesh objects, ensure they have the matching visual
   material, and choose **Collision**.
5. Select curve objects representing rail centerlines and choose **Grind**.
6. Optional/experimental: select curve objects for AI skaters and choose
   **NPC Path (Experimental)**. In
   **Object Properties > Owned World NPC Path**, choose the skater count,
   speed, and spacing. Enable **Cyclic U** on the Blender spline for a loop.
   The data exports, but reliable native AI route following is not yet part
   of the supported release feature set.
7. If validation reports missing UV layers, select the affected meshes and
   choose **Create Missing UV Layers**. Existing UVs are copied when
   possible; a mesh with no UVs still needs a normal Blender unwrap.
8. Put the 3D cursor where the player should appear and choose
   **Set Spawn at 3D Cursor**.
9. Set the map name, daylight range, sky colours, and SKATE destination.
   Expand **Advanced Lighting** to author twilight/night palettes, sun and
   moon colours and strength, day/night ambient light, and default sky
   grading.
   Add ordinary Blender Point, Spot, or Area lights anywhere in the scene;
   their colour, power, range, softness, direction, and spot cone export
   automatically. A normal Blender Sun controls the world sunlight.
10. Choose **Validate Map**. Every blocking problem is shown directly in the
   panel with the object name and suggested correction.
11. Choose **Quick Export**, or **Export As...** to select another
    destination.

The same exporter is also available through **File > Export > Skate 3 Custom
Engine Map (.skate)**.

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
restore the authored values at any time.

## Material and physics UI

Select a material and open **Material Properties > Owned World Material**.
The panel contains:

- one-click concrete, asphalt, wood, metal, grass, tile, glass, ice, stair,
  water, and instant-bail presets;
- Skate 3 wheel/grind sound, physics behaviour, and contact-pattern menus;
- collision, friction, and bounce controls;
- base colour, baked lighting, normal, ORM, and emissive image slots;
- roughness, metallic, baked-lighting strength, emissive strength, and
  transparency controls.

For a physical door, select the complete door-leaf mesh and open **Physics
Properties > Owned World Physics**. Choose **Hinged Door**, place the 3D
cursor on its hinge, and select **Set Hinge From 3D Cursor**. The same panel
controls limits, mass, damping, self-closing strength, maximum speed, push
response, friction, and bounce.

## Advanced Blender scene contract

Create these collections:

- `OW_VISUAL`: mesh objects rendered by the owned renderer.
- `OW_COLLISION`: mesh objects used only for collision. Each object has an
  `ow_material` custom property naming an exported visual material.
- `OW_GRIND`: curve objects whose splines become grind centerlines.
- `OW_NPC_PATHS`: experimental route records for native AI skaters. Object
  properties `ow_npc_skater_count`, `ow_npc_speed`, and
  `ow_npc_spawn_spacing` configure each route.

Create an empty named `OW_SPAWN`. Every visual mesh needs `UVMap` and
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

To author a door, keep its mesh in `OW_VISUAL`, choose **Hinged Door** in
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
  stairs, water, and instant-bail hazards;
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

Collision is deliberately authored independently from render meshes. Prefer
continuous rideable surfaces and simple exterior shells; do not stack
intersecting solid boxes to form stairs or leave bottom faces coplanar with a
floor. Set `ow_upward_surface=true` on floor/ramp-only proxies. Export fails
on degenerate triangles, exact/opposite-wound duplicates, or a tagged
rideable triangle whose normal does not point upward.

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
