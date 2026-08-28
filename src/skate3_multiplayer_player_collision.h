#pragma once

#include "skate3_multiplayer.h"
#include "skate3_multiplayer_player_collision_model.h"

#include <cstdint>
#include <iosfwd>

struct PPCContext;

namespace skate3::multiplayer::player_collision {

// Render-thread producer. The remote positions are copied from the immutable
// presentation that the renderer consumes, then converted from map-local to
// world coordinates. No guest pointers cross this handoff.
void PublishRemotePresentation(const char *map_name,
                               const float map_render_origin[3],
                               const RemotePresentationFrame &presentation);

// Emulation-thread consumer. This runs after the exact local player's
// SkateboardController::FillPhysOut and applies only a bounded X/Z correction
// to that board transform.
void ApplyAfterPhysOut(PPCContext &ctx, std::uint8_t *base,
                       std::uint32_t controller, std::uint32_t phys_out);

void AppendTelemetry(std::ostream &out);

} // namespace skate3::multiplayer::player_collision
