# Known issues

This is an experimental preview rather than a finished standalone game engine.

- The Custom Engine Layer is currently validated only on Windows with D3D12.
  Vulkan, Linux, and macOS builds inherit upstream support but have not been
  validated with the custom renderer and owned-world shaders.
- Native AI/NPC route records export in SKATE v8, but reliable route following
  is shelved. Maps should request zero AI skaters for release use.
- Very large maps currently load complete visual and collision packages.
  Collision streaming, HLOD generation, asynchronous asset I/O, and a formal
  memory budget are future work.
- Automatic Blender-scene import favours immediate playability and may create
  more detailed collision than a shipping map needs. Authors can replace it
  with simpler collision proxies for performance and cleaner contact.
- DXR mirrors require compatible D3D12 ray-tracing hardware. Raster fallback
  behavior is intentionally limited.
- Map changes restart the process so static collision, grind, door, renderer,
  and physics resources are rebuilt coherently.
- The layer activates once normal Skate 3 gameplay reaches a stable local
  player. Frontend and loading screens still use the upstream runtime.
- No retail game files or retail maps are bundled. The included Feature Park
  is original project content. A legally obtained Skate 3 ISO is required on
  first start.
- Internet multiplayer is an App 480 development preview. Its first launch
  needs GitHub access to acquire the pinned Steam runtime and a running,
  signed-in Steam client. If setup fails, inspect
  `.cel-steam/bootstrap.log`; ordinary gameplay and the localhost multiplayer
  fallback remain available.
- Remote skeletal animation can retain a small, rapid movement jitter even
  when packet delivery is stable. Root position, rotation, and independent
  player animation are functional, but this presentation defect remains open.
- Remote skateboard wheels can remain slightly misaligned or deform during
  skating. Their final procedural transforms are replicated independently
  from the canonical body skeleton; the major wheel/hat separation is fixed,
  but this smaller wheel presentation defect remains open.

When reporting a problem, include the map name, GPU, driver version, selected
Graphics API and Renderer, framerate cap, controller backend, and the latest
`logs/skate3_*.log`, but never upload retail game data.
