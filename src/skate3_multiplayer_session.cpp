#include "skate3_multiplayer_session.h"
#include "skate3_steam_backend.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(_WIN32)
#define NOMINMAX
#include <WinSock2.h>
#include <Windows.h>
#endif

namespace skate3::multiplayer {
namespace {

constexpr std::uint32_t kRegistryVersion = 1;
constexpr std::int32_t kDefaultBasePort = 27051;
constexpr std::int32_t kPortsPerSession = 100;

struct RegistrySession {
  std::string id;
  std::uint32_t host_pid = 0;
  std::string server_name;
  std::string host_name;
  std::string map_name;
  std::uint32_t max_players = 8;
  SessionPrivacy privacy = SessionPrivacy::kPublic;
  bool allow_late_join = true;
  std::uint64_t password_hash = 0;
  std::int32_t base_port = kDefaultBasePort;
};

struct RuntimeState {
  SessionSnapshot snapshot;
  std::optional<RegistrySession> active;
  std::filesystem::path owned_registry_file;
  std::filesystem::path owned_client_file;
  std::uint32_t local_role = 0;
};

std::mutex g_mutex;
RuntimeState g_state;

std::uint32_t CurrentPid() {
#if defined(_WIN32)
  return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
  return 0;
#endif
}

bool ProcessAlive(std::uint32_t pid) {
#if defined(_WIN32)
  if (pid == 0) {
    return false;
  }
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (process == nullptr) {
    return false;
  }
  DWORD exit_code = 0;
  const bool alive =
      GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
  CloseHandle(process);
  return alive;
#else
  return pid != 0;
#endif
}

std::filesystem::path RegistryDirectory() {
#if defined(_WIN32)
  wchar_t local_app_data[32768] = {};
  const DWORD count = GetEnvironmentVariableW(
      L"LOCALAPPDATA", local_app_data,
      static_cast<DWORD>(std::size(local_app_data)));
  if (count > 0 && count < std::size(local_app_data)) {
    return std::filesystem::path(local_app_data) /
           "Skate3CustomEngineLayer" / "MultiplayerSessions";
  }
#endif
  return std::filesystem::temp_directory_path() /
         "Skate3CustomEngineLayer" / "MultiplayerSessions";
}

std::string CleanField(std::string value, std::size_t maximum) {
  value.erase(
      std::remove_if(
          value.begin(), value.end(),
          [](char c) { return c == '\r' || c == '\n' || c == '='; }),
      value.end());
  if (value.size() > maximum) {
    value.resize(maximum);
  }
  return value;
}

std::uint64_t HashPassword(std::string_view password) {
  if (password.empty()) {
    return 0;
  }
  std::uint64_t hash = 1469598103934665603ull;
  for (unsigned char value : password) {
    hash ^= value;
    hash *= 1099511628211ull;
  }
  return hash;
}

const char* PrivacyName(SessionPrivacy privacy) {
  switch (privacy) {
    case SessionPrivacy::kFriendsOnly:
      return "friends";
    case SessionPrivacy::kInviteOnly:
      return "invite";
    default:
      return "public";
  }
}

SessionPrivacy ParsePrivacy(std::string_view value) {
  if (value == "friends") {
    return SessionPrivacy::kFriendsOnly;
  }
  if (value == "invite") {
    return SessionPrivacy::kInviteOnly;
  }
  return SessionPrivacy::kPublic;
}

bool WriteRegistrySession(const RegistrySession& session,
                          std::filesystem::path& out_path) {
  std::error_code ec;
  const auto directory = RegistryDirectory();
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    return false;
  }
  out_path = directory / (session.id + ".host");
  std::ofstream stream(out_path, std::ios::trunc);
  if (!stream) {
    return false;
  }
  stream << "version=" << kRegistryVersion << "\n"
         << "host_pid=" << session.host_pid << "\n"
         << "server_name=" << CleanField(session.server_name, 63) << "\n"
         << "host_name=" << CleanField(session.host_name, 31) << "\n"
         << "map_name=" << CleanField(session.map_name, 127) << "\n"
         << "max_players=" << session.max_players << "\n"
         << "privacy=" << PrivacyName(session.privacy) << "\n"
         << "allow_late_join=" << (session.allow_late_join ? 1 : 0) << "\n"
         << "password_hash=" << session.password_hash << "\n"
         << "base_port=" << session.base_port << "\n";
  return stream.good();
}

std::map<std::string, std::string> ReadFields(
    const std::filesystem::path& path) {
  std::map<std::string, std::string> fields;
  std::ifstream stream(path);
  std::string line;
  while (std::getline(stream, line)) {
    const auto separator = line.find('=');
    if (separator == std::string::npos) {
      continue;
    }
    fields.emplace(line.substr(0, separator), line.substr(separator + 1));
  }
  return fields;
}

template <typename T>
T NumberField(const std::map<std::string, std::string>& fields,
              const char* name, T fallback) {
  const auto iterator = fields.find(name);
  if (iterator == fields.end()) {
    return fallback;
  }
  std::istringstream stream(iterator->second);
  T result = fallback;
  stream >> result;
  return stream.fail() ? fallback : result;
}

std::optional<RegistrySession> ReadRegistrySession(
    const std::filesystem::path& path) {
  const auto fields = ReadFields(path);
  if (NumberField<std::uint32_t>(fields, "version", 0) !=
      kRegistryVersion) {
    return std::nullopt;
  }
  RegistrySession session;
  session.id = path.stem().string();
  session.host_pid =
      NumberField<std::uint32_t>(fields, "host_pid", 0);
  if (!ProcessAlive(session.host_pid)) {
    return std::nullopt;
  }
  const auto read_text = [&fields](const char* name) {
    const auto iterator = fields.find(name);
    return iterator == fields.end() ? std::string{} : iterator->second;
  };
  session.server_name = read_text("server_name");
  session.host_name = read_text("host_name");
  session.map_name = read_text("map_name");
  session.max_players = std::clamp(
      NumberField<std::uint32_t>(fields, "max_players", 8), 2u, 100u);
  session.privacy = ParsePrivacy(read_text("privacy"));
  session.allow_late_join =
      NumberField<int>(fields, "allow_late_join", 1) != 0;
  session.password_hash =
      NumberField<std::uint64_t>(fields, "password_hash", 0);
  session.base_port = NumberField<std::int32_t>(
      fields, "base_port", kDefaultBasePort);
  return session;
}

std::uint32_t CountPlayers(const RegistrySession& session) {
  std::uint32_t players = 1;
  std::error_code ec;
  const auto directory = RegistryDirectory();
  if (!std::filesystem::exists(directory, ec)) {
    return players;
  }
  const std::string prefix = session.id + ".client.";
  for (const auto& entry :
       std::filesystem::directory_iterator(directory, ec)) {
    if (ec || !entry.is_regular_file()) {
      continue;
    }
    const std::string filename = entry.path().filename().string();
    if (!filename.starts_with(prefix)) {
      continue;
    }
    const auto fields = ReadFields(entry.path());
    const auto pid = NumberField<std::uint32_t>(fields, "pid", 0);
    if (ProcessAlive(pid)) {
      ++players;
    } else {
      std::filesystem::remove(entry.path(), ec);
      ec.clear();
    }
  }
  return players;
}

std::vector<RegistrySession> DiscoverSessions() {
  std::vector<RegistrySession> sessions;
  std::error_code ec;
  const auto directory = RegistryDirectory();
  std::filesystem::create_directories(directory, ec);
  ec.clear();
  for (const auto& entry :
       std::filesystem::directory_iterator(directory, ec)) {
    if (ec || !entry.is_regular_file() ||
        entry.path().extension() != ".host") {
      continue;
    }
    auto session = ReadRegistrySession(entry.path());
    if (session) {
      sessions.push_back(std::move(*session));
    } else {
      std::filesystem::remove(entry.path(), ec);
      ec.clear();
    }
  }
  std::sort(
      sessions.begin(), sessions.end(),
      [](const RegistrySession& left, const RegistrySession& right) {
        return left.server_name < right.server_name;
      });
  return sessions;
}

bool PortAvailable(std::int32_t port) {
#if defined(_WIN32)
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    return false;
  }
  SOCKET socket_value = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_value == INVALID_SOCKET) {
    WSACleanup();
    return false;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(static_cast<u_short>(port));
  const bool available =
      bind(socket_value, reinterpret_cast<const sockaddr*>(&address),
           sizeof(address)) != SOCKET_ERROR;
  closesocket(socket_value);
  WSACleanup();
  return available;
#else
  (void)port;
  return true;
#endif
}

