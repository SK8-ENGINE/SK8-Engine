# Changelog

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
