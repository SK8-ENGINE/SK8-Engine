# SKATE v13 binary format

All integers and IEEE-754 floats are little-endian. Strings are a `u32` byte
length followed by UTF-8 bytes. Coordinates are right-handed Y-up metres.

```text
char[8] magic = "SKATE13\0"
u32 endian_marker = 0x12345678
string map_name
f32 spawn_position[3]
f32 spawn_heading_radians
f32 sky_zenith[3], sky_horizon[3], sky_nadir[3]
f32 day_night_duration_seconds
f32 day_night_start_hour
f32 day_night_orbit_azimuth
f32 day_night_end_hour
f32 day_night_ping_pong        # 0.0 false, 1.0 true
f32 twilight_zenith[3], twilight_horizon[3], twilight_nadir[3]
f32 night_zenith[3], night_horizon[3], night_nadir[3]
f32 sun_color[3], moon_color[3]
f32 sun_intensity, moon_intensity
f32 day_ambient, night_ambient
f32 sky_tint_rgb[3]          # white (1,1,1) preserves authored palettes
u32 material_count, texture_count, vertex_count
u32 index_count, collision_triangle_count, grind_rail_count
u32 hinged_door_count
u32 local_light_count
u32 npc_route_count

material[material_count]:
  string name
  u32 surface_flags
  f32 friction, restitution
  f32 display_color[3]
  f32 roughness, emissive_intensity
  u32 albedo_texture_id, indirect_lightmap_texture_id
  f32 baked_indirect_strength
  u32 normal_texture_id
  u32 orm_texture_id             # R=AO, G=roughness, B=metallic
  u32 emissive_texture_id
  u32 alpha_mode                 # 0 opaque, 1 mask, 2 blend
  f32 alpha_cutoff
  u32 skate_audio_surface        # 0..93
  u32 skate_physics_surface      # 0..13 (13 appears in retail collision)
  u32 skate_surface_pattern      # 0..15
  u32 presentation_depth_layer   # 0 world, 1 cutout, 2 decal/sign, 3 blend
  u32 has_retail_definition
  if has_retail_definition:
    u64 retail_material_guid
    u32 retail_material_handle
    i32 retail_material_group_index
    string retail_shader_name
    u32 retail_shader_family
    u32 retail_render_flags
    u32 texture_binding_count
    texture_binding[texture_binding_count]:
      string semantic
      u32 texture_id
      u32 uv_set                 # 0 base, 1 lightmap, 2 decal
      u32 address_u, address_v   # 0 wrap, 1 clamp
    u32 parameter_count
    parameter[parameter_count]:
      string name
      u32 value_count
      string value[value_count]
    string source_metadata_json

texture[texture_count]:
  string name
  u32 width, height
  u32 color_space       # 0 linear, 1 sRGB metadata
  stored_bytes rgba8    # decoded size is width * height * 4

stored_bytes visual_vertices:
  vertex[vertex_count]:
    f32 position[3], normal[3]
    f32 uv0[2], lightmap_uv[2]
    u32 material_id
    f32 decal_uv[2]
    i8 tangent_binormal_snorm8[3]
    i8 tangent_handedness_snorm8

stored_bytes visual_indices:
  u32 index[index_count]

stored_bytes collision:
  collision_triangle[collision_triangle_count]:
    f32 a[3], b[3], c[3]
    u32 surface_id, material_id
    u8 native_edge_code[3]
    u8 has_native_edge_codes

grind_rail[grind_rail_count]:
  string name
  u32 closed
  u32 representation             # 0 authored points, 1 retail native cubic
  if representation == 0:
    u32 point_count
    f32 point[point_count][3]
  if representation == 1:
    u64 retail_spline_id
    u64 retail_type_signature
    u32 retail_flags
    u32 retail_trailing_word
    u32 segment_count
    u32 native_segment[segment_count][30]

hinged_door[hinged_door_count]:
  string name
  f32 hinge_position[3]
  f32 hinge_axis[3]
  f32 closed_width_axis[3]
  f32 closed_depth_axis[3]
  f32 local_min[3], local_max[3]
  f32 minimum_angle_radians, maximum_angle_radians
  f32 initial_angle_radians
  f32 mass, angular_damping
  f32 return_spring_strength, maximum_angular_speed
  f32 contact_impulse_scale
  f32 friction, restitution
  u32 surface_id
  u32 vertex_count, index_count, collision_triangle_count
  vertex[vertex_count]                    # hinge-local vertex layout above
  u32 index[index_count]
  collision[collision_triangle_count]    # hinge-local collision layout above

local_light[local_light_count]:
  string name
  u32 type                     # 0 point, 1 spot, 2 area
  f32 position[3]
  f32 direction[3]             # normalized emission direction
  f32 color[3]                 # linear RGB
  f32 intensity
  f32 influence_radius
  f32 source_radius
  f32 spot_inner_cosine
  f32 spot_outer_cosine

npc_route[npc_route_count]:
  string name
  u32 closed
  u32 skater_count
  f32 target_speed_metres_per_second
  f32 spawn_spacing_metres
  u32 point_count
  f32 point[point_count][3]

u32 extension_count
extension[extension_count]:
  char tag[4]
  u32 schema_version
  stored_bytes payload
```

