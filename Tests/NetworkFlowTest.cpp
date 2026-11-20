#include "NetworkTestPCH.h"

#include <NetworkPlugin/Core/NetworkLevelManager.h>

#include "NetworkTestHarness.h"

using namespace NetworkTestHarness;

namespace
{
  using Role = NetworkEndpoint::Role;

  /// \brief The prefabs the flow tests spawn, registered on every endpoint.
  struct FlowPrefabs
  {
    TestPrefabs m_Library;

    /// Server-authoritative Value and Position on the root and on a child "Arm", plus an RPC probe on the root.
    plHashedString m_sPawn;

    /// Owner-authoritative Value and Position plus a transform the owner simulates.
    plHashedString m_sOwnedPawn;

    FlowPrefabs()
    {
      m_sPawn = m_Library.Add("Pawn", [](plWorld& ref_world, plGameObject* pRoot)
        {
          AddComponent<plNetworkReplicaTargetComponent>(ref_world, pRoot);
          ReplicateTarget(AddComponent<plNetworkedPropertiesComponent>(ref_world, pRoot), plReplicateAuthority::Server);
          AddComponent<plNetworkRpcProbeComponent>(ref_world, pRoot);

          plGameObject* pArm = AddChild(ref_world, pRoot, "Arm");
          AddComponent<plNetworkReplicaTargetComponent>(ref_world, pArm);
          ReplicateTarget(AddComponent<plNetworkedPropertiesComponent>(ref_world, pArm), plReplicateAuthority::Server);
        });

      m_sOwnedPawn = m_Library.Add("OwnedPawn", [](plWorld& ref_world, plGameObject* pRoot)
        {
          AddComponent<plNetworkReplicaTargetComponent>(ref_world, pRoot);
          ReplicateTarget(AddComponent<plNetworkedPropertiesComponent>(ref_world, pRoot), plReplicateAuthority::Owner);

          plNetworkedTransformComponent* pTransform = AddComponent<plNetworkedTransformComponent>(ref_world, pRoot);
          pTransform->m_vWorldBoundsMin = plVec3(-64.0f);
          pTransform->m_vWorldBoundsMax = plVec3(64.0f);
          pTransform->m_fSendRate = 60.0f;
        });
    }

    void Register(std::initializer_list<NetworkEndpoint*> endpoints)
    {
      for (NetworkEndpoint* pEndpoint : endpoints)
      {
        pEndpoint->RegisterPrefab(m_sPawn);
        pEndpoint->RegisterPrefab(m_sOwnedPawn);
      }
    }
  };

  plNetworkReplicaTargetComponent* Target(NetworkEndpoint& endpoint, plUInt32 uiObjectID, plStringView sChild = {})
  {
    return endpoint.FindSpawnedComponent<plNetworkReplicaTargetComponent>(uiObjectID, sChild);
  }

  plInt32 TargetValue(NetworkEndpoint& endpoint, plUInt32 uiObjectID, plStringView sChild = {})
  {
    plNetworkReplicaTargetComponent* pTarget = Target(endpoint, uiObjectID, sChild);
    return pTarget ? pTarget->m_iValue : -1;
  }

  void SetTargetValue(NetworkEndpoint& endpoint, plUInt32 uiObjectID, plInt32 iValue, plStringView sChild = {})
  {
    PL_LOCK(endpoint.m_World.GetWriteMarker());
    if (plNetworkReplicaTargetComponent* pTarget = Target(endpoint, uiObjectID, sChild))
      pTarget->m_iValue = iValue;
  }

  void SetTargetPosition(NetworkEndpoint& endpoint, plUInt32 uiObjectID, const plVec3& vPosition)
  {
    PL_LOCK(endpoint.m_World.GetWriteMarker());
    if (plNetworkReplicaTargetComponent* pTarget = Target(endpoint, uiObjectID))
      pTarget->m_vPosition = vPosition;
  }

