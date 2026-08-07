#pragma once

#include <cstdint>

struct PPCContext;

namespace skate3::cac_gesture {

bool IsActive();
bool IsUpdating();
int32_t Phase();
int32_t Selected();
uint32_t ResolveGestureDescriptorAddress(PPCContext& ctx, uint8_t* base,
                                         uint32_t address);
void ObserveLoadBaseAnims(PPCContext& ctx, uint8_t* base);
void ObservePendingGestureUpdate(PPCContext& ctx, uint8_t* base);
void ObserveLoadGestureById(PPCContext& ctx, uint8_t* base);
void ObserveLoadGestureByName(PPCContext& ctx, uint8_t* base);
void ObservePendingGestureGetter(PPCContext& ctx, uint8_t* base);

}  // namespace skate3::cac_gesture
