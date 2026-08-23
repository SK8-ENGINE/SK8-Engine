#pragma once

#include <cstdint>

namespace skate3::multiplayer::topology {

inline bool DirectLocalMeshEnabled(std::uint32_t participant_count) {
  return participant_count >= 2 && participant_count <= 100;
}

inline bool FullFidelitySession(std::uint32_t participant_count) {
  return participant_count >= 1 && participant_count <= 100;
}

inline bool DirectLocalTarget(std::uint32_t local_role,
                              std::uint32_t target_role,
                              std::uint32_t participant_count) {
  return DirectLocalMeshEnabled(participant_count) &&
         local_role >= 1 && local_role <= participant_count &&
         target_role >= 1 && target_role <= participant_count &&
         target_role != local_role;
}

inline bool HostRelaysLocalPackets(std::uint32_t local_role,
                                   std::uint32_t participant_count) {
  return local_role == 1 && !DirectLocalMeshEnabled(participant_count);
}

}  // namespace skate3::multiplayer::topology
