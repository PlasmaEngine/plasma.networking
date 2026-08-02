#pragma once

#include <NetworkPlugin/NetworkPluginDLL.h>
#include <Foundation/Time/Time.h>
#include <Foundation/Containers/Map.h>

class plNetworkWorldModule;
class plNetworkMessage;

/// @brief Manages synchronized network time between server and clients.
/// 
/// The server is the authoritative time source. Clients periodically sync
/// their local clock offset to match server time, accounting for RTT.
/// 
/// Usage:
/// - Server: GetNetworkTime() returns server's local time
/// - Client: GetNetworkTime() returns estimated server time (local + offset)
/// 
/// All networked gameplay logic should use network time instead of local time
/// to ensure consistent behavior across all peers.
class PL_NETWORKPLUGIN_DLL plNetworkTime
{
public:
  plNetworkTime();
  ~plNetworkTime() = default;

  /// @brief Initialize the network time system
  void Initialize(plNetworkWorldModule* pNetworkModule);

  /// @brief Shutdown and reset
  void Shutdown();

  /// @brief Update called each network tick
  void Update();

  //////////////////////////////////////////////////////////////////////////
  // Time Access
  //////////////////////////////////////////////////////////////////////////

  /// @brief Get the current synchronized network time
  /// On server: returns local time
  /// On client: returns estimated server time
  plTime GetNetworkTime() const;

  /// @brief Get network time as float seconds (convenience)
  float GetNetworkTimeSeconds() const { return static_cast<float>(GetNetworkTime().GetSeconds()); }

  /// @brief Get the local time offset from server
  /// Positive = local clock is ahead of server
  /// Negative = local clock is behind server
  plTime GetServerOffset() const { return m_ServerOffset; }

  /// @brief Get the current round-trip time to server
  plTime GetRoundTripTime() const { return m_RoundTripTime; }

  /// @brief Get smoothed RTT (for display/prediction)
  plTime GetSmoothedRTT() const { return m_SmoothedRTT; }

  /// @brief Get one-way latency estimate (RTT / 2)
  plTime GetLatency() const { return m_RoundTripTime * 0.5; }

  /// @brief Check if time has been synchronized at least once
  bool IsSynchronized() const { return m_bIsSynchronized; }

  /// @brief Get the number of successful syncs performed
  plUInt32 GetSyncCount() const { return m_uiSyncCount; }

  //////////////////////////////////////////////////////////////////////////
  // Sync Configuration
  //////////////////////////////////////////////////////////////////////////

  /// @brief Set how often to sync time with server (default: 5 seconds)
  void SetSyncInterval(plTime interval) { m_SyncInterval = interval; }
  plTime GetSyncInterval() const { return m_SyncInterval; }

  /// @brief Set offset smoothing factor (0-1, higher = faster adaptation)
  void SetSmoothingFactor(float factor) { m_fSmoothingFactor = plMath::Clamp(factor, 0.0f, 1.0f); }
  float GetSmoothingFactor() const { return m_fSmoothingFactor; }

  /// @brief Force an immediate time sync request
  void RequestSync();

  //////////////////////////////////////////////////////////////////////////
  // Message Handling (called by NetworkWorldModule)
  //////////////////////////////////////////////////////////////////////////

  /// @brief Handle incoming time sync request (server-side)
  void HandleTimeSyncRequest(plNetworkMessage& msg, plUInt32 uiClientID);

  /// @brief Handle incoming time sync response (client-side)
  void HandleTimeSyncResponse(plNetworkMessage& msg);

  //////////////////////////////////////////////////////////////////////////
  // Utility
  //////////////////////////////////////////////////////////////////////////

  /// @brief Convert a local timestamp to network time
  plTime LocalToNetworkTime(plTime localTime) const;

  /// @brief Convert a network timestamp to local time
  plTime NetworkToLocalTime(plTime networkTime) const;

  /// @brief Get time since a network timestamp
  plTime TimeSince(plTime networkTimestamp) const;

  /// @brief Check if a network timestamp is in the past
  bool HasPassed(plTime networkTimestamp) const;

private:
  void SendSyncRequest();
  void CalculateOffset(plTime t0ClientSend, plTime t1ServerReceive, plTime t2ServerSend, plTime t3ClientReceive);

  plNetworkWorldModule* m_pNetworkModule = nullptr;

  // Synchronization state
  plTime m_ServerOffset;           ///< Offset to add to local time to get server time
  plTime m_RoundTripTime;          ///< Current RTT
  plTime m_SmoothedRTT;            ///< Exponentially smoothed RTT
  bool m_bIsSynchronized = false;  ///< Has at least one sync completed
  plUInt32 m_uiSyncCount = 0;      ///< Number of successful syncs

  // Sync timing
  plTime m_LastSyncTime;           ///< When we last sent a sync request
  plTime m_PendingSyncRequestTime; ///< When current pending request was sent
  plTime m_SyncInterval;           ///< How often to sync (default 5s)
  bool m_bSyncPending = false;     ///< Waiting for sync response
  plUInt16 m_uiNextSyncSequence = 1;
  plUInt16 m_uiPendingSyncSequence = 0;
  plMap<plUInt32, plTime> m_LastServerResponseByClient;

  // Configuration
  float m_fSmoothingFactor = 0.1f; ///< How quickly to adapt to new offset (0-1)
  
  // Sync history for jitter calculation
  static constexpr plUInt32 RTT_HISTORY_SIZE = 8;
  plTime m_RTTHistory[RTT_HISTORY_SIZE];
  plUInt32 m_uiRTTHistoryIndex = 0;
};

/// @brief Snapshot timestamp for interpolation
struct PL_NETWORKPLUGIN_DLL plNetworkTimestamp
{
  plTime m_NetworkTime;    ///< Server time when snapshot was created
  plTime m_LocalTime;      ///< Local time when snapshot was received
  plUInt32 m_uiSequence;   ///< Sequence number for ordering

  plNetworkTimestamp() : m_uiSequence(0) {}
  plNetworkTimestamp(plTime networkTime, plTime localTime, plUInt32 sequence)
    : m_NetworkTime(networkTime), m_LocalTime(localTime), m_uiSequence(sequence) {}

  bool operator<(const plNetworkTimestamp& other) const { return m_uiSequence < other.m_uiSequence; }
};
