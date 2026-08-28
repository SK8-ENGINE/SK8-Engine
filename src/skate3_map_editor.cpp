#include "skate3_map_editor.h"

#include "skate3_drop_item_import.h"
#include "skate3_drop_item_library.h"
#include "skate3_mechanics_sandbox_map.h"
#include "skate3_native_collision.h"
#include "skate3_trick_pipeline.h"

#include <skate/world/map_editor.h>
#include <skate/world/skate_object_package.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#endif

REXCVAR_DECLARE(bool, skate3_native_render_scene);
REXCVAR_DECLARE(bool, skate3_native_render_scene_freecam);
REXCVAR_DECLARE(bool, skate3_native_render_scene_freecam_capture_input);

namespace skate3::map_editor {
namespace {

struct ObjectState {
  skate::world::Vec3 delta;
  skate::world::Vec3 x_axis{1.0f, 0.0f, 0.0f};
  skate::world::Vec3 y_axis{0.0f, 1.0f, 0.0f};
  skate::world::Vec3 z_axis{0.0f, 0.0f, 1.0f};
  std::uint64_t revision = 1;
};

std::atomic<bool> g_active{false};
std::atomic<std::size_t> g_selected{std::numeric_limits<std::size_t>::max()};
std::mutex g_mutex;
std::vector<ObjectState> g_objects;
std::vector<skate::world::SkateObjectAsset> g_spawn_assets;
std::vector<SpawnObjectEntry> g_spawn_entries;
std::vector<std::filesystem::path> g_spawn_paths;
std::filesystem::path g_game_data_root;
std::filesystem::path g_object_library_root;
bool g_spawn_assets_scanned = false;
std::atomic<std::size_t> g_spawn_asset_count{0};
std::atomic<bool> g_spawn_menu_visible{false};
std::atomic<std::size_t> g_pending_spawn{
    std::numeric_limits<std::size_t>::max()};
skate::world::Vec3 g_spawn_position;
bool g_spawn_position_valid = false;
std::atomic<void *> g_window{nullptr};
bool g_previous_scene = true;
bool g_previous_freecam = false;
bool g_previous_capture_input = true;
bool g_dragging = false;
std::uint64_t g_drag_request_id = 0;
bool g_left_was_down = false;
skate::world::EditorGizmoHandle g_drag_handle =
    skate::world::EditorGizmoHandle::None;
ObjectState g_drag_initial_state;
float g_drag_axis_parameter = 0.0f;
skate::world::Vec3 g_drag_rotation_vector;
skate::world::Vec3 g_drag_pivot;
std::atomic<bool> g_cursor_captured{false};

std::atomic<std::uint64_t> g_entries{0};
std::atomic<std::uint64_t> g_exits{0};
std::atomic<std::uint64_t> g_camera_frames{0};
std::atomic<std::uint64_t> g_selection_rays{0};
std::atomic<std::uint64_t> g_selection_hits{0};
std::atomic<std::uint64_t> g_transform_updates{0};
std::atomic<std::uint64_t> g_transform_commits{0};
std::atomic<std::uint64_t> g_rotation_updates{0};
std::atomic<std::uint64_t> g_interaction_us{0};
std::atomic<std::uint64_t> g_interaction_max_us{0};
std::atomic<std::uint64_t> g_mouse_capture_toggles{0};
std::atomic<std::uint64_t> g_mouse_look_samples{0};
std::atomic<std::uint64_t> g_focus_losses{0};
std::atomic<std::uint64_t> g_focus_regains{0};
std::atomic<bool> g_input_focused{false};
std::atomic<std::uint32_t> g_last_player_board_state{0xFFFFFFFFu};
std::atomic<std::uint64_t> g_last_player_board_state_frame{0};
std::atomic<std::uint64_t> g_offboard_enter_frame{0};
std::atomic<std::uint64_t> g_player_board_state_transitions{0};
std::atomic<std::uint64_t> g_rapid_onboard_returns{0};
std::atomic<std::uint64_t> g_spawn_menu_toggles{0};
std::atomic<std::uint64_t> g_spawn_requests{0};
std::atomic<std::uint64_t> g_spawn_successes{0};
std::atomic<std::uint64_t> g_spawn_failures{0};
std::atomic<std::uint64_t> g_spawn_library_refreshes{0};
bool g_spawn_position_dirty = true;
std::mutex g_import_mutex;
DefaultLibraryStatus g_import_status;
std::future<void> g_import_future;

struct PendingReplicatedEdit {
  MultiplayerEditKind kind = MultiplayerEditKind::kTransformPreview;
  MultiplayerTransform transform;
  MultiplayerSpawn spawn;
  std::shared_ptr<const skate::world::SkateObjectAsset> asset;
  std::uint32_t authority_session = 0;
  std::uint64_t authority_revision = 0;
};

bool g_multiplayer_sync_active = false;
std::uint16_t g_multiplayer_local_role = 0;
std::uint32_t g_multiplayer_local_session = 0;
std::uint64_t g_next_multiplayer_request_id = 1;
std::deque<MultiplayerEditEvent> g_multiplayer_outbound;
std::atomic<bool> g_multiplayer_outbound_pending{false};
std::deque<PendingReplicatedEdit> g_multiplayer_inbound;
std::vector<MultiplayerSpawn> g_spawn_history;
std::unordered_set<std::uint64_t> g_applied_spawn_revisions;
std::atomic<std::uint64_t> g_sync_outbound_events{0};
std::atomic<std::uint64_t> g_sync_inbound_events{0};
std::atomic<std::uint64_t> g_sync_rejected_events{0};

constexpr std::uintmax_t kMaximumReplicatedObjectBytes =
    64ull * 1024ull * 1024ull;

std::shared_ptr<const std::vector<std::uint8_t>>
ReadSpawnPackageLocked(std::size_t asset_index) {
  if (asset_index >= g_spawn_paths.size()) {
    return {};
  }
  std::ifstream stream(g_spawn_paths[asset_index],
                       std::ios::binary | std::ios::ate);
  if (!stream) {
    return {};
  }
  const std::streamoff end = stream.tellg();
  if (end <= 0 ||
      static_cast<std::uintmax_t>(end) > kMaximumReplicatedObjectBytes) {
    return {};
  }
  stream.seekg(0);
  auto bytes = std::make_shared<std::vector<std::uint8_t>>(
      static_cast<std::size_t>(end));
  stream.read(reinterpret_cast<char *>(bytes->data()), end);
  if (!stream) {
    return {};
  }
  return bytes;
}

void QueueTransformEventLocked(MultiplayerEditKind kind,
                               std::uint64_t request_id,
                               const skate::world::MapObject &object,
                               const ObjectState &state) {
  if (!g_multiplayer_sync_active || g_multiplayer_local_role == 0 ||
      request_id == 0) {
    return;
  }
  MultiplayerEditEvent event;
  event.kind = kind;
  event.source_role = g_multiplayer_local_role;
  event.request_id = request_id;
  event.transform.object_id = object.id;
  event.transform.translation = {
      object.origin.x + state.delta.x,
      object.origin.y + state.delta.y,
      object.origin.z + state.delta.z,
  };
  event.transform.basis = {
      state.x_axis.x, state.x_axis.y, state.x_axis.z,
      state.y_axis.x, state.y_axis.y, state.y_axis.z,
      state.z_axis.x, state.z_axis.y, state.z_axis.z,
  };
  if (kind == MultiplayerEditKind::kTransformPreview) {
    const auto existing = std::find_if(
        g_multiplayer_outbound.begin(), g_multiplayer_outbound.end(),
        [&event](const MultiplayerEditEvent &candidate) {
          return candidate.kind == MultiplayerEditKind::kTransformPreview &&
                 candidate.transform.object_id == event.transform.object_id;
        });
    if (existing != g_multiplayer_outbound.end()) {
      *existing = std::move(event);
      g_multiplayer_outbound_pending.store(true, std::memory_order_release);
      return;
    }
  }
  g_multiplayer_outbound.push_back(std::move(event));
  g_multiplayer_outbound_pending.store(true, std::memory_order_release);
  g_sync_outbound_events.fetch_add(1, std::memory_order_relaxed);
}

void EnsureObjectsLocked() {
  const std::size_t count =
      mechanics_sandbox::map::ActiveDefinition().editable_objects.size();
  if (g_objects.size() > count) {
    g_objects.resize(count);
    g_selected.store(std::numeric_limits<std::size_t>::max(),
                     std::memory_order_release);
  } else if (g_objects.size() < count) {
    g_objects.resize(count);
  }
}

void LoadSpawnAssetsLocked(bool force_refresh = false) {
  if (g_spawn_assets_scanned && !force_refresh) {
    return;
  }
  g_spawn_assets_scanned = true;
  std::vector<skate::world::SkateObjectAsset> loaded_assets;
  std::vector<SpawnObjectEntry> loaded_entries;
  std::vector<std::filesystem::path> loaded_paths;
  std::vector<drop_item_library::Category> categories;
  std::string discovery_error;
  if (!drop_item_library::Discover(g_object_library_root, categories,
                                   discovery_error)) {
    g_spawn_assets.clear();
    g_spawn_entries.clear();
    g_spawn_paths.clear();
    g_spawn_asset_count.store(0, std::memory_order_release);
    REXLOG_ERROR("map-editor: object library discovery failed root='{}': {}",
                 g_object_library_root.string(), discovery_error);
    return;
  }
  for (const drop_item_library::Category &category : categories) {
    for (const drop_item_library::File &file : category.files) {
      const std::filesystem::path &path = file.path;
      try {
        loaded_assets.push_back(skate::world::LoadSkateObjectPackage(path));
        loaded_entries.push_back({loaded_assets.size() - 1, category.name,
                                  loaded_assets.back().name});
        loaded_paths.push_back(path);
        REXLOG_INFO(
            "map-editor: object library loaded category='{}' '{}' from '{}'",
            category.name, loaded_assets.back().name, path.string());
      } catch (const std::exception &error) {
        g_spawn_failures.fetch_add(1, std::memory_order_relaxed);
        REXLOG_ERROR("map-editor: object library rejected '{}': {}",
                     path.string(), error.what());
      }
    }
  }
  g_spawn_assets = std::move(loaded_assets);
  g_spawn_entries = std::move(loaded_entries);
  g_spawn_paths = std::move(loaded_paths);
  g_spawn_asset_count.store(g_spawn_assets.size(), std::memory_order_release);
  REXLOG_INFO("map-editor: object library {} assets={}",
              force_refresh ? "refreshed" : "ready", g_spawn_assets.size());
}

skate::world::Vec3 HandleAxis(skate::world::EditorGizmoHandle handle) {
  using Handle = skate::world::EditorGizmoHandle;
  if (handle == Handle::TranslateX || handle == Handle::RotateX) {
    return {1.0f, 0.0f, 0.0f};
  }
  if (handle == Handle::TranslateY || handle == Handle::RotateY) {
    return {0.0f, 1.0f, 0.0f};
  }
  if (handle == Handle::TranslateZ || handle == Handle::RotateZ) {
    return {0.0f, 0.0f, 1.0f};
  }
  return {};
}

bool IsRotationHandle(skate::world::EditorGizmoHandle handle) {
  using Handle = skate::world::EditorGizmoHandle;
  return handle == Handle::RotateX || handle == Handle::RotateY ||
         handle == Handle::RotateZ;
}

#if defined(_WIN32)
HWND Window() {
  return static_cast<HWND>(g_window.load(std::memory_order_acquire));
}

void ForceCursorVisible(bool visible) {
  // ShowCursor is a per-thread display counter, not a boolean. A single call
  // can therefore leave the cursor in its previous state after overlays or
  // focus transitions have adjusted the counter.
  if (visible) {
    while (ShowCursor(TRUE) < 0) {
    }
  } else {
    while (ShowCursor(FALSE) >= 0) {
    }
  }
}

void ReleaseCursor() {
  if (!g_cursor_captured) {
    return;
  }
  ClipCursor(nullptr);
  ReleaseCapture();
  ForceCursorVisible(true);
  g_cursor_captured = false;
  REXLOG_INFO("map-editor: cursor released");
}

bool ClientCursor(HWND window, POINT &client, RECT &bounds) {
  if (window == nullptr || GetForegroundWindow() != window ||
      !GetClientRect(window, &bounds)) {
    return false;
  }
  POINT screen{};
  if (!GetCursorPos(&screen)) {
    return false;
  }
  client = screen;
  return ScreenToClient(window, &client) != FALSE;
}
#else
void ReleaseCursor() {}
#endif

void RecordInteractionTime(std::chrono::steady_clock::time_point started) {
  const auto elapsed = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - started)
          .count());
  g_interaction_us.fetch_add(elapsed, std::memory_order_relaxed);
  std::uint64_t maximum = g_interaction_max_us.load(std::memory_order_relaxed);
  while (elapsed > maximum &&
         !g_interaction_max_us.compare_exchange_weak(
             maximum, elapsed, std::memory_order_relaxed)) {
  }
}

} // namespace

