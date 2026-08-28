# Notices and upstream permission

Skate 3 Custom Engine Layer is an unofficial fan project. It is not affiliated
with, authorized by, or endorsed by Electronic Arts.

This repository is a fork of
[Skate3Recomp](https://github.com/mchughalex/skate3recomp) and uses a companion
fork of
[rexglue-skate3](https://github.com/mchughalex/rexglue-skate3). The project
maintainer has confirmed that the upstream maintainer gave permission to
publish this source fork. That permission is recorded here so the fork's
provenance is explicit; it does not replace or broaden the ownership and
licensing terms attached to upstream or third-party files.

Original Custom Engine Layer additions are covered by
[LICENSE-PROJECT.md](LICENSE-PROJECT.md). Third-party components retain their
own notices and licenses.

Owned-world rigid-body simulation uses Erin Catto's
[Box3D](https://github.com/erincatto/box3d), pinned at release `v0.1.0`
(`8441b4a06d6d09dcfb0b0f704df4d847d1437b92`). Box3D is distributed under
the MIT License; its complete license is retained at
`third_party/box3d/LICENSE` and copied into Windows release packages as
`LICENSE-Box3D.txt`.

SKATE package compression uses
[zlib](https://github.com/madler/zlib), pinned at release `v1.3.2`
(`da607da739fa6047df13e66a2af6b8bec7c2a498`), and
[Zstandard](https://github.com/facebook/zstd), pinned at release `v1.5.7`
(`f8745da6ff1ad1e7bab384bd1f9d742439278e99`). Their complete licenses are
retained at `third_party/zlib/LICENSE` and `third_party/zstd/LICENSE` and
copied into Windows release packages as `LICENSE-zlib.txt` and
`LICENSE-zstd.txt`.

Optional Windows DirectX 12 builds use
[NVIDIA RTX Streamline](https://github.com/NVIDIA-RTX/Streamline), pinned to
SDK release `v2.12.0`. The official SDK archive SHA-256 is
`f5c0a3d870707dddc3570fb4bcd3655cf48a8a68c3a9d342910cfa21b77dcf48`.
Streamline source and interface code are provided under the MIT License.
DLSS is NVIDIA proprietary technology; `nvngx_dlss.dll` is redistributed only
in object-code form under NVIDIA's DLSS license. A DLSS-enabled package keeps
the SDK's `license.txt`, `3rd-party-licenses.md`, and
`nvngx_dlss.license.txt` with the runtime. Development plugins, SDK samples,
tools, symbols, and headers are not distributed. Before a public or
commercial DLSS-enabled release, the distributor must comply with NVIDIA's
notification, attribution, protective-terms, and trademark requirements.
Exact runtime versions, checksums, official source links, and development
identity requirements are documented in
[DLSS_SUPER_RESOLUTION.md](DLSS_SUPER_RESOLUTION.md).

An optional local-only DLSS Neural Rendering integration can consume
NVIDIA-signed private Streamline 2.13 / NGX 310.8 artifacts supplied directly
to an authorized NVIDIA developer. Those preview bundles are not committed or
included in public packages. Because the supplied bundles do not contain the
complete 2.13 SDK license and redistribution notices, this repository makes no
redistribution claim for them and the release packager rejects preview-enabled
builds. Exact private artifact hashes and the capability limitation are
recorded in
[DLSS_NEURAL_RENDERING_PREVIEW.md](DLSS_NEURAL_RENDERING_PREVIEW.md).

No Skate 3 ISO, extracted retail files, title updates, DLC, saves, proprietary
maps, or Electronic Arts assets are included. Users must provide their own
legally obtained game copy. Map authors are responsible for the rights to
everything embedded in a `.skate` package.
