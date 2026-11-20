#include "NetworkTestPCH.h"

#include "NetworkTestHarness.h"

using namespace NetworkTestHarness;

namespace
{
  using Role = NetworkEndpoint::Role;

  plNetworkCounterComponent* AddCounter(NetworkEndpoint& endpoint)
  {
    PL_LOCK(endpoint.m_World.GetWriteMarker());
    plGameObjectDesc desc;
    desc.m_sName.Assign("Counter");
    desc.m_bDynamic = true;
    plGameObject* pObject = nullptr;
    endpoint.m_World.CreateObject(desc, pObject);
    plNetworkCounterComponent* pCounter = AddComponent<plNetworkCounterComponent>(endpoint.m_World, pObject);
    endpoint.m_World.Update();
    return pCounter;
  }

  /// \brief Restores the network clock when it goes out of scope.
  struct ClockOffsetScope
  {
    explicit ClockOffsetScope(plTime offset) { plNetworkClock::SetOffset(offset); }
    ~ClockOffsetScope() { plNetworkClock::SetOffset(plTime::MakeZero()); }
  };

  /// \brief Changes the host's counter every frame for the duration and returns how many updates the client applied per second.
  double MeasureCounterRate(const Pumped& pumped, NetworkEndpoint& host, plNetworkCounterComponent* pHostCounter, plNetworkCounterComponent* pClientCounter, plTime duration)
  {
    const plUInt32 uiBefore = pClientCounter->m_uiReceived;
    const plTime start = plTime::Now();
    plUInt32 uiValue = 1000;
    while (plTime::Now() - start < duration)
    {
      {
        PL_LOCK(host.m_World.GetWriteMarker());
        pHostCounter->Set(++uiValue);
      }
      PumpOnce(pumped);
      plThreadUtils::YieldTimeSlice();
    }

    return (pClientCounter->m_uiReceived - uiBefore) / duration.GetSeconds();
  }

  /// \brief Changes the host's replica every frame and returns how many distinct values the client saw per second.
  double MeasureReplicaRate(const Pumped& pumped, NetworkEndpoint& host, NetworkEndpoint& client, plTime duration)
  {
    plUInt32 uiSeen = 0;
    plInt32 iLast = client.m_Replicas[0].m_pTarget->m_iValue;
    const plTime start = plTime::Now();
    plInt32 iValue = 1000;
    while (plTime::Now() - start < duration)
    {
      {
        PL_LOCK(host.m_World.GetWriteMarker());
        host.m_Replicas[0].m_pTarget->m_iValue = ++iValue;
      }
      PumpOnce(pumped);
      if (client.m_Replicas[0].m_pTarget->m_iValue != iLast)
      {
        iLast = client.m_Replicas[0].m_pTarget->m_iValue;
        ++uiSeen;
      }
      plThreadUtils::YieldTimeSlice();
    }

    return uiSeen / duration.GetSeconds();
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Network, AdmissionUnderLatency)
{
  for (Role role : {Role::Host, Role::Server})
  {
    PL_TEST_BLOCK(plTestBlock::Enabled, "D01: a client joining under latency is not stopped by state sent before its admission")
    {
      NetworkEndpoint server, a;
      plNetworkedTransformComponent* pMover = nullptr;
      server.AddMover("Mover", &pMover);
      a.AddMover("Mover");
      pMover->m_fSendRate = 60.0f;

      a.m_Simulation.m_bEnabled = true;
      a.m_Simulation.m_fLatencyMs = 100.0f;
      a.m_Simulation.m_uiSeed = 3;

      const Pumped all({&server, &a});
      if (!PL_TEST_BOOL(server.StartAs(role) && a.Start(false, server.GetPort())))
        return;

      PumpAll(all, [&] { return a.m_pModule->IsConnected(); }, plTime::MakeFromSeconds(3));
      PumpFor(all, plTime::MakeFromMilliseconds(700));
      PL_TEST_BOOL_MSG(a.m_pModule->IsConnected() && server.FindClient(a) != nullptr, "%s: the client stopped with reason %d",
        role == Role::Host ? "host" : "dedicated server", a.m_pModule->GetLastDisconnectReason());
    }
  }
}

PL_CREATE_SIMPLE_TEST(Network, ReplicationDefaults)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "D23: state on the default channel survives a lost packet")
  {
    NetworkEndpoint host, a;
    plNetworkCounterComponent* pHostCounter = AddCounter(host);
    plNetworkCounterComponent* pCounterA = AddCounter(a);

    const Pumped all({&host, &a});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a})))
      return;

    plNetworkSimulationSettings lossy;
    lossy.m_bEnabled = true;
    lossy.m_fPacketLossPercent = 100.0f;
    lossy.m_uiSeed = 5;
    a.Simulate(lossy);

    {
      PL_LOCK(host.m_World.GetWriteMarker());
      pHostCounter->Set(5);
    }
    PumpFor(all, plTime::MakeFromMilliseconds(200));

    a.Simulate(plNetworkSimulationSettings());
    PL_TEST_BOOL_MSG(PumpAll(all, [&] { return pCounterA->m_uiValue == 5; }, plTime::MakeFromSeconds(2)), "the client holds %u", pCounterA->m_uiValue);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D24: the default priority does not cap how often a component is sent")
  {
    NetworkEndpoint host, a;
    plNetworkCounterComponent* pHostCounter = AddCounter(host);
    plNetworkCounterComponent* pCounterA = AddCounter(a);

    const Pumped all({&host, &a});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a})))
      return;

    const double fRate = MeasureCounterRate(all, host, pHostCounter, pCounterA, plTime::MakeFromSeconds(1));
    plLog::Info("NetworkRate: counter, default priority, no tick limit = {} updates/s", plArgF(fRate, 1));
    PL_TEST_BOOL_MSG(fRate >= 50.0, "%.1f updates per second", fRate);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D24: a properties component with a 60 Hz send rate is sent at 60 Hz")
  {
    NetworkEndpoint host, a;
    host.AddReplica("Crate", plReplicateAuthority::Server);
    a.AddReplica("Crate", plReplicateAuthority::Server);
    host.m_pModule->SetTickRate(60.0f);

    const Pumped all({&host, &a});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a})))
      return;

    const double fRate = MeasureReplicaRate(all, host, a, plTime::MakeFromSeconds(2));
    plLog::Info("NetworkRate: properties at 60 Hz, tick 60 Hz = {} updates/s", plArgF(fRate, 1));
    PL_TEST_BOOL_MSG(fRate >= 50.0, "%.1f updates per second", fRate);
  }
}

