#include <NetworkPlugin/NetworkPluginPCH.h>

#include <Foundation/Configuration/CVar.h>
#include <NetworkPlugin/Core/NetworkSimulation.h>

// Process-wide on purpose: every peer that is not given explicit settings follows these.
plCVarBool cvar_NetSimEnabled("net_sim_enabled", false, plCVarFlags::Default, "Enable network condition simulation");
plCVarFloat cvar_NetSimLatency("net_sim_latency", 0.0f, plCVarFlags::Default, "One-way latency in milliseconds, applied on send and on receive");
plCVarFloat cvar_NetSimJitter("net_sim_jitter", 0.0f, plCVarFlags::Default, "Latency jitter in milliseconds");
plCVarFloat cvar_NetSimMinLatency("net_sim_min_latency", 0.0f, plCVarFlags::Default, "Lowest latency jitter can reach, in milliseconds");
plCVarFloat cvar_NetSimPacketLoss("net_sim_packetloss", 0.0f, plCVarFlags::Default, "Packet loss percentage (0-100); reliable messages are resent");
plCVarFloat cvar_NetSimDuplicate("net_sim_duplicate", 0.0f, plCVarFlags::Default, "Packet duplication percentage (0-100)");
plCVarFloat cvar_NetSimReorder("net_sim_reorder", 0.0f, plCVarFlags::Default, "Packet reorder percentage (0-100)");
plCVarInt cvar_NetSimBandwidth("net_sim_bandwidth", 0, plCVarFlags::Default, "Bandwidth limit in bytes/sec per connection (0 = unlimited)");
plCVarString cvar_NetSimPreset("net_sim_preset", "", plCVarFlags::Default, "Apply preset: lan, broadband, mobile4g, mobile3g, satellite, bad, none");

plNetworkSimulationSettings plNetworkSimulationSettings::MakePreset(plNetworkSimulationPreset::Enum preset)
{
  plNetworkSimulationSettings s;
  s.m_bEnabled = preset != plNetworkSimulationPreset::None;

  switch (preset)
  {
    case plNetworkSimulationPreset::LAN:
      s.m_fLatencyMs = 2.0f;
      s.m_fJitterMs = 1.0f;
      s.m_fMinLatencyMs = 0.5f;
      break;
    case plNetworkSimulationPreset::Broadband:
      s.m_fLatencyMs = 30.0f;
      s.m_fJitterMs = 10.0f;
      s.m_fMinLatencyMs = 15.0f;
      s.m_fPacketLossPercent = 0.5f;
      s.m_fDuplicatePercent = 0.1f;
      s.m_fReorderPercent = 0.5f;
      break;
    case plNetworkSimulationPreset::Mobile4G:
      s.m_fLatencyMs = 60.0f;
      s.m_fJitterMs = 30.0f;
      s.m_fMinLatencyMs = 30.0f;
      s.m_fPacketLossPercent = 1.0f;
      s.m_fDuplicatePercent = 0.2f;
      s.m_fReorderPercent = 1.0f;
      break;
    case plNetworkSimulationPreset::Mobile3G:
      s.m_fLatencyMs = 200.0f;
      s.m_fJitterMs = 100.0f;
      s.m_fMinLatencyMs = 100.0f;
      s.m_fPacketLossPercent = 5.0f;
      s.m_fDuplicatePercent = 1.0f;
      s.m_fReorderPercent = 5.0f;
      s.m_uiBandwidthLimitBps = 50000;
      break;
    case plNetworkSimulationPreset::Satellite:
      s.m_fLatencyMs = 600.0f;
      s.m_fJitterMs = 50.0f;
      s.m_fMinLatencyMs = 500.0f;
      s.m_fPacketLossPercent = 2.0f;
      break;
    case plNetworkSimulationPreset::Bad:
      s.m_fLatencyMs = 300.0f;
      s.m_fJitterMs = 150.0f;
      s.m_fMinLatencyMs = 100.0f;
      s.m_fPacketLossPercent = 15.0f;
      s.m_fDuplicatePercent = 5.0f;
      s.m_fReorderPercent = 10.0f;
      s.m_uiBandwidthLimitBps = 20000;
      break;
    case plNetworkSimulationPreset::None:
    default:
      break;
  }

  return s;
}

