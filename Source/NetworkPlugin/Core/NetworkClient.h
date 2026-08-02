#pragma once

#include "NetworkTypes.h"
#include <NetworkPlugin/NetworkPluginDLL.h>
#include <Foundation/Time/Time.h>

/// @brief Quality level for adaptive behavior based on connection quality
struct plNetworkQualityLevel
{
  using StorageType = plUInt8;

  enum Enum
  {
    Excellent,   ///< RTT < 50ms, no packet loss
    Good,        ///< RTT < 100ms, < 1% packet loss
    Fair,        ///< RTT < 150ms, < 5% packet loss
    Poor,        ///< RTT < 250ms, < 10% packet loss
    Bad,         ///< RTT >= 250ms or > 10% packet loss
    Unknown,     ///< Not enough data yet
    Default = Unknown
  };
};

/// @brief Connection quality metrics for a network client
struct PL_NETWORKPLUGIN_DLL plNetworkConnectionQuality
{
  /// @brief Current round-trip time in milliseconds
  float m_fRTT = 0.0f;

  /// @brief Smoothed RTT (exponential moving average)
  float m_fSmoothedRTT = 0.0f;

  /// @brief RTT variance for jitter estimation
  float m_fRTTVariance = 0.0f;

  /// @brief Estimated jitter in milliseconds
  float m_fJitter = 0.0f;

  /// @brief Packet loss percentage (0-100)
  float m_fPacketLoss = 0.0f;

  /// @brief Smoothed packet loss (exponential moving average)
  float m_fSmoothedPacketLoss = 0.0f;

  /// @brief Current ping in milliseconds (RTT / 2)
  float GetPing() const { return m_fRTT * 0.5f; }

  /// @brief Get the overall quality level
  plNetworkQualityLevel::Enum GetQualityLevel() const
  {
    if (m_fSmoothedRTT < 50.0f && m_fSmoothedPacketLoss < 0.5f)
      return plNetworkQualityLevel::Excellent;
    if (m_fSmoothedRTT < 100.0f && m_fSmoothedPacketLoss < 1.0f)
      return plNetworkQualityLevel::Good;
    if (m_fSmoothedRTT < 150.0f && m_fSmoothedPacketLoss < 5.0f)
      return plNetworkQualityLevel::Fair;
    if (m_fSmoothedRTT < 250.0f && m_fSmoothedPacketLoss < 10.0f)
      return plNetworkQualityLevel::Poor;
    return plNetworkQualityLevel::Bad;
  }

  /// @brief Get quality as a normalized score (0-1, 1 = best)
  float GetQualityScore() const
  {
    // Score based on RTT (0-300ms range)
    float rttScore = 1.0f - plMath::Clamp(m_fSmoothedRTT / 300.0f, 0.0f, 1.0f);
    
    // Score based on packet loss (0-20% range)
    float lossScore = 1.0f - plMath::Clamp(m_fSmoothedPacketLoss / 20.0f, 0.0f, 1.0f);
    
    // Combined score (RTT weighted more heavily)
    return rttScore * 0.7f + lossScore * 0.3f;
  }

  /// @brief Update metrics with new RTT sample
  void UpdateRTT(float newRTT, float smoothingFactor = 0.1f)
  {
    m_fRTT = newRTT;
    
    if (m_fSmoothedRTT == 0.0f)
    {
      m_fSmoothedRTT = newRTT;
      m_fRTTVariance = 0.0f;
    }
    else
    {
      // Calculate variance for jitter
      float diff = plMath::Abs(newRTT - m_fSmoothedRTT);
      m_fRTTVariance = m_fRTTVariance * (1.0f - smoothingFactor) + diff * smoothingFactor;
      m_fJitter = m_fRTTVariance;
      
      // Update smoothed RTT
      m_fSmoothedRTT = m_fSmoothedRTT * (1.0f - smoothingFactor) + newRTT * smoothingFactor;
    }
  }

  /// @brief Update packet loss metrics
  void UpdatePacketLoss(float newPacketLoss, float smoothingFactor = 0.1f)
  {
    m_fPacketLoss = newPacketLoss;
    m_fSmoothedPacketLoss = m_fSmoothedPacketLoss * (1.0f - smoothingFactor) + newPacketLoss * smoothingFactor;
  }