void SetWindowHandle(void *window) {
  g_window.store(window, std::memory_order_release);
  g_input_focused.store(false, std::memory_order_release);
}

void ConfigureObjectLibrary(std::filesystem::path game_data_root,
                            std::filesystem::path object_library_root) {
  std::scoped_lock lock(g_mutex);
  g_game_data_root = std::move(game_data_root);
  g_object_library_root = std::move(object_library_root);
  g_spawn_assets_scanned = false;
}

void Toggle() {
  if (!HasInputFocus()) {
    return;
  }
  SetActive(!Active());
}

void SetActive(bool active) {
  const bool previous = g_active.exchange(active, std::memory_order_acq_rel);
  if (previous == active) {
    return;
  }
  if (active) {
    {
      std::scoped_lock lock(g_mutex);
      EnsureObjectsLocked();
      LoadSpawnAssetsLocked();
      g_previous_scene = REXCVAR_GET(skate3_native_render_scene);
      g_previous_freecam = REXCVAR_GET(skate3_native_render_scene_freecam);
      g_previous_capture_input =
          REXCVAR_GET(skate3_native_render_scene_freecam_capture_input);
      g_dragging = false;
      g_left_was_down = false;
      g_drag_handle = skate::world::EditorGizmoHandle::None;
    }
    REXCVAR_SET(skate3_native_render_scene, true);
    REXCVAR_SET(skate3_native_render_scene_freecam_capture_input, true);
    REXCVAR_SET(skate3_native_render_scene_freecam, true);
    g_entries.fetch_add(1, std::memory_order_relaxed);
    REXLOG_INFO(
        "map-editor: ENTER objects={} camera=native-freecam "
        "gameplay_input=suppressed skater=preserved",
        mechanics_sandbox::map::ActiveDefinition().editable_objects.size());
    return;
  }

  {
    std::scoped_lock lock(g_mutex);
    ReleaseCursor();
    g_spawn_menu_visible.store(false, std::memory_order_release);
    g_dragging = false;
    g_left_was_down = false;
    g_drag_handle = skate::world::EditorGizmoHandle::None;
    const std::size_t deselected = g_selected.exchange(
        std::numeric_limits<std::size_t>::max(), std::memory_order_acq_rel);
    if (deselected != std::numeric_limits<std::size_t>::max()) {
      REXLOG_INFO("map-editor: selection cleared on exit object_index={}",
                  deselected);
    }
  }
  REXCVAR_SET(skate3_native_render_scene_freecam, g_previous_freecam);
  REXCVAR_SET(skate3_native_render_scene_freecam_capture_input,
              g_previous_capture_input);
  REXCVAR_SET(skate3_native_render_scene, g_previous_scene);
  g_exits.fetch_add(1, std::memory_order_relaxed);
  REXLOG_INFO("map-editor: EXIT camera/input/render state restored "
              "freecam={} capture_input={} native_scene={}",
              g_previous_freecam ? 1 : 0, g_previous_capture_input ? 1 : 0,
              g_previous_scene ? 1 : 0);
}