Each `stored_bytes` record is:

```text
u32 storage_method       # 0 raw, 1 zlib-wrapped DEFLATE
u32 stored_byte_count
u8 payload[stored_byte_count]
```

The decoded byte count is inferred from the corresponding dimensions or
record count and checked before allocation. The exporter uses raw storage
only when DEFLATE would not reduce a texture. Visual vertices, indices, and
collision are emitted as bounded DEFLATE blocks. Compression is lossless:
the runtime reconstructs the same float32/u32 records and RGBA8 texels before
normal validation and renderer upload.

Visual vertices are indexed by their complete record. Two corners share a
vertex only when position, normal, all three UV channels, material ID, and the
packed tangent frame are bit-identical. UV seams, hard normals, material
boundaries, tangent handedness, triangle order, and all index references are
therefore preserved. The signed-normalized tangent frame matches the precision
of the retail packed tangent data while avoiding twelve redundant float bytes
per vertex.

For ordinary authored meshes the frame is generated from Blender's `UVMap`.
Retail extraction can instead provide the complete validated point-attribute
set `skate3_retail_normal`, `skate3_retail_tangent`, and
`skate3_retail_tangent_handedness`. The exporter reconstructs
`cross(normal, tangent) * handedness` because the compact SKATE vertex record
stores a binormal for the engine loader. Existing extracted scenes may expose
the tangent under the legacy `skate3_retail_binormal` name; that name is read
only as a compatibility path. Partial or malformed retail frame metadata is
an export error rather than a silent fallback.

Retail material definitions are additive to the renderer-neutral material
fields, so authored maps remain simple while extracted maps can retain their
original shader identity. Texture bindings are named rather than limited to a
fixed PBR slot list; University currently uses diffuse, transparent, normal,
normal2, specular, lightmap, detail, macrooverlay, decal, environment, and
noise. Parameter values remain strings because retail Attribulator data
contains numbers, texture resource names, empty markers, and other
shader-specific tokens.

The `WMET` extension uses schema version 1 and contains the losslessly
DEFLATE-compressed extraction manifest JSON. It preserves source archives,
stream cells, RX2 declarations and offsets, bounds, simulation/collision/grind
provenance, and texture decode metadata that do not belong in hot render
records. Unknown extension tags are safely skipped after their stored payload
has been validated.

The `MOBJ` extension uses schema version 2 and preserves independently
editable Blender mesh objects without changing the SKATE12 base layout. Its
decoded payload is:

```text
u32 object_count
object[object_count]:
  u32 stable_id                 # FNV-1a of the full Blender object name
  string name
  vec3 origin                  # authored map-space translation
  u32 first_render_index
  u32 render_index_count
  u32 first_collision_triangle
  u32 collision_triangle_count
  u32 grind_rail_count
  u32 grind_rail_indices[grind_rail_count]
```

Render and collision ranges refer to the unchanged flattened base arrays.
The runtime extracts each range into object-local geometry by subtracting the
same `origin`, then excludes those ranges from the immutable static render and
collision worlds. A session transform therefore drives both the per-object
draw and its native collision aggregate. Objects linked to both `OW_VISUAL`
and `OW_COLLISION` retain collision ownership; render-only objects keep a
zero collision count. Grind indices associate parented Blender grind curves
with the same runtime object pose. Schema-1 MOBJ records and older SKATE12
packages with no `MOBJ` extension continue to load unchanged.

`day_night_duration_seconds == 0` freezes celestial lighting at
`day_night_start_hour`. With a positive duration and
`day_night_ping_pong == 0`, the duration describes one complete 24-hour
cycle. With `day_night_ping_pong == 1`, the duration describes one complete
cosine-eased trip from `day_night_start_hour` to `day_night_end_hour` and
back. This allows a map to animate over a restricted daylight range without
entering night.

