---
name: sk8-auto-map
description: Automatically prepare large Blender maps for SK8 Engine using duplicate-collapsed object groups, explicit collision and presentation roles, Skate 3 gameplay materials, fixture lights, broad deduplicated grind splines, and a spawn.
---

# SK8 Auto Map

Use this workflow when a user wants an ordinary or imported Blender scene
prepared automatically for `.skate` export. Prefer broad useful coverage and
leave fine cleanup to the user.

## Workflow

1. Run `scripts/Inspect-AutoMap.ps1` on the source `.blend`. It writes:
   - `scene_inventory.json`, the duplicate-collapsed agent inventory;
   - `scene_inventory_members.json`, an exact-name sidecar for scripts.
   - `previews/`, four lightweight whole-map overview images and metadata.
2. Read `scene_inventory.json` and use an image-viewing tool to inspect every
   PNG in `previews/`. Do not load the membership sidecar into agent context
   during ordinary planning.
3. Read [references/MATERIALS.md](references/MATERIALS.md) and
   [references/PLAN_FORMAT.md](references/PLAN_FORMAT.md).
4. Write a version 3 `map_plan.json`:
   - assign every used Blender material a Skate 3 audio surface, physics
     behavior, and contact pattern;
   - assign every mesh group exactly one object role: `DEFAULT`,
     `COLLISION_ONLY`, `VISUAL_ONLY`, or `IGNORE`;
   - add lights for objects that are clearly lamps, bulbs, signs, or other
     emitting fixtures and do not already have a nearby Blender light; use
     `source_group` when every repeated fixture should receive a light;
   - use the overview images, object bounds, materials, and map context to
     choose an explicit spawn location and heading on a visible safe floor;
   - leave grind generation enabled unless the user explicitly disables it.
5. Run `scripts/Apply-AutoMap.ps1` to create a new prepared `.blend`.
6. Report the prepared file and any warnings. The user is expected to perform
   visual cleanup before release.

Do not edit the source `.blend` in place. Do not invent material IDs outside
the supplied catalog. Do not add lights to every emissive-looking material:
use object names, material names, dimensions, and location together.
Local fixture lights supplement the map's global sun and ambient lighting.
Never zero or rewrite world ambient merely because local lights exist.

Spawn placement is a visual agent decision. Inspect the top, south, east, and
isometric previews before choosing it. Cross-reference recognizable floor
geometry with the grouped inventory's world-space locations and bounds, then
write an explicit `[x, y, z]` location slightly above the floor. Do not use a
source object's or group's bounding-box center: concave and disconnected
meshes can have empty space at that point. Prefer an open, accessible riding
surface away from walls, roofs, voids, props, and obvious hazards. Human
cleanup remains expected.

Do not infer Blender texture roles from filenames and do not ask the agent to
choose albedo, normal, ORM, emissive, or lightmap images. Those are technical
Blender-to-SK8 inputs: the exporter follows supported Principled connections
and explicit Owned World Material image slots deterministically. It preserves
one standard two-image Mix, explicit UV Map and Mapping nodes, Image Texture
Repeat/Extend/Clip/Mirror, material backface culling, and supplies a box UV
fallback only when a mesh has no source UVs. Auto Prepare asks Blender to
flatten an otherwise unsupported Base Color graph across the complete 0–1
texture domain to a packed albedo. That bake evaluates colour independently
of metallic shading and supplies neutral-white temporary vertex-colour layers
for imported `TINT` inputs. Object Info Color remains preserved through
shared-texture material-tint variants. For transparent complex materials, the
same deterministic pipeline also bakes the effective Principled Alpha graph
into the generated albedo. A physical Glass BSDF receives the exporter's basic
tinted transparency fallback because the runtime cannot reproduce Blender
refraction. Both decisions come from the shader graph; never infer either from
object, material, or texture names.
If validation reports that this deterministic bake failed, or reports scalar
Bump height, an independently mapped PBR channel, or a missing explicit packed
ORM, leave it for deterministic Blender authoring rather than guessing. Never
have the agent repair UVs or shader graphs by interpreting image filenames.

The grouped inventory contains one representative record plus an instance
count and spatial range for each deterministic group. Blender groups exact
geometry with the same normalized name family, materials, world size,
collection families, visibility, current SKATE role, rigid-body hint, and
modifier shape. Applying a version 3 plan recomputes those groups from the
unchanged source scene and expands each decision to the exact objects.

Use exact `object_roles` only for a deliberate exception to a group decision
or for an individual curve. Never copy every group member into the plan. The
membership sidecar exists for deterministic scripts, validation, and targeted
human cleanup—not ordinary agent reasoning.

## Repository hygiene

When this workflow is used while developing SK8 Engine, every source or
prepared map and all of its assets, plans, inventories, exports, screenshots,
launchers, and logs are local test artifacts. Keep them under an ignored
`out/local-tools/` directory or outside the repository. Never stage or commit
them.

Use map failures to make universal exporter, renderer, validator, or workflow
improvements. Do not add exceptions keyed to a tested map's filename, object
names, material names, coordinates, or assets. Convert a discovered failure
into a minimal synthetic regression test that contains no imported-map
content. Treat Blender as the visual authority when diagnosing appearance
parity.

Collision is an explicit SKATE export decision. Blender rigid-body settings,
hidden state, collection names, and imported collider-style names are useful
hints, but none is a universal collision flag across OBJ, FBX, or glTF.
Choose roles using the object's name, materials, dimensions, visibility,
collections, location, and current SKATE role together:

- `DEFAULT`: visible solid geometry the skater should touch, stand on, or ride.
  This is the conservative choice for ambiguous ordinary map geometry.
- `COLLISION_ONLY`: a proxy mesh meant to collide but not render.
- `VISUAL_ONLY`: foliage cards, decals, effects, distant backdrops, or other
  decoration the skater should pass through.
- `IGNORE`: helpers, references, unwanted duplicate LODs, and objects that
  should not enter the SKATE export.

Preserve a single `current_skate_roles` value unless there is a clear reason
to change it. A role selected in the plan overrides automatic adoption. Names
alone must not decide collision. Human cleanup remains expected.

Do not guess transparency from an albedo image's unused alpha channel.
Imported textures often pack blend, tint, or material masks there. Auto
Prepare follows the Blender shader's effective Principled Alpha input:
disconnected Alpha remains opaque, while explicitly connected image alpha is
classified as cutout or blend presentation. Preserve that deterministic
result; only override it when the source Blender material or the user clearly
requires a different alpha mode.

Set a material's `collision_enabled` true when it is used by `DEFAULT` or
`COLLISION_ONLY` geometry. Set it false only when all uses are non-colliding.
Expanded group roles and exact overrides determine whether geometry is
exported for collision; the material setting determines whether that material
is available to collision geometry and which Skate 3 contact behavior it
carries.

The grind script intentionally creates broad coverage from exposed mesh
corners and boundaries, joins connected edges, and removes near-duplicates.
It considers only collision-bearing `DEFAULT` and `COLLISION_ONLY` meshes.
Universal per-cell and per-source density limits retain the longest nearby
chains so detailed imports do not create thousands of glitchy splines in one
spot.
Do not manually reason over individual grind edges unless the user asks for
targeted cleanup.
