#include "NetworkTestPCH.h"

#include <Foundation/Configuration/CVar.h>
#include <NetworkPlugin/Core/NetworkSimulation.h>

#include "NetworkTestHarness.h"

using namespace NetworkTestHarness;

namespace
{
  /// \brief Puts every net_sim_* CVar back to its default when it goes out of scope; the CVars are process-wide.
  struct SimulationCVarScope
  {
    ~SimulationCVarScope()
    {
      for (const char* szName : {"net_sim_latency", "net_sim_jitter", "net_sim_min_latency", "net_sim_packetloss", "net_sim_duplicate", "net_sim_reorder"})
      {
        if (auto* pCVar = static_cast<plCVarFloat*>(plCVar::FindCVarByName(szName)))
          *pCVar = 0.0f;
      }

      if (auto* pCVar = static_cast<plCVarInt*>(plCVar::FindCVarByName("net_sim_bandwidth")))
        *pCVar = 0;
      if (auto* pCVar = static_cast<plCVarBool*>(plCVar::FindCVarByName("net_sim_enabled")))
        *pCVar = false;
      if (auto* pCVar = static_cast<plCVarString*>(plCVar::FindCVarByName("net_sim_preset")))
        *pCVar = "";
    }
  };

  plDynamicArray<plNetworkSimulator::Decision> ScheduleMany(plNetworkSimulator& ref_simulator, plNetworkChannelTypeEnum::Enum channel, plUInt32 uiCount)
  {
    plNetworkSimulator::Link link;
    plDynamicArray<plNetworkSimulator::Decision> decisions;
    const plTime start = plTime::MakeFromSeconds(100.0);
    for (plUInt32 i = 0; i < uiCount; ++i)
      decisions.PushBack(ref_simulator.Schedule(link, channel, 100, start + plTime::MakeFromMilliseconds(i)));
    return decisions;
  }

