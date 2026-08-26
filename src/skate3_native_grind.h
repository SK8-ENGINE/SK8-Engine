#pragma once

#include <cstdint>
#include <iosfwd>

struct PPCContext;

namespace skate3::native_grind {

// The tSplineData observer captures the authoritative runtime owner/lifetime
// contract. The GrindData boundary rejects vanilla rail vectors before the
// game's allocator/registry can make invisible original-world rails active.
void ObserveSplineDataLoad(const PPCContext& ctx,
                           std::uint8_t* base) noexcept;
bool ShouldSuppressGrindDataAdd(const PPCContext& ctx,
                                std::uint8_t* base) noexcept;

bool Enabled();

// Compiles the active owned map's readable GrindRail paths into native
// Pegasus tSplineData and registers them through Skate's real GrindData
// implementation. Collision and presentation stay separate consumers of the
// same authored map.
void EnsureInstalled(PPCContext& ctx, std::uint8_t* base) noexcept;

void AppendTelemetry(std::ostream& out);

}  // namespace skate3::native_grind
