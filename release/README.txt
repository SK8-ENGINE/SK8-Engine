SKATE 3 CUSTOM ENGINE LAYER
===========================

This is an unofficial project and is not affiliated with Electronic Arts.
No Skate 3 retail game files or custom maps are included.

This project builds on Skate3Recomp and adds a project-owned custom world,
rendering, physics and Blender-to-SKATE layer.

This is a Windows/D3D12 preview. Read KNOWN_ISSUES.md before distributing or
reporting a problem.

DLSS SUPER RESOLUTION
---------------------

When this package includes the optional official NVIDIA runtime, Settings >
Graphics offers DLSS Off, Quality, Balanced, Performance, and DLAA on the
DirectX 12 backend. Off is the default. Unsupported or non-NVIDIA hardware,
old drivers, and missing plugins fall back to the normal renderer without a
startup error. Vulkan is not supported by this DLSS integration.

The package must contain only the pinned production DLSS runtime and its
notices. See DLSS_SUPER_RESOLUTION.md for exact versions, checksums, licensing,
and the NVIDIA coordination required before public distribution.

Private Streamline 2.13 / DLSS Neural Rendering preview binaries are never
included in this archive. Their supplied bundle lacks complete redistribution
terms. Authorized testers can read the included
DLSS_NEURAL_RENDERING_PREVIEW.md guide; ReShade, RenoDX, and private NVIDIA
files remain local-only.

FIRST START
-----------

1. Extract the entire zip to a folder you control.
2. Run skate3.exe.
3. Select your own legally obtained Skate 3 Xbox 360 ISO when prompted.
4. Allow the installer to prepare the required local game data.
5. Start the game normally. The Custom Engine Layer activates automatically
   when gameplay begins; no command-line options or development harness are
   required.

CUSTOM MAPS
-----------

Put .skate files in the included maps folder. In game, open Settings > Maps,
refresh the list, choose a package, and select Load Selected Map.

The session restarts automatically to load every map resource cleanly.
The Maps tab can also open the exact folder in File Explorer.

Select Vanilla Mode to restart into Alex's native renderer using the retail
University world's streaming, collision, GrindData, splines, shadows, and
gameplay data. No retail map is included. Selecting a custom map later restores
the saved custom-world profile.

OBJECT LIBRARY
--------------

The included objects folder lives beside maps and skate3.exe. Its immediate
subfolder names are the categories shown by the object spawner. Create any new
subfolder to add your own category, then put .skateobj files directly inside.

Press G during gameplay to open the map editor and E to open the object
spawner. Select Build / repair defaults to extract the supported vanilla park
objects from your own installed game files into their included category
folders. No retail object files are included in this release.

UPDATES
-------

Open Settings > System and select Update Custom Engine Layer. The game checks
the official release channel, downloads and verifies the new archive, closes,
updates the shipped application files, and restarts. Existing game data,
saves, settings, and custom maps are preserved.

The latest Blender addon zip is refreshed in "Blender Map Tools", but it is
not installed into Blender automatically. Install that zip manually whenever
you want to use newly added authoring features.

MULTIPLAYER PREVIEW
-------------------

Open Escape > Multiplayer to host, browse, join, or leave a session.
Multiplayer is an early visual-replication preview; gameplay authority,
scoring, remote collisions, host migration, and production security are not
finished.

Steam internet testing currently requires a private development Steam setup.
Keep Steam open and signed in before starting the game. On first launch, the
game securely downloads and verifies the pinned development runtime and
generates the local App 480 marker. Spacewar does not open another window;
Steam tracks skate3.exe as the running App 480 game.

The App 480 marker and Valve runtime are generated or acquired locally rather
than embedded in this archive. See MULTIPLAYER.md for the exact source,
verification boundary, local fallback, current bandwidth, and limitations.

BLENDER
-------

The "Blender Map Tools" folder contains the self-contained Owned World
Authoring addon and SKATE documentation. Install the zip in Blender, then
open 3D View > Sidebar > Skate 3 Map to prepare, validate, and export maps
without running scripts.

Its "Source Tools" subfolder contains the reviewed standalone map utilities,
addon source, regression tests, and portable sk8-auto-map agent workflow.
Give that workflow's SKILL.md and a Blender file to a compatible coding agent
when you want duplicate-aware material, collision, lighting, spawn, and grind
preparation. The source Blender file is never overwritten.

Only distribute maps and assets that you have permission to distribute.
See CUSTOM_MAPS.md for the complete map workflow.

VANILLA MAP EXTRACTION
----------------------

The "Vanilla Map Extraction Tools" folder contains the preservation-first
University extraction, Blender conversion, validation, and format research
tools. It contains no retail game assets or generated maps. Read its README
before use; you must provide your own legally obtained retail files, Python
dependencies, Blender, and the external UTT parser.

LICENSING
---------

Original Custom Engine Layer code is covered by LICENSE-PROJECT.md.
Upstream-derived and third-party files retain their own terms. See NOTICE.md.
