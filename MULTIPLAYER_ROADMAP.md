# Multiplayer Roadmap

This document describes the staged path from the current protocol-v11
multiplayer implementation to smooth five-player sessions and, later, larger
adaptive sessions. `MULTIPLAYER.md` remains the description of current
behavior.

The roadmap deliberately preserves the working foundations:

- The local skater keeps the original game rendering and input path.
- Remote animation is captured from the final rendered palette after
  animation, IK, tricks, bails, and attachment processing.
- Remote clothing, hair, hats, shoes, boards, trucks, and wheels remain owned
  by the sender.
- The receiver resolves recipe identifiers against its own local catalogue.
- Appearance recipes are sent on join and when changed, not continuously.
- Canonical skeleton tracks, exact appearance tracks, pivot-aware
  interpolation, and detached-board handling must not regress.
- Automated telemetry is not proof of visual correctness. The final
  multiplayer visual pass is performed by the user.

## Product milestones

### Milestone A: visually faithful sessions of five players or fewer

Nearby players should receive the full final pose at up to 60 Hz. Outfits,
attachments, boards, detached boards, tricks, bails, lighting, and shadows
must reconstruct correctly. Remote players may use a small interpolation
delay, but multiplayer must not add local input latency or create client frame
stalls.

### Milestone B: adaptive sessions of up to 100 players

Only the closest or most important players need maximum fidelity. Other
players use progressively cheaper pose, update-rate, rendering, or
presence-only tiers. Quality adapts to distance, visibility, interaction,
bandwidth, loss, jitter, CPU, and GPU load.

### Milestone C: shared protocol for P2P and future servers

Localhost, Steam P2P, a future headless relay, and a future authoritative
server must share packet schemas, scheduling, relevance, and interpolation.
A dedicated server is not an immediate implementation milestone.

## Phase 0: freeze the protocol-v11 baseline

Purpose: establish tests and measurements before changing architecture.

Work:

- Add golden tests for protocol-v11 encoding, validation, fragmentation,
  reassembly, keyframes, and deltas.
- Update or replace the historical protocol-v2 local load generator.
- Capture representative final-pose fixtures for normal skating, flip tricks,
  grinds, bails, board throws and catches, and fast wheel rotation.
- Capture representative appearance manifests for multiple outfits.
- Measure capture, encode, receive, reassembly, interpolation, appearance
  preparation, remote reconstruction, and GPU-upload time.
- Record bytes and packets separately for roots, animation, control, and
  appearances.
- Add counters for missing baselines, incomplete assemblies, interpolation
  holds, and appearance state.

Completion:

- Current packets reconstruct deterministically in automated tests.
- Repeatable two-player and five-player performance baselines exist.
- The cost of each major multiplayer stage is visible.
- No intentional packet or visual behavior has changed.

## Phase 1: lifecycle and appearance correctness

Purpose: fix bounded-state and join/rejoin failures without redesigning the
continuous animation stream.

Work:

- Key outbound state by authenticated peer identity plus connection
  generation, not role number alone.
- Reset keyframes and appearance progress when a different peer reuses a role.
- Add appearance states for cached, requested, received, installed, and failed.
- Add appearance acknowledgements and explicit resend requests.
- Add assembly deadlines and per-peer/global memory limits.
- Keep the proxy until the complete expected appearance is ready.
- Install new appearances transactionally.
- Release remote CPU and GPU resources when the peer leaves.
- Handle packet-sequence wrap with modular comparisons.
- Negotiate stronger map, build, and content compatibility.

Completion:

- Repeated leave, rejoin, and role reuse always produces the correct outfit.
- Partial or malicious transfers cannot retain unbounded memory.
- A failed wardrobe transition cannot mix old and new pieces.
- Existing clothing, board, and detached-board behavior passes the visual
  regression pass.

## Phase 2: move replication work off the render thread

Purpose: stop networking, codecs, and asset work from delaying GPU command
submission or local input presentation.

