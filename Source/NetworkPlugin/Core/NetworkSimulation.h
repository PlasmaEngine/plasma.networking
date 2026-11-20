#pragma once

#include <NetworkPlugin/NetworkPluginDLL.h>

#include <Foundation/Math/Random.h>
#include <Foundation/Time/Time.h>
#include <NetworkPlugin/Core/NetworkSimulationSettings.h>
#include <NetworkPlugin/Core/NetworkTypes.h>

/// \brief Decides what happens to each message a simulating driver passes on. One instance per driver; both directions of
/// every connection share its random sequence, each direction and connection keeps its own ordering state.
class PL_NETWORKPLUGIN_DLL plNetworkSimulator
{
public:
  /// \brief Ordering and bandwidth state of one direction of one connection.
  struct Link
  {
    plTime m_LastReliableDelivery;
    plTime m_BandwidthFreeAt;
  };

  struct Decision
  {
    bool m_bDeliver = true;
    plTime m_DeliveryTime;
    bool m_bDuplicate = false;
    plTime m_DuplicateDeliveryTime;
  };

  plNetworkSimulator();

  /// \brief Uses these conditions from now on and stops following the net_sim_* CVars.
  void SetSettings(const plNetworkSimulationSettings& settings);
  const plNetworkSimulationSettings& GetSettings() const { return m_Settings; }

  void ApplyPreset(plNetworkSimulationPreset::Enum preset) { SetSettings(plNetworkSimulationSettings::MakePreset(preset)); }

  /// \brief While set, Update() copies the net_sim_* CVars into the settings. Set by default; SetSettings clears it.
  void SetFollowCVars(bool bFollow) { m_bFollowCVars = bFollow; }
  bool IsFollowingCVars() const { return m_bFollowCVars; }

  bool IsEnabled() const { return m_Settings.m_bEnabled; }

  /// \brief Reads the CVars when following them. A net_sim_preset value is applied once and written back into the other
  /// CVars, so the next update keeps it.
  void Update();

  /// \brief Decides delivery for one message of uiSize bytes sent at now.
  Decision Schedule(Link& ref_link, plNetworkChannelTypeEnum::Enum channel, plUInt32 uiSize, plTime now);

  static bool IsReliable(plNetworkChannelTypeEnum::Enum channel)
  {
    return channel == plNetworkChannelTypeEnum::Reliable || channel == plNetworkChannelTypeEnum::ReliableOrdered;
  }

  /// \brief Unreliable channels that ENet sends sequenced: an older message arriving after a newer one is discarded.
  static bool IsSequenced(plNetworkChannelTypeEnum::Enum channel)
  {
    return channel == plNetworkChannelTypeEnum::UnreliableOrdered || channel == plNetworkChannelTypeEnum::None;
  }

  plUInt32 GetDroppedCount() const { return m_uiDropped; }
  plUInt32 GetDuplicatedCount() const { return m_uiDuplicated; }
  plUInt32 GetReorderedCount() const { return m_uiReordered; }
  plUInt32 GetResentCount() const { return m_uiResent; }
  void ResetStatistics();

private:
  void Reseed();
  bool Roll(float fPercent);
  plTime RollDelay();

  plNetworkSimulationSettings m_Settings;
  plRandom m_Random;
  bool m_bFollowCVars = true;

  plUInt32 m_uiDropped = 0;
  plUInt32 m_uiDuplicated = 0;
  plUInt32 m_uiReordered = 0;
  plUInt32 m_uiResent = 0;
};
