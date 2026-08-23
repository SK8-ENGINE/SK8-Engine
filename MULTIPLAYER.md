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
project does not require Valve's headers or import library. On a clean Windows
installation, the game performs this development setup automatically:

- download the pinned Steamworks.NET standalone archive directly from its
  [published 2025.164.1 GitHub release](https://github.com/rlabrecque/Steamworks.NET/releases/tag/2025.164.1);
- verify the archive SHA-256 before extracting it;
- verify the extracted `steam_api64.dll` SHA-256 before loading it;
- cache the verified runtime under `.cel-steam`;
- generate the local `steam_appid.txt` marker containing `480`;
- start Steam when necessary and connect to the signed-in Steam account.

The runtime DLL and App 480 marker are not committed or embedded in the public
archive. If automatic setup fails, read `.cel-steam/bootstrap.log`; multiplayer
falls back to the same-PC development transport instead of preventing the game
from starting.

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
  roles, interpolation, and relevance routing.
- Localhost development still uses role 1 as a small relay because all test
  processes share one machine. Internet Steam sessions use direct peer fan-out.
- Every receiver keeps an independent interpolation/reassembly timeline for
  every player and removes stale peers.
- Each client publishes its verified local board position, orientation, and
  board-state flags at the selected preset rate (60 times per second for
  Balanced).
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
- The sender transmits one canonical skeleton plus compact exact-palette
  tracks for small or post-processed attachments such as the hat and wheels.
  Affine components use signed 16-bit fixed point with root-relative
  translations, are fragmented into bounded datagrams, reassembled, buffered,
  and interpolated at 60 frames per second on Balanced. The higher sampling
  rate preserves fast rotations such as a freely rolling skateboard wheel;
  20 Hz can cross the 180-degree ambiguity between orientation samples.
- The sender publishes a versioned engine-owned appearance recipe. The
  receiver resolves exact vanilla CAC bind meshes from its local asset
  catalogue and installs the sender's body, clothing, hair, accessories,
  materials, and board without using Skate 3's fragile retail online/NPC
  presentation entities. The normal `RCP1` path carries the compact CAC
  recipe plus model, track, topology, and remap bindings once on join and
  whenever the appearance changes. The older assembled mesh/texture format
  remains a bounded compatibility fallback. Incomplete appearance transfers
  are limited to 16 MiB per peer and 64 MiB in total and expire after ten
  seconds without a valid chunk.
- Each client renders at most the nearest 12 remote players by default.
- Each sender routes full skeletal animation only to nearby peers inside the
  selected relevance radius. Distant peers receive inexpensive root-presence
  updates.
- Receivers skip skeletal decoding outside their visual set, cache relevance
  decisions, use larger UDP buffers, and drain bursty fragmented traffic
  without the old 256-packet frame ceiling.

## Network quality presets

Open **Escape → Multiplayer → Network Quality**:

| Preset | Root pose | Animation | Interpolation | Detail radius | Nearby players |
| --- | ---: | ---: | ---: | ---: | ---: |
| Bandwidth Saver | 30 Hz | 10 Hz | 100 ms | 50 m | 6 |
| Balanced | 60 Hz | 60 Hz | 50 ms | 80 m | 12 |
| High Fidelity | 90 Hz | 60 Hz | 35 ms | 120 m | 16 |

**Auto** chooses High Fidelity for up to 4 participants, Balanced for 5-12,
and Bandwidth Saver above 12. **Custom** exposes the individual rates,
interpolation delay, relevance radius, attachment radius, nearby-player
budget, and distant-presence rate. Balanced preserves the configuration used
for the current two-client validation.

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

A two-real-client protocol-v11 Balanced-equivalent 60 Hz run on 2026-08-22
delivered about 54-56 complete animation frames per second and settled around
274-280 KiB/s and 326-335 packets/s in each direction for one nearby detailed
skater. Both processes reported zero socket failures. Appearance transfer is
a separate one-time burst when a player first joins or changes outfit.

Steam sessions now use direct peer fan-out. With ten players all mutually
nearby, each player uploads one detailed stream to nine peers and receives
nine streams: using the measured Balanced localhost payload as a planning
estimate, about 2.5 MiB/s (roughly 20 Mbit/s) in each direction before Steam
transport overhead and appearance bootstrap traffic. No lobby owner carries
the other players' streams. This is a far healthier ten-player topology, but
it still grows approximately with the square of the nearby player count
across the whole session and is not a 100-player full-detail solution.

Higher population work still needs stronger skeleton LOD, lower-rate distant
players, interest-area partitioning, and eventually dedicated authoritative
servers or relays. The 99-bot root-presence result validates inexpensive
presence packets only, not 100 fully animated skaters.

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
- A production 100-player session still needs authoritative server
  simulation, transport-level reliability, admission and identity, abuse
  limits, interest management, and measured internet-scale animation tests.
  Direct P2P is the current small-session transport; it does not claim that
  every peer can carry an unrestricted 100-player animation fan-out.

## Transport boundary

The replicated root pose and per-mesh animation tracks, packet versioning,
map-space convention, interpolation, staleness rules, and renderer-facing
pose types are shared by Steam and localhost. Gameplay code and rendering do
not depend directly on either transport.
