#include "NetworkTestPCH.h"

#include <Core/World/World.h>
#include <NetworkPlugin/Core/NetworkBandwidth.h>

#include "NetworkBudgets.h"
#include "NetworkTestHarness.h"

using namespace NetworkTestHarness;

namespace
{
  constexpr plUInt32 s_uiReplicaCount = 16;

  /// \brief Prints the measurement for the CI log and checks it against its ceiling.
  bool WithinBudget(const char* szName, plUInt64 uiMeasured, plUInt64 uiCeiling)
  {
    plTestFramework::Output(plTestOutput::ImportantInfo, "NetworkBudget: %s = %llu bytes (ceiling %llu)", szName,
      static_cast<unsigned long long>(uiMeasured), static_cast<unsigned long long>(uiCeiling));
    return PL_TEST_BOOL_MSG(uiMeasured <= uiCeiling, "%s used %llu bytes, over its ceiling of %llu", szName,
      static_cast<unsigned long long>(uiMeasured), static_cast<unsigned long long>(uiCeiling));
  }

  struct ReceivedSnapshot
  {
    explicit ReceivedSnapshot(std::initializer_list<NetworkEndpoint*> endpoints)
    {
      for (NetworkEndpoint* pEndpoint : endpoints)
      {
        m_Endpoints.PushBack(pEndpoint);
        m_Before.PushBack(pEndpoint->BytesReceived());
      }
    }

    plUInt64 Delta(plUInt32 uiIndex) const { return m_Endpoints[uiIndex]->BytesReceived() - m_Before[uiIndex]; }
    plUInt64 Max() const
    {
      plUInt64 uiMax = 0;
      for (plUInt32 i = 0; i < m_Endpoints.GetCount(); ++i)
        uiMax = plMath::Max(uiMax, Delta(i));
      return uiMax;
    }

    plHybridArray<NetworkEndpoint*, 4> m_Endpoints;
    plHybridArray<plUInt64, 4> m_Before;
  };

  void AddReplicas(std::initializer_list<NetworkEndpoint*> endpoints, plUInt32 uiCount)
  {
    for (NetworkEndpoint* pEndpoint : endpoints)
    {
      for (plUInt32 i = 0; i < uiCount; ++i)
      {
        plStringBuilder sName;
        sName.SetFormat("Crate{}", i);
        pEndpoint->AddReplica(sName, plReplicateAuthority::Server);
      }
    }
  }

  void SetValues(NetworkEndpoint& host, plUInt32 uiFirst, plUInt32 uiCount, plInt32 iValue)
  {
    PL_LOCK(host.m_World.GetWriteMarker());
    for (plUInt32 i = uiFirst; i < uiFirst + uiCount; ++i)
    {
      host.m_Replicas[i].m_pTarget->m_iValue = iValue;
      host.m_Replicas[i].m_pTarget->m_vPosition.Set(static_cast<float>(iValue), 0.5f, -0.25f);
    }
  }

  bool HasValues(NetworkEndpoint& client, plUInt32 uiFirst, plUInt32 uiCount, plInt32 iValue)
  {
    for (plUInt32 i = uiFirst; i < uiFirst + uiCount; ++i)
    {
      if (client.m_Replicas[i].m_pTarget->m_iValue != iValue)
        return false;
    }
    return true;
  }

