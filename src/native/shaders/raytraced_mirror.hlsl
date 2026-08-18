// Project-owned hardware-ray-traced planar mirror.
//
// One inline DXR ray is generated only for screen pixels whose primary view
// ray crosses the authored mirror rectangle. Static owned-world triangles are
// traced through a BLAS/TLAS, then a second any-hit query supplies hard sun
// visibility. Presentation composites this texture by rasterizing the real
// mirror plane against the main scene depth, so foreground actors still
// occlude it correctly.

cbuffer MirrorConstants : register(b0) {
  row_major float4x4 inverse_view_projection;
  float4 camera_max_distance;
  float4 mirror_center_half_width;
  float4 mirror_right_half_height;
  float4 mirror_up_unused;
  float4 sky_zenith;
  float4 sky_horizon;
  float4 sun_direction_ambient;
  float4 sun_color_intensity;
};

RaytracingAccelerationStructure world_scene : register(t0);
ByteAddressBuffer world_vertices : register(t1);
ByteAddressBuffer world_indices : register(t2);
ByteAddressBuffer triangle_materials : register(t3);
ByteAddressBuffer dynamic_vertices : register(t4);
ByteAddressBuffer dynamic_indices : register(t5);
ByteAddressBuffer dynamic_materials : register(t6);
Texture2D<float4> dynamic_diffuse_textures[64] : register(t7);
SamplerState dynamic_texture_sampler : register(s0);
RWTexture2D<float4> mirror_output : register(u0);

float3 SkyColor(float3 direction) {
  const float height = saturate(direction.y * 0.5 + 0.5);
  const float zenith_weight = smoothstep(0.18, 0.95, height);
  float3 color = lerp(sky_horizon.rgb, sky_zenith.rgb, zenith_weight);
  const float sun = pow(
      saturate(dot(direction, normalize(sun_direction_ambient.xyz))),
      1400.0);
  return color + sun_color_intensity.rgb *
                     sun_color_intensity.w * sun * 7.0;
}

float3 PassGamma(float3 color) {
  // The owned map shader writes final gamma-authored lighting through the
  // inverse of Skate 3's host tone curve when HDR is enabled. The mirror is
  // composited into that same pre-tonemap float target, so it must use the
  // same encoding instead of feeding final-looking values into the curve a
  // second time (the previous fullbright reflection).
  const float3 tm =
      max(color, 0.0) * max(color, 0.0) *
      (2.0 / (1.41 * 1.41));
  const float3 low =
      1.0 - sqrt(saturate(1.0 - tm));
  const float3 high = 4.0 * tm - 3.0;
  return lerp(low, high, step(1.0, tm));
}

float Hash21(float2 p) {
  p = frac(p * float2(123.34, 456.21));
  p += dot(p, p + 45.32);
  return frac(p.x * p.y);
}

float2 SurfaceUv(float3 position, float3 normal, float scale) {
  const float3 axis = abs(normal);
  const float2 uv =
      axis.y >= axis.x && axis.y >= axis.z
          ? position.xz
          : (axis.z >= axis.x ? position.xy : position.zy);
  return uv * max(scale, 0.01);
}

float GridLine(float value, float width) {
  const float edge =
      min(frac(value), 1.0 - frac(value));
  return 1.0 - smoothstep(width, width + 0.006, edge);
}

