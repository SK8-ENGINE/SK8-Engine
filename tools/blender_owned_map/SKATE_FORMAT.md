# SKATE v8 binary format

All integers and IEEE-754 floats are little-endian. Strings are a `u32` byte
length followed by UTF-8 bytes. Coordinates are right-handed Y-up metres.

```text
char[8] magic = "SKATE08\0"
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
  u32 skate_physics_surface      # 0..12
  u32 skate_surface_pattern      # 0..15

texture[texture_count]:
  string name
  u32 width, height
  u32 color_space       # 0 linear, 1 sRGB metadata
  u32 rgba8_byte_count
  u8 rgba8[rgba8_byte_count]

vertex[vertex_count]:
  f32 position[3], normal[3]
  f32 uv0[2], lightmap_uv[2]
  u32 material_id

u32 index[index_count]

collision[collision_triangle_count]:
  f32 a[3], b[3], c[3]
  u32 surface_id, material_id

grind_rail[grind_rail_count]:
  string name
  u32 closed
  u32 point_count
  f32 point[point_count][3]

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
```

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

The loader retains read compatibility with SKATE v1 through v8. Missing v2
material fields use opaque, polished-concrete/smooth defaults with no
additional PBR maps. Missing v3 cycle fields retain the original full-day
behavior. Missing v6 environment fields use the engine's neutral sky grading
and standard twilight, night, sun, moon, and ambient defaults. Older packages
simply contain no authored local lights or NPC routes. NPC routes in v8 are
experimental runtime data and are not yet a stable gameplay feature.

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
