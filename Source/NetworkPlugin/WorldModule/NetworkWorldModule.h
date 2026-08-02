#pragma once

#include <NetworkPlugin/NetworkPluginDLL.h>

#include <Core/World/WorldModule.h>
#include <NetworkPlugin/Core/NetworkClient.h>
#include <NetworkPlugin/Core/NetworkPeer.h>
#include <NetworkPlugin/Core/NetworkTypes.h>
#include <NetworkPlugin/Core/NetworkObjectManager.h>
#include <NetworkPlugin/Core/NetworkRpc.h>
#include <NetworkPlugin/Core/NetworkBandwidth.h>
#include <NetworkPlugin/Core/NetworkTime.h>
#include <NetworkPlugin/Core/NetworkLagCompensation.h>
#include <NetworkPlugin/Core/NetworkLevelManager.h>
#include <NetworkPlugin/Core/NetworkSession.h>
#include <NetworkPlugin/Core/NetworkMessages.h>
#include <NetworkPlugin/Core/NetworkPrediction.h>

class plNetworkComponent;

struct PL_NETWORKPLUGIN_DLL plNetworkClientConnectionData
{
public:
  plNetworkClientConnectionData()
  : m_pClient(nullptr), m_Result(PL_SUCCESS), m_sPlatformName("Unknown")
  {
  }

  plNetworkClientConnectionData(plNetworkClient* pClient, plResult Result, const plString& sPlatformName, const plDynamicArray<plUInt8>& pData)
    : m_pClient(pClient), m_Result(Result), m_sPlatformName(sPlatformName), m_pData(pData) {}
  plNetworkClient* m_pClient;
  plResult m_Result;
  plString m_sPlatformName;
  plDynamicArray<plUInt8> m_pData;
};

struct PL_NETWORKPLUGIN_DLL plNetworkClientConnecting
{
public:
  plNetworkClientConnecting() : m_ID(plNetworkMessageIDs::Handshake), m_uiProtocolVersion(NETWORK_PROTOCOL_VERSION), m_uiGameVersion(0), m_uiPayloadSize(0)
  {
    plMemoryUtils::ZeroFill(m_szPlatformName, 32);
  }
  plNetworkMessageIDs::Enum m_ID;
  plUInt32 m_uiProtocolVersion;
  plUInt32 m_uiGameVersion;
  char m_szPlatformName[32]; // Fixed-size for POD serialization
  plUInt16 m_uiPayloadSize;
};

struct PL_NETWORKPLUGIN_DLL plNetworkClientConnectingReply
{
public:
  plNetworkClientConnectingReply() : m_uiClientID(0), m_bSuccess(false), m_Reason(plNetworkDisconnectReason::Unknown), m_uiProtocolVersion(NETWORK_PROTOCOL_VERSION) {}
  plUInt32 m_uiClientID;
  bool m_bSuccess;
  plNetworkDisconnectReason::Enum m_Reason;
  plUInt32 m_uiProtocolVersion;
};

struct PL_NETWORKPLUGIN_DLL plNetworkReplicationMessage
{
  plNetworkReplicationMessage() : m_uiClientID(0), m_uiUUID(0) {}
  plUInt32 m_uiClientID;
  plUInt32 m_uiUUID;
};

class plNetworkWorldModule;

class PL_NETWORKPLUGIN_DLL plNetworkObject
{
public:
  virtual ~plNetworkObject(){}
  virtual void OnNetworkStateChanged(plNetworkConnectionState::Enum state) {}
  virtual void OnClientConnecting(plNetworkClientConnectionData& pClient) {}
  virtual void OnClientConnected(plNetworkClient* pClient) {}
  virtual void OnClientDisconnected(plNetworkClient* pClient) {}
  virtual void OnNetworkMessage(plNetworkMessage& event) {}

  void SetNetID(plUInt32 uiNetNetID) { m_uiNetID = uiNetNetID; }
  plUInt32 GetNetID() const { return m_uiNetID; }

private:
  plUInt32 m_uiNetID = -1;
};

class PL_NETWORKPLUGIN_DLL plNetworkWorldModule : public plWorldModule
{
  PL_DECLARE_WORLD_MODULE();
  PL_ADD_DYNAMIC_REFLECTION(plNetworkWorldModule, plWorldModule);

public:
  explicit plNetworkWorldModule(plWorld* pWorld);
  ~plNetworkWorldModule() override;