plNetworkSimulator::plNetworkSimulator()
{
  Reseed();
}

void plNetworkSimulator::SetSettings(const plNetworkSimulationSettings& settings)
{
  m_bFollowCVars = false;
  const bool bReseed = settings.m_uiSeed != m_Settings.m_uiSeed;
  m_Settings = settings;
  if (bReseed)
    Reseed();
}

void plNetworkSimulator::Reseed()
{
  if (m_Settings.m_uiSeed != 0)
    m_Random.Initialize(m_Settings.m_uiSeed);
  else
    m_Random.InitializeFromCurrentTime();
}

void plNetworkSimulator::Update()
{
  if (!m_bFollowCVars)
    return;

  const plStringView sPreset = cvar_NetSimPreset.GetValue();
  if (!sPreset.IsEmpty())
  {
    plNetworkSimulationPreset::Enum preset = plNetworkSimulationPreset::None;
    bool bKnown = true;
    if (sPreset.IsEqual_NoCase("lan"))
      preset = plNetworkSimulationPreset::LAN;
    else if (sPreset.IsEqual_NoCase("broadband"))
      preset = plNetworkSimulationPreset::Broadband;
    else if (sPreset.IsEqual_NoCase("mobile4g"))
      preset = plNetworkSimulationPreset::Mobile4G;
    else if (sPreset.IsEqual_NoCase("mobile3g"))
      preset = plNetworkSimulationPreset::Mobile3G;
    else if (sPreset.IsEqual_NoCase("satellite"))
      preset = plNetworkSimulationPreset::Satellite;
    else if (sPreset.IsEqual_NoCase("bad"))
      preset = plNetworkSimulationPreset::Bad;
    else if (!sPreset.IsEqual_NoCase("none"))
      bKnown = false;

    if (bKnown)
    {
      // The preset becomes the CVar values, so reading the CVars below keeps it instead of overwriting it.
      const plNetworkSimulationSettings s = plNetworkSimulationSettings::MakePreset(preset);
      cvar_NetSimEnabled = s.m_bEnabled;
      cvar_NetSimLatency = s.m_fLatencyMs;
      cvar_NetSimJitter = s.m_fJitterMs;
      cvar_NetSimMinLatency = s.m_fMinLatencyMs;
      cvar_NetSimPacketLoss = s.m_fPacketLossPercent;
      cvar_NetSimDuplicate = s.m_fDuplicatePercent;
      cvar_NetSimReorder = s.m_fReorderPercent;
      cvar_NetSimBandwidth = static_cast<int>(s.m_uiBandwidthLimitBps);
      plLog::Info("PL Net: Applied network simulation preset '{}'", sPreset);
    }
    else
    {
      plLog::Warning("PL Net: Unknown network simulation preset '{}'", sPreset);
    }

    cvar_NetSimPreset = "";
  }

  m_Settings.m_bEnabled = cvar_NetSimEnabled;
  m_Settings.m_fLatencyMs = cvar_NetSimLatency;
  m_Settings.m_fJitterMs = cvar_NetSimJitter;
  m_Settings.m_fMinLatencyMs = cvar_NetSimMinLatency;
  m_Settings.m_fPacketLossPercent = cvar_NetSimPacketLoss;
  m_Settings.m_fDuplicatePercent = cvar_NetSimDuplicate;
  m_Settings.m_fReorderPercent = cvar_NetSimReorder;
  m_Settings.m_uiBandwidthLimitBps = static_cast<plUInt32>(plMath::Max(cvar_NetSimBandwidth.GetValue(), 0));
}

bool plNetworkSimulator::Roll(float fPercent)
{
  if (fPercent <= 0.0f)
    return false;
  return m_Random.FloatMinMax(0.0f, 100.0f) < fPercent;
}