bool Active() { return g_active.load(std::memory_order_acquire); }

void ToggleSpawnMenu() {
  if (!Active() || !HasInputFocus()) {
    return;
  }
  std::scoped_lock lock(g_mutex);
  LoadSpawnAssetsLocked();
  const bool visible = !g_spawn_menu_visible.load(std::memory_order_acquire);
  g_spawn_menu_visible.store(visible, std::memory_order_release);
  if (visible) {
    ReleaseCursor();
    g_dragging = false;
    g_drag_handle = skate::world::EditorGizmoHandle::None;
    g_spawn_position_dirty = true;
  }
  g_spawn_menu_toggles.fetch_add(1, std::memory_order_relaxed);
  REXLOG_INFO("map-editor: spawn menu {} assets={}",
              visible ? "opened" : "closed", g_spawn_assets.size());
}

bool SpawnMenuVisible() {
  return Active() && g_spawn_menu_visible.load(std::memory_order_acquire);
}

std::vector<SpawnObjectEntry> SpawnObjectEntries() {
  std::scoped_lock lock(g_mutex);
  LoadSpawnAssetsLocked();
  return g_spawn_entries;
}

bool StartDefaultLibraryImport() {
  std::filesystem::path game_data_root;
  std::filesystem::path object_library_root;
  {
    std::scoped_lock lock(g_mutex);
    game_data_root = g_game_data_root;
    object_library_root = g_object_library_root;
  }
  if (game_data_root.empty() || object_library_root.empty()) {
    std::scoped_lock lock(g_import_mutex);
    g_import_status.state = DefaultLibraryState::Failed;
    g_import_status.message =
        "The editor's game-data or objects path is not configured.";
    g_import_status.errors = {g_import_status.message};
    return false;
  }
  {
    std::scoped_lock lock(g_import_mutex);
    if (g_import_status.state == DefaultLibraryState::Running) {
      return false;
    }
    g_import_status = {};
    g_import_status.state = DefaultLibraryState::Running;
    g_import_status.message = "Starting local default-item conversion";
  }
  g_import_future =
      std::async(std::launch::async,
                 [game_data_root = std::move(game_data_root),
                  object_library_root = std::move(object_library_root)] {
                   try {
                     const drop_item_import::Result result =
                         drop_item_import::ImportDefaults(
                             game_data_root, object_library_root,
                             [](const drop_item_import::Progress &progress) {
                               std::scoped_lock lock(g_import_mutex);
                               g_import_status.completed = progress.completed;
                               g_import_status.total = progress.total;
                               g_import_status.written = progress.written;
                               g_import_status.reused = progress.reused;
                               g_import_status.unsupported =
                                   progress.unsupported;
                               g_import_status.message = progress.message;
                             });
                     std::scoped_lock lock(g_import_mutex);
                     g_import_status.completed = result.completed;
                     g_import_status.total = result.total;
                     g_import_status.written = result.written;
                     g_import_status.reused = result.reused;
                     g_import_status.unsupported = result.unsupported;
                     g_import_status.message = result.message;
                     g_import_status.errors = result.errors;
                     g_import_status.state = result.errors.empty()
                                                 ? DefaultLibraryState::Complete
                                                 : DefaultLibraryState::Failed;
                   } catch (const std::exception &exception) {
                     std::scoped_lock lock(g_import_mutex);
                     g_import_status.state = DefaultLibraryState::Failed;
                     g_import_status.message =
                         "Default-item conversion stopped unexpectedly: " +
                         std::string(exception.what());
                     g_import_status.errors = {g_import_status.message};
                   } catch (...) {
                     std::scoped_lock lock(g_import_mutex);
                     g_import_status.state = DefaultLibraryState::Failed;
                     g_import_status.message =
                         "Default-item conversion stopped unexpectedly.";
                     g_import_status.errors = {g_import_status.message};
                   }
                   {
                     std::scoped_lock lock(g_mutex);
                     g_spawn_assets_scanned = false;
                   }
                 });
  return true;
}

DefaultLibraryStatus GetDefaultLibraryStatus() {
  std::scoped_lock lock(g_import_mutex);
  return g_import_status;
}

