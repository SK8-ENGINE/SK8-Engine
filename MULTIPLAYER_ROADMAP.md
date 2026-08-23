# Multiplayer Roadmap

This document describes the staged path from the current protocol-v11
multiplayer implementation to smooth five-player sessions and, later, larger
full-fidelity sessions. `MULTIPLAYER.md` remains the description of current
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

Every player should receive the full final pose at 60 Hz. Outfits,
attachments, boards, detached boards, tricks, bails, lighting, and shadows
must reconstruct correctly. Remote players may use a small interpolation
delay, but multiplayer must not add local input latency or create client frame
stalls.

### Milestone B: full-fidelity sessions of up to 100 players

Every player receives the same complete 60 Hz final-pose stream, including
appearance-specific attachments and board state, regardless of distance,
visibility, importance, or player count. Compression, batching, shared
serialization, transport improvements, and server topology should be used
before reducing another player's fidelity. Adaptive quality is deferred
unless measured real-world limits make it necessary and a later roadmap
change explicitly authorizes it.

### Milestone C: shared protocol for P2P and future servers

Localhost, Steam P2P, a future headless relay, and a future authoritative
server must share packet schemas, scheduling, recipient routing, and
interpolation. A dedicated server is not an immediate implementation
milestone.

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
- Full-fidelity recipient fan-out and network telemetry.

The render thread should only:

- Publish the newest immutable local capture.
- Consume already prepared remote presentation snapshots.
- Execute bounded GPU resource uploads.
- Draw remote players.

The first worker version must continue producing byte-compatible
protocol-v11 traffic. Protocol redesign is a later phase.

Current checkpoint:

- The existing protocol-v11 runtime can run on one background worker behind a
  disabled-by-default cvar; the dedicated visual-check launcher enables it.
- Render-to-worker input is a latest-wins immutable capture, while prepared
  remote-player vectors are immutable and session-matched retirement events
  remain queued until the renderer consumes them.
- Protocol golden tests, lifecycle tests, and concurrent mailbox stress tests
  pass in the dedicated worktree.
- Renderer-owned appearance installation and GPU drawing remain on the render
  thread. Their existing outfit and board behavior has not been redesigned.
- A three-client user visual pass at protocol-v11 commit `7c00e28` retained
  outfits, boards, and full animation through resets and extreme board/skater
  separation. Telemetry independently showed no post-startup local-capture
  loss, socket failure, appearance release, or worker stall. A five-client
  completion pass remains outstanding.

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

Current checkpoint:

- Per-appearance renderer caches now retain weighted palette rows, canonical
  remaps, board-piece classification, and validated exact/canonical track
  ordinals.
- Steady rendering no longer scans installed mesh vertices or deep-copies a
  complete canonical remap merely to rediscover invariant bindings.
- The renderer no longer constructs a temporary remote-item pointer vector
  for every remote player on every frame.
- Compatibility appearances retain a safe rig-lookup retry and track-order
  changes fall back to a validated linear scan.
- Automated cache tests cover packed weight discovery, malformed tracks,
  stable-index hits, and changed track layouts. Runtime telemetry reports any
  unexpected per-frame weighted-row fallback or rig retry.
- A three-client visual pass at `f895626` found healthy outfits, boards, and
  animation correct. Cache telemetry reported no weighted-row fallback, rig
  retry, socket failure, or multiplayer error.
- That pass measured one-time recipe installation spikes of 221-257 ms on the
  render thread. A guarded background asset worker now prepares recipe models
  and textures with latest-outfit/session rejection before the renderer sees
  them. The wire protocol is unchanged.
- Three-client visual passes through `3486fc4` retained complete outfits,
  boards, detached-board animation, attached ROPA hair, authored hair strand
  transparency, and stable ROPA shirts. Telemetry independently showed no
  socket or appearance failure, incomplete recipe, weighted-row fallback, rig
  retry, or dropped-garment corruption. Receiver hair fade was normalized
  from transient join values such as 0.188-0.275 to 1.0, and recipe ROPA
  pieces used canonical body tracks.
- Background preparation reduced the remaining one-time render-thread
  installation work to 21.6-51.4 ms for 10-11 pieces and 23-25 textures.
  Steady multiplayer render work averaged about 0.20-0.25 ms in that pass.