plTime plNetworkSimulator::RollDelay()
{
  float fDelayMs = m_Settings.m_fLatencyMs;
  if (m_Settings.m_fJitterMs > 0.0f)
    fDelayMs += m_Random.FloatMinMax(-1.0f, 1.0f) * m_Settings.m_fJitterMs;

  fDelayMs = plMath::Max(plMath::Max(fDelayMs, m_Settings.m_fMinLatencyMs), 0.0f);
  return plTime::MakeFromMilliseconds(fDelayMs);
}

plNetworkSimulator::Decision plNetworkSimulator::Schedule(Link& ref_link, plNetworkChannelTypeEnum::Enum channel, plUInt32 uiSize, plTime now)
{
  Decision decision;
  decision.m_DeliveryTime = now;

  const bool bReliable = IsReliable(channel);

  if (!m_Settings.m_bEnabled)
  {
    // Nothing is delayed, but a message sent while earlier ones are still queued must not overtake them.
    if (bReliable || IsSequenced(channel))
      decision.m_DeliveryTime = plMath::Max(now, ref_link.m_LastReliableDelivery);
    if (bReliable)
      ref_link.m_LastReliableDelivery = decision.m_DeliveryTime;
    return decision;
  }

  plTime start = now;
  if (m_Settings.m_uiBandwidthLimitBps > 0)
  {
    start = plMath::Max(now, ref_link.m_BandwidthFreeAt);
    if (!bReliable && start - now > plTime::MakeFromMilliseconds(500))
    {
      // The link is saturated; a real router drops what does not fit in its queue.
      ++m_uiDropped;
      decision.m_bDeliver = false;
      return decision;
    }

    ref_link.m_BandwidthFreeAt = start + plTime::MakeFromSeconds(static_cast<double>(uiSize) / m_Settings.m_uiBandwidthLimitBps);
  }

  plTime delivery = start + RollDelay();

  if (bReliable)
  {
    // Each loss costs ENet a resend about one round trip later.
    for (plUInt32 uiAttempt = 0; uiAttempt < 8 && Roll(m_Settings.m_fPacketLossPercent); ++uiAttempt)
    {
      delivery += plTime::MakeFromMilliseconds(plMath::Max(2.0f * m_Settings.m_fLatencyMs, 0.0f) + 30.0f);
      ++m_uiResent;
    }

    delivery = plMath::Max(delivery, ref_link.m_LastReliableDelivery);
    ref_link.m_LastReliableDelivery = delivery;
    decision.m_DeliveryTime = delivery;
    return decision;
  }

  if (Roll(m_Settings.m_fPacketLossPercent))
  {
    ++m_uiDropped;
    decision.m_bDeliver = false;
    return decision;
  }

  if (Roll(m_Settings.m_fReorderPercent))
  {
    // Held back long enough for the next messages to overtake it, also when there is no jitter.
    delivery += plTime::MakeFromMilliseconds(plMath::Max(2.0f * m_Settings.m_fJitterMs, 20.0f));
    ++m_uiReordered;
  }

  // ENet does not hand out an unreliable sequenced message before the reliable one sent ahead of it.
  if (IsSequenced(channel))
    delivery = plMath::Max(delivery, ref_link.m_LastReliableDelivery);

  decision.m_DeliveryTime = delivery;

  if (channel == plNetworkChannelTypeEnum::Unreliable && Roll(m_Settings.m_fDuplicatePercent))
  {
    decision.m_bDuplicate = true;
    decision.m_DuplicateDeliveryTime = delivery + plTime::MakeFromMilliseconds(plMath::Max(m_Settings.m_fJitterMs, 1.0f));
    ++m_uiDuplicated;
  }

  return decision;
}

void plNetworkSimulator::ResetStatistics()
{
  m_uiDropped = 0;
  m_uiDuplicated = 0;
  m_uiReordered = 0;
  m_uiResent = 0;
}