std::optional<std::int32_t> FindHostBasePort() {
  for (std::int32_t base = kDefaultBasePort;
       base + kPortsPerSession - 1 <= 65436;
       base += kPortsPerSession) {
    if (PortAvailable(base)) {
      return base;
    }
  }
  return std::nullopt;
}

std::optional<std::uint32_t> FindClientRole(
    const RegistrySession& session) {
  for (std::uint32_t role = 2; role <= session.max_players; ++role) {
    if (PortAvailable(session.base_port + static_cast<int>(role) - 1)) {
      return role;
    }
  }
  return std::nullopt;
}

void SetLocalTransport(bool enabled, std::uint32_t role,
                       std::int32_t base_port) {
  rex::cvar::SetFlagByName(
      "skate3_multiplayer_local_base_port", std::to_string(base_port));
  rex::cvar::SetFlagByName(
      "skate3_multiplayer_local_client", std::to_string(role));
  rex::cvar::SetFlagByName(
      "skate3_multiplayer_local_visuals", enabled ? "true" : "false");
}

void RemoveOwnedFiles() {
  std::error_code ec;
  if (!g_state.owned_registry_file.empty()) {
    std::filesystem::remove(g_state.owned_registry_file, ec);
  }
  ec.clear();
  if (!g_state.owned_client_file.empty()) {
    std::filesystem::remove(g_state.owned_client_file, ec);
  }
  g_state.owned_registry_file.clear();
  g_state.owned_client_file.clear();
}