- A guarded transactional installer at `ce95e96` now stages textures, meshes,
  and per-piece render caches in an alternate renderer namespace before a
  final atomic state swap. The synchronous path remains available as a
  rollback.
- A three-client user visual pass reported the staged transition working well.
  Telemetry independently recorded complete 10-11-piece appearances after 37
  operations, no failed preparation, incomplete recipe, socket failure,
  weighted-row fallback, or rig retry, and commit operations of 0.03-0.09 ms.
- Total installation work of 36-43 ms was spread over roughly 160-298 ms
  instead of one render frame. Every observed resource operation was at or
  below 2.98 ms except one 6.99 ms texture upload. This is a soft-budget
  outlier to retain in performance regression checks, not evidence of visual
  failure.
- Commit `d7b7021` added explicit installed/pending mesh and texture ownership
  audits, balanced-release telemetry, and an offline 128-change staging churn
  test. The five-client runs reported zero missing, duplicate, orphaned, or
  unbalanced renderer resources.
- The first five-client pass exposed a localhost late-join gap: client 4 knew
  about all four peers but received only role 1's appearance. Commit `2e06e16`
  restarts each non-host sender's role-1 fanout stream once when it discovers a
  new downstream peer.
- In the follow-up five-client run, the user reported normal play looking good.
  Telemetry independently showed all five clients installing all four remote
  appearances, clients 2-5 exercising the new fanout restart, and no socket,
  appearance, recipe, rig-cache, or resource-lifetime fault. The explicit
  close/rejoin stress step was not performed and remains in the regression
  matrix rather than blocking the next protocol phase.
- Five simultaneous local GPU clients increased individual texture operations
  to occasional 10-24 ms outliers even though the final atomic commits stayed
  near zero and steady multiplayer frame work remained low. The current budget
  is soft per resource; true sub-resource upload slicing remains a performance
  follow-up if real multi-machine testing reproduces visible stalls.

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

Current checkpoint:

- Commit `6a5285c` adds an offline-only protocol-v12 foundation alongside the
  live protocol-v11 structs. It does not participate in runtime send or
  receive dispatch.
- The v12 wire layer uses a fixed 40-byte transport-independent envelope,
  explicit little-endian encoding, a 1,200-byte datagram ceiling, sender
  session and stream identities, sequence acknowledgement plus a 32-packet
  receive bitmap, and expirable/keyframe/reliable flags.
- A one-time capabilities payload carries feature bits and 64-bit map, build,
  and content hashes instead of repeating those hashes on every realtime
  packet. Unknown feature bits remain forward-compatible and negotiation
  intersects only locally understood features.
- Golden-byte, round-trip, malformed-packet, payload-boundary, feature
  negotiation, acknowledgement-window, and sequence-rollover tests pass.
  Protocol-v11 golden tests remain unchanged.
- Commit `0577e10` adds the offline-only receive-history and baseline-recovery
  state machine. It tracks a newest packet plus a 32-packet reorder window,
  handles sequence and baseline-ID rollover, rejects duplicates and obsolete
  packets, and resets all decode state when an authenticated transport
  generation changes.
- Capability activation now requires a caller-owned monotonic transport
  generation as well as matching map, build, and content hashes. Arbitrary
  realtime packets cannot activate or roll back a peer session.
- Grouped baselines are exposed as receiver-confirmed only after every
  required group has decoded. Packet acknowledgement alone never authorizes
  delta generation. Missing or incomplete baselines latch one bounded
  recovery request, with explicit timeout-driven retries rather than a
  request for every rejected delta.
- Offline tests cover packet loss, reordering, duplicates, 32-packet history
  boundaries, sequence and baseline rollover, incomplete and superseded
  grouped baselines, stale sessions, incompatible content, bounded recovery,
  and sender use of decoded-baseline reports. All six multiplayer suites pass,
  including unchanged protocol-v11 golden tests.
- The live runtime remains byte-for-byte protocol v11, so this checkpoint
  does not require an in-game visual pass.
