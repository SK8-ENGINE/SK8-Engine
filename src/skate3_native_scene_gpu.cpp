// Native scene renderer, render-thread half: consumes the FrameScene
// published by skate3_native_scene.cpp (game-thread capture/build) and draws
// it through the RHI (D3D12 / Vulkan) into the guest output texture. Shared
// cross-thread state lives in skate3_native_scene_state.h.

#include "skate3_native_debug_dialog.h"
#include "skate3_demo_path.h"
#include "skate3_native_scene.h"

#include "generated/skate3_init.h"

#include <array>
#include <atomic>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/kernel/guest_presence.h>
#include <rex/logging.h>

#include "native/skate3_native_diag.h"
#include "native/skate3_native_entity.h"
#include "native/skate3_native_guest_read.h"
#include "native/skate3_native_lw.h"
#include "native/skate3_native_palette.h"
// Offline-compiled SPIR-V for the native shaders (compiled from the HLSL
// sources with DXC): the Vulkan RHI backend consumes these blobs; the D3D12
// backend runtime-compiles the embedded HLSL as before.
#include "native/shaders/spirv/skate3_native_shaders_spirv.h"

#if (defined(REX_HAS_D3D12) && REX_HAS_D3D12) || (defined(REX_HAS_VULKAN) && REX_HAS_VULKAN)
#include <rex/graphics/native_rhi.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#if defined(_WIN32)
#include <windows.h>
#endif
#endif
#include "skate3_native_scene_state.h"
#include "skate3_native_scene_gpu_internal.h"
#include "skate3_mechanics_sandbox.h"
#include "skate3_mechanics_sandbox_map.h"
#include "skate3_multiplayer.h"
#include "skate3_multiplayer_motion_trace.h"
#include "skate3_multiplayer_pose_cadence.h"
#include "skate3_multiplayer_assets.h"
#include "skate3_multiplayer_capture.h"
#include "skate3_multiplayer_lifecycle.h"
#include "skate3_multiplayer_render_cache.h"
#include "skate3_native_collision.h"
#include "skate3_native_raytraced_mirror.h"
#include "skate3_trick_pipeline.h"

// Cvars defined in skate3_native_scene.cpp (and SDK cvars re-declared there).
REXCVAR_DECLARE(bool, async_shader_compilation);
REXCVAR_DECLARE(bool, native_render_suppress_emulated_draws);
REXCVAR_DECLARE(bool, readback_resolve_half_pixel_offset);
REXCVAR_DECLARE(bool, skate3_multiplayer_async_appearance_prepare);
REXCVAR_DECLARE(bool, skate3_multiplayer_incremental_appearance_install);
REXCVAR_DECLARE(int32_t, skate3_multiplayer_appearance_install_ops_per_frame);
REXCVAR_DECLARE(double, skate3_multiplayer_appearance_install_budget_ms);
REXCVAR_DECLARE(bool, skate3_native_render_scene_2d);
REXCVAR_DECLARE(bool, skate3_native_render_scene_backface_cull);
REXCVAR_DECLARE(bool, skate3_native_render_scene_bloom);
REXCVAR_DECLARE(bool, skate3_native_render_scene_boot_native);
REXCVAR_DECLARE(bool, skate3_native_render_scene_cas_yield);
REXCVAR_DECLARE(bool, skate3_native_render_scene_char_shadow_exact);
REXCVAR_DECLARE(bool, skate3_native_render_scene_decals);
REXCVAR_DECLARE(bool, skate3_native_render_scene_dynamic_items);
REXCVAR_DECLARE(bool, skate3_native_render_scene_dynobj_v2);
REXCVAR_DECLARE(bool, skate3_native_render_scene_entity_fade);
REXCVAR_DECLARE(bool, skate3_native_render_scene_fmv_native);
REXCVAR_DECLARE(bool, skate3_native_render_scene_fmv_yield);
REXCVAR_DECLARE(bool, skate3_native_render_scene_hdr);
REXCVAR_DECLARE(bool, skate3_native_render_scene_hdr_packed);
REXCVAR_DECLARE(bool, skate3_native_render_scene_lightmaps);
REXCVAR_DECLARE(bool, skate3_native_render_scene_lm_dump);
REXCVAR_DECLARE(bool, skate3_native_render_scene_loading_native);
REXCVAR_DECLARE(bool, skate3_native_render_scene_macro);
REXCVAR_DECLARE(bool, skate3_native_render_scene_menu_rtt_passes);
REXCVAR_DECLARE(bool, skate3_native_render_scene_menu_unsuppress);
REXCVAR_DECLARE(bool, skate3_native_render_scene_mesh_revalidate);
REXCVAR_DECLARE(bool, skate3_native_render_scene_pause_native);
REXCVAR_DECLARE(bool, skate3_native_render_scene_perf_log);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_perf_interval);
REXCVAR_DECLARE(bool, skate3_native_render_scene_perf_items);
REXCVAR_DECLARE(bool, skate3_native_render_scene_occlusion_cull);
REXCVAR_DECLARE(bool, skate3_native_render_scene_photo_display_yield);
REXCVAR_DECLARE(bool, skate3_native_render_scene_photo_grab_native);
REXCVAR_DECLARE(bool, skate3_native_render_scene_photo_native);
REXCVAR_DECLARE(bool, skate3_native_render_scene_photo_readback);
REXCVAR_DECLARE(bool, skate3_native_render_scene_photo_yield);
REXCVAR_DECLARE(bool, skate3_native_render_scene_refl_bias_auto);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ring);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ropa_blend);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ropa_inline);
REXCVAR_DECLARE(bool, skate3_native_render_scene_selection_outline);
REXCVAR_DECLARE(bool, skate3_native_render_scene_shadow_caster_parity);
REXCVAR_DECLARE(bool, skate3_native_render_scene_shadow_pcss);
REXCVAR_DECLARE(bool, skate3_native_render_scene_shadow_static_casters);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_pcss_blocker_m);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_pcss_max_m);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_pcss_min_texel);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_pcss_sun_deg);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_static_bias);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_static_bias_vk);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_static_radius);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_shadow_static_size);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_static_strength);
REXCVAR_DECLARE(bool, skate3_native_render_scene_shadows);
REXCVAR_DECLARE(bool, skate3_native_render_scene_shafts);
REXCVAR_DECLARE(bool, skate3_native_render_scene_haze);
REXCVAR_DECLARE(bool, skate3_native_render_scene_showcase);
REXCVAR_DECLARE(bool, skate3_native_render_scene_sort_opaque);
REXCVAR_DECLARE(bool, skate3_native_render_scene_splines);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ssao);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ssao_debug);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ssao_full_res);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ssr);
REXCVAR_DECLARE(bool, skate3_native_render_scene_tex_mips);
REXCVAR_DECLARE(bool, skate3_native_render_scene_tex_revalidate);
REXCVAR_DECLARE(bool, skate3_native_render_scene_transparents);
REXCVAR_DECLARE(bool, skate3_native_render_scene_world_v2);
REXCVAR_DECLARE(double, skate3_menu_blur_sigma);
REXCVAR_DECLARE(double, skate3_native_render_scene_2d_sharp);
REXCVAR_DECLARE(double, skate3_native_render_scene_bloom_intensity);
REXCVAR_DECLARE(double, skate3_native_render_scene_bloom_knee);
REXCVAR_DECLARE(double, skate3_native_render_scene_bloom_threshold);
REXCVAR_DECLARE(double, skate3_native_render_scene_refl_bias_x);
REXCVAR_DECLARE(double, skate3_native_render_scene_refl_bias_y);
REXCVAR_DECLARE(double, skate3_native_render_scene_refl_lod);
REXCVAR_DECLARE(double, skate3_native_render_scene_ssao_intensity);
REXCVAR_DECLARE(double, skate3_native_render_scene_ssao_luma_protect);
REXCVAR_DECLARE(double, skate3_native_render_scene_ssao_radius);
REXCVAR_DECLARE(double, skate3_native_render_scene_showcase_hold);
REXCVAR_DECLARE(double, skate3_native_render_scene_showcase_wipe);
REXCVAR_DECLARE(std::string, skate3_native_render_scene_showcase_order);
REXCVAR_DECLARE(double, skate3_native_render_scene_ssr_intensity);
REXCVAR_DECLARE(double, skate3_native_render_scene_ssr_thickness);
REXCVAR_DECLARE(double, skate3_native_render_scene_world_v2_tan_sign);
REXCVAR_DECLARE(int32_t, native_render_force_resolve_readback_max_length);
REXCVAR_DECLARE(int32_t, native_render_suppress_mode);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_2d_async_px);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_debug);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_detail_hold);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_hdr_debug);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_menu_rtt_scope);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_mesh_decode_budget);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_mesh_store_mb);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_msaa);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_photo_native_accum);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_photo_native_debug);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_prewarm_budget_ms);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_refl_mode);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_settle_max_frames);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_shadow_tile);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_ssr_debug);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_ssr_steps);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_tex_store_mb);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_warmup_budget_ms);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_warmup_min_items);
REXCVAR_DECLARE(std::string, skate3_native_render_scene_trace_mesh);
REXCVAR_DECLARE(std::string, skate3_native_render_scene_trace_2d);
REXCVAR_DECLARE(std::string, skate3_native_render_snapshot_dir);

#if (defined(REX_HAS_D3D12) && REX_HAS_D3D12) || (defined(REX_HAS_VULKAN) && REX_HAS_VULKAN)

namespace skate3::native_scene {
namespace {

// Retire a guest texture's GPU resources AND its view. Destruction is
// deferred inside the RHI against the CURRENT submission; the `submission`
// parameter is kept for call-site compatibility and ignored.
void RetireGuestTexture(const GuestTexture& t, uint64_t submission) {
  (void)submission;
  g_r.device->DestroyDeferred(t.texture);
  g_r.device->DestroyDeferred(t.upload);
  g_r.device->DestroyDeferred(t.upload_b);
  g_r.device->DestroyDeferred(t.srv);
}

// ---- Content store helpers --------------------------------------------------
// The store is words-keyed; the guest
// streamer's object retargeting (mip flap A<->B, detail demote, object
// reuse) is just different keys, so both states of any transition stay
// resident and no rebind can ever serve another binding's art.
std::atomic<uint64_t> g_store_evicted{0};
constexpr size_t kTexStoreCap = 12288;

uint32_t SwapU32(uint32_t v);  // defined with the decode helpers below

// (kTexStoreCap sizing: dense areas with the extended draw distance hold a
// ~3000-4000 entry live working set, and a map switch stacks two working
// sets; a 6144 cap kept the eviction latch cycling every ~20 s in dense
// areas, re-decoding content the player was about to face again (visible
// pop-in / late resolves). The count cap is a wide entry-bloat backstop
// now: the byte budget (skate3_native_render_scene_tex_store_mb) is the
// real VRAM bound and engages first on texture-heavy content.)

// Seqlock-stable read of a texture object's six fetch words, guest -> host
// order. The streamer rewrites the words word-by-word on its own thread; a
// mixed snapshot decodes a coherent image of the WRONG memory (a shared
// mip-pool page reads as a collage of neighbor art), and the result is
// stable, valid-looking content no fingerprint can reject after the fact;
// the read itself must be self-consistent. Two consecutive identical
// snapshots (4 attempts) or the caller keeps its previous route/skips.
bool ReadStableTexWords(uint8_t* base, uint32_t tex_ptr, uint32_t out[6]) {
  uint32_t raw[6];
  uint32_t raw2[6];
  bool stable = false;
  for (int attempt = 0; attempt < 4 && !stable; ++attempt) {
    if (!GuestTryCopy(raw, base + tex_ptr + 7 * 4, sizeof(raw)) ||
        !GuestTryCopy(raw2, base + tex_ptr + 7 * 4, sizeof(raw2))) {
      return false;
    }
    stable = std::memcmp(raw, raw2, sizeof(raw)) == 0;
  }
  if (!stable) {
    return false;
  }
  for (uint32_t i = 0; i < 6; ++i) {
    out[i] = SwapU32(raw[i]);
  }
  return true;
}

// Frames-per-second estimate for the eviction age guards. The LRU clocks
// are frame-stamped, so wall-clock idle thresholds must convert through the
// live frame rate: a fixed frame count that means 30 seconds at 60 fps
// passes in 3 seconds on a 600 fps loading screen, which made a whole
// map's freshly prewarmed working set eviction-eligible at takeover.
double g_evict_fps_estimate = 240.0;

void UpdateEvictFpsEstimate(uint64_t frame_number) {
  static double s_last_t = 0.0;
  static uint64_t s_last_f = 0;
  const double now = std::chrono::duration<double>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  if (s_last_t != 0.0 && now > s_last_t + 0.5) {
    const double rate = double(frame_number - s_last_f) / (now - s_last_t);
    if (rate > 1.0 && rate < 2000.0) {
      g_evict_fps_estimate = g_evict_fps_estimate * 0.7 + rate * 0.3;
    }
  }
  if (s_last_t == 0.0 || now > s_last_t + 0.5) {
    s_last_t = now;
    s_last_f = frame_number;
  }
}

// Store LRU eviction, run once per frame: superseded words states (old mip
// levels, pre-demote detail sets, one-shot UI art) hold SRV slots + GPU
// memory until nothing has routed to them for a while. Amortized: once over
// the cap, a bounded batch of oldest entries goes per frame until the store
// is back under the low-water mark (the previous evict-half-in-one-frame
// sweep retired ~1500 textures in a single frame, a measured recurring
// hitch). The idle guard is wall-clock scaled: entries used within the
// last kTexEvictMinIdleSeconds are never candidates, so a dense area whose
// working set exceeds the cap parks over it instead of thrash-evicting
// live content into a re-decode loop.
constexpr size_t kTexEvictPerFrame = 64;
constexpr double kTexEvictMinIdleSeconds = 45.0;

// Byte-budget layer over the count caps: the caps above bound ENTRY counts,
// but per-entry sizes differ per map, and after a map switch the union of
// the old and new maps' working sets sits under the idle guards for minutes
// at full size. Each store therefore also tracks an estimated GPU byte
// total (rescanned periodically, decremented per eviction); crossing the
// byte budget latches the same amortized LRU, and while the total is over
// the budget's low-water mark the idle guard shortens so the superseded
// map's content drains promptly instead of stacking VRAM across switches.
// The pressure guard stays multi-second so a live working set (stamped
// every frame it serves) is never evicted into a re-decode loop.
constexpr uint64_t kStoreByteScanIntervalFrames = 120;
constexpr double kEvictPressureIdleSeconds = 5.0;
uint64_t g_tex_store_bytes = 0;
uint64_t g_mesh_store_bytes = 0;

// Estimated bytes of one surface in the host formats the store decodes to.
uint64_t FormatSurfaceBytes(nrhi::Format format, uint32_t w, uint32_t h) {
  switch (format) {
    case nrhi::Format::kBC1_UNORM:
    case nrhi::Format::kBC4_UNORM:
      return uint64_t((w + 3) / 4) * ((h + 3) / 4) * 8;
    case nrhi::Format::kBC2_UNORM:
    case nrhi::Format::kBC3_UNORM:
    case nrhi::Format::kBC5_UNORM:
      return uint64_t((w + 3) / 4) * ((h + 3) / 4) * 16;
    case nrhi::Format::kR8_UNORM:
      return uint64_t(w) * h;
    case nrhi::Format::kR8G8_UNORM:
    case nrhi::Format::kB5G6R5_UNORM:
      return uint64_t(w) * h * 2;
    default:  // kR8G8B8A8_UNORM and anything unanticipated
      return uint64_t(w) * h * 4;
  }
}

// Committed-texture footprint estimate (base level + the mip chain's
// asymptotic 1/3; cube entries carry 6 faces). Accuracy at the few-percent
// level is enough: this feeds the eviction budget, not an allocator.
uint32_t GuestTextureGpuBytes(const GuestTexture& t, uint32_t faces = 1) {
  if (t.texture == nullptr) {
    return 0;
  }
  uint64_t bytes = FormatSurfaceBytes(t.texture->format(), t.texture->width(),
                                      t.texture->height());
  if (t.srv_mips != 1) {
    bytes += bytes / 3;
  }
  bytes *= faces;
  return bytes > UINT32_MAX ? UINT32_MAX : uint32_t(bytes);
}

void EvictTexStore(uint64_t frame_number, uint64_t submission) {
  static bool s_evicting = false;
  static uint64_t s_next_scan_frame = 0;
  static uint64_t s_evicted_run = 0;
  static uint64_t s_next_bytes_frame = 0;
  if (frame_number >= s_next_bytes_frame) {
    s_next_bytes_frame = frame_number + kStoreByteScanIntervalFrames;
    uint64_t total = 0;
    for (auto& [k, t] : g_r.tex_store) {
      if (t.gpu_bytes == 0) {
        t.gpu_bytes = GuestTextureGpuBytes(t);
      }
      total += t.gpu_bytes;
      // In-place-update entries retain their staging ping-pong buffers,
      // which land in device-local memory on resizable-BAR systems.
      if (t.upload != nullptr) {
        total += t.upload->size();
      }
      if (t.upload_b != nullptr) {
        total += t.upload_b->size();
      }
    }
    g_tex_store_bytes = total;
  }
  const uint64_t byte_cap =
      uint64_t(std::max(256, REXCVAR_GET(skate3_native_render_scene_tex_store_mb)))
      << 20;
  const uint64_t byte_low = byte_cap - byte_cap / 8;
  const size_t low_water = kTexStoreCap - kTexStoreCap / 8;
  const bool was_evicting = s_evicting;
  if (g_r.tex_store.size() > kTexStoreCap || g_tex_store_bytes > byte_cap) {
    s_evicting = true;
  } else if (g_r.tex_store.size() <= low_water && g_tex_store_bytes <= byte_low) {
    s_evicting = false;
  }
  const bool byte_pressure = g_tex_store_bytes > byte_low;
  const uint64_t min_idle_frames = std::max<uint64_t>(
      4, uint64_t(g_evict_fps_estimate * (byte_pressure ? kEvictPressureIdleSeconds
                                                        : kTexEvictMinIdleSeconds)));
  if (was_evicting != s_evicting) {
    if (s_evicting) {
      s_evicted_run = 0;
      REXLOG_INFO(
          "native-scene: tex store LRU start (size={} cap={} mb={} cap_mb={} "
          "min_idle={}f @ {:.0f}fps)",
          g_r.tex_store.size(), kTexStoreCap, g_tex_store_bytes >> 20,
          byte_cap >> 20, min_idle_frames, g_evict_fps_estimate);
    } else {
      REXLOG_INFO("native-scene: tex store LRU done (size={} mb={} evicted={})",
                  g_r.tex_store.size(), g_tex_store_bytes >> 20, s_evicted_run);
    }
  }
  if (!s_evicting || frame_number < s_next_scan_frame) {
    return;
  }
  std::vector<std::pair<uint64_t, uint64_t>> ages;  // (last-used frame, key)
  ages.reserve(g_r.tex_store.size());
  for (const auto& [k, t] : g_r.tex_store) {
    if (t.last_used_frame + min_idle_frames < frame_number) {
      ages.emplace_back(t.last_used_frame, k);
    }
  }
  size_t excess = g_r.tex_store.size() > low_water
                      ? g_r.tex_store.size() - low_water
                      : 0;
  if (byte_pressure) {
    // Keep draining while over the byte budget even with the count under
    // its cap: convert the byte excess into entries via the average size.
    const uint64_t avg =
        std::max<uint64_t>(1, g_tex_store_bytes /
                                  std::max<size_t>(1, g_r.tex_store.size()));
    excess = std::max(excess, size_t((g_tex_store_bytes - byte_low) / avg) + 1);
  }
  const size_t n = std::min({ages.size(), excess, kTexEvictPerFrame});
  if (n == 0) {
    // Everything over the cap is recent (a dense area's live working set):
    // park and re-scan later instead of burning a full-store scan per frame.
    s_next_scan_frame = frame_number + 120;
    return;
  }
  std::nth_element(ages.begin(), ages.begin() + (n - 1), ages.end());
  for (size_t i = 0; i < n; ++i) {
    const auto it = g_r.tex_store.find(ages[i].second);
    if (it != g_r.tex_store.end()) {
      g_tex_store_bytes -= std::min<uint64_t>(g_tex_store_bytes,
                                              it->second.gpu_bytes);
      RetireGuestTexture(it->second, submission);
      g_r.tex_store.erase(it);
    }
  }
  s_evicted_run += n;
  g_store_evicted.fetch_add(n, std::memory_order_relaxed);
}

// Mesh cache LRU, same amortized shape: without it every streamed arena's
// meshes accumulated for the whole session (observed: 61k live buffers
// after a few minutes of map changes). The wall-clock idle guard keeps the
// current map's prewarmed set resident (prewarm decodes thousands of
// meshes the player has not seen yet; evicting them defeats the prewarm
// and turns panning into a re-decode churn loop); an evicted mesh
// re-decodes on the workers like any first sight.
// (Cap sizing: dense maps prewarm ~21k meshes at ~13 KB average, ~280 MB
// total - a 6144 cap forced the whole prewarm through a churn funnel for
// trivial VRAM. The count cap is an entry backstop; the byte budget
// (skate3_native_render_scene_mesh_store_mb) is the VRAM bound.)
constexpr size_t kMeshStoreCap = 24576;
constexpr size_t kMeshEvictPerFrame = 64;
constexpr double kMeshEvictMinIdleSeconds = 90.0;

void EvictMeshStore(uint64_t frame_number) {
  static bool s_evicting = false;
  static uint64_t s_next_scan_frame = 0;
  static uint64_t s_evicted_run = 0;
  static uint64_t s_next_bytes_frame = 0;
  if (frame_number >= s_next_bytes_frame) {
    s_next_bytes_frame = frame_number + kStoreByteScanIntervalFrames;
    uint64_t total = 0;
    for (const auto& [k, m] : g_r.meshes) {
      total += uint64_t(m.vb_view.size_bytes) + m.ib_view.size_bytes;
    }
    g_mesh_store_bytes = total;
  }
  const uint64_t byte_cap =
      uint64_t(std::max(256, REXCVAR_GET(skate3_native_render_scene_mesh_store_mb)))
      << 20;
  const uint64_t byte_low = byte_cap - byte_cap / 8;
  const size_t low_water = kMeshStoreCap - kMeshStoreCap / 8;
  const bool was_evicting = s_evicting;
  if (g_r.meshes.size() > kMeshStoreCap || g_mesh_store_bytes > byte_cap) {
    s_evicting = true;
  } else if (g_r.meshes.size() <= low_water && g_mesh_store_bytes <= byte_low) {
    s_evicting = false;
  }
  const bool byte_pressure = g_mesh_store_bytes > byte_low;
  const uint64_t min_idle_frames =
      byte_pressure
          ? std::max<uint64_t>(
                240, uint64_t(g_evict_fps_estimate * kEvictPressureIdleSeconds))
          : std::max<uint64_t>(
                1800, uint64_t(g_evict_fps_estimate * kMeshEvictMinIdleSeconds));
  if (was_evicting != s_evicting) {
    if (s_evicting) {
      s_evicted_run = 0;
      REXLOG_INFO(
          "native-scene: mesh store LRU start (size={} cap={} mb={} cap_mb={} "
          "min_idle={}f @ {:.0f}fps)",
          g_r.meshes.size(), kMeshStoreCap, g_mesh_store_bytes >> 20,
          byte_cap >> 20, min_idle_frames, g_evict_fps_estimate);
    } else {
      REXLOG_INFO("native-scene: mesh store LRU done (size={} mb={} evicted={})",
                  g_r.meshes.size(), g_mesh_store_bytes >> 20, s_evicted_run);
    }
  }
  if (!s_evicting || frame_number < s_next_scan_frame) {
    return;
  }
  std::vector<std::pair<uint64_t, uint32_t>> ages;  // (last-used frame, key)
  ages.reserve(g_r.meshes.size());
  for (const auto& [k, m] : g_r.meshes) {
    if (m.last_used_frame + min_idle_frames < frame_number) {
      ages.emplace_back(m.last_used_frame, k);
    }
  }
  size_t excess =
      g_r.meshes.size() > low_water ? g_r.meshes.size() - low_water : 0;
  if (byte_pressure) {
    const uint64_t avg = std::max<uint64_t>(
        1, g_mesh_store_bytes / std::max<size_t>(1, g_r.meshes.size()));
    excess = std::max(excess, size_t((g_mesh_store_bytes - byte_low) / avg) + 1);
  }
  const size_t n = std::min({ages.size(), excess, kMeshEvictPerFrame});
  if (n == 0) {
    // Everything over the cap is in recent use (a dense map's population):
    // park and re-scan later instead of burning a full-store scan per frame.
    s_next_scan_frame = frame_number + 120;
    return;
  }
  std::nth_element(ages.begin(), ages.begin() + (n - 1), ages.end());
  for (size_t i = 0; i < n; ++i) {
    const auto it = g_r.meshes.find(ages[i].second);
    if (it != g_r.meshes.end()) {
      g_mesh_store_bytes -= std::min<uint64_t>(
          g_mesh_store_bytes,
          uint64_t(it->second.vb_view.size_bytes) + it->second.ib_view.size_bytes);
      g_r.device->DestroyDeferred(it->second.vb);
      g_r.device->DestroyDeferred(it->second.ib);
      g_r.ropa_shapes.erase(it->first);
      g_r.meshes.erase(it);
    }
  }
  s_evicted_run += n;
}

// Environment-cube cache LRU: a handful of cubes serve any one area, but
// entries are keyed by guest object address and accumulated across every
// map/park visited in the session. Amortized one eviction per frame; the
// idle guard keeps anything recently sampled resident.
constexpr size_t kCubeStoreCap = 48;

void EvictCubeStore(uint64_t frame_number, uint64_t submission) {
  if (g_r.cube_textures.size() <= kCubeStoreCap) {
    return;
  }
  const uint64_t min_idle_frames = std::max<uint64_t>(
      4, uint64_t(g_evict_fps_estimate * kTexEvictMinIdleSeconds));
  auto oldest = g_r.cube_textures.end();
  for (auto it = g_r.cube_textures.begin(); it != g_r.cube_textures.end(); ++it) {
    if (it->second.last_used_frame + min_idle_frames < frame_number &&
        (oldest == g_r.cube_textures.end() ||
         it->second.last_used_frame < oldest->second.last_used_frame)) {
      oldest = it;
    }
  }
  if (oldest != g_r.cube_textures.end()) {
    RetireGuestTexture(oldest->second, submission);
    g_r.cube_textures.erase(oldest);
    g_store_evicted.fetch_add(1, std::memory_order_relaxed);
  }
}

// ---- Staged texture decode (worker-thread half) ---------------------------
// The texture decoders normally finish by recording GPU copies into the
// frame's command stream, pushing a barrier and creating the SRV, all
// render-thread-only. When `g_tex_stage_out` is set (decode worker), they
// stop after filling the upload resource and export what the render-thread
// commit needs instead. RHI resource creation and upload mapping are
// thread-safe, so everything up to that point is safe off-thread.
struct StagedMipCopy {
  uint32_t offset, pitch, w, h;  // upload footprint per mip
};
struct StagedTexCommit {
  nrhi::Format copy_format = nrhi::Format::kUnknown;
  nrhi::Format srv_format = nrhi::Format::kUnknown;
  nrhi::Swizzle swizzle[4] = {nrhi::Swizzle::kX, nrhi::Swizzle::kY,
                              nrhi::Swizzle::kZ, nrhi::Swizzle::kW};
  uint32_t mip_count = 0;
  // Cube map (environment cubes): mips[] holds face-major (face * levels +
  // mip) subresource copies, the old D3D12 subresource numbering, which the
  // commit decomposes back into (face, mip), and the SRV is a kCube view
  // with cube_mip_levels levels. The cube MIP CHAIN
  // is load-bearing: the game's reflective glass perturbs its reflection
  // vector with a per-pixel normal map, so the hardware cube fetch runs at
  // a DEEP gradient-derived LOD; a mip-0-only cube shows the plaza cube's
  // baked streetlight heads as a sharp magnified smear the real console
  // output blurs away.
  bool cube = false;
  uint32_t cube_mip_levels = 1;
  // 6 faces x up to 10 levels (512 -> 1 full generated chain).
  StagedMipCopy mips[64] = {};
};
thread_local StagedTexCommit* g_tex_stage_out = nullptr;

// Render-thread half: record the staged upload's copies + barrier, create
// the SRV, mark the texture live. On view-creation failure the texture stays
// invalid (renders white; the backend-managed views make this near-impossible).
void CommitStagedGuestTexture(const NativeGuestOutputRenderContext& context,
                              GuestTexture& gt, const StagedTexCommit& sc) {
  for (uint32_t m = 0; m < sc.mip_count; ++m) {
    const StagedMipCopy& p = sc.mips[m];
    // Cube commits stage face-major (face * levels + mip) entries (the old
    // D3D12 subresource numbering): decompose into (mip, face).
    const uint32_t mip = sc.cube ? m % sc.cube_mip_levels : m;
    const uint32_t face = sc.cube ? m / sc.cube_mip_levels : 0;
    context.cmd->CopyBufferToTexture(gt.texture, mip, face, gt.upload, p.offset,
                                     p.pitch, p.w, p.h, 1);
  }
  context.cmd->Barrier(gt.texture, nrhi::ResourceState::kCopyDest,
                       nrhi::ResourceState::kPixelShaderResource);
  // The copies are recorded; the staging buffer has no further use (the
  // in-place update path recreates its own on demand), and holding it for
  // the cache entry's lifetime doubled the store's memory. Destruction is
  // deferred until the recorded submission completes.
  g_r.device->DestroyDeferred(gt.upload);
  gt.upload = nullptr;
  nrhi::TextureViewDesc vd;
  vd.format = sc.srv_format;
  for (uint32_t c = 0; c < 4; ++c) {
    vd.swizzle[c] = sc.swizzle[c];
  }
  if (sc.cube) {
    vd.dimension = nrhi::ViewDimension::kCube;
    vd.mip_levels = sc.cube_mip_levels;
  } else {
    vd.dimension = nrhi::ViewDimension::k2D;
    vd.mip_levels = sc.mip_count;
    gt.srv_format = sc.srv_format;
    gt.srv_mips = sc.mip_count;
  }
  gt.srv = g_r.device->CreateTextureView(gt.texture, vd);
  if (gt.srv == nullptr) {
    gt.valid = false;
    return;
  }
  gt.valid = true;
}

// Worker-pool result plumbing (see the prewarm queue globals above; these
// live here because they need the resource/item types).
struct StagedTexResult {
  uint32_t key = 0;        // guest texture object address (object-keyed cache)
  uint64_t words_key = 0;  // != 0: content-store result (g_r.tex_store)
  bool cube = false;       // environment cube (g_r.cube_textures)
  GuestTexture gt;
  StagedTexCommit commit;
  bool valid = false;
  // The commit-time stability verify rejected this decode (payload moved
  // between the worker's read and the commit): retry fast, not on the
  // failed-decode ladder.
  bool verify_failed = false;
  // 2D/HUD overlay origin: skip the stability verify (see PrewarmEntry::ui).
  bool ui = false;
};
struct PrewarmResult {
  DrawItem item;
  MeshBuffers buffers;
  bool mesh_valid = false;
  std::vector<StagedTexResult> textures;
  // Draw-path miss result (visible right now): the commit takes it this
  // frame regardless of the gameplay per-frame cap.
  bool miss = false;
};
std::mutex g_prewarm_out_mutex;
std::vector<PrewarmResult> g_prewarm_out;
// Failed builds (buffer objects not initialized yet) land here; the render
// thread re-injects them each frame so retries are frame-paced instead of
// hot-spinning the workers.
std::vector<PrewarmEntry> g_prewarm_retry;  // under g_prewarm_out_mutex

// Shader sources live in src/native/shaders/*.hlsl; the build embeds them
// into this generated header (cmake/EmbedShaders.cmake) so the exe stays
// self-contained. Same kFooSource char arrays as before. (The offline
// SPIR-V table for the same shaders is included at the top of the file;
// it declares ::skate3::native_spirv and must not nest inside this
// namespace.)
#include "skate3_native_shaders.h"

float HalfToFloat(uint16_t h) {
  const uint32_t sign = (h & 0x8000u) << 16;
  uint32_t exp = (h >> 10) & 0x1F;
  uint32_t mant = h & 0x3FF;
  if (exp == 0) {
    if (mant == 0) return std::bit_cast<float>(sign);
    // subnormal
    while (!(mant & 0x400)) {
      mant <<= 1;
      --exp;
    }
    ++exp;
    mant &= 0x3FF;
  } else if (exp == 31) {
    return std::bit_cast<float>(sign | 0x7F800000u | (mant << 13));
  }
  return std::bit_cast<float>(sign | ((exp + 112) << 23) | (mant << 13));
}

nrhi::Buffer* CreateUploadBuffer(
    nrhi::Device* device, size_t size,
    nrhi::BufferBindClass bind_class = nrhi::BufferBindClass::kFull) {
  nrhi::BufferDesc desc;
  desc.size = size;
  desc.heap = nrhi::HeapKind::kUpload;
  desc.bind_class = bind_class;
  return device->CreateBuffer(desc);  // nullptr on failure, like the old helper
}

// ---- Dynamic-mesh buffer reuse pool ---------------------------------------
// Cloth/ropa garments and streaming heals replace their VB/IB every commit;
// creating and destroying upload buffers per frame churns the allocator and
// contends with the decode workers. Replaced buffers park here keyed by
// exact byte size and are reused once the GPU is past the submission that
// last referenced them (a stable garment then alternates between two
// allocations). Thread-safe: decode workers and the render thread both
// acquire and retire.
struct PooledMeshBuffer {
  nrhi::Buffer* buffer = nullptr;
  uint64_t retire_submission = 0;
};
std::mutex g_mesh_pool_mutex;
std::vector<PooledMeshBuffer> g_mesh_pool;
constexpr size_t kMeshPoolCap = 96;

void PoolMeshBuffer(nrhi::Device* device, nrhi::Buffer* buffer) {
  if (buffer == nullptr) return;
  nrhi::Buffer* overflow = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mesh_pool_mutex);
    if (g_mesh_pool.size() >= kMeshPoolCap) {
      overflow = g_mesh_pool.front().buffer;
      g_mesh_pool.erase(g_mesh_pool.begin());
    }
    g_mesh_pool.push_back({buffer, device->CurrentSubmission()});
  }
  device->DestroyDeferred(overflow);
}

nrhi::Buffer* AcquireMeshUploadBuffer(nrhi::Device* device, size_t size) {
  {
    const uint64_t completed = device->CompletedSubmission();
    std::lock_guard<std::mutex> lock(g_mesh_pool_mutex);
    for (auto it = g_mesh_pool.begin(); it != g_mesh_pool.end(); ++it) {
      if (it->buffer->size() == size && it->retire_submission < completed) {
        nrhi::Buffer* buffer = it->buffer;
        *it = g_mesh_pool.back();
        g_mesh_pool.pop_back();
        return buffer;
      }
    }
  }
  return CreateUploadBuffer(device, size, nrhi::BufferBindClass::kVertexIndex);
}

constexpr uint32_t kSandboxSkyMesh = 0xF00D0002u;
constexpr uint32_t kSandboxMapMeshBase = 0xF1000000u;
constexpr std::size_t kSandboxMapMeshCapacity = 0x00FFFFFFu;
constexpr uint32_t kSandboxKinematicMeshBase = 0xF2000000u;
constexpr std::size_t kSandboxKinematicMeshCapacity = 0x0000FFFFu;
constexpr uint32_t kSandboxHingedDoorMeshBase = 0xF2100000u;
constexpr std::size_t kSandboxHingedDoorMeshCapacity = 0x0000FFFFu;
constexpr uint32_t kSandboxWaterMesh = 0xF3000000u;
constexpr uint32_t kSandboxWaterPusherMesh = 0xF3000001u;
constexpr uint32_t kSandboxMovingLightMesh = 0xF4000000u;
constexpr uint32_t kSandboxRainMesh = 0xF5000000u;
constexpr uint32_t kSandboxLightningMesh = 0xF5000001u;
constexpr uint32_t kSandboxRemoteSkaterMesh = 0xF6000000u;

uint32_t SandboxMapMeshKey(std::size_t chunk_index) {
  return kSandboxMapMeshBase + static_cast<uint32_t>(chunk_index);
}

bool EnsureSandboxVisualMesh(
    nrhi::Device* device, uint32_t key,
    const mechanics_sandbox::map::VisualMesh& mesh,
    const char* label) {
  if (g_r.meshes.find(key) != g_r.meshes.end()) {
    return true;
  }
  if (mesh.vertices.empty() || mesh.indices.empty() ||
      mesh.vertices.size() > 0xFFFFu) {
    static std::atomic<uint32_t> s_invalid_map_log{0};
    if (s_invalid_map_log.fetch_add(1, std::memory_order_relaxed) == 0) {
      REXLOG_ERROR(
          "native-scene: sandbox {} geometry is empty or too large", label);
    }
    return false;
  }
  static_assert(sizeof(mechanics_sandbox::map::VisualVertex) == 56);
  nrhi::Buffer* vb = AcquireMeshUploadBuffer(
      device, mesh.vertices.size() * sizeof(mechanics_sandbox::map::VisualVertex));
  nrhi::Buffer* ib = AcquireMeshUploadBuffer(device,
                                              mesh.indices.size() * sizeof(uint16_t));
  if (vb == nullptr || ib == nullptr) {
    static std::atomic<uint32_t> s_buffer_map_log{0};
    if (s_buffer_map_log.fetch_add(1, std::memory_order_relaxed) == 0) {
      REXLOG_ERROR(
          "native-scene: sandbox {} buffer allocation failed (vb={} ib={})",
          label, vb != nullptr ? 1 : 0, ib != nullptr ? 1 : 0);
    }
    PoolMeshBuffer(device, vb);
    PoolMeshBuffer(device, ib);
    return false;
  }
  void* vb_ptr = device->Map(vb);
  void* ib_ptr = device->Map(ib);
  if (vb_ptr == nullptr || ib_ptr == nullptr) {
    static std::atomic<uint32_t> s_map_map_log{0};
    if (s_map_map_log.fetch_add(1, std::memory_order_relaxed) == 0) {
      REXLOG_ERROR(
          "native-scene: sandbox {} buffer mapping failed (vb={} ib={})",
          label, vb_ptr != nullptr ? 1 : 0, ib_ptr != nullptr ? 1 : 0);
    }
    if (vb_ptr != nullptr) device->Unmap(vb);
    if (ib_ptr != nullptr) device->Unmap(ib);
    PoolMeshBuffer(device, vb);
    PoolMeshBuffer(device, ib);
    return false;
  }
  std::memcpy(vb_ptr, mesh.vertices.data(),
              mesh.vertices.size() * sizeof(mechanics_sandbox::map::VisualVertex));
  std::memcpy(ib_ptr, mesh.indices.data(), mesh.indices.size() * sizeof(uint16_t));
  device->Unmap(vb);
  device->Unmap(ib);
  MeshBuffers buffers;
  buffers.vb = vb;
  buffers.ib = ib;
  buffers.vb_view = {vb, 0,
                     uint32_t(mesh.vertices.size() * sizeof(mechanics_sandbox::map::VisualVertex)),
                     uint32_t(sizeof(mechanics_sandbox::map::VisualVertex))};
  buffers.ib_view = {ib, 0, uint32_t(mesh.indices.size() * sizeof(uint16_t))};
  buffers.fingerprint = 1;
  buffers.last_used_frame = 0;
  g_r.meshes.emplace(key, std::move(buffers));
  return true;
}

bool EnsureSandboxMapChunk(nrhi::Device* device,
                           std::size_t chunk_index) {
  const mechanics_sandbox::map::VisualWorld& world =
      mechanics_sandbox::map::ActiveVisualWorld();
  if (chunk_index >= world.chunks.size() ||
      chunk_index >= kSandboxMapMeshCapacity) {
    return false;
  }
  return EnsureSandboxVisualMesh(
      device, SandboxMapMeshKey(chunk_index), world.chunks[chunk_index],
      "map chunk");
}

bool EnsureSandboxKinematicMesh(nrhi::Device* device,
                                std::size_t object_index) {
  if (object_index >=
          mechanics_sandbox::map::ActiveKinematicObjectCount() ||
      object_index >= kSandboxKinematicMeshCapacity) {
    return false;
  }
  return EnsureSandboxVisualMesh(
      device,
      kSandboxKinematicMeshBase +
          static_cast<uint32_t>(object_index),
      mechanics_sandbox::map::ActiveKinematicVisualMesh(object_index),
      "kinematic object");
}

bool EnsureSandboxHingedDoorMesh(nrhi::Device* device,
                                 std::size_t door_index) {
  if (door_index >=
          mechanics_sandbox::map::ActiveHingedDoorCount() ||
      door_index >= kSandboxHingedDoorMeshCapacity) {
    return false;
  }
  return EnsureSandboxVisualMesh(
      device,
      kSandboxHingedDoorMeshBase +
          static_cast<std::uint32_t>(door_index),
      mechanics_sandbox::map::ActiveHingedDoorVisualMesh(door_index),
      "hinged door");
}

bool EnsureSandboxSkyMesh(nrhi::Device* device) {
  return EnsureSandboxVisualMesh(
      device, kSandboxSkyMesh,
      mechanics_sandbox::map::ActiveSkyMesh(), "sky");
}

bool EnsureSandboxMovingLightMesh(nrhi::Device* device) {
  return EnsureSandboxVisualMesh(
      device, kSandboxMovingLightMesh,
      mechanics_sandbox::map::ActiveMovingLightVisualMesh(),
      "moving light orb");
}

bool EnsureSandboxRemoteSkaterMesh(nrhi::Device* device) {
  return EnsureSandboxVisualMesh(
      device, kSandboxRemoteSkaterMesh,
      mechanics_sandbox::map::ActiveRemoteSkaterVisualMesh(),
      "remote skater");
}

bool EnsureSandboxRainMesh(nrhi::Device* device) {
  return EnsureSandboxVisualMesh(
      device, kSandboxRainMesh,
      mechanics_sandbox::map::ActiveRainVisualMesh(), "storm rain");
}

bool EnsureSandboxLightningMesh(nrhi::Device* device) {
  return EnsureSandboxVisualMesh(
      device, kSandboxLightningMesh,
      mechanics_sandbox::map::ActiveLightningVisualMesh(),
      "storm lightning");
}

bool UpdateSandboxDynamicVisualMesh(
    nrhi::Device* device, uint32_t key,
    const mechanics_sandbox::map::VisualMesh& mesh,
    const char* label) {
  if (!EnsureSandboxVisualMesh(device, key, mesh, label)) {
    return false;
  }
  auto found = g_r.meshes.find(key);
  if (found == g_r.meshes.end() ||
      found->second.vb_view.size_bytes !=
          mesh.vertices.size() *
              sizeof(mechanics_sandbox::map::VisualVertex)) {
    return false;
  }

  nrhi::Buffer* replacement = AcquireMeshUploadBuffer(
      device, found->second.vb_view.size_bytes);
  if (replacement == nullptr) {
    return false;
  }
  void* mapped = device->Map(replacement);
  if (mapped == nullptr) {
    PoolMeshBuffer(device, replacement);
    return false;
  }
  std::memcpy(
      mapped, mesh.vertices.data(),
      mesh.vertices.size() *
          sizeof(mechanics_sandbox::map::VisualVertex));
  device->Unmap(replacement);

  nrhi::Buffer* previous = found->second.vb;
  found->second.vb = replacement;
  found->second.vb_view = {
      replacement, 0,
      static_cast<uint32_t>(
          mesh.vertices.size() *
          sizeof(mechanics_sandbox::map::VisualVertex)),
      static_cast<uint32_t>(
          sizeof(mechanics_sandbox::map::VisualVertex)),
  };
  PoolMeshBuffer(device, previous);
  return true;
}

uint16_t SwapU16(uint16_t v) { return uint16_t((v >> 8) | (v << 8)); }
uint32_t SwapU32(uint32_t v) {
#if defined(_MSC_VER)
  return _byteswap_ulong(v);
#else
  return __builtin_bswap32(v);
#endif
}

// Upload sub-allocation alignment for per-mip footprints (the old
// D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT; row pitches use
// nrhi::kRowPitchAlignment = the old D3D12_TEXTURE_DATA_PITCH_ALIGNMENT).
constexpr uint32_t kUploadPlacementAlignment = 512;

// Decode guest vertices into {float3 position, float2 uv, float2 uv2,
// unorm4 blend weights, u8x4 blend indices, float3 normal, float2 decal_uv}
// (56-byte stride). decal_uv = the 3rd/4th halves of a half4 first texcoord
// (the packed second UV pair environment.decal art is mapped with), equal to
// the first pair when the element has only two halves.
// vb_payload/ib_payload: optional pre-copied guest payload snapshots (the
// dynamic cloth decode jobs snapshot on the guest thread and convert on a
// worker, see DynDecodeJob); when null the payloads are read live from
// guest memory with the guarded copy.
bool DecodeMesh(nrhi::Device* device, uint8_t* base, const DrawItem& item,
                MeshBuffers& out, const uint8_t* vb_payload = nullptr,
                const uint8_t* ib_payload = nullptr) {
  const uint32_t num_verts = item.vb_bytes / item.stride;
  if (num_verts == 0) return false;
  // This runs on the render thread; the guest payloads were valid on the
  // game thread this frame but streaming can decommit them in between.
  // Copy them out with the lock-free guarded copy (never VirtualQuery here:
  // the VAD lock stalls behind the guest streaming threads while panning).
  static thread_local std::vector<uint8_t> vb_scratch;
  static thread_local std::vector<uint8_t> ib_scratch;
  if (vb_payload == nullptr) {
    vb_scratch.resize(item.vb_bytes);
    if (!GuestTryCopy(vb_scratch.data(), base + item.vb_addr, item.vb_bytes)) {
      return false;
    }
    vb_payload = vb_scratch.data();
  }
  if (!item.cloth_quads && ib_payload == nullptr) {
    ib_scratch.resize(size_t(item.ib_count) * 2);
    if (!GuestTryCopy(ib_scratch.data(), base + item.ib_addr, size_t(item.ib_count) * 2)) {
      return false;
    }
    ib_payload = ib_scratch.data();
  }
  nrhi::Buffer* vb = AcquireMeshUploadBuffer(device, size_t(num_verts) * 56);
  nrhi::Buffer* ib = AcquireMeshUploadBuffer(device, size_t(item.ib_count) * 2);
  if (!vb || !ib) {
    PoolMeshBuffer(device, vb);
    PoolMeshBuffer(device, ib);
    return false;
  }

  // two_sided_sheet detection eligibility (see MeshBuffers): small static
  // triangle-list meshes only. Strips alternate winding per triangle and
  // skinned/cloth meshes deform, so both stay on the uncull(ed) PSO.
  bool detect_sheet =
      !item.skinned && !item.cloth_quads && item.ib_count >= 6 && item.ib_count <= 8192;
  for (const DrawEntry& de : item.draws) {
    if (de.prim != 4) {
      detect_sheet = false;
    }
  }
  std::vector<float> sheet_pos;
  if (detect_sheet) {
    sheet_pos.reserve(size_t(num_verts) * 3);
  }

  const uint8_t* src_vb = vb_payload;
  float* dst = nullptr;
  uint32_t garbage = 0;
  int min_bi = 255;
  int max_bi = -1;
  dst = static_cast<float*>(device->Map(vb));
  for (uint32_t v = 0; v < num_verts; ++v) {
    const uint8_t* p = src_vb + size_t(v) * item.stride + item.pos_offset;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    switch (item.pos_fmt) {
      case 57: {  // k_32_32_32_FLOAT
        x = std::bit_cast<float>(SwapU32(*reinterpret_cast<const uint32_t*>(p)));
        y = std::bit_cast<float>(SwapU32(*reinterpret_cast<const uint32_t*>(p + 4)));
        z = std::bit_cast<float>(SwapU32(*reinterpret_cast<const uint32_t*>(p + 8)));
        break;
      }
      case 32: {  // k_16_16_16_16_FLOAT
        x = HalfToFloat(SwapU16(*reinterpret_cast<const uint16_t*>(p)));
        y = HalfToFloat(SwapU16(*reinterpret_cast<const uint16_t*>(p + 2)));
        z = HalfToFloat(SwapU16(*reinterpret_cast<const uint16_t*>(p + 4)));
        break;
      }
      case 26: {  // k_16_16_16_16 snorm character dequant
        const auto s16 = [&](int off) {
          return int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(p + off)));
        };
        constexpr float kScale = 2.0f / 32767.0f;
        x = s16(0) * kScale;
        y = s16(2) * kScale + 0.8f;
        z = s16(4) * kScale;
        break;
      }
      default:
        device->Unmap(vb);
        device->DestroyDeferred(vb);
        device->DestroyDeferred(ib);
        return false;
    }
    if (!(x == x && y == y && z == z) ||
        x < item.bbox_min[0] - 2.f || x > item.bbox_max[0] + 2.f ||
        y < item.bbox_min[1] - 2.f || y > item.bbox_max[1] + 2.f ||
        z < item.bbox_min[2] - 2.f || z > item.bbox_max[2] + 2.f) {
      ++garbage;
    }
    const auto decode_uv = [&](uint32_t fmt, uint32_t offset, float& u, float& w) {
      u = 0.0f;
      w = 0.0f;
      if (fmt == 0) return;
      const uint8_t* q = src_vb + size_t(v) * item.stride + offset;
      switch (fmt) {
        case 31:  // k_16_16_FLOAT
        case 32:  // k_16_16_16_16_FLOAT (use xy)
          u = HalfToFloat(SwapU16(*reinterpret_cast<const uint16_t*>(q)));
          w = HalfToFloat(SwapU16(*reinterpret_cast<const uint16_t*>(q + 2)));
          break;
        case 37:  // k_32_32_FLOAT (hair strand-alpha UV; xenos enum 37)
        case 38:  // k_32_32_32_32_FLOAT (use xy)
          u = std::bit_cast<float>(SwapU32(*reinterpret_cast<const uint32_t*>(q)));
          w = std::bit_cast<float>(SwapU32(*reinterpret_cast<const uint32_t*>(q + 4)));
          break;
        case 25: {  // k_16_16 (snorm; UVs span the full s16 range for [0,1])
          const int16_t su = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q)));
          const int16_t sv = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q + 2)));
          u = su / 32767.0f;
          w = sv / 32767.0f;
          break;
        }
        case 26: {  // k_16_16_16_16 (snorm; use xy: unwrap UV sets)
          const int16_t su = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q)));
          const int16_t sv = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q + 2)));
          u = su / 32767.0f;
          w = sv / 32767.0f;
          break;
        }
        default:
          break;
      }
    };
    dst[v * 14 + 0] = x;
    dst[v * 14 + 1] = y;
    dst[v * 14 + 2] = z;
    if (detect_sheet) {
      sheet_pos.push_back(x);
      sheet_pos.push_back(y);
      sheet_pos.push_back(z);
    }
    decode_uv(item.uv_fmt, item.uv_offset, dst[v * 14 + 3], dst[v * 14 + 4]);
    decode_uv(item.uv2_fmt, item.uv2_offset, dst[v * 14 + 5], dst[v * 14 + 6]);
    // The second texcoord is the lightmap/decal UNWRAP, stored as an
    // absolute value with tangent handedness in the sign bits, in BOTH the
    // s16-snorm (fmt 26, decal meshes) and half-float (fmt 31, world tiles)
    // encodings (decalenvironment VS: o0.zw = |uv|; baseenvironment VS:
    // maxs r2.zw = |r4.xy|). Raw signed values sample mirrored atlas cells.
    // Exception: hair meshes, their second texcoord is the strand-alpha
    // UV, passed RAW by the hair VS (o0.zw = uv, float2), and the ocean,
    // whose VS passes the lightmap UV raw (Out.UV.zw = In.LM, no abs).
    if (item.char_family < 4 && item.water_ocean == 0) {
      dst[v * 14 + 5] = std::fabs(dst[v * 14 + 5]);
      dst[v * 14 + 6] = std::fabs(dst[v * 14 + 6]);
    }
    // Blend weights (unorm bytes) and indices (raw bytes). Guest u8x4
    // attributes are stored big-endian per 32-bit word; swap so component k
    // in the shader matches guest component k.
    uint32_t bw = 0;
    uint32_t bi = 0;
    if (item.skinned) {
      bw = SwapU32(*reinterpret_cast<const uint32_t*>(src_vb + size_t(v) * item.stride +
                                                      item.bw_offset));
      bi = SwapU32(*reinterpret_cast<const uint32_t*>(src_vb + size_t(v) * item.stride +
                                                      item.bi_offset));
      for (int k = 0; k < 4; ++k) {
        const uint8_t weight = uint8_t(bw >> (k * 8));
        const uint8_t index = uint8_t(bi >> (k * 8));
        if (weight != 0) {
          if (index < min_bi) min_bi = index;
          if (index > max_bi) max_bi = index;
        }
      }
    }
    std::memcpy(&dst[v * 14 + 7], &bw, 4);
    std::memcpy(&dst[v * 14 + 8], &bi, 4);
    // Decal-art UV: second half pair of a half4 first texcoord, else uv0.
    if (item.uv_fmt == 32) {
      decode_uv(31, item.uv_offset + 4, dst[v * 14 + 12], dst[v * 14 + 13]);
    } else {
      dst[v * 14 + 12] = dst[v * 14 + 3];
      dst[v * 14 + 13] = dst[v * 14 + 4];
    }
    // Vertex normal: k_10_11_11 packed (x 11 bits, y 11 bits, z 10 bits,
    // LSB to MSB, signed). Zero when absent -> derivative face-normal
    // fallback in the shader.
    const auto unpack_10_11_11 = [&](uint32_t offset, float out[3]) {
      const uint32_t word = SwapU32(*reinterpret_cast<const uint32_t*>(
          src_vb + size_t(v) * item.stride + offset));
      const int32_t ix = int32_t(word << 21) >> 21;
      const int32_t iy = int32_t((word >> 11) << 21) >> 21;
      const int32_t iz = int32_t((word >> 22) << 22) >> 22;
      out[0] = float(ix) / 1023.0f;
      out[1] = float(iy) / 1023.0f;
      out[2] = float(iz) / 511.0f;
    };
    // Stored-tangent-frame pack (world-shading / dynobj v2): snorm
    // component to the unorm byte of the blend-weight word.
    const auto pk = [](float f) {
      return uint32_t(std::lround((f * 0.5f + 0.5f) * 255.0f)) & 0xFFu;
    };
    float n3[3] = {0.0f, 0.0f, 0.0f};
    if (item.water_flowing && !item.skinned && item.uv2_fmt == 26 &&
        item.tan_fmt == 16) {
      // Water meshes (flowingwater VS layout): the fmt-26 element is
      // lm_norm: xy = |unwrap| with sign bits, zw = normal.xy (snorm),
      // normal.z = sign.y * sqrt(1 - xy^2), and the usage-6 k_10_11_11
      // element is the TANGENT (no usage-3 normal, no usage-7 binormal).
      // Pack the tangent into the free blend-weight bytes with the unwrap
      // sign.x in the sentinel byte; the shader rebuilds the frame as
      // B = sign.x * cross(N, T), exactly the game's VS
      // (vBinormal = signs.x * cross(vNormal, vTangent)).
      const uint8_t* q = src_vb + size_t(v) * item.stride + item.uv2_offset;
      const int16_t sx = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q)));
      const int16_t sy = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q + 2)));
      const int16_t nx = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q + 4)));
      const int16_t ny = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q + 6)));
      n3[0] = nx / 32767.0f;
      n3[1] = ny / 32767.0f;
      // The game clamps xy_len at 0.999 (water normals ride |xy| ~ 1, so
      // z floors at 0.0316 rather than 0, verified against the VS ucode).
      const float xylen =
          std::min(n3[0] * n3[0] + n3[1] * n3[1], 0.999f);
      n3[2] = (sy > 0 ? 1.0f : -1.0f) * std::sqrt(1.0f - xylen);
      float t3[3];
      unpack_10_11_11(item.tangent_offset, t3);
      const uint32_t packed = pk(t3[0]) | (pk(t3[1]) << 8) |
                              (pk(t3[2]) << 16) |
                              ((sx > 0 ? 200u : 100u) << 24);
      std::memcpy(&dst[v * 14 + 7], &packed, 4);
    } else if (item.env_family != 0 &&
        (item.env_family <= 6 || item.env_family == 13) &&
        item.uv2_fmt == 26) {
      // Exact world families: the REAL vertex normal is packed in the
      // lightmap-unwrap element (fmt 26 s16x4): zw = normal.xy (snorm), and
      // the unwrap xy SIGN bits carry the handedness; sign.y flips
      // normal.z (baseenvironment VS: vNormal.z = signs.y * sqrt(1 - xy^2);
      // signs = saturate(65535 * lm.xy) * 2 - 1). The k_10_11_11 element on
      // these meshes is the BINORMAL, not the normal; using it as the
      // normal breaks the sun/spec terms of the exact shading.
      const uint8_t* q = src_vb + size_t(v) * item.stride + item.uv2_offset;
      const int16_t sx = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q)));
      const int16_t sy = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q + 2)));
      const int16_t nx = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q + 4)));
      const int16_t ny = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q + 6)));
      n3[0] = nx / 32767.0f;
      n3[1] = ny / 32767.0f;
      const float d = 1.0f - n3[0] * n3[0] - n3[1] * n3[1];
      n3[2] = (sy > 0 ? 1.0f : -1.0f) * std::sqrt(d > 0.0f ? d : 0.0f);
      // World-shading v2: the STORED tangent frame. These static meshes
      // never skin, so the blend-weight bytes are free: pack the mesh's
      // k_10_11_11 binormal (unorm-mapped) plus the unwrap sign.x tangent
      // handedness into them. The shader reconstructs T = cross(B, N) x
      // handedness; authoring decides per UV island whether the frame
      // follows a mirror, which derivative frames cannot know (the wooden
      // ramp panels banded light/dark per island without this).
      if (!item.skinned && item.normal_fmt == 16) {
        float b3[3];
        unpack_10_11_11(item.normal_offset, b3);
        // w byte is a three-state presence sentinel: 0 = no stored frame
        // (meshes that skip this pack leave the word zero), 100 = negative
        // handedness, 200 = positive.
        const uint32_t packed = pk(b3[0]) | (pk(b3[1]) << 8) |
                                (pk(b3[2]) << 16) |
                                ((sx > 0 ? 200u : 100u) << 24);
        std::memcpy(&dst[v * 14 + 7], &packed, 4);
      }
    } else if (item.normal_fmt == 16) {
      // (Vehicle meshes: the game's VS derives cross(tangent, binormal)
      // instead, but the stored usage-3 normal MATCHES it on every vertex
      // with a non-degenerate tangent frame and stays sane on the ~13%
      // interior verts where t is parallel to b and the cross vanishes,
      // measured in capture, so the stored normal is used.)
      unpack_10_11_11(item.normal_offset, n3);
      // Dynamicobject v2: rigid props store a full usage-6/7 tangent +
      // binormal pair alongside the usage-3 normal. Pack the binormal plus
      // the handedness relating cross(B, N) to the stored tangent into the
      // unused blend-weight bytes (same sentinel encoding as the env
      // families above); the authored frame carries the per-UV-island
      // mirror decisions the screen-space fallback cannot know.
      if (!item.skinned && item.dynobj != 0 && item.tb_fmt == 16) {
        float t3[3], b3[3];
        unpack_10_11_11(item.tangent_offset, t3);
        unpack_10_11_11(item.binormal_offset, b3);
        const float h = (b3[1] * n3[2] - b3[2] * n3[1]) * t3[0] +
                        (b3[2] * n3[0] - b3[0] * n3[2]) * t3[1] +
                        (b3[0] * n3[1] - b3[1] * n3[0]) * t3[2];
        const uint32_t packed = pk(b3[0]) | (pk(b3[1]) << 8) |
                                (pk(b3[2]) << 16) |
                                ((h >= 0.0f ? 200u : 100u) << 24);
        std::memcpy(&dst[v * 14 + 7], &packed, 4);
      }
    } else if (item.tb_fmt == 16) {
      // NPC character meshes carry no normal element; the game's VS
      // derives it as cross(tangent, binormal) (usage 6 x usage 7; verified
      // against the emulated VS outputs). Without this the strong
      // character N.L shading exposes the face-normal fallback as visible
      // low-poly facets on every pedestrian.
      float t3[3], b3[3];
      unpack_10_11_11(item.tangent_offset, t3);
      unpack_10_11_11(item.binormal_offset, b3);
      n3[0] = t3[1] * b3[2] - t3[2] * b3[1];
      n3[1] = t3[2] * b3[0] - t3[0] * b3[2];
      n3[2] = t3[0] * b3[1] - t3[1] * b3[0];
      // Dynamicobject v2, tangent+binormal-only layout variant: the derived
      // normal makes cross(B, N) == T x |B|^2 up to orthogonality error, so
      // the handedness is positive by construction.
      if (!item.skinned && item.dynobj != 0) {
        const uint32_t packed = pk(b3[0]) | (pk(b3[1]) << 8) |
                                (pk(b3[2]) << 16) | (200u << 24);
        std::memcpy(&dst[v * 14 + 7], &packed, 4);
      }
    }
    dst[v * 14 + 9] = n3[0];
    dst[v * 14 + 10] = n3[1];
    dst[v * 14 + 11] = n3[2];
  }
  // Dynobj v2 layout telemetry (first few meshes): which tangent-frame
  // source the props carry decides between the packed authored frame and
  // the shader's screen-space fallback.
  if (item.dynobj != 0) {
    static std::atomic<int> dynobj_layout_logged{0};
    if (dynobj_layout_logged.fetch_add(1, std::memory_order_relaxed) < 8) {
      REXLOG_INFO(
          "native-scene: dynobj mesh {:08X} layout normal_fmt={} tb_fmt={} "
          "skinned={}",
          item.mesh, item.normal_fmt, item.tb_fmt, item.skinned);
    }
  }
  // Stretch-veto probe (see g_skin_probe): cache ~32 decoded sample verts
  // so the guest thread can cheaply skin what the GPU will actually draw.
  if (item.skinned) {
    constexpr uint32_t kProbe = 32;
    const uint32_t pn = std::min(kProbe, num_verts);
    SkinProbe probe;
    probe.fp = item.fingerprint;
    probe.s.reserve(pn);
    for (uint32_t s = 0; s < pn; ++s) {
      const uint32_t v = pn > 1 ? (s * (num_verts - 1) / (pn - 1)) : 0;
      SkinProbeSample ps;
      ps.p[0] = dst[v * 14 + 0];
      ps.p[1] = dst[v * 14 + 1];
      ps.p[2] = dst[v * 14 + 2];
      std::memcpy(&ps.bw, &dst[v * 14 + 7], 4);
      std::memcpy(&ps.bi, &dst[v * 14 + 8], 4);
      probe.s.push_back(ps);
    }
    std::lock_guard<std::mutex> lock(g_skin_probe_mutex);
    if (g_skin_probe.size() > 4096) {
      g_skin_probe.clear();
    }
    g_skin_probe[item.mesh] = std::move(probe);
  }
  // ROPA shape blending: retain the decoded vertex array (14 floats per
  // vertex, the scene VS layout) so consecutive generations can be lerped
  // onto the motion-smoothing play clock at draw time; the stepped shape
  // against the interpolated body was the tee jelly (worse at LOWER fps:
  // the excursion scales with the guest period; emulated pairs pose N with
  // shape N and shows none of it).
  if (item.ropa) {
    out.ropa_verts.assign(dst, dst + size_t(num_verts) * 14);
  }
  const bool retain_raytracing =
      item.dyn_entity || item.skinned || item.ropa ||
      item.char_family != 0;
  if (retain_raytracing) {
    out.raytracing_verts.assign(dst, dst + size_t(num_verts) * 14);
  }
  device->Unmap(vb);
  // Blend indices outside the captured palette read garbage rows and mangle
  // the vertex. Indices are plain bone numbers, bone k = palette rows 3k.
  // (Dyn decode jobs carry no palette: the item's bones ride the scene item,
  // not the decode; skip the check there.)
  if (item.skinned && max_bi >= 0 && !item.bones.empty()) {
    const uint32_t palette_bones = uint32_t(item.bones.size() / 12);
    if (uint32_t(max_bi) >= palette_bones) {
      REXLOG_WARN(
          "native-scene: skinned mesh {:08X} blend index range {}..{} exceeds "
          "captured palette of {} bones",
          item.mesh, min_bi, max_bi, palette_bones);
    }
  }
  if (garbage != 0) {
    REXLOG_WARN(
        "native-scene: mesh {:08X} decoded {} of {} verts outside bbox "
        "({:.1f},{:.1f},{:.1f})..({:.1f},{:.1f},{:.1f}) fmt {} stride {} vb {:08X}",
        item.mesh, garbage, num_verts, item.bbox_min[0], item.bbox_min[1], item.bbox_min[2],
        item.bbox_max[0], item.bbox_max[1], item.bbox_max[2], item.pos_fmt, item.stride,
        item.vb_addr);
  }

  uint16_t* dst_ib = nullptr;
  dst_ib = static_cast<uint16_t*>(device->Map(ib));
  if (item.cloth_quads) {
    // Quad-list topology with no guest index buffer (live vertex range
    // already exact from the draw args): two triangles per quad.
    const uint32_t quads = item.ib_count / 6;
    for (uint32_t q = 0; q < quads; ++q) {
      const uint16_t v = uint16_t(q * 4);
      uint16_t* o = dst_ib + q * 6;
      o[0] = v;
      o[1] = uint16_t(v + 1);
      o[2] = uint16_t(v + 2);
      o[3] = v;
      o[4] = uint16_t(v + 2);
      o[5] = uint16_t(v + 3);
    }
  } else {
    const uint16_t* src_ib = reinterpret_cast<const uint16_t*>(ib_payload);
    for (uint32_t i = 0; i < item.ib_count; ++i) {
      dst_ib[i] = SwapU16(src_ib[i]);
    }
  }
  // Front/back sheet pattern: opposite-winding twin triangles a few mm-cm
  // apart ALONG THE NORMAL. The two sides are often triangulated along
  // OPPOSITE quad diagonals (downtown lamppost posters: sheets 4mm apart,
  // twin centroids 0.26m apart in-plane), so twins are matched by
  // plane-to-plane distance with an in-plane tolerance scaled to triangle
  // size; raw centroid distance misses them. >=60% twinned marks the mesh
  // double-sided (banners are 100%); O(T^2) but only for <=8192-index
  // static tri-list meshes and only once per decode.
  bool two_sided = false;
  if (detect_sheet) {
    struct SheetTri {
      float c[3];
      float n[3];
      float edge;  // longest edge length (in-plane tolerance scale)
    };
    std::vector<SheetTri> tris;
    tris.reserve(item.ib_count / 3);
    for (const DrawEntry& de : item.draws) {
      if (uint64_t(de.start_index) + de.index_count > item.ib_count) {
        continue;
      }
      for (uint32_t i = 0; i + 2 < de.index_count; i += 3) {
        const uint32_t a = uint32_t(dst_ib[de.start_index + i]) + de.base_vertex;
        const uint32_t b = uint32_t(dst_ib[de.start_index + i + 1]) + de.base_vertex;
        const uint32_t c = uint32_t(dst_ib[de.start_index + i + 2]) + de.base_vertex;
        if (a >= num_verts || b >= num_verts || c >= num_verts) {
          continue;
        }
        const float* pa = &sheet_pos[size_t(a) * 3];
        const float* pb = &sheet_pos[size_t(b) * 3];
        const float* pc = &sheet_pos[size_t(c) * 3];
        const float e1[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
        const float e2[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
        const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                            e1[0] * e2[1] - e1[1] * e2[0]};
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len < 1e-8f) {
          continue;
        }
        SheetTri t;
        for (int k = 0; k < 3; ++k) {
          t.c[k] = (pa[k] + pb[k] + pc[k]) / 3.0f;
          t.n[k] = n[k] / len;
        }
        const float e3[3] = {pc[0] - pb[0], pc[1] - pb[1], pc[2] - pb[2]};
        const float l1 = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2];
        const float l2 = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
        const float l3 = e3[0] * e3[0] + e3[1] * e3[1] + e3[2] * e3[2];
        t.edge = std::sqrt(std::max(l1, std::max(l2, l3)));
        tris.push_back(t);
      }
    }
    if (tris.size() >= 2) {
      std::vector<char> used(tris.size(), 0);
      size_t twins = 0;
      for (size_t i = 0; i < tris.size(); ++i) {
        if (used[i]) {
          continue;
        }
        for (size_t j = i + 1; j < tris.size(); ++j) {
          if (used[j]) {
            continue;
          }
          const float dx = tris[i].c[0] - tris[j].c[0];
          const float dy = tris[i].c[1] - tris[j].c[1];
          const float dz = tris[i].c[2] - tris[j].c[2];
          const float d2 = dx * dx + dy * dy + dz * dz;
          // Plane separation along i's normal must be small (back-to-back
          // sheets); in-plane offset up to the triangle scale (opposite
          // diagonal splits put twin centroids half a quad apart).
          const float along = std::fabs(dx * tris[i].n[0] + dy * tris[i].n[1] +
                                        dz * tris[i].n[2]);
          const float lat_limit = 0.5f * (tris[i].edge + tris[j].edge);
          if (along > 0.05f || d2 - along * along > lat_limit * lat_limit) {
            continue;
          }
          const float dot = tris[i].n[0] * tris[j].n[0] + tris[i].n[1] * tris[j].n[1] +
                            tris[i].n[2] * tris[j].n[2];
          if (dot < -0.9f) {
            used[i] = 1;
            used[j] = 1;
            twins += 2;
            break;
          }
        }
      }
      // Fully twinned sheets (banners ~100%), OR a meaningful twin patch
      // inside a larger single-sided mesh (harbor sailboats: the twinned
      // SAIL rides a hull/mast mesh at 4-8% twins; the coincident copies
      // sit below far-field depth precision and z-fight into per-frame
      // lit/dark shimmer at range). The twins themselves are the evidence
      // the mesh was authored for culling-on; coincident opposite-normal
      // copies would z-fight on console too, so cull the whole mesh.
      // EXCEPT alpha-tested foliage (tree fams 9/10, envsimple.alphatest 7,
      // alphatest dynobj, transparent, and unclassified fam 0): a z-fight
      // between twinned leaf cards is invisible through the alpha test, so
      // twins do NOT prove culling-on there; culling stripped every
      // single-sided leaf/branch card seen from its back (the missing-
      // foliage regression). Those keep the strict fully-twinned rule.
      const bool partial_rule_ok =
          !item.transparent && item.dynobj != 2 && item.env_family != 0 &&
          item.env_family != 7 && item.env_family != 9 &&
          item.env_family != 10 && item.env_family != 13;
      two_sided = twins * 10 >= tris.size() * 6 ||
                  (partial_rule_ok && twins >= 12 && twins * 33 >= tris.size());
      if (two_sided) {
        static std::atomic<uint32_t> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
          REXLOG_INFO("native-scene: two-sided sheet mesh {:08X} ({} indices, {}/{} twins) -> cull-back",
                      item.mesh, item.ib_count, twins, tris.size());
        }
      }
    }
  }
  if (retain_raytracing) {
    out.raytracing_indices.assign(dst_ib, dst_ib + item.ib_count);
  }
  device->Unmap(ib);

  out.vb = vb;
  out.ib = ib;
  out.vb_view = {vb, 0, num_verts * 56u, 56u};
  out.ib_view = {ib, 0, item.ib_count * 2u};
  out.two_sided_sheet = two_sided;
  return true;
}

// Decode a guest texture from its 6 fetch-constant words (host-endian),
// the v1-verified path: CPU untile block by block through the 0xA0000000
// physical mirror, endian swap, and create its SRV view.
// The 3D path reads the words from renderengine::Texture objects; the 2D
// path passes the device fetch-shadow words directly.
// BC1/DXT1 block decode (both color modes) into 16 RGBA8 texels.
void DecodeBc1Block(const uint8_t* b, uint8_t px[16][4]) {
  const uint16_t c0 = uint16_t(b[0] | (b[1] << 8));
  const uint16_t c1 = uint16_t(b[2] | (b[3] << 8));
  uint8_t col[4][4];
  // Endpoint expansion by BIT REPLICATION, what GPU hardware does. The
  // previous integer-division expansion (v*255/31) reads up to 1/255 dark;
  // on the reflective glass that error, folded through the constant detail
  // texture, tilted every reflection ~0.8 deg (found empirically via the
  // F12 trim sliders: the hand-matched values equaled the
  // replication expansion exactly).
  const auto expand = [](uint16_t c, uint8_t* o) {
    const uint32_t r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
    o[0] = uint8_t((r5 << 3) | (r5 >> 2));
    o[1] = uint8_t((g6 << 2) | (g6 >> 4));
    o[2] = uint8_t((b5 << 3) | (b5 >> 2));
    o[3] = 255;
  };
  expand(c0, col[0]);
  expand(c1, col[1]);
  if (c0 > c1) {
    for (int k = 0; k < 3; ++k) {
      col[2][k] = uint8_t((2 * col[0][k] + col[1][k]) / 3);
      col[3][k] = uint8_t((col[0][k] + 2 * col[1][k]) / 3);
    }
    col[2][3] = 255;
    col[3][3] = 255;
  } else {
    for (int k = 0; k < 3; ++k) {
      col[2][k] = uint8_t((col[0][k] + col[1][k]) / 2);
      col[3][k] = 0;
    }
    col[2][3] = 255;
    col[3][3] = 0;
  }
  uint32_t bits =
      uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) | (uint32_t(b[7]) << 24);
  for (int i = 0; i < 16; ++i) {
    std::memcpy(px[i], col[bits & 3u], 4);
    bits >>= 2;
  }
}

// Runtime-composed textures (lightmap atlas pages above all) ship with NO
// mip chain; sampling their razor-contrast mip 0 under minification
// shimmers, grazing-angle banner posters, distant thin geometry, because
// the pixel footprint spans many texels the aniso sampler cannot average at
// mip 0 alone. The game masked this with its 720p softness and the 4-tap
// lightmap filter; at native 4K we need real prefiltering. Small no-mip
// DXT1/8888 textures are decoded to RGBA8 and uploaded with a CPU
// box-filtered mip chain instead. Returns false to fall back to the plain
// single-mip path.
bool UploadGeneratedMips(const NativeGuestOutputRenderContext& context, uint8_t* base,
                         const rex::graphics::TextureInfo& info, uint32_t fetch_swizzle,
                         GuestTexture& out) {
  const rex::graphics::FormatInfo* format_info = info.format_info();
  const uint32_t bytes_per_block = format_info->bytes_per_block();
  const uint32_t bytes_per_block_log2 = uint32_t(std::countr_zero(bytes_per_block));
  const uint32_t width = info.width + 1u;
  const uint32_t height = info.height + 1u;
  const uint32_t block_w = format_info->block_width;
  const uint32_t block_h = format_info->block_height;
  const bool bc1 =
      rex::graphics::GetBaseFormat(info.format) == xenos::TextureFormat::k_DXT1;

  // Guest mip 0 copy: same packed-base + tiled macro-row padding rules as
  // the plain path.
  uint32_t ox = 0, oy = 0;
  const uint32_t addr = info.GetMipLocation(0, &ox, &oy, true);
  if (addr == 0) {
    return false;
  }
  const uint32_t pitch_blocks = info.extent.block_pitch_h;
  const uint32_t min_size = info.memory.base_size;
  uint32_t size = min_size;
  const uint32_t cols = (width + block_w - 1) / block_w;
  const uint32_t rows = (height + block_h - 1) / block_h;
  if (info.is_tiled) {
    const uint32_t padded_rows = ((rows + oy) + 31u) & ~31u;
    size = std::max(size, padded_rows * pitch_blocks * bytes_per_block);
  }
  static thread_local std::vector<uint8_t> gen_scratch;
  gen_scratch.resize(size);
  if (!GuestTryCopy(gen_scratch.data(), base + (0xA0000000u | addr), size)) {
    if (min_size >= size ||
        !GuestTryCopy(gen_scratch.data(), base + (0xA0000000u | addr), min_size)) {
      return false;
    }
    size = min_size;
  }

  // Untile into linear block rows, endian-swap per row. Run-copy untiling:
  // same contiguity rule as the plain path (see EnsureGuestTextureFromWords):
  // aligned x-runs of clamp(16 >> bpb_log2, 1, 8) blocks are contiguous.
  std::vector<uint8_t> linear(size_t(cols) * rows * bytes_per_block);
  const uint32_t run_blocks = std::clamp(16u >> bytes_per_block_log2, 1u, 8u);
  for (uint32_t by = 0; by < rows; ++by) {
    uint8_t* out_row = linear.data() + size_t(by) * cols * bytes_per_block;
    if (!info.is_tiled) {
      const uint32_t row_off = ((by + oy) * pitch_blocks + ox) * bytes_per_block;
      const uint32_t row_bytes = cols * bytes_per_block;
      if (row_off + row_bytes <= size) {
        std::memcpy(out_row, gen_scratch.data() + row_off, row_bytes);
      } else {
        for (uint32_t bx = 0; bx < cols; ++bx) {
          const uint32_t off = row_off + bx * bytes_per_block;
          if (off + bytes_per_block > size) {
            std::memset(out_row + size_t(bx) * bytes_per_block, 0, bytes_per_block);
            continue;
          }
          std::memcpy(out_row + size_t(bx) * bytes_per_block, gen_scratch.data() + off,
                      bytes_per_block);
        }
      }
    } else {
      uint32_t bx = 0;
      while (bx < cols) {
        const uint32_t x = bx + ox;
        const uint32_t run = std::min(cols - bx, run_blocks - (x & (run_blocks - 1)));
        const uint32_t off = uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
            int32_t(x), int32_t(by + oy), pitch_blocks, bytes_per_block_log2));
        const uint32_t bytes = run * bytes_per_block;
        if (off + bytes <= size) {
          std::memcpy(out_row + size_t(bx) * bytes_per_block, gen_scratch.data() + off,
                      bytes);
        } else {
          for (uint32_t i = 0; i < run; ++i) {
            const uint32_t boff = off + i * bytes_per_block;
            if (boff + bytes_per_block > size) {
              std::memset(out_row + size_t(bx + i) * bytes_per_block, 0, bytes_per_block);
            } else {
              std::memcpy(out_row + size_t(bx + i) * bytes_per_block,
                          gen_scratch.data() + boff, bytes_per_block);
            }
          }
        }
        bx += run;
      }
    }
    SwapGuestEndian(out_row, cols * bytes_per_block, info.endianness);
  }

  // Decode to RGBA8 mip 0.
  std::vector<uint8_t> rgba(size_t(width) * height * 4);
  if (bc1) {
    for (uint32_t by = 0; by < rows; ++by) {
      for (uint32_t bx = 0; bx < cols; ++bx) {
        uint8_t px[16][4];
        DecodeBc1Block(linear.data() + (size_t(by) * cols + bx) * bytes_per_block, px);
        for (uint32_t t = 0; t < 16; ++t) {
          const uint32_t x = bx * 4 + (t & 3u);
          const uint32_t y = by * 4 + (t >> 2);
          if (x < width && y < height) {
            std::memcpy(&rgba[(size_t(y) * width + x) * 4], px[t], 4);
          }
        }
      }
    }
  } else {  // k_8_8_8_8: rows are already RGBA8 after the endian swap
    for (uint32_t y = 0; y < height; ++y) {
      std::memcpy(&rgba[size_t(y) * width * 4], linear.data() + size_t(y) * cols * 4,
                  size_t(width) * 4);
    }
  }

  // Diagnostic dump: what the RUNTIME decoded, for an offline byte diff
  // against gsnap_tex_decode of the same fetch words (the reflective_trans
  // glass panels' wrong-brightness hunt: their composed lightmap pages take
  // exactly this generated-mips path).
  if (REXCVAR_GET(skate3_native_render_scene_lm_dump)) {
    char path[260];
    std::snprintf(path, sizeof(path),
                  "native_texture_dumps/gen_%08X_%ux%u_t%u.rgba",
                  info.memory.base_address, width, height,
                  info.is_tiled ? 1u : 0u);
    if (FILE* f = std::fopen(path, "wb")) {
      std::fwrite(rgba.data(), 1, rgba.size(), f);
      std::fclose(f);
    }
  }

  // Box-filtered chain down to 1x1.
  std::vector<std::vector<uint8_t>> mips;
  mips.emplace_back(std::move(rgba));
  uint32_t mw = width, mh = height;
  while (mw > 1 || mh > 1) {
    const uint32_t nw = std::max(mw >> 1, 1u);
    const uint32_t nh = std::max(mh >> 1, 1u);
    const std::vector<uint8_t>& srcm = mips.back();
    std::vector<uint8_t> dstm(size_t(nw) * nh * 4);
    for (uint32_t y = 0; y < nh; ++y) {
      const uint32_t y0 = std::min(y * 2, mh - 1);
      const uint32_t y1 = std::min(y * 2 + 1, mh - 1);
      for (uint32_t x = 0; x < nw; ++x) {
        const uint32_t x0 = std::min(x * 2, mw - 1);
        const uint32_t x1 = std::min(x * 2 + 1, mw - 1);
        for (int k = 0; k < 4; ++k) {
          const uint32_t sum = srcm[(size_t(y0) * mw + x0) * 4 + k] +
                               srcm[(size_t(y0) * mw + x1) * 4 + k] +
                               srcm[(size_t(y1) * mw + x0) * 4 + k] +
                               srcm[(size_t(y1) * mw + x1) * 4 + k];
          dstm[(size_t(y) * nw + x) * 4 + k] = uint8_t((sum + 2) / 4);
        }
      }
    }
    mips.emplace_back(std::move(dstm));
    mw = nw;
    mh = nh;
  }
  const uint32_t mip_count = uint32_t(mips.size());

  // Upload plan + resource (RGBA8).
  struct Plan {
    uint32_t offset, pitch, w, h;
  };
  std::vector<Plan> plans(mip_count);
  uint32_t upload_size = 0;
  for (uint32_t m = 0; m < mip_count; ++m) {
    Plan& p = plans[m];
    p.w = std::max(width >> m, 1u);
    p.h = std::max(height >> m, 1u);
    p.pitch = (p.w * 4u + (nrhi::kRowPitchAlignment - 1u)) &
              ~(nrhi::kRowPitchAlignment - 1u);
    p.offset = (upload_size + (kUploadPlacementAlignment - 1u)) &
               ~(kUploadPlacementAlignment - 1u);
    upload_size = p.offset + p.pitch * p.h;
  }
  nrhi::Device* device = context.device;
  nrhi::TextureDesc desc;
  desc.kind = nrhi::TextureKind::k2D;
  desc.width = width;
  desc.height = height;
  desc.mip_levels = mip_count;
  desc.format = nrhi::Format::kR8G8B8A8_UNORM;
  desc.initial_state = nrhi::ResourceState::kCopyDest;
  out.texture = device->CreateTexture(desc);
  if (out.texture == nullptr) {
    return false;
  }
  out.upload = CreateUploadBuffer(device, upload_size, nrhi::BufferBindClass::kCopySrc);
  if (!out.upload) {
    device->DestroyDeferred(out.texture);
    out.texture = nullptr;
    return false;
  }
  uint8_t* mapping = static_cast<uint8_t*>(device->Map(out.upload));
  for (uint32_t m = 0; m < mip_count; ++m) {
    const Plan& p = plans[m];
    for (uint32_t y = 0; y < p.h; ++y) {
      std::memcpy(mapping + p.offset + size_t(y) * p.pitch, &mips[m][size_t(y) * p.w * 4],
                  size_t(p.w) * 4);
    }
  }
  device->Unmap(out.upload);

  if (g_tex_stage_out != nullptr) {
    // Decode worker: export the commit recipe; the render thread records
    // the copies + barrier and creates the SRV (CommitStagedGuestTexture).
    StagedTexCommit& sc = *g_tex_stage_out;
    sc.copy_format = nrhi::Format::kR8G8B8A8_UNORM;
    sc.srv_format = nrhi::Format::kR8G8B8A8_UNORM;
    ComposeSrvSwizzle(fetch_swizzle, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, sc.swizzle);
    sc.mip_count = std::min<uint32_t>(mip_count, 16);
    for (uint32_t m = 0; m < sc.mip_count; ++m) {
      sc.mips[m] = {plans[m].offset, plans[m].pitch, plans[m].w, plans[m].h};
    }
  } else {
    for (uint32_t m = 0; m < mip_count; ++m) {
      const Plan& p = plans[m];
      context.cmd->CopyBufferToTexture(out.texture, m, 0, out.upload, p.offset,
                                       p.pitch, p.w, p.h, 1);
    }
    context.cmd->Barrier(out.texture, nrhi::ResourceState::kCopyDest,
                         nrhi::ResourceState::kPixelShaderResource);
    // Copies recorded: release the staging buffer (deferred until the
    // submission completes); the in-place path recreates its own on demand.
    device->DestroyDeferred(out.upload);
    out.upload = nullptr;

    nrhi::TextureViewDesc srv;
    srv.dimension = nrhi::ViewDimension::k2D;
    srv.format = nrhi::Format::kR8G8B8A8_UNORM;
    ComposeSrvSwizzle(fetch_swizzle, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, srv.swizzle);
    srv.mip_levels = mip_count;
    out.srv_format = srv.format;
    out.srv_mips = mip_count;
    out.srv = device->CreateTextureView(out.texture, srv);
    if (out.srv == nullptr) {
      return false;
    }
  }
  out.payload_addr = 0xA0000000u | info.memory.base_address;
  out.payload_size = size;
  BuildPayloadProbes(info, addr, ox, oy, pitch_blocks, size, out);
  out.payload_fp = SampleProbeFingerprint(base, out);
  out.near_black = SampleProbeNearBlack(base, out);
  out.recheck_frame = 0;
  out.valid = g_tex_stage_out == nullptr;  // staged: live only after commit
  return true;
}

// Phase attribution for the SLOW-inline-decode log: time inside RHI
// resource creation / the generated-mips path / the guest scratch copies,
// reset at each EnsureGuestTextureFromWords entry (per thread: workers and
// the render thread decode concurrently).
thread_local uint64_t g_tex_dec_create_ns = 0;
thread_local uint64_t g_tex_dec_gen_ns = 0;
thread_local uint64_t g_tex_dec_copy_ns = 0;

bool EnsureGuestTextureFromWords(const NativeGuestOutputRenderContext& context,
                                 uint8_t* base, const uint32_t words[6],
                                 GuestTexture& out) {
  g_tex_dec_create_ns = 0;
  g_tex_dec_gen_ns = 0;
  g_tex_dec_copy_ns = 0;
  std::memcpy(out.fetch_words, words, 6 * sizeof(uint32_t));

  xenos::xe_gpu_texture_fetch_t fetch = {};
  fetch.dword_0 = words[0];
  fetch.dword_1 = words[1];
  fetch.dword_2 = words[2];
  fetch.dword_3 = words[3];
  fetch.dword_4 = words[4];
  fetch.dword_5 = words[5];
  if (fetch.type != xenos::FetchConstantType::kTexture || fetch.base_address == 0) {
    return false;
  }
  // Pre-validate the raw format BEFORE Prepare: TextureInfo::Prepare calls
  // TextureExtent::Calculate, which divides by the format table's
  // block_width/block_height/bytes_per_block; a garbage fetch constant
  // (freed texture object read mid-teardown during a map change) carries a
  // format whose table entry has zeros and crashed a prewarm worker with an
  // integer divide-by-zero (dump skate3.exe.pre-icon.24004, second load
  // into Daly Estates).
  const rex::graphics::FormatInfo* pre_fi =
      rex::graphics::FormatInfo::Get(uint32_t(fetch.format));
  if (pre_fi == nullptr || pre_fi->block_width == 0 || pre_fi->block_height == 0 ||
      pre_fi->bytes_per_block() == 0) {
    return false;
  }
  rex::graphics::TextureInfo info;
  if (!rex::graphics::TextureInfo::Prepare(fetch, &info)) {
    return false;
  }
  HostTextureFormat host;
  if (!GetHostTextureFormat(info.format, host)) {
    return false;
  }
  if (info.dimension != xenos::DataDimension::k2DOrStacked || info.is_stacked ||
      info.width >= 8192 || info.height >= 8192 || info.memory.base_address == 0 ||
      info.memory.base_size == 0 || info.memory.base_size > 64u * 1024u * 1024u) {
    return false;
  }

  const rex::graphics::FormatInfo* format_info = info.format_info();
  const uint32_t bytes_per_block = format_info->bytes_per_block();
  if (bytes_per_block == 0 || (bytes_per_block & (bytes_per_block - 1)) != 0) {
    return false;
  }
  const uint32_t bytes_per_block_log2 = uint32_t(std::countr_zero(bytes_per_block));
  const uint32_t width = info.width + 1u;
  const uint32_t height = info.height + 1u;
  const uint32_t block_w = format_info->block_width;
  const uint32_t block_h = format_info->block_height;
  const uint32_t host_width = ((width + block_w - 1) / block_w) * block_w;
  const uint32_t host_height = ((height + block_h - 1) / block_h) * block_h;

  // Upload the guest MIP CHAIN, not just mip 0; sampling mip 0 at distance
  // is the source of the grass "TV static" and flickering floor/window
  // lines. Power-of-two sizes only (everything the game ships) so BC block
  // alignment holds on every level.
  const bool pow2 = (width & (width - 1)) == 0 && (height & (height - 1)) == 0;
  uint32_t mip_count = 1;
  if (pow2 && info.memory.mip_address != 0 &&
      REXCVAR_GET(skate3_native_render_scene_tex_mips)) {
    const uint32_t avail = std::min(info.mip_levels(), info.GetMaxMipLevels());
    while (mip_count < avail && (width >> mip_count) >= 4 && (height >> mip_count) >= 4) {
      uint32_t ox = 0, oy = 0;
      if (info.GetMipLocation(mip_count, &ox, &oy, true) == 0) {
        break;
      }
      ++mip_count;
    }
  }
  // No guest chain at all (runtime-composed lightmap pages): generate one,
  // see UploadGeneratedMips. Small DXT1/8888 textures only; falls back to
  // the plain single-mip path on any failure.
  if (mip_count == 1 && pow2 && REXCVAR_GET(skate3_native_render_scene_tex_mips) &&
      width >= 8 && height >= 8 && width <= 512 && height <= 512) {
    const auto base_fmt = rex::graphics::GetBaseFormat(info.format);
    if (base_fmt == xenos::TextureFormat::k_DXT1 ||
        base_fmt == xenos::TextureFormat::k_8_8_8_8) {
      GuestTexture gen = out;  // keeps fetch_words already copied
      const auto gen_t0 = PerfClock::now();
      const bool gen_ok = UploadGeneratedMips(context, base, info, fetch.swizzle, gen);
      g_tex_dec_gen_ns += PerfNsSince(gen_t0);
      if (gen_ok) {
        out = gen;
        return true;
      }
    }
  }

  // Copy the whole guest mip chain out up front with the lock-free guarded
  // copy (never VirtualQuery on the render thread: the VAD lock stalls
  // behind the guest streaming threads exactly while panning streams
  // textures in), truncating the chain at the first unreadable level.
  struct MipSrc {
    uint32_t addr, scratch_off, size, min_size, pitch_blocks, ox, oy;
  };
  MipSrc srcs[16] = {};
  static thread_local std::vector<uint8_t> tex_scratch;
  uint32_t scratch_total = 0;
  for (uint32_t m = 0; m < mip_count; ++m) {
    uint32_t ox = 0, oy = 0;
    // Mip 0 through GetMipLocation too: textures <= 16 texels on a side
    // store their BASE level packed inside a 32x32 tile at a block offset.
    // Reading at (0,0) decoded garbage; the 16x16 "default_white" macro
    // overlay came out as pink/black blocks and multiplied giant soft black
    // blobs over every wall whose material uses it (validated: with the
    // packed offset it decodes pure white). Non-packed textures return the
    // plain base address with zero offsets.
    const uint32_t mip_addr = info.GetMipLocation(m, &ox, &oy, true);
    const auto ext = info.GetMipExtent(m, true);
    MipSrc& s = srcs[m];
    s.addr = mip_addr;
    s.pitch_blocks = m == 0 ? info.extent.block_pitch_h : ext.block_pitch_h;
    s.size = m == 0 ? info.memory.base_size : ext.all_blocks() * bytes_per_block;
    s.min_size = s.size;
    if (info.is_tiled) {
      // Tiled addressing swizzles across 32x32-BLOCK macro tiles AND, for
      // narrow block formats, interleaves across 64x64/128x128-block
      // portions whose byte extent EXCEEDS the linear size; 16bpp reaches
      // 0xC00 bytes from a 32x32 tile origin vs the naive 32*32*2 = 0x800.
      // The previous padded-macro-ROW estimate (added for the 32x32 DXT5
      // HUD compass icons, whose last block sat at 5952 vs base_size 1024)
      // missed that interleave: every 16bpp mip's swizzled offsets for the
      // BOTTOM half of its rows landed past the estimate, the range guard
      // zeroed them, and every PCU Library banner rendered with its lower
      // half black at mip-1 viewing distance (MIP DIAG: guard_zeroed
      // exactly total/2 on 64x64 fmt-4 mips, total/1 on packed oy=16 mips).
      // Size the copy with the SDK's swizzle-aware upper bound instead; if
      // that over-reaches the committed allocation the copy loop below
      // falls back to the reported size and marks the decode incomplete.
      const uint32_t mw = std::max(width >> m, 1u);
      const uint32_t mh = std::max(height >> m, 1u);
      const uint32_t right = (mw + block_w - 1) / block_w + ox;
      const uint32_t bottom = (mh + block_h - 1) / block_h + oy;
      s.size = std::max(
          s.size, rex::graphics::texture_util::GetTiledAddressUpperBound2D(
                      right, bottom, s.pitch_blocks, bytes_per_block_log2));
    }
    s.ox = ox;
    s.oy = oy;
    s.scratch_off = scratch_total;
    scratch_total += s.size;
  }
  const auto copy_t0 = PerfClock::now();
  tex_scratch.resize(scratch_total);
  uint32_t mips_copied = 0;
  bool copy_truncated = false;
  for (uint32_t m = 0; m < mip_count; ++m) {
    MipSrc& s = srcs[m];
    if (!GuestTryCopy(tex_scratch.data() + s.scratch_off,
                      base + (0xA0000000u | s.addr), s.size)) {
      if (s.min_size >= s.size ||
          !GuestTryCopy(tex_scratch.data() + s.scratch_off,
                        base + (0xA0000000u | s.addr), s.min_size)) {
        break;
      }
      // Tiled fallback: the padded macro rows hold real blocks past
      // min_size; every one of them uploads as ZERO below (the PCU
      // Library half-black banner mips). The decode is marked incomplete
      // so the draw path keeps retrying until the pool commits.
      s.size = s.min_size;
      copy_truncated = true;
    }
    ++mips_copied;
  }
  g_tex_dec_copy_ns += PerfNsSince(copy_t0);
  if (mips_copied == 0) {
    return false;
  }
  mip_count = mips_copied;
  out.incomplete = copy_truncated;
  if (copy_truncated) {
    static std::atomic<uint32_t> s_trunc_logs{0};
    if (s_trunc_logs.fetch_add(1, std::memory_order_relaxed) < 16) {
      REXLOG_INFO(
          "native-scene: texture decode INCOMPLETE (tiled mip copy fell back "
          "to min_size: zeroed tail blocks) {}x{} mips={} w1={:08X}; will "
          "re-decode until complete",
          width, height, mip_count, words[1]);
    }
  }

  // Per-mip upload footprints (D3D12 alignment rules).
  struct MipPlan {
    uint32_t offset, pitch, cols, rows;
  };
  MipPlan plans[16] = {};
  uint32_t upload_size = 0;
  for (uint32_t m = 0; m < mip_count; ++m) {
    const uint32_t mw = std::max(width >> m, 1u);
    const uint32_t mh = std::max(height >> m, 1u);
    MipPlan& p = plans[m];
    p.cols = (mw + block_w - 1) / block_w;
    p.rows = (mh + block_h - 1) / block_h;
    p.pitch = (p.cols * bytes_per_block + (nrhi::kRowPitchAlignment - 1u)) &
              ~(nrhi::kRowPitchAlignment - 1u);
    p.offset = (upload_size + (kUploadPlacementAlignment - 1u)) &
               ~(kUploadPlacementAlignment - 1u);
    upload_size = p.offset + p.pitch * p.rows;
  }

  nrhi::Device* device = context.device;
  nrhi::TextureDesc desc;
  desc.kind = nrhi::TextureKind::k2D;
  desc.width = host_width;
  desc.height = host_height;
  desc.mip_levels = mip_count;
  desc.format = host.resource_format;
  desc.initial_state = nrhi::ResourceState::kCopyDest;
  const auto create_t0 = PerfClock::now();
  out.texture = device->CreateTexture(desc);
  if (out.texture == nullptr) {
    return false;
  }
  out.upload = CreateUploadBuffer(device, upload_size, nrhi::BufferBindClass::kCopySrc);
  g_tex_dec_create_ns += PerfNsSince(create_t0);
  if (!out.upload) {
    device->DestroyDeferred(out.texture);
    out.texture = nullptr;
    return false;
  }
  uint8_t* mapping = static_cast<uint8_t*>(device->Map(out.upload));
  const bool swap_rb_565 =
      rex::graphics::GetBaseFormat(info.format) == xenos::TextureFormat::k_5_6_5;
  for (uint32_t m = 0; m < mip_count; ++m) {
    const MipPlan& p = plans[m];
    const uint32_t ox = srcs[m].ox;
    const uint32_t oy = srcs[m].oy;
    const uint32_t src_pitch_blocks = srcs[m].pitch_blocks;
    const uint32_t src_size = srcs[m].size;
    const uint8_t* guest = tex_scratch.data() + srcs[m].scratch_off;
    const uint32_t row_bytes = p.cols * bytes_per_block;
    uint32_t guard_zeroed = 0;  // blocks zeroed by the range guard (diag)
    // Run-copy untiling. The per-BLOCK GetTiledOffset2D loop made a single
    // 1280x720 8888 decode cost 3-13 ms (921k address computations + 4-byte
    // memcpys); animating fullscreen menu art and FMV planes pay that on
    // EVERY content change, which is where the menu frame spikes lived.
    // From the tiling formula (util.cpp GetTiledOffset2D): within a row, an
    // x-run aligned to run_blocks = clamp(16 >> bpb_log2, 1, 8) maps to
    // CONTIGUOUS byte offsets; the micro term stays inside its 16-byte
    // nibble group ((y & 0xE) contributes only multiples of 16 at every
    // bpb), and runs <= 8 aligned blocks never cross the (x>>3)/(x>>5) mix
    // boundaries. Linear mips copy whole rows.
    //
    // ROWS STAGE THROUGH A CACHED BUFFER, never the upload mapping directly:
    // upload heaps are WRITE-COMBINED memory, and SwapGuestEndian/565-swap
    // READ the row back; uncached WC reads cost ~33 ns/byte, which was the
    // long-unattributed 35-135 ms decode class (512x512 = 1 MB = 34 ms,
    // 1024x1024+chain = 135 ms; linear fit across a slow-decode log).
    // The mapping gets exactly one streaming memcpy per row.
    const uint32_t run_blocks =
        std::clamp(16u >> bytes_per_block_log2, 1u, 8u);
    static thread_local std::vector<uint8_t> row_buf;
    row_buf.resize(row_bytes);
    for (uint32_t by = 0; by < p.rows; ++by) {
      uint8_t* out_row = row_buf.data();
      if (!info.is_tiled) {
        const uint32_t row_off =
            ((by + oy) * src_pitch_blocks + ox) * bytes_per_block;
        if (row_off + row_bytes <= src_size) {
          std::memcpy(out_row, guest + row_off, row_bytes);
        } else {
          for (uint32_t bx = 0; bx < p.cols; ++bx) {
            const uint32_t off = row_off + bx * bytes_per_block;
            if (off + bytes_per_block > src_size) {
              std::memset(out_row + size_t(bx) * bytes_per_block, 0, bytes_per_block);
              ++guard_zeroed;
              continue;
            }
            std::memcpy(out_row + size_t(bx) * bytes_per_block, guest + off,
                        bytes_per_block);
          }
        }
      } else {
        uint32_t bx = 0;
        while (bx < p.cols) {
          const uint32_t x = bx + ox;
          const uint32_t run =
              std::min(p.cols - bx, run_blocks - (x & (run_blocks - 1)));
          const uint32_t off = uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
              int32_t(x), int32_t(by + oy), src_pitch_blocks, bytes_per_block_log2));
          const uint32_t bytes = run * bytes_per_block;
          if (off + bytes <= src_size) {
            std::memcpy(out_row + size_t(bx) * bytes_per_block, guest + off, bytes);
          } else {
            // Padded macro rows past the copied size: per-block guard-zero
            // (offsets within the run are contiguous, same proof as above).
            for (uint32_t i = 0; i < run; ++i) {
              const uint32_t boff = off + i * bytes_per_block;
              if (boff + bytes_per_block > src_size) {
                std::memset(out_row + size_t(bx + i) * bytes_per_block, 0,
                            bytes_per_block);
                ++guard_zeroed;
              } else {
                std::memcpy(out_row + size_t(bx + i) * bytes_per_block, guest + boff,
                            bytes_per_block);
              }
            }
          }
          bx += run;
        }
      }
      SwapGuestEndian(out_row, row_bytes, info.endianness);
      if (swap_rb_565) {
        for (uint32_t i = 0; i + 2 <= row_bytes; i += 2) {
          uint16_t value;
          std::memcpy(&value, out_row + i, sizeof(value));
          value = uint16_t((value & 0x07E0u) | ((value >> 11) & 0x1Fu) |
                           ((value & 0x1Fu) << 11));
          std::memcpy(out_row + i, &value, sizeof(value));
        }
      }
      std::memcpy(mapping + p.offset + size_t(by) * p.pitch, out_row, row_bytes);
    }
    // Half-black-mip diagnostic (PCU Library banners): discriminate "the
    // guest pool genuinely holds zeros for this mip" from "our addressing
    // zeroed/misread it". Samples 32 uploaded blocks spread over the mip;
    // guard_zeroed separates range-guard zeroing from zero CONTENT.
    if (m > 0) {
      uint32_t zero_samples = 0;
      const uint32_t total_blocks = p.rows * p.cols;
      for (uint32_t s = 0; s < 32; ++s) {
        const uint32_t bi = uint32_t(uint64_t(total_blocks - 1) * s / 31u);
        const uint32_t by = bi / p.cols;
        const uint32_t bx = bi % p.cols;
        uint64_t q = 0;
        std::memcpy(&q, mapping + p.offset + size_t(by) * p.pitch +
                            size_t(bx) * bytes_per_block,
                    std::min<uint32_t>(8, bytes_per_block));
        zero_samples += q == 0 ? 1 : 0;
      }
      if (total_blocks >= 32 && (guard_zeroed * 4 >= total_blocks ||
                                 zero_samples >= 12)) {
        static std::atomic<uint32_t> s_mip_diag{0};
        if (s_mip_diag.fetch_add(1, std::memory_order_relaxed) < 24) {
          REXLOG_INFO(
              "native-scene: MIP DIAG {}x{} mip {}/{} zero_samples={}/32 "
              "guard_zeroed={}/{} ox={} oy={} pitch_b={} size={} min={} "
              "tiled={} fmt={} w0={:08X} w1={:08X} w2={:08X} mip_addr={:08X}",
              width, height, m, mip_count, zero_samples, guard_zeroed,
              total_blocks, ox, oy, src_pitch_blocks, src_size,
              srcs[m].min_size, info.is_tiled ? 1 : 0, uint32_t(info.format),
              words[0], words[1], words[2], srcs[m].addr);
        }
      }
    }
  }
  // Diagnostic dump (skate3_native_render_scene_lm_dump): no-chain textures
  // through the PLAIN path (the >512px composed lightmap pages the
  // generated-mips gate excludes): mip 0 as linear block rows, raw guest
  // block format post-endian-swap, for offline decode + byte-diff against
  // gsnap_tex_decode of the same fetch words.
  if (mip_count == 1 && REXCVAR_GET(skate3_native_render_scene_lm_dump)) {
    char path[260];
    std::snprintf(path, sizeof(path),
                  "native_texture_dumps/plain_%08X_%ux%u_f%u_t%u.blk",
                  info.memory.base_address, width, height,
                  uint32_t(info.format), info.is_tiled ? 1u : 0u);
    if (FILE* f = std::fopen(path, "wb")) {
      const MipPlan& p0 = plans[0];
      for (uint32_t by = 0; by < p0.rows; ++by) {
        std::fwrite(mapping + p0.offset + size_t(by) * p0.pitch, 1,
                    size_t(p0.cols) * bytes_per_block, f);
      }
      std::fclose(f);
    }
  }
  device->Unmap(out.upload);

  if (g_tex_stage_out != nullptr) {
    // Decode worker: export the commit recipe; the render thread records
    // the copies + barrier and creates the SRV (CommitStagedGuestTexture).
    StagedTexCommit& sc = *g_tex_stage_out;
    sc.copy_format = host.resource_format;
    sc.srv_format = host.srv_format;
    ComposeSrvSwizzle(fetch.swizzle, host.host_swizzle, sc.swizzle);
    sc.mip_count = std::min<uint32_t>(mip_count, 16);
    for (uint32_t m = 0; m < sc.mip_count; ++m) {
      sc.mips[m] = {plans[m].offset, plans[m].pitch, std::max(host_width >> m, 1u),
                    std::max(host_height >> m, 1u)};
    }
  } else {
    // Record the upload copies into the deferred command list.
    for (uint32_t m = 0; m < mip_count; ++m) {
      const MipPlan& p = plans[m];
      context.cmd->CopyBufferToTexture(out.texture, m, 0, out.upload, p.offset,
                                       p.pitch, std::max(host_width >> m, 1u),
                                       std::max(host_height >> m, 1u), 1);
    }
    context.cmd->Barrier(out.texture, nrhi::ResourceState::kCopyDest,
                         nrhi::ResourceState::kPixelShaderResource);
    // Copies recorded: release the staging buffer (deferred until the
    // submission completes); the in-place path recreates its own on demand.
    device->DestroyDeferred(out.upload);
    out.upload = nullptr;

    // SRV view with the composed swizzle.
    nrhi::TextureViewDesc srv;
    srv.dimension = nrhi::ViewDimension::k2D;
    srv.format = host.srv_format;
    ComposeSrvSwizzle(fetch.swizzle, host.host_swizzle, srv.swizzle);
    srv.mip_levels = mip_count;
    out.srv_format = srv.format;
    out.srv_mips = mip_count;
    out.srv = device->CreateTextureView(out.texture, srv);
    if (out.srv == nullptr) {
      return false;
    }
  }
  // Payload sample for content revalidation (see GuestTexture).
  out.payload_addr = 0xA0000000u | info.memory.base_address;
  out.payload_size = srcs[0].size;
  BuildPayloadProbes(info, srcs[0].addr, srcs[0].ox, srcs[0].oy,
                     srcs[0].pitch_blocks, srcs[0].size, out);
  out.payload_fp = SampleProbeFingerprint(base, out);
  out.near_black = SampleProbeNearBlack(base, out);
  out.recheck_frame = 0;
  out.valid = g_tex_stage_out == nullptr;  // staged: live only after commit
  return true;
}

// (The object-keyed EnsureGuestTexture wrapper is gone: everything decodes
// from an explicit stable words snapshot, ReadStableTexWords +
// EnsureGuestTextureFromWords, and lands in the words-keyed store.)

// In-place re-decode for HOT single-mip 2D content (video planes, animating
// fullscreen menu/loading art): identical fetch words = identical layout, so
// the committed texture, upload footprint and SRV slot are all reusable;
// the retire+recreate path paid two GPU resource creations per
// content change (~90/s during FMV playback, plus every animation step of
// fullscreen frontend art). Non-pow2 single-mip entries only: pow2 entries
// may own a real or generated mip chain (the full path handles those).
// The TEAR-GUARD also lives here: the CPU writer (VP6 video decode, APT
// re-raster) can be mid-write when the liveness probe fires; decoding a
// half-written fullscreen payload shows a frame of mixed old/new content:
// a subtle whole-screen brightness flicker on fades and video. The guest
// copy retries until the probe fingerprint reads stable across it.
bool UpdateGuestTexture2DInPlace(const NativeGuestOutputRenderContext& context,
                                 uint8_t* base, GuestTexture& t) {
  if (!t.valid || t.texture == nullptr || t.srv_mips != 1 || t.incomplete) {
    return false;
  }
  xenos::xe_gpu_texture_fetch_t fetch = {};
  fetch.dword_0 = t.fetch_words[0];
  fetch.dword_1 = t.fetch_words[1];
  fetch.dword_2 = t.fetch_words[2];
  fetch.dword_3 = t.fetch_words[3];
  fetch.dword_4 = t.fetch_words[4];
  fetch.dword_5 = t.fetch_words[5];
  if (fetch.type != xenos::FetchConstantType::kTexture || fetch.base_address == 0) {
    return false;
  }
  const rex::graphics::FormatInfo* pre_fi =
      rex::graphics::FormatInfo::Get(uint32_t(fetch.format));
  if (pre_fi == nullptr || pre_fi->block_width == 0 || pre_fi->block_height == 0 ||
      pre_fi->bytes_per_block() == 0) {
    return false;
  }
  rex::graphics::TextureInfo info;
  if (!rex::graphics::TextureInfo::Prepare(fetch, &info)) {
    return false;
  }
  HostTextureFormat host;
  if (!GetHostTextureFormat(info.format, host)) {
    return false;
  }
  if (info.dimension != xenos::DataDimension::k2DOrStacked || info.is_stacked ||
      info.width >= 8192 || info.height >= 8192 || info.memory.base_address == 0 ||
      info.memory.base_size == 0 || info.memory.base_size > 64u * 1024u * 1024u) {
    return false;
  }
  const rex::graphics::FormatInfo* format_info = info.format_info();
  const uint32_t bytes_per_block = format_info->bytes_per_block();
  if (bytes_per_block == 0 || (bytes_per_block & (bytes_per_block - 1)) != 0) {
    return false;
  }
  const uint32_t bytes_per_block_log2 = uint32_t(std::countr_zero(bytes_per_block));
  const uint32_t width = info.width + 1u;
  const uint32_t height = info.height + 1u;
  if ((width & (width - 1)) == 0 && (height & (height - 1)) == 0) {
    return false;  // pow2: may own a mip chain, full path
  }
  if (host.srv_format != t.srv_format) {
    return false;  // the existing SRV recipe must stay exact
  }
  const uint32_t block_w = format_info->block_width;
  const uint32_t block_h = format_info->block_height;

  // Mip-0 source extent, same rules as the full decode.
  uint32_t ox = 0, oy = 0;
  const uint32_t addr = info.GetMipLocation(0, &ox, &oy, true);
  if (addr == 0) {
    return false;
  }
  const uint32_t pitch_blocks = info.extent.block_pitch_h;
  const uint32_t cols = (width + block_w - 1) / block_w;
  const uint32_t rows = (height + block_h - 1) / block_h;
  const uint32_t min_size = info.memory.base_size;
  uint32_t size = min_size;
  if (info.is_tiled) {
    size = std::max(size, rex::graphics::texture_util::GetTiledAddressUpperBound2D(
                              cols + ox, rows + oy, pitch_blocks, bytes_per_block_log2));
  }
  static thread_local std::vector<uint8_t> inplace_scratch;
  inplace_scratch.resize(size);

  // Tear-guarded guest copy: retry while the writer races us.
  uint64_t fp = 0;
  for (int attempt = 0; attempt < 3; ++attempt) {
    const uint64_t fp_before = SampleProbeFingerprint(base, t);
    if (!GuestTryCopy(inplace_scratch.data(), base + (0xA0000000u | addr), size)) {
      if (min_size >= size ||
          !GuestTryCopy(inplace_scratch.data(), base + (0xA0000000u | addr), min_size)) {
        return false;  // payload unreadable: let the full path sort it out
      }
      size = min_size;
    }
    fp = SampleProbeFingerprint(base, t);
    if (fp != 0 && fp == fp_before) {
      break;
    }
  }

  // Upload footprint (single mip at offset 0).
  const uint32_t pitch =
      (cols * bytes_per_block + (nrhi::kRowPitchAlignment - 1u)) &
      ~(nrhi::kRowPitchAlignment - 1u);
  const uint32_t upload_size = pitch * rows;
  nrhi::Device* device = context.device;
  nrhi::Buffer*& up = t.upload_flip ? t.upload_b : t.upload;
  if (up != nullptr && up->size() < upload_size) {
    // Never expected (same words = same footprint): recreate defensively.
    device->DestroyDeferred(up);
    up = nullptr;
  }
  if (up == nullptr) {
    up = CreateUploadBuffer(device, upload_size, nrhi::BufferBindClass::kCopySrc);
    if (up == nullptr) {
      return false;
    }
  }
  uint8_t* mapping = static_cast<uint8_t*>(device->Map(up));
  if (mapping == nullptr) {
    return false;
  }
  const bool swap_rb_565 =
      rex::graphics::GetBaseFormat(info.format) == xenos::TextureFormat::k_5_6_5;
  const uint8_t* guest = inplace_scratch.data();
  const uint32_t row_bytes = cols * bytes_per_block;
  const uint32_t run_blocks = std::clamp(16u >> bytes_per_block_log2, 1u, 8u);
  // Rows stage through a CACHED buffer; the endian/565 swaps read the row
  // back, and reading the write-combined upload mapping costs ~33 ns/byte
  // (the 35-135 ms decode class; see the plain path).
  static thread_local std::vector<uint8_t> row_buf;
  row_buf.resize(row_bytes);
  for (uint32_t by = 0; by < rows; ++by) {
    uint8_t* out_row = row_buf.data();
    if (!info.is_tiled) {
      const uint32_t row_off = ((by + oy) * pitch_blocks + ox) * bytes_per_block;
      if (row_off + row_bytes <= size) {
        std::memcpy(out_row, guest + row_off, row_bytes);
      } else {
        for (uint32_t bx = 0; bx < cols; ++bx) {
          const uint32_t off = row_off + bx * bytes_per_block;
          if (off + bytes_per_block > size) {
            std::memset(out_row + size_t(bx) * bytes_per_block, 0, bytes_per_block);
          } else {
            std::memcpy(out_row + size_t(bx) * bytes_per_block, guest + off,
                        bytes_per_block);
          }
        }
      }
    } else {
      uint32_t bx = 0;
      while (bx < cols) {
        const uint32_t x = bx + ox;
        const uint32_t run = std::min(cols - bx, run_blocks - (x & (run_blocks - 1)));
        const uint32_t off = uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
            int32_t(x), int32_t(by + oy), pitch_blocks, bytes_per_block_log2));
        const uint32_t bytes = run * bytes_per_block;
        if (off + bytes <= size) {
          std::memcpy(out_row + size_t(bx) * bytes_per_block, guest + off, bytes);
        } else {
          for (uint32_t i = 0; i < run; ++i) {
            const uint32_t boff = off + i * bytes_per_block;
            if (boff + bytes_per_block > size) {
              std::memset(out_row + size_t(bx + i) * bytes_per_block, 0, bytes_per_block);
            } else {
              std::memcpy(out_row + size_t(bx + i) * bytes_per_block, guest + boff,
                          bytes_per_block);
            }
          }
        }
        bx += run;
      }
    }
    SwapGuestEndian(out_row, row_bytes, info.endianness);
    if (swap_rb_565) {
      for (uint32_t i = 0; i + 2 <= row_bytes; i += 2) {
        uint16_t value;
        std::memcpy(&value, out_row + i, sizeof(value));
        value = uint16_t((value & 0x07E0u) | ((value >> 11) & 0x1Fu) |
                         ((value & 0x1Fu) << 11));
        std::memcpy(out_row + i, &value, sizeof(value));
      }
    }
    std::memcpy(mapping + size_t(by) * pitch, out_row, row_bytes);
  }
  device->Unmap(up);

  // Barriers must land in the deferred list BEFORE the copy: flush both
  // sides explicitly (earlier draws this frame sample the OLD content,
  // later ones the new: same semantics as a retire+recreate mid-frame).
  context.cmd->Barrier(t.texture, nrhi::ResourceState::kPixelShaderResource,
                       nrhi::ResourceState::kCopyDest);
  context.cmd->FlushBarriers();
  context.cmd->CopyBufferToTexture(t.texture, 0, 0, up, 0, pitch, cols * block_w,
                                   rows * block_h, 1);
  context.cmd->Barrier(t.texture, nrhi::ResourceState::kCopyDest,
                       nrhi::ResourceState::kPixelShaderResource);
  context.cmd->FlushBarriers();

  t.upload_flip = !t.upload_flip;
  t.payload_fp = fp;
  t.near_black = SampleProbeNearBlack(base, t);
  t.recheck_frame = 0;
  return true;
}

// Environment CUBE map for the water / reflective-glass reflection term
// (t6). Six faces untiled independently per level (Xenos cubes are 2D-tiled
// per face slice), WITH the guest mip chain; gradient-derived LOD on the
// normal-mapped reflection vector is what blurs baked cube detail
// (streetlight heads) into the soft reflections of the real console output.
bool EnsureGuestCubeTexture(const NativeGuestOutputRenderContext& context, uint8_t* base,
                            uint32_t tex_ptr, GuestTexture& out) {
  uint32_t words[6] = {};
  {
    uint32_t raw[6];
    if (!GuestTryCopy(raw, base + tex_ptr + 7 * 4, sizeof(raw))) {
      return false;
    }
    for (uint32_t i = 0; i < 6; ++i) {
      words[i] = SwapU32(raw[i]);
    }
  }
  std::memcpy(out.fetch_words, words, sizeof(words));
  xenos::xe_gpu_texture_fetch_t fetch = {};
  fetch.dword_0 = words[0];
  fetch.dword_1 = words[1];
  fetch.dword_2 = words[2];
  fetch.dword_3 = words[3];
  fetch.dword_4 = words[4];
  fetch.dword_5 = words[5];
  if (fetch.type != xenos::FetchConstantType::kTexture || fetch.base_address == 0) {
    return false;
  }
  // Same divide-by-zero pre-guard as EnsureGuestTextureFromWords (garbage
  // format -> zero block dims inside Prepare's extent math).
  const rex::graphics::FormatInfo* pre_fi =
      rex::graphics::FormatInfo::Get(uint32_t(fetch.format));
  if (pre_fi == nullptr || pre_fi->block_width == 0 || pre_fi->block_height == 0 ||
      pre_fi->bytes_per_block() == 0) {
    return false;
  }
  rex::graphics::TextureInfo info;
  if (!rex::graphics::TextureInfo::Prepare(fetch, &info)) {
    return false;
  }
  if (info.dimension != xenos::DataDimension::kCube || info.memory.base_address == 0 ||
      info.width >= 1024 || info.height >= 1024) {
    return false;
  }
  HostTextureFormat host;
  if (!GetHostTextureFormat(info.format, host)) {
    return false;
  }
  const rex::graphics::FormatInfo* format_info = info.format_info();
  const uint32_t bytes_per_block = format_info->bytes_per_block();
  if (bytes_per_block == 0 || (bytes_per_block & (bytes_per_block - 1)) != 0) {
    return false;
  }
  const uint32_t bytes_per_block_log2 = uint32_t(std::countr_zero(bytes_per_block));
  const uint32_t width = info.width + 1u;
  const uint32_t height = info.height + 1u;
  const uint32_t block_w = format_info->block_width;
  const uint32_t block_h = format_info->block_height;
  const uint32_t host_width = ((width + block_w - 1) / block_w) * block_w;
  const uint32_t host_height = ((height + block_h - 1) / block_h) * block_h;
  // A FULL mip chain is load-bearing for the reflective families. The real
  // baseenvironmentreflective PS perturbs its reflection vector with a
  // per-pixel normal map, so adjacent pixels reflect degrees apart; the
  // hardware cube fetch runs at a VERY deep gradient-derived LOD and the
  // whole reflection resolves to a near-face-average wash (the emulated
  // frame's uniform blue glass; offline: a CUBE_LOD=8 box-downsample probe
  // reproduces the emulated facade's uniformity where mip 0 shows a sharp
  // baked tree/streetlight blob). A truncated chain
  // clamps the LOD shallow and leaves that blob visible. DXT1 cubes (every
  // env cube observed) decode mip 0 to RGBA8 and generate the complete
  // chain down to 1x1 below; other formats fall back to copying the guest
  // chain (down to 32px: smaller levels live packed inside a shared 32x32
  // tile, see GetPackedTileOffset).
  const xenos::TextureFormat cube_base_fmt = rex::graphics::GetBaseFormat(info.format);
  const bool rgba_chain =
      (cube_base_fmt == xenos::TextureFormat::k_DXT1 ||
       cube_base_fmt == xenos::TextureFormat::k_8_8_8_8) &&
      width >= 8 && (width & (width - 1)) == 0 && width == height;
  uint32_t mip_levels = 1;
  if (!rgba_chain && info.memory.mip_address != 0 && (width & (width - 1)) == 0 &&
      (height & (height - 1)) == 0) {
    const uint32_t avail = std::min(info.mip_levels(), info.GetMaxMipLevels());
    while (mip_levels < avail && (width >> mip_levels) >= 32 &&
           (height >> mip_levels) >= 32) {
      uint32_t ox = 0, oy = 0;
      if (info.GetMipLocation(mip_levels, &ox, &oy, true) == 0 || ox != 0 || oy != 0) {
        break;
      }
      ++mip_levels;
    }
  }
  // Per-level guest layout: each level stores the six face slices
  // consecutively (extent depth = 6; GetMipLocation walks whole levels).
  struct CubeLevel {
    uint32_t addr, pitch_blocks, slice_bytes, cols, rows, scratch_off;
    uint32_t up_pitch, up_face_bytes, w, h;
  };
  CubeLevel lv[6] = {};
  uint32_t scratch_total = 0;
  for (uint32_t m = 0; m < mip_levels; ++m) {
    CubeLevel& L = lv[m];
    const auto ext = m == 0 ? info.extent : info.GetMipExtent(m, true);
    uint32_t ox = 0, oy = 0;
    L.addr = m == 0 ? info.memory.base_address : info.GetMipLocation(m, &ox, &oy, true);
    L.pitch_blocks = ext.block_pitch_h;
    L.slice_bytes = ext.block_pitch_h * ext.block_pitch_v * bytes_per_block;
    const uint32_t mw = std::max(width >> m, 1u);
    const uint32_t mh = std::max(height >> m, 1u);
    L.cols = (mw + block_w - 1) / block_w;
    L.rows = (mh + block_h - 1) / block_h;
    L.w = L.cols * block_w;
    L.h = L.rows * block_h;
    L.up_pitch = (L.cols * bytes_per_block + (nrhi::kRowPitchAlignment - 1u)) &
                 ~(nrhi::kRowPitchAlignment - 1u);
    L.up_face_bytes =
        (L.up_pitch * L.rows + (kUploadPlacementAlignment - 1u)) &
        ~(kUploadPlacementAlignment - 1u);
    L.scratch_off = scratch_total;
    scratch_total += L.slice_bytes * 6;
  }
  if (scratch_total == 0 || scratch_total > 24u * 1024u * 1024u) {
    return false;
  }
  static thread_local std::vector<uint8_t> cube_scratch;
  cube_scratch.resize(scratch_total);
  for (uint32_t m = 0; m < mip_levels; ++m) {
    if (!GuestTryCopy(cube_scratch.data() + lv[m].scratch_off,
                      base + (0xA0000000u | lv[m].addr), lv[m].slice_bytes * 6)) {
      if (m == 0) {
        return false;
      }
      mip_levels = m;  // truncate the chain at the first unreadable level
      break;
    }
  }

  if (rgba_chain) {
    // Decode mip 0 -> RGBA8 per face (DXT1 block decode, or plain 8888
    // texels: the water canal cubes), box-filter the full chain to 1x1,
    // upload as an RGBA cube. The GUEST cube mip chain is never trusted:
    // its packed sub-32px levels and per-face slice layout are exactly
    // where garbage sneaks in, and the reflection fetch runs at a deep
    // gradient LOD where a bad level dominates. CPU cost is one-time per
    // cube (runs on the decode workers).
    nrhi::Device* device = context.device;
    const uint32_t levels = 1u + uint32_t(std::countr_zero(width));
    struct Level {
      uint32_t w, pitch, face_bytes, upload_off;  // upload_off within a face
    };
    Level lvs[16] = {};
    uint32_t face_upload = 0;
    for (uint32_t m = 0; m < levels; ++m) {
      Level& L = lvs[m];
      L.w = std::max(width >> m, 1u);
      L.pitch = (L.w * 4u + (nrhi::kRowPitchAlignment - 1u)) &
                ~(nrhi::kRowPitchAlignment - 1u);
      L.face_bytes = (L.pitch * L.w + (kUploadPlacementAlignment - 1u)) &
                     ~(kUploadPlacementAlignment - 1u);
      L.upload_off = face_upload;
      face_upload += L.face_bytes;
    }
    nrhi::TextureDesc desc;
    desc.kind = nrhi::TextureKind::kCube;
    desc.width = width;
    desc.height = height;
    desc.mip_levels = levels;
    desc.format = nrhi::Format::kR8G8B8A8_UNORM;
    desc.initial_state = nrhi::ResourceState::kCopyDest;
    out.texture = device->CreateTexture(desc);
    if (out.texture == nullptr) {
      return false;
    }
    out.upload = CreateUploadBuffer(device, face_upload * 6, nrhi::BufferBindClass::kCopySrc);
    if (!out.upload) {
      device->DestroyDeferred(out.texture);
      out.texture = nullptr;
      return false;
    }
    uint8_t* mapping = static_cast<uint8_t*>(device->Map(out.upload));
    static thread_local std::vector<uint8_t> rgba;   // level 0 of one face
    static thread_local std::vector<uint8_t> down;   // downsample scratch
    static thread_local std::vector<uint8_t> bc_row; // one untiled block row
    const CubeLevel& L0 = lv[0];
    bc_row.resize(size_t(L0.cols) * bytes_per_block);
    for (uint32_t face = 0; face < 6; ++face) {
      // Per-face: the downsample loop below SWAPS rgba/down, so their sizes
      // end the chain tiny; the next face's full-size decode writes must
      // not index a shrunken buffer.
      rgba.resize(size_t(width) * height * 4);
      down.resize(size_t(width / 2) * (height / 2) * 4);
      const uint8_t* guest =
          cube_scratch.data() + L0.scratch_off + size_t(face) * L0.slice_bytes;
      for (uint32_t by = 0; by < L0.rows; ++by) {
        for (uint32_t bx = 0; bx < L0.cols; ++bx) {
          uint32_t source_offset;
          if (info.is_tiled) {
            source_offset = uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
                int32_t(bx), int32_t(by), L0.pitch_blocks, bytes_per_block_log2));
          } else {
            source_offset = (by * L0.pitch_blocks + bx) * bytes_per_block;
          }
          if (source_offset + bytes_per_block > L0.slice_bytes) {
            std::memset(&bc_row[size_t(bx) * bytes_per_block], 0, bytes_per_block);
            continue;
          }
          std::memcpy(&bc_row[size_t(bx) * bytes_per_block], guest + source_offset,
                      bytes_per_block);
        }
        SwapGuestEndian(bc_row.data(), uint32_t(bc_row.size()), info.endianness);
        if (cube_base_fmt == xenos::TextureFormat::k_DXT1) {
          for (uint32_t bx = 0; bx < L0.cols; ++bx) {
            uint8_t px[16][4];
            DecodeBc1Block(&bc_row[size_t(bx) * bytes_per_block], px);
            for (uint32_t r = 0; r < 4; ++r) {
              std::memcpy(&rgba[(size_t(by * 4 + r) * width + bx * 4) * 4],
                          px[r * 4], 16);
            }
          }
        } else {
          // k_8_8_8_8: post-endian-swap bytes are already component order
          // X,Y,Z,W (the SRV swizzle applies the fetch's channel remap).
          std::memcpy(&rgba[size_t(by) * width * 4], bc_row.data(),
                      size_t(width) * 4);
        }
      }
      // Diagnostic dump (skate3_native_render_scene_lm_dump): the decoded
      // face exactly as uploaded, for offline byte-diff against the
      // reference decode of the same fetch words.
      if (REXCVAR_GET(skate3_native_render_scene_lm_dump)) {
        char path[260];
        std::snprintf(path, sizeof(path),
                      "native_texture_dumps/cube_%08X_f%u_%ux%u.rgba",
                      info.memory.base_address, face, width, height);
        if (FILE* f = std::fopen(path, "wb")) {
          std::fwrite(rgba.data(), 1, size_t(width) * height * 4, f);
          std::fclose(f);
        }
      }
      // Upload level 0, then box-filter down the chain in place.
      const uint8_t* src = rgba.data();
      uint32_t w = width;
      for (uint32_t m = 0; m < levels; ++m) {
        uint8_t* up = mapping + size_t(face) * face_upload + lvs[m].upload_off;
        for (uint32_t y = 0; y < w; ++y) {
          std::memcpy(up + size_t(y) * lvs[m].pitch, src + size_t(y) * w * 4,
                      size_t(w) * 4);
        }
        if (m + 1 >= levels) {
          break;
        }
        const uint32_t hw = w / 2;
        for (uint32_t y = 0; y < hw; ++y) {
          for (uint32_t x = 0; x < hw; ++x) {
            for (uint32_t c = 0; c < 4; ++c) {
              const uint32_t s =
                  uint32_t(src[((y * 2) * w + x * 2) * 4 + c]) +
                  uint32_t(src[((y * 2) * w + x * 2 + 1) * 4 + c]) +
                  uint32_t(src[((y * 2 + 1) * w + x * 2) * 4 + c]) +
                  uint32_t(src[((y * 2 + 1) * w + x * 2 + 1) * 4 + c]);
              down[(size_t(y) * hw + x) * 4 + c] = uint8_t((s + 2) / 4);
            }
          }
        }
        rgba.swap(down);
        src = rgba.data();
        w = hw;
      }
    }
    device->Unmap(out.upload);
    REXLOG_DEBUG("native-scene: cube {:08X} {}x{} fmt={} -> RGBA full chain ({} levels)",
                 tex_ptr, width, height, uint32_t(info.format), levels);
    nrhi::Swizzle swizzle_mapping[4];
    ComposeSrvSwizzle(fetch.swizzle, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
                      swizzle_mapping);
    if (g_tex_stage_out != nullptr) {
      StagedTexCommit& sc = *g_tex_stage_out;
      sc.copy_format = nrhi::Format::kR8G8B8A8_UNORM;
      sc.srv_format = nrhi::Format::kR8G8B8A8_UNORM;
      for (uint32_t c = 0; c < 4; ++c) {
        sc.swizzle[c] = swizzle_mapping[c];
      }
      sc.cube = true;
      sc.cube_mip_levels = levels;
      sc.mip_count = 6 * levels;
      for (uint32_t face = 0; face < 6; ++face) {
        for (uint32_t m = 0; m < levels; ++m) {
          sc.mips[face * levels + m] = {face * face_upload + lvs[m].upload_off,
                                        lvs[m].pitch, lvs[m].w, lvs[m].w};
        }
      }
      out.payload_addr = 0xA0000000u | info.memory.base_address;
      out.payload_size = lv[0].slice_bytes * 6;
      out.payload_fp = SamplePayloadFingerprint(base, out.payload_addr, out.payload_size);
      out.recheck_frame = 0;
      out.valid = false;  // live only after commit
      return true;
    }
    for (uint32_t face = 0; face < 6; ++face) {
      for (uint32_t m = 0; m < levels; ++m) {
        context.cmd->CopyBufferToTexture(
            out.texture, m, face, out.upload,
            size_t(face) * face_upload + lvs[m].upload_off, lvs[m].pitch,
            lvs[m].w, lvs[m].w, 1);
      }
    }
    context.cmd->Barrier(out.texture, nrhi::ResourceState::kCopyDest,
                         nrhi::ResourceState::kPixelShaderResource);
    // Copies recorded: release the staging buffer (deferred until the
    // submission completes).
    device->DestroyDeferred(out.upload);
    out.upload = nullptr;
    nrhi::TextureViewDesc srv;
    srv.dimension = nrhi::ViewDimension::kCube;
    srv.format = nrhi::Format::kR8G8B8A8_UNORM;
    for (uint32_t c = 0; c < 4; ++c) {
      srv.swizzle[c] = swizzle_mapping[c];
    }
    srv.mip_levels = levels;
    out.srv = device->CreateTextureView(out.texture, srv);
    if (out.srv == nullptr) {
      return false;
    }
    out.payload_addr = 0xA0000000u | info.memory.base_address;
    out.payload_size = lv[0].slice_bytes * 6;
    out.payload_fp = SamplePayloadFingerprint(base, out.payload_addr, out.payload_size);
    out.recheck_frame = 0;
    out.valid = true;
    return true;
  }

  uint32_t face_upload = 0;  // one face's full mip chain in the upload buffer
  for (uint32_t m = 0; m < mip_levels; ++m) {
    face_upload += lv[m].up_face_bytes;
  }
  nrhi::Device* device = context.device;
  nrhi::TextureDesc desc;
  desc.kind = nrhi::TextureKind::kCube;
  desc.width = host_width;
  desc.height = host_height;
  desc.mip_levels = mip_levels;
  desc.format = host.resource_format;
  desc.initial_state = nrhi::ResourceState::kCopyDest;
  out.texture = device->CreateTexture(desc);
  if (out.texture == nullptr) {
    return false;
  }
  out.upload = CreateUploadBuffer(device, face_upload * 6, nrhi::BufferBindClass::kCopySrc);
  if (!out.upload) {
    device->DestroyDeferred(out.texture);
    out.texture = nullptr;
    return false;
  }
  uint8_t* mapping = static_cast<uint8_t*>(device->Map(out.upload));
  const auto upload_offset = [&](uint32_t face, uint32_t m) {
    uint32_t off = face * face_upload;
    for (uint32_t k = 0; k < m; ++k) {
      off += lv[k].up_face_bytes;
    }
    return off;
  };
  // (The "Xenos cube T runs bottom-up" flip that briefly lived here was
  // WRONG; it matched two probe pixels by coincidence and turned every
  // facade pavement-white in game. The stored face orientation is correct
  // as-is; the emulated look comes from LOD depth, not orientation.)
  for (uint32_t face = 0; face < 6; ++face) {
    for (uint32_t m = 0; m < mip_levels; ++m) {
      const CubeLevel& L = lv[m];
      const uint8_t* guest =
          cube_scratch.data() + L.scratch_off + size_t(face) * L.slice_bytes;
      uint8_t* up = mapping + upload_offset(face, m);
      const uint32_t row_bytes = L.cols * bytes_per_block;
      for (uint32_t by = 0; by < L.rows; ++by) {
        uint8_t* out_row = up + size_t(by) * L.up_pitch;
        for (uint32_t bx = 0; bx < L.cols; ++bx) {
          uint32_t source_offset;
          if (info.is_tiled) {
            source_offset = uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
                int32_t(bx), int32_t(by), L.pitch_blocks, bytes_per_block_log2));
          } else {
            source_offset = (by * L.pitch_blocks + bx) * bytes_per_block;
          }
          if (source_offset + bytes_per_block > L.slice_bytes) {
            std::memset(out_row + size_t(bx) * bytes_per_block, 0, bytes_per_block);
            continue;
          }
          std::memcpy(out_row + size_t(bx) * bytes_per_block, guest + source_offset,
                      bytes_per_block);
        }
        SwapGuestEndian(out_row, row_bytes, info.endianness);
      }
    }
  }
  device->Unmap(out.upload);

  REXLOG_DEBUG("native-scene: cube {:08X} {}x{} fmt={} mips={} (of {} avail)", tex_ptr,
               width, height, uint32_t(info.format), mip_levels, info.mip_levels());
  if (g_tex_stage_out != nullptr) {
    // Decode worker: export the commit recipe, face-major (face * levels +
    // mip) entries matching the old D3D12 subresource numbering.
    StagedTexCommit& sc = *g_tex_stage_out;
    sc.copy_format = host.resource_format;
    sc.srv_format = host.srv_format;
    ComposeSrvSwizzle(fetch.swizzle, host.host_swizzle, sc.swizzle);
    sc.cube = true;
    sc.cube_mip_levels = mip_levels;
    sc.mip_count = 6 * mip_levels;
    for (uint32_t face = 0; face < 6; ++face) {
      for (uint32_t m = 0; m < mip_levels; ++m) {
        sc.mips[face * mip_levels + m] = {upload_offset(face, m), lv[m].up_pitch,
                                          lv[m].w, lv[m].h};
      }
    }
    out.payload_addr = 0xA0000000u | info.memory.base_address;
    out.payload_size = lv[0].slice_bytes * 6;
    out.payload_fp = SamplePayloadFingerprint(base, out.payload_addr, out.payload_size);
    out.recheck_frame = 0;
    out.valid = false;  // live only after commit
    return true;
  }

  for (uint32_t face = 0; face < 6; ++face) {
    for (uint32_t m = 0; m < mip_levels; ++m) {
      context.cmd->CopyBufferToTexture(out.texture, m, face, out.upload,
                                       upload_offset(face, m), lv[m].up_pitch,
                                       lv[m].w, lv[m].h, 1);
    }
  }
  context.cmd->Barrier(out.texture, nrhi::ResourceState::kCopyDest,
                       nrhi::ResourceState::kPixelShaderResource);
  // Copies recorded: release the staging buffer (deferred until the
  // submission completes).
  device->DestroyDeferred(out.upload);
  out.upload = nullptr;
  nrhi::TextureViewDesc srv;
  srv.dimension = nrhi::ViewDimension::kCube;
  srv.format = host.srv_format;
  ComposeSrvSwizzle(fetch.swizzle, host.host_swizzle, srv.swizzle);
  srv.mip_levels = mip_levels;
  out.srv = device->CreateTextureView(out.texture, srv);
  if (out.srv == nullptr) {
    return false;
  }
  out.payload_addr = 0xA0000000u | info.memory.base_address;
  out.payload_size = lv[0].slice_bytes * 6;
  out.payload_fp = SamplePayloadFingerprint(base, out.payload_addr, out.payload_size);
  out.recheck_frame = 0;
  out.valid = true;
  return true;
}

// Scene vertex layout (DecodeMesh's 56-byte output): shared by the scene
// PSO family and the shadow-caster PSO. `location` = the element's
// declaration order in the scene VS input struct (the Vulkan input
// location; see nrhi::InputElementDesc).

}  // namespace

// ---- EnsurePipeline helpers ------------------------------------------------
// Split from the former ~1,000-line EnsurePipeline: one resource
// group per function, bodies unchanged. Every helper is idempotent (guarded
// by its own g_r state) and returns false only on a failure that must abort
// the native path (g_r.failed set where the original did).

// Looks up the offline-compiled SPIR-V for (file, entry, variant); variant is
// the canonical macro signature ("", "SAMPLES=4", "PFX_MSAA=1"). Returns a
// fully-populated ShaderDesc; SPIR-V absent only if the table and the compile
// matrix ever drift (Vulkan pipeline creation then fails loudly, D3D12 is
// unaffected).
nrhi::ShaderDesc MakeShaderDesc(nrhi::ShaderStage stage, const char* file,
                                const char* hlsl_source, const char* entry,
                                const nrhi::ShaderMacro* macros,
                                const char* variant) {
  nrhi::ShaderDesc sd;
  sd.stage = stage;
  sd.name = file;
  sd.hlsl_source = hlsl_source;
  sd.entry_point = entry;
  sd.macros = macros;
  for (size_t i = 0; i < skate3::native_spirv::kNativeSpirvBlobCount; ++i) {
    const auto& b = skate3::native_spirv::kNativeSpirvBlobs[i];
    if (std::strcmp(b.file, file) == 0 && std::strcmp(b.entry, entry) == 0 &&
        std::strcmp(b.variant, variant) == 0) {
      sd.spirv = b.data;
      sd.spirv_size_bytes = b.size_bytes;
      break;
    }
  }
  return sd;
}

bool EnsureRootSignature(const NativeGuestOutputRenderContext& context) {
  if (g_r.layout) {
    return true;
  }
  {
    nrhi::BindingLayoutDesc ld;
    // NOTE the 64-DWORD root-signature budget (a D3D12-backend constraint:
    // the layout maps 1:1 onto a D3D12 root signature there): 52 constants +
    // 6 descriptor tables (1 each) + 1 root SRV (2) + 2 root CBVs (2 each)
    // = 64, FULL. Going past 64 makes the D3D12 layout creation fail
    // (renderer falls back to emulated). Any further addition must pack into
    // existing rows/tables.
    ld.param_count = 10;
    ld.params[0] = {nrhi::BindingParamKind::kConstants, /*b*/ 0, 52,
                    nrhi::Visibility::kAll};
    ld.params[1] = {nrhi::BindingParamKind::kTextureTable, /*t*/ 0, 1,
                    nrhi::Visibility::kPixel};
    ld.params[2] = {nrhi::BindingParamKind::kTextureTable, 1, 1,
                    nrhi::Visibility::kPixel};
    ld.params[3] = {nrhi::BindingParamKind::kBufferSrv, /*t*/ 2, 1,
                    nrhi::Visibility::kVertex};
    // Macro overlay (t3).
    ld.params[4] = {nrhi::BindingParamKind::kTextureTable, 3, 1,
                    nrhi::Visibility::kPixel};
    // Decal art / spec masks (t4). Second entry of the table = the fam 5/6
    // normal map (t5), bound via cmd->SetTexturePair. Draws without a pair
    // leave t5 at the backend fallback; the shader only samples t5 when
    // overlay.w == 4 (pair bound).
    ld.params[5] = {nrhi::BindingParamKind::kTextureTable, 4, 2,
                    nrhi::Visibility::kPixel};
    // Dynamic-shadow additions: per-frame receiver constants (b1).
    ld.params[6] = {nrhi::BindingParamKind::kConstantBuffer, /*b*/ 1, 1,
                    nrhi::Visibility::kPixel};
    // Environment cube (t6) + blurred shadow atlas (t7) as ONE two-entry
    // table (bound together via SetTexturePair); merging them freed the
    // root-signature DWORD the v2 material table below needs.
    ld.params[7] = {nrhi::BindingParamKind::kTextureTable, 6, 2,
                    nrhi::Visibility::kPixel};
    // World-shading v2 material maps: detail (t8), spec/ecc (t9), and three
    // independent owned-world static-shadow cascades (t10..t12). Retained
    // rendering continues to use its tiled atlas in t10. Extending this
    // existing descriptor table costs no root-signature DWORDs.
    ld.params[8] = {nrhi::BindingParamKind::kTextureTable, 8, 5,
                    nrhi::Visibility::kPixel};
    // Character lighting block (b2): the canonical per-draw rows captured
    // from the guest PS bank (CaptureCharLighting), sliced out of the bone
    // upload ring per character draw.
    ld.params[9] = {nrhi::BindingParamKind::kConstantBuffer, 2, 1,
                    nrhi::Visibility::kPixel};
    ld.static_sampler_count = 2;
    ld.static_samplers[0] = {/*s*/ 0, nrhi::Filter::kAnisotropic,
                             nrhi::AddressMode::kWrap, 8};
    // s1: bilinear CLAMP for the 2D overlay. The HUD fetch constants carry
    // clamp_x/clamp_y = 2 (clamp-to-edge), and the clock face is built from
    // MIRRORED quadrant tiles whose outer-edge UVs run past 1.0; wrap
    // sampling pulls the opposite edge of the art in as 1px seam lines
    // (bright rim row across the middle, dark center column at the edges).
    ld.static_samplers[1] = {1, nrhi::Filter::kLinear,
                             nrhi::AddressMode::kClamp, 1};
    ld.allow_input_layout = true;
    g_r.layout = context.device->CreateBindingLayout(ld);
    if (g_r.layout == nullptr) {
      REXLOG_ERROR("native-scene: root signature creation failed");
      g_r.failed = true;
      return false;
    }
  }
  return true;
}

// The main scene PSO and its culling/blend/depth variants (cull-back
// sheets, transparent, entity fade, hair 2-pass, no-depth, outline mask),
// built at the g_r.msaa sample count EnsurePipeline selected.
bool EnsureScenePsoFamily(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;
  // Scene color format: the float HDR intermediate when the HDR post chain
  // is active (the tonemap pass then writes the gamma guest output),
  // otherwise the guest output itself.
  const nrhi::Format scene_fmt = g_r.hdr_active
                                     ? g_r.hdr_scene_format
                                     : context.guest_output->format();
  // g_r.msaa is selected by EnsurePipeline (which triggers this rebuild on
  // change).

  // The family rebuilds on HDR/MSAA toggles / output-format changes: retire the
  // previous pipelines (in-flight frames keep them alive via the deferred
  // destruction queue).
  for (nrhi::Pipeline** p :
       {&g_r.pso, &g_r.pso_cullback, &g_r.pso_transparent, &g_r.pso_fade,
        &g_r.pso_hair_a, &g_r.pso_hair_b, &g_r.pso_nodepth,
        &g_r.pso_outline_mask}) {
    if (*p != nullptr) {
      device->DestroyDeferred(*p);
      *p = nullptr;
    }
  }

  // HDR=1 switches the pixel shader's output encode to pre-tonemap linear
  // (see scene.hlsl ToneOut/PassGamma); SHOWCASE=1 compiles the build-up
  // split/mask gates in (swapped in only while a run is live). The variant
  // string is the canonical ";"-joined macro signature the offline SPIR-V
  // matrix uses.
  nrhi::ShaderMacro ps_defs[3];
  uint32_t ps_def_count = 0;
  if (g_r.hdr_active) {
    ps_defs[ps_def_count++] = {"HDR", "1"};
  }
  if (g_r.showcase_shaders) {
    ps_defs[ps_def_count++] = {"SHOWCASE", "1"};
  }
  ps_defs[ps_def_count] = {nullptr, nullptr};
  char ps_variant[24];
  std::snprintf(ps_variant, sizeof(ps_variant), "%s%s%s",
                g_r.hdr_active ? "HDR=1" : "",
                g_r.hdr_active && g_r.showcase_shaders ? ";" : "",
                g_r.showcase_shaders ? "SHOWCASE=1" : "");
  nrhi::Shader* vs = device->CreateShader(
      MakeShaderDesc(nrhi::ShaderStage::kVertex, "scene.hlsl", kShaderSource,
                     "vs_main", nullptr, ""));
  nrhi::Shader* ps = device->CreateShader(
      MakeShaderDesc(nrhi::ShaderStage::kPixel, "scene.hlsl", kShaderSource,
                     "ps_main", ps_def_count != 0 ? ps_defs : nullptr,
                     ps_variant));
  if (vs == nullptr || ps == nullptr) {
    REXLOG_ERROR("native-scene: shader compile failed");
    g_r.failed = true;
    return false;
  }
  nrhi::GraphicsPipelineDesc pso;
  pso.layout = g_r.layout;
  pso.vs = vs;
  pso.ps = ps;
  pso.input_elements = kSceneInputLayout;
  pso.input_element_count = 7;
  pso.vertex_stride = 56;
  pso.cull = nrhi::CullMode::kNone;
  pso.depth_clip = true;
  pso.depth.test_enable = true;
  pso.depth.write_enable = true;
  pso.depth.func = nrhi::CompareFunc::kLess;
  pso.rtv_format = scene_fmt;
  pso.dsv_format = nrhi::Format::kD32_FLOAT;
  pso.sample_count = g_r.msaa;
  g_r.pso = device->CreateGraphicsPipeline(pso);
  // Culling variant for double-sided sheet props (banners/flags,
  // MeshBuffers::two_sided_sheet). The sheet whose winding-derived world
  // normal faces the camera is the one the game keeps (its lightmap cell
  // reproduces the emulated banner exactly: albedo x lm x 2 lands within
  // 3-8% of the F11 emulated reference on capture 1783387480); under our
  // pipeline those triangles are D3D12 BACK faces, so cull FRONT.
  // (CULL_BACK was tried first and kept the wrong sheet; banners rendered
  // the sun-side lightmap cell ~2.4x brighter than emulated.)
  pso.cull = nrhi::CullMode::kFront;
  g_r.pso_cullback = device->CreateGraphicsPipeline(pso);
  if (g_r.pso_cullback == nullptr) {
    REXLOG_WARN("native-scene: cull-back PSO creation failed");
    // sheets fall back to the uncull(ed) PSO
  }
  pso.cull = nrhi::CullMode::kNone;
  // Transparent variant: straight alpha blend, depth-tested, no z-write.
  pso.depth.write_enable = false;
  pso.blend.enable = true;
  pso.blend.src = nrhi::BlendFactor::kSrcAlpha;
  pso.blend.dst = nrhi::BlendFactor::kInvSrcAlpha;
  pso.blend.op = nrhi::BlendOp::kAdd;
  pso.blend.src_alpha = nrhi::BlendFactor::kOne;
  pso.blend.dst_alpha = nrhi::BlendFactor::kInvSrcAlpha;
  pso.blend.op_alpha = nrhi::BlendOp::kAdd;
  g_r.pso_transparent = device->CreateGraphicsPipeline(pso);
  // Entity-fade variant: z-write ON (see RendererState::pso_fade). Drawn
  // at the head of the blended sub-pass so glass/hair still composite
  // over the faded body.
  pso.depth.write_enable = true;
  g_r.pso_fade = device->CreateGraphicsPipeline(pso);
  // (nullptr = fade items fall back to the z-write-off blend)
  pso.depth.write_enable = false;
  // Hair passes: the game draws hair twice with the SAME shader; cull
  // BACK then cull FRONT (cac_hair.xml passes 0/1) so far-side strands
  // never composite over near-side ones (one uncull(ed) blended pass
  // interleaves them per triangle order = crunchy noise). Same blend /
  // z-write-off state as the transparent variant.
  pso.cull = nrhi::CullMode::kBack;
  g_r.pso_hair_a = device->CreateGraphicsPipeline(pso);
  pso.cull = nrhi::CullMode::kFront;
  g_r.pso_hair_b = device->CreateGraphicsPipeline(pso);
  pso.cull = nrhi::CullMode::kNone;
  pso.blend = {};
  pso.depth.write_enable = true;
  pso.depth.test_enable = false;
  pso.dsv_format = nrhi::Format::kUnknown;
  g_r.pso_nodepth = device->CreateGraphicsPipeline(pso);
  // Selection-outline mask: the same scene VS/PS (the tint.a > 0 solid
  // path renders flat 1.0) into the small single-sample R8 target. No
  // depth: the mask is the full silhouette. The guest marking pass is
  // depth-tested, so a partially occluded selection outlines slightly
  // differently, acceptable for the editor UI.
  pso.rtv_format = nrhi::Format::kR8_UNORM;
  pso.sample_count = 1;
  g_r.pso_outline_mask = device->CreateGraphicsPipeline(pso);
  if (g_r.pso_outline_mask == nullptr) {
    REXLOG_WARN("native-scene: outline mask PSO creation failed");
    // outline pass disables itself
  }
  pso.rtv_format = scene_fmt;
  pso.sample_count = g_r.msaa;
  device->DestroyDeferred(vs);
  device->DestroyDeferred(ps);
  if (g_r.pso == nullptr || g_r.pso_nodepth == nullptr ||
      g_r.pso_transparent == nullptr) {
    REXLOG_ERROR("native-scene: PSO creation failed");
    g_r.failed = true;
    return false;
  }
  return true;
}

// Fullscreen MSAA resolve PSO (no-op below MSAA 2x).
bool EnsureResolvePso(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;
  if (g_r.resolve_pso != nullptr) {
    device->DestroyDeferred(g_r.resolve_pso);
    g_r.resolve_pso = nullptr;
  }
  if (g_r.msaa > 1) {
    char samples[8];
    std::snprintf(samples, sizeof(samples), "%u", g_r.msaa);
    const nrhi::ShaderMacro macros[] = {{"SAMPLES", samples},
                                        {nullptr, nullptr}};
    char variant[16];
    std::snprintf(variant, sizeof(variant), "SAMPLES=%u", g_r.msaa);
    nrhi::Shader* rvs = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kVertex, "resolve.hlsl",
                       kResolveShaderSource, "vs_main", macros, variant));
    nrhi::Shader* rps = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "resolve.hlsl",
                       kResolveShaderSource, "ps_main", macros, variant));
    if (rvs == nullptr || rps == nullptr) {
      REXLOG_ERROR("native-scene: resolve shader compile failed");
      g_r.failed = true;
      return false;
    }
    nrhi::GraphicsPipelineDesc rp;
    rp.layout = g_r.layout;
    rp.vs = rvs;
    rp.ps = rps;
    rp.cull = nrhi::CullMode::kNone;
    // Under HDR the resolve averages the float MSAA scene into the 1x float
    // plane (hdr_resolved); classic resolves straight into the guest output.
    rp.rtv_format = g_r.hdr_active ? g_r.hdr_scene_format
                                   : context.guest_output->format();
    rp.sample_count = 1;
    g_r.resolve_pso = device->CreateGraphicsPipeline(rp);
    device->DestroyDeferred(rvs);
    device->DestroyDeferred(rps);
    if (g_r.resolve_pso == nullptr) {
      REXLOG_ERROR("native-scene: resolve PSO creation failed");
      g_r.failed = true;
      return false;
    }
  }
  return true;
}

// Popup background blur pipelines; PSO-create failure only disables blur.
bool EnsureBlurPsos(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;
  {
    // Popup background blur pipelines (blur_hBlur/vBlur port + basictex
    // replace): fullscreen-triangle passes, no vertex buffer.
    nrhi::Shader* bvs = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kVertex, "blur.hlsl",
                       kBlurShaderSource, "vs_main", nullptr, ""));
    nrhi::Shader* bps = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "blur.hlsl",
                       kBlurShaderSource, "ps_main", nullptr, ""));
    nrhi::Shader* bblit = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "blur.hlsl",
                       kBlurShaderSource, "ps_blit", nullptr, ""));
    nrhi::Shader* bdown = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "blur.hlsl",
                       kBlurShaderSource, "ps_down", nullptr, ""));
    nrhi::Shader* bgauss = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "blur.hlsl",
                       kBlurShaderSource, "ps_menu_gauss", nullptr, ""));
    if (bvs == nullptr || bps == nullptr || bblit == nullptr ||
        bdown == nullptr || bgauss == nullptr) {
      REXLOG_ERROR("native-scene: blur shader compile failed");
      g_r.failed = true;
      return false;
    }
    nrhi::GraphicsPipelineDesc bp;
    bp.layout = g_r.layout;
    bp.vs = bvs;
    bp.ps = bps;
    bp.cull = nrhi::CullMode::kNone;
    bp.rtv_format = context.guest_output->format();
    bp.sample_count = 1;
    if (g_r.pso_blur) device->DestroyDeferred(g_r.pso_blur);
    if (g_r.pso_blur_blit) device->DestroyDeferred(g_r.pso_blur_blit);
    if (g_r.pso_blur_down) device->DestroyDeferred(g_r.pso_blur_down);
    if (g_r.pso_menu_gauss) device->DestroyDeferred(g_r.pso_menu_gauss);
    g_r.pso_blur = device->CreateGraphicsPipeline(bp);
    bp.ps = bblit;
    g_r.pso_blur_blit = device->CreateGraphicsPipeline(bp);
    bp.ps = bdown;
    g_r.pso_blur_down = device->CreateGraphicsPipeline(bp);
    bp.ps = bgauss;
    g_r.pso_menu_gauss = device->CreateGraphicsPipeline(bp);
    device->DestroyDeferred(bvs);
    device->DestroyDeferred(bps);
    device->DestroyDeferred(bblit);
    device->DestroyDeferred(bdown);
    device->DestroyDeferred(bgauss);
    if (g_r.pso_blur == nullptr || g_r.pso_blur_blit == nullptr ||
        g_r.pso_blur_down == nullptr || g_r.pso_menu_gauss == nullptr) {
      REXLOG_ERROR("native-scene: blur PSO creation failed");
      g_r.pso_blur = nullptr;
      g_r.pso_blur_blit = nullptr;  // blur unavailable; everything else runs
      g_r.pso_blur_down = nullptr;
      g_r.pso_menu_gauss = nullptr;
    }
  }
  return true;
}

// Selection-outline edge composite PSO; create failure disables outline.
bool EnsureOutlineEdgePso(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;
  {
    // Selection-outline edge composite (postfx_edgedetectstencil port):
    // fullscreen triangle over the resolved output, additive blend.
    nrhi::Shader* ovs = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kVertex, "outline.hlsl",
                       kOutlineShaderSource, "vs_main", nullptr, ""));
    nrhi::Shader* ops = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "outline.hlsl",
                       kOutlineShaderSource, "ps_main", nullptr, ""));
    if (ovs == nullptr || ops == nullptr) {
      REXLOG_ERROR("native-scene: outline shader compile failed");
      g_r.failed = true;
      return false;
    }
    nrhi::GraphicsPipelineDesc op;
    op.layout = g_r.layout;
    op.vs = ovs;
    op.ps = ops;
    op.blend.enable = true;
    op.blend.src = nrhi::BlendFactor::kOne;
    op.blend.dst = nrhi::BlendFactor::kOne;
    op.blend.op = nrhi::BlendOp::kAdd;
    op.blend.src_alpha = nrhi::BlendFactor::kZero;
    op.blend.dst_alpha = nrhi::BlendFactor::kOne;
    op.blend.op_alpha = nrhi::BlendOp::kAdd;
    op.cull = nrhi::CullMode::kNone;
    op.rtv_format = context.guest_output->format();
    op.sample_count = 1;
    if (g_r.pso_outline_edge) device->DestroyDeferred(g_r.pso_outline_edge);
    g_r.pso_outline_edge = device->CreateGraphicsPipeline(op);
    device->DestroyDeferred(ovs);
    device->DestroyDeferred(ops);
    if (g_r.pso_outline_edge == nullptr) {
      REXLOG_WARN("native-scene: outline edge PSO creation failed");
      // outline unavailable; everything else runs
    }
  }
  return true;
}

// 2D overlay pipeline.
bool Ensure2dPso(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;
  {
    // 2D overlay pipeline: standard alpha blend, no depth, drawn into the
    // resolved guest output (sample count 1).
    nrhi::Shader* uvs = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kVertex, "overlay2d.hlsl",
                       kShader2dSource, "vs_main", nullptr, ""));
    nrhi::Shader* ups = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "overlay2d.hlsl",
                       kShader2dSource, "ps_main", nullptr, ""));
    if (uvs == nullptr || ups == nullptr) {
      REXLOG_ERROR("native-scene: 2D shader compile failed");
      g_r.failed = true;
      return false;
    }
    static constexpr nrhi::InputElementDesc input2d[3] = {
        {"POSITION", 0, 0, nrhi::Format::kR32G32B32A32_FLOAT, 0},
        {"TEXCOORD", 0, 1, nrhi::Format::kR32G32_FLOAT, 16},
        {"COLOR", 0, 2, nrhi::Format::kR8G8B8A8_UNORM, 24}};
    nrhi::GraphicsPipelineDesc up;
    up.layout = g_r.layout;
    up.vs = uvs;
    up.ps = ups;
    up.blend.enable = true;
    // Straight (non-premultiplied) alpha, verified against the decoded
    // HUD clock art: the glass-sheen texture is white RGB at low alpha
    // (premultiplied blending blows it out into an opaque white blob).
    up.blend.src = nrhi::BlendFactor::kSrcAlpha;
    up.blend.dst = nrhi::BlendFactor::kInvSrcAlpha;
    up.blend.op = nrhi::BlendOp::kAdd;
    up.blend.src_alpha = nrhi::BlendFactor::kOne;
    up.blend.dst_alpha = nrhi::BlendFactor::kInvSrcAlpha;
    up.blend.op_alpha = nrhi::BlendOp::kAdd;
    up.cull = nrhi::CullMode::kNone;
    up.input_elements = input2d;
    up.input_element_count = 3;
    up.vertex_stride = 28;
    up.rtv_format = context.guest_output->format();
    up.sample_count = 1;
    g_r.pso_2d = device->CreateGraphicsPipeline(up);
    // FMV variant: identical pipeline, ps_yuv2d combine (see the movie-quad
    // substitution in the 2D replay). Failure only loses native FMV; the
    // emulated yield fallback covers it.
    nrhi::Shader* uyuv = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "overlay2d.hlsl",
                       kShader2dSource, "ps_yuv2d", nullptr, ""));
    if (uyuv != nullptr) {
      up.ps = uyuv;
      if (g_r.pso_yuv2d) device->DestroyDeferred(g_r.pso_yuv2d);
      g_r.pso_yuv2d = device->CreateGraphicsPipeline(up);
      if (g_r.pso_yuv2d == nullptr) {
        REXLOG_WARN("native-scene: FMV 2D PSO creation failed - movies yield");
      }
      device->DestroyDeferred(uyuv);
    } else {
      REXLOG_WARN("native-scene: ps_yuv2d compile failed - movies yield");
    }
    device->DestroyDeferred(uvs);
    device->DestroyDeferred(ups);
    if (g_r.pso_2d == nullptr) {
      REXLOG_ERROR("native-scene: 2D PSO creation failed");
      g_r.failed = true;
      return false;
    }
  }
  return true;
}

// In-world spline pipelines (darken + additive default).
bool EnsureSplinePsos(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;
  {
    // In-world spline pipelines: drawn inside the scene pass (MSAA sample
    // count, depth test LESS_EQUAL, no z-write) with the game's own blend
    // states (spline.xml): darken = straight alpha, default = additive.
    // Under HDR the pixel shaders encode through the tone chain's inverse
    // (SplineOut) so the host tonemap restores the authored look.
    for (nrhi::Pipeline** p : {&g_r.pso_spline_darken, &g_r.pso_spline_default}) {
      if (*p != nullptr) {
        device->DestroyDeferred(*p);
        *p = nullptr;
      }
    }
    // SHOWCASE=1 compiles the blackout gate in (see spline.hlsl
    // SplineVisible); like the scene family, the variant follows the
    // showcase swap so normal sessions carry no gate at all.
    nrhi::ShaderMacro sp_defs[3];
    uint32_t sp_def_count = 0;
    if (g_r.hdr_active) {
      sp_defs[sp_def_count++] = {"HDR", "1"};
    }
    if (g_r.showcase_shaders) {
      sp_defs[sp_def_count++] = {"SHOWCASE", "1"};
    }
    sp_defs[sp_def_count] = {nullptr, nullptr};
    char sp_variant[24];
    std::snprintf(sp_variant, sizeof(sp_variant), "%s%s%s",
                  g_r.hdr_active ? "HDR=1" : "",
                  g_r.hdr_active && g_r.showcase_shaders ? ";" : "",
                  g_r.showcase_shaders ? "SHOWCASE=1" : "");
    nrhi::Shader* svs = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kVertex, "spline.hlsl",
                       kShaderSplineSource, "vs_main", nullptr, ""));
    nrhi::Shader* spd = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "spline.hlsl",
                       kShaderSplineSource, "ps_default",
                       sp_def_count != 0 ? sp_defs : nullptr, sp_variant));
    nrhi::Shader* spk = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "spline.hlsl",
                       kShaderSplineSource, "ps_darken",
                       sp_def_count != 0 ? sp_defs : nullptr, sp_variant));
    if (svs == nullptr || spd == nullptr || spk == nullptr) {
      REXLOG_ERROR("native-scene: spline shader compile failed");
      g_r.failed = true;
      return false;
    }
    static constexpr nrhi::InputElementDesc input_spline[3] = {
        {"POSITION", 0, 0, nrhi::Format::kR32G32B32A32_FLOAT, 0},
        {"TEXCOORD", 0, 1, nrhi::Format::kR32G32_FLOAT, 16},
        {"TEXCOORD", 1, 2, nrhi::Format::kR32_FLOAT, 24}};
    nrhi::GraphicsPipelineDesc sp;
    sp.layout = g_r.layout;
    sp.vs = svs;
    sp.cull = nrhi::CullMode::kNone;
    sp.depth_clip = true;
    sp.depth.test_enable = true;
    sp.depth.write_enable = false;
    sp.depth.func = nrhi::CompareFunc::kLessEqual;
    sp.input_elements = input_spline;
    sp.input_element_count = 3;
    sp.vertex_stride = 28;
    sp.rtv_format = g_r.hdr_active ? g_r.hdr_scene_format
                                   : context.guest_output->format();
    sp.dsv_format = nrhi::Format::kD32_FLOAT;
    sp.sample_count = g_r.msaa;
    sp.blend.enable = true;
    sp.blend.op = nrhi::BlendOp::kAdd;
    sp.blend.op_alpha = nrhi::BlendOp::kAdd;
    // darken pass: straight alpha.
    sp.ps = spk;
    sp.blend.src = nrhi::BlendFactor::kSrcAlpha;
    sp.blend.dst = nrhi::BlendFactor::kInvSrcAlpha;
    sp.blend.src_alpha = nrhi::BlendFactor::kSrcAlpha;
    sp.blend.dst_alpha = nrhi::BlendFactor::kInvSrcAlpha;
    g_r.pso_spline_darken = device->CreateGraphicsPipeline(sp);
    // default pass: additive glow.
    sp.ps = spd;
    sp.blend.src = nrhi::BlendFactor::kOne;
    sp.blend.dst = nrhi::BlendFactor::kOne;
    sp.blend.src_alpha = nrhi::BlendFactor::kOne;
    sp.blend.dst_alpha = nrhi::BlendFactor::kOne;
    g_r.pso_spline_default = device->CreateGraphicsPipeline(sp);
    device->DestroyDeferred(svs);
    device->DestroyDeferred(spd);
    device->DestroyDeferred(spk);
    if (g_r.pso_spline_darken == nullptr || g_r.pso_spline_default == nullptr) {
      REXLOG_ERROR("native-scene: spline PSO creation failed");
      g_r.failed = true;
      return false;
    }
  }
  return true;
}

// Dynamic-shadow caster + per-tile blur/convert PSOs.
bool EnsureShadowPsos(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;
  {
    // Dynamic-shadow pipelines: casters (scene VS with a light-space
    // "view-proj", MIN-blend depth/uncoverage accumulation, no depth
    // buffer, depth clip OFF so casters outside the 12 m height window
    // clamp like the game accepts) + the per-tile blur/convert pass.
    nrhi::Shader* cvs = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kVertex, "scene.hlsl", kShaderSource,
                       "vs_main", nullptr, ""));
    nrhi::Shader* cps = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "scene.hlsl", kShaderSource,
                       "ps_shadow_caster", nullptr, ""));
    nrhi::Shader* cpc = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "scene.hlsl", kShaderSource,
                       "ps_shadow_caster_clip", nullptr, ""));
    nrhi::Shader* bvs = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kVertex, "shadow_blur.hlsl",
                       kShadowBlurSource, "vs_main", nullptr, ""));
    nrhi::Shader* bps = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "shadow_blur.hlsl",
                       kShadowBlurSource, "ps_main", nullptr, ""));
    if (cvs == nullptr || cps == nullptr || cpc == nullptr || bvs == nullptr ||
        bps == nullptr) {
      REXLOG_ERROR("native-scene: shadow shader compile failed");
      g_r.failed = true;
      return false;
    }
    nrhi::GraphicsPipelineDesc cp;
    cp.layout = g_r.layout;
    cp.vs = cvs;
    cp.ps = cps;
    cp.blend.enable = true;
    cp.blend.src = nrhi::BlendFactor::kOne;
    cp.blend.dst = nrhi::BlendFactor::kOne;
    cp.blend.op = nrhi::BlendOp::kMin;
    cp.blend.src_alpha = nrhi::BlendFactor::kOne;
    cp.blend.dst_alpha = nrhi::BlendFactor::kOne;
    cp.blend.op_alpha = nrhi::BlendOp::kMin;
    cp.blend.write_mask = 0x3;  // RG only
    cp.cull = nrhi::CullMode::kNone;
    cp.depth_clip = false;
    cp.input_elements = kSceneInputLayout;
    cp.input_element_count = 7;
    cp.vertex_stride = 56;
    cp.rtv_format = nrhi::Format::kR16G16_UNORM;
    cp.sample_count = 1;
    g_r.pso_shadow_caster = device->CreateGraphicsPipeline(cp);
    cp.ps = cpc;
    g_r.pso_shadow_caster_clip = device->CreateGraphicsPipeline(cp);
    nrhi::GraphicsPipelineDesc bp;
    bp.layout = g_r.layout;
    bp.vs = bvs;
    bp.ps = bps;
    bp.cull = nrhi::CullMode::kNone;
    bp.depth_clip = true;
    bp.rtv_format = nrhi::Format::kR16G16_UNORM;
    bp.sample_count = 1;
    g_r.pso_shadow_blur = device->CreateGraphicsPipeline(bp);
    device->DestroyDeferred(cvs);
    device->DestroyDeferred(cps);
    device->DestroyDeferred(cpc);
    device->DestroyDeferred(bvs);
    device->DestroyDeferred(bps);
    if (g_r.pso_shadow_caster == nullptr ||
        g_r.pso_shadow_caster_clip == nullptr ||
        g_r.pso_shadow_blur == nullptr) {
      REXLOG_ERROR("native-scene: shadow PSO creation failed");
      g_r.failed = true;
      return false;
    }
  }
  return true;
}

// The bone / ROPA / 2D-vertex upload rings (persistently mapped). The old
// RTV/DSV/SRV descriptor heaps and their increment-size bookkeeping are gone
// - render targets bind by texture and sampled views are backend-managed
// objects. (Name kept for the call sites.)
bool EnsureHeapsAndRings(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;

  if (!g_r.bone_ring) {
    g_r.bone_ring = CreateUploadBuffer(
        device, size_t(RendererState::kBoneRegionSize) * RendererState::kBoneRegions);
    g_r.bone_ring_cpu =
        g_r.bone_ring ? static_cast<uint8_t*>(device->Map(g_r.bone_ring))
                      : nullptr;
    if (g_r.bone_ring_cpu == nullptr) {
      g_r.failed = true;
      return false;
    }
  }

  if (!g_r.ropa_ring) {
    g_r.ropa_ring = CreateUploadBuffer(
        device, size_t(RendererState::kRopaRegionSize) * RendererState::kBoneRegions);
    g_r.ropa_ring_cpu =
        g_r.ropa_ring ? static_cast<uint8_t*>(device->Map(g_r.ropa_ring))
                      : nullptr;
    if (g_r.ropa_ring_cpu == nullptr) {
      g_r.failed = true;
      return false;
    }
  }

  if (!g_r.ui_ring) {
    g_r.ui_ring = CreateUploadBuffer(
        device, size_t(RendererState::kUiRegionSize) * RendererState::kUiRegions);
    g_r.ui_ring_cpu = g_r.ui_ring
                          ? static_cast<uint8_t*>(device->Map(g_r.ui_ring))
                          : nullptr;
    if (g_r.ui_ring_cpu == nullptr) {
      g_r.failed = true;
      return false;
    }
  }
  return true;
}

// ---- Photo-editor postfx chain (photo_fx.hlsl) -----------------------------
// Root signature + eight PSOs + the fixed-size intermediates. Lazy: built on
// the first photo-editor frame (a one-time ~100 ms compile the frozen-scene
// editor absorbs invisibly). Output-sized targets are (re)built per frame by
// the render block on size change.
bool EnsurePhotoFxPipeline(const NativeGuestOutputRenderContext& context) {
  if (g_r.pfx_ready) {
    return true;
  }
  if (g_r.pfx_failed) {
    return false;
  }
  nrhi::Device* device = context.device;
  const auto fail = [&](const char* what) {
    REXLOG_ERROR("native-scene: photo postfx pipeline setup failed ({})", what);
    g_r.pfx_failed = true;
    return false;
  };
  // Binding layout: CBV b0 + ONE eight-texture table t0..t7 + samplers. (The
  // old eight single-SRV tables would need nine Vulkan descriptor sets,
  // over the 8-set minimum on Intel/MoltenVK, so the passes gather their
  // t0..t7 views and bind them with one cmd->SetTextures(1, views, 8).)
  if (g_r.pfx_layout == nullptr) {
    nrhi::BindingLayoutDesc ld;
    ld.param_count = 2;
    ld.params[0] = {nrhi::BindingParamKind::kConstantBuffer, /*b*/ 0, 1,
                    nrhi::Visibility::kAll};
    ld.params[1] = {nrhi::BindingParamKind::kTextureTable, /*t*/ 0, 8,
                    nrhi::Visibility::kPixel};
    ld.static_sampler_count = 3;
    ld.static_samplers[0] = {/*s*/ 0, nrhi::Filter::kLinear,
                             nrhi::AddressMode::kClamp, 1};
    ld.static_samplers[1] = {1, nrhi::Filter::kPoint, nrhi::AddressMode::kClamp,
                             1};  // packed depth
    ld.static_samplers[2] = {2, nrhi::Filter::kLinear, nrhi::AddressMode::kWrap,
                             1};  // grain
    ld.allow_input_layout = false;
    g_r.pfx_layout = device->CreateBindingLayout(ld);
    if (g_r.pfx_layout == nullptr) {
      return fail("root signature");
    }
  }
  // Shaders + PSOs. Pass order matches RendererState::pfx_pso.
  struct Entry {
    const char* vs;
    const char* ps;
    nrhi::Format rtv;
  };
  const Entry entries[9] = {
      {"vs_raw", "ps_depthpack", nrhi::Format::kR8G8B8A8_UNORM},
      {"vs_offset", "ps_visualfx", nrhi::Format::kR8G8B8A8_UNORM},
      {"vs_offset", "ps_dof_down", nrhi::Format::kR8G8B8A8_UNORM},
      {"vs_offset", "ps_dof_mb", nrhi::Format::kR8G8B8A8_UNORM},
      {"vs_offset", "ps_dof", nrhi::Format::kR8G8B8A8_UNORM},
      {"vs_raw", "ps_uber", nrhi::Format::kR8G8B8A8_UNORM},
      {"vs_scaled", "ps_fisheye", context.guest_output->format()},
      {"vs_raw", "ps_blit", nrhi::Format::kR8G8B8A8_UNORM},
      // Debug visualizer (photo_native_debug cvar): CoC / packed-depth view
      // drawn over the output instead of the fisheye result.
      {"vs_raw", "ps_pfx_debug", context.guest_output->format()},
  };
  const nrhi::ShaderMacro msaa_defines[] = {{"PFX_MSAA", "1"},
                                            {nullptr, nullptr}};
  for (int i = 0; i < 9; ++i) {
    const bool msaa_pass = (i == 0 && g_r.msaa > 1);
    const nrhi::ShaderMacro* defs = msaa_pass ? msaa_defines : nullptr;
    const char* variant = msaa_pass ? "PFX_MSAA=1" : "";
    nrhi::Shader* vs = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kVertex, "photo_fx.hlsl",
                       kPhotoFxShaderSource, entries[i].vs, defs, variant));
    nrhi::Shader* ps = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "photo_fx.hlsl",
                       kPhotoFxShaderSource, entries[i].ps, defs, variant));
    if (vs == nullptr || ps == nullptr) {
      REXLOG_ERROR("native-scene: photo postfx shader compile failed ({})",
                   entries[i].ps);
      g_r.pfx_failed = true;
      return false;
    }
    nrhi::GraphicsPipelineDesc pso;
    pso.layout = g_r.pfx_layout;
    pso.vs = vs;
    pso.ps = ps;
    pso.cull = nrhi::CullMode::kNone;
    pso.depth_clip = true;
    pso.rtv_format = entries[i].rtv;
    pso.sample_count = 1;
    g_r.pfx_pso[i] = device->CreateGraphicsPipeline(pso);
    device->DestroyDeferred(vs);
    device->DestroyDeferred(ps);
    if (g_r.pfx_pso[i] == nullptr) {
      return fail(entries[i].ps);
    }
  }
  // Fixed-size intermediates + the identity grade LUT + the CB ring.
  {
    nrhi::TextureDesc desc;
    desc.format = nrhi::Format::kR8G8B8A8_UNORM;
    desc.usage = nrhi::kTextureUsageRenderTarget;
    desc.initial_state = nrhi::ResourceState::kRenderTarget;
    struct Fixed {
      nrhi::Texture** res;
      uint32_t w, h;
    };
    const Fixed fixed[3] = {
        {&g_r.pfx_half[0], RendererState::kPfxHalfW, RendererState::kPfxHalfH},
        {&g_r.pfx_half[1], RendererState::kPfxHalfW, RendererState::kPfxHalfH},
        {&g_r.pfx_quarter, RendererState::kPfxQuarterW, RendererState::kPfxQuarterH},
    };
    for (const Fixed& f : fixed) {
      if (*f.res != nullptr) {
        continue;
      }
      desc.width = f.w;
      desc.height = f.h;
      *f.res = device->CreateTexture(desc);
      if (*f.res == nullptr) {
        return fail("intermediate target");
      }
    }
    if (g_r.pfx_lut == nullptr) {
      // 32^3 identity grade LUT (the editor captures run with the LUT blend
      // weight at 0; identity keeps any treatment that enables it neutral
      // instead of garbage). Coordinate mapping mirrors the uber literals:
      // u = g*0.96875 + 0.015625, v = r*(-0.96875) + 0.984375 (flipped),
      // w = b*0.96875 + 0.015625, so voxel (ix,iy,iz) stores
      // r = (0.984375 - v)/0.96875, g = (u - 0.015625)/0.96875, b likewise.
      nrhi::TextureDesc lut;
      lut.kind = nrhi::TextureKind::k3D;
      lut.width = 32;
      lut.height = 32;
      lut.depth = 32;
      lut.format = nrhi::Format::kR8G8B8A8_UNORM;
      lut.initial_state = nrhi::ResourceState::kCopyDest;
      g_r.pfx_lut = device->CreateTexture(lut);
      if (g_r.pfx_lut == nullptr) {
        return fail("LUT");
      }
      // Upload: 32 rows of 32 texels x 32 slices, 256-byte row pitch.
      const uint32_t row_pitch = 256;
      const uint32_t slice_pitch = row_pitch * 32;
      g_r.pfx_lut_upload =
          CreateUploadBuffer(device, size_t(slice_pitch) * 32, nrhi::BufferBindClass::kCopySrc);
      if (g_r.pfx_lut_upload == nullptr) {
        return fail("LUT upload");
      }
      uint8_t* p = static_cast<uint8_t*>(device->Map(g_r.pfx_lut_upload));
      if (p == nullptr) {
        return fail("LUT map");
      }
      for (uint32_t iz = 0; iz < 32; ++iz) {
        for (uint32_t iy = 0; iy < 32; ++iy) {
          uint8_t* row = p + size_t(iz) * slice_pitch + size_t(iy) * row_pitch;
          const float v = (float(iy) + 0.5f) / 32.0f;
          const float w = (float(iz) + 0.5f) / 32.0f;
          const float r = (0.984375f - v) / 0.96875f;
          const float b = (w - 0.015625f) / 0.96875f;
          for (uint32_t ix = 0; ix < 32; ++ix) {
            const float u = (float(ix) + 0.5f) / 32.0f;
            const float g = (u - 0.015625f) / 0.96875f;
            row[ix * 4 + 0] = uint8_t(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
            row[ix * 4 + 1] = uint8_t(std::clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
            row[ix * 4 + 2] = uint8_t(std::clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
            row[ix * 4 + 3] = 255;
          }
        }
      }
      device->Unmap(g_r.pfx_lut_upload);
    }
    if (g_r.pfx_cb == nullptr) {
      // 8 pass slots x 4 KB x 4 frames in flight.
      // 10 slots per frame region: depth pack + accum feed + 6 passes +
      // accum mode 2 + the debug view.
      g_r.pfx_cb = CreateUploadBuffer(device, 10u * 4096u * 4u);
      g_r.pfx_cb_ptr = g_r.pfx_cb
                           ? static_cast<uint8_t*>(device->Map(g_r.pfx_cb))
                           : nullptr;
      if (g_r.pfx_cb_ptr == nullptr) {
        return fail("constant ring");
      }
    }
  }
  // Fixed-target views (2/3 = halves, 4 = quarter, 6 = LUT; 0/1/5/7, the
  // output-sized full/depth targets and the native depth resource, are
  // (re)created by the render block on size change).
  if (!g_r.pfx_srv_allocated) {
    nrhi::TextureViewDesc vd;
    vd.mip_levels = 1;
    g_r.pfx_srv[2] = device->CreateTextureView(g_r.pfx_half[0], vd);
    g_r.pfx_srv[3] = device->CreateTextureView(g_r.pfx_half[1], vd);
    g_r.pfx_srv[4] = device->CreateTextureView(g_r.pfx_quarter, vd);
    // LUT SRV (Texture3D).
    nrhi::TextureViewDesc lv;
    lv.dimension = nrhi::ViewDimension::k3D;
    lv.mip_levels = 1;
    g_r.pfx_srv[6] = device->CreateTextureView(g_r.pfx_lut, lv);
    if (g_r.pfx_srv[2] == nullptr || g_r.pfx_srv[3] == nullptr ||
        g_r.pfx_srv[4] == nullptr || g_r.pfx_srv[6] == nullptr) {
      return fail("fixed-target views");
    }
    g_r.pfx_srv_allocated = true;
  }
  g_r.pfx_ready = true;
  REXLOG_INFO("native-scene: photo postfx pipeline ready (9 passes, msaa={})",
              g_r.msaa);
  return true;
}

// Shadow atlas targets + the always-bound b1 receiver constant buffer.
bool EnsureShadowResources(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;
  // Cascade tile size: explicit, or auto = the game's 512 tiles at the
  // RENDER resolution scale. guest_output is the scaled frontbuffer (720p x
  // the draw resolution scale, independent of window/monitor size), so
  // deriving from its height follows the Resolution Scale setting, including
  // any device-limit clamping the texture cache applied, and gives the same
  // effective shadow raster the emulated GPU renders at that scale.
  const int32_t tile_cfg = REXCVAR_GET(skate3_native_render_scene_shadow_tile);
  const uint32_t automatic_tile =
      std::min(512u * std::max(
                          1u, (context.guest_output_height + 719u) / 720u),
               4096u);
  const uint32_t want_tile =
      tile_cfg > 0
          ? uint32_t(tile_cfg)
          : (mechanics_sandbox::VisualMapEnabled()
                 ? std::max(automatic_tile, 2048u)
                 : automatic_tile);
  if (g_r.shadow_raw != nullptr && g_r.shadow_tile != want_tile) {
    // Hot tile-size change: retire the atlas chain; recreated below. The
    // new atlas starts in RENDER_TARGET state and is re-rendered by this
    // frame's shadow pass.
    nrhi::Texture** targets[3] = {&g_r.shadow_raw, &g_r.shadow_mid,
                                  &g_r.shadow_final};
    nrhi::TextureView** views[3] = {&g_r.shadow_srv_raw, &g_r.shadow_srv_mid,
                                    &g_r.shadow_srv_final};
    for (int t = 0; t < 3; ++t) {
      if (*views[t] != nullptr) {
        device->DestroyDeferred(*views[t]);
        *views[t] = nullptr;
      }
      if (*targets[t] != nullptr) {
        device->DestroyDeferred(*targets[t]);
        *targets[t] = nullptr;
      }
    }
  }
  if (!g_r.shadow_raw && REXCVAR_GET(skate3_native_render_scene_shadows)) {
    // Dynamic-shadow atlas chain: raw casters -> hblur intermediate ->
    // blurred final (the texture the scene pass samples). Three fixed-size
    // R16G16_UNORM targets (the game's atlas is fmt 25 = 16_16 fixed point;
    // half-float ulp at the typical ~0.85 depth is ~6 mm of world height,
    // too coarse for board/feet-height casters 1-2 cm off the ground),
    // 3 tiles of tile x tile each.
    g_r.shadow_tile = want_tile;
    nrhi::TextureDesc desc;
    desc.width = g_r.shadow_tile * 3;
    desc.height = g_r.shadow_tile;
    desc.format = nrhi::Format::kR16G16_UNORM;
    desc.usage = nrhi::kTextureUsageRenderTarget;
    desc.initial_state = nrhi::ResourceState::kRenderTarget;
    desc.clear_color[0] = 1.0f;  // depth: far
    desc.clear_color[1] = 1.0f;  // "uncoverage": empty
    nrhi::Texture** targets[3] = {&g_r.shadow_raw, &g_r.shadow_mid,
                                  &g_r.shadow_final};
    nrhi::TextureView** views[3] = {&g_r.shadow_srv_raw, &g_r.shadow_srv_mid,
                                    &g_r.shadow_srv_final};
    for (int t = 0; t < 3; ++t) {
      // The raw and final tiles are the shadow-dump readback sources, so
      // those two also need copy-source usage; the intermediate is never
      // copied and stays render-target only.
      desc.usage = nrhi::kTextureUsageRenderTarget |
                   (t != 1 ? nrhi::kTextureUsageCopySource
                           : nrhi::kTextureUsageNone);
      *targets[t] = device->CreateTexture(desc);
      if (*targets[t] == nullptr) {
        REXLOG_ERROR("native-scene: shadow atlas creation failed");
        g_r.failed = true;
        return false;
      }
      nrhi::TextureViewDesc vd;
      vd.mip_levels = 1;
      *views[t] = device->CreateTextureView(*targets[t], vd);
      if (*views[t] == nullptr) {
        REXLOG_ERROR("native-scene: shadow atlas creation failed");
        g_r.failed = true;
        return false;
      }
    }
    g_r.shadow_in_srv_state = false;
    REXLOG_INFO("native-scene: shadow atlas created ({}x{} tiles{})",
                g_r.shadow_tile, g_r.shadow_tile,
                tile_cfg > 0 ? "" : ", auto");
  }
  if (!g_r.world_shadow && g_r.shadow_raw != nullptr) {
    // dynamicobject static world-shadow map (see RendererState). Fixed at
    // the game's own 512x512: a coarse region-scale baked-shade map whose
    // PCF footprint the props' shading was tuned against.
    nrhi::TextureDesc desc;
    desc.width = RendererState::kWorldShadowSize;
    desc.height = RendererState::kWorldShadowSize;
    desc.format = nrhi::Format::kR16G16_UNORM;
    desc.usage = nrhi::kTextureUsageRenderTarget;
    desc.initial_state = nrhi::ResourceState::kRenderTarget;
    desc.clear_color[0] = 1.0f;  // depth: far = lit
    desc.clear_color[1] = 1.0f;
    g_r.world_shadow = device->CreateTexture(desc);
    if (g_r.world_shadow != nullptr) {
      nrhi::TextureViewDesc vd;
      vd.mip_levels = 1;
      g_r.world_shadow_srv = device->CreateTextureView(g_r.world_shadow, vd);
    }
    if (g_r.world_shadow_srv == nullptr) {
      REXLOG_ERROR("native-scene: world-shadow map creation failed");
      g_r.failed = true;
      return false;
    }
    g_r.world_shadow_in_srv = false;
    g_r.world_shadow_primed = false;
  }
  uint32_t want_static_size = uint32_t(std::clamp(
      REXCVAR_GET(skate3_native_render_scene_shadow_static_size), 1024, 8192));
  if (device->backend() == nrhi::Backend::kD3D12) {
    // The 3-tile cascade row must fit D3D12's 16384 2D-texture width cap:
    // per-tile sizes above 5461 cannot allocate there (the 8192 setting
    // asks for 24576x8192), while desktop Vulkan allows 32768-wide.
    while (want_static_size * 3 > 16384) {
      want_static_size /= 2;
    }
  }
  if (g_r.static_sun != nullptr && g_r.static_sun_requested != want_static_size) {
    // Hot size change: retire the map; recreated below. The recreated map
    // is uninitialized, so force the cross-frame cache to re-render it
    // (RenderStaticSunMap rebuilds whenever nsm_built_radius <= 0).
    if (g_r.static_sun_srv != nullptr) {
      device->DestroyDeferred(g_r.static_sun_srv);
      g_r.static_sun_srv = nullptr;
    }
    device->DestroyDeferred(g_r.static_sun);
    g_r.static_sun = nullptr;
    if (g_r.static_sun_history_srv != nullptr) {
      device->DestroyDeferred(g_r.static_sun_history_srv);
      g_r.static_sun_history_srv = nullptr;
    }
    if (g_r.static_sun_history != nullptr) {
      device->DestroyDeferred(g_r.static_sun_history);
      g_r.static_sun_history = nullptr;
    }
    g_r.static_sun_history_valid = false;
    g_r.nsm_progressive_build = false;
    g_r.nsm_built_radius = 0.0f;
  }
  const uint32_t owned_cascade_sizes[3] = {
      std::min(want_static_size, 2048u),
      std::min(want_static_size, 1536u),
      std::min(want_static_size, 1024u),
  };
  bool retire_owned_cascades = false;
  for (uint32_t cascade = 0; cascade < 3; ++cascade) {
    retire_owned_cascades |=
        g_r.owned_static_sun[cascade] != nullptr &&
        g_r.owned_static_sun_size[cascade] != owned_cascade_sizes[cascade];
  }
  if (retire_owned_cascades) {
    for (uint32_t cascade = 0; cascade < 3; ++cascade) {
      if (g_r.owned_static_sun_srv[cascade] != nullptr) {
        device->DestroyDeferred(g_r.owned_static_sun_srv[cascade]);
        g_r.owned_static_sun_srv[cascade] = nullptr;
      }
      if (g_r.owned_static_sun[cascade] != nullptr) {
        device->DestroyDeferred(g_r.owned_static_sun[cascade]);
        g_r.owned_static_sun[cascade] = nullptr;
      }
      g_r.owned_static_sun_valid[cascade] = false;
      g_r.owned_static_sun_in_srv[cascade] = false;
      g_r.owned_nsm_last_update[cascade] = 0;
      g_r.owned_nsm_next_update[cascade] = {};
    }
  }
  if (!g_r.static_sun && g_r.shadow_raw != nullptr &&
      REXCVAR_GET(skate3_native_render_scene_shadow_static_casters)) {
    // Native static sun-shadow map (see RendererState). THREE cascade
    // tiles side by side: inner (r/6, centimeter contact detail with
    // useful reach), mid (r/2) and far (full radius, large-caster
    // coverage); size is per tile.
    g_r.static_sun_requested = want_static_size;
    g_r.static_sun_size = want_static_size;
    nrhi::TextureDesc desc;
    desc.width = g_r.static_sun_size * 3;
    desc.height = g_r.static_sun_size;
    desc.format = nrhi::Format::kR16G16_UNORM;
    desc.usage = nrhi::kTextureUsageRenderTarget;
    desc.initial_state = nrhi::ResourceState::kRenderTarget;
    desc.clear_color[0] = 1.0f;  // depth: far = lit
    desc.clear_color[1] = 1.0f;
    g_r.static_sun = device->CreateTexture(desc);
    // Allocation-failure fallback (VRAM pressure, driver limits): a
    // coarser static sun map beats marking the whole renderer failed and
    // dropping the session to emulated output.
    while (g_r.static_sun == nullptr && g_r.static_sun_size > 1024) {
      g_r.static_sun_size /= 2;
      desc.width = g_r.static_sun_size * 3;
      desc.height = g_r.static_sun_size;
      REXLOG_WARN(
          "native-scene: static sun-shadow map allocation failed, retrying "
          "at {}x{}",
          g_r.static_sun_size * 3, g_r.static_sun_size);
      g_r.static_sun = device->CreateTexture(desc);
    }
    if (g_r.static_sun != nullptr) {
      nrhi::TextureViewDesc vd;
      vd.mip_levels = 1;
      g_r.static_sun_srv = device->CreateTextureView(g_r.static_sun, vd);
    }
    if (g_r.static_sun_srv == nullptr) {
      REXLOG_ERROR("native-scene: static sun-shadow map creation failed");
      g_r.failed = true;
      return false;
    }
    g_r.static_sun_in_srv = false;
    // A second same-resolution atlas preserves the previous sun sample
    // while the newly rendered one fades in. Failure is non-fatal: the
    // renderer falls back to the original single-atlas stepping behavior
    // rather than dropping native presentation.
    desc.initial_state = nrhi::ResourceState::kRenderTarget;
    g_r.static_sun_history = device->CreateTexture(desc);
    if (g_r.static_sun_history != nullptr) {
      nrhi::TextureViewDesc vd;
      vd.mip_levels = 1;
      g_r.static_sun_history_srv =
          device->CreateTextureView(g_r.static_sun_history, vd);
    }
    if (g_r.static_sun_history_srv == nullptr) {
      if (g_r.static_sun_history != nullptr) {
        device->DestroyDeferred(g_r.static_sun_history);
        g_r.static_sun_history = nullptr;
      }
      REXLOG_WARN(
          "native-scene: static sun-shadow history atlas unavailable; "
          "temporal blending disabled");
    }
    g_r.static_sun_history_in_srv = false;
    g_r.static_sun_history_valid = false;
    REXLOG_INFO(
        "native-scene: static sun-shadow map created ({}x{}, 3 cascades, "
        "temporal_blend={})",
        g_r.static_sun_size * 3, g_r.static_sun_size,
        g_r.static_sun_history != nullptr ? 1 : 0);
  }
  if (g_r.shadow_raw != nullptr &&
      REXCVAR_GET(skate3_native_render_scene_shadow_static_casters)) {
    for (uint32_t cascade = 0; cascade < 3; ++cascade) {
      if (g_r.owned_static_sun[cascade] != nullptr) {
        continue;
      }
      g_r.owned_static_sun_size[cascade] = owned_cascade_sizes[cascade];
      nrhi::TextureDesc desc;
      desc.width = owned_cascade_sizes[cascade];
      desc.height = owned_cascade_sizes[cascade];
      desc.format = nrhi::Format::kR16G16_UNORM;
      desc.usage = nrhi::kTextureUsageRenderTarget;
      desc.initial_state = nrhi::ResourceState::kRenderTarget;
      desc.clear_color[0] = 1.0f;
      desc.clear_color[1] = 1.0f;
      g_r.owned_static_sun[cascade] = device->CreateTexture(desc);
      if (g_r.owned_static_sun[cascade] != nullptr) {
        nrhi::TextureViewDesc vd;
        vd.mip_levels = 1;
        g_r.owned_static_sun_srv[cascade] =
            device->CreateTextureView(g_r.owned_static_sun[cascade], vd);
      }
      if (g_r.owned_static_sun_srv[cascade] == nullptr) {
        REXLOG_ERROR(
            "native-scene: owned static cascade {} creation failed ({}x{})",
            cascade, owned_cascade_sizes[cascade],
            owned_cascade_sizes[cascade]);
        g_r.failed = true;
        return false;
      }
      g_r.owned_static_sun_in_srv[cascade] = false;
      g_r.owned_static_sun_valid[cascade] = false;
    }
    static bool s_logged_owned_cascades = false;
    if (!s_logged_owned_cascades) {
      s_logged_owned_cascades = true;
      REXLOG_INFO(
          "native-scene: independent owned static cascades created "
          "(near={} mid={} far={})",
          owned_cascade_sizes[0], owned_cascade_sizes[1],
          owned_cascade_sizes[2]);
    }
  }
  if (!g_r.shadow_cb) {
    // Always created (even with shadows off): the scene PS declares b1 and
    // a root CBV must be bound; a zeroed block disables the shadow branch.
    g_r.shadow_cb = CreateUploadBuffer(
        device, size_t(RendererState::kShadowCbSlice) * RendererState::kShadowCbRegions);
    g_r.shadow_cb_cpu =
        g_r.shadow_cb ? static_cast<uint8_t*>(device->Map(g_r.shadow_cb))
                      : nullptr;
    if (g_r.shadow_cb_cpu == nullptr) {
      g_r.failed = true;
      return false;
    }
  }
  return true;
}

// Blur intermediates + the output-sized selection-outline mask. Failures
// only disable their feature, never abort the native path.
bool EnsureBlurOutlineTargets(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;
  if (g_r.blur_tex[0] == nullptr && g_r.pso_blur != nullptr) {
    // Popup background blur intermediates at the game's fixed 1152x640
    // internal resolution (the bilinear stretch back to the output is what
    // produces the authentic frosted-glass lattice).
    nrhi::TextureDesc desc;
    desc.width = RendererState::kBlurWidth;
    desc.height = RendererState::kBlurHeight;
    desc.format = context.guest_output->format();
    desc.initial_state = nrhi::ResourceState::kRenderTarget;
    bool ok = true;
    for (int t = 0; t < 2 && ok; ++t) {
      // blur_tex[0] is also the photo-grab readback source (copied into the
      // grab readback buffers), so it alone needs copy-source usage.
      desc.usage = nrhi::kTextureUsageRenderTarget |
                   (t == 0 ? nrhi::kTextureUsageCopySource
                           : nrhi::kTextureUsageNone);
      g_r.blur_tex[t] = device->CreateTexture(desc);
      if (g_r.blur_tex[t] == nullptr) {
        ok = false;
        break;
      }
      nrhi::TextureViewDesc vd;
      vd.mip_levels = 1;
      g_r.blur_srv[t] = device->CreateTextureView(g_r.blur_tex[t], vd);
      if (g_r.blur_srv[t] == nullptr) {
        ok = false;
        break;
      }
    }
    if (!ok) {
      REXLOG_ERROR("native-scene: blur intermediate creation failed; blur disabled");
      for (int t = 0; t < 2; ++t) {
        if (g_r.blur_srv[t]) device->DestroyDeferred(g_r.blur_srv[t]);
        g_r.blur_srv[t] = nullptr;
        if (g_r.blur_tex[t]) device->DestroyDeferred(g_r.blur_tex[t]);
        g_r.blur_tex[t] = nullptr;
      }
      g_r.pso_blur = nullptr;  // leaked PSO acceptable on this cold path
    }
  }

  // Settings-menu gaussian intermediates: half the output resolution
  // (sigma halves with it; the final bilinear stretch back is invisible
  // under a real blur), recreated whenever the output size changes.
  const uint32_t menu_w = std::max(1u, context.guest_output_width / 2);
  const uint32_t menu_h = std::max(1u, context.guest_output_height / 2);
  if ((g_r.menu_blur_tex[0] == nullptr || g_r.menu_blur_w != menu_w ||
       g_r.menu_blur_h != menu_h) &&
      g_r.pso_menu_gauss != nullptr) {
    for (int t = 0; t < 2; ++t) {
      if (g_r.menu_blur_srv[t]) device->DestroyDeferred(g_r.menu_blur_srv[t]);
      g_r.menu_blur_srv[t] = nullptr;
      if (g_r.menu_blur_tex[t]) device->DestroyDeferred(g_r.menu_blur_tex[t]);
      g_r.menu_blur_tex[t] = nullptr;
    }
    nrhi::TextureDesc desc;
    desc.width = menu_w;
    desc.height = menu_h;
    desc.format = context.guest_output->format();
    desc.initial_state = nrhi::ResourceState::kRenderTarget;
    desc.usage = nrhi::kTextureUsageRenderTarget;
    bool ok = true;
    for (int t = 0; t < 2 && ok; ++t) {
      g_r.menu_blur_tex[t] = device->CreateTexture(desc);
      if (g_r.menu_blur_tex[t] == nullptr) {
        ok = false;
        break;
      }
      nrhi::TextureViewDesc vd;
      vd.mip_levels = 1;
      g_r.menu_blur_srv[t] = device->CreateTextureView(g_r.menu_blur_tex[t], vd);
      if (g_r.menu_blur_srv[t] == nullptr) {
        ok = false;
        break;
      }
    }
    if (ok) {
      g_r.menu_blur_w = menu_w;
      g_r.menu_blur_h = menu_h;
    } else {
      REXLOG_ERROR(
          "native-scene: menu blur intermediate creation failed; menu blur "
          "disabled");
      for (int t = 0; t < 2; ++t) {
        if (g_r.menu_blur_srv[t]) device->DestroyDeferred(g_r.menu_blur_srv[t]);
        g_r.menu_blur_srv[t] = nullptr;
        if (g_r.menu_blur_tex[t]) device->DestroyDeferred(g_r.menu_blur_tex[t]);
        g_r.menu_blur_tex[t] = nullptr;
      }
      g_r.pso_menu_gauss = nullptr;  // leaked PSO acceptable on this cold path
    }
  }

  if ((g_r.outline_mask == nullptr ||
       g_r.outline_mask_width != context.guest_output_width ||
       g_r.outline_mask_height != context.guest_output_height) &&
      g_r.pso_outline_mask != nullptr && g_r.pso_outline_edge != nullptr) {
    // Selection-outline mask: single-sample R8 target at output resolution
    // (a 1152x640 mask left the contour centerline visibly stairstepped;
    // the mask's own rasterization aliasing survives any amount of
    // downstream filtering).
    if (g_r.outline_mask) {
      device->DestroyDeferred(g_r.outline_mask);
      g_r.outline_mask = nullptr;
    }
    nrhi::TextureDesc desc;
    desc.width = context.guest_output_width;
    desc.height = context.guest_output_height;
    desc.format = nrhi::Format::kR8_UNORM;
    desc.usage = nrhi::kTextureUsageRenderTarget;
    desc.initial_state = nrhi::ResourceState::kRenderTarget;
    g_r.outline_mask = device->CreateTexture(desc);
    if (g_r.outline_mask == nullptr) {
      REXLOG_ERROR("native-scene: outline mask creation failed; outline disabled");
      g_r.pso_outline_edge = nullptr;
    } else {
      g_r.outline_mask_width = context.guest_output_width;
      g_r.outline_mask_height = context.guest_output_height;
      // Re-point the mask view at the recreated texture.
      if (g_r.outline_mask_srv) {
        device->DestroyDeferred(g_r.outline_mask_srv);
        g_r.outline_mask_srv = nullptr;
      }
      nrhi::TextureViewDesc vd;
      vd.mip_levels = 1;
      g_r.outline_mask_srv = device->CreateTextureView(g_r.outline_mask, vd);
      g_r.outline_mask_srv_allocated = g_r.outline_mask_srv != nullptr;
    }
  }
  return true;
}

// 1x1 white diffuse fallback + 1x1x6 mid-gray environment-cube fallback.
bool EnsureFallbackTextures(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;
  if (!g_r.white.valid) {
    // 1x1 white fallback for items without a resolved diffuse texture.
    nrhi::TextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.format = nrhi::Format::kR8G8B8A8_UNORM;
    desc.initial_state = nrhi::ResourceState::kCopyDest;
    g_r.white.texture = device->CreateTexture(desc);
    if (g_r.white.texture == nullptr) {
      g_r.failed = true;
      return false;
    }
    g_r.white.upload = CreateUploadBuffer(device, 256, nrhi::BufferBindClass::kCopySrc);
    if (!g_r.white.upload) {
      g_r.failed = true;
      return false;
    }
    uint8_t* mapping = static_cast<uint8_t*>(device->Map(g_r.white.upload));
    std::memset(mapping, 0xFF, 4);
    device->Unmap(g_r.white.upload);
    context.cmd->CopyBufferToTexture(g_r.white.texture, 0, 0, g_r.white.upload,
                                     0, 256, 1, 1, 1);
    context.cmd->Barrier(g_r.white.texture, nrhi::ResourceState::kCopyDest,
                         nrhi::ResourceState::kPixelShaderResource);
    g_r.device->DestroyDeferred(g_r.white.upload);
    g_r.white.upload = nullptr;
    nrhi::TextureViewDesc vd;
    vd.mip_levels = 1;
    g_r.white.srv = device->CreateTextureView(g_r.white.texture, vd);
    if (g_r.white.srv == nullptr) {
      g_r.failed = true;
      return false;
    }
    g_r.white.valid = true;
  }
  if (!g_r.white_cube.valid) {
    // 1x1x6 mid-gray fallback cube for the water reflection slot (t6): a
    // TextureCube SRV must always be bound where the shader declares one.
    nrhi::TextureDesc desc;
    desc.kind = nrhi::TextureKind::kCube;
    desc.width = 1;
    desc.height = 1;
    desc.format = nrhi::Format::kR8G8B8A8_UNORM;
    desc.initial_state = nrhi::ResourceState::kCopyDest;
    g_r.white_cube.texture = device->CreateTexture(desc);
    if (g_r.white_cube.texture == nullptr) {
      g_r.failed = true;
      return false;
    }
    g_r.white_cube.upload = CreateUploadBuffer(device, 512 * 6, nrhi::BufferBindClass::kCopySrc);
    if (!g_r.white_cube.upload) {
      g_r.failed = true;
      return false;
    }
    uint8_t* mapping = static_cast<uint8_t*>(device->Map(g_r.white_cube.upload));
    for (uint32_t f = 0; f < 6; ++f) {
      std::memset(mapping + f * 512, 0x80, 4);
    }
    device->Unmap(g_r.white_cube.upload);
    for (uint32_t f = 0; f < 6; ++f) {
      context.cmd->CopyBufferToTexture(g_r.white_cube.texture, 0, f,
                                       g_r.white_cube.upload, f * 512, 256, 1,
                                       1, 1);
    }
    context.cmd->Barrier(g_r.white_cube.texture, nrhi::ResourceState::kCopyDest,
                         nrhi::ResourceState::kPixelShaderResource);
    g_r.device->DestroyDeferred(g_r.white_cube.upload);
    g_r.white_cube.upload = nullptr;
    nrhi::TextureViewDesc vd;
    vd.dimension = nrhi::ViewDimension::kCube;
    vd.mip_levels = 1;
    g_r.white_cube.srv = device->CreateTextureView(g_r.white_cube.texture, vd);
    if (g_r.white_cube.srv == nullptr) {
      g_r.failed = true;
      return false;
    }
    g_r.white_cube.valid = true;
  }
  return true;
}

enum class OwnedTextureRole : uint8_t {
  Color,
  Normal,
  Data,
};

nrhi::TextureView* ResolveOwnedMapTexture(
    const NativeGuestOutputRenderContext& context,
    skate::world::TextureId texture_id,
    OwnedTextureRole role = OwnedTextureRole::Color) {
  if (texture_id == 0) {
    return g_r.white.srv;
  }
  const auto cached = g_r.owned_map_textures.find(texture_id);
  if (cached != g_r.owned_map_textures.end()) {
    return cached->second.valid ? cached->second.srv : g_r.white.srv;
  }

  const skate::world::ImageTexture* source =
      mechanics_sandbox::map::ActiveImageTexture(texture_id);
  if (source == nullptr || source->width == 0 || source->height == 0 ||
      source->rgba8.size() !=
          std::size_t(source->width) * source->height * 4u) {
    REXLOG_ERROR(
        "native-scene: owned map texture {} is missing or malformed",
        texture_id);
    g_r.owned_map_textures.emplace(texture_id, GuestTexture{});
    return g_r.white.srv;
  }

  GuestTexture texture;
  // Imported images are stored in scene-linear RGBA8. Build a complete
  // CPU-filtered mip chain once at first use so the scene's 8x anisotropic
  // sampler has actual lower-frequency data at distance. Normal maps need
  // vector-aware filtering; averaging their encoded RGB directly shortens
  // the normal and produces dark, lumpy specular response.
  std::vector<std::vector<std::uint8_t>> mips;
  mips.push_back(source->rgba8);
  std::uint32_t mip_width = source->width;
  std::uint32_t mip_height = source->height;
  while (mip_width > 1 || mip_height > 1) {
    const std::uint32_t next_width = std::max(mip_width >> 1, 1u);
    const std::uint32_t next_height = std::max(mip_height >> 1, 1u);
    const std::vector<std::uint8_t>& input = mips.back();
    std::vector<std::uint8_t> output(
        std::size_t(next_width) * next_height * 4u);
    for (std::uint32_t y = 0; y < next_height; ++y) {
      const std::uint32_t y0 = std::min(y * 2u, mip_height - 1u);
      const std::uint32_t y1 = std::min(y0 + 1u, mip_height - 1u);
      for (std::uint32_t x = 0; x < next_width; ++x) {
        const std::uint32_t x0 = std::min(x * 2u, mip_width - 1u);
        const std::uint32_t x1 = std::min(x0 + 1u, mip_width - 1u);
        const std::size_t source_pixels[4] = {
            (std::size_t(y0) * mip_width + x0) * 4u,
            (std::size_t(y0) * mip_width + x1) * 4u,
            (std::size_t(y1) * mip_width + x0) * 4u,
            (std::size_t(y1) * mip_width + x1) * 4u,
        };
        const std::size_t destination =
            (std::size_t(y) * next_width + x) * 4u;
        if (role == OwnedTextureRole::Normal) {
          float normal[3] = {};
          for (const std::size_t pixel : source_pixels) {
            normal[0] += float(input[pixel + 0]) * (2.0f / 255.0f) - 1.0f;
            normal[1] += float(input[pixel + 1]) * (2.0f / 255.0f) - 1.0f;
            normal[2] += float(input[pixel + 2]) * (2.0f / 255.0f) - 1.0f;
          }
          const float length = std::sqrt(
              normal[0] * normal[0] + normal[1] * normal[1] +
              normal[2] * normal[2]);
          if (length > 1.0e-6f) {
            for (float& component : normal) {
              component /= length;
            }
          } else {
            normal[0] = normal[1] = 0.0f;
            normal[2] = 1.0f;
          }
          for (int channel = 0; channel < 3; ++channel) {
            output[destination + channel] = std::uint8_t(std::clamp(
                std::lround((normal[channel] * 0.5f + 0.5f) * 255.0f),
                0l, 255l));
          }
        } else {
          for (int channel = 0; channel < 3; ++channel) {
            const std::uint32_t sum =
                input[source_pixels[0] + channel] +
                input[source_pixels[1] + channel] +
                input[source_pixels[2] + channel] +
                input[source_pixels[3] + channel];
            output[destination + channel] =
                std::uint8_t((sum + 2u) / 4u);
          }
        }
        const std::uint32_t alpha =
            input[source_pixels[0] + 3] + input[source_pixels[1] + 3] +
            input[source_pixels[2] + 3] + input[source_pixels[3] + 3];
        output[destination + 3] = std::uint8_t((alpha + 2u) / 4u);
      }
    }
    mips.emplace_back(std::move(output));
    mip_width = next_width;
    mip_height = next_height;
  }
  struct MipUpload {
    std::uint64_t offset = 0;
    std::uint32_t row_pitch = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
  };
  std::vector<MipUpload> uploads(mips.size());
  std::uint64_t upload_size = 0;
  for (std::size_t mip = 0; mip < mips.size(); ++mip) {
    MipUpload& upload = uploads[mip];
    upload.width = std::max(source->width >> mip, 1u);
    upload.height = std::max(source->height >> mip, 1u);
    const std::uint32_t row_bytes = upload.width * 4u;
    upload.row_pitch =
        (row_bytes + nrhi::kRowPitchAlignment - 1u) &
        ~(nrhi::kRowPitchAlignment - 1u);
    upload.offset =
        (upload_size + kUploadPlacementAlignment - 1u) &
        ~(std::uint64_t(kUploadPlacementAlignment) - 1u);
    upload_size =
        upload.offset + std::uint64_t(upload.row_pitch) * upload.height;
  }

  nrhi::TextureDesc desc;
  desc.width = source->width;
  desc.height = source->height;
  desc.mip_levels = static_cast<std::uint32_t>(mips.size());
  desc.format = nrhi::Format::kR8G8B8A8_UNORM;
  desc.initial_state = nrhi::ResourceState::kCopyDest;
  texture.texture = context.device->CreateTexture(desc);
  texture.upload = CreateUploadBuffer(
      context.device, upload_size,
      nrhi::BufferBindClass::kCopySrc);
  if (texture.texture == nullptr || texture.upload == nullptr) {
    context.device->DestroyDeferred(texture.texture);
    context.device->DestroyDeferred(texture.upload);
    REXLOG_ERROR(
        "native-scene: GPU allocation failed for owned map texture {} "
        "({}x{})",
        source->name, source->width, source->height);
    g_r.owned_map_textures.emplace(texture_id, GuestTexture{});
    return g_r.white.srv;
  }

  auto* destination =
      static_cast<std::uint8_t*>(context.device->Map(texture.upload));
  if (destination == nullptr) {
    context.device->DestroyDeferred(texture.texture);
    context.device->DestroyDeferred(texture.upload);
    g_r.owned_map_textures.emplace(texture_id, GuestTexture{});
    return g_r.white.srv;
  }
  for (std::size_t mip = 0; mip < mips.size(); ++mip) {
    const MipUpload& upload = uploads[mip];
    const std::size_t row_bytes = std::size_t(upload.width) * 4u;
    for (std::uint32_t row = 0; row < upload.height; ++row) {
      std::memcpy(
          destination + upload.offset +
              std::uint64_t(row) * upload.row_pitch,
          mips[mip].data() + std::size_t(row) * row_bytes,
          row_bytes);
    }
  }
  context.device->Unmap(texture.upload);
  for (std::size_t mip = 0; mip < mips.size(); ++mip) {
    const MipUpload& upload = uploads[mip];
    context.cmd->CopyBufferToTexture(
        texture.texture, static_cast<std::uint32_t>(mip), 0,
        texture.upload, upload.offset, upload.row_pitch,
        upload.width, upload.height, 1);
  }
  context.cmd->Barrier(
      texture.texture, nrhi::ResourceState::kCopyDest,
      nrhi::ResourceState::kPixelShaderResource);
  context.device->DestroyDeferred(texture.upload);
  texture.upload = nullptr;
  nrhi::TextureViewDesc view;
  view.mip_levels = static_cast<std::uint32_t>(mips.size());
  texture.srv = context.device->CreateTextureView(texture.texture, view);
  texture.valid = texture.srv != nullptr;
  if (!texture.valid) {
    context.device->DestroyDeferred(texture.texture);
    texture.texture = nullptr;
  }
  nrhi::TextureView* resolved =
      texture.valid ? texture.srv : g_r.white.srv;
  g_r.owned_map_textures.emplace(texture_id, std::move(texture));
  REXLOG_INFO(
      "native-scene: uploaded owned map texture {} '{}' ({}x{}, {} mips)",
      texture_id, source->name, source->width, source->height, mips.size());
  return resolved;
}

// Depth buffer + MSAA color target, rebuilt on output-size change.
bool EnsureOutputSizedTargets(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;
  const uint32_t width = context.guest_output_width;
  const uint32_t height = context.guest_output_height;
  const nrhi::Format want_scene_fmt =
      g_r.hdr_active ? g_r.hdr_scene_format : nrhi::Format::kUnknown;
  if (!g_r.depth || g_r.depth_width != width || g_r.depth_height != height ||
      g_r.targets_hdr != g_r.hdr_active ||
      g_r.targets_scene_fmt != want_scene_fmt ||
      g_r.targets_msaa != g_r.msaa) {
    if (g_r.depth) {
      // The AO/SSR/volumetric scene-depth SRVs alias this texture and
      // re-point on pointer identity; heap reuse can hand the NEW depth
      // texture the OLD one's address, so that comparison must never be
      // the only invalidation. Retire the views with the buffer they view:
      // a stale view kept sampling the retired depth image (still in
      // attachment layout), a GPU fault and device hang on Vulkan.
      nrhi::TextureView** depth_views[3] = {
          &g_r.ao_depth_srv, &g_r.ssr_depth_srv, &g_r.vol_depth_srv};
      nrhi::Texture** depth_views_of[3] = {
          &g_r.ao_depth_srv_of, &g_r.ssr_depth_srv_of, &g_r.vol_depth_srv_of};
      for (int v = 0; v < 3; ++v) {
        if (*depth_views[v] != nullptr) {
          g_r.device->DestroyDeferred(*depth_views[v]);
          *depth_views[v] = nullptr;
        }
        *depth_views_of[v] = nullptr;
      }
      g_r.device->DestroyDeferred(g_r.depth);
      g_r.depth = nullptr;
    }
    // kD32_FLOAT here is the backend's R32_TYPELESS + D32 DSV + R32_FLOAT
    // SRV arrangement, not a fully-typed depth format: the photo-editor
    // postfx depth pack samples this buffer through an R32_FLOAT SRV, and
    // casting an SRV over a fully-typed depth resource is undefined in
    // D3D12; in practice those reads returned the 1.0 clear
    // value for the whole frame, which saturated the ported DoF CoC
    // everywhere (a uniform smear + silhouette halos + scaffold ghosting;
    // sim-verified as the cleared-depth failure mode).
    nrhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = nrhi::Format::kD32_FLOAT;
    desc.sample_count = g_r.msaa;
    desc.usage = nrhi::kTextureUsageDepthStencil;
    desc.initial_state = nrhi::ResourceState::kDepthWrite;
    desc.clear_depth = 1.0f;
    g_r.depth = g_r.device->CreateTexture(desc);
    if (g_r.depth == nullptr) {
      g_r.failed = true;
      return false;
    }
    g_r.depth_width = width;
    g_r.depth_height = height;

    if (g_r.msaa > 1) {
      // MSAA color target + its Texture2DMS view for the fullscreen resolve
      // pass. Lives in RENDER_TARGET state between frames. Float under HDR
      // (the scene writes pre-tonemap linear).
      if (g_r.msaa_color) {
        g_r.device->DestroyDeferred(g_r.msaa_color);
        g_r.msaa_color = nullptr;
      }
      nrhi::TextureDesc cdesc = desc;
      cdesc.format = g_r.hdr_active ? g_r.hdr_scene_format
                                    : context.guest_output->format();
      cdesc.usage = nrhi::kTextureUsageRenderTarget;
      cdesc.initial_state = nrhi::ResourceState::kRenderTarget;
      cdesc.clear_color[0] = 0.25f;
      cdesc.clear_color[1] = 0.35f;
      cdesc.clear_color[2] = 0.55f;
      cdesc.clear_color[3] = 1.0f;
      g_r.msaa_color = g_r.device->CreateTexture(cdesc);
      if (g_r.msaa_color == nullptr) {
        if (g_r.hdr_active) {
          // Float MSAA target unavailable: drop to the classic path instead
          // of killing the native renderer (rebuilt next frame).
          REXLOG_WARN(
              "native-scene: HDR MSAA color target creation failed, "
              "falling back to the classic tonemap path");
          g_r.hdr_failed = true;
          g_r.depth_width = 0;
          return false;
        }
        g_r.failed = true;
        return false;
      }
      // Re-point the resolve pass's view at the recreated texture.
      if (g_r.msaa_srv_slot) {
        g_r.device->DestroyDeferred(g_r.msaa_srv_slot);
        g_r.msaa_srv_slot = nullptr;
      }
      nrhi::TextureViewDesc vd;
      vd.dimension = nrhi::ViewDimension::k2DMS;
      g_r.msaa_srv_slot = device->CreateTextureView(g_r.msaa_color, vd);
      g_r.msaa_srv_allocated = g_r.msaa_srv_slot != nullptr;
    } else {
      // MSAA switched off: retire the multisample color target and its
      // resolve view (the scene renders directly into the 1x target).
      if (g_r.msaa_srv_slot) {
        g_r.device->DestroyDeferred(g_r.msaa_srv_slot);
        g_r.msaa_srv_slot = nullptr;
        g_r.msaa_srv_allocated = false;
      }
      if (g_r.msaa_color) {
        g_r.device->DestroyDeferred(g_r.msaa_color);
        g_r.msaa_color = nullptr;
      }
    }

    // 1x float scene plane for the HDR post chain: the MSAA resolve
    // destination, or the scene target itself when MSAA is off. Idles in
    // RENDER_TARGET state; the bloom/tonemap passes sample it through
    // hdr_srv.
    if (g_r.hdr_srv != nullptr) {
      g_r.device->DestroyDeferred(g_r.hdr_srv);
      g_r.hdr_srv = nullptr;
    }
    if (g_r.hdr_resolved != nullptr) {
      g_r.device->DestroyDeferred(g_r.hdr_resolved);
      g_r.hdr_resolved = nullptr;
    }
    if (g_r.hdr_active) {
      nrhi::TextureDesc hdesc;
      hdesc.width = width;
      hdesc.height = height;
      hdesc.mip_levels = 1;
      hdesc.format = g_r.hdr_scene_format;
      hdesc.usage = nrhi::kTextureUsageRenderTarget;
      hdesc.initial_state = nrhi::ResourceState::kRenderTarget;
      g_r.hdr_resolved = g_r.device->CreateTexture(hdesc);
      if (g_r.hdr_resolved != nullptr) {
        nrhi::TextureViewDesc vd;
        vd.mip_levels = 1;
        g_r.hdr_srv = g_r.device->CreateTextureView(g_r.hdr_resolved, vd);
      }
      if (g_r.hdr_resolved == nullptr || g_r.hdr_srv == nullptr) {
        REXLOG_WARN(
            "native-scene: HDR scene plane creation failed, falling back "
            "to the classic tonemap path");
        g_r.hdr_failed = true;
        g_r.depth_width = 0;
        return false;
      }
    }
    g_r.targets_hdr = g_r.hdr_active;
    g_r.targets_scene_fmt = want_scene_fmt;
    g_r.targets_msaa = g_r.msaa;
  }
  return true;
}

bool EnsurePipeline(const NativeGuestOutputRenderContext& context) {
  if (g_r.failed) return false;
  nrhi::Device* device = context.device;
  g_r.device = device;

  if (!EnsureRootSignature(context)) {
    return false;
  }

  // HDR path decision, latched for the whole pipeline family (scene/spline
  // PSO formats + shader variants, resolve target, AO composite target,
  // output-sized targets). Hot cvar toggles rebuild everything below;
  // hdr_failed pins the classic path after a float-target failure.
  const bool hdr_want =
      REXCVAR_GET(skate3_native_render_scene_hdr) && !g_r.hdr_failed;
  const nrhi::Format hdr_fmt_want =
      REXCVAR_GET(skate3_native_render_scene_hdr_packed)
          ? nrhi::Format::kR11G11B10_FLOAT
          : nrhi::Format::kR16G16B16A16_FLOAT;
  // MSAA level: the requested count, reduced to what the scene color format
  // supports (1 disables and renders directly into the 1x target). The RHI
  // walks the requested count down in powers of two, exactly like the old
  // CheckFeatureSupport loop. Hot: a change rebuilds the pipeline family
  // (the AO/SSR/volumetric passes and the MSAA targets latch on g_r.msaa
  // and follow).
  const nrhi::Format scene_fmt_want =
      hdr_want ? hdr_fmt_want : context.guest_output->format();
  const int32_t msaa_req = REXCVAR_GET(skate3_native_render_scene_msaa);
  uint32_t msaa_want =
      msaa_req >= 8 ? 8u : msaa_req >= 4 ? 4u : msaa_req >= 2 ? 2u : 1u;
  msaa_want = device->GetSupportedSampleCount(scene_fmt_want, msaa_want);
  if (!g_r.pso || g_r.rtv_format != context.guest_output->format() ||
      g_r.hdr_active != hdr_want ||
      (hdr_want && g_r.hdr_scene_format != hdr_fmt_want) ||
      g_r.msaa != msaa_want ||
      g_r.showcase_shaders != g_r.showcase_shaders_want) {
    if (g_r.msaa != msaa_want && g_r.pfx_ready) {
      // The photo-postfx depth-pack pass is compiled against the depth
      // buffer's sample count (PFX_MSAA variant); retire the chain's PSOs
      // so the next photo-editor frame rebuilds them.
      for (nrhi::Pipeline*& p : g_r.pfx_pso) {
        if (p != nullptr) {
          device->DestroyDeferred(p);
          p = nullptr;
        }
      }
      g_r.pfx_ready = false;
    }
    g_r.hdr_active = hdr_want;
    g_r.hdr_scene_format = hdr_fmt_want;
    g_r.msaa = msaa_want;
    g_r.showcase_shaders = g_r.showcase_shaders_want;
    if (!EnsureScenePsoFamily(context) || !EnsureResolvePso(context) ||
        !EnsureBlurPsos(context) || !EnsureOutlineEdgePso(context) ||
        !Ensure2dPso(context) || !EnsureSplinePsos(context) ||
        !EnsureShadowPsos(context)) {
      if (g_r.showcase_shaders) {
        // A showcase-variant build failure must not pin the sticky failure
        // latch: drop the swap request so the F5 retry rebuilds the
        // standard shaders.
        g_r.showcase_shaders_want = false;
        g_r.showcase_shaders = false;
        REXCVAR_SET(skate3_native_render_scene_showcase, false);
      }
      return false;
    }
    REXLOG_INFO("native-scene: pipelines created (MSAA x{}, {}{})", g_r.msaa,
                g_r.hdr_active ? "HDR" : "classic",
                g_r.showcase_shaders ? ", showcase variants" : "");
    g_r.rtv_format = context.guest_output->format();
  }

  if (!EnsureHeapsAndRings(context) || !EnsureShadowResources(context)) {
    return false;
  }
  EnsureBlurOutlineTargets(context);
  if (!EnsureFallbackTextures(context) || !EnsureOutputSizedTargets(context)) {
    return false;
  }
  if (g_r.hdr_active && !EnsureHdrPipeline(context)) {
    // hdr_failed is set: the next frame rebuilds the classic path. Abort
    // this frame (the scene PSOs already target the float format).
    return false;
  }

  if (g_r.rtv_resource != context.guest_output) {
    // The presenter recreated the output image (resize). Render-target
    // binding is by texture now (the old heap-slot-0 RTV is gone); re-point
    // the cached identity and the sampled view of the output (blur source /
    // photo passes).
    if (g_r.output_srv_slot) {
      device->DestroyDeferred(g_r.output_srv_slot);
      g_r.output_srv_slot = nullptr;
    }
    nrhi::TextureViewDesc vd;
    vd.mip_levels = 1;
    g_r.output_srv_slot = device->CreateTextureView(context.guest_output, vd);
    g_r.output_srv_allocated = g_r.output_srv_slot != nullptr;
    g_r.rtv_resource = context.guest_output;
  }
  return true;
}

namespace {

// ---- Warm decode (loading-screen prewarm + gameplay warmup) --------------
// Decodes, within `deadline`, every GPU resource `item`'s draw would
// resolve: the mesh VB/IB (with fingerprint revalidation) and each texture
// slot its material binds. Mirrors the draw loop's miss paths, including
// negative-caching of failed texture decodes and retiring replaced
// resources, so the caches end up exactly as drawing would leave them and
// the takeover frame has nothing left to pay. Work past the deadline counts
// as deferred. Cached textures get ONE content revalidation per warmup
// (recheck_frame gate; the frame counter is frozen while yielded): a
// texture pre-decoded while its payload was still streaming in is healed
// here, under budget, instead of by an unbudgeted re-decode burst on the
// takeover frame.
struct WarmCounters {
  uint32_t decodes = 0;
  uint32_t deferred = 0;
};

void WarmItemResources(const NativeGuestOutputRenderContext& context, uint8_t* base,
                       uint64_t frame_number, const DrawItem& item,
                       std::chrono::steady_clock::time_point deadline,
                       WarmCounters& wc) {
  const auto within = [&] { return std::chrono::steady_clock::now() < deadline; };

  bool need_mesh = false;
  auto mit = g_r.meshes.find(item.mesh);
  if (mit == g_r.meshes.end()) {
    need_mesh = true;
  } else if (mit->second.fingerprint != item.fingerprint && !item.ropa &&
             REXCVAR_GET(skate3_native_render_scene_mesh_revalidate)) {
    // Streaming filled/replaced the payload after an earlier (pre)decode.
    // Ropa meshes are excluded: the cloth sim rewrites their payload every
    // frame BY DESIGN (fingerprint never converges), so revalidating them
    // here just destroys+redecodes the base mesh once per settle frame -
    // the churn that kept the settle pass extending itself (observed: one
    // mesh re-fingerprinting 4x in 50 ms). The draw path serves ropa
    // content from the ropa ring / shape generations regardless.
    if (within()) {
      if (g_warm_mesh_log_budget.load(std::memory_order_relaxed) > 0 &&
          g_warm_mesh_log_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
        REXLOG_DEBUG("native-scene: settle mesh REVALIDATE mesh={:08X} fp {:016X}->{:016X}",
                     item.mesh, mit->second.fingerprint, item.fingerprint);
      }
      PoolMeshBuffer(g_r.device, mit->second.vb);
      PoolMeshBuffer(g_r.device, mit->second.ib);
      g_r.meshes.erase(mit);
      need_mesh = true;
    } else {
      ++wc.deferred;
    }
  }
  if (need_mesh) {
    if (!within()) {
      ++wc.deferred;
    } else {
      ++wc.decodes;
      MeshBuffers buffers;
      if (DecodeMesh(g_r.device, base, item, buffers)) {
        buffers.fingerprint = item.fingerprint;
        buffers.last_used_frame = frame_number;
        g_r.meshes.emplace(item.mesh, buffers);
      }
      // Failures retry through the draw path (logged + counted there).
    }
  }

  const auto warm_texture = [&](uint32_t tex_ptr) {
    if (tex_ptr == 0) {
      return;
    }
    uint32_t words[6];
    if (!ReadStableTexWords(base, tex_ptr, words) || words[1] == 0) {
      return;  // unreadable / mid-rewrite / demoted: the draw path routes it
    }
    const uint64_t key = FetchWordsKey(words);
    auto it = g_r.tex_store.find(key);
    if (it != g_r.tex_store.end()) {
      if (!it->second.valid || frame_number < it->second.recheck_frame ||
          !REXCVAR_GET(skate3_native_render_scene_tex_revalidate)) {
        return;  // negative caches retry via the draw path's schedule
      }
      if (!within()) {
        ++wc.deferred;
        return;
      }
      it->second.recheck_frame = frame_number + 16;
      const uint64_t fp = SampleProbeFingerprint(base, it->second);
      if (!it->second.incomplete && (fp == 0 || fp == it->second.payload_fp)) {
        return;
      }
      if (g_warm_tex_log_budget.load(std::memory_order_relaxed) > 0 &&
          g_warm_tex_log_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
        REXLOG_DEBUG("native-scene: settle tex RETIRE ptr={:08X} key={:016X} incomplete={} "
                     "fp {:016X}->{:016X}",
                     tex_ptr, key, it->second.incomplete, it->second.payload_fp, fp);
      }
      RetireGuestTexture(it->second, context.device->CurrentSubmission());
      g_r.tex_store.erase(it);
    }
    if (!within()) {
      ++wc.deferred;
      return;
    }
    ++wc.decodes;
    GuestTexture gt;
    EnsureGuestTextureFromWords(context, base, words, gt);
    if (!gt.valid) {
      // Negative-cache exactly like the draw path so a permanently
      // unreadable payload cannot hold warmup open.
      std::memcpy(gt.fetch_words, words, sizeof(gt.fetch_words));
      gt.retry_after_frame = frame_number + 120;
    }
    gt.last_used_frame = frame_number;
    g_r.tex_store.emplace(key, gt);
  };
  // Draw-time fetch-word bindings (streamed artwork / decal ad overrides)
  // share the same store.
  const auto warm_fetch_words = [&](const uint32_t words[6]) {
    if (words[1] == 0) {
      return;
    }
    const uint64_t fkey = FetchWordsKey(words);
    if (g_r.tex_store.contains(fkey)) {
      return;
    }
    if (!within()) {
      ++wc.deferred;
      return;
    }
    ++wc.decodes;
    GuestTexture gt;
    EnsureGuestTextureFromWords(context, base, words, gt);
    gt.last_used_frame = frame_number;
    g_r.tex_store.emplace(fkey, gt);
  };

  warm_fetch_words(item.diffuse_fetch);
  warm_texture(item.diffuse_tex);
  if (REXCVAR_GET(skate3_native_render_scene_lightmaps)) {
    warm_texture(item.lightmap_tex);
  }
  if (REXCVAR_GET(skate3_native_render_scene_macro)) {
    warm_texture(item.macro_tex);
  }
  if (item.water || item.char_family >= 6 || item.dynobj != 0 ||
      (item.env_family >= 1 && item.env_family <= 6)) {
    // v2: fams 1-4 and the dynobj families sample their base normal map
    // per-pixel too.
    warm_texture(item.water_normal);
  }
  if (item.water_ocean == 1) {
    warm_texture(item.water_normal2);  // second PCA component (t8)
  }
  if ((item.env_family >= 1 && item.env_family <= 4 && item.env_family != 2) ||
      item.dynobj != 0) {
    // v2 detail normal map (t8; fam 2 carries no base+detail pair).
    warm_texture(item.detail_tex);
  }
  if (item.decal && REXCVAR_GET(skate3_native_render_scene_decals)) {
    warm_texture(item.decal_art);
    warm_fetch_words(item.decal_fetch);
  }
  if (item.char_family >= 4 && item.char_family <= 5) {
    warm_texture(item.hair_alpha_tex);
  }
  if ((item.env_family != 0 && item.env_family != 10) || item.unlit ||
      item.dynobj != 0) {
    // unlit = sky: spec_tex is the 1D sun gradient. Decal fams (3/4) and
    // the dynobj families consume their spec masks too (v2, t9).
    warm_texture(item.spec_tex);
  }
  // Environment cube (negative-cached like the draw path).
  if ((item.water || item.char_family >= 6 ||
       (item.env_family >= 5 && item.env_family <= 6) ||
       item.env_family == 13) &&
      item.water_env != 0 && !g_r.cube_textures.contains(item.water_env)) {
    if (!within()) {
      ++wc.deferred;
    } else {
      ++wc.decodes;
      GuestTexture c{};
      if (!EnsureGuestCubeTexture(context, base, item.water_env, c)) {
        if (c.upload) g_r.device->DestroyDeferred(c.upload);
        if (c.texture) g_r.device->DestroyDeferred(c.texture);
        c = GuestTexture{};
        c.valid = false;
      }
      c.last_used_frame = frame_number;
      g_r.cube_textures.emplace(item.water_env, c);
    }
  }
}

// ---- Steady-state miss routing (render thread -> decode workers) ----------
// Draw-path cache misses for STATIC content (world meshes, material
// textures) enqueue here instead of decoding inline on the render thread:
// the item skips / renders white / keeps its previous decode for the 1-3
// frames the workers need, instead of stalling the frame for the decode
// (measured ~10 ms avg, ~70 ms max per texture, the panning lag spikes).
// Dynamic payloads (skinned, cloth, ropa) still decode inline: their buffers
// change every frame, so an async result would always be stale.
void EnqueueMeshMiss(uint32_t mesh) {
  std::lock_guard<std::mutex> lock(g_prewarm_mutex);
  if (g_miss_queue.size() < 65536 && g_miss_inflight_mesh.insert(mesh).second) {
    PrewarmEntry e{mesh, 8};
    e.miss = true;
    g_miss_queue.push_back(e);
    g_prewarm_cv.notify_one();
  }
}


// Words-keyed texture miss (streamed-artwork posters / event ads): the art
// exists only as draw-time fetch words. Decoded unbudgeted inline these were
// a traversal hitch (a poster decode costs the same ~10 ms as any texture);
// while a decode is in flight the item falls back to its channel diffuse
// (the placeholder poster), not white.
void EnqueueWordsMiss(uint64_t key, const uint32_t words[6], bool ui = false) {
  std::lock_guard<std::mutex> lock(g_prewarm_mutex);
  if (g_miss_queue.size() < 65536 && g_miss_inflight_words.insert(key).second) {
    PrewarmEntry e{0, 0, 0, key};
    std::memcpy(e.words, words, sizeof(e.words));
    e.miss = true;
    e.ui = ui;
    g_miss_queue.push_back(e);
    g_prewarm_cv.notify_one();
  }
}

// Environment-cube miss: one cube decode measured up to ~100 ms inline;
// the gray fallback cube shows for the 1-3 frames the workers need instead.
void EnqueueCubeMiss(uint32_t tex) {
  std::lock_guard<std::mutex> lock(g_prewarm_mutex);
  if (g_miss_queue.size() < 65536 && g_miss_inflight_tex.insert(tex).second) {
    PrewarmEntry e{0, 0, tex};
    e.cube = true;
    e.miss = true;
    g_miss_queue.push_back(e);
    g_prewarm_cv.notify_one();
  }
}

// ---- Prewarm decode workers -----------------------------------------------
// Process one registered mesh on a worker: build the item (same walk and
// payload fingerprint as the capture, so cache entries are identical),
// decode the mesh into upload-heap buffers, and stage every material
// texture up to a filled upload resource. Results go to g_prewarm_out for
// the render thread's commit.
void ProcessPrewarmEntry(uint8_t* base, const PrewarmEntry& e) {
  if (e.mesh == 0 && e.tex == 0 && e.wkey != 0) {
    // Words-keyed texture miss (posters/ads, see EnqueueWordsMiss): stage
    // the decode from the captured fetch words.
    StagedTexResult tr;
    tr.words_key = e.wkey;
    tr.ui = e.ui;
    NativeGuestOutputRenderContext stage_ctx{};
    stage_ctx.device = g_r.device;
    g_tex_stage_out = &tr.commit;
    tr.valid = EnsureGuestTextureFromWords(stage_ctx, base, e.words, tr.gt);
    g_tex_stage_out = nullptr;
    PrewarmResult res;
    res.item.mesh = 0;
    res.mesh_valid = false;
    res.miss = e.miss;
    res.textures.push_back(std::move(tr));
    std::lock_guard<std::mutex> lock(g_prewarm_out_mutex);
    g_prewarm_out.push_back(std::move(res));
    return;
  }
  if (e.mesh == 0 && e.tex != 0) {
    // Environment-cube miss (see EnqueueCubeMiss, the only object-keyed
    // texture path left): stage the decode up to a filled upload resource;
    // the commit records the GPU copies + SRV.
    StagedTexResult tr;
    tr.key = e.tex;
    tr.cube = true;
    NativeGuestOutputRenderContext stage_ctx{};
    stage_ctx.device = g_r.device;
    g_tex_stage_out = &tr.commit;
    tr.valid = EnsureGuestCubeTexture(stage_ctx, base, e.tex, tr.gt);
    g_tex_stage_out = nullptr;
    PrewarmResult res;
    res.item.mesh = 0;  // texture-only result (DrawItem::mesh has no default)
    res.mesh_valid = false;
    res.miss = e.miss;
    res.textures.push_back(std::move(tr));
    std::lock_guard<std::mutex> lock(g_prewarm_out_mutex);
    g_prewarm_out.push_back(std::move(res));
    return;
  }
  uint8_t record[0x60];
  DrawItem item{};
  if (!GuestTryCopy(record, base + e.mesh, sizeof(record)) ||
      !BuildItemFromMesh(base, e.mesh, item)) {
    // Buffer objects can finish initializing shortly after registration;
    // retries are re-injected frame-paced by the render thread.
    bool dropped = false;
    {
      std::lock_guard<std::mutex> lock(g_prewarm_out_mutex);
      if (e.retries > 0) {
        g_prewarm_retry.push_back({e.mesh, uint16_t(e.retries - 1)});
      } else {
        g_prewarm_dropped.fetch_add(1, std::memory_order_relaxed);
        dropped = true;
      }
    }
    if (dropped) {
      // A dropped draw-path miss must leave the in-flight set so a later
      // frame can retry it (the payload may finish streaming in).
      std::lock_guard<std::mutex> lock(g_prewarm_mutex);
      g_miss_inflight_mesh.erase(e.mesh);
    }
    return;
  }
  // One representative draw entry so DecodeMesh's two-sided-sheet detection
  // keys off the real primitive type (empty draw lists would pass it
  // unconditionally). Guarded: island params live outside the validated
  // mesh record.
  const uint32_t num_islands = REX_LOAD_U32(e.mesh + 0x38);
  const uint32_t island_params = REX_LOAD_U32(e.mesh + 0x44);
  uint32_t prim0 = 0;
  if (num_islands != 0 && num_islands < 4096 &&
      GuestTryLoadU32(base, island_params, &prim0)) {
    item.draws.push_back(DrawEntry{prim0, 0, 0, item.ib_count});
  }

  PrewarmResult res;
  res.miss = e.miss;
  res.mesh_valid = DecodeMesh(g_r.device, base, item, res.buffers);
  if (res.mesh_valid) {
    res.buffers.fingerprint = item.fingerprint;
  }

  // Stage the material textures (cube maps stay on the render thread:
  // rare, and their decode has its own path). Dedupe through the shared
  // per-load set: workers cannot read the render thread's g_r caches, so a
  // texture cached from a previous map decodes once more per load; the
  // commit discards the duplicate.
  const auto stage_texture = [&](uint32_t tex_ptr) {
    if (tex_ptr == 0) {
      return;
    }
    // Stable words snapshot: the decode and its store key both come from
    // this snapshot, so a mid-rewrite object can never stage a mixed state.
    uint32_t words[6];
    if (!ReadStableTexWords(base, tex_ptr, words) || words[1] == 0) {
      return;
    }
    const uint64_t wkey = FetchWordsKey(words);
    {
      // Words-aware dedupe: the same content staged once per load; a
      // rebound object (new words) re-stages under its new key.
      std::lock_guard<std::mutex> lock(g_prewarm_mutex);
      const auto [it, fresh] = g_prewarm_tex_seen.try_emplace(tex_ptr, wkey);
      if (!fresh) {
        if (it->second == wkey) {
          return;
        }
        it->second = wkey;
      }
    }
    StagedTexResult tr;
    tr.words_key = wkey;
    // Staged mode uses only context.device (copies/barrier/SRV are
    // exported for the commit), so a device-only context suffices.
    NativeGuestOutputRenderContext stage_ctx{};
    stage_ctx.device = g_r.device;
    g_tex_stage_out = &tr.commit;
    tr.valid = EnsureGuestTextureFromWords(stage_ctx, base, words, tr.gt);
    g_tex_stage_out = nullptr;
    if (!tr.valid) {
      std::memcpy(tr.gt.fetch_words, words, sizeof(tr.gt.fetch_words));
    }
    res.textures.push_back(std::move(tr));
  };
  stage_texture(item.diffuse_tex);
  if (REXCVAR_GET(skate3_native_render_scene_lightmaps)) {
    stage_texture(item.lightmap_tex);
  }
  if (REXCVAR_GET(skate3_native_render_scene_macro)) {
    stage_texture(item.macro_tex);
  }
  if (item.water || item.char_family >= 6 || item.dynobj != 0 ||
      (item.env_family >= 1 && item.env_family <= 6)) {
    // v2: fams 1-4 and the dynobj families sample their base normal map
    // per-pixel too.
    stage_texture(item.water_normal);
  }
  if (item.water_ocean == 1) {
    stage_texture(item.water_normal2);  // second PCA component (t8)
  }
  if ((item.env_family >= 1 && item.env_family <= 4 && item.env_family != 2) ||
      item.dynobj != 0) {
    // v2 detail normal map (t8; fam 2 carries no base+detail pair).
    stage_texture(item.detail_tex);
  }
  if (item.decal && REXCVAR_GET(skate3_native_render_scene_decals)) {
    stage_texture(item.decal_art);
  }
  if (item.char_family >= 4 && item.char_family <= 5) {
    stage_texture(item.hair_alpha_tex);
  }
  if ((item.env_family != 0 && item.env_family != 10) || item.unlit ||
      item.dynobj != 0) {
    // unlit = sky: spec_tex is the 1D sun gradient. Decal fams (3/4) and
    // the dynobj families consume their spec masks too (v2, t9).
    stage_texture(item.spec_tex);
  }

  res.item = std::move(item);
  std::lock_guard<std::mutex> lock(g_prewarm_out_mutex);
  g_prewarm_out.push_back(std::move(res));
}

void PrewarmWorkerLoop() {
#if defined(_WIN32)
  // Below-normal priority: the workers flood in exactly when the guest's
  // single-threaded world ACTIVATION runs (registration is the final load
  // phase), and at normal priority they stretch the game's own black
  // window at the loading->gameplay boundary. At below-normal they only
  // soak idle cores and the guest always wins the contention.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
  for (;;) {
    if (!SceneEnabled()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      continue;
    }
    PrewarmEntry e{};
    DynDecodeJob dyn;
    bool have_dyn = false;
    {
      std::unique_lock<std::mutex> lock(g_prewarm_mutex);
      g_prewarm_cv.wait_for(lock, std::chrono::milliseconds(500), [] {
        return !g_prewarm_queue.empty() || !g_miss_queue.empty() ||
               !g_dyn_jobs.empty();
      });
      if (!g_dyn_jobs.empty()) {
        // Dynamic cloth first: these are per-frame payloads whose result
        // should land at the very next commit.
        dyn = std::move(g_dyn_jobs.front());
        g_dyn_jobs.erase(g_dyn_jobs.begin());
        have_dyn = true;
      } else if (!g_miss_queue.empty()) {
        // Draw-path misses next: this content is visible RIGHT NOW (white /
        // skipped geometry). On the old shared LIFO queue a streaming
        // registration burst kept cutting the line ahead of the visible
        // miss; medium-distance pop-in lasted the whole backlog.
        e = g_miss_queue.front();
        g_miss_queue.erase(g_miss_queue.begin());
      } else if (!g_prewarm_queue.empty()) {
        e = g_prewarm_queue.back();
        g_prewarm_queue.pop_back();
      } else {
        continue;
      }
    }
    uint8_t* base = g_guest_base.load(std::memory_order_relaxed);
    if (base == nullptr || g_r.device == nullptr) {
      if (!have_dyn) {
        std::lock_guard<std::mutex> lock(g_prewarm_out_mutex);
        g_prewarm_retry.push_back(e);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    // Decode workers only ever read guest memory on the renderer's behalf:
    // keep them permanently armed for raw-load read-fault recovery (POSIX;
    // no-op on Windows).
    ArmGuestReadRecoveryForThread(base);
    if (have_dyn) {
      PrewarmResult res;
      res.mesh_valid =
          DecodeMesh(g_r.device, base, dyn.item, res.buffers, dyn.vb.data(),
                     dyn.ib.empty() ? nullptr : dyn.ib.data());
      if (res.mesh_valid) {
        res.buffers.fingerprint = dyn.item.fingerprint;
        res.buffers.dyn_seq = dyn.seq;
        res.item = std::move(dyn.item);
        res.miss = true;  // per-frame cloth: never behind the commit cap
        std::lock_guard<std::mutex> lock(g_prewarm_out_mutex);
        g_prewarm_out.push_back(std::move(res));
      }
      continue;
    }
    ProcessPrewarmEntry(base, e);
  }
}

// Lazily start the decode workers (process-lifetime, parked on the queue's
// condition variable when idle). Only started once the pipeline exists;
// the workers create GPU resources through g_r.device (thread-safe).
void EnsurePrewarmWorkers() {
  if (g_r.device == nullptr ||
      g_prewarm_workers_started.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  const unsigned hw = std::max(4u, std::thread::hardware_concurrency());
  // Below-normal priority makes extra workers near-free (they only soak
  // idle cores; the guest always wins the contention) while the pool size
  // governs how fast a gameplay streaming burst decodes; 4 workers left
  // medium-distance pop-in on big sectors visibly behind the emulated
  // renderer.
  const unsigned n = std::clamp(hw / 3u, 2u, 8u);
  for (unsigned i = 0; i < n; ++i) {
    std::thread(PrewarmWorkerLoop).detach();
  }
  REXLOG_INFO("native-scene: {} prewarm decode workers started", n);
}

// Render-thread commit of the workers' results: record GPU copies +
// barriers, create SRVs, insert into the caches, re-inject retries.
// Microseconds per item, safe to run every frame (loading, settle and
// gameplay alike).
void PrewarmCommit(const NativeGuestOutputRenderContext& context,
                   uint64_t frame_number, bool loading = false) {
  {
    std::scoped_lock lock(g_prewarm_out_mutex, g_prewarm_mutex);
    if (!g_prewarm_retry.empty()) {
      g_prewarm_queue.insert(g_prewarm_queue.end(), g_prewarm_retry.begin(),
                             g_prewarm_retry.end());
      g_prewarm_retry.clear();
      g_prewarm_cv.notify_all();
    }
  }
  std::vector<PrewarmResult> done;
  {
    std::lock_guard<std::mutex> lock(g_prewarm_out_mutex);
    // Soft cap per frame: during the activation burst the workers can
    // complete hundreds of items between two refreshes, and committing
    // them all at once records thousands of copies + SRVs into a single
    // frame (measured ~96 us per item; a 256 batch is ~25 ms). Behind a
    // loading screen that is fine; in gameplay the cap stays small so a
    // streaming burst spreads over a few frames instead of hitching one.
    const size_t kMaxCommitPerFrame = loading ? 256 : 32;
    if (g_prewarm_out.size() <= kMaxCommitPerFrame) {
      done.swap(g_prewarm_out);
    } else {
      // Oldest first: draining from the END starved early results under a
      // sustained streaming burst (they sat behind an ever-refilling tail,
      // so exactly the content that had waited longest stayed popped-out).
      // Draw-path MISS results (content visible right now: white/skipped)
      // bypass the cap entirely: under a big sector streaming burst they
      // otherwise queued behind hundreds of speculative prewarm results
      // for several frames of visible pop-in. Only a handful arrive per
      // frame, so the bypass cannot recreate the commit hitch the cap
      // exists to prevent.
      std::vector<PrewarmResult> rest;
      rest.reserve(g_prewarm_out.size());
      size_t taken = 0;
      for (PrewarmResult& r : g_prewarm_out) {
        const bool is_miss = r.miss;
        if (is_miss || taken < kMaxCommitPerFrame) {
          if (!is_miss) {
            ++taken;
          }
          done.push_back(std::move(r));
        } else {
          rest.push_back(std::move(r));
        }
      }
      g_prewarm_out.swap(rest);
    }
  }
  if (done.empty()) {
    return;
  }
  const auto commit_t0 = PerfClock::now();
  bool committed_tex = false;
  // For the payload-stability verify below (SEH-guarded sampled reads).
  uint8_t* verify_base = g_guest_base.load(std::memory_order_relaxed);
  for (PrewarmResult& r : done) {
    if (r.mesh_valid) {
      auto mit = g_r.meshes.find(r.item.mesh);
      const bool superseded =
          mit != g_r.meshes.end() && mit->second.dyn_seq > r.buffers.dyn_seq;
      // ROPA shape-generation ring: retain this decode's vertex array
      // (keyed by dyn_seq) for the draw-time blend onto the play clock.
      // Runs even when the GPU buffers get dropped as identical below;
      // the SEQ still advances and the interp ring may reference it.
      if (r.item.ropa && r.buffers.dyn_seq != 0 && !superseded &&
          !r.buffers.ropa_verts.empty()) {
        auto& ring = g_r.ropa_shapes[r.item.mesh];
        const double now_s =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        ring.push_back(
            {r.buffers.dyn_seq, now_s, std::move(r.buffers.ropa_verts)});
        // The 8-tap boxcar kernel reaches filter_w/2 (~25 ms) past the
        // play clock (itself ~2 guest periods behind), plus decode-latency
        // slack. Generations arrive per rendered frame while the cloth sim
        // runs (the pose/shape pairing depends on that cadence), so the
        // retention must scale with the highest supported render rate:
        // 48 slots keep the kernel's ~55 ms reach covered up to ~800 fps.
        while (ring.size() > 48) {
          ring.pop_front();
        }
        if (g_r.ropa_shapes.size() > 64) {
          g_r.ropa_shapes.clear();  // outfit-change growth backstop
        }
      }
      if (mit != g_r.meshes.end() &&
          (mit->second.fingerprint == r.buffers.fingerprint || superseded)) {
        // Identical content already cached (lost the race against the draw
        // path / an earlier result), or a NEWER dynamic decode already
        // landed (multi-worker reordering must not step the cloth
        // backwards): the staged buffers were never referenced by any
        // submission.
        PoolMeshBuffer(g_r.device, r.buffers.vb);
        PoolMeshBuffer(g_r.device, r.buffers.ib);
        mit->second.last_used_frame = frame_number;
      } else {
        if (mit != g_r.meshes.end()) {
          // Stale decode (miss-driven revalidation heal): swap it out. The
          // old buffers may be referenced by the in-flight submission.
          PoolMeshBuffer(g_r.device, mit->second.vb);
          PoolMeshBuffer(g_r.device, mit->second.ib);
          g_r.meshes.erase(mit);
        }
        r.buffers.last_used_frame = frame_number;
        g_r.meshes.emplace(r.item.mesh, r.buffers);
      }
    }
    for (StagedTexResult& t : r.textures) {
      // Payload-stability verify: a decode read while its payload was still
      // STREAMING IN is a garbage interleave (the mip-churn "goes black,
      // then reloads" flash; fresh mip words repoint mid-upload, and the
      // fingerprint sampled right after the worker's read can look stable).
      // The commit runs 1-3 frames later: re-sample here and FAIL unstable
      // results, so the cache keeps the previous good decode and the retry
      // clock re-runs the heal once the payload settles. Cubes are exempt
      // (static assets; a failed cube negative-caches permanently).
      // UI-origin results skip the verify (see PrewarmEntry::ui): animating
      // APT art legitimately rewrites its payload every guest frame, so the
      // re-sample below would reject every mid-animation commit and freeze
      // the element; the 2D resolve's content probe is the heal path there.
      if (t.valid && !t.cube && !t.ui && verify_base != nullptr &&
          t.gt.payload_addr != 0 &&
          SampleProbeFingerprint(verify_base, t.gt) != t.gt.payload_fp) {
        if (t.gt.texture) {
          g_r.device->DestroyDeferred(t.gt.texture);
          t.gt.texture = nullptr;
        }
        if (t.gt.upload) {
          g_r.device->DestroyDeferred(t.gt.upload);
          t.gt.upload = nullptr;
        }
        t.valid = false;
        t.verify_failed = true;
        g_heal_verify_fail.fetch_add(1, std::memory_order_relaxed);
      }
      if (t.cube) {
        // Environment cube: lands in the cube cache. Cubes are static
        // assets: an existing valid entry wins; a failed decode
        // negative-caches like the old inline path did.
        auto cit = g_r.cube_textures.find(t.key);
        if (cit != g_r.cube_textures.end() && cit->second.valid) {
          if (t.gt.texture) g_r.device->DestroyDeferred(t.gt.texture);
          if (t.gt.upload) g_r.device->DestroyDeferred(t.gt.upload);
          continue;
        }
        if (cit != g_r.cube_textures.end()) {
          RetireGuestTexture(cit->second, context.device->CurrentSubmission());
          g_r.cube_textures.erase(cit);
        }
        if (t.valid) {
          CommitStagedGuestTexture(context, t.gt, t.commit);
          committed_tex = true;
        }
        if (t.gt.last_used_frame == 0) {
          t.gt.last_used_frame = frame_number;
        }
        g_r.cube_textures.emplace(t.key, t.gt);
        continue;
      }
      if (t.words_key != 0) {
        // Store commit: the result files under its decode-time words key
        // unconditionally; a rebound object simply routes elsewhere, so a
        // worker result can never land on the wrong identity. The only
        // remaining valid->valid swap class is an in-place content change
        // at the same words (event-ad rotation, mip-pool fills, composed
        // lightmap pages); the payload verify above covers exactly that.
        const bool tr_key =
            !g_trace_keys.empty() && g_trace_keys.count(t.words_key) != 0;
        auto wit = g_r.tex_store.find(t.words_key);
        if (tr_key) {
          REXLOG_INFO(
              "tex-trace: f{} COMMIT key={:016X} valid={} vfail={} "
              "fp={:016X} inc={} nb={} cached={}",
              frame_number, t.words_key, t.valid ? 1 : 0,
              t.verify_failed ? 1 : 0, t.gt.payload_fp,
              t.gt.incomplete ? 1 : 0, t.gt.near_black ? 1 : 0,
              wit != g_r.tex_store.end()
                  ? (wit->second.valid ? "valid" : "invalid")
                  : "none");
        }
        if (wit != g_r.tex_store.end()) {
          // Same-content dedup, except a complete re-decode always
          // displaces an incomplete cached entry (truncated tiled-mip copy:
          // the zeroed blocks live in mips the fingerprint never samples).
          const bool same_content = t.valid && wit->second.valid &&
                                    t.gt.payload_fp == wit->second.payload_fp &&
                                    !(wit->second.incomplete && !t.gt.incomplete);
          if (same_content || (!t.valid && wit->second.valid)) {
            // Keep the cached decode ("keep the old decode when the payload
            // became unreadable": mips stream out at range). A failed heal
            // of a still-serving entry needs no retry stamp: the payload
            // poll re-detects on its own cadence and the miss-inflight set
            // already dedupes.
            if (same_content && t.gt.near_black && wit->second.near_black &&
                wit->second.nb_redecodes < 255) {
              // A forced near-black re-decode came back identical: one more
              // confirmation toward "genuinely uniform content".
              ++wit->second.nb_redecodes;
            }
            if (t.gt.texture) g_r.device->DestroyDeferred(t.gt.texture);
            if (t.gt.upload) g_r.device->DestroyDeferred(t.gt.upload);
            continue;
          }
          if (t.valid && wit->second.valid) {
            // In-place content swap: the only commit class a player can
            // SEE; rolling-capped log so a flicker sighting names its
            // texture.
            static std::atomic<uint32_t> s_swap_logs{0};
            static std::atomic<int64_t> s_swap_win{0};
            const int64_t now_s =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            int64_t swin = s_swap_win.load(std::memory_order_relaxed);
            if (now_s - swin >= 5 &&
                s_swap_win.compare_exchange_strong(swin, now_s)) {
              s_swap_logs.store(0, std::memory_order_relaxed);
            }
            if (s_swap_logs.fetch_add(1) < 8) {
              REXLOG_DEBUG(
                  "native-scene: texture heal commit key={:016X} fp {:016X} "
                  "-> {:016X}",
                  t.words_key, wit->second.payload_fp, t.gt.payload_fp);
            }
          }
          if (!t.valid) {
            t.gt.fail_count = wit->second.fail_count;  // keep the backoff arc
          }
          t.gt.last_used_frame = wit->second.last_used_frame;
          RetireGuestTexture(wit->second, context.device->CurrentSubmission());
          g_r.tex_store.erase(wit);
        }
        if (t.valid) {
          CommitStagedGuestTexture(context, t.gt, t.commit);
          committed_tex = true;
          // Content landed this frame; the video-start cold/hot classifier
          // (GuestTexture::last_change_frame) keys off commit times.
          t.gt.last_change_frame = frame_number;
        } else {
          t.gt.fail_count = BumpFail(t.gt.fail_count);
          t.gt.retry_after_frame = frame_number + RetryBackoff(t.gt.fail_count);
          // Failed decodes render white: log each once (capped) so white
          // meshes stay attributable to a specific texture.
          static std::unordered_set<uint64_t> logged_failed;
          if (logged_failed.size() < 64 &&
              logged_failed.insert(t.words_key).second) {
            REXLOG_INFO(
                "native-scene: texture decode FAILED key={:016X} "
                "fetch=[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]",
                t.words_key, t.gt.fetch_words[0], t.gt.fetch_words[1],
                t.gt.fetch_words[2], t.gt.fetch_words[3], t.gt.fetch_words[4],
                t.gt.fetch_words[5]);
          }
        }
        // Fresh commits must enter the store with a live LRU stamp: an
        // unstamped (0) clock sorts as the oldest entry, and with the store
        // over cap the eviction latch would retire brand-new decodes before
        // their first draw ever stamps them - a commit/evict/re-decode loop
        // that starved dense areas of all new content.
        if (t.gt.last_used_frame == 0) {
          t.gt.last_used_frame = frame_number;
        }
        g_r.tex_store.emplace(t.words_key, t.gt);
        continue;
      }
      // No words key and not a cube: an empty/failed stage slot; release
      // whatever it carries (nothing routes to it).
      if (t.gt.texture) g_r.device->DestroyDeferred(t.gt.texture);
      if (t.gt.upload) g_r.device->DestroyDeferred(t.gt.upload);
    }
    g_prewarm_done.fetch_add(1, std::memory_order_relaxed);
  }
  // Release the miss-in-flight keys so later revalidation cycles can enqueue
  // these again (erasing keys that were never in the sets is harmless).
  {
    std::lock_guard<std::mutex> lock(g_prewarm_mutex);
    for (const PrewarmResult& r : done) {
      if (r.item.mesh != 0) {
        g_miss_inflight_mesh.erase(r.item.mesh);
      }
      for (const StagedTexResult& t : r.textures) {
        if (t.words_key != 0) {
          g_miss_inflight_words.erase(t.words_key);
        } else {
          g_miss_inflight_tex.erase(t.key);
        }
      }
    }
  }
  if (committed_tex) {
    context.cmd->FlushBarriers();
  }
  g_pw_commit.Add(uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(PerfClock::now() - commit_t0)
          .count()));
}

// Render-thread mirror of the presence-context check (set in YieldForMenus):
// menu screens shorten the 2D texture liveness recheck cadence so in-place
// UI-texture rewrites (portrait resolves) heal fast.
std::atomic<bool> g_in_menus_frame{false};

// Set by YieldForMenus, consumed by RenderScene in the same call (render
// thread only): this frame is a NATIVE loading-screen frame; no world scene
// exists (or it is the previous map's stale one), so RenderScene renders a
// black backdrop + the captured 2D loading UI instead of the world.
bool g_loading_native_frame = false;
// Render thread only: the previous rendered frame was a native loading
// frame. Used to HOLD the loading visuals through the post-load takeover
// gate window; the emulated output was suppressed all through the native
// loading screen, so yielding there would flash the stale pre-load frame.
bool g_loading_hold = false;

// Menus / pause / loading yield gate (see the comment at the call site):
// returns true when RenderScene must yield this frame to the emulated
// output. Handles the cache clears on entry and the takeover re-arm +
// loading-screen pipeline build / prewarm commit while in a load, whether
// the loading pixels themselves render emulated (yield) or natively
// (g_loading_native_frame).
bool YieldForMenus(const NativeGuestOutputRenderContext& context) {
  static bool s_in_loading = false;
  static bool s_seen_gameplay = false;
  static bool s_pause_native = false;
  const uint32_t gameplay_context =
      rex::kernel::guest_presence::GameplayContextValue();
  const bool direct_boot_loading =
      skate3::demo_path::DirectBootLoadingVisualActive();
  const bool in_menus = gameplay_context == 0 || direct_boot_loading;
  // Render-thread mirror for the 2D texture resolver: menu screens shorten
  // the content-liveness recheck cadence (see resolve_2d_texture) so
  // in-place rewrites of UI textures (the one-shot skater-portrait resolves)
  // heal within a couple of frames instead of up to 16.
  g_in_menus_frame.store(in_menus, std::memory_order_relaxed);
  if (!in_menus) {
    s_seen_gameplay = true;
  }
  // In-game pause menu: the presence context reads 0, but the world keeps
  // resubmitting perspective scenes behind the menu (loading screens and the
  // boot frontend stop publishing): stay native there so the pause backdrop
  // renders natively and the caches survive the pause. The 2D pause UI rides
  // the same captured-APT overlay replay as the gameplay HUD. If publishes
  // go stale (a load was picked from the pause menu, or the game stops
  // redrawing the world), this degrades to the yield path below within
  // ~300 ms, cache clears and all.
  // boot_native lifts the first-gameplay prerequisite from both native menu
  // modes: a boot-frontend 3D backdrop renders like a pause backdrop, and
  // everything else (videos, menus, the first load) renders as 2D-over-black.
  const bool boot_native =
      direct_boot_loading ||
      REXCVAR_GET(skate3_native_render_scene_boot_native);
  bool pause_native = false;
  if (in_menus && (s_seen_gameplay || boot_native) &&
      REXCVAR_GET(skate3_native_render_scene_pause_native)) {
    const int64_t last_ns = g_last_publish_ns.load(std::memory_order_relaxed);
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    pause_native = last_ns >= 0 && now_ns - last_ns < 300'000'000;
  }
  if (pause_native != s_pause_native) {
    s_pause_native = pause_native;
    if (pause_native) {
      REXLOG_INFO(
          "native-scene: pause menu over live world - staying NATIVE "
          "(2d stats at entry: draws_2d={} other={} dropped={})",
          g_draws_2d.load(std::memory_order_relaxed),
          g_draws_2d_other.load(std::memory_order_relaxed),
          g_draws_2d_dropped.load(std::memory_order_relaxed));
    } else {
      REXLOG_INFO(
          "native-scene: leaving native pause ({}; 2d stats at exit: "
          "draws_2d={} other={} dropped={})",
          in_menus ? "scene publishes went stale - loading/frontend"
                   : "gameplay resumed",
          g_draws_2d.load(std::memory_order_relaxed),
          g_draws_2d_other.load(std::memory_order_relaxed),
          g_draws_2d_dropped.load(std::memory_order_relaxed));
    }
  }
  // Menu-context un-suppression: the game produces some content as one-shot
  // off-screen renders; the team-menu skater portrait boxes are a
  // render-to-texture pass issued once when the screen opens (and re-issued
  // after an edit). With emulated draws suppressed those passes never
  // execute, the resolve never writes the portrait texture, and the boxes
  // stay empty forever (the F11 emulated pair-shot showed the same empty box
  // - the texture is persistent guest state that was simply never filled).
  // While a menu context is up, clear the SDK suppress cvar so the emulated
  // pipeline keeps every RTT/composite current; the extra GPU cost is
  // menu-only. The saved value is restored on the first gameplay frame, so a
  // user toggle of the underlying cvar in the debug dialog survives (it is
  // re-read at each menu entry).
  {
    static bool s_unsup_forced = false;
    static bool s_unsup_saved = false;
    const bool want =
        in_menus && REXCVAR_GET(skate3_native_render_scene_menu_unsuppress);
    if (want && !s_unsup_forced) {
      s_unsup_saved = REXCVAR_GET(native_render_suppress_emulated_draws);
      if (s_unsup_saved) {
        REXCVAR_SET(native_render_suppress_emulated_draws, false);
        REXLOG_INFO(
            "native-scene: menu context - emulated draw suppression OFF "
            "(one-shot render-to-texture passes execute; restored on "
            "gameplay)");
      }
      s_unsup_forced = true;
    } else if (!want && s_unsup_forced) {
      if (s_unsup_saved) {
        REXCVAR_SET(native_render_suppress_emulated_draws, true);
        REXLOG_INFO(
            "native-scene: leaving menu context - emulated draw suppression "
            "restored");
      }
      s_unsup_forced = false;
    }
  }
  // Menu-context suppression FILTER relaxation (the near-native sibling of
  // the full lift above): mode 0 keeps the framebuffer passes suppressed,
  // the screen stays natively composed, but lets the sub-framebuffer RTT
  // passes execute, which is where the one-shot skater-portrait renders
  // live (their pitch sits inside the mode-2 suppressed band; with mode 2
  // active in menus the boxes stayed empty).
  {
    static bool s_mode_forced = false;
    static int32_t s_mode_saved = 0;
    // menu_rtt_scope 1: only hold mode 0 inside the portrait window
    // (screen-transition grace / not-known-steady screens / CAS); outside
    // it, menus keep the gameplay-proven mode-2 suppression instead of
    // executing the game's postfx chain emulated at scale every frame
    // (the pause-menu/title-screen GPU contention).
    const bool want =
        in_menus && REXCVAR_GET(skate3_native_render_scene_menu_rtt_passes) &&
        (REXCVAR_GET(skate3_native_render_scene_menu_rtt_scope) == 0 ||
         PortraitRttWindowActive());
    if (want && !s_mode_forced) {
      s_mode_saved = REXCVAR_GET(native_render_suppress_mode);
      // Mode 3, not 0: the portrait-class RTTs (census 560-1200 during the
      // window) execute, but the 1152-wide main scene + postfx band stays
      // suppressed; mode 0 ran the game's whole pipeline at scaled
      // resolution for the window's duration, dropping the pause menu to
      // ~60 fps whenever a screen push (or its 3 s transition grace)
      // opened the window.
      if (s_mode_saved != 3) {
        REXCVAR_SET(native_render_suppress_mode, 3);
        REXLOG_INFO(
            "native-scene: portrait window - suppress mode {} -> 3 (portrait "
            "RTT passes execute, scene/postfx band stays suppressed; "
            "restored when the window closes)",
            s_mode_saved);
      }
      s_mode_forced = true;
    } else if (!want && s_mode_forced) {
      if (s_mode_saved != 3) {
        REXCVAR_SET(native_render_suppress_mode, s_mode_saved);
        REXLOG_INFO(
            "native-scene: portrait window closed - suppress mode {} restored",
            s_mode_saved);
      }
      s_mode_forced = false;
    }
    // Same menu window: shader compilation goes SYNCHRONOUS. With
    // async_shader_compilation on, the d3d12 command processor SKIPS any
    // draw whose pipeline is still compiling (command_processor.cpp
    // ConfigurePipeline tail): fine mid-gameplay, but the skater-portrait
    // boxes are ONE-SHOT renders: pieces skipped during a first-run compile
    // are baked into the portrait forever (the armless/torso-less
    // skaters; later runs are fine because the
    // shader/pipeline disk storage is warm). Menus tolerate the one-time
    // compile stalls invisibly.
    static bool s_async_forced = false;
    static bool s_async_saved = false;
    if (want && !s_async_forced) {
      s_async_saved = REXCVAR_GET(async_shader_compilation);
      if (s_async_saved) {
        REXCVAR_SET(async_shader_compilation, false);
        REXLOG_INFO(
            "native-scene: menu context - shader compilation synchronous "
            "(one-shot portrait renders can't skip still-compiling pieces)");
      }
      s_async_forced = true;
    } else if (!want && s_async_forced) {
      if (s_async_saved) {
        REXCVAR_SET(async_shader_compilation, true);
      }
      s_async_forced = false;
    }
  }
  const bool in_loading = in_menus && !pause_native;
  // Loading screens themselves render natively too (black + the captured 2D
  // loading UI) when enabled, everything after the first gameplay. The
  // housekeeping below runs for the loading STATE either way; only the
  // yield decision changes.
  const bool loading_native =
      in_loading && (s_seen_gameplay || boot_native) &&
      REXCVAR_GET(skate3_native_render_scene_loading_native);
  g_loading_native_frame = loading_native;
  if (in_loading != s_in_loading) {
    s_in_loading = in_loading;
    if (in_loading) {
      REXLOG_INFO(
          "native-scene: menus/loading - {} (presence context)",
          loading_native ? "rendering the loading screen NATIVELY"
                         : "yielding to emulated output");
      // Arena addresses are reused across map loads: let the next load's
      // registrations re-queue meshes (and re-stage textures) at reused
      // addresses, and drop the cached item cores built from them.
      ClearItemCache();
      // Off-screen retention holds guest-address-keyed copies too.
      g_retained_clear.store(true, std::memory_order_relaxed);
      // The static-caster cache holds world-positioned records from the
      // OUTGOING map; different maps share coordinate ranges, so stale
      // records whose meshes survive the transition would keep casting
      // (an entire arriving area sat under the previous map's building
      // shadows). Invalidate the sun map with it.
      g_r.static_casters.clear();
      g_r.nsm_dirty = true;
      g_r.static_sun_valid = false;
      g_r.nsm_built_radius = 0.0f;
      std::lock_guard<std::mutex> lock(g_prewarm_mutex);
      g_prewarm_seen.clear();
      g_prewarm_tex_seen.clear();
    } else {
      size_t queued = 0;
      {
        std::lock_guard<std::mutex> lock(g_prewarm_mutex);
        queued = g_prewarm_queue.size();
      }
      REXLOG_INFO(
          "native-scene: gameplay (prewarm: {} meshes decoded, {} dropped, {} still "
          "queued)",
          g_prewarm_done.load(std::memory_order_relaxed),
          g_prewarm_dropped.load(std::memory_order_relaxed), queued);
    }
  }
  if (in_loading) {
    // Re-arm the takeover gate for the next stretch of gameplay (map
    // load, unpause).
    g_warmup_armed.store(true, std::memory_order_relaxed);
    {
      // Freshness gate: only scenes published AFTER this point qualify;
      // g_scene still holds the previous map's last scene all through the
      // loading screen (BuildFrameScene stops publishing without a
      // perspective view).
      std::lock_guard<std::mutex> lock(g_scene_mutex);
      g_warmup_fresh_generation = (g_scene ? g_scene->generation : 0) + 1;
    }
    // Build the pipelines / render targets behind the loading screen so
    // the one-time PSO compilation (~200 ms) never lands on a gameplay
    // frame.
    if (!g_r.failed && g_r.pso == nullptr) {
      EnsurePipeline(context);
    }
    if (loading_native) {
      // RenderScene renders this frame (black + 2D loading UI) and runs
      // the prewarm commit itself with the loading budget.
      return false;
    }
    // THE loading-screen heavy lifting runs on the prewarm decode WORKER
    // POOL (a serial render-thread drain both tanked the loading spinner
    // to ~13 fps and still left 1500 of a map's ~2600 meshes undecoded at
    // takeover; map-change loads register most of the world in their
    // final seconds and drop the guest to 10-25 fps while doing it).
    // Here the render thread only commits finished results: record the
    // GPU copies, create SRVs, insert into the caches.
    if (REXCVAR_GET(skate3_native_render_scene_prewarm_budget_ms) > 0 &&
        !g_r.failed && g_r.pso != nullptr) {
      EnsurePrewarmWorkers();
      PrewarmCommit(context, g_frames_rendered.load(std::memory_order_relaxed),
                    /*loading=*/true);
    }
    return true;
  }
  return false;
}

  // Photo-mission photo editor (the "Pick a photo" screen with the depth of
  // field / saturation / brightness / contrast controls; FE screen class
  // PhotoSelect, challenge/photoselect.swf): the editor's effects ARE the
  // game's postfx chain, which native rendering suppresses, so natively
  // the photo showed the raw scene and the controls did nothing. Yield to
  // the emulated output while the editor is up: the emulated frame there is
  // complete and exact, and the scene is frozen so emulated-path
  // performance is fine. Unlike the menus branch above this touches no
  // caches and no takeover gates; native rendering resumes on the next
  // frame after the editor closes.
  //
  // Detection is a per-frame poll of the game's FE state (stateless, so it
  // can't get stuck): FrontEndManager singleton ptr global 0x830CFE14
  // (TU3; from SingletonHolder<FrontEndManager>::Instance = 824AD2F0),
  // whose NIS FE push-state stack (eastl::vector of 20-byte records at
  // +0x210) holds a {1, 11} record exactly while the photographer NIS has
  // the editor pushed. Surveyed across 50 gsnaps spanning gameplay, menus
  // and other missions: every non-editor record reads {x, -1}; only the
  // photo-editor capture shows a non-(-1) second field. The
  // PhotoReplayController heartbeat (sub_825623F0 hook) is kept as a
  // secondary signal for photo flows that bypass the photographer NIS.
// FMV playback (intro logos, any full-motion video): the frame is
// CPU-decoded into a texture (VideoRenderer_RwTexture Lock/Fill/Unlock) and
// reaches the screen through the game's postfx chain + swap; no capturable
// 2D draw exists (captured FMV frames show ~15 draws, all postfx
// passes + fade fills), so the native path has nothing to replay. Yield
// while the MovieDecoder::Decode heartbeat is fresh; the emulated frame is
// complete and correct there (photo-editor class). Touches no caches or
// takeover gates; native rendering resumes on the next frame after the
// movie ends.
bool YieldForMovie() {
  if (!REXCVAR_GET(skate3_native_render_scene_fmv_yield)) {
    return false;
  }
  const int64_t last_ns = g_movie_decode_last_ns.load(std::memory_order_relaxed);
  if (last_ns < 0) {
    return false;
  }
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             PerfClock::now().time_since_epoch())
                             .count();
  const bool active = now_ns - last_ns < 500'000'000;
  // LAST-RESORT gating: yielding INSTANTLY at video start presented the
  // emulated framebuffer, which under draw suppression still holds a stale
  // frame: the flash of the PREVIOUS video at every video boundary
  // (measured: yield, planes published +16 ms, substitution
  // native +38 ms, a ~5-frame stale-frame window). The substitution path
  // gets 400 ms from the first heartbeat to engage, and once it has drawn
  // recently the yield stays off entirely (covers the video-END race the
  // same way). Genuine native-FMV failure (fmv_native off, pso missing,
  // plane decode failure) still reaches the emulated yield after the
  // grace window.
  static int64_t s_fresh_since = -1;
  if (!active) {
    s_fresh_since = -1;
  } else if (s_fresh_since < 0) {
    s_fresh_since = now_ns;
  }
  const int64_t native_ns = g_movie_native_last_ns.load(std::memory_order_relaxed);
  // Yield only for a video that is ON SCREEN and NOT being served:
  // (a) quad_recent - the 2D replay recently carried a video quad (detection
  //     runs regardless of movie_sub). A skipped/force-completed video
  //     (intro skip) leaves the decoder heartbeat alive ~0.5 s with no quad
  //     anywhere; yielding then flashed the stale emulated buffer.
  // (b) staleness measured decoder-vs-native, not wall-clock: at a manual
  //     skip the quad vanishes instantly but the decoder tail keeps beating
  //     up to ~500 ms - a genuinely unsubstituted video keeps decoding long
  //     past the last native draw and crosses the 1 s bar within ~1.4 s of
  //     starting; a skip tail (<= ~600 ms) never can.
  const int64_t quad_ns = g_movie_quad_last_ns.load(std::memory_order_relaxed);
  const bool quad_recent = quad_ns >= 0 && now_ns - quad_ns < 500'000'000;
  const bool yield = active && now_ns - s_fresh_since >= 400'000'000 && quad_recent &&
                     (native_ns < 0 || last_ns - native_ns > 1'000'000'000);
  static bool s_active = false;
  if (yield != s_active) {
    s_active = yield;
    if (yield) {
      REXLOG_INFO(
          "native-scene: FMV playing - yielding to emulated output "
          "(MovieDecoder heartbeat; substitution did not engage)");
    } else {
      REXLOG_INFO("native-scene: FMV ended - native output resumes");
    }
  }
  return yield;
}

}  // namespace
// The photo-editor detection, shared by the yield and the photo-grab
// readback window: nullptr when inactive, else the name of the signal.
const char* PhotoEditorSignal(uint8_t* base) {
  const char* signal = nullptr;
  {
    constexpr uint32_t kFrontEndManagerPtr = 0x830CFE14;
    uint32_t mgr = 0, beg = 0, end = 0;
    if (GuestTryLoadU32(base, kFrontEndManagerPtr, &mgr) && mgr != 0 &&
        GuestTryLoadU32(base, mgr + 0x210, &beg) &&
        GuestTryLoadU32(base, mgr + 0x214, &end) && beg < end &&
        end - beg <= 20 * 16) {
      const uint32_t n = (end - beg) / 20;
      for (uint32_t i = 0; i < n && signal == nullptr; ++i) {
        uint32_t f0 = 0, f1 = 0;
        if (GuestTryLoadU32(base, beg + i * 20, &f0) &&
            GuestTryLoadU32(base, beg + i * 20 + 4, &f1) && f0 == 1 && f1 == 11) {
          signal = "FE PhotoSelect push-state";
        }
      }
    }
  }
  if (signal == nullptr) {
    const int64_t last_ns = g_photo_replay_last_ns.load(std::memory_order_relaxed);
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               PerfClock::now().time_since_epoch())
                               .count();
    if (last_ns >= 0 && now_ns - last_ns < 250'000'000) {
      signal = "PhotoReplayController heartbeat";
    }
  }
  return signal;
}

namespace {

// ---- Native photo grab (photo_grab_native) ---------------------------------
// The grab content is produced NATIVELY while emulated-draw suppression stays
// ON: RenderScene downsamples the finished native frame into blur_tex[0]
// (1152x640: the blur space IS the game's screenshot raster), copies it to a
// persistently-mapped READBACK buffer (double-buffered, never a GPU drain),
// and the next armed frame CPU-tiles the completed one big-endian into the
// guest screenshot target that ScreenshotBackEnd::GrabScreenshot reads.
// Layout contract (traced through sub_824FD550 -> the sub_82A7DC20 surface
// download): tiled Xenos 2D, 32bpp big-endian ARGB (guest byte order
// A,R,G,B); the game XGUntileTextureLevel's it into a linear buffer and
// hands the raw bytes to JPEG::WriteJPEG with no channel swap anywhere. The
// 160x90 gallery thumb is CPU-resized from this same image INSIDE
// GrabScreenshot (cImageOperateMan::ResizeJPEG); there is no second buffer
// to fill. Ground truth fetch words (from capture): 1152x640
// pitch 1152 k_8_8_8_8 tiled at 0x04911000 (the same surface the SDK's
// Import-Skater special case hardcodes). The write transform is
// byte-for-byte PROVEN offline against that capture's real readback
// content: tiling + [A,R,G,B] guest order reproduced all 0x2D0000 bytes
// with zero mismatches (alpha included: the emulated resolve also wrote
// 255).
constexpr uint32_t kGrabGuestAddr = 0x04911000;
// The CPU (and the app's whole texture-decode path) accesses physical
// memory through the 0xA0000000 cached-physical view; the gsnap ground
// truth lives at 0xA4911000 and plain base+0x04911000 is NOT an alias of
// those pages. All host-side reads/writes of the grab target go through
// this view.
constexpr uint32_t kGrabGuestView = 0xA0000000u | kGrabGuestAddr;
// Photo window armed AND fulfilled natively this window (RenderScene's grab
// block produces/consumes while this is set; the watchdog in
// UpdatePhotoGrabWindow clears it and flips to the emulated fallback if
// native frames stop landing).
std::atomic<bool> g_photo_grab_native_armed{false};
// Last successful guest write (PerfClock ns): the watchdog liveness signal.
std::atomic<int64_t> g_grab_last_write_ns{-1};
// The photo display-card quad was in the 2D stream this instant (PerfClock
// ns, -1 = never). Stamped by Publish2dDraws (guest thread, runs every
// guest frame regardless of yields) whenever the card-signature quad, the
// 504x640 LINEAR JPEG-decode texture, unique to the photo display card,
// is present. Drives YieldForPhotoDisplay: the display screen renders
// EMULATED, because the game composes the framed card (white border /
// caption / logo) in a ONE-SHOT pitch-1280 pass whose timing proved
// unpredictable (observed anywhere from settle+0.8s to settle+4.0s; every
// readback-window attempt missed it and cost seconds of concurrent-pipeline
// lag). Yielded-emulated, the compose and the display chain just work
// GPU-side with no readbacks, and with the native renderer not running
// concurrently the frozen scene renders at the emulated pipeline's own
// healthy rate. (g_photo_card_seen_ns itself is defined next to
// g_photo_flow_frame; Publish2dDraws lives outside this anonymous
// namespace.)
// The display yield is active / when it last ended (watchdog interplay:
// grab writes legitimately stop while yielded, and get fresh grace after).
std::atomic<bool> g_photo_display_yielding{false};
std::atomic<int64_t> g_photo_display_yield_end_ns{-1};

// Validate the game-side target before writing guest memory: GrabScreenshot
// reads its main-target dims from the postfx render manager (mgr =
// *(0x83083C5C), width/height at mgr+380/384). The GPU address has been
// 0x04911000 in every observed session, but refuse to write if the dims
// chain does not confirm the expected 1152x640 raster.
bool GrabTargetValid(uint8_t* base) {
  uint32_t mgr = 0, w = 0, h = 0;
  if (!GuestTryLoadU32(base, 0x83083C5C, &mgr) || mgr == 0 ||
      !GuestTryLoadU32(base, mgr + 380, &w) ||
      !GuestTryLoadU32(base, mgr + 384, &h)) {
    return false;
  }
  return w == RendererState::kBlurWidth && h == RendererState::kBlurHeight;
}

// CPU-tile one mapped readback image (R10G10B10A2 rows at kGrabRowPitch)
// into the guest screenshot target: assemble the tiled big-endian ARGB image
// in a host staging buffer, then land it with one SEH-guarded copy. Inverse
// of the decode path's run-copy untiler: at 32bpp a 4-texel-aligned x-run of
// 4 maps to contiguous tiled bytes (same proof as the untiler's run_blocks =
// 16 >> bpb_log2), so one GetTiledOffset2D per 4 texels. 1152x640 = 184k
// offset computations, well under a millisecond.
bool WriteGrabToGuest(uint8_t* base, const uint8_t* src) {
  constexpr uint32_t kW = RendererState::kBlurWidth;
  constexpr uint32_t kH = RendererState::kBlurHeight;
  static thread_local std::vector<uint8_t> staging;
  staging.resize(size_t(kW) * kH * 4);
  for (uint32_t y = 0; y < kH; ++y) {
    const uint32_t* row = reinterpret_cast<const uint32_t*>(
        src + size_t(y) * RendererState::kGrabRowPitch);
    for (uint32_t x = 0; x < kW; x += 4) {
      const uint32_t off = uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
          int32_t(x), int32_t(y), kW, 2));
      uint32_t* dst = reinterpret_cast<uint32_t*>(staging.data() + off);
      for (uint32_t i = 0; i < 4; ++i) {
        // R10G10B10A2 (R bits 0..9, G 10..19, B 20..29), top 8 of each ->
        // little-endian byte order A,R,G,B = the guest's big-endian ARGB.
        const uint32_t v = row[x + i];
        dst[i] = 0xFFu | ((v << 6) & 0xFF00u) | ((v << 4) & 0xFF0000u) |
                 ((v << 2) & 0xFF000000u);
      }
    }
  }
  return GuestTryCopy(base + kGrabGuestView, staging.data(), staging.size());
}

// The photo-grab window (see skate3_native_render_scene_photo_readback /
// photo_grab_native): while the photo editor is up OR a TakePhoto fired
// within the last few seconds, the game CPU-reads the resolved screenshot
// target from guest memory to JPEG-encode the photo. Two ways to fulfill it:
//  - NATIVE (photo_grab_native, default): RenderScene writes the target
//    itself from the native output; emulated-draw suppression stays ON, so
//    the emulated pipeline never renders the scene+postfx concurrently (that
//    concurrent 3x3-scale render was the editor's 40-50 fps, proven by the
//    kFast run: zero readback drains, same fps).
//  - EMULATED fallback: (a) arm the SDK's forced small-resolve CPU readback
//    so the game's own resolve lands in guest memory, and (b) lift
//    emulated-draw suppression so the passes that render the target execute.
// Runs every frame from RenderScene regardless of the yield decisions. A
// watchdog flips native -> emulated mid-window if no native grab write has
// landed recently (mode toggle, unexpected yield, readback failure).
void UpdatePhotoGrabWindow(uint8_t* base) {
  // The grab target is 1152x640x4 = 0x2D0000 bytes (the same surface the
  // SDK's Import-Skater special case reads); the thumb path is smaller.
  // Framebuffer-sized resolves (0x384000 at 1280x720) stay excluded.
  constexpr int32_t kForceReadbackMaxLength = 0x2D0000;
  static bool s_armed = false;
  static bool s_emulated_armed = false;
  static bool s_burst_armed = false;
  static int64_t s_arm_ns = 0;
  static int64_t s_last_run_ns = 0;
  static int32_t s_readback_saved = 0;
  static bool s_suppress_saved = false;
  static bool s_halfpx_saved = false;
  static int32_t s_burst_max_saved = 0;
  static bool s_burst_suppress_saved = false;
  static bool s_burst_halfpx_saved = false;
  static int64_t s_burst_open_ns = 0;
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             PerfClock::now().time_since_epoch())
                             .count();
  const auto disarm_burst = [&] {
    s_burst_armed = false;
    if (s_burst_max_saved <= 0) {
      REXCVAR_SET(native_render_force_resolve_readback_max_length,
                  s_burst_max_saved);
    }
    if (s_burst_suppress_saved) {
      REXCVAR_SET(native_render_suppress_emulated_draws, true);
    }
    if (!s_burst_halfpx_saved) {
      REXCVAR_SET(readback_resolve_half_pixel_offset, false);
    }
  };
  // This function only runs while the native renderer is producing frames
  // (RenderScene). A gap means the native path was toggled off or yielding
  // - grab writes legitimately paused, so give the watchdog a fresh grace
  // period on resume instead of instantly tripping the fallback
  // (an emulated/native comparison toggle on the display
  // screen fired the watchdog 2 ms after switching back).
  if (s_armed && s_last_run_ns != 0 && now_ns - s_last_run_ns > 300'000'000) {
    s_arm_ns = now_ns;
  }
  s_last_run_ns = now_ns;
  bool want = false;
  if (REXCVAR_GET(skate3_native_render_scene_photo_readback)) {
    want = PhotoEditorSignal(base) != nullptr;
    if (!want) {
      const int64_t last_ns = g_take_photo_last_ns.load(std::memory_order_relaxed);
      if (last_ns >= 0) {
        want = now_ns - last_ns < 3'000'000'000;
      }
    }
  }
  // Same window arms the photo postfx constant capture (CapturePfxConstants
  // runs on the guest thread at the draw-done hook).
  g_photo_flow_frame.store(want, std::memory_order_relaxed);
  const auto arm_emulated = [&] {
    s_readback_saved = REXCVAR_GET(native_render_force_resolve_readback_max_length);
    if (s_readback_saved <= 0) {
      REXCVAR_SET(native_render_force_resolve_readback_max_length,
                  kForceReadbackMaxLength);
    }
    s_suppress_saved = REXCVAR_GET(native_render_suppress_emulated_draws);
    if (s_suppress_saved) {
      REXCVAR_SET(native_render_suppress_emulated_draws, false);
    }
    // Sample host-pixel CENTERS in the scaled->1x readback extraction while
    // the window is armed (photo quality: undoes the D3D9 half-pixel shift
    // that becomes a scale/2-pixel offset at scaled resolutions).
    s_halfpx_saved = REXCVAR_GET(readback_resolve_half_pixel_offset);
    if (!s_halfpx_saved) {
      REXCVAR_SET(readback_resolve_half_pixel_offset, true);
    }
    s_emulated_armed = true;
  };
  // Shader compilation goes SYNCHRONOUS for the whole photo flow; the
  // menus lesson applied here: with async_shader_compilation on, the d3d12
  // command processor SKIPS any draw whose pipeline is still compiling, and
  // the photo display screen's framed-card compose is a ONE-SHOT pass.
  // Its shaders have been suppressed-cold in every native session (and the
  // pipeline caches were invalidated by SDK rebuilds), so the compose's
  // draws were skipped at the exact moment they finally ran, which is why
  // the card stayed un-framed EVEN IN EMULATED rendering.
  // One-time compile stalls during a photo UI moment are invisible.
  static bool s_async_forced = false;
  static bool s_async_saved = false;
  if (want && !s_async_forced) {
    s_async_saved = REXCVAR_GET(async_shader_compilation);
    if (s_async_saved) {
      REXCVAR_SET(async_shader_compilation, false);
      REXLOG_INFO(
          "native-scene: photo flow - shader compilation synchronous (the "
          "one-shot card compose can't skip still-compiling draws)");
    }
    s_async_forced = true;
  } else if (!want && s_async_forced) {
    if (s_async_saved) {
      REXCVAR_SET(async_shader_compilation, true);
    }
    s_async_forced = false;
  }
  if (want && !s_armed) {
    s_armed = true;
    s_arm_ns = now_ns;
    g_grab_last_write_ns.store(-1, std::memory_order_relaxed);
    g_r.grab_writes = 0;
    g_r.grab_cpu_us = 0;
    // Drop any readback still pending from a previous window; its content
    // is stale (pre-window) and must never land ahead of fresh frames.
    g_r.grab_pending[0] = false;
    g_r.grab_pending[1] = false;
    if (REXCVAR_GET(skate3_native_render_scene_photo_grab_native) &&
        !g_r.grab_failed) {
      g_photo_grab_native_armed.store(true, std::memory_order_relaxed);
      REXLOG_INFO(
          "native-scene: photo flow - NATIVE grab armed (suppression stays "
          "on; native output CPU-tiled into the guest screenshot target; "
          "display card yields to emulated; shutter burst on grab request)");
    } else {
      arm_emulated();
      REXLOG_INFO(
          "native-scene: photo flow - forcing small-resolve CPU readback "
          "(the photo grab reads the resolved screenshot target from guest "
          "memory){}",
          s_suppress_saved ? " + emulated draw suppression OFF" : "");
    }
  } else if (!want && s_armed) {
    s_armed = false;
    if (s_burst_armed) {
      disarm_burst();
    }
    if (g_photo_grab_native_armed.exchange(false, std::memory_order_relaxed)) {
      REXLOG_INFO(
          "native-scene: photo flow ended - native grab wrote {} frames "
          "(avg tile+write {} us)",
          g_r.grab_writes,
          g_r.grab_writes ? g_r.grab_cpu_us / g_r.grab_writes : 0);
    }
    if (s_emulated_armed) {
      s_emulated_armed = false;
      if (s_readback_saved <= 0) {
        REXCVAR_SET(native_render_force_resolve_readback_max_length,
                    s_readback_saved);
      }
      if (s_suppress_saved) {
        REXCVAR_SET(native_render_suppress_emulated_draws, true);
      }
      if (!s_halfpx_saved) {
        REXCVAR_SET(readback_resolve_half_pixel_offset, false);
      }
      REXLOG_INFO(
          "native-scene: photo flow ended - resolve readback{} restored",
          s_suppress_saved ? " + suppression" : "");
    }
  }
  // Watchdog: native mode armed but no grab write has landed within 700 ms
  // of the window opening / the last write / the display yield ending;
  // native frames stopped while the native output should be live
  // (persistent yield, readback failure). Flip to the emulated fallback so
  // the photo still works; one-way for the rest of this window. Skipped
  // while the display-card yield is active: grab writes legitimately stop
  // there (the photo was already taken and saved).
  if (s_armed && !s_emulated_armed &&
      !g_photo_display_yielding.load(std::memory_order_relaxed) &&
      g_photo_grab_native_armed.load(std::memory_order_relaxed)) {
    const int64_t last_write = g_grab_last_write_ns.load(std::memory_order_relaxed);
    const int64_t yield_end =
        g_photo_display_yield_end_ns.load(std::memory_order_relaxed);
    const int64_t basis = std::max(std::max(last_write, s_arm_ns), yield_end);
    if (now_ns - basis > 700'000'000) {
      g_photo_grab_native_armed.store(false, std::memory_order_relaxed);
      if (s_burst_armed) {
        disarm_burst();
      }
      arm_emulated();
      REXLOG_WARN(
          "native-scene: photo flow - native grab produced no frames for "
          "700 ms; falling back to the forced-readback window{}",
          s_suppress_saved ? " + suppression lifted" : "");
    }
  }
  // SHUTTER BURST: the game builds the framed card as a dedicated frame
  // sequence at the grab - request (hooked: OnPhotoGrabRequest) -> render
  // the shot + card-composite passes -> resolve -> GrabScreenshot/
  // OnScreenShot CPU-read the resolves and CPU-compose the card texture.
  // Under the native path those frames are suppressed and nothing lands in
  // CPU memory, so the compose blends zeros = the un-framed card (proven:
  // the control run's composite CPU copies all land inside this window;
  // every post-hoc timing window missed it because the CONSUMER runs
  // immediately at the shutter). For ~1.5 s from the request: suppression
  // fully lifted + kFull readbacks up to 0x2D0000 (the full composite
  // class; the handful of drains sits inside the shutter freeze the game
  // already has). The emulated 0x04911000 resolve may overwrite the
  // natively-written photo during the burst, same frozen scene, emulated
  // 3x3 quality, the pre-native-grab standard, acceptable.
  if (s_armed && !s_emulated_armed &&
      g_photo_grab_native_armed.load(std::memory_order_relaxed)) {
    const int64_t req = g_photo_grab_request_ns.load(std::memory_order_relaxed);
    // EVENT-DRIVEN close: GrabScreenshot COMPLETING (post-call stamp,
    // OnPhotoGrabDone) means every remaining card-build step, the
    // OnScreenShot compose included, is CPU-side work on memory the burst
    // already copied. The burst therefore spans exactly request ->
    // grab-done: the 1-3 shot frames, the physical minimum, all inside the
    // game's own shutter freeze, instead of a timer running into the
    // display fly-in (the user-visible lag spike). A 150 ms grace after
    // grab-done covers the aux-target grab some flows issue on the
    // following frame; the 1.5 s cap remains only as the fallback for a
    // request that never grabs.
    const int64_t done_ns = g_photo_grab_done_ns.load(std::memory_order_relaxed);
    const bool grab_done = done_ns > req && now_ns - done_ns > 150'000'000;
    const bool burst_want =
        req >= 0 && now_ns - req < 1'500'000'000 && !grab_done;
    if (burst_want && !s_burst_armed) {
      s_burst_armed = true;
      s_burst_max_saved =
          REXCVAR_GET(native_render_force_resolve_readback_max_length);
      if (s_burst_max_saved <= 0) {
        REXCVAR_SET(native_render_force_resolve_readback_max_length, 0x2D0000);
      }
      s_burst_suppress_saved = REXCVAR_GET(native_render_suppress_emulated_draws);
      if (s_burst_suppress_saved) {
        REXCVAR_SET(native_render_suppress_emulated_draws, false);
      }
      s_burst_halfpx_saved = REXCVAR_GET(readback_resolve_half_pixel_offset);
      if (!s_burst_halfpx_saved) {
        REXCVAR_SET(readback_resolve_half_pixel_offset, true);
      }
      s_burst_open_ns = now_ns;
      REXLOG_INFO(
          "native-scene: photo grab REQUEST - shutter burst OPEN "
          "(suppression lifted + <=0x2D0000 kFull readbacks; the shot + "
          "card-composite frames render and land in guest memory; closes "
          "when the grab completes)");
    } else if (!burst_want && s_burst_armed) {
      disarm_burst();
      REXLOG_INFO(
          "native-scene: shutter burst closed after {} ms ({}) - "
          "suppression + readback restored",
          (now_ns - s_burst_open_ns) / 1'000'000,
          grab_done ? "grab completed" : "1.5 s cap");
    }
  }
}

// Photo display-card screen: yield to the emulated output while the framed
// card is up (card-signature quad in the 2D stream, stamped by
// Publish2dDraws). The game composes the framed card in a ONE-SHOT
// pitch-1280 pass at an unpredictable moment after display-open
// (observed settle+0.8s to settle+4.0s); chasing it with
// suppression-lift + forced-readback windows never worked and cost
// seconds of concurrent-pipeline lag. Yielded-emulated, the compose and the
// display chain run GPU-side with no readbacks, exactly as on console, and
// with the native renderer idle the frozen scene renders at the emulated
// pipeline's own rate. A short PRE-ROLL lifts suppression while the native
// output still presents, so the emulated framebuffer has repainted (the
// card fly-in, live) before the swap, no stale-frame flash (the FMV-yield
// lesson).
bool YieldForPhotoDisplay() {
  constexpr int64_t kCardGoneNs = 250'000'000;
  constexpr int64_t kPreRollNs = 250'000'000;
  static bool s_preroll = false;
  static bool s_suppress_saved = false;
  static int64_t s_preroll_ns = 0;
  // Default OFF since the shutter burst was added: the framed card
  // is CPU-composed into the card texture at the grab, and the native 2D
  // replay samples that same texture; the display screen renders fully
  // native (card + blurred backdrop + buttons). The yield remains as a
  // safety hatch for display-screen regressions.
  if (!REXCVAR_GET(skate3_native_render_scene_photo_display_yield)) {
    if (s_preroll) {
      s_preroll = false;
      if (s_suppress_saved) {
        REXCVAR_SET(native_render_suppress_emulated_draws, true);
      }
      if (g_photo_display_yielding.exchange(false, std::memory_order_relaxed)) {
        g_photo_display_yield_end_ns.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                PerfClock::now().time_since_epoch())
                .count(),
            std::memory_order_relaxed);
      }
    }
    return false;
  }
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             PerfClock::now().time_since_epoch())
                             .count();
  const int64_t seen = g_photo_card_seen_ns.load(std::memory_order_relaxed);
  const bool card_up = g_photo_flow_frame.load(std::memory_order_relaxed) &&
                       seen >= 0 && now_ns - seen < kCardGoneNs;
  if (!card_up) {
    if (s_preroll) {
      s_preroll = false;
      if (s_suppress_saved) {
        REXCVAR_SET(native_render_suppress_emulated_draws, true);
      }
      if (g_photo_display_yielding.exchange(false, std::memory_order_relaxed)) {
        g_photo_display_yield_end_ns.store(now_ns, std::memory_order_relaxed);
        REXLOG_INFO(
            "native-scene: photo display card gone - native output resumes "
            "(suppression restored)");
      }
    }
    return false;
  }
  if (!s_preroll) {
    s_preroll = true;
    s_preroll_ns = now_ns;
    s_suppress_saved = REXCVAR_GET(native_render_suppress_emulated_draws);
    if (s_suppress_saved) {
      REXCVAR_SET(native_render_suppress_emulated_draws, false);
    }
    REXLOG_INFO(
        "native-scene: photo display card up - emulated pre-roll "
        "(suppression lifted; yielding to the emulated output in {} ms so "
        "the game's own card compose + display chain render there)",
        kPreRollNs / 1'000'000);
  }
  if (now_ns - s_preroll_ns < kPreRollNs) {
    return false;
  }
  if (!g_photo_display_yielding.exchange(true, std::memory_order_relaxed)) {
    REXLOG_INFO("native-scene: photo display - yielded to the emulated output");
  }
  return true;
}

bool YieldForPhotoEditor(uint8_t* base) {
  // The native photo-fx chain (photo_fx.hlsl, photo_native cvar) takes
  // precedence: the editor stays native and RenderScene applies the game's
  // postfx as exact ported passes with live-captured constants.
  const bool native_fx = REXCVAR_GET(skate3_native_render_scene_photo_native);
  if (!native_fx && !REXCVAR_GET(skate3_native_render_scene_photo_yield)) {
    return false;
  }
  static bool s_in_photo_editor = false;
  const char* signal = PhotoEditorSignal(base);
  const bool photo_active = signal != nullptr;
  if (photo_active != s_in_photo_editor) {
    s_in_photo_editor = photo_active;
    if (photo_active) {
      REXLOG_INFO(
          "native-scene: photo editor - {} ({})",
          native_fx ? "staying NATIVE (ported postfx chain)"
                    : "yielding to emulated output (the game's postfx applies "
                      "the photo effects)",
          signal);
    } else {
      REXLOG_INFO("native-scene: photo editor closed");
    }
  }
  if (native_fx) {
    return false;
  }
  return photo_active && REXCVAR_GET(skate3_native_render_scene_photo_yield);
}

// Create-a-skater editor (the 'Edit Skater' screen: skater + garage wall,
// Skin/Clothing/Body Mods panels): a special FE renderer the native scene
// does not model; the skater draws with editor-only CAC shader variants
// (cacstamp_skin_nisPS / cac_cloth_nisPS / cac_face_nisPS...) whose constant
// layouts differ from gameplay (the cacstamp map shifted +1 row: light c10,
// key c16, SH c25..c33 scale c22.y, tint c24, alpha c23.x, measured
// in capture), so every char-lighting capture is rejected and
// the skater rendered legacy-shaded (grey tank, pale skin, black jeans). The
// editor also runs per-frame texture-space composite passes (cac*_unwrapPS
// paint the edited garment/skin art into textures) and its own DOF postfx;
// live-edit previews are only correct with the full emulated chain. Yield
// while it is up (photo-editor class: scene is a small frozen room, emulated
// performance is fine; no cache or takeover-gate side effects; native
// resumes the frame after the editor closes, and the un-suppressed yield
// window also lets the game re-render the team-box skater portrait RTT that
// follows an accepted edit).
//
// Detection: stateless per-frame poll of the FrontEndManager push-state
// stack (same struct as YieldForPhotoEditor above) for a record with screen
// id 15, surveyed across the 40 gsnaps on hand (gameplay, pause root 56,
// team screen 63, photo editor {1,11}, FMV): id 15 appears exactly in the
// CAS editor capture and nowhere else.
bool YieldForCasEditor(uint8_t* base) {
  if (!REXCVAR_GET(skate3_native_render_scene_cas_yield)) {
    return false;
  }
  static bool s_in_cas_editor = false;
  const char* signal = nullptr;
  {
    constexpr uint32_t kFrontEndManagerPtr = 0x830CFE14;
    uint32_t mgr = 0, beg = 0, end = 0;
    if (GuestTryLoadU32(base, kFrontEndManagerPtr, &mgr) && mgr != 0 &&
        GuestTryLoadU32(base, mgr + 0x210, &beg) &&
        GuestTryLoadU32(base, mgr + 0x214, &end) && beg < end &&
        end - beg <= 20 * 16) {
      const uint32_t n = (end - beg) / 20;
      for (uint32_t i = 0; i < n && signal == nullptr; ++i) {
        uint32_t f0 = 0;
        if (GuestTryLoadU32(base, beg + i * 20, &f0) && f0 == 15) {
          signal = "FE push-state id 15";
        }
      }
    }
  }
  if (signal == nullptr) {
    // Shader heartbeat: the editor's own "_nis" pixel shaders were set
    // within the last 0.5 s (see IsCasEditorPs): covers editor entry
    // points that use a different FE screen id (the startup new-game flow
    // stayed NATIVE on the FE detection alone).
    const int64_t last_ns = g_cas_ps_last_ns.load(std::memory_order_relaxed);
    if (last_ns >= 0) {
      const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
      if (now_ns - last_ns < 500'000'000) {
        signal = "CAS _nis shader heartbeat";
      }
    }
  }
  const bool cas_active = signal != nullptr;
  if (cas_active != s_in_cas_editor) {
    s_in_cas_editor = cas_active;
    if (cas_active) {
      REXLOG_INFO(
          "native-scene: create-a-skater editor - yielding to emulated "
          "output ({}; editor CAC shading + live composite passes render "
          "exactly there)",
          signal);
    } else {
      REXLOG_INFO(
          "native-scene: create-a-skater editor closed - native output "
          "resumes");
    }
  }
  return cas_active;
}

// Services the debug-dialog texture/mesh cache flushes. (The old
// retired-buffer drain / SRV-slot recycling is gone: deferred destruction
// is backend-internal now: Device::DestroyDeferred.)
void ReleaseRetiredAndFlushCaches(const NativeGuestOutputRenderContext& context) {
  // Debug-dialog cache flushes: retire every cached decode (freed once the
  // GPU is done with the current submission) so hot-toggled decode settings
  // rebuild the world with the new rules this frame.
  if (g_flush_textures.exchange(false, std::memory_order_relaxed)) {
    const uint64_t submission = context.device->CurrentSubmission();
    for (auto& [key, t] : g_r.tex_store) {
      RetireGuestTexture(t, submission);
    }
    g_r.tex_store.clear();
    g_r.tex_routes.clear();
    g_r.words_sticky.clear();
    g_r.tex_sticky.clear();
    g_r.tex_pending_first.clear();
    REXLOG_INFO("native-scene: texture cache flushed (debug dialog)");
  }
  if (g_flush_meshes.exchange(false, std::memory_order_relaxed)) {
    for (auto& [key, m] : g_r.meshes) {
      if (m.vb) g_r.device->DestroyDeferred(m.vb);
      if (m.ib) g_r.device->DestroyDeferred(m.ib);
    }
    g_r.meshes.clear();
    {
      std::lock_guard<std::mutex> lock(g_mesh_pool_mutex);
      for (const PooledMeshBuffer& p : g_mesh_pool) {
        g_r.device->DestroyDeferred(p.buffer);
      }
      g_mesh_pool.clear();
    }
    ClearItemCache();  // decode-affecting toggles should re-walk items too
    REXLOG_INFO("native-scene: mesh cache flushed (debug dialog)");
  }
}

  // ---- Dynamic-shadow atlas pass ----
  // Renders the frame's dynamic casters (skinned characters + rigid
  // non-identity-world props: exactly the game's caster list) into the
  // three cascade tiles with the captured light rows, then applies the
  // game's coverage blur + depth dilation. Runs before the main pass so the
  // scene shader can sample the finished atlas.
// Read-only resolved-texture lookup (route -> store entry) for the caster
// passes' alpha-test binding: the main pass owns decode scheduling and
// live guest reads; an unresolved diffuse just casts opaque for the frames
// a first-sight decode takes.
static nrhi::TextureView* LookupResolvedTexture(uint32_t tex_ptr) {
  if (tex_ptr == 0) {
    return nullptr;
  }
  auto rit = g_r.tex_routes.find(tex_ptr);
  if (rit == g_r.tex_routes.end()) {
    return nullptr;
  }
  auto sit = g_r.tex_store.find(rit->second.key);
  if (sit == g_r.tex_store.end() || !sit->second.valid) {
    return nullptr;
  }
  return sit->second.srv;
}

// Build project-owned concentric CSM receiver rows. These use the same
// compact row contract as Skate's captured atlas (base X/Y/depth plus two
// scale/offset cascades), but they are driven by the owned celestial key and
// a stable camera-centered light basis. Static SKATE casters and retained
// dynamic skater/board casters can therefore share one physical sun axis.
bool BuildOwnedShadowRows(const FrameScene& scene, float out[36]) {
  if (!mechanics_sandbox::VisualMapEnabled()) {
    return false;
  }
  float origin[3] = {};
  if (!mechanics_sandbox::SandboxMapRenderOrigin(origin)) {
    return false;
  }
  const skate::world::DayNightState celestial =
      mechanics_sandbox::map::ActiveDayNightState();
  float sun[3] = {
      celestial.light_direction_to_light.x,
      celestial.light_direction_to_light.y,
      celestial.light_direction_to_light.z,
  };
  const float slen =
      std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
  if (slen < 0.5f || celestial.light_intensity <= 0.001f) {
    return false;
  }
  for (float& value : sun) {
    value /= slen;
  }

  std::memset(out, 0, 36 * sizeof(float));
  const float zl[3] = {-sun[0], -sun[1], -sun[2]};
  float xl[3] = {zl[2], 0.0f, -zl[0]};
  const float xlen = std::sqrt(xl[0] * xl[0] + xl[2] * xl[2]);
  if (xlen > 1.0e-5f) {
    xl[0] /= xlen;
    xl[2] /= xlen;
  } else {
    xl[0] = 1.0f;
    xl[2] = 0.0f;
  }
  const float yl[3] = {
      zl[1] * xl[2] - zl[2] * xl[1],
      zl[2] * xl[0] - zl[0] * xl[2],
      zl[0] * xl[1] - zl[1] * xl[0],
  };

  // The owned near cascade exists primarily for the skater and board.
  // A 30 m half-extent spent most of a 1024/2048 tile on empty world and
  // gave body self-shadows 3-6 cm texels. Fourteen metres keeps the chase
  // camera and skater comfortably covered at ~1.4 cm per texel on the
  // owned 2048 quality floor; the middle cascade catches the handoff.
  constexpr float kNearRadius = 14.0f;
  constexpr float kMiddleRadius = 55.0f;
  const float far_radius = std::clamp(
      float(REXCVAR_GET(skate3_native_render_scene_shadow_static_radius)),
      120.0f, 600.0f);
  const float depth_half = far_radius * 2.0f;
  const float tile = float(std::max(1u, g_r.shadow_tile));

  float center[3] = {scene.cam_pos[0], scene.cam_pos[1], scene.cam_pos[2]};
  // Snap in the near-cascade plane. This prevents camera sub-pixel movement
  // from making the skater/board penumbra shimmer at high output scales.
  const float texel_m = 2.0f * kNearRadius / tile;
  const float center_x =
      xl[0] * center[0] + xl[1] * center[1] + xl[2] * center[2];
  const float center_y =
      yl[0] * center[0] + yl[1] * center[1] + yl[2] * center[2];
  const float snap_x = std::round(center_x / texel_m) * texel_m - center_x;
  const float snap_y = std::round(center_y / texel_m) * texel_m - center_y;
  for (int axis = 0; axis < 3; ++axis) {
    center[axis] += xl[axis] * snap_x + yl[axis] * snap_y;
  }

  for (int axis = 0; axis < 3; ++axis) {
    out[axis] = xl[axis] / kNearRadius;
    out[12 + axis] = yl[axis] / kNearRadius;
    out[16 + axis] = zl[axis] / (2.0f * depth_half);
  }
  out[3] = -(out[0] * center[0] + out[1] * center[1] +
             out[2] * center[2]);
  out[15] = -(out[12] * center[0] + out[13] * center[1] +
              out[14] * center[2]);
  out[19] = 0.5f - (out[16] * center[0] + out[17] * center[1] +
                    out[18] * center[2]);
  out[4] = kNearRadius / kMiddleRadius;
  out[5] = kNearRadius / kMiddleRadius;
  out[8] = kNearRadius / far_radius;
  out[9] = kNearRadius / far_radius;
  out[20] = 0.00045f;
  out[24] = sun[0];
  out[25] = sun[1];
  out[26] = sun[2];
  out[32] = 0.08f;
  out[33] = 0.09f;
  out[34] = 0.11f;
  return true;
}

// Continue an owned static-world shadow rebuild in the inactive atlas.
// Six spatial chunks per frame keeps the draw submission bounded while the
// active atlas remains immutable and sampleable. This changes frame-time
// distribution, not the amount of geometry rendered per completed update.
static bool RenderOwnedStaticSunSlice(
    const NativeGuestOutputRenderContext& context, uint32_t bone_region,
    uint64_t frame_number) {
  if (!g_r.nsm_progressive_build) {
    return false;
  }
  if (g_r.static_sun_history == nullptr ||
      g_r.static_sun_history_srv == nullptr ||
      g_r.pso_shadow_caster == nullptr ||
      g_r.pso_shadow_caster_clip == nullptr) {
    g_r.nsm_progressive_build = false;
    g_r.static_sun_history_valid = false;
    return true;
  }

  const mechanics_sandbox::map::VisualWorld& world =
      mechanics_sandbox::map::ActiveVisualWorld();
  constexpr std::size_t kChunksPerFrame = 6;
  const std::size_t first =
      std::min(g_r.nsm_progressive_chunk, world.chunks.size());
  const std::size_t last =
      std::min(first + kChunksPerFrame, world.chunks.size());
  float owned_origin[3] = {};
  if (!mechanics_sandbox::SandboxMapRenderOrigin(owned_origin)) {
    g_r.nsm_progressive_build = false;
    return true;
  }

  nrhi::Cmd* cmd = context.cmd;
  cmd->SetRenderTargets(g_r.static_sun_history, nullptr);
  cmd->SetBindingLayout(g_r.layout);
  cmd->SetConstantBuffer(9, g_r.bone_ring, bone_region);
  cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
  const float size = float(g_r.static_sun_size);
  constexpr float kRatioMid = 2.0f;
  constexpr float kRatioInner = 6.0f;

  for (int tile = 0; tile < 3; ++tile) {
    const float* tvp = g_r.nsm_progressive_vp[tile];
    const float cull = tile == 0   ? 1.05f / kRatioInner
                       : tile == 1 ? 1.05f / kRatioMid
                                   : 1.05f;
    cmd->SetViewport(
        nrhi::Viewport{size * tile, 0.0f, size, size, 0.0f, 1.0f});
    cmd->SetScissor(nrhi::Rect{int32_t(g_r.static_sun_size) * tile, 0,
                               int32_t(g_r.static_sun_size) * (tile + 1),
                               int32_t(g_r.static_sun_size)});
    for (int phase = 0; phase < 2; ++phase) {
      cmd->SetPipeline(phase == 0 ? g_r.pso_shadow_caster
                                  : g_r.pso_shadow_caster_clip);
      for (std::size_t chunk_index = first; chunk_index < last;
           ++chunk_index) {
        const mechanics_sandbox::map::VisualChunk& chunk =
            world.chunks[chunk_index];
        float umin = std::numeric_limits<float>::max();
        float umax = -umin;
        float vmin = umin;
        float vmax = -umin;
        for (int corner = 0; corner < 8; ++corner) {
          const float wx =
              owned_origin[0] +
              ((corner & 1) ? chunk.bounds_max[0] : chunk.bounds_min[0]);
          const float wy =
              owned_origin[1] +
              ((corner & 2) ? chunk.bounds_max[1] : chunk.bounds_min[1]);
          const float wz =
              owned_origin[2] +
              ((corner & 4) ? chunk.bounds_max[2] : chunk.bounds_min[2]);
          const float u = g_r.nsm_progressive_rows[0] * wx +
                          g_r.nsm_progressive_rows[1] * wy +
                          g_r.nsm_progressive_rows[2] * wz +
                          g_r.nsm_progressive_rows[3];
          const float v = g_r.nsm_progressive_rows[4] * wx +
                          g_r.nsm_progressive_rows[5] * wy +
                          g_r.nsm_progressive_rows[6] * wz +
                          g_r.nsm_progressive_rows[7];
          umin = std::min(umin, u);
          umax = std::max(umax, u);
          vmin = std::min(vmin, v);
          vmax = std::max(vmax, v);
        }
        if (umax < -cull || umin > cull || vmax < -cull ||
            vmin > cull ||
            !EnsureSandboxMapChunk(context.device, chunk_index)) {
          continue;
        }
        auto mit = g_r.meshes.find(SandboxMapMeshKey(chunk_index));
        if (mit == g_r.meshes.end()) {
          continue;
        }
        mit->second.last_used_frame = frame_number;
        float constants[52] = {};
        constants[0] = constants[5] = constants[10] = constants[15] = 1.0f;
        constants[12] = owned_origin[0];
        constants[13] = owned_origin[1];
        constants[14] = owned_origin[2];
        float* mvp = constants + 16;
        for (int row = 0; row < 4; ++row) {
          for (int col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
              sum += constants[row * 4 + k] * tvp[k * 4 + col];
            }
            mvp[row * 4 + col] = sum;
          }
        }
        cmd->SetRootConstants(0, 52, constants, 0);
        cmd->SetBufferSrv(3, g_r.bone_ring, bone_region);
        cmd->SetVertexBuffer(mit->second.vb_view.buffer,
                             mit->second.vb_view.offset,
                             mit->second.vb_view.size_bytes,
                             mit->second.vb_view.stride);
        cmd->SetIndexBuffer(mit->second.ib_view.buffer,
                            mit->second.ib_view.offset,
                            mit->second.ib_view.size_bytes);
        for (const mechanics_sandbox::map::VisualDraw& draw : chunk.draws) {
          const bool mask =
              draw.alpha_mode ==
              skate::world::SurfaceMaterial::AlphaMode::Mask;
          if (draw.alpha_mode ==
                  skate::world::SurfaceMaterial::AlphaMode::Blend ||
              mask != (phase == 1)) {
            continue;
          }
          if (phase == 1) {
            cmd->SetTexture(
                1, ResolveOwnedMapTexture(context, draw.albedo_texture));
          }
          cmd->DrawIndexed(draw.index_count, draw.first_index, 0);
          ++g_r.nsm_progressive_draws;
        }
      }
    }
  }

  g_r.nsm_progressive_chunk = last;
  if (last < world.chunks.size()) {
    return true;
  }

  cmd->Barrier(g_r.static_sun_history, nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kPixelShaderResource);
  cmd->FlushBarriers();
  g_r.static_sun_history_in_srv = true;

  // The completed inactive texture becomes current. The previous complete
  // current texture becomes history, so the existing receiver transition
  // remains valid and never samples a half-built atlas.
  std::memcpy(g_r.nsm_history_rows, g_r.nsm_rows,
              sizeof(g_r.nsm_history_rows));
  std::swap(g_r.static_sun, g_r.static_sun_history);
  std::swap(g_r.static_sun_srv, g_r.static_sun_history_srv);
  std::swap(g_r.static_sun_in_srv, g_r.static_sun_history_in_srv);
  std::memcpy(g_r.nsm_rows, g_r.nsm_progressive_rows,
              sizeof(g_r.nsm_rows));
  std::memcpy(g_r.nsm_center, g_r.nsm_progressive_center,
              sizeof(g_r.nsm_center));
  std::memcpy(g_r.nsm_sun, g_r.nsm_progressive_sun,
              sizeof(g_r.nsm_sun));
  g_r.nsm_built_radius = g_r.nsm_progressive_built_radius;
  g_r.nsm_depth_range = g_r.nsm_progressive_depth_range;
  g_r.nsm_radius = g_r.nsm_progressive_radius;
  g_r.static_sun_history_valid = true;
  g_r.static_sun_valid = true;

  const auto now = std::chrono::steady_clock::now();
  if (g_r.nsm_blend_started.time_since_epoch().count() != 0) {
    const float completion_interval =
        std::chrono::duration<float>(now - g_r.nsm_blend_started).count();
    g_r.nsm_blend_seconds =
        std::clamp(completion_interval * 0.9f, 0.15f, 1.5f);
  }
  g_r.nsm_blend_started = now;
  g_r.nsm_last_build_frame = frame_number;
  g_r.nsm_progressive_build = false;
  if (g_r.nsm_progressive_sequence <= 8 ||
      (g_r.nsm_progressive_sequence & 63u) == 0) {
    REXLOG_INFO(
        "native-scene: owned sun-shadow progressive rebuild complete "
        "(chunks={} caster_draws={} build={})",
        world.chunks.size(), g_r.nsm_progressive_draws,
        g_r.nsm_progressive_sequence);
  }
  return true;
}

// Render one independently-owned static-world map per cascade. Near, middle,
// and far have separate resources, transforms, and update clocks: transitioning
// or clearing one map cannot invalidate either of the others. Spatial SKATE
// chunks are rejected against the exact light-space box before any material
// ranges are submitted.
static bool RenderOwnedStaticCascades(
    const NativeGuestOutputRenderContext& context, const FrameScene& scene,
    const float* owned_rows, uint32_t bone_region, uint64_t frame_number) {
  for (uint32_t cascade = 0; cascade < 3; ++cascade) {
    if (g_r.owned_static_sun[cascade] == nullptr ||
        g_r.owned_static_sun_srv[cascade] == nullptr) {
      return false;
    }
  }
  float owned_origin[3] = {};
  if (!mechanics_sandbox::SandboxMapRenderOrigin(owned_origin)) {
    return false;
  }
  float sun[3] = {owned_rows[24], owned_rows[25], owned_rows[26]};
  const float slen =
      std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
  if (slen < 0.5f) {
    return g_r.owned_static_sun_valid[0] &&
           g_r.owned_static_sun_valid[1] &&
           g_r.owned_static_sun_valid[2];
  }
  for (float& v : sun) {
    v /= slen;
  }
  if (sun[1] < 0.08f) {
    return g_r.owned_static_sun_valid[0] &&
           g_r.owned_static_sun_valid[1] &&
           g_r.owned_static_sun_valid[2];
  }

  const float zl[3] = {-sun[0], -sun[1], -sun[2]};
  float xl[3] = {zl[2], 0.0f, -zl[0]};
  const float xll = std::sqrt(xl[0] * xl[0] + xl[2] * xl[2]);
  if (xll > 1e-5f) {
    xl[0] /= xll;
    xl[2] /= xll;
  } else {
    xl[0] = 1.0f;
    xl[2] = 0.0f;
  }
  const float yl[3] = {zl[1] * xl[2] - zl[2] * xl[1],
                       zl[2] * xl[0] - zl[0] * xl[2],
                       zl[0] * xl[1] - zl[1] * xl[0]};
  const float far_radius = std::clamp(
      float(REXCVAR_GET(skate3_native_render_scene_shadow_static_radius)),
      20.0f, 500.0f);
  const float radii[3] = {
      std::max(14.0f, far_radius / 6.0f),
      std::max(42.0f, far_radius / 2.0f),
      far_radius,
  };
  // The recomp may submit this scene hundreds of times per second. Real-time
  // clocks keep "live" near shadows at display-rate instead of tying their
  // cost to an uncapped internal frame counter. Middle/far run at lower rates
  // where their projected movement is much smaller.
  constexpr float update_hz[3] = {60.0f, 30.0f, 15.0f};
  const float depth_range = far_radius * 4.0f;
  const float depth_half = depth_range * 0.5f;
  const mechanics_sandbox::map::VisualWorld& world =
      mechanics_sandbox::map::ActiveVisualWorld();
  nrhi::Cmd* cmd = context.cmd;

  for (uint32_t cascade = 0; cascade < 3; ++cascade) {
    const auto now = std::chrono::steady_clock::now();
    const bool due =
        !g_r.owned_static_sun_valid[cascade] ||
        g_r.owned_nsm_next_update[cascade].time_since_epoch().count() == 0 ||
        now >= g_r.owned_nsm_next_update[cascade];
    if (!due) {
      continue;
    }
    const auto submit_started = std::chrono::steady_clock::now();
    const float radius = radii[cascade];
    const float texel_m =
        2.0f * radius /
        float(std::max(1u, g_r.owned_static_sun_size[cascade]));
    float center[3] = {scene.cam_pos[0], scene.cam_pos[1], scene.cam_pos[2]};
    const float cx = xl[0] * center[0] + xl[1] * center[1] +
                     xl[2] * center[2];
    const float cy = yl[0] * center[0] + yl[1] * center[1] +
                     yl[2] * center[2];
    const float dx = cx - std::round(cx / texel_m) * texel_m;
    const float dy = cy - std::round(cy / texel_m) * texel_m;
    for (int axis = 0; axis < 3; ++axis) {
      center[axis] -= xl[axis] * dx + yl[axis] * dy;
    }
    float rows[12] = {};
    for (int axis = 0; axis < 3; ++axis) {
      rows[axis] = xl[axis] / radius;
      rows[4 + axis] = yl[axis] / radius;
      rows[8 + axis] = zl[axis] / depth_range;
    }
    rows[3] = -(rows[0] * center[0] + rows[1] * center[1] +
                rows[2] * center[2]);
    rows[7] = -(rows[4] * center[0] + rows[5] * center[1] +
                rows[6] * center[2]);
    rows[11] = 0.5f - (rows[8] * center[0] + rows[9] * center[1] +
                       rows[10] * center[2]);
    float vp[16] = {};
    for (int row = 0; row < 3; ++row) {
      vp[row * 4 + 0] = rows[row];
      vp[row * 4 + 1] = rows[4 + row];
      vp[row * 4 + 2] = rows[8 + row];
    }
    vp[12] = rows[3];
    vp[13] = rows[7];
    vp[14] = rows[11];
    vp[15] = 1.0f;
    const auto chunk_overlaps_cascade =
        [&](const mechanics_sandbox::map::VisualChunk& chunk) {
          float umin = std::numeric_limits<float>::max();
          float umax = -umin;
          float vmin = umin;
          float vmax = -umin;
          for (int corner = 0; corner < 8; ++corner) {
            const float wx =
                owned_origin[0] +
                ((corner & 1) ? chunk.bounds_max[0] : chunk.bounds_min[0]);
            const float wy =
                owned_origin[1] +
                ((corner & 2) ? chunk.bounds_max[1] : chunk.bounds_min[1]);
            const float wz =
                owned_origin[2] +
                ((corner & 4) ? chunk.bounds_max[2] : chunk.bounds_min[2]);
            const float u =
                rows[0] * wx + rows[1] * wy + rows[2] * wz + rows[3];
            const float v =
                rows[4] * wx + rows[5] * wy + rows[6] * wz + rows[7];
            umin = std::min(umin, u);
            umax = std::max(umax, u);
            vmin = std::min(vmin, v);
            vmax = std::max(vmax, v);
          }
          return umax >= -1.05f && umin <= 1.05f && vmax >= -1.05f &&
                 vmin <= 1.05f;
        };
    uint32_t overlap_count = 0;
    for (const mechanics_sandbox::map::VisualChunk& chunk : world.chunks) {
      overlap_count += chunk_overlaps_cascade(chunk) ? 1u : 0u;
    }
    // During session transitions the camera can briefly contain a valid
    // numeric position that is nowhere near the owned map. Keep the last
    // complete cascade rather than clearing it to fully lit for that frame.
    if (overlap_count == 0 && g_r.owned_static_sun_valid[cascade]) {
      continue;
    }

    if (g_r.owned_static_sun_in_srv[cascade]) {
      cmd->Barrier(g_r.owned_static_sun[cascade],
                   nrhi::ResourceState::kPixelShaderResource,
                   nrhi::ResourceState::kRenderTarget);
      cmd->FlushBarriers();
      g_r.owned_static_sun_in_srv[cascade] = false;
    }
    const float clear[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    cmd->ClearRenderTarget(g_r.owned_static_sun[cascade], clear);
    cmd->SetRenderTargets(g_r.owned_static_sun[cascade], nullptr);
    cmd->SetBindingLayout(g_r.layout);
    cmd->SetConstantBuffer(9, g_r.bone_ring, bone_region);
    cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
    const float size = float(g_r.owned_static_sun_size[cascade]);
    cmd->SetViewport(nrhi::Viewport{0.0f, 0.0f, size, size, 0.0f, 1.0f});
    cmd->SetScissor(
        nrhi::Rect{0, 0, int32_t(g_r.owned_static_sun_size[cascade]),
                   int32_t(g_r.owned_static_sun_size[cascade])});

    uint32_t caster_draws = 0;
    uint32_t visible_chunks = 0;
    for (int alpha_phase = 0; alpha_phase < 2; ++alpha_phase) {
      cmd->SetPipeline(alpha_phase == 0 ? g_r.pso_shadow_caster
                                        : g_r.pso_shadow_caster_clip);
      for (std::size_t chunk_index = 0; chunk_index < world.chunks.size();
           ++chunk_index) {
        const mechanics_sandbox::map::VisualChunk& chunk =
            world.chunks[chunk_index];
        if (!chunk_overlaps_cascade(chunk) ||
            !EnsureSandboxMapChunk(context.device, chunk_index)) {
          continue;
        }
        if (alpha_phase == 0) {
          ++visible_chunks;
        }
        auto mit = g_r.meshes.find(SandboxMapMeshKey(chunk_index));
        if (mit == g_r.meshes.end()) {
          continue;
        }
        mit->second.last_used_frame = frame_number;
        float constants[52] = {};
        constants[0] = constants[5] = constants[10] = constants[15] = 1.0f;
        constants[12] = owned_origin[0];
        constants[13] = owned_origin[1];
        constants[14] = owned_origin[2];
        float* mvp = constants + 16;
        for (int row = 0; row < 4; ++row) {
          for (int col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
              sum += constants[row * 4 + k] * vp[k * 4 + col];
            }
            mvp[row * 4 + col] = sum;
          }
        }
        cmd->SetRootConstants(0, 52, constants, 0);
        cmd->SetBufferSrv(3, g_r.bone_ring, bone_region);
        cmd->SetVertexBuffer(mit->second.vb_view.buffer,
                             mit->second.vb_view.offset,
                             mit->second.vb_view.size_bytes,
                             mit->second.vb_view.stride);
        cmd->SetIndexBuffer(mit->second.ib_view.buffer,
                            mit->second.ib_view.offset,
                            mit->second.ib_view.size_bytes);
        if (alpha_phase == 0) {
          // Static opaque casters do not sample material textures. Collapse
          // adjacent material ranges into one index submission whenever the
          // exporter laid them contiguously; the GTA scene has thousands of
          // visual materials but only tens of spatial chunks.
          uint32_t batch_first = 0;
          uint32_t batch_count = 0;
          const auto flush_batch = [&]() {
            if (batch_count == 0) {
              return;
            }
            cmd->DrawIndexed(batch_count, batch_first, 0);
            ++caster_draws;
            batch_count = 0;
          };
          for (const mechanics_sandbox::map::VisualDraw& draw : chunk.draws) {
            if (draw.alpha_mode !=
                skate::world::SurfaceMaterial::AlphaMode::Opaque) {
              flush_batch();
              continue;
            }
            if (batch_count != 0 &&
                draw.first_index == batch_first + batch_count) {
              batch_count += draw.index_count;
            } else {
              flush_batch();
              batch_first = draw.first_index;
              batch_count = draw.index_count;
            }
          }
          flush_batch();
        } else {
          for (const mechanics_sandbox::map::VisualDraw& draw : chunk.draws) {
            if (draw.alpha_mode !=
                skate::world::SurfaceMaterial::AlphaMode::Mask) {
              continue;
            }
            cmd->SetTexture(
                1, ResolveOwnedMapTexture(context, draw.albedo_texture));
            cmd->DrawIndexed(draw.index_count, draw.first_index, 0);
            ++caster_draws;
          }
        }
      }
    }
    cmd->Barrier(g_r.owned_static_sun[cascade],
                 nrhi::ResourceState::kRenderTarget,
                 nrhi::ResourceState::kPixelShaderResource);
    cmd->FlushBarriers();
    g_r.owned_static_sun_in_srv[cascade] = true;
    g_r.owned_static_sun_valid[cascade] = true;
    std::memcpy(g_r.owned_nsm_rows[cascade], rows, sizeof(rows));
    std::memcpy(g_r.owned_nsm_center[cascade], center, sizeof(center));
    std::memcpy(g_r.owned_nsm_sun[cascade], sun, sizeof(sun));
    g_r.owned_nsm_radius[cascade] = radius;
    g_r.owned_nsm_depth_range[cascade] = depth_range;
    g_r.owned_nsm_last_update[cascade] = frame_number;
    const float phase_seconds =
        g_r.owned_nsm_update_count[cascade] == 0
            ? (cascade == 0 ? 0.0f : (cascade == 1 ? 0.008f : 0.025f))
            : 0.0f;
    g_r.owned_nsm_next_update[cascade] =
        now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                  std::chrono::duration<float>(
                      1.0f / update_hz[cascade] + phase_seconds));
    g_r.owned_nsm_last_draws[cascade] = caster_draws;
    g_r.owned_nsm_last_chunks[cascade] = visible_chunks;
    const uint64_t update = ++g_r.owned_nsm_update_count[cascade];
    if (update <= 8 || (update & 255u) == 0) {
      const float submit_ms =
          std::chrono::duration<float, std::milli>(
              std::chrono::steady_clock::now() - submit_started)
              .count();
      REXLOG_INFO(
          "native-scene: owned static cascade {} update "
          "(radius={:.1f}m chunks={}/{} draws={} cpu_submit={:.2f}ms "
          "update_hz={:.0f} update={})",
          cascade, radius, visible_chunks, world.chunks.size(), caster_draws,
          submit_ms, update_hz[cascade], update);
    }
  }
  return g_r.owned_static_sun_valid[0] &&
         g_r.owned_static_sun_valid[1] &&
         g_r.owned_static_sun_valid[2];
}

// Native static sun-shadow map: one camera-centered ortho depth pass over
// the STATIC world items along the material sun (the captured c6 row:
// the direction the world materials and their baked shadows are lit
// from). The game's own cascade transforms are a stylized near-vertical
// projection fit to a ~12 m height window (measured ~43 degrees off the
// material sun): adequate for the dynamic casters they were built for,
// but tall static geometry projected through them shadows its own
// plan-view footprint. This map gives static shade a true sun axis, a
// full-range depth window (no clamp blobs) and its own resolution;
// receivers min it with the CSM term (SampleStaticSun), bottoming out at
// the configured strength. Re-rendered every frame; the box follows the
// camera, with the origin snapped to the texel grid so the static raster
// never swims sub-texel (foliage shimmer).
static void RenderStaticSunMap(const NativeGuestOutputRenderContext& context,
                               const FrameScene& scene, uint32_t bone_region,
                               int32_t debug_mode, uint64_t frame_number) {
  const bool previous_map_valid = g_r.static_sun_valid;
  g_r.static_sun_valid = false;
  float owned_rows[36] = {};
  const bool owned_shadow = BuildOwnedShadowRows(scene, owned_rows);
  static bool s_shadow_mode_initialized = false;
  static bool s_last_owned_shadow = false;
  const bool shadow_mode_changed =
      !s_shadow_mode_initialized || owned_shadow != s_last_owned_shadow;
  if (shadow_mode_changed) {
    // F5 can swap between the retained renderer and the owned world without
    // recreating GPU state. Never serve a cache rendered from the other
    // geometry set after that boundary changes.
    g_r.nsm_built_radius = 0.0f;
    g_r.nsm_dirty = true;
    g_r.nsm_progressive_build = false;
    g_r.static_sun_history_valid = false;
    s_last_owned_shadow = owned_shadow;
    s_shadow_mode_initialized = true;
  }
  if (g_r.static_sun == nullptr || g_r.pso_shadow_caster == nullptr ||
      g_r.pso_shadow_caster_clip == nullptr || debug_mode != 0 ||
      (!scene.shadow_valid && !owned_shadow) ||
      !REXCVAR_GET(skate3_native_render_scene_shadows) ||
      !REXCVAR_GET(skate3_native_render_scene_shadow_static_casters)) {
    g_r.owned_nsm_active = false;
    return;
  }
  if (owned_shadow) {
    g_r.owned_nsm_active = RenderOwnedStaticCascades(
        context, scene, owned_rows, bone_region, frame_number);
    g_r.static_sun_valid = g_r.owned_nsm_active;
    return;
  }
  g_r.owned_nsm_active = false;
  if (owned_shadow && g_r.nsm_progressive_build) {
    g_r.static_sun_valid = previous_map_valid;
    RenderOwnedStaticSunSlice(context, bone_region, frame_number);
    return;
  }
  // Sun axis = the material sun (c6). It is the only sun-like direction
  // in the captured data: the game's cascade transforms AND its baked
  // static-shade (world-shadow) projection are both the same stylized
  // near-vertical axis, measured ~43 degrees off c6, projecting statics
  // along either shadows their plan-view footprint. Shadows cast along c6
  // are geometrically consistent with the materials' lighting; where the
  // baked lightmaps were authored with yet another sun the two can
  // disagree, which the strength floor and the receivers' min-clamp keep
  // plausible rather than doubled.
  float sun[3] = {
      owned_shadow ? owned_rows[24] : scene.shadow_rows[24],
      owned_shadow ? owned_rows[25] : scene.shadow_rows[25],
      owned_shadow ? owned_rows[26] : scene.shadow_rows[26],
  };
  const float slen =
      std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
  bool sun_usable = slen >= 0.5f;
  if (sun_usable) {
    for (float& v : sun) {
      v /= slen;
    }
    sun_usable = sun[1] >= 0.08f;  // at/below the horizon = no sun term
  }
  // Single-frame capture outliers (a foreign light bank riding the row
  // capture, or a mid-transition frame) must not flap the term or the
  // built axis: dropping the whole map for one frame flashed every static
  // shadow lighter, and an unratelimited axis-delta rebuild rebuilt the map
  // along the outlier axis and back. Serve the cached map through short
  // outlier runs; a persistent change (genuine dusk, time-of-day scripts)
  // passes after a few frames.
  static uint32_t s_unusable_run = 0;
  if (!sun_usable) {
    if (g_r.nsm_built_radius > 0.0f && ++s_unusable_run <= 30) {
      g_r.static_sun_valid = true;  // hold the cached map + stored rows
      static std::atomic<uint32_t> s_hold_logs{0};
      const uint32_t n = s_hold_logs.fetch_add(1, std::memory_order_relaxed);
      if (n < 8 || (n & 255u) == 0) {
        REXLOG_INFO(
            "native-scene: static sun capture unusable (len={:.2f} y={:.2f}) - "
            "serving cached map (run={} n={})",
            slen, slen >= 0.5f ? sun[1] : 0.0f, s_unusable_run, n + 1);
      }
    }
    return;
  }
  s_unusable_run = 0;
  // The million-triangle SKATE needs a coarser static-atlas sun quantum than
  // retained retail scenery. Dynamic character/board cascades still follow
  // the celestial direction every frame; the large static atlas advances in
  // approximately two-degree steps, hidden by its soft filter, instead of
  // replaying 5k+ caster draws for every tiny clock movement.
  const float sun_rebuild_dot =
      owned_shadow ? 0.9993908f : 0.999995f;
  if (g_r.nsm_built_radius > 0.0f) {
    const float held_dot = sun[0] * g_r.nsm_sun[0] + sun[1] * g_r.nsm_sun[1] +
                           sun[2] * g_r.nsm_sun[2];
    if (held_dot < sun_rebuild_dot) {
      // The captured axis disagrees with the built one: require it to
      // persist two consecutive build frames before it can drive a
      // rebuild; until then keep working along the held axis.
      static float s_cand[3] = {};
      static uint64_t s_cand_frame = 0;
      const float cand_dot =
          sun[0] * s_cand[0] + sun[1] * s_cand[1] + sun[2] * s_cand[2];
      const bool confirmed = s_cand_frame + 1 == frame_number && cand_dot > 0.9999f;
      std::memcpy(s_cand, sun, sizeof(s_cand));
      s_cand_frame = frame_number;
      if (!confirmed) {
        static std::atomic<uint32_t> s_jump_logs{0};
        const uint32_t n = s_jump_logs.fetch_add(1, std::memory_order_relaxed);
        if (n < 8 || (n & 255u) == 0) {
          REXLOG_INFO(
              "native-scene: static sun axis jump held (dot={:.6f} n={})",
              held_dot, n + 1);
        }
        std::memcpy(sun, g_r.nsm_sun, sizeof(sun));
      }
    }
  }
  const float zl[3] = {-sun[0], -sun[1], -sun[2]};
  float xl[3] = {zl[2], 0.0f, -zl[0]};
  const float xll = std::sqrt(xl[0] * xl[0] + xl[2] * xl[2]);
  if (xll > 1e-5f) {
    xl[0] /= xll;
    xl[2] /= xll;
  } else {
    xl[0] = 1.0f;
    xl[2] = 0.0f;
  }
  const float yl[3] = {zl[1] * xl[2] - zl[2] * xl[1],
                       zl[2] * xl[0] - zl[0] * xl[2],
                       zl[0] * xl[1] - zl[1] * xl[0]};
  const float radius = std::clamp(
      float(REXCVAR_GET(skate3_native_render_scene_shadow_static_radius)),
      20.0f, 500.0f);
  // Depth window +-2r along the axis: covers cliffs/towers far above the
  // box at ~16-bit millimeter precision.
  const float depth_half = radius * 2.0f;
  const float texel_m = 2.0f * radius / float(g_r.static_sun_size);
  const float* cam = scene.cam_pos;
  // Upsert this frame's visible statics into the persistent caster cache
  // (same static filter as the world-shadow map pass). scene.items is the
  // game's view-culled list; drawing the map from it directly popped
  // shadows with the camera; the cache keeps once-seen geometry casting
  // from any direction. A new or content-changed record marks the map
  // dirty for a rebuild.
  if (!owned_shadow) {
    for (const DrawItem& item : scene.items) {
      if (item.transparent || item.unlit || item.cloth_quads || item.water ||
          item.water_ocean != 0 || item.skinned || item.ropa ||
          item.char_family != 0 || item.dynobj != 0) {
        continue;
      }
    // Key: mesh + quantized world translation (placed instances of one
    // mesh are distinct casters; a re-streamed identical placement maps
    // back onto its record).
    const auto q = [](float v) {
      return uint64_t(int64_t(std::llround(double(v) * 8.0)));
    };
    uint64_t key = uint64_t(item.mesh) * 0x9E3779B97F4A7C15ull;
    key ^= q(item.world[12]) * 0xC2B2AE3D27D4EB4Full;
    key ^= (q(item.world[13]) + 0x165667B19E3779F9ull) * 0x27D4EB2F165667C5ull;
    key ^= (q(item.world[14]) + 0x9E3779B97F4A7C15ull) * 0x85EBCA77C2B2AE63ull;
    RendererState::StaticCaster& rec = g_r.static_casters[key];
    if (rec.mesh == 0 || rec.fingerprint != item.fingerprint) {
      rec.mesh = item.mesh;
      rec.fingerprint = item.fingerprint;
      std::memcpy(rec.world, item.world, sizeof(rec.world));
      std::memcpy(rec.bbox_min, item.bbox_min, sizeof(rec.bbox_min));
      std::memcpy(rec.bbox_max, item.bbox_max, sizeof(rec.bbox_max));
      rec.diffuse_tex = item.diffuse_tex;
      rec.clip = item.env_family == 7 || item.env_family == 9 ||
                 item.env_family == 10;
      rec.draws.assign(item.draws.begin(), item.draws.end());
      g_r.nsm_dirty = true;
    }
      rec.last_seen_frame = frame_number;
    }
  }
  // Cache maintenance: distance eviction (region streamed away) plus a
  // long staleness timeout, which bounds ghost shadows from content the
  // game actually removed (deleted park-editor objects) at the cost of a
  // late pop for casters never re-viewed.
  if (frame_number >= g_r.static_casters_sweep_frame) {
    g_r.static_casters_sweep_frame = frame_number + 600;
    const float evict_r = radius * 6.0f;
    for (auto it = g_r.static_casters.begin();
         it != g_r.static_casters.end();) {
      const RendererState::StaticCaster& rec = it->second;
      const float ex = rec.world[12] - cam[0];
      const float ey = rec.world[13] - cam[1];
      const float ez = rec.world[14] - cam[2];
      const bool far_away = ex * ex + ey * ey + ez * ez > evict_r * evict_r;
      const bool stale = frame_number > rec.last_seen_frame + 30000;
      if (far_away || stale) {
        it = g_r.static_casters.erase(it);
        g_r.nsm_dirty = true;
      } else {
        ++it;
      }
    }
  }
  // Cross-frame cache: the map re-renders only when something it depends
  // on changed. Between rebuilds the stored rows and the map contents are
  // served as-is; statics and the sun are near-constant, so this removes
  // almost the entire steady-state caster cost (and freezes the raster,
  // so foliage dapple cannot shimmer frame to frame).
  // Dirty (content changed) rebuilds are RATE-LIMITED: dense areas stream
  // new casters near-continuously, and an unthrottled dirty flag degraded
  // to a full map redraw every frame. A <=30-frame shadow latency for
  // newly streamed geometry is invisible; drift/sun rebuilds stay
  // immediate.
  bool rebuild = g_r.nsm_built_radius <= 0.0f ||
                  std::fabs(radius - g_r.nsm_built_radius) > 0.5f ||
                  frame_number >= g_r.nsm_rebuild_frame ||
                  (g_r.nsm_dirty &&
                  frame_number >= g_r.nsm_last_build_frame + 30);
  if (!rebuild) {
    // Camera drift from the built center, measured in the map plane: the
    // INNER tile (radius/6) must keep covering the player's
    // surroundings, so the threshold rides its extent.
    const float dcx = cam[0] - g_r.nsm_center[0];
    const float dcy = cam[1] - g_r.nsm_center[1];
    const float dcz = cam[2] - g_r.nsm_center[2];
    const float du = xl[0] * dcx + xl[1] * dcy + xl[2] * dcz;
    const float dv = yl[0] * dcx + yl[1] * dcy + yl[2] * dcz;
    rebuild = std::max(std::fabs(du), std::fabs(dv)) > radius * 0.0667f;
  }
  if (!rebuild) {
    rebuild = sun[0] * g_r.nsm_sun[0] + sun[1] * g_r.nsm_sun[1] +
                  sun[2] * g_r.nsm_sun[2] <
              sun_rebuild_dot;
  }
  if (!rebuild) {
    g_r.static_sun_valid = true;  // serve the cached map + stored rows
    return;
  }
  // Preserve the last complete atlas and its transform, then render into
  // the alternate texture. Receivers cross-fade the two independent
  // projections over the interval between sun quanta, turning the former
  // two-degree shadow jump into continuous motion without submitting the
  // million-triangle world on every frame.
  // The inactive atlas cannot be incrementally rendered while it remains in
  // the scene texture table: D3D12 still validates/binds the SRV even when a
  // shader branch would not sample it. That prototype caused whole-map
  // flashes and is intentionally disabled pending independent per-cascade
  // resources.
  const bool progressive_build = false;
  float displayed_rows[12] = {};
  float displayed_center[3] = {};
  float displayed_sun[3] = {};
  float displayed_built_radius = g_r.nsm_built_radius;
  float displayed_depth_range = g_r.nsm_depth_range;
  float displayed_radius = g_r.nsm_radius;
  if (progressive_build) {
    std::memcpy(displayed_rows, g_r.nsm_rows, sizeof(displayed_rows));
    std::memcpy(displayed_center, g_r.nsm_center,
                sizeof(displayed_center));
    std::memcpy(displayed_sun, g_r.nsm_sun, sizeof(displayed_sun));
  }
  const bool temporal_blend =
      !progressive_build && owned_shadow && previous_map_valid &&
      !shadow_mode_changed && g_r.static_sun_history != nullptr &&
      g_r.static_sun_history_srv != nullptr;
  if (temporal_blend) {
    std::swap(g_r.static_sun, g_r.static_sun_history);
    std::swap(g_r.static_sun_srv, g_r.static_sun_history_srv);
    std::swap(g_r.static_sun_in_srv, g_r.static_sun_history_in_srv);
    std::memcpy(g_r.nsm_history_rows, g_r.nsm_rows,
                sizeof(g_r.nsm_history_rows));
    g_r.static_sun_history_valid = true;
    const auto now = std::chrono::steady_clock::now();
    if (g_r.nsm_blend_started.time_since_epoch().count() != 0) {
      const float rebuild_interval =
          std::chrono::duration<float>(now - g_r.nsm_blend_started).count();
      // Finish just before the next expected sun-quantum rebuild. This
      // keeps motion continuous while ensuring the next history swap starts
      // from a fully converged atlas.
      g_r.nsm_blend_seconds =
          std::clamp(rebuild_interval * 0.9f, 0.15f, 1.5f);
    }
    g_r.nsm_blend_started = now;
  } else if (!progressive_build) {
    g_r.static_sun_history_valid = false;
  } else {
    // The inactive atlas is about to become a render target and must not be
    // sampled while only part of the map has reached it.
    g_r.static_sun_history_valid = false;
  }
  g_r.nsm_dirty = false;
  // Retained geometry is stream-fed, so it keeps a periodic refresh.
  // SKATE geometry is immutable for the process lifetime: camera drift,
  // radius changes, F5 mode changes, and sun-axis changes already invalidate
  // it explicitly. A 600-frame timer on a fast GPU rebuilt thousands of
  // owned material ranges every ~2 seconds for no visual benefit.
  g_r.nsm_rebuild_frame =
      owned_shadow ? std::numeric_limits<uint64_t>::max()
                   : frame_number + 600;
  g_r.nsm_last_build_frame = frame_number;
  g_r.nsm_built_radius = radius;
  std::memcpy(g_r.nsm_sun, sun, sizeof(g_r.nsm_sun));
  float o[3] = {cam[0], cam[1], cam[2]};
  const float cx = xl[0] * o[0] + xl[1] * o[1] + xl[2] * o[2];
  const float cy = yl[0] * o[0] + yl[1] * o[1] + yl[2] * o[2];
  const float dx = cx - std::round(cx / texel_m) * texel_m;
  const float dy = cy - std::round(cy / texel_m) * texel_m;
  for (int a = 0; a < 3; ++a) {
    o[a] -= xl[a] * dx + yl[a] * dy;
  }
  std::memcpy(g_r.nsm_center, o, sizeof(g_r.nsm_center));
  // world -> map rows, shared by this caster pass and the receivers
  // (b1 nsm_x/y/z): uc = dot(row.xyz, wp) + row.w.
  float rows[12];
  for (int a = 0; a < 3; ++a) {
    rows[0 + a] = xl[a] / radius;
    rows[4 + a] = yl[a] / radius;
    rows[8 + a] = zl[a] / (2.0f * depth_half);
  }
  rows[3] = -(rows[0] * o[0] + rows[1] * o[1] + rows[2] * o[2]);
  rows[7] = -(rows[4] * o[0] + rows[5] * o[1] + rows[6] * o[2]);
  rows[11] = 0.5f - (rows[8] * o[0] + rows[9] * o[1] + rows[10] * o[2]);
  std::memcpy(g_r.nsm_rows, rows, sizeof(rows));
  g_r.nsm_depth_range = 2.0f * depth_half;
  g_r.nsm_radius = radius;
  // View-proj columns from the rows (same construction as the CSM
  // lightvp; clip.y lands in the sampler's v = -y/2 + 0.5 convention via
  // the raster y-flip).
  float vp[16] = {};
  for (int r = 0; r < 3; ++r) {
    vp[r * 4 + 0] = rows[0 + r];
    vp[r * 4 + 1] = rows[4 + r];
    vp[r * 4 + 2] = rows[8 + r];
  }
  vp[12] = rows[3];
  vp[13] = rows[7];
  vp[14] = rows[11];
  vp[15] = 1.0f;
  // Finer-tile view-projections: same origin, radius/2 and radius/6,
  // exact rescales of the far transform's X/Y columns (the shader selects
  // tiles by the same ratios; keep in sync with its
  // kRatioInner/kRatioMid). Snapping used the FAR texel grid, whose
  // multiples are also finer-texel multiples (integer 1:2:6 chain), so
  // every tile stays swim-free.
  constexpr float kRatioMid = 2.0f;
  constexpr float kRatioInner = 6.0f;
  float vp_mid[16];
  float vp_inner[16];
  std::memcpy(vp_mid, vp, sizeof(vp));
  std::memcpy(vp_inner, vp, sizeof(vp));
  for (int r = 0; r < 4; ++r) {
    vp_mid[r * 4 + 0] *= kRatioMid;
    vp_mid[r * 4 + 1] *= kRatioMid;
    vp_inner[r * 4 + 0] *= kRatioInner;
    vp_inner[r * 4 + 1] *= kRatioInner;
  }
  if (progressive_build) {
    std::memcpy(g_r.nsm_progressive_rows, g_r.nsm_rows,
                sizeof(g_r.nsm_progressive_rows));
    std::memcpy(g_r.nsm_progressive_vp[0], vp_inner, sizeof(vp_inner));
    std::memcpy(g_r.nsm_progressive_vp[1], vp_mid, sizeof(vp_mid));
    std::memcpy(g_r.nsm_progressive_vp[2], vp, sizeof(vp));
    std::memcpy(g_r.nsm_progressive_center, g_r.nsm_center,
                sizeof(g_r.nsm_progressive_center));
    std::memcpy(g_r.nsm_progressive_sun, g_r.nsm_sun,
                sizeof(g_r.nsm_progressive_sun));
    g_r.nsm_progressive_built_radius = g_r.nsm_built_radius;
    g_r.nsm_progressive_depth_range = g_r.nsm_depth_range;
    g_r.nsm_progressive_radius = g_r.nsm_radius;

    // Keep every receiver parameter pointed at the complete displayed map
    // until the background target is fully rendered.
    std::memcpy(g_r.nsm_rows, displayed_rows, sizeof(g_r.nsm_rows));
    std::memcpy(g_r.nsm_center, displayed_center, sizeof(g_r.nsm_center));
    std::memcpy(g_r.nsm_sun, displayed_sun, sizeof(g_r.nsm_sun));
    g_r.nsm_built_radius = displayed_built_radius;
    g_r.nsm_depth_range = displayed_depth_range;
    g_r.nsm_radius = displayed_radius;

    nrhi::Cmd* progressive_cmd = context.cmd;
    if (g_r.static_sun_history_in_srv) {
      progressive_cmd->Barrier(
          g_r.static_sun_history,
          nrhi::ResourceState::kPixelShaderResource,
          nrhi::ResourceState::kRenderTarget);
      progressive_cmd->FlushBarriers();
      g_r.static_sun_history_in_srv = false;
    }
    const float clear[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    progressive_cmd->ClearRenderTarget(g_r.static_sun_history, clear);
    g_r.nsm_progressive_build = true;
    g_r.nsm_progressive_chunk = 0;
    g_r.nsm_progressive_draws = 0;
    ++g_r.nsm_progressive_sequence;
    if (g_r.nsm_progressive_sequence <= 8 ||
        (g_r.nsm_progressive_sequence & 63u) == 0) {
      REXLOG_INFO(
          "native-scene: owned sun-shadow progressive rebuild started "
          "(chunks={} batch=6 build={})",
          mechanics_sandbox::map::ActiveVisualWorld().chunks.size(),
          g_r.nsm_progressive_sequence);
    }
    RenderOwnedStaticSunSlice(context, bone_region, frame_number);
    return;
  }
  nrhi::Cmd* cmd = context.cmd;
  if (g_r.static_sun_in_srv) {
    cmd->Barrier(g_r.static_sun, nrhi::ResourceState::kPixelShaderResource,
                 nrhi::ResourceState::kRenderTarget);
    cmd->FlushBarriers();
    g_r.static_sun_in_srv = false;
  }
  const float clear[4] = {1.0f, 1.0f, 0.0f, 0.0f};
  cmd->ClearRenderTarget(g_r.static_sun, clear);
  cmd->SetRenderTargets(g_r.static_sun, nullptr);
  cmd->SetBindingLayout(g_r.layout);
  cmd->SetConstantBuffer(9, g_r.bone_ring, bone_region);
  const float size = float(g_r.static_sun_size);
  uint32_t owned_caster_draws = 0;
  const auto draw_owned_casters =
      [&](const float* tvp, float cull, int phase) {
        float owned_origin[3] = {};
        if (!mechanics_sandbox::SandboxMapRenderOrigin(owned_origin)) {
          return;
        }
        const mechanics_sandbox::map::VisualWorld& owned_world =
            mechanics_sandbox::map::ActiveVisualWorld();
        for (std::size_t chunk_index = 0;
             chunk_index < owned_world.chunks.size(); ++chunk_index) {
          const mechanics_sandbox::map::VisualChunk& chunk =
              owned_world.chunks[chunk_index];
          float umin = std::numeric_limits<float>::max();
          float umax = -umin;
          float vmin = umin;
          float vmax = -umin;
          for (int corner = 0; corner < 8; ++corner) {
            const float wx =
                owned_origin[0] +
                ((corner & 1) ? chunk.bounds_max[0] : chunk.bounds_min[0]);
            const float wy =
                owned_origin[1] +
                ((corner & 2) ? chunk.bounds_max[1] : chunk.bounds_min[1]);
            const float wz =
                owned_origin[2] +
                ((corner & 4) ? chunk.bounds_max[2] : chunk.bounds_min[2]);
            const float u =
                rows[0] * wx + rows[1] * wy + rows[2] * wz + rows[3];
            const float v =
                rows[4] * wx + rows[5] * wy + rows[6] * wz + rows[7];
            umin = std::min(umin, u);
            umax = std::max(umax, u);
            vmin = std::min(vmin, v);
            vmax = std::max(vmax, v);
          }
          if (umax < -cull || umin > cull || vmax < -cull ||
              vmin > cull ||
              !EnsureSandboxMapChunk(context.device, chunk_index)) {
            continue;
          }
          auto mit = g_r.meshes.find(SandboxMapMeshKey(chunk_index));
          if (mit == g_r.meshes.end()) {
            continue;
          }
          mit->second.last_used_frame = frame_number;
          float constants[52] = {};
          constants[0] = constants[5] = constants[10] = constants[15] = 1.0f;
          constants[12] = owned_origin[0];
          constants[13] = owned_origin[1];
          constants[14] = owned_origin[2];
          float* mvp = constants + 16;
          for (int r = 0; r < 4; ++r) {
            for (int col = 0; col < 4; ++col) {
              float sum = 0.0f;
              for (int k = 0; k < 4; ++k) {
                sum += constants[r * 4 + k] * tvp[k * 4 + col];
              }
              mvp[r * 4 + col] = sum;
            }
          }
          cmd->SetRootConstants(0, 52, constants, 0);
          cmd->SetBufferSrv(3, g_r.bone_ring, bone_region);
          cmd->SetVertexBuffer(mit->second.vb_view.buffer,
                               mit->second.vb_view.offset,
                               mit->second.vb_view.size_bytes,
                               mit->second.vb_view.stride);
          cmd->SetIndexBuffer(mit->second.ib_view.buffer,
                              mit->second.ib_view.offset,
                              mit->second.ib_view.size_bytes);
          for (const mechanics_sandbox::map::VisualDraw& draw : chunk.draws) {
            const bool mask =
                draw.alpha_mode ==
                skate::world::SurfaceMaterial::AlphaMode::Mask;
            if (draw.alpha_mode ==
                    skate::world::SurfaceMaterial::AlphaMode::Blend ||
                mask != (phase == 1)) {
              continue;
            }
            if (phase == 1) {
              cmd->SetTexture(
                  1, ResolveOwnedMapTexture(context, draw.albedo_texture));
            }
            cmd->DrawIndexed(draw.index_count, draw.first_index, 0);
            ++owned_caster_draws;
          }
        }
      };
  // Per tile (0 = inner r/6, 1 = mid r/2, 2 = far), two phases each:
  // opaque statics, then the alpha-tested families (trees, alphatest
  // fences/grates) with the clip PSO and their diffuse.
  for (int tile = 0; tile < 3; ++tile) {
    const float* tvp = tile == 0 ? vp_inner : (tile == 1 ? vp_mid : vp);
    // Cull threshold in FAR uc units (finer tiles cover 1/ratio of the
    // far extent).
    const float cull = tile == 0   ? 1.05f / kRatioInner
                       : tile == 1 ? 1.05f / kRatioMid
                                   : 1.05f;
    cmd->SetViewport(
        nrhi::Viewport{size * tile, 0.0f, size, size, 0.0f, 1.0f});
    cmd->SetScissor(nrhi::Rect{int32_t(g_r.static_sun_size) * tile, 0,
                               int32_t(g_r.static_sun_size) * (tile + 1),
                               int32_t(g_r.static_sun_size)});
    for (int phase = 0; phase < 2; ++phase) {
      cmd->SetPipeline(phase == 0 ? g_r.pso_shadow_caster
                                  : g_r.pso_shadow_caster_clip);
      if (owned_shadow) {
        draw_owned_casters(tvp, cull, phase);
        continue;
      }
      for (const auto& [key, rec] : g_r.static_casters) {
        if (rec.clip != (phase == 1)) {
          continue;
        }
        auto mit = g_r.meshes.find(rec.mesh);
        // Fingerprint mismatch = the arena address was reused for
        // different content; drawing it would raster a foreign mesh into
        // the map. The record refreshes the next time the item is
        // actually seen.
        if (mit == g_r.meshes.end() ||
            mit->second.fingerprint != rec.fingerprint) {
          continue;
        }
        // NSM casters cover a wide radius the main pass never draws; touch
        // the LRU clock so the cascades keep their meshes resident.
        mit->second.last_used_frame = frame_number;
        // Cull by the mesh bbox footprint in (far) map space.
        float umin = std::numeric_limits<float>::max(), umax = -umin;
        float vmin = umin, vmax = -umin;
        for (int corner = 0; corner < 8; ++corner) {
          const float px = (corner & 1) ? rec.bbox_max[0] : rec.bbox_min[0];
          const float py = (corner & 2) ? rec.bbox_max[1] : rec.bbox_min[1];
          const float pz = (corner & 4) ? rec.bbox_max[2] : rec.bbox_min[2];
          float w[3];
          for (int a = 0; a < 3; ++a) {
            w[a] = px * rec.world[0 + a] + py * rec.world[4 + a] +
                   pz * rec.world[8 + a] + rec.world[12 + a];
          }
          const float u = rows[0] * w[0] + rows[1] * w[1] + rows[2] * w[2] +
                          rows[3];
          const float v = rows[4] * w[0] + rows[5] * w[1] + rows[6] * w[2] +
                          rows[7];
          umin = std::min(umin, u);
          umax = std::max(umax, u);
          vmin = std::min(vmin, v);
          vmax = std::max(vmax, v);
        }
        if (umax < -cull || umin > cull || vmax < -cull || vmin > cull) {
          continue;
        }
        float constants[52] = {};
        std::memcpy(constants, rec.world, sizeof(rec.world));
        float* mvp = constants + 16;
        for (int r = 0; r < 4; ++r) {
          for (int col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
              sum += rec.world[r * 4 + k] * tvp[k * 4 + col];
            }
            mvp[r * 4 + col] = sum;
          }
        }
        cmd->SetRootConstants(0, 52, constants, 0);
        cmd->SetBufferSrv(3, g_r.bone_ring, bone_region);
        if (phase == 1) {
          nrhi::TextureView* srv = LookupResolvedTexture(rec.diffuse_tex);
          cmd->SetTexture(1, srv != nullptr ? srv : g_r.white.srv);
        }
        cmd->SetVertexBuffer(mit->second.vb_view.buffer,
                             mit->second.vb_view.offset,
                             mit->second.vb_view.size_bytes,
                             mit->second.vb_view.stride);
        cmd->SetIndexBuffer(mit->second.ib_view.buffer,
                            mit->second.ib_view.offset,
                            mit->second.ib_view.size_bytes);
        for (const DrawEntry& draw : rec.draws) {
          if (draw.prim == 4) {
            cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
          } else if (draw.prim == 6) {
            cmd->SetPrimitiveTopology(
                nrhi::PrimitiveTopology::kTriangleStrip);
          } else {
            continue;
          }
          cmd->DrawIndexed(draw.index_count, draw.start_index,
                           draw.base_vertex);
        }
      }
    }
  }
  if (owned_shadow) {
    static std::atomic<uint32_t> s_owned_shadow_builds{0};
    const uint32_t build =
        s_owned_shadow_builds.fetch_add(1, std::memory_order_relaxed) + 1;
    if (build <= 8 || (build & 63u) == 0) {
      REXLOG_INFO(
          "native-scene: owned sun-shadow cascades rebuilt "
          "(chunks={} caster_draws={} radius={:.1f}m build={})",
          mechanics_sandbox::map::ActiveVisualWorld().chunks.size(),
          owned_caster_draws, radius, build);
    }
  }
  cmd->Barrier(g_r.static_sun, nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kPixelShaderResource);
  cmd->FlushBarriers();
  g_r.static_sun_in_srv = true;
  g_r.static_sun_valid = true;
}

bool RenderShadowAtlas(const NativeGuestOutputRenderContext& context,
                       const FrameScene& scene, const float* sh,
                       bool shadow_rows_valid, bool owned_shadow,
                       uint32_t bone_region, int32_t debug_mode,
                       uint32_t* out_draws) {
  nrhi::Cmd* cmd = context.cmd;
  bool shadow_ready = false;
  uint32_t shadow_draws = 0;
  size_t owned_caster_count = 0;
  // Atlas-readback bookkeeping: write completed dumps to disk in frame
  // order; re-arm once the recording session that requested them ends.
  if (!g_recording.load(std::memory_order_relaxed) &&
      g_r.shadow_dump_written == g_r.shadow_dump_enqueued) {
    g_r.shadow_dump_done = false;
    g_r.shadow_dump_enqueued = 0;
    g_r.shadow_dump_written = 0;
  }
  while (g_r.shadow_dump_written < g_r.shadow_dump_enqueued &&
         context.device->CompletedSubmission() >
             g_r.shadow_dump_submission[g_r.shadow_dump_written]) {
    const uint32_t k = g_r.shadow_dump_written;
    const uint32_t tile_px = g_r.shadow_tile;
    const uint32_t pitch = tile_px * 3u * 4u;
    const uint64_t plane = uint64_t(pitch) * tile_px;
    context.device->InvalidateForRead(g_r.shadow_dump_buf[k], 0, plane * 2);
    std::string dir = REXCVAR_GET(skate3_native_render_snapshot_dir);
    if (dir.empty()) {
      dir = "native_render_snapshots";
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path =
        std::filesystem::path(dir) /
        ("shadow_atlas_" + std::to_string(uint64_t(std::time(nullptr))) + "_f" +
         std::to_string(k) + "_" + std::to_string(tile_px * 3) + "x" +
         std::to_string(tile_px) + "_rg16.bin");
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(g_r.shadow_dump_ptr[k]),
              std::streamsize(plane * 2));
    REXLOG_INFO(
        "native-scene: shadow atlas frame {} dumped (raw+blurred planes) -> {}",
        k, path.string());
    ++g_r.shadow_dump_written;
  }
  if (REXCVAR_GET(skate3_native_render_scene_shadows) && shadow_rows_valid &&
      g_r.shadow_raw != nullptr && g_r.pso_shadow_caster != nullptr &&
      g_r.pso_shadow_blur != nullptr && debug_mode == 0) {
    struct Caster {
      const DrawItem* item;
      uint32_t bone_offset;
      bool bones;
      // Alpha-tested caster (dynamicobject.alphatest): draws with the clip
      // PSO and the item's diffuse bound so the cutout silhouette casts,
      // not the full card quad.
      bool clip;
      nrhi::TextureView* clip_srv;
    };
    std::vector<Caster> casters;
    bool any_clip = false;
    for (const DrawItem& item : scene.items) {
      if (item.transparent || item.unlit || item.cloth_quads) {
        continue;
      }
      const bool skinned = item.skinned && !item.bones.empty();
      // The game's CSM casts only DYNAMIC content: characters/vehicles
      // (skinned), Ropa cloth, and dynamicobject props (the truck's caster
      // draws, F10-verified). Static world scenery has its shadows BAKED
      // into the lightmaps, and that includes world-PLACED instances
      // (streetlights, trees, rails), which carry real world transforms.
      // The old identity-matrix test let every placed prop into the caster
      // pass, painting phantom streetlight-head/tree silhouettes onto the
      // plaza glass towers (a "reflection-like" soft dark blob high on the
      // facade, geometrically impossible as a real shadow: sun at 47 deg,
      // lamp 8 m tall, blob at 96 m; the emulated frame has no such
      // shadow). Moreover the cascade transforms are a stylized
      // near-vertical projection serving a ~12 m height window; tall
      // static geometry projected through them shadows its own plan-view
      // footprint. Live static shade comes from the separate sun-aligned
      // static shadow map instead (RenderStaticSunMap).
      if (!skinned && item.dynobj == 0 && !item.ropa && item.char_family == 0) {
        continue;
      }
      // Entities the game is holding invisible (spawn settle / distance
      // fade, see CharFadeAlpha) must not paint a shadow either; the
      // emulated frame shows neither the NPC nor a blob under it.
      if (item.char_family != 0 && CharFadeAlpha(item) < 0.05f &&
          REXCVAR_GET(skate3_native_render_scene_entity_fade)) {
        continue;
      }
      // Per-piece caster parity: the game's own shadow passes skip some
      // character pieces (the CAS trucker hat casts NOTHING; natively
      // casting it painted a hard low-sun brim band across the editor face
      // that the emulated frame never shows). Cast only what the game
      // submitted through an ortho bank this frame (see
      // DrawItem::shadow_caster / g_frame_ortho_ctx).
      if (item.char_family != 0 && !item.shadow_caster &&
          REXCVAR_GET(skate3_native_render_scene_shadow_caster_parity)) {
        continue;
      }
      // NO inline decode here (this block used to re-decode every cloth
      // garment every frame, ~2.9 ms, the 160 fps cap during real play).
      // The dyn decode jobs / worker miss queue keep the cache fresh, one
      // frame behind the sim; a first-sight caster shadows 1-2 frames late.
      if (!g_r.meshes.contains(item.mesh)) {
        continue;
      }
      Caster c{&item, 0, false, false, nullptr};
      if (item.dynobj == 2) {
        c.clip = true;
        c.clip_srv = LookupResolvedTexture(item.diffuse_tex);
        any_clip = true;
      }
      if (skinned) {
        const uint32_t bytes = uint32_t(item.bones.size() * sizeof(float));
        const uint32_t offset = (g_r.bone_ring_offset + 255u) & ~255u;
        if (offset + bytes > RendererState::kBoneRegionSize) {
          continue;
        }
        std::memcpy(g_r.bone_ring_cpu + bone_region + offset, item.bones.data(), bytes);
        g_r.bone_ring_offset = offset + bytes;
        c.bone_offset = offset;
        c.bones = true;
      }
      casters.push_back(c);
    }
    if (owned_shadow) {
      owned_caster_count = casters.size();
    }
    if (!casters.empty()) {
      if (g_r.shadow_in_srv_state) {
        for (nrhi::Texture* res : {g_r.shadow_raw, g_r.shadow_mid, g_r.shadow_final}) {
          cmd->Barrier(res, nrhi::ResourceState::kPixelShaderResource,
                       nrhi::ResourceState::kRenderTarget);
        }
        cmd->FlushBarriers();
        g_r.shadow_in_srv_state = false;
      }
      const uint32_t tile = g_r.shadow_tile;
      const float shadow_clear[4] = {1.0f, 1.0f, 0.0f, 0.0f};
      cmd->ClearRenderTarget(g_r.shadow_raw, shadow_clear);
      cmd->SetRenderTargets(g_r.shadow_raw, nullptr);
      cmd->SetBindingLayout(g_r.layout);
      cmd->SetPipeline(g_r.pso_shadow_caster);
      // Unused by the caster shaders, but never leave root CBVs unset.
      cmd->SetConstantBuffer(9, g_r.bone_ring, bone_region);
      for (int ci = 0; ci < 3; ++ci) {
        // Cascade scale/offset (cascade 0 = identity; PS c1/c2 for 1/2).
        float sx = 1.0f, sy = 1.0f, ox = 0.0f, oy = 0.0f;
        if (ci == 1) {
          sx = sh[4]; sy = sh[5]; ox = sh[6]; oy = sh[7];
        } else if (ci == 2) {
          sx = sh[8]; sy = sh[9]; ox = sh[10]; oy = sh[11];
        }
        // Light view-proj, row-vector convention: clip.x = ls_i.x,
        // clip.y = ls_i.y, clip.z = the height-ramp depth, clip.w = 1,
        // columns built from the receiver rows c0/c3/c4.
        float lightvp[16];
        for (int r = 0; r < 3; ++r) {
          lightvp[r * 4 + 0] = sh[0 + r] * sx;
          lightvp[r * 4 + 1] = sh[12 + r] * sy;
          lightvp[r * 4 + 2] = sh[16 + r];
          lightvp[r * 4 + 3] = 0.0f;
        }
        lightvp[12] = sh[3] * sx + ox;
        lightvp[13] = sh[15] * sy + oy;
        lightvp[14] = sh[19];
        lightvp[15] = 1.0f;
        cmd->SetViewport(nrhi::Viewport{float(tile) * ci, 0.0f, float(tile),
                                        float(tile), 0.0f, 1.0f});
        cmd->SetScissor(nrhi::Rect{int32_t(tile) * ci, 0,
                                   int32_t(tile) * (ci + 1), int32_t(tile)});
        // Two phases per cascade: opaque casters, then the alpha-tested
        // set with the clip PSO and per-item diffuse.
        for (int phase = 0; phase < (any_clip ? 2 : 1); ++phase) {
          cmd->SetPipeline(phase == 0 ? g_r.pso_shadow_caster
                                      : g_r.pso_shadow_caster_clip);
          for (const Caster& c : casters) {
            if (c.clip != (phase == 1)) {
              continue;
            }
            auto mit = g_r.meshes.find(c.item->mesh);
            if (mit == g_r.meshes.end()) {
              continue;  // undecodable this frame; casts once the workers land
            }
            // A stale fingerprint is fine here: cloth decodes ride one frame
            // behind the sim by design (dyn decode jobs).
            float constants[52] = {};
            std::memcpy(constants, c.item->world, sizeof(c.item->world));
            float* mvp = constants + 16;
            for (int r = 0; r < 4; ++r) {
              for (int col = 0; col < 4; ++col) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                  sum += c.item->world[r * 4 + k] * lightvp[k * 4 + col];
                }
                mvp[r * 4 + col] = sum;
              }
            }
            constants[33] = c.bones ? 1.0f : 0.0f;  // tint.g = skinned branch
            cmd->SetRootConstants(0, 52, constants, 0);
            cmd->SetBufferSrv(3, g_r.bone_ring,
                              bone_region + (c.bones ? c.bone_offset : 0));
            if (phase == 1) {
              cmd->SetTexture(1, c.clip_srv != nullptr ? c.clip_srv
                                                       : g_r.white.srv);
            }
            cmd->SetVertexBuffer(mit->second.vb_view.buffer,
                                 mit->second.vb_view.offset,
                                 mit->second.vb_view.size_bytes,
                                 mit->second.vb_view.stride);
            cmd->SetIndexBuffer(mit->second.ib_view.buffer,
                                mit->second.ib_view.offset,
                                mit->second.ib_view.size_bytes);
            for (const DrawEntry& draw : c.item->draws) {
              if (draw.prim == 4) {
                cmd->SetPrimitiveTopology(
                    nrhi::PrimitiveTopology::kTriangleList);
              } else if (draw.prim == 6) {
                cmd->SetPrimitiveTopology(
                    nrhi::PrimitiveTopology::kTriangleStrip);
              } else {
                continue;
              }
              cmd->DrawIndexed(draw.index_count, draw.start_index,
                               draw.base_vertex);
              ++shadow_draws;
            }
          }
        }
      }
      // Blur/convert chain: raw -> (hblur) -> mid -> (vblur) -> final. The
      // game's kernels: 5-tap Gaussian coverage cascade 0, 3-tap cascade 1,
      // format-convert only for cascade 2 (weights (1,0,0), 0 taps), plus
      // depth dilation into the penumbra. One fullscreen-triangle draw per
      // tile per direction, taps clamped inside the tile.
      cmd->Barrier(g_r.shadow_raw, nrhi::ResourceState::kRenderTarget,
                   nrhi::ResourceState::kPixelShaderResource);
      cmd->FlushBarriers();
      cmd->SetPipeline(g_r.pso_shadow_blur);
      cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
      const float kernels[3][3] = {{0.292082f, 0.233881f, 0.120078f},
                                   {0.667243f, 0.166379f, 0.0f},
                                   {1.0f, 0.0f, 0.0f}};
      const float ntaps[3] = {2.0f, 1.0f, 0.0f};
      const auto blur_pass = [&](int ci, bool horizontal,
                                 nrhi::TextureView* src, nrhi::Texture* dst,
                                 bool src_raw) {
        cmd->SetRenderTargets(dst, nullptr);
        cmd->SetViewport(nrhi::Viewport{float(tile) * ci, 0.0f, float(tile),
                                        float(tile), 0.0f, 1.0f});
        cmd->SetScissor(nrhi::Rect{int32_t(tile) * ci, 0,
                                   int32_t(tile) * (ci + 1), int32_t(tile)});
        // Blur tap offsets step RASTER texels, matching the emulated GPU:
        // the game's blur PS taps via tfetch integer offsets (OffsetX/Y
        // -2..+2), which a resolution-scaled
        // raster applies in physical texels, so the penumbra NARROWS as the
        // tile grows (the emulated baseline is sharper than original
        // hardware; 512 reproduces the softer original-360 look). Scaling
        // the step by tile/512 to hold a console-width penumbra was tried
        // and made receivers visibly blurrier than the emulated reference.
        const float bc[12] = {horizontal ? 1.0f : 0.0f, horizontal ? 0.0f : 1.0f,
                              ntaps[ci], src_raw ? 1.0f : 0.0f,
                              kernels[ci][0], kernels[ci][1], kernels[ci][2], 0.0f,
                              float(tile) * ci, float(tile) * (ci + 1) - 1.0f,
                              float(tile) - 1.0f, 0.0f};
        cmd->SetRootConstants(0, 12, bc, 0);
        cmd->SetTexture(1, src);
        cmd->Draw(3, 0);
      };
      for (int ci = 0; ci < 3; ++ci) {
        blur_pass(ci, true, g_r.shadow_srv_raw, g_r.shadow_mid, true);
      }
      cmd->Barrier(g_r.shadow_mid, nrhi::ResourceState::kRenderTarget,
                   nrhi::ResourceState::kPixelShaderResource);
      cmd->FlushBarriers();
      for (int ci = 0; ci < 3; ++ci) {
        blur_pass(ci, false, g_r.shadow_srv_mid, g_r.shadow_final, false);
      }
      cmd->Barrier(g_r.shadow_final, nrhi::ResourceState::kRenderTarget,
                   nrhi::ResourceState::kPixelShaderResource);
      cmd->FlushBarriers();
      g_r.shadow_in_srv_state = true;
      shadow_ready = shadow_draws > 0;
      // Atlas readback on kShadowDumpFrames consecutive frames per
      // recording session (see the completion handler at function entry):
      // per frame, raw plane at offset 0, blurred plane after it. Each
      // frame gets its own readback buffer so no frame waits on a copy.
      if (g_recording.load(std::memory_order_relaxed) &&
          !g_r.shadow_dump_done &&
          g_r.shadow_dump_enqueued < RendererState::kShadowDumpFrames) {
        const uint32_t k = g_r.shadow_dump_enqueued;
        const uint32_t pitch = tile * 3u * 4u;
        const uint64_t plane = uint64_t(pitch) * tile;
        if (g_r.shadow_dump_buf[k] == nullptr) {
          nrhi::BufferDesc desc;
          desc.size = plane * 2;
          desc.heap = nrhi::HeapKind::kReadback;
          g_r.shadow_dump_buf[k] = context.device->CreateBuffer(desc);
          g_r.shadow_dump_ptr[k] =
              g_r.shadow_dump_buf[k] != nullptr
                  ? static_cast<uint8_t*>(
                        context.device->Map(g_r.shadow_dump_buf[k]))
                  : nullptr;
        }
        if (g_r.shadow_dump_ptr[k] != nullptr) {
          cmd->Barrier(g_r.shadow_raw, nrhi::ResourceState::kPixelShaderResource,
                       nrhi::ResourceState::kCopySource);
          cmd->Barrier(g_r.shadow_final,
                       nrhi::ResourceState::kPixelShaderResource,
                       nrhi::ResourceState::kCopySource);
          cmd->FlushBarriers();
          cmd->CopyTextureToBuffer(g_r.shadow_dump_buf[k], 0, pitch,
                                   g_r.shadow_raw, 0, tile * 3, tile);
          cmd->CopyTextureToBuffer(g_r.shadow_dump_buf[k], plane, pitch,
                                   g_r.shadow_final, 0, tile * 3, tile);
          cmd->Barrier(g_r.shadow_raw, nrhi::ResourceState::kCopySource,
                       nrhi::ResourceState::kPixelShaderResource);
          cmd->Barrier(g_r.shadow_final, nrhi::ResourceState::kCopySource,
                       nrhi::ResourceState::kPixelShaderResource);
          cmd->FlushBarriers();
          g_r.shadow_dump_submission[k] = context.device->CurrentSubmission();
          ++g_r.shadow_dump_enqueued;
          if (g_r.shadow_dump_enqueued == RendererState::kShadowDumpFrames) {
            g_r.shadow_dump_done = true;
          }
        } else {
          g_r.shadow_dump_done = true;  // allocation failed; don't retry
        }
      }
    }
  }
  // ---- dynamicobject static world-shadow map ----
  // The game's props sample a 512x512 baked-shade depth map (tf1) the guest
  // never renders in native mode (its memory stays zeroed). Re-render it
  // natively from the frame's STATIC world items with the captured c5/c6/c7
  // projection, ps_shadow_caster convention (x = light-space depth, MIN
  // blend). The map ACCUMULATES across frames without clearing: the item
  // list is view-culled, and dropping off-screen casters would flicker
  // props' shade with the camera; static geometry seen once stays in the
  // map until the transform changes (streaming region switch), which
  // re-primes it.
  if (g_r.world_shadow != nullptr && g_r.pso_shadow_caster != nullptr &&
      debug_mode == 0 && scene.dynobj_ws_valid &&
      REXCVAR_GET(skate3_native_render_scene_shadows) &&
      REXCVAR_GET(skate3_native_render_scene_dynobj_v2)) {
    bool changed = !g_r.world_shadow_primed;
    for (int k = 0; k < 12 && !changed; ++k) {
      changed = std::fabs(scene.dynobj_ws[k] - g_r.world_shadow_rows[k]) >
                1e-4f * std::max(1.0f, std::fabs(g_r.world_shadow_rows[k]));
    }
    static uint64_t ws_pass_counter = 0;
    ++ws_pass_counter;
    // Accumulation cadence: a full static-item pass every 4th frame keeps
    // newly streamed / newly visible geometry flowing into the map at
    // negligible steady-state cost; a transform change renders immediately.
    if (changed || (ws_pass_counter & 3u) == 0) {
      if (g_r.world_shadow_in_srv) {
        cmd->Barrier(g_r.world_shadow, nrhi::ResourceState::kPixelShaderResource,
                     nrhi::ResourceState::kRenderTarget);
        cmd->FlushBarriers();
        g_r.world_shadow_in_srv = false;
      }
      if (changed) {
        const float ws_clear[4] = {1.0f, 1.0f, 0.0f, 0.0f};
        cmd->ClearRenderTarget(g_r.world_shadow, ws_clear);
        std::memcpy(g_r.world_shadow_rows, scene.dynobj_ws,
                    sizeof(g_r.world_shadow_rows));
        g_r.world_shadow_drawn.clear();
        if (!g_r.world_shadow_primed) {
          REXLOG_INFO(
              "native-scene: world-shadow map primed (dynobj c5/c6/c7 rows)");
        }
        g_r.world_shadow_primed = true;
      }
      // View-proj columns = the game's own projection rows: clip.x/y/z =
      // dot(wp4, c5/c6/c7), clip.w = 1 (same construction as the CSM
      // lightvp above).
      float wsvp[16];
      for (int r = 0; r < 3; ++r) {
        wsvp[r * 4 + 0] = scene.dynobj_ws[r];
        wsvp[r * 4 + 1] = scene.dynobj_ws[4 + r];
        wsvp[r * 4 + 2] = scene.dynobj_ws[8 + r];
        wsvp[r * 4 + 3] = 0.0f;
      }
      wsvp[12] = scene.dynobj_ws[3];
      wsvp[13] = scene.dynobj_ws[7];
      wsvp[14] = scene.dynobj_ws[11];
      wsvp[15] = 1.0f;
      cmd->SetRenderTargets(g_r.world_shadow, nullptr);
      cmd->SetBindingLayout(g_r.layout);
      cmd->SetPipeline(g_r.pso_shadow_caster);
      cmd->SetConstantBuffer(9, g_r.bone_ring, bone_region);
      const float ws_size = float(RendererState::kWorldShadowSize);
      cmd->SetViewport(nrhi::Viewport{0.0f, 0.0f, ws_size, ws_size, 0.0f, 1.0f});
      cmd->SetScissor(nrhi::Rect{0, 0, int32_t(RendererState::kWorldShadowSize),
                                 int32_t(RendererState::kWorldShadowSize)});
      for (const DrawItem& item : scene.items) {
        // STATIC world geometry only: the game's map bakes buildings/trees/
        // ground. Dynamic content (characters, vehicles, cloth, movable
        // props) casts through the CSM instead, accumulating a movable
        // prop here would freeze its shade at a stale position.
        if (item.transparent || item.unlit || item.cloth_quads || item.water ||
            item.skinned || item.ropa || item.char_family != 0 ||
            item.dynobj != 0) {
          continue;
        }
        auto mit = g_r.meshes.find(item.mesh);
        if (mit == g_r.meshes.end()) {
          continue;
        }
        // Accumulate each item exactly once per map generation: MIN-blend
        // re-draws are idempotent, so anything already in the map is pure
        // redundant work (the whole static item list re-rendered every 4th
        // frame paced this pass). Keyed on mesh + content fingerprint +
        // world transform; an in-place content swap or a moved instance
        // reads as new and lands on the next accumulation frame.
        uint64_t ws_key = 1469598103934665603ull ^ item.mesh;
        ws_key = (ws_key ^ mit->second.fingerprint) * 1099511628211ull;
        const uint8_t* wb = reinterpret_cast<const uint8_t*>(item.world);
        for (size_t bi = 0; bi < sizeof(item.world); ++bi) {
          ws_key = (ws_key ^ wb[bi]) * 1099511628211ull;
        }
        if (!g_r.world_shadow_drawn.insert(ws_key).second) {
          continue;
        }
        float constants[52] = {};
        std::memcpy(constants, item.world, sizeof(item.world));
        float* mvp = constants + 16;
        for (int r = 0; r < 4; ++r) {
          for (int col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
              sum += item.world[r * 4 + k] * wsvp[k * 4 + col];
            }
            mvp[r * 4 + col] = sum;
          }
        }
        cmd->SetRootConstants(0, 52, constants, 0);
        cmd->SetBufferSrv(3, g_r.bone_ring, bone_region);
        cmd->SetVertexBuffer(mit->second.vb_view.buffer,
                             mit->second.vb_view.offset,
                             mit->second.vb_view.size_bytes,
                             mit->second.vb_view.stride);
        cmd->SetIndexBuffer(mit->second.ib_view.buffer,
                            mit->second.ib_view.offset,
                            mit->second.ib_view.size_bytes);
        for (const DrawEntry& draw : item.draws) {
          if (draw.prim == 4) {
            cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
          } else if (draw.prim == 6) {
            cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
          } else {
            continue;
          }
          cmd->DrawIndexed(draw.index_count, draw.start_index, draw.base_vertex);
        }
      }
      cmd->Barrier(g_r.world_shadow, nrhi::ResourceState::kRenderTarget,
                   nrhi::ResourceState::kPixelShaderResource);
      cmd->FlushBarriers();
      g_r.world_shadow_in_srv = true;
    }
  }
  if (owned_shadow) {
    static uint64_t s_owned_shadow_frames = 0;
    static size_t s_last_owned_casters = std::numeric_limits<size_t>::max();
    static bool s_last_owned_ready = false;
    ++s_owned_shadow_frames;
    if (s_owned_shadow_frames <= 4 ||
        owned_caster_count != s_last_owned_casters ||
        shadow_ready != s_last_owned_ready ||
        (s_owned_shadow_frames % 600u) == 0) {
      REXLOG_INFO(
          "native-scene: owned dynamic shadows "
          "(scene_items={} casters={} draws={} ready={})",
          scene.items.size(), owned_caster_count, shadow_draws,
          shadow_ready ? 1 : 0);
      s_last_owned_casters = owned_caster_count;
      s_last_owned_ready = shadow_ready;
    }
  }
  *out_draws = shadow_draws;
  return shadow_ready;
}

  // Selection-outline mask (see kOutlineShaderSource): re-render the frame's
  // selected items into the small R8 target while the scene pass state is
  // still bound. The edge composite runs after the resolve, on the
  // single-sample output.
bool RenderOutlineMask(const NativeGuestOutputRenderContext& context,
                       const FrameScene& scene, const nrhi::Viewport& viewport,
                       const nrhi::Rect& scissor, bool msaa_on,
                       nrhi::Texture* scene_color, nrhi::Texture* scene_depth,
                       bool use_depth) {
  nrhi::Cmd* cmd = context.cmd;
  bool outline_ready = false;
  if (REXCVAR_GET(skate3_native_render_scene_selection_outline) &&
      g_r.pso_outline_mask != nullptr && g_r.pso_outline_edge != nullptr &&
      g_r.outline_mask != nullptr) {
    std::vector<const DrawItem*> sel;
    for (const DrawItem& item : scene.items) {
      // Skinned items are excluded: the mask VS runs the rigid path (world
      // matrix), which renders a skinned mesh at bind pose at the origin.
      if (item.selected && !item.skinned) {
        sel.push_back(&item);
      }
    }
    if (!sel.empty()) {
      const float mask_clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      cmd->ClearRenderTarget(g_r.outline_mask, mask_clear);
      cmd->SetRenderTargets(g_r.outline_mask, nullptr);
      cmd->SetViewport(nrhi::Viewport{0.0f, 0.0f,
                                      float(g_r.outline_mask_width),
                                      float(g_r.outline_mask_height), 0.0f,
                                      1.0f});
      cmd->SetScissor(nrhi::Rect{0, 0, int32_t(g_r.outline_mask_width),
                                 int32_t(g_r.outline_mask_height)});
      cmd->SetPipeline(g_r.pso_outline_mask);
      for (const DrawItem* item : sel) {
        auto mit = g_r.meshes.find(item->mesh);
        if (mit == g_r.meshes.end() || mit->second.fingerprint != item->fingerprint) {
          continue;  // decoded by the main pass this frame; masks from the next
        }
        float constants[52] = {};
        std::memcpy(constants, item->world, sizeof(item->world));
        float* mvp = constants + 16;
        for (int r = 0; r < 4; ++r) {
          for (int c = 0; c < 4; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
              sum += constants[r * 4 + k] * scene.view_proj[k * 4 + c];
            }
            mvp[r * 4 + c] = sum;
          }
        }
        // tint = (1, 0, 0, 1): the scene PS's solid-color early-out
        // (tint.a > 0) writes 1.0 into the R8 mask; tint.g = 0 keeps the VS
        // skinning branch off.
        constants[32] = 1.0f;
        constants[35] = 1.0f;
        cmd->SetRootConstants(0, 52, constants, 0);
        cmd->SetVertexBuffer(mit->second.vb_view.buffer,
                             mit->second.vb_view.offset,
                             mit->second.vb_view.size_bytes,
                             mit->second.vb_view.stride);
        cmd->SetIndexBuffer(mit->second.ib_view.buffer,
                            mit->second.ib_view.offset,
                            mit->second.ib_view.size_bytes);
        for (const DrawEntry& draw : item->draws) {
          if (draw.prim == 4) {
            cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
          } else if (draw.prim == 6) {
            cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
          } else {
            continue;
          }
          cmd->DrawIndexed(draw.index_count, draw.start_index,
                           draw.base_vertex);
          outline_ready = true;
        }
      }
      // Restore the pass state the resolve/2D paths rely on (fullscreen
      // viewport; the non-MSAA path keeps rendering into the scene target).
      cmd->SetViewport(viewport);
      cmd->SetScissor(scissor);
      if (!msaa_on) {
        cmd->SetRenderTargets(scene_color, use_depth ? scene_depth : nullptr);
      }
      if (outline_ready) {
        cmd->Barrier(g_r.outline_mask, nrhi::ResourceState::kRenderTarget,
                     nrhi::ResourceState::kPixelShaderResource);
      }
    }
  }
  return outline_ready;
}

// Selection-outline composite: additive stencil-edge-detect over the
// resolved output (before the popup blur, like the game's postfx order).
void RenderOutlineComposite(const NativeGuestOutputRenderContext& context,
                            const FrameScene& scene, nrhi::Texture* output,
                            const nrhi::Viewport& viewport,
                            const nrhi::Rect& scissor) {
  nrhi::Cmd* cmd = context.cmd;
  cmd->FlushBarriers();
  cmd->SetRenderTargets(output, nullptr);
  cmd->SetViewport(viewport);
  cmd->SetScissor(scissor);
  cmd->SetPipeline(g_r.pso_outline_edge);
  cmd->SetRootConstants(0, 4, scene.outline_color, 0);
  cmd->SetTexture(1, g_r.outline_mask_srv);
  cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
  cmd->Draw(3, 0);
  // Back to the mask's steady state for the next frame.
  cmd->Barrier(g_r.outline_mask, nrhi::ResourceState::kPixelShaderResource,
               nrhi::ResourceState::kRenderTarget);
}

// Windowed perf + telemetry log lines (verbatim from the former tail of
// RenderScene). Window length in frames = the perf-interval cvar.
void LogFrameStats(const FrameScene& scene, uint64_t frames, uint32_t drawn,
                   uint32_t drawn_2d, uint32_t drawn_spline, bool shadow_ready,
                   uint32_t shadow_draws) {
  // Raw guest-load read-fault recoveries (POSIX only; always 0 on Windows).
  // Each one is a streaming race that revoked a captured range mid-walk and
  // would otherwise be a fatal SIGSEGV; a slowly growing count during heavy
  // world streaming is expected and benign.
  {
    static uint64_t s_recov_reported = 0;
    static uint64_t s_recov_frame = 0;
    const uint64_t recov = GuestReadRecoveryCount();
    if (recov != s_recov_reported &&
        (s_recov_frame == 0 || frames - s_recov_frame >= 600)) {
      REXLOG_INFO("native-scene: guest read faults recovered total={} (+{})",
                  recov, recov - s_recov_reported);
      s_recov_reported = recov;
      s_recov_frame = frames;
    }
  }
  const uint64_t interval = uint64_t(
      std::max(60, REXCVAR_GET(skate3_native_render_scene_perf_interval)));
  if (frames % interval == 0 && REXCVAR_GET(skate3_native_render_scene_perf_log)) {
    // CPU-side perf snapshot for this window. guest_fps is derived
    // from the guest frame interval; capture/build run on the guest render
    // thread (they extend guest frame time directly), render/items/shadow on
    // the command processor thread, decode inline on the render thread
    // (count = decodes this window; max = the worst single decode).
    const double guest_dt_ms = g_pw_guest_dt.AvgMs();
    REXLOG_INFO(
        "native-scene perf: guest_fps={:.0f} guest_dt_max={:.1f}ms "
        "capture={:.2f}/{:.2f}ms build={:.2f}/{:.2f}ms "
        "bld[2d={:.2f}/{:.2f} spl={:.2f}/{:.2f} pal={:.2f}/{:.2f}]ms "
        "dt[{}|{}|{}|{}|{}] ptail={:.2f}/{:.2f}ms | render={:.2f}/{:.2f}ms "
        "items={:.2f}/{:.2f}ms shadow={:.2f}/{:.2f}ms "
        "pre={:.2f}/{:.2f}ms settle[{:.2f}/{:.2f}ms n={} dec={} def={}] "
        "tail={:.2f}/{:.2f}ms twod={:.2f}/{:.2f}ms "
        "decode[mesh n={} avg={:.2f} max={:.2f}ms tex n={} avg={:.2f} max={:.2f}ms] "
        "commit={:.2f}/{:.2f}ms itemcache[hit={} build={}] cam[chg={} rep={} maxstreak={}]",
        guest_dt_ms > 0.0 ? 1000.0 / guest_dt_ms : 0.0, g_pw_guest_dt.MaxMs(),
        g_pw_capture.AvgMs(), g_pw_capture.MaxMs(), g_pw_build.AvgMs(),
        g_pw_build.MaxMs(), g_pw_b2d.AvgMs(), g_pw_b2d.MaxMs(), g_pw_bspl.AvgMs(),
        g_pw_bspl.MaxMs(), g_pw_bpal.AvgMs(), g_pw_bpal.MaxMs(),
        g_dt_hist[0].exchange(0, std::memory_order_relaxed),
        g_dt_hist[1].exchange(0, std::memory_order_relaxed),
        g_dt_hist[2].exchange(0, std::memory_order_relaxed),
        g_dt_hist[3].exchange(0, std::memory_order_relaxed),
        g_dt_hist[4].exchange(0, std::memory_order_relaxed),
        g_pw_pal_tail.AvgMs(), g_pw_pal_tail.MaxMs(),
        g_pw_render.AvgMs(), g_pw_render.MaxMs(),
        g_pw_items.AvgMs(), g_pw_items.MaxMs(), g_pw_shadow.AvgMs(),
        g_pw_shadow.MaxMs(), g_pw_pre.AvgMs(), g_pw_pre.MaxMs(),
        g_pw_settle.AvgMs(), g_pw_settle.MaxMs(),
        g_pw_settle.count.load(std::memory_order_relaxed),
        g_warm_decodes.exchange(0, std::memory_order_relaxed),
        g_warm_deferred.exchange(0, std::memory_order_relaxed),
        g_pw_tail.AvgMs(), g_pw_tail.MaxMs(), g_pw_2d.AvgMs(), g_pw_2d.MaxMs(),
        g_pw_mesh_decode.count.load(std::memory_order_relaxed),
        g_pw_mesh_decode.AvgMs(), g_pw_mesh_decode.MaxMs(),
        g_pw_tex_decode.count.load(std::memory_order_relaxed), g_pw_tex_decode.AvgMs(),
        g_pw_tex_decode.MaxMs(), g_pw_commit.AvgMs(), g_pw_commit.MaxMs(),
        g_item_cache_hits.exchange(0, std::memory_order_relaxed),
        g_item_cache_builds.exchange(0, std::memory_order_relaxed),
        g_cam_changes.exchange(0, std::memory_order_relaxed),
        g_cam_repeats.exchange(0, std::memory_order_relaxed),
        g_cam_max_streak.exchange(0, std::memory_order_relaxed));
    REXLOG_INFO(
        "multiplayer-perf: samples={} "
        "local_pose={:.3f}/{:.3f}ms "
        "appearance={:.3f}/{:.3f}ms "
        "tick={:.3f}/{:.3f}ms "
        "install={:.3f}/{:.3f}ms "
        "remote={:.3f}/{:.3f}ms "
        "total={:.3f}/{:.3f}ms",
        g_pw_mp_total.count.load(std::memory_order_relaxed),
        g_pw_mp_local_pose.AvgMs(),
        g_pw_mp_local_pose.MaxMs(),
        g_pw_mp_local_appearance.AvgMs(),
        g_pw_mp_local_appearance.MaxMs(),
        g_pw_mp_tick.AvgMs(), g_pw_mp_tick.MaxMs(),
        g_pw_mp_install.AvgMs(), g_pw_mp_install.MaxMs(),
        g_pw_mp_remote.AvgMs(), g_pw_mp_remote.MaxMs(),
        g_pw_mp_total.AvgMs(), g_pw_mp_total.MaxMs());
    // Deep per-item attribution (see the perf-items cvar): visibility-class
    // draw costs, completed-draw stage split, build-walk decomposition, and
    // the off-screen retention pass. Averages are per item (the windows Add
    // once per item), so av= is the per-item unit cost in microseconds.
    if (REXCVAR_GET(skate3_native_render_scene_perf_items)) {
      const auto avg_us = [](const PerfWindow& w) { return w.AvgMs() * 1000.0; };
      REXLOG_INFO(
          "native-scene perf-items: draw[vis n={} av={:.2f}us mx={:.2f}ms | "
          "occ n={} av={:.2f}us mx={:.2f}ms | out n={} av={:.2f}us "
          "mx={:.2f}ms ret_out={}] idx_k[vis={} occ={} out={}] "
          "grid[valid={} age={}] "
          "stages_us[mesh={:.2f} tex={:.2f} const={:.2f} submit={:.2f} n={}] "
          "build_us[core n={} av={:.2f} fp n={} av={:.2f} walk n={} av={:.2f} "
          "fetch av={:.2f}] wloop[{:.2f}/{:.2f}ms recs={}] "
          "retain[{:.2f}/{:.2f}ms app={} live={}]",
          g_pw_di_in.count.load(std::memory_order_relaxed), avg_us(g_pw_di_in),
          g_pw_di_in.MaxMs(),
          g_pw_di_occ.count.load(std::memory_order_relaxed),
          avg_us(g_pw_di_occ), g_pw_di_occ.MaxMs(),
          g_pw_di_out.count.load(std::memory_order_relaxed),
          avg_us(g_pw_di_out), g_pw_di_out.MaxMs(),
          g_vis_out_retained.exchange(0, std::memory_order_relaxed),
          g_vis_in_indices.exchange(0, std::memory_order_relaxed) / 1000,
          g_vis_occ_indices.exchange(0, std::memory_order_relaxed) / 1000,
          g_vis_out_indices.exchange(0, std::memory_order_relaxed) / 1000,
          g_r.occl_grid_valid ? 1 : 0,
          g_r.occl_grid_valid ? frames - g_r.occl_grid_frame : 0,
          avg_us(g_pw_di_mesh), avg_us(g_pw_di_tex), avg_us(g_pw_di_const),
          avg_us(g_pw_di_submit),
          g_pw_di_submit.count.load(std::memory_order_relaxed),
          g_pw_bi_core.count.load(std::memory_order_relaxed),
          avg_us(g_pw_bi_core),
          g_pw_bi_fp.count.load(std::memory_order_relaxed), avg_us(g_pw_bi_fp),
          g_pw_bi_walk.count.load(std::memory_order_relaxed),
          avg_us(g_pw_bi_walk), avg_us(g_pw_bi_fetch), g_pw_bi_wloop.AvgMs(),
          g_pw_bi_wloop.MaxMs(),
          g_bi_records.exchange(0, std::memory_order_relaxed),
          g_pw_bi_retain.AvgMs(), g_pw_bi_retain.MaxMs(),
          g_retained_appended.exchange(0, std::memory_order_relaxed),
          g_retained_live.load(std::memory_order_relaxed));
    }
    for (PerfWindow* w : {&g_pw_guest_dt, &g_pw_capture, &g_pw_build, &g_pw_pal_tail,
                          &g_pw_b2d, &g_pw_bspl, &g_pw_bpal,
                          &g_pw_render, &g_pw_items, &g_pw_shadow, &g_pw_pre,
                          &g_pw_settle, &g_pw_tail, &g_pw_2d, &g_pw_mesh_decode,
                          &g_pw_tex_decode, &g_pw_commit,
                          &g_pw_mp_local_pose, &g_pw_mp_local_appearance,
                          &g_pw_mp_tick, &g_pw_mp_install, &g_pw_mp_remote,
                          &g_pw_mp_total,
                          &g_pw_di_in, &g_pw_di_occ, &g_pw_di_out,
                          &g_pw_di_mesh, &g_pw_di_tex, &g_pw_di_const,
                          &g_pw_di_submit, &g_pw_bi_core, &g_pw_bi_fp,
                          &g_pw_bi_walk, &g_pw_bi_fetch, &g_pw_bi_wloop,
                          &g_pw_bi_retain}) {
      w->Reset();
    }
    // Refill the settle offender-log + slow-frame budgets for the next window.
    g_warm_mesh_log_budget.store(4, std::memory_order_relaxed);
    g_warm_tex_log_budget.store(4, std::memory_order_relaxed);
    g_slow_frame_log_budget.store(3, std::memory_order_relaxed);
  }
  if (frames % interval == 0 && REXCVAR_GET(skate3_native_render_scene_perf_log)) {
    uint32_t lw_ctxs = 0, lw_ents = 0;
    skate3::native_lw::QueryLwStats(&lw_ctxs, &lw_ents);
    REXLOG_INFO(
        "native-scene: frame {} items={} draws={} draws_2d={} drawn_2d={} "
        "splines[{}/{}] "
        "2d[other={} dropped={} askip={} astale={} textures={}] cached_meshes={} mesh_mb={} textures={} tex_mb={} "
        "vs_uploads={} palettes={} palette_base_plus1={} ropa[rigid={} stale={} rescued={} relax={} caster={} incoh={} stretch={} blend={} blendmiss={}] dyn_gap={} skinned={} skinned_skipped={} foreign_bank={} "
        "rigid[pending={} dropped={} worldprops={}] "
        "rej[dyn={} range={} chain={} geom={} draws={} bbox={}] "
        "rr[decode_fail={} no_bones={} mesh_deferred={} tex_deferred={}] "
        "store[n={} routes={} evict={}] "
        "heal[vfail={} demote={}] serve[sticky={} skipnew={} adstale={} adnone={}] "
        "shadow[valid={} ready={} draws={}] char[attempt={} valid={} drawn={} "
        "reused={}] dynobj[valid={} drawn={}] water[valid={} drawn={}] "
        "lw[ctxs={} ents={} stamp={} fade0={} resc={} fill={} pal={} rows={}] "
        "refl[pair={} flat={} gate={:#x}] flips={} alt[bones={} shadow={}] "
        "rows_inst={} pal_srv={} occl_culled={} guest_skip={} build_skip={}",
        frames, scene.items.size(), drawn, g_draws_2d.load(), drawn_2d,
        drawn_spline, g_draws_spline.load(),
        g_draws_2d_other.load(), g_draws_2d_dropped.load(),
        g_2d_async_skip.load(), g_2d_async_stale.load(), g_r.tex_store.size(),
        g_r.meshes.size(), g_mesh_store_bytes >> 20, g_r.tex_store.size(),
        g_tex_store_bytes >> 20,
        g_vs_uploads.load(), g_palette_snapshots.load(), g_palette_base_plus1.load(),
        g_ropa_rigid.load(), g_ropa_stale.load(), g_ropa_rescued.load(),
        g_ropa_relaxed.load(), g_ropa_caster.load(), g_pub_incoherent.load(),
        g_stretch_veto.load(),
        g_ropa_blend_drawn.load(), g_ropa_blend_miss.load(), g_dyn_gap.load(),
        g_skinned_items.load(),
        g_skinned_skipped.load(), g_capture_foreign_bank.load(),
        g_rigid_pending.load(), g_rigid_dropped.load(),
        g_world_props.load(),
        g_rej_no_dynstate.load(), g_rej_dyn_range.load(),
        g_rej_chain.load(), g_rej_geom.load(), g_rej_draws.load(), g_rej_bbox.load(),
        g_rr_decode_fail.load(), g_rr_no_bones.load(), g_rr_mesh_deferred.load(),
        g_rr_tex_deferred.load(), g_r.tex_store.size(), g_r.tex_routes.size(),
        g_store_evicted.load(), g_heal_verify_fail.load(),
        g_demote_hold.load(),
        g_tex_sticky_served.load(), g_skip_new.load(), g_ad_stale_served.load(),
        g_ad_placeholder.load(), scene.shadow_valid,
        shadow_ready, shadow_draws,
        g_char_attempts.load(), g_char_valid.load(), g_char_drawn.load(),
        g_char_rows_reused.load(), scene.dynobj_valid,
        g_dynobj_drawn.load(), scene.water_valid, g_water_drawn.load(),
        lw_ctxs, lw_ents, g_lw_stamped.load(),
        g_lw_fade0.load(), g_lw_ctx_rescued.load(), g_lw_gap_filled.load(),
        g_lw_pal_sub.load(), g_lw_rows_served.load(),
        g_refl_pair.load(), g_refl_flat.load(), g_refl_gate.load(),
        g_hair_route_flips.load(), g_hair_bone_alternations.load(),
        g_shadow_alternations.load(), g_char_rows_inst_served.load(),
        g_pal_served_total.load(), g_occl_culled.load(),
        g_occl_guest_skipped.load(), g_occl_build_skipped.load());
  }
}

// ---- Graphics build-up showcase -------------------------------------------
// Sequencer for the layer-by-layer showcase (cvar
// skate3_native_render_scene_showcase / the bind_skate3_showcase hotkey):
// one wipe strips the frame to clay geometry, then each following wipe
// reveals the next visual layer, a vertical split sweeping left-to-right
// with BOTH sides rendered live (per-pixel stage selection in the shaders,
// no frozen frames). The split state rides the per-frame b1 rows
// (scene.hlsl sh_v2.yzw) and g_r.showcase_rows (the SSR composite reads it
// from root constants); every consumer treats an all-zero row as "showcase
// off". Runs on the render thread once per frame at b1 staging. The layer
// mask encoding matches the shader contract in scene.hlsl ShowcaseMask;
// the reveal order and grouping come from the user-editable
// skate3_native_render_scene_showcase_order cvar (kShowcaseLayers tokens).
static void TickShowcase(uint32_t output_width, bool hdr_on) {
  // One reveal step: the b1 row encoding (256 + the cumulative layer mask;
  // 0 = the full render) and its log label.
  struct Step {
    float value;
    std::string label;
  };
  struct State {
    bool active = false;
    std::vector<Step> steps;
    int logged = -1;
    double wipe_s = 3.0;
    double hold_s = 2.5;
    PerfClock::time_point t0;
  };
  static State s;
  float* rows = g_r.showcase_rows;
  rows[0] = rows[1] = rows[2] = 0.0f;
  const bool want = REXCVAR_GET(skate3_native_render_scene_showcase);
  if (!s.active) {
    if (!want) {
      // No run pending: release the showcase shader swap (the next
      // EnsurePipeline rebuild returns the family to the standard
      // variants with the showcase code compiled out).
      g_r.showcase_shaders_want = false;
      return;
    }
    // The run's shaders carry the split/mask gates only in their
    // SHOWCASE=1 variants: request the swap and hold the start until the
    // next EnsurePipeline rebuild has them live (rows stay zero, so the
    // waiting frames render normally).
    g_r.showcase_shaders_want = true;
    if (!g_r.showcase_shaders) {
      return;
    }
    // Layer availability: the material looks need nothing; the shadow and
    // post layers join only when their feature is enabled (the post gates
    // live in the HDR tonemap, so they need the HDR chain). The subtractive
    // layers need their content published/composited at all.
    const auto layer_available = [&](uint32_t bit) -> bool {
      switch (bit) {
        case 8u:
          return REXCVAR_GET(skate3_native_render_scene_shadows);
        case 16u:
          return hdr_on && REXCVAR_GET(skate3_native_render_scene_ssao);
        case 32u:
          return hdr_on && REXCVAR_GET(skate3_native_render_scene_ssr);
        case 64u:
          return hdr_on && (REXCVAR_GET(skate3_native_render_scene_shafts) ||
                            REXCVAR_GET(skate3_native_render_scene_haze));
        case 128u:
          return hdr_on && REXCVAR_GET(skate3_native_render_scene_bloom);
        case 256u:
          return REXCVAR_GET(skate3_native_render_scene_decals);
        case 512u:
          return REXCVAR_GET(skate3_native_render_scene_dynamic_items);
        default:
          return true;
      }
    };
    uint32_t avail_mask = 0;
    for (const auto& layer : kShowcaseLayers) {
      if (layer_available(layer.bit)) {
        avail_mask |= layer.bit;
      }
    }
    // Build the reveal steps from the user-ordered layer list (syntax in
    // the skate3_native_render_scene_showcase_order cvar description):
    // comma-separated steps, '+' joins layers into one wipe, '-' prefix =
    // disabled. Unknown, unavailable and duplicate tokens drop out; steps
    // left empty are skipped. The run opens ON black (mask bit 1024; a
    // snap, not a wipe - see the step-0 timing below), wipes clay in from
    // it, and closes by wiping back out to black. The >= hold_s stretches
    // of pure black double as machine-findable cut markers for screen
    // recordings (local/scripts/cut_showcase_clips.py pairs them with
    // ffmpeg blackdetect).
    s.steps.clear();
    s.steps.push_back({float(256u + 1024u), "black (start marker)"});
    s.steps.push_back({256.0f, "clay geometry"});
    uint32_t cum = 0;
    const std::string order =
        REXCVAR_GET(skate3_native_render_scene_showcase_order);
    size_t pos = 0;
    while (pos <= order.size()) {
      size_t end = order.find(',', pos);
      if (end == std::string::npos) {
        end = order.size();
      }
      const std::string group = order.substr(pos, end - pos);
      pos = end + 1;
      uint32_t add = 0;
      std::string label;
      size_t g = 0;
      while (g <= group.size()) {
        size_t ge = group.find('+', g);
        if (ge == std::string::npos) {
          ge = group.size();
        }
        std::string tok = group.substr(g, ge - g);
        g = ge + 1;
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) {
          tok.erase(tok.begin());
        }
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) {
          tok.pop_back();
        }
        for (char& ch : tok) {
          if (ch >= 'A' && ch <= 'Z') {
            ch = char(ch + 32);
          }
        }
        if (tok.empty() || tok[0] == '-') {
          continue;  // disabled layer (kept in the string for its position)
        }
        for (const auto& layer : kShowcaseLayers) {
          if (tok == layer.token) {
            if ((avail_mask & layer.bit) != 0 && (cum & layer.bit) == 0 &&
                (add & layer.bit) == 0) {
              add |= layer.bit;
              if (!label.empty()) {
                label += " & ";
              }
              label += layer.label;
            }
            break;
          }
        }
      }
      if (add != 0) {
        cum |= add;
        s.steps.push_back({float(256u + cum), std::move(label)});
      }
    }
    // Subtractive layers (decals/grime, dynamic entities) left out of the
    // order or '-'disabled are not build-up beats: fold their bits into
    // every step, clay included, so their content renders from the first
    // frame exactly as in the non-showcase frame. An UNAVAILABLE layer
    // needs no fold; its feature cvar already removes the content itself.
    const uint32_t always = kShowcaseSubtractiveMask & avail_mask & ~cum;
    if (always != 0u) {
      for (Step& st : s.steps) {
        st.value = float(256u + ((uint32_t(st.value) - 256u) | always));
      }
    }
    // A final full-render reveal when the list leaves layers unrevealed
    // (the materials look subsumes the albedo/lighting bits, so those two
    // stop counting once materials is in; the subtractive bits are either
    // revealed steps or folded in above, so they never force the step).
    const bool covered =
        (cum & 4u) != 0 && ((avail_mask & ~cum) & 0xF8u) == 0u;
    if (!covered) {
      s.steps.push_back({0.0f, "full render"});
    }
    // Closing blackout bookend (the end-of-run cut marker).
    s.steps.push_back({float(256u + 1024u), "black (end marker)"});
    // Durations snapshot at start so a live cvar edit cannot jump the
    // elapsed-time -> step mapping mid-run.
    s.wipe_s = std::max(0.2, REXCVAR_GET(skate3_native_render_scene_showcase_wipe));
    s.hold_s = std::max(0.0, REXCVAR_GET(skate3_native_render_scene_showcase_hold));
    s.t0 = PerfClock::now();
    s.logged = -1;
    s.active = true;
    REXLOG_INFO(
        "native-scene: showcase started ({} steps, wipe {:.1f}s, hold "
        "{:.1f}s, hdr={}, order=\"{}\")",
        s.steps.size(), s.wipe_s, s.hold_s, hdr_on ? 1 : 0, order);
  }
  if (!want) {
    s.active = false;
    g_r.showcase_shaders_want = false;
    REXLOG_INFO("native-scene: showcase cancelled");
    return;
  }
  const double elapsed =
      std::chrono::duration<double>(PerfClock::now() - s.t0).count();
  const double per_step = s.wipe_s + s.hold_s;
  // Step 0 (the blackout start bookend) SNAPS to black instead of wiping
  // from the live frame and lasts hold_s only: the recording cut lands on
  // a hard black edge, and the build-up proper starts with clay wiping in
  // from black.
  int idx;
  double phase;
  if (elapsed < s.hold_s) {
    idx = 0;
    phase = s.wipe_s;  // past the wipe window: hold, no split
  } else {
    const double e = elapsed - s.hold_s;
    idx = 1 + int(e / per_step);
    phase = e - double(idx - 1) * per_step;
  }
  if (idx >= int(s.steps.size())) {
    s.active = false;
    g_r.showcase_shaders_want = false;
    REXCVAR_SET(skate3_native_render_scene_showcase, false);
    REXLOG_INFO("native-scene: showcase finished");
    return;
  }
  const float cur = s.steps[idx].value;
  if (idx != s.logged) {
    REXLOG_INFO("native-scene: showcase step {}/{} - {}", idx + 1,
                s.steps.size(), s.steps[idx].label);
    s.logged = idx;
  }
  const float prev = idx == 0 ? 0.0f : s.steps[idx - 1].value;
  if (phase < s.wipe_s) {
    float f = float(phase / s.wipe_s);
    f = f * f * (3.0f - 2.0f * f);  // ease the sweep in and out
    rows[0] = cur;
    rows[1] = prev;
    // Overshoot slightly so the divider line exits the screen edge cleanly.
    rows[2] = f * float(output_width) * 1.02f;
  } else {
    rows[0] = rows[1] = cur;
    rows[2] = 0.0f;
  }
}

// True when the item's world-space bbox is provably hidden behind already-
// rendered geometry: its NEAREST corner view-depth is farther than the
// FARTHEST scene depth (tile MAX) in every occlusion-grid tile it covers.
// The corners are projected with the view_proj the grid's depth was
// rendered with (1-2 frames old; see the reduce in ApplySsaoPass), keeping
// bounds and depth consistent under camera motion. Anything not provable -
// invalid grid, a near-plane crosser, bounds leaving that frame's frustum -
// classifies as visible; the consumers (telemetry and the occlusion cull)
// must never overstate occlusion. `depth_margin` is the extra distance (m)
// the item must sit behind every tile's far depth: the telemetry uses a
// small margin (0.25, enough that an item's own last-frame depth counts as
// visible), the cull a larger one for disocclusion safety at speed.
bool ItemOccludedByGrid(const DrawItem& it, float depth_margin) {
  if (!g_r.occl_grid_valid ||
      g_r.occl_grid.size() <
          size_t(RendererState::kOcclGridW) * RendererState::kOcclGridH) {
    return false;
  }
  const float* vp = g_r.occl_grid_vp;
  float u0 = 1.0f, v0 = 1.0f, u1 = 0.0f, v1 = 0.0f;
  float min_z = 3.4e38f;
  for (int c = 0; c < 8; ++c) {
    const float l[3] = {c & 1 ? it.bbox_max[0] : it.bbox_min[0],
                        c & 2 ? it.bbox_max[1] : it.bbox_min[1],
                        c & 4 ? it.bbox_max[2] : it.bbox_min[2]};
    const float* w = it.world;
    float p[3];
    for (int k = 0; k < 3; ++k) {
      p[k] = l[0] * w[0 * 4 + k] + l[1] * w[1 * 4 + k] + l[2] * w[2 * 4 + k] +
             w[3 * 4 + k];
    }
    float clip[4];
    for (int k = 0; k < 4; ++k) {
      clip[k] = p[0] * vp[0 * 4 + k] + p[1] * vp[1 * 4 + k] +
                p[2] * vp[2 * 4 + k] + vp[3 * 4 + k];
    }
    if (clip[3] < 0.25f || clip[2] < 0.0f || clip[0] < -clip[3] ||
        clip[0] > clip[3] || clip[1] < -clip[3] || clip[1] > clip[3]) {
      return false;
    }
    const float u = 0.5f + 0.5f * clip[0] / clip[3];
    const float v = 0.5f - 0.5f * clip[1] / clip[3];
    u0 = std::min(u0, u);
    v0 = std::min(v0, v);
    u1 = std::max(u1, u);
    v1 = std::max(v1, v);
    min_z = std::min(min_z, clip[3]);  // row-vector proj: w == view Z
  }
  const int gw = int(RendererState::kOcclGridW);
  const int gh = int(RendererState::kOcclGridH);
  const int tx0 = std::clamp(int(u0 * float(gw)), 0, gw - 1);
  const int tx1 = std::clamp(int(u1 * float(gw)), 0, gw - 1);
  const int ty0 = std::clamp(int(v0 * float(gh)), 0, gh - 1);
  const int ty1 = std::clamp(int(v1 * float(gh)), 0, gh - 1);
  const float need = min_z - depth_margin;
  for (int ty = ty0; ty <= ty1; ++ty) {
    const float* row = &g_r.occl_grid[size_t(ty) * gw];
    for (int tx = tx0; tx <= tx1; ++tx) {
      if (row[tx] >= need) {
        return false;
      }
    }
  }
  return true;
}

// Culled-ctx publication for the guest-side dispatch filter: the classify
// loop collects the MeshContexts of statics it occlusion-culled and swaps
// them in here once per rendered frame (empty when the cull is inactive, so
// the guest filter clears within a frame). The timestamp lets the consumer
// expire the set when the native render idles.
std::mutex g_occl_pub_mutex;
std::vector<uint32_t> g_occl_pub_ctxs;
std::atomic<int64_t> g_occl_pub_ms{0};

}  // namespace

bool CopyOcclusionCulledCtxs(std::vector<uint32_t>& out) {
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  if (now_ms - g_occl_pub_ms.load(std::memory_order_relaxed) > 250) {
    out.clear();
    return false;
  }
  std::lock_guard<std::mutex> lock(g_occl_pub_mutex);
  out = g_occl_pub_ctxs;
  return !out.empty();
}

void AddGuestOcclSkipped(uint32_t n) {
  g_occl_guest_skipped.fetch_add(n, std::memory_order_relaxed);
}

namespace {

bool SandboxShouldDrawItem(const DrawItem& item) {
  if (!mechanics_sandbox::Active()) {
    return true;
  }
  switch (mechanics_sandbox::CurrentDiagnosticMode()) {
    case mechanics_sandbox::DiagnosticMode::BackgroundOnly:
      return false;
    case mechanics_sandbox::DiagnosticMode::CandidateWorldOn:
      return true;
    case mechanics_sandbox::DiagnosticMode::AllDynamic:
    case mechanics_sandbox::DiagnosticMode::CandidateOnly:
      // World sort-list contexts are non-zero too. Use the existing capture
      // provenance bit: dynamic presentation items are marked at their
      // verified capture seams, while static world items remain in the scene
      // only to preserve camera/frame state.
      return item.dyn_entity;
  }
  return false;
}

RaytracedDynamicScene BuildRaytracedLocalPresentation(
    const FrameScene& scene,
    const std::unordered_map<const DrawItem*,
                             const GuestTexture*>& diffuse_by_item) {
  RaytracedDynamicScene result;

  const auto normalize3 = [](float value[3]) {
    const float length_squared =
        value[0] * value[0] + value[1] * value[1] +
        value[2] * value[2];
    if (length_squared > 1.0e-12f) {
      const float inverse = 1.0f / std::sqrt(length_squared);
      value[0] *= inverse;
      value[1] *= inverse;
      value[2] *= inverse;
    } else {
      value[0] = 0.0f;
      value[1] = 1.0f;
      value[2] = 0.0f;
    }
  };
  const auto transform_position = [](const float matrix[16],
                                     const float input[3],
                                     float output[3]) {
    for (int column = 0; column < 3; ++column) {
      output[column] =
          input[0] * matrix[column] +
          input[1] * matrix[4 + column] +
          input[2] * matrix[8 + column] +
          matrix[12 + column];
    }
  };
  const auto transform_normal = [](const float matrix[16],
                                   const float input[3],
                                   float output[3]) {
    for (int column = 0; column < 3; ++column) {
      output[column] =
          input[0] * matrix[column] +
          input[1] * matrix[4 + column] +
          input[2] * matrix[8 + column];
    }
  };

  for (const DrawItem& item : scene.items) {
    if (!item.dyn_entity || item.ctx == 0 ||
        !SandboxShouldDrawItem(item)) {
      continue;
    }
    native_entity::CtxInfo identity;
    if (!native_entity::LookupCtx(item.ctx, &identity) ||
        mechanics_sandbox::ClassifyPresentationEntity(
            identity.entity,
            static_cast<uint8_t>(identity.cls)) !=
            mechanics_sandbox::PresentationDecision::Keep) {
      continue;
    }
    const auto mesh_it = g_r.meshes.find(item.mesh);
    if (mesh_it == g_r.meshes.end()) {
      continue;
    }
    const MeshBuffers& buffers = mesh_it->second;
    const std::vector<float>& decoded =
        item.ropa && !buffers.ropa_verts.empty()
            ? buffers.ropa_verts
            : buffers.raytracing_verts;
    if (decoded.empty() || decoded.size() % 14 != 0 ||
        buffers.raytracing_indices.empty()) {
      continue;
    }

    const uint32_t vertex_base =
        static_cast<uint32_t>(result.vertices.size());
    const uint32_t vertex_count =
        static_cast<uint32_t>(decoded.size() / 14);
    result.vertices.reserve(result.vertices.size() + vertex_count);
    for (uint32_t vertex = 0; vertex < vertex_count; ++vertex) {
      const float* source = decoded.data() + size_t(vertex) * 14;
      float position[3] = {source[0], source[1], source[2]};
      float normal[3] = {source[9], source[10], source[11]};
      if (item.skinned && !item.bones.empty()) {
        uint32_t packed_weights = 0;
        uint32_t packed_indices = 0;
        std::memcpy(&packed_weights, source + 7, sizeof(packed_weights));
        std::memcpy(&packed_indices, source + 8, sizeof(packed_indices));
        float skinned_position[3] = {};
        float skinned_normal[3] = {};
        float weight_sum = 0.0f;
        for (int influence = 0; influence < 4; ++influence) {
          const float weight =
              float((packed_weights >> (influence * 8)) & 0xffu) /
              255.0f;
          const uint32_t bone =
              (packed_indices >> (influence * 8)) & 0xffu;
          const size_t row = size_t(bone) * 12;
          if (weight <= 0.0f || row + 11 >= item.bones.size()) {
            continue;
          }
          const float* b = item.bones.data() + row;
          for (int axis = 0; axis < 3; ++axis) {
            const float* r = b + axis * 4;
            skinned_position[axis] +=
                weight * (position[0] * r[0] +
                          position[1] * r[1] +
                          position[2] * r[2] + r[3]);
            skinned_normal[axis] +=
                weight * (normal[0] * r[0] +
                          normal[1] * r[1] +
                          normal[2] * r[2]);
          }
          weight_sum += weight;
        }
        if (weight_sum > 0.001f) {
          const float inverse = 1.0f / weight_sum;
          for (int axis = 0; axis < 3; ++axis) {
            position[axis] = skinned_position[axis] * inverse;
            normal[axis] = skinned_normal[axis];
          }
        }
      } else {
        float transformed_normal[3];
        transform_normal(item.world, normal, transformed_normal);
        std::memcpy(normal, transformed_normal, sizeof(normal));
      }
      float world_position[3];
      transform_position(item.world, position, world_position);
      normalize3(normal);
      result.vertices.push_back(
          {{world_position[0], world_position[1], world_position[2]},
           {normal[0], normal[1], normal[2]},
           {source[3], source[4]}});
    }

    RaytracedDynamicMaterial material;
    // Keep a readable fallback for a texture still streaming, but use the
    // exact SRV served to the main character draw whenever it is available.
    // Character recolour rows are material multipliers, not replacement
    // albedo; ShadeCharacter applies them after sampling the authored page.
    if (item.tint[3] > 0.01f) {
      material.color[0] = std::max(0.03f, item.tint[0]);
      material.color[1] = std::max(0.03f, item.tint[1]);
      material.color[2] = std::max(0.03f, item.tint[2]);
    } else {
      static constexpr float palette[][3] = {
          {0.18f, 0.20f, 0.23f}, {0.48f, 0.12f, 0.08f},
          {0.11f, 0.22f, 0.32f}, {0.52f, 0.39f, 0.20f},
          {0.32f, 0.34f, 0.36f}};
      const auto& color = palette[(item.mesh >> 4) % std::size(palette)];
      std::memcpy(material.color, color, sizeof(material.color));
    }
    const auto texture_it = diffuse_by_item.find(&item);
    if (texture_it != diffuse_by_item.end() &&
        texture_it->second != nullptr &&
        texture_it->second->texture != nullptr &&
        texture_it->second->srv != nullptr) {
      const auto existing = std::find_if(
          result.diffuse_textures.begin(),
          result.diffuse_textures.end(),
          [texture_it](const RaytracedDynamicScene::TextureBinding& binding) {
            return binding.view == texture_it->second->srv;
          });
      if (existing != result.diffuse_textures.end()) {
        material.texture_index =
            static_cast<std::uint32_t>(
                existing - result.diffuse_textures.begin());
      } else if (result.diffuse_textures.size() <
                 kRaytracedDynamicTextureLimit) {
        material.texture_index =
            static_cast<std::uint32_t>(
                result.diffuse_textures.size());
        result.diffuse_textures.push_back(
            {texture_it->second->texture,
             texture_it->second->srv});
      }
    }
    material.roughness =
        item.char_family == 0 ? 0.55f : 0.78f;
    if (item.char_family != 0 &&
        item.char_rows[14 * 4 + 1] > 0.0f) {
      material.lighting_index =
          static_cast<uint32_t>(
              result.character_lighting.size());
      material.character_family = item.char_family;
      RaytracedCharacterLighting lighting;
      std::memcpy(
          lighting.rows, item.char_rows,
          sizeof(lighting.rows));
      result.character_lighting.push_back(lighting);
    }

    const auto append_triangle =
        [&](uint32_t a, uint32_t b, uint32_t c) {
          if (a >= vertex_count || b >= vertex_count ||
              c >= vertex_count || a == b || b == c || a == c) {
            return;
          }
          result.indices.push_back(vertex_base + a);
          result.indices.push_back(vertex_base + b);
          result.indices.push_back(vertex_base + c);
          result.triangle_materials.push_back(material);
        };
    for (const DrawEntry& draw : item.draws) {
      if (uint64_t(draw.start_index) + draw.index_count >
          buffers.raytracing_indices.size()) {
        continue;
      }
      const uint16_t* indices =
          buffers.raytracing_indices.data() + draw.start_index;
      if (draw.prim == 4) {
        for (uint32_t i = 0; i + 2 < draw.index_count; i += 3) {
          append_triangle(
              uint32_t(indices[i]) + draw.base_vertex,
              uint32_t(indices[i + 1]) + draw.base_vertex,
              uint32_t(indices[i + 2]) + draw.base_vertex);
        }
      } else if (draw.prim == 6) {
        for (uint32_t i = 0; i + 2 < draw.index_count; ++i) {
          const uint32_t a =
              uint32_t(indices[i + (i & 1u)]) + draw.base_vertex;
          const uint32_t b =
              uint32_t(indices[i + ((i & 1u) ? 0u : 1u)]) +
              draw.base_vertex;
          const uint32_t c =
              uint32_t(indices[i + 2]) + draw.base_vertex;
          append_triangle(a, b, c);
        }
      }
    }
  }
  float map_origin[3] = {};
  if (mechanics_sandbox::SandboxMapRenderOrigin(map_origin)) {
    const mechanics_sandbox::map::VisualMesh& sphere =
        mechanics_sandbox::map::ActiveMovingLightVisualMesh();
    for (std::size_t light_index = 0;
         light_index <
             mechanics_sandbox::map::ActiveMovingLightCount();
         ++light_index) {
      mechanics_sandbox::map::MovingLightSnapshot light;
      if (!mechanics_sandbox::map::ActiveMovingLightSnapshot(
              light_index, light)) {
        continue;
      }
      RaytracedMovingLight rt_light;
      rt_light.position_range[0] =
          map_origin[0] + light.position[0];
      rt_light.position_range[1] =
          map_origin[1] + light.position[1];
      rt_light.position_range[2] =
          map_origin[2] + light.position[2];
      rt_light.position_range[3] = light.influence_radius;
      rt_light.color_intensity[0] = light.color[0];
      rt_light.color_intensity[1] = light.color[1];
      rt_light.color_intensity[2] = light.color[2];
      rt_light.color_intensity[3] = light.intensity;
      rt_light.source_radius = light.source_radius;
      result.moving_lights.push_back(rt_light);

      if (!light.visible_source) {
        continue;
      }
      const std::uint32_t vertex_base =
          static_cast<std::uint32_t>(result.vertices.size());
      result.vertices.reserve(
          result.vertices.size() + sphere.vertices.size());
      for (const mechanics_sandbox::map::VisualVertex& source :
           sphere.vertices) {
        result.vertices.push_back(
            {{rt_light.position_range[0] +
                  source.position[0] * light.source_radius,
              rt_light.position_range[1] +
                  source.position[1] * light.source_radius,
              rt_light.position_range[2] +
                  source.position[2] * light.source_radius},
             {source.normal[0], source.normal[1], source.normal[2]},
             {source.uv[0], source.uv[1]}});
      }
      RaytracedDynamicMaterial emissive;
      emissive.color[0] = light.color[0] * 4.5f;
      emissive.color[1] = light.color[1] * 4.5f;
      emissive.color[2] = light.color[2] * 4.5f;
      emissive.roughness = 0.08f;
      emissive.texture_flags = 1u;
      for (std::size_t index = 0;
           index + 2 < sphere.indices.size(); index += 3) {
        result.indices.push_back(
            vertex_base + sphere.indices[index]);
        result.indices.push_back(
            vertex_base + sphere.indices[index + 1]);
        result.indices.push_back(
            vertex_base + sphere.indices[index + 2]);
        result.triangle_materials.push_back(emissive);
      }
    }
    mechanics_sandbox::map::MovingLightSnapshot lightning;
    if (result.moving_lights.size() < 4 &&
        mechanics_sandbox::map::ActiveLightningLightSnapshot(
            lightning)) {
      RaytracedMovingLight rt_light;
      rt_light.position_range[0] =
          map_origin[0] + lightning.position[0];
      rt_light.position_range[1] =
          map_origin[1] + lightning.position[1];
      rt_light.position_range[2] =
          map_origin[2] + lightning.position[2];
      rt_light.position_range[3] =
          lightning.influence_radius;
      rt_light.color_intensity[0] = lightning.color[0];
      rt_light.color_intensity[1] = lightning.color[1];
      rt_light.color_intensity[2] = lightning.color[2];
      rt_light.color_intensity[3] = lightning.intensity;
      rt_light.source_radius = lightning.source_radius;
      result.moving_lights.push_back(rt_light);
    }
  }
  return result;
}

#pragma pack(push, 1)
struct NetworkAppearanceHeader {
  uint32_t magic = 0x31505041u;  // "APP1"
  uint16_t version = 2;
  uint16_t texture_count = 0;
  uint16_t piece_count = 0;
  uint16_t reserved = 0;
  float root_position[3] = {};
  float root_x_axis[3] = {1.0f, 0.0f, 0.0f};
  float root_z_axis[3] = {0.0f, 0.0f, 1.0f};
};

struct NetworkAppearanceMip {
  uint32_t offset = 0;
  uint32_t pitch = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

struct NetworkAppearanceTexture {
  uint64_t source_key = 0;
  uint32_t copy_format = 0;
  uint32_t srv_format = 0;
  uint32_t payload_bytes = 0;
  uint16_t mip_count = 0;
  uint8_t swizzle[4] = {};
  uint16_t reserved = 0;
  NetworkAppearanceMip mips[16] = {};
};

struct NetworkAppearancePiece {
  uint32_t track_key = 0;
  uint64_t fingerprint = 0;
  uint32_t flags = 0;
  uint8_t char_family = 0;
  uint8_t reserved0 = 0;
  uint16_t bone_count = 0;
  uint16_t draw_count = 0;
  uint16_t remap_count = 0;
  uint32_t vertex_count = 0;
  uint32_t index_count = 0;
  uint16_t diffuse_texture = 0xFFFFu;
  uint16_t normal_texture = 0xFFFFu;
  uint16_t alpha_texture = 0xFFFFu;
  uint16_t spec_texture = 0xFFFFu;
  float char_rows[72] = {};
  float tint[4] = {};
  float world[16] = {};
  float bbox_min[3] = {};
  float bbox_max[3] = {};
};

struct NetworkRecipeAppearanceHeader {
  uint32_t magic = 0x31504352u;  // "RCP1"
  uint16_t version = 1;
  uint16_t binding_count = 0;
  uint32_t recipe_bytes = 0;
  float root_position[3] = {};
  float root_x_axis[3] = {1.0f, 0.0f, 0.0f};
  float root_z_axis[3] = {0.0f, 0.0f, 1.0f};
};

struct NetworkRecipeAppearanceBinding {
  uint64_t model_id = 0;
  uint32_t track_key = 0;
  uint32_t flags = 0;
  uint8_t char_family = 0;
  uint8_t reserved = 0;
  uint16_t remap_count = 0;
  float char_rows[72] = {};
  float tint[4] = {};
};
#pragma pack(pop)

enum NetworkAppearancePieceFlags : uint32_t {
  kNetworkPieceSkinned = 1u << 0,
  kNetworkPieceHair = 1u << 1,
  kNetworkPieceRopa = 1u << 2,
  kNetworkPieceCharAlpha = 1u << 3,
  kNetworkPieceShadow = 1u << 4,
};

void NormalizeRemoteAppearanceTransientRows(
    DrawItem& item, uint32_t role) {
  const float captured_fade = item.char_rows[14 * 4 + 0];
  const float captured_hair_scale =
      item.char_rows[13 * 4 + 3];
  // Appearance rows are transmitted once, but these fields are per-frame
  // entity fades. Capturing a player during map-entry fade permanently made
  // ordinary pieces invisible and multiplied hair strand coverage by values
  // such as 0.275. Remote-player visibility is managed by multiplayer
  // relevance; retain the coverage texture, but never retain the sender's
  // transient fade in the appearance recipe.
  item.char_rows[14 * 4 + 0] = 1.0f;
  if (item.hair) {
    item.char_rows[13 * 4 + 3] = 1.0f;
  }
  if (captured_fade < 0.999f ||
      (item.hair && captured_hair_scale < 0.999f)) {
    REXLOG_INFO(
        "multiplayer-appearance-normalize: role={} mesh={:08X} "
        "kind={} fade={:.3f}->1.000 hair_scale={:.3f}->{}",
        role, item.mesh, item.hair ? "hair" : "character",
        captured_fade, captured_hair_scale,
        item.hair ? "1.000" : "unchanged");
  }
}

template <typename T>
void AppendAppearancePod(
    std::vector<uint8_t>& bytes, const T& value) {
  static_assert(std::is_trivially_copyable_v<T>);
  const auto* begin =
      reinterpret_cast<const uint8_t*>(&value);
  bytes.insert(bytes.end(), begin, begin + sizeof(T));
}

template <typename T>
bool ReadAppearancePod(
    const std::vector<uint8_t>& bytes,
    size_t& cursor, T& value) {
  static_assert(std::is_trivially_copyable_v<T>);
  if (cursor + sizeof(T) > bytes.size()) {
    return false;
  }
  std::memcpy(&value, bytes.data() + cursor, sizeof(T));
  cursor += sizeof(T);
  return true;
}

uint64_t AppearanceHashBytes(
    uint64_t hash, const void* data, size_t bytes) {
  const auto* source =
      static_cast<const uint8_t*>(data);
  for (size_t index = 0; index < bytes; ++index) {
    hash = (hash ^ source[index]) * 1099511628211ull;
  }
  return hash;
}

struct CapturedAppearanceTexture {
  NetworkAppearanceTexture header;
  std::vector<uint8_t> payload;
};

using AppearanceWeightedRows =
    multiplayer::render_cache::WeightedPaletteRows;

bool CaptureAppearanceTexture(
    const NativeGuestOutputRenderContext& context,
    uint8_t* base, uint32_t texture_object,
    CapturedAppearanceTexture& output) {
  if (base == nullptr || texture_object == 0) {
    return false;
  }
  uint32_t words[6] = {};
  if (!ReadStableTexWords(base, texture_object, words)) {
    return false;
  }
  GuestTexture staged;
  StagedTexCommit commit;
  g_tex_stage_out = &commit;
  const bool decoded =
      EnsureGuestTextureFromWords(
          context, base, words, staged);
  g_tex_stage_out = nullptr;
  if (!decoded || staged.texture == nullptr ||
      staged.upload == nullptr || commit.mip_count == 0 ||
      commit.mip_count > 16 ||
      commit.copy_format == nrhi::Format::kUnknown ||
      commit.srv_format == nrhi::Format::kUnknown ||
      staged.upload->size() == 0 ||
      staged.upload->size() > 8u * 1024u * 1024u) {
    context.device->DestroyDeferred(staged.texture);
    context.device->DestroyDeferred(staged.upload);
    return false;
  }
  void* mapping = context.device->Map(staged.upload);
  if (mapping == nullptr) {
    context.device->DestroyDeferred(staged.texture);
    context.device->DestroyDeferred(staged.upload);
    return false;
  }
  output = {};
  output.header.source_key = FetchWordsKey(words);
  output.header.copy_format =
      static_cast<uint32_t>(commit.copy_format);
  output.header.srv_format =
      static_cast<uint32_t>(commit.srv_format);
  output.header.payload_bytes =
      static_cast<uint32_t>(staged.upload->size());
  output.header.mip_count =
      static_cast<uint16_t>(commit.mip_count);
  for (size_t component = 0; component < 4; ++component) {
    output.header.swizzle[component] =
        static_cast<uint8_t>(commit.swizzle[component]);
  }
  for (size_t mip = 0; mip < commit.mip_count; ++mip) {
    output.header.mips[mip] = {
        commit.mips[mip].offset,
        commit.mips[mip].pitch,
        commit.mips[mip].w,
        commit.mips[mip].h};
  }
  output.payload.resize(staged.upload->size());
  std::memcpy(
      output.payload.data(), mapping,
      output.payload.size());
  context.device->Unmap(staged.upload);
  context.device->DestroyDeferred(staged.texture);
  context.device->DestroyDeferred(staged.upload);
  return true;
}

multiplayer::AppearanceBlob BuildLocalRecipeAppearanceBlob(
    const std::vector<const DrawItem*>& items,
    const multiplayer::AnimationPose& animation) {
  constexpr uint32_t kRecipeGuestAddress = 0x8309A800u;
  constexpr size_t kRecipeReadBytes = 6500;
  static uint64_t cached_recipe_identity = 0;
  static std::vector<uint8_t> cached_recipe_bytes;
  static std::vector<uint8_t> profile_recipe_bytes;
  static multiplayer_assets::RecipeAppearance cached_recipe;
  static multiplayer::AppearanceBlob cached_blob;
  uint8_t* base =
      g_guest_base.load(std::memory_order_relaxed);
  if (base == nullptr || items.empty() ||
      !animation.presentation_root_valid) {
    return {};
  }
  multiplayer_assets::PollLocalProfileRecipe(
      profile_recipe_bytes);
  std::array<uint8_t, kRecipeReadBytes> recipe_buffer{};
  const uint8_t* recipe_data = nullptr;
  size_t recipe_size = 0;
  if (!profile_recipe_bytes.empty()) {
    recipe_data = profile_recipe_bytes.data();
    recipe_size = profile_recipe_bytes.size();
  } else {
    if (!GuestTryCopy(
            recipe_buffer.data(), base + kRecipeGuestAddress,
            recipe_buffer.size())) {
      return {};
    }
    recipe_size = recipe_buffer.size();
    while (recipe_size != 0 &&
           recipe_buffer[recipe_size - 1] == 0) {
      --recipe_size;
    }
    recipe_data = recipe_buffer.data();
  }
  if (recipe_data == nullptr || recipe_size < 64) {
    return {};
  }
  const uint64_t recipe_identity = AppearanceHashBytes(
      1469598103934665603ull,
      recipe_data, recipe_size);
  if (recipe_identity != cached_recipe_identity) {
    std::vector<uint8_t> recipe(
        recipe_data, recipe_data + recipe_size);
    multiplayer_assets::RecipeAppearance resolved;
    if (!multiplayer_assets::ResolveRecipeAppearance(
            recipe, false, resolved)) {
      return cached_blob;
    }
    cached_recipe_identity = recipe_identity;
    cached_recipe_bytes = std::move(recipe);
    cached_recipe = std::move(resolved);
  }
  if (cached_recipe.pieces.empty()) {
    return cached_blob;
  }
  const std::vector<uint8_t>& recipe = cached_recipe_bytes;

  struct BindingBuild {
    NetworkRecipeAppearanceBinding wire;
    std::vector<uint16_t> remap;
  };
  // Dynamic character pieces are not guaranteed to submit on every rendered
  // frame. Retain the most recently matched animation binding for each recipe
  // model so a transiently absent deck, truck, wheel, or garment does not
  // replace its real sender track key with zero and churn the appearance.
  static uint64_t cached_binding_recipe_identity = 0;
  static std::unordered_map<uint64_t, BindingBuild>
      cached_bindings;
  if (cached_binding_recipe_identity != recipe_identity) {
    cached_binding_recipe_identity = recipe_identity;
    cached_bindings.clear();
  }
  std::vector<BindingBuild> bindings;
  bindings.reserve(cached_recipe.pieces.size());
  std::unordered_set<uint32_t> used_meshes;
  size_t matched_pieces = 0;
  for (const multiplayer_assets::RecipePiece& recipe_piece :
       cached_recipe.pieces) {
    BindingBuild binding;
    binding.wire.model_id = recipe_piece.model_id;
    const uint64_t recipe_topology = AppearanceHashBytes(
        1469598103934665603ull,
        recipe_piece.mesh.indices.data(),
        recipe_piece.mesh.indices.size() * sizeof(uint16_t));
    const DrawItem* match = nullptr;
    for (const DrawItem* item : items) {
      if (item == nullptr || item->mesh == 0 ||
          used_meshes.contains(item->mesh)) {
        continue;
      }
      const auto mesh = g_r.meshes.find(item->mesh);
      if (mesh == g_r.meshes.end() ||
          mesh->second.raytracing_verts.size() !=
              recipe_piece.mesh.vertices.size() ||
          mesh->second.raytracing_indices.size() !=
              recipe_piece.mesh.indices.size()) {
        continue;
      }
      const uint64_t item_topology = AppearanceHashBytes(
          1469598103934665603ull,
          mesh->second.raytracing_indices.data(),
          mesh->second.raytracing_indices.size() *
              sizeof(uint16_t));
      if (item_topology == recipe_topology) {
        match = item;
        break;
      }
    }
    if (match != nullptr) {
      used_meshes.insert(match->mesh);
      ++matched_pieces;
      // Recipe geometry is an immutable bind mesh. A ROPA source track
      // describes a dynamic CPU-cloth VB and may temporarily switch modes
      // when the garment table changes. Applying that exact track to the
      // bind mesh double-deforms it into a long ribbon. Leave ROPA recipe
      // pieces on the canonical body track and use the recipe mesh's own
      // palette mapping. Detached-board tracks remain exact.
      binding.wire.track_key =
          match->ropa ? 0u : match->mesh;
      binding.wire.flags =
          (match->skinned ? kNetworkPieceSkinned : 0u) |
          (match->hair ? kNetworkPieceHair : 0u) |
          (match->ropa ? kNetworkPieceRopa : 0u) |
          (match->char_alpha ? kNetworkPieceCharAlpha : 0u) |
          (match->shadow_caster ? kNetworkPieceShadow : 0u);
      binding.wire.char_family = match->char_family;
      std::copy_n(
          match->char_rows, 72, binding.wire.char_rows);
      std::copy_n(
          match->tint, 4, binding.wire.tint);
      if (match->ropa) {
        binding.remap =
            recipe_piece.mesh.palette_to_canonical;
      } else {
        native_palette::CanonicalRigSample rig;
        if (native_palette::LookupCanonicalRig(
                match->ctx, match->mesh, rig) &&
            !rig.palette_to_canonical.empty()) {
          binding.remap.assign(
              rig.palette_to_canonical.begin(),
              rig.palette_to_canonical.end());
        }
      }
    } else {
      const auto cached_binding =
          cached_bindings.find(recipe_piece.model_id);
      if (cached_binding != cached_bindings.end()) {
        binding = cached_binding->second;
      }
    }
    if (binding.remap.empty()) {
      binding.remap =
          recipe_piece.mesh.palette_to_canonical;
    }
    if (binding.remap.size() > 256) {
      binding.remap.resize(256);
    }
    binding.wire.remap_count =
        static_cast<uint16_t>(binding.remap.size());
    if (match != nullptr) {
      cached_bindings[recipe_piece.model_id] = binding;
    }
    bindings.push_back(std::move(binding));
  }
  if (bindings.empty()) {
    return cached_blob;
  }

  uint64_t identity = recipe_identity;
  for (const BindingBuild& binding : bindings) {
    identity = AppearanceHashBytes(
        identity, &binding.wire.model_id,
        sizeof(binding.wire.model_id));
    identity = AppearanceHashBytes(
        identity, &binding.wire.track_key,
        sizeof(binding.wire.track_key));
    identity = AppearanceHashBytes(
        identity, &binding.wire.flags,
        sizeof(binding.wire.flags));
    identity = AppearanceHashBytes(
        identity, &binding.wire.char_family,
        sizeof(binding.wire.char_family));
    identity = AppearanceHashBytes(
        identity, binding.wire.tint,
        sizeof(binding.wire.tint));
    if (!binding.remap.empty()) {
      identity = AppearanceHashBytes(
          identity, binding.remap.data(),
          binding.remap.size() * sizeof(uint16_t));
    }
  }
  if (cached_blob.identity == identity &&
      cached_blob.bytes != nullptr) {
    return cached_blob;
  }

  NetworkRecipeAppearanceHeader header;
  header.binding_count =
      static_cast<uint16_t>(bindings.size());
  header.recipe_bytes =
      static_cast<uint32_t>(recipe.size());
  std::copy_n(
      animation.presentation_root_position, 3,
      header.root_position);
  std::copy_n(
      animation.presentation_root_x_axis, 3,
      header.root_x_axis);
  std::copy_n(
      animation.presentation_root_z_axis, 3,
      header.root_z_axis);
  auto bytes =
      std::make_shared<std::vector<uint8_t>>();
  bytes->reserve(
      sizeof(header) + recipe.size() +
      bindings.size() *
          (sizeof(NetworkRecipeAppearanceBinding) + 64));
  AppendAppearancePod(*bytes, header);
  bytes->insert(
      bytes->end(), recipe.begin(), recipe.end());
  for (const BindingBuild& binding : bindings) {
    AppendAppearancePod(*bytes, binding.wire);
    for (uint16_t remap : binding.remap) {
      AppendAppearancePod(*bytes, remap);
    }
  }
  cached_blob.identity = identity;
  cached_blob.bytes = std::move(bytes);
  REXLOG_INFO(
      "multiplayer: built recipe appearance id={:016X} "
      "recipe_bytes={} wire_bytes={} pieces={} matched={}",
      cached_blob.identity, recipe.size(),
      cached_blob.bytes->size(), bindings.size(),
      matched_pieces);
  return cached_blob;
}

multiplayer::AppearanceBlob BuildLocalAppearanceBlob(
    const NativeGuestOutputRenderContext& context,
    const std::vector<const DrawItem*>& items,
    const multiplayer::AnimationPose& animation) {
  const multiplayer::AppearanceBlob recipe =
      BuildLocalRecipeAppearanceBlob(items, animation);
  if (recipe.identity != 0 && recipe.bytes != nullptr) {
    return recipe;
  }
  struct ObservedAppearancePiece {
    DrawItem item;
    uint64_t identity = 0;
    std::chrono::steady_clock::time_point last_seen{};
  };
  static uint64_t cached_identity = 0;
  static multiplayer::AppearanceBlob cached;
  static std::vector<uint64_t> cached_piece_identities;
  static uint64_t observed_identity = 0;
  static std::chrono::steady_clock::time_point
      observed_since{};
  static std::unordered_map<uint32_t, ObservedAppearancePiece>
      observed_pieces;
  static uint32_t observed_presentation = 0;
  static size_t observed_ready_count = 0;
  static size_t logged_observed_count = 0;
  static size_t logged_ready_count = 0;
  uint8_t* base =
      g_guest_base.load(std::memory_order_relaxed);
  if (base == nullptr || items.empty() ||
      !animation.presentation_root_valid) {
    return {};
  }

  // Appearance identity must describe the equipped assets, not their live
  // animation payload. ROPA garments rewrite their vertex buffer every
  // simulation frame, so DrawItem::fingerprint is intentionally unstable
  // for them. Including it here rebuilt and restarted a multi-megabyte
  // appearance transfer every render frame after changing shirts.
  //
  // Build an order-independent signature from stable mesh/material/layout
  // identity instead. Mesh and texture object identity detect equipped
  // asset replacement without importing renderer cache churn into the
  // network protocol.
  const auto piece_identity_for =
      [](const DrawItem& item) {
    uint64_t piece_identity = 1469598103934665603ull;
    const auto mix = [&piece_identity](
                         const auto& value) {
      piece_identity = AppearanceHashBytes(
          piece_identity, &value, sizeof(value));
    };
    mix(item.mesh);
    mix(item.vb_bytes);
    mix(item.ib_count);
    mix(item.stride);
    mix(item.pos_fmt);
    mix(item.uv_fmt);
    mix(item.uv2_fmt);
    mix(item.char_family);
    const uint32_t flags =
        (item.skinned ? kNetworkPieceSkinned : 0u) |
        (item.hair ? kNetworkPieceHair : 0u) |
        (item.ropa ? kNetworkPieceRopa : 0u) |
        (item.char_alpha ? kNetworkPieceCharAlpha : 0u);
    mix(flags);
    const uint32_t texture_objects[4] = {
        item.diffuse_tex, item.water_normal,
        item.hair_alpha_tex, item.spec_tex};
    for (uint32_t texture_object : texture_objects) {
      // The texture object is the equipped material binding. Fetch-word
      // keys and payload fingerprints change when the texture streamer
      // promotes/demotes mip levels; treating that as an outfit change
      // would resend the same clothing while skating around the map.
      mix(texture_object);
    }
    piece_identity = AppearanceHashBytes(
        piece_identity, item.tint, sizeof(item.tint));
    if (!item.draws.empty()) {
      piece_identity = AppearanceHashBytes(
          piece_identity, item.draws.data(),
          item.draws.size() * sizeof(DrawEntry));
    }
    return piece_identity;
  };

  const auto now = std::chrono::steady_clock::now();
  const uint32_t local_presentation =
      mechanics_sandbox::LocalPresentationCandidate();
  if (local_presentation != 0 &&
      local_presentation != observed_presentation) {
    observed_pieces.clear();
    observed_identity = 0;
    observed_since = now;
    observed_ready_count = 0;
    observed_presentation = local_presentation;
  }
  bool observed_changed = false;
  for (const DrawItem* item : items) {
    if (item == nullptr || item->mesh == 0) {
      continue;
    }
    const uint64_t piece_identity =
        piece_identity_for(*item);
    auto [found, inserted] = observed_pieces.try_emplace(
        item->mesh);
    ObservedAppearancePiece& observed = found->second;
    if (inserted || observed.identity != piece_identity) {
      observed_changed = true;
    }
    // Keep the assembled slot's transform/material payload current without
    // treating animation-time values as an outfit edit. The first version
    // retained the first startup DrawItem forever, which could freeze an
    // identity ROPA world matrix on one client and a live matrix on another.
    observed.item = *item;
    observed.identity = piece_identity;
    observed.last_seen = now;
  }
  if (observed_changed) {
    observed_since = now;
  }

  // A CAC character is submitted in several render passes. Hair, ROPA
  // cloth and some body attachments do not necessarily occur in the same
  // frame as the base body. Publishing a single-frame subset is what made
  // remote shirts/heads disappear, then reappear detached after an outfit
  // edit. Assemble one item per stable mesh slot across frames and wait
  // until every observed slot has decoded geometry before serializing it.
  std::vector<const DrawItem*> capture_items;
  std::vector<uint64_t> piece_identities;
  capture_items.reserve(observed_pieces.size());
  piece_identities.reserve(observed_pieces.size());
  size_t ready_pieces = 0;
  for (auto& [mesh_key, observed] : observed_pieces) {
    (void)mesh_key;
    const auto mesh = g_r.meshes.find(observed.item.mesh);
    if (mesh == g_r.meshes.end() ||
        mesh->second.raytracing_verts.empty() ||
        mesh->second.raytracing_indices.empty()) {
      continue;
    }
    ++ready_pieces;
    capture_items.push_back(&observed.item);
    piece_identities.push_back(observed.identity);
  }
  if (ready_pieces != observed_ready_count) {
    observed_ready_count = ready_pieces;
    observed_since = now;
  }
  if (logged_observed_count != observed_pieces.size() ||
      logged_ready_count != ready_pieces) {
    logged_observed_count = observed_pieces.size();
    logged_ready_count = ready_pieces;
    REXLOG_INFO(
        "multiplayer: appearance capture assembled slots={} ready={} "
        "cached={}",
        observed_pieces.size(), ready_pieces,
        cached.bytes != nullptr ? cached_piece_identities.size() : 0);
  }
  if (observed_pieces.empty() ||
      ready_pieces != observed_pieces.size()) {
    return cached;
  }
  if (ready_pieces == 0) {
    return cached;
  }
  std::sort(
      piece_identities.begin(), piece_identities.end());
  uint64_t identity = 1469598103934665603ull;
  for (uint64_t piece_identity : piece_identities) {
    identity = AppearanceHashBytes(
        identity, &piece_identity,
        sizeof(piece_identity));
  }
  identity = AppearanceHashBytes(
      identity, &ready_pieces, sizeof(ready_pieces));
  if (identity == cached_identity &&
      cached.bytes != nullptr) {
    return cached;
  }
  if (identity != observed_identity) {
    observed_identity = identity;
    observed_since = now;
    return cached;
  }
  // Outfit changes are assembled over several frames and briefly publish
  // partial piece sets. Wait for one stable set before doing any texture
  // decode or replacing the currently advertised snapshot. Receivers keep
  // displaying the previous complete outfit during this debounce window.
  // A candidate that is only a subset of the currently advertised outfit
  // is normally a capture gap (loading camera, missing shadow/main pass),
  // not somebody unequipping clothes. Hold it substantially longer. A set
  // containing a genuinely new mesh/material identity is an actual outfit
  // edit and can replace the cache promptly.
  const bool candidate_is_subset =
      !cached_piece_identities.empty() &&
      piece_identities.size() <
          cached_piece_identities.size() &&
      std::includes(
          cached_piece_identities.begin(),
          cached_piece_identities.end(),
          piece_identities.begin(),
          piece_identities.end());
  const auto appearance_settle_time =
      candidate_is_subset
          ? std::chrono::seconds(20)
          : std::chrono::seconds(5);
  if (observed_since ==
          std::chrono::steady_clock::time_point{} ||
      now - observed_since < appearance_settle_time) {
    return cached;
  }

  struct PieceBuild {
    const DrawItem* item = nullptr;
    const MeshBuffers* mesh = nullptr;
    native_palette::CanonicalRigSample rig;
    multiplayer_assets::BindMesh bind_mesh;
    bool use_bind_mesh = false;
    uint16_t textures[4] = {
        0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu};
  };
  std::vector<PieceBuild> pieces;
  std::vector<CapturedAppearanceTexture> textures;
  std::unordered_map<uint64_t, uint16_t> texture_by_key;
  const auto texture_index =
      [&](uint32_t texture_object) -> uint16_t {
        if (texture_object == 0) {
          return 0xFFFFu;
        }
        uint32_t words[6] = {};
        if (!ReadStableTexWords(
                base, texture_object, words)) {
          return 0xFFFFu;
        }
        const uint64_t key = FetchWordsKey(words);
        const auto existing = texture_by_key.find(key);
        if (existing != texture_by_key.end()) {
          return existing->second;
        }
        if (textures.size() >= 255) {
          return 0xFFFFu;
        }
        CapturedAppearanceTexture captured;
        if (!CaptureAppearanceTexture(
                context, base, texture_object,
                captured)) {
          return 0xFFFFu;
        }
        const uint16_t index =
            static_cast<uint16_t>(textures.size());
        texture_by_key.emplace(key, index);
        textures.push_back(std::move(captured));
        return index;
      };

  for (const DrawItem* item : capture_items) {
    if (item == nullptr) {
      continue;
    }
    const auto mesh = g_r.meshes.find(item->mesh);
    if (mesh == g_r.meshes.end() ||
        mesh->second.raytracing_verts.empty() ||
        mesh->second.raytracing_indices.empty()) {
      continue;
    }
    PieceBuild piece;
    piece.item = item;
    piece.mesh = &mesh->second;
    native_palette::LookupCanonicalRig(
        item->ctx, item->mesh, piece.rig);
    if (item->ropa) {
      const uint64_t topology_hash = AppearanceHashBytes(
          1469598103934665603ull,
          mesh->second.raytracing_indices.data(),
          mesh->second.raytracing_indices.size() *
              sizeof(uint16_t));
      piece.use_bind_mesh =
          multiplayer_assets::ResolveRopaBindMesh(
              item->char_family,
              static_cast<uint32_t>(
                  mesh->second.raytracing_verts.size() / 14),
              static_cast<uint32_t>(
                  mesh->second.raytracing_indices.size()),
              topology_hash, piece.bind_mesh);
      if (piece.use_bind_mesh) {
        // UpdateBoneTransforms publishes the exact live remap used by the
        // loaded CAC model. Prefer that verified runtime table whenever its
        // palette shape matches the resolved RX2. The RX2 name-derived map
        // is only a fallback for CPU-only ROPA pieces (notably some hair)
        // that never expose a live palette. Overwriting a valid runtime
        // shirt remap here attached its bind vertices to the wrong canonical
        // rows and collapsed the garment around the presentation root.
        const bool have_exact_runtime_remap =
            piece.rig.palette_to_canonical.size() ==
            piece.bind_mesh.palette_to_canonical.size();
        size_t remap_differences = 0;
        if (have_exact_runtime_remap) {
          for (size_t index = 0;
               index < piece.rig.palette_to_canonical.size();
               ++index) {
            remap_differences +=
                piece.rig.palette_to_canonical[index] !=
                        piece.bind_mesh.palette_to_canonical[index]
                    ? 1
                    : 0;
          }
        } else {
          piece.rig.palette_to_canonical =
              piece.bind_mesh.palette_to_canonical;
        }
        REXLOG_INFO(
            "multiplayer: ROPA palette mesh={:08X} source={} "
            "bones={} name_map_differences={}",
            item->mesh,
            have_exact_runtime_remap ? "runtime" : "rx2-names",
            piece.rig.palette_to_canonical.size(),
            have_exact_runtime_remap ? remap_differences : 0);
      }
    }
    piece.textures[0] = texture_index(item->diffuse_tex);
    piece.textures[1] = texture_index(item->water_normal);
    piece.textures[2] = texture_index(item->hair_alpha_tex);
    piece.textures[3] = texture_index(item->spec_tex);
    pieces.push_back(std::move(piece));
  }
  if (pieces.empty()) {
    return cached;
  }
  const multiplayer::AnimationTrack* canonical_track = nullptr;
  for (const multiplayer::AnimationTrack& track :
       animation.tracks) {
    if (track.mesh_key ==
            multiplayer::kCanonicalSkeletonTrackKey &&
        !track.bone_rows.empty() &&
        track.bone_rows.size() % 12 == 0) {
      canonical_track = &track;
      break;
    }
  }
  for (const PieceBuild& piece : pieces) {
    if (!piece.use_bind_mesh) {
      continue;
    }
    const AppearanceWeightedRows weighted =
        multiplayer::render_cache::FindWeightedPaletteRows(
            piece.bind_mesh.vertices,
            piece.rig.palette_to_canonical.size());
    std::string mapping;
    for (std::size_t palette_row = 0;
         palette_row < weighted.used.size(); ++palette_row) {
      if (!weighted.used[palette_row]) {
        continue;
      }
      if (!mapping.empty()) {
        mapping += ",";
      }
      mapping += std::to_string(palette_row);
      mapping += "->";
      mapping +=
          palette_row <
                  piece.rig.palette_to_canonical.size()
              ? std::to_string(
                    piece.rig
                        .palette_to_canonical[palette_row])
              : "x";
    }
    REXLOG_INFO(
        "multiplayer-bind-contract: mesh={:08X} "
        "asset={:016X} palette={} weighted={} invalid={} map=[{}]",
        piece.item->mesh, piece.bind_mesh.asset_id,
        piece.rig.palette_to_canonical.size(),
        weighted.count, weighted.invalid_influences, mapping);
    if (canonical_track == nullptr ||
        piece.mesh->raytracing_verts.size() !=
            piece.bind_mesh.vertices.size()) {
      continue;
    }
    const std::size_t canonical_bones =
        canonical_track->bone_rows.size() / 12;
    double raw_error = 0.0;
    double world_error = 0.0;
    double transpose_raw_error = 0.0;
    double transpose_world_error = 0.0;
    double raw_delta[3] = {};
    double world_delta[3] = {};
    double transpose_raw_delta[3] = {};
    double transpose_world_delta[3] = {};
    double uv0_error = 0.0;
    double uv1_error = 0.0;
    double uv0_max = 0.0;
    double uv1_max = 0.0;
    std::size_t compared = 0;
    std::size_t invalid = 0;
    for (std::size_t vertex = 0;
         vertex + 14 <= piece.bind_mesh.vertices.size();
         vertex += 14) {
      const float* bind =
          piece.bind_mesh.vertices.data() + vertex;
      const float* live =
          piece.mesh->raytracing_verts.data() + vertex;
      for (std::size_t component = 0; component < 2;
           ++component) {
        const double uv0_delta =
            double(bind[3 + component]) -
            live[3 + component];
        const double uv1_delta =
            double(bind[5 + component]) -
            live[5 + component];
        uv0_error += uv0_delta * uv0_delta;
        uv1_error += uv1_delta * uv1_delta;
        uv0_max = std::max(
            uv0_max, std::abs(uv0_delta));
        uv1_max = std::max(
            uv1_max, std::abs(uv1_delta));
      }
      std::uint32_t packed_weights = 0;
      std::uint32_t packed_indices = 0;
      std::memcpy(
          &packed_weights, bind + 7,
          sizeof(packed_weights));
      std::memcpy(
          &packed_indices, bind + 8,
          sizeof(packed_indices));
      float predicted[3] = {};
      float predicted_transpose[3] = {};
      float weight_sum = 0.0f;
      bool vertex_valid = true;
      for (std::size_t influence = 0; influence < 4;
           ++influence) {
        const std::uint8_t weight_byte =
            static_cast<std::uint8_t>(
                packed_weights >> (influence * 8));
        if (weight_byte == 0) {
          continue;
        }
        const std::uint8_t palette_row =
            static_cast<std::uint8_t>(
                packed_indices >> (influence * 8));
        if (palette_row >=
            piece.rig.palette_to_canonical.size()) {
          vertex_valid = false;
          ++invalid;
          break;
        }
        const std::size_t canonical_bone =
            piece.rig
                .palette_to_canonical[palette_row];
        if (canonical_bone >= canonical_bones) {
          vertex_valid = false;
          ++invalid;
          break;
        }
        const float weight =
            static_cast<float>(weight_byte) / 255.0f;
        const float* bone =
            canonical_track->bone_rows.data() +
            canonical_bone * 12;
        for (std::size_t row = 0; row < 3; ++row) {
          predicted[row] +=
              weight *
              (bind[0] * bone[row * 4 + 0] +
               bind[1] * bone[row * 4 + 1] +
               bind[2] * bone[row * 4 + 2] +
               bone[row * 4 + 3]);
          predicted_transpose[row] +=
              weight *
              (bind[0] * bone[0 * 4 + row] +
               bind[1] * bone[1 * 4 + row] +
               bind[2] * bone[2 * 4 + row] +
               bone[row * 4 + 3]);
        }
        weight_sum += weight;
      }
      if (!vertex_valid || weight_sum <= 1.0e-6f) {
        continue;
      }
      for (std::size_t component = 0; component < 3;
           ++component) {
        predicted[component] /= weight_sum;
        predicted_transpose[component] /= weight_sum;
      }
      float live_world[3] = {};
      for (std::size_t component = 0; component < 3;
           ++component) {
        live_world[component] =
            live[0] * piece.item->world[component] +
            live[1] * piece.item->world[4 + component] +
            live[2] * piece.item->world[8 + component] +
            piece.item->world[12 + component];
      }
      for (std::size_t component = 0; component < 3;
           ++component) {
        const double raw =
            double(predicted[component]) - live[component];
        const double world =
            double(predicted[component]) -
            live_world[component];
        const double transpose_raw =
            double(predicted_transpose[component]) -
            live[component];
        const double transpose_world =
            double(predicted_transpose[component]) -
            live_world[component];
        raw_error += raw * raw;
        world_error += world * world;
        transpose_raw_error +=
            transpose_raw * transpose_raw;
        transpose_world_error +=
            transpose_world * transpose_world;
        raw_delta[component] += raw;
        world_delta[component] += world;
        transpose_raw_delta[component] += transpose_raw;
        transpose_world_delta[component] +=
            transpose_world;
      }
      ++compared;
    }
    const auto rms =
        [compared](double error) {
          return compared == 0
                     ? -1.0
                     : std::sqrt(
                           error /
                           (double(compared) * 3.0));
        };
    const auto centered_rms =
        [compared](double error,
                   const double delta[3]) {
          if (compared == 0) {
            return -1.0;
          }
          const double inverse =
              1.0 / double(compared);
          double centered = error;
          for (std::size_t component = 0;
               component < 3; ++component) {
            centered -=
                delta[component] * delta[component] *
                inverse;
          }
          return std::sqrt(
              std::max(centered, 0.0) /
              (double(compared) * 3.0));
        };
    REXLOG_INFO(
        "multiplayer-bind-invariant: mesh={:08X} asset={:016X} "
        "verts={} invalid={} "
        "row_raw={:.5f}/{:.5f} row_world={:.5f}/{:.5f} "
        "transpose_raw={:.5f}/{:.5f} "
        "transpose_world={:.5f}/{:.5f} "
        "uv0={:.6f}/{:.6f} uv1={:.6f}/{:.6f}",
        piece.item->mesh, piece.bind_mesh.asset_id,
        compared, invalid,
        rms(raw_error),
        centered_rms(raw_error, raw_delta),
        rms(world_error),
        centered_rms(world_error, world_delta),
        rms(transpose_raw_error),
        centered_rms(
            transpose_raw_error, transpose_raw_delta),
        rms(transpose_world_error),
        centered_rms(
            transpose_world_error,
            transpose_world_delta),
        compared == 0
            ? -1.0
            : std::sqrt(
                  uv0_error /
                  (double(compared) * 2.0)),
        uv0_max,
        compared == 0
            ? -1.0
            : std::sqrt(
                  uv1_error /
                  (double(compared) * 2.0)),
        uv1_max);
  }
  NetworkAppearanceHeader header;
  std::copy_n(
      animation.presentation_root_position, 3,
      header.root_position);
  std::copy_n(
      animation.presentation_root_x_axis, 3,
      header.root_x_axis);
  std::copy_n(
      animation.presentation_root_z_axis, 3,
      header.root_z_axis);
  header.texture_count =
      static_cast<uint16_t>(textures.size());
  header.piece_count =
      static_cast<uint16_t>(pieces.size());
  auto bytes =
      std::make_shared<std::vector<uint8_t>>();
  bytes->reserve(1024 * 1024);
  AppendAppearancePod(*bytes, header);
  for (const CapturedAppearanceTexture& texture :
       textures) {
    AppendAppearancePod(*bytes, texture.header);
    bytes->insert(
        bytes->end(), texture.payload.begin(),
        texture.payload.end());
  }
  for (const PieceBuild& piece : pieces) {
    const std::vector<float>& appearance_vertices =
        piece.use_bind_mesh
            ? piece.bind_mesh.vertices
            : piece.mesh->raytracing_verts;
    const std::vector<uint16_t>& appearance_indices =
        piece.use_bind_mesh
            ? piece.bind_mesh.indices
            : piece.mesh->raytracing_indices;
    NetworkAppearancePiece wire;
    wire.track_key = piece.item->mesh;
    wire.fingerprint = piece.item->fingerprint;
    wire.flags =
        ((piece.item->skinned || piece.use_bind_mesh)
             ? kNetworkPieceSkinned
             : 0u) |
        (piece.item->hair ? kNetworkPieceHair : 0u) |
        ((piece.item->ropa && !piece.use_bind_mesh)
             ? kNetworkPieceRopa
             : 0u) |
        (piece.item->char_alpha ? kNetworkPieceCharAlpha : 0u) |
        (piece.item->shadow_caster ? kNetworkPieceShadow : 0u);
    wire.char_family = piece.item->char_family;
    wire.bone_count = static_cast<uint16_t>(
        std::min<size_t>(
            piece.use_bind_mesh
                ? piece.rig.palette_to_canonical.size()
                : piece.item->bones.size() / 12,
            256));
    wire.draw_count = static_cast<uint16_t>(
        std::min<size_t>(
            piece.item->draws.size(), 256));
    wire.remap_count = static_cast<uint16_t>(
        std::min<size_t>(
            piece.rig.palette_to_canonical.size(), 256));
    wire.vertex_count = static_cast<uint32_t>(
        appearance_vertices.size() / 14);
    wire.index_count = static_cast<uint32_t>(
        appearance_indices.size());
    wire.diffuse_texture = piece.textures[0];
    wire.normal_texture = piece.textures[1];
    wire.alpha_texture = piece.textures[2];
    wire.spec_texture = piece.textures[3];
    if (piece.item->ropa && !piece.use_bind_mesh) {
      const uint64_t topology_hash = AppearanceHashBytes(
          1469598103934665603ull,
          piece.mesh->raytracing_indices.data(),
          piece.mesh->raytracing_indices.size() *
              sizeof(uint16_t));
      REXLOG_INFO(
          "multiplayer: ropa asset signature mesh={:08X} fam={} "
          "verts={} indices={} draws={} stride={} topology={:016X} "
          "bbox=({:.4f},{:.4f},{:.4f})..({:.4f},{:.4f},{:.4f})",
          piece.item->mesh, piece.item->char_family,
          wire.vertex_count, wire.index_count, wire.draw_count,
          piece.item->stride, topology_hash,
          piece.item->bbox_min[0], piece.item->bbox_min[1],
          piece.item->bbox_min[2], piece.item->bbox_max[0],
          piece.item->bbox_max[1], piece.item->bbox_max[2]);
    }
    if (piece.use_bind_mesh) {
      REXLOG_INFO(
          "multiplayer: bind-skinned ROPA mesh={:08X} "
          "asset={:016X} fam={} verts={} indices={} bones={}",
          piece.item->mesh, piece.bind_mesh.asset_id,
          piece.item->char_family, wire.vertex_count,
          wire.index_count, wire.bone_count);
    }
    std::copy_n(
        piece.item->char_rows, 72, wire.char_rows);
    std::copy_n(piece.item->tint, 4, wire.tint);
    if (piece.use_bind_mesh) {
      // The replicated canonical palette maps bind-model vertices directly
      // into world space. ROPA's original item.world belongs to Skate's
      // already-CPU-deformed rigid draw; applying it to the bind-skinned
      // result transforms the garment twice and leaves it detached.
      wire.world[0] = 1.0f;
      wire.world[5] = 1.0f;
      wire.world[10] = 1.0f;
      wire.world[15] = 1.0f;
    } else {
      std::copy_n(piece.item->world, 16, wire.world);
    }
    std::copy_n(
        piece.use_bind_mesh
            ? piece.bind_mesh.bbox_min
            : piece.item->bbox_min,
        3, wire.bbox_min);
    std::copy_n(
        piece.use_bind_mesh
            ? piece.bind_mesh.bbox_max
            : piece.item->bbox_max,
        3, wire.bbox_max);
    AppendAppearancePod(*bytes, wire);
    bytes->insert(
        bytes->end(),
        reinterpret_cast<const uint8_t*>(
            piece.item->draws.data()),
        reinterpret_cast<const uint8_t*>(
            piece.item->draws.data() + wire.draw_count));
    for (size_t index = 0;
         index < wire.remap_count; ++index) {
      const uint16_t mapped =
          static_cast<uint16_t>(
              std::min<size_t>(
                  piece.rig.palette_to_canonical[index],
                  0xFFFFu));
      AppendAppearancePod(*bytes, mapped);
    }
    bytes->insert(
        bytes->end(),
        reinterpret_cast<const uint8_t*>(
            appearance_vertices.data()),
        reinterpret_cast<const uint8_t*>(
            appearance_vertices.data() +
            size_t(wire.vertex_count) * 14));
    bytes->insert(
        bytes->end(),
        reinterpret_cast<const uint8_t*>(
            appearance_indices.data()),
        reinterpret_cast<const uint8_t*>(
            appearance_indices.data() +
            wire.index_count));
    if (bytes->size() > 16u * 1024u * 1024u) {
      REXLOG_WARN(
          "multiplayer: appearance snapshot exceeded 16 MiB; "
          "keeping previous appearance");
      return cached;
    }
  }
  cached_identity = identity;
  cached_piece_identities = piece_identities;
  cached.identity = identity;
  cached.bytes = std::move(bytes);
  REXLOG_INFO(
      "multiplayer: built appearance id={:016X} pieces={} "
      "textures={} bytes={}",
      identity, pieces.size(), textures.size(),
      cached.bytes->size());
  return cached;
}

struct RemoteAppearancePieceRenderCache {
  uint32_t track_key = 0;
  AppearanceWeightedRows weighted_rows;
  std::vector<uint16_t> palette_to_canonical;
  std::size_t exact_track_index =
      multiplayer::render_cache::kInvalidAnimationTrackIndex;
  bool rig_lookup_complete = false;
  bool skateboard_piece = false;
};

struct RemoteAppearanceRenderState {
  uint64_t identity = 0;
  uint32_t session = 0;
  bool installation_reported = false;
  uint32_t installation_reported_session = 0;
  float root_position[3] = {};
  float root_x_axis[3] = {1.0f, 0.0f, 0.0f};
  float root_z_axis[3] = {0.0f, 0.0f, 1.0f};
  std::vector<DrawItem> items;
  std::vector<RemoteAppearancePieceRenderCache> piece_caches;
  std::size_t canonical_track_index =
      multiplayer::render_cache::kInvalidAnimationTrackIndex;
  std::vector<uint64_t> texture_store_keys;
};

std::unordered_map<uint32_t, RemoteAppearanceRenderState>
    g_remote_appearances;

struct RemoteRecipeAppearanceBinding {
  NetworkRecipeAppearanceBinding wire;
  std::vector<uint16_t> remap;
};

struct PendingRemoteRecipeInstall {
  uint64_t identity = 0;
  uint32_t session = 0;
  uint8_t resource_slot = 0;
  std::shared_ptr<const multiplayer_assets::RecipeAppearance>
      resolved;
  std::unordered_map<uint64_t, RemoteRecipeAppearanceBinding>
      bindings;
  std::vector<uint64_t> texture_ids;
  std::unordered_map<uint64_t, uint32_t> texture_objects;
  std::size_t texture_index = 0;
  std::size_t piece_index = 0;
  RemoteAppearanceRenderState staged;
  std::chrono::steady_clock::time_point started;
  uint64_t last_progress_frame =
      std::numeric_limits<uint64_t>::max();
  uint32_t progress_frames = 0;
  uint32_t operations = 0;
  double operation_ms_sum = 0.0;
  double operation_ms_max = 0.0;
  std::size_t scanned_vertices = 0;
  std::size_t weighted_rows = 0;
  std::size_t resolved_rigs = 0;
};

std::unordered_map<uint32_t, PendingRemoteRecipeInstall>
    g_pending_remote_recipe_installs;

struct RemoteAppearanceInstallFrameBudget {
  uint64_t frame = std::numeric_limits<uint64_t>::max();
  uint32_t operations = 0;
  std::chrono::steady_clock::time_point started;
};

RemoteAppearanceInstallFrameBudget
    g_remote_appearance_install_budget;

struct RemoteAppearanceResourceAudit {
  std::size_t installed_roles = 0;
  std::size_t pending_roles = 0;
  std::size_t installed_meshes = 0;
  std::size_t pending_meshes = 0;
  std::size_t installed_textures = 0;
  std::size_t pending_textures = 0;
  std::size_t missing_meshes = 0;
  std::size_t missing_textures = 0;
  std::size_t duplicate_mesh_owners = 0;
  std::size_t duplicate_texture_owners = 0;
};

RemoteAppearanceResourceAudit
CollectRemoteAppearanceResourceAudit() {
  RemoteAppearanceResourceAudit audit;
  std::unordered_set<uint32_t> owned_meshes;
  std::unordered_set<uint64_t> owned_textures;

  const auto observe_state =
      [&audit, &owned_meshes, &owned_textures](
          const RemoteAppearanceRenderState& state,
          bool pending) {
        if (state.identity == 0 && state.items.empty() &&
            state.texture_store_keys.empty()) {
          return;
        }
        if (pending) {
          ++audit.pending_roles;
          audit.pending_meshes += state.items.size();
          audit.pending_textures +=
              state.texture_store_keys.size();
        } else {
          ++audit.installed_roles;
          audit.installed_meshes += state.items.size();
          audit.installed_textures +=
              state.texture_store_keys.size();
        }
        for (const DrawItem& item : state.items) {
          if (!owned_meshes.insert(item.mesh).second) {
            ++audit.duplicate_mesh_owners;
          }
          if (!g_r.meshes.contains(item.mesh)) {
            ++audit.missing_meshes;
          }
        }
        for (uint64_t store_key :
             state.texture_store_keys) {
          if (!owned_textures.insert(store_key).second) {
            ++audit.duplicate_texture_owners;
          }
          if (!g_r.tex_store.contains(store_key)) {
            ++audit.missing_textures;
          }
        }
      };

  for (const auto& [role, state] : g_remote_appearances) {
    static_cast<void>(role);
    observe_state(state, false);
  }
  for (const auto& [role, transaction] :
       g_pending_remote_recipe_installs) {
    static_cast<void>(role);
    observe_state(transaction.staged, true);
  }

  return audit;
}

void LogRemoteAppearanceResourceAudit(
    const char* event, uint32_t role) {
  const RemoteAppearanceResourceAudit audit =
      CollectRemoteAppearanceResourceAudit();
  const std::size_t faults =
      audit.missing_meshes + audit.missing_textures +
      audit.duplicate_mesh_owners +
      audit.duplicate_texture_owners;
  if (faults == 0) {
    REXLOG_INFO(
        "multiplayer-appearance-resources: event={} role={} "
        "installed_roles={} pending_roles={} "
        "installed_meshes={} pending_meshes={} "
        "installed_textures={} pending_textures={} faults=0",
        event, role, audit.installed_roles,
        audit.pending_roles, audit.installed_meshes,
        audit.pending_meshes, audit.installed_textures,
        audit.pending_textures);
    return;
  }
  REXLOG_WARN(
      "multiplayer-appearance-resources: event={} role={} "
      "installed_roles={} pending_roles={} "
      "installed_meshes={} pending_meshes={} "
      "installed_textures={} pending_textures={} faults={} "
      "missing_meshes={} missing_textures={} "
      "duplicate_meshes={} duplicate_textures={}",
      event, role, audit.installed_roles,
      audit.pending_roles, audit.installed_meshes,
      audit.pending_meshes, audit.installed_textures,
      audit.pending_textures, faults,
      audit.missing_meshes, audit.missing_textures,
      audit.duplicate_mesh_owners,
      audit.duplicate_texture_owners);
}

bool AcquireRemoteAppearanceInstallOperation(uint64_t frame) {
  RemoteAppearanceInstallFrameBudget& budget =
      g_remote_appearance_install_budget;
  if (budget.frame != frame) {
    budget.frame = frame;
    budget.operations = 0;
    budget.started = std::chrono::steady_clock::now();
  }
  const uint32_t max_operations =
      static_cast<uint32_t>(std::clamp(
          REXCVAR_GET(
              skate3_multiplayer_appearance_install_ops_per_frame),
          1, 16));
  if (budget.operations >= max_operations) {
    return false;
  }
  if (budget.operations != 0) {
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - budget.started)
            .count();
    const double max_ms = std::clamp(
        REXCVAR_GET(
            skate3_multiplayer_appearance_install_budget_ms),
        0.25, 16.0);
    if (elapsed_ms >= max_ms) {
      return false;
    }
  }
  ++budget.operations;
  return true;
}

bool ResolveRemoteAppearanceRigCache(
    const DrawItem& item,
    RemoteAppearancePieceRenderCache& cache) {
  if (cache.rig_lookup_complete) {
    return !cache.palette_to_canonical.empty();
  }
  if (!item.skinned || item.bones.empty()) {
    cache.rig_lookup_complete = true;
    return false;
  }
  if (!item.multiplayer_palette_to_canonical.empty()) {
    cache.palette_to_canonical.assign(
        item.multiplayer_palette_to_canonical.begin(),
        item.multiplayer_palette_to_canonical.end());
    cache.rig_lookup_complete = true;
  } else {
    native_palette::CanonicalRigSample rig;
    if (native_palette::LookupCanonicalRig(
            item.ctx, item.mesh, rig) &&
        !rig.palette_to_canonical.empty()) {
      cache.palette_to_canonical =
          std::move(rig.palette_to_canonical);
      cache.rig_lookup_complete = true;
    }
  }
  if (cache.rig_lookup_complete) {
    cache.skateboard_piece =
        std::any_of(
            cache.palette_to_canonical.begin(),
            cache.palette_to_canonical.end(),
            [](std::size_t canonical_bone) {
              return canonical_bone >= 25 &&
                     canonical_bone <= 31;
            });
  }
  return !cache.palette_to_canonical.empty();
}

void BuildRemoteAppearancePieceRenderCache(
    const DrawItem& item,
    RemoteAppearancePieceRenderCache& cache,
    std::size_t& scanned_vertices,
    std::size_t& weighted_rows,
    std::size_t& resolved_rigs) {
  cache.track_key =
      item.multiplayer_track_key != 0
          ? item.multiplayer_track_key
          : item.mesh;
  const std::size_t palette_bones = item.bones.size() / 12;
  const auto installed_mesh = g_r.meshes.find(item.mesh);
  if (item.skinned && palette_bones != 0 &&
      installed_mesh != g_r.meshes.end()) {
    cache.weighted_rows =
        multiplayer::render_cache::FindWeightedPaletteRows(
            installed_mesh->second.raytracing_verts,
            palette_bones);
    scanned_vertices +=
        installed_mesh->second.raytracing_verts.size() / 14;
    weighted_rows += cache.weighted_rows.count;
  }
  if (ResolveRemoteAppearanceRigCache(item, cache)) {
    ++resolved_rigs;
  }
}

void BuildRemoteAppearanceRenderCaches(
    uint32_t role, RemoteAppearanceRenderState& state) {
  state.piece_caches.clear();
  state.piece_caches.reserve(state.items.size());
  state.canonical_track_index =
      multiplayer::render_cache::kInvalidAnimationTrackIndex;
  std::size_t scanned_vertices = 0;
  std::size_t weighted_rows = 0;
  std::size_t resolved_rigs = 0;
  for (const DrawItem& item : state.items) {
    RemoteAppearancePieceRenderCache cache;
    BuildRemoteAppearancePieceRenderCache(
        item, cache, scanned_vertices, weighted_rows,
        resolved_rigs);
    state.piece_caches.push_back(std::move(cache));
  }
  REXLOG_INFO(
      "multiplayer-render-cache: prepared role={} session={} "
      "appearance={:016X} pieces={} vertices={} weighted_rows={} "
      "resolved_rigs={}",
      role, state.session, state.identity,
      state.piece_caches.size(), scanned_vertices,
      weighted_rows, resolved_rigs);
}

struct RemoteVisualTelemetryState {
  uint32_t session = 0;
  bool real_skater = false;
  bool initialized = false;
};

std::unordered_map<uint32_t, RemoteVisualTelemetryState>
    g_remote_visual_telemetry;

struct RemoteRenderMotionTelemetry {
  uint32_t session = 0;
  multiplayer::motion::Window motion;
  multiplayer::pose_cadence::Window interpolated_pose;
  multiplayer::pose_cadence::Window applied_palette;
};

std::unordered_map<uint32_t, RemoteRenderMotionTelemetry>
    g_remote_render_motion;

multiplayer::pose_cadence::Window g_local_capture_cadence;

std::uint64_t PoseCadenceNowMicroseconds() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::uint64_t HashAnimationPose(
    const multiplayer::AnimationPose& animation) {
  std::uint64_t hash = 1469598103934665603ull;
  hash = AppearanceHashBytes(
      hash, &animation.root_bone, sizeof(animation.root_bone));
  hash = AppearanceHashBytes(
      hash, animation.root_position,
      sizeof(animation.root_position));
  hash = AppearanceHashBytes(
      hash, &animation.presentation_root_valid,
      sizeof(animation.presentation_root_valid));
  hash = AppearanceHashBytes(
      hash, animation.presentation_root_position,
      sizeof(animation.presentation_root_position));
  hash = AppearanceHashBytes(
      hash, animation.presentation_root_x_axis,
      sizeof(animation.presentation_root_x_axis));
  hash = AppearanceHashBytes(
      hash, animation.presentation_root_z_axis,
      sizeof(animation.presentation_root_z_axis));
  for (const multiplayer::AnimationTrack& track :
       animation.tracks) {
    hash = AppearanceHashBytes(
        hash, &track.mesh_key, sizeof(track.mesh_key));
    const std::size_t row_count = track.bone_rows.size();
    hash = AppearanceHashBytes(
        hash, &row_count, sizeof(row_count));
    if (!track.bone_rows.empty()) {
      hash = AppearanceHashBytes(
          hash, track.bone_rows.data(),
          track.bone_rows.size() * sizeof(float));
    }
  }
  return hash;
}

void RecordLocalCaptureCadence(
    const multiplayer::AnimationPose& animation) {
  if (animation.tracks.empty()) {
    return;
  }
  g_local_capture_cadence.Record(
      PoseCadenceNowMicroseconds(), animation.sequence,
      HashAnimationPose(animation));
}

void RecordRemoteAppliedPalette(
    uint32_t role, uint32_t session, uint32_t sequence,
    std::uint64_t palette_hash, std::size_t palette_bones) {
  if (palette_bones == 0) {
    return;
  }
  RemoteRenderMotionTelemetry& remote =
      g_remote_render_motion[role];
  if (remote.session != session) {
    remote = {};
    remote.session = session;
  }
  remote.applied_palette.Record(
      PoseCadenceNowMicroseconds(), sequence, palette_hash);
}

void RecordRemotePresentationHandoff(
    const multiplayer::RemotePresentationFrame& presentation) {
  struct HandoffTelemetry {
    std::chrono::steady_clock::time_point last_log{};
    std::uint64_t last_sequence = 0;
    std::uint64_t render_calls = 0;
    std::uint64_t unique_presentations = 0;
    std::uint64_t repeated_presentations = 0;
    std::uint64_t skipped_presentations = 0;
  };
  static HandoffTelemetry telemetry;
  const auto now = std::chrono::steady_clock::now();
  ++telemetry.render_calls;
  if (presentation.sequence == telemetry.last_sequence) {
    ++telemetry.repeated_presentations;
  } else {
    if (telemetry.last_sequence != 0 &&
        presentation.sequence > telemetry.last_sequence + 1) {
      telemetry.skipped_presentations +=
          presentation.sequence - telemetry.last_sequence - 1;
    }
    telemetry.last_sequence = presentation.sequence;
    ++telemetry.unique_presentations;
  }
  if (telemetry.last_log ==
      std::chrono::steady_clock::time_point{}) {
    telemetry.last_log = now;
    return;
  }
  if (now - telemetry.last_log < std::chrono::seconds(5)) {
    return;
  }

  const double seconds =
      std::chrono::duration<double>(
          now - telemetry.last_log)
          .count();
  REXLOG_INFO(
      "multiplayer-render-handoff: calls={} unique={} repeated={} "
      "skipped={} rates={:.1f}/{:.1f}Hz",
      telemetry.render_calls,
      telemetry.unique_presentations,
      telemetry.repeated_presentations,
      telemetry.skipped_presentations,
      static_cast<double>(telemetry.render_calls) / seconds,
      static_cast<double>(telemetry.unique_presentations) / seconds);
  const multiplayer::pose_cadence::Snapshot local_capture =
      g_local_capture_cadence.ReadAndReset();
  REXLOG_INFO(
      "multiplayer-pose-cadence: stage=capture role=local "
      "samples={} changes={} repeats={} seq_changes={} "
      "alternations={} max_repeat={} hold={:.1f}ms",
      local_capture.samples, local_capture.changes,
      local_capture.repeats, local_capture.sequence_changes,
      local_capture.alternations,
      local_capture.maximum_repeat_run,
      local_capture.maximum_hold_ms);
  for (auto& [role, remote] : g_remote_render_motion) {
    const multiplayer::motion::Snapshot motion =
        remote.motion.ReadAndReset();
    REXLOG_INFO(
        "multiplayer-render-motion: role={} session={} "
        "n={} dt={:.2f}/{:.2f}/{:.2f}ms "
        "speed={:.3f} speed_change={:.3f}/{:.3f}",
        role, remote.session, motion.samples,
        motion.average_interval_ms,
        motion.minimum_interval_ms,
        motion.maximum_interval_ms,
        motion.average_speed,
        motion.average_speed_change,
        motion.maximum_speed_change);
    const multiplayer::pose_cadence::Snapshot interpolated =
        remote.interpolated_pose.ReadAndReset();
    REXLOG_INFO(
        "multiplayer-pose-cadence: stage=interpolated "
        "role={} session={} samples={} changes={} repeats={} "
        "seq_changes={} alternations={} max_repeat={} "
        "hold={:.1f}ms",
        role, remote.session, interpolated.samples,
        interpolated.changes, interpolated.repeats,
        interpolated.sequence_changes,
        interpolated.alternations,
        interpolated.maximum_repeat_run,
        interpolated.maximum_hold_ms);
    const multiplayer::pose_cadence::Snapshot applied =
        remote.applied_palette.ReadAndReset();
    REXLOG_INFO(
        "multiplayer-pose-cadence: stage=applied "
        "role={} session={} samples={} changes={} repeats={} "
        "seq_changes={} alternations={} max_repeat={} "
        "hold={:.1f}ms",
        role, remote.session, applied.samples,
        applied.changes, applied.repeats,
        applied.sequence_changes, applied.alternations,
        applied.maximum_repeat_run,
        applied.maximum_hold_ms);
  }
  telemetry.render_calls = 0;
  telemetry.unique_presentations = 0;
  telemetry.repeated_presentations = 0;
  telemetry.skipped_presentations = 0;
  telemetry.last_log = now;
}

void RecordRemoteRenderMotion(
    uint32_t role, uint32_t session,
    const multiplayer::AnimationPose& animation) {
  if (animation.sender_time_us == 0) {
    return;
  }
  RemoteRenderMotionTelemetry& remote =
      g_remote_render_motion[role];
  if (remote.session != session) {
    remote = {};
    remote.session = session;
  }
  remote.motion.Record(
      animation.sender_time_us, animation.root_position);
  remote.interpolated_pose.Record(
      PoseCadenceNowMicroseconds(), animation.sequence,
      HashAnimationPose(animation));
}

void ReleaseRemoteAppearanceResources(
    const NativeGuestOutputRenderContext& context,
    uint32_t role,
    const RemoteAppearanceRenderState& state,
    const char* reason) {
  std::size_t released_textures = 0;
  std::size_t released_texture_routes = 0;
  for (uint64_t store_key : state.texture_store_keys) {
    const auto texture = g_r.tex_store.find(store_key);
    if (texture != g_r.tex_store.end()) {
      RetireGuestTexture(
          texture->second,
          context.device->CurrentSubmission());
      g_r.tex_store.erase(texture);
      ++released_textures;
    }
    for (auto route = g_r.tex_routes.begin();
         route != g_r.tex_routes.end();) {
      if (route->second.key == store_key) {
        route = g_r.tex_routes.erase(route);
        ++released_texture_routes;
      } else {
        ++route;
      }
    }
  }

  std::unordered_set<uint32_t> released_mesh_keys;
  released_mesh_keys.reserve(state.items.size());
  std::size_t released_meshes = 0;
  for (const DrawItem& item : state.items) {
    if (!released_mesh_keys.insert(item.mesh).second) {
      continue;
    }
    const auto mesh = g_r.meshes.find(item.mesh);
    if (mesh == g_r.meshes.end()) {
      continue;
    }
    PoolMeshBuffer(context.device, mesh->second.vb);
    PoolMeshBuffer(context.device, mesh->second.ib);
    g_r.meshes.erase(mesh);
    ++released_meshes;
  }

  const auto mismatch =
      [](std::size_t expected, std::size_t actual) {
        return expected > actual ? expected - actual
                                 : actual - expected;
      };
  const std::size_t release_faults =
      mismatch(state.texture_store_keys.size(),
               released_textures) +
      mismatch(released_mesh_keys.size(), released_meshes) +
      mismatch(state.texture_store_keys.size(),
               released_texture_routes);
  if (release_faults == 0) {
    REXLOG_INFO(
        "multiplayer: released renderer appearance role={} "
        "session={} id={:016X} meshes={}/{} textures={}/{} "
        "routes={}/{} faults=0 reason={}",
        role, state.session, state.identity,
        released_meshes, released_mesh_keys.size(),
        released_textures, state.texture_store_keys.size(),
        released_texture_routes,
        state.texture_store_keys.size(), reason);
  } else {
    REXLOG_WARN(
        "multiplayer: released renderer appearance role={} "
        "session={} id={:016X} meshes={}/{} textures={}/{} "
        "routes={}/{} faults={} reason={}",
        role, state.session, state.identity,
        released_meshes, released_mesh_keys.size(),
        released_textures, state.texture_store_keys.size(),
        released_texture_routes,
        state.texture_store_keys.size(), release_faults,
        reason);
  }
}

void CancelPendingRemoteRecipeInstall(
    const NativeGuestOutputRenderContext& context,
    uint32_t role, const char* reason) {
  const auto pending =
      g_pending_remote_recipe_installs.find(role);
  if (pending == g_pending_remote_recipe_installs.end()) {
    return;
  }
  ReleaseRemoteAppearanceResources(
      context, role, pending->second.staged, reason);
  multiplayer_assets::ForgetRecipeAppearanceResolve(
      role, pending->second.session);
  REXLOG_INFO(
      "multiplayer-appearance-install-cancel: role={} session={} "
      "appearance={:016X} textures={}/{} pieces={}/{} reason={}",
      role, pending->second.session, pending->second.identity,
      pending->second.texture_index,
      pending->second.texture_ids.size(),
      pending->second.piece_index,
      pending->second.resolved == nullptr
          ? 0
          : pending->second.resolved->pieces.size(),
      reason);
  g_pending_remote_recipe_installs.erase(pending);
  LogRemoteAppearanceResourceAudit("cancel", role);
}

void ObserveRemoteAppearanceSession(
    const NativeGuestOutputRenderContext& context,
    uint32_t role, uint32_t session) {
  const auto pending =
      g_pending_remote_recipe_installs.find(role);
  if (pending != g_pending_remote_recipe_installs.end() &&
      multiplayer::lifecycle::RemoteAppearanceSessionChanged(
          pending->second.session, session)) {
    CancelPendingRemoteRecipeInstall(
        context, role, "pending session changed");
  }
  const auto installed = g_remote_appearances.find(role);
  if (installed == g_remote_appearances.end()) {
    return;
  }
  if (multiplayer::lifecycle::RemoteAppearanceSessionChanged(
          installed->second.session, session)) {
    if (REXCVAR_GET(
            skate3_multiplayer_async_appearance_prepare)) {
      multiplayer_assets::ForgetRecipeAppearanceResolve(
          role, installed->second.session);
    }
    ReleaseRemoteAppearanceResources(
        context, role, installed->second, "session changed");
    g_remote_appearances.erase(installed);
  g_remote_visual_telemetry.erase(role);
  g_remote_render_motion.erase(role);
    LogRemoteAppearanceResourceAudit(
        "session-release", role);
  }
}

void ReleaseRetiredRemoteAppearance(
    const NativeGuestOutputRenderContext& context,
    const multiplayer::RemotePeerRetirement& retirement) {
  if (REXCVAR_GET(
          skate3_multiplayer_async_appearance_prepare)) {
    multiplayer_assets::ForgetRecipeAppearanceResolve(
        retirement.role, retirement.session);
  }
  const auto pending =
      g_pending_remote_recipe_installs.find(retirement.role);
  if (pending != g_pending_remote_recipe_installs.end() &&
      multiplayer::lifecycle::RemoteAppearanceRetirementMatches(
          pending->second.session, retirement.session)) {
    CancelPendingRemoteRecipeInstall(
        context, retirement.role, "pending peer retired");
  }
  const auto installed =
      g_remote_appearances.find(retirement.role);
  if (installed == g_remote_appearances.end() ||
      !multiplayer::lifecycle::RemoteAppearanceRetirementMatches(
          installed->second.session, retirement.session)) {
    return;
  }
  ReleaseRemoteAppearanceResources(
      context, retirement.role, installed->second,
      "peer retired");
  g_remote_appearances.erase(installed);
  g_remote_visual_telemetry.erase(retirement.role);
  g_remote_render_motion.erase(retirement.role);
  LogRemoteAppearanceResourceAudit(
      "peer-retire", retirement.role);
}

bool InstallRemoteRecipeAppearanceIncremental(
    const NativeGuestOutputRenderContext& context,
    uint64_t frame_number, uint32_t role, uint32_t session,
    const multiplayer::AppearanceBlob& appearance) {
  if (appearance.bytes == nullptr) {
    return false;
  }
  RemoteAppearanceRenderState& installed_state =
      g_remote_appearances[role];
  if (installed_state.identity == appearance.identity &&
      installed_state.session == session &&
      !installed_state.items.empty()) {
    bool meshes_ready = true;
    for (const DrawItem& item : installed_state.items) {
      meshes_ready =
          meshes_ready && g_r.meshes.contains(item.mesh);
    }
    if (meshes_ready) {
      return true;
    }
    ReleaseRemoteAppearanceResources(
        context, role, installed_state,
        "same appearance resources missing");
    installed_state = {};
  }

  auto pending =
      g_pending_remote_recipe_installs.find(role);
  if (pending != g_pending_remote_recipe_installs.end() &&
      (pending->second.identity != appearance.identity ||
       pending->second.session != session)) {
    CancelPendingRemoteRecipeInstall(
        context, role, "appearance superseded");
    pending = g_pending_remote_recipe_installs.end();
  }

  if (pending == g_pending_remote_recipe_installs.end()) {
    const std::vector<uint8_t>& bytes = *appearance.bytes;
    size_t cursor = 0;
    NetworkRecipeAppearanceHeader header;
    if (!ReadAppearancePod(bytes, cursor, header) ||
        header.magic != 0x31504352u ||
        header.version != 1 ||
        header.binding_count == 0 ||
        header.binding_count > 64 ||
        header.recipe_bytes < 64 ||
        header.recipe_bytes > 16u * 1024u ||
        cursor + header.recipe_bytes > bytes.size()) {
      return false;
    }
    const multiplayer_assets::RecipeResolveKey resolve_key = {
        role, session, appearance.identity};
    std::shared_ptr<const multiplayer_assets::RecipeAppearance>
        prepared;
    const multiplayer_assets::RecipeResolveStatus status =
        multiplayer_assets::PollRecipeAppearanceResolve(
            resolve_key, prepared);
    if (status ==
            multiplayer_assets::RecipeResolveStatus::kPending ||
        status ==
            multiplayer_assets::RecipeResolveStatus::kFailed) {
      return false;
    }
    if (status ==
        multiplayer_assets::RecipeResolveStatus::kUnknown) {
      std::vector<uint8_t> recipe(
          bytes.begin() + cursor,
          bytes.begin() + cursor + header.recipe_bytes);
      multiplayer_assets::QueueRecipeAppearanceResolve(
          resolve_key, std::move(recipe));
      return false;
    }
    if (prepared == nullptr) {
      return false;
    }
    cursor += header.recipe_bytes;

    PendingRemoteRecipeInstall next;
    next.identity = appearance.identity;
    next.session = session;
    next.resource_slot =
        multiplayer::lifecycle::
            NextRemoteAppearanceResourceSlot(
                !installed_state.items.empty(),
                installed_state.items.empty()
                    ? 0
                    : installed_state.items.front().mesh);
    next.resolved = std::move(prepared);
    next.started = std::chrono::steady_clock::now();
    next.bindings.reserve(header.binding_count);
    for (size_t binding_index = 0;
         binding_index < header.binding_count;
         ++binding_index) {
      RemoteRecipeAppearanceBinding binding;
      if (!ReadAppearancePod(bytes, cursor, binding.wire) ||
          binding.wire.model_id == 0 ||
          binding.wire.remap_count == 0 ||
          binding.wire.remap_count > 256 ||
          cursor +
                  size_t(binding.wire.remap_count) *
                      sizeof(uint16_t) >
              bytes.size()) {
        return false;
      }
      binding.remap.resize(binding.wire.remap_count);
      std::memcpy(
          binding.remap.data(), bytes.data() + cursor,
          binding.remap.size() * sizeof(uint16_t));
      cursor += binding.remap.size() * sizeof(uint16_t);
      next.bindings.emplace(
          binding.wire.model_id, std::move(binding));
    }
    if (cursor != bytes.size() ||
        !multiplayer::lifecycle::CompleteAppearancePieceCount(
            header.binding_count,
            next.resolved->pieces.size())) {
      return false;
    }
    for (const multiplayer_assets::RecipePiece& source :
         next.resolved->pieces) {
      if (!next.bindings.contains(source.model_id) ||
          source.mesh.vertices.empty() ||
          source.mesh.indices.empty()) {
        return false;
      }
    }
    next.staged.identity = appearance.identity;
    next.staged.session = session;
    std::copy_n(
        header.root_position, 3,
        next.staged.root_position);
    std::copy_n(
        header.root_x_axis, 3,
        next.staged.root_x_axis);
    std::copy_n(
        header.root_z_axis, 3,
        next.staged.root_z_axis);
    next.staged.items.reserve(
        next.resolved->pieces.size());
    next.staged.piece_caches.reserve(
        next.resolved->pieces.size());
    next.staged.texture_store_keys.reserve(
        next.resolved->textures.size());
    next.texture_ids.reserve(
        next.resolved->textures.size());
    for (const auto& [texture_id, source] :
         next.resolved->textures) {
      if (!source.bytes.empty() && source.width != 0 &&
          source.height != 0 && source.row_pitch != 0) {
        next.texture_ids.push_back(texture_id);
      }
    }
    std::sort(
        next.texture_ids.begin(), next.texture_ids.end());
    next.texture_objects.reserve(next.texture_ids.size());
    const auto [created, inserted] =
        g_pending_remote_recipe_installs.emplace(
            role, std::move(next));
    if (!inserted) {
      return false;
    }
    pending = created;
    REXLOG_INFO(
        "multiplayer-appearance-install-start: role={} "
        "session={} appearance={:016X} slot={} pieces={} "
        "textures={}",
        role, session, appearance.identity,
        pending->second.resource_slot,
        pending->second.resolved->pieces.size(),
        pending->second.texture_ids.size());
    LogRemoteAppearanceResourceAudit("start", role);
  }

  PendingRemoteRecipeInstall& transaction =
      pending->second;
  if (!AcquireRemoteAppearanceInstallOperation(
          frame_number)) {
    return false;
  }
  if (transaction.last_progress_frame != frame_number) {
    transaction.last_progress_frame = frame_number;
    ++transaction.progress_frames;
  }
  const auto operation_start =
      std::chrono::steady_clock::now();
  const char* phase = "commit";
  std::size_t phase_index = 0;
  std::size_t phase_total = 1;

  if (transaction.texture_index <
      transaction.texture_ids.size()) {
    phase = "texture";
    phase_index = transaction.texture_index + 1;
    phase_total = transaction.texture_ids.size();
    const uint64_t texture_id =
        transaction.texture_ids[
            transaction.texture_index];
    const auto source_it =
        transaction.resolved->textures.find(texture_id);
    if (source_it ==
        transaction.resolved->textures.end()) {
      CancelPendingRemoteRecipeInstall(
          context, role, "missing staged texture");
      return false;
    }
    const multiplayer_assets::RecipeTexture& source =
        source_it->second;
    const uint64_t content_key = AppearanceHashBytes(
        appearance.identity, &texture_id,
        sizeof(texture_id));
    const uint64_t store_key =
        multiplayer::lifecycle::
            RemoteAppearanceTextureStoreKey(
                role, content_key);
    const uint32_t object_key =
        multiplayer::lifecycle::
            RemoteAppearanceTextureObjectKey(
                transaction.resource_slot, role,
                transaction.texture_index);
    auto old = g_r.tex_store.find(store_key);
    if (old != g_r.tex_store.end()) {
      RetireGuestTexture(
          old->second,
          context.device->CurrentSubmission());
      g_r.tex_store.erase(old);
    }
    nrhi::BufferDesc buffer_desc;
    buffer_desc.size = source.bytes.size();
    buffer_desc.heap = nrhi::HeapKind::kUpload;
    buffer_desc.bind_class =
        nrhi::BufferBindClass::kCopySrc;
    nrhi::Buffer* upload =
        context.device->CreateBuffer(buffer_desc);
    nrhi::TextureDesc texture_desc;
    texture_desc.kind = nrhi::TextureKind::k2D;
    texture_desc.width = source.width;
    texture_desc.height = source.height;
    texture_desc.mip_levels = 1;
    texture_desc.format =
        source.format ==
                multiplayer_assets::TextureFormat::kBc1
            ? nrhi::Format::kBC1_UNORM
            : nrhi::Format::kBC3_UNORM;
    texture_desc.initial_state =
        nrhi::ResourceState::kCopyDest;
    nrhi::Texture* gpu_texture =
        context.device->CreateTexture(texture_desc);
    if (upload == nullptr || gpu_texture == nullptr) {
      context.device->DestroyDeferred(upload);
      context.device->DestroyDeferred(gpu_texture);
      CancelPendingRemoteRecipeInstall(
          context, role, "texture allocation failed");
      return false;
    }
    void* mapping = context.device->Map(upload);
    if (mapping == nullptr) {
      context.device->DestroyDeferred(upload);
      context.device->DestroyDeferred(gpu_texture);
      CancelPendingRemoteRecipeInstall(
          context, role, "texture map failed");
      return false;
    }
    std::memcpy(
        mapping, source.bytes.data(), source.bytes.size());
    context.device->Unmap(upload);
    context.cmd->CopyBufferToTexture(
        gpu_texture, 0, 0, upload, 0,
        source.row_pitch, source.width, source.height, 1);
    context.cmd->Barrier(
        gpu_texture, nrhi::ResourceState::kCopyDest,
        nrhi::ResourceState::kPixelShaderResource);
    nrhi::TextureViewDesc view_desc;
    view_desc.dimension = nrhi::ViewDimension::k2D;
    view_desc.format = texture_desc.format;
    view_desc.mip_levels = 1;
    GuestTexture guest;
    guest.texture = gpu_texture;
    context.device->DestroyDeferred(upload);
    guest.upload = nullptr;
    guest.srv = context.device->CreateTextureView(
        gpu_texture, view_desc);
    guest.srv_format = view_desc.format;
    guest.srv_mips = 1;
    guest.valid = guest.srv != nullptr;
    guest.last_used_frame =
        std::numeric_limits<uint64_t>::max() / 2;
    if (!guest.valid) {
      RetireGuestTexture(
          guest, context.device->CurrentSubmission());
      CancelPendingRemoteRecipeInstall(
          context, role, "texture view failed");
      return false;
    }
    g_r.tex_store.emplace(store_key, std::move(guest));
    transaction.staged.texture_store_keys.push_back(
        store_key);
    RendererState::TexRoute route;
    route.key = store_key;
    route.demoted = true;
    g_r.tex_routes[object_key] = route;
    transaction.texture_objects.emplace(
        texture_id, object_key);
    ++transaction.texture_index;
  } else if (
      transaction.piece_index <
      transaction.resolved->pieces.size()) {
    phase = "piece";
    phase_index = transaction.piece_index + 1;
    phase_total = transaction.resolved->pieces.size();
    const multiplayer_assets::RecipePiece& source =
        transaction.resolved->pieces[
            transaction.piece_index];
    const auto binding =
        transaction.bindings.find(source.model_id);
    if (binding == transaction.bindings.end()) {
      CancelPendingRemoteRecipeInstall(
          context, role, "missing staged binding");
      return false;
    }
    const RemoteRecipeAppearanceBinding& metadata =
        binding->second;
    DrawItem item{};
    item.mesh =
        multiplayer::lifecycle::RemoteAppearanceMeshKey(
            transaction.resource_slot, role,
            transaction.piece_index);
    item.multiplayer_track_key =
        metadata.wire.track_key;
    item.fingerprint = AppearanceHashBytes(
        appearance.identity, &source.model_id,
        sizeof(source.model_id));
    item.skinned = true;
    item.hair =
        (metadata.wire.flags & kNetworkPieceHair) != 0 ||
        source.category == "Hair";
    item.ropa = false;
    item.char_alpha =
        (metadata.wire.flags &
         kNetworkPieceCharAlpha) != 0;
    item.shadow_caster =
        (metadata.wire.flags &
         kNetworkPieceShadow) != 0;
    item.char_family =
        metadata.wire.char_family != 0
            ? metadata.wire.char_family
            : (item.hair ? 4 : 2);
    item.pending = false;
    item.retained = true;
    item.dyn_entity = true;
    item.lw_alpha = -1.0f;
    item.ib_count =
        static_cast<uint32_t>(
            source.mesh.indices.size());
    item.bones.resize(metadata.remap.size() * 12);
    item.multiplayer_palette_to_canonical =
        metadata.remap;
    std::copy_n(
        metadata.wire.char_rows, 72, item.char_rows);
    std::copy_n(
        metadata.wire.tint, 4, item.tint);
    NormalizeRemoteAppearanceTransientRows(item, role);
    if ((metadata.wire.flags &
         kNetworkPieceRopa) != 0) {
      REXLOG_INFO(
          "multiplayer-appearance-binding: role={} kind=ropa "
          "model={:016X} rx_mesh={:08X} route={} "
          "track={:08X} remap={}",
          role, source.model_id, item.mesh,
          metadata.wire.track_key == 0
              ? "canonical"
              : "exact",
          metadata.wire.track_key, metadata.remap.size());
    }
    item.world[0] = 1.0f;
    item.world[5] = 1.0f;
    item.world[10] = 1.0f;
    item.world[15] = 1.0f;
    std::copy_n(
        source.mesh.bbox_min, 3, item.bbox_min);
    std::copy_n(
        source.mesh.bbox_max, 3, item.bbox_max);
    item.draws.push_back(
        DrawEntry{
            4, 0, 0,
            static_cast<uint32_t>(
                source.mesh.indices.size())});
    const auto texture_object =
        [&transaction](uint64_t texture_id) {
          const auto found =
              transaction.texture_objects.find(texture_id);
          return found ==
                         transaction.texture_objects.end()
                     ? 0u
                     : found->second;
        };
    item.diffuse_tex =
        texture_object(source.diffuse_texture);
    item.water_normal =
        texture_object(source.normal_texture);
    item.hair_alpha_tex =
        texture_object(source.alpha_texture);
    item.spec_tex =
        texture_object(source.specular_texture);

    const size_t vertex_bytes =
        source.mesh.vertices.size() * sizeof(float);
    const size_t index_bytes =
        source.mesh.indices.size() * sizeof(uint16_t);
    nrhi::Buffer* vb = AcquireMeshUploadBuffer(
        context.device, vertex_bytes);
    nrhi::Buffer* ib = AcquireMeshUploadBuffer(
        context.device, index_bytes);
    if (vb == nullptr || ib == nullptr) {
      PoolMeshBuffer(context.device, vb);
      PoolMeshBuffer(context.device, ib);
      CancelPendingRemoteRecipeInstall(
          context, role, "mesh allocation failed");
      return false;
    }
    void* vb_mapping = context.device->Map(vb);
    void* ib_mapping = context.device->Map(ib);
    if (vb_mapping == nullptr || ib_mapping == nullptr) {
      if (vb_mapping != nullptr) {
        context.device->Unmap(vb);
      }
      if (ib_mapping != nullptr) {
        context.device->Unmap(ib);
      }
      PoolMeshBuffer(context.device, vb);
      PoolMeshBuffer(context.device, ib);
      CancelPendingRemoteRecipeInstall(
          context, role, "mesh map failed");
      return false;
    }
    std::memcpy(
        vb_mapping, source.mesh.vertices.data(),
        vertex_bytes);
    std::memcpy(
        ib_mapping, source.mesh.indices.data(),
        index_bytes);
    context.device->Unmap(vb);
    context.device->Unmap(ib);
    auto old_mesh = g_r.meshes.find(item.mesh);
    if (old_mesh != g_r.meshes.end()) {
      PoolMeshBuffer(
          context.device, old_mesh->second.vb);
      PoolMeshBuffer(
          context.device, old_mesh->second.ib);
      g_r.meshes.erase(old_mesh);
    }
    MeshBuffers buffers;
    buffers.vb = vb;
    buffers.ib = ib;
    buffers.vb_view = {
        vb, 0, static_cast<uint32_t>(vertex_bytes), 56};
    buffers.ib_view = {
        ib, 0, static_cast<uint32_t>(index_bytes)};
    buffers.fingerprint = item.fingerprint;
    buffers.raytracing_verts = source.mesh.vertices;
    buffers.raytracing_indices = source.mesh.indices;
    g_r.meshes.emplace(item.mesh, std::move(buffers));
    transaction.staged.items.push_back(std::move(item));
    RemoteAppearancePieceRenderCache cache;
    BuildRemoteAppearancePieceRenderCache(
        transaction.staged.items.back(), cache,
        transaction.scanned_vertices,
        transaction.weighted_rows,
        transaction.resolved_rigs);
    transaction.staged.piece_caches.push_back(
        std::move(cache));
    ++transaction.piece_index;
  } else {
    if (!multiplayer::lifecycle::CompleteAppearancePieceCount(
            transaction.bindings.size(),
            transaction.staged.items.size())) {
      CancelPendingRemoteRecipeInstall(
          context, role, "staged piece count incomplete");
      return false;
    }
    REXLOG_INFO(
        "multiplayer-render-cache: prepared role={} "
        "session={} appearance={:016X} pieces={} vertices={} "
        "weighted_rows={} resolved_rigs={}",
        role, transaction.staged.session,
        transaction.staged.identity,
        transaction.staged.piece_caches.size(),
        transaction.scanned_vertices,
        transaction.weighted_rows,
        transaction.resolved_rigs);
    RemoteAppearanceRenderState previous =
        std::move(g_remote_appearances[role]);
    g_remote_appearances[role] =
        std::move(transaction.staged);
    if (previous.identity != 0 ||
        !previous.items.empty() ||
        !previous.texture_store_keys.empty()) {
      ReleaseRemoteAppearanceResources(
          context, role, previous,
          "transaction committed");
    }
  }

  const double operation_ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() -
          operation_start)
          .count();
  ++transaction.operations;
  transaction.operation_ms_sum += operation_ms;
  transaction.operation_ms_max =
      std::max(transaction.operation_ms_max, operation_ms);
  REXLOG_INFO(
      "multiplayer-appearance-install-step: role={} "
      "session={} appearance={:016X} phase={} index={}/{} "
      "upload={:.3f}ms frame={} operation={}",
      role, session, appearance.identity, phase,
      phase_index, phase_total, operation_ms,
      transaction.progress_frames, transaction.operations);

  if (phase[0] != 'c') {
    return false;
  }

  const double wall_ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() -
          transaction.started)
          .count();
  const uint32_t progress_frames =
      transaction.progress_frames;
  const uint32_t operations = transaction.operations;
  const double operation_ms_sum =
      transaction.operation_ms_sum;
  const double operation_ms_max =
      transaction.operation_ms_max;
  const std::size_t texture_count =
      transaction.texture_objects.size();
  const std::size_t piece_count =
      g_remote_appearances[role].items.size();
  transaction.resolved.reset();
  multiplayer_assets::ForgetRecipeAppearanceResolve(
      role, session);
  g_pending_remote_recipe_installs.erase(role);
  LogRemoteAppearanceResourceAudit("commit", role);
  REXLOG_INFO(
      "multiplayer-appearance-install: role={} session={} "
      "appearance={:016X} upload={:.3f}ms total={:.3f}ms "
      "wall={:.3f}ms frames={} operations={} pieces={} "
      "textures={}",
      role, session, appearance.identity,
      operation_ms_max, operation_ms_sum, wall_ms,
      progress_frames, operations, piece_count,
      texture_count);
  REXLOG_INFO(
      "multiplayer: installed recipe appearance role={} "
      "id={:016X} pieces={} textures={} wire_bytes={}",
      role, appearance.identity, piece_count,
      texture_count, appearance.bytes->size());
  return true;
}

bool InstallRemoteRecipeAppearance(
    const NativeGuestOutputRenderContext& context,
    uint64_t frame_number,
    uint32_t role,
    uint32_t session,
    const multiplayer::AppearanceBlob& appearance) {
  if (appearance.bytes == nullptr) {
    return false;
  }
  const bool async_prepare =
      REXCVAR_GET(skate3_multiplayer_async_appearance_prepare);
  if (REXCVAR_GET(
          skate3_multiplayer_incremental_appearance_install) &&
      async_prepare) {
    return InstallRemoteRecipeAppearanceIncremental(
        context, frame_number, role, session, appearance);
  }
  if (g_pending_remote_recipe_installs.contains(role)) {
    CancelPendingRemoteRecipeInstall(
        context, role, "incremental installer disabled");
  }
  RemoteAppearanceRenderState& state =
      g_remote_appearances[role];
  if (state.identity == appearance.identity &&
      state.session == session &&
      !state.items.empty()) {
    bool meshes_ready = true;
    for (const DrawItem& item : state.items) {
      meshes_ready =
          meshes_ready && g_r.meshes.contains(item.mesh);
    }
    if (meshes_ready) {
      return true;
    }
  }
  const multiplayer_assets::RecipeResolveKey resolve_key = {
      role, session, appearance.identity};
  std::shared_ptr<const multiplayer_assets::RecipeAppearance>
      prepared;
  multiplayer_assets::RecipeResolveStatus resolve_status =
      multiplayer_assets::RecipeResolveStatus::kUnknown;
  if (async_prepare) {
    resolve_status =
        multiplayer_assets::PollRecipeAppearanceResolve(
            resolve_key, prepared);
    if (resolve_status ==
            multiplayer_assets::RecipeResolveStatus::kPending ||
        resolve_status ==
            multiplayer_assets::RecipeResolveStatus::kFailed) {
      return false;
    }
  }
  const std::vector<uint8_t>& bytes =
      *appearance.bytes;
  size_t cursor = 0;
  NetworkRecipeAppearanceHeader header;
  if (!ReadAppearancePod(bytes, cursor, header) ||
      header.magic != 0x31504352u ||
      header.version != 1 ||
      header.binding_count == 0 ||
      header.binding_count > 64 ||
      header.recipe_bytes < 64 ||
      header.recipe_bytes > 16u * 1024u ||
      cursor + header.recipe_bytes > bytes.size()) {
    return false;
  }
  if (async_prepare &&
      resolve_status ==
          multiplayer_assets::RecipeResolveStatus::kUnknown) {
    std::vector<uint8_t> recipe(
        bytes.begin() + cursor,
        bytes.begin() + cursor + header.recipe_bytes);
    multiplayer_assets::QueueRecipeAppearanceResolve(
        resolve_key, std::move(recipe));
    return false;
  }
  std::vector<uint8_t> recipe;
  if (!async_prepare) {
    recipe.assign(
        bytes.begin() + cursor,
        bytes.begin() + cursor + header.recipe_bytes);
  }
  cursor += header.recipe_bytes;
  struct ParsedBinding {
    NetworkRecipeAppearanceBinding wire;
    std::vector<uint16_t> remap;
  };
  std::unordered_map<uint64_t, ParsedBinding> bindings;
  bindings.reserve(header.binding_count);
  for (size_t binding_index = 0;
       binding_index < header.binding_count;
       ++binding_index) {
    ParsedBinding binding;
    if (!ReadAppearancePod(bytes, cursor, binding.wire) ||
        binding.wire.model_id == 0 ||
        binding.wire.remap_count == 0 ||
        binding.wire.remap_count > 256 ||
        cursor +
                size_t(binding.wire.remap_count) *
                    sizeof(uint16_t) >
            bytes.size()) {
      return false;
    }
    binding.remap.resize(binding.wire.remap_count);
    std::memcpy(
        binding.remap.data(), bytes.data() + cursor,
        binding.remap.size() * sizeof(uint16_t));
    cursor += binding.remap.size() * sizeof(uint16_t);
    bindings.emplace(
        binding.wire.model_id, std::move(binding));
  }
  if (cursor != bytes.size()) {
    return false;
  }

  multiplayer_assets::RecipeAppearance synchronous_resolved;
  const multiplayer_assets::RecipeAppearance* resolved =
      prepared.get();
  if (!async_prepare) {
    if (!multiplayer_assets::ResolveRecipeAppearance(
            recipe, true, synchronous_resolved)) {
      return false;
    }
    resolved = &synchronous_resolved;
  }
  if (resolved == nullptr) {
    return false;
  }
  const auto install_start = std::chrono::steady_clock::now();
  if (!multiplayer::lifecycle::CompleteAppearancePieceCount(
          header.binding_count, resolved->pieces.size())) {
    REXLOG_WARN(
        "multiplayer: recipe appearance incomplete role={} "
        "id={:016X} expected_pieces={} resolved_pieces={}",
        role, appearance.identity, header.binding_count,
        resolved->pieces.size());
    return false;
  }
  for (const multiplayer_assets::RecipePiece& source :
       resolved->pieces) {
    if (!bindings.contains(source.model_id) ||
        source.mesh.vertices.empty() ||
        source.mesh.indices.empty()) {
      return false;
    }
  }
  RemoteAppearanceRenderState installed;
  installed.identity = appearance.identity;
  installed.session = session;
  std::copy_n(
      header.root_position, 3, installed.root_position);
  std::copy_n(
      header.root_x_axis, 3, installed.root_x_axis);
  std::copy_n(
      header.root_z_axis, 3, installed.root_z_axis);
  installed.items.reserve(resolved->pieces.size());
  installed.texture_store_keys.reserve(
      resolved->textures.size());

  std::unordered_map<uint64_t, uint32_t> texture_objects;
  texture_objects.reserve(resolved->textures.size());
  uint32_t texture_index = 0;
  for (const auto& [texture_id, source] :
       resolved->textures) {
    if (source.bytes.empty() || source.width == 0 ||
        source.height == 0 || source.row_pitch == 0) {
      continue;
    }
    const uint64_t content_key = AppearanceHashBytes(
        appearance.identity, &texture_id,
        sizeof(texture_id));
    const uint64_t store_key =
        multiplayer::lifecycle::RemoteAppearanceTextureStoreKey(
            role, content_key);
    const uint32_t object_key =
        0xF1000000u |
        ((role & 0xFFu) << 16) |
        (texture_index & 0xFFFFu);
    ++texture_index;
    auto old = g_r.tex_store.find(store_key);
    if (old != g_r.tex_store.end()) {
      RetireGuestTexture(
          old->second,
          context.device->CurrentSubmission());
      g_r.tex_store.erase(old);
    }
    nrhi::BufferDesc buffer_desc;
    buffer_desc.size = source.bytes.size();
    buffer_desc.heap = nrhi::HeapKind::kUpload;
    buffer_desc.bind_class =
        nrhi::BufferBindClass::kCopySrc;
    nrhi::Buffer* upload =
        context.device->CreateBuffer(buffer_desc);
    nrhi::TextureDesc texture_desc;
    texture_desc.kind = nrhi::TextureKind::k2D;
    texture_desc.width = source.width;
    texture_desc.height = source.height;
    texture_desc.mip_levels = 1;
    texture_desc.format =
        source.format ==
                multiplayer_assets::TextureFormat::kBc1
            ? nrhi::Format::kBC1_UNORM
            : nrhi::Format::kBC3_UNORM;
    texture_desc.initial_state =
        nrhi::ResourceState::kCopyDest;
    nrhi::Texture* gpu_texture =
        context.device->CreateTexture(texture_desc);
    if (upload == nullptr || gpu_texture == nullptr) {
      context.device->DestroyDeferred(upload);
      context.device->DestroyDeferred(gpu_texture);
      return false;
    }
    void* mapping = context.device->Map(upload);
    if (mapping == nullptr) {
      context.device->DestroyDeferred(upload);
      context.device->DestroyDeferred(gpu_texture);
      return false;
    }
    std::memcpy(
        mapping, source.bytes.data(), source.bytes.size());
    context.device->Unmap(upload);
    context.cmd->CopyBufferToTexture(
        gpu_texture, 0, 0, upload, 0,
        source.row_pitch, source.width, source.height, 1);
    context.cmd->Barrier(
        gpu_texture, nrhi::ResourceState::kCopyDest,
        nrhi::ResourceState::kPixelShaderResource);
    nrhi::TextureViewDesc view_desc;
    view_desc.dimension = nrhi::ViewDimension::k2D;
    view_desc.format = texture_desc.format;
    view_desc.mip_levels = 1;
    GuestTexture guest;
    guest.texture = gpu_texture;
    context.device->DestroyDeferred(upload);
    guest.upload = nullptr;
    guest.srv = context.device->CreateTextureView(
        gpu_texture, view_desc);
    guest.srv_format = view_desc.format;
    guest.srv_mips = 1;
    guest.valid = guest.srv != nullptr;
    guest.last_used_frame =
        std::numeric_limits<uint64_t>::max() / 2;
    if (!guest.valid) {
      RetireGuestTexture(
          guest, context.device->CurrentSubmission());
      return false;
    }
    g_r.tex_store.emplace(store_key, std::move(guest));
    installed.texture_store_keys.push_back(store_key);
    RendererState::TexRoute route;
    route.key = store_key;
    route.demoted = true;
    g_r.tex_routes[object_key] = route;
    texture_objects.emplace(texture_id, object_key);
  }

  const auto texture_object =
      [&texture_objects](uint64_t texture_id) {
        const auto found =
            texture_objects.find(texture_id);
        return found == texture_objects.end()
                   ? 0u
                   : found->second;
      };
  size_t piece_index = 0;
  for (const multiplayer_assets::RecipePiece& source :
       resolved->pieces) {
    const auto binding = bindings.find(source.model_id);
    if (binding == bindings.end() ||
        source.mesh.vertices.empty() ||
        source.mesh.indices.empty()) {
      continue;
    }
    const ParsedBinding& metadata = binding->second;
    DrawItem item{};
    item.mesh =
        0xE1000000u |
        ((role & 0xFFu) << 16) |
        static_cast<uint32_t>(piece_index++);
    item.multiplayer_track_key =
        metadata.wire.track_key;
    item.fingerprint = AppearanceHashBytes(
        appearance.identity, &source.model_id,
        sizeof(source.model_id));
    item.skinned = true;
    item.hair =
        (metadata.wire.flags & kNetworkPieceHair) != 0 ||
        source.category == "Hair";
    // Recipe geometry is the immutable bind mesh. ROPA's source flag means
    // its live local VB was CPU-deformed; carrying that flag to this bind
    // model would disable the canonical GPU skinning that keeps the shirt
    // attached.
    item.ropa = false;
    item.char_alpha =
        (metadata.wire.flags &
         kNetworkPieceCharAlpha) != 0;
    item.shadow_caster =
        (metadata.wire.flags &
         kNetworkPieceShadow) != 0;
    item.char_family =
        metadata.wire.char_family != 0
            ? metadata.wire.char_family
            : (item.hair ? 4 : 2);
    item.pending = false;
    item.retained = true;
    item.dyn_entity = true;
    item.lw_alpha = -1.0f;
    item.ib_count =
        static_cast<uint32_t>(source.mesh.indices.size());
    item.bones.resize(metadata.remap.size() * 12);
    item.multiplayer_palette_to_canonical =
        metadata.remap;
    std::copy_n(
        metadata.wire.char_rows, 72, item.char_rows);
    std::copy_n(
        metadata.wire.tint, 4, item.tint);
    NormalizeRemoteAppearanceTransientRows(item, role);
    if ((metadata.wire.flags & kNetworkPieceRopa) != 0) {
      REXLOG_INFO(
          "multiplayer-appearance-binding: role={} kind=ropa "
          "model={:016X} rx_mesh={:08X} route={} track={:08X} "
          "remap={}",
          role, source.model_id, item.mesh,
          metadata.wire.track_key == 0 ? "canonical" : "exact",
          metadata.wire.track_key, metadata.remap.size());
    }
    item.world[0] = 1.0f;
    item.world[5] = 1.0f;
    item.world[10] = 1.0f;
    item.world[15] = 1.0f;
    std::copy_n(
        source.mesh.bbox_min, 3, item.bbox_min);
    std::copy_n(
        source.mesh.bbox_max, 3, item.bbox_max);
    item.draws.push_back(
        DrawEntry{
            4, 0, 0,
            static_cast<uint32_t>(
                source.mesh.indices.size())});
    item.diffuse_tex =
        texture_object(source.diffuse_texture);
    item.water_normal =
        texture_object(source.normal_texture);
    item.hair_alpha_tex =
        texture_object(source.alpha_texture);
    item.spec_tex =
        texture_object(source.specular_texture);

    const size_t vertex_bytes =
        source.mesh.vertices.size() * sizeof(float);
    const size_t index_bytes =
        source.mesh.indices.size() * sizeof(uint16_t);
    nrhi::Buffer* vb = AcquireMeshUploadBuffer(
        context.device, vertex_bytes);
    nrhi::Buffer* ib = AcquireMeshUploadBuffer(
        context.device, index_bytes);
    if (vb == nullptr || ib == nullptr) {
      PoolMeshBuffer(context.device, vb);
      PoolMeshBuffer(context.device, ib);
      return false;
    }
    void* vb_mapping = context.device->Map(vb);
    void* ib_mapping = context.device->Map(ib);
    if (vb_mapping == nullptr || ib_mapping == nullptr) {
      if (vb_mapping != nullptr) {
        context.device->Unmap(vb);
      }
      if (ib_mapping != nullptr) {
        context.device->Unmap(ib);
      }
      PoolMeshBuffer(context.device, vb);
      PoolMeshBuffer(context.device, ib);
      return false;
    }
    std::memcpy(
        vb_mapping, source.mesh.vertices.data(),
        vertex_bytes);
    std::memcpy(
        ib_mapping, source.mesh.indices.data(),
        index_bytes);
    context.device->Unmap(vb);
    context.device->Unmap(ib);
    auto old_mesh = g_r.meshes.find(item.mesh);
    if (old_mesh != g_r.meshes.end()) {
      PoolMeshBuffer(
          context.device, old_mesh->second.vb);
      PoolMeshBuffer(
          context.device, old_mesh->second.ib);
      g_r.meshes.erase(old_mesh);
    }
    MeshBuffers buffers;
    buffers.vb = vb;
    buffers.ib = ib;
    buffers.vb_view = {
        vb, 0, static_cast<uint32_t>(vertex_bytes), 56};
    buffers.ib_view = {
        ib, 0, static_cast<uint32_t>(index_bytes)};
    buffers.fingerprint = item.fingerprint;
    buffers.raytracing_verts = source.mesh.vertices;
    buffers.raytracing_indices = source.mesh.indices;
    g_r.meshes.emplace(item.mesh, std::move(buffers));
    installed.items.push_back(std::move(item));
  }
  if (!multiplayer::lifecycle::CompleteAppearancePieceCount(
          header.binding_count, installed.items.size())) {
    return false;
  }

  if (state.identity != 0 &&
      state.identity != installed.identity) {
    for (uint64_t old_store_key :
         state.texture_store_keys) {
      const auto old_texture =
          g_r.tex_store.find(old_store_key);
      if (old_texture != g_r.tex_store.end()) {
        RetireGuestTexture(
            old_texture->second,
            context.device->CurrentSubmission());
        g_r.tex_store.erase(old_texture);
      }
      for (auto route = g_r.tex_routes.begin();
           route != g_r.tex_routes.end();) {
        if (route->second.key == old_store_key) {
          route = g_r.tex_routes.erase(route);
        } else {
          ++route;
        }
      }
    }
    std::unordered_set<uint32_t> installed_meshes;
    for (const DrawItem& item : installed.items) {
      installed_meshes.insert(item.mesh);
    }
    for (const DrawItem& old_item : state.items) {
      if (installed_meshes.contains(old_item.mesh)) {
        continue;
      }
      const auto old_mesh =
          g_r.meshes.find(old_item.mesh);
      if (old_mesh != g_r.meshes.end()) {
        PoolMeshBuffer(
            context.device, old_mesh->second.vb);
        PoolMeshBuffer(
            context.device, old_mesh->second.ib);
        g_r.meshes.erase(old_mesh);
      }
    }
  }
  BuildRemoteAppearanceRenderCaches(role, installed);
  state = std::move(installed);
  LogRemoteAppearanceResourceAudit(
      "synchronous-install", role);
  if (async_prepare) {
    // Leave the table as the final owner so its asset worker, rather than
    // this render thread, releases the large CPU-side decode result.
    prepared.reset();
    multiplayer_assets::ForgetRecipeAppearanceResolve(
        role, session);
  }
  const double install_ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - install_start)
          .count();
  REXLOG_INFO(
      "multiplayer-appearance-install: role={} session={} "
      "appearance={:016X} upload={:.3f}ms pieces={} textures={}",
      role, session, appearance.identity, install_ms,
      state.items.size(), texture_objects.size());
  REXLOG_INFO(
      "multiplayer: installed recipe appearance role={} "
      "id={:016X} pieces={} textures={} wire_bytes={}",
      role, appearance.identity, state.items.size(),
      texture_objects.size(), bytes.size());
  return true;
}

bool InstallRemoteAppearance(
    const NativeGuestOutputRenderContext& context,
    uint64_t frame_number,
    uint32_t role,
    uint32_t session,
    const multiplayer::AppearanceBlob& appearance) {
  if (appearance.identity == 0 ||
      appearance.bytes == nullptr ||
      appearance.bytes->empty()) {
    return false;
  }
  const auto pending =
      g_pending_remote_recipe_installs.find(role);
  if (pending != g_pending_remote_recipe_installs.end() &&
      (pending->second.identity != appearance.identity ||
       pending->second.session != session)) {
    CancelPendingRemoteRecipeInstall(
        context, role, "appearance reverted or superseded");
  }
  RemoteAppearanceRenderState& state =
      g_remote_appearances[role];
  if (state.identity == appearance.identity &&
      state.session == session &&
      !state.items.empty()) {
    bool meshes_ready = true;
    for (const DrawItem& item : state.items) {
      meshes_ready = meshes_ready &&
                     g_r.meshes.contains(item.mesh);
    }
    if (meshes_ready) {
      return true;
    }
  }
  const std::vector<uint8_t>& bytes =
      *appearance.bytes;
  if (bytes.size() >= sizeof(uint32_t)) {
    uint32_t magic = 0;
    std::memcpy(&magic, bytes.data(), sizeof(magic));
    if (magic == 0x31504352u) {
      return InstallRemoteRecipeAppearance(
          context, frame_number, role, session, appearance);
    }
  }
  size_t cursor = 0;
  NetworkAppearanceHeader header;
  if (!ReadAppearancePod(bytes, cursor, header) ||
      header.magic != 0x31505041u ||
      header.version != 2 ||
      header.texture_count > 255 ||
      header.piece_count == 0 ||
      header.piece_count > 64) {
    return false;
  }
  RemoteAppearanceRenderState installed;
  installed.identity = appearance.identity;
  installed.session = session;
  std::copy_n(
      header.root_position, 3,
      installed.root_position);
  std::copy_n(
      header.root_x_axis, 3,
      installed.root_x_axis);
  std::copy_n(
      header.root_z_axis, 3,
      installed.root_z_axis);
  installed.items.reserve(header.piece_count);
  installed.texture_store_keys.reserve(
      header.texture_count);
  std::vector<uint32_t> texture_objects(
      header.texture_count, 0);
  for (size_t texture_index = 0;
       texture_index < header.texture_count;
       ++texture_index) {
    NetworkAppearanceTexture texture;
    if (!ReadAppearancePod(bytes, cursor, texture) ||
        texture.mip_count == 0 ||
        texture.mip_count > 16 ||
        texture.payload_bytes == 0 ||
        texture.payload_bytes > 8u * 1024u * 1024u ||
        cursor + texture.payload_bytes > bytes.size() ||
        texture.mips[0].width == 0 ||
        texture.mips[0].height == 0) {
      return false;
    }
    const uint64_t content_key =
        appearance.identity ^
        (0x9E3779B97F4A7C15ull *
         (texture_index + 1));
    const uint64_t store_key =
        multiplayer::lifecycle::RemoteAppearanceTextureStoreKey(
            role, content_key);
    const uint32_t object_key =
        0xF0000000u |
        ((role & 0xFFu) << 16) |
        static_cast<uint32_t>(texture_index);
    auto old = g_r.tex_store.find(store_key);
    if (old != g_r.tex_store.end()) {
      RetireGuestTexture(
          old->second,
          context.device->CurrentSubmission());
      g_r.tex_store.erase(old);
    }
    nrhi::BufferDesc buffer_desc;
    buffer_desc.size = texture.payload_bytes;
    buffer_desc.heap = nrhi::HeapKind::kUpload;
    buffer_desc.bind_class =
        nrhi::BufferBindClass::kCopySrc;
    nrhi::Buffer* upload =
        context.device->CreateBuffer(buffer_desc);
    nrhi::TextureDesc texture_desc;
    texture_desc.kind = nrhi::TextureKind::k2D;
    texture_desc.width = texture.mips[0].width;
    texture_desc.height = texture.mips[0].height;
    texture_desc.mip_levels = texture.mip_count;
    texture_desc.format =
        static_cast<nrhi::Format>(texture.copy_format);
    texture_desc.initial_state =
        nrhi::ResourceState::kCopyDest;
    nrhi::Texture* gpu_texture =
        context.device->CreateTexture(texture_desc);
    if (upload == nullptr || gpu_texture == nullptr) {
      context.device->DestroyDeferred(upload);
      context.device->DestroyDeferred(gpu_texture);
      return false;
    }
    void* mapping = context.device->Map(upload);
    if (mapping == nullptr) {
      context.device->DestroyDeferred(upload);
      context.device->DestroyDeferred(gpu_texture);
      return false;
    }
    std::memcpy(
        mapping, bytes.data() + cursor,
        texture.payload_bytes);
    context.device->Unmap(upload);
    cursor += texture.payload_bytes;
    for (size_t mip = 0; mip < texture.mip_count; ++mip) {
      const NetworkAppearanceMip& plan =
          texture.mips[mip];
      if (plan.offset >= texture.payload_bytes ||
          plan.pitch == 0 || plan.width == 0 ||
          plan.height == 0) {
        context.device->DestroyDeferred(upload);
        context.device->DestroyDeferred(gpu_texture);
        return false;
      }
      context.cmd->CopyBufferToTexture(
          gpu_texture, static_cast<uint32_t>(mip), 0,
          upload, plan.offset, plan.pitch,
          plan.width, plan.height, 1);
    }
    context.cmd->Barrier(
        gpu_texture, nrhi::ResourceState::kCopyDest,
        nrhi::ResourceState::kPixelShaderResource);
    nrhi::TextureViewDesc view_desc;
    view_desc.dimension = nrhi::ViewDimension::k2D;
    view_desc.format =
        static_cast<nrhi::Format>(texture.srv_format);
    view_desc.mip_levels = texture.mip_count;
    for (size_t component = 0; component < 4; ++component) {
      view_desc.swizzle[component] =
          static_cast<nrhi::Swizzle>(
              texture.swizzle[component]);
    }
    GuestTexture guest;
    guest.texture = gpu_texture;
    // The copy commands now own this upload until the current submission
    // completes. Keeping it in the long-lived remote texture entry doubled
    // appearance memory for no benefit.
    context.device->DestroyDeferred(upload);
    guest.upload = nullptr;
    guest.srv = context.device->CreateTextureView(
        gpu_texture, view_desc);
    guest.srv_format = view_desc.format;
    guest.srv_mips = texture.mip_count;
    guest.valid = guest.srv != nullptr;
    guest.last_used_frame =
        std::numeric_limits<uint64_t>::max() / 2;
    if (!guest.valid) {
      RetireGuestTexture(
          guest, context.device->CurrentSubmission());
      return false;
    }
    g_r.tex_store.emplace(store_key, std::move(guest));
    installed.texture_store_keys.push_back(store_key);
    RendererState::TexRoute route;
    route.key = store_key;
    route.demoted = true;
    g_r.tex_routes[object_key] = route;
    texture_objects[texture_index] = object_key;
  }

  for (size_t piece_index = 0;
       piece_index < header.piece_count;
       ++piece_index) {
    NetworkAppearancePiece piece;
    if (!ReadAppearancePod(bytes, cursor, piece) ||
        piece.vertex_count == 0 ||
        piece.vertex_count > 0xFFFFu ||
        piece.index_count == 0 ||
        piece.index_count > 4u * 1024u * 1024u ||
        piece.draw_count == 0 ||
        piece.draw_count > 256 ||
        piece.remap_count > 256 ||
        piece.bone_count > 256) {
      return false;
    }
    const size_t draw_bytes =
        size_t(piece.draw_count) * sizeof(DrawEntry);
    const size_t remap_bytes =
        size_t(piece.remap_count) * sizeof(uint16_t);
    const size_t vertex_bytes =
        size_t(piece.vertex_count) * 14 * sizeof(float);
    const size_t index_bytes =
        size_t(piece.index_count) * sizeof(uint16_t);
    if (cursor + draw_bytes + remap_bytes +
            vertex_bytes + index_bytes >
        bytes.size()) {
      return false;
    }
    DrawItem item{};
    item.mesh =
        0xE0000000u |
        ((role & 0xFFu) << 16) |
        static_cast<uint32_t>(piece_index);
    item.multiplayer_track_key = piece.track_key;
    item.fingerprint = piece.fingerprint;
    item.skinned =
        (piece.flags & kNetworkPieceSkinned) != 0;
    item.hair =
        (piece.flags & kNetworkPieceHair) != 0;
    item.ropa =
        (piece.flags & kNetworkPieceRopa) != 0;
    item.char_alpha =
        (piece.flags & kNetworkPieceCharAlpha) != 0;
    item.shadow_caster =
        (piece.flags & kNetworkPieceShadow) != 0;
    item.char_family = piece.char_family;
    item.pending = false;
    item.retained = true;
    item.dyn_entity = true;
    item.lw_alpha = -1.0f;
    item.ib_count = piece.index_count;
    item.bones.resize(
        size_t(piece.bone_count) * 12);
    std::copy_n(piece.char_rows, 72, item.char_rows);
    std::copy_n(piece.tint, 4, item.tint);
    NormalizeRemoteAppearanceTransientRows(item, role);
    std::copy_n(piece.world, 16, item.world);
    std::copy_n(piece.bbox_min, 3, item.bbox_min);
    std::copy_n(piece.bbox_max, 3, item.bbox_max);
    item.draws.resize(piece.draw_count);
    std::memcpy(
        item.draws.data(), bytes.data() + cursor,
        draw_bytes);
    cursor += draw_bytes;
    item.multiplayer_palette_to_canonical.resize(
        piece.remap_count);
    std::memcpy(
        item.multiplayer_palette_to_canonical.data(),
        bytes.data() + cursor, remap_bytes);
    cursor += remap_bytes;
    const uint8_t* vertex_bytes_source =
        bytes.data() + cursor;
    cursor += vertex_bytes;
    const uint8_t* index_bytes_source =
        bytes.data() + cursor;
    cursor += index_bytes;
    const auto texture_object =
        [&texture_objects](uint16_t index) {
          return index < texture_objects.size()
                     ? texture_objects[index]
                     : 0u;
        };
    item.diffuse_tex =
        texture_object(piece.diffuse_texture);
    item.water_normal =
        texture_object(piece.normal_texture);
    item.hair_alpha_tex =
        texture_object(piece.alpha_texture);
    item.spec_tex =
        texture_object(piece.spec_texture);

    nrhi::Buffer* vb = AcquireMeshUploadBuffer(
        context.device, vertex_bytes);
    nrhi::Buffer* ib = AcquireMeshUploadBuffer(
        context.device, index_bytes);
    if (vb == nullptr || ib == nullptr) {
      PoolMeshBuffer(context.device, vb);
      PoolMeshBuffer(context.device, ib);
      return false;
    }
    void* vb_mapping = context.device->Map(vb);
    void* ib_mapping = context.device->Map(ib);
    if (vb_mapping == nullptr || ib_mapping == nullptr) {
      if (vb_mapping != nullptr) {
        context.device->Unmap(vb);
      }
      if (ib_mapping != nullptr) {
        context.device->Unmap(ib);
      }
      PoolMeshBuffer(context.device, vb);
      PoolMeshBuffer(context.device, ib);
      return false;
    }
    std::memcpy(
        vb_mapping, vertex_bytes_source, vertex_bytes);
    std::memcpy(
        ib_mapping, index_bytes_source, index_bytes);
    context.device->Unmap(vb);
    context.device->Unmap(ib);
    auto old_mesh = g_r.meshes.find(item.mesh);
    if (old_mesh != g_r.meshes.end()) {
      PoolMeshBuffer(context.device, old_mesh->second.vb);
      PoolMeshBuffer(context.device, old_mesh->second.ib);
      g_r.meshes.erase(old_mesh);
    }
    MeshBuffers buffers;
    buffers.vb = vb;
    buffers.ib = ib;
    buffers.vb_view = {
        vb, 0, static_cast<uint32_t>(vertex_bytes), 56};
    buffers.ib_view = {
        ib, 0, static_cast<uint32_t>(index_bytes)};
    buffers.fingerprint = item.fingerprint;
    buffers.raytracing_verts.resize(
        size_t(piece.vertex_count) * 14);
    std::memcpy(
        buffers.raytracing_verts.data(),
        vertex_bytes_source, vertex_bytes);
    buffers.raytracing_indices.resize(
        piece.index_count);
    std::memcpy(
        buffers.raytracing_indices.data(),
        index_bytes_source, index_bytes);
    if (item.ropa) {
      buffers.ropa_verts =
          buffers.raytracing_verts;
    }
    g_r.meshes.emplace(item.mesh, std::move(buffers));
    installed.items.push_back(std::move(item));
  }
  if (cursor != bytes.size()) {
    return false;
  }
  if (state.identity != 0 &&
      state.identity != installed.identity) {
    for (uint64_t old_store_key :
         state.texture_store_keys) {
      const auto old_texture =
          g_r.tex_store.find(old_store_key);
      if (old_texture != g_r.tex_store.end()) {
        RetireGuestTexture(
            old_texture->second,
            context.device->CurrentSubmission());
        g_r.tex_store.erase(old_texture);
      }
      for (auto route = g_r.tex_routes.begin();
           route != g_r.tex_routes.end();) {
        if (route->second.key == old_store_key) {
          route = g_r.tex_routes.erase(route);
        } else {
          ++route;
        }
      }
    }
    std::unordered_set<uint32_t> installed_meshes;
    installed_meshes.reserve(installed.items.size());
    for (const DrawItem& item : installed.items) {
      installed_meshes.insert(item.mesh);
    }
    for (const DrawItem& old_item : state.items) {
      if (installed_meshes.contains(old_item.mesh)) {
        continue;
      }
      const auto old_mesh =
          g_r.meshes.find(old_item.mesh);
      if (old_mesh != g_r.meshes.end()) {
        PoolMeshBuffer(
            context.device, old_mesh->second.vb);
        PoolMeshBuffer(
            context.device, old_mesh->second.ib);
        g_r.meshes.erase(old_mesh);
      }
    }
  }
  BuildRemoteAppearanceRenderCaches(role, installed);
  state = std::move(installed);
  LogRemoteAppearanceResourceAudit(
      "legacy-install", role);
  REXLOG_INFO(
      "multiplayer: installed appearance role={} id={:016X} "
      "pieces={} textures={}",
      role, appearance.identity, state.items.size(),
      texture_objects.size());
  return true;
}

void DrawSandboxMap(const NativeGuestOutputRenderContext& context,
                    nrhi::Cmd* cmd, const FrameScene& scene,
                    uint64_t frame_number, bool use_depth,
                    bool shadow_ready,
                    std::vector<DrawItem>* multiplayer_remote_items) {
  if (multiplayer_remote_items != nullptr) {
    multiplayer_remote_items->clear();
  }
  if (!mechanics_sandbox::VisualMapEnabled() || cmd == nullptr) {
    return;
  }
  mechanics_sandbox::ObserveSandboxCamera(scene.cam_pos);
  float origin[3] = {};
  if (!mechanics_sandbox::SandboxMapRenderOrigin(origin)) {
    static std::atomic<uint32_t> s_origin_log{0};
    if (s_origin_log.fetch_add(1, std::memory_order_relaxed) == 0) {
      REXLOG_WARN("native-scene: sandbox map draw skipped; origin unavailable");
    }
    return;
  }
  static std::atomic<uint32_t> s_map_transform_log{0};
  if (s_map_transform_log.fetch_add(1, std::memory_order_relaxed) == 0) {
    REXLOG_INFO("native-scene: sandbox map draw transform origin=({:.3f},{:.3f},{:.3f}) "
                "camera=({:.3f},{:.3f},{:.3f})",
                origin[0], origin[1], origin[2], scene.cam_pos[0],
                scene.cam_pos[1], scene.cam_pos[2]);
  }
  // Render in the same verified board-origin frame used by owned collision.
  // The old camera-relative probe made a huge clipped triangle appear in the
  // sky and visually disconnected the park from its collision geometry.
  float constants[52] = {};
  const auto set_world_translation =
      [&constants, &scene](float x, float y, float z,
                           float scale = 1.0f) {
        std::fill(constants, constants + 32, 0.0f);
        constants[0] = scale;
        constants[5] = scale;
        constants[10] = scale;
        constants[15] = 1.0f;
        constants[12] = x;
        constants[13] = y;
        constants[14] = z;
        for (int r = 0; r < 4; ++r) {
          for (int c = 0; c < 4; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
              sum += constants[r * 4 + k] *
                     scene.view_proj[k * 4 + c];
            }
            constants[16 + r * 4 + c] = sum;
          }
        }
      };
  const auto set_world_basis =
      [&constants, &scene](
          const skate::world::Vec3& x_axis,
          const skate::world::Vec3& y_axis,
          const skate::world::Vec3& z_axis,
          const skate::world::Vec3& translation) {
        std::fill(constants, constants + 32, 0.0f);
        constants[0] = x_axis.x;
        constants[1] = x_axis.y;
        constants[2] = x_axis.z;
        constants[4] = y_axis.x;
        constants[5] = y_axis.y;
        constants[6] = y_axis.z;
        constants[8] = z_axis.x;
        constants[9] = z_axis.y;
        constants[10] = z_axis.z;
        constants[12] = translation.x;
        constants[13] = translation.y;
        constants[14] = translation.z;
        constants[15] = 1.0f;
        for (int row = 0; row < 4; ++row) {
          for (int column = 0; column < 4; ++column) {
            float sum = 0.0f;
            for (int inner = 0; inner < 4; ++inner) {
              sum += constants[row * 4 + inner] *
                     scene.view_proj[inner * 4 + column];
            }
            constants[16 + row * 4 + column] = sum;
          }
        }
      };
  set_world_translation(origin[0], origin[1], origin[2]);
  constants[36] = scene.cam_pos[0];
  constants[37] = scene.cam_pos[1];
  constants[38] = scene.cam_pos[2];
  constants[39] = -41.0f;
  const skate::world::DayNightState celestial =
      mechanics_sandbox::map::ActiveDayNightState();
  constants[40] = celestial.light_direction_to_light.x;
  constants[41] = celestial.light_direction_to_light.y;
  constants[42] = celestial.light_direction_to_light.z;
  constants[43] = celestial.ambient;
  constants[44] = celestial.light_color.x;
  constants[45] = celestial.light_color.y;
  constants[46] = celestial.light_color.z;
  constants[47] = celestial.light_intensity;

  cmd->SetBindingLayout(g_r.layout);
  cmd->SetPipeline(use_depth ? g_r.pso : g_r.pso_nodepth);
  cmd->SetRootConstants(0, 52, constants, 0);
  cmd->SetBufferSrv(3, g_r.bone_ring, 0);
  cmd->SetTexture(1, g_r.white.srv);
  cmd->SetTexture(2, g_r.white.srv);
  cmd->SetTexture(4, g_r.white.srv);
  cmd->SetTexture(5, g_r.white.srv);
  cmd->SetTexturePair(
      7, g_r.white_cube.srv,
      shadow_ready ? g_r.shadow_srv_final : g_r.white.srv);
  // Keep both static sun maps on t10/t11 while the owned-map renderer
  // changes t8/t9 material resources.
  nrhi::TextureView* t8_default[5] = {
      g_r.white.srv, g_r.white.srv,
      g_r.owned_nsm_active ? g_r.owned_static_sun_srv[0]
                           : (g_r.static_sun_valid ? g_r.static_sun_srv
                                                   : g_r.white.srv),
      g_r.owned_nsm_active ? g_r.owned_static_sun_srv[1] : g_r.white.srv,
      g_r.owned_nsm_active ? g_r.owned_static_sun_srv[2] : g_r.white.srv};
  cmd->SetTextures(8, t8_default, 5);
  cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);

  const mechanics_sandbox::map::VisualWorld& world =
      mechanics_sandbox::map::ActiveVisualWorld();
  constexpr float kDetailRenderDistance = 768.0f;
  const float local_camera_x = scene.cam_pos[0] - origin[0];
  const float local_camera_z = scene.cam_pos[2] - origin[2];
  const int32_t camera_cell_x = static_cast<int32_t>(
      std::floor(local_camera_x / world.chunk_size));
  const int32_t camera_cell_z = static_cast<int32_t>(
      std::floor(local_camera_z / world.chunk_size));
  const int32_t cell_radius = static_cast<int32_t>(
      std::ceil(kDetailRenderDistance / world.chunk_size));
  uint32_t candidate_chunks = 0;
  uint32_t visible_chunks = 0;
  uint32_t draw_calls = 0;
  const auto draw_chunk = [&](std::size_t chunk_index) {
    const mechanics_sandbox::map::VisualChunk& chunk =
        world.chunks[chunk_index];
    ++candidate_chunks;
    float corners[8][3];
    for (int corner = 0; corner < 8; ++corner) {
      corners[corner][0] =
          origin[0] + ((corner & 1) ? chunk.bounds_max[0]
                                    : chunk.bounds_min[0]);
      corners[corner][1] =
          origin[1] + ((corner & 2) ? chunk.bounds_max[1]
                                    : chunk.bounds_min[1]);
      corners[corner][2] =
          origin[2] + ((corner & 4) ? chunk.bounds_max[2]
                                    : chunk.bounds_min[2]);
    }
    if (CornersOutsideFrustum(corners, scene.view_proj, 1.05f)) {
      return;
    }
    ++visible_chunks;
    if (!EnsureSandboxMapChunk(context.device, chunk_index)) {
      return;
    }
    const auto mesh =
        g_r.meshes.find(SandboxMapMeshKey(chunk_index));
    if (mesh == g_r.meshes.end()) {
      return;
    }
    mesh->second.last_used_frame = frame_number;
    cmd->SetVertexBuffer(
        mesh->second.vb_view.buffer, mesh->second.vb_view.offset,
        mesh->second.vb_view.size_bytes, mesh->second.vb_view.stride);
    cmd->SetIndexBuffer(
        mesh->second.ib_view.buffer, mesh->second.ib_view.offset,
        mesh->second.ib_view.size_bytes);
    // Material colours remain authored beside collision properties, while
    // spatial submission is now independent per visible chunk.
    for (const mechanics_sandbox::map::VisualDraw& draw : chunk.draws) {
      const bool imported =
          draw.albedo_texture != 0 || draw.indirect_lightmap != 0 ||
          draw.normal_texture != 0 || draw.orm_texture != 0 ||
          draw.emissive_texture != 0;
      const bool alpha_blend =
          draw.alpha_mode ==
          skate::world::SurfaceMaterial::AlphaMode::Blend;
      cmd->SetPipeline(
          alpha_blend && g_r.pso_transparent != nullptr
              ? g_r.pso_transparent
              : (use_depth ? g_r.pso : g_r.pso_nodepth));
      cmd->SetTexture(
          1, ResolveOwnedMapTexture(context, draw.albedo_texture));
      cmd->SetTexture(
          2, ResolveOwnedMapTexture(
                 context, draw.indirect_lightmap, OwnedTextureRole::Data));
      cmd->SetTexturePair(
          5,
          ResolveOwnedMapTexture(context, draw.emissive_texture),
          ResolveOwnedMapTexture(
              context, draw.normal_texture, OwnedTextureRole::Normal));
      nrhi::TextureView* owned_material_maps[5] = {
          g_r.white.srv,
          ResolveOwnedMapTexture(
              context, draw.orm_texture, OwnedTextureRole::Data),
          g_r.owned_nsm_active ? g_r.owned_static_sun_srv[0]
                               : g_r.white.srv,
          g_r.owned_nsm_active ? g_r.owned_static_sun_srv[1]
                               : g_r.white.srv,
          g_r.owned_nsm_active ? g_r.owned_static_sun_srv[2]
                               : g_r.white.srv};
      cmd->SetTextures(8, owned_material_maps, 5);
      constants[32] = draw.color[0];
      constants[33] = draw.color[1];
      constants[34] = draw.color[2];
      constants[35] = imported ? -draw.alpha_cutoff : 0.0f;
      std::uint32_t owned_flags = 1u;
      owned_flags |=
          static_cast<std::uint32_t>(draw.alpha_mode) << 1u;
      owned_flags |= draw.normal_texture != 0 ? 8u : 0u;
      owned_flags |= draw.orm_texture != 0 ? 16u : 0u;
      owned_flags |= draw.emissive_texture != 0 ? 32u : 0u;
      owned_flags |= draw.indirect_lightmap != 0 ? 64u : 0u;
      constants[48] =
          imported ? -static_cast<float>(owned_flags) : draw.material[0];
      constants[49] =
          imported ? draw.baked_indirect_strength : draw.material[1];
      constants[50] = draw.material[2];
      constants[51] =
          imported
              ? (draw.material[2] < 0.0f ? -draw.material[2] : 0.0f)
              : draw.material[3];
      cmd->SetRootConstants(0, 52, constants, 0);
      cmd->DrawIndexed(draw.index_count, draw.first_index, 0);
      ++draw_calls;
    }
  };
  // The cell table is sorted by (x,z), allowing a bounded detail-radius
  // lookup instead of scanning every chunk in a continent-sized map.
  for (int32_t cell_x = camera_cell_x - cell_radius;
       cell_x <= camera_cell_x + cell_radius; ++cell_x) {
    for (int32_t cell_z = camera_cell_z - cell_radius;
         cell_z <= camera_cell_z + cell_radius; ++cell_z) {
      const auto cell = std::lower_bound(
          world.cells.begin(), world.cells.end(),
          std::pair<int32_t, int32_t>{cell_x, cell_z},
          [](const mechanics_sandbox::map::VisualCellRange& left,
             const std::pair<int32_t, int32_t>& right) {
            return std::pair<int32_t, int32_t>{
                       left.cell_x, left.cell_z} < right;
          });
      if (cell == world.cells.end() || cell->cell_x != cell_x ||
          cell->cell_z != cell_z) {
        continue;
      }
      for (std::size_t offset = 0; offset < cell->chunk_count; ++offset) {
        draw_chunk(cell->first_chunk + offset);
      }
    }
  }

  // Advance and upload the project-owned heightfield. The solver itself is
  // fixed-step and renderer-independent; wall-clock time is only accumulated
  // here because this is the single render-thread owner of its GPU snapshot.
  static auto water_clock = std::chrono::steady_clock::now();
  const auto water_now = std::chrono::steady_clock::now();
  const float water_frame_seconds =
      std::chrono::duration<float>(water_now - water_clock).count();
  water_clock = water_now;
  if (mechanics_sandbox::map::AdvanceWaterSimulation(
          water_frame_seconds)) {
    const mechanics_sandbox::map::VisualMesh& water =
        mechanics_sandbox::map::ActiveWaterVisualMesh();
    if (UpdateSandboxDynamicVisualMesh(
            context.device, kSandboxWaterMesh, water,
            "simulated water")) {
      const auto water_mesh = g_r.meshes.find(kSandboxWaterMesh);
      if (water_mesh != g_r.meshes.end()) {
        set_world_translation(origin[0], origin[1], origin[2]);
        constants[39] = -43.0f;
        water_mesh->second.last_used_frame = frame_number;
        cmd->SetVertexBuffer(
            water_mesh->second.vb_view.buffer,
            water_mesh->second.vb_view.offset,
            water_mesh->second.vb_view.size_bytes,
            water_mesh->second.vb_view.stride);
        cmd->SetIndexBuffer(
            water_mesh->second.ib_view.buffer,
            water_mesh->second.ib_view.offset,
            water_mesh->second.ib_view.size_bytes);
        for (const mechanics_sandbox::map::VisualDraw& draw :
             water.draws) {
          constants[32] = draw.color[0];
          constants[33] = draw.color[1];
          constants[34] = draw.color[2];
          constants[35] = 0.0f;
          constants[48] = 0.0f;
          constants[49] = 0.0f;
          constants[50] = 0.0f;
          constants[51] = 0.0f;
          cmd->SetRootConstants(0, 52, constants, 0);
          cmd->DrawIndexed(
              draw.index_count, draw.first_index, 0);
          ++draw_calls;
        }
      }
    }

    float pusher_position[3] = {};
    const mechanics_sandbox::map::VisualMesh& pusher =
        mechanics_sandbox::map::ActiveWaterPusherVisualMesh();
    if (mechanics_sandbox::map::ActiveWaterPusherPose(
            pusher_position) &&
        EnsureSandboxVisualMesh(
            context.device, kSandboxWaterPusherMesh, pusher,
            "water pusher")) {
      const auto pusher_mesh =
          g_r.meshes.find(kSandboxWaterPusherMesh);
      if (pusher_mesh != g_r.meshes.end()) {
        set_world_translation(
            origin[0] + pusher_position[0],
            origin[1] + pusher_position[1],
            origin[2] + pusher_position[2]);
        constants[39] = -41.25f;
        pusher_mesh->second.last_used_frame = frame_number;
        cmd->SetVertexBuffer(
            pusher_mesh->second.vb_view.buffer,
            pusher_mesh->second.vb_view.offset,
            pusher_mesh->second.vb_view.size_bytes,
            pusher_mesh->second.vb_view.stride);
        cmd->SetIndexBuffer(
            pusher_mesh->second.ib_view.buffer,
            pusher_mesh->second.ib_view.offset,
            pusher_mesh->second.ib_view.size_bytes);
        for (const mechanics_sandbox::map::VisualDraw& draw :
             pusher.draws) {
          constants[32] = draw.color[0];
          constants[33] = draw.color[1];
          constants[34] = draw.color[2];
          constants[35] = 0.0f;
          constants[48] = draw.material[0];
          constants[49] = draw.material[1];
          constants[50] = draw.material[2];
          constants[51] = draw.material[3];
          cmd->SetRootConstants(0, 52, constants, 0);
          cmd->DrawIndexed(
              draw.index_count, draw.first_index, 0);
          ++draw_calls;
        }
      }
    }
    set_world_translation(origin[0], origin[1], origin[2]);
    constants[39] = -41.0f;
  }

  // Kinematic objects use the pose most recently committed to both native
  // collision buffers. They are separate GPU meshes so their vertices never
  // need to be rebuilt or uploaded as they move.
  const skate::world::MapDefinition& definition =
      mechanics_sandbox::map::ActiveDefinition();
  for (std::size_t object_index = 0;
       object_index < definition.kinematic_boxes.size();
       ++object_index) {
    float position[3] = {};
    float velocity[3] = {};
    if (!native_collision::KinematicObjectPose(
            object_index, position, velocity)) {
      continue;
    }
    const skate::world::KinematicBox& object =
        definition.kinematic_boxes[object_index];
    const float world_position[3] = {
        origin[0] + position[0],
        origin[1] + position[1],
        origin[2] + position[2],
    };
    float corners[8][3];
    for (int corner = 0; corner < 8; ++corner) {
      corners[corner][0] =
          world_position[0] +
          ((corner & 1) ? object.local_max.x
                        : object.local_min.x);
      corners[corner][1] =
          world_position[1] +
          ((corner & 2) ? object.local_max.y
                        : object.local_min.y);
      corners[corner][2] =
          world_position[2] +
          ((corner & 4) ? object.local_max.z
                        : object.local_min.z);
    }
    if (CornersOutsideFrustum(corners, scene.view_proj, 1.05f) ||
        !EnsureSandboxKinematicMesh(
            context.device, object_index)) {
      continue;
    }
    const auto mesh = g_r.meshes.find(
        kSandboxKinematicMeshBase +
        static_cast<uint32_t>(object_index));
    if (mesh == g_r.meshes.end()) {
      continue;
    }

    set_world_translation(world_position[0], world_position[1],
                          world_position[2]);
    // -41.25 keeps the owned material family while asking the pixel shader
    // to derive procedural coordinates from this object's local frame.
    // Static map draws remain at -41.0 and retain world-aligned materials.
    constants[39] = -41.25f;
    mesh->second.last_used_frame = frame_number;
    cmd->SetVertexBuffer(
        mesh->second.vb_view.buffer, mesh->second.vb_view.offset,
        mesh->second.vb_view.size_bytes, mesh->second.vb_view.stride);
    cmd->SetIndexBuffer(
        mesh->second.ib_view.buffer, mesh->second.ib_view.offset,
        mesh->second.ib_view.size_bytes);
    for (const mechanics_sandbox::map::VisualDraw& draw :
         mechanics_sandbox::map::ActiveKinematicVisualMesh(
             object_index).draws) {
      constants[32] = draw.color[0];
      constants[33] = draw.color[1];
      constants[34] = draw.color[2];
      constants[35] = 0.0f;
      constants[48] = draw.material[0];
      constants[49] = draw.material[1];
      constants[50] = draw.material[2];
      constants[51] = draw.material[3];
      cmd->SetRootConstants(0, 52, constants, 0);
      cmd->DrawIndexed(draw.index_count, draw.first_index, 0);
      ++draw_calls;
    }
    constants[39] = -41.0f;
  }

  // Blender-authored doors are separate rigid leaves. The same hinge angle
  // published after native collision is used here, so the textured door and
  // its collision volume cannot follow different timelines.
  const auto rotate_about_axis = [](
      skate::world::Vec3 value, skate::world::Vec3 axis, float angle) {
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    return value * cosine +
           skate::world::Cross(axis, value) * sine +
           axis * (skate::world::Dot(axis, value) * (1.0f - cosine));
  };
  for (std::size_t door_index = 0;
       door_index < definition.hinged_doors.size(); ++door_index) {
    float angle = 0.0f;
    float angular_velocity = 0.0f;
    if (!native_collision::HingedDoorPose(
            door_index, &angle, &angular_velocity)) {
      continue;
    }
    (void)angular_velocity;
    const skate::world::HingedDoor& door =
        definition.hinged_doors[door_index];
    const skate::world::Vec3 x_axis = rotate_about_axis(
        door.closed_width_axis, door.hinge_axis, angle);
    const skate::world::Vec3 z_axis = rotate_about_axis(
        door.closed_depth_axis, door.hinge_axis, angle);
    const skate::world::Vec3 translation{
        origin[0] + door.hinge_position.x,
        origin[1] + door.hinge_position.y,
        origin[2] + door.hinge_position.z,
    };
    float corners[8][3];
    for (int corner = 0; corner < 8; ++corner) {
      const float local_x =
          (corner & 1) ? door.local_max.x : door.local_min.x;
      const float local_y =
          (corner & 2) ? door.local_max.y : door.local_min.y;
      const float local_z =
          (corner & 4) ? door.local_max.z : door.local_min.z;
      const skate::world::Vec3 point =
          translation + x_axis * local_x +
          door.hinge_axis * local_y + z_axis * local_z;
      corners[corner][0] = point.x;
      corners[corner][1] = point.y;
      corners[corner][2] = point.z;
    }
    if (CornersOutsideFrustum(corners, scene.view_proj, 1.05f) ||
        !EnsureSandboxHingedDoorMesh(context.device, door_index)) {
      continue;
    }
    const auto mesh = g_r.meshes.find(
        kSandboxHingedDoorMeshBase +
        static_cast<std::uint32_t>(door_index));
    if (mesh == g_r.meshes.end()) {
      continue;
    }
    set_world_basis(
        x_axis, door.hinge_axis, z_axis, translation);
    constants[39] = -41.0f;
    mesh->second.last_used_frame = frame_number;
    cmd->SetVertexBuffer(
        mesh->second.vb_view.buffer, mesh->second.vb_view.offset,
        mesh->second.vb_view.size_bytes, mesh->second.vb_view.stride);
    cmd->SetIndexBuffer(
        mesh->second.ib_view.buffer, mesh->second.ib_view.offset,
        mesh->second.ib_view.size_bytes);
    for (const mechanics_sandbox::map::VisualDraw& draw :
         mechanics_sandbox::map::ActiveHingedDoorVisualMesh(
             door_index).draws) {
      const bool imported =
          draw.albedo_texture != 0 || draw.indirect_lightmap != 0 ||
          draw.normal_texture != 0 || draw.orm_texture != 0 ||
          draw.emissive_texture != 0;
      const bool alpha_blend =
          draw.alpha_mode ==
          skate::world::SurfaceMaterial::AlphaMode::Blend;
      cmd->SetPipeline(
          alpha_blend && g_r.pso_transparent != nullptr
              ? g_r.pso_transparent
              : (use_depth ? g_r.pso : g_r.pso_nodepth));
      cmd->SetTexture(
          1, ResolveOwnedMapTexture(context, draw.albedo_texture));
      cmd->SetTexture(
          2, ResolveOwnedMapTexture(
                 context, draw.indirect_lightmap,
                 OwnedTextureRole::Data));
      cmd->SetTexturePair(
          5,
          ResolveOwnedMapTexture(context, draw.emissive_texture),
          ResolveOwnedMapTexture(
              context, draw.normal_texture,
              OwnedTextureRole::Normal));
      nrhi::TextureView* door_material_maps[5] = {
          g_r.white.srv,
          ResolveOwnedMapTexture(
              context, draw.orm_texture, OwnedTextureRole::Data),
          g_r.owned_nsm_active ? g_r.owned_static_sun_srv[0]
                               : g_r.white.srv,
          g_r.owned_nsm_active ? g_r.owned_static_sun_srv[1]
                               : g_r.white.srv,
          g_r.owned_nsm_active ? g_r.owned_static_sun_srv[2]
                               : g_r.white.srv};
      cmd->SetTextures(8, door_material_maps, 5);
      constants[32] = draw.color[0];
      constants[33] = draw.color[1];
      constants[34] = draw.color[2];
      constants[35] = imported ? -draw.alpha_cutoff : 0.0f;
      std::uint32_t owned_flags = 1u;
      owned_flags |=
          static_cast<std::uint32_t>(draw.alpha_mode) << 1u;
      owned_flags |= draw.normal_texture != 0 ? 8u : 0u;
      owned_flags |= draw.orm_texture != 0 ? 16u : 0u;
      owned_flags |= draw.emissive_texture != 0 ? 32u : 0u;
      owned_flags |= draw.indirect_lightmap != 0 ? 64u : 0u;
      constants[48] =
          imported ? -static_cast<float>(owned_flags)
                   : draw.material[0];
      constants[49] =
          imported ? draw.baked_indirect_strength
                   : draw.material[1];
      constants[50] = draw.material[2];
      constants[51] =
          imported
              ? (draw.material[2] < 0.0f
                     ? -draw.material[2]
                     : 0.0f)
              : draw.material[3];
      cmd->SetRootConstants(0, 52, constants, 0);
      cmd->DrawIndexed(draw.index_count, draw.first_index, 0);
      ++draw_calls;
    }
  }
  cmd->SetPipeline(use_depth ? g_r.pso : g_r.pso_nodepth);
  cmd->SetTexture(1, g_r.white.srv);
  cmd->SetTexture(2, g_r.white.srv);
  cmd->SetTexturePair(5, g_r.white.srv, g_r.white.srv);
  cmd->SetTextures(8, t8_default, 5);
  set_world_translation(origin[0], origin[1], origin[2]);

  const auto multiplayer_perf_t0 = PerfClock::now();
  // Capture the local player's final rendered skeleton. This is downstream
  // of Skate 3's animation graph, IK and trick evaluation, so the network
  // layer replicates the pose the sender actually displayed instead of
  // trying to reconstruct animation state from gameplay flags.
  multiplayer::AnimationPose local_animation;
  local_animation.sender_time_us = scene.sample_time_us;
  bool local_animation_root_anchored = false;
  std::vector<const DrawItem*> local_player_items;
  float local_board_position[3] = {};
  const uint32_t local_presentation =
      mechanics_sandbox::LocalPresentationCandidate();
  std::size_t local_capture_skater_items = 0;
  std::size_t local_capture_exact_items = 0;
  std::size_t local_capture_rejected_items = 0;
  std::size_t local_capture_far_exact_items = 0;
  float local_capture_maximum_exact_distance = 0.0f;
  trick_pipeline::LiveSpatialSnapshot local_spatial;
  const bool have_local_spatial =
      trick_pipeline::CurrentLiveSpatialSnapshot(
          local_spatial);
  const bool local_skater_offboard =
      have_local_spatial &&
      local_spatial.board_state_flags != 0xFFFFFFFFu &&
      (local_spatial.board_state_flags & 0x7u) != 0;
  const bool have_local_board_position =
      trick_pipeline::CurrentLocalBoardPosition(
          local_board_position);
  if (have_local_board_position) {
    std::memcpy(
        local_animation.root_position, local_board_position,
        sizeof(local_animation.root_position));
    for (const DrawItem& item : scene.items) {
      if (!item.dyn_entity || item.pending || item.retained ||
          item.ctx == 0) {
        continue;
      }
      native_entity::CtxInfo identity;
      if (!native_entity::LookupCtx(item.ctx, &identity)) {
        continue;
      }
      const bool skater_family =
          identity.cls == native_entity::EntClass::kSkater ||
          identity.cls == native_entity::EntClass::kColorized ||
          identity.cls == native_entity::EntClass::kCac ||
          identity.cls == native_entity::EntClass::kSkaterAux;
      if (!skater_family) {
        continue;
      }
      ++local_capture_skater_items;
      const bool exact_local_presentation =
          local_presentation != 0 &&
          identity.entity == local_presentation;
      local_capture_exact_items +=
          exact_local_presentation ? 1u : 0u;
      float anchor[3] = {
          item.world[12], item.world[13], item.world[14]};
      if (item.bones.size() >= 12) {
        anchor[0] = item.bones[3];
        anchor[1] = item.bones[7];
        anchor[2] = item.bones[11];
      }
      const float dx = anchor[0] - local_board_position[0];
      const float dy = anchor[1] - local_board_position[1];
      const float dz = anchor[2] - local_board_position[2];
      const bool finite_distance =
          std::isfinite(dx) && std::isfinite(dy) &&
          std::isfinite(dz);
      const float distance_squared =
          finite_distance
              ? dx * dx + dy * dy + dz * dz
              : std::numeric_limits<float>::infinity();
      bool detached_board_piece = false;
      if (local_skater_offboard && finite_distance &&
          distance_squared >
              multiplayer::capture::
                  kFallbackBoardRadiusSquared &&
          item.skinned) {
        native_palette::CanonicalRigSample board_rig;
        detached_board_piece =
            native_palette::LookupCanonicalRig(
                item.ctx, item.mesh, board_rig) &&
            std::any_of(
                board_rig.palette_to_canonical.begin(),
                board_rig.palette_to_canonical.end(),
                [](std::size_t canonical_bone) {
                  return canonical_bone >= 25 &&
                         canonical_bone <= 31;
                });
      }
      if (exact_local_presentation && finite_distance &&
          distance_squared >
              multiplayer::capture::
                  kFallbackBoardRadiusSquared) {
        ++local_capture_far_exact_items;
        local_capture_maximum_exact_distance =
            std::max(
                local_capture_maximum_exact_distance,
                std::sqrt(distance_squared));
      }
      if (!multiplayer::capture::LocalPresentationPieceOwned(
              local_presentation, identity.entity,
              finite_distance, distance_squared,
              detached_board_piece)) {
        ++local_capture_rejected_items;
        continue;
      }
      if (detached_board_piece && local_presentation == 0) {
        static bool s_logged_detached_board_bypass = false;
        if (!s_logged_detached_board_bypass) {
          s_logged_detached_board_bypass = true;
          REXLOG_INFO(
              "multiplayer-detached-board-capture: "
              "mesh={:08X} distance={:.3f} "
              "ownership_filter=bypassed",
              item.mesh, std::sqrt(distance_squared));
        }
      }
      local_player_items.push_back(&item);
      if (!item.skinned || item.bones.empty() ||
          item.bones.size() % 12 != 0) {
        continue;
      }
      native_palette::CanonicalRigSample rig;
      if (!native_palette::LookupCanonicalRig(
              item.ctx, item.mesh, rig) ||
          rig.canonical_rows.empty() ||
          rig.canonical_rows.size() % 12 != 0 ||
          rig.palette_to_canonical.empty()) {
        continue;
      }
      if (!local_animation.presentation_root_valid) {
        const std::size_t palette_bones =
            std::min(
                item.bones.size() / 12,
                rig.palette_to_canonical.size());
        for (std::size_t palette_bone = 0;
             palette_bone < palette_bones; ++palette_bone) {
          if (rig.palette_to_canonical[palette_bone] != 0) {
            continue;
          }
          const float* root =
              item.bones.data() + palette_bone * 12;
          float x_axis[3] = {
              root[0], root[4], root[8]};
          float z_axis[3] = {
              root[2], root[6], root[10]};
          const auto normalize_axis = [](float axis[3]) {
            const float length_squared =
                axis[0] * axis[0] +
                axis[1] * axis[1] +
                axis[2] * axis[2];
            if (!std::isfinite(length_squared) ||
                length_squared < 1.0e-8f) {
              return false;
            }
            const float inverse_length =
                1.0f / std::sqrt(length_squared);
            axis[0] *= inverse_length;
            axis[1] *= inverse_length;
            axis[2] *= inverse_length;
            return true;
          };
          if (!normalize_axis(x_axis)) {
            continue;
          }
          const float projection =
              z_axis[0] * x_axis[0] +
              z_axis[1] * x_axis[1] +
              z_axis[2] * x_axis[2];
          for (std::size_t component = 0;
               component < 3; ++component) {
            z_axis[component] -=
                x_axis[component] * projection;
          }
          const float root_position[3] = {
              root[3], root[7], root[11]};
          const float dx =
              root_position[0] - local_board_position[0];
          const float dy =
              root_position[1] - local_board_position[1];
          const float dz =
              root_position[2] - local_board_position[2];
          if (!normalize_axis(z_axis) ||
              !std::isfinite(dx) || !std::isfinite(dy) ||
              !std::isfinite(dz) ||
              dx * dx + dy * dy + dz * dz > 16.0f) {
            continue;
          }
          std::copy_n(
              root_position, 3,
              local_animation.presentation_root_position);
          // Bone rows and their translation reference must describe the
          // same immutable scene sample. Using the independently sampled
          // physics-board position here makes the whole skeleton jump by
          // the capture-time difference on each animation update.
          std::copy_n(
              root_position, 3,
              local_animation.root_position);
          std::copy_n(
              x_axis, 3,
              local_animation.presentation_root_x_axis);
          std::copy_n(
              z_axis, 3,
              local_animation.presentation_root_z_axis);
          local_animation.root_bone = 0;
          local_animation.presentation_root_valid = true;
          break;
        }
      }
      // UpdateBoneTransforms' source palette is the canonical skeleton.
      // Resolve the source Matrix44 convention against the exact final
      // local palette. Different image paths expose either row-vector
      // (translation in row 3) or column-vector (translation in column 3)
      // storage; choosing the lower-error interpretation here is a layout
      // conversion, not a positional correction.
      const bool rig_has_canonical_root =
          std::find(
              rig.palette_to_canonical.begin(),
              rig.palette_to_canonical.end(),
              std::size_t{0}) !=
          rig.palette_to_canonical.end();
      if (local_animation.tracks.empty() ||
          (!local_animation_root_anchored &&
           rig_has_canonical_root)) {
        const auto layout_error =
            [&item, &rig](const std::vector<float>& rows) {
              if (rows.empty() || rows.size() % 12 != 0) {
                return std::numeric_limits<double>::infinity();
              }
              const std::size_t canonical_bones =
                  rows.size() / 12;
              const std::size_t palette_bones =
                  std::min(
                      item.bones.size() / 12,
                      rig.palette_to_canonical.size());
              double error = 0.0;
              std::size_t compared = 0;
              for (std::size_t palette_bone = 0;
                   palette_bone < palette_bones;
                   ++palette_bone) {
                const std::size_t canonical_bone =
                    rig.palette_to_canonical[palette_bone];
                if (canonical_bone >= canonical_bones) {
                  continue;
                }
                const float* candidate =
                    rows.data() + canonical_bone * 12;
                const float* rendered =
                    item.bones.data() + palette_bone * 12;
                for (std::size_t component = 0;
                     component < 12; ++component) {
                  if (!std::isfinite(candidate[component]) ||
                      !std::isfinite(rendered[component])) {
                    return std::numeric_limits<double>::infinity();
                  }
                  const double difference =
                      static_cast<double>(candidate[component]) -
                      static_cast<double>(rendered[component]);
                  error += difference * difference;
                  ++compared;
                }
              }
              return compared == 0
                         ? std::numeric_limits<double>::infinity()
                         : error / double(compared);
            };
        const double row_vector_error =
            layout_error(rig.canonical_rows);
        const double column_vector_error =
            layout_error(
                rig.canonical_rows_column_vector);
        const bool use_column_vector = false;
        std::vector<float> canonical_world;
        std::size_t transform_anchor =
            std::numeric_limits<std::size_t>::max();
        const auto transform_to_world =
            [&item, &rig, &transform_anchor](
                const std::vector<float>& local_rows,
                std::vector<float>& world_rows) {
              const std::size_t canonical_bones =
                  local_rows.size() / 12;
              const std::size_t palette_bones =
                  std::min(
                      item.bones.size() / 12,
                      rig.palette_to_canonical.size());
              float local_to_world[12] = {};
              bool valid = false;
              for (std::size_t pass = 0;
                   pass < 2 && !valid; ++pass) {
                for (std::size_t palette_bone = 0;
                     palette_bone < palette_bones;
                     ++palette_bone) {
                  const std::size_t canonical_bone =
                      rig.palette_to_canonical[palette_bone];
                  const bool canonical_root = canonical_bone == 0;
                  if ((pass == 0 && !canonical_root) ||
                      (pass == 1 && canonical_root) ||
                      canonical_bone >= canonical_bones) {
                    continue;
                  }
                  const float* local =
                      local_rows.data() + canonical_bone * 12;
                  const float determinant =
                      local[0] *
                              (local[5] * local[10] -
                               local[6] * local[9]) -
                      local[1] *
                              (local[4] * local[10] -
                               local[6] * local[8]) +
                      local[2] *
                              (local[4] * local[9] -
                               local[5] * local[8]);
                  if (!std::isfinite(determinant) ||
                      std::fabs(determinant) < 1.0e-8f) {
                    continue;
                  }
                  const float d = 1.0f / determinant;
                  const float inverse[9] = {
                      (local[5] * local[10] -
                       local[6] * local[9]) * d,
                      (local[2] * local[9] -
                       local[1] * local[10]) * d,
                      (local[1] * local[6] -
                       local[2] * local[5]) * d,
                      (local[6] * local[8] -
                       local[4] * local[10]) * d,
                      (local[0] * local[10] -
                       local[2] * local[8]) * d,
                      (local[2] * local[4] -
                       local[0] * local[6]) * d,
                      (local[4] * local[9] -
                       local[5] * local[8]) * d,
                      (local[1] * local[8] -
                       local[0] * local[9]) * d,
                      (local[0] * local[5] -
                       local[1] * local[4]) * d};
                  const float* rendered =
                      item.bones.data() + palette_bone * 12;
                  for (std::size_t row = 0; row < 3; ++row) {
                    for (std::size_t column = 0;
                         column < 3; ++column) {
                      local_to_world[row * 4 + column] = 0.0f;
                      for (std::size_t inner = 0;
                           inner < 3; ++inner) {
                        local_to_world[row * 4 + column] +=
                            rendered[row * 4 + inner] *
                            inverse[inner * 3 + column];
                      }
                    }
                    local_to_world[row * 4 + 3] =
                        rendered[row * 4 + 3];
                    for (std::size_t inner = 0;
                         inner < 3; ++inner) {
                      local_to_world[row * 4 + 3] -=
                          local_to_world[row * 4 + inner] *
                          local[inner * 4 + 3];
                    }
                  }
                  transform_anchor = canonical_bone;
                  valid = true;
                  break;
                }
              }
              if (!valid) {
                return false;
              }
              world_rows.resize(local_rows.size());
              for (std::size_t bone = 0;
                   bone < canonical_bones; ++bone) {
                const float* local =
                    local_rows.data() + bone * 12;
                float* world =
                    world_rows.data() + bone * 12;
                for (std::size_t row = 0; row < 3; ++row) {
                  for (std::size_t column = 0;
                       column < 3; ++column) {
                    world[row * 4 + column] = 0.0f;
                    for (std::size_t inner = 0;
                         inner < 3; ++inner) {
                      world[row * 4 + column] +=
                          local_to_world[row * 4 + inner] *
                          local[inner * 4 + column];
                    }
                  }
                  world[row * 4 + 3] =
                      local_to_world[row * 4 + 3];
                  for (std::size_t inner = 0;
                       inner < 3; ++inner) {
                    world[row * 4 + 3] +=
                        local_to_world[row * 4 + inner] *
                        local[inner * 4 + 3];
                  }
                }
              }
              return true;
            };
        if (!transform_to_world(
                rig.canonical_rows, canonical_world)) {
          continue;
        }
        if (!local_animation.presentation_root_valid &&
            transform_anchor !=
                std::numeric_limits<std::size_t>::max()) {
          const auto anchor_it = std::find(
              rig.palette_to_canonical.begin(),
              rig.palette_to_canonical.end(),
              transform_anchor);
          const std::size_t palette_anchor =
              static_cast<std::size_t>(
                  anchor_it -
                  rig.palette_to_canonical.begin());
          if (anchor_it != rig.palette_to_canonical.end() &&
              (palette_anchor + 1) * 12 <= item.bones.size()) {
            const float* root =
                item.bones.data() + palette_anchor * 12;
            float x_axis[3] = {
                root[0], root[4], root[8]};
            float z_axis[3] = {
                root[2], root[6], root[10]};
            const auto normalize_axis = [](float axis[3]) {
              const float length_squared =
                  axis[0] * axis[0] +
                  axis[1] * axis[1] +
                  axis[2] * axis[2];
              if (!std::isfinite(length_squared) ||
                  length_squared < 1.0e-8f) {
                return false;
              }
              const float inverse_length =
                  1.0f / std::sqrt(length_squared);
              axis[0] *= inverse_length;
              axis[1] *= inverse_length;
              axis[2] *= inverse_length;
              return true;
            };
            if (normalize_axis(x_axis)) {
              const float projection =
                  z_axis[0] * x_axis[0] +
                  z_axis[1] * x_axis[1] +
                  z_axis[2] * x_axis[2];
              for (std::size_t component = 0;
                   component < 3; ++component) {
                z_axis[component] -=
                    x_axis[component] * projection;
              }
              const float root_position[3] = {
                  root[3], root[7], root[11]};
              if (normalize_axis(z_axis) &&
                  std::isfinite(root_position[0]) &&
                  std::isfinite(root_position[1]) &&
                  std::isfinite(root_position[2])) {
                std::copy_n(
                    root_position, 3,
                    local_animation
                        .presentation_root_position);
                std::copy_n(
                    root_position, 3,
                    local_animation.root_position);
                std::copy_n(
                    x_axis, 3,
                    local_animation
                        .presentation_root_x_axis);
                std::copy_n(
                    z_axis, 3,
                    local_animation
                        .presentation_root_z_axis);
                local_animation.root_bone =
                    static_cast<std::uint16_t>(
                        transform_anchor);
                local_animation.presentation_root_valid = true;
              }
            }
          }
        }
        static std::atomic<std::uint32_t>
            s_canonical_layout_logs{0};
        if (s_canonical_layout_logs.fetch_add(
                1, std::memory_order_relaxed) < 4) {
          REXLOG_INFO(
              "multiplayer: canonical Matrix44 layout={} "
              "row_error={:.6f} column_error={:.6f} "
              "mesh={:08X} anchor={} "
              "item_world=({:.6f},{:.6f},{:.6f}) "
              "animation_root=({:.6f},{:.6f},{:.6f})",
              use_column_vector ? "column-vector"
                                : "row-vector",
              row_vector_error, column_vector_error,
              item.mesh, transform_anchor,
              item.world[12], item.world[13], item.world[14],
              local_animation.root_position[0],
              local_animation.root_position[1],
              local_animation.root_position[2]);
        }
        local_animation.tracks.clear();
        local_animation.tracks.push_back(
            {multiplayer::kCanonicalSkeletonTrackKey,
             std::move(canonical_world)});
        local_animation_root_anchored =
            rig_has_canonical_root;
      }
    }
  }

  {
    const bool have_local_animation =
        !local_animation.tracks.empty();
    static int s_previous_capture_state = -1;
    static bool s_far_exact_episode = false;
    const int capture_state = have_local_animation ? 1 : 0;
    if (capture_state != s_previous_capture_state) {
      REXLOG_INFO(
          "multiplayer-local-capture: state={} candidate={:08X} "
          "board={} scene_items={} skater_items={} exact_items={} "
          "accepted_items={} rejected_items={} far_exact_items={} "
          "max_exact_distance={:.3f} tracks={}",
          have_local_animation ? "ready" : "missing",
          local_presentation,
          have_local_board_position ? 1 : 0,
          scene.items.size(), local_capture_skater_items,
          local_capture_exact_items, local_player_items.size(),
          local_capture_rejected_items,
          local_capture_far_exact_items,
          local_capture_maximum_exact_distance,
          local_animation.tracks.size());
      s_previous_capture_state = capture_state;
    }
    const bool far_exact_episode =
        local_capture_far_exact_items != 0;
    if (far_exact_episode && !s_far_exact_episode) {
      REXLOG_INFO(
          "multiplayer-local-capture: exact entity accepted beyond "
          "fallback board radius candidate={:08X} items={} "
          "max_distance={:.3f}",
          local_presentation, local_capture_far_exact_items,
          local_capture_maximum_exact_distance);
    }
    s_far_exact_episode = far_exact_episode;
  }

  // Bind-skinned ROPA garments do not expose a live palette of their own.
  // Replace each canonical row represented by a current final player piece
  // with the exact row used for that displayed frame. LookupCanonicalRig now
  // supplies the current canonical source first, so rows not represented by
  // an ordinary piece remain on the same simulation sample instead of mixing
  // current body rows with a previous-frame shirt skeleton.
  if (!local_animation.tracks.empty() &&
      local_animation.tracks.front().mesh_key ==
          multiplayer::kCanonicalSkeletonTrackKey) {
    std::vector<float>& canonical =
        local_animation.tracks.front().bone_rows;
    const std::size_t canonical_bones =
        canonical.size() / 12;
    for (const DrawItem* item : local_player_items) {
      if (item == nullptr || !item->skinned ||
          item->bones.empty() ||
          item->bones.size() % 12 != 0) {
        continue;
      }
      native_palette::CanonicalRigSample rig;
      if (!native_palette::LookupCanonicalRig(
              item->ctx, item->mesh, rig) ||
          rig.palette_to_canonical.empty()) {
        continue;
      }
      const std::size_t palette_bones =
          std::min(
              item->bones.size() / 12,
              rig.palette_to_canonical.size());
      for (std::size_t palette_bone = 0;
           palette_bone < palette_bones; ++palette_bone) {
        const std::size_t canonical_bone =
            rig.palette_to_canonical[palette_bone];
        if (canonical_bone >= canonical_bones) {
          continue;
        }
        const float* source =
            item->bones.data() + palette_bone * 12;
        float basis_energy = 0.0f;
        bool finite = true;
        for (std::size_t component = 0;
             component < 12; ++component) {
          finite =
              finite && std::isfinite(source[component]);
          if (component != 3 && component != 7 &&
              component != 11) {
            basis_energy +=
                source[component] * source[component];
          }
        }
        if (!finite || basis_energy <= 0.01f) {
          continue;
        }
        std::copy_n(
            source, 12,
            canonical.begin() + canonical_bone * 12);
      }
    }
  }

  // Send the final rendered palette for each ordinary skinned piece alongside
  // the coherent canonical fallback. Exact sender-owned appearance tracks
  // take precedence on the receiver.
  if (!local_animation.tracks.empty()) {
    for (const DrawItem* item : local_player_items) {
      if (item == nullptr || !item->skinned ||
          item->bones.empty() ||
          item->bones.size() % 12 != 0) {
        continue;
      }
      std::size_t weighted_rows = 0;
      {
        std::lock_guard<std::mutex> lock(
            g_skin_probe_mutex);
        const auto probe = g_skin_probe.find(item->mesh);
        if (probe != g_skin_probe.end()) {
          for (const SkinProbeSample& sample :
               probe->second.s) {
            for (std::size_t influence = 0;
                 influence < 4; ++influence) {
              const std::uint8_t weight =
                  static_cast<std::uint8_t>(
                      (sample.bw >>
                       (influence * 8)) &
                      0xFFu);
              const std::uint8_t bone =
                  static_cast<std::uint8_t>(
                      (sample.bi >>
                       (influence * 8)) &
                      0xFFu);
              if (weight != 0) {
                weighted_rows = std::max(
                    weighted_rows,
                    std::size_t(bone) + 1);
              }
            }
          }
        }
      }
      if (weighted_rows == 0) {
        continue;
      }
      weighted_rows = std::min(
          weighted_rows, item->bones.size() / 12);
      // Capture every live final palette as an exact track. The canonical
      // hierarchy remains in the packet for bind-skinned ROPA and fallback,
      // but a sender-owned appearance piece should consume the exact matrix
      // rows that produced the sender's displayed frame. This includes a
      // detached board: its final palette follows the real throw trajectory,
      // while the separately sampled board-controller body remains beside the
      // walking skater.
      multiplayer::AnimationTrack exact_track;
      exact_track.mesh_key = item->mesh;
      exact_track.bone_rows.assign(
          item->bones.begin(),
          item->bones.begin() +
              weighted_rows * 12);
      local_animation.tracks.push_back(
          std::move(exact_track));
    }
  }
  RecordLocalCaptureCadence(local_animation);

  // One-shot audit of the rows the character meshes actually weight against
  // versus the canonical remap currently used for multiplayer. This exposes
  // attachment/board pieces whose compact remap is being applied at the
  // wrong destination row without altering their presentation.
  {
    static bool s_logged_multiplayer_rig_audit = false;
    if (!s_logged_multiplayer_rig_audit &&
        !local_animation.tracks.empty() &&
        local_animation.tracks.front().mesh_key ==
            multiplayer::kCanonicalSkeletonTrackKey) {
      s_logged_multiplayer_rig_audit = true;
      const std::vector<float>& canonical =
          local_animation.tracks.front().bone_rows;
      const std::size_t canonical_bones = canonical.size() / 12;
      for (const DrawItem* item : local_player_items) {
        native_palette::CanonicalRigSample rig;
        const bool have_rig =
            item != nullptr &&
            native_palette::LookupCanonicalRig(
                item->ctx, item->mesh, rig) &&
            !rig.palette_to_canonical.empty();
        std::array<bool, 256> weighted{};
        {
          std::lock_guard<std::mutex> lock(g_skin_probe_mutex);
          const auto probe = g_skin_probe.find(item->mesh);
          if (probe != g_skin_probe.end()) {
            for (const SkinProbeSample& sample :
                 probe->second.s) {
              for (std::size_t influence = 0;
                   influence < 4; ++influence) {
                const std::uint8_t weight =
                    static_cast<std::uint8_t>(
                        (sample.bw >>
                         (influence * 8)) &
                        0xFFu);
                const std::uint8_t bone =
                    static_cast<std::uint8_t>(
                        (sample.bi >>
                         (influence * 8)) &
                        0xFFu);
                if (weight != 0) {
                  weighted[bone] = true;
                }
              }
            }
          }
        }
        std::size_t weighted_count = 0;
        std::size_t mapped_weighted = 0;
        std::size_t minimum_weighted = weighted.size();
        std::size_t maximum_weighted = 0;
        double translation_error_squared = 0.0;
        double maximum_translation_error = 0.0;
        std::string weighted_mapping;
        for (std::size_t bone = 0;
             bone < weighted.size(); ++bone) {
          if (!weighted[bone]) {
            continue;
          }
          ++weighted_count;
          minimum_weighted =
              std::min(minimum_weighted, bone);
          maximum_weighted =
              std::max(maximum_weighted, bone);
          if (!weighted_mapping.empty()) {
            weighted_mapping += ",";
          }
          weighted_mapping += std::to_string(bone);
          weighted_mapping += "->";
          if (!have_rig ||
              bone >= rig.palette_to_canonical.size() ||
              bone >= item->bones.size() / 12) {
            weighted_mapping += "x";
            continue;
          }
          const std::size_t canonical_bone =
              rig.palette_to_canonical[bone];
          weighted_mapping +=
              std::to_string(canonical_bone);
          if (canonical_bone >= canonical_bones) {
            continue;
          }
          ++mapped_weighted;
          const float* expected =
              item->bones.data() + bone * 12;
          const float* reconstructed =
              canonical.data() + canonical_bone * 12;
          const double dx =
              double(reconstructed[3]) - expected[3];
          const double dy =
              double(reconstructed[7]) - expected[7];
          const double dz =
              double(reconstructed[11]) - expected[11];
          const double error =
              std::sqrt(dx * dx + dy * dy + dz * dz);
          translation_error_squared += error * error;
          maximum_translation_error =
              std::max(maximum_translation_error, error);
        }
        const double translation_rms =
            mapped_weighted == 0
                ? -1.0
                : std::sqrt(
                      translation_error_squared /
                      double(mapped_weighted));
        REXLOG_INFO(
            "multiplayer-rig-audit: mesh={:08X} fam={} shadow={} "
            "palette={} remap={} weighted={} range={}-{} mapped={} "
            "translation_rms={:.4f} max={:.4f} map=[{}]",
            item->mesh, item->char_family,
            item->shadow_caster ? 1 : 0,
            item->bones.size() / 12,
            have_rig ? rig.palette_to_canonical.size() : 0,
            weighted_count,
            weighted_count == 0 ? 0 : minimum_weighted,
            weighted_count == 0 ? 0 : maximum_weighted,
            mapped_weighted, translation_rms,
            maximum_translation_error, weighted_mapping);
      }
    }
  }

  const auto local_appearance_t0 = PerfClock::now();
  g_pw_mp_local_pose.Add(uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          local_appearance_t0 - multiplayer_perf_t0)
          .count()));
  const multiplayer::AppearanceBlob local_appearance =
      BuildLocalAppearanceBlob(
          context, local_player_items, local_animation);
  g_pw_mp_local_appearance.Add(
      PerfNsSince(local_appearance_t0));
  multiplayer::RemotePresentationFrame remote_presentation;
  const auto multiplayer_tick_t0 = PerfClock::now();
  const bool have_remote_players =
      multiplayer::TickLocalVisuals(
          mechanics_sandbox::map::ActiveMapName(), origin,
          local_animation.tracks.empty() &&
                  !local_animation.presentation_root_valid
              ? nullptr
              : &local_animation,
          local_appearance.identity != 0 &&
                  local_appearance.bytes != nullptr
              ? &local_appearance
              : nullptr,
          remote_presentation);
  RecordRemotePresentationHandoff(remote_presentation);
  g_pw_mp_tick.Add(PerfNsSince(multiplayer_tick_t0));
  const auto multiplayer_remote_t0 = PerfClock::now();
  uint64_t multiplayer_install_ns = 0;
  for (const multiplayer::RemotePeerRetirement& retirement :
       remote_presentation.retirements) {
    ReleaseRetiredRemoteAppearance(context, retirement);
  }
  if (have_remote_players &&
      remote_presentation.players != nullptr) {
  for (const multiplayer::RemotePlayer& remote_player :
       *remote_presentation.players) {
    const multiplayer::RemotePose& remote_pose = remote_player.pose;
    const multiplayer::AnimationPose& remote_animation =
        remote_player.animation;
    RecordRemoteRenderMotion(
        remote_player.role, remote_player.session,
        remote_animation);
    const std::size_t remote_item_start =
        multiplayer_remote_items == nullptr
            ? 0
            : multiplayer_remote_items->size();
    bool replicated_real_skater = false;
    // Never substitute the receiver's local outfit for a remote player.
    // Besides making both skaters look identical, wardrobe transitions can
    // expose a partial local piece set and duplicate malformed body parts.
    // Use the simple proxy until the sender-owned appearance is complete.
    const std::vector<DrawItem>* appearance_items = nullptr;
    RemoteAppearanceRenderState*
        remote_appearance_state = nullptr;
    const auto multiplayer_install_t0 = PerfClock::now();
    ObserveRemoteAppearanceSession(
        context, remote_player.role, remote_player.session);
    const bool appearance_installed =
        InstallRemoteAppearance(
            context, frame_number, remote_player.role,
            remote_player.session,
            remote_player.appearance);
    multiplayer_install_ns +=
        PerfNsSince(multiplayer_install_t0);
    if (appearance_installed) {
      const auto installed =
          g_remote_appearances.find(remote_player.role);
      if (installed != g_remote_appearances.end() &&
          !installed->second.items.empty()) {
        if (!installed->second.installation_reported ||
            installed->second.installation_reported_session !=
                remote_player.session) {
          multiplayer::ReportRemoteAppearanceInstalled(
              remote_player.role,
              remote_player.session,
              remote_player.appearance.identity);
          installed->second.installation_reported_session =
              remote_player.session;
          installed->second.installation_reported = true;
        }
        remote_appearance_state =
            &installed->second;
        appearance_items = &installed->second.items;
      }
    }
    if (multiplayer_remote_items != nullptr &&
        appearance_items != nullptr &&
        !appearance_items->empty()) {
      trick_pipeline::LiveSpatialSnapshot local_spatial;
      const bool have_presentation_root =
          local_animation.presentation_root_valid;
      const bool have_local_root =
          have_presentation_root ||
          trick_pipeline::CurrentLiveSpatialSnapshot(
              local_spatial);
      if (have_local_root) {
        float local_board[16] = {};
        float remote_board[16] = {};
        const auto load_axis =
            [&local_spatial](std::size_t axis, std::size_t component) {
              return std::bit_cast<float>(
                  axis == 0
                      ? local_spatial.x_axis_bits[component]
                      : local_spatial.z_axis_bits[component]);
            };
        float local_x[3] = {
            have_presentation_root
                ? local_animation
                      .presentation_root_x_axis[0]
                : load_axis(0, 0),
            have_presentation_root
                ? local_animation
                      .presentation_root_x_axis[1]
                : load_axis(0, 1),
            have_presentation_root
                ? local_animation
                      .presentation_root_x_axis[2]
                : load_axis(0, 2)};
        float local_z[3] = {
            have_presentation_root
                ? local_animation
                      .presentation_root_z_axis[0]
                : load_axis(1, 0),
            have_presentation_root
                ? local_animation
                      .presentation_root_z_axis[1]
                : load_axis(1, 1),
            have_presentation_root
                ? local_animation
                      .presentation_root_z_axis[2]
                : load_axis(1, 2)};
        float local_y[3] = {
            local_z[1] * local_x[2] - local_z[2] * local_x[1],
            local_z[2] * local_x[0] - local_z[0] * local_x[2],
            local_z[0] * local_x[1] - local_z[1] * local_x[0]};
        const float local_position[3] = {
            have_presentation_root
                ? local_animation
                      .presentation_root_position[0]
                : std::bit_cast<float>(
                      local_spatial.position_bits[0]),
            have_presentation_root
                ? local_animation
                      .presentation_root_position[1]
                : std::bit_cast<float>(
                      local_spatial.position_bits[1]),
            have_presentation_root
                ? local_animation
                      .presentation_root_position[2]
                : std::bit_cast<float>(
                      local_spatial.position_bits[2])};
        const float remote_position[3] = {
            origin[0] + remote_pose.position[0],
            origin[1] + remote_pose.position[1],
            origin[2] + remote_pose.position[2]};
        const float* local_axes[3] = {local_x, local_y, local_z};
        const float* remote_axes[3] = {
            remote_pose.x_axis, remote_pose.y_axis,
            remote_pose.z_axis};
        float bone_space_rotation[3][3] = {};
        float bone_space_translation[3] = {};
        for (std::size_t row = 0; row < 3; ++row) {
          for (std::size_t column = 0; column < 3; ++column) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
              bone_space_rotation[row][column] +=
                  remote_axes[axis][row] *
                  local_axes[axis][column];
            }
          }
          bone_space_translation[row] = remote_position[row];
          for (std::size_t column = 0; column < 3; ++column) {
            bone_space_translation[row] -=
                bone_space_rotation[row][column] *
                local_position[column];
          }
        }
        for (std::size_t row = 0; row < 3; ++row) {
          for (std::size_t column = 0; column < 3; ++column) {
            local_board[row * 4 + column] =
                local_axes[row][column];
            remote_board[row * 4 + column] =
                remote_axes[row][column];
          }
        }
        local_board[12] = local_position[0];
        local_board[13] = local_position[1];
        local_board[14] = local_position[2];
        local_board[15] = 1.0f;
        remote_board[12] = remote_position[0];
        remote_board[13] = remote_position[1];
        remote_board[14] = remote_position[2];
        remote_board[15] = 1.0f;

        float inverse_local[16] = {};
        inverse_local[15] = 1.0f;
        for (std::size_t row = 0; row < 3; ++row) {
          for (std::size_t column = 0; column < 3; ++column) {
            inverse_local[row * 4 + column] =
                local_board[column * 4 + row];
          }
        }
        for (std::size_t column = 0; column < 3; ++column) {
          inverse_local[12 + column] =
              -(local_position[0] * inverse_local[column] +
                local_position[1] * inverse_local[4 + column] +
                local_position[2] * inverse_local[8 + column]);
        }
        const auto multiply4 = [](const float left[16],
                                  const float right[16],
                                  float out[16]) {
          float result[16] = {};
          for (std::size_t row = 0; row < 4; ++row) {
            for (std::size_t column = 0; column < 4; ++column) {
              for (std::size_t inner = 0; inner < 4; ++inner) {
                result[row * 4 + column] +=
                    left[row * 4 + inner] *
                    right[inner * 4 + column];
              }
            }
          }
          std::memcpy(out, result, sizeof(result));
        };
        float local_to_remote[16] = {};
        multiply4(inverse_local, remote_board, local_to_remote);
        float rigid_to_remote[16] = {};
        std::memcpy(
            rigid_to_remote, local_to_remote,
            sizeof(rigid_to_remote));
        bool streamed_rigid_transform_valid =
            remote_appearance_state == nullptr;
        if (remote_appearance_state != nullptr) {
          float source_x[3] = {
              remote_appearance_state->root_x_axis[0],
              remote_appearance_state->root_x_axis[1],
              remote_appearance_state->root_x_axis[2]};
          float source_z[3] = {
              remote_appearance_state->root_z_axis[0],
              remote_appearance_state->root_z_axis[1],
              remote_appearance_state->root_z_axis[2]};
          const auto normalize_source_axis =
              [](float axis[3]) {
                const float length_squared =
                    axis[0] * axis[0] +
                    axis[1] * axis[1] +
                    axis[2] * axis[2];
                if (!std::isfinite(length_squared) ||
                    length_squared < 1.0e-8f) {
                  return false;
                }
                const float inverse_length =
                    1.0f / std::sqrt(length_squared);
                axis[0] *= inverse_length;
                axis[1] *= inverse_length;
                axis[2] *= inverse_length;
                return true;
              };
          const bool source_x_valid =
              normalize_source_axis(source_x);
          if (source_x_valid) {
            const float projection =
                source_z[0] * source_x[0] +
                source_z[1] * source_x[1] +
                source_z[2] * source_x[2];
            for (std::size_t component = 0;
                 component < 3; ++component) {
              source_z[component] -=
                  source_x[component] * projection;
            }
          }
          const float* source_position =
              remote_appearance_state->root_position;
          if (source_x_valid &&
              normalize_source_axis(source_z) &&
              std::isfinite(source_position[0]) &&
              std::isfinite(source_position[1]) &&
              std::isfinite(source_position[2])) {
            const float source_y[3] = {
                source_z[1] * source_x[2] -
                    source_z[2] * source_x[1],
                source_z[2] * source_x[0] -
                    source_z[0] * source_x[2],
                source_z[0] * source_x[1] -
                    source_z[1] * source_x[0]};
            const float* source_axes[3] = {
                source_x, source_y, source_z};
            float inverse_source[16] = {};
            inverse_source[15] = 1.0f;
            for (std::size_t row = 0; row < 3; ++row) {
              for (std::size_t column = 0;
                   column < 3; ++column) {
                inverse_source[row * 4 + column] =
                    source_axes[column][row];
              }
            }
            for (std::size_t column = 0;
                 column < 3; ++column) {
              inverse_source[12 + column] =
                  -(source_position[0] *
                        inverse_source[column] +
                    source_position[1] *
                        inverse_source[4 + column] +
                    source_position[2] *
                        inverse_source[8 + column]);
            }
            multiply4(
                inverse_source, remote_board,
                rigid_to_remote);
            streamed_rigid_transform_valid = true;
          }
        }

        bool canonical_track_cache_hit = false;
        const multiplayer::AnimationTrack* canonical_track =
            multiplayer::render_cache::FindAnimationTrack(
                remote_animation.tracks,
                multiplayer::kCanonicalSkeletonTrackKey,
                remote_appearance_state
                    ->canonical_track_index,
                &canonical_track_cache_hit);
        // Animation-track rows are already interpolated model-to-world
        // skinning affines in the receiver's map space. Do not align them
        // again to the independently smoothed pose root: that second
        // rotation affects every track, so even a detached skateboard
        // rotates in lockstep with the skater as the two streams advance.
        // The pose root remains authoritative for the simple proxy and
        // non-skinned fallback pieces only.

        multiplayer_remote_items->reserve(
            multiplayer_remote_items->size() +
            appearance_items->size());
        std::size_t remapped_skinned_items = 0;
        std::size_t network_animated_bones = 0;
        std::size_t palette_bone_count = 0;
        std::size_t network_candidate_bones = 0;
        std::size_t network_invalid_bones = 0;
        std::size_t network_unresolved_active_bones = 0;
        std::size_t skipped_skinned_items = 0;
        std::size_t cached_track_hits =
            canonical_track_cache_hit ? 1 : 0;
        std::size_t track_scans =
            canonical_track_cache_hit ? 0 : 1;
        std::size_t runtime_weighted_scans = 0;
        std::size_t runtime_rig_retries = 0;
        std::uint64_t applied_palette_hash =
            1469598103934665603ull;
        std::size_t applied_palette_bones = 0;
        for (std::size_t appearance_piece_index = 0;
             appearance_piece_index < appearance_items->size();
             ++appearance_piece_index) {
          const DrawItem& source =
              (*appearance_items)[appearance_piece_index];
          RemoteAppearancePieceRenderCache* piece_cache =
              appearance_piece_index <
                      remote_appearance_state
                          ->piece_caches.size()
                  ? &remote_appearance_state
                         ->piece_caches[
                             appearance_piece_index]
                  : nullptr;
          DrawItem clone = source;
          clone.selected = false;
          clone.retained = false;
          clone.pending = false;
          clone.lw_alpha = -1.0f;
          bool remapped = false;
          bool unresolved_active_row = false;
          if (clone.skinned && !clone.bones.empty()) {
            const std::size_t palette_bones =
                clone.bones.size() / 12;
            AppearanceWeightedRows
                fallback_weighted_rows;
            if (piece_cache == nullptr) {
              const auto installed_mesh =
                  g_r.meshes.find(clone.mesh);
              if (installed_mesh != g_r.meshes.end()) {
                fallback_weighted_rows =
                    multiplayer::render_cache::
                        FindWeightedPaletteRows(
                            installed_mesh->second
                                .raytracing_verts,
                            palette_bones);
                ++runtime_weighted_scans;
              }
            }
            const AppearanceWeightedRows& weighted_rows =
                piece_cache != nullptr
                    ? piece_cache->weighted_rows
                    : fallback_weighted_rows;
            const uint32_t track_key =
                piece_cache != nullptr
                    ? piece_cache->track_key
                    : (clone.multiplayer_track_key != 0
                           ? clone.multiplayer_track_key
                           : clone.mesh);
            bool exact_track_cache_hit = false;
            std::size_t uncached_track_index =
                multiplayer::render_cache::
                    kInvalidAnimationTrackIndex;
            std::size_t& exact_track_index =
                piece_cache != nullptr
                    ? piece_cache->exact_track_index
                    : uncached_track_index;
            const multiplayer::AnimationTrack*
                exact_track =
                    multiplayer::render_cache::
                        FindAnimationTrack(
                            remote_animation.tracks,
                            track_key,
                            exact_track_index,
                            &exact_track_cache_hit);
            if (exact_track_cache_hit) {
              ++cached_track_hits;
            } else {
              ++track_scans;
            }
            std::vector<uint16_t> fallback_remap;
            if (piece_cache == nullptr) {
              fallback_remap.assign(
                  clone
                      .multiplayer_palette_to_canonical
                      .begin(),
                  clone
                      .multiplayer_palette_to_canonical
                      .end());
              if (fallback_remap.empty()) {
                native_palette::CanonicalRigSample rig;
                if (native_palette::LookupCanonicalRig(
                        clone.ctx, clone.mesh, rig)) {
                  fallback_remap =
                      std::move(
                          rig.palette_to_canonical);
                }
              }
            } else if (
                !piece_cache->rig_lookup_complete) {
              ++runtime_rig_retries;
              ResolveRemoteAppearanceRigCache(
                  clone, *piece_cache);
            }
            const std::vector<uint16_t>&
                palette_to_canonical =
                    piece_cache != nullptr
                        ? piece_cache
                              ->palette_to_canonical
                        : fallback_remap;
            const bool have_rig =
                !palette_to_canonical.empty();
            const std::size_t canonical_bones =
                canonical_track == nullptr
                    ? 0
                    : canonical_track->bone_rows.size() / 12;
            const bool skateboard_piece =
                piece_cache != nullptr
                    ? piece_cache->skateboard_piece
                    : have_rig &&
                          std::any_of(
                              palette_to_canonical.begin(),
                              palette_to_canonical.end(),
                              [](std::size_t canonical_bone) {
                                return canonical_bone >= 25 &&
                                       canonical_bone <= 31;
                              });
            if (skateboard_piece || clone.ropa || clone.hair) {
              const std::uint32_t track_key =
                  clone.multiplayer_track_key != 0
                      ? clone.multiplayer_track_key
                      : clone.mesh;
              const std::uint64_t route_key =
                  (std::uint64_t(remote_player.role) << 32) |
                  track_key;
              static std::mutex s_route_log_mutex;
              static std::unordered_set<std::uint64_t>
                  s_route_logs;
              std::lock_guard<std::mutex> route_lock(
                  s_route_log_mutex);
              if (s_route_logs.insert(route_key).second) {
                const std::size_t mapped_canonical =
                    palette_to_canonical.empty()
                        ? std::numeric_limits<std::size_t>::max()
                        : palette_to_canonical.front();
                const float* exact_root =
                    exact_track != nullptr
                        ? exact_track->bone_rows.data()
                        : nullptr;
                const float* canonical_root =
                    canonical_track != nullptr &&
                            mapped_canonical < canonical_bones
                        ? canonical_track->bone_rows.data() +
                              mapped_canonical * 12
                        : nullptr;
                REXLOG_INFO(
                    "multiplayer-animation-route: role={} "
                    "kind={} rx_mesh={:08X} track={:08X} "
                    "palette={} weighted={} remap_first={} "
                    "alpha_tex={:08X} alpha_scale={:.3f} "
                    "char_valid={} "
                    "exact_bones={} exact_t=({:.3f},{:.3f},{:.3f}) "
                    "canonical_t=({:.3f},{:.3f},{:.3f}) "
                    "anim_root=({:.3f},{:.3f},{:.3f})",
                    remote_player.role,
                    skateboard_piece
                        ? "board"
                        : (clone.hair ? "hair" : "ropa"),
                    clone.mesh, track_key, palette_bones,
                    weighted_rows.count, mapped_canonical,
                    clone.hair_alpha_tex,
                    clone.char_rows[13 * 4 + 3],
                    clone.char_rows[14 * 4 + 1] > 0.0f ? 1 : 0,
                    exact_track == nullptr
                        ? 0
                        : exact_track->bone_rows.size() / 12,
                    exact_root == nullptr ? 0.0f : exact_root[3],
                    exact_root == nullptr ? 0.0f : exact_root[7],
                    exact_root == nullptr ? 0.0f : exact_root[11],
                    canonical_root == nullptr
                        ? 0.0f
                        : canonical_root[3],
                    canonical_root == nullptr
                        ? 0.0f
                        : canonical_root[7],
                    canonical_root == nullptr
                        ? 0.0f
                        : canonical_root[11],
                    remote_animation.root_position[0],
                    remote_animation.root_position[1],
                    remote_animation.root_position[2]);
              }
            }
            remapped = palette_bones != 0;
            palette_bone_count += palette_bones;
            for (std::size_t palette_bone = 0;
                 palette_bone < palette_bones; ++palette_bone) {
              float source_bone[12] = {};
              std::copy_n(
                  clone.bones.begin() + palette_bone * 12,
                  12, source_bone);
              bool used_network_animation = false;
              const float* network_bone = nullptr;
              if (exact_track != nullptr &&
                  palette_bone <
                      exact_track->bone_rows.size() / 12) {
                network_bone =
                    exact_track->bone_rows.data() +
                    palette_bone * 12;
              } else if (
                  canonical_track != nullptr && have_rig &&
                  palette_bone <
                      palette_to_canonical.size()) {
                const std::size_t canonical_bone =
                    palette_to_canonical[palette_bone];
                if (canonical_bone < canonical_bones) {
                  network_bone =
                      canonical_track->bone_rows.data() +
                      canonical_bone * 12;
                }
              }
              if (network_bone != nullptr) {
                  ++network_candidate_bones;
                  float basis_energy = 0.0f;
                  bool finite = true;
                  for (std::size_t component = 0;
                       component < 12; ++component) {
                    finite =
                        finite && std::isfinite(
                                      network_bone[component]);
                    if (component != 3 && component != 7 &&
                        component != 11) {
                      basis_energy +=
                          network_bone[component] *
                          network_bone[component];
                    }
                  }
                  if (!finite || basis_energy <= 0.01f) {
                    ++network_invalid_bones;
                    if (weighted_rows.used[palette_bone]) {
                      unresolved_active_row = true;
                      ++network_unresolved_active_bones;
                    }
                  } else {
                    std::copy_n(
                        network_bone, 12,
                        clone.bones.begin() +
                            palette_bone * 12);
                    used_network_animation = true;
                    ++network_animated_bones;
                  }
              }
              if (network_bone == nullptr &&
                  weighted_rows.used[palette_bone]) {
                unresolved_active_row = true;
                ++network_unresolved_active_bones;
              }
              if (used_network_animation) {
                continue;
              }
              for (std::size_t row = 0; row < 3; ++row) {
                for (std::size_t column = 0;
                     column < 3; ++column) {
                  float value = 0.0f;
                  for (std::size_t inner = 0;
                       inner < 3; ++inner) {
                    value +=
                        bone_space_rotation[row][inner] *
                        source_bone[inner * 4 + column];
                  }
                  clone.bones[
                      palette_bone * 12 + row * 4 + column] =
                      value;
                }
                float translation =
                    bone_space_translation[row];
                for (std::size_t inner = 0;
                     inner < 3; ++inner) {
                  translation +=
                      bone_space_rotation[row][inner] *
                      source_bone[inner * 4 + 3];
                }
                clone.bones[
                    palette_bone * 12 + row * 4 + 3] =
                    translation;
              }
            }
            // A skinned draw is atomic: every palette row referenced by its
            // GPU weights must come from this peer's current animation
            // sample. Rendering a partially resolved mesh with the
            // zero-initialized appearance palette is what produced
            // stretched hair and garments collapsing toward the origin.
            if (unresolved_active_row) {
              ++skipped_skinned_items;
              continue;
            }
          }
          if (remapped) {
            ++remapped_skinned_items;
          } else if (clone.skinned && !clone.bones.empty()) {
            ++skipped_skinned_items;
            continue;
          } else {
            if (!streamed_rigid_transform_valid) {
              continue;
            }
            float transformed[16] = {};
            multiply4(
                clone.world, rigid_to_remote,
                transformed);
            std::memcpy(
                clone.world, transformed, sizeof(clone.world));
          }
          if (clone.skinned && !clone.bones.empty()) {
            applied_palette_hash = AppearanceHashBytes(
                applied_palette_hash, &clone.mesh,
                sizeof(clone.mesh));
            const std::size_t bone_count =
                clone.bones.size() / 12;
            applied_palette_hash = AppearanceHashBytes(
                applied_palette_hash, &bone_count,
                sizeof(bone_count));
            applied_palette_hash = AppearanceHashBytes(
                applied_palette_hash, clone.bones.data(),
                clone.bones.size() * sizeof(float));
            applied_palette_bones += bone_count;
          }
          multiplayer_remote_items->push_back(std::move(clone));
        }
        RecordRemoteAppliedPalette(
            remote_player.role, remote_player.session,
            remote_animation.sequence, applied_palette_hash,
            applied_palette_bones);
        static std::atomic<std::uint32_t> s_replication_logs{0};
        const std::uint32_t log_index =
            s_replication_logs.fetch_add(1, std::memory_order_relaxed);
        if (log_index < 4) {
          REXLOG_INFO(
              "multiplayer: skater pieces local={} remote_tracks={} "
              "network_bones={} remapped_skinned={} skipped_skinned={} "
              "published={}",
              appearance_items->size(),
              remote_animation.tracks.size(),
              network_animated_bones,
              remapped_skinned_items,
              skipped_skinned_items,
              multiplayer_remote_items->size() - remote_item_start);
        }
        struct BoneGateTelemetry {
          std::chrono::steady_clock::time_point last_log{};
          std::uint64_t frames = 0;
          std::uint64_t palette = 0;
          std::uint64_t candidates = 0;
          std::uint64_t used = 0;
          std::uint64_t invalid = 0;
          std::uint64_t unresolved_active = 0;
          std::uint64_t cached_track_hits = 0;
          std::uint64_t track_scans = 0;
          std::uint64_t runtime_weighted_scans = 0;
          std::uint64_t runtime_rig_retries = 0;
          std::size_t minimum_used =
              std::numeric_limits<std::size_t>::max();
          std::size_t maximum_used = 0;
        };
        static BoneGateTelemetry s_bone_gate;
        const auto bone_gate_now =
            std::chrono::steady_clock::now();
        if (s_bone_gate.last_log ==
            std::chrono::steady_clock::time_point{}) {
          s_bone_gate.last_log = bone_gate_now;
        }
        ++s_bone_gate.frames;
        s_bone_gate.palette += palette_bone_count;
        s_bone_gate.candidates += network_candidate_bones;
        s_bone_gate.used += network_animated_bones;
        s_bone_gate.invalid += network_invalid_bones;
        s_bone_gate.unresolved_active +=
            network_unresolved_active_bones;
        s_bone_gate.cached_track_hits +=
            cached_track_hits;
        s_bone_gate.track_scans += track_scans;
        s_bone_gate.runtime_weighted_scans +=
            runtime_weighted_scans;
        s_bone_gate.runtime_rig_retries +=
            runtime_rig_retries;
        s_bone_gate.minimum_used =
            std::min(
                s_bone_gate.minimum_used,
                network_animated_bones);
        s_bone_gate.maximum_used =
            std::max(
                s_bone_gate.maximum_used,
                network_animated_bones);
        if (bone_gate_now - s_bone_gate.last_log >=
            std::chrono::seconds(5)) {
          const double divisor =
              static_cast<double>(
                  std::max<std::uint64_t>(
                      s_bone_gate.frames, 1));
          REXLOG_INFO(
              "multiplayer-bone-gate: frames={} palette={:.1f} "
              "candidates={:.1f} used={:.1f} used_range={}-{} "
              "invalid={:.1f} unresolved_active={:.1f}",
              s_bone_gate.frames,
              static_cast<double>(s_bone_gate.palette) /
                  divisor,
              static_cast<double>(s_bone_gate.candidates) /
                  divisor,
              static_cast<double>(s_bone_gate.used) /
                  divisor,
              s_bone_gate.minimum_used,
              s_bone_gate.maximum_used,
              static_cast<double>(s_bone_gate.invalid) /
                  divisor,
              static_cast<double>(
                  s_bone_gate.unresolved_active) /
                  divisor);
          REXLOG_INFO(
              "multiplayer-render-cache: frames={} "
              "track_hits={:.1f} track_scans={:.1f} "
              "weighted_fallbacks={} rig_retries={}",
              s_bone_gate.frames,
              static_cast<double>(
                  s_bone_gate.cached_track_hits) /
                  divisor,
              static_cast<double>(
                  s_bone_gate.track_scans) /
                  divisor,
              s_bone_gate.runtime_weighted_scans,
              s_bone_gate.runtime_rig_retries);
          s_bone_gate = {};
          s_bone_gate.last_log = bone_gate_now;
        }
        replicated_real_skater =
            multiplayer_remote_items->size() > remote_item_start;
      }
    }

    RemoteVisualTelemetryState& visual_state =
        g_remote_visual_telemetry[remote_player.role];
    if (!visual_state.initialized ||
        visual_state.session != remote_player.session ||
        visual_state.real_skater != replicated_real_skater) {
      visual_state.session = remote_player.session;
      visual_state.real_skater = replicated_real_skater;
      visual_state.initialized = true;
      REXLOG_INFO(
          "multiplayer-visual-state: role={} session={} mode={} "
          "appearance={:016X} animation_tracks={}",
          remote_player.role, remote_player.session,
          replicated_real_skater ? "appearance" : "proxy",
          remote_appearance_state == nullptr
              ? std::uint64_t{0}
              : remote_appearance_state->identity,
          remote_animation.tracks.size());
    }

    // Keep the simple proxy as a startup/packet-loss fallback. It disappears
    // as soon as one complete skeletal frame has arrived.
    if (!replicated_real_skater &&
        EnsureSandboxRemoteSkaterMesh(context.device)) {
    const auto remote_mesh = g_r.meshes.find(kSandboxRemoteSkaterMesh);
    if (remote_mesh != g_r.meshes.end()) {
      const skate::world::Vec3 remote_x{
          remote_pose.x_axis[0], remote_pose.x_axis[1],
          remote_pose.x_axis[2]};
      const skate::world::Vec3 remote_y{
          remote_pose.y_axis[0], remote_pose.y_axis[1],
          remote_pose.y_axis[2]};
      const skate::world::Vec3 remote_z{
          remote_pose.z_axis[0], remote_pose.z_axis[1],
          remote_pose.z_axis[2]};
      const skate::world::Vec3 remote_position{
          origin[0] + remote_pose.position[0],
          origin[1] + remote_pose.position[1],
          origin[2] + remote_pose.position[2]};
      set_world_basis(
          remote_x, remote_y, remote_z, remote_position);
      constants[39] = -41.25f;
      remote_mesh->second.last_used_frame = frame_number;
      cmd->SetVertexBuffer(
          remote_mesh->second.vb_view.buffer,
          remote_mesh->second.vb_view.offset,
          remote_mesh->second.vb_view.size_bytes,
          remote_mesh->second.vb_view.stride);
      cmd->SetIndexBuffer(
          remote_mesh->second.ib_view.buffer,
          remote_mesh->second.ib_view.offset,
          remote_mesh->second.ib_view.size_bytes);
      for (const mechanics_sandbox::map::VisualDraw& draw :
           mechanics_sandbox::map::ActiveRemoteSkaterVisualMesh().draws) {
        constants[32] = draw.color[0];
        constants[33] = draw.color[1];
        constants[34] = draw.color[2];
        constants[35] = 0.0f;
        constants[48] = draw.material[0];
        constants[49] = draw.material[1];
        constants[50] = draw.material[2];
        constants[51] = draw.material[3];
        cmd->SetRootConstants(0, 52, constants, 0);
        cmd->DrawIndexed(draw.index_count, draw.first_index, 0);
        ++draw_calls;
      }
      set_world_translation(origin[0], origin[1], origin[2]);
      constants[39] = -41.0f;
    }
    }
  }
  }
  g_pw_mp_install.Add(multiplayer_install_ns);
  g_pw_mp_remote.Add(
      PerfNsSince(multiplayer_remote_t0));
  g_pw_mp_total.Add(PerfNsSince(multiplayer_perf_t0));

  // The same cached authored poses are consumed later by the DXR dynamic
  // scene. Rendering the emissive source meshes here makes their motion and
  // their illumination visibly inseparable.
  if (mechanics_sandbox::map::ActiveMovingLightCount() != 0 &&
      EnsureSandboxMovingLightMesh(context.device)) {
    const auto light_mesh =
        g_r.meshes.find(kSandboxMovingLightMesh);
    if (light_mesh != g_r.meshes.end()) {
      light_mesh->second.last_used_frame = frame_number;
      cmd->SetVertexBuffer(
          light_mesh->second.vb_view.buffer,
          light_mesh->second.vb_view.offset,
          light_mesh->second.vb_view.size_bytes,
          light_mesh->second.vb_view.stride);
      cmd->SetIndexBuffer(
          light_mesh->second.ib_view.buffer,
          light_mesh->second.ib_view.offset,
          light_mesh->second.ib_view.size_bytes);
      for (std::size_t light_index = 0;
           light_index <
               mechanics_sandbox::map::ActiveMovingLightCount();
           ++light_index) {
        mechanics_sandbox::map::MovingLightSnapshot light;
        if (!mechanics_sandbox::map::ActiveMovingLightSnapshot(
                light_index, light)) {
          continue;
        }
        if (!light.visible_source) {
          continue;
        }
        set_world_translation(
            origin[0] + light.position[0],
            origin[1] + light.position[1],
            origin[2] + light.position[2],
            light.source_radius);
        constants[32] = 1.0f;
        constants[33] = 1.0f;
        constants[34] = 1.0f;
        constants[35] = 0.0f;
        constants[39] = -44.0f;
        constants[40] = light.color[0];
        constants[41] = light.color[1];
        constants[42] = light.color[2];
        constants[43] = 0.0f;
        constants[48] = 0.0f;
        constants[49] = 0.0f;
        constants[50] = 0.0f;
        constants[51] = 0.0f;
        cmd->SetRootConstants(0, 52, constants, 0);
        cmd->DrawIndexed(
            static_cast<uint32_t>(
                mechanics_sandbox::map::
                    ActiveMovingLightVisualMesh().indices.size()),
            0, 0);
        ++draw_calls;
      }
    }
  }

  // Camera-centred storm rain. The mesh is regenerated from the deterministic
  // weather time, then uploaded through the same ring-safe path as water.
  const mechanics_sandbox::map::WeatherSnapshot weather =
      mechanics_sandbox::map::ActiveWeatherSnapshot();
  if (weather.rain_intensity > 0.0f) {
    const float local_camera[3] = {
        scene.cam_pos[0] - origin[0],
        scene.cam_pos[1] - origin[1],
        scene.cam_pos[2] - origin[2],
    };
    mechanics_sandbox::map::UpdateRainVisualMesh(local_camera);
    const mechanics_sandbox::map::VisualMesh& rain =
        mechanics_sandbox::map::ActiveRainVisualMesh();
    if (UpdateSandboxDynamicVisualMesh(
            context.device, kSandboxRainMesh, rain, "storm rain")) {
      const auto mesh = g_r.meshes.find(kSandboxRainMesh);
      if (mesh != g_r.meshes.end()) {
        set_world_translation(origin[0], origin[1], origin[2]);
        constants[32] = 0.62f;
        constants[33] = 0.66f;
        constants[34] = 0.71f;
        constants[35] = 0.0f;
        constants[39] = -45.0f;
        constants[40] = 0.0f;
        constants[41] = 0.0f;
        constants[42] = 0.0f;
        constants[43] = weather.rain_intensity;
        cmd->SetPipeline(g_r.pso_transparent);
        cmd->SetVertexBuffer(
            mesh->second.vb_view.buffer, mesh->second.vb_view.offset,
            mesh->second.vb_view.size_bytes,
            mesh->second.vb_view.stride);
        cmd->SetIndexBuffer(
            mesh->second.ib_view.buffer, mesh->second.ib_view.offset,
            mesh->second.ib_view.size_bytes);
        cmd->SetRootConstants(0, 52, constants, 0);
        cmd->DrawIndexed(
            static_cast<std::uint32_t>(rain.indices.size()), 0, 0);
        ++draw_calls;
      }
    }
  }

  if (weather.flash_intensity > 0.001f &&
      !mechanics_sandbox::map::ActiveLightningVisualMesh().indices.empty() &&
      UpdateSandboxDynamicVisualMesh(
          context.device, kSandboxLightningMesh,
          mechanics_sandbox::map::ActiveLightningVisualMesh(),
          "storm lightning")) {
    const auto mesh = g_r.meshes.find(kSandboxLightningMesh);
    if (mesh != g_r.meshes.end()) {
      set_world_translation(origin[0], origin[1], origin[2]);
      constants[32] = 1.0f;
      constants[33] = 1.0f;
      constants[34] = 1.0f;
      constants[35] = 0.0f;
      constants[39] = -44.0f;
      constants[40] = 0.52f * weather.flash_intensity;
      constants[41] = 0.70f * weather.flash_intensity;
      constants[42] = 1.0f * weather.flash_intensity;
      constants[43] = 0.0f;
      cmd->SetPipeline(g_r.pso_transparent);
      cmd->SetVertexBuffer(
          mesh->second.vb_view.buffer, mesh->second.vb_view.offset,
          mesh->second.vb_view.size_bytes,
          mesh->second.vb_view.stride);
      cmd->SetIndexBuffer(
          mesh->second.ib_view.buffer, mesh->second.ib_view.offset,
          mesh->second.ib_view.size_bytes);
      cmd->SetRootConstants(0, 52, constants, 0);
      cmd->DrawIndexed(
          static_cast<std::uint32_t>(
              mechanics_sandbox::map::ActiveLightningVisualMesh()
                  .indices.size()),
          0, 0);
      ++draw_calls;
    }
  }
  cmd->SetPipeline(use_depth ? g_r.pso : g_r.pso_nodepth);
  constants[39] = -41.0f;

  uint32_t resident_chunks = 0;
  for (std::size_t chunk_index = 0;
       chunk_index < world.chunks.size(); ++chunk_index) {
    resident_chunks +=
        g_r.meshes.contains(SandboxMapMeshKey(chunk_index)) ? 1u : 0u;
  }
  mechanics_sandbox::RecordMapChunks(
      static_cast<uint32_t>(world.chunks.size()), candidate_chunks,
      visible_chunks, resident_chunks, draw_calls);
  mechanics_sandbox::RecordMapDraw(draw_calls != 0);
}

void DrawSandboxSky(const NativeGuestOutputRenderContext& context,
                    nrhi::Cmd* cmd, const FrameScene& scene,
                    uint64_t frame_number) {
  if (!mechanics_sandbox::VisualMapEnabled() || cmd == nullptr ||
      !mechanics_sandbox::map::ActiveDefinition().sky.enabled ||
      !EnsureSandboxSkyMesh(context.device)) {
    return;
  }
  const auto mesh = g_r.meshes.find(kSandboxSkyMesh);
  if (mesh == g_r.meshes.end()) {
    return;
  }
  mesh->second.last_used_frame = frame_number;

  // The owned sphere follows the camera. It has no parallax and does not
  // depend on Skate's sky assets or the authored world's physical extent.
  float constants[52] = {};
  constants[0] = 1.0f;
  constants[5] = 1.0f;
  constants[10] = 1.0f;
  constants[15] = 1.0f;
  constants[12] = scene.cam_pos[0];
  constants[13] = scene.cam_pos[1];
  constants[14] = scene.cam_pos[2];
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) {
      float sum = 0.0f;
      for (int index = 0; index < 4; ++index) {
        sum += constants[row * 4 + index] *
               scene.view_proj[index * 4 + column];
      }
      constants[16 + row * 4 + column] = sum;
    }
  }
  constants[36] = scene.cam_pos[0];
  constants[37] = scene.cam_pos[1];
  constants[38] = scene.cam_pos[2];
  constants[39] = -42.0f;
  const skate::world::DayNightState celestial =
      mechanics_sandbox::map::ActiveDayNightState();
  constants[32] = celestial.sun_direction_to_light.x;
  constants[33] = celestial.sun_direction_to_light.y;
  constants[34] = celestial.sun_direction_to_light.z;
  constants[35] = 0.0f;
  constants[40] = celestial.sky_zenith.x;
  constants[41] = celestial.sky_zenith.y;
  constants[42] = celestial.sky_zenith.z;
  constants[43] = celestial.star_visibility;
  constants[44] = celestial.sky_horizon.x;
  constants[45] = celestial.sky_horizon.y;
  constants[46] = celestial.sky_horizon.z;
  constants[47] = celestial.sun_visibility;
  constants[48] = celestial.sky_nadir.x;
  constants[49] = celestial.sky_nadir.y;
  constants[50] = celestial.sky_nadir.z;
  constants[51] = celestial.moon_visibility;

  cmd->SetBindingLayout(g_r.layout);
  cmd->SetPipeline(g_r.pso_nodepth);
  cmd->SetRootConstants(0, 52, constants, 0);
  cmd->SetBufferSrv(3, g_r.bone_ring, 0);
  cmd->SetTexture(1, g_r.white.srv);
  cmd->SetTexture(2, g_r.white.srv);
  cmd->SetTexture(4, g_r.white.srv);
  cmd->SetTexture(5, g_r.white.srv);
  cmd->SetTexturePair(7, g_r.white_cube.srv, g_r.white.srv);
  nrhi::TextureView* defaults[5] = {
      g_r.white.srv, g_r.white.srv, g_r.white.srv, g_r.white.srv,
      g_r.white.srv};
  cmd->SetTextures(8, defaults, 5);
  cmd->SetVertexBuffer(
      mesh->second.vb_view.buffer, mesh->second.vb_view.offset,
      mesh->second.vb_view.size_bytes, mesh->second.vb_view.stride);
  cmd->SetIndexBuffer(
      mesh->second.ib_view.buffer, mesh->second.ib_view.offset,
      mesh->second.ib_view.size_bytes);
  cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
  uint32_t draw_calls = 0;
  for (const mechanics_sandbox::map::VisualDraw& draw :
       mechanics_sandbox::map::ActiveSkyMesh().draws) {
    cmd->SetRootConstants(0, 52, constants, 0);
    cmd->DrawIndexed(draw.index_count, draw.first_index, 0);
    ++draw_calls;
  }
  mechanics_sandbox::RecordSkyDraw(draw_calls);
}

bool RenderScene(const NativeGuestOutputRenderContext& context, void* /*user_data*/) {
  if (!SceneEnabled() ||
      (context.backend != NativeGuestOutputBackend::kD3D12 &&
       context.backend != NativeGuestOutputBackend::kVulkan)) {
    return false;
  }
  skate3::mechanics_sandbox::RecordRenderStage(
      skate3::mechanics_sandbox::RenderStage::Entered);
  {
    // Debug stress driver (skate3_native_render_scene_maxq_cycle): cycles
    // the max-quality toggle to exercise the hot rebuild path.
    static uint64_t s_maxq_cycle_frame = 0;
    skate3::MaxQualityAutoCycle(++s_maxq_cycle_frame);
  }
  // While the game reports menus / loading (presence context 0x8001 == 0),
  // yield to the emulated output, EXCEPT the in-game pause menu (world
  // still publishing perspective scenes), which stays native when
  // skate3_native_render_scene_pause_native is on: the pause UI rides the
  // same captured-APT 2D replay as the gameplay HUD, and the backdrop is the
  // ordinary native world. Loading screens and the boot frontend still
  // render emulated (complete and correct there).
  // The photo-grab readback window runs before any yield decision so it
  // updates every frame in every mode (the grab must work whether the photo
  // flow renders yielded-emulated or native).
  uint8_t* base = g_guest_base.load(std::memory_order_relaxed);
  if (base != nullptr) {
    // The render thread reads guest memory for the rest of the frame (mesh
    // decode, texture fingerprints, dynamic-item interpolation) and runs no
    // guest code of its own: keep it permanently armed for raw-load
    // read-fault recovery (POSIX; no-op on Windows).
    ArmGuestReadRecoveryForThread(base);
    UpdatePhotoGrabWindow(base);
  }
  if (YieldForMenus(context)) {
    skate3::mechanics_sandbox::RecordRenderStage(
        skate3::mechanics_sandbox::RenderStage::YieldedForMenus);
    return false;
  }
  if (base == nullptr) {
    return false;
  }
  if (YieldForPhotoDisplay()) {
    skate3::mechanics_sandbox::RecordRenderStage(
        skate3::mechanics_sandbox::RenderStage::YieldedForPhoto);
    return false;
  }
  if (YieldForPhotoEditor(base)) {
    skate3::mechanics_sandbox::RecordRenderStage(
        skate3::mechanics_sandbox::RenderStage::YieldedForPhoto);
    return false;
  }
  if (YieldForCasEditor(base)) {
    skate3::mechanics_sandbox::RecordRenderStage(
        skate3::mechanics_sandbox::RenderStage::YieldedForPhoto);
    return false;
  }

  const auto render_t0 = PerfClock::now();
  const auto perf_ns_since = [](PerfClock::time_point t0) {
    return uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(PerfClock::now() - t0)
            .count());
  };

  bool loading_native = g_loading_native_frame;
  // Render the takeover-gate window (warmup armed, no fresh substantial
  // scene yet) as a native loading frame instead of yielding, in two cases:
  // (a) g_loading_hold: the frames right after a native loading screen
  //     (the presence context flips to gameplay before the first fresh
  //     scene, and the emulated output was suppressed all through the load,
  //     so yielding would flash its stale pre-load content);
  // (b) boot_native: the whole startup flow (intro videos, boot frontend)
  //     runs with the gate armed and no scene published, and should render
  //     natively as 2D-over-black rather than fall back to emulated.
  if (!loading_native &&
      (g_loading_hold || REXCVAR_GET(skate3_native_render_scene_boot_native)) &&
      REXCVAR_GET(skate3_native_render_scene_warmup_budget_ms) > 0 &&
      g_warmup_armed.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> lock(g_scene_mutex);
    // A fresh CAS-editor scene qualifies at ANY size: the startup-flow
    // editor is skater-only (~10 items, below warmup_min_items) but is a
    // complete, deliberate scene; holding it rendered the 3D view black
    // behind the live editor menu. It renders WITHOUT disarming the gate
    // (see the takeover gate below); real gameplay still needs min_items.
    const bool fresh =
        g_scene && g_scene->generation >= g_warmup_fresh_generation;
    const bool ready =
        fresh && (g_scene->items.size() >=
                      size_t(REXCVAR_GET(skate3_native_render_scene_warmup_min_items)) ||
                  (!g_scene->items.empty() && CasEditorActive(base)));
    loading_native = !ready;
  }
  g_loading_hold = loading_native;
  std::shared_ptr<const FrameScene> scene_ptr;
  if (loading_native) {
    // Native loading screen: there is no current world scene (g_scene holds
    // the PREVIOUS map's stale one); render an empty scene, i.e. a black
    // backdrop, and let the 2D overlay tail replay the game's live loading
    // UI (Publish2dDraws publishes every guest frame, loads included).
    // Value-initialized: zero items, no shadow/blur/outline, generation 0.
    static const std::shared_ptr<const FrameScene> s_loading_scene =
        std::make_shared<const FrameScene>();
    scene_ptr = s_loading_scene;
  } else {
    std::lock_guard<std::mutex> lock(g_scene_mutex);
    if (!g_scene ||
        (g_scene->items.empty() && !skate3::mechanics_sandbox::Active())) {
      return false;
    }
    scene_ptr = g_scene;
  }
  const FrameScene& scene = *scene_ptr;
  skate3::mechanics_sandbox::RecordRenderStage(
      skate3::mechanics_sandbox::RenderStage::SceneReady);

  if (!EnsurePipeline(context)) {
    return false;
  }
  skate3::mechanics_sandbox::RecordRenderStage(
      skate3::mechanics_sandbox::RenderStage::PipelineReady);
  // Flush any barriers pushed by lazy resource creation (white texture).
  context.cmd->FlushBarriers();

  // FMV routing: prefer the native path; video quads in the 2D replay are
  // SUBSTITUTED with the ps_yuv2d combine, matched by their own captured
  // slot-0 fetch (== that video's Y plane; through ps_main a movie quad
  // renders as an opaque black cover, intro, or slow greyscale luma,
  // camera-page previews). Order-faithful: backdrop fills land under the
  // video like the emulated frame; multiple simultaneous videos each match
  // their own plane set. Only when this path is unavailable does a live
  // movie heartbeat yield to the emulated output.
  MoviePlanes movies[kMaxMovies];
  {
    std::lock_guard<std::mutex> lock(g_movie_mutex);
    std::memcpy(movies, g_movies, sizeof(movies));
  }
  const int64_t movie_now_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          PerfClock::now().time_since_epoch())
          .count();
  bool movie_fresh = false;
  for (const MoviePlanes& m : movies) {
    movie_fresh |= m.ns >= 0 && movie_now_ns - m.ns < 500'000'000;
  }
  // Movie SESSION start (publish-freshness OFF->ON edge = a video began):
  // the triple substitution only serves plane content DECODED DURING THIS
  // SESSION (GuestTexture::last_change_frame >= this stamp). The APT plane
  // copies keep their addresses across videos, so at video N+1's start
  // both the store AND guest memory can still hold video N's last frame;
  // serving either flashed the previous video for a few frames at every
  // boundary. Until the new video's first frame lands (content change ->
  // inline re-decode), the quad holds black, which is what a starting
  // video looks like.
  static uint64_t s_movie_session_frame = 0;
  {
    static bool s_movie_fresh_prev = false;
    if (movie_fresh && !s_movie_fresh_prev) {
      s_movie_session_frame = g_frames_rendered.load(std::memory_order_relaxed);
    }
    s_movie_fresh_prev = movie_fresh;
  }
  const bool movie_sub = movie_fresh &&
                         REXCVAR_GET(skate3_native_render_scene_fmv_native) &&
                         g_r.pso_yuv2d != nullptr;
  if (!movie_sub && YieldForMovie()) {
    return false;
  }

  nrhi::Cmd* cmd = context.cmd;

  ReleaseRetiredAndFlushCaches(context);

  // Acquire a bone / ROPA upload region no submitted GPU work still owns.
  // CPU frame modulo is unsafe here: this renderer can produce hundreds of
  // frames per second while several submissions remain in flight, so an
  // eight-frame ring used to overwrite palettes before queued character
  // draws consumed them.
  const uint64_t frame_number = g_frames_rendered.load(std::memory_order_relaxed);
  const uint64_t current_submission =
      context.device->CurrentSubmission();
  const uint64_t completed_submission =
      context.device->CompletedSubmission();
  uint32_t upload_region_index =
      g_r.next_upload_region % RendererState::kBoneRegions;
  bool found_upload_region = false;
  for (uint32_t scan = 0;
       scan < RendererState::kBoneRegions; ++scan) {
    const uint32_t candidate =
        (g_r.next_upload_region + scan) %
        RendererState::kBoneRegions;
    const uint64_t owner =
        g_r.upload_region_submissions[candidate];
    if (owner == 0 || owner <= completed_submission) {
      upload_region_index = candidate;
      found_upload_region = true;
      break;
    }
  }
  if (!found_upload_region) {
    // A 32-deep upload ring should stay ahead of the command processor's
    // bounded in-flight queue. Preserve rendering if that invariant is
    // violated, but make the unsafe reuse explicit in telemetry.
    uint64_t oldest_submission =
        std::numeric_limits<uint64_t>::max();
    for (uint32_t candidate = 0;
         candidate < RendererState::kBoneRegions; ++candidate) {
      if (g_r.upload_region_submissions[candidate] <
          oldest_submission) {
        oldest_submission =
            g_r.upload_region_submissions[candidate];
        upload_region_index = candidate;
      }
    }
    ++g_r.upload_region_unsafe_reuses;
  }
  g_r.upload_region_submissions[upload_region_index] =
      std::max<uint64_t>(current_submission, 1);
  g_r.next_upload_region =
      (upload_region_index + 1) %
      RendererState::kBoneRegions;
  const uint32_t bone_region =
      upload_region_index *
      RendererState::kBoneRegionSize;
  g_r.bone_ring_offset = 0;
  g_r.ropa_ring_offset = 0;
  {
    struct UploadRingTelemetry {
      std::chrono::steady_clock::time_point last_log{};
      uint64_t maximum_submission_lag = 0;
      uint32_t maximum_busy_regions = 0;
      uint64_t previous_unsafe_reuses = 0;
    };
    static UploadRingTelemetry telemetry;
    const auto upload_now = std::chrono::steady_clock::now();
    uint32_t busy_regions = 0;
    for (const uint64_t owner :
         g_r.upload_region_submissions) {
      busy_regions +=
          owner != 0 && owner > completed_submission ? 1u : 0u;
    }
    telemetry.maximum_busy_regions =
        std::max(telemetry.maximum_busy_regions, busy_regions);
    telemetry.maximum_submission_lag =
        std::max(
            telemetry.maximum_submission_lag,
            current_submission >= completed_submission
                ? current_submission - completed_submission
                : uint64_t{0});
    if (telemetry.last_log ==
        std::chrono::steady_clock::time_point{}) {
      telemetry.last_log = upload_now;
    } else if (
        upload_now - telemetry.last_log >=
        std::chrono::seconds(5)) {
      REXLOG_INFO(
          "multiplayer-gpu-upload-ring: regions={} busy={}/{} "
          "submission_lag={}/{} unsafe_reuse={} total={}",
          RendererState::kBoneRegions, busy_regions,
          telemetry.maximum_busy_regions,
          current_submission >= completed_submission
              ? current_submission - completed_submission
              : uint64_t{0},
          telemetry.maximum_submission_lag,
          g_r.upload_region_unsafe_reuses -
              telemetry.previous_unsafe_reuses,
          g_r.upload_region_unsafe_reuses);
      telemetry.last_log = upload_now;
      telemetry.maximum_submission_lag = 0;
      telemetry.maximum_busy_regions = 0;
      telemetry.previous_unsafe_reuses =
          g_r.upload_region_unsafe_reuses;
    }
  }

  {
    const std::string tm(REXCVAR_GET(skate3_native_render_scene_trace_mesh));
    const uint32_t parsed =
        tm.empty() ? 0u : uint32_t(std::strtoul(tm.c_str(), nullptr, 16));
    if (parsed != g_trace_mesh_addr) {
      g_trace_mesh_addr = parsed;
      g_trace_keys.clear();
      g_trace_sig.clear();
      REXLOG_INFO("tex-trace: mesh={:08X} {}", parsed,
                  parsed ? "TRACING" : "off");
    }
    const std::string t2(REXCVAR_GET(skate3_native_render_scene_trace_2d));
    const uint32_t parsed2 =
        t2.empty() ? 0u : uint32_t(std::strtoul(t2.c_str(), nullptr, 16));
    if (parsed2 != g_trace_2d_w1) {
      g_trace_2d_w1 = parsed2;
      REXLOG_INFO("2d-trace: w1={:08X} {}", parsed2,
                  parsed2 ? "TRACING" : "off");
    }
  }

  // ---- Loading -> gameplay takeover (seamless boot / map-change loads) ----
  // The loading-screen prewarm (menus branch above) already decoded the
  // registered world behind the load; the FIRST substantial post-load scene
  // renders natively right away. Gates kept: staleness (g_scene holds the
  // PREVIOUS map's scene through the whole load; rendering it shows
  // old-map garbage) and min items (the capture holds a near-empty scene
  // for a few frames while the game fades in; taking over there shows a
  // black/void world; the brief yield shows the game's own fade instead).
  // The frames after takeover run a budgeted settle pass for whatever
  // prewarm missed (dynamic entities, late textures), and the draw path's
  // miss budgets are clamped while settling so leftovers render white/skip
  // for a frame instead of freezing the takeover frame.
  const int32_t warmup_ms = REXCVAR_GET(skate3_native_render_scene_warmup_budget_ms);
  bool settling = false;
  // The takeover gates judge REAL scenes only; a native loading frame
  // renders its empty scene deliberately (the gates re-run as usual once
  // the presence context flips back to gameplay).
  if (warmup_ms > 0 && !loading_native &&
      REXCVAR_GET(skate3_native_render_scene_debug) == 0) {
    if (g_warmup_armed.load(std::memory_order_relaxed)) {
      const bool small_or_stale =
          scene.generation < g_warmup_fresh_generation ||
          scene.items.size() <
              size_t(REXCVAR_GET(skate3_native_render_scene_warmup_min_items));
      // A fresh CAS-editor scene renders despite being under min_items (the
      // startup-flow editor is skater-only, ~10 items), but does NOT count
      // as the gameplay takeover; the gate stays armed for the real load
      // that follows the editor.
      const bool editor_scene = scene.generation >= g_warmup_fresh_generation &&
                                !scene.items.empty() && CasEditorActive(base);
      if (small_or_stale && !editor_scene) {
        // Stale or fade-in scene: yield (brief, a few frames). Keep
        // committing worker results meanwhile; every pre-takeover frame
        // counts on map changes.
        PrewarmCommit(context, frame_number);
        return false;
      }
      if (!small_or_stale) {
        g_warmup_armed.store(false, std::memory_order_relaxed);
        g_settle_until_frame = frame_number + 120;
        g_takeover_frame = frame_number;
        size_t queued = 0;
        {
          std::lock_guard<std::mutex> lock(g_prewarm_mutex);
          queued = g_prewarm_queue.size();
        }
        REXLOG_INFO(
            "native-scene: taking over natively ({} items; prewarm {} done / {} dropped "
            "/ {} queued)",
            scene.items.size(), g_prewarm_done.load(std::memory_order_relaxed),
            g_prewarm_dropped.load(std::memory_order_relaxed), queued);
      }
    }
    settling = frame_number < g_settle_until_frame;
    if (settling) {
      const auto settle_t0 = PerfClock::now();
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(warmup_ms);
      WarmCounters wc;
      for (const DrawItem& item : scene.items) {
        WarmItemResources(context, base, frame_number, item, deadline, wc);
      }
      if (wc.deferred != 0) {
        // Still behind: keep the settle pass (and the draw-path budget
        // clamp) alive until a frame clears with budget to spare - but never
        // past the hard cap: revalidation-churning content (streamed
        // payloads, post-map-change stale items) re-defers every frame and
        // would otherwise hold the full budget open for the whole session.
        const int32_t max_frames =
            REXCVAR_GET(skate3_native_render_scene_settle_max_frames);
        const uint64_t extended = frame_number + 8;
        if (max_frames <= 0 || extended <= g_takeover_frame + uint64_t(max_frames)) {
          g_settle_until_frame = std::max<uint64_t>(g_settle_until_frame, extended);
        } else if (g_settle_until_frame <= frame_number + 1) {
          REXLOG_INFO(
              "native-scene: settle pass CAPPED {} frames after takeover with {} "
              "deferred (revalidation churn?) - draw-path budgets resume",
              frame_number - g_takeover_frame, wc.deferred);
        }
      }
      if (wc.decodes > 0) {
        cmd->FlushBarriers();
      }
      g_warm_decodes.fetch_add(wc.decodes, std::memory_order_relaxed);
      g_warm_deferred.fetch_add(wc.deferred, std::memory_order_relaxed);
      g_pw_settle.Add(perf_ns_since(settle_t0));
    }
  }

  // GPU-time attribution: every pass below marks its start; the backend's
  // timestamp-bucket profiler (d3d12/vulkan_gpu_timestamp_buckets) reports
  // per-pass spans. The first mark starts here so the frame's store commits
  // and evictions are attributed separately from the render passes.
  cmd->ProfileRegion(nrhi::ProfileStage::kCommit);

  if (!g_r.announced) {
    g_r.announced = true;
    REXLOG_INFO("native-scene: rendering natively ({} items, {}x{})", scene.items.size(),
                context.guest_output_width, context.guest_output_height);
  }

  // Commit finished worker decodes every frame (streamed arenas decode on
  // the workers before their first draw instead of hitching the first frame
  // that sees them). Near-no-op when nothing completed. The workers also
  // serve the draw path's steady-state misses (EnqueueMeshMiss/
  // EnqueueWordsMiss), so make sure they exist even on a session that never
  // showed a loading screen with the pipeline up.
  EnsurePrewarmWorkers();
  // Native loading frames take the loading-screen commit budget; the heavy
  // decode lifting behind the load is unchanged from the yielded path.
  PrewarmCommit(context, frame_number, /*loading=*/loading_native);
  // Content-store LRU: superseded words states (old mip levels, pre-demote
  // detail sets, one-shot UI art) age out once nothing routes to them; the
  // mesh cache ages out streamed-out arenas the same way.
  UpdateEvictFpsEstimate(frame_number);
  EvictTexStore(frame_number, context.device->CurrentSubmission());
  EvictMeshStore(frame_number);
  EvictCubeStore(frame_number, context.device->CurrentSubmission());

  bool shadow_ready = false;
  uint32_t shadow_draws = 0;
  const auto shadow_t0 = PerfClock::now();
  float owned_shadow_rows[36] = {};
  const bool owned_shadow =
      BuildOwnedShadowRows(scene, owned_shadow_rows);
  const float* sh =
      owned_shadow ? owned_shadow_rows : scene.shadow_rows;
  const bool shadow_rows_valid =
      owned_shadow || scene.shadow_valid;
  const int32_t debug_mode = REXCVAR_GET(skate3_native_render_scene_debug);
  cmd->ProfileRegion(nrhi::ProfileStage::kShadow);
  shadow_ready = RenderShadowAtlas(
      context, scene, sh, shadow_rows_valid, owned_shadow, bone_region,
      debug_mode, &shadow_draws);
  cmd->ProfileRegion(nrhi::ProfileStage::kStaticSun);
  RenderStaticSunMap(context, scene, bone_region, debug_mode, frame_number);
  g_pw_shadow.Add(perf_ns_since(shadow_t0));
  cmd->ProfileRegion(nrhi::ProfileStage::kMain);
  // Scene-level flicker probe: the shadow cascade rows (or validity/readiness)
  // returning EXACTLY to the previous-but-one frame's state; hair samples the
  // atlas, so an alternating cascade capture darkens/lightens it per frame.
  {
    uint64_t sh_h = 1469598103934665603ull;
    const uint8_t* sb = reinterpret_cast<const uint8_t*>(sh);
    for (size_t bi = 0; bi < sizeof(owned_shadow_rows); ++bi) {
      sh_h = (sh_h ^ sb[bi]) * 1099511628211ull;
    }
    sh_h ^= (shadow_rows_valid ? 2u : 0u) | (shadow_ready ? 4u : 0u);
    static uint64_t s_sh_h1 = 0, s_sh_h2 = 0;  // render thread
    if (s_sh_h2 != 0 && sh_h != s_sh_h1 && sh_h == s_sh_h2) {
      g_shadow_alternations.fetch_add(1, std::memory_order_relaxed);
      static std::atomic<uint32_t> s_sh_logged{0};
      const uint32_t sl = s_sh_logged.fetch_add(1, std::memory_order_relaxed);
      if (sl < 24 || (sl & 1023u) == 0) {
        REXLOG_DEBUG(
            "native-scene: SHADOW ALTERNATION valid={} ready={} draws={} "
            "row0=({:.3f},{:.3f},{:.3f},{:.3f}) (n={})",
            shadow_rows_valid ? 1 : 0, shadow_ready ? 1 : 0, shadow_draws,
            sh[0], sh[1], sh[2], sh[3], sl);
      }
    }
    s_sh_h2 = s_sh_h1;
    s_sh_h1 = sh_h;
  }

  // The scene draws into the MSAA target when enabled (resolved into the 1x
  // scene plane at the end of the pass), or straight into the 1x plane. The
  // 1x plane is the float HDR intermediate when the HDR post chain is live
  // (ps_tonemap then writes the guest output), else the guest output itself.
  const bool hdr_on = g_r.hdr_active && g_r.hdr_resolved != nullptr &&
                      g_r.hdr_srv != nullptr && g_r.pso_tonemap != nullptr;
  const bool msaa_on = g_r.msaa > 1 && g_r.msaa_color != nullptr && g_r.resolve_pso != nullptr;
  nrhi::Texture* scene_color =
      msaa_on ? g_r.msaa_color
              : (hdr_on ? g_r.hdr_resolved : context.guest_output);
  if (!msaa_on && !hdr_on) {
    cmd->Barrier(context.guest_output, nrhi::ResourceState::kGuestOutput,
                 nrhi::ResourceState::kRenderTarget);
    cmd->FlushBarriers();
  }

  const bool use_depth = debug_mode != 4;
  // Loading frames clear to black (the game's loading UI composes over
  // black); real scenes keep the sky-ish debug clear that shows through
  // undecoded holes.
  const bool sandbox = skate3::mechanics_sandbox::Active();
  float clear_color[4] = {
      loading_native ? 0.0f : (sandbox ? 0.075f : 0.25f),
      loading_native ? 0.0f : (sandbox ? 0.105f : 0.35f),
      loading_native ? 0.0f : (sandbox ? 0.145f : 0.55f), 1.0f};
  if (hdr_on) {
    // The clear is authored in the final gamma space; the HDR plane holds
    // pre-tonemap values, so encode through the tone chain's inverse (the
    // same transform as scene.hlsl PassGamma) so undecoded holes keep the
    // same debug color after the tonemap pass.
    for (int k = 0; k < 3; ++k) {
      const float tm =
          clear_color[k] * clear_color[k] * (2.0f / (1.41f * 1.41f));
      clear_color[k] = tm > 1.0f
                           ? tm * 4.0f - 3.0f
                           : 1.0f - std::sqrt(std::max(1.0f - tm, 0.0f));
    }
  }
  cmd->ClearRenderTarget(scene_color, clear_color);
  if (use_depth) {
    cmd->ClearDepth(g_r.depth, 1.0f);
    cmd->SetRenderTargets(scene_color, g_r.depth);
  } else {
    cmd->SetRenderTargets(scene_color, nullptr);
  }

  const nrhi::Viewport viewport{0.0f,
                                0.0f,
                                float(context.guest_output_width),
                                float(context.guest_output_height),
                                0.0f,
                                1.0f};
  cmd->SetViewport(viewport);
  const nrhi::Rect scissor{0, 0, int32_t(context.guest_output_width),
                           int32_t(context.guest_output_height)};
  cmd->SetScissor(scissor);
  cmd->SetBindingLayout(g_r.layout);
  cmd->SetPipeline(use_depth ? g_r.pso : g_r.pso_nodepth);
  skate3::mechanics_sandbox::RecordRenderStage(
      skate3::mechanics_sandbox::RenderStage::BackgroundCleared);
  // Per-item PSO tracking for the opaque pass: two_sided_sheet meshes swap
  // to the backface-culling variant (see MeshBuffers), everything else uses
  // the pass PSO. Only meaningful while use_depth; the transparent sub-pass
  // sets its own PSO and is not switched per item.
  nrhi::Pipeline* scene_pso_bound = use_depth ? g_r.pso : g_r.pso_nodepth;

  // Per-frame dynamic-shadow bindings: receiver constants at b1 (a small
  // per-frame CBV ring: the 52-float root-constant block is full) and the
  // blurred atlas at t5 (white when no atlas was rendered this frame; the
  // shader is also gated by sh_misc.y).
  if (g_r.shadow_cb != nullptr) {
    static auto moving_light_clock =
        std::chrono::steady_clock::now();
    const auto moving_light_now =
        std::chrono::steady_clock::now();
    mechanics_sandbox::map::AdvanceMovingLights(
        std::chrono::duration<float>(
            moving_light_now - moving_light_clock).count());
    mechanics_sandbox::map::AdvanceDayNightCycle(
        std::chrono::duration<float>(
            moving_light_now - moving_light_clock).count());
    mechanics_sandbox::map::AdvanceWeather(
        std::chrono::duration<float>(
            moving_light_now - moving_light_clock).count());
    moving_light_clock = moving_light_now;
    const uint32_t cb_offset =
        uint32_t(frame_number % RendererState::kShadowCbRegions) *
        RendererState::kShadowCbSlice;
    float* cb = reinterpret_cast<float*>(g_r.shadow_cb_cpu + cb_offset);
    std::memset(cb, 0, RendererState::kShadowCbSlice);
    if (shadow_ready) {
      std::memcpy(cb + 0, sh + 0, 4 * sizeof(float));    // sh_x   = PS c0
      std::memcpy(cb + 4, sh + 12, 4 * sizeof(float));   // sh_y   = PS c3
      std::memcpy(cb + 8, sh + 16, 4 * sizeof(float));   // sh_z   = PS c4
      std::memcpy(cb + 12, sh + 4, 4 * sizeof(float));   // sh_c1  = PS c1
      std::memcpy(cb + 16, sh + 8, 4 * sizeof(float));   // sh_c2  = PS c2
      cb[20] = sh[32];                                   // sh_color = PS c8
      cb[21] = sh[33];
      cb[22] = sh[34];
      cb[23] = 0.299f * sh[32] + 0.587f * sh[33] + 0.114f * sh[34];
      cb[24] = sh[20];  // depth bias (PS c5.x)
      cb[25] = 1.0f;    // enable
      // sh_misc.zw = atlas dimensions (3*tile x tile) for the char path's
      // point-snapped taps.
      cb[26] = float(g_r.shadow_tile * 3);
      cb[27] = float(g_r.shadow_tile);
      // sh_char = the game's per-cascade character receive biases + enable
      // (see FrameScene::char_shadow_bias; exact 9-tap char sampling).
      if (owned_shadow) {
        // Owned CSM depth spans hundreds of metres, unlike Skate 3's
        // compact character atlas. Reusing its unitless 0.005/0.014/0.020
        // values offsets receivers by metres. Express stable per-cascade
        // character bias in world metres and convert through the actual
        // owned depth row.
        const float owned_z_scale =
            std::sqrt(sh[16] * sh[16] + sh[17] * sh[17] +
                      sh[18] * sh[18]);
        cb[56] = 0.018f * owned_z_scale;
        cb[57] = 0.028f * owned_z_scale;
        cb[58] = 0.045f * owned_z_scale;
        cb[59] = 1.0f;
      } else if (scene.char_shadow_bias[0] > 0.0f &&
                 REXCVAR_GET(skate3_native_render_scene_char_shadow_exact)) {
        cb[56] = scene.char_shadow_bias[0];
        cb[57] = scene.char_shadow_bias[1];
        cb[58] = scene.char_shadow_bias[2];
        cb[59] = 1.0f;
      }
    }
    // PCSS soft-shadow rows (sh_pcss / sh_pcss2, cb[136..143]). Filled
    // whenever the light rows are valid; the static sun-shadow sampler
    // shares the angular knobs even on frames with no dynamic casters.
    if (shadow_rows_valid) {
      cb[136] =
          REXCVAR_GET(skate3_native_render_scene_shadow_pcss) ? 1.0f : 0.0f;
      cb[137] = std::tan(
          0.5f * 0.01745329f *
          float(REXCVAR_GET(skate3_native_render_scene_shadow_pcss_sun_deg)));
      cb[138] =
          float(REXCVAR_GET(skate3_native_render_scene_shadow_pcss_max_m));
      const float zlen =
          std::sqrt(sh[16] * sh[16] + sh[17] * sh[17] +
                    sh[18] * sh[18]);
      // A small metric slope term suppresses grazing-angle acne on the
      // high-resolution owned character receiver without detaching feet,
      // board, or limb-on-body contact shadows.
      cb[139] = owned_shadow ? 0.010f * zlen : 0.0f;
      cb[140] =
          float(REXCVAR_GET(skate3_native_render_scene_shadow_pcss_blocker_m));
      const float xlen =
          std::sqrt(sh[0] * sh[0] + sh[1] * sh[1] + sh[2] * sh[2]);
      cb[141] = zlen > 1e-8f ? 1.0f / zlen : 0.0f;
      cb[142] = xlen;
      cb[143] = float(
          REXCVAR_GET(skate3_native_render_scene_shadow_pcss_min_texel));
    }
    // Native static sun-shadow rows (nsm_x/y/z + params, cb[144..163]);
    // nsm_p.x = 0 disables the term in every branch (the scene receive, the
    // character branch and the volumetric shafts all read this slice).
    // Gated on FRESH world rows, not just shadow_valid: interior venues
    // (park-editor warehouses) never produce a sane environment bank, so the
    // sun direction here would be a stale outdoor capture. The map then
    // shades the whole interior as roof-shadowed, and with no game shadow
    // pass this frame sh_color is zero, so the exact env families clamp
    // min(lm^2, s + sh_color) to pure black - the missing walls/floor.
    // The game itself never sun-shadows static world geometry indoors;
    // standing the map down restores its exact lm^2 shading.
    if (g_r.static_sun_valid && (owned_shadow || scene.shadow_fresh)) {
      const bool independent_owned =
          owned_shadow && g_r.owned_nsm_active;
      const float* primary_rows =
          independent_owned ? g_r.owned_nsm_rows[0] : g_r.nsm_rows;
      const float primary_radius =
          independent_owned ? g_r.owned_nsm_radius[0] : g_r.nsm_radius;
      const float primary_depth_range =
          independent_owned ? g_r.owned_nsm_depth_range[0]
                            : g_r.nsm_depth_range;
      const uint32_t primary_size =
          independent_owned ? g_r.owned_static_sun_size[0]
                            : g_r.static_sun_size;
      std::memcpy(cb + 144, primary_rows, sizeof(g_r.nsm_rows));
      cb[156] = std::clamp(
          float(REXCVAR_GET(skate3_native_render_scene_shadow_static_strength)),
          0.0f, 1.0f);
      cb[157] = 1.0f / primary_radius;
      cb[158] = primary_depth_range;  // meters per depth unit
      // Filter floor: 2 physical texels (uc spans [-1,1] over one tile);
      // the map has no dilation blur, so a sub-texel kernel exposes raw
      // stair-step aliasing at full strength; two texels of
      // rotated-poisson filtering dissolve the steps into a smooth edge.
      cb[159] = 2.0f * 2.0f *
                float(REXCVAR_GET(
                    skate3_native_render_scene_shadow_pcss_min_texel)) /
                float(std::max(1u, primary_size));
      float bias_m = float(
          REXCVAR_GET(skate3_native_render_scene_shadow_static_bias));
      if (context.device->backend() == nrhi::Backend::kVulkan) {
        // The two backends compile the caster/receiver depth math through
        // different shader toolchains; their rounding differences leave
        // borderline texels flipping at map rebuilds without this margin.
        bias_m += float(
            REXCVAR_GET(skate3_native_render_scene_shadow_static_bias_vk));
      }
      cb[160] = bias_m / primary_depth_range;  // base receiver bias
      cb[161] = bias_m / primary_depth_range;  // slope-scaled term
      cb[162] = 0.0f;  // reserved (cascade ratios are shader constants)
      cb[163] = float(std::max(1u, primary_size));
      if (independent_owned) {
        std::memcpy(cb + 216, g_r.owned_nsm_rows[1],
                    sizeof(g_r.owned_nsm_rows[1]));
        std::memcpy(cb + 228, g_r.owned_nsm_rows[2],
                    sizeof(g_r.owned_nsm_rows[2]));
        cb[240] = float(g_r.owned_static_sun_size[0]);
        cb[241] = float(g_r.owned_static_sun_size[1]);
        cb[242] = float(g_r.owned_static_sun_size[2]);
        cb[243] = 1.0f;
      } else {
        cb[243] = 0.0f;
      }
    }
    float owned_origin[3] = {};
    if (mechanics_sandbox::Active() &&
        mechanics_sandbox::SandboxMapRenderOrigin(
            owned_origin)) {
      // Maps retain every authored light. Keep the per-frame shader work
      // bounded by selecting the 64 lights most relevant to the camera.
      constexpr std::size_t kMaximumOwnedLocalLights = 64;
      struct RankedOwnedLight {
        mechanics_sandbox::map::MovingLightSnapshot light;
        float score = 0.0f;
      };
      std::vector<RankedOwnedLight> ranked_lights;
      ranked_lights.reserve(
          mechanics_sandbox::map::ActiveMovingLightCount());
      for (std::size_t light_index = 0;
           light_index <
               mechanics_sandbox::map::ActiveMovingLightCount();
           ++light_index) {
        mechanics_sandbox::map::MovingLightSnapshot light;
        if (!mechanics_sandbox::map::ActiveMovingLightSnapshot(
                light_index, light)) {
          continue;
        }
        const float world_x = owned_origin[0] + light.position[0];
        const float world_y = owned_origin[1] + light.position[1];
        const float world_z = owned_origin[2] + light.position[2];
        const float delta_x = world_x - scene.cam_pos[0];
        const float delta_y = world_y - scene.cam_pos[1];
        const float delta_z = world_z - scene.cam_pos[2];
        const float range =
            std::max(light.influence_radius, 0.01f);
        ranked_lights.push_back(
            {light,
             (delta_x * delta_x + delta_y * delta_y +
              delta_z * delta_z) /
                 (range * range)});
      }
      const std::size_t selected_light_count =
          std::min(ranked_lights.size(), kMaximumOwnedLocalLights);
      if (ranked_lights.size() > selected_light_count) {
        std::partial_sort(
            ranked_lights.begin(),
            ranked_lights.begin() + selected_light_count,
            ranked_lights.end(),
            [](const RankedOwnedLight& left,
               const RankedOwnedLight& right) {
              return left.score < right.score;
            });
      }
      std::size_t light_count = 0;
      for (std::size_t light_index = 0;
           light_index < selected_light_count; ++light_index) {
        const mechanics_sandbox::map::MovingLightSnapshot& light =
            ranked_lights[light_index].light;
        const std::size_t position = 244 + light_count * 4;
        const std::size_t color = 500 + light_count * 4;
        const std::size_t direction = 756 + light_count * 4;
        const std::size_t spot = 1012 + light_count * 4;
        cb[position + 0] =
            owned_origin[0] + light.position[0];
        cb[position + 1] =
            owned_origin[1] + light.position[1];
        cb[position + 2] =
            owned_origin[2] + light.position[2];
        cb[position + 3] = light.influence_radius;
        cb[color + 0] = light.color[0];
        cb[color + 1] = light.color[1];
        cb[color + 2] = light.color[2];
        cb[color + 3] = light.intensity;
        cb[direction + 0] = light.direction[0];
        cb[direction + 1] = light.direction[1];
        cb[direction + 2] = light.direction[2];
        cb[direction + 3] = static_cast<float>(light.type);
        cb[spot + 0] = light.spot_inner_cosine;
        cb[spot + 1] = light.spot_outer_cosine;
        cb[spot + 2] = light.source_radius;
        ++light_count;
      }
      mechanics_sandbox::map::MovingLightSnapshot lightning;
      if (light_count < kMaximumOwnedLocalLights &&
          mechanics_sandbox::map::ActiveLightningLightSnapshot(
              lightning)) {
        const std::size_t position = 244 + light_count * 4;
        const std::size_t color = 500 + light_count * 4;
        const std::size_t direction = 756 + light_count * 4;
        const std::size_t spot = 1012 + light_count * 4;
        cb[position + 0] =
            owned_origin[0] + lightning.position[0];
        cb[position + 1] =
            owned_origin[1] + lightning.position[1];
        cb[position + 2] =
            owned_origin[2] + lightning.position[2];
        cb[position + 3] = lightning.influence_radius;
        cb[color + 0] = lightning.color[0];
        cb[color + 1] = lightning.color[1];
        cb[color + 2] = lightning.color[2];
        cb[color + 3] = lightning.intensity;
        cb[direction + 0] = 0.0f;
        cb[direction + 1] = -1.0f;
        cb[direction + 2] = 0.0f;
        cb[direction + 3] = 0.0f;
        cb[spot + 0] = 1.0f;
        cb[spot + 1] = 1.0f;
        cb[spot + 2] = lightning.source_radius;
        ++light_count;
      }
      cb[200] = static_cast<float>(light_count);
      const skate::world::DayNightState celestial =
          mechanics_sandbox::map::ActiveDayNightState();
      cb[201] =
          mechanics_sandbox::map::ActiveDefinition()
                  .day_night_cycle.enabled
              ? 1.0f
              : 0.0f;
      cb[202] = celestial.night_amount;
      cb[203] = celestial.daylight_amount;
      cb[204] = celestial.light_direction_to_light.x;
      cb[205] = celestial.light_direction_to_light.y;
      cb[206] = celestial.light_direction_to_light.z;
      cb[207] = celestial.light_intensity;
      cb[208] = celestial.light_color.x;
      cb[209] = celestial.light_color.y;
      cb[210] = celestial.light_color.z;
      cb[211] = celestial.ambient;
      cb[212] = celestial.sun_visibility;
      cb[213] = celestial.moon_visibility;
      cb[214] = celestial.twilight_amount;
      cb[215] = celestial.star_visibility;
    }
    // Exact world-shading frame rows (valid whenever the env-family PS bank
    // was captured this frame, independent of the shadow ATLAS being
    // rendered; draw_item only selects the exact branch when
    // scene.shadow_valid).
    if (scene.shadow_valid) {
      cb[28] = sh[24];  // sun direction (PS c6), for the normal-map kd (v2)
      cb[29] = sh[25];
      cb[30] = sh[26];
      // Owned rows intentionally replace only the light/shadow contract.
      // Exposure and the retained-character material multiplier still come
      // from the captured game frame (outside the compact 36-float owned
      // row block).
      cb[31] = scene.shadow_rows[40];  // scene exposure (PS c10.x)
      cb[32] = scene.shadow_rows[45];  // material multiplier (PS c11.y)
      cb[33] = scene.family_rows[0];  // tree lightmap scale (tree PS c0.x)
      cb[34] = scene.family_rows[1];  // tree lightmap floor (tree PS c0.y)
      cb[35] = scene.family_rows[2];  // tree tint multiplier (tree PS c4.y)
      cb[36] = scene.fog_ramp[0];     // global fog: sat(d*x+y)^z
      cb[37] = scene.fog_ramp[1];
      cb[38] = scene.fog_ramp[2];
      cb[39] = scene.family_rows[3];  // proxyworld scale (proxy PS c3.y)
      std::memcpy(cb + 40, scene.fog_color, 4 * sizeof(float));
    }
    // World-shading v2 row (sh_v2, cb[60..63]): x = stored-tangent
    // polarity, yzw = the build-up showcase split state (stage left/right
    // of the split + its position in output pixels; zeros = showcase off).
    cb[60] =
        float(REXCVAR_GET(skate3_native_render_scene_world_v2_tan_sign));
    TickShowcase(context.guest_output_width, hdr_on);
    cb[61] = g_r.showcase_rows[0];
    cb[62] = g_r.showcase_rows[1];
    cb[63] = g_r.showcase_rows[2];
    // dynamicobject.fx frame rows (dyn_sun/dyn_amb/dyn_misc at cb[44..55]).
    if (scene.dynobj_valid) {
      cb[44] = scene.dynobj_rows[0];  // sun dir (PS c9)
      cb[45] = scene.dynobj_rows[1];
      cb[46] = scene.dynobj_rows[2];
      cb[47] = scene.dynobj_rows[3];  // scene exposure (c13.x)
      cb[48] = scene.dynobj_rows[4];  // flat ambient rgb (c15.xyz)
      cb[49] = scene.dynobj_rows[5];
      cb[50] = scene.dynobj_rows[6];
      cb[51] = scene.dynobj_rows[7];  // bounce scale (c15.w)
      cb[52] = scene.dynobj_rows[8];  // material multiplier (c14.y)
      cb[53] = scene.dynobj_rows[9];  // static world-shadow floor (c8.w)
      // Static world-shadow transform rows (dyn_wsx/dyn_wsy/dyn_wsz,
      // cb[64..75]): consumed only when a draw carries the misc.z world-
      // shadow bind flag (the map primed and bound at t4).
      if (scene.dynobj_ws_valid) {
        std::memcpy(cb + 64, scene.dynobj_ws, sizeof(scene.dynobj_ws));
      }
    }
    // flowingwateralpha m_params rows (wat_p0..wat_p3 at cb[76..91]):
    // m_params[0..2] verbatim, then (time, mask threshold, spec power,
    // alpha floor). Consumed by the exact water branch (cam_pos.w = -30),
    // which is only selected per draw when scene.water_valid.
    if (scene.water_valid) {
      std::memcpy(cb + 76, scene.water_rows, 12 * sizeof(float));   // c11..c13
      cb[88] = scene.water_rows[16];  // g_fAnimationTime (c15.x)
      cb[89] = scene.water_rows[13];  // mask threshold (c14.y)
      cb[90] = scene.water_rows[14];  // spec power (c14.z)
      cb[91] = scene.water_rows[15];  // alpha floor (c14.w)
    }
    // ocean_defaultPS rows (oc_mean/oc_w0..5/oc_p0..2 at cb[92..131]) and
    // the oceanreflection fade row (orf at cb[132..135]); consumed by the
    // exact ocean branches (cam_pos.w = -31 / -32).
    if (scene.ocean_valid) {
      std::memcpy(cb + 92, scene.ocean_rows, sizeof(scene.ocean_rows));
    }
    if (scene.oceanrefl_valid) {
      std::memcpy(cb + 132, scene.oceanrefl_rows,
                  sizeof(scene.oceanrefl_rows));
    }
    cmd->SetConstantBuffer(6, g_r.shadow_cb, cb_offset);
    // b2 (character lighting) default: point at the ring base so the root
    // CBV is never left unset; character draws re-point it per item.
    cmd->SetConstantBuffer(9, g_r.bone_ring, bone_region);
    // t6/t7 ride one table: default = white cube + this frame's atlas
    // (draw_item re-pairs with the item's real cube when one resolves).
    cmd->SetTexturePair(7, g_r.white_cube.srv,
                        shadow_ready ? g_r.shadow_srv_final : g_r.white.srv);
    // v2 material table default (t8/t9 white) + independent static-shadow
    // maps at t10..t12. Every lit branch can sample them.
    nrhi::TextureView* t8_default[5] = {
        g_r.white.srv, g_r.white.srv,
        g_r.owned_nsm_active ? g_r.owned_static_sun_srv[0]
                             : (g_r.static_sun_valid ? g_r.static_sun_srv
                                                     : g_r.white.srv),
        g_r.owned_nsm_active ? g_r.owned_static_sun_srv[1] : g_r.white.srv,
        g_r.owned_nsm_active ? g_r.owned_static_sun_srv[2] : g_r.white.srv};
    cmd->SetTextures(8, t8_default, 5);
  }
  nrhi::TextureView* const nsm_view =
      g_r.owned_nsm_active
          ? g_r.owned_static_sun_srv[0]
          : (g_r.static_sun_valid ? g_r.static_sun_srv : g_r.white.srv);
  nrhi::TextureView* const nsm_mid_view =
      g_r.owned_nsm_active ? g_r.owned_static_sun_srv[1] : g_r.white.srv;
  nrhi::TextureView* const nsm_far_view =
      g_r.owned_nsm_active ? g_r.owned_static_sun_srv[2] : g_r.white.srv;

  // The owned map and player now share one world frame and depth buffer.
  // Drawing the park first gives the retained player ordinary occlusion
  // against its floor, ramps, and obstacles.
  DrawSandboxSky(context, cmd, scene, frame_number);
  std::vector<DrawItem> multiplayer_remote_items;
  DrawSandboxMap(
      context, cmd, scene, frame_number, use_depth, shadow_ready,
      &multiplayer_remote_items);
  cmd->SetPipeline(use_depth ? g_r.pso : g_r.pso_nodepth);

  uint32_t drawn = 0;
  uint32_t item_index = 0;
  // F7 scene-composition ring (RequestSceneRingDump): one compact signature
  // per scene item per frame, ~900 frames deep. A 1-2 frame artifact (the
  // dam-bank blue flash) is uncapturable by F10/F11; the ring lets a
  // keypress seconds later name exactly which item appeared / vanished /
  // changed textures on the artifact frame. Entries are recorded at
  // classification below; draw_item stamps the issued-draw count, so an
  // item that early-returned (deferred mesh, skip-new, fade skip) shows
  // drawn=0 that frame.
  SceneRingFrame* ring_frame = nullptr;
  std::unordered_map<const DrawItem*, uint32_t> ring_map;
  if (REXCVAR_GET(skate3_native_render_scene_ring) && debug_mode == 0) {
    g_scene_ring.emplace_back();
    while (g_scene_ring.size() > kSceneRingFrames) {
      g_scene_ring.pop_front();
    }
    ring_frame = &g_scene_ring.back();
    ring_frame->frame = frame_number;
    std::memcpy(ring_frame->cam, scene.cam_pos, sizeof(ring_frame->cam));
    ring_frame->fog[0] = scene.fog_ramp[0];
    ring_frame->fog[1] = scene.fog_ramp[1];
    ring_frame->fog[2] = scene.fog_ramp[2];
    ring_frame->fog[3] = scene.fog_color[0];
    ring_frame->fog[4] = scene.fog_color[1];
    ring_frame->fog[5] = scene.fog_color[2];
    std::memcpy(ring_frame->family_rows, scene.family_rows,
                sizeof(ring_frame->family_rows));
    ring_frame->sky_height = scene.sky_height;
    ring_frame->shadow_valid = scene.shadow_valid;
    ring_frame->shadow_ready = shadow_ready;
    ring_frame->shadow_draws = uint16_t(std::min<uint32_t>(shadow_draws, 0xFFFFu));
    ring_frame->static_sun_valid = g_r.static_sun_valid;
    std::memcpy(ring_frame->shadow_rows, scene.shadow_rows,
                sizeof(ring_frame->shadow_rows));
    ring_frame->items.reserve(scene.items.size());
    ring_map.reserve(scene.items.size());
  }
  // Inline decode budget for DYNAMIC payloads only (skinned/cloth/ropa
  // buffers that change every frame). Static meshes and all textures route
  // to the decode workers on miss; the render thread never pays their
  // decode cost.
  int32_t mesh_budget = REXCVAR_GET(skate3_native_render_scene_mesh_decode_budget);
  if (settling) {
    // Post-takeover settle window: cap inline dynamic decodes too, so the
    // takeover frame never absorbs an unbounded decode burst.
    if (mesh_budget == 0 || mesh_budget > 16) mesh_budget = 16;
  }
  uint32_t mesh_decodes = 0;
  // Item drawing body. environment.decal items draw in the same opaque pass:
  // they ARE the wall/ground sections they cover, with the art composited
  // in-shader (alpha-blending them as separate overlay geometry punched
  // holes in the world).
  // Ripple scroll clock for the water branch (seconds; wraps every hour to
  // keep float precision on the scrolled UVs).
  static const std::chrono::steady_clock::time_point water_t0 =
      std::chrono::steady_clock::now();
  const float water_time = float(
      std::chrono::duration<double>(std::chrono::steady_clock::now() - water_t0).count() -
      std::floor(
          std::chrono::duration<double>(std::chrono::steady_clock::now() - water_t0).count() /
          3600.0) *
          3600.0);

  // Words-keyed texture lookup with payload revalidation, shared by the
  // streamed-artwork diffuse override and the 2D pass. The event-ad system
  // rotates artwork IN PLACE; it streams the next poster into the same
  // guest buffer, so the fetch words (this cache's key) never change; without
  // the fingerprint recheck every frame keeps the first ad ever decoded at
  // that address and the wall posters diverge from the emulated frame.
  const auto find_words_texture = [&](uint64_t key) {
    auto it = g_r.tex_store.find(key);
    if (it != g_r.tex_store.end()) {
      it->second.last_used_frame = frame_number;
    }
    if (it != g_r.tex_store.end() && it->second.valid &&
        frame_number >= it->second.recheck_frame &&
        REXCVAR_GET(skate3_native_render_scene_tex_revalidate)) {
      // 2-frame cadence in menu/editor contexts, like the item-draw poll
      // (in-place CAS composite rewrites during edits).
      it->second.recheck_frame =
          frame_number +
          (g_in_menus_frame.load(std::memory_order_relaxed) ? 2 : 16);
      const uint64_t fp = SampleProbeFingerprint(base, it->second);
      const bool fp_new = fp != 0 && fp != it->second.payload_fp;
      if (it->second.recheck_count < 3) {
        ++it->second.recheck_count;
      }
      if (it->second.incomplete || fp_new) {
        // In-place content rotation (event ads stream the next poster into
        // the same buffer; the words, and so the key, never change): heal
        // on the workers; keep serving the current decode meanwhile.
        EnqueueWordsMiss(key, it->second.fetch_words);
      }
    }
    return it;
  };
  // Resolve six raw fetch-constant words (a draw-time streamed-artwork
  // binding with no guest texture object) through the words-keyed cache.
  // `site` identifies the consuming slot (mesh << 1 | slot) for the sticky
  // fallback: streaming rebinds NEW mip words as you approach the art (the
  // cache key changes wholesale), and dropping to the placeholder / channel
  // diffuse for the worker round trip was the visible poster/decal flash;
  // the site's previous art keeps serving until the new decode lands.
  // Returns null only when nothing ever decoded for this site.
  const auto resolve_fetch_words = [&](const uint32_t words[6], uint64_t site,
                                       bool retained) -> const GuestTexture* {
    const uint64_t fkey = FetchWordsKey(words);
    auto fit = find_words_texture(fkey);
    if (fit == g_r.tex_store.end()) {
      if (!retained) {
        EnqueueWordsMiss(fkey, words);
      }
    } else if (fit->second.valid) {
      g_r.words_sticky[site] = fkey;
      return &fit->second;
    } else if (!retained && frame_number >= fit->second.retry_after_frame) {
      // Failed words decode (payload was mid-stream at first sight): keep
      // retrying; without this the entry negative-cached until the words
      // changed again.
      EnqueueWordsMiss(fkey, words);
      fit->second.retry_after_frame = frame_number + 30;
    }
    const auto pit = g_r.words_sticky.find(site);
    if (pit != g_r.words_sticky.end() && pit->second != fkey) {
      const auto old = g_r.tex_store.find(pit->second);
      if (old != g_r.tex_store.end() && old->second.valid) {
        old->second.last_used_frame = frame_number;
        g_ad_stale_served.fetch_add(1, std::memory_order_relaxed);
        return &old->second;
      }
    }
    // Nothing decoded for this site yet: the caller falls back to the
    // channel diffuse (the baked placeholder poster). If the "decal flash
    // and replace" report survives the dense-probe fix, a climbing none=
    // with a capped log here names this path as the flasher.
    g_ad_placeholder.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<uint32_t> s_ad_logs{0};
    if (!retained && s_ad_logs.fetch_add(1) < 24) {
      REXLOG_INFO(
          "native-scene: ad/decal words site={:X} fkey={:016X} -> placeholder "
          "(no decode yet) words=[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]",
          site, fkey, words[0], words[1], words[2], words[3], words[4],
          words[5]);
    }
    return nullptr;
  };

  // ---- SSR reflective-item capture (see ApplySsrPass) ----
  // draw_item records each reflective item's fully-staged draw state
  // (constants + resolved texture views) so the reflection G-buffer pass
  // after the scene pass re-renders exactly what the main pass drew.
  struct SsrGbufItem {
    float constants[52];
    nrhi::TextureView* t3;  // macro slot (water ripple normal map)
    nrhi::TextureView* t4;  // spec/reflection masks
    nrhi::TextureView* t5;  // fam 5/6 normal map (white when unpaired)
    uint32_t mesh;
    uint64_t fingerprint;
    const DrawItem* item;
  };
  std::vector<SsrGbufItem> ssr_items;
  // Exact diffuse page served by each presentation draw. The DXR mirror is
  // built after the main pass, so retain the resolved GuestTexture identity
  // here rather than independently resolving streaming state a second time.
  std::unordered_map<const DrawItem*, const GuestTexture*>
      raytraced_diffuse_by_item;
  const bool ssr_on =
      hdr_on && use_depth && debug_mode == 0 && !loading_native &&
      !g_r.ssr_failed && REXCVAR_GET(skate3_native_render_scene_ssr) &&
      // The published projection must be the live perspective matrix (same
      // gate as the SSAO pass).
      scene.proj[11] == 1.0f && scene.proj[0] != 0.0f &&
      scene.proj[5] != 0.0f && scene.proj[14] != 0.0f;

  // Deep per-item profiling (perf-items log line): stage timestamps inside
  // draw_item plus an in/out-of-frustum split of each item's total draw
  // cost. Sampled once per RenderScene call; the per-item clock reads only
  // happen while the cvar is on.
  const bool prof_items = REXCVAR_GET(skate3_native_render_scene_perf_items);
  const auto draw_item = [&](const DrawItem& item) {
    // Stage split points (profiling only): t0..t1 mesh serve, t1..t2 texture
    // serve, t2..t3 constants assembly, t3..end binds + draw recording.
    // Early returns skip the stage windows; the caller's in/out totals still
    // capture them.
    PerfClock::time_point di_t0{}, di_t1{}, di_t2{}, di_t3{};
    if (prof_items) {
      di_t0 = PerfClock::now();
    }
    // NO per-frame inline decodes here. Static content (world geometry,
    // props) loads/heals on the decode workers via the miss queue; a
    // texture decode averages ~10 ms and panning surfaces dozens of new
    // payloads in one frame; inline decode WAS the panning lag spike.
    // Dynamic payloads (skinned, CPU-rewritten every frame) are kept
    // fresh by the dyn decode jobs (guest-thread snapshot -> worker), one
    // frame behind the sim; only their FIRST sight decodes inline. The
    // cloth-quads particle path (gated off by default) still re-decodes
    // inline on change; it has no job route.
    // EXCEPTION: ROPA garments decode INLINE (skate3_native_render_scene_
    // ropa_inline): the worker route put the GPU-resident cloth shape 1-2
    // frames behind the body, visible as jelly/clip-through-torso during
    // direction changes (the shape only pauses when motion is steady). A
    // garment decode is sub-millisecond (a few hundred verts; it was
    // lumped in with the expensive texture decodes in the perf overhaul),
    // and the game's ping-pong double buffer makes the LIVE read tear-safe
    // (the sim writes the other half).
    const bool ropa_inline =
        item.ropa && REXCVAR_GET(skate3_native_render_scene_ropa_inline);
    const bool dynamic_payload = item.skinned || item.cloth_quads || item.ropa;
    auto it = g_r.meshes.find(item.mesh);
    if (item.retained &&
        (it == g_r.meshes.end() || it->second.fingerprint != item.fingerprint)) {
      // Retained off-screen items (edge-of-view guard band) outlive their
      // guest-side lifetime guarantees; the arena may have streamed out or
      // been reused since capture. Draw only the exact cached decode; no
      // guest reads, no heals, no miss enqueues from here.
      return;
    }
    if (it != g_r.meshes.end() && it->second.fingerprint != item.fingerprint &&
        REXCVAR_GET(skate3_native_render_scene_mesh_revalidate)) {
      if (item.cloth_quads || ropa_inline) {
        PoolMeshBuffer(g_r.device, it->second.vb);
        PoolMeshBuffer(g_r.device, it->second.ib);
        g_r.meshes.erase(it);
        it = g_r.meshes.end();
      } else if (!dynamic_payload) {
        // Streaming heal: keep drawing the old decode this frame; the
        // workers decode the new payload and the commit swaps it in.
        EnqueueMeshMiss(item.mesh);
      }
    }
    if (it == g_r.meshes.end()) {
      if (!item.cloth_quads && !ropa_inline) {
        // ALL mesh misses decode on the workers, including first-sight
        // skinned entities (a streamed-in NPC appears 1-2 frames late
        // instead of hitching the frame; the dyn decode jobs usually land
        // the same content even sooner from the guest-thread snapshot).
        EnqueueMeshMiss(item.mesh);
        g_rr_mesh_deferred.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      if (mesh_budget > 0 && mesh_decodes >= uint32_t(mesh_budget)) {
        g_rr_mesh_deferred.fetch_add(1, std::memory_order_relaxed);
        return;  // decodes on a later frame
      }
      ++mesh_decodes;
      const auto decode_t0 = PerfClock::now();
      MeshBuffers buffers;
      const bool decode_ok = DecodeMesh(g_r.device, base, item, buffers);
      g_pw_mesh_decode.Add(perf_ns_since(decode_t0));
      if (!decode_ok) {
        g_rr_decode_fail.fetch_add(1, std::memory_order_relaxed);
        static std::unordered_set<uint32_t> logged;
        if (logged.size() < 32 && logged.insert(item.mesh).second) {
          REXLOG_WARN("native-scene: DecodeMesh FAILED mesh={:08X} vb={:08X} fmt={} stride={}",
                      item.mesh, item.vb_addr, item.pos_fmt, item.stride);
        }
        return;
      }
      buffers.fingerprint = item.fingerprint;
      it = g_r.meshes.emplace(item.mesh, buffers).first;
    }
    it->second.last_used_frame = frame_number;
    // Loading prewarm reconstructs meshes from their bare mesh records,
    // before per-instance character/skinning provenance exists. Such a
    // cache entry is perfectly valid for raster drawing, but lacks the CPU
    // decoded copy needed by the mirror. Fill that sidecar once from the
    // live, fully classified item; keep the already-resident raster buffers.
    if (item.dyn_entity &&
        it->second.raytracing_verts.empty() &&
        !item.retained) {
      MeshBuffers raytracing_decode;
      if (DecodeMesh(
              g_r.device, base, item, raytracing_decode)) {
        it->second.raytracing_verts =
            std::move(raytracing_decode.raytracing_verts);
        it->second.raytracing_indices =
            std::move(raytracing_decode.raytracing_indices);
        if (item.ropa && !raytracing_decode.ropa_verts.empty()) {
          it->second.ropa_verts =
              std::move(raytracing_decode.ropa_verts);
        }
        PoolMeshBuffer(g_r.device, raytracing_decode.vb);
        PoolMeshBuffer(g_r.device, raytracing_decode.ib);
      }
    }
    const MeshBuffers& buffers = it->second;
    if (prof_items) {
      di_t1 = PerfClock::now();
    }

    // Double-sided sheet props draw with backface culling; without it the
    // front/back copies z-fight into lightmap flicker at range (banners/
    // flags). Opaque depth pass only; a mirrored instance (negative world
    // determinant) would flip winding, so those stay uncull(ed).
    // (hair items with a validated lighting capture draw in the blended
    // sub-pass under their own cull PSOs; never reset those here)
    const bool hair_pass = item.char_family >= 4 && item.char_family <= 5 &&
                           item.char_rows[14 * 4 + 1] > 0.0f;
    // reflective_trans glass draws in the blended sub-pass whenever its
    // exact branch is live (same gate as the sub-pass routing below); the
    // opaque cull-PSO reset must not fire there.
    const bool refl_trans_pass =
        item.env_family == 13 && debug_mode == 0 && scene.shadow_valid;
    // Character items mid-fade (spawn settle / distance) draw in the blended
    // sub-pass too (same gate as the routing below), same exemption.
    const bool char_fade_pass =
        (item.char_family == 1 || item.char_family == 2 ||
         item.char_family == 3 || item.char_family == 6) &&
        debug_mode == 0 && CharFadeAlpha(item) < 0.999f &&
        REXCVAR_GET(skate3_native_render_scene_entity_fade);
    if (use_depth && !item.transparent && !item.water && !hair_pass &&
        !refl_trans_pass && !char_fade_pass && g_r.pso_cullback != nullptr) {
      const float* w = item.world;
      const float det3 = w[0] * (w[5] * w[10] - w[6] * w[9]) -
                         w[1] * (w[4] * w[10] - w[6] * w[8]) +
                         w[2] * (w[4] * w[9] - w[5] * w[8]);
      // Game-parity backface culling: every world environment material's
      // XML sets CULLMODE=FRONT (== our CULL_FRONT; the banner work
      // calibrated game-kept faces as our D3D12 BACK faces). CULL_NONE
      // showed interior/away faces the game never renders, e.g. the
      // building wall's inside face stacking behind the translucent canopy
      // glass. Trees/alphatest (fams 7/9/10: leaf/fence cards read from
      // both sides) and mirrored instances (flipped winding) stay
      // uncull(ed), matching the two-sided-sheet rules.
      const bool cull_family =
          REXCVAR_GET(skate3_native_render_scene_backface_cull) &&
          item.env_family != 0 && item.env_family != 7 &&
          item.env_family != 9 && item.env_family != 10 &&
          item.env_family != 13;
      nrhi::Pipeline* want =
          ((buffers.two_sided_sheet || cull_family) && det3 >= 0.0f)
              ? g_r.pso_cullback
              : g_r.pso;
      if (want != scene_pso_bound) {
        cmd->SetPipeline(want);
        scene_pso_bound = want;
      }
    }

    // Resolve guest textures (white fallback) through the words-keyed
    // content store: object -> stable live words -> store entry
    // (console identity semantics; a
    // retargeted or reused object is just a different key, never a stale
    // serve). The object addresses were readable on the game thread this
    // frame; NO VirtualQuery here; it takes the process VAD lock, which
    // the guest streaming threads hammer, and ~2 calls per item stalled
    // the whole renderer to 3 fps.
    // Set by resolve_texture_raw when it returns the white fallback while a
    // decode is in flight (first-sight miss or a still-failing heal); the
    // sticky wrapper below then serves the item's last-good texture instead.
    bool tex_pending = false;
    const auto resolve_texture_raw = [&](uint32_t tex_ptr) -> const GuestTexture* {
      if (tex_ptr == 0) {
        return &g_r.white;
      }
      const bool trm = g_trace_mesh_addr != 0 && item.mesh == g_trace_mesh_addr;
      auto rit = g_r.tex_routes.find(tex_ptr);
      if (!item.retained) {
        // Route refresh: seqlock-stable read of the live fetch words (a
        // mid-rewrite mixed snapshot must never become a key; it would
        // decode a coherent image of the WRONG memory, the pool-page
        // collage class). An unstable read keeps the previous route for a
        // frame. Retained items skip all live reads: the object may be
        // gone; their route is frozen at retention.
        uint32_t live[6];
        if (ReadStableTexWords(base, tex_ptr, live)) {
          const bool demoted = (live[1] & 0xFFFFF000u) == 0u;
          if (rit == g_r.tex_routes.end()) {
            rit = g_r.tex_routes.emplace(tex_ptr, RendererState::TexRoute{})
                      .first;
            std::memcpy(rit->second.words, live, sizeof(live));
            rit->second.key = FetchWordsKey(live);
            rit->second.demoted = demoted;
          } else if (demoted) {
            // Mip-0 demoted (base address cleared; the old pool range is
            // already reused): hold the pre-demote route; its decode
            // carries the full chain, strictly better, and suspend its
            // payload polls (the probes would read the reused pool and
            // heal in foreign bytes). A re-promote publishes fresh words
            // and re-routes.
            if (!rit->second.demoted) {
              rit->second.demoted = true;
              g_demote_hold.fetch_add(1, std::memory_order_relaxed);
              if (trm) {
                REXLOG_INFO("tex-trace: f{} obj={:08X} DEMOTED (route held, "
                            "polls suspended)",
                            frame_number, tex_ptr);
              }
            }
          } else if (std::memcmp(live, rit->second.words, sizeof(live)) != 0) {
            if (trm) {
              REXLOG_INFO(
                  "tex-trace: f{} obj={:08X} REROUTE old=[{:08X} {:08X} "
                  "{:08X} {:08X} {:08X} {:08X}] new=[{:08X} {:08X} {:08X} "
                  "{:08X} {:08X} {:08X}]",
                  frame_number, tex_ptr, rit->second.words[0],
                  rit->second.words[1], rit->second.words[2],
                  rit->second.words[3], rit->second.words[4],
                  rit->second.words[5], live[0], live[1], live[2], live[3],
                  live[4], live[5]);
            }
            std::memcpy(rit->second.words, live, sizeof(live));
            rit->second.key = FetchWordsKey(live);
            rit->second.demoted = false;
          } else {
            rit->second.demoted = false;
          }
        } else if (trm) {
          REXLOG_INFO("tex-trace: f{} obj={:08X} words UNSTABLE (mid-rewrite)",
                      frame_number, tex_ptr);
        }
      }
      if (rit == g_r.tex_routes.end()) {
        // Unreadable/unstable object with no prior route: nothing safe to
        // serve or decode yet; next frame's read settles it.
        tex_pending = true;
        return &g_r.white;
      }
      const RendererState::TexRoute& route = rit->second;
      auto sit = g_r.tex_store.find(route.key);
      if (sit == g_r.tex_store.end()) {
        if (item.retained || route.demoted) {
          // Retained: no enqueues, no live reads. Demote-held with nothing
          // cached: the pre-demote pool may already be reused; a decode
          // would commit foreign bytes under a good key. White/sticky until
          // a re-promote publishes live words.
          tex_pending = true;
          return &g_r.white;
        }
        // Decode on the workers; white/sticky for the 1-3 frames that takes
        // (inline decode measured ~10 ms avg / ~70 ms max, the panning
        // lag spikes).
        if (trm) {
          REXLOG_INFO("tex-trace: f{} obj={:08X} MISS key={:016X} enqueued",
                      frame_number, tex_ptr, route.key);
        }
        EnqueueWordsMiss(route.key, route.words);
        g_rr_tex_deferred.fetch_add(1, std::memory_order_relaxed);
        tex_pending = true;
        return &g_r.white;
      }
      GuestTexture& e = sit->second;
      e.last_used_frame = frame_number;
      if (!e.valid) {
        // Failed decode: retry on its backoff clock (the payload usually
        // lands within a few frames of first sight; the commit stamps the
        // real backoff).
        if (!item.retained && !route.demoted &&
            frame_number >= e.retry_after_frame) {
          e.retry_after_frame = frame_number + 120;
          EnqueueWordsMiss(route.key, route.words);
        }
        if (trm) {
          REXLOG_INFO("tex-trace: f{} obj={:08X} INVALID entry key={:016X} "
                      "(retry at f{})",
                      frame_number, tex_ptr, route.key, e.retry_after_frame);
        }
        tex_pending = true;
        return &g_r.white;
      }
      // Payload revalidation, the one irreducible heuristic: the game
      // streams content IN PLACE at addresses the words already point to
      // (event-ad rotation, mip-pool fills, composed lightmap pages) with
      // no CPU-visible event. Escalating cadence (2/4/8 then 16): a decode
      // taken while the payload was still streaming reads back garbage;
      // fresh entries re-verify fast, then settle to the cheap steady
      // cadence. An incomplete decode (truncated tiled-mip copy) and a
      // near_black verdict re-decode regardless of the mip-0 fingerprint;
      // the commit's same-content dedup absorbs the no-ops. Suspended while
      // the route is demote-held (the probes would read the reused pool).
      if (!item.retained && !route.demoted &&
          frame_number >= e.recheck_frame &&
          REXCVAR_GET(skate3_native_render_scene_tex_revalidate)) {
        // Menu/editor contexts poll STEADY entries on a 2-frame cadence
        // (mirrors the 2D resolver): the CAS editor recomposes the skater's
        // skin/garment textures IN PLACE progressively over ~a second on
        // every edit, and the 16-frame steady cadence made each composite
        // step land ~100+ ms late, desynchronized across the pieces, the
        // "broken for 1-2 s after changing skin color" state. Gameplay
        // keeps the cheap steady cadence.
        e.recheck_frame =
            frame_number +
            (e.recheck_count < 3
                 ? (2ull << e.recheck_count)
                 : (g_in_menus_frame.load(std::memory_order_relaxed) ? 2ull
                                                                     : 16ull));
        const uint64_t fp = SampleProbeFingerprint(base, e);
        const bool fp_new = fp != 0 && fp != e.payload_fp;
        if (trm) {
          REXLOG_INFO(
              "tex-trace: f{} obj={:08X} poll key={:016X} fp={:016X} "
              "cached={:016X} new={} inc={} nb={} cnt={}",
              frame_number, tex_ptr, route.key, fp, e.payload_fp,
              fp_new ? 1 : 0, e.incomplete ? 1 : 0, e.near_black ? 1 : 0,
              e.recheck_count);
        }
        if (e.recheck_count < 3) {
          ++e.recheck_count;
        }
        if (fp_new || e.incomplete || (e.near_black && e.nb_redecodes < 3)) {
          // Re-decode churn diagnostic: repeated in-place payload changes
          // on one key = streaming oscillation, visible as texture flicker
          // on the affected meshes.
          static std::atomic<uint32_t> s_redecode_logs{0};
          if (s_redecode_logs.fetch_add(1) < 256) {
            REXLOG_INFO(
                "native-scene: texture re-decode key={:016X} reason={}{}{} "
                "words=[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]",
                route.key, fp_new ? "fp" : "", e.incomplete ? "inc" : "",
                e.near_black ? "nb" : "", e.fetch_words[0], e.fetch_words[1],
                e.fetch_words[2], e.fetch_words[3], e.fetch_words[4],
                e.fetch_words[5]);
          }
          if (trm) {
            REXLOG_INFO("tex-trace: f{} obj={:08X} ENQUEUE heal key={:016X} "
                        "reason={}{}{}",
                        frame_number, tex_ptr, route.key, fp_new ? "fp" : "",
                        e.incomplete ? "inc" : "", e.near_black ? "nb" : "");
          }
          EnqueueWordsMiss(route.key, e.fetch_words);
        }
      }
      return &e;
    };
    // Sticky serving: streaming rotates content onto NEW texture objects (a
    // mip promote is usually a fresh object: 287 first-sight objects vs 73
    // in-place rebinds in one 45 s traversal), so a plain
    // cache miss white-flashed content that was on screen with the previous
    // mip one frame earlier. While the new object's decode is in flight,
    // serve the last texture this item slot successfully resolved, the
    // same visual as the console's own mip transition. slot: 0 diffuse,
    // 1 lightmap, 2 macro, 3 normal/ripple, 4 decal art, 5 hair, 6 spec.
    const auto resolve_texture = [&](uint32_t tex_ptr,
                                     uint32_t slot) -> const GuestTexture* {
      tex_pending = false;
      const GuestTexture* t = resolve_texture_raw(tex_ptr);
      // Near-uniform-black decodes on the WHITE-NEUTRAL slots (1 lightmap,
      // 2 macro) serve the white fallback until a heal lands real content.
      // Lightmap: a real-but-black page binds with tint.r > 0 and the CSM
      // min-clamp renders the surface BLACK (the door 59810af's shader gate
      // cannot see). Macro: since 59810af the weathering multiplies OVER
      // the composited decal art, so a mid-stream dark macro decode turns
      // the paint into the black-square flash; white is the macro's
      // authored neutral (materials without weathering bind default_white).
      // Slots with legitimately dark content (diffuse, decal art, spec)
      // are untouched. Applied in the wrapper so retained items get the
      // same protection.
      const bool trm = g_trace_mesh_addr != 0 && item.mesh == g_trace_mesh_addr;
      if ((slot == 1 || slot == 2) && t != &g_r.white && t->valid &&
          t->near_black) {
        if (trm) {
          REXLOG_INFO("tex-trace: f{} slot={} obj={:08X} NEAR-BLACK -> white",
                      frame_number, slot, tex_ptr);
        }
        static std::atomic<uint32_t> s_nb_logs{0};
        if (s_nb_logs.fetch_add(1, std::memory_order_relaxed) < 24) {
          REXLOG_INFO(
              "native-scene: near-black decode obj={:08X} slot={} served as "
              "white fallback (mid-compose/stream content)",
              tex_ptr, slot);
        }
        return &g_r.white;
      }
      if (item.retained || tex_ptr == 0) {
        return t;
      }
      const uint64_t skey = (uint64_t(item.mesh) << 3) | slot;
      if (t != &g_r.white) {
        RendererState::TexStickySite& site = g_r.tex_sticky[skey];
        // Material-detail downgrade hold: the game's streaming demotes a
        // nearby mesh to its UN (undetailed) material for a fraction of a
        // second and back, the visible flash to completely different/
        // blurry art. Under words identity a demote is just a smaller-area
        // key; the site's last-adopted decode is still resident in the
        // store, so keep serving it while the downgrade is young. A
        // persistent downgrade (a real demote as the player leaves) adopts
        // after the hold window.
        const int32_t hold = REXCVAR_GET(skate3_native_render_scene_detail_hold);
        const uint64_t cur_key = FetchWordsKey(t->fetch_words);
        const uint64_t cur_area = FetchWordsArea(t->fetch_words);
        if (hold > 0 && site.area > cur_area && site.words_key != 0) {
          const auto hit = g_r.tex_store.find(site.words_key);
          const GuestTexture* held =
              hit != g_r.tex_store.end() && hit->second.valid ? &hit->second
                                                              : nullptr;
          if (held != nullptr) {
            // The held entry may no longer be polled by any live route;
            // re-probe its payload on its own recheck clock so a reused
            // pool page abandons the hold instead of serving foreign
            // bytes.
            hit->second.last_used_frame = frame_number;
            if (frame_number >= hit->second.recheck_frame) {
              hit->second.recheck_frame = frame_number + 16;
              const uint64_t fp = SampleProbeFingerprint(base, hit->second);
              if (fp == 0 || fp != hit->second.payload_fp) {
                held = nullptr;
              }
            }
          }
          if (held != nullptr) {
            if (site.downgrade_since == 0) {
              site.downgrade_since = frame_number;
              static std::atomic<uint32_t> s_hold_logs{0};
              if (s_hold_logs.fetch_add(1, std::memory_order_relaxed) < 24) {
                REXLOG_INFO(
                    "native-scene: detail-downgrade HOLD mesh={:08X} slot={} "
                    "held={:016X} bound={:016X} area {} -> {}",
                    item.mesh, slot, site.words_key, cur_key, site.area,
                    cur_area);
              }
            }
            if (frame_number - site.downgrade_since < uint64_t(hold)) {
              if (trm && ((frame_number - site.downgrade_since) & 63u) == 0) {
                REXLOG_INFO(
                    "tex-trace: f{} slot={} DETAIL-HOLD serving fp={:016X} "
                    "(bound area {} < {})",
                    frame_number, slot, held->payload_fp, cur_area,
                    site.area);
              }
              return held;
            }
          }
        }
        if (trm && site.words_key != 0 && site.words_key != cur_key) {
          REXLOG_INFO(
              "tex-trace: f{} slot={} ADOPT key {:016X}(area {}) -> "
              "{:016X}(area {})",
              frame_number, slot, site.words_key, site.area, cur_key,
              cur_area);
        }
        site.words_key = cur_key;
        site.area = cur_area;
        site.downgrade_since = 0;
        if (!g_r.tex_pending_first.empty()) {
          g_r.tex_pending_first.erase(tex_ptr);
        }
        return t;
      }
      if (tex_pending) {
        // The current binding's decode is in flight: serve the site's
        // previous art from the store, the console's own mip-transition
        // look instead of a white flash.
        const auto sit = g_r.tex_sticky.find(skey);
        if (sit != g_r.tex_sticky.end() && sit->second.words_key != 0) {
          const auto old = g_r.tex_store.find(sit->second.words_key);
          if (old != g_r.tex_store.end() && old->second.valid) {
            old->second.last_used_frame = frame_number;
            g_tex_sticky_served.fetch_add(1, std::memory_order_relaxed);
            if (trm) {
              REXLOG_INFO("tex-trace: f{} slot={} STICKY serving key={:016X} "
                          "(pending {:08X})",
                          frame_number, slot, sit->second.words_key, tex_ptr);
            }
            return &old->second;
          }
        }
      }
      if (trm && t == &g_r.white) {
        REXLOG_INFO("tex-trace: f{} slot={} serving WHITE (req {:08X})",
                    frame_number, slot, tex_ptr);
      }
      return t;
    };
    // Streamed-artwork diffuse override (see DrawItem::diffuse_fetch): the
    // real art exists only as draw-time fetch words; resolve those through
    // the words-keyed cache (shared with the 2D pass; the art has no guest
    // object to key on).
    const GuestTexture* diffuse =
        item.diffuse_fetch[1] != 0
            ? resolve_fetch_words(item.diffuse_fetch, uint64_t(item.mesh) << 1,
                                  item.retained)
            : nullptr;
    if (diffuse == nullptr) {
      diffuse = resolve_texture(item.diffuse_tex, 0);
      if (diffuse == &g_r.white && tex_pending && item.diffuse_tex != 0 &&
          !item.retained) {
        // Brand-new content, first decode in flight, and no previous mip
        // cached to serve: draw NOTHING for a short window instead of a
        // white flash: the emulated pop-in semantics (the game shows
        // streamed content only once its data is ready). Bounded so a
        // permanently failing texture still falls back to visible white.
        const auto [pit, fresh] =
            g_r.tex_pending_first.try_emplace(item.diffuse_tex, frame_number);
        if (frame_number - pit->second < 20) {
          g_skip_new.fetch_add(1, std::memory_order_relaxed);
          return;
        }
      }
    }
    if (item.dyn_entity && diffuse != nullptr &&
        diffuse->texture != nullptr && diffuse->srv != nullptr) {
      raytraced_diffuse_by_item[&item] = diffuse;
    }
    const GuestTexture* lightmap =
        item.lightmap_tex != 0 && REXCVAR_GET(skate3_native_render_scene_lightmaps)
            ? resolve_texture(item.lightmap_tex, 1)
            : nullptr;
    if (lightmap == &g_r.white) {
      lightmap = nullptr;
    }

    // constants = world + mvp (world * view_proj, row-vector) + tint + cam
    // + material tint + overlay params + misc. tint.a > 0 selects debug
    // solid colors; tint.r > 0 marks a bound lightmap. For transparent
    // items misc.yzw carries the fog ramp and mat_tint the fog color.
    if (prof_items) {
      di_t2 = PerfClock::now();
    }
    float constants[52] = {};
    std::memcpy(constants, item.world, sizeof(item.world));
    if (item.unlit) {
      // sky.*: the dome mesh is CAMERA-RELATIVE (sky.fx defaultVS adds
      // g_vViewPos to every vertex). Anchoring it at the world origin put
      // the baked skyline panorama ~700 m off, visibly rotated/parallaxed
      // against the emulated frame. The game's sky viewpos tracks the camera
      // in x/z but pins Y at the level's fixed sky elevation (captured per
      // frame from the sky draw's VS bank; 165.0 in every capture); using
      // cam.y rendered the skyline ~160 m too LOW.
      constants[12] += scene.cam_pos[0];
      constants[13] += scene.sky_height;
      constants[14] += scene.cam_pos[2];
    }
    float* mvp = constants + 16;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        float sum = 0.0f;
        for (int k = 0; k < 4; ++k) {
          sum += constants[r * 4 + k] * scene.view_proj[k * 4 + c];
        }
        mvp[r * 4 + c] = sum;
      }
    }
    // Bone palette upload for skinned items; tint.g flags skinning.
    bool bones_bound = false;
    if (item.skinned && !item.bones.empty()) {
      const uint32_t bytes = uint32_t(item.bones.size() * sizeof(float));
      const uint32_t offset = (g_r.bone_ring_offset + 255u) & ~255u;
      if (offset + bytes <= RendererState::kBoneRegionSize) {
        std::memcpy(g_r.bone_ring_cpu + bone_region + offset, item.bones.data(), bytes);
        g_r.bone_ring_offset = offset + bytes;
        cmd->SetBufferSrv(3, g_r.bone_ring, bone_region + offset);
        bones_bound = true;
      }
    }
    if (!bones_bound) {
      if (item.skinned && !item.bones.empty()) {
        // Ring exhausted: this item renders bind-pose at identity,
        // effectively invisible. Must never happen silently.
        g_rr_no_bones.fetch_add(1, std::memory_order_relaxed);
      }
      cmd->SetBufferSrv(3, g_r.bone_ring, 0);
    }

    if (debug_mode >= 2) {
      // Stable per-object colors: hash the mesh address, not the (sort-order
      // dependent) item index.
      const uint32_t hash = (item.mesh >> 4) * 2654435761u;
      constants[32] = float((hash >> 0) & 0xFF) / 255.0f;
      constants[33] = float((hash >> 8) & 0xFF) / 255.0f;
      constants[34] = float((hash >> 16) & 0xFF) / 255.0f;
      constants[35] = 1.0f;
    } else {
      constants[32] = lightmap != nullptr ? 1.0f : 0.0f;
      // tint.g doubles as the "character: alpha = gloss, no alpha-test"
      // marker; ropa garments rendered rigid have no bones but must not be
      // alpha-clipped (their VS skinning branch is inert: zero weights).
      constants[33] = (bones_bound || item.ropa) ? 1.0f : 0.0f;
      constants[34] = item.unlit ? 1.0f : 0.0f;
      // tint.a < 0 marks dynamic-entity items for the showcase dyn layer
      // (ps_main clips them until the bit reveals); the solid-color
      // early-out only ever reads tint.a > 0. Staged only while a showcase
      // frame is live (nonzero split rows) so normal rendering never
      // carries the marker at all.
      constants[35] = (item.dyn_entity && (g_r.showcase_rows[0] != 0.0f ||
                                           g_r.showcase_rows[1] != 0.0f))
                          ? -1.0f
                          : 0.0f;
    }
    constants[36] = scene.cam_pos[0];
    constants[37] = scene.cam_pos[1];
    constants[38] = scene.cam_pos[2];
    // cam_pos.w = character shading family: > 0 switches the PS to the
    // captured character-lighting branch (rows uploaded to b2 below);
    // char_rows[14*4+1] stays 0 when the capture failed validation, which
    // keeps the item on the legacy empirical shading.
    constants[39] = 0.0f;
    if (debug_mode == 0 && item.char_family != 0 &&
        item.char_rows[14 * 4 + 1] > 0.0f) {
      const uint32_t offset = (g_r.bone_ring_offset + 255u) & ~255u;
      // 18 float4 rows = 288 bytes -> a 512-byte slot keeps the next
      // allocation 256-aligned (CBV offset requirement).
      if (offset + 512u <= RendererState::kBoneRegionSize) {
        std::memcpy(g_r.bone_ring_cpu + bone_region + offset, item.char_rows,
                    sizeof(item.char_rows));
        g_r.bone_ring_offset = offset + 512u;
        cmd->SetConstantBuffer(9, g_r.bone_ring, bone_region + offset);
        constants[39] = item.char_rows[14 * 4 + 1];
        g_char_drawn.fetch_add(1, std::memory_order_relaxed);
      }
    }
    // cam_pos.w = -family selects the exact world-material branch. Gated on
    // the frame rows being captured (scene.shadow_valid carries the scene
    // exposure / material multiplier at b1); without them the tone chain
    // would multiply by zero and render black.
    if (debug_mode == 0 && item.env_family != 0 && scene.shadow_valid &&
        !item.water && !item.transparent &&
        // Fam 14 needs the frame's scroll rows (time + multiplier); the
        // tone chain would multiply by zero without them; the legacy
        // shading covers the capture-less frames.
        (item.env_family != 14 || scene.scroll_valid)) {
      constants[39] = -float(item.env_family);
    }
    // cam_pos.w = -(20 + variant) selects the exact dynamicobject branch
    // (rigid props). Gated on the frame's dynamicobject lighting rows having
    // been captured (dyn_* at b1); without them the tone chain multiplies by
    // zero and renders black; fall back to the legacy shading otherwise.
    if (debug_mode == 0 && item.dynobj != 0 && scene.dynobj_valid) {
      constants[39] = -float(20 + item.dynobj);
      g_dynobj_drawn.fetch_add(1, std::memory_order_relaxed);
    }
    std::memcpy(constants + 40, item.tint, 4 * sizeof(float));
    // t3 = macro grime/crack overlay, t4 = decal art for environment.decal
    // surfaces (in-shader composite). Independent slots: decal ground/wall
    // sections carry the same macrooverlay as their non-decal neighbors, and
    // binding only the art dropped the macro multiply there; alternating
    // plaza sections rendered ~1.4x too bright (the ground checkerboard).
    const GuestTexture* macro_tex =
        item.macro_tex != 0 && REXCVAR_GET(skate3_native_render_scene_macro)
            ? resolve_texture(item.macro_tex, 2)
            : &g_r.white;
    if ((item.water || item.char_family >= 6 ||
         (item.char_family >= 1 && item.char_family <= 2)) &&
        item.water_normal != 0) {
      // Water rides its ripple normal map in the macro slot (water never
      // carries a macro overlay; overlay.z stays 0 below so the macro
      // composite path never runs). Vehicles do the same with their DXN
      // panel normal map; without it the hinged panels' vertex normals
      // face away from the sun and shade as a dark ambient-blue patch that
      // stops at the door seams (the exact PS with a FLAT map reproduces
      // that artifact; with the real map it matches the emulated car).
      // Character fams 1/2 (defaultcharacter / CAC skin, face, cloth,
      // lenses) do the same with their DXT5nm `normal` channel, the
      // garment crease / skin pore detail the emulated render shows;
      // overlay.z > 0 tells the char branch the map resolved.
      macro_tex = resolve_texture(item.water_normal, 3);
    }
    // Water / vehicle environment cube (t6, root param 8): decoded once per
    // guest object into the cube cache; the gray fallback cube otherwise.
    // Vehicle materials carry an `environment` channel that resolves through
    // the same chan+0x1C path as the ocean's. Cube decodes run on the
    // workers (a SINGLE inline cube decode measured up to ~100 ms, the
    // largest remaining traversal hitch when a vehicle / reflective area
    // streamed in); flat-gray reflections for the 1-3 frames in flight are
    // invisible.
    const GuestTexture* cube_tex = &g_r.white_cube;
    if ((item.water || item.char_family >= 6 ||
         (item.env_family >= 5 && item.env_family <= 6) ||
         item.env_family == 13) &&
        item.water_env != 0) {
      auto cit = g_r.cube_textures.find(item.water_env);
      if (cit == g_r.cube_textures.end()) {
        if (!item.retained) {
          EnqueueCubeMiss(item.water_env);
        }
      } else if (cit->second.valid) {
        cube_tex = &cit->second;
        cit->second.last_used_frame = frame_number;
      }
    }
    const GuestTexture* decal_tex = item.decal && item.decal_art != 0 &&
                                            REXCVAR_GET(skate3_native_render_scene_decals)
                                        ? resolve_texture(item.decal_art, 4)
                                        : &g_r.white;
    // Streamed-artwork decal override (see DrawItem::decal_fetch): ad frames
    // covered by an environment.decal section get the current event-ad art
    // bound over the decal channel at draw time.
    if (item.decal && item.decal_fetch[1] != 0 &&
        REXCVAR_GET(skate3_native_render_scene_decals)) {
      const GuestTexture* ad = resolve_fetch_words(
          item.decal_fetch, (uint64_t(item.mesh) << 1) | 1, item.retained);
      if (ad != nullptr) {
        decal_tex = ad;
      }
    }
    if (((item.char_family >= 4 && item.char_family <= 5) || item.char_alpha) &&
        item.hair_alpha_tex != 0) {
      // Hair strand coverage rides the (otherwise unused) decal slot; the
      // PS hair branch samples it at the raw second texcoord. The white
      // fallback keeps failed decodes opaque rather than invisible.
      // character.alpha lenses use the same plumbing (ch_misc.z signals the
      // fam-1/2 branch to take alpha from this texture).
      decal_tex = resolve_texture(item.hair_alpha_tex, 5);
    }
    // F7 ring: stamp the SERVED content fingerprints (see SceneRingItem);
    // pointer identity cannot see an in-place content swap.
    if (ring_frame != nullptr) {
      const auto rit = ring_map.find(&item);
      if (rit != ring_map.end()) {
        SceneRingItem& ri = ring_frame->items[rit->second];
        const auto fp_of = [](const GuestTexture* t) -> uint64_t {
          return t != nullptr && t->valid ? t->payload_fp : 0;
        };
        ri.fp_diffuse = fp_of(diffuse);
        ri.fp_lightmap = fp_of(lightmap);
        ri.fp_macro = fp_of(macro_tex);
        ri.fp_decal = fp_of(decal_tex);
      }
    }
    if (g_trace_mesh_addr != 0 && item.mesh == g_trace_mesh_addr) {
      // Traced-mesh per-frame summary: SERVED state (post-resolve), logged
      // when anything changes. The mesh's texture objects also register for
      // the worker-commit trace.
      const auto fp_of = [](const GuestTexture* t) -> uint64_t {
        return t != nullptr && t->valid ? t->payload_fp : 0;
      };
      for (const GuestTexture* t :
           {diffuse, lightmap, macro_tex, decal_tex}) {
        if (t != nullptr && t != &g_r.white) {
          g_trace_keys.insert(FetchWordsKey(t->fetch_words));
        }
      }
      uint64_t sig = uint64_t(item.diffuse_tex) ^
                     (uint64_t(item.decal_art) << 16) ^
                     (uint64_t(item.lightmap_tex) << 32) ^
                     (uint64_t(item.macro_tex) << 48);
      sig ^= fp_of(diffuse) * 3 ^ fp_of(decal_tex) * 5 ^ fp_of(lightmap) * 7 ^
             fp_of(macro_tex) * 11;
      sig ^= item.retained ? 1 : 0;
      uint64_t& last = g_trace_sig[item.ctx];
      if (sig != last) {
        last = sig;
        REXLOG_INFO(
            "tex-trace: f{} SERVED ctx={:08X} retained={} "
            "dif={:08X}/{:016X} dec={:08X}/{:016X} lm={:08X}/{:016X} "
            "mac={:08X}/{:016X}",
            frame_number, item.ctx, item.retained ? 1 : 0, item.diffuse_tex,
            fp_of(diffuse), item.decal_art, fp_of(decal_tex),
            item.lightmap_tex, fp_of(lightmap), item.macro_tex,
            fp_of(macro_tex));
      }
    }
    // Exact env families without decal art bind the material's spec/ecc/
    // refmask map (or the animated.tree noise tint) in the free decal slot;
    // overlay.w == 3 tells the shader the masks are live.
    bool spec_bound = false;
    if (((item.env_family != 0 && !item.decal && item.env_family != 10) ||
         item.water_flowing) &&
        item.spec_tex != 0) {
      const GuestTexture* spec = resolve_texture(item.spec_tex, 6);
      if (spec != &g_r.white) {
        decal_tex = spec;
        spec_bound = true;
      }
    }
    // Sky dome: the material's `specular` channel is the 1D radial sun
    // gradient (512x16), bound in the free decal slot for the exact sky
    // branch. Until it decodes the dome falls back to the plain fullbright
    // panorama (sunless for the 1-3 frames in flight).
    bool sky_sun_bound = false;
    if (item.unlit && item.spec_tex != 0) {
      const GuestTexture* sun = resolve_texture(item.spec_tex, 6);
      if (sun != &g_r.white) {
        decal_tex = sun;
        sky_sun_bound = true;
      }
    }
    // Character skin/face spec-mask map (the cacstamp `specular` channel):
    // rides the free decal slot like the env families. Written to
    // overlay.w below: 3 = mask bound, 2 = channel present but not yet
    // decoded (the PS masks the spec OFF; the DXT1 skin diffuse's opaque
    // alpha would read as a full-white mask), 0 = no channel (the PS masks
    // by diffuse alpha^2, cloth/jeans). char_alpha lenses keep the decal
    // slot for their coverage texture.
    float char_spec = 0.0f;
    if ((item.char_family == 1 || item.char_family == 2) && !item.char_alpha &&
        item.spec_tex != 0) {
      const GuestTexture* spec = resolve_texture(item.spec_tex, 6);
      if (spec != &g_r.white && spec->valid && spec->srv_mips != 0) {
        decal_tex = spec;
        char_spec = 3.0f;
      } else {
        char_spec = 2.0f;
      }
    }
    const bool is_decal =
        item.char_family < 4 && item.decal && decal_tex != &g_r.white;
    // Fam 5/6 masks+normal pair (t4/t5): the reflective PS perturbs its
    // reflection/spec with the material's normal map (shader overlay.w == 4
    // branch, the fix for the giant flat-normal cube smear on glass
    // facades). Both views bind together at the draw via SetTexturePair;
    // pair descriptors are backend-cached, so prewarm/revalidation texture
    // swaps can never leave a stale descriptor. Until the normal map
    // decodes, overlay.w stays 3 (flat-normal reflection, the old behavior).
    const GuestTexture* pair_normal = nullptr;
    bool normal_paired = false;
    if (item.env_family >= 5 && item.env_family <= 6) {
      uint32_t gate = 0;
      if (!spec_bound) gate |= 1;
      if (item.water_normal == 0) gate |= 2;
      if (spec_bound && (!decal_tex->valid || decal_tex->srv_mips == 0)) gate |= 4;
      if (gate == 0) {
        const GuestTexture* nrm = resolve_texture(item.water_normal, 3);
        if (nrm == &g_r.white) {
          gate |= 8;
        } else {
          if (!nrm->valid) gate |= 16;
          if (nrm->srv_mips == 0) gate |= 32;
        }
        if (gate == 0) {
          pair_normal = nrm;
          normal_paired = true;
        }
      }
      if (normal_paired) {
        g_refl_pair.fetch_add(1, std::memory_order_relaxed);
      } else {
        g_refl_flat.fetch_add(1, std::memory_order_relaxed);
        g_refl_gate.store(gate, std::memory_order_relaxed);
      }
    }
    // World-shading v2 (fams 1-4): the material's real per-pixel maps. The
    // base normal map rides the free t5 pair slot (t4 holds the spec masks
    // or the decal art there), the detail normal map + the decal families'
    // spec/ecc masks ride the t8/t9 pair. misc.z carries the bind flags
    // (1 = normal, 2 = detail, 4 = spec2), misc.w the detailNormalUVScale
    // channel constant; both slots are spare on opaque fams 1-4 (fog only
    // uses them on transparent/water items, F12 only on fams 5/6/13).
    const GuestTexture* v2_detail = nullptr;
    const GuestTexture* v2_spec2 = nullptr;
    uint32_t v2_flags = 0;
    if (item.env_family >= 1 && item.env_family <= 4 && !item.transparent &&
        !item.water && debug_mode == 0 && scene.shadow_valid &&
        REXCVAR_GET(skate3_native_render_scene_world_v2)) {
      if (item.water_normal != 0) {
        const GuestTexture* nrm = resolve_texture(item.water_normal, 3);
        if (nrm != &g_r.white && nrm->valid && nrm->srv_mips != 0) {
          pair_normal = nrm;
          normal_paired = true;  // t5 pair bind; overlay.w stays 0..3 here
          v2_flags |= 1;
        }
      }
      // Fam 2 (environmentsimple) carries a plain normal map with no
      // base+detail pair; its vnd is the raw 2t-1 (shader-side).
      if ((v2_flags & 1u) != 0 && item.env_family != 2 &&
          item.detail_tex != 0 && item.detail_scale > 0.0f) {
        const GuestTexture* det = resolve_texture(item.detail_tex, 7);
        if (det != &g_r.white && det->valid && det->srv_mips != 0) {
          v2_detail = det;
          v2_flags |= 2;
        }
      }
      if (item.decal && item.spec_tex != 0) {
        const GuestTexture* sp = resolve_texture(item.spec_tex, 6);
        if (sp != &g_r.white && sp->valid && sp->srv_mips != 0) {
          v2_spec2 = sp;
          v2_flags |= 4;
        }
      }
    }
    // Dynamicobject v2: the same misc.z/w convention and t5/t8/t9 slots as
    // fams 1-4; dynobj draws are never transparent/water and carry
    // env_family 0, so both misc slots are free, and t4 stays on its white
    // fallback (the spec-mask t4 bind is env-family-only), so the t5 pair
    // costs nothing. The spec/ecc masks go to t9 rather than t4: it keeps
    // t4's semantics untouched and matches the decal families' 2-channel
    // mask convention. Gated exactly like the PS branch selection (dynobj
    // frame rows captured) plus the dynobj_v2 cvar for A/B against the v1
    // flat response.
    if (item.dynobj != 0 && debug_mode == 0 && scene.dynobj_valid &&
        REXCVAR_GET(skate3_native_render_scene_dynobj_v2)) {
      if (item.water_normal != 0) {
        const GuestTexture* nrm = resolve_texture(item.water_normal, 3);
        if (nrm != &g_r.white && nrm->valid && nrm->srv_mips != 0) {
          pair_normal = nrm;
          normal_paired = true;
          v2_flags |= 1;
        }
      }
      if ((v2_flags & 1u) != 0 && item.detail_tex != 0 &&
          item.detail_scale > 0.0f) {
        const GuestTexture* det = resolve_texture(item.detail_tex, 7);
        if (det != &g_r.white && det->valid && det->srv_mips != 0) {
          v2_detail = det;
          v2_flags |= 2;
        }
      }
      if (item.spec_tex != 0) {
        const GuestTexture* sp = resolve_texture(item.spec_tex, 6);
        if (sp != &g_r.white && sp->valid && sp->srv_mips != 0) {
          v2_spec2 = sp;
          v2_flags |= 4;
        }
      }
      // Static world-shadow map at t4 (flag 8): the native re-render of the
      // game's baked-shade map (see RenderShadowAtlas). t4 is otherwise the
      // white fallback on dynobj draws. Without it the PS world term is 1
      // and props inside baked building shade light at full key.
      if (g_r.world_shadow_srv != nullptr && g_r.world_shadow_in_srv &&
          scene.dynobj_ws_valid) {
        v2_flags |= 8;
      }
    }
    // Exact ocean (fam 31): the second PCA normal component rides the free
    // t8 pair slot and the 16x16 macro overlay the t9 slot (water items use
    // neither). v2_flags stays 0; the ocean branch signals its binds
    // through the overlay row.
    bool ocean_n2 = false;
    bool ocean_ov = false;
    if (item.water_ocean == 1 && debug_mode == 0 && scene.shadow_valid &&
        scene.ocean_valid) {
      if (item.water_normal2 != 0) {
        const GuestTexture* n2 = resolve_texture(item.water_normal2, 7);
        if (n2 != &g_r.white && n2->valid && n2->srv_mips != 0) {
          v2_detail = n2;
          ocean_n2 = true;
        }
      }
      if (item.macro_tex != 0) {
        const GuestTexture* mo = resolve_texture(item.macro_tex, 6);
        if (mo != &g_r.white && mo->valid && mo->srv_mips != 0) {
          v2_spec2 = mo;
          ocean_ov = true;
        }
      }
    }
    constants[44] = item.macro_scale;
    constants[45] = item.macro_opacity;
    constants[46] = macro_tex != &g_r.white ? 1.0f : 0.0f;
    // overlay.w: 1 = single-placement decal (art clamps), 2 = tileable
    // decal (art wraps; clamping a many-period uv range stretched the
    // border texels into the giant cliff-face streaks), 3 = spec masks
    // bound (exact env families). The 4-encode (t5 normal pair driving the
    // reflective trim path) is fams 5/6 only; fams 1-4 signal their
    // normal map through the misc.z flags instead.
    constants[47] =
        is_decal ? (item.decal_tileable ? 2.0f : 1.0f)
                 : (spec_bound ? ((normal_paired && item.env_family >= 5)
                                      ? 4.0f
                                      : 3.0f)
                               : 0.0f);
    if (char_spec != 0.0f) {
      // Character fam 1/2 spec-mask state (see the bind above); chars
      // never take the is_decal/spec_bound paths, so the slot is theirs.
      constants[47] = char_spec;
    }
    if (item.char_family == 1 || item.char_family == 2) {
      // misc.y = normal/spec-map LOD bias to the console's 640p-gradient
      // mip (the fam 5/6 cube-bias rationale): at 4K the shader's UV
      // gradients pick ~1.75 mips finer than the game's own render, and
      // mip-0 sampling keeps fine wrinkle noise the console filters away
      // - the authored garment folds read weaker than the emulated
      // reference without this.
      constants[49] =
          log2f(std::max(1.0f, float(context.guest_output_height) / 640.0f));
    }
    // Exact flowingwateralpha branch (cam_pos.w = -30): the canal/waterfall
    // shader hand-ported from the game's own PS and verified per-pixel
    // against the ucode. Requires the frame m_params rows (b1), the shared
    // world frame rows (exposure/sun/fog at b1, scene.shadow_valid) and the
    // ripple normal map resolved in the macro slot; the legacy water
    // shading covers every gap (and every non-flowing water material).
    const bool water_exact = item.water && item.water_flowing &&
                             debug_mode == 0 && scene.shadow_valid &&
                             scene.water_valid && macro_tex != &g_r.white;
    // Exact ocean surface (fam 31): both PCA component maps must resolve
    // (t3 = comp 0 via the water_normal macro-slot bind, t8 = comp 1); the
    // legacy water shading covers every gap. The horizon sheet (fam 32)
    // needs only its baked texture (t0) and the captured fade row.
    const bool ocean_exact = item.water && item.water_ocean == 1 &&
                             debug_mode == 0 && scene.shadow_valid &&
                             scene.ocean_valid && macro_tex != &g_r.white &&
                             ocean_n2;
    const bool oceanrefl_exact = item.water && item.water_ocean == 2 &&
                                 debug_mode == 0 && scene.shadow_valid &&
                                 scene.oceanrefl_valid;
    if (water_exact) {
      // overlay.x = spec/reflection masks bound at t4, overlay.y = real
      // environment cube at t6, overlay.z = ripple resolved (gate above).
      constants[39] = -30.0f;
      constants[44] = spec_bound ? 1.0f : 0.0f;
      constants[45] = cube_tex != &g_r.white_cube ? 1.0f : 0.0f;
      constants[46] = 1.0f;
      constants[47] = 0.0f;
      g_water_drawn.fetch_add(1, std::memory_order_relaxed);
    } else if (ocean_exact) {
      // overlay.x = second PCA component at t8 (gate above guarantees it),
      // overlay.y = real environment cube at t6, overlay.z = macro overlay
      // at t9, overlay.w = macroOverlayUVScale.
      constants[39] = -31.0f;
      constants[44] = 1.0f;
      constants[45] = cube_tex != &g_r.white_cube ? 1.0f : 0.0f;
      constants[46] = ocean_ov ? 1.0f : 0.0f;
      constants[47] = item.macro_scale;
      g_water_drawn.fetch_add(1, std::memory_order_relaxed);
    } else if (oceanrefl_exact) {
      constants[39] = -32.0f;
      constants[44] = 0.0f;
      constants[45] = 0.0f;
      constants[46] = 0.0f;
      constants[47] = 0.0f;
      g_water_drawn.fetch_add(1, std::memory_order_relaxed);
    } else if (item.water) {
      // overlay.x = ripple scroll time, overlay.y = real environment cube
      // bound at t6, overlay.z = ripple normal map resolved (in the macro
      // slot; the shader synthesizes procedural ripples otherwise).
      // overlay.w = 1 marks water with NO diffuse channel (ocean.default):
      // the body term must be zero (ocean.fx diffTerm = 0); the white
      // fallback diffuse otherwise renders the whole sea as a bright plain.
      constants[44] = water_time;
      constants[45] = cube_tex != &g_r.white_cube ? 1.0f : 0.0f;
      constants[46] = macro_tex != &g_r.white ? 1.0f : 0.0f;
      constants[47] = item.diffuse_tex == 0 ? 1.0f : 0.0f;
    } else if (item.char_family >= 6) {
      // Vehicles reuse the water convention: overlay.y > 0 = a real
      // environment cube bound at t6 (the PS vehicle branch's reflection
      // term). Vehicles never carry macro/decal channels, so the macro
      // defaults staged above are inert, but overlay.y must not inherit
      // macro_opacity's 1.0 default when no cube resolved.
      constants[45] = cube_tex != &g_r.white_cube ? 1.0f : 0.0f;
    }
    // misc.x: alpha-blended sub-pass item (1 = transparentenvironment
    // shading, 2 = water branch); fog rides in misc.yzw (ramp) and the
    // mat_tint row (color), unused by these items otherwise
    // (root-signature DWORD budget).
    constants[48] = item.water ? 2.0f : (item.transparent ? 1.0f : 0.0f);
    if (water_exact || ocean_exact || oceanrefl_exact) {
      // misc.y = cube LOD bias (same 640p-gradient rationale as fams 5/6);
      // fog comes from the shared b1 rows; misc.zw stay free. misc.x keeps
      // the water marker for the SSR G-buffer restage.
      constants[49] =
          log2f(std::max(1.0f, float(context.guest_output_height) / 640.0f));
      constants[50] = 0.0f;
      constants[51] = 0.0f;
    } else if (item.transparent || item.water) {
      constants[49] = scene.fog_ramp[0];
      constants[50] = scene.fog_ramp[1];
      constants[51] = scene.fog_ramp[2];
      std::memcpy(constants + 40, scene.fog_color, 4 * sizeof(float));
    } else if (item.env_family == 14 && constants[39] < -13.5f) {
      // Fam 14 (scrollincandescent, the LED chyron): misc.xy = the frame's
      // UV scroll offset (g_fAnimationTime x the material's channel speeds,
      // wrapped to one texture period here; the shader adds a small
      // bounded offset instead of losing uv precision to a large raw
      // time x speed product), misc.z = the material multiplier
      // m_params[0].y.
      const float ou = scene.scroll_rows[0] * item.scroll_u;
      const float ov = scene.scroll_rows[0] * item.scroll_v;
      constants[48] = ou - std::floor(ou);
      constants[49] = ov - std::floor(ov);
      constants[50] = scene.scroll_rows[1];
    } else if (v2_flags != 0) {
      // misc.z = the v2 material bind flags, misc.w = detailNormalUVScale
      // (opaque fams 1-4 and the dynobj families; see the v2 blocks above).
      constants[50] = float(v2_flags);
      constants[51] = item.detail_scale;
    } else if ((item.env_family >= 5 && item.env_family <= 6) ||
               item.env_family == 13) {
      // misc.y = cube LOD bias for the reflective families: the guest's
      // cube fetch computes its gradient LOD at the game's own 1152x640
      // render; at 4K our reflection-vector gradients are ~3.4x smaller
      // per pixel, so mips alone leave baked cube detail (the streetlight
      // heads) visible that the console blurs away.
      // misc.z/misc.w = the F12 reflection-isolation controls (see the
      // refl_mode cvar; both spare on opaque items; the fog packing only
      // uses these slots on transparent/water).
      constants[49] =
          log2f(std::max(1.0f, float(context.guest_output_height) / 640.0f));
      constants[50] = float(REXCVAR_GET(skate3_native_render_scene_refl_mode));
      constants[51] = float(REXCVAR_GET(skate3_native_render_scene_refl_lod));
      // misc.x (spare on opaque fam 5/6): both constant normal-tilt trims,
      // fixed-point packed (each mapped to 0..999 around 500; float-exact).
      // Only when the EXACT branch will run (same gate as cam_pos.w = -fam)
      // - the legacy fallback reads misc.x as the transparent/water flag.
      // Fam 13 has no normal map (its PS reflects off the vertex normal),
      // so its misc.x stays 0.
      if (debug_mode == 0 && scene.shadow_valid && item.env_family != 13) {
        double bx = REXCVAR_GET(skate3_native_render_scene_refl_bias_x);
        double by = REXCVAR_GET(skate3_native_render_scene_refl_bias_y);
        if (REXCVAR_GET(skate3_native_render_scene_refl_bias_auto) &&
            item.detail_tex != 0) {
          // Derive the fold from the material's own detail texture: its
          // first BC1 block decoded with hardware bit replication, texels
          // averaged, folded as 2*d - 1. Cached per texture object; the
          // packed-tile quirk of <=16px textures is benign here (neighbor
          // packed mips of a constant texture hold the same constant).
          // Implausible results (non-DXT1 / unreadable / |fold| > 0.1)
          // keep the cvar values.
          static std::unordered_map<uint32_t, std::pair<float, float>> fold_cache;
          auto fit = fold_cache.find(item.detail_tex);
          if (fit == fold_cache.end()) {
            std::pair<float, float> fold{float(bx), float(by)};
            uint32_t raw[6];
            if (GuestTryCopy(raw, base + item.detail_tex + 7 * 4, sizeof(raw))) {
              const uint32_t w0 = SwapU32(raw[0]);
              const uint32_t w1 = SwapU32(raw[1]);
              uint8_t block[8];
              if ((w0 & 3) == 2 && (w1 & 0x3F) == 0x12 &&
                  GuestTryCopy(block,
                               base + (0xA0000000u | ((w1 >> 12) << 12)), 8)) {
                uint8_t le[8];
                for (int k = 0; k < 8; k += 2) {  // k_8in16 guest endianness
                  le[k] = block[k + 1];
                  le[k + 1] = block[k];
                }
                uint8_t px[16][4];
                DecodeBc1Block(le, px);
                float ax = 0.0f, ay = 0.0f;
                for (int k = 0; k < 16; ++k) {
                  ax += px[k][0];
                  ay += px[k][1];
                }
                const float fx = (ax / 16.0f) * (2.0f / 255.0f) - 1.0f;
                const float fy = (ay / 16.0f) * (2.0f / 255.0f) - 1.0f;
                if (std::fabs(fx) < 0.1f && std::fabs(fy) < 0.1f) {
                  fold = {fx, fy};
                }
                REXLOG_INFO(
                    "native-scene: detail fold tex={:08X} = ({:+.6f}, {:+.6f})",
                    item.detail_tex, fx, fy);
              }
            }
            fit = fold_cache.emplace(item.detail_tex, fold).first;
          }
          bx = fit->second.first;
          by = fit->second.second;
        }
        const auto pack_trim = [](double v) {
          int i = int(std::lround(v * 1000.0)) + 500;
          return std::clamp(i, 0, 999);
        };
        constants[48] = float(pack_trim(bx) + 1000 * pack_trim(by));
      }
    }
    // cam_pos.w = -40 selects the exact sky branch: the game computes the
    // sun glow inside the dome shader (see the PS sky branch). Needs the
    // frame's captured sky sun rows AND the 1D gradient bound above; falls
    // back to the legacy fullbright dome otherwise. The sky item never uses
    // mat_tint/overlay/misc, so those root constants carry the sun rows.
    if (debug_mode == 0 && item.unlit && sky_sun_bound && scene.sky_sun_valid) {
      constants[39] = -40.0f;
      constants[40] = scene.sky_sun[0];  // mat_tint.xyz = sun direction
      constants[41] = scene.sky_sun[1];
      constants[42] = scene.sky_sun[2];
      constants[43] = scene.sky_height;  // mat_tint.w = dome viewpos Y
      constants[44] = scene.sky_sun[3];  // overlay.x = sun angular scale
      constants[45] = scene.sky_sun[4];  // overlay.y = pre-tone multiplier
      constants[46] = 0.0f;
      constants[47] = 0.0f;
      constants[49] = scene.sky_sun[5];  // misc.y = scene exposure
    }
    // SSR: record reflective items, env fams 5/6/13 on their exact branch
    // with live spec masks (the reflection mask rides t4.z), plus water,
    // for the reflection G-buffer pass after the scene pass.
    if (ssr_on && !item.skinned &&
        (item.water ||
         ((item.env_family == 5 || item.env_family == 6 ||
           item.env_family == 13) &&
          constants[39] < -4.5f && constants[47] > 2.5f))) {
      SsrGbufItem si;
      std::memcpy(si.constants, constants, sizeof(constants));
      si.t3 = macro_tex->srv;
      si.t4 = decal_tex->srv;
      si.t5 = normal_paired ? pair_normal->srv : g_r.white.srv;
      si.mesh = item.mesh;
      si.fingerprint = item.fingerprint;
      si.item = &item;
      ssr_items.push_back(si);
    }
    if (prof_items) {
      di_t3 = PerfClock::now();
    }
    cmd->SetRootConstants(0, 52, constants, 0);

    cmd->SetTexture(1, diffuse->srv);
    cmd->SetTexture(2, (lightmap != nullptr ? lightmap : &g_r.white)->srv);
    cmd->SetTexture(4, macro_tex->srv);
    // t4 override: the dynobj world-shadow map rides the free first entry
    // of the t4/t5 pair table (flag 8: dynobj draws never bind decal art
    // or spec masks there).
    nrhi::TextureView* t4_view =
        (v2_flags & 8u) != 0 ? g_r.world_shadow_srv : decal_tex->srv;
    if (normal_paired || t4_view != decal_tex->srv) {
      cmd->SetTexturePair(5, t4_view,
                          normal_paired ? pair_normal->srv : g_r.white.srv);
    } else {
      cmd->SetTexture(5, decal_tex->srv);
    }
    // t6 (cube) shares its table with t7 (shadow atlas); re-pair both.
    cmd->SetTexturePair(7, cube_tex->srv,
                        shadow_ready ? g_r.shadow_srv_final : g_r.white.srv);
    // World-shading v2 material pair (t8 detail + t9 decal spec); the exact
    // ocean rides the same pair (t8 = second PCA component, t9 = overlay).
    if (v2_flags != 0 || ocean_n2 || ocean_ov) {
      // Rebinding only t8/t9 would reset the three static-shadow maps.
      nrhi::TextureView* t8_views[5] = {
          (v2_detail != nullptr ? v2_detail : &g_r.white)->srv,
          (v2_spec2 != nullptr ? v2_spec2 : &g_r.white)->srv, nsm_view,
          nsm_mid_view, nsm_far_view};
      cmd->SetTextures(8, t8_views, 5);
    }
    // ROPA shape blend (see RendererState::ropa_shapes): combine the shape
    // generations with the kernel weights InterpolateDynamicItems computed
    // (the SAME 8-tap boxcar / pair-lerp the body bones and garment world
    // took this frame) into the per-frame ropa upload ring and draw from
    // it; the stepped cloth shape against the interpolated body was the
    // tee jelly/clip-through, and a filter-MISMATCHED blend (plain lerp vs
    // boxcar body) kept a period-scaled residue of it. Positions/normals
    // (floats 0..6) blend; packed attributes (blend words, uvs: floats
    // 7..13) copy from the newest generation present. Generations missing
    // from the ring (evicted / decode in flight) renormalize over what IS
    // present when at least half the kernel's weight survives.
    VbBinding item_vbv = buffers.vb_view;
    if (item.ropa && item.shape_count > 0 &&
        REXCVAR_GET(skate3_native_render_scene_ropa_blend)) {
      const std::vector<float>* gv[DrawItem::kShapeGens] = {};
      float gw[DrawItem::kShapeGens] = {};
      int ng = 0;
      float total = 0.0f;
      uint64_t newest_seq = 0;
      const std::vector<float>* newest = nullptr;
      // Generations must match the RESIDENT decode's extent exactly; the
      // index buffer references that many vertices. After a re-stream/
      // outfit swap the ring briefly holds stale-size generations; binding
      // one against the current IB reads past the bound VB and collapses
      // triangles (a momentary partial-invisible blink). Stale sizes drop
      // out here; if too much kernel weight is stale, the raw resident VB
      // draws instead (always self-consistent).
      const size_t want_floats =
          size_t(buffers.vb_view.size_bytes) / sizeof(float);
      const auto rit = g_r.ropa_shapes.find(item.mesh);
      if (rit != g_r.ropa_shapes.end() && !rit->second.empty()) {
        // Sim-sleep gate: generations grossly older than the ring's newest
        // were recorded before a cloth-sim sleep (continuous streams span
        // ~114 ms end to end). Blending one against the fresh drape draws
        // the garment where/how the character USED to stand, the
        // detached/perpendicular class. Dropped generations shed their
        // kernel weight; if too much weight is stale the raw resident VB
        // draws instead (self-consistent current shape).
        const double ring_newest_t = rit->second.back().t;
        for (int k = 0; k < item.shape_count; ++k) {
          for (const RendererState::RopaGen& g : rit->second) {
            if (g.seq != item.shape_seq[k]) {
              continue;
            }
            if (g.verts.size() != want_floats) {
              break;  // stale-size generation (re-stream/outfit swap)
            }
            if (ring_newest_t - g.t > 0.35) {
              static std::atomic<uint32_t> s_stale{0};
              const uint32_t ln =
                  s_stale.fetch_add(1, std::memory_order_relaxed);
              if (ln < 16 || (ln & 1023u) == 0) {
                REXLOG_DEBUG(
                    "native-scene: ropa shape STALE gen dropped mesh={:08X} "
                    "age={:.2f}s seq_gap={} (n={})",
                    item.mesh, ring_newest_t - g.t,
                    rit->second.back().seq - g.seq, ln);
              }
              break;
            }
            gv[ng] = &g.verts;
            gw[ng] = item.shape_w[k];
            total += item.shape_w[k];
            ++ng;
            if (g.seq >= newest_seq) {
              newest_seq = g.seq;
              newest = &g.verts;
            }
            break;
          }
        }
      }
      const uint32_t region =
          upload_region_index *
          RendererState::kRopaRegionSize;
      const uint32_t bytes = uint32_t(want_floats * sizeof(float));
      if (ng > 0 && total >= 0.5f && newest != nullptr &&
          buffers.vb_view.stride == 56 &&
          g_r.ropa_ring_offset + bytes <= RendererState::kRopaRegionSize) {
        float* dst = reinterpret_cast<float*>(g_r.ropa_ring_cpu + region +
                                              g_r.ropa_ring_offset);
        const float inv = 1.0f / total;
        for (size_t v = 0; v + 14 <= want_floats; v += 14) {
          float blend7[7] = {};
          for (int k = 0; k < ng; ++k) {
            const float* src = gv[k]->data() + v;
            const float wk = gw[k] * inv;
            for (int f = 0; f < 7; ++f) {
              blend7[f] += src[f] * wk;
            }
          }
          // One store per float: dst is write-combined upload memory.
          std::memcpy(dst + v, blend7, sizeof(blend7));
          std::memcpy(dst + v + 7, newest->data() + v + 7, 7 * sizeof(float));
        }
        item_vbv.buffer = g_r.ropa_ring;
        item_vbv.offset = region + g_r.ropa_ring_offset;
        item_vbv.size_bytes = bytes;
        item_vbv.stride = buffers.vb_view.stride;
        g_r.ropa_ring_offset += (bytes + 255u) & ~255u;
        g_ropa_blend_drawn.fetch_add(1, std::memory_order_relaxed);
      } else {
        // Fallback to the raw resident shape. Rate-limited detail log: an
        // ALTERNATION of these with blended frames renders the garment
        // hopping between the play-clock drape and the zero-lag drape.
        g_ropa_blend_miss.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint32_t> s_blend_miss_log{0};
        const uint32_t ln =
            s_blend_miss_log.fetch_add(1, std::memory_order_relaxed);
        if (ln < 64 || (ln & 63u) == 0) {
          REXLOG_DEBUG(
              "native-scene: ropa blend MISS mesh={:08X} sc={} ng={} "
              "total={:.2f} stride={} ring={} ring_off={} (n={})",
              item.mesh, item.shape_count, ng, total, buffers.vb_view.stride,
              rit != g_r.ropa_shapes.end() ? rit->second.size() : size_t(0),
              g_r.ropa_ring_offset, ln);
        }
      }
    }
    cmd->SetVertexBuffer(item_vbv.buffer, item_vbv.offset, item_vbv.size_bytes,
                         item_vbv.stride);
    cmd->SetIndexBuffer(buffers.ib_view.buffer, buffers.ib_view.offset,
                        buffers.ib_view.size_bytes);
    for (const DrawEntry& draw : item.draws) {
      if (draw.prim == 4) {
        cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
      } else if (draw.prim == 6) {
        cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
      } else {
        continue;
      }
      cmd->DrawIndexed(draw.index_count, draw.start_index, draw.base_vertex);
      ++drawn;
      if (ring_frame != nullptr) {
        const auto rit = ring_map.find(&item);
        if (rit != ring_map.end()) {
          ++ring_frame->items[rit->second].drawn;
        }
      }
    }
    // Stage attribution for draws that reached submission (early returns
    // above skip this; their time still lands in the caller's in/out
    // windows).
    if (prof_items && di_t3 != PerfClock::time_point{}) {
      const auto di_t4 = PerfClock::now();
      const auto ns = [](PerfClock::time_point a, PerfClock::time_point b) {
        return uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
      };
      g_pw_di_mesh.Add(ns(di_t0, di_t1));
      g_pw_di_tex.Add(ns(di_t1, di_t2));
      g_pw_di_const.Add(ns(di_t2, di_t3));
      g_pw_di_submit.Add(ns(di_t3, di_t4));
    }
  };
  // Profiling-mode consumption of the occlusion-grid readback ring (filled
  // by the perf-items reduce in ApplySsaoPass on previous frames): newest
  // completed slot wins, every completed slot retires. A grid that stops
  // refreshing (AO off, loading flows) goes invalid instead of classifying
  // against ancient depth.
  const bool occl_cull_wanted =
      REXCVAR_GET(skate3_native_render_scene_occlusion_cull);
  if ((prof_items || occl_cull_wanted) && !g_r.occl_failed &&
      g_r.occl_readback_ptr[1] != nullptr) {
    const uint64_t occl_done = context.device->CompletedSubmission();
    int newest = -1;
    for (int i = 0; i < 2; ++i) {
      if (g_r.occl_pending[i] && g_r.occl_submission[i] < occl_done &&
          (newest < 0 ||
           g_r.occl_submission[i] > g_r.occl_submission[newest])) {
        newest = i;
      }
    }
    if (newest >= 0) {
      context.device->InvalidateForRead(
          g_r.occl_readback[newest], 0,
          uint64_t(RendererState::kOcclRowPitch) * RendererState::kOcclGridH);
      g_r.occl_grid.resize(size_t(RendererState::kOcclGridW) *
                           RendererState::kOcclGridH);
      for (uint32_t row = 0; row < RendererState::kOcclGridH; ++row) {
        std::memcpy(&g_r.occl_grid[size_t(row) * RendererState::kOcclGridW],
                    g_r.occl_readback_ptr[newest] +
                        size_t(row) * RendererState::kOcclRowPitch,
                    RendererState::kOcclGridW * sizeof(float));
      }
      std::memcpy(g_r.occl_grid_vp, g_r.occl_vp[newest],
                  sizeof(g_r.occl_grid_vp));
      std::memcpy(g_r.occl_grid_cam, g_r.occl_cam[newest],
                  sizeof(g_r.occl_grid_cam));
      g_r.occl_grid_frame = frame_number;
      g_r.occl_grid_valid = true;
      for (int i = 0; i < 2; ++i) {
        if (g_r.occl_pending[i] && g_r.occl_submission[i] < occl_done) {
          g_r.occl_pending[i] = false;
        }
      }
    } else if (g_r.occl_grid_valid &&
               frame_number - g_r.occl_grid_frame > 240) {
      g_r.occl_grid_valid = false;
    }
  }
  // Per-draw wrapper for the profiling mode: attribute each item's total
  // draw_item cost to its visibility class. Out-of-frustum items and
  // occlusion-proven-hidden items cost CPU here but can never contribute
  // pixels; a large share in either class is a visibility-culling gap, not
  // a scene-density cost.
  const auto record_item_draws = [&](const DrawItem& item,
                                     uint32_t before_draws) {
    native_entity::CtxInfo identity;
    if (item.ctx != 0 &&
        native_entity::LookupCtx(item.ctx, &identity)) {
      skate3::mechanics_sandbox::RecordRenderedPresentation(
          identity.entity, drawn - before_draws);
    }
  };
  const auto timed_draw = [&](const DrawItem& item) {
    if (!prof_items) {
      const uint32_t before_draws = drawn;
      draw_item(item);
      record_item_draws(item, before_draws);
      return;
    }
    // Statics only (same gate as the retention pass): a skinned/ropa item's
    // pose lives in its bone palette, its identity world + bind-pose bbox
    // say nothing about where it is; those always classify as visible.
    const bool classifiable = !item.skinned && !item.ropa &&
                              !item.cloth_quads && item.bones.empty();
    const bool off =
        classifiable && ItemOutsideFrustum(item, scene.view_proj, 1.0f);
    const bool occluded =
        classifiable && !off && ItemOccludedByGrid(item, 0.25f);
    uint64_t indices = 0;
    for (const DrawEntry& e : item.draws) {
      indices += e.index_count;
    }
    const auto t0 = PerfClock::now();
    const uint32_t before_draws = drawn;
    draw_item(item);
    record_item_draws(item, before_draws);
    const uint64_t total_ns = perf_ns_since(t0);
    if (off) {
      g_pw_di_out.Add(total_ns);
      g_vis_out_indices.fetch_add(indices, std::memory_order_relaxed);
      if (item.retained) {
        g_vis_out_retained.fetch_add(1, std::memory_order_relaxed);
      }
    } else if (occluded) {
      g_pw_di_occ.Add(total_ns);
      g_vis_occ_indices.fetch_add(indices, std::memory_order_relaxed);
    } else {
      g_pw_di_in.Add(total_ns);
      g_vis_in_indices.fetch_add(indices, std::memory_order_relaxed);
    }
  };

  // Opaque items first; environment.transparent items are deferred to an
  // alpha-blended sub-pass (depth test on, z-write off) drawn back-to-front;
  // interleaved opaque rendering alpha-tested the mist sheets into solid
  // white cloud blobs.
  const auto items_t0 = PerfClock::now();
  const auto view_dist2 = [&](const DrawItem& item) {
    float c[3], w[3];
    for (int a = 0; a < 3; ++a) {
      c[a] = (item.bbox_min[a] + item.bbox_max[a]) * 0.5f;
    }
    for (int a = 0; a < 3; ++a) {
      w[a] = c[0] * item.world[0 * 4 + a] + c[1] * item.world[1 * 4 + a] +
             c[2] * item.world[2 * 4 + a] + item.world[3 * 4 + a];
    }
    float d2 = 0.0f;
    for (int a = 0; a < 3; ++a) {
      const float d = w[a] - scene.cam_pos[a];
      d2 += d * d;
    }
    return d2;
  };
  std::vector<const DrawItem*> transparent_items;
  // Mid-fade entity items (see CharFadeAlpha / pso_fade): blended but with
  // z-write ON, drawn at the HEAD of the blended sub-pass; they behave like
  // main-pass objects whose glass/hair still composites over them, and depth
  // writes stop their own overlapping pieces (skin under clothes, far-side
  // doors/wheels through the body shell) from double-blending into an x-ray.
  const auto char_fade_zwrite = [&](const DrawItem& it) {
    return debug_mode == 0 && it.char_rows[14 * 4 + 1] > 0.0f &&
           (it.char_family == 1 || it.char_family == 2 ||
            it.char_family == 3 || it.char_family == 6) &&
           REXCVAR_GET(skate3_native_render_scene_entity_fade) &&
           CharFadeAlpha(it) < 0.999f;
  };
  // Occlusion cull: statics provably hidden behind already-rendered
  // geometry skip the color pass entirely (every scene.items consumer that
  // must still see them - shadow casters, the world-shadow map, outline,
  // warm-settle - iterates the list itself and is unaffected). Engaged only
  // while the grid's camera matches this frame's within 1 m: the stored-
  // frustum check inside ItemOccludedByGrid already keeps rotation safe
  // (bounds outside the grid's view classify visible), and past 1 m of
  // translation the 1-2 frame-old depth could hide content parallax has
  // revealed. At gameplay speeds the grid refreshes well inside that
  // budget, so the gate only stands the cull down across teleports and
  // camera cuts.
  bool occl_cull_active = occl_cull_wanted && debug_mode == 0 &&
                          g_r.occl_grid_valid;
  if (occl_cull_active) {
    float cam_d2 = 0.0f;
    for (int a = 0; a < 3; ++a) {
      const float d = scene.cam_pos[a] - g_r.occl_grid_cam[a];
      cam_d2 += d * d;
    }
    occl_cull_active = cam_d2 < 1.0f;
  }
  std::vector<uint32_t> culled_ctxs;
  if (occl_cull_active) {
    culled_ctxs.reserve(scene.items.size() / 2);
  }
  // Opaque items draw front-to-back (bbox-center distance): early-z rejects
  // occluded pixels before the heavy material PS runs. The game's sort-list
  // order is by render state, not depth; depth-write LESS_EQUAL makes the
  // final image order-independent, so reordering is safe.
  std::vector<std::pair<float, const DrawItem*>> opaque_items;
  opaque_items.reserve(scene.items.size());
  for (const DrawItem& item : scene.items) {
    const uint32_t index = item_index++;
    if (debug_mode == 1) {
      break;
    }
    if (debug_mode == 3 && index >= 20) {
      break;
    }
    if (!SandboxShouldDrawItem(item)) {
      continue;
    }
    if (ring_frame != nullptr) {
      SceneRingItem ri{};
      ri.ctx = item.ctx;
      ri.mesh = item.mesh;
      ri.diffuse = item.diffuse_tex;
      ri.lightmap = item.lightmap_tex;
      ri.vb_obj = item.vb_obj;
      ri.spec = item.spec_tex;
      ri.macro = item.macro_tex;
      ri.decal_art = item.decal_art;
      ri.wnormal = item.water_normal;
      uint32_t idx_total = 0;
      for (const DrawEntry& e : item.draws) {
        idx_total += e.index_count;
      }
      ri.indices = idx_total;
      ri.env_family = item.env_family;
      ri.char_family = item.char_family;
      ri.flags = uint8_t((item.transparent ? 1u : 0u) | (item.water ? 2u : 0u) |
                         (item.skinned ? 4u : 0u) | (item.retained ? 8u : 0u) |
                         (item.pending ? 16u : 0u) |
                         (item.caster_bank ? 32u : 0u) |
                         (item.decal ? 64u : 0u));
      ring_map.emplace(&item, uint32_t(ring_frame->items.size()));
      ring_frame->items.push_back(ri);
    }
    // Hair with a validated lighting capture joins the sorted alpha
    // sub-pass (strand coverage blend, depth test on / z-write off, the
    // game's own hair render state); without the capture it stays on the
    // legacy opaque path. Vehicle glass (fam 7) blends there too,
    // reflection-only windows at the captured alpha; vehicle bodies (fam 6)
    // stay opaque.
    const bool char_capture_ok = item.char_rows[14 * 4 + 1] > 0.0f;
    const bool hair_blend = item.char_family >= 4 && item.char_family <= 5 &&
                            char_capture_ok;
    const bool glass_blend = item.char_family == 7 && char_capture_ok;
    // character.alpha accessory pieces (sunglass lenses): translucent, alpha
    // from the coverage texture at uv2 (ch_misc.z set by the capture); the
    // game draws them after every opaque character piece (see
    // DrawItem::char_alpha). Gated on the SAME capture validation as hair:
    // without validated rows ch_misc.z is 0 and the item stays opaque.
    const bool cac_alpha_blend = item.char_alpha && char_capture_ok &&
                                 item.char_rows[14 * 4 + 2] > 0.0f;
    // Flicker probes: a hair/character-alpha piece whose pass route flips
    // between consecutive frames, or whose bone palette returns bit-exact
    // to the previous-but-one frame's value (A,B,A,B: genuine motion
    // never round-trips in two rendered frames), is the visible hair
    // flicker.
    if (item.char_family >= 4 || item.char_alpha) {
      struct RouteProbe {
        uint8_t route = 0;
        uint64_t bones_h1 = 0;  // last frame's palette hash
        uint64_t bones_h2 = 0;  // the frame before
      };
      static std::unordered_map<uint64_t, RouteProbe> s_probe;  // render thread
      const uint64_t rk = (uint64_t(item.mesh) << 32) | item.ctx;
      const uint8_t route = uint8_t((hair_blend || cac_alpha_blend) ? 2 : 1);
      uint64_t bones_h = 1469598103934665603ull;
      if (!item.bones.empty()) {
        const uint8_t* bb = reinterpret_cast<const uint8_t*>(item.bones.data());
        const size_t bn = item.bones.size() * sizeof(float);
        for (size_t bi = 0; bi < bn; ++bi) {
          bones_h = (bones_h ^ bb[bi]) * 1099511628211ull;
        }
      }
      auto [rit2, fresh2] = s_probe.try_emplace(rk);
      RouteProbe& pr = rit2->second;
      if (!fresh2 && pr.route != route) {
        g_hair_route_flips.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint32_t> s_flip_logged{0};
        const uint32_t fl = s_flip_logged.fetch_add(1, std::memory_order_relaxed);
        if (fl < 24 || (fl & 511u) == 0) {
          REXLOG_DEBUG(
              "native-scene: hair ROUTE FLIP mesh={:08X} ctx={:08X} fam={} "
              "alpha={} -> route {} rows14=({:.3f},{:.3f},{:.3f}) (n={})",
              item.mesh, item.ctx, item.char_family, item.char_alpha ? 1 : 0,
              route, item.char_rows[14 * 4 + 0], item.char_rows[14 * 4 + 1],
              item.char_rows[14 * 4 + 2], fl);
        }
      }
      if (!fresh2 && !item.bones.empty() && bones_h != pr.bones_h1 &&
          bones_h == pr.bones_h2) {
        g_hair_bone_alternations.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint32_t> s_bone_alt_logged{0};
        const uint32_t bl = s_bone_alt_logged.fetch_add(1, std::memory_order_relaxed);
        if (bl < 24 || (bl & 1023u) == 0) {
          REXLOG_DEBUG(
              "native-scene: hair BONES ALTERNATION mesh={:08X} ctx={:08X} "
              "fam={} bones={} src={} caster_bank={} (n={})",
              item.mesh, item.ctx, item.char_family, item.bones.size() / 12,
              item.dbg_src, item.caster_bank ? 1 : 0, bl);
        }
      }
      pr.route = route;
      pr.bones_h2 = pr.bones_h1;
      pr.bones_h1 = bones_h;
      if (s_probe.size() > 4096) {
        s_probe.clear();
      }
    }
    // Per-entity spawn/distance fade (CharFadeAlpha): the game submits
    // LivingWorld entity draws at alpha 0 through the whole spawn settle
    // (NPCs drop ~1 m to the ground before fading in) and ramps alpha with
    // distance; skip invisible items entirely, blend mid-fade ones.
    // LW-mapped items (lw_alpha >= 0) do not need a validated lighting
    // capture to honor the fade: the entity alpha is authoritative even
    // when the capture chain failed; a spawn-settling NPC is invisible
    // regardless of whether its rows validated this frame.
    const bool entity_fade = debug_mode == 0 && item.char_family != 0 &&
                             (char_capture_ok || item.lw_alpha >= 0.0f) &&
                             REXCVAR_GET(skate3_native_render_scene_entity_fade);
    const float fade_a = entity_fade ? CharFadeAlpha(item) : 1.0f;
    // One-frame fade-blink guard: a mesh that rendered ~opaque last frame
    // cannot legitimately sit at alpha 0 this frame; the game's spawn/
    // distance fades ramp over ~0.5 s. An opaque->0 step means a garbage/
    // foreign constant row served the alpha this capture (the clone-shared
    // char-rows suspect; observed as part of the tee flickering invisible
    // for a moment). Repair: draw the item OPAQUE this frame (skip the fade
    // skip + the mid-fade blend routing) and log it. The last-alpha map
    // updates from the RAW value, so a persisting alpha 0 (real despawn /
    // spawn settle) only gets one repaired frame and then skips normally.
    // LW-mapped items bypass the blink repair entirely (read AND write):
    // the entity alpha cannot blink; the repair exists for garbage/foreign
    // CAPTURED rows, and its mesh key is clone-shared, which force-drew
    // spawn-settling clones OPAQUE every frame their twin was visible (the
    // "NPC drops out of the sky with no fade" sighting).
    bool fade_blink = false;
    if (item.char_family != 0 && debug_mode == 0 && item.lw_alpha < 0.0f) {
      static std::unordered_map<uint32_t, uint8_t> s_fade_opaque;  // render thread
      uint8_t& was_opaque = s_fade_opaque[item.mesh];
      if (entity_fade && fade_a <= 0.004f && was_opaque) {
        fade_blink = true;
        static std::atomic<uint64_t> s_blinks{0};
        const uint64_t n = s_blinks.fetch_add(1, std::memory_order_relaxed);
        if (n < 16 || (n & 255u) == 0) {
          REXLOG_DEBUG(
              "native-scene: fade BLINK repaired mesh={:08X} fam={} ropa={} "
              "src={} rows13=({:.3f},{:.3f},{:.3f},{:.3f}) "
              "rows14=({:.3f},{:.3f}) (n={})",
              item.mesh, item.char_family, item.ropa ? 1 : 0, item.dbg_src,
              item.char_rows[13 * 4 + 0], item.char_rows[13 * 4 + 1],
              item.char_rows[13 * 4 + 2], item.char_rows[13 * 4 + 3],
              item.char_rows[14 * 4 + 0], item.char_rows[14 * 4 + 1], n);
        }
      }
      was_opaque = (!entity_fade || fade_a > 0.9f) ? 1 : 0;
      if (s_fade_opaque.size() > 1024) {
        s_fade_opaque.clear();
      }
    }
    if (entity_fade && fade_a <= 0.004f && !fade_blink) {
      if (item.lw_alpha >= 0.0f) {
        g_lw_fade0.fetch_add(1, std::memory_order_relaxed);
      }
      continue;
    }
    const bool char_fade_blend = char_fade_zwrite(item) && !fade_blink;
    // environment.reflective_trans (fam 13): blended glass canopies, joins
    // the sorted alpha sub-pass whenever its exact branch is live (same
    // shadow_valid gate as cam_pos.w = -fam; the legacy fallback renders it
    // opaque exactly as before classification).
    const bool refl_trans_blend =
        item.env_family == 13 && scene.shadow_valid;
    const auto stamp_route = [&](uint8_t route) {
      if (ring_frame != nullptr) {
        const auto rit = ring_map.find(&item);
        if (rit != ring_map.end()) {
          ring_frame->items[rit->second].route = route;
        }
      }
    };
    // Occlusion cull (statics only; same classifiability gate as the
    // profiler): the 1 m depth margin on top of the conservative bbox test
    // keeps reveal edges safe at speed - deeply hidden mass sits many
    // meters behind its occluders, so the margin costs almost none of the
    // win. Ring route 3 = culled. Live-submitted culled ctxs (never
    // retained ones: a re-appended item's ctx may already be freed, and a
    // reused address would filter an unrelated new instance) feed the
    // guest-side dispatch filter via the publication below.
    if (occl_cull_active && !item.skinned && !item.ropa &&
        !item.cloth_quads && item.bones.empty() &&
        ItemOccludedByGrid(item, 1.0f)) {
      g_occl_culled.fetch_add(1, std::memory_order_relaxed);
      if (!item.retained && item.ctx != 0) {
        culled_ctxs.push_back(item.ctx);
      }
      stamp_route(3);
      continue;
    }
    // The exact ocean surface (fam 31) is an OPAQUE draw in the game (its
    // PS outputs alpha 0 and it writes z-prepass depth); routing it through
    // the sorted alpha sub-pass would composite the blended horizon sheet
    // (ocean.reflection) UNDER it. The legacy fallback also renders opaque,
    // so the route holds while textures/rows are still resolving.
    const bool ocean_opaque = item.water && item.water_ocean == 1 &&
                              scene.shadow_valid && scene.ocean_valid;
    if ((item.transparent || item.water || hair_blend || glass_blend ||
         cac_alpha_blend || refl_trans_blend || char_fade_blend) &&
        !ocean_opaque && debug_mode == 0) {
      if (REXCVAR_GET(skate3_native_render_scene_transparents)) {
        transparent_items.push_back(&item);
        stamp_route(2);
      }
      continue;
    }
    opaque_items.emplace_back(view_dist2(item), &item);
    stamp_route(1);
  }
  // Remote character clones use the same material routing as their local
  // source pieces. They intentionally bypass entity-fade and occlusion
  // bookkeeping: they are presentation replicas, not guest entities.
  for (const DrawItem& item : multiplayer_remote_items) {
    if (!SandboxShouldDrawItem(item)) {
      continue;
    }
    const bool char_capture_ok =
        item.char_rows[14 * 4 + 1] > 0.0f;
    const bool hair_blend =
        item.char_family >= 4 && item.char_family <= 5 &&
        char_capture_ok;
    const bool glass_blend =
        item.char_family == 7 && char_capture_ok;
    const bool cac_alpha_blend =
        item.char_alpha && char_capture_ok &&
        item.char_rows[14 * 4 + 2] > 0.0f;
    const bool refl_trans_blend =
        item.env_family == 13 && scene.shadow_valid;
    const bool ocean_opaque =
        item.water && item.water_ocean == 1 &&
        scene.shadow_valid && scene.ocean_valid;
    if ((item.transparent || item.water || hair_blend ||
         glass_blend || cac_alpha_blend || refl_trans_blend) &&
        !ocean_opaque && debug_mode == 0) {
      if (REXCVAR_GET(skate3_native_render_scene_transparents)) {
        transparent_items.push_back(&item);
      }
    } else {
      opaque_items.emplace_back(view_dist2(item), &item);
    }
  }
  // Publish the culled-ctx set every rendered frame, including empty ones:
  // the guest filter must clear promptly when the cull stands down. Ctxs
  // the build-side skip left out of this scene stay culled by re-inclusion
  // (they could not be re-tested; their staggered rebuild frame is when
  // they re-enter scene.items and get a fresh verdict) - but only while
  // the cull is ACTIVE: on stand-down (debug views, the camera-motion
  // gate, teleports) the set must drain so every skipped item resumes
  // building on the next guest frame.
  {
    if (occl_cull_active) {
      culled_ctxs.insert(culled_ctxs.end(), scene.occl_build_skipped.begin(),
                         scene.occl_build_skipped.end());
    }
    const int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    std::sort(culled_ctxs.begin(), culled_ctxs.end());
    std::lock_guard<std::mutex> lock(g_occl_pub_mutex);
    g_occl_pub_ctxs.swap(culled_ctxs);
    g_occl_pub_ms.store(now_ms, std::memory_order_relaxed);
  }
  if (REXCVAR_GET(skate3_native_render_scene_sort_opaque) && debug_mode == 0) {
    std::stable_sort(opaque_items.begin(), opaque_items.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
  }
  for (const auto& [dist, item] : opaque_items) {
    timed_draw(*item);
  }
  if (!transparent_items.empty() && g_r.pso_transparent != nullptr) {
    std::stable_sort(transparent_items.begin(), transparent_items.end(),
                     [&](const DrawItem* a, const DrawItem* b) {
                       // Fading entities (z-write blend) draw before every
                       // z-write-off blend so hair/glass composite over them.
                       const bool fa = char_fade_zwrite(*a);
                       const bool fb = char_fade_zwrite(*b);
                       if (fa != fb) {
                         return fa;
                       }
                       return view_dist2(*a) > view_dist2(*b);
                     });
    cmd->SetPipeline(use_depth ? g_r.pso_transparent : g_r.pso_nodepth);
    nrhi::Pipeline* blend_bound = use_depth ? g_r.pso_transparent : g_r.pso_nodepth;
    for (const DrawItem* item : transparent_items) {
      // Mid-fade entity pieces: alpha blend with z-write ON (see pso_fade).
      if (use_depth && g_r.pso_fade != nullptr && char_fade_zwrite(*item)) {
        if (blend_bound != g_r.pso_fade) {
          cmd->SetPipeline(g_r.pso_fade);
          blend_bound = g_r.pso_fade;
        }
        timed_draw(*item);
        continue;
      }
      const bool hair = item->char_family >= 4 && item->char_family <= 5 &&
                        item->char_rows[14 * 4 + 1] > 0.0f;
      if (hair && use_depth && g_r.pso_hair_a != nullptr && g_r.pso_hair_b != nullptr) {
        // The game's two hair passes: cull BACK then cull FRONT with the
        // same shader: keeps far-side strands from compositing over
        // near-side ones (one uncull(ed) pass reads as crunchy noise).
        cmd->SetPipeline(g_r.pso_hair_a);
        timed_draw(*item);
        cmd->SetPipeline(g_r.pso_hair_b);
        timed_draw(*item);
        cmd->SetPipeline(blend_bound);
        continue;
      }
      // reflective_trans glass culls like the game (its material family's
      // XML: CULLMODE=FRONT): the canopy panels are double-glazed pane
      // PAIRS a few cm apart; uncull(ed) we composited BOTH panes (an
      // extra a^2 blend layer the emulated frame doesn't have).
      // pso_hair_b IS the transparent state with CULL_FRONT.
      if (item->env_family == 13 && use_depth && g_r.pso_hair_b != nullptr &&
          REXCVAR_GET(skate3_native_render_scene_backface_cull)) {
        const float* w = item->world;
        const float det3 = w[0] * (w[5] * w[10] - w[6] * w[9]) -
                           w[1] * (w[4] * w[10] - w[6] * w[8]) +
                           w[2] * (w[4] * w[9] - w[5] * w[8]);
        nrhi::Pipeline* want =
            det3 >= 0.0f ? g_r.pso_hair_b
                         : (use_depth ? g_r.pso_transparent : g_r.pso_nodepth);
        if (want != blend_bound) {
          cmd->SetPipeline(want);
          blend_bound = want;
        }
        timed_draw(*item);
        continue;
      }
      if (blend_bound != (use_depth ? g_r.pso_transparent : g_r.pso_nodepth)) {
        blend_bound = use_depth ? g_r.pso_transparent : g_r.pso_nodepth;
        cmd->SetPipeline(blend_bound);
      }
      timed_draw(*item);
    }
  }
  skate3::mechanics_sandbox::RecordRenderedItems(drawn);
  skate3::mechanics_sandbox::RecordRenderStage(
      skate3::mechanics_sandbox::RenderStage::MainPassComplete);
  g_pw_items.Add(perf_ns_since(items_t0));
  g_pw_pre.Add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   items_t0 - render_t0)
                   .count()));
  const auto mid_t0 = PerfClock::now();

  // Guest-texture resolver shared by the spline pass (pre-resolve, in the
  // scene pass) and the HUD pass (post-resolve); both allocate strip/quad
  // vertices from the same per-frame ui_ring region.
  // Per-frame inline budget for HOT content re-decodes (actively-animating
  // elements + video planes): inline keeps their content latency at ONE
  // frame; overflow degrades to the async heal for the rest of the frame.
  int64_t hot_inline_budget_ns = 4'000'000;
  // force_inline: the dormant FMV bracket-fallback resolve stays on the
  // inline decode (fires at most once per plane set).
  // video: the FMV triple resolves; content changes always decode inline
  // (frame-exact playback), and the caller additionally gates serving on
  // last_change_frame >= the movie session start (see the triple loop).
  const auto resolve_2d_texture = [&](const uint32_t fetch[6],
                                      bool force_inline = false,
                                      bool video = false) -> const GuestTexture* {
    if ((fetch[0] & 0x3u) != 2 || fetch[1] == 0) {
      return &g_r.white;
    }
    const uint64_t key = FetchWordsKey(fetch);
    // 2D-resolver trace (trace_2d cvar, matched on fetch word 1): logs the
    // per-resolve entry state + route below, and registers the words key so
    // the worker-commit trace covers its heals too.
    const bool tr2d = g_trace_2d_w1 != 0 && fetch[1] == g_trace_2d_w1;
    if (tr2d) {
      g_trace_keys.insert(key);
    }
    // Routing (see the 2d_async_px cvar): INLINE-FIRST under the per-frame
    // budget; the run-copy untiler + in-place hot updates made decodes
    // cheap (1280x720 sub-ms; no CreateCommittedResource churn on content
    // changes), so first sightings AND content changes decode inline for
    // 1-frame content latency. The async workers are the burst-overflow
    // valve only: they exist because inline decodes stall the frame AND the
    // guest (swap blocks), which mattered when a screen change brought
    // 100+ ms of per-pixel decode work, but routinely
    // skipping first-sight quads made every APT re-raster (new address =
    // new key) blink its element for 1-3 frames (the menu flicker).
    const int32_t async_px = REXCVAR_GET(skate3_native_render_scene_2d_async_px);
    const uint32_t px_w = (fetch[2] & 0x1FFFu) + 1;
    const uint32_t px_h = ((fetch[2] >> 13) & 0x1FFFu) + 1;
    const bool async_ui = !force_inline && async_px > 0 &&
                          uint64_t(px_w) * px_h > uint64_t(async_px);
    // PLAIN store lookup on purpose: find_words_texture is the 3D poster
    // revalidator; it probes, RE-ARMS recheck_frame and enqueues its own
    // ui=false heal, which made this resolver's liveness block dead code
    // (recheck always freshly re-armed before it ran; astale=0 across whole
    // sessions) and put every 2D content update on that 4-5
    // frame async round trip.
    auto it = g_r.tex_store.find(key);
    bool inline_redecode = false;
    if (it != g_r.tex_store.end()) {
      it->second.last_used_frame = frame_number;
    }
    if (it != g_r.tex_store.end() && it->second.valid &&
        frame_number >= it->second.recheck_frame &&
        REXCVAR_GET(skate3_native_render_scene_tex_revalidate)) {
      // Content liveness for CPU-rewritten UI art: video frames and APT
      // re-rasterized tiles rewrite the same payload with the fetch words
      // unchanged; without the probe the words-keyed cache would freeze
      // them on their first decoded content. Incomplete decodes (truncated
      // tiled-mip copy) re-decode here too, like the 3D route revalidator;
      // the 2D-only art (menu thumbnails) has no other heal path.
      const uint64_t fp = SampleProbeFingerprint(base, it->second);
      if ((fp != 0 && fp != it->second.payload_fp) || it->second.incomplete) {
        // Rolling-capped heal telemetry for menu-class art (the map-switch
        // thumbnail wrong-image reports): names the key, the route taken
        // and the fingerprint transition.
        static std::atomic<uint32_t> s_2d_heal_logs{0};
        const bool log_heal =
            uint64_t(px_w) * px_h >= 32768 &&
            s_2d_heal_logs.fetch_add(1, std::memory_order_relaxed) < 64;
        // Content changes decode INLINE while the per-frame budget lasts
        // (1-frame latency for animating UI); fullscreen-class art gets an
        // extended budget; serving it stale/blank is a whole-screen
        // artifact, and the run-copy untiler made even 1280x720 cheap.
        const bool fullscreen_class = uint64_t(px_w) * px_h >= 460'000;
        inline_redecode =
            video || !async_ui || hot_inline_budget_ns > 0 ||
            (fullscreen_class && hot_inline_budget_ns > -4'000'000);
        if (log_heal) {
          REXLOG_INFO(
              "native-scene: 2D content heal {}x{} key={:016X} fp {:016X}->"
              "{:016X} inc={} route={}",
              px_w, px_h, key, it->second.payload_fp, fp,
              it->second.incomplete ? 1 : 0,
              inline_redecode ? "inline" : "async");
        }
        if (inline_redecode) {
          // Same layout = update the committed texture in place (no
          // CreateCommittedResource churn, SRV slot kept, tear-guarded).
          const auto up_t0 = PerfClock::now();
          if (UpdateGuestTexture2DInPlace(context, base, it->second)) {
            const uint64_t up_ns = perf_ns_since(up_t0);
            hot_inline_budget_ns -= int64_t(up_ns);
            g_pw_tex_decode.Add(up_ns);
            it->second.last_change_frame = frame_number;
            return &it->second;
          }
          if (tr2d) {
            REXLOG_INFO("2d-trace: f{} key={:016X} inplace update REFUSED - "
                        "retire + fresh decode",
                        frame_number, key);
          }
          RetireGuestTexture(it->second, context.device->CurrentSubmission());
          g_r.tex_store.erase(it);
          it = g_r.tex_store.end();  // falls into the inline decode below
        } else {
          // Budget exhausted (burst overflow): heal on the workers, serve
          // the stale decode meanwhile.
          EnqueueWordsMiss(key, fetch, /*ui=*/true);
          it->second.recheck_frame = frame_number + 2;
          g_2d_async_stale.fetch_add(1, std::memory_order_relaxed);
        }
      } else {
        // Menu screens probe on a 2-frame cadence: the skater-portrait
        // boxes are resolved IN PLACE (words unchanged) up to hundreds of
        // ms after the quad first draws (asset streaming first), and a
        // 16-frame recheck left the pre-resolve memory content, the
        // loading-poster flash, on screen for its full window.
        it->second.recheck_frame =
            frame_number + (g_in_menus_frame.load(std::memory_order_relaxed) ? 2 : 16);
      }
    }
    if (it == g_r.tex_store.end()) {
      // First sightings also decode INLINE while the budget lasts. The old
      // always-async policy skipped the quad for the 1-3 worker-round-trip
      // frames, but animating APT art re-rasterizes at NEW addresses, so
      // every animation step was a "first sighting" and menu elements
      // (including fullscreen backdrops) BLINKED through every animation
      // (askip +150/window during navigation, visible
      // menu flicker/brightness pulsing). Async remains the burst-overflow
      // valve only.
      const bool fullscreen_class = uint64_t(px_w) * px_h >= 460'000;
      const bool inline_ok =
          hot_inline_budget_ns > 0 ||
          (fullscreen_class && hot_inline_budget_ns > -4'000'000);
      if (async_ui && !video && !inline_redecode && !inline_ok) {
        EnqueueWordsMiss(key, fetch, /*ui=*/true);
        g_2d_async_skip.fetch_add(1, std::memory_order_relaxed);
        if (tr2d) {
          REXLOG_INFO("2d-trace: f{} key={:016X} SKIP (async first sight)",
                      frame_number, key);
        }
        return nullptr;
      }
      // Small HUD/spline art decodes inline (sub-ms; async would pop
      // HUD elements for no gain).
      const auto hud_t0 = PerfClock::now();
      GuestTexture gt;
      EnsureGuestTextureFromWords(context, base, fetch, gt);
      const uint64_t decode_ns = perf_ns_since(hud_t0);
      hot_inline_budget_ns -= int64_t(decode_ns);
      g_pw_tex_decode.Add(decode_ns);
      // Attribution for residual render-thread stalls: anything still
      // decoding inline for >3 ms should either move over the async
      // threshold or explain itself here.
      if (decode_ns > 3'000'000) {
        static std::atomic<uint32_t> s_slow{0};
        const uint32_t n = s_slow.fetch_add(1, std::memory_order_relaxed);
        if (n < 24 || (n & 255u) == 0) {
          REXLOG_INFO(
              "native-scene: SLOW inline 2D decode {:.1f}ms (copy {:.1f} "
              "create {:.1f} gen {:.1f}) {}x{} fetch=[{:08X} {:08X} {:08X}] "
              "forced={} (n={})",
              double(decode_ns) / 1e6, double(g_tex_dec_copy_ns) / 1e6,
              double(g_tex_dec_create_ns) / 1e6, double(g_tex_dec_gen_ns) / 1e6,
              px_w, px_h, fetch[0], fetch[1], fetch[2], force_inline ? 1 : 0, n);
        }
      }
      if (!gt.valid) {
        static std::unordered_set<uint64_t> logged;
        if (logged.size() < 32 && logged.insert(key).second) {
          REXLOG_INFO(
              "native-scene: 2D texture decode FAILED fetch=[{:08X} {:08X} {:08X} "
              "{:08X} {:08X} {:08X}]",
              fetch[0], fetch[1], fetch[2], fetch[3], fetch[4], fetch[5]);
        }
      }
      cmd->FlushBarriers();
      gt.last_used_frame = frame_number;
      gt.last_change_frame = frame_number;
      if (tr2d) {
        REXLOG_INFO("2d-trace: f{} key={:016X} INLINE decode valid={} inc={} "
                    "fp={:016X}",
                    frame_number, key, gt.valid ? 1 : 0, gt.incomplete ? 1 : 0,
                    gt.payload_fp);
      }
      it = g_r.tex_store.emplace(key, gt).first;
    }
    if (it != g_r.tex_store.end() &&
        (tr2d || (g_in_menus_frame.load(std::memory_order_relaxed) &&
                  uint64_t(px_w) * px_h >= 32768))) {
      // Menu thumbnail-class serve log: per buffer (fetch word 1), log when
      // EITHER the served cache content (stored fp) OR the live guest
      // content (probe fp) changes. A served-fp that lags the live-fp is
      // exactly the stale-thumbnail bug; the buffer holds the new area's
      // art while we keep serving the previous decode.
      static std::mutex s_2d_log_mutex;
      static std::unordered_map<uint32_t, uint64_t> s_2d_seen;  // w1 -> stored^live
      const uint64_t live_fp = SampleProbeFingerprint(base, it->second);
      const uint64_t sig = it->second.payload_fp ^ (live_fp * 3);
      std::lock_guard<std::mutex> lk(s_2d_log_mutex);
      auto sit = s_2d_seen.find(fetch[1]);
      if (sit == s_2d_seen.end() || sit->second != sig) {
        s_2d_seen[fetch[1]] = sig;
        REXLOG_INFO(
            "2d-thumb: f{} w1={:08X} {}x{} key={:016X} served_fp={:016X} "
            "live_fp={:016X}{} valid={} inc={} recheck=f{}",
            frame_number, fetch[1], px_w, px_h, key, it->second.payload_fp,
            live_fp,
            (live_fp != 0 && live_fp != it->second.payload_fp) ? " STALE" : "",
            it->second.valid ? 1 : 0, it->second.incomplete ? 1 : 0,
            it->second.recheck_frame);
      }
    }
    return it->second.valid ? &it->second : &g_r.white;
  };
  const uint32_t ui_region =
      uint32_t(frame_number % RendererState::kUiRegions) * RendererState::kUiRegionSize;
  uint32_t ui_offset = 0;

  // In-world neon splines (waypoint arrows / marker beams): replayed inside
  // the scene pass, depth-tested against the world like the emulated frame,
  // in submission order (darken backdrop passes precede the additive glow).
  uint32_t drawn_spline = 0;
  if (REXCVAR_GET(skate3_native_render_scene_splines) &&
      g_r.pso_spline_default != nullptr && g_r.ui_ring_cpu != nullptr) {
    std::vector<SplineDraw> scene_spline;
    {
      std::lock_guard<std::mutex> lock(g_2d_mutex);
      scene_spline = g_scene_spline;
    }
    for (const SplineDraw& s : scene_spline) {
      const uint32_t bytes = uint32_t(s.verts.size());
      if (bytes == 0 || ui_offset + bytes > RendererState::kUiRegionSize) {
        continue;
      }
      std::memcpy(g_r.ui_ring_cpu + ui_region + ui_offset, s.verts.data(), bytes);
      const GuestTexture* spline_tex = resolve_2d_texture(s.fetch);
      if (spline_tex == nullptr) {
        continue;  // big-art decode in flight on the workers; skip a frame
      }
      cmd->SetPipeline(s.pass == 1 ? g_r.pso_spline_darken
                                   : g_r.pso_spline_default);
      // Root constants: the scene's (smoothed) view_proj rows; the verts
      // are world-space, then i_intensity as staged (c149).
      float spline_consts[20];
      std::memcpy(spline_consts, scene.view_proj, sizeof(float) * 16);
      std::memcpy(spline_consts + 16, s.consts + 149 * 4, sizeof(float) * 4);
      // intensity.zw = showcase blackout gate (the guest row's unused
      // lanes): z = split x, w = left|right visibility bits. The spline PS
      // draws outside the scene shaders, so the blackout stage must gate
      // it here or the neon lines leak over the black recording bookends.
      const auto side_visible = [](float v) {
        return v < 255.5f || ((uint32_t(v + 0.5f) - 256u) & 1024u) == 0u;
      };
      spline_consts[18] = g_r.showcase_rows[2];
      spline_consts[19] =
          float((side_visible(g_r.showcase_rows[0]) ? 1 : 0) |
                (side_visible(g_r.showcase_rows[1]) ? 2 : 0));
      cmd->SetRootConstants(0, 20, spline_consts, 0);
      cmd->SetTexture(1, spline_tex->srv);
      cmd->SetVertexBuffer(g_r.ui_ring, ui_region + ui_offset, bytes, 28);
      cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
      cmd->Draw(s.count, 0);
      ui_offset += bytes;
      ++drawn_spline;
    }
  }

  if (use_depth && !loading_native &&
      mechanics_sandbox::Active()) {
    const RaytracedDynamicScene raytraced_dynamic =
        BuildRaytracedLocalPresentation(
            scene, raytraced_diffuse_by_item);
    if (RenderRaytracedMirror(
            context, cmd, scene, scene_color, g_r.depth,
            msaa_on ? g_r.msaa : 1u, &raytraced_dynamic)) {
      // The DXR composite owns a compact root signature. Restore the main
      // scene layout before outline/SSR/resolve: those paths deliberately
      // latch this layout and only restage the bindings they consume.
      cmd->SetBindingLayout(g_r.layout);
    }
  }

  const bool outline_ready =
      RenderOutlineMask(context, scene, viewport, scissor, msaa_on, scene_color,
                        g_r.depth, use_depth);

  // ---- SSR reflection G-buffer (scene.hlsl ps_refl_gbuf) ----
  // Re-render the frame's reflective items (captured with their main-pass
  // constants/textures by draw_item) into the half-res G-buffer while the
  // main binding layout is still live. The march/composite consume it after
  // the resolve + AO passes (ApplySsrPass); it hands off in
  // PIXEL_SHADER_RESOURCE state via ssr_gbuf_ready.
  g_r.ssr_gbuf_ready = false;
  if (!ssr_items.empty() && EnsureSsrPipeline(context) &&
      EnsureSsrTargets(context)) {
    // Front-to-back for early-z; the pass's own depth buffer resolves
    // overlap (a curved facade's far side shares texels with its near side
    // within one mesh; no draw order can fix that).
    std::stable_sort(ssr_items.begin(), ssr_items.end(),
                     [&](const SsrGbufItem& a, const SsrGbufItem& b) {
                       return view_dist2(*a.item) < view_dist2(*b.item);
                     });
    const float gbuf_clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    cmd->ClearRenderTarget(g_r.ssr_gbuf, gbuf_clear);
    cmd->ClearDepth(g_r.ssr_gbuf_depth, 1.0f);
    cmd->SetRenderTargets(g_r.ssr_gbuf, g_r.ssr_gbuf_depth);
    cmd->SetViewport(nrhi::Viewport{0.0f, 0.0f, float(g_r.ssr_width),
                                    float(g_r.ssr_height), 0.0f, 1.0f});
    cmd->SetScissor(
        nrhi::Rect{0, 0, int32_t(g_r.ssr_width), int32_t(g_r.ssr_height)});
    cmd->SetPipeline(g_r.pso_ssr_gbuf);
    uint32_t gbuf_draws = 0;
    for (const SsrGbufItem& si : ssr_items) {
      auto mit = g_r.meshes.find(si.mesh);
      if (mit == g_r.meshes.end() ||
          mit->second.fingerprint != si.fingerprint) {
        continue;  // decode swapped since the main-pass draw; next frame
      }
      cmd->SetRootConstants(0, 52, si.constants, 0);
      cmd->SetTexture(4, si.t3);
      cmd->SetTexturePair(5, si.t4, si.t5);
      cmd->SetVertexBuffer(mit->second.vb_view.buffer,
                           mit->second.vb_view.offset,
                           mit->second.vb_view.size_bytes,
                           mit->second.vb_view.stride);
      cmd->SetIndexBuffer(mit->second.ib_view.buffer,
                          mit->second.ib_view.offset,
                          mit->second.ib_view.size_bytes);
      for (const DrawEntry& draw : si.item->draws) {
        if (draw.prim == 4) {
          cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
        } else if (draw.prim == 6) {
          cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
        } else {
          continue;
        }
        cmd->DrawIndexed(draw.index_count, draw.start_index,
                         draw.base_vertex);
        ++gbuf_draws;
      }
    }
    // Restore the pass state the resolve/post paths rely on.
    cmd->SetViewport(viewport);
    cmd->SetScissor(scissor);
    if (!msaa_on) {
      cmd->SetRenderTargets(scene_color, use_depth ? g_r.depth : nullptr);
    }
    if (gbuf_draws > 0) {
      cmd->Barrier(g_r.ssr_gbuf, nrhi::ResourceState::kRenderTarget,
                   nrhi::ResourceState::kPixelShaderResource);
      g_r.ssr_gbuf_ready = true;
    }
  }

  cmd->ProfileRegion(nrhi::ProfileStage::kResolve);
  if (msaa_on) {
    // Resolve: average the MSAA samples into the 1x scene plane (the float
    // HDR plane, or the guest output on the classic path) with a fullscreen
    // pass, then restore steady-state resource states.
    cmd->Barrier(g_r.msaa_color, nrhi::ResourceState::kRenderTarget,
                 nrhi::ResourceState::kPixelShaderResource);
    if (!hdr_on) {
      cmd->Barrier(context.guest_output, nrhi::ResourceState::kGuestOutput,
                   nrhi::ResourceState::kRenderTarget);
    }
    cmd->FlushBarriers();
    cmd->SetRenderTargets(hdr_on ? g_r.hdr_resolved : context.guest_output,
                          nullptr);
    cmd->SetPipeline(g_r.resolve_pso);
    cmd->SetTexture(1, g_r.msaa_srv_slot);
    cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
    cmd->Draw(3, 0);
    cmd->Barrier(g_r.msaa_color, nrhi::ResourceState::kPixelShaderResource,
                 nrhi::ResourceState::kRenderTarget);
  }

  // Classic path: the outline composites straight onto the resolved guest
  // output. Under HDR it runs after the tonemap below (a gamma-space
  // screen overlay, not scene lighting).
  if (outline_ready && !hdr_on) {
    RenderOutlineComposite(context, scene, context.guest_output, viewport,
                           scissor);
  }

  // ---- Screen-space ambient occlusion (ssao.hlsl: GTAO) ----
  // Multiplies the resolved scene by a horizon-based visibility term before
  // any downstream consumer (bloom/tonemap under HDR, photo chain, 2D
  // overlay, blur). Runs after the outline composite, which relies on the
  // main layout latched from the scene pass.
  bool post_ran = false;
  bool ssao_ran = false;
  cmd->ProfileRegion(nrhi::ProfileStage::kAmbientOcclusion);
  if (use_depth && !loading_native &&
      REXCVAR_GET(skate3_native_render_scene_ssao) &&
      ApplySsaoPass(context, cmd, scene, viewport, scissor)) {
    post_ran = true;
    ssao_ran = true;
  }

  // ---- Screen-space reflections (ssr.hlsl) ----
  // March + composite onto the pre-tonemap HDR plane, between the AO pass
  // and the HDR post, so reflections tonemap and bloom exactly like
  // directly-visible scenery. The reflection G-buffer was drawn inside the
  // scene pass (ssr_gbuf_ready); the SSAO linear-depth plane is reused when
  // AO ran this frame.
  cmd->ProfileRegion(nrhi::ProfileStage::kSsr);
  if (g_r.ssr_gbuf_ready &&
      ApplySsrPass(context, cmd, scene, viewport, scissor, ssao_ran)) {
    post_ran = true;
  }

  // ---- Volumetric lighting (hdr.hlsl ps_vol_*) ----
  // Shadow-marched sun shafts (per-step visibility against the CSM atlas +
  // the static world-shadow map), plus the constant staging for the
  // directional haze; both terms join in ps_tonemap after the AO multiply,
  // so they bloom and tonemap like scene light.
  cmd->ProfileRegion(nrhi::ProfileStage::kVolumetrics);
  if (hdr_on && use_depth && !loading_native &&
      ApplyVolumetricPass(context, cmd, scene, viewport, scissor, ssao_ran,
                          frame_number)) {
    post_ran = true;
  }

  // ---- HDR post (hdr.hlsl: bloom pyramid + the extracted tonemap) ----
  // Bloom reads the AO-composited float scene; ps_tonemap applies the
  // game's shared tone chain once into the guest output, which every later
  // consumer (photo chain, 2D overlay, blur, grab) reads exactly as on the
  // classic path.
  cmd->ProfileRegion(nrhi::ProfileStage::kBloom);
  if (hdr_on) {
    ApplyHdrPost(context, cmd, viewport, scissor, loading_native,
                 frame_number);
    post_ran = true;
    skate3::mechanics_sandbox::RecordRenderStage(
        skate3::mechanics_sandbox::RenderStage::HdrPostComplete);
  }

  if (post_ran) {
    // The AO/HDR chains switched binding layouts; restore the main-pass
    // root bindings the later passes latch (same restore as the photo
    // chain's).
    cmd->SetViewport(viewport);
    cmd->SetScissor(scissor);
    cmd->SetBindingLayout(g_r.layout);
    if (g_r.shadow_cb != nullptr) {
      const uint32_t cb_offset =
          uint32_t(frame_number % RendererState::kShadowCbRegions) *
        RendererState::kShadowCbSlice;
      cmd->SetConstantBuffer(6, g_r.shadow_cb, cb_offset);
      cmd->SetConstantBuffer(9, g_r.bone_ring, bone_region);
    }
  }

  if (outline_ready && hdr_on) {
    RenderOutlineComposite(context, scene, context.guest_output, viewport,
                           scissor);
  }

  // ---- Photo-editor postfx chain (photo_fx.hlsl: exact ucode ports) ----
  // While the photo-mission photo editor is up and every pass's live
  // constants were captured this frame, apply the game's own chain over the
  // resolved native frame: depth pack -> visualfx (grade/vignette/CoC) ->
  // DOF downsample -> tap9dofMotionBlur -> tap9dof -> uber -> fisheye.
  if (scene.photo_fx.valid &&
      REXCVAR_GET(skate3_native_render_scene_photo_native) &&
      EnsurePhotoFxPipeline(context)) {
    const auto pfx_to_srv = [&](nrhi::Texture* r) {
      cmd->Barrier(r, nrhi::ResourceState::kRenderTarget,
                   nrhi::ResourceState::kPixelShaderResource);
    };
    const auto pfx_to_rt = [&](nrhi::Texture* r) {
      cmd->Barrier(r, nrhi::ResourceState::kPixelShaderResource,
                   nrhi::ResourceState::kRenderTarget);
    };
    const auto pfx_flush = [&] { cmd->FlushBarriers(); };
    // Output-sized targets (visualfx out, uber out, packed depth).
    bool pfx_ok = true;
    if (g_r.pfx_width != context.guest_output_width ||
        g_r.pfx_height != context.guest_output_height || g_r.pfx_full[0] == nullptr) {
      nrhi::Texture** res[3] = {&g_r.pfx_full[0], &g_r.pfx_full[1], &g_r.pfx_depth};
      nrhi::TextureView** views[3] = {&g_r.pfx_srv[0], &g_r.pfx_srv[1],
                                      &g_r.pfx_srv[5]};
      nrhi::TextureDesc desc;
      desc.width = context.guest_output_width;
      desc.height = context.guest_output_height;
      desc.mip_levels = 1;
      desc.format = nrhi::Format::kR8G8B8A8_UNORM;
      desc.usage = nrhi::kTextureUsageRenderTarget;
      desc.initial_state = nrhi::ResourceState::kRenderTarget;
      for (int i = 0; i < 3 && pfx_ok; ++i) {
        if (*views[i] != nullptr) {
          g_r.device->DestroyDeferred(*views[i]);
          *views[i] = nullptr;
        }
        if (*res[i] != nullptr) {
          g_r.device->DestroyDeferred(*res[i]);
          *res[i] = nullptr;
        }
        *res[i] = context.device->CreateTexture(desc);
        if (*res[i] == nullptr) {
          pfx_ok = false;
          break;
        }
        nrhi::TextureViewDesc vd;
        vd.mip_levels = 1;
        *views[i] = context.device->CreateTextureView(*res[i], vd);
        if (*views[i] == nullptr) {
          pfx_ok = false;
          break;
        }
      }
      if (pfx_ok) {
        g_r.pfx_width = context.guest_output_width;
        g_r.pfx_height = context.guest_output_height;
      }
    }
    if (pfx_ok) {
      // Identity grade-LUT upload (once); the staging buffer is done after
      // the copy is recorded.
      if (!g_r.pfx_lut_uploaded) {
        cmd->CopyBufferToTexture(g_r.pfx_lut, 0, 0, g_r.pfx_lut_upload, 0, 256,
                                 32, 32, 32);
        cmd->Barrier(g_r.pfx_lut, nrhi::ResourceState::kCopyDest,
                     nrhi::ResourceState::kPixelShaderResource);
        g_r.device->DestroyDeferred(g_r.pfx_lut_upload);
        g_r.pfx_lut_upload = nullptr;
        g_r.pfx_lut_uploaded = true;
      }
      // Native depth SRV (re-pointed when the depth texture is rebuilt on
      // output resize; D32 textures view as R32_FLOAT automatically).
      {
        static nrhi::Texture* s_pfx_depth_tex = nullptr;
        if (g_r.pfx_srv[7] == nullptr || s_pfx_depth_tex != g_r.depth) {
          if (g_r.pfx_srv[7] != nullptr) {
            g_r.device->DestroyDeferred(g_r.pfx_srv[7]);
            g_r.pfx_srv[7] = nullptr;
          }
          nrhi::TextureViewDesc sd;
          if (g_r.msaa > 1) {
            sd.dimension = nrhi::ViewDimension::k2DMS;
          } else {
            sd.dimension = nrhi::ViewDimension::k2D;
            sd.mip_levels = 1;
          }
          g_r.pfx_srv[7] = context.device->CreateTextureView(g_r.depth, sd);
          s_pfx_depth_tex = g_r.depth;
        }
      }
      // (The guest-output view, the finished native frame = the chain's
      // scene input, is g_r.output_srv_slot, owned/re-pointed by
      // EnsurePipeline on output change.)
      // The two static input textures (fetch words captured at the game's
      // own uber/fisheye flushes): the 512x2 vignette gradient + the grain.
      const GuestTexture* vig =
          resolve_2d_texture(scene.photo_fx.vignette_fetch, /*force_inline=*/true);
      const GuestTexture* grain =
          resolve_2d_texture(scene.photo_fx.grain_fetch, /*force_inline=*/true);
      nrhi::TextureView* const vig_slot =
          (vig != nullptr && vig->valid) ? vig->srv : g_r.white.srv;
      nrhi::TextureView* const grain_slot =
          (grain != nullptr && grain->valid) ? grain->srv : g_r.white.srv;

      // Baked literal rows c250..c255 per pass (the shader asset footers the
      // game loads via PM4 LOAD_ALU_CONSTANT to PS rows 252+; values read
      // from capture).
      static constexpr float kPfxLiterals[kPfxPassCount][6][4] = {
          // visualfx
          {{0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {-0.200000003f, 1.0f, 1.41412354f, 2.0f},
           {1.51991853e-05f, 0.99609381f, 0.00389099144f, 0.00392156886f},
           {0.200000003f, 0.300000012f, 0.5f, 1.0f}},
          // dof downsample
          {{0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {0.000868055562f, 0.00156250002f, 0.25f, 0.00100000005f}},
          // tap9dofMotionBlur
          {{0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {1.0f, 1.5f, 9.99999975e-05f, 0.00392156886f},
           {1.51991853e-05f, 0.99609381f, 0.00389099144f, 0.0f}},
          // tap9dof
          {{0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {0.100000001f, 0, 0, 0}},
          // uber
          {{0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {2.0f, 0.5f, -1.0f, 0.0f},
           {0.015625f, 0.984375f, 0.96875f, -0.96875f},
           {1.0f, 0, 0, 0}},
          // fisheye
          {{0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {-1.0f, 0.5f, 0, 0},
           {-0.5f, -0.888888896f, 1.0f, 1.77777779f}},
      };
      const uint32_t cb_slot_base = uint32_t(frame_number % 4) * 10;
      uint32_t cb_slot_next = 0;
      const int32_t pfx_debug =
          REXCVAR_GET(skate3_native_render_scene_photo_native_debug);
      // Returns the byte OFFSET into g_r.pfx_cb for SetConstantBuffer(0, ...).
      const auto fill_cb = [&](int pass) -> uint64_t {
        const uint32_t slot = cb_slot_base + (cb_slot_next++);
        uint8_t* dst = g_r.pfx_cb_ptr + size_t(slot) * 4096;
        std::memset(dst, 0, 4096);
        float* rows = reinterpret_cast<float*>(dst);
        if (pass >= 0) {
          std::memcpy(rows, scene.photo_fx.ps[pass], sizeof(scene.photo_fx.ps[pass]));
          std::memcpy(rows + 240 * 4, scene.photo_fx.vs[pass],
                      sizeof(scene.photo_fx.vs[pass]));
          std::memcpy(rows + 250 * 4, kPfxLiterals[pass], sizeof(kPfxLiterals[pass]));
          // The game's quad VSs add half a DEST texel to the sampling uv
          // (visualfx c3 = 0.5/1152,0.5/640; dof passes c3 = 0.5/576,
          // 0.5/320; fisheye c2 = 0.5/1280,0.5/720), D3D9/Xenos raster
          // compensation: their pixel centers interpolate uv = i/W, so
          // +half lands on the intended (i+0.5)/W. Our D3D12 fullscreen
          // triangle interpolates (i+0.5)/W ALREADY; keeping the captured
          // offsets shifts each pass's sampling by half a dest texel
          // (content drifts up-left ~1.5 half-res texels across the three
          // half-res DOF passes = the observed "blur starts above the
          // pole" mask misregistration). ZERO them
          // natively. Guarded to the expected sub-pixel magnitudes so a
          // mis-captured row is left alone.
          {
            float* half_px =
                rows + (pass == kPfxFisheye ? 242 : 243) * 4;
            if (std::fabs(half_px[0]) < 0.002f && std::fabs(half_px[1]) < 0.004f) {
              half_px[0] = 0.0f;
              half_px[1] = 0.0f;
            }
          }
        }
        rows[248 * 4 + 0] = float(context.guest_output_width);
        rows[248 * 4 + 1] = float(context.guest_output_height);
        rows[249 * 4 + 0] = float(pfx_debug);
        return uint64_t(slot) * 4096;
      };
      // One 8-entry texture table (pfx_layout param 1, registers t0..t7);
      // the argument order keeps the old per-parameter helper's shape (t5
      // last), the gather re-orders into register order.
      const auto pfx_bind_all = [&](nrhi::TextureView* t0, nrhi::TextureView* t1,
                                    nrhi::TextureView* t2, nrhi::TextureView* t3,
                                    nrhi::TextureView* t4, nrhi::TextureView* t6,
                                    nrhi::TextureView* t7, nrhi::TextureView* t5) {
        nrhi::TextureView* views[8] = {t0, t1, t2, t3, t4, t5, t6, t7};
        cmd->SetTextures(1, views, 8);
      };
      nrhi::TextureView* const W = g_r.white.srv;
      const nrhi::Viewport half_vp{0.0f, 0.0f, float(RendererState::kPfxHalfW),
                                   float(RendererState::kPfxHalfH), 0.0f, 1.0f};
      const nrhi::Rect half_sc{0, 0, int32_t(RendererState::kPfxHalfW),
                               int32_t(RendererState::kPfxHalfH)};
      const nrhi::Viewport quarter_vp{0.0f, 0.0f,
                                      float(RendererState::kPfxQuarterW),
                                      float(RendererState::kPfxQuarterH), 0.0f,
                                      1.0f};
      const nrhi::Rect quarter_sc{0, 0, int32_t(RendererState::kPfxQuarterW),
                                  int32_t(RendererState::kPfxQuarterH)};
      const auto set_rtv = [&](nrhi::Texture* target) {
        cmd->SetRenderTargets(target, nullptr);
      };
      cmd->SetBindingLayout(g_r.pfx_layout);
      cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
      const int32_t accum_mode =
          REXCVAR_GET(skate3_native_render_scene_photo_native_accum);
      // Live capture telemetry (~every 2 s while the chain runs): the rows
      // that move with the editor sliders. Lets a session log confirm the
      // draw-time capture tracks DoF (dof c0.x / dofmb c2.x, 0 = slider
      // off), focus (visualfx c2.y), the grade matrix (visualfx c7) and the
      // uber jitter phase against the emulated ground-truth values.
      {
        static uint64_t s_pfx_log_frame = 0;
        if (frame_number - s_pfx_log_frame > 120) {
          s_pfx_log_frame = frame_number;
          const auto& fx = scene.photo_fx;
          REXLOG_DEBUG(
              "native-scene: pfx live rows: visualfx c2=({:.3f},{:.3f}) "
              "c4.x={:.5f} c7=({:.4f},{:.4f},{:.4f},{:.4f}) | dofmb "
              "c2.x={:.3f} | dof c0.x={:.3f} | uber c0.x={:.3f} "
              "c2.x={:.4f} c5.x={:.3f} | vfx vs c3=({:.6f},{:.6f})",
              fx.ps[kPfxVisualFx][2][0], fx.ps[kPfxVisualFx][2][1],
              fx.ps[kPfxVisualFx][4][0], fx.ps[kPfxVisualFx][7][0],
              fx.ps[kPfxVisualFx][7][1], fx.ps[kPfxVisualFx][7][2],
              fx.ps[kPfxVisualFx][7][3], fx.ps[kPfxDofMB][2][0],
              fx.ps[kPfxDof][0][0], fx.ps[kPfxUber][0][0],
              fx.ps[kPfxUber][2][0], fx.ps[kPfxUber][5][0],
              fx.vs[kPfxVisualFx][3][0], fx.vs[kPfxVisualFx][3][1]);
        }
      }

      // 1) Depth pack: native depth (sample 0) -> the console D24-as-8888
      //    layout at output res.
      cmd->Barrier(g_r.depth, nrhi::ResourceState::kDepthWrite,
                   nrhi::ResourceState::kPixelShaderResource);
      pfx_flush();
      set_rtv(g_r.pfx_depth);
      cmd->SetViewport(viewport);
      cmd->SetScissor(scissor);
      cmd->SetPipeline(g_r.pfx_pso[0]);
      cmd->SetConstantBuffer(0, g_r.pfx_cb, fill_cb(-1));
      pfx_bind_all(W, W, W, W, g_r.pfx_srv[6], W, W, g_r.pfx_srv[7]);
      cmd->Draw(3, 0);
      cmd->Barrier(g_r.depth, nrhi::ResourceState::kPixelShaderResource,
                   nrhi::ResourceState::kDepthWrite);
      pfx_to_srv(g_r.pfx_depth);
      // Scene input -> SRV for the rest of the chain.
      cmd->Barrier(context.guest_output, nrhi::ResourceState::kRenderTarget,
                   nrhi::ResourceState::kPixelShaderResource);
      pfx_flush();

      // 2) Accumulation input (visualfx t3). Mode 0 = black, 1 = the scene
      //    downsampled (this frame, the editor scene is frozen).
      if (accum_mode == 0) {
        const float black[4] = {0, 0, 0, 0};
        cmd->ClearRenderTarget(g_r.pfx_quarter, black);
      } else if (accum_mode == 1) {
        set_rtv(g_r.pfx_quarter);
        cmd->SetViewport(quarter_vp);
        cmd->SetScissor(quarter_sc);
        cmd->SetPipeline(g_r.pfx_pso[7]);
        cmd->SetConstantBuffer(0, g_r.pfx_cb, fill_cb(-1));
        pfx_bind_all(g_r.output_srv_slot, W, W, W, g_r.pfx_srv[6], W, W, W);
        cmd->Draw(3, 0);
      }
      pfx_to_srv(g_r.pfx_quarter);
      pfx_flush();

      // 3) visualfx (full res): scene + depth + accumulation -> pfx_full[0].
      set_rtv(g_r.pfx_full[0]);
      cmd->SetViewport(viewport);
      cmd->SetScissor(scissor);
      cmd->SetPipeline(g_r.pfx_pso[1]);
      cmd->SetConstantBuffer(0, g_r.pfx_cb, fill_cb(kPfxVisualFx));
      pfx_bind_all(g_r.output_srv_slot, g_r.pfx_srv[5], W, g_r.pfx_srv[4],
                   g_r.pfx_srv[6], W, W, W);
      cmd->Draw(3, 0);
      pfx_to_srv(g_r.pfx_full[0]);
      pfx_flush();

      // 4) DOF downsample: pfx_full[0] -> pfx_half[0].
      set_rtv(g_r.pfx_half[0]);
      cmd->SetViewport(half_vp);
      cmd->SetScissor(half_sc);
      cmd->SetPipeline(g_r.pfx_pso[2]);
      cmd->SetConstantBuffer(0, g_r.pfx_cb, fill_cb(kPfxDofDown));
      pfx_bind_all(g_r.pfx_srv[0], W, W, W, g_r.pfx_srv[6], W, W, W);
      cmd->Draw(3, 0);
      pfx_to_srv(g_r.pfx_half[0]);
      pfx_flush();

      // 5) tap9dofMotionBlur: pfx_half[0] + depth -> pfx_half[1].
      set_rtv(g_r.pfx_half[1]);
      cmd->SetPipeline(g_r.pfx_pso[3]);
      cmd->SetConstantBuffer(0, g_r.pfx_cb, fill_cb(kPfxDofMB));
      pfx_bind_all(g_r.pfx_srv[2], g_r.pfx_srv[5], W, W, g_r.pfx_srv[6], W, W, W);
      cmd->Draw(3, 0);
      pfx_to_srv(g_r.pfx_half[1]);
      pfx_to_rt(g_r.pfx_half[0]);
      pfx_flush();

      // 6) tap9dof: pfx_half[1] -> pfx_half[0].
      set_rtv(g_r.pfx_half[0]);
      cmd->SetPipeline(g_r.pfx_pso[4]);
      cmd->SetConstantBuffer(0, g_r.pfx_cb, fill_cb(kPfxDof));
      pfx_bind_all(g_r.pfx_srv[3], W, W, W, g_r.pfx_srv[6], W, W, W);
      cmd->Draw(3, 0);
      pfx_to_srv(g_r.pfx_half[0]);
      pfx_flush();

      // 7) uber (full res): graded sharp + depth + LUT + grain + blurred
      //    half -> pfx_full[1].
      set_rtv(g_r.pfx_full[1]);
      cmd->SetViewport(viewport);
      cmd->SetScissor(scissor);
      cmd->SetPipeline(g_r.pfx_pso[5]);
      cmd->SetConstantBuffer(0, g_r.pfx_cb, fill_cb(kPfxUber));
      pfx_bind_all(g_r.pfx_srv[0], g_r.pfx_srv[5], W, W, g_r.pfx_srv[6],
                   grain_slot, g_r.pfx_srv[2], W);
      cmd->Draw(3, 0);
      pfx_to_srv(g_r.pfx_full[1]);
      // The finished chain replaces the output: back to RT for fisheye.
      cmd->Barrier(context.guest_output,
                   nrhi::ResourceState::kPixelShaderResource,
                   nrhi::ResourceState::kRenderTarget);
      pfx_flush();

      // 8) fisheye (to screen): lens warp + vignette gradient + tint.
      cmd->SetRenderTargets(context.guest_output, nullptr);
      cmd->SetPipeline(g_r.pfx_pso[6]);
      cmd->SetConstantBuffer(0, g_r.pfx_cb, fill_cb(kPfxFisheye));
      pfx_bind_all(g_r.pfx_srv[1], W, vig_slot, W, g_r.pfx_srv[6], W, W, W);
      cmd->Draw(3, 0);

      // 8b) Debug view (photo_native_debug): overwrite the output with the
      //     visualfx CoC map (mode 1) or the packed-depth reconstruction
      //     (mode 2): t0 = visualfx out, t1 = packed depth, both still in
      //     SRV state here.
      if (pfx_debug > 0 && g_r.pfx_pso[8] != nullptr) {
        cmd->SetPipeline(g_r.pfx_pso[8]);
        cmd->SetConstantBuffer(0, g_r.pfx_cb, fill_cb(-1));
        pfx_bind_all(g_r.pfx_srv[0], g_r.pfx_srv[5], W, W, g_r.pfx_srv[6], W,
                     W, W);
        cmd->Draw(3, 0);
      }

      // 9) Accumulation mode 2: next frame's visualfx t3 = the finished
      //    frame downsampled (pfx_full[1] is still in SRV state here).
      if (accum_mode == 2) {
        pfx_to_rt(g_r.pfx_quarter);
        pfx_flush();
        set_rtv(g_r.pfx_quarter);
        cmd->SetViewport(quarter_vp);
        cmd->SetScissor(quarter_sc);
        cmd->SetPipeline(g_r.pfx_pso[7]);
        cmd->SetConstantBuffer(0, g_r.pfx_cb, fill_cb(-1));
        pfx_bind_all(g_r.pfx_srv[1], W, W, W, g_r.pfx_srv[6], W, W, W);
        cmd->Draw(3, 0);
      }

      // Restore steady states (all pfx color targets idle as RENDER_TARGET)
      // + the main pass's root bindings for the 2D overlay.
      pfx_to_rt(g_r.pfx_full[0]);
      pfx_to_rt(g_r.pfx_full[1]);
      pfx_to_rt(g_r.pfx_half[0]);
      pfx_to_rt(g_r.pfx_half[1]);
      if (accum_mode != 2) {
        pfx_to_rt(g_r.pfx_quarter);
      }
      pfx_to_rt(g_r.pfx_depth);
      pfx_flush();
      cmd->SetViewport(viewport);
      cmd->SetScissor(scissor);
      cmd->SetBindingLayout(g_r.layout);
      if (g_r.shadow_cb != nullptr) {
        const uint32_t cb_offset =
            uint32_t(frame_number % RendererState::kShadowCbRegions) *
        RendererState::kShadowCbSlice;
        cmd->SetConstantBuffer(6, g_r.shadow_cb, cb_offset);
        cmd->SetConstantBuffer(9, g_r.bone_ring, bone_region);
      }
      static bool s_pfx_first = true;
      if (s_pfx_first) {
        s_pfx_first = false;
        REXLOG_INFO(
            "native-scene: photo editor postfx chain LIVE (native; accum mode {}, "
            "vignette {}, grain {})",
            accum_mode, vig_slot == W ? "WHITE-fallback" : "resolved",
            grain_slot == W ? "WHITE-fallback" : "resolved");
      }
    }
  }

  // Hor+ ultrawide: the 2D stream and the guest screenshot raster are
  // authored for the 16:9 frontbuffer. On a wide output the 2D replay
  // scales clip-space X by this factor (centering ortho HUD/menu/FMV draws
  // in a pillarboxed 16:9 band and narrowing perspective SimpleDraw markers
  // to match the widened scene projection - one operation covers both), and
  // the photo grab center-crops the same band.
  const float out_aspect2d =
      float(context.guest_output_width) / float(context.guest_output_height);
  const float wide_2d_scale =
      out_aspect2d > (16.0f / 9.0f) * 1.01f ? (16.0f / 9.0f) / out_aspect2d : 1.0f;

  cmd->ProfileRegion(nrhi::ProfileStage::k2d);

  // ---- Native photo grab (photo_grab_native) ----
  // Consume: CPU-tile the newest fence-completed readback into the guest
  // screenshot target. Produce: downsample this frame's finished output
  // (photo editor = the ported postfx chain result just drawn above; plain
  // TakePhoto window = the plain frame, both the correct grab semantics)
  // into blur_tex[0] via the blur chain's prefiltered downsample (its
  // 1152x640 blur space IS the game's screenshot raster) and enqueue the
  // copy into the free readback buffer. Runs BEFORE the 2D overlay so the
  // photo never contains editor chrome / HUD, and before the blur block so
  // this scratch use of blur_tex[0] is enqueued ahead of any blur rewrite.
  if (g_photo_grab_native_armed.load(std::memory_order_relaxed) &&
      g_r.pso_blur_down != nullptr && g_r.blur_tex[0] != nullptr &&
      g_r.output_srv_allocated && !g_r.grab_failed) {
    // Lazy readback-pair creation (persistently mapped; the copy is never
    // waited on same-frame; the old path's per-resolve GPU drain was the
    // editor's multi-second stall class).
    if (g_r.grab_readback[0] == nullptr) {
      nrhi::BufferDesc desc;
      desc.size =
          uint64_t(RendererState::kGrabRowPitch) * RendererState::kBlurHeight;
      desc.heap = nrhi::HeapKind::kReadback;
      for (int i = 0; i < 2 && !g_r.grab_failed; ++i) {
        g_r.grab_readback[i] = g_r.device->CreateBuffer(desc);
        if (g_r.grab_readback[i] == nullptr ||
            (g_r.grab_readback_ptr[i] = static_cast<uint8_t*>(
                 g_r.device->Map(g_r.grab_readback[i]))) == nullptr) {
          g_r.grab_failed = true;
        }
      }
      if (g_r.grab_failed) {
        REXLOG_ERROR(
            "native-scene: photo grab readback creation failed - native "
            "grab disabled (watchdog will fall back)");
      }
    }
    if (!g_r.grab_failed) {
      // Consume the newest completed buffer; retire older completed ones
      // unwritten so stale content never lands after fresh content.
      const uint64_t grab_completed = context.device->CompletedSubmission();
      int newest = -1;
      for (int i = 0; i < 2; ++i) {
        if (g_r.grab_pending[i] && g_r.grab_submission[i] < grab_completed &&
            (newest < 0 ||
             g_r.grab_submission[i] > g_r.grab_submission[newest])) {
          newest = i;
        }
      }
      if (newest >= 0) {
        static bool s_grab_write_warned = false;
        const auto tile_t0 = PerfClock::now();
        // Make the GPU's completed copy visible to the CPU read below (no-op
        // on D3D12; cache invalidate on non-coherent Vulkan memory).
        g_r.device->InvalidateForRead(
            g_r.grab_readback[newest], 0,
            uint64_t(RendererState::kGrabRowPitch) * RendererState::kBlurHeight);
        const bool wrote = GrabTargetValid(base) &&
                           WriteGrabToGuest(base, g_r.grab_readback_ptr[newest]);
        if (wrote) {
          g_r.grab_cpu_us +=
              uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                           PerfClock::now() - tile_t0)
                           .count());
          ++g_r.grab_writes;
          g_grab_last_write_ns.store(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  PerfClock::now().time_since_epoch())
                  .count(),
              std::memory_order_relaxed);
          if (g_r.grab_writes == 1) {
            REXLOG_INFO(
                "native-scene: photo grab - first native frame written to "
                "guest 0x{:08X} via the 0xA0000000 view (tiled BE ARGB "
                "1152x640)",
                kGrabGuestAddr);
          }
        } else if (!s_grab_write_warned) {
          s_grab_write_warned = true;
          REXLOG_WARN(
              "native-scene: photo grab - guest target validation/write "
              "FAILED (dims chain mismatch or unmapped page); the window "
              "watchdog will fall back to forced readbacks");
        }
        for (int i = 0; i < 2; ++i) {
          if (g_r.grab_pending[i] && g_r.grab_submission[i] < grab_completed) {
            g_r.grab_pending[i] = false;
          }
        }
      }
      // Produce into the free buffer (skip the frame if both are still in
      // flight, never wait).
      const uint32_t w = g_r.grab_write_index;
      if (!g_r.grab_pending[w]) {
        // (The output view g_r.output_srv_slot tracks the current output
        // texture; EnsurePipeline re-points it on output change.)
        cmd->Barrier(context.guest_output, nrhi::ResourceState::kRenderTarget,
                     nrhi::ResourceState::kPixelShaderResource);
        cmd->FlushBarriers();
        cmd->SetRenderTargets(g_r.blur_tex[0], nullptr);
        const nrhi::Viewport grab_vp{0.0f,
                                     0.0f,
                                     float(RendererState::kBlurWidth),
                                     float(RendererState::kBlurHeight),
                                     0.0f,
                                     1.0f};
        const nrhi::Rect grab_sc{0, 0, int32_t(RendererState::kBlurWidth),
                                 int32_t(RendererState::kBlurHeight)};
        cmd->SetViewport(grab_vp);
        cmd->SetScissor(grab_sc);
        cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
        cmd->SetPipeline(g_r.pso_blur_down);
        // Source UV rect (second register): the centered 16:9 band on a
        // wide output; the guest screenshot raster is 16:9.
        const float grab_consts[8] = {1.0f / float(context.guest_output_width),
                                      1.0f / float(context.guest_output_height),
                                      0.0f,
                                      0.0f,
                                      wide_2d_scale,
                                      1.0f,
                                      (1.0f - wide_2d_scale) * 0.5f,
                                      0.0f};
        cmd->SetRootConstants(0, 8, grab_consts, 0);
        cmd->SetTexture(1, g_r.output_srv_slot);
        cmd->Draw(3, 0);
        // blur_tex[0] -> the readback buffer; restore steady states.
        cmd->Barrier(g_r.blur_tex[0], nrhi::ResourceState::kRenderTarget,
                     nrhi::ResourceState::kCopySource);
        cmd->Barrier(context.guest_output,
                     nrhi::ResourceState::kPixelShaderResource,
                     nrhi::ResourceState::kRenderTarget);
        cmd->FlushBarriers();
        cmd->CopyTextureToBuffer(g_r.grab_readback[w], 0,
                                 RendererState::kGrabRowPitch, g_r.blur_tex[0],
                                 0, RendererState::kBlurWidth,
                                 RendererState::kBlurHeight);
        cmd->Barrier(g_r.blur_tex[0], nrhi::ResourceState::kCopySource,
                     nrhi::ResourceState::kRenderTarget);
        cmd->FlushBarriers();
        // Restore the frame's output binding for the passes that follow.
        cmd->SetRenderTargets(context.guest_output, nullptr);
        cmd->SetViewport(viewport);
        cmd->SetScissor(scissor);
        g_r.grab_submission[w] = context.device->CurrentSubmission();
        g_r.grab_pending[w] = true;
        g_r.grab_write_index = 1 - w;
      }
    }
  }

  // Popup background blur: exact port of the game's blur_hBlur/vBlur +
  // postfx_basictex chain (see kBlurShaderSource): H blur of the finished
  // frame into a 1152x640 intermediate, V blur, then a fullscreen bilinear
  // stretch back over the output. Runs only on frames where the game issued
  // the blur draws (scene.ui_blur = captured kernel scale). The popup's own
  // 2D draws follow after and stay sharp.
  if (scene.ui_blur > 0.0f && g_r.pso_blur != nullptr && g_r.pso_blur_blit != nullptr &&
      g_r.pso_blur_down != nullptr && g_r.blur_tex[0] != nullptr) {
    // Temporal smoothing of the captured radius + fade (~50 ms time
    // constant, render thread): the live bank reads can land mid-rewrite
    // (1-frame value flaps = the tinted-backdrop flicker), and smoothing
    // FROM the off state (radius 0, white tint) supplies the pause-open
    // animate-in even when the game's own ramp frames were missed by the
    // capture gate. A >0.25 s gap since the last blur frame = a fresh ON
    // edge (resets the state); at the render rate active frames are ~7 ms
    // apart so the reset can never fire mid-popup.
    static float s_blur_r = 0.0f;
    static float s_blur_tint[3] = {1.0f, 1.0f, 1.0f};
    static int64_t s_blur_ns = 0;
    const int64_t blur_now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    PerfClock::now().time_since_epoch())
                                    .count();
    float blur_dt = s_blur_ns != 0 ? float(blur_now_ns - s_blur_ns) * 1e-9f : 1.0f;
    if (blur_dt > 0.25f) {
      s_blur_r = 0.0f;
      s_blur_tint[0] = s_blur_tint[1] = s_blur_tint[2] = 1.0f;
      blur_dt = 0.0f;
    }
    s_blur_ns = blur_now_ns;
    const float blur_a = 1.0f - std::exp(-blur_dt / 0.05f);
    s_blur_r += (scene.ui_blur - s_blur_r) * blur_a;
    for (int a = 0; a < 3; ++a) {
      s_blur_tint[a] += (scene.ui_blur_color[a] - s_blur_tint[a]) * blur_a;
    }
    // (The guest output texture can change between frames; the dedicated
    // view g_r.output_srv_slot is re-pointed by EnsurePipeline on change.)
    const nrhi::Viewport blur_vp{0.0f, 0.0f, float(RendererState::kBlurWidth),
                                 float(RendererState::kBlurHeight), 0.0f, 1.0f};
    const nrhi::Rect blur_sc{0, 0, int32_t(RendererState::kBlurWidth),
                             int32_t(RendererState::kBlurHeight)};
    const auto to_srv = [&](nrhi::Texture* r) {
      cmd->Barrier(r, nrhi::ResourceState::kRenderTarget,
                   nrhi::ResourceState::kPixelShaderResource);
    };
    const auto to_rt = [&](nrhi::Texture* r) {
      cmd->Barrier(r, nrhi::ResourceState::kPixelShaderResource,
                   nrhi::ResourceState::kRenderTarget);
    };
    const auto flush = [&] { cmd->FlushBarriers(); };
    cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
    // Downsample (prefiltered): output -> blur_tex[0], the game's 1152x640
    // blur space. The H/V passes then run 1:1 like the original chain.
    to_srv(context.guest_output);
    flush();
    cmd->SetRenderTargets(g_r.blur_tex[0], nullptr);
    cmd->SetViewport(blur_vp);
    cmd->SetScissor(blur_sc);
    cmd->SetPipeline(g_r.pso_blur_down);
    // Full-frame source rect: the blur squeezes the whole wide frame into
    // the 1152x640 blur space and the final stretch-back undoes it.
    const float d_consts[8] = {1.0f / float(context.guest_output_width),
                               1.0f / float(context.guest_output_height),
                               0.0f,
                               0.0f,
                               1.0f,
                               1.0f,
                               0.0f,
                               0.0f};
    cmd->SetRootConstants(0, 8, d_consts, 0);
    cmd->SetTexture(1, g_r.output_srv_slot);
    cmd->Draw(3, 0);
    // H: blur_tex[0] -> blur_tex[1].
    to_srv(g_r.blur_tex[0]);
    flush();
    cmd->SetRenderTargets(g_r.blur_tex[1], nullptr);
    cmd->SetPipeline(g_r.pso_blur);
    const float h_consts[8] = {1.0f,
                               0.0f,
                               s_blur_r,
                               0.0f,
                               s_blur_tint[0],
                               s_blur_tint[1],
                               s_blur_tint[2],
                               1.0f};
    cmd->SetRootConstants(0, 8, h_consts, 0);
    cmd->SetTexture(1, g_r.blur_srv[0]);
    cmd->Draw(3, 0);
    // V: blur_tex[1] -> blur_tex[0].
    to_srv(g_r.blur_tex[1]);
    to_rt(g_r.blur_tex[0]);
    flush();
    cmd->SetRenderTargets(g_r.blur_tex[0], nullptr);
    const float v_consts[8] = {0.0f,
                               1.0f,
                               s_blur_r,
                               0.0f,
                               s_blur_tint[0],
                               s_blur_tint[1],
                               s_blur_tint[2],
                               1.0f};
    cmd->SetRootConstants(0, 8, v_consts, 0);
    cmd->SetTexture(1, g_r.blur_srv[1]);
    cmd->Draw(3, 0);
    // Replace: blur_tex[0] stretched over the full output (basictex).
    to_srv(g_r.blur_tex[0]);
    to_rt(context.guest_output);
    flush();
    cmd->SetRenderTargets(context.guest_output, nullptr);
    cmd->SetViewport(viewport);
    cmd->SetScissor(scissor);
    cmd->SetPipeline(g_r.pso_blur_blit);
    cmd->SetTexture(1, g_r.blur_srv[0]);
    cmd->Draw(3, 0);
    // Restore the intermediates' steady state for the next blur frame.
    to_rt(g_r.blur_tex[0]);
    to_rt(g_r.blur_tex[1]);
  }

  // 2D overlay (HUD/APT): replay the frame's captured 2D draws over the
  // resolved output, in submission order, with the game's own transform
  // constants and textures. In gameplay these draws compose the game's
  // full-screen HUD overlay texture at true screen coordinates; drawing
  // them here IS the composite the (suppressed) emulated pass used to do.
  const auto twod_t0 = PerfClock::now();
  uint32_t drawn_2d = 0;
  const bool suppress_direct_boot_frontend_overlay =
      skate3::demo_path::DirectBootLoadingVisualActive() &&
      rex::kernel::guest_presence::GameplayContextValue() != 0;
  if (!suppress_direct_boot_frontend_overlay &&
      REXCVAR_GET(skate3_native_render_scene_2d) && g_r.pso_2d != nullptr &&
      g_r.ui_ring_cpu != nullptr) {
    std::vector<Draw2d> scene_2d;
    {
      std::lock_guard<std::mutex> lock(g_2d_mutex);
      scene_2d = g_scene_2d;
    }
    if (!scene_2d.empty()) {
      cmd->SetPipeline(g_r.pso_2d);
      // One shared draw routine for both the RTT passes and the screen pass.
      // ui_region/ui_offset continue after the spline pass's allocations.
      // yuv != nullptr redirects this draw to the FMV combine: pso_yuv2d
      // with the three movie plane views on params 1/2/5 (t0/t1/t4), the
      // quad's own geometry/transform (so windowed movies place exactly).
      const auto emit_draw = [&](const Draw2d& d,
                                 nrhi::TextureView* const* yuv = nullptr) {
        const uint32_t bytes = uint32_t(d.verts.size());
        if (bytes == 0 || d.stride != 28) {
          return;
        }
        if (ui_offset + bytes > RendererState::kUiRegionSize) {
          g_draws_2d_dropped.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        std::memcpy(g_r.ui_ring_cpu + ui_region + ui_offset, d.verts.data(), bytes);
        nrhi::TextureView* srv_view;
        if (yuv != nullptr) {
          srv_view = yuv[0];
        } else {
          const GuestTexture* t = resolve_2d_texture(d.fetch);
          if (t == nullptr) {
            // Big-art decode in flight on the workers (large-art async
            // routing); skip the quad; it lands 1-3 frames later.
            return;
          }
          srv_view = t->srv;
        }
        float constants[40];
        std::memcpy(constants, d.consts, sizeof(d.consts));
        // Wide output: scale the staged projection's clip.x row (the VS
        // computes clip.x = dot(wp, c0)) so the 16:9-authored stream lands
        // centered (see wide_2d_scale).
        if (wide_2d_scale != 1.0f) {
          constants[0] *= wide_2d_scale;
          constants[1] *= wide_2d_scale;
          constants[2] *= wide_2d_scale;
          constants[3] *= wide_2d_scale;
        }
        // 2D ortho draws have no translation row in the projection (c3 ==
        // (0,0,0,1)); perspective view-proj rows do. Half-pixel applies to
        // the former only.
        const bool ortho = d.consts[12] == 0.0f && d.consts[13] == 0.0f &&
                           d.consts[14] == 0.0f && d.consts[15] == 1.0f;
        constants[36] = ortho ? 1.0f : 0.0f;
        // m[9].y: sharp-magnification amount for APT cached-bitmap tiles
        // (see the cvar; the shader gates on actual fetch magnification).
        constants[37] = float(std::clamp(
            REXCVAR_GET(skate3_native_render_scene_2d_sharp), 0.0, 2.0));
        // m[9].z: clip-space X extent of the 2D band, the edge-snap
        // boundary in the VS (0 = full target). m[9].w: unused. The D3D9
        // half-pixel shift is derived in the VS from the draw's own ortho
        // scale (+0.5 GUEST pixel down-right, matching the emulated path's
        // half_pixel_offset reversal); the old up-left OUTPUT-pixel nudge
        // staged here left a see-through sliver along the bottom/right of
        // edge-to-edge loading quads.
        constants[38] = wide_2d_scale != 1.0f ? wide_2d_scale : 0.0f;
        constants[39] = 0.0f;
        cmd->SetRootConstants(0, 40, constants, 0);
        cmd->SetTexture(1, srv_view);
        if (yuv != nullptr) {
          cmd->SetPipeline(g_r.pso_yuv2d);
          cmd->SetTexture(2, yuv[1]);
          cmd->SetTexture(5, yuv[2]);
        }
        cmd->SetVertexBuffer(g_r.ui_ring, ui_region + ui_offset, bytes,
                             d.stride);
        cmd->SetPrimitiveTopology(d.prim == 5
                                      ? nrhi::PrimitiveTopology::kTriangleStrip
                                      : nrhi::PrimitiveTopology::kTriangleList);
        cmd->Draw(d.count, 0);
        if (yuv != nullptr) {
          cmd->SetPipeline(g_r.pso_2d);
        }
        ui_offset += bytes;
        ++drawn_2d;
      };

      cmd->SetRenderTargets(context.guest_output, nullptr);
      cmd->SetViewport(viewport);
      cmd->SetScissor(scissor);
      // Native FMV substitution, self-contained: a video quad's console
      // shader binds Y at the DRAW's fetch slot 0 and the U/V planes at
      // slots 1/2: three valid distinct textures with the chroma at
      // exactly half the luma dimensions is a video draw, regardless of
      // which UI path drew it (the camera-page previews are plain APT
      // elements; matching against the VideoRenderer-published planes was
      // refuted twice; the UI paths sample APT-side plane COPIES, e.g.
      // 0x1F6xxxxx vs the published 0xA59xxxxx). Resolved
      // triples are cached per frame by the Y address, and their store
      // entries are forced to a per-frame liveness probe (a 30 fps video
      // rewriting its planes every few RENDERED frames would otherwise
      // settle the probe to 16-frame sampling, the "sluggish video").
      const auto yuv_triple = [](const Draw2d& d) -> bool {
        const uint32_t* s0 = d.fetch;
        const uint32_t* s1 = d.fetch + 6;
        const uint32_t* s2 = d.fetch + 12;
        if ((s0[0] & 3u) != 2 || (s1[0] & 3u) != 2 || (s2[0] & 3u) != 2 ||
            s0[1] == 0 || s1[1] == 0 || s2[1] == 0 || s1[1] == s0[1] ||
            s2[1] == s0[1] || s1[1] == s2[1]) {
          return false;
        }
        const uint32_t w0 = (s0[2] & 0x1FFFu) + 1, h0 = ((s0[2] >> 13) & 0x1FFFu) + 1;
        const uint32_t w1 = (s1[2] & 0x1FFFu) + 1, h1 = ((s1[2] >> 13) & 0x1FFFu) + 1;
        const uint32_t w2 = (s2[2] & 0x1FFFu) + 1, h2 = ((s2[2] >> 13) & 0x1FFFu) + 1;
        const auto half = [](uint32_t full, uint32_t c) {
          return c == full / 2 || c == (full + 1) / 2;
        };
        return w0 >= 32 && h0 >= 32 && half(w0, w1) && half(h0, h1) &&
               w2 == w1 && h2 == h1;
      };
      struct TripleCacheEntry {
        uint32_t y_addr;
        bool ok;
        nrhi::TextureView* slots[3];
      };
      TripleCacheEntry triple_cache[kMaxMovies];
      int triple_count = 0;
      // Backup: the freshest VideoRenderer-published plane set, for a
      // bracketed movie quad without a readable triple (the boot intro
      // rendered through this before the triple detection existed).
      // Resolved lazily; the capped log tracks whether it is still ever
      // needed; if it stays silent across sessions, this path and the
      // OnMovieFrame publish machinery behind it can be retired.
      nrhi::TextureView* fallback_slots[3] = {};
      int fallback_state = 0;  // 0 = unresolved, 1 = ok, -1 = unavailable
      const auto fallback_yuv = [&]() -> nrhi::TextureView* const* {
        if (fallback_state == 0) {
          fallback_state = -1;
          const MoviePlanes* best = nullptr;
          for (const MoviePlanes& m : movies) {
            if (m.ns >= 0 && movie_now_ns - m.ns < 500'000'000 &&
                (best == nullptr || m.ns > best->ns)) {
              best = &m;
            }
          }
          if (best != nullptr) {
            bool ok = true;
            for (int p = 0; p < 3 && ok; ++p) {
              auto hot = g_r.tex_store.find(FetchWordsKey(best->words[p]));
              if (hot != g_r.tex_store.end()) {
                hot->second.recheck_frame = 0;
              }
              const GuestTexture* t =
                  resolve_2d_texture(best->words[p], /*force_inline=*/true);
              ok = t != &g_r.white && t->texture != nullptr;
              fallback_slots[p] = t->srv;
            }
            if (ok) {
              fallback_state = 1;
            }
          }
        }
        return fallback_state == 1 ? fallback_slots : nullptr;
      };
      bool movie_drawn = false;
      for (const Draw2d& d : scene_2d) {
        nrhi::TextureView* const* yuv = nullptr;
        // Video-quad detection runs regardless of movie_sub: a quad whose
        // own fetch slots form a YUV triple must NEVER draw through ps_main
        // - that renders the raw Y plane (greyscale luma, or the PREVIOUS
        // video's frame while the plane copies are still stale), the
        // video-boundary flash class. If the planes can't resolve yet
        // (async decode in flight, substitution off) the quad is SKIPPED;
        // black under a starting video is what the real thing looks like.
        bool video_quad = false;
        if (yuv_triple(d)) {
          video_quad = true;
          if (movie_sub) {
            TripleCacheEntry* e = nullptr;
            for (int t = 0; t < triple_count && e == nullptr; ++t) {
              if (triple_cache[t].y_addr == d.fetch[1]) {
                e = &triple_cache[t];
              }
            }
            if (e == nullptr && triple_count < kMaxMovies) {
              e = &triple_cache[triple_count++];
              e->y_addr = d.fetch[1];
              e->ok = true;
              bool decode_failed = false;
              for (int p = 0; p < 3 && e->ok; ++p) {
                const uint32_t* w = d.fetch + p * 6;
                // Plain find (find_words_texture would probe + enqueue its
                // own ui=false heal first): force the per-frame probe, the
                // resolve inline-decodes any content change (video=true).
                auto hot = g_r.tex_store.find(FetchWordsKey(w));
                if (hot != g_r.tex_store.end()) {
                  hot->second.recheck_frame = 0;  // content-hot: per-frame probe
                }
                const GuestTexture* t =
                    resolve_2d_texture(w, /*force_inline=*/false, /*video=*/true);
                // Session gate: only content decoded during THIS movie
                // session serves; at a video boundary both the store and
                // guest memory can still hold the PREVIOUS video's last
                // frame (the plane copies keep their addresses); the quad
                // holds black until the new video's first frame lands.
                e->ok = t != nullptr && t != &g_r.white && t->texture != nullptr &&
                        t->last_change_frame >= s_movie_session_frame;
                decode_failed |= t == &g_r.white;
                e->slots[p] = e->ok ? t->srv : nullptr;
              }
              if (decode_failed) {
                static std::atomic<uint32_t> s_triple_failed{0};
                if (s_triple_failed.fetch_add(1, std::memory_order_relaxed) < 8) {
                  REXLOG_INFO(
                      "native-scene: FMV plane triple resolve FAILED "
                      "(y={:08X})",
                      d.fetch[1]);
                }
              }
            }
            if (e != nullptr && e->ok) {
              yuv = e->slots;
            }
          }
        }
        if (yuv == nullptr && movie_sub && (d.flags & 0x2u) != 0 &&
            d.src_stride == 24) {
          // Bracketed movie quad without a readable triple: through ps_main
          // it draws its c8 opaque-black cover, also a video quad.
          video_quad = true;
          yuv = fallback_yuv();
          if (yuv != nullptr) {
            static std::atomic<uint32_t> s_fb_logged{0};
            if (s_fb_logged.fetch_add(1, std::memory_order_relaxed) < 4) {
              REXLOG_INFO(
                  "native-scene: FMV bracket fallback served a quad (no "
                  "readable YUV triple on it)");
            }
          }
        }
        if (video_quad) {
          // Consulted by YieldForMovie: a video is only "on screen" while
          // the 2D replay actually carries a video quad. A skipped/dismissed
          // video leaves the decoder tail beating with no quad anywhere -
          // yielding then flashes the (suppressed, stale) emulated buffer
          // for nothing.
          g_movie_quad_last_ns.store(movie_now_ns, std::memory_order_relaxed);
        }
        if (video_quad && yuv == nullptr) {
          static std::atomic<uint32_t> s_vskip{0};
          const uint32_t n = s_vskip.fetch_add(1, std::memory_order_relaxed);
          if (n < 8 || (n & 511u) == 0) {
            REXLOG_INFO(
                "native-scene: video quad skipped (planes not ready, "
                "y={:08X}, movie_sub={}) (n={})",
                d.fetch[1], movie_sub ? 1 : 0, n);
          }
          continue;
        }
        emit_draw(d, yuv);
        movie_drawn |= yuv != nullptr;
      }
      if (movie_drawn) {
        g_movie_native_last_ns.store(movie_now_ns, std::memory_order_relaxed);
      }
      if (movie_drawn) {
        static std::atomic<bool> s_movie_logged{false};
        if (!s_movie_logged.exchange(true, std::memory_order_relaxed)) {
          REXLOG_INFO(
              "native-scene: FMV rendering NATIVELY (movie-quad YUV "
              "substitution)");
        }
      }
    }
  }

  // Settings-menu backdrop blur over everything drawn this frame (scene +
  // HUD/2D + FMV). Shared with the emulated-output post-processor path.
  // Host state read directly at RENDER time - routing it through the
  // published FrameScene made the blur depend on fresh guest publishes,
  // which the startup flow doesn't reliably produce (stale scenes rendered
  // with a frozen sigma of 0).
  const float menu_blur_target = g_settings_menu_blur.load(std::memory_order_relaxed)
                                     ? float(REXCVAR_GET(skate3_menu_blur_sigma))
                                     : 0.0f;
  ApplyMenuBlurPass(context, cmd, menu_blur_target,
                    /*output_in_guest_output_state=*/false);

  cmd->Barrier(context.guest_output, nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kGuestOutput);
  cmd->FlushBarriers();
  cmd->ProfileRegion(nrhi::ProfileStage::kTail);
  skate3::mechanics_sandbox::RecordRenderStage(
      skate3::mechanics_sandbox::RenderStage::Presented);

  g_pw_2d.Add(perf_ns_since(twod_t0));
  g_pw_tail.Add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    twod_t0 - mid_t0)
                    .count()));
  g_pw_render.Add(perf_ns_since(render_t0));
  const uint64_t frames = g_frames_rendered.fetch_add(1) + 1;
  MaybeDumpSceneRing();
  LogFrameStats(scene, frames, drawn, drawn_2d, drawn_spline, shadow_ready,
                shadow_draws);
  return true;
}

}  // namespace

bool SceneFailed() { return g_r.failed; }

void ResetSceneFailure() {
  if (g_r.failed) {
    g_r.failed = false;
    REXLOG_INFO(
        "native-scene: sticky pipeline failure cleared; the next frame "
        "retries the full pipeline build");
  }
}

void Install() {
  // Registered even when the scene cvar starts off: RenderScene yields to the
  // emulated output while disabled, and the runtime toggle (F5) can flip the
  // cvar live at any point after boot.
  rex::graphics::SetNativeGuestOutputRenderer(&RenderScene, nullptr);
  // Settings-menu backdrop blur over frames the EMULATED path produced
  // (boot/startup before the native scene takes over, manual emulated
  // mode); native-rendered frames blur inline in RenderScene.
  rex::graphics::SetNativeGuestOutputPostProcessor(&PostProcessGuestOutput, nullptr);
  REXLOG_INFO("native-scene: guest output renderer registered (scene {})",
              SceneEnabled() ? "on" : "off");
}

}  // namespace skate3::native_scene

#else  // !(REX_HAS_D3D12 || REX_HAS_VULKAN)

namespace skate3::native_scene {
void Install() {}
bool SceneFailed() { return false; }
void ResetSceneFailure() {}
}  // namespace skate3::native_scene

#endif  // REX_HAS_D3D12 || REX_HAS_VULKAN