  void Initialize() override;

  PL_NODISCARD PL_FORCE_INLINE float GetTickRate() const { return m_fNetworkTickRate; }
  PL_FORCE_INLINE void SetTickRate(float fTickRate) { m_fNetworkTickRate = fTickRate; }

  PL_FORCE_INLINE void SetGameVersion(plUInt32 uiGameVersion) { m_uiGameVersion = uiGameVersion; }

  /// @brief Set the network configuration (address, port, connection limit, etc.)
  /// Must be called before StartServer(), StartClient(), or StartHost()
  void SetNetworkConfig(const plNetworkConfig& config) { m_Config = config; }
  
  /// @brief Get the current network configuration
  PL_NODISCARD const plNetworkConfig& GetNetworkConfig() const { return m_Config; }

  PL_NODISCARD PL_FORCE_INLINE plNetworkPeer* GetPeer() const { return m_pPeer; }
  PL_NODISCARD PL_FORCE_INLINE plEnum<plNetworkRoleEnum> GetRole() const { return m_Role; }
  PL_NODISCARD PL_FORCE_INLINE plEnum<plNetworkConnectionState> GetStatus() const { return m_Status; }
  PL_NODISCARD PL_FORCE_INLINE plUInt32 GetServerClientID() const { return m_uiServerClientID; }
  PL_NODISCARD PL_FORCE_INLINE plUInt32 GetLocalClientID() const { return m_uiLocalClientID; }
  PL_NODISCARD PL_FORCE_INLINE plNetworkClient* GetLocalClient() const { return m_pLocalClient; }
  PL_NODISCARD PL_FORCE_INLINE plDynamicArray<plNetworkClient*>& GetClients() { return m_pClients; }

  PL_NODISCARD PL_FORCE_INLINE bool IsClient() const { return m_Role == plNetworkRoleEnum::Client; }
  PL_NODISCARD PL_FORCE_INLINE bool IsServer() const { return m_Role == plNetworkRoleEnum::Server; }
  PL_NODISCARD PL_FORCE_INLINE bool IsHost() const { return m_Role == plNetworkRoleEnum::Host; }
  PL_NODISCARD PL_FORCE_INLINE bool IsConnected() const { return m_Status == plNetworkConnectionState::Connected; }
  PL_NODISCARD PL_FORCE_INLINE bool IsOffline() const { return m_Status == plNetworkConnectionState::Disconnected || m_Status == plNetworkConnectionState::Offline; }

  void LogInfo();

  plNetworkClient* GetClient(plUInt32 uiClientID);
  plNetworkClient* GetClient(const plNetworkConnection& connection);

  bool StartServer();
  bool StartClient();
  bool StartHost();

  void RegisterNetworkComponent(plNetworkComponent* pComponent);
  void RemoveNetworkComponent(plNetworkComponent* pComponent);

  void RegisterNetworkObject(plNetworkObject* pNetObject);
  void RemoveNetworkObject(plNetworkObject* pNetObject);

  //////////////////////////////////////////////////////////////////////////
  // Object & RPC Managers
  //////////////////////////////////////////////////////////////////////////

  /// @brief Get the network object manager for spawn/despawn operations
  PL_NODISCARD PL_FORCE_INLINE plNetworkObjectManager* GetObjectManager() { return &m_ObjectManager; }
  PL_NODISCARD PL_FORCE_INLINE const plNetworkObjectManager* GetObjectManager() const { return &m_ObjectManager; }

  /// @brief Get the RPC manager for remote procedure calls
  PL_NODISCARD PL_FORCE_INLINE plNetworkRpcManager* GetRpcManager() { return &m_RpcManager; }
  PL_NODISCARD PL_FORCE_INLINE const plNetworkRpcManager* GetRpcManager() const { return &m_RpcManager; }

  /// @brief Get the bandwidth manager for optimization
  PL_NODISCARD PL_FORCE_INLINE plNetworkBandwidthManager* GetBandwidthManager() { return &m_BandwidthManager; }
  PL_NODISCARD PL_FORCE_INLINE const plNetworkBandwidthManager* GetBandwidthManager() const { return &m_BandwidthManager; }

  PL_NODISCARD PL_FORCE_INLINE plNetworkPredictionManager* GetPredictionManager() { return &m_PredictionManager; }
  PL_NODISCARD PL_FORCE_INLINE const plNetworkPredictionManager* GetPredictionManager() const { return &m_PredictionManager; }

