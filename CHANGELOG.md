# Changelog

## 0.1.0-preview.10 - 2026-08-21

- Restored the Blender-owned map generators, included Feature Park artifacts,
  material shader, and owned-map GPU submission behavior from the stable
  pre-preview.9 baseline.
- Removed the preview.9 material/rendering regression that made signs turn
  black, washed out the world, and suppressed the expected baked-lighting
  presentation, while retaining the preview.9 multiplayer, mechanics, and
  character work.

## 0.1.0-preview.9 - 2026-08-21

- Replaced Steam's logical-host animation relay with authenticated direct
  peer-to-peer fan-out so every nearby player contributes their own upload
  instead of concentrating the complete session on one player's connection.
- Added **Auto**, **Bandwidth Saver**, **Balanced**, **High Fidelity**, and
  **Custom** multiplayer network-quality controls under the in-game
  Multiplayer settings page.
- Added sender-owned remote appearances for the Custom Engine Layer,
  including animated vanilla clothing, hair, body, board, and accessories.
  Exact CAC bind meshes are resolved from the local vanilla asset catalogue;
  the current bootstrap bundle supplies the remaining presentation data
  without using Skate 3's retail online/NPC presentation entities.
- Fixed local presentation ownership so installing or updating a remote
  appearance cannot replace, detach, or move the local player's clothing and
  hair.
- Added canonical skeleton and attachment-track replication with
  root-relative quantization, changed-bone keyframes, interpolation, and
  distance-aware animation/attachment routing.
- Expanded the included Feature Park and Blender showcase with richer
  self-contained PBR materials, improved normal/ORM generation, authored
  lights, metallic environment response, and corrected lightmap handling.
- Propagated complete material, texture, lightmap, normal, ORM, emissive, and
  alpha metadata to kinematic and water meshes.

## 0.1.0-preview.8 - 2026-08-20

- Expanded the Blender owned-world authoring tools and ordinary-scene import
  workflow.
- Fixed controller input while multiplayer runs through Spacewar (App 480) by
  installing a local standard-gamepad Steam Input action manifest.

## 0.1.0-preview.7 - 2026-08-19

- Fixed the immediate startup termination after installing the bundled TU3
  `default.xexp` by restoring the complete generated TU3 function table.
- Added a packaging check for all configured TU3 function roots so an
  incomplete generated executable cannot be released again.

## 0.1.0-preview.6 - 2026-08-19

- Restored Skate 3's live vanilla HUD over the Custom Engine Layer world,
  including trick text, notifications, pause UI, and loading UI.
- Keep the game's independent 2D/APT presentation stream enabled while the
  retail 3D world remains hidden.

## 0.1.0-preview.5 - 2026-08-19

- Added zero-configuration Steam multiplayer setup for Windows preview users.
  On first launch, the game now downloads the pinned Steamworks.NET standalone
  runtime directly from its GitHub release, verifies both the archive and
  extracted `steam_api64.dll` with fixed SHA-256 values, and caches it locally.
- Generate the App 480 development marker locally instead of requiring users
  to copy `steam_appid.txt` into the installation.
- Start the Steam client automatically when it is not running and retry Steam
  initialization while the client starts.
- Prevent failed setup from spawning a new installer every frame. Setup now
  makes one bounded attempt and writes a focused `.cel-steam/bootstrap.log`
  diagnostic on failure.
- Keep Valve's runtime and the App 480 marker out of the source repository and
  public release archive; they are acquired or generated on the user's own
  machine.

## 0.1.0-preview.4 - 2026-08-19

- Added the first multiplayer session flow under **Escape > Multiplayer**,
  including hosting, worldwide Steam lobby discovery, joining, leaving,
  privacy, password, late-join, and 2-100 player session settings.
- Added authenticated Steam Networking Messages transport with lobby-member
  role assignment, logical-host relay, map/protocol compatibility checks,
  relevance routing, stale-peer removal, and a localhost UDP fallback for
  development without Steam.
- Added exact remote Skate 3 character rendering from the sender's final
  animation palettes, including the skater, clothing, accessories, board,
  tricks, IK, and bails.
- Added timestamped interpolation and authoritative collision-tested root
  replication so remote skaters no longer walk through a wall while the
  source player remains blocked.
- Added distance-based animation routing, a 12-player local high-detail
  budget, low-rate distant presence updates, and burst-safe packet draining
  as the first large-lobby optimization pass.
- Added multiplayer telemetry, repeatable local multi-client launching, and a
  synthetic 99-peer root-routing load test.
- Initialize Steam before the input subsystem so Steam Input cannot reorder
  an already-created controller device when multiplayer starts.
- Added the multiplayer guide to both the repository and Windows release
  package.

## 0.1.0-preview.3 - 2026-08-18

- Blender addon 1.7.4 refreshes stale automatically imported materials
  from their live Principled shader graphs, fixing ordinary Blender maps that
  previously exported with zero textures after an incomplete first import.
  Editing a material through the addon UI transfers ownership to the author,
  so deliberate material overrides remain protected.
- The SKATE package loader now validates embedded texture bytes against the
  declared dimensions instead of applying the generic element-count ceiling
  to raw bytes. This restores the format's documented 4K/8K texture support
  while retaining dimension, payload-size, and package-size safety checks.

- Blender add-on 1.7.3 excludes imported player-size and scale-reference
  helpers at validation and package-writing boundaries, including scenes
  previously prepared by an older add-on.
- Blender add-on 1.7.2 reloads its exporter implementation after an
  in-process extension update, preventing a new panel from calling an older
  cached `export_scene` signature.
- Added one-click export for ordinary Blender scenes. Untagged visible meshes,
  Principled textures/material values, UV channels, genuine Blender lights,
  common collider naming conventions, and sensible static collision are now
  adopted automatically while preserving existing authored overrides.
- Kept player spawn, grind paths, doors, and experimental NPC routes as
  deliberate authoring choices instead of unreliable geometry guesses.
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
- Fixed one-click export of some imported Blender 5.1 meshes where Blender
  created a requested UV layer but returned `None` from its Python API.
- Replaced per-corner Python visual serialization with byte-identical bulk
  NumPy packing, chunked index generation, and buffered collision writes.
- Added live 0-100% Blender progress with named collision, visual, texture,
  write, and cache-hash stages.
- Hash clean packed-image source bytes directly for the incremental cache
  instead of expanding them to float RGBA, and make automatic fallback
  material selection deterministic across Blender launches.

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