Introduce a background replication worker responsible for:

- Steam and localhost transport processing.
- Packet validation and receive draining.
- Fragment reassembly and decoding.
- Per-peer clocks and jitter buffers.
- Animation encoding and send scheduling.
- Relevance selection and network telemetry.

The render thread should only:

- Publish the newest immutable local capture.
- Consume already prepared remote presentation snapshots.
- Execute bounded GPU resource uploads.
- Draw remote players.

The first worker version must continue producing byte-compatible
protocol-v11 traffic. Protocol redesign is a later phase.

Completion:

- Networking progresses independently of render cadence.
- Protocol-v11 golden traces remain unchanged.
- Normal multiplayer work causes no unbounded render-thread stalls.
- Two-player and five-player visual results match the baseline.

## Phase 3: cache and harden appearance rendering

Purpose: remove repeated per-frame mesh work while improving visual
correctness.

Work:

- Cache topology signatures, weighted palette rows, canonical remaps, and
  exact-track lookup indices when an appearance is installed.
- Stop rescanning every vertex influence for every remote piece every frame.
- Avoid repeated deep copies of both complete canonical rig layouts.
- Resolve and decode recipe assets on an asset worker.
- Submit a bounded, transactional GPU installation command.
- Share immutable meshes and textures by content identity.
- Prune per-peer bindings and unreferenced resources.
- Move dynamic character-lighting data out of appearance identity.
- Include appropriate high-detail remotes in shadow rendering.
- Resolve row-vector versus column-vector canonical layout selection.
- Validate runtime-composed shoe, skin, board, and wheel textures.

Completion:

- Steady remote rendering no longer performs invariant topology or vertex
  discovery.
- Appearance loading does not read files or decode textures on the GPU-command
  thread.
- Repeated joins and wardrobe changes do not leak remote resources.
- Lighting, shadows, and complete-piece behavior pass the visual regression
  pass.

## Phase 4: protocol-v12 realtime recovery

Purpose: prefer the newest pose and recover quickly from packet loss.

Work:

- Add explicit little-endian serialization and capability negotiation.
- Keep protocol v11 available during migration.
- Add peer/session generation and map/build/content hashes.
- Acknowledge the newest completely decoded pose baseline plus a recent
  receive bitmap.
- Build deltas only against receiver-confirmed baselines.
- Add a reliable request for an immediate new baseline.
- Send continuous pose data unreliably with an expiry/latest-wins policy.
- Split poses into independently useful groups, such as core body, legs,
  arms, board/trucks, wheels, and appearance-specific attachments.
- Keep critical control reliable and appearance bulk reliable but
  rate-limited.

Completion:

- Losing one realtime group does not discard the entire pose.
- Losing a baseline does not cause a long delta-decode blackout.
- Obsolete animation is discarded instead of delivered late.
- Appearance traffic cannot head-of-line block realtime animation.

## Phase 5: reduce bandwidth behind fidelity gates

Purpose: make exact nearby animation affordable without changing its visible
result.

Order:

1. Bit-pack current fields without reducing precision.
2. Canonicalize quaternion signs before delta comparison.
3. Test smallest-three quaternion encoding.
4. Use per-chain translation ranges and omit reconstructable translations.
5. Preserve affine transforms for scale, shear, pivots, board, and attachment
   exceptions.
6. Reduce precision only for lower relevance tiers.

Every change is evaluated using final skinned-vertex displacement,
foot/board contact, board/truck/wheel pivot error, rapid-spin error, temporal
stability, and a user visual pass.

Initial target: reduce one exact nearby stream from the current measured
approximately 274-280 KiB/s toward 90 KB/s average application payload.

## Phase 6: adaptive quality and relevance

Purpose: scale while retaining full fidelity for the most important players.

Initial tiers:

