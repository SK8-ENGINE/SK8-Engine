#include "skate3_steam_backend.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string_view>

#include <rex/logging.h>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#endif

namespace skate3::multiplayer::steam {
namespace {

constexpr int kCallbackSteamApiCallCompleted = 703;
constexpr int kCallbackLobbyEnter = 504;
constexpr int kCallbackLobbyChatUpdate = 506;
constexpr int kCallbackLobbyMatchList = 510;
constexpr int kCallbackLobbyCreated = 513;
constexpr int kCallbackNetworkingSessionRequest = 1251;
constexpr int kResultOk = 1;
constexpr std::uint32_t kProtocolVersion = 2;
constexpr int kLobbyPrivate = 0;
constexpr int kLobbyFriendsOnly = 1;
constexpr int kLobbyPublic = 2;
constexpr int kLobbyComparisonEqual = 0;
constexpr int kLobbyDistanceWorldwide = 3;
constexpr int kSendUnreliableNoDelayAutoRestart = 1 | 4 | 32;
constexpr int kNetworkingChannel = 0;
constexpr std::string_view kGameKey = "skate3-custom-engine-layer";

#pragma pack(push, 8)
struct CallbackMessage {
  int steam_user = 0;
  int callback = 0;
  void* parameter = nullptr;
  int parameter_bytes = 0;
};

struct SteamApiCallCompleted {
  std::uint64_t call = 0;
  int callback = 0;
  std::uint32_t parameter_bytes = 0;
};

struct LobbyCreated {
  int result = 0;
  std::uint64_t lobby_id = 0;
};

struct LobbyEnter {
  std::uint64_t lobby_id = 0;
  std::uint32_t chat_permissions = 0;
  bool locked = false;
  std::uint32_t response = 0;
};

struct LobbyMatchList {
  std::uint32_t count = 0;
};

struct LobbyChatUpdate {
  std::uint64_t lobby_id = 0;
  std::uint64_t changed_user = 0;
  std::uint64_t making_change = 0;
  std::uint32_t state_change = 0;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct SteamNetworkingIdentity {
  int type = 0;
  int size = 0;
  std::uint32_t reserved[32] = {};
};
#pragma pack(pop)

struct SteamNetworkingMessage {
  void* data = nullptr;
  int byte_count = 0;
  std::uint32_t connection = 0;
  SteamNetworkingIdentity identity;
  std::int64_t connection_user_data = 0;
  std::int64_t received_time = 0;
  std::int64_t message_number = 0;
  void* free_data = nullptr;
  void* release = nullptr;
  int channel = 0;
  int flags = 0;
  std::int64_t user_data = 0;
  std::uint16_t lane = 0;
  std::uint16_t padding = 0;
};

static_assert(sizeof(CallbackMessage) == 24);
static_assert(sizeof(SteamApiCallCompleted) == 16);
static_assert(sizeof(LobbyCreated) == 16);
static_assert(sizeof(LobbyEnter) == 24);
static_assert(sizeof(SteamNetworkingIdentity) == 136);

#if defined(_WIN32)
template <typename T>
bool LoadFunction(HMODULE module, const char* name, T& output) {
  output = reinterpret_cast<T>(GetProcAddress(module, name));
  return output != nullptr;
}
#endif

struct Api {
#if defined(_WIN32)
  HMODULE module = nullptr;
#endif
  using InitFlat = int (*)(char*);
  using Shutdown = void (*)();
  using GetPipe = int (*)();
  using ManualInit = void (*)();
  using ManualRunFrame = void (*)(int);
  using ManualGetNext = bool (*)(int, CallbackMessage*);
  using ManualFreeLast = void (*)(int);
  using ManualGetResult =
      bool (*)(int, std::uint64_t, void*, int, int, bool*);
  using GetInterface = void* (*)();
  using UserGetSteamId = std::uint64_t (*)(void*);
  using FriendsGetPersonaName = const char* (*)(void*);
  using RequestLobbyList = std::uint64_t (*)(void*);
  using AddLobbyStringFilter = void (*)(void*, const char*, const char*, int);
  using AddLobbyDistanceFilter = void (*)(void*, int);
  using AddLobbyResultCountFilter = void (*)(void*, int);
  using GetLobbyByIndex = std::uint64_t (*)(void*, int);
  using CreateLobby = std::uint64_t (*)(void*, int, int);
  using JoinLobby = std::uint64_t (*)(void*, std::uint64_t);
  using LeaveLobby = void (*)(void*, std::uint64_t);
  using GetNumLobbyMembers = int (*)(void*, std::uint64_t);
  using GetLobbyMemberByIndex =
      std::uint64_t (*)(void*, std::uint64_t, int);
  using GetLobbyData = const char* (*)(void*, std::uint64_t, const char*);
  using SetLobbyData =
      bool (*)(void*, std::uint64_t, const char*, const char*);
  using SetLobbyJoinable = bool (*)(void*, std::uint64_t, bool);
  using GetLobbyOwner = std::uint64_t (*)(void*, std::uint64_t);
  using IdentitySetSteamId = void (*)(SteamNetworkingIdentity*, std::uint64_t);
  using IdentityGetSteamId =
      std::uint64_t (*)(SteamNetworkingIdentity*);
  using SendNetworkMessage =
      int (*)(void*, SteamNetworkingIdentity*, const void*, std::uint32_t,
              int, int);
  using ReceiveNetworkMessages =
      int (*)(void*, int, SteamNetworkingMessage**, int);
  using AcceptNetworkSession =
      bool (*)(void*, SteamNetworkingIdentity*);
  using ReleaseNetworkMessage = void (*)(SteamNetworkingMessage*);

