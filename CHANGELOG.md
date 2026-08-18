# Changelog

## Unreleased

- Made Blender NPC routes and grind collections genuinely optional during
  export instead of failing when their collections are absent.
- Added full-map collision validation, grouped object-level diagnostics, and
  non-destructive export-time cleanup of harmless zero-area and duplicate
  collision triangles without changing visual meshes or UVs.
- Kept wrong-facing rideable surfaces as explicit blockers to catch inverted
  ramp collision before it can cause instant bails in game.
- Removed ordinary maps' artificial 128-metre native-collision seams by
  compiling one continuous RenderWare KD collision mesh whenever format
  limits permit; spatial chunking is now only an oversized-map fallback.
- Added a one-button Windows release updater under **Settings > System** with
  asynchronous progress, GitHub-only downloads, exact size/SHA-256
  verification, staged installation, and automatic restart.
- Preserved retail game data, saves, settings, and user maps during updates
  while refreshing shipped files and the downloadable Blender addon zip.
- Fixed SKATE v8 names in the map browser and marked future format packages
  as **Update Required** instead of attempting to load them.
- Documented backward-compatible old-map loading and safe rejection of maps
  created by newer, unsupported exporters.

## 0.1.0-preview.2 - 2026-08-18

- Bundled the original Blender Feature Park `.skate` map so a fresh
  installation starts in the complete demonstration world.
- Added the self-contained Blender 5.1 source scene under the top-level
  `maps` folder as a working authoring example.
- Updated release packaging and documentation to include only these two
  explicitly reviewed first-party map files while continuing to reject
  arbitrary map and Blender payloads.

## 0.1.0-preview.1 - 2026-08-18

First public Windows/D3D12 preview.

- Added the Custom Engine Layer as the normal, default gameplay world.
- Added `.skate` map discovery and switching through **Settings > Maps**.
- Added the Blender 5 authoring addon and SKATE v8 exporter.
- Added authoritative static collision and native grind registration.
- Added image-backed PBR materials, transparency, baked indirect light,
  dynamic sunlight, shadows, local Blender lights, and day/night controls.
- Added contact-driven hinged doors.
- Added experimental water, weather, wet surfaces, mirrors, moving platforms,
  and ray-traced reflection paths.
- Added runtime world-lighting controls through **Settings > World**.
- Removed retail static-world collision and background world actors while the
  custom layer is active.
- Added release packaging guards that reject retail game data and proprietary
  map payloads.

NPC route export remains experimental and is disabled in the included Feature
Park authoring generator.
