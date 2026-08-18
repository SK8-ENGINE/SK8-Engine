#pragma once

#include <cstdint>
#include <iosfwd>

struct PPCContext;

namespace skate3::native_grind {

// Generated-function observers for the retail tSplineData load path and the
// GrindData registration boundary. They capture the authoritative runtime
// owner/lifetime contract without replacing either function.
void ObserveSplineDataLoad(const PPCContext& ctx,
                           std::uint8_t* base) noexcept;
void ObserveGrindDataAdd(const PPCContext& ctx,
                         std::uint8_t* base) noexcept;

bool Enabled();

// Compiles the active owned map's readable GrindRail paths into native
// Pegasus tSplineData and registers them through Skate's real GrindData
// implementation. Collision and presentation stay separate consumers of the
// same authored map.
void EnsureInstalled(PPCContext& ctx, std::uint8_t* base) noexcept;

void AppendTelemetry(std::ostream& out);

}  // namespace skate3::native_grind
