#include "NetworkPlugin/Core/NetworkSimulation.h"
#include <Foundation/Logging/Log.h>
#include <Foundation/Configuration/CVar.h>

//////////////////////////////////////////////////////////////////////////
// Console Variables
//////////////////////////////////////////////////////////////////////////

plCVarBool cvar_NetSimEnabled("net_sim_enabled", false, plCVarFlags::Default, "Enable network condition simulation");
plCVarFloat cvar_NetSimLatency("net_sim_latency", 0.0f, plCVarFlags::Default, "Additional latency in milliseconds");
plCVarFloat cvar_NetSimJitter("net_sim_jitter", 0.0f, plCVarFlags::Default, "Latency jitter in milliseconds");
plCVarFloat cvar_NetSimPacketLoss("net_sim_packetloss", 0.0f, plCVarFlags::Default, "Packet loss percentage (0-100)");
plCVarFloat cvar_NetSimDuplicate("net_sim_duplicate", 0.0f, plCVarFlags::Default, "Packet duplication percentage (0-100)");
plCVarFloat cvar_NetSimReorder("net_sim_reorder", 0.0f, plCVarFlags::Default, "Packet reorder percentage (0-100)");
plCVarInt cvar_NetSimBandwidth("net_sim_bandwidth", 0, plCVarFlags::Default, "Bandwidth limit in bytes/sec (0 = unlimited)");
plCVarString cvar_NetSimPreset("net_sim_preset", "", plCVarFlags::Default, "Apply preset: lan, broadband, mobile3g, mobile4g, satellite, bad, none");

// Global simulator instance
static plNetworkSimulator s_NetworkSimulator;

//////////////////////////////////////////////////////////////////////////
// plNetworkSimulator Implementation
//////////////////////////////////////////////////////////////////////////

plNetworkSimulator::plNetworkSimulator()
{
  m_Random.Initialize(static_cast<plUInt64>(plTime::Now().GetNanoseconds()));
}

void plNetworkSimulator::Initialize()
{
  m_DelayedPackets.Clear();
  ResetStatistics();
  m_LastBandwidthReset = plTime::Now();
  plLog::Dev("PL Net: Network simulator initialized");
}

void plNetworkSimulator::ApplyPresetLAN()
{
  m_Settings.m_bEnabled = true;
  m_Settings.m_fLatencyMs = 2.0f;
  m_Settings.m_fJitterMs = 1.0f;
  m_Settings.m_fMinLatencyMs = 0.5f;
  m_Settings.m_fPacketLossPercent = 0.0f;
  m_Settings.m_fDuplicatePercent = 0.0f;
  m_Settings.m_fReorderPercent = 0.0f;
  m_Settings.m_uiBandwidthLimitBps = 0;
  plLog::Info("PL Net: Applied LAN preset ({}ms latency)", m_Settings.m_fLatencyMs);
}

void plNetworkSimulator::ApplyPresetBroadband()
{
  m_Settings.m_bEnabled = true;
  m_Settings.m_fLatencyMs = 30.0f;
  m_Settings.m_fJitterMs = 10.0f;
  m_Settings.m_fMinLatencyMs = 15.0f;
  m_Settings.m_fPacketLossPercent = 0.5f;
  m_Settings.m_fDuplicatePercent = 0.1f;
  m_Settings.m_fReorderPercent = 0.5f;
  m_Settings.m_uiBandwidthLimitBps = 0;
  plLog::Info("PL Net: Applied Broadband preset ({}ms latency, {}% loss)", 
    m_Settings.m_fLatencyMs, m_Settings.m_fPacketLossPercent);
}

void plNetworkSimulator::ApplyPresetMobile3G()
{
  m_Settings.m_bEnabled = true;
  m_Settings.m_fLatencyMs = 200.0f;
  m_Settings.m_fJitterMs = 100.0f;
  m_Settings.m_fMinLatencyMs = 100.0f;
  m_Settings.m_fPacketLossPercent = 5.0f;
  m_Settings.m_fDuplicatePercent = 1.0f;
  m_Settings.m_fReorderPercent = 5.0f;
  m_Settings.m_uiBandwidthLimitBps = 50000; // ~400 kbps
  plLog::Info("PL Net: Applied Mobile 3G preset ({}ms latency, {}% loss)", 
    m_Settings.m_fLatencyMs, m_Settings.m_fPacketLossPercent);
}