float3 MaterialAlbedo(float3 base, float2 uv,
                      int pattern, float variation) {
  const float2 cell = floor(uv);
  const float noise = Hash21(cell) * 2.0 - 1.0;
  const float fine =
      Hash21(floor(uv * 7.0)) * 2.0 - 1.0;
  float shade =
      1.0 + variation * (noise * 0.45 + fine * 0.20);
  if (pattern == 1) {
    const float joints =
        max(GridLine(uv.x * 0.22, 0.010),
            GridLine(uv.y * 0.22, 0.010));
    shade = shade * (1.0 - joints * 0.28) +
            fine * variation * 0.10;
  } else if (pattern == 2) {
    const float speck =
        step(0.89, Hash21(floor(uv * 11.0)));
    shade =
        0.90 + fine * variation * 0.32 + speck * 0.13;
  } else if (pattern == 3) {
    const float row = floor(uv.y);
    const float2 brick_uv =
        float2(uv.x + fmod(abs(row), 2.0) * 0.5, uv.y);
    const float mortar =
        max(GridLine(brick_uv.x, 0.055),
            GridLine(brick_uv.y, 0.075));
    const float brick_noise =
        Hash21(floor(brick_uv)) * 2.0 - 1.0;
    const float3 brick =
        base * (0.88 + brick_noise * variation);
    const float3 grout =
        lerp(base, float3(0.52, 0.49, 0.43), 0.72);
    return lerp(brick, grout, mortar);
  } else if (pattern == 4) {
    const float seam =
        max(GridLine(uv.x * 0.28, 0.012),
            GridLine(uv.y * 0.12, 0.008));
    shade = 1.0 + noise * variation * 0.35 +
            sin(uv.y * 38.0) * 0.018 - seam * 0.32;
  } else if (pattern == 5) {
    const float plank = GridLine(uv.y * 0.32, 0.018);
    const float grain =
        sin(uv.x * 7.0 + sin(uv.y * 1.7) * 2.4) * 0.5 +
        sin(uv.x * 19.0) * 0.18;
    shade =
        0.94 + grain * variation * 0.34 - plank * 0.30;
  } else if (pattern == 6) {
    const float grout =
        max(GridLine(uv.x, 0.020),
            GridLine(uv.y, 0.020));
    shade =
        1.0 + noise * variation * 0.22 - grout * 0.24;
  } else if (pattern == 7) {
    const float blades =
        sin(uv.x * 15.0 + sin(uv.y * 8.0)) *
        sin(uv.y * 17.0);
    shade =
        0.82 + fine * variation * 0.50 + blades * 0.08;
  } else if (pattern == 8) {
    const float chip =
        step(0.965, Hash21(floor(uv * 5.0)));
    shade =
        0.98 + fine * variation * 0.16 - chip * 0.22;
  }
  return max(base * shade, 0.0);
}

float3 LoadNormal(uint vertex_index, bool dynamic_hit) {
  if (dynamic_hit) {
    return asfloat(
        dynamic_vertices.Load3(vertex_index * 32 + 12));
  }
  return asfloat(
      world_vertices.Load3(vertex_index * 24 + 12));
}

float2 LoadUv(uint vertex_index) {
  return asfloat(
      dynamic_vertices.Load2(vertex_index * 32 + 24));
}

uint3 LoadIndices(uint primitive, bool dynamic_hit) {
  if (dynamic_hit) {
    return dynamic_indices.Load3(primitive * 12);
  }
  return world_indices.Load3(primitive * 12);
}

float4 LoadMaterial(uint primitive, bool dynamic_hit) {
  if (dynamic_hit) {
    const uint material_offset =
        dynamic_materials.Load(0);
    const uint material_stride =
        dynamic_materials.Load(4);
    return asfloat(
        dynamic_materials.Load4(
            material_offset + primitive * material_stride));
  }
  return asfloat(
      triangle_materials.Load4(primitive * 32));
}

float4 LoadMaterialDetail(uint primitive, bool dynamic_hit) {
  if (dynamic_hit) {
    const uint material_offset =
        dynamic_materials.Load(0);
    const uint material_stride =
        dynamic_materials.Load(4);
    return asfloat(
        dynamic_materials.Load4(
            material_offset + primitive * material_stride + 16));
  }
  return asfloat(
      triangle_materials.Load4(primitive * 32 + 16));
}

float4 LoadCharacterLighting(uint lighting_index, uint row) {
  const uint lighting_offset =
      dynamic_materials.Load(8);
  const uint lighting_stride =
      dynamic_materials.Load(12);
  return asfloat(dynamic_materials.Load4(
      lighting_offset + lighting_index * lighting_stride +
      row * 16));
}

float4 LoadMovingLightPosition(uint light_index) {
  const uint offset = dynamic_materials.Load(16);
  const uint stride = dynamic_materials.Load(20);
  return asfloat(dynamic_materials.Load4(
      offset + light_index * stride));
}

float4 LoadMovingLightColor(uint light_index) {
  const uint offset = dynamic_materials.Load(16);
  const uint stride = dynamic_materials.Load(20);
  return asfloat(dynamic_materials.Load4(
      offset + light_index * stride + 16));
}

