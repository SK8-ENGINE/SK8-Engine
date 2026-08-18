// Character families: the game's own lighting in LINEAR space (diffuse is
// gamma -> square it), then the exact tone chain from the disassembly and
// the postfx uber's 1.41 scene multiplier (which the empirical world
// shading already folds into its constants; without it characters sit
// ~30% darker than their surroundings, measured on an F11 A/B pair).
float4 ShadeCharacter(VSOut i, float4 albedo) {
  float fam = cam_pos.w;
  float3 dlin = albedo.rgb * albedo.rgb;
  float3 cn = dot(i.nrm, i.nrm) > 0.01
                  ? normalize(i.nrm)
                  : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
  float ndl = saturate(dot(cn, ch_light.xyz));
  float3 vd = -normalize(i.rpos);
  float3 lin;
  // The retained branches below still reproduce Skate 3 when the owned
  // world is disabled. In owned-world mode these carry only material
  // identity into the project-owned lighting model; none of the captured
  // retail key/ambient result is reused.
  float3 owned_albedo = dlin;
  float3 owned_normal = cn;
  float owned_roughness = 0.72;
  float out_a = 1.0;
  if (fam > 5.5) {
    // Traffic vehicles (vehicle.fx fam 6 body / vehicle_glass.fx fam 7
    // windows, disassembled from vehicle_defaultPS): paint recolor where
    // the diffuse green channel is below the mask threshold (red-channel
    // mask * colorize_red + blue-channel mask * colorize_blue, the taxi
    // yellow), key light + the livingworld flat ambient, phong specular
    // along the reflected sun, and an environment-cube reflection scaled
    // by fresnel and the gloss packed in the diffuse alpha. Glass keeps
    // only the reflection terms (its tint rows are zero) and blends at
    // the captured alpha. overlay.y > 0 = the material's cube resolved
    // at t6 (same convention as water).
    float3 sel;
    if (fam < 6.5) {
      sel = dlin.g > 0.001225
                ? dlin
                : ch_tintA.rgb * dlin.r + ch_tintB.rgb * dlin.b;
    } else {
      sel = ch_tintA.rgb;
    }
    // DXN panel normal map (the material's `normal` channel, riding the
    // macro slot; overlay.z > 0 = resolved). The vertex layout carries no
    // tangent frame, so build a screen-space cotangent frame from the
    // position/uv derivatives; it reproduces the authored panel shading
    // including mirrored UV islands. Skipping the map entirely shades the
    // hinged panels by their vertex normals, which face away from the sun
    // - the dark ambient-blue "misdrawn shadow" that stopped at the door
    // seam (verified against the ucode: flat map = the artifact, real
    // map = the emulated car).
    float3 vn = cn;
    if (overlay.z > 0.5) {
      float2 nm = macro.Sample(smp, i.uv).rg * 2.0 - 1.0;
      float3 dp1 = ddx(i.rpos), dp2 = ddy(i.rpos);
      float2 du1 = ddx(i.uv), du2 = ddy(i.uv);
      float3 dp2p = cross(dp2, cn), dp1p = cross(cn, dp1);
      float3 tt = dp2p * du1.x + dp1p * du2.x;
      float3 bb = dp2p * du1.y + dp1p * du2.y;
      float im = rsqrt(max(max(dot(tt, tt), dot(bb, bb)), 1e-12));
      vn = normalize(nm.x * tt * im + nm.y * bb * im +
                     cn * sqrt(saturate(1.0 - dot(nm, nm))));
    }
    float vndl = saturate(dot(vn, ch_light.xyz));
    float3 rfl = ch_light.xyz - 2.0 * dot(vn, ch_light.xyz) * vn;
    // The sun spec is gated on N.L >= 0 (the ucode multiplies the spec
    // term by an sge result); the cube reflection is not.
    float spec = pow(saturate(dot(vd, -rfl)), max(ch_sh[0].w, 1.0)) *
                 (dot(vn, ch_light.xyz) >= 0.0 ? 1.0 : 0.0);
    float fres = pow(1.0 - saturate(dot(vn, vd)), max(ch_light.w, 1.0));
    float3 cube = float3(0.0, 0.0, 0.0);
    if (overlay.y > 0.5) {
      cube = env_cube.Sample(smp, reflect(-vd, vn)).rgb;
      cube *= cube;  // the PS consumes the cube squared (linear space)
    }
    // Gloss = the diffuse alpha SQUARED: the ucode squares the whole
    // diffuse fetch (linear-space decode), alpha included; raw alpha
    // over-specs ~5x and mottles the body panels.
    float gloss = fam < 6.5 ? albedo.a * albedo.a : 1.0;
    lin = sel * (ch_key.rgb * vndl + ch_amb.rgb) +
          (spec * ch_sh[0].rgb + cube) * fres * gloss;
    out_a = ch_misc.x;
  } else if (fam > 3.5) {
    // Hair (cac_hair / defaulthair): key on a wrapped N.L ramp + flat
    // ambient, fresnel rim tint on a steeper ramp; strand coverage from
    // the mesh's "alpha" channel at the raw second texcoord (bound at t4)
    // - alpha-blended in the sorted sub-pass (hair drawn opaque is the
    // blocky-helmet look).
    // CAC hair diffuse pages are intentionally grayscale. The actual
    // per-skater colour was already captured into mat_tint from the
    // character.hair PS c17 row, but the native shader previously ignored
    // it, leaving dark brown/black hair neutral grey.
    float3 hair_albedo =
        dlin * (mat_tint.w > 0.5 ? max(mat_tint.rgb, 0.0) : 1.0);
    float fres = pow(1.0 - saturate(dot(cn, vd)), max(ch_light.w, 1.0));
    float3 hl = ch_key.rgb * (saturate(ndl * 0.75 + 0.25) + ch_amb.w) +
                ch_tintB.rgb * fres * saturate(ndl * 1.75 + 0.25);
    lin = hair_albedo * hl;
    owned_albedo = hair_albedo;
    owned_roughness = 0.88;
    out_a = saturate(decal_art.Sample(smp, i.uv2).r * ch_tintB.w);
  } else if (fam > 2.5) {
    // livingworld pedestrians: the diffuse is a stamp-mask atlas; red
    // regions recolor with tintA, blue with tintB (judged in linear
    // space; real-color regions have green above the threshold).
    // The game's character PSes multiply the key light by the CSM shadow
    // (tap >= ray = lit); characters are casters themselves, so an extra
    // receiver bias suppresses self-shadow acne while the body-onto-board
    // / body-onto-NPC shading survives (the sun-axis depth gap there is
    // tens of cm). Without this the held skateboard, a big flat surface
    // that is almost always inside the skater's own shadow, renders
    // fully sunlit (near-white) against the emulated dark deck.
    float csm = min(SampleCsmShadowSoft(i.rpos + cam_pos.xyz, 0.012, cn, i.pos.xy),
                    SampleStaticSun(i.rpos + cam_pos.xyz, cn, i.pos.xy));
    float3 sel = dlin.g > 0.001225
                     ? dlin
                     : ch_tintA.rgb * dlin.r + ch_tintB.rgb * dlin.b;
    lin = sel * (ch_key.rgb * ndl * csm + ch_amb.rgb);
    owned_albedo = sel;
    // livingworld_stamp_defaultPS ends `max oC0.w, c21.x`: the entity's
    // spawn/distance fade. Only visible when the item is routed to the
    // blended sub-pass (alpha < 1); the opaque pass ignores it.
    out_a = ch_misc.x;
  } else {
    // defaultcharacter / CAC pieces: key light + SH irradiance ambient,
    // key gated by the CSM shadow (see the livingworld comment above).
    // The material's DXT5nm normal map (x in ALPHA, y in GREEN; the PSes
    // read tf4.wy) rides the macro slot; overlay.z > 0 = resolved. The
    // skinned vertex layout has no free bytes for the authored tangent
    // frame (blend weights/indices own them), so use the screen-space
    // cotangent frame. ScreenTangentFrame's env calibration (negated U
    // axis, raw V axis) holds for characters too: projecting the skinned
    // meshes through the frame's real view_proj and taking the D3D y-down
    // screen-basis derivatives on the CAMERA-FACING triangles dots the
    // frame against the skinned usage-6/7 tangent frame at -0.95 (tt.T) /
    // +0.95 (bb.B); an edge-basis check without the projection lands on
    // the back-face orientation and reads inverted. Map x rides T, map y
    // rides B (the VS skins the usage-6 tangent into the interpolator the
    // PS pairs with tf4.w, the usage-7 binormal into the tf4.y one).
    float3 vn = cn;
    if (overlay.z > 0.5) {
      // misc.y = LOD bias to the console's 640p-gradient mip (same
      // rationale as the reflective families' cube bias): mip 0 at 4K
      // keeps fine wrinkle noise the console filters away, which reads
      // as weaker authored folds than the emulated reference.
      float2 nm = macro.SampleBias(smp, i.uv, misc.y).ag * 2.0 - 1.0;
      float3 tt, bb;
      ScreenTangentFrame(cn, i.rpos, i.uv, tt, bb);
      vn = normalize(nm.x * tt + nm.y * bb +
                     cn * sqrt(saturate(1.0 - dot(nm, nm))));
    }
    float vndl_u = dot(vn, ch_light.xyz);
    float vndl = saturate(vndl_u);
    float csm = min(SampleCsmShadowSoft(i.rpos + cam_pos.xyz, 0.012, vn, i.pos.xy),
                    SampleStaticSun(i.rpos + cam_pos.xyz, vn, i.pos.xy));
    if (ch_tintA.w > 0.0) {
      dlin *= ch_tintA.rgb;
    }
    owned_albedo = dlin;
    owned_normal = vn;
    owned_roughness = fam > 1.5 ? 0.62 : 0.72;
    float3 irr = saturate(
        ch_sh[0].rgb + vn.x * ch_sh[1].rgb + vn.y * ch_sh[2].rgb +
        vn.z * ch_sh[3].rgb + (vn.x * vn.z) * ch_sh[4].rgb +
        (vn.z * vn.y) * ch_sh[5].rgb + (vn.y * vn.x) * ch_sh[6].rgb +
        (vn.z * vn.z) * ch_sh[7].rgb +
        (vn.x * vn.x - vn.y * vn.y) * ch_sh[8].rgb);
    float3 lit = ch_key.rgb * vndl * csm + irr * ch_amb.w;
    float3 spec = float3(0.0, 0.0, 0.0);
    // Rim light + key/rim phong specular, exact from the character PSes
    // (ch_ks.w == 0 = rows not captured -> the terms vanish). The key spec
    // reflects the sun about the mapped normal, gated on N.L >= 0 and the
    // shadow; the rim terms share the game's fixed rim direction built
    // from the sun and view. Spec mask = the diffuse alpha SQUARED
    // (linear-space decode, like the vehicle gloss); skin/face carry a
    // dedicated mask map in the free decal slot instead (overlay.w = 3;
    // 2 = map present but not yet decoded -> mask 0, because the DXT1
    // skin diffuse's opaque alpha would read as a full-white mask).
    if (ch_ks.w > 0.0) {
      float fb = 1.0 - saturate(dot(vn, vd));
      float kfres = pow(max(fb, 1e-6), ch_rim.w);
      float rfres = pow(max(fb, 1e-6), ch_misc.w);
      float3 rd = normalize(ch_light.xyz * float3(-1.0, 0.2, -1.0) - vd);
      lit += saturate(rfres * saturate(dot(vn, rd))) * ch_rim.rgb;
      float3 kr = ch_light.xyz - 2.0 * vndl_u * vn;
      float ks = pow(saturate(dot(vd, -kr)), ch_ks.w) * csm *
                 (vndl_u >= 0.0 ? 1.0 : 0.0);
      float3 rr = rd - 2.0 * dot(vn, rd) * vn;
      float rs = pow(saturate(dot(vd, -rr)), ch_rs.w) * rfres;
      float smask = overlay.w > 2.5 ? decal_art.SampleBias(smp, i.uv, misc.y).r
                                    : (overlay.w > 1.5 ? 0.0 : albedo.a);
      smask *= smask;
      spec = saturate((ks * ch_ks.rgb * kfres + rs * ch_rs.rgb) * smask);
    }
    lin = dlin * lit + spec;
    // The PS multiplies the lit color by the material multiplier
    // m_params[0].y before the tone chain (1.2 on the gameplay banks;
    // without it the jeans sit ~14% darker than the emulated reference).
    // Captured into the otherwise-unused tintB.w on fams 1/2; 0 = an
    // older capture without it.
    if (ch_tintB.w > 0.25) {
      lin *= ch_tintB.w;
    }
    // defaultcharacter/cacstamp PSes end `max oC0.w, c13.x / c22.x`:
    // the entity's spawn fade (see the livingworld comment above).
    out_a = ch_misc.x;
    // character.alpha accessory (sunglass lens): coverage from the mesh's
    // "alpha" channel at the raw second texcoord, like hair (cac_alphaPS:
    // oC0.w = tf5(uv2).r * c22.x), routed to the blended sub-pass.
    if (ch_misc.z > 0.5) {
      out_a *= saturate(decal_art.Sample(smp, i.uv2).r);
    }
  }
  if (owned_light_meta.y > 0.5 && fam < 5.5) {
    // Fully relight retained character materials inside the owned world.
    // Do not scale and reuse `lin`: it already contains Skate 3's key,
    // ambient, SH, rim, exposure and tone response. Reusing it and then
    // adding the owned sun was the direct-light double count that made the
    // player glow white outdoors.
    const float night = saturate(owned_light_meta.z);
    const float3 world_position = i.rpos + cam_pos.xyz;
    const float static_visibility =
        SampleStaticSun(world_position, owned_normal, i.pos.xy);
    const float dynamic_visibility =
        SampleCsmShadowSoft(
            world_position, 0.018, owned_normal, i.pos.xy);
    const float direct_visibility =
        min(static_visibility, dynamic_visibility);
    const float3 light_direction =
        normalize(owned_celestial_direction.xyz);
    const float celestial_ndotl =
        saturate(dot(owned_normal, light_direction));
    const float wrapped_ndotl =
        saturate((dot(owned_normal, light_direction) + 0.20) / 1.20);
    const float diffuse_response =
        fam > 3.5
            ? lerp(wrapped_ndotl * 0.42, celestial_ndotl, 0.58)
            : lerp(wrapped_ndotl * 0.24, celestial_ndotl, 0.84);
    const float sky_amount =
        saturate(owned_normal.y * 0.5 + 0.5);
    const float3 sky_fill =
        lerp(float3(0.16, 0.17, 0.19),
             float3(0.35, 0.47, 0.64), sky_amount);
    const float shelter_visibility =
        lerp(0.30, 1.0, static_visibility);
    float3 ambient_light =
        sky_fill * owned_celestial_color.w *
        lerp(0.82, 1.28, sky_amount) * shelter_visibility * 1.34;
    // Characters need the lower-hemisphere bounce that their original
    // material rig supplied separately from the directional key. This is
    // owned sky/ground irradiance, not retained vanilla light: it keeps
    // back-facing clothing and skin readable in open sunlight while still
    // respecting shelter visibility.
    const float3 ground_fill =
        float3(0.24, 0.20, 0.17) *
        owned_celestial_color.w *
        lerp(0.42, 0.18, sky_amount) *
        shelter_visibility;
    ambient_light += ground_fill;
    const float light_luma =
        dot(owned_celestial_color.rgb,
            float3(0.299, 0.587, 0.114));
    // Preserve the authored skin/clothing colour under a very warm moving
    // sun. The world keeps the full celestial colour; character diffuse
    // uses a small neutral adaptation like a camera's skin-tone response.
    const float3 character_light_color =
        lerp(owned_celestial_color.rgb, light_luma.xxx, 0.16);
    float3 direct_light =
        character_light_color *
        owned_celestial_direction.w *
        diffuse_response *
        lerp(0.16, 1.0, direct_visibility);
    lin = owned_albedo * (ambient_light + direct_light);

    // Restrained owned-direction specular. It follows the same sun and
    // visibility as the diffuse term and therefore cannot reintroduce a
    // bright vanilla rim in world shadow.
    const float3 halfway =
        normalize(light_direction + normalize(-i.rpos));
    const float spec_power =
        lerp(96.0, 8.0, owned_roughness);
    const float specular =
        pow(saturate(dot(owned_normal, halfway)), spec_power) *
        celestial_ndotl * direct_visibility *
        lerp(0.22, 0.025, owned_roughness);
    lin += character_light_color * specular *
           lerp(float3(0.04, 0.04, 0.04), owned_albedo, 0.06);
    lin += OwnedMovingLightContribution(
        world_position, owned_normal, normalize(-i.rpos),
        owned_albedo, owned_roughness) *
        lerp(0.12, 1.0, night);
    // m_params[0].y is a material response scalar (normally ~1.2), not a
    // light. Retaining it restores the intended cloth/skin colour density
    // without bringing back Skate 3's key or SH result.
    if (fam < 2.5 && ch_tintB.w > 0.25) {
      lin *= clamp(ch_tintB.w, 0.8, 1.35);
    }
    // Retained character textures were authored for this highlight-
    // compressing curve. Feed it a controlled owned exposure instead of
    // the captured vanilla exposure: shadows lift gently, direct whites
    // stop clipping, and channel compression improves colour balance.
    const float owned_exposure = lerp(1.12, 0.96, night);
    return ToneOut(max(lin, 0.0) * owned_exposure, out_a, false);
  }
  // Exact tone chain: sqrt(0.5 * (max(x*E/4 + 0.75, 1) - sat(1 - x*E)^2)).
  float E = max(ch_key.w, 0.01);
  return ToneOut(lin * E, out_a, false);

}