void plNetworkSimulator::ApplyPresetMobile4G()
{
  m_Settings.m_bEnabled = true;
  m_Settings.m_fLatencyMs = 60.0f;
  m_Settings.m_fJitterMs = 30.0f;
  m_Settings.m_fMinLatencyMs = 30.0f;
  m_Settings.m_fPacketLossPercent = 1.0f;
  m_Settings.m_fDuplicatePercent = 0.2f;
  m_Settings.m_fReorderPercent = 1.0f;
  m_Settings.m_uiBandwidthLimitBps = 0;
  plLog::Info("PL Net: Applied Mobile 4G preset ({}ms latency, {}% loss)", 
    m_Settings.m_fLatencyMs, m_Settings.m_fPacketLossPercent);
}

void plNetworkSimulator::ApplyPresetSatellite()
{
  m_Settings.m_bEnabled = true;
  m_Settings.m_fLatencyMs = 600.0f;
  m_Settings.m_fJitterMs = 50.0f;
  m_Settings.m_fMinLatencyMs = 500.0f;
  m_Settings.m_fPacketLossPercent = 2.0f;
  m_Settings.m_fDuplicatePercent = 0.0f;
  m_Settings.m_fReorderPercent = 0.0f;
  m_Settings.m_uiBandwidthLimitBps = 0;
  plLog::Info("PL Net: Applied Satellite preset ({}ms latency, {}% loss)", 
    m_Settings.m_fLatencyMs, m_Settings.m_fPacketLossPercent);
}

void plNetworkSimulator::ApplyPresetBadConnection()
{
  m_Settings.m_bEnabled = true;
  m_Settings.m_fLatencyMs = 300.0f;
  m_Settings.m_fJitterMs = 150.0f;
  m_Settings.m_fMinLatencyMs = 100.0f;
  m_Settings.m_fPacketLossPercent = 15.0f;
  m_Settings.m_fDuplicatePercent = 5.0f;
  m_Settings.m_fReorderPercent = 10.0f;
  m_Settings.m_uiBandwidthLimitBps = 20000;
  plLog::Info("PL Net: Applied Bad Connection preset ({}ms latency, {}% loss)", 
    m_Settings.m_fLatencyMs, m_Settings.m_fPacketLossPercent);
}

void plNetworkSimulator::ApplyPresetNone()
{
  m_Settings.m_bEnabled = false;
  m_Settings.m_fLatencyMs = 0.0f;
  m_Settings.m_fJitterMs = 0.0f;
  m_Settings.m_fMinLatencyMs = 0.0f;
  m_Settings.m_fPacketLossPercent = 0.0f;
  m_Settings.m_fDuplicatePercent = 0.0f;
  m_Settings.m_fReorderPercent = 0.0f;
  m_Settings.m_uiBandwidthLimitBps = 0;
  Clear();
  plLog::Info("PL Net: Network simulation disabled");
}

bool plNetworkSimulator::QueueOutgoingPacket(const plUInt8* data, plUInt32 size, plUInt32 connectionID)
{
  if (!m_Settings.m_bEnabled)
    return true; // Pass through

  m_uiTotalPackets++;

  // Check bandwidth limit
  if (m_Settings.m_uiBandwidthLimitBps > 0)
  {
    plTime now = plTime::Now();
    if ((now - m_LastBandwidthReset).GetSeconds() >= 1.0)
    {
      m_uiBytesSentThisSecond = 0;
      m_LastBandwidthReset = now;
    }

    if (m_uiBytesSentThisSecond + size > m_Settings.m_uiBandwidthLimitBps)
    {
      m_uiDroppedPackets++;
      return false; // Drop due to bandwidth limit
    }
    m_uiBytesSentThisSecond += size;
  }

  // Check packet loss
  if (m_Settings.ShouldDropPacket(m_Random))
  {
    m_uiDroppedPackets++;
    return false;
  }

  // Calculate delivery time
  float latencyMs = m_Settings.GetEffectiveLatency(m_Random);
  plTime deliveryTime = plTime::Now() + plTime::MakeFromMilliseconds(latencyMs);

  // Check for reordering - if so, add extra delay
  if (m_Settings.ShouldReorderPacket(m_Random))
  {
    deliveryTime += plTime::MakeFromMilliseconds(m_Settings.m_fJitterMs * 2.0f);
    m_uiReorderedPackets++;
  }

  // Queue the packet
  plDelayedPacket packet;
  packet.m_DeliveryTime = deliveryTime;
  packet.m_Data.SetCountUninitialized(size);
  plMemoryUtils::Copy(packet.m_Data.GetData(), data, size);
  packet.m_uiConnectionID = connectionID;
  packet.m_bIsDuplicate = false;
  m_DelayedPackets.PushBack(packet);

  // Check for duplication
  if (m_Settings.ShouldDuplicatePacket(m_Random))
  {
    plDelayedPacket duplicate = packet;
    duplicate.m_bIsDuplicate = true;
    // Duplicate arrives slightly later
    duplicate.m_DeliveryTime += plTime::MakeFromMilliseconds(m_Settings.m_fJitterMs);
    m_DelayedPackets.PushBack(duplicate);
    m_uiDuplicatedPackets++;
  }

  return true;
}