- Commit `9d9d15a` defines the offline-only v12 pose-control and grouped-pose
  wire formats. A 20-byte reliable control payload carries decoded-baseline
  reports and bounded baseline requests targeted to an exact role, stream,
  and session. A fixed 24-byte pose-group header leaves 1,136 bytes for data
  inside the 1,200-byte datagram ceiling.
- Each pose group has an independent ID, encoding, element count, pose ID,
  receiver-confirmed baseline reference, and bounded fragment range. One
  group is capped at 64 KiB and 58 datagrams, preventing untrusted size and
  fragment-count growth. The current v11 word stream has an explicit migration
  encoding; later bit packing does not require a new envelope or fragmenter.
- Baseline and delta chunks are explicitly unreliable and expirable.
  Baselines carry the keyframe flag; recovery controls are reliable and never
  expirable. Delta headers must reference an older receiver-confirmed
  baseline under rollover-safe sequence ordering.
- Packet acknowledgement is recorded for each valid fragment before
  reassembly, while decoded-baseline acknowledgement remains withheld until
  every required group reconstructs. This keeps loss telemetry accurate
  without falsely authorizing deltas from partial data.
- Golden-byte, round-trip, exact-datagram-budget, maximum-fragment,
  malformed-header, truncation, trailing-data, flag-policy, and generation
  separation tests pass. All seven multiplayer suites pass, including the
  unchanged protocol-v11 golden tests.
- The live runtime remains byte-for-byte protocol v11, so this second
  checkpoint also does not require an in-game visual pass.
- Commit `011cfec` adds the offline v12 pose packetizer and bounded fragment
  reassembler. The packetizer emits transport-neutral descriptors, then
  copies each source range directly into one caller-owned datagram; it does
  not allocate or duplicate the complete pose once per fragment.
- Reassembly is latest-wins independently per sender, session, stream,
  message kind, and group. A newer pose supersedes an incomplete older pose,
  obsolete fragments are rejected, and duplicate fragments must have
  identical bytes, timestamp, metadata, and rollover-safe base sequence.
  Conflicts fail closed instead of combining two frames.
- Default reassembly limits are eight active groups, 256 KiB buffered data,
  64 KiB per group, 58 fragments per group, and a 250 ms incomplete-fragment
  lifetime. Completion, expiry, conflict, supersession, generation reset, and
  resource-pressure eviction release their owned buffers deterministically.
- Expired or evicted baseline groups report a recovery mask into the bounded
  request latch. Ordinary expired deltas are simply discarded and do not
  request a baseline. Packet receipt still enters the receive bitmap before
  reassembly, while only complete decoded group sets confirm a baseline.
- The synthetic end-to-end path now exercises encode, decode, packet history,
  reordered reassembly, grouped baseline completion, and delta acceptance.
  It also covers exact fragment boundaries from 1 byte through 64 KiB,
  58-fragment groups, sequence rollover, loss, duplication, conflicting
  bytes and timestamps, latest-pose supersession, expiry, slot and byte
  limits, and recovery retry behavior.
- All eight multiplayer suites pass, including unchanged protocol-v11 golden
  tests. The live runtime remains byte-for-byte protocol v11, so this
  checkpoint does not require an in-game visual pass.
- Commit `153ef23` adds a transport-neutral outbound scheduler with isolated
  queue and byte limits for reliable control, expirable realtime data, and
  reliable appearance bulk. The scheduler owns datagram bytes independently
  of localhost, Steam, or a future server transport.
- Control drains before realtime, and realtime drains before appearance.
  Appearance has an independent per-drain budget and cannot consume control
  or realtime queue capacity. A high-priority message that does not fit the
  current transport budget is never bypassed by lower-priority traffic.
- Realtime queues are latest-wins per target and stream, reject stale or
  conflicting duplicates with rollover-safe sequence comparisons, expire
  before transmission, and evict their oldest realtime data under their own
  class pressure. Reliable control and appearance remain FIFO and are never
  silently converted into expirable traffic.
- Synthetic overload tests cover appearance saturation, independent priority
  admission, strict drain order, realtime replacement and sequence rollover,
  expiry, class limits, queue accounting, and rejected replacements retaining
  the last valid realtime message. All nine multiplayer suites pass.
