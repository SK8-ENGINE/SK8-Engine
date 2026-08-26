# Custom maps

This fork loads renderer-neutral `.skate` packages created by the original
Blender exporter in `tools/blender_owned_map`.

## Installing and switching maps

1. Start the game and open **Settings > Maps > Open Maps Folder**.
2. Copy one or more `.skate` files into that folder.
3. Select **Refresh Map List**.
4. Choose a package and select **Load Selected Map**.

The session restarts automatically when a map is changed. This is an
intentional release boundary: it guarantees that static collision, moving
physics objects, hinged doors, grind splines, textures, shadows, mirrors,
water, weather, and renderer caches are rebuilt against one coherent world.
The selected package is remembered for the next launch.

Release archives contain a `maps` folder beside the executable, so that is
the active folder for a normal extracted build. It includes the first-party
Blender Feature Park demonstration and selects it automatically on a fresh
installation. Its editable, self-contained `.blend` source is included beside
the playable package. If no local `maps` folder is present, the game uses:

- Windows: `%APPDATA%\skate3\maps`
- portable mode: `maps` beside the executable

The Maps tab always shows and opens the exact folder currently in use.

## Package compatibility

The runtime accepts little-endian SKATE v1-v14 packages. Current Blender
exports use v14 and can contain:

- chunked visual geometry and embedded image textures;
- albedo, normal, ORM, emissive and baked-indirect maps;
- opaque, alpha-cutout and alpha-blended materials;
- independent collision geometry;
- Skate 3 audio, physics and contact material channels;
- grind paths;
- contact-driven hinged rigid doors;
- ordinary Blender Point, Spot, Area, and Sun lights;
- spawn, sky, day/night, weather, water, mirror and moving-light metadata.
- stable per-object identity, render/collision ownership, transforms, and
  grind associations used by the in-game editor.
- opt-in Box3D static and dynamic bodies with per-object shapes, density,
  contact properties, damping, gravity scale, and sleep state. Physics is
  disabled by default for older packages and unmarked objects.

Spawnable `.skateobj` v2 packages may contain multiple independently
simulated roots. The tracked `box3d_cube_pyramid.skateobj` example contains a
static base and ten separate dynamic cubes; its deterministic Blender
generator is `tools/blender_owned_map/create_box3d_cube_pyramid.py`.

NPC route records are an experimental preview. The exporter retains them for
future testing, but map authors should not currently rely on AI skaters
remaining on an authored route.

Compatibility is deliberately one-way:

- New Custom Engine Layer releases continue to read older `.skate` versions
  and supply safe defaults for fields that did not exist yet.
- An older game release cannot understand features added by a newer exporter.
  If it finds a future package version, the Maps menu marks it **Update
  Required** and refuses to restart into it.
- Updating the bundled Blender addon does not rewrite existing `.blend` or
  `.skate` files. Install the newer addon manually when you want to export
  newer format features.

This means old maps are backward compatible with the current engine. New maps
are not guaranteed to be forward compatible with old engines; they fail
cleanly rather than being partially interpreted.

Open **Settings > World** while playing to pause or scrub the authored
day/night clock and tune its speed, range, sun direction, sky RGB colour,
directional sunlight RGB/strength, and ambient light. These changes are live
session overrides. **Restore Map Defaults** returns to the values embedded
by the map author without rewriting the `.skate` file.

Large packages can use substantial CPU and GPU memory. Loading through the
menu restarts the old process and includes a shutdown watchdog so a stalled
guest heap cannot leave a second multi-gigabyte process running.

## Blender exporter

Release users install `Blender Map Tools/owned_world_material_addon.zip`
through Blender's **Edit > Preferences > Get Extensions > Install from
Disk**. From a source checkout, run
`tools/blender_owned_map/Build-Addon.ps1` first; its generated ZIP is ignored
and must not be committed.
The addon contains the complete exporter. Open **3D View > Sidebar >
Skate 3 Map**, place the player spawn, and export. An ordinary unconfigured
`.blend` is prepared automatically: visible meshes, Principled materials and
textures, UV channels, genuine Blender lights, and sensible static collision
are adopted without scripts. Manual visual, collision, grind, material, door,
and experimental NPC controls remain available as overrides.

Full UI guidance, scene conventions, and optional command-line automation are
documented in `tools/blender_owned_map/README.md`; the binary specification
is in `SKATE_FORMAT.md`.

The export panel enables **Editable Map Objects** by default. Disable that
option when a map should keep all of its normal rendering, collision,
materials, and grind paths but expose none of its existing geometry to the
in-game editor. This avoids per-object editor records without changing the
playable static map.

The exporter and runtime are original code and do not invoke, bundle, or
depend on ArenaBuilder.

## Distribution rules

The game, source archive, and release package must not contain Skate 3 retail
files, DLC, extracted executable data, or proprietary custom maps. SKATE can
embed all source textures and geometry, so map authors must have permission
to distribute every asset in a package.
