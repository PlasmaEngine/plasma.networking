#pragma once

#include <NetworkPlugin/NetworkPluginDLL.h>
#include <Foundation/Time/Time.h>
#include <Foundation/Containers/Deque.h>
#include <Foundation/Math/Random.h>

/// @brief Network simulation settings for testing/debugging
/// Allows simulating various network conditions like latency, jitter, and packet loss
struct PL_NETWORKPLUGIN_DLL plNetworkSimulationSettings
{
  /// @brief Whether simulation is enabled
  bool m_bEnabled = false;

  /// @brief Additional latency to add (in milliseconds)
  float m_fLatencyMs = 0.0f;

  /// @brief Latency variation/jitter (in milliseconds)
  float m_fJitterMs = 0.0f;

  /// @brief Minimum latency (latency - jitter cannot go below this)
  float m_fMinLatencyMs = 0.0f;

  /// @brief Packet loss percentage (0-100)
  float m_fPacketLossPercent = 0.0f;

  /// @brief Packet duplication percentage (0-100)
  float m_fDuplicatePercent = 0.0f;

  /// @brief Packet reordering percentage (0-100)
  float m_fReorderPercent = 0.0f;

  /// @brief Bandwidth limit in bytes per second (0 = unlimited)
  plUInt32 m_uiBandwidthLimitBps = 0;

  /// @brief Apply latency calculation with jitter
  float GetEffectiveLatency(plRandom& rng) const
  {
    if (m_fJitterMs <= 0.0f)
      return m_fLatencyMs;

    float jitter = (rng.FloatMinMax(-1.0f, 1.0f)) * m_fJitterMs;
    return plMath::Max(m_fMinLatencyMs, m_fLatencyMs + jitter);
  }

  /// @brief Check if a packet should be dropped
  bool ShouldDropPacket(plRandom& rng) const
  {
    if (m_fPacketLossPercent <= 0.0f)
      return false;
    return rng.FloatMinMax(0.0f, 100.0f) < m_fPacketLossPercent;
  }

  /// @brief Check if a packet should be duplicated
  bool ShouldDuplicatePacket(plRandom& rng) const
  {
    if (m_fDuplicatePercent <= 0.0f)
      return false;
    return rng.FloatMinMax(0.0f, 100.0f) < m_fDuplicatePercent;
  }

  /// @brief Check if a packet should be reordered
  bool ShouldReorderPacket(plRandom& rng) const
  {
    if (m_fReorderPercent <= 0.0f)
      return false;
    return rng.FloatMinMax(0.0f, 100.0f) < m_fReorderPercent;
  }
};

/// @brief A delayed packet waiting to be delivered
struct plDelayedPacket
{
  plTime m_DeliveryTime;       ///< When to deliver this packet
  plDynamicArray<plUInt8> m_Data;  ///< Packet data
  plUInt32 m_uiConnectionID;   ///< Target connection
  bool m_bIsDuplicate = false; ///< Whether this is a duplicate packet
};

/// @brief Network condition simulator for testing
/// Intercepts packets and applies artificial delays, loss, etc.
class PL_NETWORKPLUGIN_DLL plNetworkSimulator
{
public:
  plNetworkSimulator();
  ~plNetworkSimulator() = default;

  /// @brief Initialize the simulator
  void Initialize();

  /// @brief Get current settings
  plNetworkSimulationSettings& GetSettings() { return m_Settings; }
  const plNetworkSimulationSettings& GetSettings() const { return m_Settings; }

  /// @brief Apply settings
  void SetSettings(const plNetworkSimulationSettings& settings) { m_Settings = settings; }

  /// @brief Enable or disable simulation
  void SetEnabled(bool enabled) { m_Settings.m_bEnabled = enabled; }
  bool IsEnabled() const { return m_Settings.m_bEnabled; }

  //////////////////////////////////////////////////////////////////////////
  // Presets
  //////////////////////////////////////////////////////////////////////////

  /// @brief Apply LAN-like conditions (minimal latency)
  void ApplyPresetLAN();

  /// @brief Apply broadband conditions (moderate latency)
  void ApplyPresetBroadband();

  /// @brief Apply mobile/3G conditions (high latency, some loss)
  void ApplyPresetMobile3G();

  /// @brief Apply mobile/4G conditions (moderate latency, low loss)
  void ApplyPresetMobile4G();

  /// @brief Apply satellite conditions (very high latency)
  void ApplyPresetSatellite();

  /// @brief Apply terrible conditions (high latency, high loss, jitter)
  void ApplyPresetBadConnection();

  /// @brief Reset to no simulation
  void ApplyPresetNone();

  //////////////////////////////////////////////////////////////////////////
  // Packet Processing
  //////////////////////////////////////////////////////////////////////////

  /// @brief Queue a packet for delayed delivery
  /// @return true if packet should be sent (not dropped), false if dropped
  bool QueueOutgoingPacket(const plUInt8* data, plUInt32 size, plUInt32 connectionID);

  /// @brief Get packets ready for delivery
  void GetReadyPackets(plTime currentTime, plDynamicArray<plDelayedPacket>& outPackets);

  /// @brief Update - called each frame
  void Update(plTime currentTime);

  /// @brief Clear all queued packets
  void Clear();

  //////////////////////////////////////////////////////////////////////////
  // Statistics
  //////////////////////////////////////////////////////////////////////////

  /// @brief Get number of packets currently delayed
  plUInt32 GetQueuedPacketCount() const { return m_DelayedPackets.GetCount(); }

  /// @brief Get total bytes queued
  plUInt32 GetQueuedBytes() const;

  /// @brief Get packets dropped this session
  plUInt32 GetDroppedPacketCount() const { return m_uiDroppedPackets; }

  /// @brief Get packets duplicated this session
  plUInt32 GetDuplicatedPacketCount() const { return m_uiDuplicatedPackets; }

  /// @brief Reset statistics
  void ResetStatistics();

private:
  plNetworkSimulationSettings m_Settings;
  plDeque<plDelayedPacket> m_DelayedPackets;
  plRandom m_Random;

  // Statistics
  plUInt32 m_uiDroppedPackets = 0;
  plUInt32 m_uiDuplicatedPackets = 0;
  plUInt32 m_uiReorderedPackets = 0;
  plUInt32 m_uiTotalPackets = 0;

  // Bandwidth limiting
  plUInt32 m_uiBytesSentThisSecond = 0;
  plTime m_LastBandwidthReset;
};

//////////////////////////////////////////////////////////////////////////
// Console Variables for Network Simulation
//////////////////////////////////////////////////////////////////////////

// These would be registered as CVars in the engine
// Usage:
//   net_sim_enabled 1
//   net_sim_latency 100
//   net_sim_jitter 20
//   net_sim_packetloss 5
//   net_sim_preset "mobile4g"

namespace plNetworkSimCVars
{
  /// @brief Register network simulation console variables
  void RegisterCVars();

  /// @brief Get the global simulator instance
  plNetworkSimulator* GetSimulator();
}
