#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <vector>

namespace skate3::multiplayer {

inline constexpr std::uint32_t kCanonicalSkeletonTrackKey = 0x314E4143u;

// Renderer-neutral replicated pose. Positions are expressed in the active
// custom map's local coordinate frame so two recomp processes do not need to
// share the retail world's hidden absolute origin.
struct RemotePose {
  float position[3] = {};
  float x_axis[3] = {1.0f, 0.0f, 0.0f};
  float y_axis[3] = {0.0f, 1.0f, 0.0f};
  float z_axis[3] = {0.0f, 0.0f, 1.0f};
  std::uint32_t board_state_flags = 0xFFFFFFFFu;
  // Receiver-local presentation metadata. This is never serialized. It
  // marks an interpolation segment whose packet endpoints are far enough
  // apart that a collision proxy must snap with overlap grace instead of
  // sweeping through the world.
  bool collision_discontinuity = false;
};

// Canonical model-to-world skeleton produced by Skate 3 before per-mesh bone
// remapping. Protocol v6 sends one kCanonicalSkeletonTrackKey track; each
// receiver rebuilds its locally loaded clothing palettes with the game's own
// captured mesh remaps. Each bone is three float4 affine rows; translations
// are map-local while in transit and restored to the active map origin.
struct AnimationTrack {
  std::uint32_t mesh_key = 0;
  std::vector<float> bone_rows;
};

struct AnimationPose {
  std::uint64_t sender_time_us = 0;
  std::uint32_t sequence = 0;
  std::uint16_t root_bone = 0xFFFFu;
  float root_position[3] = {};
  // Final rendered character root used by the lightweight pose stream and
  // receiver-side clone mapping. This deliberately stays separate from
  // root_position, which is the translation reference for encoded bones.
  bool presentation_root_valid = false;
  float presentation_root_position[3] = {};
  float presentation_root_x_axis[3] = {1.0f, 0.0f, 0.0f};
  float presentation_root_z_axis[3] = {0.0f, 0.0f, 1.0f};
  std::vector<AnimationTrack> tracks;
};

// Versioned engine-owned appearance payload. The primary format carries the
// sender's compact CAC recipe plus animation-binding metadata; the receiver
// resolves meshes and textures from its local vanilla asset catalogue.
// Runtime animation remains a separate compact stream. The older assembled
// mesh/texture bundle remains readable as a compatibility fallback.
struct AppearanceBlob {
  std::uint64_t identity = 0;
  std::shared_ptr<const std::vector<std::uint8_t>> bytes;
};

struct RemotePlayer {
  std::uint32_t role = 0;
  std::uint32_t session = 0;
  // The receiving process and map generation that prepared this immutable
  // presentation. These fields are local-only and let other presentation
  // consumers reject a stale mailbox frame during role, session, or map
  // transitions without changing the network protocol.
  std::uint32_t receiver_role = 0;
  std::uint32_t receiver_session = 0;
  std::uint32_t presentation_map_hash = 0;
  RemotePose pose;
  AnimationPose animation;
  AppearanceBlob appearance;
};

// One renderer-owned peer generation that the replication core has
// definitively replaced or forgotten. The session prevents a delayed
// retirement from releasing a newer occupant of the same reusable role.
struct RemotePeerRetirement {
  std::uint32_t role = 0;
  std::uint32_t session = 0;
};

// Immutable prepared presentation published by the replication core. A
// worker can replace the shared player vector without mutating data currently
// consumed by the renderer; retirement events remain one-shot.
struct RemotePresentationFrame {
  std::uint64_t sequence = 0;
  std::shared_ptr<const std::vector<RemotePlayer>> players;
  std::vector<RemotePeerRetirement> retirements;
};

// Samples the verified local board, services the current transport, and
// returns independently smoothed remote players alive on the same map. The
// first transport is localhost UDP; the packet and pose seam is deliberately
// independent from it so another transport can replace only networking.
bool TickLocalVisuals(const char* map_name,
                       const float map_render_origin[3],
                       const AnimationPose* local_animation,
                       const AppearanceBlob* local_appearance,
                       RemotePresentationFrame& out_presentation);

void AppendTelemetry(std::ostream& out);

// Reports that the renderer committed the complete sender-owned appearance
// currently associated with this role, process session, and identity. Stale
// reports are ignored by the replication runtime.
void ReportRemoteAppearanceInstalled(std::uint32_t role,
                                     std::uint32_t session,
                                     std::uint64_t appearance_id);

}  // namespace skate3::multiplayer