void plNetworkSimulator::GetReadyPackets(plTime currentTime, plDynamicArray<plDelayedPacket>& outPackets)
{
  outPackets.Clear();

  // Find packets ready for delivery
  for (plUInt32 i = 0; i < m_DelayedPackets.GetCount(); )
  {
    if (m_DelayedPackets[i].m_DeliveryTime <= currentTime)
    {
      outPackets.PushBack(m_DelayedPackets[i]);
      m_DelayedPackets.RemoveAtAndCopy(i);
    }
    else
    {
      ++i;
    }
  }
}

void plNetworkSimulator::Update(plTime currentTime)
{
  // Sync settings from CVars
  m_Settings.m_bEnabled = cvar_NetSimEnabled;
  m_Settings.m_fLatencyMs = cvar_NetSimLatency;
  m_Settings.m_fJitterMs = cvar_NetSimJitter;
  m_Settings.m_fPacketLossPercent = cvar_NetSimPacketLoss;
  m_Settings.m_fDuplicatePercent = cvar_NetSimDuplicate;
  m_Settings.m_fReorderPercent = cvar_NetSimReorder;
  m_Settings.m_uiBandwidthLimitBps = static_cast<plUInt32>(cvar_NetSimBandwidth.GetValue());

  // Check for preset changes
  plStringView preset = cvar_NetSimPreset.GetValue();
  if (!preset.IsEmpty())
  {
    if (preset.IsEqual_NoCase("lan"))
      ApplyPresetLAN();
    else if (preset.IsEqual_NoCase("broadband"))
      ApplyPresetBroadband();
    else if (preset.IsEqual_NoCase("mobile3g"))
      ApplyPresetMobile3G();
    else if (preset.IsEqual_NoCase("mobile4g"))
      ApplyPresetMobile4G();
    else if (preset.IsEqual_NoCase("satellite"))
      ApplyPresetSatellite();
    else if (preset.IsEqual_NoCase("bad"))
      ApplyPresetBadConnection();
    else if (preset.IsEqual_NoCase("none"))
      ApplyPresetNone();

    // Clear the preset CVar after applying
    cvar_NetSimPreset = "";
  }
}

void plNetworkSimulator::Clear()
{
  m_DelayedPackets.Clear();
}

plUInt32 plNetworkSimulator::GetQueuedBytes() const
{
  plUInt32 total = 0;
  for (const auto& packet : m_DelayedPackets)
  {
    total += packet.m_Data.GetCount();
  }
  return total;
}

void plNetworkSimulator::ResetStatistics()
{
  m_uiDroppedPackets = 0;
  m_uiDuplicatedPackets = 0;
  m_uiReorderedPackets = 0;
  m_uiTotalPackets = 0;
  m_uiBytesSentThisSecond = 0;
}

//////////////////////////////////////////////////////////////////////////
// CVar Registration
//////////////////////////////////////////////////////////////////////////

namespace plNetworkSimCVars
{
  void RegisterCVars()
  {
    // CVars are auto-registered via static initialization
    plLog::Dev("PL Net: Network simulation CVars registered");
  }

  plNetworkSimulator* GetSimulator()
  {
    return &s_NetworkSimulator;
  }
}
