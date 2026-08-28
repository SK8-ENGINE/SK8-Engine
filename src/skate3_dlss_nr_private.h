#pragma once

// NVIDIA's private Streamline 2.13 preview supplied to the developer does not
// include its matching C++ headers. Keep the recovered ABI isolated here and
// compile it only for explicitly enabled local preview builds. The feature ID,
// structure GUID/version/layout and function signature were verified against
// NVIDIA-signed sl.dlss_nr.dll 2.13.0.0; no RenoDX implementation code is used.

#include <sl.h>

#include <cstddef>
#include <cstdint>

namespace sl {

inline constexpr Feature kFeatureDLSS_NR = 1004;
inline constexpr BufferType kBufferTypeUpliftInputColor = 70;
inline constexpr BufferType kBufferTypeUpliftOutputColor = 71;
inline constexpr std::uint64_t kSDKVersionDLSSNRPreview =
    (std::uint64_t{2} << 48) | (std::uint64_t{13} << 32) |
    kSDKVersionMagic;

enum class DLSSNRMode : std::uint32_t {
  eOff = 0,
  eOn = 1,
};

// {29DFDFE0-273A-4E72-B492-2DC823D5B1AD}
SL_STRUCT_BEGIN(
    DLSSNROptions,
    StructType({0x29dfdfe0,
                0x273a,
                0x4e72,
                {0xb4, 0x92, 0x2d, 0xc8, 0x23, 0xd5, 0xb1, 0xad}}),
    kStructVersion3)
  DLSSNRMode mode = DLSSNRMode::eOff;
  float intensity = 1.0f;
  float localToneStrength = 1.0f;
  float localStructureStrength = 1.0f;
  float globalToneStrength = 1.0f;

  // Version 2.
  std::uint32_t style = 0;
  std::uint32_t preset = 0;
  Boolean useAutoMask = Boolean::eFalse;
  float skinStructureStrength = 1.0f;

  // Version 3.
  std::uint32_t performanceMode = 3;
SL_STRUCT_END()

using PFun_slDLSSNRSetOptions =
    sl::Result(const sl::ViewportHandle &viewport,
               const sl::DLSSNROptions &options);

static_assert(sizeof(DLSSNROptions) == 72);
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#endif
static_assert(offsetof(DLSSNROptions, mode) == 32);
static_assert(offsetof(DLSSNROptions, intensity) == 36);
static_assert(offsetof(DLSSNROptions, style) == 52);
static_assert(offsetof(DLSSNROptions, useAutoMask) == 60);
static_assert(offsetof(DLSSNROptions, skinStructureStrength) == 64);
static_assert(offsetof(DLSSNROptions, performanceMode) == 68);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

} // namespace sl
