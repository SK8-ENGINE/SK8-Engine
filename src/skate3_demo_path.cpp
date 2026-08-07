#include "skate3_demo_path.h"

#include "generated/skate3_init.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/kernel/guest_presence.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xam/input_injection.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/function_dispatcher.h>

#if defined(_WIN32)
#include <Windows.h>
#endif

REXCVAR_DEFINE_BOOL(skate3_demo_path, false, "Skate 3",
                    "Probe and automate the boot path to gameplay");
REXCVAR_DEFINE_BOOL(skate3_direct_boot, false, "Skate 3",
                    "Source-level boot path that bypasses frontend states and "
                    "loads the signed-in save directly");
REXCVAR_DEFINE_BOOL(skate3_demo_path_probe, false, "Skate 3",
                    "Log Skate 3 boot/frontend states used by the demo path");
REXCVAR_DEFINE_BOOL(skate3_demo_path_signed_in, false, "Skate 3",
                    "Demo path: keep the real signed-in profile (and its save) instead of "
                    "forcing a signed-out boot");
REXCVAR_DEFINE_STRING(skate3_demo_path_gameplay_inputs, "", "Skate 3",
                      "Demo path: comma-separated pad inputs injected once after gameplay "
                      "settles (tokens: a b x y start back lb rb lt rt up down left right l3 "
                      "r3; a ':ms' suffix overrides the delay AFTER that input, e.g. rt:1500). "
                      "Map switch to PCU Library: "
                      "start,a,rt:1500,rt:1500,down,down,a,down,down,down,down,down,a,a");
REXCVAR_DEFINE_INT32(skate3_demo_path_input_settle_ms, 2500, "Skate 3",
                     "Demo path: wait this long after gameplay is reached before injecting "
                     "skate3_demo_path_gameplay_inputs")
    .range(0, 60000);
REXCVAR_DEFINE_INT32(skate3_demo_path_input_delay_ms, 600, "Skate 3",
                     "Demo path: delay between injected gameplay inputs")
    .range(50, 10000);
REXCVAR_DEFINE_BOOL(skate3_intro_movie_skip, true, "Skate 3",
                    "Skip the frontend intro movie when A or Start is pressed "
                    "(the default keyboard bindings make that Space and Enter)");