  InitFlat init_flat = nullptr;
  Shutdown shutdown = nullptr;
  GetPipe get_pipe = nullptr;
  ManualInit manual_init = nullptr;
  ManualRunFrame manual_run_frame = nullptr;
  ManualGetNext manual_get_next = nullptr;
  ManualFreeLast manual_free_last = nullptr;
  ManualGetResult manual_get_result = nullptr;
  GetInterface matchmaking_interface = nullptr;
  GetInterface user_interface = nullptr;
  GetInterface friends_interface = nullptr;
  GetInterface networking_interface = nullptr;
  UserGetSteamId user_get_steam_id = nullptr;
  FriendsGetPersonaName friends_get_persona_name = nullptr;
  RequestLobbyList request_lobby_list = nullptr;
  AddLobbyStringFilter add_lobby_string_filter = nullptr;
  AddLobbyDistanceFilter add_lobby_distance_filter = nullptr;
  AddLobbyResultCountFilter add_lobby_result_count_filter = nullptr;
  GetLobbyByIndex get_lobby_by_index = nullptr;
  CreateLobby create_lobby = nullptr;
  JoinLobby join_lobby = nullptr;
  LeaveLobby leave_lobby = nullptr;
  GetNumLobbyMembers get_num_lobby_members = nullptr;
  GetLobbyMemberByIndex get_lobby_member_by_index = nullptr;
  GetLobbyData get_lobby_data = nullptr;
  SetLobbyData set_lobby_data = nullptr;
  SetLobbyJoinable set_lobby_joinable = nullptr;
  GetLobbyOwner get_lobby_owner = nullptr;
  IdentitySetSteamId identity_set_steam_id = nullptr;
  IdentityGetSteamId identity_get_steam_id = nullptr;
  SendNetworkMessage send_network_message = nullptr;
  ReceiveNetworkMessages receive_network_messages = nullptr;
  AcceptNetworkSession accept_network_session = nullptr;
  ReleaseNetworkMessage release_network_message = nullptr;
};

struct Runtime {
  Api api;
  State state;
  void* matchmaking = nullptr;
  void* user = nullptr;
  void* friends = nullptr;
  void* networking = nullptr;
  int pipe = 0;
  std::uint64_t pending_lobby_list = 0;
  std::uint64_t pending_create = 0;
  std::uint64_t pending_join = 0;
  std::uint64_t requested_join_lobby = 0;
  std::uint64_t requested_password_hash = 0;
  std::string pending_server_name;
  std::string pending_host_name;
  std::string pending_map_name;
  std::uint32_t pending_max_players = 8;
  std::uint32_t pending_privacy = 0;
  bool pending_allow_late_join = true;
  std::uint64_t pending_password_hash = 0;
};

std::mutex g_mutex;
Runtime g_runtime;

std::filesystem::path ExecutableDirectory() {
#if defined(_WIN32)
  std::array<wchar_t, 32768> path{};
  const DWORD size = GetModuleFileNameW(
      nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (size > 0 && size < path.size()) {
    return std::filesystem::path(path.data()).parent_path();
  }
#endif
  return std::filesystem::current_path();
}

std::string LobbyData(Runtime& runtime, std::uint64_t lobby,
                      const char* key) {
  const char* value =
      runtime.api.get_lobby_data(runtime.matchmaking, lobby, key);
  return value == nullptr ? std::string{} : std::string(value);
}

std::uint32_t ParseU32(std::string_view text,
                       std::uint32_t fallback = 0) {
  std::uint32_t value = fallback;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} ? value : fallback;
}

std::uint64_t ParseU64(std::string_view text,
                       std::uint64_t fallback = 0) {
  std::uint64_t value = fallback;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} ? value : fallback;
}

std::string RoleKey(std::uint64_t steam_id) {
  return "role_" + std::to_string(steam_id);
}

bool SetLobbyData(Runtime& runtime, std::uint64_t lobby,
                  const char* key, const std::string& value) {
  return runtime.api.set_lobby_data(
      runtime.matchmaking, lobby, key, value.c_str());
}

int SteamLobbyType(std::uint32_t privacy) {
  switch (privacy) {
    case 1:
      return kLobbyFriendsOnly;
    case 2:
      return kLobbyPrivate;
    default:
      return kLobbyPublic;
  }
}

std::vector<Peer> LobbyPeersLocked(Runtime& runtime) {
  std::vector<Peer> peers;
  if (!runtime.state.in_lobby || runtime.state.lobby_id == 0) {
    return peers;
  }
  const int count = std::max(
      runtime.api.get_num_lobby_members(
          runtime.matchmaking, runtime.state.lobby_id),
      0);
  const std::uint64_t owner = runtime.api.get_lobby_owner(
      runtime.matchmaking, runtime.state.lobby_id);
  std::vector<Peer> clients;
  for (int index = 0; index < count; ++index) {
    const auto id = runtime.api.get_lobby_member_by_index(
        runtime.matchmaking, runtime.state.lobby_id, index);
    if (id != 0 && id != owner) {
      const std::uint32_t role = ParseU32(
          LobbyData(
              runtime, runtime.state.lobby_id, RoleKey(id).c_str()),
          0);
      if (role >= 2 && role <= 100) {
        clients.push_back({role, id});
      }
    }
  }
  if (owner != 0) {
    peers.push_back({1, owner});
  }
  std::sort(
      clients.begin(), clients.end(),
      [](const Peer& left, const Peer& right) {
        return left.role == right.role
                   ? left.steam_id < right.steam_id
                   : left.role < right.role;
      });
  peers.insert(peers.end(), clients.begin(), clients.end());
  return peers;
}

void AssignStableRoles(Runtime& runtime) {
  if (!runtime.state.in_lobby || !runtime.state.is_host ||
      runtime.state.lobby_id == 0) {
    return;
  }
  const int count = std::max(
      runtime.api.get_num_lobby_members(
          runtime.matchmaking, runtime.state.lobby_id),
      0);
  std::vector<std::uint64_t> clients;
  clients.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    const std::uint64_t id = runtime.api.get_lobby_member_by_index(
        runtime.matchmaking, runtime.state.lobby_id, index);
    if (id != 0 && id != runtime.state.host_steam_id) {
      clients.push_back(id);
    }
  }
  std::sort(clients.begin(), clients.end());