  plVec3 TargetPosition(NetworkEndpoint& endpoint, plUInt32 uiObjectID)
  {
    plNetworkReplicaTargetComponent* pTarget = Target(endpoint, uiObjectID);
    return pTarget ? pTarget->m_vPosition : plVec3(-1.0f);
  }

  bool HasObject(NetworkEndpoint& endpoint, plUInt32 uiObjectID) { return endpoint.FindSpawned(uiObjectID) != nullptr; }

  plVec3 SpawnedPosition(NetworkEndpoint& endpoint, plUInt32 uiObjectID)
  {
    plGameObject* pObject = endpoint.FindSpawned(uiObjectID);
    return pObject ? pObject->GetGlobalPosition() : plVec3(-100.0f);
  }

  void MoveSpawned(NetworkEndpoint& endpoint, plUInt32 uiObjectID, const plVec3& vPosition)
  {
    PL_LOCK(endpoint.m_World.GetWriteMarker());
    if (plGameObject* pObject = endpoint.FindSpawned(uiObjectID))
      pObject->SetGlobalPosition(vPosition);
  }

  void SetReplicaValue(NetworkEndpoint& endpoint, plUInt32 uiReplica, plInt32 iValue)
  {
    PL_LOCK(endpoint.m_World.GetWriteMarker());
    endpoint.m_Replicas[uiReplica].m_pTarget->m_iValue = iValue;
  }

  plInt32 ReplicaValue(NetworkEndpoint& endpoint, plUInt32 uiReplica) { return endpoint.m_Replicas[uiReplica].m_pTarget->m_iValue; }

  const char* RoleName(Role role) { return role == Role::Host ? "host" : "dedicated server"; }
} // namespace