- The next live slice keeps every protocol-v11 packet byte unchanged while
  assigning explicit transport reliability: control and appearance remain
  reliable, while root and skeletal animation become latest-wins/unreliable
  on Steam. Telemetry and a user-run visual gate must confirm continuous pose,
  outfit, and board behavior before any v12 capability advertisement is
  enabled.
- Commit `0bb6843` applies that first live scheduling policy without changing
  protocol-v11 packet bytes, capture, quantization, fragmentation, host
  routing, receive assembly, interpolation, appearance recipes, or rendering.
  Root and skeletal animation now use Steam's unreliable auto-restart send
  mode; control and appearance continue to use reliable auto-restart.
  Localhost remains UDP, now carrying the same explicit traffic classes.
- Every successful send records reliable/unreliable packet and byte totals.
  Animation-unreliable, appearance-reliable, and control-reliable counters
  prove each packet class used its intended policy, while a policy-error
  counter catches any call-site mismatch. Five-second rate logs report both
  class bandwidth and reliable/unreliable bandwidth.
- The telemetry analyzer reports the policy marker, cumulative class-policy
  counters, and policy errors per client. The dedicated
  `RUN_MULTIPLAYER_REALTIME_PRIORITY_CHECK.bat` incrementally builds the exact
  worktree revision, runs all nine offline suites, stages five isolated
  persistent-profile clients, enables existing performance diagnostics, and
  writes a timestamped run with a policy-specific three-minute visual script.
- Full Release compilation and all nine offline multiplayer suites pass. The
  live checkpoint was exercised by the user in run
  `20260823-153038-086c02cc`. The user reported correct multiplayer
  functionality and appearance behavior, but also clearly visible,
  longstanding remote-motion jitter. Some sender roles appeared substantially
  smoother than others to every viewer, so the five-player fidelity gate is
  not complete.
- Telemetry confirms all five clients reached four known and four visible
  peers, installed four complete remote appearances, used the intended
  unreliable-animation/reliable-control-and-appearance policy with zero
  policy errors, and reported no socket or resource failures. It does not
  prove the user's visual result.
- Across the same run, senders averaged only 48.2-49.9 complete animation
  frames per second despite the configured 60 Hz target, with five-second
  windows dipping as low as 27.8-38.0 fps. Receiver summaries averaged
  20.2-20.9 ms sample periods and 4.5-5.7 ms timing variation, while
  32.8-52.6% of presentation ticks were pinned to the newest available
  animation sample instead of interpolating between two samples. The current
  aggregate telemetry cannot identify which sender roles account for those
  underruns because the per-peer timing fields are overwritten during
  iteration.
- Before changing interpolation behavior, add per-sender/per-receiver timing
  telemetry for source capture cadence, completed-frame arrival cadence,
  interpolation cursor margin, held-latest runs, sample loss/supersession, and
  worker publication age. Use it to correlate the consistently rough sender
  roles reported by the user. The subsequent playback-delay/cadence
  experiment did not change pose bytes, appearance, board handling, local
  rendering, or recipient fidelity.
- Commit `5f7cd9e` adds a deterministic five-second timing record for every
  receiver/sender pair. It reports completed-frame rate, sender period,
  arrival variation, chosen playback delay, average/minimum/maximum cursor
  margin, retained sample count, interpolated/held-latest/held-oldest
  presentation counts, maximum consecutive held-latest run, completed-frame
  sequence gaps, and superseded incomplete assemblies. The analyzer retains
  the latest record for each sender instead of collapsing all peers into one
  aggregate.
- Commit `cd4d2bc` replaces elapsed-since-last-send checks with phase-retaining
  periodic deadlines for root and animation. At a 4 ms worker interval, the
  old check could only send every fifth tick and therefore turned a requested
  60 Hz stream into approximately 50 Hz. The new gate alternates worker ticks
  around the 16.67 ms deadline to retain the requested average rate, sends
  only the latest fresh capture, and advances past missed deadlines without a
  catch-up burst after a stall.
- Deadline tests cover 60 Hz pacing on a 4 ms worker and a 500 ms stall with
  no burst. All nine offline multiplayer suites and a full Release build pass.
  `RUN_MULTIPLAYER_SMOOTHNESS_CHECK.bat` stages five persistent-profile
  clients and records the per-pair diagnostics. Its first visual result is
  recorded below.
