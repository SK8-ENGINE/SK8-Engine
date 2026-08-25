# Skate 3 UI extraction toolkit

This is a clean, read-only Python toolkit for studying the retail front-end
assets while the replacement UI is rendered natively in C++/ImGui. It does not
execute or import any program from the supplied archives, and none of these
formats are required by the game at runtime.

Current output:

- BIG/EB v3 archives: validated listing and extraction, including standard
  RefPack entries.
- APT + CONST: character definitions, frame controls, placement matrices,
  labels, action offsets and timing exported as JSON.
- GEO: shapes, render units, texture IDs, UV matrices and triangles as JSON.
- RPS3/RX2 (RenderWare 4 arenas): texture names, platform metadata, exact raw
  texture payloads, and dependency-free PNG decoding for Xbox 360 DXT1,
  DXT3, DXT5 and A8R8G8B8 textures.
- Bundle scan: pairs related files and writes a single manifest.
- Project extraction: reads the installed `fedata.big` and `fetexture.big`
  archives, preserves the exact retail APT/CONST/GEO/RX2 inputs, exports
  timeline and geometry metadata, decodes texture previews, and records source
  hashes in a native-UI asset-cache manifest. `--include-dynamic` adds
  `fedynamic.big`.
- Menu audio: resolves front-end event names and gains from the retail VLT
  database, follows their `sk8_menu` SPLC patch graphs, preserves the exact
  embedded XMA payloads, and decodes local float WAVs for the native runtime.

The parsers are bounds checked, reject unsafe archive paths, and never modify
their inputs. Existing output files are not overwritten unless `--force` is
passed.

## Usage

From this directory:

```powershell
python extract_ui.py big-list path\to\fedata.big
python extract_ui.py big-extract path\to\fedata.big extracted\fedata
python extract_ui.py apt-json extracted\foo.apt extracted\foo.const foo.json
python extract_ui.py geo-json extracted\foo.geo foo.geo.json
python extract_ui.py rw4-extract extracted\foo.rx2 extracted\foo_textures
python extract_ui.py bundle extracted bundle_manifest.json
python extract_ui.py project-extract C:\path\to\game C:\path\to\ui_asset_cache
python extract_ui.py career-main-states-json C:\path\to\ui_asset_cache `
  C:\path\to\flat_retail_image.bin `
  C:\path\to\ui_asset_cache\metadata\scenes\career_main `
  --copy-profile custom --force
python extract_ui.py menu-audio-extract C:\path\to\game `
  C:\path\to\ui_asset_cache\assets\audio\sk8_menu `
  --ffmpeg C:\ffmpeg\bin\ffmpeg.exe --force
