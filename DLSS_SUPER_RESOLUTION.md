# NVIDIA DLSS Super Resolution

SK8 Engine can optionally build NVIDIA DLSS Super Resolution support for its
Windows DirectX 12 renderer. This integration is deliberately limited to DLSS
Super Resolution and DLAA. It does not load or expose Frame Generation, Multi
Frame Generation, Ray Reconstruction, Reflex, or other Streamline features.

## Runtime behavior

The Graphics settings page offers Off, Quality, Balanced, Performance, and
DLAA when the DirectX 12 backend is active. NVIDIA's DLSS plugin selects the
optimal render resolution for the current output resolution; the engine does
not use hard-coded scale percentages. The status text reports the selected
mode, actual internal and output dimensions, and a concise reason when the
feature is unavailable.

Off is the default. An invalid saved value is treated as Off. Vulkan does not
expose the setting because this integration currently supports DirectX 12
only.

The Streamline runtime is loaded dynamically from the executable directory.
A build without DLSS, a missing plugin, an unsupported GPU or driver, and a
non-NVIDIA GPU all retain the existing native rendering path without requiring
an NVIDIA DLL or showing a startup error.

DLSS consumes the pre-tonemap HDR scene color, D32 depth, signed
previous-minus-current motion vectors in normalized window space (with
Streamline's matching `{1,1}` scale), unjittered current and previous camera
matrices, projection data, camera basis and position,
per-frame jitter, auto exposure, and an explicit history reset. The evaluation
is recorded near the beginning of post-processing, after native opaque and
transparent scene rendering and the existing scene-space preparation passes,
and before bloom, tone mapping, HUD, menus, text, and native editor overlays.
Rigid, skinned, owned-world, and editor-moved objects retain previous
transforms; skinned draws retain their previous bone palette.

History resets on activation, quality or render-size changes, output resize,
scene generation changes such as map loads and respawns, camera discontinuity
or teleport, missed native frames, and device or viewport recreation.
Rendering below output resolution uses NVIDIA's documented mip LOD bias.

## Building

Use only the official NVIDIA RTX Streamline SDK 2.12.0 release:

- Release: <https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.12.0>
- Archive: <https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.12.0/streamline-sdk-v2.12.0.zip>
- Archive SHA-256:
  `f5c0a3d870707dddc3570fb4bcd3655cf48a8a68c3a9d342910cfa21b77dcf48`

Configure with:

```powershell
cmake -S . -B out/build/dlss-release `
  -DSKATE3_ENABLE_DLSS_SR=ON `
  -DSKATE3_STREAMLINE_SDK_ROOT=C:\path\to\streamline-sdk-v2.12.0
cmake --build out/build/dlss-release --target skate3
```

The configure step rejects runtime files whose SHA-256 does not match the
pinned SDK:

| Runtime file | Version | SHA-256 |
|---|---:|---|
| `sl.interposer.dll` | 2.12.0.0 | `2a79db6857ae8c75bbd871a9489c48bc6a39f7fcc88b9b02afd53d0376cbec66` |
| `sl.common.dll` | 2.12.0.0 | `c57930ef5a8a3fe9be85efdf71a61d8107c1148e8a6aed456464547128f7f4ae` |
| `sl.dlss.dll` | 2.12.0.0 | `a997022d2b93601e0eefc3ddb3067c36df386dd3163ae71e11095191fb14f8e4` |
| `nvngx_dlss.dll` | 310.7.0.0 | `be6e434a94ca32499515eb62ca0e6c274526055d568d0426e4c652dcdfb6ee6e` |

The release package contains only these four production runtime files and
their required notices. Development plugins, samples, headers, import
libraries, symbols, and SDK tools are excluded.

## NVIDIA identity and release requirements

Local development uses Streamline's documented custom-engine path with
application ID `0`, a real engine name/version, and no invented project GUID.
An NVIDIA-issued numeric application ID and project GUID can be supplied at
configure time with `SKATE3_NVIDIA_APPLICATION_ID` and
`SKATE3_NVIDIA_PROJECT_ID`.

The Streamline core preferences document the custom engine plus engine-version
identity as an alternative when an application ID is not supplied. However,
the Streamline DLSS guide separately states that an NVIDIA-provided application
ID is required for NGX components such as DLSS. The unregistered `0` value is
therefore suitable only for checking the integration's graceful local failure
path; real RTX image-quality validation and any public DLSS-enabled release
require an ID issued by NVIDIA. This project never substitutes the SDK
sample's ID or invents one.

Before a public or commercial DLSS-enabled release, the distributor must also
coordinate with NVIDIA, satisfy the DLSS object-code redistribution terms
(including NVIDIA's notice/attribution and protective terms), and notify
NVIDIA as required by that license.

See the official
[Streamline programming guide](https://github.com/NVIDIA-RTX/Streamline/blob/v2.12.0/docs/ProgrammingGuide.md),
[DLSS programming guide](https://github.com/NVIDIA-RTX/Streamline/blob/v2.12.0/docs/ProgrammingGuideDLSS.md),
and [DLSS license](https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt).