  /// @brief Get the network time manager for synchronized time
  PL_NODISCARD PL_FORCE_INLINE plNetworkTime* GetNetworkTime() { return &m_NetworkTime; }
  PL_NODISCARD PL_FORCE_INLINE const plNetworkTime* GetNetworkTime() const { return &m_NetworkTime; }

  /// @brief Get the lag compensation system (server-side rewind for hit validation)
  PL_NODISCARD PL_FORCE_INLINE plNetworkLagCompensation* GetLagCompensation() { return &m_LagCompensation; }
  PL_NODISCARD PL_FORCE_INLINE const plNetworkLagCompensation* GetLagCompensation() const { return &m_LagCompensation; }

  /// Validate a hitscan ray against server history. Claimed time is clamped by RTT and to 250 ms.
  plUInt32 LagCompensatedRaycast(plUInt32 uiShooterClientID, plTime claimedRenderTime,
    const plVec3& vOrigin, const plVec3& vDirection, float fMaxDistance, plVec3& out_vHitPoint) const;

  /// @brief Get the level manager for synchronized level loading
  PL_NODISCARD PL_FORCE_INLINE plNetworkLevelManager* GetLevelManager() { return &m_LevelManager; }
  PL_NODISCARD PL_FORCE_INLINE const plNetworkLevelManager* GetLevelManager() const { return &m_LevelManager; }

  /// @brief Get the session manager for high-level session lifecycle
  PL_NODISCARD PL_FORCE_INLINE plNetworkSession* GetSession() { return &m_Session; }
  PL_NODISCARD PL_FORCE_INLINE const plNetworkSession* GetSession() const { return &m_Session; }

  /// @brief Get synchronized network time (convenience method)
  PL_NODISCARD plTime GetSyncedTime() const { return m_NetworkTime.GetNetworkTime(); }

  /// @brief Get round-trip time to server (convenience method)
  PL_NODISCARD plTime GetRTT() const { return m_NetworkTime.GetRoundTripTime(); }

  //////////////////////////////////////////////////////////////////////////
  // Authority Validation
  //////////////////////////////////////////////////////////////////////////

  /// @brief Check if a client has authority to modify a network component
  bool HasAuthority(plNetworkComponent* pComponent, plUInt32 uiClientID) const;

  /// @brief Check if the local client has authority over a component
  bool HasLocalAuthority(plNetworkComponent* pComponent) const;

  /// @brief Validate that an incoming state update is authorized
  bool ValidateStateUpdate(plUInt32 uiComponentNetID, plUInt32 uiSenderClientID) const;

  //////////////////////////////////////////////////////////////////////////
  // Late-Joiner Synchronization
  //////////////////////////////////////////////////////////////////////////

  /// @brief Sync all component states to a newly connected client
  void SyncComponentStatesToClient(plNetworkClient* pClient);

  /// @brief Sync a specific component's state to a client
  void SyncComponentStateToClient(plNetworkComponent* pComponent, plNetworkClient* pClient);

  //////////////////////////////////////////////////////////////////////////
  // Graceful Disconnect
  //////////////////////////////////////////////////////////////////////////

  /// @brief Stop with a specific disconnect reason
  void Stop(plNetworkDisconnectReason::Enum reason = plNetworkDisconnectReason::ClientQuit);

  /// @brief Kick a client with a reason
  void KickClient(plUInt32 uiClientID, plNetworkDisconnectReason::Enum reason = plNetworkDisconnectReason::Kicked);

  /// @brief Send disconnect message (client to server)
  void SendDisconnect(plNetworkDisconnectReason::Enum reason);

  /// @brief Broadcast disconnect to all clients (for server shutdown)
  void BroadcastDisconnect(plNetworkDisconnectReason::Enum reason);

  //////////////////////////////////////////////////////////////////////////
  // Rate Limiting
  //////////////////////////////////////////////////////////////////////////

  /// @brief Set maximum messages per second per client (0 = no limit)
  void SetRateLimit(plUInt32 uiMaxMessagesPerSecond) { m_uiRateLimitPerSecond = uiMaxMessagesPerSecond; }
  plUInt32 GetRateLimit() const { return m_uiRateLimitPerSecond; }

  /// @brief Set max violations before kick (0 = never kick)
  void SetMaxRateLimitViolations(plUInt32 uiMax) { m_uiMaxRateLimitViolations = uiMax; }