PL_CREATE_SIMPLE_TEST(Network, SpawnAndOwnership)
{
  for (Role role : {Role::Host, Role::Server})
  {
    PL_TEST_BLOCK(plTestBlock::Enabled, "Spawn and despawn reach both clients")
    {
      FlowPrefabs prefabs;
      NetworkEndpoint server, a, b;
      prefabs.Register({&server, &a, &b});

      const Pumped all({&server, &a, &b});
      if (!PL_TEST_BOOL(ConnectAllAs(role, all, server, {&a, &b})))
        return;

      const plUInt32 uiObject = server.Spawn(prefabs.m_sPawn, server.m_pModule->GetServerClientID(), plVec3(1, 2, 3));
      if (!PL_TEST_BOOL(uiObject != 0))
        return;

      PL_TEST_BOOL_MSG(PumpAll(all, [&] { return HasObject(a, uiObject) && HasObject(b, uiObject); }), "%s: spawn", RoleName(role));
      PL_TEST_VEC3(SpawnedPosition(a, uiObject), plVec3(1, 2, 3), 0.001f);

      server.Despawn(uiObject);
      PL_TEST_BOOL_MSG(PumpAll(all, [&] { return !HasObject(a, uiObject) && !HasObject(b, uiObject); }), "%s: despawn", RoleName(role));
    }

    PL_TEST_BLOCK(plTestBlock::Enabled, "D02: an ownership transfer reaches both clients")
    {
      FlowPrefabs prefabs;
      NetworkEndpoint server, a, b;
      prefabs.Register({&server, &a, &b});

      const Pumped all({&server, &a, &b});
      if (!PL_TEST_BOOL(ConnectAllAs(role, all, server, {&a, &b})))
        return;

      const plUInt32 uiObject = server.Spawn(prefabs.m_sPawn, server.m_pModule->GetServerClientID());
      if (!PL_TEST_BOOL(uiObject != 0 && PumpAll(all, [&] { return HasObject(a, uiObject) && HasObject(b, uiObject); })))
        return;

      const plUInt32 uiA = a.m_pModule->GetLocalClientID();
      {
        PL_LOCK(server.m_World.GetWriteMarker());
        PL_TEST_BOOL(server.m_pModule->GetObjectManager()->TransferOwnership(uiObject, uiA));
      }

      PL_TEST_BOOL_MSG(PumpAll(all, [&] { return a.GetOwnerOf(uiObject) == uiA && b.GetOwnerOf(uiObject) == uiA; }),
        "%s: the clients still name owner %u and %u instead of %u", RoleName(role), a.GetOwnerOf(uiObject), b.GetOwnerOf(uiObject), uiA);
    }

    PL_TEST_BLOCK(plTestBlock::Enabled, "D03: a value set right after the spawn reaches connected clients")
    {
      FlowPrefabs prefabs;
      NetworkEndpoint server, a, b;
      prefabs.Register({&server, &a, &b});

      const Pumped all({&server, &a, &b});
      if (!PL_TEST_BOOL(ConnectAllAs(role, all, server, {&a, &b})))
        return;

      const plUInt32 uiObject = server.Spawn(prefabs.m_sPawn, server.m_pModule->GetServerClientID());
      SetTargetValue(server, uiObject, 42);

      PL_TEST_BOOL_MSG(PumpAll(all, [&] { return TargetValue(a, uiObject) == 42 && TargetValue(b, uiObject) == 42; }),
        "%s: the clients hold %d and %d", RoleName(role), TargetValue(a, uiObject), TargetValue(b, uiObject));
    }

    PL_TEST_BLOCK(plTestBlock::Enabled, "D09: an RPC sent in the frame of the spawn reaches the spawned component")
    {
      FlowPrefabs prefabs;
      NetworkEndpoint server, a, b;
      prefabs.Register({&server, &a, &b});

      const Pumped all({&server, &a, &b});
      if (!PL_TEST_BOOL(ConnectAllAs(role, all, server, {&a, &b})))
        return;

      const plUInt32 uiObject = server.Spawn(prefabs.m_sPawn, server.m_pModule->GetServerClientID());
      {
        PL_LOCK(server.m_World.GetWriteMarker());
        plNetworkRpcProbeComponent* pProbe = server.FindSpawnedComponent<plNetworkRpcProbeComponent>(uiObject);
        if (!PL_TEST_BOOL(pProbe != nullptr))
          return;
        PL_TEST_BOOL(server.m_pModule->GetRpcManager()->CallRpc(pProbe, "RpcAllClients", 5));
      }

      auto Calls = [&](NetworkEndpoint& endpoint)
      {
        plNetworkRpcProbeComponent* pProbe = endpoint.FindSpawnedComponent<plNetworkRpcProbeComponent>(uiObject);
        return pProbe ? pProbe->m_uiAllClients : 0u;
      };
      PL_TEST_BOOL_MSG(PumpAll(all, [&] { return Calls(a) == 1 && Calls(b) == 1; }), "%s: the clients ran it %u and %u times", RoleName(role), Calls(a), Calls(b));
    }

    PL_TEST_BLOCK(plTestBlock::Enabled, "D10: the child components of two instances of one prefab replicate separately")
    {
      FlowPrefabs prefabs;
      NetworkEndpoint server, a, b;
      prefabs.Register({&server, &a, &b});

      const Pumped all({&server, &a, &b});
      if (!PL_TEST_BOOL(ConnectAllAs(role, all, server, {&a, &b})))
        return;

      const plUInt32 uiFirst = server.Spawn(prefabs.m_sPawn, server.m_pModule->GetServerClientID());
      const plUInt32 uiSecond = server.Spawn(prefabs.m_sPawn, server.m_pModule->GetServerClientID());
      if (!PL_TEST_BOOL(PumpAll(all, [&] { return HasObject(a, uiSecond) && HasObject(b, uiSecond); })))
        return;

      PumpFor(all, plTime::MakeFromMilliseconds(100));
      SetTargetValue(server, uiFirst, 8, "Arm");
      SetTargetValue(server, uiSecond, 9, "Arm");

      PL_TEST_BOOL_MSG(PumpAll(all, [&] { return TargetValue(a, uiFirst, "Arm") == 8 && TargetValue(a, uiSecond, "Arm") == 9; }),
        "%s: client A holds %d and %d", RoleName(role), TargetValue(a, uiFirst, "Arm"), TargetValue(a, uiSecond, "Arm"));
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D10: same-named scene roots replicate separately")
  {
    NetworkEndpoint host, a;
    for (NetworkEndpoint* pEndpoint : {&host, &a})
    {
      pEndpoint->AddReplica("Twin", plReplicateAuthority::Server);
      pEndpoint->AddReplica("Twin", plReplicateAuthority::Server);
    }

    const Pumped all({&host, &a});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a})))
      return;

    SetReplicaValue(host, 0, 1);
    SetReplicaValue(host, 1, 2);
    PL_TEST_BOOL_MSG(PumpAll(all, [&] { return ReplicaValue(a, 0) == 1 && ReplicaValue(a, 1) == 2; }), "client holds %d and %d", ReplicaValue(a, 0), ReplicaValue(a, 1));
  }
}

