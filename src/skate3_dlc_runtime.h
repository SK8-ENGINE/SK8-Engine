#pragma once

#include "generated/skate3_init.h"

namespace skate3::dlc_runtime {

bool IsGunplayGestureArchiveMounted();
uint32_t ProbeGunplayGestureCollection(PPCContext& ctx, uint8_t* base);
void ObserveManagerConstructor(PPCContext& ctx);
void ObserveManagerRun(PPCContext& ctx);
void ObserveManagerEnumerate(PPCContext& ctx);
void ObserveManagerRefresh(PPCContext& ctx);
void ObserveDriverMount(PPCContext& ctx);
void ObserveContentManagerEnumerate(PPCContext& ctx, uint8_t* base);
void ObserveAddArchiveFromFile(PPCContext& ctx, uint8_t* base);
void ObserveAsyncFileOpen(PPCContext& ctx, uint8_t* base);
void ObserveVaultDlcLoadUpdate(PPCContext& ctx, uint8_t* base);
void ObserveFindCollection(PPCContext& ctx);
void ObserveLanguageDlcLoadUpdate(PPCContext& ctx);

}  // namespace skate3::dlc_runtime