PL_CREATE_SIMPLE_TEST(Network, LongUptime)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "D04: after 30 days of uptime a 60 Hz tick still replicates at 60 Hz")
  {
    ClockOffsetScope clock(plTime::MakeFromHours(24.0 * 30.0));

    NetworkEndpoint host, a;
    plNetworkCounterComponent* pHostCounter = AddCounter(host);
    plNetworkCounterComponent* pCounterA = AddCounter(a);
    host.m_pModule->SetTickRate(60.0f);

    // Isolates the tick from the priority intervals that D24 covers.
    plNetworkBandwidthConfig bandwidth = host.m_pModule->GetBandwidthManager()->GetConfig();
    bandwidth.m_bEnablePriorityScheduling = false;
    host.m_pModule->GetBandwidthManager()->SetConfig(bandwidth);

    const Pumped all({&host, &a});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a})))
      return;

    const double fRate = MeasureCounterRate(all, host, pHostCounter, pCounterA, plTime::MakeFromSeconds(2));
    plLog::Info("NetworkRate: tick 60 Hz after 30 days of uptime = {} updates/s", plArgF(fRate, 1));
    PL_TEST_BOOL_MSG(fRate >= 55.0 && fRate <= 65.0, "%.1f updates per second", fRate);
  }
}

namespace
{
  struct ConditionPrefabs
  {
    TestPrefabs m_Library;
    plHashedString m_sPawn;

    ConditionPrefabs()
    {
      m_sPawn = m_Library.Add("ConditionPawn", [](plWorld& ref_world, plGameObject* pRoot)
        {
          AddComponent<plNetworkReplicaTargetComponent>(ref_world, pRoot);
          ReplicateTarget(AddComponent<plNetworkedPropertiesComponent>(ref_world, pRoot), plReplicateAuthority::Server);
          AddComponent<plNetworkRpcProbeComponent>(ref_world, pRoot);
        });
    }
  };

  plInt32 SpawnedValue(NetworkEndpoint& endpoint, plUInt32 uiObjectID)
  {
    plNetworkReplicaTargetComponent* pTarget = endpoint.FindSpawnedComponent<plNetworkReplicaTargetComponent>(uiObjectID);
    return pTarget ? pTarget->m_iValue : -1;
  }