PL_CREATE_SIMPLE_TEST(Network, LateJoin)
{
  for (Role role : {Role::Host, Role::Server})
  {
    PL_TEST_BLOCK(plTestBlock::Enabled, "D03: a late joiner gets runtime spawns with their current state")
    {
      FlowPrefabs prefabs;
      NetworkEndpoint server, a, b;
      prefabs.Register({&server, &a, &b});

      const Pumped all({&server, &a, &b});
      if (!PL_TEST_BOOL(ConnectAllAs(role, all, server, {&a})))
        return;

      plUInt32 objects[3] = {};
      for (plUInt32 i = 0; i < 3; ++i)
        objects[i] = server.Spawn(prefabs.m_sPawn, server.m_pModule->GetServerClientID());

      PumpFor(all, plTime::MakeFromMilliseconds(100));
      for (plUInt32 i = 0; i < 3; ++i)
        SetTargetValue(server, objects[i], 10 + i);

      if (!PL_TEST_BOOL(PumpAll(all, [&] { return TargetValue(a, objects[2]) == 12; })))
        return;

      if (!PL_TEST_BOOL(b.Start(false, server.GetPort())))
        return;

      PL_TEST_BOOL_MSG(PumpAll(all, [&] { return TargetValue(b, objects[0]) == 10 && TargetValue(b, objects[1]) == 11 && TargetValue(b, objects[2]) == 12; }),
        "%s: the late joiner holds %d, %d and %d", RoleName(role), TargetValue(b, objects[0]), TargetValue(b, objects[1]), TargetValue(b, objects[2]));
    }

    PL_TEST_BLOCK(plTestBlock::Enabled, "A late joiner does not get despawned objects")
    {
      FlowPrefabs prefabs;
      NetworkEndpoint server, a, b;
      prefabs.Register({&server, &a, &b});

      const Pumped all({&server, &a, &b});
      if (!PL_TEST_BOOL(ConnectAllAs(role, all, server, {&a})))
        return;

      const plUInt32 uiKept = server.Spawn(prefabs.m_sPawn, server.m_pModule->GetServerClientID());
      const plUInt32 uiGone = server.Spawn(prefabs.m_sPawn, server.m_pModule->GetServerClientID());
      PumpAll(all, [&] { return HasObject(a, uiGone); });
      server.Despawn(uiGone);
      PumpFor(all, plTime::MakeFromMilliseconds(50));

      if (!PL_TEST_BOOL(b.Start(false, server.GetPort())))
        return;

      PL_TEST_BOOL(PumpAll(all, [&] { return HasObject(b, uiKept); }));
      PumpFor(all, plTime::MakeFromMilliseconds(100));
      PL_TEST_BOOL(!HasObject(b, uiGone));
    }
  }
}

