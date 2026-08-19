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
unrelated Spacewar test lobbies from appearing. Lobby ownership defines the
logical host; packets from other members travel upstream to that host and are
relevance-routed to the other players.

If the Steam runtime is absent or Steam is not running, the menu reports that
status and falls back to same-PC discovery. Local discovery records live under
the user's local application-data directory and are removed when a session
closes; they are not maps, saves, or project files.

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
project does not require Valve's headers or import library. A development
install still needs Valve's redistributable `steam_api64.dll` beside
`skate3.exe`, Steam running and signed in, and a development
`steam_appid.txt`. Neither runtime file is committed to this repository.

`steam_appid.txt` containing `480` is a development override only. It must not
ship in public releases. A public release must use this project's own Steam
AppID and follow Valve's setup and redistributable requirements.

## What works

- Roles 1-100 communicate through Steam Networking Messages when a Steam
  lobby is active. Role 1 is the lobby owner/logical host; roles 2-100 send
  one upstream stream to it.
- Steam identities authenticate transport senders. The host rejects a client
  packet whose claimed role does not match its Steam lobby member.
- The fallback backend uses non-blocking localhost UDP with the same packets,
  roles, interpolation, and relevance routing.
- The host learns peer endpoints, relays packets, removes stale peers, and
  keeps an independent interpolation/reassembly timeline for every player.
- Each client publishes its verified local board position, orientation, and
  board-state flags 60 times per second by default.
- Positions use custom-map coordinates rather than the hidden retail world's
  absolute coordinates.
- Received poses use their sender simulation timestamps rather than packet
  arrival timing. They are buffered 50 ms behind real time by default so a
  render stall or a batch of arriving packets cannot introduce a catch-up
  step.
- Packets from a different map, protocol version, client slot, process
  session, or unexpected Steam identity are rejected.
- A stale remote player disappears after 1.5 seconds.
- The sender captures Skate 3's final rendered bone palettes after its
  animation graph, IK, tricks, and bails have evaluated.
- Character pieces retain their own mesh-specific bone remaps. Complete
  animation frames are compacted to half-float affine components plus
  root-relative signed translations, fragmented into bounded UDP datagrams,
  reassembled, buffered, and interpolated at 20 frames per second by default.
- The receiver draws the actual Skate 3 skater, clothing, accessories, and
  board meshes. The cyan rigid proxy remains only as a startup/packet-loss
  fallback until the first complete skeletal frame arrives.
- Each client renders at most the nearest 12 remote players by default.
- The host routes full skeletal animation only to each receiver's nearest
  high-detail set inside the 80-metre relevance radius. Distant players use
  two inexpensive root-presence updates per second.
- The host skips skeletal decoding for players outside its own visual set,
  caches per-frame relevance decisions, uses larger UDP buffers, and drains
  bursty fragmented traffic without the old 256-packet frame ceiling.

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

The script uses the sibling `Skate3CustomEngineLayer-Player` install by
default. Pass another clean install when required:

```powershell
.\scripts\Launch-Local-Multiplayer.ps1 `
  -SourceInstallRoot 'D:\Games\Skate3CustomEngineLayer'
```

It stages two ignored portable roots under `out\local-multiplayer`, gives each
client independent writable user data, and junctions the legally supplied
`game` directory and user `maps` directory rather than duplicating them.
Direct boot is enabled for this developer test; add `-NoDirectBoot` to use the
normal frontend.

## Repeatable load check

With one real process running as role 1, synthetic root-pose peers can exercise
the actual host socket and relay path:

```powershell
.\scripts\Test-LocalMultiplayerLoad.ps1 `
  -Bots 99 `
  -MapName blender_feature_park
```

The first 99-peer run sustained 18,810 upstream pose packets over ten seconds.
Every simulated peer received host/relay traffic; aggregate outgoing traffic
received by the bots was 5.95 Mbit/s. The real host remained alive and
responsive. This validates 100-peer root presence and relevance routing, not
100 simultaneous full animation streams or internet conditions.

## Measured skeletal-stream cost

A clean three-real-client run on 2026-08-19 reported zero rejected packets
and zero socket failures. Each sender delivered about 18-19 complete
animation frames per second and transmitted 840 mesh-palette bone entries per
frame. A non-host client used roughly 389-416 KiB/s upstream and received
778-805 KiB/s for two nearby remote players. The logical host received about
803 KiB/s and transmitted about 1.58 MiB/s while publishing itself and
relaying both clients.

This is correct exact-pose replication, but it is not the final large-lobby
animation format. The current stream sends every mesh-specific final 3x4 bone
transform every animation frame. Components are already reduced from 32-bit
floats to nine half-floats plus three root-relative signed 16-bit translations
per palette bone, but shared skeleton transforms are repeated across clothing,
body, shoes, hair, and board palettes. There is no temporal delta encoding.

The next protocol step should send a reliable, infrequent rig/palette remap
descriptor and one canonical skeleton pose per frame. Per-mesh palettes can
then be reconstructed locally. Quaternion/translation quantization,
changed-bone masks, periodic keyframes, and distance-dependent pose rates
should follow. Until that work is measured, the 99-bot root-routing result
must not be extrapolated to 100 full animated skaters.

## Current non-goals

- No remote collision, pushing, grinding, scoring, trick validation, respawn
  authority, host migration, voice, or production security.
- Bails are visually replicated because they are part of the final bone pose,
  but their gameplay result is not authoritative on the other client.
- Appearance metadata is not transmitted yet. The receiver currently reuses
  its locally loaded character meshes and materials, so differently
  customized players need a later asset/customization identity stream.
- Rigid cloth uses the receiver's locally available deformed geometry while
  skeletal pieces use the sender's exact pose. A dedicated cloth stream is a
  later quality feature.
- App 480 is suitable only for private development testing. Production
  distribution requires a project-owned Steam AppID.
- Passwords are currently a convenience lobby filter, not production-grade
  authentication. Do not reuse an important password.
- One user's game process is currently the logical host. A production
  100-player session still needs transport-level reliability, admission and
  identity, abuse limits, host migration or a dedicated-host option, and a
  measured full-animation bandwidth test. The current work prevents rendering
  and relaying all 99 players at full detail; it does not claim that a home
  connection can carry an unrestricted 100-player animation fan-out.

## Transport boundary

The replicated root pose and per-mesh animation tracks, packet versioning,
map-space convention, interpolation, staleness rules, and renderer-facing
pose types are shared by Steam and localhost. Gameplay code and rendering do
not depend directly on either transport.