  bool HasArguments(NetworkEndpoint& endpoint, plUInt32 uiObjectID, plUInt32 uiCount)
  {
    plNetworkRpcProbeComponent* pProbe = endpoint.FindSpawnedComponent<plNetworkRpcProbeComponent>(uiObjectID);
    if (pProbe == nullptr || pProbe->m_Arguments.GetCount() != uiCount)
      return false;

    for (plUInt32 i = 0; i < uiCount; ++i)
    {
      if (pProbe->m_Arguments[i] != static_cast<plInt32>(i))
        return false;
    }
    return true;
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Network, ReplicationUnderConditions)
{
  struct Case
  {
    const char* m_szName;
    plNetworkSimulationPreset::Enum m_Preset;
  };

  const Case cases[] = {
    {"LAN", plNetworkSimulationPreset::LAN},
    {"Broadband", plNetworkSimulationPreset::Broadband},
    {"Mobile 4G", plNetworkSimulationPreset::Mobile4G},
    {"Mobile 3G", plNetworkSimulationPreset::Mobile3G},
    {"Satellite", plNetworkSimulationPreset::Satellite},
    {"Bad", plNetworkSimulationPreset::Bad},
  };

  for (const Case& c : cases)
  {
    PL_TEST_BLOCK(plTestBlock::Enabled, "Scene state, spawns, RPCs and transforms converge under each preset")
    {
      const plTime timeout = plTime::MakeFromSeconds(20);
      ConditionPrefabs prefabs;
      NetworkEndpoint host, a, b;
      for (NetworkEndpoint* pEndpoint : {&host, &a, &b})
      {
        pEndpoint->RegisterPrefab(prefabs.m_sPawn);
        pEndpoint->AddReplica("Crate", plReplicateAuthority::Server);
      }

      plGameObject* pMover = host.AddMover("Mover");
      plGameObject* pMoverA = a.AddMover("Mover");
      plGameObject* pMoverB = b.AddMover("Mover");

      a.m_Simulation = plNetworkSimulationSettings::MakePreset(c.m_Preset);
      a.m_Simulation.m_uiSeed = 11;
      b.m_Simulation = plNetworkSimulationSettings::MakePreset(c.m_Preset);
      b.m_Simulation.m_uiSeed = 22;

      const Pumped all({&host, &a, &b});
      if (!PL_TEST_BOOL_MSG(ConnectAllAs(Role::Host, all, host, {&a, &b}, timeout), "%s: connect", c.m_szName))
        return;

      // Scene state: thirty changes, the last one wins.
      for (plInt32 i = 1; i <= 30; ++i)
      {
        {
          PL_LOCK(host.m_World.GetWriteMarker());
          host.m_Replicas[0].m_pTarget->m_iValue = i;
        }
        PumpFor(all, plTime::MakeFromMilliseconds(15));
      }
      PL_TEST_BOOL_MSG(PumpAll(all, [&] { return a.m_Replicas[0].m_pTarget->m_iValue == 30 && b.m_Replicas[0].m_pTarget->m_iValue == 30; }, timeout),
        "%s: scene state ends at %d and %d", c.m_szName, a.m_Replicas[0].m_pTarget->m_iValue, b.m_Replicas[0].m_pTarget->m_iValue);

      // Spawns with state, one despawned again.
      const plUInt32 uiKept = host.Spawn(prefabs.m_sPawn, 0);
      const plUInt32 uiGone = host.Spawn(prefabs.m_sPawn, 0);
      PumpFor(all, plTime::MakeFromMilliseconds(50));
      {
        PL_LOCK(host.m_World.GetWriteMarker());
        host.FindSpawnedComponent<plNetworkReplicaTargetComponent>(uiKept)->m_iValue = 55;
      }
      host.Despawn(uiGone);
      PL_TEST_BOOL_MSG(PumpAll(all, [&] { return SpawnedValue(a, uiKept) == 55 && SpawnedValue(b, uiKept) == 55 && !a.FindSpawned(uiGone) && !b.FindSpawned(uiGone); }, timeout),
        "%s: spawned state ends at %d and %d", c.m_szName, SpawnedValue(a, uiKept), SpawnedValue(b, uiKept));

      // Reliable RPCs: every call once, in order.
      {
        PL_LOCK(host.m_World.GetWriteMarker());
        plNetworkRpcProbeComponent* pProbe = host.FindSpawnedComponent<plNetworkRpcProbeComponent>(uiKept);
        for (plInt32 i = 0; i < 10; ++i)
          host.m_pModule->GetRpcManager()->CallRpc(pProbe, "RpcAllClients", i);
      }
      PL_TEST_BOOL_MSG(PumpAll(all, [&] { return HasArguments(a, uiKept, 10) && HasArguments(b, uiKept, 10); }, timeout), "%s: RPCs", c.m_szName);

      // A transform that stops moving ends where it stopped.
      {
        PL_LOCK(host.m_World.GetWriteMarker());
        pMover->SetGlobalPosition(plVec3(5.0f, 5.0f, 5.0f));
      }
      PL_TEST_BOOL_MSG(PumpAll(all, [&] { return pMoverA->GetGlobalPosition().IsEqual(plVec3(5.0f), 0.01f) && pMoverB->GetGlobalPosition().IsEqual(plVec3(5.0f), 0.01f); }, timeout),
        "%s: transform", c.m_szName);

      PL_TEST_BOOL_MSG(a.m_pModule->IsConnected() && b.m_pModule->IsConnected(), "%s: a client disconnected", c.m_szName);
    }
  }
}