- The user ran `20260823-154251-e50cf311` and reported that all remote skaters
  still jittered. The sender deadline change did raise complete animation
  delivery from the previous approximately 48-50 fps to approximately
  55-60 fps with zero completed-frame gaps or superseded assemblies, but it
  did not solve presentation smoothness.
- Per-pair telemetry identified buffer underrun as the remaining direct
  failure: many paths held the newest complete pose for 45-63% of
  presentation ticks, average cursor margin sat near or below zero, and
  minimum margin reached -82 ms. The previous 50-66 ms adaptive delay was
  shorter than the measured complete-frame delivery stalls.
- The next correction retains 16 rather than eight complete animation
  samples and chooses a per-peer delay of at least five measured frame periods
  plus eight measured timing-variation units, capped at 250 ms and never below
  the configured floor. This produces approximately 108-164 ms for the
  measured five-client paths and preserves one root/skeleton sender timeline.
  The smoothness launcher now asks only for two minutes of ordinary play; it
  requires no per-role choreography or client identification.
- Commit `5a577e1` bounded the presentation cursor to an interpolatable
  skeletal interval after receiver stalls, moved configured localhost tests
  from host relay to direct peer fan-out, shared identical frame
  serialization across recipients, enlarged UDP queues, and explicitly paced
  the five local test clients at 120 FPS.
- In run `20260823-194302-5a577e17`, the user reported that some peer views
  became very smooth while others remained laggy. Telemetry independently
  showed zero held-latest playback freezes, relay traffic, and socket
  failures, but the legacy relevance policy suppressed complete animation
  streams for selected sender/receiver pairs.
- Commit `909df1e` first made five-player sessions exempt from distance and
  attachment filtering. The product policy was then simplified further:
  every supported 1-100-player session is full fidelity. Active distance,
  visibility, nearby-player, far-presence, attachment, and population-based
  downgrade paths are removed. Adaptive fidelity is deferred unless future
  measured limits justify a new explicit decision.
- In run `20260823-195635-7da2d97a`, the user reported correct full-fidelity
  behavior with only a very small residual micro-stutter, which is deferred
  for later polish. Telemetry independently showed all 20 sender/receiver
  paths completing at 60 Hz, zero held-latest playback, sequence gaps,
  superseded assemblies, relevance drops, relayed realtime packets, socket
  failures, delivery-policy errors, renderer-resource faults, or unsafe GPU
  upload reuse. This telemetry does not establish the user's visual result.
- Commit `0ee80c9` enables the first live protocol-v12 migration gate without
  changing gameplay traffic. A peer must first advertise v12 support through
  the authenticated v11 control path, then pass the explicit-endian v12
  capability envelope and map/build/content compatibility checks for its
  current role, process session, and monotonic transport generation. Stale or
  incompatible generations cannot activate. Root, pose, appearance, board,
  and rendering traffic remain protocol v11.
- Commit `8c94fe6` moves the live root and board-state snapshot to an
  80-byte explicit-little-endian v12 datagram after both peers acknowledge
  the active capability generation. Peers that have not completed that gate,
  including dynamic localhost host-relay paths, retain the v11 fallback.
  Decoded v12 snapshots enter the existing pose buffer, interpolation, board,
  and renderer path; skeletal animation and appearance remain v11.
- In run `20260823-201047-8c94fe69`, the user reported that the five-client
  root/board migration looked good. Telemetry independently showed every
  client negotiating v12 with all four peers and carrying approximately
  21,000 root snapshots in each direction. All 20 sender/receiver paths
  completed at 59.4-60.2 Hz with zero held-latest playback, sequence gaps, or
  superseded assemblies. There were no capability incompatibilities, socket
  failures, delivery-policy errors, appearance-resource faults, renderer
  release faults, or unsafe GPU upload reuse. Client 4 rejected one early
  root datagram, then accepted more than 21,000 without another rejection;
  the isolated rejection did not produce a sustained stream fault. This
  telemetry does not establish the user's visual result.