void ResetActiveState() {
  RemoveOwnedFiles();
  SetLocalTransport(false, 0, kDefaultBasePort);
  g_state.active.reset();
  g_state.local_role = 0;
  g_state.snapshot.phase = SessionPhase::kOffline;
  g_state.snapshot.is_host = false;
  g_state.snapshot.status = "Not connected.";
  g_state.snapshot.session_id.clear();
  g_state.snapshot.session_name.clear();
  g_state.snapshot.host_name.clear();
  g_state.snapshot.map_name.clear();
  g_state.snapshot.players = 0;
  g_state.snapshot.max_players = 0;
}

void PopulateBrowser(const std::string& active_map) {
  g_state.snapshot.servers.clear();
  for (const RegistrySession& session : DiscoverSessions()) {
    ServerListing listing;
    listing.id = session.id;
    listing.name = session.server_name;
    listing.host_name = session.host_name;
    listing.map_name = session.map_name;
    listing.players = CountPlayers(session);
    listing.max_players = session.max_players;
    listing.privacy = session.privacy;
    listing.passworded = session.password_hash != 0;
    listing.compatible =
        session.allow_late_join &&
        listing.players < listing.max_players &&
        (active_map.empty() || session.map_name.empty() ||
         session.map_name == active_map);
    if (!session.allow_late_join) {
      listing.compatibility_note = "Late joining is disabled.";
    } else if (listing.players >= listing.max_players) {
      listing.compatibility_note = "Server is full.";
    } else if (!active_map.empty() && !session.map_name.empty() &&
               session.map_name != active_map) {
      listing.compatibility_note =
          "Load map '" + session.map_name + "' before joining.";
    }
    g_state.snapshot.servers.push_back(std::move(listing));
  }
}

void PopulateSteamBrowser(const steam::State& state,
                          const std::string& active_map) {
  g_state.snapshot.servers.clear();
  for (const steam::Lobby& lobby : state.lobbies) {
    ServerListing listing;
    listing.id = std::to_string(lobby.id);
    listing.name =
        lobby.name.empty() ? "Steam Lobby " + listing.id : lobby.name;
    listing.host_name = lobby.host_name;
    listing.map_name = lobby.map_name;
    listing.players = lobby.players;
    listing.max_players = lobby.max_players;
    listing.privacy = static_cast<SessionPrivacy>(
        std::min(lobby.privacy, 2u));
    listing.passworded = lobby.passworded;
    listing.compatible =
        lobby.allow_late_join &&
        listing.players < listing.max_players &&
        (active_map.empty() || lobby.map_name.empty() ||
         lobby.map_name == active_map);
    if (!lobby.allow_late_join) {
      listing.compatibility_note = "Late joining is disabled.";
    } else if (listing.players >= listing.max_players) {
      listing.compatibility_note = "Server is full.";
    } else if (!active_map.empty() && !lobby.map_name.empty() &&
               lobby.map_name != active_map) {
      listing.compatibility_note =
          "Load map '" + lobby.map_name + "' before joining.";
    }
    g_state.snapshot.servers.push_back(std::move(listing));
  }
}

std::optional<std::uint64_t> ParseSteamLobbyId(
    std::string_view text) {
  std::uint64_t value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} ||
      result.ptr != text.data() + text.size() || value == 0) {
    return std::nullopt;
  }
  return value;
}