  /// @brief Reset all metrics
  void Reset()
  {
    m_fRTT = 0.0f;
    m_fSmoothedRTT = 0.0f;
    m_fRTTVariance = 0.0f;
    m_fJitter = 0.0f;
    m_fPacketLoss = 0.0f;
    m_fSmoothedPacketLoss = 0.0f;
  }
};

class PL_NETWORKPLUGIN_DLL plNetworkClient
{
public:
  virtual ~plNetworkClient() = default;

  plUInt32 m_uiClientID = 0;

  plNetworkConnection m_Connection;

  plEnum<plNetworkConnectionState> m_ConnectionState = plNetworkConnectionState::Offline;

  /// @brief Timestamp of last received message from this client
  plTime m_LastActivityTime;

  /// @brief Connection quality metrics
  plNetworkConnectionQuality m_ConnectionQuality;

  /// @brief Update the last activity timestamp to now
  void UpdateActivity() { m_LastActivityTime = plTime::Now(); }

  /// @brief Get seconds since last activity
  float GetSecondsSinceLastActivity() const
  {
    return static_cast<float>((plTime::Now() - m_LastActivityTime).GetSeconds());
  }

  /// @brief Get the connection quality level
  plNetworkQualityLevel::Enum GetQualityLevel() const
  {
    return m_ConnectionQuality.GetQualityLevel();
  }

  /// @brief Get current ping in milliseconds
  float GetPing() const
  {
    return m_ConnectionQuality.GetPing();
  }

  /// @brief Get current RTT in milliseconds
  float GetRTT() const
  {
    return m_ConnectionQuality.m_fRTT;
  }

  /// @brief Get current jitter in milliseconds
  float GetJitter() const
  {
    return m_ConnectionQuality.m_fJitter;
  }

  /// @brief Get current packet loss percentage
  float GetPacketLoss() const
  {
    return m_ConnectionQuality.m_fPacketLoss;
  }

  //////////////////////////////////////////////////////////////////////////
  // Rate Limiting
  //////////////////////////////////////////////////////////////////////////

  /// @brief Current messages per second from this client
  float m_fMessagesPerSecond = 0.0f;

  /// @brief Messages received in current second
  plUInt32 m_uiMessagesThisSecond = 0;

  /// @brief Timestamp when message counting started
  plTime m_MessageCountStartTime;

  /// @brief Last time the rate limit counter was reset
  plTime m_LastRateLimitReset;

  /// @brief Number of rate limit violations
  plUInt32 m_uiRateLimitViolations = 0;

  /// @brief Malformed protocol messages observed from this client.
  plUInt32 m_uiProtocolViolations = 0;

  /// @brief Last time a protocol-violation warning was emitted.
  plTime m_LastProtocolViolationLog;

  /// @brief Increment message counter for rate limiting
  void RecordMessageReceived()
  {
    plTime now = plTime::Now();
    if ((now - m_MessageCountStartTime).GetSeconds() >= 1.0)
    {
      m_fMessagesPerSecond = static_cast<float>(m_uiMessagesThisSecond);
      m_uiMessagesThisSecond = 0;
      m_MessageCountStartTime = now;
    }
    m_uiMessagesThisSecond++;
  }

  /// @brief Check if client exceeds rate limit
  bool ExceedsRateLimit(plUInt32 maxMessagesPerSecond) const
  {
    return m_fMessagesPerSecond > static_cast<float>(maxMessagesPerSecond);
  }

  //////////////////////////////////////////////////////////////////////////
  // Reconnection Support
  //////////////////////////////////////////////////////////////////////////

  /// @brief Session token for reconnection
  plUInt64 m_uiSessionToken = 0;

  /// @brief Time when client disconnected (for reconnection window)
  plTime m_DisconnectTime;

  /// @brief Time when reconnection window expires
  plTime m_ReconnectExpiry;

  /// @brief Whether this client slot is reserved for reconnection
  bool m_bReservedForReconnect = false;
};