PL_CREATE_SIMPLE_TEST(Network, ClientOwnedState)
{
  for (Role role : {Role::Host, Role::Server})
  {
    PL_TEST_BLOCK(plTestBlock::Enabled, "D11: a client-owned transform reaches the server and the other client")
    {
      FlowPrefabs prefabs;
      NetworkEndpoint server, a, b;
      prefabs.Register({&server, &a, &b});

      const Pumped all({&server, &a, &b});
      if (!PL_TEST_BOOL(ConnectAllAs(role, all, server, {&a, &b})))
        return;

      const plUInt32 uiObject = server.Spawn(prefabs.m_sOwnedPawn, a.m_pModule->GetLocalClientID());
      if (!PL_TEST_BOOL(PumpAll(all, [&] { return HasObject(a, uiObject) && HasObject(b, uiObject); })))
        return;

      const plVec3 vTarget(4.0f, -2.0f, 1.0f);
      MoveSpawned(a, uiObject, vTarget);
      PL_TEST_BOOL_MSG(PumpAll(all, [&] { return SpawnedPosition(server, uiObject).IsEqual(vTarget, 0.01f) && SpawnedPosition(b, uiObject).IsEqual(vTarget, 0.01f); }),
        "%s: server at (%.2f %.2f %.2f), B at (%.2f %.2f %.2f)", RoleName(role), SpawnedPosition(server, uiObject).x, SpawnedPosition(server, uiObject).y,
        SpawnedPosition(server, uiObject).z, SpawnedPosition(b, uiObject).x, SpawnedPosition(b, uiObject).y, SpawnedPosition(b, uiObject).z);
      PL_TEST_BOOL_MSG(SpawnedPosition(a, uiObject).IsEqual(vTarget, 0.001f), "The owner keeps the transform it simulates.");
    }

    PL_TEST_BLOCK(plTestBlock::Enabled, "D06: an owner's interpolated upload ends on its final value at the other client")
    {
      FlowPrefabs prefabs;
      NetworkEndpoint server, a, b;
      prefabs.Register({&server, &a, &b});

      const Pumped all({&server, &a, &b});
      if (!PL_TEST_BOOL(ConnectAllAs(role, all, server, {&a, &b})))
        return;

      const plUInt32 uiObject = server.Spawn(prefabs.m_sOwnedPawn, a.m_pModule->GetLocalClientID());
      if (!PL_TEST_BOOL(PumpAll(all, [&] { return HasObject(a, uiObject) && HasObject(b, uiObject) && HasObject(server, uiObject); })))
        return;

      PumpFor(all, plTime::MakeFromMilliseconds(100));
      {
        PL_LOCK(server.m_World.GetWriteMarker());
        server.FindSpawnedComponent<plNetworkedPropertiesComponent>(uiObject)->m_fInterpolationTime = 0.25f;
      }

      SetTargetPosition(a, uiObject, plVec3(1.0f, 0.0f, 0.0f));
      PumpFor(all, plTime::MakeFromMilliseconds(60));
      SetTargetPosition(a, uiObject, plVec3(2.0f, 0.0f, 0.0f));

      PL_TEST_BOOL_MSG(PumpAll(all, [&] { return TargetPosition(b, uiObject).IsEqual(plVec3(2.0f, 0.0f, 0.0f), 0.001f); }),
        "%s: B ends at x = %.3f", RoleName(role), TargetPosition(b, uiObject).x);
    }

    PL_TEST_BLOCK(plTestBlock::Enabled, "D06: a relay held back by the send rate is sent later")
    {
      FlowPrefabs prefabs;
      NetworkEndpoint server, a, b;
      prefabs.Register({&server, &a, &b});

      const Pumped all({&server, &a, &b});
      if (!PL_TEST_BOOL(ConnectAllAs(role, all, server, {&a, &b})))
        return;

      const plUInt32 uiObject = server.Spawn(prefabs.m_sOwnedPawn, a.m_pModule->GetLocalClientID());
      if (!PL_TEST_BOOL(PumpAll(all, [&] { return HasObject(a, uiObject) && HasObject(b, uiObject) && HasObject(server, uiObject); })))
        return;

      PumpFor(all, plTime::MakeFromMilliseconds(100));
      {
        PL_LOCK(server.m_World.GetWriteMarker());
        server.FindSpawnedComponent<plNetworkedPropertiesComponent>(uiObject)->m_fSendRate = 4.0f;
      }

      SetTargetValue(a, uiObject, 1);
      PumpAll(all, [&] { return TargetValue(b, uiObject) == 1; });
      SetTargetValue(a, uiObject, 2);

      PL_TEST_BOOL_MSG(PumpAll(all, [&] { return TargetValue(b, uiObject) == 2; }), "%s: B holds %d", RoleName(role), TargetValue(b, uiObject));
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D05: a host-owned Owner-authority value reaches the clients")
  {
    FlowPrefabs prefabs;
    NetworkEndpoint host, a, b;
    prefabs.Register({&host, &a, &b});

    const Pumped all({&host, &a, &b});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a, &b})))
      return;

    const plUInt32 uiObject = host.Spawn(prefabs.m_sOwnedPawn, host.m_pModule->GetLocalClientID());
    if (!PL_TEST_BOOL(PumpAll(all, [&] { return HasObject(a, uiObject) && HasObject(b, uiObject); })))
      return;

    PumpFor(all, plTime::MakeFromMilliseconds(100));
    SetTargetValue(host, uiObject, 5);
    MoveSpawned(host, uiObject, plVec3(3.0f, 3.0f, 3.0f));

    PL_TEST_BOOL_MSG(PumpAll(all, [&] { return TargetValue(a, uiObject) == 5 && TargetValue(b, uiObject) == 5; }), "the clients hold %d and %d",
      TargetValue(a, uiObject), TargetValue(b, uiObject));
    PL_TEST_BOOL(PumpAll(all, [&] { return SpawnedPosition(a, uiObject).IsEqual(plVec3(3.0f), 0.01f); }));
  }
}