SessionSnapshot SnapshotLocked(const std::string& active_map,
                               bool refresh) {
  const steam::State steam_state = steam::GetState();
  g_state.snapshot.steam_available = steam_state.initialized;
  g_state.snapshot.backend_name =
      steam_state.initialized ? "Steam P2P (Spacewar / App 480)"
                              : "Local PC Test";
  g_state.snapshot.steam_status = steam_state.status;
  if (steam_state.initialized) {
    if (refresh) {
      steam::RefreshLobbies();
    }
    PopulateSteamBrowser(steam_state, active_map);
    if (steam_state.in_lobby) {
      g_state.snapshot.phase =
          steam_state.is_host ? SessionPhase::kHosting
                              : SessionPhase::kConnected;
      g_state.snapshot.is_host = steam_state.is_host;
      g_state.snapshot.status = steam_state.status;
      g_state.snapshot.session_id =
          std::to_string(steam_state.lobby_id);
      g_state.snapshot.session_name = steam_state.lobby_name;
      g_state.snapshot.host_name = steam_state.lobby_host_name;
      g_state.snapshot.map_name = steam_state.lobby_map_name;
      g_state.snapshot.players = steam_state.lobby_players;
      g_state.snapshot.max_players = steam_state.lobby_max_players;
    } else if (!g_state.active) {
      g_state.snapshot.phase =
          steam_state.status.find("failed") != std::string::npos
              ? SessionPhase::kError
              : SessionPhase::kOffline;
      g_state.snapshot.is_host = false;
      g_state.snapshot.status = steam_state.status;
      g_state.snapshot.session_id.clear();
      g_state.snapshot.session_name.clear();
      g_state.snapshot.host_name.clear();
      g_state.snapshot.map_name.clear();
      g_state.snapshot.players = 0;
      g_state.snapshot.max_players = 0;
    }
    return g_state.snapshot;
  }
  if (refresh) {
    PopulateBrowser(active_map);
  }
  if (g_state.active) {
    g_state.snapshot.players = CountPlayers(*g_state.active);
  }
  return g_state.snapshot;
}

}  // namespace

SessionSnapshot GetSessionSnapshot(const std::string& active_map) {
  steam::Tick();
  std::scoped_lock lock(g_mutex);
  return SnapshotLocked(active_map, false);
}

SessionSnapshot RefreshServerBrowser(const std::string& active_map) {
  steam::Tick();
  std::scoped_lock lock(g_mutex);
  return SnapshotLocked(active_map, true);
}

bool HostSession(const HostSettings& settings) {
  steam::Tick();
  std::scoped_lock lock(g_mutex);
  const steam::State steam_state = steam::GetState();
  if (steam_state.initialized) {
    steam::LeaveLobby();
    ResetActiveState();
    const bool started = steam::HostLobby(
        CleanField(settings.server_name.empty()
                       ? settings.host_name + "'s Game"
                       : settings.server_name,
                   63),
        CleanField(settings.host_name, 31),
        CleanField(settings.map_name, 127),
        std::clamp(settings.max_players, 2u, 100u),
        static_cast<std::uint32_t>(settings.privacy),
        settings.allow_late_join, HashPassword(settings.password));
    g_state.snapshot.status =
        started ? "Creating Steam lobby..."
                : steam::GetState().status;
    return started;
  }
  ResetActiveState();
  const auto base_port = FindHostBasePort();
  if (!base_port) {
    g_state.snapshot.phase = SessionPhase::kError;
    g_state.snapshot.status =
        "Could not reserve a local multiplayer port.";
    return false;
  }

  RegistrySession session;
  session.host_pid = CurrentPid();
  session.id = std::to_string(session.host_pid) + "-" +
               std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count());
  session.server_name =
      CleanField(settings.server_name.empty()
                     ? settings.host_name + "'s Game"
                     : settings.server_name,
                 63);
  session.host_name = CleanField(settings.host_name, 31);
  session.map_name = CleanField(settings.map_name, 127);
  session.max_players = std::clamp(settings.max_players, 2u, 100u);
  session.privacy = settings.privacy;
  session.allow_late_join = settings.allow_late_join;
  session.password_hash = HashPassword(settings.password);
  session.base_port = *base_port;
  if (!WriteRegistrySession(session, g_state.owned_registry_file)) {
    g_state.snapshot.phase = SessionPhase::kError;
    g_state.snapshot.status =
        "Could not create the local server-browser entry.";
    return false;
  }

  SetLocalTransport(true, 1, session.base_port);
  g_state.active = session;
  g_state.local_role = 1;
  g_state.snapshot.phase = SessionPhase::kHosting;
  g_state.snapshot.is_host = true;
  g_state.snapshot.status =
      "Hosting locally. Other clients on this PC can now refresh and join.";
  g_state.snapshot.session_id = session.id;
  g_state.snapshot.session_name = session.server_name;
  g_state.snapshot.host_name = session.host_name;
  g_state.snapshot.map_name = session.map_name;
  g_state.snapshot.players = 1;
  g_state.snapshot.max_players = session.max_players;
  PopulateBrowser(session.map_name);
  REXLOG_INFO(
      "multiplayer-session: hosting '{}' as role 1 on local base port {} "
      "(max={} map='{}')",
      session.server_name, session.base_port, session.max_players,
      session.map_name);
  return true;
}