  std::array<bool, 101> used{};
  used[1] = true;
  std::vector<std::pair<std::uint64_t, std::uint32_t>> assignments;
  assignments.reserve(clients.size());
  for (std::uint64_t id : clients) {
    const std::uint32_t existing = ParseU32(
        LobbyData(
            runtime, runtime.state.lobby_id, RoleKey(id).c_str()),
        0);
    if (existing >= 2 && existing <= 100 && !used[existing]) {
      used[existing] = true;
      assignments.push_back({id, existing});
    } else {
      assignments.push_back({id, 0});
    }
  }
  for (auto& [id, role] : assignments) {
    if (role != 0) {
      continue;
    }
    for (std::uint32_t candidate = 2; candidate <= 100; ++candidate) {
      if (!used[candidate]) {
        role = candidate;
        used[candidate] = true;
        SetLobbyData(
            runtime, runtime.state.lobby_id, RoleKey(id).c_str(),
            std::to_string(role));
        break;
      }
    }
  }
}

void UpdateLobbyMembership(Runtime& runtime) {
  if (!runtime.state.in_lobby || runtime.state.lobby_id == 0) {
    runtime.state.local_role = 0;
    runtime.state.host_steam_id = 0;
    runtime.state.is_host = false;
    runtime.state.lobby_name.clear();
    runtime.state.lobby_host_name.clear();
    runtime.state.lobby_map_name.clear();
    runtime.state.lobby_players = 0;
    runtime.state.lobby_max_players = 0;
    return;
  }
  runtime.state.host_steam_id = runtime.api.get_lobby_owner(
      runtime.matchmaking, runtime.state.lobby_id);
  runtime.state.is_host =
      runtime.state.local_steam_id == runtime.state.host_steam_id;
  AssignStableRoles(runtime);
  runtime.state.lobby_name =
      LobbyData(runtime, runtime.state.lobby_id, "name");
  runtime.state.lobby_host_name =
      LobbyData(runtime, runtime.state.lobby_id, "host");
  runtime.state.lobby_map_name =
      LobbyData(runtime, runtime.state.lobby_id, "map");
  runtime.state.lobby_players = static_cast<std::uint32_t>(std::max(
      runtime.api.get_num_lobby_members(
          runtime.matchmaking, runtime.state.lobby_id),
      0));
  runtime.state.lobby_max_players = ParseU32(
      LobbyData(runtime, runtime.state.lobby_id, "max_players"), 8);
  runtime.state.local_role = 0;
  for (const Peer& peer : LobbyPeersLocked(runtime)) {
    if (peer.steam_id == runtime.state.local_steam_id) {
      runtime.state.local_role = peer.role;
      break;
    }
  }
}

void PopulateLobbyList(Runtime& runtime, std::uint32_t count) {
  runtime.state.lobbies.clear();
  runtime.state.lobbies.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    const std::uint64_t id = runtime.api.get_lobby_by_index(
        runtime.matchmaking, static_cast<int>(index));
    if (id == 0 || LobbyData(runtime, id, "cel_game") != kGameKey) {
      continue;
    }
    Lobby lobby;
    lobby.id = id;
    lobby.name = LobbyData(runtime, id, "name");
    lobby.host_name = LobbyData(runtime, id, "host");
    lobby.map_name = LobbyData(runtime, id, "map");
    lobby.players = static_cast<std::uint32_t>(std::max(
        runtime.api.get_num_lobby_members(runtime.matchmaking, id), 0));
    lobby.max_players =
        ParseU32(LobbyData(runtime, id, "max_players"), 8);
    lobby.privacy = ParseU32(LobbyData(runtime, id, "privacy"), 0);
    lobby.passworded =
        ParseU64(LobbyData(runtime, id, "password_hash"), 0) != 0;
    lobby.allow_late_join =
        LobbyData(runtime, id, "late_join") != "0";
    runtime.state.lobbies.push_back(std::move(lobby));
  }
  runtime.state.busy = false;
  runtime.state.status =
      runtime.state.lobbies.empty()
          ? "Steam lobby search completed; no Custom Engine Layer games found."
          : "Steam lobby search completed.";
}