float LoadMovingLightRadius(uint light_index) {
  const uint offset = dynamic_materials.Load(16);
  const uint stride = dynamic_materials.Load(20);
  return asfloat(dynamic_materials.Load(
      offset + light_index * stride + 32));
}

float MovingLightVisibility(
    float3 position, float3 normal, float3 light_position,
    float source_radius) {
  static const float3 samples[4] = {
      float3(0.577, 0.577, 0.577),
      float3(-0.577, -0.577, 0.577),
      float3(-0.577, 0.577, -0.577),
      float3(0.577, -0.577, -0.577)};
  float visibility = 0.0;
  [unroll] for (uint sample_index = 0;
                sample_index < 4; ++sample_index) {
    const float3 sample_position =
        light_position + samples[sample_index] * source_radius;
    const float3 delta = sample_position - position;
    const float distance_to_sample = length(delta);
    RayDesc shadow_ray;
    shadow_ray.Origin = position + normal * 0.025;
    shadow_ray.Direction = delta / max(distance_to_sample, 0.001);
    shadow_ray.TMin = 0.01;
    shadow_ray.TMax =
        max(distance_to_sample - source_radius * 0.35, 0.011);
    RayQuery<RAY_FLAG_FORCE_OPAQUE |
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH>
        shadow_query;
    shadow_query.TraceRayInline(
        world_scene, RAY_FLAG_NONE, 0xff, shadow_ray);
    while (shadow_query.Proceed()) {
    }
    visibility +=
        shadow_query.CommittedStatus() == COMMITTED_NOTHING
            ? 0.25
            : 0.0;
  }
  return visibility;
}

float3 MovingLightContribution(
    float3 position, float3 normal, float3 view_direction,
    float3 albedo, float roughness) {
  float3 total = 0.0;
  const uint light_count =
      min((uint)(sky_zenith.w + 0.5), 4u);
  [loop] for (uint light_index = 0;
              light_index < light_count; ++light_index) {
    const float4 light_position =
        LoadMovingLightPosition(light_index);
    const float4 light_color =
        LoadMovingLightColor(light_index);
    const float source_radius =
        LoadMovingLightRadius(light_index);
    const float3 delta = light_position.xyz - position;
    const float distance_squared =
        max(dot(delta, delta), 0.04);
    const float distance_to_light = sqrt(distance_squared);
    const float3 direction = delta / distance_to_light;
    const float ndotl = saturate(dot(normal, direction));
    const float range_fade = saturate(
        1.0 - distance_squared /
                  max(light_position.w * light_position.w, 0.01));
    const float attenuation =
        range_fade * range_fade /
        max(1.0, distance_squared * 0.22);
    const float visibility = MovingLightVisibility(
        position, normal, light_position.xyz, source_radius);
    const float3 halfway =
        normalize(direction + view_direction);
    const float specular =
        pow(saturate(dot(normal, halfway)),
            lerp(96.0, 10.0, saturate(roughness))) *
        ndotl * lerp(0.34, 0.055, saturate(roughness));
    total += light_color.rgb * light_color.w *
             attenuation * visibility *
             (albedo * ndotl + specular);
  }
  return total;
}

float SunVisibility(float3 position, float3 normal,
                    float3 light_direction) {
  const float3 reference =
      abs(light_direction.y) < 0.95
          ? float3(0.0, 1.0, 0.0)
          : float3(1.0, 0.0, 0.0);
  const float3 tangent =
      normalize(cross(reference, light_direction));
  const float3 bitangent =
      normalize(cross(light_direction, tangent));
  static const float2 disk[4] = {
      float2(-0.65, -0.28), float2(0.48, -0.57),
      float2(-0.24, 0.69), float2(0.71, 0.31)};
  float visibility = 0.0;
  [unroll] for (uint sample_index = 0;
                sample_index < 4; ++sample_index) {
    const float3 direction = normalize(
        light_direction +
        tangent * disk[sample_index].x * 0.010 +
        bitangent * disk[sample_index].y * 0.010);
    RayDesc shadow_ray;
    shadow_ray.Origin = position + normal * 0.035;
    shadow_ray.Direction = direction;
    shadow_ray.TMin = 0.01;
    shadow_ray.TMax = 600.0;
    RayQuery<RAY_FLAG_FORCE_OPAQUE |
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH>
        shadow_query;
    shadow_query.TraceRayInline(
        world_scene, RAY_FLAG_NONE, 0xff, shadow_ray);
    while (shadow_query.Proceed()) {
    }
    visibility +=
        shadow_query.CommittedStatus() == COMMITTED_NOTHING
            ? 0.25
            : 0.0;
  }
  return visibility;
}

