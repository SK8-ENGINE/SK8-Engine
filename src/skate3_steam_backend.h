#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace skate3::multiplayer::steam {

struct Lobby {
  std::uint64_t id = 0;
  std::string name;
  std::string host_name;
  std::string map_name;
  std::uint32_t players = 0;
  std::uint32_t max_players = 0;
  std::uint32_t privacy = 0;
  bool passworded = false;
  bool allow_late_join = true;
};

struct State {
  bool library_found = false;
  bool initialized = false;
  bool busy = false;
  bool in_lobby = false;
  bool is_host = false;
  std::string status;
  std::string persona_name;
  std::uint64_t local_steam_id = 0;
  std::uint64_t lobby_id = 0;
  std::uint64_t host_steam_id = 0;
  std::uint32_t local_role = 0;
  std::string lobby_name;
  std::string lobby_host_name;
  std::string lobby_map_name;
  std::uint32_t lobby_players = 0;
  std::uint32_t lobby_max_players = 0;
  std::vector<Lobby> lobbies;
};

struct Peer {
  std::uint32_t role = 0;
  std::uint64_t steam_id = 0;
};

struct Message {
  std::uint64_t sender_steam_id = 0;
  std::vector<std::byte> bytes;
};

bool Initialize();
bool IsInitialized();
void Tick();
State GetState();
void RefreshLobbies();
bool HostLobby(const std::string& server_name,
               const std::string& host_name,
               const std::string& map_name,
               std::uint32_t max_players,
               std::uint32_t privacy,
               bool allow_late_join,
               std::uint64_t password_hash);
bool JoinLobby(std::uint64_t lobby_id, std::uint64_t password_hash);
void LeaveLobby();
void Shutdown();

bool TransportActive();
std::uint32_t LocalRole();
std::uint64_t HostSteamId();
std::vector<Peer> LobbyPeers();
bool SendPacketToPeer(std::uint64_t steam_id, const void* bytes,
                      std::size_t byte_count, bool reliable);
std::vector<Message> ReceiveMessages(std::size_t maximum_messages);

}  // namespace skate3::multiplayer::steam