  /// @brief Record a malformed message and disconnect repeat offenders.
  void RecordProtocolViolation(plNetworkClient* pClient, const char* szReason);

  //////////////////////////////////////////////////////////////////////////
  // Message Broadcasting
  //////////////////////////////////////////////////////////////////////////

  /// @brief Send a plMessage to all registered network components.
  /// Used internally by Session, LevelManager, and the WorldModule itself
  /// to notify game code via the standard PL_MESSAGE_HANDLER pattern.
  void BroadcastToNetworkComponents(plMessage& ref_msg);

private:

  void OnUpdate(const UpdateContext& ctxt);

  void BroadcastNetworkStateChange();
  void BroadcastClientConnected(plNetworkClient* pClient);
  void BroadcastClientDisconnected(plNetworkClient* pClient);

  void HandleHandshake(plNetworkEvent& event, plNetworkClient* pClient);
  void HandleHandshakeReply(plNetworkEvent& event, plNetworkClient* pClient);

  void GetTargets(plDynamicArray<plNetworkConnection>& targets);

  bool StartPeer();
  void StopPeer();

  /// @brief Check for and disconnect inactive clients
  void CheckClientTimeouts();

  /// @brief Check if a client exceeds rate limit
  bool CheckRateLimit(plNetworkClient* pClient);

  /// @brief Generate a new session token for reconnection
  plUInt64 GenerateSessionToken();

  /// @brief Find a client by their session token (for reconnection)
  plNetworkClient* FindClientBySessionToken(plUInt64 uiSessionToken);

  /// @brief Clean up expired reconnection slots
  void CleanupExpiredReconnectSlots();

  /// @brief Handle engine component messages (64-127 range)
  void HandleEngineMessage(plUInt8 messageId, plNetworkMessage& msg, plNetworkConnection sender);
  void RecordLagCompensationSnapshots();

  float m_fNetworkTickRate = 60.0f;
  float m_fClientTimeoutSeconds = 30.0f; ///< Disconnect clients after this many seconds of inactivity
  plNetworkConfig m_Config;              ///< Network configuration (address, port, limits)
  plNetworkPeer* m_pPeer = nullptr;
  plEnum<plNetworkRoleEnum> m_Role = plNetworkRoleEnum::Undefined;
  plEnum<plNetworkConnectionState> m_Status = plNetworkConnectionState::Disconnected;
  plUInt32 m_uiServerClientID = 0;
  plUInt32 m_uiLocalClientID = 0;
  plUInt32 m_uiNextClientID = 0;
  plUInt32 m_uiNextNetID = 1; ///< Per-module network ID counter (reset per world instance)
  plNetworkClient* m_pLocalClient = nullptr;
  plDynamicArray<plNetworkClient*> m_pClients;
  plTime m_LastUpdate;
  plUInt32 m_uiGameVersion = 0;

  /// @brief Rate limiting configuration
  plUInt32 m_uiRateLimitPerSecond = 1000;      ///< Max messages per second per client (0 = no limit)
  plUInt32 m_uiMaxRateLimitViolations = 5;     ///< Max violations before kick (0 = never kick)
  plUInt32 m_uiMaxProtocolViolations = 5;      ///< Max malformed messages before kick

  /// @brief Reconnection configuration  
  float m_fReconnectWindowSeconds = 60.0f;     ///< How long to keep a slot for reconnection
  plUInt64 m_uiNextSessionToken = 1;           ///< Counter for session tokens

  plDynamicArray<plNetworkComponent*> m_NetworkComponents;
  plDynamicArray<plNetworkObject*> m_NetworkObjects;

  /// @brief Object manager for spawn/despawn
  plNetworkObjectManager m_ObjectManager;

  /// @brief RPC manager for remote procedure calls
  plNetworkRpcManager m_RpcManager;

  /// @brief Bandwidth manager for optimization
  plNetworkBandwidthManager m_BandwidthManager;

  /// @brief Network time synchronization
  plNetworkTime m_NetworkTime;

  /// @brief Fixed-tick owner prediction and server reconciliation
  plNetworkPredictionManager m_PredictionManager;

  /// @brief Lag compensation for server-side rewind
  plNetworkLagCompensation m_LagCompensation;

  /// @brief Level manager for synchronized level loading
  plNetworkLevelManager m_LevelManager;

  /// @brief Session manager for high-level session lifecycle
  plNetworkSession m_Session;

};
