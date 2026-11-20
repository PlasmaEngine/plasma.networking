#pragma once

#include <NetworkPlugin/NetworkPluginDLL.h>

/// \brief Named network conditions for the simulator.
struct plNetworkSimulationPreset
{
  using StorageType = plUInt8;

  enum Enum : plUInt8
  {
    None,      ///< No simulation.
    LAN,       ///< 2 ms latency, 1 ms jitter, no loss.
    Broadband, ///< 30 ms latency, 10 ms jitter, 0.5 % loss.
    Mobile4G,  ///< 60 ms latency, 30 ms jitter, 1 % loss.
    Mobile3G,  ///< 200 ms latency, 100 ms jitter, 5 % loss, 50 KB/s.
    Satellite, ///< 600 ms latency, 50 ms jitter, 2 % loss.
    Bad,       ///< 300 ms latency, 150 ms jitter, 15 % loss, 20 KB/s.
    Default = None
  };
};

/// \brief Network conditions for one direction of every connection of one peer.
///
/// A peer applies them to what it sends and again to what it receives, so a client with 50 ms latency sees 100 ms of
/// round trip to a host that simulates nothing. The simulator runs above ENet: a lost reliable message is delivered late,
/// as ENet would after a resend, and never dropped; reliable messages keep their order. Only unreliable messages are
/// dropped, duplicated or reordered. Unreliable sequenced messages (UnreliableOrdered and None) that arrive behind a newer
/// one are discarded, as ENet does.
struct PL_NETWORKPLUGIN_DLL plNetworkSimulationSettings
{
  bool m_bEnabled = false;

  float m_fLatencyMs = 0.0f;          ///< One-way delay added to every message.
  float m_fJitterMs = 0.0f;           ///< Random variation of the delay, in both directions.
  float m_fMinLatencyMs = 0.0f;       ///< The delay with jitter never goes below this.
  float m_fPacketLossPercent = 0.0f;  ///< Unreliable messages are dropped, reliable ones are resent later.
  float m_fDuplicatePercent = 0.0f;   ///< Unsequenced unreliable messages are delivered twice.
  float m_fReorderPercent = 0.0f;     ///< The message is held back so later ones overtake it.
  plUInt32 m_uiBandwidthLimitBps = 0; ///< Bytes per second per connection; 0 = unlimited.

  /// \brief Seeds the random decisions. The same seed and the same sequence of messages give the same decisions.
  /// 0 seeds from the clock.
  plUInt64 m_uiSeed = 0;

  static plNetworkSimulationSettings MakePreset(plNetworkSimulationPreset::Enum preset);
};
