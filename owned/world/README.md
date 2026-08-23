# Owned world and map module

This is the first project-owned subsystem intended to replace Skate 3's
original world incrementally.

The module is deliberately independent of:

- generated recomp functions and guest addresses;
- the current native sandbox renderer vertex format;
- Godot or another presentation engine;
- extracted retail map assets;
- a particular third-party physics engine.

## Current boundary

One readable `MapDefinition` owns:

- named surface materials;
- a player spawn transform;
- an owned sky palette and directional sun;
- renderer-neutral vertices and indices;
- a spatial render-world compiler with exact chunk bounds;
- matching collision triangles;
- named grind-rail polylines and exact retail native cubic segments;
- authored native-AI route polylines and population intent;
- stable surface IDs;
- downward ray/ground queries;
- sphere contact queries.

`MakeStarterFlatgroundMap()` is a completely handwritten 320-by-320-metre
district containing plazas, streets, banks, quarter pipes, stairs, rails,
ledges, planters, loading docks, and perimeter buildings. Both visual and
collision geometry are built from the same source calls so they cannot
silently drift apart.

`BuildRenderWorld()` clips triangles at 64-metre X/Z cell boundaries and
emits bounded material batches. This matters for genuinely large geometry:
an apron or terrain triangle spanning many cells cannot be assigned to one
giant false bound and defeat culling. The recomp adapter uses the resulting
sorted cell table for bounded detail-radius lookup, frustum culling, lazy GPU
upload, and existing mesh-LRU residency.

`BuildGrindSplineData()` compiles the readable `GrindRail` paths to
relocatable Pegasus `tSplineData`. Each segment contains its native direction,
start point, inverse length, bounds, cumulative length, parent rail, and
previous/next links. The recomp adapter relocates the blob into persistent
guest memory and registers it through Skate 3's authoritative `GrindData`
runtime; it does not implement proximity snapping or a parallel grind
simulation. Retail imports use the same path while preserving their original
spline ID, type signature, cubic coefficients, and auxiliary segment words;
only translated positions/bounds and regenerated guest links differ.

`NpcRoute` supplies map-local guide points, target speed, spacing, and an
authored population count. The recomp adapter replaces the future navigation
node consumed by Skate 3's AI path controller while preserving its native
input generation, board physics, animation, collision response, and bails.

## Build and test

From the fork root:

```powershell
cmake -S owned/world -B out/build/owned-world `
  -DSKATE_OWNED_WORLD_BUILD_TESTS=ON
cmake --build out/build/owned-world --config Release
ctest --test-dir out/build/owned-world -C Release --output-on-failure
```

## Integration sequence

1. [x] Keep this library independently tested.
2. [x] Add a recomp presentation adapter that converts `RenderMesh` into the
   existing native scene vertex format.
3. [x] Replace the old sandbox map definitions with
   `MakeStarterFlatgroundMap()`.
4. [x] Recover a narrow player-owned runtime handoff around
   `SkateboardController::FillPhysOut` and `Skateboard::SetPosition`.
5. [x] Add a default-off experimental adapter that applies downward
   `RayHit` results to board height and the exact player's grounded state.
6. [x] Compile the owned triangles into a native RenderWare clustered mesh
   and install it as the sole static-world collision provider.
7. [x] Add chunked visual compilation, bounded spatial lookup, lazy GPU
   residency, an owned camera-relative sky, and authored sun lighting.
8. [x] Remove the single-cluster collision limit, add authored procedural
   material patterns, and receive the runtime's dynamic cascaded shadows.
9. [x] Add readable owned grind paths, a tested native `tSplineData`
   compiler, and authoritative GrindData registration.
10. [x] Add image-backed PBR materials and owned static shadow casters.
11. [ ] Add streamed collision registration, environment probes, LOD/HLOD,
   and asynchronous asset I/O.

The recomp sandbox now links this library directly. Its visible map mesh and
read-only sphere-contact observer both consume the same
`MakeStarterFlatgroundMap()` instance. Live telemetry identifies this boundary
with `sandbox_map_source=owned`.

The release executable enables the Custom Engine Layer, native collision, and
retail-collision replacement by default. The owned map is compiled into Skate
3's real static collision format without a development harness or command-line
flags. No board-height correction or grounded-state override is required.
Human play validation has confirmed the floor and ramps.

The collision compiler now partitions KD-tree leaves into native clusters,
keeping every cluster within RenderWare's 255-local-vertex limit while
retaining a larger global welded mesh. This removes the old whole-map limit
but is not yet collision streaming: the complete map is still one registered
aggregate and must next adopt renderer-cell lifetime management.

Materials carry a project-owned pattern, world-space texture scale,
roughness, and variation. The current shader evaluates those patterns
procedurally and receives the runtime's dynamic cascaded shadows. This is a
real material/lighting contract, but it is not yet an image-backed PBR asset
pipeline, owned static-shadow pass, global illumination system, or
probe-based sky-lighting solution.
