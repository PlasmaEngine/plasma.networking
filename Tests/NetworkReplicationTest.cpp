#include "NetworkTestPCH.h"

#include <Core/World/World.h>
#include <NetworkPlugin/Components/NetworkedPropertiesComponent.h>
#include <NetworkPlugin/Components/NetworkedTransformComponent.h>

#include "NetworkTestHarness.h"

using namespace NetworkTestHarness;

PL_BEGIN_COMPONENT_TYPE(plNetworkReplicaTargetComponent, 1, plComponentMode::Dynamic)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Value", m_iValue),
    PL_MEMBER_PROPERTY("Position", m_vPosition),
  }
  PL_END_PROPERTIES;
}
PL_END_COMPONENT_TYPE

namespace
{
  void SetReplica(NetworkEndpoint& endpoint, plUInt32 uiReplica, plInt32 iValue, const plVec3& vPosition)
  {
    PL_LOCK(endpoint.m_World.GetWriteMarker());
    endpoint.m_Replicas[uiReplica].m_pTarget->m_iValue = iValue;
    endpoint.m_Replicas[uiReplica].m_pTarget->m_vPosition = vPosition;
  }

  void SetReplicaValue(NetworkEndpoint& endpoint, plUInt32 uiReplica, plInt32 iValue)
  {
    PL_LOCK(endpoint.m_World.GetWriteMarker());
    endpoint.m_Replicas[uiReplica].m_pTarget->m_iValue = iValue;
  }

  plInt32 ReplicaValue(NetworkEndpoint& endpoint, plUInt32 uiReplica) { return endpoint.m_Replicas[uiReplica].m_pTarget->m_iValue; }
  plVec3 ReplicaPosition(NetworkEndpoint& endpoint, plUInt32 uiReplica) { return endpoint.m_Replicas[uiReplica].m_pTarget->m_vPosition; }

  /// \brief Gives every endpoint the same ownership record, the way a spawn would.
  void OwnReplica(std::initializer_list<NetworkEndpoint*> endpoints, plUInt32 uiReplica, plUInt32 uiObjectID, plUInt32 uiOwnerClientID)
  {
    for (NetworkEndpoint* pEndpoint : endpoints)
    {
      PL_LOCK(pEndpoint->m_World.GetWriteMarker());
      plNetworkTestAccess::Own(pEndpoint->m_pModule, pEndpoint->m_Replicas[uiReplica].m_pObject, uiObjectID, uiOwnerClientID);
    }
  }

