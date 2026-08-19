#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace skate3::multiplayer {

enum class SessionPhase {
  kOffline,
  kHosting,
  kConnected,
  kError,
};

enum class SessionPrivacy {
  kPublic,
  kFriendsOnly,
  kInviteOnly,
};

struct HostSettings {
  std::string server_name;
  std::string password;
  std::string host_name;
  std::string map_name;
  std::uint32_t max_players = 8;
  SessionPrivacy privacy = SessionPrivacy::kPublic;
  bool allow_late_join = true;
};

struct ServerListing {
  std::string id;
  std::string name;
  std::string host_name;
  std::string map_name;
  std::uint32_t players = 1;
  std::uint32_t max_players = 8;
  SessionPrivacy privacy = SessionPrivacy::kPublic;
  bool passworded = false;
  bool compatible = true;
  std::string compatibility_note;
  std::uint32_t ping_ms = 0;
};

struct SessionSnapshot {
  SessionPhase phase = SessionPhase::kOffline;
  bool is_host = false;
  bool steam_available = false;
  std::string backend_name = "Local PC Test";
  std::string steam_status;
  std::string status = "Not connected.";
  std::string session_id;
  std::string session_name;
  std::string host_name;
  std::string map_name;
  std::uint32_t players = 0;
  std::uint32_t max_players = 0;
  std::vector<ServerListing> servers;
};

// Temporary same-PC discovery and lifecycle for validating the complete
// menu/session flow. The session model deliberately does not expose sockets;
// Steam lobbies and Steam Networking Messages can replace this backend
// without changing the settings overlay.
SessionSnapshot GetSessionSnapshot(const std::string& active_map);
SessionSnapshot RefreshServerBrowser(const std::string& active_map);
bool HostSession(const HostSettings& settings);
bool JoinSession(const std::string& server_id, const std::string& password,
                 const std::string& active_map);
void LeaveSession();
void ShutdownSessions();

}  // namespace skate3::multiplayer