  /// \brief Moves the host object a little every frame for the duration, so the transform always has something to send.
  void MoveFor(const Pumped& pumped, NetworkEndpoint& host, plGameObject* pObject, plTime duration)
  {
    const plTime start = plTime::Now();
    while (plTime::Now() - start < duration)
    {
      {
        PL_LOCK(host.m_World.GetWriteMarker());
        const float fT = static_cast<float>((plTime::Now() - start).GetSeconds());
        pObject->SetGlobalPosition(plVec3(fT * 4.0f, 2.0f, 3.0f));
      }
      PumpOnce(pumped);
      plThreadUtils::YieldTimeSlice();
    }
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Network, BandwidthBudget)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "Replicated properties stay inside their byte budgets")
  {
    NetworkEndpoint host, a, b;
    AddReplicas({&host, &a, &b}, s_uiReplicaCount);

    const Pumped all({&host, &a, &b});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a, &b})))
      return;

    // Let the initial sync and the first time-sync rounds finish before anything is measured.
    PumpFor(all, plTime::MakeFromMilliseconds(500));

    {
      const ReceivedSnapshot received({&a, &b});
      PumpFor(all, plTime::MakeFromSeconds(2));
      WithinBudget("Idle, per client, 2 s", received.Max(), NetworkBudgets::kIdlePerClient2s);
    }

    plUInt64 uiOneReplica = 0;
    {
      const ReceivedSnapshot received({&a, &b});
      SetValues(host, 0, 1, 1);
      if (!PL_TEST_BOOL(PumpAll(all, [&] { return HasValues(a, 0, 1, 1) && HasValues(b, 0, 1, 1); })))
        return;
      uiOneReplica = received.Max();
      WithinBudget("One replica changed, per client", uiOneReplica, NetworkBudgets::kOneReplicaUpdatePerClient);
    }

    {
      const plUInt64 uiHostSentBefore = host.BytesSent();
      const ReceivedSnapshot received({&a, &b});
      SetValues(host, 0, s_uiReplicaCount, 2);
      if (!PL_TEST_BOOL(PumpAll(all, [&] { return HasValues(a, 0, s_uiReplicaCount, 2) && HasValues(b, 0, s_uiReplicaCount, 2); })))
        return;

      const plUInt64 uiSixteen = received.Max();
      WithinBudget("Sixteen replicas changed, per client", uiSixteen, NetworkBudgets::kSixteenReplicaUpdatePerClient);
      PL_TEST_BOOL_MSG(uiSixteen <= s_uiReplicaCount * uiOneReplica, "Sixteen changes in one frame cost %llu bytes, more than sixteen separate changes (%llu)",
        static_cast<unsigned long long>(uiSixteen), static_cast<unsigned long long>(s_uiReplicaCount * uiOneReplica));

      // Each client gets one copy of each change. The host's upload is the two copies plus acks, not a relay per pair.
      const plUInt64 uiA = received.Delta(0);
      const plUInt64 uiB = received.Delta(1);
      PL_TEST_BOOL_MSG(plMath::Max(uiA, uiB) <= plMath::Min(uiA, uiB) + plMath::Max<plUInt64>(64, plMath::Min(uiA, uiB) / 4),
        "The clients received different amounts for the same change: %llu and %llu", static_cast<unsigned long long>(uiA),
        static_cast<unsigned long long>(uiB));
      const plUInt64 uiHostSent = host.BytesSent() - uiHostSentBefore;
      PL_TEST_BOOL_MSG(uiHostSent <= (uiA + uiB) + (uiA + uiB) / 4 + 128, "The host sent %llu bytes for %llu received in total",
        static_cast<unsigned long long>(uiHostSent), static_cast<unsigned long long>(uiA + uiB));
    }

    {
      const ReceivedSnapshot received({&a, &b});
      PumpFor(all, plTime::MakeFromSeconds(1));
      WithinBudget("Sixteen unchanged replicas, per client, 1 s", received.Max(), NetworkBudgets::kUnchangedReplicasPerClient1s);
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "A replicated transform stays inside its byte budget")
  {
    NetworkEndpoint host, a, b;
    plGameObject* pHostMover = host.AddMover("Mover");
    a.AddMover("Mover");
    b.AddMover("Mover");

    const Pumped all({&host, &a, &b});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a, &b})))
      return;
    PumpFor(all, plTime::MakeFromMilliseconds(500));

    {
      const ReceivedSnapshot received({&a, &b});
      MoveFor(all, host, pHostMover, plTime::MakeFromSeconds(1));
      WithinBudget("Moving transform, per client, 1 s", received.Max(), NetworkBudgets::kMovingTransformPerClient1s);
    }

    {
      const ReceivedSnapshot received({&a, &b});
      PumpFor(all, plTime::MakeFromSeconds(1));
      WithinBudget("Stationary transform, per client, 1 s", received.Max(), NetworkBudgets::kStationaryTransformPerClient1s);
    }
  }

  // plNetworkBandwidthConfig::m_uiMaxBytesPerTick is recorded but the replication send loop does not enforce it yet.
  PL_TEST_BLOCK(plTestBlock::Disabled, "The configured per-frame byte budget is honoured")
  {
    constexpr plUInt32 uiReplicas = 32;
    constexpr plUInt32 uiBudget = 512;

    NetworkEndpoint host, a;
    AddReplicas({&host, &a}, uiReplicas);

    const Pumped all({&host, &a});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a})))
      return;
    PumpFor(all, plTime::MakeFromMilliseconds(500));

    {
      PL_LOCK(host.m_World.GetWriteMarker());
      plNetworkBandwidthConfig config = host.m_pModule->GetBandwidthManager()->GetConfig();
      config.m_uiMaxBytesPerTick = uiBudget;
      host.m_pModule->GetBandwidthManager()->SetConfig(config);
    }

    SetValues(host, 0, uiReplicas, 3);
    plUInt64 uiWorstFrame = 0;
    PL_TEST_BOOL(PumpAll(all, [&]
      {
        const plUInt64 uiBefore = host.BytesSent();
        host.Pump();
        uiWorstFrame = plMath::Max(uiWorstFrame, host.BytesSent() - uiBefore);
        a.Pump();
        return HasValues(a, 0, uiReplicas, 3);
      }));
    WithinBudget("Worst host frame with a 512 byte budget", uiWorstFrame, uiBudget + 128);
  }
}