template <typename T>
std::optional<T> GetCallResult(Runtime& runtime,
                               const SteamApiCallCompleted& completed,
                               int expected_callback) {
  T result{};
  bool failed = false;
  if (completed.callback != expected_callback ||
      completed.parameter_bytes != sizeof(T) ||
      !runtime.api.manual_get_result(
          runtime.pipe, completed.call, &result, sizeof(result),
          expected_callback, &failed) ||
      failed) {
    return std::nullopt;
  }
  return result;
}

void FinishCreatedLobby(Runtime& runtime, const LobbyCreated& created) {
  runtime.pending_create = 0;
  runtime.state.busy = false;
  if (created.result != kResultOk || created.lobby_id == 0) {
    runtime.state.status =
        "Steam could not create the lobby (result " +
        std::to_string(created.result) + ").";
    return;
  }
  runtime.state.in_lobby = true;
  runtime.state.lobby_id = created.lobby_id;
  SetLobbyData(runtime, created.lobby_id, "cel_game",
               std::string(kGameKey));
  SetLobbyData(runtime, created.lobby_id, "cel_protocol",
               std::to_string(kProtocolVersion));
  SetLobbyData(runtime, created.lobby_id, "name",
               runtime.pending_server_name);
  SetLobbyData(runtime, created.lobby_id, "host",
               runtime.pending_host_name);
  SetLobbyData(runtime, created.lobby_id, "map",
               runtime.pending_map_name);
  SetLobbyData(runtime, created.lobby_id, "max_players",
               std::to_string(runtime.pending_max_players));
  SetLobbyData(runtime, created.lobby_id, "privacy",
               std::to_string(runtime.pending_privacy));
  SetLobbyData(runtime, created.lobby_id, "late_join",
               runtime.pending_allow_late_join ? "1" : "0");
  SetLobbyData(runtime, created.lobby_id, "password_hash",
               std::to_string(runtime.pending_password_hash));
  runtime.api.set_lobby_joinable(
      runtime.matchmaking, created.lobby_id,
      runtime.pending_allow_late_join);
  UpdateLobbyMembership(runtime);
  runtime.state.status =
      "Steam lobby created. Friends or another Steam account can now join.";
  REXLOG_INFO(
      "steam-multiplayer: created lobby {} as Steam user {}",
      created.lobby_id, runtime.state.local_steam_id);
}

