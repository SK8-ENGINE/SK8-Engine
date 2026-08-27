# Release guide

This repository is the top-level source fork. It intentionally keeps the
Skate-specific rexglue runtime as a Git submodule, matching upstream's
architecture. End users still receive one normal application archive; source
builders clone with `--recursive`.

The initial Custom Engine Layer preview is supported only on Windows/D3D12.
Upstream platform support is retained, but the custom shaders and renderer
changes must be validated before publishing Linux, Vulkan, or macOS builds.

Before publishing a source or binary revision:

1. Confirm the companion runtime submodule resolves from the public
   `SK8-ENGINE/SK8-Engine-Runtime` fork.
2. Confirm `generated/`, `game/`, runtime data, retail media, DLC, saves,
   `.skate` packages, Blender scenes, export caches, logs and screenshots are
   untracked.
3. Run the owned-world and standalone protocol/runtime CTest suites, the
   vanilla-map extraction Python suite, Python source compilation, and all
   five Blender addon/agent workflow tests.
4. Build the Windows Release target from a recursive clean clone.
5. Package with the script below and inspect the complete staged file list.
6. Confirm `NOTICE.md`, `LICENSE-PROJECT.md`, `KNOWN_ISSUES.md`, and checksums
   are present in the archive.
7. Upload the archive, then copy the generated
   `out/packages/update-manifest.toml` to
   `release/update-manifest-v2.toml` on the public default branch. The legacy
   `release/update-manifest.toml` is intentionally frozen at preview.15
   because that updater crashes before it can install a newer executable. Do
   not advertise an asset until its uploaded size and SHA-256 match the
   current v2 manifest.

## Windows binary package

After building the Release target, run:

```powershell
.\scripts\Package-Windows-Release.ps1 `
  -Version 0.1.0 `
  -BuildDirectory .\out\build\release
```

The packager rebuilds `owned_world_material_addon.zip` from its reviewed
source files before staging it, so the release always contains the same
self-contained GUI/exporter code as the repository.

It also emits `update-manifest.toml` beside the archive. The in-game updater
in preview.17 and newer reads the reviewed
`release/update-manifest-v2.toml` copy from the public default branch,
downloads only GitHub-hosted assets, verifies the exact size and SHA-256, and
then overlays release-owned files. It preserves `game`, `saves`,
`settings.toml`, `active_map.txt`, and user-added map files. Preview.15 and
preview.16 users need one manual install of preview.17.

For the canonical research workspace's existing build directory:

```powershell
.\scripts\Package-Windows-Release.ps1 `
  -Version dev `
  -BuildDirectory ..\build\skate3-custom-engine-layer-release
```

The default Windows archive is named
`Skate3CustomEngineLayer-<version>-Windows.zip`. The executable remains
`skate3.exe` for runtime and save-data compatibility.

The packager creates a zip containing only:

- `skate3.exe`;
- `rexruntime.dll`;
- map/player documentation;
- the first-party Blender Feature Park `.skate` map and its self-contained
  editable `.blend` source;
- project licensing, notices, changelog, and known issues;
- the Blender addon, format documentation, complete source-only map toolkit,
  regression tests, and portable `sk8-auto-map` agent workflow;
- the source-only vanilla-map extraction, Blender conversion, validation,
  schemas, and tests, without retail inputs or generated outputs;
- a release-facing `maps` folder for additional packages; and
- SHA-256 checksums.

It fails closed if the stage contains known retail, generated-code, map,
Blender-scene, cache, log, save, or diagnostic file types.

## Source validation

Clone into a new directory rather than relying on an existing submodule
checkout:

```powershell
git clone --recursive `
  https://github.com/SK8-ENGINE/SK8-Engine.git
```

The build still requires the developer's own extracted Skate 3 game data.
Never copy that data into the source tree used for publication.

## Runtime validation

Validate both a small map and a very large map through **Settings > Maps**.
Confirm the new process log names the selected package and that only one
`skate3` process remains after the five-second shutdown watchdog. Visual and
playability testing remains manual; release automation does not move the
skater or capture screenshots.