float3 ShadeCharacter(
    float3 albedo, float3 normal, float3 view_direction,
    float visibility, uint lighting_index,
    uint family) {
  const float4 ch_tint_a =
      LoadCharacterLighting(lighting_index, 12);
  const float4 ch_tint_b =
      LoadCharacterLighting(lighting_index, 13);
  float3 dlinear = albedo * albedo;
  if (family <= 2) {
    if (ch_tint_a.w > 0.0) {
      dlinear *= ch_tint_a.rgb;
    }
  } else if (family <= 3) {
    dlinear =
        dlinear.g > 0.001225
            ? dlinear
            : ch_tint_a.rgb * dlinear.r +
                  ch_tint_b.rgb * dlinear.b;
  }
  const float3 light_direction =
      normalize(sun_direction_ambient.xyz);
  const float ndotl =
      saturate(dot(normal, light_direction));
  const float wrapped =
      saturate((dot(normal, light_direction) + 0.20) / 1.20);
  const float response =
      family >= 4
          ? lerp(wrapped * 0.42, ndotl, 0.58)
          : lerp(wrapped * 0.24, ndotl, 0.84);
  const float sky_amount =
      saturate(normal.y * 0.5 + 0.5);
  const float3 sky_fill =
      lerp(float3(0.16, 0.17, 0.19),
           float3(0.35, 0.47, 0.64), sky_amount);
  float3 ambient =
      sky_fill * sun_direction_ambient.w *
      lerp(0.82, 1.28, sky_amount) * 1.34;
  ambient +=
      float3(0.24, 0.20, 0.17) *
      sun_direction_ambient.w *
      lerp(0.42, 0.18, sky_amount);
  const float light_luma =
      dot(sun_color_intensity.rgb,
          float3(0.299, 0.587, 0.114));
  const float3 character_light_color =
      lerp(sun_color_intensity.rgb, light_luma.xxx, 0.16);
  const float3 direct =
      character_light_color *
      sun_color_intensity.w * response *
      lerp(0.16, 1.0, visibility);
  float3 result = dlinear * (ambient + direct);
  if (family <= 2 && ch_tint_b.w > 0.25) {
    result *= clamp(ch_tint_b.w, 0.8, 1.35);
  }
  return max(result, 0.0);
}