  bool HasTransform(plGameObject* pObject, const plVec3& vPosition, const plQuat& qRotation)
  {
    return pObject->GetGlobalPosition().IsEqual(vPosition, 0.01f) &&
           pObject->GetGlobalRotation().IsEqualRotation(qRotation, plAngle::MakeFromDegree(1.0f).GetRadian());
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Network, TwoClientReplication)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "Host-authoritative properties reach both clients, and only changed entries are sent")
  {
    NetworkEndpoint host, a, b;
    for (NetworkEndpoint* pEndpoint : {&host, &a, &b})
      pEndpoint->AddReplica("Crate", plReplicateAuthority::Server);

    const Pumped all({&host, &a, &b});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a, &b})))
      return;

    SetReplica(host, 0, 7, plVec3(1.0f, 2.0f, 3.0f));
    PL_TEST_BOOL(PumpAll(all, [&]
      { return ReplicaValue(a, 0) == 7 && ReplicaValue(b, 0) == 7 && ReplicaPosition(a, 0) == plVec3(1.0f, 2.0f, 3.0f) && ReplicaPosition(b, 0) == plVec3(1.0f, 2.0f, 3.0f); }));

    // A client-side marker in Position survives an update that changes only Value, so Position was not in the bitmask.
    SetReplica(a, 0, 7, plVec3(9.0f));
    SetReplica(b, 0, 7, plVec3(9.0f));
    SetReplicaValue(host, 0, 8);
    PL_TEST_BOOL(PumpAll(all, [&] { return ReplicaValue(a, 0) == 8 && ReplicaValue(b, 0) == 8; }));
    PL_TEST_VEC3(ReplicaPosition(a, 0), plVec3(9.0f), 0.0f);
    PL_TEST_VEC3(ReplicaPosition(b, 0), plVec3(9.0f), 0.0f);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "A host-authoritative scene transform reaches both clients")
  {
    NetworkEndpoint host, a, b;
    plGameObject* pHostMover = host.AddMover("Mover");
    plGameObject* pMoverA = a.AddMover("Mover");
    plGameObject* pMoverB = b.AddMover("Mover");

    const Pumped all({&host, &a, &b});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a, &b})))
      return;

    const plVec3 vTarget(5.0f, -3.0f, 2.0f);
    const plQuat qTarget = plQuat::MakeFromAxisAndAngle(plVec3::MakeAxisZ(), plAngle::MakeFromDegree(90.0f));
    {
      PL_LOCK(host.m_World.GetWriteMarker());
      pHostMover->SetGlobalPosition(vTarget);
      pHostMover->SetGlobalRotation(qTarget);
    }

    PL_TEST_BOOL(PumpAll(all, [&] { return HasTransform(pMoverA, vTarget, qTarget) && HasTransform(pMoverB, vTarget, qTarget); }));
    PL_TEST_BOOL_MSG(HasTransform(pHostMover, vTarget, qTarget), "The host must keep the transform it simulates.");
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "An owning client's upload is relayed by the host to the other client")
  {
    NetworkEndpoint host, a, b;
    for (NetworkEndpoint* pEndpoint : {&host, &a, &b})
      pEndpoint->AddReplica("Pawn", plReplicateAuthority::Owner);

    const Pumped all({&host, &a, &b});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a, &b})))
      return;

    OwnReplica({&host, &a, &b}, 0, 500, a.m_pModule->GetLocalClientID());

    SetReplicaValue(a, 0, 21);
    PL_TEST_BOOL(PumpAll(all, [&] { return ReplicaValue(host, 0) == 21 && ReplicaValue(b, 0) == 21; }));

    // B does not own the pawn, so its local edit is never sent and the other peers keep the owner's value.
    SetReplicaValue(b, 0, 99);
    PumpFor(all, plTime::MakeFromMilliseconds(300));
    PL_TEST_INT(ReplicaValue(host, 0), 21);
    PL_TEST_INT(ReplicaValue(a, 0), 21);

    SetReplicaValue(a, 0, 22);
    PL_TEST_BOOL(PumpAll(all, [&] { return ReplicaValue(host, 0) == 22 && ReplicaValue(b, 0) == 22; }));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "A client that joins late receives the current state without a new change")
  {
    NetworkEndpoint host, a, b;
    for (NetworkEndpoint* pEndpoint : {&host, &a, &b})
      pEndpoint->AddReplica("Crate", plReplicateAuthority::Server);

    const Pumped all({&host, &a, &b});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a})))
      return;

    SetReplica(host, 0, 41, plVec3(4.0f, 1.0f, 0.0f));
    if (!PL_TEST_BOOL(PumpAll(all, [&] { return ReplicaValue(a, 0) == 41; })))
      return;

    if (!PL_TEST_BOOL(b.Start(false, host.GetPort())))
      return;
    PL_TEST_BOOL(PumpAll(all, [&] { return b.m_pModule->IsConnected() && ReplicaValue(b, 0) == 41 && ReplicaPosition(b, 0) == plVec3(4.0f, 1.0f, 0.0f); }));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "200 consecutive changes end on the last value on both clients with no packets lost")
  {
    NetworkEndpoint host, a, b;
    for (NetworkEndpoint* pEndpoint : {&host, &a, &b})
      pEndpoint->AddReplica("Counter", plReplicateAuthority::Server);

    const Pumped all({&host, &a, &b});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a, &b})))
      return;

    for (plInt32 i = 1; i <= 200; ++i)
    {
      SetReplicaValue(host, 0, i);
      PumpOnce(all);
    }

    PL_TEST_BOOL(PumpAll(all, [&] { return ReplicaValue(a, 0) == 200 && ReplicaValue(b, 0) == 200; }));
    for (NetworkEndpoint* pClient : {&a, &b})
      PL_TEST_INT(pClient->m_pModule->GetPeer()->GetDriver()->GetStats().m_uiTotalPacketsLost, 0);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Teardown: the host stops with two clients connected")
  {
    const plUInt32 uiPeersBefore = plNetworkPeer::GetPeerCount();
    NetworkEndpoint host, a, b;
    const Pumped all({&host, &a, &b});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a, &b})))
      return;

    host.Stop();
    PL_TEST_BOOL(PumpAll(all, [&] { return a.m_pModule->GetPeer() == nullptr && b.m_pModule->GetPeer() == nullptr; }));
    PL_TEST_BOOL(!a.m_pModule->IsConnected() && !b.m_pModule->IsConnected());
    PL_TEST_INT(plNetworkPeer::GetPeerCount(), uiPeersBefore);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Teardown: a client world destroyed without Stop leaves the host promptly")
  {
    const plUInt32 uiPeersBefore = plNetworkPeer::GetPeerCount();
    {
      NetworkEndpoint host, a;
      NetworkEndpoint* pB = PL_DEFAULT_NEW(NetworkEndpoint);
      if (!PL_TEST_BOOL(ConnectAll(Pumped({&host, &a, pB}), host, {&a, pB})))
      {
        PL_DEFAULT_DELETE(pB);
        return;
      }

      PL_DEFAULT_DELETE(pB);
      const Pumped remaining({&host, &a});
      PL_TEST_BOOL_MSG(PumpAll(remaining, [&] { return host.m_pModule->GetClients().GetCount() == 1; }, plTime::MakeFromSeconds(2)),
        "The destroyed client's disconnect must reach the host, not wait for a timeout.");
      PL_TEST_BOOL(a.m_pModule->IsConnected());
      PL_TEST_INT(plNetworkPeer::GetPeerCount(), uiPeersBefore + 2);
    }
    PL_TEST_INT(plNetworkPeer::GetPeerCount(), uiPeersBefore);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Teardown: ten connect, replicate and destroy rounds leave no peers behind")
  {
    const plUInt32 uiPeersBefore = plNetworkPeer::GetPeerCount();
    for (plInt32 iRound = 0; iRound < 10; ++iRound)
    {
      {
        NetworkEndpoint host, a, b;
        for (NetworkEndpoint* pEndpoint : {&host, &a, &b})
          pEndpoint->AddReplica("Crate", plReplicateAuthority::Server);

        const Pumped all({&host, &a, &b});
        if (!PL_TEST_BOOL(ConnectAll(all, host, {&a, &b})))
          return;

        SetReplicaValue(host, 0, 100 + iRound);
        if (!PL_TEST_BOOL(PumpAll(all, [&] { return ReplicaValue(a, 0) == 100 + iRound && ReplicaValue(b, 0) == 100 + iRound; })))
          return;
      }

      if (!PL_TEST_INT(plNetworkPeer::GetPeerCount(), uiPeersBefore))
        return;
    }
  }
}
