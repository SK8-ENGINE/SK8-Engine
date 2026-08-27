# Agent instructions

This is the public SK8 Engine source repository. The Skate-specific runtime is
the `third_party/rexglue-sdk` Git submodule. Preserve unrelated user changes and
never reset, clean, or broadly revert the working tree.

## Imported-map development

Imported maps are local test inputs, never repository content. Keep source
maps, converted Blender scenes, textures, generated `.skate` packages,
inventories, plans, screenshots, launchers, logs, and extracted assets under
ignored `out/local-tools/` paths or outside the repository. Do not stage or
commit them, even when they expose an exporter or renderer defect.

Use imported-map testing to improve universal Blender authoring, export,
runtime rendering, collision, validation, and agent-workflow behavior:

- treat Blender as the visual authority for authored maps;
- fix reusable pipeline behavior instead of adding map-name, object-name,
  material-name, coordinate, or asset-specific exceptions;
- reproduce regressions with minimal synthetic geometry or metadata in
  portable tests, without copying the imported map or its assets;
- keep the checked-in `tools/blender_owned_map/agent_workflow/sk8-auto-map`
  skill and its public documentation synchronized with workflow changes.

Eligible changes are portable source, schemas, tests, documentation, and
reusable tooling. Existing first-party demonstration assets are unrelated to
this imported-map test policy and do not authorize adding another map.

## Releases

Read `RELEASE.md` before publishing. When the user asks to commit, push, and
release, treat the steps below as the canonical runbook. Do not redesign the
release process or repeatedly rebuild unchanged source.

1. Inspect both repositories and state exactly what will be committed. Commit
   and push the runtime submodule first, then commit its resulting pointer and
   the authorized top-level changes.
2. Run the owned-world tests and both supported Blender addon workflow tests
   once. Do not claim that automation visually validated gameplay.
3. Create the intended version tag locally before configuring the release
   build so the executable embeds the exact release version. Do not push the
   tag or publish anything until packaging succeeds.
4. Build the tagged source from an isolated recursive checkout using the
   developer's legal game-data directory outside the repository.
5. Seed that checkout with the complete, previously validated ignored
   `generated/` directory from the canonical workspace before configuring or
   building.

### Critical TU3 generated-code rule

Do **not** run `generate-all` during a release when the available input is the
loose/stable-base XEXP data. That path has produced only 45 of the required
1,727 TU3 function roots. A release requires the validated generated set with
all 1,727 roots.

If an isolated checkout lacks `generated/`, copy the entire validated
`generated/` tree from the canonical workspace, refresh its timestamps if the
build system has stale objects, configure, and build once. The package script
must report:

```text
Verified TU3 function coverage: 1727/1727 roots registered.
```

If no validated 1,727-root generated cache is available, stop and report the
release as blocked. Never publish a partial generated build and never commit or
package the ignored generated tree.

### Package and publish

1. Run `scripts/Package-Windows-Release.ps1` with the exact version and release
   build directory.
2. Confirm the ZIP contains only the intended distributable files and no
   retail data, generated code, caches, logs, saves, screenshots, or research
   artifacts.
3. Confirm the executable version, ZIP size, SHA-256, generated updater
   manifest, and the package script's 1,727/1,727 registration result.
4. Push the source tag and create a GitHub **prerelease** with the ZIP.
5. Download the published asset again and verify that its size and SHA-256
   exactly match the local package.
6. Copy the verified generated manifest values into
   `release/update-manifest.toml`, commit that manifest after the tagged source
   commit, and fast-forward the public default branch.
7. Finish by reporting the release URL, source tag commit, default-branch
   commit, runtime commit, asset size, SHA-256, and tests performed.

The release tag intentionally points to the source used to build the binary.
The default branch may be one small commit ahead containing only the verified
updater manifest.

## Release automation

Ordinary hosted CI must not be expected to fetch or store retail Skate 3 data.
A future GitHub Actions release workflow must use a secured self-hosted Windows
runner with the legal local game data and validated generated cache. Until that
exists, follow the local runbook above.

## Multiplayer visual validation

The user performs visual multiplayer testing. Process startup, logs, packet
telemetry, or two running clients are not proof of a visual fix. Ask for a
visual pass only when a justified build actually needs one.

For all multiplayer implementation work:

- Work only in the dedicated multiplayer Git worktree selected by the user.
  Keep multiplayer source changes, builds, staged clients, test artifacts,
  scripts, and telemetry output there. Do not modify the main checkout or
  another agent's worktree.
- Automated agents must never boot or launch Skate 3 or any real game client.
  They may compile and run non-game unit, protocol, synthetic, and offline
  telemetry tests.
- When in-game validation is justified, provide a convenient user-run Windows
  `.bat` launcher under the worktree's ignored
  `out/local-tools/<feature>/` directory. Never stage or commit that launcher.
  It must build the exact worktree source without falling back to an older
  binary, stage isolated clients, enable the required diagnostics, and write
  every run to a named timestamped directory.
- Before the user runs a visual check, state the exact launcher, client count,
  scenario, duration, visual success criteria, and visual failure criteria.
  Never execute the visual-check `.bat` as an agent.
- After the user reports that the run is complete, analyze the generated
  telemetry independently. Report the user's visual verdict separately from
  telemetry-backed findings. If the logs cannot answer a relevant failure,
  improve the instrumentation and prepare another user-run check rather than
  launching the game.

## Repository hygiene

- Never commit personal absolute paths, machine-specific launchers, local
  configuration, retail data, build output, telemetry, logs, generated
  archives, or extracted game content.
- Keep user-run `.bat` files under ignored `out/local-tools/<feature>/`.
  Portable project automation belongs in reviewed scripts with no personal
  defaults.
- Run `scripts/Test-RepositoryHygiene.ps1` before proposing a commit or pull
  request.