```

Use `--include-dynamic` for the larger dynamic front-end set. Use repeated
`--prefix` options to target a narrower archive subtree. The project extractor
never changes the supplied game files and refuses to place its output inside
the game-data tree. `--update` hashes the source archives and reuses an
up-to-date cache, otherwise it refreshes the generated files. Decoded textures
include both a viewable PNG and raw RGBA pixels suitable for direct native UI
upload.

The generated cache contains copyrighted retail assets from the user's own
copy of Skate 3. It must remain local and must not be committed or
redistributed. The repository contains only extraction/decoding code.

## Asset-cache layout

`project-extract` produces a self-contained local cache:

```text
ui_asset_cache/
  manifest.json                 source archive hashes and extraction settings
  raw/                          exact retail APT/CONST/GEO/RX2 inputs
  metadata/                     parsed bundles, timelines, geometry and scenes
    scenes/career_main/
      index.json                available selection-state scenes
      category_0_option_0.json  flattened crossbar/category render states
      category_4_option_3.json
      game_settings.json        exact extracted Game Settings popup scene
  assets/                       decoded PNG previews and native RGBA payloads
    audio/sk8_menu/
      manifest.json             event, VLT, patch and stream provenance
      streams/*.xma             exact wrapped retail XMA1 payloads
      streams/*.wav             local 48 kHz float runtime decodes
```

The paths under `raw/` preserve the archive-relative retail paths. Generated
scene JSON refers back to source bundles, character IDs, frame labels,
ActionScript operations and bytecode offsets wherever those details determine
runtime behavior. This makes the generated result auditable without making a
screenshot the source of layout or timing values.

## Menu-audio pipeline

`menu-audio-extract` obtains the audio from the user's installed game rather
than shipping or recreating it. The extraction chain is:

1. resolve the authored `core_nav_up`, `core_nav_down`, `crossbar_up`,
   `crossbar_down`, `core_a_button`, `core_b_button`, `core_fade`,
   `crossbar_in`, `crossbar_out` and `core_popup` events in the `fe` VLT
   class;
2. follow `PtrN` section 1's event-name symbols to the matching relocated
   `skatercollections.bin` rows, then read each event's retail gain and
   `sk8_menu` sound enum;
3. map the enum through its SPLC event-to-graph indirection and retain
   authored graph/grain gains, pitch ranges, grain delays and random-choice
   grain groups;
4. copy every referenced embedded XMA1 payload byte-for-byte; and
5. ask FFmpeg to decode those payloads into 48 kHz mono float WAVs.

FFmpeg is used only as an XMA decoder and can be supplied with `--ffmpeg`,
discovered on `PATH`, or found at `C:\ffmpeg\bin\ffmpeg.exe`. The manifest
records source-archive and bank hashes, VLT expression, symbol and relocation
offsets, SPLC patch/root/grain metadata, stream offsets and sample counts. This
lets the native consumer be checked all the way back to the user's retail
files. The symbol relocation is important: a collection's nearby section-0
pointer can name an unrelated serialized row, while section 1 is the retail
event-to-row index used to recover the game-resolved sound enum.
The consumer follows the retail parameter path: live front-end volume setting,
VLT event gain, graph gain, grain gain and random gain are multiplied for
amplitude; graph and grain pitch ranges determine playback rate; each grain
waits for its independently authored base/random delay; and random grain nodes
select one uniform child. This timing is significant for the A/B cues: their
short layers are deliberately staggered rather than started as one composite
transient.

The native menu fires navigation audio only when the selected row actually
changes, confirm audio on selection, back audio before closing, and fade audio
at the dimmer's authored intro/outro points. Those spots come from the retail
ActionScript behavior; they are not inferred from a recording. The SPLC grain
parameters are retained in the manifest even where their runtime semantics
have not yet been assigned a native interpretation.

The extracted `menu_picker.const` bytecode symbols establish that vertical
`AptUp`/`AptDown` navigation calls `FE_SOUND_CORE_NAV_UP` and
`FE_SOUND_CORE_NAV_DOWN`. `crossbar_up`/`crossbar_down` belong to horizontal
category movement. Runtime telemetry separately confirms
`core_a_button`/`core_b_button` and identifies `crossbar_out`/`crossbar_in` as
the paired child-page transition cues.

### Runtime audio telemetry

`vanilla_ui_audio_trace.bat` records the recomp's existing read-only probes
while the user operates the real retail menu. It never builds, launches,
drives, or changes the game. Press Enter once to arm it, freely navigate with
Up, Down, A and B, then press Enter once more to stop. The resulting
timestamped JSON under `out/logs/` places processed controller changes and
calls to retail `GlobalFEPlaySound` (`0x825DFAF0`) on the game's frame
timeline. Each sound event includes its caller and guest `r3-r10` values.

For additional research, an optional
`out/logs/vanilla_ui_audio_trace_targets.txt` can replace the built-in target
with up to 32 guest function addresses. An absent or empty file selects
`GlobalFEPlaySound`.

## Career > Main pipeline

`career-main-states-json` emits one fully flattened, source-traceable scene for
every enabled row in all five executable-derived crossbar categories. In
addition to the settled state, every drawable receives a stable extraction key
and its exact world matrix. The extractor follows APT frame labels, `stop()`
actions (including playback that wraps to an earlier stop frame), display-list
moves and colour transforms to emit compact per-item motion tracks.

The command also emits `game_settings.json`. Its six top-level rows come from
the executable's Game Settings descriptor and index tables. Panel dimensions,
centering, content origin, 36-unit row pitch, Pulpboard texture, bitmap-font
measurements, and intro/outro timelines come from the shipped `options`,
`panel`, `highlight_option`, and `HighlightText` components. This screen is
display-only for now and does not mutate game configuration.

The default `custom` copy profile keeps the retail presentation but substitutes
project-owned placeholder text: Custom Maps, Custom Models, Mod Library,
Session Lab, Replay Studio, Creator Hub and Online Hub. Both labels and helper
copy are measured with the extracted bitmap-font metrics, so glow lengths
continue to follow the same source `setLength()` behavior. Every option retains
its original localized copy in `retail_copy` for provenance. Pass
`--copy-profile retail` to produce an untouched textual reference scene.

Career > Main currently exports:

- `animatein`, source frames 5-34 (30 frames / 480 ms);
- `outro`, source frames 35-46 (12 frames / 192 ms);
- the title `bounce`, category/icon startup, and row/icon startup clips
  triggered by the retail initialization ActionScript;
- adjacent row transitions reproducing the exact selected and unselected
  wrapper timelines plus the immediate `PositionColumn()` layout update; and
- the three synchronized 300-frame selected-glow alpha tracks from
  `menu_part_glow` and `menu_part_text_hilite`.

The native renderer plays all tracks at the packaged 16 ms frame duration.
It does not substitute procedural easing, fades, pulses, or screenshot-derived
timings. Each clip records its source bundle, character, label/frame sequence,
and the recovered ActionScript calls that trigger it.

Row spacing comes from the retail `PositionColumn()` ActionScript rather than
image measurement. The recovered constants are `COLUMN_Y = 225`,
`BASE_SPACING = 44` and `SELECT_MARGIN = 9`. The column receives the selection
margin only while option zero is selected. For later selections, the selected
row and the row immediately below it each receive a margin, producing 53-unit
clearance on both sides of the enlarged selected icon while ordinary gaps stay
44 units. The generated scene records the function's source bytecode range in
`layout.position_column_source`.

Each scene also includes the retail
`screens/main/dimmer` movie in its source provenance. The extractor resolves
its `intro` timeline to the authored stop frame and preserves the asset in the
local cache, but does not paint it on Career > Main. A settled live source
probe proves that this page's dark, slightly warm backdrop comes from the
retail fixed-resolution blur at kernel `8.0`, with `(90,85,81)/255` colour
modulation applied by both its horizontal and vertical passes. The native
Career backdrop reproduces that GPU chain directly; it does not approximate
the result with the brown RX2 texture or a clipped fullscreen ImGui dimmer.
The 1280x720 APT stage is mapped independently across both axes to the complete
guest-output paint rectangle, including odd-sized host frames.

## Native runtime bootstrap

`skate3_ui_asset_bootstrap` is a dependency-free C++ target that uses the same
BIG/RefPack/RW4/Xbox decoding rules for assets required during first boot. It
exists so packaged builds do not require Python:

```powershell
cmake --build out\build\release --target skate3_ui_asset_bootstrap
out\build\release\skate3_ui_asset_bootstrap.exe C:\path\to\game C:\path\to\ui_asset_cache
```

The game calls this native bootstrap when the user-local cache is absent. The
Python project extractor remains the comprehensive developer/research tool for
all timelines, geometry, original arenas, decoded textures, manifests, and
source hashes. Both implementations are regression-checked against the same
retail assets.

## Verification

Run the synthetic parser, timeline, layout and texture regression tests from
this directory:

```powershell
python -m unittest discover -s tests -v
```

Regenerate the six Career scenes after changing extraction behavior, then
compile the native consumer from the repository root:

```powershell
cmake --build out\build\release --target skate3 --config Release -j 4
```

The generated asset cache is intentionally ignored by Git. Commit extractor
and native-renderer source changes, never the user's extracted retail assets.
Visual checks are performed separately with the prepared worktree launcher.

The supplied `bigfile.exe` was used only as a static format reference. The
toolkit implements the relevant format handling itself.