- Exact: full final pose, normally 60 Hz.
- Near: reduced pose, normally 30 Hz.
- Mid: important bones and board state, normally 10 Hz.
- Presence: root and direction, normally 1-2 Hz.
- Dormant: no periodic update until relevance changes.

The scheduler considers:

- Distance with enter/exit hysteresis.
- Camera visibility and projected size.
- Shared rail, collision, trick, bail, or other interaction.
- Spectating, party, or challenge importance.
- Recent state change and time since last update.
- Estimated packet cost.
- Available bandwidth, loss, jitter, and queue delay.
- Client CPU and GPU budgets.

Use per-connection and per-source byte budgets with age-based fairness. Five
healthy nearby players should remain at Exact quality.

## Phase 7: Steam Networking Sockets transport

Purpose: gain explicit connections, batching, connection statistics, queue
visibility, and future server entry points.

Keep one replication core behind transport adapters for:

- Localhost UDP.
- Current Steam Networking Messages.
- Steam Networking Sockets P2P/listen sessions.
- A future dedicated relay/server connection.

Separate realtime, critical control, and bulk appearance traffic into
independent ordering and scheduling classes.

## Phase 8: larger sessions and future dedicated relay

For 2-5 players, retain direct Steam P2P for high-rate visual pose data.

For 6-20 players, retain P2P support but bound each sender's high-detail
recipient count and use adaptive lower tiers for the rest. Do not make the
host relay every complete animation stream.

For 20-100 players, prefer a dedicated relevance-aware relay:

- Each client uploads one primary source stream.
- The relay reads root, relevance, and pose-group metadata.
- It forwards an appropriate subset and quality to every receiver.
- It does not require retail meshes or textures.
- Appearance recipes remain sender-owned and receiver-resolved.

The first dedicated component may be a visual relay. Future authoritative
root, events, scoring, or simulation can use the same protocol envelope
without replacing appearance and final-pose replication.

## Five-player acceptance contract

Normal validation envelope:

- Sender and receiver maintain 60 FPS.
- RTT is at most 80 ms.
- p95 jitter is at most 10 ms.
- Random loss is at most 1%.
- Reordering is at most 0.1%.

Required outcomes:

- No change to local input or original local-skater rendering.
- No unbounded multiplayer work on the GPU-command thread.
- All required appearance pieces and identities match the sender.
- No partial real appearance suppresses the complete proxy.
- Root, pose, board, and attachments use the same sender timeline.
- Baseline recovery completes within 100 ms under the normal envelope.
- At least 99.9% of steady remote rendered frames interpolate usable samples.
- Weighted-vertex, contact, and pivot error stay within the recorded fidelity
  thresholds.
- The final user visual pass covers outfits, hair, hats, shoes, boards,
  detached boards, tricks, bails, lighting, and shadows.

## Validation matrix

Automated network tests should cover:

- RTT: 20, 80, 150, and 200 ms.
- Random loss: 0%, 1%, 3%, and 5%.
- Burst loss: 100, 250, and 500 ms.
- Jitter: 0, 5, 20, and 50 ms.
- Reordering and duplication.
- Uplink limits: 1, 5, and 10 Mbit/s.
- Appearance transfers during active animation.
- Join storms, wardrobe churn, role reuse, map mismatch, and sequence wrap.
- Render stall, minimize, and resume.

Soak tests should include two and five real clients, mixed-tier 6- and
20-client sessions, and realistic changing-pose headless tests at 50 and 100
clients. Root-only synthetic bots are presence/load tests, not evidence of
full-animation or visual capacity.

## Change discipline

- Keep documentation, correctness fixes, threading changes, protocol changes,
  compression, and adaptive scheduling in separate commits.
- Every implementation commit states the visual invariants it preserves.
- Do not combine the network worker, protocol v12, and compression into one
  refactor.
- Do not replace exact nearby final-pose replication with semantic animation.
- Do not make continuous pose reliable.
- Do not transmit recipes or retail asset data continuously.
- Do not treat successful packet logs as proof of visual correctness.
