#pragma once

#include <NetworkPlugin/NetworkPluginDLL.h>
#include <NetworkPlugin/Core/NetworkTypes.h>
#include <NetworkPlugin/Core/NetworkClient.h>

#include <Foundation/Containers/HashTable.h>

class plNetworkWorldModule;

/// @brief Session lifecycle states
struct plNetworkSessionState
{
  using StorageType = plUInt8;

  enum Enum : plUInt8
  {
    None = 0,       ///< No session active
    Lobby,          ///< In lobby, waiting for players
    Loading,        ///< Level is loading
    InGame,         ///< Gameplay is active
    PostGame,       ///< Post-game screen (scoreboards, etc.)
    Migrating,      ///< Host migration in progress (reserved for future)
    Default = None
  };
};

/// @brief High-level session management for multiplayer games.
///
/// Wraps the lower-level NetworkWorldModule connection APIs and adds
/// session concepts: lobby state, ready checks, player tracking,
/// and lifecycle callbacks.
///
/// On the **host/server** the session is authoritative: only the session leader
/// (the host) can call StartGame(). Ready state is tracked in memory on the host.
///
/// On **clients** the session mirrors state received from the host via the
/// OnSessionStateChanged callback.
class PL_NETWORKPLUGIN_DLL plNetworkSession
{
public:
  plNetworkSession();
  ~plNetworkSession();

  /// @brief Initialize with the network world module
  void Initialize(plNetworkWorldModule* pNetworkModule);

  /// @brief Shutdown and reset all state
  void Shutdown();

  //////////////////////////////////////////////////////////////////////////
  // Session Lifecycle
  //////////////////////////////////////////////////////////////////////////

  /// @brief Create a new session as host
  /// @param config Network configuration (port, limits, etc.)
  /// @return True if the session was created successfully
  bool CreateSession(const plNetworkConfig& config);

  /// @brief Join an existing session as a client
  /// @param config Network configuration (server address, port, etc.)
  /// @return True if the connection attempt started
  bool JoinSession(const plNetworkConfig& config);

  /// @brief Leave the current session
  void LeaveSession();

  //////////////////////////////////////////////////////////////////////////
  // Session Properties
  //////////////////////////////////////////////////////////////////////////

  /// @brief Set the maximum number of players for this session
  void SetMaxPlayers(plUInt32 uiMaxPlayers) { m_uiMaxPlayers = uiMaxPlayers; }

  /// @brief Get the maximum number of players
  PL_NODISCARD plUInt32 GetMaxPlayers() const { return m_uiMaxPlayers; }

  /// @brief Set session visibility (for future matchmaking integration)
  void SetSessionVisibility(bool bPublic) { m_bPublicSession = bPublic; }

  /// @brief Get session visibility
  PL_NODISCARD bool IsPublicSession() const { return m_bPublicSession; }

  /// @brief Get the current session state
  PL_NODISCARD plNetworkSessionState::Enum GetSessionState() const { return m_State; }

  /// @brief Set the session state (typically called by the host)
  void SetSessionState(plNetworkSessionState::Enum state);

  /// @brief Get a list of connected player client IDs
  PL_NODISCARD const plDynamicArray<plUInt32>& GetPlayerList() const { return m_PlayerList; }

  /// @brief Get the number of connected players
  PL_NODISCARD plUInt32 GetPlayerCount() const { return m_PlayerList.GetCount(); }

  //////////////////////////////////////////////////////////////////////////
  // Ready State
  //////////////////////////////////////////////////////////////////////////

  /// @brief Set the ready state for a player (host tracks this)
  /// On a client, this sends the ready state to the server.
  void SetReady(plUInt32 uiClientID, bool bReady);

  /// @brief Convenience: set the local player's ready state
  void SetLocalReady(bool bReady);

  /// @brief Check if a specific player is ready
  PL_NODISCARD bool IsPlayerReady(plUInt32 uiClientID) const;

  /// @brief Check if all connected players are ready
  PL_NODISCARD bool AreAllPlayersReady() const;

  /// @brief Check if the local player is the session leader (host)
  PL_NODISCARD bool IsSessionLeader() const;

  //////////////////////////////////////////////////////////////////////////
  // Game Flow
  //////////////////////////////////////////////////////////////////////////

  /// @brief Start the game (leader only).
  /// Transitions from Lobby to Loading.
  /// If a LevelManager is available and sLevelPath is provided, triggers level load.
  /// @param sLevelPath Optional level to load. Empty = just transition state.
  /// @return True if state transition was valid (was in Lobby).
  bool StartGame(const plString& sLevelPath = plString());

  //////////////////////////////////////////////////////////////////////////
  // Internal - called by NetworkWorldModule
  //////////////////////////////////////////////////////////////////////////

  /// @brief Notify session that a client connected
  void OnClientConnected(plUInt32 uiClientID);

  /// @brief Notify session that a client disconnected
  void OnClientDisconnected(plUInt32 uiClientID);

private:
  plNetworkWorldModule* m_pNetworkModule = nullptr;

  plNetworkSessionState::Enum m_State = plNetworkSessionState::None;
  plUInt32 m_uiMaxPlayers = 16;
  bool m_bPublicSession = false;

  /// @brief List of connected player client IDs
  plDynamicArray<plUInt32> m_PlayerList;

  /// @brief Ready state per client ID (host-authoritative)
  plHashTable<plUInt32, bool> m_ReadyStates;

};
