# Multiplayer Foundation

The current multiplayer slice lets independent Custom Engine Layer processes
exchange player poses and render each other in the same custom map. Steam
Matchmaking now supplies internet lobby discovery and Steam Networking
Messages supplies authenticated P2P transport. The previous same-PC UDP
backend remains available as an offline development fallback.

## In-game session menu

Open **Escape → Multiplayer** to use the first complete session flow:

- host a game with a name, 2-100 player limit, privacy preset, optional
  password, and late-join setting;
- refresh the server browser from another running client;
- select and join a compatible session on the same loaded map;
- leave a joined game or stop hosting without restarting the game.

When Steam is initialized, the browser searches worldwide for lobbies tagged
with the Custom Engine Layer game and protocol identifiers. This prevents
unrelated Spacewar test lobbies from appearing. The lobby owner coordinates
discovery and membership, but gameplay pose, animation, and appearance
packets travel directly between authenticated Steam peers. No player's
connection acts as a relay for the rest of the lobby.

If the Steam runtime is absent, Steam is not running, or the signed-in Steam
session becomes unavailable, the Multiplayer tab remains visible but disables
its controls and reports `Start Steam to use multiplayer`. Offline and
single-player play remain available. The game performs one actual Steamworks
connection check during startup. If that check fails, the disabled
page provides a `Check for Steam` button that performs one new check without
launching Steam or restarting the game. Once connected, the game continues to
monitor the live service state so a later disconnect degrades safely. Same-PC
discovery remains an explicit development/test mode rather than an automatic
user-facing fallback.

## Steam / Spacewar boundary

SteamDB's App 480 charts page reports how many users are running Valve's
Spacewar example. It is not a server browser or relay service. During private
development, AppID 480 initializes Steamworks so the project can exercise:

- Steam Matchmaking lobbies for server discovery, lobby metadata, privacy,
  admission and invites;
- Steam Networking Messages for authenticated P2P game packets over Steam
  Datagram Relay;
- Steam identities instead of local process roles.

The backend loads the Steamworks flat C ABI dynamically, so compiling the
project does not require Valve's headers or import library. On a clean Windows
installation, the game performs this development setup automatically:

- download the pinned Steamworks.NET standalone archive directly from its
  [published 2025.164.1 GitHub release](https://github.com/rlabrecque/Steamworks.NET/releases/tag/2025.164.1);
- verify the archive SHA-256 before extracting it;
- verify the extracted `steam_api64.dll` SHA-256 before loading it;
- cache the verified runtime under `.cel-steam`;
- generate the local `steam_appid.txt` marker containing `480`;
- connect to the signed-in Steam account when the user has Steam running.

The runtime DLL and App 480 marker are not committed or embedded in the public
archive. If automatic setup fails, read `.cel-steam/bootstrap.log`; multiplayer
remains disabled while offline and single-player play continue normally.

Spacewar does not open as a second game window. When setup succeeds, Steam
tracks the running `skate3.exe` process as App 480 and friends may see the user
playing Spacewar.

`steam_appid.txt` containing `480` remains a development override generated on
the user's machine. A production release must use this project's own Steam
AppID and follow Valve's setup and redistributable requirements.

## What works

- Roles 1-100 communicate directly through Steam Networking Messages when a
  Steam lobby is active. The lobby owner is role 1 for session coordination,
  but is not a gameplay-packet relay.
- Steam identities authenticate transport senders. Every receiver rejects a
  packet whose claimed role does not match its Steam lobby member.
- The fallback backend uses non-blocking localhost UDP with the same packets,
  roles, interpolation, and full-fidelity routing.
- The visual-check launcher enables the first protocol-v11-compatible
  replication worker. The render thread publishes its newest immutable local
  capture through a latest-wins mailbox and consumes immutable prepared remote
  presentations; transport polling, validation, reassembly, send scheduling,
  interpolation, recipient fan-out, and network telemetry run on the worker.
  One-shot peer retirements accumulate until consumed rather than being overwritten by
  a newer presentation. The worker remains disabled by default outside this
  controlled validation path.
- Installed remote appearances now retain their weighted palette-row masks,
  canonical remaps, board-piece classification, and validated animation-track
  ordinals. Steady rendering no longer rescans every clothing mesh's packed
  vertex influences or builds a second temporary canonical-remap vector for
  binding discovery each frame. Older compatibility appearances can still
  retry a missing live rig lookup.
- The visual-check path prepares remote recipe meshes and textures on one
  background asset worker. Requests are latest-wins per role and keyed by
  process session plus appearance identity, so a slow old decode cannot
  replace a newer wardrobe or reconnect generation. The render thread still
  validates the compact wire bindings and performs GPU resource creation.
  This path is disabled by default outside controlled validation while its
  visual and stall telemetry gate is completed.
- Configured localhost visual checks use direct peer fan-out, so role 1 does
  not relay every realtime fragment. Dynamic localhost sessions may retain
  role 1 discovery/relay compatibility. Internet Steam sessions use direct
  peer fan-out.
- Every receiver keeps an independent interpolation/reassembly timeline for
  every player and removes stale peers.
- Each client publishes its verified local board position, orientation, and
  board-state flags at the full-fidelity 60 Hz rate.
- Positions use custom-map coordinates rather than the hidden retail world's
  absolute coordinates.
- Received poses use their sender simulation timestamps rather than packet
  arrival timing. The balanced baseline adds no configured interpolation
  floor, but the monotonic presentation clock remains bounded to a completed
  skeletal interval. The five-client localhost acceptance run consequently
  retained about 74-76 ms of natural look-behind with zero held-latest
  playback or sequence gaps.
- Packets from a different map, protocol version, client slot, process
  session, or unexpected Steam identity are rejected.
- Peers advertise optional control capabilities with a fixed-size packet.
  Steam and configured localhost meshes send these advertisements directly;
  dynamic localhost discovery may use a directed host relay. Peers that do
  not advertise a feature retain the existing protocol-v11 behavior.
- Peers that advertise protocol-v12 support exchange a reliable 76-byte v12
  capability datagram over direct Steam or localhost connections. It uses
  explicit little-endian serialization and validates the current
  authenticated role/session plus map, build-contract, and content-contract
  identities before activating. Negotiated peers send root snapshots and
  continuous skeletal animation through the v12 envelope. Animation deltas
  reference only receiver-confirmed complete keyframes, split at the
  1,200-byte datagram boundary, expire instead of becoming reliable backlog,
  and request bounded keyframe recovery after an unavailable baseline.
  Protocol v11 remains the compatibility fallback.
- The v12 sender quantizes and serializes once for recipients with the same
  confirmed baseline, chooses the smallest exact raw, semantic, block, or
  predictive representation, then optionally Snappy-compresses those exact
  bytes. The receiver decompresses before the unchanged validated pose
  decoder. Snappy is a lossless outer wrapper; it does not change quaternion,
  translation, affine, board, wheel, or attachment precision.
- A stale remote player disappears after 1.5 seconds.
- The sender captures Skate 3's final rendered bone palettes after its
  animation graph, IK, tricks, and bails have evaluated.
- Once the sender has identified its exact local presentation entity, that
  entity remains the capture owner through bails and board separation.
  Board-distance proximity is used only during the startup fallback before
  an exact entity is known, so a separated skater is not replaced by the
  teal proxy merely for moving more than four map units from the board.
- The sender transmits one canonical skeleton plus compact exact-palette
  tracks for small or post-processed attachments such as the hat and wheels.
  Affine components use signed 16-bit fixed point with root-relative
  translations, are fragmented into bounded datagrams, reassembled, buffered,
  and interpolated at the render frame rate. The internet baseline samples a
  complete final skeletal pose at 20 Hz while root/board motion remains 60 Hz.
  This preserves every captured track and its existing quantization, but it is
  a temporal sampling change whose wheel, trick, bail, and detached-board
  behavior requires the user visual gate.
- The sender publishes a versioned engine-owned appearance recipe. The
  receiver resolves exact vanilla CAC bind meshes from its local asset
  catalogue and installs the sender's body, clothing, hair, accessories,
  materials, and board without using Skate 3's fragile retail online/NPC
  presentation entities. The normal `RCP1` path carries the compact CAC
  recipe plus model, track, topology, and remap bindings once on join and
  whenever the appearance changes. The older assembled mesh/texture format
  remains a bounded compatibility fallback. Incomplete appearance transfers
  are limited to 16 MiB per peer and 64 MiB in total and expire after ten
  seconds without a valid chunk. Peers that negotiate appearance-state
  control messages acknowledge complete byte receipt separately from
  renderer installation. The renderer reports installation only after the
  complete sender-declared recipe piece count resolves and commits, and the
  report is matched against role, process session, and appearance identity.
  Renderer texture ownership is isolated per role even when multiple players
  wear the same recipe. A reused role releases the previous process session's
  meshes and textures immediately. Definitively forgotten peer generations
  emit a session-matched retirement to the renderer; temporary menu or
  visibility changes do not unload and rebuild appearances. Steam and
  configured localhost meshes associate transfer state with each direct
  recipient. Localhost repeats the complete UDP stream three times so a
  single dropped chunk cannot permanently strand the proxy.
- Every active peer receives the complete root, canonical skeleton, exact
  appearance tracks, board, wheel, hair, hat, and attachment stream. Root
  motion is sampled at 60 Hz and complete skeletal poses at 20 Hz. Distance,
  visibility, importance, and player count do not select a lower tier.
- Every valid remote peer is reconstructed and rendered. There is no nearest
  player cap, skeletal decode suppression, or far-presence mode.
- Receivers use larger UDP buffers and drain bursty fragmented traffic
  without the old 256-packet frame ceiling.

## Full-fidelity policy

The current runtime has one replication quality: complete-pose fidelity. Legacy
quality, relevance-radius, attachment-radius, nearby-player-budget, and
far-presence settings remain registered so existing configuration files load,
but they do not downgrade or suppress another player's stream. Root snapshots
are sent at 60 Hz and complete final skeletal poses at the configured 20 Hz
internet baseline to every recipient. Remote reconstruction continues at
render cadence. The interpolation floor remains configurable because buffering
changes presentation latency and jitter tolerance, not spatial pose precision.
The balanced default is zero additional floor; positive values remain
available for explicitly impaired network conditions.

Adaptive tiers are intentionally deferred. They may be reconsidered only if
measured real-world full-fidelity sessions demonstrate a limit that cannot be
addressed first through compression, batching, shared serialization,
transport improvements, parallelism, or relay/server topology.

Both clients currently start at the same authored map spawn and may overlap
until one moves. Replication uses the exact collision-tested source position;
the debugging-only `skate3_multiplayer_local_lane_spacing` setting defaults to
zero because any presentation offset becomes spatially incorrect beside
walls, ramps, and other geometry.

## Launching two local clients

Build the project normally, close any running Skate 3 process, then run:

```powershell
.\scripts\Launch-Local-Multiplayer.ps1
```

Launch more real local clients with:

```powershell
.\scripts\Launch-Local-Multiplayer.ps1 -Clients 3
```

For implementation acceptance checks, automated agents do not launch the
game. The user runs the fail-closed visual-check script from the dedicated
multiplayer worktree, either directly or through an ignored local `.bat`
prepared under `out\local-tools`:

```powershell
.\scripts\Run-MultiplayerVisualCheck.ps1
```

It incrementally builds that worktree, stages fresh portable clients under a
timestamped `out\visual-checks` directory, enables multiplayer and renderer
telemetry, records the exact commit/diff and binary hashes, and writes
`out\visual-checks\LATEST.txt`. It never falls back to a binary from another
checkout. Each numbered role has separate persistent profile storage under
`out\visual-check-profiles/clientN`; a role's saved outfit therefore survives
later launcher runs while logs and binaries remain isolated by timestamp.
The first persistent profiles are seeded from the latest visual run when one
exists, preserving outfits already edited there. The generated
`clients\client3\relaunch-client3.bat`, or
`scripts\Relaunch-MultiplayerVisualClient.ps1 -Role 3`, restarts role 3 in the
same evidence directory after the user closes that client for a role-reuse
check. Visual correctness is the user's decision; the generated logs support
a separate telemetry review.

The script uses the sibling `Skate3CustomEngineLayer-Player` install by
default. Pass another clean install when required:

```powershell
.\scripts\Launch-Local-Multiplayer.ps1 `
  -SourceInstallRoot 'D:\Games\Skate3CustomEngineLayer'
```

For the current development bridge, the launcher automatically uses the
extracted Create-a-Skater database under the sibling `Skate3Research`
workspace when it is present. Pass another legal local extraction explicitly
with `-CacAssetRoot`. Without this database, ROPA shirts and hair fall back to
an already-deformed rigid snapshot and cannot remain attached to a remote
animation.

It stages two ignored portable roots under `out\local-multiplayer`, gives each
client independent writable user data, and junctions the legally supplied
`game` directory and user `maps` directory rather than duplicating them.
Direct boot is enabled for this developer test; add `-NoDirectBoot` to use the
normal frontend.

## Localhost root-presence load check

With one real process running as role 1, synthetic root-pose peers can exercise
the actual host socket and relay path. The script emits the current
protocol-v11 root packet:

```powershell
.\scripts\Test-LocalMultiplayerLoad.ps1 `
  -Bots 99 `
  -MapName blender_feature_park
```

The first 99-peer run sustained 18,810 upstream pose packets over ten seconds.
Every simulated peer received localhost relay traffic; aggregate outgoing
traffic received by the bots was 5.95 Mbit/s. The real process remained alive
and responsive. This validates 100-peer root presence and the localhost test
relay, not 100 simultaneous full animation streams or internet conditions.

## Measured skeletal-stream cost

A two-real-client protocol-v11 full-fidelity 60 Hz run on 2026-08-22
delivered about 54-56 complete animation frames per second and settled around
274-280 KiB/s and 326-335 packets/s in each direction for one fully replicated
skater. Both processes reported zero socket failures. Appearance transfer is
a separate one-time burst when a player first joins or changes outfit.
The exact v12 predictive checkpoint measured roughly 114 KiB/s per synthetic
typical stream including datagram headers. The lossless Snappy outer wrapper
reduces the same code-derived typical corpus to about 42.7 KiB/s and 1.05
datagrams per frame, with about 1.1 microseconds of additional compression
work per frame on the development machine. Gentle and stress corpora project
to about 41.5 and 33.2 KiB/s respectively. These are deterministic codec
benchmarks, not live network measurements or proof of visual correctness; the
combined five-client visual/telemetry gate remains required.
The first retail five-client Snappy run was much less compressible than the
synthetic corpus: it measured about 265-300 KiB/s per peer at 60 skeletal
samples per second. The internet baseline therefore activates the existing
animation-rate control at 20 Hz while retaining 60 Hz root snapshots. Its
acceptance target is at most 112 KiB/s of application traffic per peer, or
about 8 Mibit/s upload for a ten-player direct mesh. The analyzer calculates
both values from every user-run visual check. The five-client zero-floor run
confirmed 103.7-111.3 KiB/s per peer and 402-424 KiB/s total upload per client,
or approximately 3.1-3.3 Mibit/s before outer transport framing.
Visual-check builds also emit periodic `multiplayer-perf` windows for local
final-pose capture, appearance capture/cache work, the replication tick,
remote appearance installation, remote reconstruction, and their total
command-processor-thread cost. Average and maximum times are reported
separately so one-time appearance spikes do not hide inside steady-state cost.
When the replication worker is enabled, periodic `multiplayer-worker` lines
report worker tick average/maximum time, published and superseded local
captures, render consumptions, output sequence, and local-input age. The
renderer also emits `multiplayer-render-cache` lines for one-time cache
preparation, validated track-index hits/scans, and any unexpected runtime
weight or rig fallback. The visual-check analyzer includes the latest timing
and cache lines. Background recipe work emits
`multiplayer-appearance-prepare` state and CPU timing, while the remaining
render-thread upload emits `multiplayer-appearance-install` timing. The
three-client `f895626` baseline measured 221-257 ms one-time synchronous
installation maxima; the background-preparation check should reduce those
render-thread maxima without changing the installed outfit.

Steam sessions now use direct peer fan-out. With ten players all mutually
present, each player uploads one exact stream to nine peers and receives nine
streams. The 20 Hz internet target caps this at about 0.98 MiB/s
(approximately 8 Mibit/s) of application datagrams in each direction before
Steam framing and appearance bootstrap traffic. No lobby owner carries the
other players' streams. Aggregate direct-mesh traffic still grows
approximately with the square of player count.

The baseline follows established engine practice rather than treating render
rate as network rate: Valve documents typical 20-30 packet/s state updates and
a 100 ms interpolation period, Epic describes smoothing 30 Hz network updates
at much higher display rates, and Unity documents 30 Hz as its default network
tick while recommending interpolation when lowering update frequency. This
project deliberately uses a lower configured floor after its completed-pair
clock was validated in the five-client local run:

- https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking
- https://dev.epicgames.com/documentation/en-us/unreal-engine/understanding-networked-movement-in-the-character-movement-component-for-unreal-engine
- https://docs-multiplayer.unity3d.com/netcode/2.1.1/components/networktransform/

Higher population work must first pursue compression, packet batching, shared
serialization, parallel reconstruction, and full-fidelity relays or servers.
The 99-bot root-presence result validates socket/load behavior only, not 100
fully animated skaters.

## Known wardrobe validation limitation

If Skate 3's own clothing screen fails to display a local piece and the
native scene exposes no local skater items, multiplayer has no complete local
pose or appearance to publish. Other clients then correctly remain on the
teal proxy, and a receiver that also lacks its own live presentation root may
show incoming peers as proxies. Telemetry from the `f895626` wardrobe run
captured this as `multiplayer-local-capture: state=missing` with
`skater_items=0`; it was separate from the remote render-cache behavior.
Diagnosing why the retail clothing presentation itself lost the top remains a
separate correctness task.

## Current non-goals

- No remote collision, pushing, grinding, scoring, trick validation, respawn
  authority, voice, or production security.
- Bails are visually replicated because they are part of the final bone pose,
  but their gameplay result is not authoritative on the other client.
- Rigid cloth uses the receiver's locally available deformed geometry while
  skeletal pieces use the sender's exact pose. A dedicated cloth stream is a
  later quality feature.
- App 480 is suitable only for private development testing. Production
  distribution requires a project-owned Steam AppID.
- Passwords are currently a convenience lobby filter, not production-grade
  authentication. Do not reuse an important password.
- A production 100-player session still needs deployed relay capacity,
  admission and identity, abuse limits, and measured internet-scale animation
  tests. Authoritative gameplay simulation can follow without replacing the
  visual replication envelope. Direct P2P is the current small/medium-session
  transport; larger sessions should change topology rather than silently
  reduce player detail.

## Transport boundary

The replicated root pose and per-mesh animation tracks, packet versioning,
map-space convention, class scheduling, interpolation, staleness rules, and
renderer-facing pose types are shared by Steam and localhost. A common
transport-adapter contract represents localhost UDP, Steam Networking
Messages, explicit Steam Networking Sockets, and a future dedicated
connection without placing platform addresses in replication packets.

The current Steam Messages backend runs on Steam Networking Sockets
internally and now queries its official session real-time status for ping,
quality, data rates, send capacity, pending reliable/unreliable bytes, queue
time, and maximum jitter. An explicit connection-oriented Sockets adapter can
therefore be introduced behind the same contract when dedicated/listen
connections are deployed; changing packet schemas or interpolation is not
required.

The future visual relay router is also transport-neutral. It authenticates a
connection's role and process session from envelope metadata and forwards the
original immutable datagram to every other role (or one directed control
target). It never reads retail meshes, textures, skeletons, or pose values.
Synthetic tests exercise this fan-out at 2, 5, 20, 50, and 100 players and
reject unknown connections, role spoofing, stale sessions, invalid targets,
and role-reuse leakage.

Run the non-game protocol, impairment, lifecycle, worker, routing, and
full-pose scale suites repeatedly with:

```powershell
.\scripts\Run-MultiplayerSyntheticSoak.ps1 -Minutes 10
```

It never launches Skate 3. Each run writes a timestamped summary and complete
CTest output under `out\synthetic-soaks`.