void FinishJoinedLobby(Runtime& runtime, const LobbyEnter& entered) {
  runtime.pending_join = 0;
  runtime.state.busy = false;
  if (entered.response != 1 || entered.lobby_id == 0) {
    runtime.state.status =
        "Steam lobby join failed (response " +
        std::to_string(entered.response) + ").";
    return;
  }
  runtime.state.in_lobby = true;
  runtime.state.lobby_id = entered.lobby_id;
  UpdateLobbyMembership(runtime);
  runtime.state.status = "Connected to the Steam lobby.";
  REXLOG_INFO(
      "steam-multiplayer: joined lobby {} as role {} (Steam user {})",
      entered.lobby_id, runtime.state.local_role,
      runtime.state.local_steam_id);
}

void DispatchCallback(Runtime& runtime, const CallbackMessage& message) {
  if (message.parameter == nullptr) {
    return;
  }
  if (message.callback == kCallbackSteamApiCallCompleted &&
      message.parameter_bytes >= sizeof(SteamApiCallCompleted)) {
    const auto completed =
        *static_cast<const SteamApiCallCompleted*>(message.parameter);
    if (completed.call == runtime.pending_lobby_list) {
      auto result = GetCallResult<LobbyMatchList>(
          runtime, completed, kCallbackLobbyMatchList);
      runtime.pending_lobby_list = 0;
      if (result) {
        PopulateLobbyList(runtime, result->count);
      } else {
        runtime.state.busy = false;
        runtime.state.status = "Steam lobby search failed.";
      }
    } else if (completed.call == runtime.pending_create) {
      auto result = GetCallResult<LobbyCreated>(
          runtime, completed, kCallbackLobbyCreated);
      if (result) {
        FinishCreatedLobby(runtime, *result);
      } else {
        runtime.pending_create = 0;
        runtime.state.busy = false;
        runtime.state.status = "Steam lobby creation failed.";
      }
    } else if (completed.call == runtime.pending_join) {
      auto result = GetCallResult<LobbyEnter>(
          runtime, completed, kCallbackLobbyEnter);
      if (result) {
        FinishJoinedLobby(runtime, *result);
      } else {
        runtime.pending_join = 0;
        runtime.state.busy = false;
        runtime.state.status = "Steam lobby join failed.";
      }
    }
  } else if (message.callback == kCallbackLobbyEnter &&
             message.parameter_bytes >= sizeof(LobbyEnter)) {
    const auto entered =
        *static_cast<const LobbyEnter*>(message.parameter);
    if (runtime.state.lobby_id == 0 ||
        runtime.state.lobby_id == entered.lobby_id) {
      FinishJoinedLobby(runtime, entered);
    }
  } else if (message.callback == kCallbackLobbyChatUpdate &&
             message.parameter_bytes >= sizeof(LobbyChatUpdate)) {
    const auto update =
        *static_cast<const LobbyChatUpdate*>(message.parameter);
    if (update.lobby_id == runtime.state.lobby_id) {
      UpdateLobbyMembership(runtime);
    }
  } else if (message.callback == kCallbackNetworkingSessionRequest &&
             message.parameter_bytes >=
                 sizeof(SteamNetworkingIdentity)) {
    auto identity =
        *static_cast<const SteamNetworkingIdentity*>(message.parameter);
    runtime.api.accept_network_session(
        runtime.networking, &identity);
  }
}

