#pragma once

#include <iosfwd>

namespace skate3::owned_world_boundary {

// True when the custom-world runtime owns the active scene and retail
// dynamic-world actors must not be created behind it.
bool SuppressingRetailWorldActors();

// Appends compact machine-readable counters to the harness STATUS response.
void AppendTelemetry(std::ostream& out);

}  // namespace skate3::owned_world_boundary
