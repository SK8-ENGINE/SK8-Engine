# DLSS Neural Rendering private preview

SK8 Engine has an optional Windows DirectX 12 integration for NVIDIA's private
Streamline 2.13 DLSS Neural Rendering feature. NVIDIA's signed plugin identifies
this as Streamline feature `1004` (`kFeatureDLSS_NR`). This is a separate
full-resolution post-pass after DLSS Super Resolution or DLAA; it is not a new
DLSS SR quality preset.

This preview does not add Frame Generation, Multi Frame Generation, Ray
Reconstruction, Reflex, or any unrelated NVIDIA feature. Vulkan is not
supported by SK8's integration even though the private plugin advertises a
Vulkan implementation.

## Pipeline and resources

The pass runs after the successful pre-tonemap HDR DLSS SR evaluation and
before bloom, tonemapping, outlines, HUD, menus, text, and editor overlays. It
receives:

- the full-output-resolution `R16G16B16A16_FLOAT` DLSS SR result as
  `kBufferTypeUpliftInputColor` (tag 70);
- a distinct full-output-resolution `R16G16B16A16_FLOAT` unordered-access
  output as `kBufferTypeUpliftOutputColor` (tag 71);
- the renderer's real D32 scene depth and signed motion vectors at internal
  render resolution;
- the same current/previous camera matrices, jitter, motion-vector scale,
  depth convention, and temporal reset supplied to DLSS SR.

On success the neural result replaces the SR image before the existing HDR
post chain. If setup, tagging, capability detection, or evaluation fails, SK8
leaves the valid SR image untouched. Turning DLSS SR Off preserves the original
native path.

The Graphics settings screen persists Neural Rendering On/Off and the private
plugin's genuine intensity, local/global tone, local/skin structure,
automatic-mask, style, preset, and performance controls. Values are
range-checked. Undocumented style, preset, and performance choices retain
their numeric SDK values rather than inventing labels. The generic RenoDX
addon was used only as a behavioral reference for parameter names and post-SR
placement; no addon code, shaders, or binary content is included.

## Private artifacts

The developer supplied two equivalent private archives. They are not committed:

| Artifact | Version | SHA-256 |
|---|---:|---|
| `SL 2.13.rar` | private bundle | `3ba4a7ab78a914bbe06cf5d30e5a83837e53854879da440fb6312d073d3f4455` |
| `streamline.zip` | private bundle | `ee86af93a25437477f28b8e3dc0078030fb7ece8ff44e0c13215ab0c57cfba61` |
| `sl.interposer.dll` | 2.13.0.0 | `27b2190057994c0b287c2c5716953bf1586f6499ac12fbbb2092b9aaf8396570` |
| `sl.common.dll` | 2.13.0.0 | `a4b2b5acbe49fbc6d44dd432cac19cd53218f698b2539dc7ed0fb268c72cfc8d` |
| `sl.dlss.dll` | 2.13.0.0 | `1eb5fb3d6f01d340fe086d981cc2de4f18aa6d05ee276e5cf28ecd54818dcc8b` |
| `sl.dlss_nr.dll` | 2.13.0.0 | `9f6672e5e0170dc118a3188d21bda187e1fc1aa3502895b21ab846d23165c11d` |
| `nvngx_dlss.dll` | 310.8.0.0 | `c85f971ce023c9f3492fc7455f0b01a24ba18ea39636407a846902c4360b0b7e` |
| `nvngx_dlssnr.dll` | 310.8.0.0 | `e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e` |

Windows Authenticode validates all six DLLs as NVIDIA-signed. The Streamline
log identifies this build as `v2.13.0-beta10`, Git SHA `11ffc00ec`.

The supplied bundles omit the matching 2.13 headers, the complete Streamline
license, `3rd-party-licenses.md`, and a DLSS Neural Rendering redistribution
notice. Therefore the repository does not claim redistribution permission,
does not commit these binaries, and deliberately refuses to package a public
release from a preview-enabled build. Obtain the complete licensed SDK and
written redistribution direction from the NVIDIA developer representative
before shipping.

The currently public official sources remain:

- <https://github.com/NVIDIA-RTX/Streamline/releases>
- <https://github.com/NVIDIA/DLSS>
- <https://developer.nvidia.com/rtx/streamline>

At the time this integration was made, those public sources did not document
Streamline 2.13, feature 1004, or DLSS Neural Rendering 310.8.

## Local build and capability result

Configure only with the authorized local private SDK root:

```powershell
cmake -S . -B out/build/dlss5-release `
  -DSKATE3_ENABLE_DLSS_SR=ON `
  -DSKATE3_ENABLE_DLSS_NR_PREVIEW=ON `
  -DSKATE3_STREAMLINE_SDK_ROOT=C:\path\to\authorized\streamline-2.13
cmake --build out/build/dlss5-release --target skate3
```

The configuration rejects any runtime whose SHA-256 differs from the table.
Missing neural DLLs at runtime disable only Neural Rendering; they do not stop
the game or DLSS SR from starting.

The automated D3D12 API smoke check on the development host produced:

```text
set-device=0 dlss-sr-support=0 dlss-nr-support=32 nr-function=31 pointer=no
```

Its Streamline log confirms that 2.13, host SDK version 2.13, and DLSS SR 310.8
initialized, then reports:

```text
ngxResult failed 0xbad0000c
DLSS-NR feature is not supported. Please check if you have a valid
nvngx_dlssnr.dll or your driver supports DLSS-NR.
```

NVIDIA's public NGX definitions decode `0xBAD0000C` as
`NVSDK_NGX_Result_FAIL_OutOfDate`: the requested function requires a newer
display driver or feature library. The supplied feature library is the signed
310.8.0 file, while the development host is already running NVIDIA's latest
public WHQL driver available on 2026-08-28 (610.74). Therefore final image and
performance validation requires the matching NVIDIA DLSS 5 preview/insider
driver; replacing the application capability check with an unofficial bypass
is not an acceptable integration.

The normal custom-engine `ProjectDesc` path works for DLSS SR and no
application ID has been invented. The available artifacts do not establish
whether NVIDIA also requires an issued application ID or entitlement for
Neural Rendering release access; confirm that and obtain the matching headers,
driver, and redistribution terms from the NVIDIA representative.