IDs are one-based; zero means “not present” for texture references.

SKATE v2 and v3 albedo bytes are scene-linear UNORM8. Textures referenced as
`indirect_lightmap_texture_id` use `sqrt(linear / 4)` in RGB, retaining
low-energy indirect detail and headroom through a compact UNORM8 upload.
Shaders decode those channels as `encoded * encoded * 4`.

Retail Skate 3 pages already store the console light quantity consumed as
`encoded * encoded`. The Blender addon marks those source images with
`ow_lightmap_encoding = skate3_retail_sqrt_linear_over_4`, preserves their
bytes, and multiplies the exported `baked_indirect_strength` by `0.25`.
This reuses the package's common shader decode without changing the format or
quietly rescaling authored Blender bakes.

Door geometry uses an orthonormal hinge-local frame. Local X crosses the
closed leaf, local Y follows `hinge_axis`, and local Z is leaf thickness.
The runtime rotates the complete visual/collision body around the hinge from
contact impulses; the package contains no trigger or opening animation.

Normal Blender Point, Spot, and Area objects become `local_light` records
without addon-specific tagging. A Blender Sun controls the map's directional
sun colour, intensity, and horizontal orbit direction and is not duplicated
as a local light. Packages have no authored-light count ceiling; the raster
renderer dynamically selects the lights relevant to the current view.

NPC routes provide navigation intent to Skate 3's native AI skater
controller. They do not contain scripted transforms: native steering, board
physics, collision, animation, tricks, and bails remain authoritative.

Retail native grind segments preserve the first 120 bytes of each Pegasus
`tSplineData` segment as 30 exact IEEE-754 word patterns. Their polynomial is
`D + C*t + B*t^2 + A*t^3`; the runtime translates only D and the native
bounds, regenerates parent/previous/next guest links, and leaves coefficients
and auxiliary values unchanged. Blender Bezier handles are a review/edit
representation. Export rejects a retail curve if its controls no longer match
the retained native payload, preventing an edit from silently emitting stale
grind data.

Retail collision imports set `has_native_edge_codes` and retain the exact
RenderWare edge/corner feature bytes decoded from the source `ClusteredMesh`.
The native collision compiler emits those bytes unchanged. Blender-authored
collision leaves the marker clear, and the compiler derives adjacency,
edge-angle, and smooth-vertex codes from geometry as before.

Every SKATE material participates in runtime directional and local lighting.
An indirect lightmap is an additive static-bounce term; it never changes the
material into a baked-only or unlit surface. The runtime's Dynamic Lighting
setting is the one global presentation switch for that live contribution.

`presentation_depth_layer` resolves intentionally coplanar authored detail
without changing geometry. The renderer applies a tiny projection-space depth
preference to layers 1 through 3, so signs and decals remain at their exact
authored world transforms and collision is unaffected. A deterministic
millimetre-scale material tie order resolves overlapping LOD/sign materials
inside the same layer. Runtime world compilation also removes exact
same-winding duplicate presentation faces while retaining reverse-wound
partners used for intentional two-sided foliage. Exporters infer a default
from alpha mode and common sign/decal material names, while `ow_depth_layer`
can explicitly select 0 through 3.

The loader retains read compatibility with SKATE v1 through v13. Missing v2
material fields use opaque, polished-concrete/smooth defaults with no
additional PBR maps. Missing v3 cycle fields retain the original full-day
behavior. Missing v6 environment fields use the engine's neutral sky grading
and standard twilight, night, sun, moon, and ambient defaults. Older packages
simply contain no authored local lights or NPC routes. Packages before v12 use
the base UV as decal UV and have no tangent frame, retail material definition,
or extension table. Packages before v13 infer their presentation depth layer
from alpha mode and material name. NPC routes in v8 are experimental runtime
data and are not yet a stable gameplay feature.

## Version compatibility

Every incompatible layout change increments the two-digit version in the
eight-byte magic. The runtime keeps explicit readers for older versions and
fills newly introduced fields with documented defaults. A runtime that sees
a package version newer than it supports must reject it with an update
requirement; it must never guess at the newer byte layout.

Accordingly, newer runtimes are backward compatible with older packages.
Older runtimes are not forward compatible with newer package features.
Authors should keep the editable `.blend` source so a map can be re-exported
with a future addon when required.