bool LoadApi(Runtime& runtime) {
#if defined(_WIN32)
  const auto library_path = ExecutableDirectory() / "steam_api64.dll";
  runtime.state.library_found =
      std::filesystem::is_regular_file(library_path);
  if (!runtime.state.library_found) {
    runtime.state.status =
        "steam_api64.dll is not installed beside skate3.exe.";
    return false;
  }
  runtime.api.module = LoadLibraryW(library_path.c_str());
  if (runtime.api.module == nullptr) {
    runtime.state.status = "steam_api64.dll could not be loaded.";
    return false;
  }
#define LOAD_STEAM(field, export_name)                                      \
  if (!LoadFunction(runtime.api.module, export_name, runtime.api.field)) {   \
    runtime.state.status = std::string("Steam API export missing: ") +       \
                           export_name;                                      \
    return false;                                                           \
  }
  LOAD_STEAM(init_flat, "SteamAPI_InitFlat");
  LOAD_STEAM(shutdown, "SteamAPI_Shutdown");
  LOAD_STEAM(get_pipe, "SteamAPI_GetHSteamPipe");
  LOAD_STEAM(manual_init, "SteamAPI_ManualDispatch_Init");
  LOAD_STEAM(manual_run_frame, "SteamAPI_ManualDispatch_RunFrame");
  LOAD_STEAM(manual_get_next, "SteamAPI_ManualDispatch_GetNextCallback");
  LOAD_STEAM(manual_free_last, "SteamAPI_ManualDispatch_FreeLastCallback");
  LOAD_STEAM(manual_get_result,
             "SteamAPI_ManualDispatch_GetAPICallResult");
  LOAD_STEAM(matchmaking_interface, "SteamAPI_SteamMatchmaking_v009");
  LOAD_STEAM(user_interface, "SteamAPI_SteamUser_v023");
  LOAD_STEAM(friends_interface, "SteamAPI_SteamFriends_v018");
  LOAD_STEAM(networking_interface,
             "SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
  LOAD_STEAM(user_get_steam_id, "SteamAPI_ISteamUser_GetSteamID");
  LOAD_STEAM(friends_get_persona_name,
             "SteamAPI_ISteamFriends_GetPersonaName");
  LOAD_STEAM(request_lobby_list,
             "SteamAPI_ISteamMatchmaking_RequestLobbyList");
  LOAD_STEAM(add_lobby_string_filter,
             "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter");
  LOAD_STEAM(add_lobby_distance_filter,
             "SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter");
  LOAD_STEAM(add_lobby_result_count_filter,
             "SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter");
  LOAD_STEAM(get_lobby_by_index,
             "SteamAPI_ISteamMatchmaking_GetLobbyByIndex");
  LOAD_STEAM(create_lobby, "SteamAPI_ISteamMatchmaking_CreateLobby");
  LOAD_STEAM(join_lobby, "SteamAPI_ISteamMatchmaking_JoinLobby");
  LOAD_STEAM(leave_lobby, "SteamAPI_ISteamMatchmaking_LeaveLobby");
  LOAD_STEAM(get_num_lobby_members,
             "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
  LOAD_STEAM(get_lobby_member_by_index,
             "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
  LOAD_STEAM(get_lobby_data,
             "SteamAPI_ISteamMatchmaking_GetLobbyData");
  LOAD_STEAM(set_lobby_data,
             "SteamAPI_ISteamMatchmaking_SetLobbyData");
  LOAD_STEAM(set_lobby_joinable,
             "SteamAPI_ISteamMatchmaking_SetLobbyJoinable");
  LOAD_STEAM(get_lobby_owner,
             "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
  LOAD_STEAM(identity_set_steam_id,
             "SteamAPI_SteamNetworkingIdentity_SetSteamID64");
  LOAD_STEAM(identity_get_steam_id,
             "SteamAPI_SteamNetworkingIdentity_GetSteamID64");
  LOAD_STEAM(send_network_message,
             "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
  LOAD_STEAM(receive_network_messages,
             "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
  LOAD_STEAM(accept_network_session,
             "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
  LOAD_STEAM(release_network_message,
             "SteamAPI_SteamNetworkingMessage_t_Release");
#undef LOAD_STEAM
  return true;
#else
  runtime.state.status = "Steam is currently supported only on Windows.";
  return false;
#endif
}

bool InitializeLocked(Runtime& runtime) {
  if (runtime.state.initialized) {
    return true;
  }
  if (runtime.api.module == nullptr && !LoadApi(runtime)) {
    return false;
  }
  std::array<char, 1024> error{};
  if (runtime.api.init_flat(error.data()) != 0) {
    runtime.state.status =
        error[0] == '\0'
            ? "SteamAPI initialization failed. Start Steam and sign in."
            : std::string(error.data());
    return false;
  }
  runtime.matchmaking = runtime.api.matchmaking_interface();
  runtime.user = runtime.api.user_interface();
  runtime.friends = runtime.api.friends_interface();
  runtime.networking = runtime.api.networking_interface();
  runtime.pipe = runtime.api.get_pipe();
  if (runtime.matchmaking == nullptr || runtime.user == nullptr ||
      runtime.friends == nullptr || runtime.networking == nullptr ||
      runtime.pipe == 0) {
    runtime.api.shutdown();
    runtime.state.status =
        "Steam initialized but required interfaces were unavailable.";
    return false;
  }
  runtime.api.manual_init();
  runtime.state.local_steam_id =
      runtime.api.user_get_steam_id(runtime.user);
  const char* persona =
      runtime.api.friends_get_persona_name(runtime.friends);
  runtime.state.persona_name =
      persona == nullptr ? std::string{} : std::string(persona);
  runtime.state.initialized = runtime.state.local_steam_id != 0;
  runtime.state.status =
      runtime.state.initialized
          ? "Steam connected as " + runtime.state.persona_name +
                " using development AppID 480."
          : "Steam did not return a signed-in user.";
  REXLOG_INFO(
      "steam-multiplayer: initialized={} user={} persona='{}'",
      runtime.state.initialized, runtime.state.local_steam_id,
      runtime.state.persona_name);
  return runtime.state.initialized;
}

void TickLocked(Runtime& runtime) {
  if (!InitializeLocked(runtime)) {
    return;
  }
  runtime.api.manual_run_frame(runtime.pipe);
  CallbackMessage message;
  while (runtime.api.manual_get_next(runtime.pipe, &message)) {
    DispatchCallback(runtime, message);
    runtime.api.manual_free_last(runtime.pipe);
    message = {};
  }
  if (runtime.state.in_lobby) {
    UpdateLobbyMembership(runtime);
  }
}

}  // namespace

bool Initialize() {
  std::scoped_lock lock(g_mutex);
  return InitializeLocked(g_runtime);
}

void Tick() {
  std::scoped_lock lock(g_mutex);
  TickLocked(g_runtime);
}

State GetState() {
  std::scoped_lock lock(g_mutex);
  InitializeLocked(g_runtime);
  return g_runtime.state;
}

void RefreshLobbies() {
  std::scoped_lock lock(g_mutex);
  if (!InitializeLocked(g_runtime) || g_runtime.state.busy) {
    return;
  }
  g_runtime.api.add_lobby_string_filter(
      g_runtime.matchmaking, "cel_game", kGameKey.data(),
      kLobbyComparisonEqual);
  g_runtime.api.add_lobby_string_filter(
      g_runtime.matchmaking, "cel_protocol",
      std::to_string(kProtocolVersion).c_str(),
      kLobbyComparisonEqual);
  g_runtime.api.add_lobby_distance_filter(
      g_runtime.matchmaking, kLobbyDistanceWorldwide);
  g_runtime.api.add_lobby_result_count_filter(
      g_runtime.matchmaking, 100);
  g_runtime.pending_lobby_list =
      g_runtime.api.request_lobby_list(g_runtime.matchmaking);
  g_runtime.state.busy = g_runtime.pending_lobby_list != 0;
  g_runtime.state.status =
      g_runtime.state.busy ? "Searching Steam lobbies..."
                           : "Steam lobby search could not start.";
}

bool HostLobby(const std::string& server_name,
               const std::string& host_name,
               const std::string& map_name,
               std::uint32_t max_players,
               std::uint32_t privacy,
               bool allow_late_join,
               std::uint64_t password_hash) {
  std::scoped_lock lock(g_mutex);
  if (!InitializeLocked(g_runtime) || g_runtime.state.busy ||
      g_runtime.state.in_lobby) {
    return false;
  }
  g_runtime.pending_server_name = server_name;
  g_runtime.pending_host_name =
      g_runtime.state.persona_name.empty()
          ? host_name
          : g_runtime.state.persona_name;
  g_runtime.pending_map_name = map_name;
  g_runtime.pending_max_players =
      std::clamp(max_players, 2u, 100u);
  g_runtime.pending_privacy = std::min(privacy, 2u);
  g_runtime.pending_allow_late_join = allow_late_join;
  g_runtime.pending_password_hash = password_hash;
  g_runtime.pending_create = g_runtime.api.create_lobby(
      g_runtime.matchmaking, SteamLobbyType(privacy),
      static_cast<int>(g_runtime.pending_max_players));
  g_runtime.state.busy = g_runtime.pending_create != 0;
  g_runtime.state.status =
      g_runtime.state.busy ? "Creating Steam lobby..."
                           : "Steam lobby creation could not start.";
  return g_runtime.state.busy;
}

bool JoinLobby(std::uint64_t lobby_id, std::uint64_t password_hash) {
  std::scoped_lock lock(g_mutex);
  if (!InitializeLocked(g_runtime) || g_runtime.state.busy ||
      g_runtime.state.in_lobby || lobby_id == 0) {
    return false;
  }
  const std::uint64_t expected_password =
      ParseU64(LobbyData(g_runtime, lobby_id, "password_hash"), 0);
  if (expected_password != password_hash) {
    g_runtime.state.status = "The Steam lobby password is incorrect.";
    return false;
  }
  g_runtime.requested_join_lobby = lobby_id;
  g_runtime.requested_password_hash = password_hash;
  g_runtime.pending_join =
      g_runtime.api.join_lobby(g_runtime.matchmaking, lobby_id);
  g_runtime.state.busy = g_runtime.pending_join != 0;
  g_runtime.state.status =
      g_runtime.state.busy ? "Joining Steam lobby..."
                           : "Steam lobby join could not start.";
  return g_runtime.state.busy;
}

void LeaveLobby() {
  std::scoped_lock lock(g_mutex);
  if (g_runtime.state.initialized && g_runtime.state.lobby_id != 0) {
    g_runtime.api.leave_lobby(
        g_runtime.matchmaking, g_runtime.state.lobby_id);
  }
  g_runtime.state.in_lobby = false;
  g_runtime.state.is_host = false;
  g_runtime.state.busy = false;
  g_runtime.state.lobby_id = 0;
  g_runtime.state.host_steam_id = 0;
  g_runtime.state.local_role = 0;
  g_runtime.state.lobby_name.clear();
  g_runtime.state.lobby_host_name.clear();
  g_runtime.state.lobby_map_name.clear();
  g_runtime.state.lobby_players = 0;
  g_runtime.state.lobby_max_players = 0;
  g_runtime.state.status = g_runtime.state.initialized
                               ? "Steam connected; not in a lobby."
                               : g_runtime.state.status;
}

void Shutdown() {
  std::scoped_lock lock(g_mutex);
  if (g_runtime.state.initialized && g_runtime.state.lobby_id != 0) {
    g_runtime.api.leave_lobby(
        g_runtime.matchmaking, g_runtime.state.lobby_id);
  }
  if (g_runtime.state.initialized && g_runtime.api.shutdown) {
    g_runtime.api.shutdown();
  }
#if defined(_WIN32)
  if (g_runtime.api.module != nullptr) {
    FreeLibrary(g_runtime.api.module);
  }
#endif
  g_runtime = {};
}

bool TransportActive() {
  std::scoped_lock lock(g_mutex);
  return g_runtime.state.initialized && g_runtime.state.in_lobby &&
         g_runtime.state.local_role != 0;
}

std::uint32_t LocalRole() {
  std::scoped_lock lock(g_mutex);
  return g_runtime.state.local_role;
}

std::uint64_t HostSteamId() {
  std::scoped_lock lock(g_mutex);
  return g_runtime.state.host_steam_id;
}

std::vector<Peer> LobbyPeers() {
  std::scoped_lock lock(g_mutex);
  return LobbyPeersLocked(g_runtime);
}

bool SendPacketToPeer(std::uint64_t steam_id, const void* bytes,
                      std::size_t byte_count) {
  std::scoped_lock lock(g_mutex);
  if (!g_runtime.state.initialized || !g_runtime.state.in_lobby ||
      steam_id == 0 || bytes == nullptr || byte_count == 0 ||
      byte_count > UINT32_MAX) {
    return false;
  }
  SteamNetworkingIdentity identity{};
  g_runtime.api.identity_set_steam_id(&identity, steam_id);
  return g_runtime.api.send_network_message(
             g_runtime.networking, &identity, bytes,
             static_cast<std::uint32_t>(byte_count),
             kSendUnreliableNoDelayAutoRestart, kNetworkingChannel) ==
         kResultOk;
}

std::vector<Message> ReceiveMessages(std::size_t maximum_messages) {
  std::scoped_lock lock(g_mutex);
  std::vector<Message> result;
  if (!g_runtime.state.initialized || !g_runtime.state.in_lobby ||
      maximum_messages == 0) {
    return result;
  }
  constexpr std::size_t kBatchSize = 128;
  std::array<SteamNetworkingMessage*, kBatchSize> messages{};
  while (result.size() < maximum_messages) {
    const int requested = static_cast<int>(std::min(
        kBatchSize, maximum_messages - result.size()));
    const int received = g_runtime.api.receive_network_messages(
        g_runtime.networking, kNetworkingChannel, messages.data(),
        requested);
    if (received <= 0) {
      break;
    }
    for (int index = 0; index < received; ++index) {
      SteamNetworkingMessage* source = messages[index];
      if (source != nullptr && source->data != nullptr &&
          source->byte_count > 0) {
        Message output;
        output.sender_steam_id =
            g_runtime.api.identity_get_steam_id(&source->identity);
        output.bytes.resize(
            static_cast<std::size_t>(source->byte_count));
        std::memcpy(
            output.bytes.data(), source->data, output.bytes.size());
        result.push_back(std::move(output));
      }
      if (source != nullptr) {
        g_runtime.api.release_network_message(source);
      }
    }
  }
  return result;
}

}  // namespace skate3::multiplayer::steam