  /// \brief Waits until the client has measured its round trip to the server.
  bool WaitForTimeSync(const Pumped& pumped, NetworkEndpoint& client)
  {
    return PumpAll(pumped, [&] { return client.m_pModule->GetNetworkTime()->IsSynchronized(); }, plTime::MakeFromSeconds(5));
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Network, Simulator)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "D39: a preset applied through net_sim_preset survives the next update")
  {
    SimulationCVarScope cvars;
    auto* pPreset = static_cast<plCVarString*>(plCVar::FindCVarByName("net_sim_preset"));
    if (!PL_TEST_BOOL(pPreset != nullptr))
      return;

    plNetworkSimulator simulator;
    *pPreset = "mobile4g";
    simulator.Update();
    simulator.Update();

    PL_TEST_BOOL(simulator.IsEnabled());
    PL_TEST_FLOAT(simulator.GetSettings().m_fLatencyMs, 60.0f, 0.0f);
    PL_TEST_FLOAT(simulator.GetSettings().m_fPacketLossPercent, 1.0f, 0.0f);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D39: reordering holds messages back when there is no jitter")
  {
    plNetworkSimulationSettings settings;
    settings.m_bEnabled = true;
    settings.m_fLatencyMs = 10.0f;
    settings.m_fReorderPercent = 30.0f;
    settings.m_uiSeed = 7;

    plNetworkSimulator simulator;
    simulator.SetSettings(settings);
    const auto decisions = ScheduleMany(simulator, plNetworkChannelTypeEnum::Unreliable, 200);

    plUInt32 uiOvertaken = 0;
    for (plUInt32 i = 1; i < decisions.GetCount(); ++i)
    {
      if (decisions[i].m_DeliveryTime < decisions[i - 1].m_DeliveryTime)
        ++uiOvertaken;
    }
    PL_TEST_BOOL_MSG(uiOvertaken > 10, "%u of 200 messages were overtaken", uiOvertaken);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D39: the same seed gives the same decisions, another seed others")
  {
    plNetworkSimulationSettings settings = plNetworkSimulationSettings::MakePreset(plNetworkSimulationPreset::Bad);
    settings.m_uiBandwidthLimitBps = 0;
    settings.m_uiSeed = 1234;

    plNetworkSimulator first, second, other;
    first.SetSettings(settings);
    second.SetSettings(settings);
    settings.m_uiSeed = 4321;
    other.SetSettings(settings);

    const auto a = ScheduleMany(first, plNetworkChannelTypeEnum::Unreliable, 300);
    const auto b = ScheduleMany(second, plNetworkChannelTypeEnum::Unreliable, 300);
    const auto c = ScheduleMany(other, plNetworkChannelTypeEnum::Unreliable, 300);

    bool bSame = true;
    bool bDiffers = false;
    for (plUInt32 i = 0; i < a.GetCount(); ++i)
    {
      bSame = bSame && a[i].m_bDeliver == b[i].m_bDeliver && a[i].m_DeliveryTime == b[i].m_DeliveryTime;
      bDiffers = bDiffers || a[i].m_bDeliver != c[i].m_bDeliver || a[i].m_DeliveryTime != c[i].m_DeliveryTime;
    }
    PL_TEST_BOOL(bSame);
    PL_TEST_BOOL(bDiffers);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D39: reliable messages are delayed by loss, never dropped, and keep their order")
  {
    plNetworkSimulationSettings settings = plNetworkSimulationSettings::MakePreset(plNetworkSimulationPreset::Bad);
    settings.m_uiBandwidthLimitBps = 0; // 100 bytes per millisecond would saturate the link and drop for that reason.
    settings.m_uiSeed = 99;

    plNetworkSimulator simulator;
    simulator.SetSettings(settings);
    const auto decisions = ScheduleMany(simulator, plNetworkChannelTypeEnum::ReliableOrdered, 500);

    bool bAllDelivered = true;
    bool bOrdered = true;
    for (plUInt32 i = 0; i < decisions.GetCount(); ++i)
    {
      bAllDelivered = bAllDelivered && decisions[i].m_bDeliver && !decisions[i].m_bDuplicate;
      bOrdered = bOrdered && (i == 0 || decisions[i - 1].m_DeliveryTime <= decisions[i].m_DeliveryTime);
    }
    PL_TEST_BOOL(bAllDelivered);
    PL_TEST_BOOL(bOrdered);
    PL_TEST_BOOL(simulator.GetResentCount() > 0);

    const auto unreliable = ScheduleMany(simulator, plNetworkChannelTypeEnum::Unreliable, 500);
    plUInt32 uiDropped = 0;
    for (const auto& decision : unreliable)
      uiDropped += decision.m_bDeliver ? 0 : 1;
    PL_TEST_BOOL_MSG(uiDropped > 25 && uiDropped < 150, "%u of 500 unreliable messages dropped at 15 %% loss", uiDropped);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D39: each peer simulates its own conditions on send and receive")
  {
    NetworkEndpoint host, a, b;
    a.m_Simulation.m_bEnabled = true;
    a.m_Simulation.m_fLatencyMs = 40.0f;
    a.m_Simulation.m_uiSeed = 1;

    const Pumped all({&host, &a, &b});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a, &b})))
      return;

    if (!PL_TEST_BOOL(WaitForTimeSync(all, a) && WaitForTimeSync(all, b)))
      return;

    const double fRttA = a.m_pModule->GetRTT().GetMilliseconds();
    const double fRttB = b.m_pModule->GetRTT().GetMilliseconds();
    PL_TEST_BOOL_MSG(fRttA >= 75.0 && fRttA < 300.0, "A has 40 ms each way and measured %.1f ms of round trip", fRttA);
    PL_TEST_BOOL_MSG(fRttB < 40.0, "B simulates nothing and measured %.1f ms of round trip", fRttB);

    // Conditions change while the session runs.
    plNetworkSimulationSettings none;
    a.Simulate(none);
    a.m_pModule->GetNetworkTime()->RequestSync();
    PumpFor(all, plTime::MakeFromMilliseconds(300));
    PL_TEST_BOOL(a.m_pModule->IsConnected());
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D39: peers without settings follow the net_sim CVars")
  {
    SimulationCVarScope cvars;
    *static_cast<plCVarFloat*>(plCVar::FindCVarByName("net_sim_latency")) = 30.0f;
    *static_cast<plCVarBool*>(plCVar::FindCVarByName("net_sim_enabled")) = true;

    NetworkEndpoint host, a;
    const Pumped all({&host, &a});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a})))
      return;

    if (!PL_TEST_BOOL(WaitForTimeSync(all, a)))
      return;

    // Both peers follow the CVars, so each direction is delayed twice: 4 x 30 ms of round trip.
    const double fRtt = a.m_pModule->GetRTT().GetMilliseconds();
    PL_TEST_BOOL_MSG(fRtt >= 110.0 && fRtt < 400.0, "measured %.1f ms of round trip", fRtt);
  }
}