PL_CREATE_SIMPLE_TEST(Network, LevelTravel)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "D03: scene state set before a client loaded the level reaches it")
  {
    NetworkEndpoint host, a;
    const Pumped all({&host, &a});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a})))
      return;

    {
      PL_LOCK(host.m_World.GetWriteMarker());
      host.m_pModule->GetLevelManager()->RequestLevelLoad("Level2", 30.0f);
    }

    if (!PL_TEST_BOOL(PumpAll(all, [&] { return a.m_pModule->GetLevelManager()->GetState() == plNetworkLevelState::Loading; })))
      return;

    // The host is already in the new level and changes its state while the client still loads it.
    host.AddReplica("Level2Crate", plReplicateAuthority::Server);
    SetReplicaValue(host, 0, 77);
    PumpFor(all, plTime::MakeFromMilliseconds(200));

    a.AddReplica("Level2Crate", plReplicateAuthority::Server);
    {
      PL_LOCK(a.m_World.GetWriteMarker());
      a.m_pModule->GetLevelManager()->NotifyLoadComplete();
    }

    PL_TEST_BOOL(PumpAll(all, [&] { return a.m_pModule->GetLevelManager()->GetState() == plNetworkLevelState::Ready; }));
    PL_TEST_BOOL_MSG(PumpAll(all, [&] { return ReplicaValue(a, 0) == 77; }), "the client holds %d", ReplicaValue(a, 0));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D29: a client that disconnects during a level load does not hold the level")
  {
    NetworkEndpoint host, a, b;
    const Pumped all({&host, &a, &b});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a, &b})))
      return;

    {
      PL_LOCK(host.m_World.GetWriteMarker());
      host.m_pModule->GetLevelManager()->RequestLevelLoad("Level2", 30.0f);
    }

    if (!PL_TEST_BOOL(PumpAll(all, [&] { return a.m_pModule->GetLevelManager()->GetState() == plNetworkLevelState::Loading && b.m_pModule->GetLevelManager()->GetState() == plNetworkLevelState::Loading; })))
      return;

    b.Stop();
    PumpAll(all, [&] { return host.m_pModule->GetClients().GetCount() == 1; });
    {
      PL_LOCK(a.m_World.GetWriteMarker());
      a.m_pModule->GetLevelManager()->NotifyLoadComplete();
    }

    PL_TEST_BOOL_MSG(PumpAll(all, [&] { return host.m_pModule->GetLevelManager()->GetState() == plNetworkLevelState::Ready; }, plTime::MakeFromSeconds(3)),
      "The host still waits for the client that left.");
  }
}