bool JoinSession(const std::string& server_id, const std::string& password,
                 const std::string& active_map) {
  steam::Tick();
  std::scoped_lock lock(g_mutex);
  const steam::State steam_state = steam::GetState();
  if (steam_state.initialized) {
    const auto lobby_id = ParseSteamLobbyId(server_id);
    if (!lobby_id) {
      g_state.snapshot.phase = SessionPhase::kError;
      g_state.snapshot.status = "That Steam lobby ID is invalid.";
      return false;
    }
    steam::LeaveLobby();
    ResetActiveState();
    const bool started =
        steam::JoinLobby(*lobby_id, HashPassword(password));
    g_state.snapshot.status =
        started ? "Joining Steam lobby..."
                : steam::GetState().status;
    return started;
  }
  ResetActiveState();
  std::optional<RegistrySession> target;
  for (const RegistrySession& session : DiscoverSessions()) {
    if (session.id == server_id) {
      target = session;
      break;
    }
  }
  if (!target) {
    g_state.snapshot.phase = SessionPhase::kError;
    g_state.snapshot.status =
        "That server is no longer available. Refresh the list.";
    return false;
  }
  const auto players = CountPlayers(*target);
  if (!target->allow_late_join || players >= target->max_players) {
    g_state.snapshot.phase = SessionPhase::kError;
    g_state.snapshot.status =
        players >= target->max_players ? "That server is full."
                                       : "Late joining is disabled.";
    return false;
  }
  if (!active_map.empty() && !target->map_name.empty() &&
      active_map != target->map_name) {
    g_state.snapshot.phase = SessionPhase::kError;
    g_state.snapshot.status =
        "Load map '" + target->map_name + "' before joining.";
    return false;
  }
  if (target->password_hash != HashPassword(password)) {
    g_state.snapshot.phase = SessionPhase::kError;
    g_state.snapshot.status = "The server password is incorrect.";
    return false;
  }
  const auto role = FindClientRole(*target);
  if (!role) {
    g_state.snapshot.phase = SessionPhase::kError;
    g_state.snapshot.status =
        "No free local client slot could be reserved.";
    return false;
  }

  std::error_code ec;
  std::filesystem::create_directories(RegistryDirectory(), ec);
  g_state.owned_client_file =
      RegistryDirectory() /
      (target->id + ".client." + std::to_string(CurrentPid()));
  std::ofstream marker(g_state.owned_client_file, std::ios::trunc);
  marker << "pid=" << CurrentPid() << "\nrole=" << *role << "\n";
  if (!marker.good()) {
    g_state.snapshot.phase = SessionPhase::kError;
    g_state.snapshot.status = "Could not register this local client.";
    return false;
  }

  SetLocalTransport(true, *role, target->base_port);
  g_state.active = *target;
  g_state.local_role = *role;
  g_state.snapshot.phase = SessionPhase::kConnected;
  g_state.snapshot.is_host = false;
  g_state.snapshot.status =
      "Connected to the local test session as player " +
      std::to_string(*role) + ".";
  g_state.snapshot.session_id = target->id;
  g_state.snapshot.session_name = target->server_name;
  g_state.snapshot.host_name = target->host_name;
  g_state.snapshot.map_name = target->map_name;
  g_state.snapshot.players = CountPlayers(*target);
  g_state.snapshot.max_players = target->max_players;
  PopulateBrowser(active_map);
  REXLOG_INFO(
      "multiplayer-session: joined '{}' as local role {} on base port {}",
      target->server_name, *role, target->base_port);
  return true;
}

void LeaveSession() {
  steam::Tick();
  std::scoped_lock lock(g_mutex);
  steam::LeaveLobby();
  if (g_state.active) {
    REXLOG_INFO(
        "multiplayer-session: leaving '{}' (role={})",
        g_state.active->server_name, g_state.local_role);
  }
  ResetActiveState();
}

void ShutdownSessions() {
  LeaveSession();
  steam::Shutdown();
}

}  // namespace skate3::multiplayer