namespace skate3::demo_path {
namespace {

constexpr uint32_t kFrontEndStatePressStart = 24;
constexpr uint32_t kFrontEndStateLanguageSelect = 47;
// Signed byte consumed by Skate 3's profile helpers as the active XAM user
// index. The normal Press Start path fills it from the controller that
// accepted the title screen; direct boot has no physical accept event, so
// explicitly bind the already-created local profile in XAM slot 0.
constexpr uint32_t kActiveUserIndex = 0x82FC8851;

std::atomic<uint32_t> g_last_requested_state{0};
std::atomic<uint32_t> g_last_frontend_manager{0};
std::atomic<uint32_t> g_press_start_boot_flow{0};
std::atomic<bool> g_seen_language_update{false};
std::atomic<bool> g_direct_language_confirmed{false};
std::atomic<bool> g_direct_press_start_confirmed{false};
std::atomic<bool> g_direct_autosave_confirmed{false};
std::atomic<bool> g_logged_language_vtable{false};
std::atomic<bool> g_logged_press_start_vtable{false};
std::atomic<uint32_t> g_last_language_select_event{std::numeric_limits<uint32_t>::max()};
std::atomic<uint32_t> g_last_press_start_event{std::numeric_limits<uint32_t>::max()};
std::atomic<uint32_t> g_last_post_press_start_event{std::numeric_limits<uint32_t>::max()};
std::atomic<uint32_t> g_automation_stage{0};
std::atomic<bool> g_skip_intro_movie{false};
std::atomic<bool> g_logged_intro_movie_skip{false};
std::atomic<bool> g_f10_poll_was_down{false};

bool ProbeEnabled() {
  return rex::cvar::Query<bool>("skate3_demo_path") ||
         rex::cvar::Query<bool>("skate3_demo_path_probe") ||
         rex::cvar::Query<bool>("skate3_direct_boot");
}

bool AutomationEnabled() {
  return rex::cvar::Query<bool>("skate3_demo_path");
}

bool DirectBootEnabled() {
  return rex::cvar::Query<bool>("skate3_direct_boot");
}

const char* KnownFrontEndStateName(uint32_t state_id) {
  switch (state_id) {
    case kFrontEndStatePressStart:
      return "press-start";
    case kFrontEndStateLanguageSelect:
      return "language-select";
    default:
      return "unknown";
  }
}

void LogBootFlowEventChange(std::atomic<uint32_t>& last_event, const char* name, uint32_t event,
                            uint32_t state_this) {
  if (!ProbeEnabled()) {
    return;
  }

  uint32_t previous = last_event.load(std::memory_order_relaxed);
  while (previous != event) {
    if (last_event.compare_exchange_weak(previous, event, std::memory_order_relaxed)) {
      REXLOG_INFO("Skate 3 demo path: {} event={} this=0x{:08X}", name, event, state_this);
      return;
    }
  }
}

void QueueLanguageAcceptIfNeeded() {
  if (!AutomationEnabled()) {
    return;
  }

  uint32_t expected = 0;
  if (!g_automation_stage.compare_exchange_strong(expected, 1, std::memory_order_relaxed)) {
    return;
  }

  rex::kernel::xam::QueueSyntheticInput(rex::input::X_INPUT_GAMEPAD_A, 8);
  REXLOG_INFO("Skate 3 demo path: queued language-select A pulse");
}

void EnableTitleStartAutoTapIfNeeded(const char* reason) {
  if (!AutomationEnabled()) {
    return;
  }

  uint32_t stage = g_automation_stage.load(std::memory_order_relaxed);
  while (stage < 2) {
    if (g_automation_stage.compare_exchange_weak(stage, 2, std::memory_order_relaxed)) {
      rex::kernel::xam::SetSyntheticAutoTap(rex::input::X_INPUT_GAMEPAD_START, true);
      REXLOG_INFO("Skate 3 demo path: enabled press-start auto-tap ({})", reason);
      return;
    }
  }
}

void EnableIntroMovieSkipIfNeeded() {
  if (!AutomationEnabled()) {
    return;
  }

  bool expected = false;
  if (g_skip_intro_movie.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
    REXLOG_INFO("Skate 3 demo path: enabled intro movie completion override");
  }
}

void PollF10Marker() {
#if defined(_WIN32)
  if (!ProbeEnabled()) {
    return;
  }

  const bool f10_down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
  const bool was_down = g_f10_poll_was_down.exchange(f10_down, std::memory_order_relaxed);
  if (f10_down && !was_down) {
    REXLOG_WARN("Skate 3 demo path: polled F10 milestone marker");
  }
#endif
}

void LogFrontEndStateVtableOnce(uint8_t* base, const char* label,
                                uint32_t state_object,
                                std::atomic<bool>& logged) {
  if (!ProbeEnabled() || !state_object) {
    return;
  }
  bool expected = false;
  if (!logged.compare_exchange_strong(expected, true,
                                      std::memory_order_relaxed)) {
    return;
  }

  const uint32_t vtable = REX_LOAD_U32(state_object);
  std::ostringstream entries;
  for (uint32_t offset = 0; offset <= 80; offset += 4) {
    if (offset) {
      entries << ',';
    }
    entries << std::hex << std::uppercase << offset << '='
            << REX_LOAD_U32(vtable + offset);
  }
  REXLOG_INFO(
      "Skate 3 demo path: {} object=0x{:08X} vtable=0x{:08X} slots {}",
      label, state_object, vtable, entries.str());
}

extern "C" REX_FUNC(Skate3DemoPath_SetFrontEndStateHook) {
  const uint32_t manager = ctx.r3.u32;
  const uint32_t state_id = ctx.r4.u32;
  const uint32_t mode = ctx.r5.u32;
  const uint32_t caller_lr = ctx.lr;

  g_last_frontend_manager.store(manager, std::memory_order_relaxed);
  ObserveFrontEndState(manager, state_id, mode, caller_lr);

  sub_82D0AFA0(ctx, base);
}

extern "C" REX_FUNC(Skate3DemoPath_LanguageSelectStateHook) {
  PollF10Marker();
  LogBootFlowEventChange(g_last_language_select_event, "BootFlow LanguageSelectState",
                         ctx.r4.u32, ctx.r3.u32);
  if (ctx.r4.u32 == 4) {
    EnableIntroMovieSkipIfNeeded();
  }

  sub_826FDD70(ctx, base);
}

extern "C" REX_FUNC(Skate3DemoPath_ShowPressStartModeHook) {
  PollF10Marker();
  const uint32_t event = ctx.r4.u32;
  const uint32_t boot_flow = ctx.r3.u32;
  LogBootFlowEventChange(g_last_press_start_event, "BootFlow ShowPressStartMode", event,
                         boot_flow);
  if (event == 1) {
    g_press_start_boot_flow.store(boot_flow, std::memory_order_relaxed);
    g_skip_intro_movie.store(false, std::memory_order_relaxed);
    EnableTitleStartAutoTapIfNeeded("press-start state");
  }

  sub_826FE1D8(ctx, base);

  // ShowPressStart event 4 tears down the title-screen controller binding.
  // With no physical Start press, the native teardown copies its sentinel
  // controller index (0xFF) back over the slot selected above. DLC discovery
  // runs immediately afterward and treats 0xFF as "no signed-in user", so it
  // exits before creating the Marketplace enumerator. Keep the local profile
  // bound to XAM slot 0 for the remainder of direct boot.
  if (DirectBootEnabled() &&
      g_direct_press_start_confirmed.load(std::memory_order_relaxed) &&
      REX_LOAD_U8(kActiveUserIndex) != 0) {
    const uint8_t overwritten_user = REX_LOAD_U8(kActiveUserIndex);
    REX_STORE_U8(kActiveUserIndex, 0);
    REXLOG_INFO(
        "Skate 3 direct boot: restored active user 0 after ShowPressStart "
        "event={} overwrote it with 0x{:02X}",
        event, overwritten_user);
  }

  if (DirectBootEnabled() && event == 1 &&
      g_direct_press_start_confirmed.load(std::memory_order_relaxed) &&
      REX_LOAD_U8(boot_flow + 49) != 0 &&
      !g_direct_autosave_confirmed.load(std::memory_order_relaxed)) {
    bool expected = false;
    if (g_direct_autosave_confirmed.compare_exchange_strong(
            expected, true, std::memory_order_relaxed)) {
      // Warning type 23 is now registered, but no renderer has seen it. Remove
      // it through the game's UI manager so the ordinary asynchronous
      // modal-closed event is preserved, then run its native Continue callback.
      PPCContext close_ctx = ctx;
      close_ctx.r3.u64 = REX_LOAD_U32(0x830CFE1C);
      close_ctx.r4.u64 = REX_LOAD_U32(boot_flow + 52);
      sub_82D0A628(close_ctx, base);

      PPCContext continue_ctx = ctx;
      continue_ctx.r5.u64 = boot_flow;
      sub_826FE638(continue_ctx, base);
      REXLOG_INFO(
          "Skate 3 direct boot: removed the autosave notice through the "
          "native UI manager before rendering and invoked Continue");
    }
  }
}

extern "C" REX_FUNC(Skate3DemoPath_PressStartUpdateHook) {
  PollF10Marker();
  LogFrontEndStateVtableOnce(base, "FrontEndState_PressStart", ctx.r3.u32,
                             g_logged_press_start_vtable);

  if (!DirectBootEnabled()) {
    sub_82609050(ctx, base);
    return;
  }

  // Run the native update until it opens its own acceptance gate. Besides the
  // original 1500 ms title delay, this gives the profile service time to bind
  // user 0. Accepting on the first frame enters an unsigned session.
  const uint32_t press_start_state = ctx.r3.u32;
  PPCContext update_ctx = ctx;
  sub_82609050(update_ctx, base);

  if (g_direct_press_start_confirmed.load(std::memory_order_relaxed) ||
      REX_LOAD_U8(press_start_state + 72) == 0) {
    ctx.r3.u64 = 0;
    return;
  }

  bool expected = false;
  if (!g_direct_press_start_confirmed.compare_exchange_strong(
          expected, true, std::memory_order_relaxed)) {
    ctx.r3.u64 = 0;
    return;
  }

  REX_STORE_U8(kActiveUserIndex, 0);
  // Re-assert the host profile state here, after the runtime and XAM profile
  // service both exist. Early path/profile setup can precede command-line
  // direct-boot flag application, but this transition is immediately before
  // Skate 3 queries its own profile helper to construct the post-title task.
  rex::cvar::SetFlagByName("user_profile_signed_in", "true");
  rex::cvar::SetFlagByName("user_live_signed_in", "false");

  // ShowPressStart's normal event-1 path performs this refresh immediately
  // after copying the accepting controller's user index. It ran earlier with
  // "no accepting controller" in direct boot, so repeat it after binding slot
  // 0 to update the game's cached sign-in state and notify its profile clients.
  PPCContext profile_refresh_ctx = ctx;
  sub_8263E810(profile_refresh_ctx, base);

  PPCContext profile_check_ctx = ctx;
  profile_check_ctx.r3.u64 = 0;
  sub_8263E788(profile_check_ctx, base);
  REXLOG_INFO(
      "Skate 3 direct boot: bound active user={} profile helper signed_in={}",
      REX_LOAD_U8(kActiveUserIndex), profile_check_ctx.r3.u32 & 0xFF);

  // The update can accept automatically once the signed-in profile reports
  // ready. Otherwise invoke the same arm/accept events produced by Start.
  if (REX_LOAD_U8(press_start_state + 60) == 0) {
    PPCContext accept_ctx = ctx;
    accept_ctx.r3.u64 = press_start_state;
    accept_ctx.r4.u64 = 0;
    accept_ctx.r5.u64 = 0;
    sub_82609238(accept_ctx, base);
    accept_ctx.r3.u64 = press_start_state;
    accept_ctx.r4.u64 = 0;
    accept_ctx.r5.u64 = 44;
    sub_82609238(accept_ctx, base);
  }

  g_automation_stage.store(2, std::memory_order_relaxed);
  REXLOG_INFO(
      "Skate 3 direct boot: profile-ready PressStart accepted without "
      "controller input");
  ctx.r3.u64 = 0;
}

extern "C" REX_FUNC(Skate3DemoPath_PostPressStartStateHook) {
  const uint32_t boot_flow =
      g_press_start_boot_flow.load(std::memory_order_relaxed);
  if (ctx.r3.u32 == boot_flow) {
    LogBootFlowEventChange(g_last_post_press_start_event,
                           "BootFlow PostPressStartState", ctx.r4.u32,
                           ctx.r3.u32);
    if (DirectBootEnabled() && ctx.r4.u32 == 1 &&
        g_direct_autosave_confirmed.load(std::memory_order_relaxed) &&
        rex::kernel::guest_presence::GameplayContextValue() == 0 &&
        REX_LOAD_U8(boot_flow + 33) == 0) {
      // The UI manager has established the loading context. Since direct boot
      // removed the notice before rendering, there is no close animation to
      // emit message 38; apply that message's sole state effect now.
      REX_STORE_U8(boot_flow + 33, 1);
      REXLOG_INFO(
          "Skate 3 direct boot: completed the eliminated notice's close "
          "lifecycle after the native loading context became active");
    }
  }
  sub_826FE4A8(ctx, base);
}

extern "C" REX_FUNC(Skate3DemoPath_SetBootFlowStateHook) {
  const uint32_t boot_flow =
      g_press_start_boot_flow.load(std::memory_order_relaxed);
  if (ProbeEnabled() && ctx.r3.u32 == boot_flow) {
    REXLOG_INFO(
        "Skate 3 demo path: BootFlow SetState function=0x{:08X} arg={} "
        "mode={} fields[8]=0x{:08X} [20]=0x{:08X} [24]=0x{:08X} "
        "[28]=0x{:08X} flags[33]={} [40]={} [49]={} [50]={}",
        ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
        REX_LOAD_U32(boot_flow + 8), REX_LOAD_U32(boot_flow + 20),
        REX_LOAD_U32(boot_flow + 24), REX_LOAD_U32(boot_flow + 28),
        REX_LOAD_U8(boot_flow + 33), REX_LOAD_U8(boot_flow + 40),
        REX_LOAD_U8(boot_flow + 49), REX_LOAD_U8(boot_flow + 50));
  }
  sub_826DFB30(ctx, base);
}

extern "C" REX_FUNC(Skate3DemoPath_CompleteBootFlowStateHook) {
  const uint32_t boot_flow =
      g_press_start_boot_flow.load(std::memory_order_relaxed);
  if (ProbeEnabled() && ctx.r3.u32 == boot_flow) {
    REXLOG_INFO(
        "Skate 3 demo path: BootFlow CompleteState current=0x{:08X} "
        "fields[20]=0x{:08X} [24]=0x{:08X} [28]=0x{:08X} "
        "flags[33]={} [40]={} [49]={} [50]={}",
        REX_LOAD_U32(boot_flow + 8), REX_LOAD_U32(boot_flow + 20),
        REX_LOAD_U32(boot_flow + 24), REX_LOAD_U32(boot_flow + 28),
        REX_LOAD_U8(boot_flow + 33), REX_LOAD_U8(boot_flow + 40),
        REX_LOAD_U8(boot_flow + 49), REX_LOAD_U8(boot_flow + 50));
  }
  sub_826DF2C8(ctx, base);
}

extern "C" REX_FUNC(Skate3DemoPath_LanguageSelectUpdateHook) {
  PollF10Marker();
  LogFrontEndStateVtableOnce(base, "FrontEndState_LanguageSelect", ctx.r3.u32,
                             g_logged_language_vtable);
  if (ProbeEnabled()) {
    bool expected = false;
    if (g_seen_language_update.compare_exchange_strong(expected, true,
                                                       std::memory_order_relaxed)) {
      REXLOG_INFO(
          "Skate 3 demo path: FrontEndState_LanguageSelect::OnUpdate first seen "
          "dt={} this=0x{:08X}",
          ctx.r4.u32, ctx.r3.u32);
      QueueLanguageAcceptIfNeeded();
    }
  }

  if (DirectBootEnabled()) {
    bool expected = false;
    if (g_direct_language_confirmed.compare_exchange_strong(
            expected, true, std::memory_order_relaxed)) {
      // FrontEndState_LanguageSelect::OnEvent(event=2) is the game's own
      // confirm-language transition. Invoke it directly before the language
      // screen gets an update/render cycle; no controller state is forged.
      PPCContext confirm_ctx = ctx;
      confirm_ctx.r4.u64 = 0;
      confirm_ctx.r5.u64 = 2;
      sub_82639440(confirm_ctx, base);
      g_automation_stage.store(1, std::memory_order_relaxed);
      REXLOG_INFO(
          "Skate 3 direct boot: invoked language confirmation event directly");
      ctx.r3.u64 = 0;
      return;
    }
  }

  sub_82639400(ctx, base);
}

// Scripted post-gameplay input sequence (e.g. driving the pause-menu map
// switch that reproduces the sticky-slow-GPU state). Runs on its own thread
// with real-time pacing: waits for the gameplay presence context, lets the
// scene settle, then injects one pad input at a time.
struct GameplayInputToken {
  const char* name;
  uint16_t buttons;
  uint8_t left_trigger;
  uint8_t right_trigger;
};

constexpr GameplayInputToken kGameplayInputTokens[] = {
    {"a", rex::input::X_INPUT_GAMEPAD_A, 0, 0},
    {"b", rex::input::X_INPUT_GAMEPAD_B, 0, 0},
    {"x", rex::input::X_INPUT_GAMEPAD_X, 0, 0},
    {"y", rex::input::X_INPUT_GAMEPAD_Y, 0, 0},
    {"start", rex::input::X_INPUT_GAMEPAD_START, 0, 0},
    {"back", rex::input::X_INPUT_GAMEPAD_BACK, 0, 0},
    {"lb", rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER, 0, 0},
    {"rb", rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0},
    {"lt", 0, 255, 0},
    {"rt", 0, 0, 255},
    {"up", rex::input::X_INPUT_GAMEPAD_DPAD_UP, 0, 0},
    {"down", rex::input::X_INPUT_GAMEPAD_DPAD_DOWN, 0, 0},
    {"left", rex::input::X_INPUT_GAMEPAD_DPAD_LEFT, 0, 0},
    {"right", rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT, 0, 0},
    {"l3", rex::input::X_INPUT_GAMEPAD_LEFT_THUMB, 0, 0},
    {"r3", rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB, 0, 0},
};

const GameplayInputToken* FindGameplayInputToken(const std::string& name) {
  for (const GameplayInputToken& token : kGameplayInputTokens) {
    if (name == token.name) {
      return &token;
    }
  }
  return nullptr;
}

// Set once during app setup (before the guest boots), read from the guest
// thread by the movie-skip poll.
std::function<rex::input::InputSystem*()> g_ui_input_provider;

// User-facing movie skip: a fresh A / Start press while a frontend movie is
// updating completes it (the generated FEMoviePlayer::Update patch consults
// ShouldForceIntroMovieComplete every movie tick, demo path or not). The
// merged raw pad state is polled so the default keyboard bindings (Space = A,
// Enter = Start) work identically on every backend; rising-edge only, so a
// button held since before the movie doesn't blow straight through it.
bool UserRequestedMovieSkip() {
  static std::atomic<bool> s_was_down{true};
  if (!REXCVAR_GET(skate3_intro_movie_skip) || !g_ui_input_provider) {
    return false;
  }
  rex::input::InputSystem* input = g_ui_input_provider();
  if (input == nullptr) {
    return false;
  }
  constexpr uint16_t kSkipButtons =
      rex::input::X_INPUT_GAMEPAD_A | rex::input::X_INPUT_GAMEPAD_START;
  rex::input::X_INPUT_GAMEPAD pad;
  const bool down =
      input->GetUiGamepadState(&pad) && (pad.buttons & kSkipButtons) != 0;
  const bool was_down = s_was_down.exchange(down, std::memory_order_relaxed);
  if (down && !was_down) {
    REXLOG_INFO("Skate 3: frontend movie skipped by user input");
    return true;
  }
  return false;
}

std::atomic<bool> g_input_worker_quit{false};
std::thread g_input_worker;

void JoinGameplayInputWorker() {
  g_input_worker_quit.store(true, std::memory_order_relaxed);
  if (g_input_worker.joinable()) {
    g_input_worker.join();
  }
}

bool InterruptibleSleepMs(int64_t total_ms) {
  constexpr int64_t kSliceMs = 100;
  while (total_ms > 0) {
    if (g_input_worker_quit.load(std::memory_order_relaxed)) {
      return false;
    }
    int64_t slice = total_ms < kSliceMs ? total_ms : kSliceMs;
    std::this_thread::sleep_for(std::chrono::milliseconds(slice));
    total_ms -= slice;
  }
  return !g_input_worker_quit.load(std::memory_order_relaxed);
}

void StartGameplayInputWorkerIfNeeded() {
  if (!AutomationEnabled()) {
    return;
  }
  const std::string sequence = REXCVAR_GET(skate3_demo_path_gameplay_inputs);
  if (sequence.empty()) {
    return;
  }

  const int32_t settle_ms = REXCVAR_GET(skate3_demo_path_input_settle_ms);
  const int32_t default_delay_ms = REXCVAR_GET(skate3_demo_path_input_delay_ms);

  struct SequenceEntry {
    const GameplayInputToken* token;
    int32_t delay_after_ms;
  };
  std::vector<SequenceEntry> tokens;
  std::stringstream stream(sequence);
  std::string raw_token;
  while (std::getline(stream, raw_token, ',')) {
    // Allow spaces around tokens.
    size_t begin = raw_token.find_first_not_of(" \t");
    size_t end = raw_token.find_last_not_of(" \t");
    if (begin == std::string::npos) {
      continue;
    }
    std::string name = raw_token.substr(begin, end - begin + 1);
    // Optional ':ms' suffix overriding the delay after this input (slower UI
    // transitions - the map-menu RT tabs - need more time to register).
    int32_t delay_after_ms = default_delay_ms;
    if (size_t colon = name.find(':'); colon != std::string::npos) {
      delay_after_ms = std::atoi(name.c_str() + colon + 1);
      if (delay_after_ms <= 0) {
        REXLOG_WARN("Skate 3 demo path: bad delay in gameplay input token '{}' - sequence "
                    "disabled",
                    name);
        return;
      }
      name.resize(colon);
    }
    const GameplayInputToken* token = FindGameplayInputToken(name);
    if (!token) {
      REXLOG_WARN("Skate 3 demo path: unknown gameplay input token '{}' - sequence disabled",
                  name);
      return;
    }
    tokens.push_back({token, delay_after_ms});
  }
  if (tokens.empty()) {
    return;
  }
  g_input_worker = std::thread([tokens, settle_ms] {
    // Wait for the gameplay presence context (0x8001 == 1).
    while (rex::kernel::guest_presence::GameplayContextValue() != 1) {
      if (!InterruptibleSleepMs(100)) {
        return;
      }
    }
    REXLOG_INFO("Skate 3 demo path: gameplay reached; injecting {} inputs after {} ms settle",
                tokens.size(), settle_ms);
    if (!InterruptibleSleepMs(settle_ms)) {
      return;
    }
    for (size_t i = 0; i < tokens.size(); ++i) {
      const GameplayInputToken* token = tokens[i].token;
      // ~8 polls at the game's 60 Hz input tick = a ~130 ms press. Triggers
      // are analog and debounced more heavily by the game's menus - hold them
      // for a human-tap-length ~270 ms.
      const bool is_trigger = token->left_trigger != 0 || token->right_trigger != 0;
      rex::kernel::xam::QueueSyntheticInput(token->buttons, token->left_trigger,
                                            token->right_trigger, is_trigger ? 16 : 8);
      REXLOG_INFO("Skate 3 demo path: injected gameplay input {}/{} '{}' (delay {} ms)", i + 1,
                  tokens.size(), token->name, tokens[i].delay_after_ms);
      if (!InterruptibleSleepMs(tokens[i].delay_after_ms)) {
        return;
      }
    }
    REXLOG_INFO("Skate 3 demo path: gameplay input sequence complete");
  });
  std::atexit(JoinGameplayInputWorker);
}

}  // namespace

void InstallHooks(rex::runtime::FunctionDispatcher* dispatcher) {
  if (!dispatcher || !ProbeEnabled()) {
    return;
  }

  dispatcher->SetFunction(0x82D0AFA0, &Skate3DemoPath_SetFrontEndStateHook);
  dispatcher->SetFunction(0x826FDD70, &Skate3DemoPath_LanguageSelectStateHook);
  dispatcher->SetFunction(0x826FE1D8, &Skate3DemoPath_ShowPressStartModeHook);
  dispatcher->SetFunction(0x826FE4A8, &Skate3DemoPath_PostPressStartStateHook);
  dispatcher->SetFunction(0x82609050, &Skate3DemoPath_PressStartUpdateHook);
  dispatcher->SetFunction(0x82639400, &Skate3DemoPath_LanguageSelectUpdateHook);
  dispatcher->SetFunction(0x826DFB30, &Skate3DemoPath_SetBootFlowStateHook);
  dispatcher->SetFunction(0x826DF2C8, &Skate3DemoPath_CompleteBootFlowStateHook);
  REXLOG_INFO("Skate 3 demo path: frontend probe hooks installed");
  StartGameplayInputWorkerIfNeeded();
}

rex::input::InputSystem* GetUiInputSystem() {
  return g_ui_input_provider ? g_ui_input_provider() : nullptr;
}

void SetUiInputProvider(std::function<rex::input::InputSystem*()> provider) {
  g_ui_input_provider = std::move(provider);
}

bool ShouldForceIntroMovieComplete() {
  // Agent boot mode never needs a frontend movie. Complete every movie as
  // soon as FEMoviePlayer::Update first sees it, including logos that occur
  // before the language state enables the ordinary demo-path skip.
  if (AutomationEnabled() || DirectBootEnabled()) {
    bool expected = false;
    if (g_logged_intro_movie_skip.compare_exchange_strong(expected, true,
                                                          std::memory_order_relaxed)) {
      REXLOG_INFO("Skate 3 demo path: forcing all frontend movies complete");
    }
    return true;
  }
  return UserRequestedMovieSkip();
}

bool DirectBootLoadingVisualActive() {
  return DirectBootEnabled() &&
         rex::kernel::guest_presence::GameplayContextValue() != 1;
}

void ObserveFrontEndState(uint32_t manager, uint32_t state_id,
                          uint32_t mode, uint32_t caller_lr) {
  g_last_requested_state.store(state_id, std::memory_order_relaxed);
  if (ProbeEnabled()) {
    REXLOG_INFO(
        "Skate 3 demo path: FE SetState state={} ({}) mode={} manager=0x{:08X} lr=0x{:08X}",
        state_id, KnownFrontEndStateName(state_id), mode, manager, caller_lr);
  }
}

uint32_t AutomationStage() {
  return g_automation_stage.load(std::memory_order_relaxed);
}

uint32_t LastRequestedFrontEndState() {
  return g_last_requested_state.load(std::memory_order_relaxed);
}

bool SeenLanguageUpdate() {
  return g_seen_language_update.load(std::memory_order_relaxed);
}

}  // namespace skate3::demo_path