- The next offline checkpoint defines the exact v12 migration bridge for the
  already validated v11 animation words. It stores the animation root,
  root-bone identity, word count, and every 16-bit word explicitly in little
  endian, then carries that byte stream through the existing bounded pose
  packetizer and reassembler. Golden bytes, malformed input, maximum 8,192
  word frames, reverse-order 15-fragment reassembly, and bit-exact recovery
  are covered without changing live send, receive, or rendering behavior.
- The live grouped-animation checkpoint advertises the v12 pose-group
  feature under a new build-compatibility identity. After two-way
  negotiation, each complete 60 Hz animation is encoded once as the exact
  current word stream plus its root metadata, fragmented into at most 1,200
  byte v12 datagrams, and reassembled with bounded latest-pose state. It then
  enters the same word decoder, interpolation buffer, attachments, and
  renderer used by v11. Peers without the negotiated feature and dynamic
  localhost host-relay sessions retain the v11 path. The first frame after a
  peer switches from v11 is forced to be a self-contained keyframe, so the
  new stream never depends on a pre-negotiation v11 baseline having arrived.
- This first live pose-group step deliberately keeps one complete group and
  the existing v11 keyframe/delta choice. It does not yet claim independent
  body-part recovery or receiver-confirmed baselines. Dedicated counters
  distinguish v12 animation fragments, completed groups, and rejections; a
  five-client user visual and telemetry gate is required before baseline
  acknowledgement and recovery controls are enabled.
- In run `20260823-202317-59443ab0`, the user reported that the five-client
  v12 skeletal stream looked good. All clients completed approximately
  28,000-29,000 v12 animation groups with zero v12 animation-fragment
  rejections, and all 20 sender/receiver paths recovered to approximately
  60 Hz with zero held-latest playback, completed-frame gaps, or superseded
  assemblies in the final telemetry windows. The clients ended at
  116.6-120.0 render handoffs per second with four known and four visible
  peers and no socket, delivery-policy, appearance-resource, or GPU-resource
  faults. This telemetry does not establish the user's visual result.
- The same run contained a temporary all-client render/capture slowdown:
  worst render-handoff windows fell to 11.7-18.2 per second and maximum local
  capture gaps reached 390-498 ms before recovering. The user identified the
  associated unfocused-window behavior as a likely transient Windows issue
  and confirmed normal visuals after it cleared. No v12 animation rejection
  or sustained network-path fault accompanied it. Retain this as a test-host
  observation and require reproduction before changing interpolation or the
  live protocol in response.
- The next live recovery slice advertises the existing v12 pose-
  acknowledgement feature under a new build-compatibility identity. A
  receiver reports a baseline only after the complete animation group has
  reassembled, decoded, and entered the existing animation path. Missing,
  superseded, conflicted, or resource-evicted baseline data latches one
  reliable request; the sender answers by forcing the next animation frame
  to be a self-contained keyframe rather than waiting for the normal
  20-frame interval.
- Pose controls are generation-, role-, target-session-, stream-, and
  group-bound, use the shared v12 control receive history for duplicate and
  stale rejection, and remain separate from unreliable realtime fragments.
  Dedicated telemetry records reports, requests, forced keyframes, and
  rejected controls. This slice deliberately preserves the validated v11
  keyframe/delta encoder; using only receiver-confirmed baselines for delta
  construction remains the next independently testable step.
- In run `20260823-203620-d6ea6010`, the user reported that the five-client
  recovery-control build looked good. Telemetry independently showed all 20
  sender/receiver paths finishing at 59.9-60.2 Hz with zero held-latest
  playback, completed-frame gaps, or superseded assemblies. Every client had
  four known and four visible peers, completed thousands of v12 animation
  groups, and exchanged thousands of decoded-baseline reports. A healthy run
  required no baseline requests or forced recovery keyframes.
- Animation-fragment, socket, delivery-policy, appearance-resource, and
  unsafe GPU-reuse faults remained zero. Client 2 rejected one delayed pose
  control among thousands, while clients 4 and 5 rejected 20 and 10 isolated
  root snapshots respectively without a sustained stream failure. Before
  decoded-baseline reports control delta selection, retain a bounded history
  of offered baselines so a valid delayed report cannot be mistaken for an
  unknown baseline. This telemetry does not establish the user's visual
  result.