bool RefreshSpawnObjects() {
  if (!Active() || !HasInputFocus()) {
    return false;
  }
  std::scoped_lock lock(g_mutex);
  if (g_pending_spawn.load(std::memory_order_acquire) !=
      std::numeric_limits<std::size_t>::max()) {
    REXLOG_WARN(
        "map-editor: object library refresh deferred while a spawn is pending");
    return false;
  }
  LoadSpawnAssetsLocked(true);
  g_spawn_library_refreshes.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void SetMultiplayerSyncState(bool active, std::uint16_t local_role,
                             std::uint32_t local_session) {
  std::scoped_lock lock(g_mutex);
  active = active && local_role >= 1 && local_role <= 100 && local_session != 0;
  if (g_multiplayer_sync_active == active &&
      g_multiplayer_local_role == (active ? local_role : 0) &&
      g_multiplayer_local_session == (active ? local_session : 0)) {
    return;
  }
  g_multiplayer_sync_active = active;
  g_multiplayer_local_role = active ? local_role : 0;
  g_multiplayer_local_session = active ? local_session : 0;
  g_multiplayer_outbound.clear();
  g_multiplayer_outbound_pending.store(false, std::memory_order_release);
  g_multiplayer_inbound.clear();
  g_drag_request_id = 0;
  REXLOG_INFO("map-editor: multiplayer sync {} role={} session={}",
              active ? "active" : "inactive", g_multiplayer_local_role,
              g_multiplayer_local_session);
}

bool MultiplayerSyncActive() {
  std::scoped_lock lock(g_mutex);
  return g_multiplayer_sync_active;
}

std::vector<MultiplayerEditEvent> DrainMultiplayerEditEvents() {
  if (!g_multiplayer_outbound_pending.load(std::memory_order_acquire)) {
    return {};
  }
  std::scoped_lock lock(g_mutex);
  std::vector<MultiplayerEditEvent> events;
  events.reserve(g_multiplayer_outbound.size());
  while (!g_multiplayer_outbound.empty()) {
    events.push_back(std::move(g_multiplayer_outbound.front()));
    g_multiplayer_outbound.pop_front();
  }
  g_multiplayer_outbound_pending.store(false, std::memory_order_release);
  return events;
}

MultiplayerEditSnapshot CaptureMultiplayerEditSnapshot() {
  std::scoped_lock lock(g_mutex);
  EnsureObjectsLocked();
  MultiplayerEditSnapshot snapshot;
  snapshot.spawns = g_spawn_history;
  const auto &objects =
      mechanics_sandbox::map::ActiveDefinition().editable_objects;
  for (std::size_t index = 0;
       index < objects.size() && index < g_objects.size(); ++index) {
    const ObjectState &state = g_objects[index];
    if (state.revision <= 1) {
      continue;
    }
    MultiplayerTransform transform;
    transform.object_id = objects[index].id;
    transform.translation = {
        objects[index].origin.x + state.delta.x,
        objects[index].origin.y + state.delta.y,
        objects[index].origin.z + state.delta.z,
    };
    transform.basis = {
        state.x_axis.x, state.x_axis.y, state.x_axis.z,
        state.y_axis.x, state.y_axis.y, state.y_axis.z,
        state.z_axis.x, state.z_axis.y, state.z_axis.z,
    };
    snapshot.transforms.push_back(transform);
  }
  return snapshot;
}

bool QueueReplicatedTransform(const MultiplayerTransform &transform,
                              bool committed, std::uint32_t authority_session,
                              std::uint64_t authority_revision) {
  if (transform.object_id == 0 || authority_session == 0 ||
      (committed && authority_revision == 0)) {
    g_sync_rejected_events.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  for (const float value : transform.translation) {
    if (!std::isfinite(value)) {
      g_sync_rejected_events.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
  }
  for (const float value : transform.basis) {
    if (!std::isfinite(value)) {
      g_sync_rejected_events.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
  }
  std::scoped_lock lock(g_mutex);
  if (!g_multiplayer_sync_active || g_multiplayer_inbound.size() >= 1024) {
    g_sync_rejected_events.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  PendingReplicatedEdit edit;
  edit.kind = committed ? MultiplayerEditKind::kTransformCommit
                        : MultiplayerEditKind::kTransformPreview;
  edit.transform = transform;
  edit.authority_session = authority_session;
  edit.authority_revision = authority_revision;
  if (!committed) {
    const auto existing = std::find_if(
        g_multiplayer_inbound.begin(), g_multiplayer_inbound.end(),
        [&transform](const PendingReplicatedEdit &candidate) {
          return candidate.kind == MultiplayerEditKind::kTransformPreview &&
                 candidate.transform.object_id == transform.object_id;
        });
    if (existing != g_multiplayer_inbound.end()) {
      *existing = std::move(edit);
      return true;
    }
  }
  g_multiplayer_inbound.push_back(std::move(edit));
  g_sync_inbound_events.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool QueueReplicatedSpawn(
    std::shared_ptr<const skate::world::SkateObjectAsset> asset,
    MultiplayerSpawn spawn, std::uint32_t authority_session,
    std::uint64_t authority_revision) {
  if (asset == nullptr || spawn.package == nullptr || spawn.package->empty() ||
      authority_session == 0 || authority_revision == 0) {
    g_sync_rejected_events.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  std::scoped_lock lock(g_mutex);
  if (!g_multiplayer_sync_active || g_multiplayer_inbound.size() >= 1024) {
    g_sync_rejected_events.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const std::uint64_t revision_key =
      (static_cast<std::uint64_t>(authority_session) << 32) ^
      authority_revision ^ (authority_revision >> 32) * 0x9E3779B185EBCA87ull;
  if (g_applied_spawn_revisions.contains(revision_key)) {
    return true;
  }
  for (const PendingReplicatedEdit &pending : g_multiplayer_inbound) {
    if (pending.kind == MultiplayerEditKind::kSpawn &&
        pending.authority_session == authority_session &&
        pending.authority_revision == authority_revision) {
      return true;
    }
  }
  PendingReplicatedEdit edit;
  edit.kind = MultiplayerEditKind::kSpawn;
  edit.spawn = std::move(spawn);
  edit.asset = std::move(asset);
  edit.authority_session = authority_session;
  edit.authority_revision = authority_revision;
  g_multiplayer_inbound.push_back(std::move(edit));
  g_sync_inbound_events.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool QueueSpawnObject(std::size_t asset_index) {
  std::scoped_lock lock(g_mutex);
  LoadSpawnAssetsLocked();
  if (!Active() || asset_index >= g_spawn_assets.size() ||
      !g_spawn_position_valid) {
    g_spawn_failures.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (g_multiplayer_sync_active && g_multiplayer_local_role != 1) {
    const auto package = ReadSpawnPackageLocked(asset_index);
    if (package == nullptr) {
      g_spawn_failures.fetch_add(1, std::memory_order_relaxed);
      REXLOG_ERROR("map-editor: could not read selected .skateobj for "
                   "multiplayer replication");
      return false;
    }
    MultiplayerEditEvent event;
    event.kind = MultiplayerEditKind::kSpawn;
    event.source_role = g_multiplayer_local_role;
    event.request_id = g_next_multiplayer_request_id++;
    event.spawn.position = {
        g_spawn_position.x,
        g_spawn_position.y,
        g_spawn_position.z,
    };
    event.spawn.package = package;
    g_multiplayer_outbound.push_back(std::move(event));
    g_multiplayer_outbound_pending.store(true, std::memory_order_release);
    g_spawn_menu_visible.store(false, std::memory_order_release);
    g_spawn_requests.fetch_add(1, std::memory_order_relaxed);
    g_sync_outbound_events.fetch_add(1, std::memory_order_relaxed);
    REXLOG_INFO("map-editor: multiplayer spawn proposed asset={} name='{}' "
                "bytes={} position=({:.3f},{:.3f},{:.3f})",
                asset_index, g_spawn_assets[asset_index].name, package->size(),
                g_spawn_position.x, g_spawn_position.y, g_spawn_position.z);
    return true;
  }
  g_pending_spawn.store(asset_index, std::memory_order_release);
  g_spawn_menu_visible.store(false, std::memory_order_release);
  g_spawn_requests.fetch_add(1, std::memory_order_relaxed);
  REXLOG_INFO("map-editor: spawn requested asset={} name='{}' "
              "position=({:.3f},{:.3f},{:.3f})",
              asset_index, g_spawn_assets[asset_index].name, g_spawn_position.x,
              g_spawn_position.y, g_spawn_position.z);
  return true;
}

bool ApplyPendingSpawn() {
  std::scoped_lock lock(g_mutex);
  EnsureObjectsLocked();
  if (!g_multiplayer_inbound.empty()) {
    PendingReplicatedEdit edit = std::move(g_multiplayer_inbound.front());
    g_multiplayer_inbound.pop_front();
    if (edit.kind == MultiplayerEditKind::kSpawn) {
      try {
        const std::size_t object_index =
            mechanics_sandbox::map::AppendSpawnedObject(
                *edit.asset, {edit.spawn.position[0], edit.spawn.position[1],
                              edit.spawn.position[2]});
        EnsureObjectsLocked();
        g_selected.store(object_index, std::memory_order_release);
        g_spawn_history.push_back(edit.spawn);
        const std::uint64_t revision_key =
            (static_cast<std::uint64_t>(edit.authority_session) << 32) ^
            edit.authority_revision ^
            (edit.authority_revision >> 32) * 0x9E3779B185EBCA87ull;
        g_applied_spawn_revisions.insert(revision_key);
        g_transform_commits.fetch_add(1, std::memory_order_relaxed);
        g_spawn_successes.fetch_add(1, std::memory_order_relaxed);
        REXLOG_INFO("map-editor: replicated spawn applied authority_session={} "
                    "revision={} object_index={}",
                    edit.authority_session, edit.authority_revision,
                    object_index);
        return true;
      } catch (const std::exception &error) {
        g_spawn_failures.fetch_add(1, std::memory_order_relaxed);
        g_sync_rejected_events.fetch_add(1, std::memory_order_relaxed);
        REXLOG_ERROR("map-editor: replicated spawn apply failed: {}",
                     error.what());
        return false;
      }
    }

    const auto &definition = mechanics_sandbox::map::ActiveDefinition();
    const auto found = std::find_if(
        definition.editable_objects.begin(), definition.editable_objects.end(),
        [&edit](const skate::world::MapObject &object) {
          return object.id == edit.transform.object_id;
        });
    if (found == definition.editable_objects.end()) {
      g_sync_rejected_events.fetch_add(1, std::memory_order_relaxed);
      REXLOG_WARN("map-editor: replicated transform references unknown id={}",
                  edit.transform.object_id);
      return false;
    }
    const std::size_t index = static_cast<std::size_t>(
        std::distance(definition.editable_objects.begin(), found));
    ObjectState &state = g_objects[index];
    state.delta = {
        edit.transform.translation[0] - found->origin.x,
        edit.transform.translation[1] - found->origin.y,
        edit.transform.translation[2] - found->origin.z,
    };
    state.x_axis = {
        edit.transform.basis[0],
        edit.transform.basis[1],
        edit.transform.basis[2],
    };
    state.y_axis = {
        edit.transform.basis[3],
        edit.transform.basis[4],
        edit.transform.basis[5],
    };
    state.z_axis = {
        edit.transform.basis[6],
        edit.transform.basis[7],
        edit.transform.basis[8],
    };
    ++state.revision;
    g_transform_updates.fetch_add(1, std::memory_order_relaxed);
    if (edit.kind == MultiplayerEditKind::kTransformCommit) {
      g_transform_commits.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
  }

  const std::size_t asset_index = g_pending_spawn.exchange(
      std::numeric_limits<std::size_t>::max(), std::memory_order_acq_rel);
  if (asset_index == std::numeric_limits<std::size_t>::max()) {
    return false;
  }
  if (asset_index >= g_spawn_assets.size() || !g_spawn_position_valid) {
    g_spawn_failures.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  try {
    const auto package = ReadSpawnPackageLocked(asset_index);
    if (g_multiplayer_sync_active && g_multiplayer_local_role == 1 &&
        package == nullptr) {
      g_spawn_failures.fetch_add(1, std::memory_order_relaxed);
      REXLOG_ERROR("map-editor: host spawn cancelled because the selected "
                   ".skateobj could not be read for replication");
      return false;
    }
    const std::size_t object_index =
        mechanics_sandbox::map::AppendSpawnedObject(g_spawn_assets[asset_index],
                                                    g_spawn_position);
    EnsureObjectsLocked();
    g_selected.store(object_index, std::memory_order_release);
    g_transform_commits.fetch_add(1, std::memory_order_relaxed);
    g_spawn_successes.fetch_add(1, std::memory_order_relaxed);
    if (package != nullptr) {
      MultiplayerSpawn spawn;
      spawn.position = {
          g_spawn_position.x,
          g_spawn_position.y,
          g_spawn_position.z,
      };
      spawn.package = package;
      g_spawn_history.push_back(spawn);
      if (g_multiplayer_sync_active) {
        MultiplayerEditEvent event;
        event.kind = MultiplayerEditKind::kSpawn;
        event.source_role = g_multiplayer_local_role;
        event.request_id = g_next_multiplayer_request_id++;
        event.spawn = std::move(spawn);
        g_multiplayer_outbound.push_back(std::move(event));
        g_multiplayer_outbound_pending.store(true, std::memory_order_release);
        g_sync_outbound_events.fetch_add(1, std::memory_order_relaxed);
      }
    }
    return true;
  } catch (const std::exception &error) {
    g_spawn_failures.fetch_add(1, std::memory_order_relaxed);
    REXLOG_ERROR("map-editor: spawn failed asset={}: {}", asset_index,
                 error.what());
    return false;
  }
}

bool HasInputFocus() {
#if defined(_WIN32)
  const HWND window = Window();
  const bool focused = window != nullptr && GetForegroundWindow() == window &&
                       IsWindowVisible(window) != FALSE &&
                       IsIconic(window) == FALSE;
  const bool previous =
      g_input_focused.exchange(focused, std::memory_order_acq_rel);
  if (Active() && previous != focused) {
    if (focused) {
      g_focus_regains.fetch_add(1, std::memory_order_relaxed);
      REXLOG_INFO("map-editor: game input focus regained");
    } else {
      g_focus_losses.fetch_add(1, std::memory_order_relaxed);
      REXLOG_INFO(
          "map-editor: game input focus lost; keyboard/mouse suspended");
    }
  }
  return focused;
#else
  return true;
#endif
}

bool ConsumeMouseLook(double &yaw_radians, double &pitch_radians) {
  yaw_radians = 0.0;
  pitch_radians = 0.0;
  if (!Active() || SpawnMenuVisible()) {
    std::scoped_lock lock(g_mutex);
    ReleaseCursor();
    return false;
  }
#if defined(_WIN32)
  std::scoped_lock lock(g_mutex);
  const bool right_down = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
  HWND window = Window();
  if (!HasInputFocus()) {
    ReleaseCursor();
    return false;
  }
  RECT client{};
  if (!GetClientRect(window, &client)) {
    ReleaseCursor();
    return false;
  }
  POINT center{
      (client.left + client.right) / 2,
      (client.top + client.bottom) / 2,
  };
  POINT center_screen = center;
  if (!ClientToScreen(window, &center_screen)) {
    ReleaseCursor();
    return false;
  }
  if (!right_down) {
    const bool was_captured = g_cursor_captured;
    ReleaseCursor();
    if (was_captured) {
      g_mouse_capture_toggles.fetch_add(1, std::memory_order_relaxed);
    }
    return false;
  }
  if (!g_cursor_captured) {
    RECT clip = client;
    POINT upper_left{clip.left, clip.top};
    POINT lower_right{clip.right, clip.bottom};
    ClientToScreen(window, &upper_left);
    ClientToScreen(window, &lower_right);
    clip = {upper_left.x, upper_left.y, lower_right.x, lower_right.y};
    SetCapture(window);
    ClipCursor(&clip);
    ForceCursorVisible(false);
    SetCursorPos(center_screen.x, center_screen.y);
    g_cursor_captured = true;
    g_dragging = false;
    g_drag_handle = skate::world::EditorGizmoHandle::None;
    g_left_was_down = false;
    g_mouse_capture_toggles.fetch_add(1, std::memory_order_relaxed);
    REXLOG_INFO("map-editor: cursor captured for mouse look (RMB held)");
    return true;
  }
  POINT cursor{};
  if (!GetCursorPos(&cursor)) {
    return true;
  }
  constexpr double kRadiansPerPixel = 0.0022;
  yaw_radians =
      static_cast<double>(cursor.x - center_screen.x) * kRadiansPerPixel;
  pitch_radians =
      -static_cast<double>(cursor.y - center_screen.y) * kRadiansPerPixel;
  SetCursorPos(center_screen.x, center_screen.y);
  if (yaw_radians != 0.0 || pitch_radians != 0.0) {
    g_mouse_look_samples.fetch_add(1, std::memory_order_relaxed);
  }
  return true;
#else
  return false;
#endif
}

void ObservePlayerState() noexcept {
  trick_pipeline::LiveSpatialSnapshot snapshot;
  if (!trick_pipeline::CurrentLiveSpatialSnapshot(snapshot) ||
      snapshot.board_state_flags == 0xFFFFFFFFu) {
    return;
  }
  const std::uint32_t flags = snapshot.board_state_flags;
  const std::uint32_t previous =
      g_last_player_board_state.exchange(flags, std::memory_order_acq_rel);
  g_last_player_board_state_frame.store(snapshot.frame,
                                        std::memory_order_release);
  if (previous == 0xFFFFFFFFu || previous == flags) {
    return;
  }

  const bool was_offboard = (previous & 0x7u) != 0u;
  const bool offboard = (flags & 0x7u) != 0u;
  const float position[3] = {
      std::bit_cast<float>(snapshot.position_bits[0]),
      std::bit_cast<float>(snapshot.position_bits[1]),
      std::bit_cast<float>(snapshot.position_bits[2]),
  };
  g_player_board_state_transitions.fetch_add(1, std::memory_order_relaxed);
  REXLOG_INFO("map-editor: player board-state transition frame={} "
              "flags=0x{:08X}->0x{:08X} offboard={}->{} "
              "position=({:.3f},{:.3f},{:.3f}) editor_active={}",
              snapshot.frame, previous, flags, was_offboard ? 1 : 0,
              offboard ? 1 : 0, position[0], position[1], position[2],
              Active() ? 1 : 0);

  if (!was_offboard && offboard) {
    g_offboard_enter_frame.store(snapshot.frame, std::memory_order_release);
  } else if (was_offboard && !offboard) {
    const std::uint64_t entered =
        g_offboard_enter_frame.exchange(0, std::memory_order_acq_rel);
    if (entered != 0 && snapshot.frame >= entered &&
        snapshot.frame - entered <= 120) {
      g_rapid_onboard_returns.fetch_add(1, std::memory_order_relaxed);
      REXLOG_WARN("map-editor: rapid offboard return observed "
                  "entered_frame={} returned_frame={} duration_frames={}",
                  entered, snapshot.frame, snapshot.frame - entered);
    }
  }
}

void UpdateInteraction(const float view[16], const float projection[16],
                       const float camera_position[3]) {
  if (!Active()) {
    return;
  }
  const auto started = std::chrono::steady_clock::now();
  g_camera_frames.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
  std::scoped_lock lock(g_mutex);
  EnsureObjectsLocked();
  if (g_cursor_captured) {
    g_left_was_down = false;
    RecordInteractionTime(started);
    return;
  }
  HWND window = Window();
  POINT cursor{};
  RECT bounds{};
  if (!ClientCursor(window, cursor, bounds)) {
    g_dragging = false;
    g_left_was_down = false;
    RecordInteractionTime(started);
    return;
  }
  const int width = bounds.right - bounds.left;
  const int height = bounds.bottom - bounds.top;
  if (width <= 1 || height <= 1) {
    RecordInteractionTime(started);
    return;
  }
  const float ndc_x =
      2.0f * static_cast<float>(cursor.x - bounds.left) / width - 1.0f;
  const float ndc_y =
      1.0f - 2.0f * static_cast<float>(cursor.y - bounds.top) / height;
  skate::world::EditorRay ray;
  try {
    ray = skate::world::BuildEditorCameraRay(view, projection, camera_position,
                                             ndc_x, ndc_y);
  } catch (...) {
    RecordInteractionTime(started);
    return;
  }
  const bool left_down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
  const bool spawn_menu_visible =
      g_spawn_menu_visible.load(std::memory_order_acquire);
  if (!spawn_menu_visible && !left_down && !g_left_was_down) {
    RecordInteractionTime(started);
    return;
  }
  const auto &definition = mechanics_sandbox::map::ActiveDefinition();
  float map_origin_values[3] = {};
  if (!native_collision::MapWorldOrigin(map_origin_values)) {
    RecordInteractionTime(started);
    return;
  }
  const skate::world::Vec3 map_origin{
      map_origin_values[0],
      map_origin_values[1],
      map_origin_values[2],
  };
  std::vector<skate::world::EditorObjectTransform> transforms;
  transforms.reserve(definition.editable_objects.size());
  for (std::size_t index = 0; index < definition.editable_objects.size();
       ++index) {
    transforms.push_back({
        .translation = map_origin + definition.editable_objects[index].origin +
                       g_objects[index].delta,
        .x_axis = g_objects[index].x_axis,
        .y_axis = g_objects[index].y_axis,
        .z_axis = g_objects[index].z_axis,
    });
  }
  if (spawn_menu_visible && g_spawn_position_dirty) {
    try {
      const skate::world::EditorRay center_ray =
          skate::world::BuildEditorCameraRay(view, projection, camera_position,
                                             0.0f, 0.0f);
      g_spawn_position =
          center_ray.origin + center_ray.direction * 8.0f - map_origin;
      g_spawn_position_valid = true;
      const skate::world::MapObjectHit placement = skate::world::PickMapObject(
          definition, transforms, center_ray, 200.0f);
      if (placement.hit) {
        g_spawn_position =
            placement.point - map_origin + placement.normal * 0.02f;
      }
    } catch (...) {
      g_spawn_position_valid = false;
    }
    g_spawn_position_dirty = false;
  }
  if (spawn_menu_visible) {
    g_dragging = false;
    g_left_was_down = false;
    RecordInteractionTime(started);
    return;
  }

  if (left_down && !g_left_was_down) {
    const std::size_t selected = g_selected.load(std::memory_order_acquire);
    if (selected < transforms.size()) {
      const float gizmo_scale = std::clamp(
          skate::world::Length(transforms[selected].translation -
                               skate::world::Vec3{camera_position[0],
                                                  camera_position[1],
                                                  camera_position[2]}) *
              0.12f,
          0.3f, 8.0f);
      const skate::world::EditorGizmoHit gizmo = skate::world::PickEditorGizmo(
          ray, transforms[selected].translation, gizmo_scale);
      if (gizmo.handle != skate::world::EditorGizmoHandle::None) {
        const skate::world::Vec3 axis = HandleAxis(gizmo.handle);
        bool began = false;
        if (IsRotationHandle(gizmo.handle)) {
          began = skate::world::EditorRotationDragVector(
              ray, transforms[selected].translation, axis,
              g_drag_rotation_vector);
        } else {
          began = skate::world::EditorAxisDragParameter(
              ray, transforms[selected].translation, axis,
              g_drag_axis_parameter);
        }
        if (began) {
          g_dragging = true;
          g_drag_request_id = g_next_multiplayer_request_id++;
          g_drag_handle = gizmo.handle;
          g_drag_initial_state = g_objects[selected];
          g_drag_pivot = transforms[selected].translation;
          REXLOG_INFO("map-editor: gizmo drag begin id={} name='{}' handle={}",
                      definition.editable_objects[selected].id,
                      definition.editable_objects[selected].name,
                      static_cast<int>(gizmo.handle));
          g_left_was_down = true;
          RecordInteractionTime(started);
          return;
        }
      }
    }

    g_selection_rays.fetch_add(1, std::memory_order_relaxed);
    const skate::world::MapObjectHit hit =
        skate::world::PickMapObject(definition, transforms, ray);
    if (hit.hit) {
      const auto &object = definition.editable_objects[hit.object_index];
      g_selected.store(hit.object_index, std::memory_order_release);
      g_selection_hits.fetch_add(1, std::memory_order_relaxed);
      g_dragging = false;
      g_drag_handle = skate::world::EditorGizmoHandle::None;
      REXLOG_INFO("map-editor: selection ray ndc=({:.3f},{:.3f}) "
                  "hit id={} name='{}' distance={:.3f} "
                  "world=({:.3f},{:.3f},{:.3f})",
                  ndc_x, ndc_y, object.id, object.name, hit.distance,
                  hit.point.x, hit.point.y, hit.point.z);
    } else {
      g_selected.store(std::numeric_limits<std::size_t>::max(),
                       std::memory_order_release);
      g_dragging = false;
      REXLOG_INFO("map-editor: selection ray ndc=({:.3f},{:.3f}) miss", ndc_x,
                  ndc_y);
    }
  } else if (left_down && g_dragging) {
    const std::size_t selected = g_selected.load(std::memory_order_acquire);
    if (selected < g_objects.size()) {
      ObjectState next = g_drag_initial_state;
      const skate::world::Vec3 axis = HandleAxis(g_drag_handle);
      if (IsRotationHandle(g_drag_handle)) {
        skate::world::Vec3 current;
        if (skate::world::EditorRotationDragVector(ray, g_drag_pivot, axis,
                                                   current)) {
          const float radians = skate::world::EditorSignedRotation(
              g_drag_rotation_vector, current, axis);
          next.x_axis = skate::world::RotateEditorVector(
              g_drag_initial_state.x_axis, axis, radians);
          next.y_axis = skate::world::RotateEditorVector(
              g_drag_initial_state.y_axis, axis, radians);
          next.z_axis = skate::world::RotateEditorVector(
              g_drag_initial_state.z_axis, axis, radians);
        }
      } else {
        float parameter = 0.0f;
        if (skate::world::EditorAxisDragParameter(ray, g_drag_pivot, axis,
                                                  parameter)) {
          next.delta = g_drag_initial_state.delta +
                       axis * (parameter - g_drag_axis_parameter);
        }
      }
      ObjectState &state = g_objects[selected];
      const bool translation_changed =
          skate::world::LengthSquared(next.delta - state.delta) > 1.0e-6f;
      const bool rotation_changed =
          skate::world::LengthSquared(next.x_axis - state.x_axis) +
              skate::world::LengthSquared(next.y_axis - state.y_axis) +
              skate::world::LengthSquared(next.z_axis - state.z_axis) >
          1.0e-6f;
      if (translation_changed || rotation_changed) {
        const std::uint64_t revision = state.revision + 1;
        state = next;
        state.revision = revision;
        g_transform_updates.fetch_add(1, std::memory_order_relaxed);
        if (rotation_changed) {
          g_rotation_updates.fetch_add(1, std::memory_order_relaxed);
        }
        QueueTransformEventLocked(MultiplayerEditKind::kTransformPreview,
                                  g_drag_request_id,
                                  definition.editable_objects[selected], state);
        if ((state.revision & 31u) == 0u) {
          const auto &object = definition.editable_objects[selected];
          REXLOG_INFO("map-editor: transform id={} name='{}' "
                      "translation=({:.3f},{:.3f},{:.3f}) "
                      "basis_x=({:.3f},{:.3f},{:.3f}) revision={}",
                      object.id, object.name, object.origin.x + state.delta.x,
                      object.origin.y + state.delta.y,
                      object.origin.z + state.delta.z, state.x_axis.x,
                      state.x_axis.y, state.x_axis.z, state.revision);
        }
      }
    }
  }
  if (!left_down && g_left_was_down) {
    if (g_dragging) {
      const std::size_t selected = g_selected.load(std::memory_order_acquire);
      g_transform_commits.fetch_add(1, std::memory_order_relaxed);
      if (selected < definition.editable_objects.size()) {
        QueueTransformEventLocked(
            MultiplayerEditKind::kTransformCommit, g_drag_request_id,
            definition.editable_objects[selected], g_objects[selected]);
        REXLOG_INFO("map-editor: gizmo drag commit id={} name='{}' "
                    "handle={} revision={}",
                    definition.editable_objects[selected].id,
                    definition.editable_objects[selected].name,
                    static_cast<int>(g_drag_handle),
                    g_objects[selected].revision);
      }
    }
    g_dragging = false;
    g_drag_request_id = 0;
    g_drag_handle = skate::world::EditorGizmoHandle::None;
  }
  g_left_was_down = left_down;
#endif
  RecordInteractionTime(started);
}

bool ObjectTransform(std::size_t index, float out_translation[3],
                     std::uint64_t *out_revision) {
  return ObjectTransform(index, out_translation, nullptr, out_revision);
}

bool ObjectTransform(std::size_t index, float out_translation[3],
                     float out_basis[9], std::uint64_t *out_revision) {
  if (out_translation == nullptr) {
    return false;
  }
  std::scoped_lock lock(g_mutex);
  EnsureObjectsLocked();
  const auto &objects =
      mechanics_sandbox::map::ActiveDefinition().editable_objects;
  if (index >= objects.size() || index >= g_objects.size()) {
    return false;
  }
  const skate::world::Vec3 translation =
      objects[index].origin + g_objects[index].delta;
  out_translation[0] = translation.x;
  out_translation[1] = translation.y;
  out_translation[2] = translation.z;
  if (out_basis != nullptr) {
    out_basis[0] = g_objects[index].x_axis.x;
    out_basis[1] = g_objects[index].x_axis.y;
    out_basis[2] = g_objects[index].x_axis.z;
    out_basis[3] = g_objects[index].y_axis.x;
    out_basis[4] = g_objects[index].y_axis.y;
    out_basis[5] = g_objects[index].y_axis.z;
    out_basis[6] = g_objects[index].z_axis.x;
    out_basis[7] = g_objects[index].z_axis.y;
    out_basis[8] = g_objects[index].z_axis.z;
  }
  if (out_revision != nullptr) {
    *out_revision = g_objects[index].revision;
  }
  return true;
}

bool IsSelected(std::size_t index) {
  return Active() && g_selected.load(std::memory_order_acquire) == index;
}

std::size_t SelectedObject() {
  return g_selected.load(std::memory_order_acquire);
}

int ActiveGizmoHandle() {
  std::scoped_lock lock(g_mutex);
  return static_cast<int>(g_drag_handle);
}

std::uint64_t TransformCommitSerial() {
  return g_transform_commits.load(std::memory_order_acquire);
}

void AppendTelemetry(std::ostream &out) {
  const std::size_t selected = SelectedObject();
  std::uint64_t desired_revision = 0;
  std::uint64_t applied_revision = 0;
  float desired_translation[3] = {};
  float applied_translation[3] = {};
  float desired_basis[9] = {};
  float applied_basis[9] = {};
  const bool desired_valid =
      selected != std::numeric_limits<std::size_t>::max() &&
      ObjectTransform(selected, desired_translation, desired_basis,
                      &desired_revision);
  const bool applied_valid =
      selected != std::numeric_limits<std::size_t>::max() &&
      native_collision::EditableObjectPose(selected, applied_translation,
                                           applied_basis, &applied_revision);
  bool transform_synchronized =
      desired_valid && applied_valid && desired_revision == applied_revision &&
      std::abs(desired_translation[0] - applied_translation[0]) < 1.0e-5f &&
      std::abs(desired_translation[1] - applied_translation[1]) < 1.0e-5f &&
      std::abs(desired_translation[2] - applied_translation[2]) < 1.0e-5f;
  for (std::size_t component = 0; transform_synchronized && component < 9;
       ++component) {
    transform_synchronized =
        std::abs(desired_basis[component] - applied_basis[component]) < 1.0e-5f;
  }
  trick_pipeline::LiveSpatialSnapshot player_spatial;
  const bool player_spatial_valid =
      trick_pipeline::CurrentLiveSpatialSnapshot(player_spatial);
  const bool player_offboard =
      player_spatial_valid && player_spatial.board_state_flags != 0xFFFFFFFFu &&
      (player_spatial.board_state_flags & 0x7u) != 0;
  out << " map_editor_active=" << (Active() ? 1 : 0) << " map_editor_objects="
      << mechanics_sandbox::map::ActiveDefinition().editable_objects.size()
      << " map_editor_selected="
      << (SelectedObject() == std::numeric_limits<std::size_t>::max()
              ? -1
              : static_cast<std::int64_t>(SelectedObject()))
      << " map_editor_entries=" << g_entries.load(std::memory_order_relaxed)
      << " map_editor_exits=" << g_exits.load(std::memory_order_relaxed)
      << " map_editor_camera_frames="
      << g_camera_frames.load(std::memory_order_relaxed)
      << " map_editor_selection_rays="
      << g_selection_rays.load(std::memory_order_relaxed)
      << " map_editor_selection_hits="
      << g_selection_hits.load(std::memory_order_relaxed)
      << " map_editor_transform_updates="
      << g_transform_updates.load(std::memory_order_relaxed)
      << " map_editor_transform_commits="
      << g_transform_commits.load(std::memory_order_relaxed)
      << " map_editor_rotation_updates="
      << g_rotation_updates.load(std::memory_order_relaxed)
      << " map_editor_spawn_menu_visible=" << (SpawnMenuVisible() ? 1 : 0)
      << " map_editor_spawn_assets="
      << g_spawn_asset_count.load(std::memory_order_acquire)
      << " map_editor_spawn_menu_toggles="
      << g_spawn_menu_toggles.load(std::memory_order_relaxed)
      << " map_editor_spawn_requests="
      << g_spawn_requests.load(std::memory_order_relaxed)
      << " map_editor_spawn_successes="
      << g_spawn_successes.load(std::memory_order_relaxed)
      << " map_editor_spawn_failures="
      << g_spawn_failures.load(std::memory_order_relaxed)
      << " map_editor_spawn_library_refreshes="
      << g_spawn_library_refreshes.load(std::memory_order_relaxed)
      << " map_editor_multiplayer_sync_active="
      << (MultiplayerSyncActive() ? 1 : 0)
      << " map_editor_sync_outbound_events="
      << g_sync_outbound_events.load(std::memory_order_relaxed)
      << " map_editor_sync_inbound_events="
      << g_sync_inbound_events.load(std::memory_order_relaxed)
      << " map_editor_sync_rejected_events="
      << g_sync_rejected_events.load(std::memory_order_relaxed)
      << " map_editor_interaction_us="
      << g_interaction_us.load(std::memory_order_relaxed)
      << " map_editor_interaction_max_us="
      << g_interaction_max_us.load(std::memory_order_relaxed)
      << " map_editor_cursor_captured=" << (g_cursor_captured ? 1 : 0)
      << " map_editor_mouse_capture_toggles="
      << g_mouse_capture_toggles.load(std::memory_order_relaxed)
      << " map_editor_mouse_look_samples="
      << g_mouse_look_samples.load(std::memory_order_relaxed)
      << " map_editor_input_focused="
      << (g_input_focused.load(std::memory_order_relaxed) ? 1 : 0)
      << " map_editor_focus_losses="
      << g_focus_losses.load(std::memory_order_relaxed)
      << " map_editor_focus_regains="
      << g_focus_regains.load(std::memory_order_relaxed)
      << " map_editor_player_board_state_transitions="
      << g_player_board_state_transitions.load(std::memory_order_relaxed)
      << " map_editor_rapid_onboard_returns="
      << g_rapid_onboard_returns.load(std::memory_order_relaxed)
      << " map_editor_player_board_state_frame="
      << g_last_player_board_state_frame.load(std::memory_order_relaxed)
      << " map_editor_player_spatial_valid=" << (player_spatial_valid ? 1 : 0)
      << " map_editor_player_board_state_flags="
      << (player_spatial_valid ? player_spatial.board_state_flags : 0xFFFFFFFFu)
      << " map_editor_player_offboard=" << (player_offboard ? 1 : 0)
      << " map_editor_desired_revision="
      << (desired_valid ? desired_revision : 0)
      << " map_editor_collision_revision="
      << (applied_valid ? applied_revision : 0)
      << " map_editor_transform_synchronized="
      << (transform_synchronized ? 1 : 0) << " map_editor_desired_x="
      << (desired_valid ? desired_translation[0] : 0.0f)
      << " map_editor_desired_y="
      << (desired_valid ? desired_translation[1] : 0.0f)
      << " map_editor_desired_z="
      << (desired_valid ? desired_translation[2] : 0.0f)
      << " map_editor_collision_x="
      << (applied_valid ? applied_translation[0] : 0.0f)
      << " map_editor_collision_y="
      << (applied_valid ? applied_translation[1] : 0.0f)
      << " map_editor_collision_z="
      << (applied_valid ? applied_translation[2] : 0.0f)
      << " map_editor_desired_basis_xx="
      << (desired_valid ? desired_basis[0] : 0.0f)
      << " map_editor_desired_basis_xy="
      << (desired_valid ? desired_basis[1] : 0.0f)
      << " map_editor_desired_basis_xz="
      << (desired_valid ? desired_basis[2] : 0.0f)
      << " map_editor_collision_basis_xx="
      << (applied_valid ? applied_basis[0] : 0.0f)
      << " map_editor_collision_basis_xy="
      << (applied_valid ? applied_basis[1] : 0.0f)
      << " map_editor_collision_basis_xz="
      << (applied_valid ? applied_basis[2] : 0.0f);
}

} // namespace skate3::map_editor
