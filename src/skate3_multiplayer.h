#pragma once

#include <cstdint>
#include <iosfwd>
#include <vector>

namespace skate3::multiplayer {

// Renderer-neutral replicated pose. Positions are expressed in the active
// custom map's local coordinate frame so two recomp processes do not need to
// share the retail world's hidden absolute origin.
struct RemotePose {
  float position[3] = {};
  float x_axis[3] = {1.0f, 0.0f, 0.0f};
  float y_axis[3] = {0.0f, 1.0f, 0.0f};
  float z_axis[3] = {0.0f, 0.0f, 1.0f};
  std::uint32_t board_state_flags = 0xFFFFFFFFu;
};

// Final model-to-world palettes produced by Skate 3's animation renderer.
// Character pieces can use different bone remaps, so each mesh carries its
// own palette instead of sharing one guessed "master" skeleton. Each bone is
// three float4 affine rows; translations are map-local while in transit and
// are restored to the receiver's active map origin on output.
struct AnimationTrack {
  std::uint32_t mesh_key = 0;
  std::vector<float> bone_rows;
};

struct AnimationPose {
  std::uint64_t sender_time_us = 0;
  std::uint32_t sequence = 0;
  float root_position[3] = {};
  std::vector<AnimationTrack> tracks;
};

struct RemotePlayer {
  std::uint32_t role = 0;
  RemotePose pose;
  AnimationPose animation;
};

// Samples the verified local board, services the current transport, and
// returns independently smoothed remote players alive on the same map. The
// first transport is localhost UDP; the packet and pose seam is deliberately
// independent from it so another transport can replace only networking.
bool TickLocalVisuals(const char* map_name,
                      const float map_render_origin[3],
                      const AnimationPose* local_animation,
                      std::vector<RemotePlayer>& out_remotes);

void AppendTelemetry(std::ostream& out);

}  // namespace skate3::multiplayer