- The sender baseline state now retains the 32 most recent offered baselines.
  A delayed authenticated decode report can confirm any retained offer, while
  a reordered older report cannot roll an already confirmed baseline
  backward. Unknown, evicted, and old-generation reports still fail closed.
  Offline tests cover delayed reports, reorder, bounded eviction, and
  generation reset before the live encoder begins selecting its delta base
  from this state.
- The live encoder now keeps offered quantized keyframes separate per
  recipient and does not install one as that recipient's delta base until its
  decoded-baseline report arrives. While confirmation is pending, subsequent
  frames remain self-contained keyframes. A valid report installs the exact
  retained quantized frame, then removes obsolete retained offers; a recovery
  request clears the confirmed base before forcing a fresh keyframe.
- This changes dependency ownership, not pose data: quantization, word
  encoding, fragmentation, send rate, interpolation, attachments, boards,
  appearance, and rendering remain unchanged. Dedicated counters report
  offered/unconfirmed keyframes and installed receiver-confirmed baselines.
  A five-client user visual and telemetry gate is required before beginning
  bandwidth packing.

Completion:

- Losing one realtime group does not discard the entire pose.
- Losing a baseline does not cause a long delta-decode blackout.
- Obsolete animation is discarded instead of delivered late.
- Appearance traffic cannot head-of-line block realtime animation.

## Phase 5: reduce bandwidth without fidelity loss

Purpose: make exact animation for every player affordable without changing
its visible result.

Order:

1. Bit-pack current fields without reducing precision.
2. Canonicalize quaternion signs before delta comparison.
3. Test smallest-three quaternion encoding.
4. Use per-chain translation ranges and omit reconstructable translations.
5. Preserve affine transforms for scale, shear, pivots, board, and attachment
   exceptions.
6. Reject any encoding whose measured or visual error exceeds the
   full-fidelity contract; there are no lower relevance tiers.

Every change is evaluated using final skinned-vertex displacement,
foot/board contact, board/truck/wheel pivot error, rapid-spin error, temporal
stability, and a user visual pass.

Initial target: reduce one exact full-fidelity stream from the current measured
approximately 274-280 KiB/s toward 90 KB/s average application payload.

## Phase 6: full-fidelity scale validation

Purpose: increase player count without changing what any receiver sees.

Every active player remains Exact: complete final pose, appearance-specific
tracks, board state, and attachments at 60 Hz. There are no Near, Mid,
Presence, Dormant, distance, visibility, importance, or player-count tiers.

Scale work should focus on:

- Shared quantization and serialization across recipients.
- Lossless or fidelity-verified packet compression.
- Batching and reduced packet count.
- Transport queue visibility and congestion diagnostics.
- Parallel decode/reconstruction and bounded renderer work.
- Direct P2P versus relay/server topology.
- CPU, GPU, memory, and bandwidth profiling at increasing player counts.

Adaptive fidelity remains a deferred contingency, not an implementation
milestone. Reintroducing it requires measured evidence from real full-fidelity
tests, a written fidelity impact, and an explicit roadmap decision.

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

For 6-20 players, retain P2P support and send the same full-fidelity stream to
every recipient. Share serialization work and do not make the host relay every
complete animation stream.

For 20-100 players, prefer a dedicated full-fidelity relay:

- Each client uploads one primary source stream.
- The relay reads root and pose-group metadata.
- It forwards every complete player stream to every receiver.
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

Soak tests should include two and five real clients, full-fidelity 6- and
20-client sessions, and realistic changing-pose headless tests at 50 and 100
clients. Root-only synthetic bots are load tests, not evidence of
full-animation or visual capacity.

## Change discipline

- Keep documentation, correctness fixes, threading changes, protocol changes,
  compression, and transport scheduling in separate commits.
- Every implementation commit states the visual invariants it preserves.
- Do not combine the network worker, protocol v12, and compression into one
  refactor.
- Do not replace exact nearby final-pose replication with semantic animation.
- Do not make continuous pose reliable.
- Do not transmit recipes or retail asset data continuously.
- Do not treat successful packet logs as proof of visual correctness.
