#pragma once

#include <NetworkPlugin/NetworkPluginDLL.h>
#include <NetworkPlugin/Core/NetworkTypes.h>

#include <Foundation/Containers/Set.h>
#include <Foundation/Time/Time.h>

class plNetworkWorldModule;

/// @brief State of the level loading process
struct plNetworkLevelState
{
  using StorageType = plUInt8;

  enum Enum : plUInt8
  {
    None = 0,       ///< No level load in progress
    Loading,        ///< Level load requested, waiting for clients
    Ready,          ///< All clients loaded (or timed out), level is playable
    Default = None
  };
};

/// @brief Manages synchronized level loading across server and clients
///
/// Server flow:
///   1. Call RequestLevelLoad(path) to send LevelLoad to all clients
///   2. Clients receive LevelLoad, begin async load, then send LevelLoadComplete
///   3. When all clients ack (or timeout), server sends LevelStart
///
/// Late-joiner flow:
///   On new client connect, if a level is active, server sends LevelLoad for current level.
///   Client loads and acks; server sends LevelStart to that client only.
class PL_NETWORKPLUGIN_DLL plNetworkLevelManager
{
public:
  plNetworkLevelManager();
  ~plNetworkLevelManager();

  /// @brief Initialize with the network module
  void Initialize(plNetworkWorldModule* pNetworkModule);

  /// @brief Shutdown and reset state
  void Shutdown();

  /// @brief Tick - checks timeout on server side
  void Update(float fDeltaSeconds);

  //////////////////////////////////////////////////////////////////////////
  // Server API
  //////////////////////////////////////////////////////////////////////////

  /// @brief (Server) Request all clients to load a level
  /// @param sLevelPath Asset path of the level to load
  /// @param fTimeoutSeconds Max seconds to wait for all acks (0 = no timeout)
  void RequestLevelLoad(const plString& sLevelPath, float fTimeoutSeconds = 30.0f);

  /// @brief (Server) Handle a LevelLoadComplete message from a client
  void HandleLevelLoadComplete(plUInt32 uiClientID, plUInt64 uiGeneration);

  /// @brief (Server) Sync level state to a late-joining client
  void SyncLevelToClient(plUInt32 uiClientID);

  //////////////////////////////////////////////////////////////////////////
  // Client API
  //////////////////////////////////////////////////////////////////////////

  /// @brief (Client) Handle an incoming LevelLoad message
  /// The game should connect to OnLevelLoadRequested to actually load the level,
  /// then call NotifyLoadComplete() when done.
  void HandleLevelLoadMessage(const plString& sLevelPath, plUInt64 uiGeneration);

  /// @brief (Client) Notify the server that level loading is complete
  void NotifyLoadComplete();

  /// @brief (Client) Handle an incoming LevelStart message
  void HandleLevelStartMessage(plUInt64 uiGeneration);

  //////////////////////////////////////////////////////////////////////////
  // State Queries
  //////////////////////////////////////////////////////////////////////////

  /// @brief Get the current level state
  PL_NODISCARD plNetworkLevelState::Enum GetState() const { return m_State; }

  /// @brief Get the path of the currently loading or loaded level
  PL_NODISCARD const plString& GetCurrentLevelPath() const { return m_sCurrentLevelPath; }
  plUInt64 GetLoadGeneration() const { return m_uiLoadGeneration; }

  /// @brief (Server) Check if all connected clients have finished loading
  PL_NODISCARD bool AreAllClientsLoaded() const;

private:
  /// @brief (Server) Send LevelStart to all connected clients
  void BroadcastLevelStart();

  /// @brief (Server) Send LevelStart to a specific client
  void SendLevelStartToClient(plUInt32 uiClientID);

  plNetworkWorldModule* m_pNetworkModule = nullptr;

  plNetworkLevelState::Enum m_State = plNetworkLevelState::None;
  plString m_sCurrentLevelPath;
  plUInt64 m_uiLoadGeneration = 0;

  // Server-side tracking
  plSet<plUInt32> m_PendingClients;    ///< Clients that have not yet acked load
  plSet<plUInt32> m_LoadedClients;     ///< Clients that have acked load
  float m_fTimeoutSeconds = 30.0f;
  float m_fTimeoutAccumulator = 0.0f;

};
