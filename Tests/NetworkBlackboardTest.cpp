#include "NetworkTestPCH.h"

#include <Foundation/Reflection/ReflectionUtils.h>
#include <GameEngine/Gameplay/BlackboardComponent.h>
#include <NetworkPlugin/Components/NetworkedBlackboardComponent.h>

#include "NetworkTestHarness.h"

using namespace NetworkTestHarness;

namespace
{
  /// \brief Adds a scene object "Board" with a local blackboard that a server-authoritative networked blackboard replicates.
  plBlackboard* AddBoard(NetworkEndpoint& endpoint)
  {
    PL_LOCK(endpoint.m_World.GetWriteMarker());
    plGameObjectDesc desc;
    desc.m_sName.Assign("Board");
    desc.m_bDynamic = true;
    plGameObject* pObject = nullptr;
    endpoint.m_World.CreateObject(desc, pObject);

    plLocalBlackboardComponent* pBoard = AddComponent<plLocalBlackboardComponent>(endpoint.m_World, pObject);
    AddComponent<plNetworkedBlackboardComponent>(endpoint.m_World, pObject);
    endpoint.m_World.Update();
    return pBoard->GetBoard().Borrow();
  }

  plBlackboard* SpawnedBoard(NetworkEndpoint& endpoint, plUInt32 uiObjectID)
  {
    plLocalBlackboardComponent* pBoard = endpoint.FindSpawnedComponent<plLocalBlackboardComponent>(uiObjectID);
    return pBoard ? pBoard->GetBoard().Borrow() : nullptr;
  }

  plInt32 Entry(plBlackboard* pBoard, plStringView sName)
  {
    if (pBoard == nullptr)
      return -1;

    const plBlackboard::Entry* pEntry = pBoard->GetEntry(plTempHashedString(sName));
    return pEntry != nullptr ? pEntry->m_Value.ConvertTo<plInt32>() : -1;
  }

  void Set(NetworkEndpoint& endpoint, plBlackboard* pBoard, plStringView sName, plInt32 iValue)
  {
    PL_LOCK(endpoint.m_World.GetWriteMarker());
    pBoard->SetEntryValue(sName, iValue);
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Network, Blackboard)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "Server-authoritative entries reach the clients")
  {
    NetworkEndpoint host, a;
    plBlackboard* pHostBoard = AddBoard(host);
    plBlackboard* pBoardA = AddBoard(a);

    const Pumped all({&host, &a});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a})))
      return;

    Set(host, pHostBoard, "Health", 5);
    PL_TEST_BOOL(PumpAll(all, [&] { return Entry(pBoardA, "Health") == 5; }));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D07: a late joiner gets every entry")
  {
    NetworkEndpoint host, a, b;
    plBlackboard* pHostBoard = AddBoard(host);
    plBlackboard* pBoardA = AddBoard(a);
    plBlackboard* pBoardB = AddBoard(b);

    const Pumped all({&host, &a, &b});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a})))
      return;

    Set(host, pHostBoard, "Health", 5);
    Set(host, pHostBoard, "Ammo", 3);
    if (!PL_TEST_BOOL(PumpAll(all, [&] { return Entry(pBoardA, "Health") == 5 && Entry(pBoardA, "Ammo") == 3; })))
      return;

    if (!PL_TEST_BOOL(b.Start(false, host.GetPort())))
      return;

    NETWORK_TEST_WAIT(PumpAll(all, [&] { return Entry(pBoardB, "Health") == 5 && Entry(pBoardB, "Ammo") == 3; }), "the late joiner holds Health %d, Ammo %d",
      Entry(pBoardB, "Health"), Entry(pBoardB, "Ammo"));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D07: removing an entry removes it on the clients")
  {
    NetworkEndpoint host, a;
    plBlackboard* pHostBoard = AddBoard(host);
    plBlackboard* pBoardA = AddBoard(a);

    const Pumped all({&host, &a});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a})))
      return;

    Set(host, pHostBoard, "Health", 5);
    Set(host, pHostBoard, "Ammo", 3);
    if (!PL_TEST_BOOL(PumpAll(all, [&] { return Entry(pBoardA, "Ammo") == 3; })))
      return;

    {
      PL_LOCK(host.m_World.GetWriteMarker());
      plHashedString sAmmo;
      sAmmo.Assign("Ammo");
      pHostBoard->RemoveEntry(sAmmo);
    }

    NETWORK_TEST_WAIT(PumpAll(all, [&] { return Entry(pBoardA, "Ammo") == -1; }), "the client still holds Ammo %d", Entry(pBoardA, "Ammo"));
    PL_TEST_INT(Entry(pBoardA, "Health"), 5);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D07: a change pending while another client is admitted still reaches the connected client")
  {
    NetworkEndpoint host, a;
    plBlackboard* pHostBoard = AddBoard(host);
    plBlackboard* pBoardA = AddBoard(a);

    const Pumped all({&host, &a});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a})))
      return;

    HostileClient late(host.GetPort(), 0);
    if (!PL_TEST_BOOL(PumpAll(Pumped({&host, &a}, {&late}), [&] { return late.m_bConnected && host.m_pModule->GetClients().GetCount() == 2; })))
      return;

    // The host admits the new client in the same update in which it first sees the change.
    late.Handshake();
    Set(host, pHostBoard, "Score", 9);
    host.Pump();

    NETWORK_TEST_WAIT(PumpAll(Pumped({&host, &a}, {&late}), [&] { return Entry(pBoardA, "Score") == 9; }), "the connected client holds Score %d", Entry(pBoardA, "Score"));
    late.Disconnect();
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "D07: owner-writable entries reach the server and the other client")
  {
    TestPrefabs prefabs;
    const plHashedString sPrefab = prefabs.Add("OwnedBoard", [](plWorld& ref_world, plGameObject* pRoot)
      {
        AddComponent<plLocalBlackboardComponent>(ref_world, pRoot);
        plNetworkedBlackboardComponent* pNetworked = AddComponent<plNetworkedBlackboardComponent>(ref_world, pRoot);
        plReflectionUtils::SetMemberPropertyValue(static_cast<const plAbstractMemberProperty*>(pNetworked->GetDynamicRTTI()->FindPropertyByName("Authority")),
          pNetworked, plInt64(plNetworkBlackboardAuthority::OwnerWritable));
      });

    NetworkEndpoint host, a, b;
    for (NetworkEndpoint* pEndpoint : {&host, &a, &b})
      pEndpoint->RegisterPrefab(sPrefab);

    const Pumped all({&host, &a, &b});
    if (!PL_TEST_BOOL(ConnectAll(all, host, {&a, &b})))
      return;

    const plUInt32 uiObject = host.Spawn(sPrefab, a.m_pModule->GetLocalClientID());
    if (!PL_TEST_BOOL(PumpAll(all, [&] { return SpawnedBoard(a, uiObject) && SpawnedBoard(b, uiObject); })))
      return;
    PumpFor(all, plTime::MakeFromMilliseconds(100));

    Set(a, SpawnedBoard(a, uiObject), "Score", 7);
    NETWORK_TEST_WAIT(PumpAll(all, [&] { return Entry(SpawnedBoard(host, uiObject), "Score") == 7 && Entry(SpawnedBoard(b, uiObject), "Score") == 7; }),
      "host holds %d, B holds %d", Entry(SpawnedBoard(host, uiObject), "Score"), Entry(SpawnedBoard(b, uiObject), "Score"));
  }
}
