# Map plan format

Write UTF-8 JSON using this structure:

```json
{
  "version": 3,
  "map_name": "My Map",
  "materials": [
    {
      "material": "Concrete_Wall",
      "audio_surface": 4,
      "physics_surface": 2,
      "surface_pattern": 0,
      "collision_enabled": true
    }
  ],
  "object_group_roles": [
    {"group": "mesh_2e13c82c53368df87ca0", "role": "DEFAULT"},
    {"group": "mesh_48dafdb1688b62db0a89", "role": "COLLISION_ONLY"},
    {"group": "mesh_734c972cbf34e20daae1", "role": "VISUAL_ONLY"},
    {"group": "mesh_e18ed98f1669783ca845", "role": "IGNORE"}
  ],
  "object_roles": [],
  "lights": [
    {
      "source_group": "mesh_a5713cae44cf0e5a84b4",
      "type": "POINT",
      "color": [1.0, 0.72, 0.42],
      "energy": 900.0,
      "range": 14.0,
      "offset": [0.0, 0.0, -0.15]
    }
  ],
  "spawn": {
    "location": [12.0, -4.5, 0.15],
    "heading_degrees": 0.0
  },
  "grinds": {
    "enabled": true,
    "minimum_segment_length": 0.35,
    "minimum_chain_length": 0.8,
    "minimum_corner_angle_degrees": 8.0,
    "maximum_slope_degrees": 65.0,
    "deduplicate_distance": 0.08,
    "join_distance": 0.04,
    "density_cell_size": 2.0,
    "maximum_splines_per_cell": 4,
    "maximum_splines_per_source": 128
  }
}
```

Grinds are generated only from meshes assigned `DEFAULT` or
`COLLISION_ONLY`. The density settings are universal anti-spam limits: the
longest useful chains win when imported detail produces many nearby sharp
edges. Keep the defaults for large imported maps unless a human deliberately
wants denser coverage. Set a limit to `0` only to disable that limit.

## Materials

Create one entry for every material listed under `used_materials` in the
grouped inventory. IDs must come from `MATERIALS.md`.

`collision_enabled` normally stays true. Set it true for materials used by
`DEFAULT` or `COLLISION_ONLY` objects. Set it false only when every use is
visual-only or ignored. Group roles and explicit overrides, not material
names, decide whether mesh geometry enters collision export.

## Lights

Supported types are `POINT`, `SPOT`, and `AREA`. If `location` is omitted,
the light is placed at the center of `source_object`, plus `offset`.

Use `source_group` to create one light at every member of a repeated fixture
group. Use `source_object` for one exact fixture. Do not include both.

For a spot or area light, include `target` to aim it:

```json
{
  "source_object": "Floodlight_A",
  "type": "SPOT",
  "target": [10.0, 4.0, 0.0],
  "energy": 1200.0,
  "range": 20.0
}
```

Use `location` directly when the source object's center is not a useful light
position. Do not add a light when a suitable Blender light already exists.

## Spawn

Agents must provide an explicit world-space `location` chosen after viewing
all generated scene previews and cross-referencing the grouped inventory.
Place Z slightly above the visible floor. Choose an open, accessible riding
surface away from walls, roofs, voids, props, and obvious hazards.

`source_object` and `source_group` remain accepted for old or human-authored
plans, but agents must not use them for automatic spawn placement. Their
bounding-box centers may be empty space for concave or disconnected meshes.
Human adjustment afterward is expected.

## Mesh group roles

Version 3 requires exactly one `object_group_roles` entry for every entry in
`mesh_groups`. Use the opaque group ID exactly as written:

```json
[
  {"group": "mesh_2e13c82c53368df87ca0", "role": "DEFAULT"},
  {"group": "mesh_48dafdb1688b62db0a89", "role": "COLLISION_ONLY"},
  {"group": "mesh_734c972cbf34e20daae1", "role": "VISUAL_ONLY"},
  {"group": "mesh_e18ed98f1669783ca845", "role": "IGNORE"}
]
```

Mesh roles:

- `DEFAULT`: presentation and collision.
- `COLLISION_ONLY`: collision without presentation.
- `VISUAL_ONLY`: presentation without collision.
- `IGNORE`: remove the object from SKATE export groups while preserving its
  ordinary Blender collections.

`object_roles` is optional in version 3. Use an exact mesh entry only to
override its group role for a real instance-specific exception. Exact curve
entries may use `GRIND`, `PATH`, or `IGNORE`. Do not expand every group member
into this list; Blender does that mechanically during application.

Do not read `scene_inventory_members.json` merely to write these decisions.
It is a script-only exact-name sidecar. If the source scene changes between
inspection and application, inspect it again and regenerate the plan rather
than trying to reuse stale group IDs.

`current_skate_roles` records explicit SKATE groups already present in the
source scene. Preserve a single existing role unless there is a clear reason
to override it. `rigid_body_hint` reports Blender simulation metadata only;
it can support a decision but is not authoritative for `.skate` export.

Versions 1 and 2 remain accepted for compatibility, but agents must write
version 3 from grouped inventories.
