# DLSS Neural Rendering private preview

SK8 Engine has an optional Windows DirectX 12 integration for NVIDIA's private
Streamline 2.13 DLSS Neural Rendering feature. NVIDIA's signed plugin identifies
this as Streamline feature `1004` (`kFeatureDLSS_NR`). This is a separate
full-resolution post-pass after DLAA; it is not a new DLSS SR quality preset.
The preview is deliberately limited to DLAA because that is the only mode
validated to retain a correct full-resolution color/depth/motion contract.
Quality, Balanced, and Performance remain available for ordinary DLSS Super
Resolution but do not expose Neural Rendering.

This preview does not add Frame Generation, Multi Frame Generation, Ray
Reconstruction, Reflex, or any unrelated NVIDIA feature. Vulkan is not
supported by SK8's integration even though the private plugin advertises a
Vulkan implementation.

## Pipeline and resources

The pass runs after the successful pre-tonemap HDR DLAA evaluation and
before bloom, tonemapping, outlines, HUD, menus, text, and editor overlays. It
receives:

- the full-output-resolution `R16G16B16A16_FLOAT` DLAA result as
  `kBufferTypeUpliftInputColor` (tag 70);
- a distinct full-output-resolution `R16G16B16A16_FLOAT` unordered-access
  output as `kBufferTypeUpliftOutputColor` (tag 71);
- the renderer's real full-resolution D32 scene depth and signed motion
  vectors;
- the same current/previous camera matrices, jitter, motion-vector scale,
  depth convention, and temporal reset supplied to DLSS SR.

On success the neural result replaces the DLAA image before the existing HDR
post chain. If setup, tagging, capability detection, or evaluation fails, SK8
leaves the valid DLAA image untouched. Turning DLSS Off preserves the original
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
the game or ordinary DLSS SR from starting. Selecting a non-DLAA quality mode
also disables the private post-pass cleanly.

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

## Personal DLAA validation through a trusted RenoDX add-on

An authorized developer can also perform a local, personal interoperability
check with ReShade's add-on-enabled runtime and a separately obtained,
trusted RenoDX DLSS 5 add-on. Neither ReShade nor the add-on is part of SK8,
and neither binary is committed, packaged, downloaded, or redistributed by
this repository. An `.addon64` file is executable code with the same access as
the game process; use only a copy whose provenance and hash you have verified.

The configuration validated on the development host was:

- SK8's Windows DirectX 12 renderer;
- the private Streamline 2.13 / NGX 310.8 runtime listed above;
- ReShade 6.8.0's official add-on-enabled `ReShade64.dll`, installed beside
  `skate3.exe` as `dxgi.dll`;
- RenoDX DLSS 5 add-on version `v0.2026.827.2036`, API 18, SHA-256
  `87aef9ddd937c7241e6bf8d8efea0045d63559135e254c60dab316db3d3a4aee`,
  installed beside `skate3.exe` with its `.addon64` extension.

### Exact local file layout

Use the directory containing the exact feature build's `skate3.exe`. Do not
install into the source directory, game-data directory, extracted ISO, or a
different SK8 worktree. The local test directory should contain:

| File beside `skate3.exe` | Obtain it from | Notes |
|---|---|---|
| `sl.interposer.dll` | authorized Streamline 2.13 `bin/x64` | Keep the filename unchanged. |
| `sl.common.dll` | authorized Streamline 2.13 `bin/x64` | Keep the filename unchanged. |
| `sl.dlss.dll` | authorized Streamline 2.13 `bin/x64` | Keep the filename unchanged. |
| `sl.dlss_nr.dll` | authorized Streamline 2.13 `bin/x64` | Private preview plugin; do not redistribute. |
| `nvngx_dlss.dll` | authorized Streamline 2.13 `bin/x64` | Signed DLSS SR runtime. |
| `nvngx_dlssnr.dll` | authorized Streamline 2.13 `bin/x64` | Private preview runtime; do not redistribute. |
| `dxgi.dll` | official ReShade 6.8.0 add-on build's `ReShade64.dll` | Rename only this file from `ReShade64.dll` to `dxgi.dll`. |
| `renodx-dlss5.addon64` | independently obtained trusted copy | Preserve the `.addon64` filename and verify its provenance/hash first. |

The resulting directory is:

```text
<feature-build>\
  skate3.exe
  rexruntime.dll
  sl.interposer.dll
  sl.common.dll
  sl.dlss.dll
  sl.dlss_nr.dll
  nvngx_dlss.dll
  nvngx_dlssnr.dll
  dxgi.dll
  renodx-dlss5.addon64
```

Alternatively, run the official ReShade installer, select this exact
`skate3.exe`, choose DirectX 10/11/12, and select the build with full add-on
support. ReShade effects and shader packages are not required. Confirm that
the installer placed its loader beside this executable. Never obtain ReShade,
NVIDIA DLLs, or add-ons from DLL aggregation or mod-rehosting sites.

To reproduce the successful path:

1. Build SK8 with `SKATE3_ENABLE_DLSS_SR=ON` and
   `SKATE3_ENABLE_DLSS_NR_PREVIEW=ON` using an authorized private SDK root.
2. Create the exact executable-folder layout above using the official ReShade
   add-on-enabled runtime and the independently trusted add-on. Do not place
   these files in the source tree or a release archive.
3. Select DirectX 12 and **DLAA** in SK8's NVIDIA DLSS Super Resolution
   setting. Keep SK8's separate **Neural Rendering** setting **Off**.
4. Open ReShade with **Home**, select **Add-ons**, and verify **DLSS 5 Neural
   Rendering** is loaded. Enable **Neural Uplift**. The add-on's separate
   upscaling option is unnecessary in DLAA and should remain disabled.
5. Remain in gameplay long enough for temporal history to settle. The add-on
   status should report feature-18 evaluations with identical input, guide,
   and output dimensions and should advance beyond frame 60.

This path was confirmed by the add-on reporting successful feature-18
evaluation through at least frame 60 with 2560x1440 input, guides, and output.
The generic add-on is **not compatible with Quality, Balanced, or
Performance** in this integration: it accepted the first low-resolution
transition frame and then rejected subsequent frames despite Streamline
receiving the correct tagged extents. Use DLAA only. Do not work around the
guard by fabricating or spatially resizing depth or motion vectors.

ReShade or another DXGI proxy can intercept factory and swap-chain creation
before Streamline observes them. SK8 therefore registers its D3D12 device
immediately after creation and upgrades the created presentation interface
through Streamline when the optional interposer is loaded. Without those two
steps the add-on may appear in ReShade while never evaluating during presents.
When no Streamline interposer is loaded, these checks are no-ops and the
original renderer initialization remains unchanged.

The normal custom-engine `ProjectDesc` path works for DLSS SR and no
application ID has been invented. The available artifacts do not establish
whether NVIDIA also requires an issued application ID or entitlement for
Neural Rendering release access; confirm that and obtain the matching headers,
driver, and redistribution terms from the NVIDIA representative.