[numthreads(8, 8, 1)]
void cs_main(uint3 dispatch_id : SV_DispatchThreadID) {
  const uint2 pixel = dispatch_id.xy;
  uint width;
  uint height;
  mirror_output.GetDimensions(width, height);
  const uint2 dimensions = uint2(width, height);
  if (any(pixel >= dimensions)) {
    return;
  }

  const float2 uv =
      (float2(pixel) + 0.5) / float2(dimensions);
  const float2 ndc = float2(uv.x * 2.0 - 1.0,
                            1.0 - uv.y * 2.0);
  float4 far_h = mul(float4(ndc, 1.0, 1.0),
                     inverse_view_projection);
  far_h.xyz /= max(abs(far_h.w), 1.0e-6);
  const float3 camera = camera_max_distance.xyz;
  const float3 primary_direction =
      normalize(far_h.xyz - camera);

  const float3 center = mirror_center_half_width.xyz;
  const float half_width = mirror_center_half_width.w;
  const float3 right = normalize(mirror_right_half_height.xyz);
  const float half_height = mirror_right_half_height.w;
  const float3 up = normalize(mirror_up_unused.xyz);
  const float3 plane_normal = normalize(cross(right, up));
  const float denominator = dot(primary_direction, plane_normal);
  if (abs(denominator) < 1.0e-5) {
    mirror_output[pixel] = 0.0;
    return;
  }
  const float mirror_distance =
      dot(center - camera, plane_normal) / denominator;
  if (mirror_distance <= 0.0) {
    mirror_output[pixel] = 0.0;
    return;
  }
  const float3 mirror_point =
      camera + primary_direction * mirror_distance;
  const float3 mirror_offset = mirror_point - center;
  const float local_right = dot(mirror_offset, right);
  const float local_up = dot(mirror_offset, up);
  // The root signature is already at D3D12's 64-DWORD ceiling. Puddle kind
  // and ripple strength share mirror_up_unused.w, while reflectivity and
  // weather time occupy unused alpha/max-distance components.
  const bool puddle = mirror_up_unused.w > 0.5;
  float coverage = 1.0;
  if (!puddle &&
      (abs(local_right) > half_width ||
       abs(local_up) > half_height)) {
    mirror_output[pixel] = 0.0;
    return;
  }

  float3 normal = plane_normal;
  if (puddle) {
    const float2 puddle_uv =
        float2(local_right / half_width,
               local_up / half_height);
    const float radial = length(puddle_uv);
    const float edge_noise =
        sin(local_right * 2.73 + local_up * 1.91) * 0.035 +
        sin(local_right * 5.37 - local_up * 3.11) * 0.022 +
        sin(local_right * 9.17 + local_up * 6.43) * 0.012;
    const float edge = 0.94 + edge_noise;
    if (radial > edge) {
      mirror_output[pixel] = 0.0;
      return;
    }
    coverage = smoothstep(edge, edge - 0.10, radial);

    const float time = camera_max_distance.w;
    const float ripple = frac(mirror_up_unused.w);
    const float2 world_xz = mirror_point.xz;
    const float slope_right =
        cos(dot(world_xz, float2(2.1, 1.3)) + time * 5.3) *
            ripple +
        cos(dot(world_xz, float2(-3.7, 2.8)) - time * 4.1) *
            ripple * 0.55;
    const float slope_up =
        cos(dot(world_xz, float2(1.2, -2.9)) + time * 4.7) *
            ripple * 0.80 +
        cos(dot(world_xz, float2(4.4, 3.1)) - time * 6.2) *
            ripple * 0.40;
    normal = normalize(
        plane_normal + right * slope_right + up * slope_up);
  }

  const float3 reflected_direction =
      normalize(reflect(primary_direction, normal));
  RayDesc reflection_ray;
  reflection_ray.Origin =
      mirror_point + reflected_direction * 0.025;
  reflection_ray.Direction = reflected_direction;
  reflection_ray.TMin = 0.01;
  reflection_ray.TMax = 600.0;

  RayQuery<RAY_FLAG_FORCE_OPAQUE> reflection_query;
  reflection_query.TraceRayInline(
      world_scene, RAY_FLAG_NONE, 0xff, reflection_ray);
  while (reflection_query.Proceed()) {
  }

  float3 result;
  bool exact_character_output = false;
  if (reflection_query.CommittedStatus() ==
      COMMITTED_TRIANGLE_HIT) {
    const uint primitive =
        reflection_query.CommittedPrimitiveIndex();
    const bool dynamic_hit =
        reflection_query.CommittedInstanceID() != 0;
    const uint3 indices =
        LoadIndices(primitive, dynamic_hit);
    const float2 bary =
        reflection_query.CommittedTriangleBarycentrics();
    const float3 weights =
        float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    float3 hit_normal = normalize(
        LoadNormal(indices.x, dynamic_hit) * weights.x +
        LoadNormal(indices.y, dynamic_hit) * weights.y +
        LoadNormal(indices.z, dynamic_hit) * weights.z);
    if (!reflection_query.CommittedTriangleFrontFace()) {
      hit_normal = -hit_normal;
    }
    const float3 hit_position =
        reflection_ray.Origin +
        reflection_ray.Direction *
            reflection_query.CommittedRayT();
    const float4 material =
        LoadMaterial(primitive, dynamic_hit);
    const float4 material_detail =
        LoadMaterialDetail(primitive, dynamic_hit);
    const uint lighting_index =
        dynamic_hit ? asuint(material_detail.x) : 0xffffffff;
    const uint character_family =
        dynamic_hit ? asuint(material_detail.y) : 0;
    const uint texture_index =
        dynamic_hit ? asuint(material_detail.z) : 0xffffffff;
    const uint material_flags =
        dynamic_hit ? asuint(material_detail.w) : 0u;
    const bool exact_character =
        dynamic_hit && lighting_index != 0xffffffff &&
        character_family != 0;
    exact_character_output = exact_character;
    const float3 light_direction =
        normalize(sun_direction_ambient.xyz);
    const float visibility =
        SunVisibility(
            hit_position, hit_normal, light_direction);
    const float ndotl =
        saturate(dot(hit_normal, light_direction));
    const float wrap = saturate(
        (dot(hit_normal, light_direction) + 0.20) / 1.20);
    const float sky_amount =
        saturate(hit_normal.y * 0.5 + 0.5);
    const float3 sky_fill =
        lerp(float3(0.16, 0.17, 0.19),
             float3(0.35, 0.47, 0.64), sky_amount);
    const float3 ambient_light =
        sky_fill *
        (sun_direction_ambient.w *
         lerp(0.72, 1.12, sky_amount));
    float3 diffuse_light =
        sun_color_intensity.rgb *
        sun_color_intensity.w *
        lerp(wrap * 0.24, ndotl, 0.84);
    diffuse_light *= lerp(0.28, 1.0, visibility);
    const float3 view_direction =
        normalize(-reflection_ray.Direction);
    const float3 halfway =
        normalize(light_direction + view_direction);
    const float roughness = saturate(material.w);
    const float specular =
        pow(saturate(dot(hit_normal, halfway)),
            lerp(96.0, 8.0, roughness)) *
        ndotl * lerp(0.24, 0.035, roughness);
    const float fresnel =
        pow(1.0 -
                saturate(dot(hit_normal, view_direction)),
            4.0);
    const float orientation_ao =
        lerp(0.82, 1.0, sky_amount);
    float3 albedo;
    if (dynamic_hit &&
        texture_index < 64) {
      const float2 hit_uv =
          LoadUv(indices.x) * weights.x +
          LoadUv(indices.y) * weights.y +
          LoadUv(indices.z) * weights.z;
      albedo =
          dynamic_diffuse_textures[
              NonUniformResourceIndex(texture_index)]
              .SampleLevel(
                  dynamic_texture_sampler, hit_uv, 0.0).rgb;
    } else if (dynamic_hit) {
      albedo = material.rgb;
    } else {
      albedo = MaterialAlbedo(
          material.rgb,
          SurfaceUv(
              hit_position, hit_normal,
              material_detail.y),
          int(material_detail.x + 0.5),
          material_detail.z);
    }
    if (exact_character && character_family >= 4 &&
        character_family <= 5) {
      // Character hair pages are grayscale; material.rgb carries the
      // captured per-skater hair tint.
      albedo *= material.rgb;
    }
    const bool static_emissive =
        !dynamic_hit && material_detail.w > 0.0;
    if (static_emissive) {
      result = albedo * material_detail.w;
    } else if (exact_character) {
      result = ShadeCharacter(
          albedo, hit_normal, view_direction,
          visibility, lighting_index, character_family);
      result += MovingLightContribution(
          hit_position, hit_normal, view_direction,
          albedo, roughness);
    } else if ((material_flags & 1u) != 0u) {
      result = material.rgb;
    } else {
      result =
          albedo * (ambient_light + diffuse_light) *
              orientation_ao +
          sun_color_intensity.rgb * specular +
          sky_fill * fresnel *
              (1.0 - roughness) * 0.08;
      result += MovingLightContribution(
          hit_position, hit_normal, view_direction,
          albedo, roughness);
    }

    if (!static_emissive && !exact_character &&
        (material_flags & 1u) == 0u) {
      const float distance_fog = saturate(
          reflection_query.CommittedRayT() /
          600.0);
      result = lerp(
          result, SkyColor(reflected_direction),
          distance_fog * distance_fog * 0.55);
    }
  } else {
    result = SkyColor(reflected_direction);
  }

  if (puddle) {
    // Wet ground retains a faint cool absorption and receives just enough
    // scene underneath to avoid reading as a portal cut into the floor.
    result *= float3(0.91, 0.96, 1.0);
  }
  mirror_output[pixel] = float4(
      exact_character_output
          ? result * 0.965
          : PassGamma(result * 0.965),
      puddle
          ? saturate(sky_horizon.w * coverage)
          : 1.0);
}
